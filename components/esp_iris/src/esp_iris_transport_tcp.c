#include "esp_iris_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

static void close_client(iris_transport_state_t *state)
{
    if (state->client_fd >= 0) {
        close(state->client_fd);
        state->client_fd = -1;
    }
    state->link_up = false;
}

static esp_err_t open_listener(iris_transport_state_t *state)
{
    if (state->listen_fd >= 0) {
        return ESP_OK;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    const int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        close(fd);
        return ESP_FAIL;
    }

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_ESP_IRIS_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(fd, 2) != 0) {
        close(fd);
        return ESP_FAIL;
    }
    state->listen_fd = fd;
    return ESP_OK;
}

static esp_err_t tcp_start(iris_runtime_t *runtime,
                           iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->driver_started) {
        return ESP_OK;
    }
    state->listen_fd = -1;
    state->client_fd = -1;
    state->driver_started = true;
    state->link_up = false;
    state->reported_link_up = false;
    /* socket() asserts inside lwIP when tcpip_init has not run yet. Initialize
     * only the global TCP/IP core; the product still owns interfaces, Wi-Fi,
     * addressing and reconnect policy. esp_netif_init() is idempotent. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        state->driver_started = false;
        return err;
    }
    /* An IP interface may still be created after esp_iris_start(). Binding
     * INADDR_ANY is safe now and listener creation remains retryable. */
    (void)open_listener(state);
    return ESP_OK;
}

static void tcp_stop(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL) {
        return;
    }
    close_client(state);
    if (state->listen_fd >= 0) {
        close(state->listen_fd);
        state->listen_fd = -1;
    }
    state->driver_started = false;
    state->reported_link_up = false;
}

static void tcp_disconnect(iris_runtime_t *runtime,
                           iris_transport_state_t *state)
{
    (void)runtime;
    close_client(state);
}

static iris_link_event_t tcp_transport_poll(iris_runtime_t *runtime,
                                            iris_transport_state_t *state)
{
    (void)runtime;
    if (state->listen_fd < 0) {
        (void)open_listener(state);
    }
    if (state->listen_fd >= 0) {
        struct sockaddr_storage peer;
        socklen_t peer_size = sizeof(peer);
        const int client = accept(state->listen_fd,
                                  (struct sockaddr *)&peer, &peer_size);
        if (client >= 0) {
            if (state->client_fd >= 0) {
                close(client);
            } else {
                const int enabled = 1;
                (void)fcntl(client, F_SETFL, O_NONBLOCK);
                (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                                 &enabled, sizeof(enabled));
                (void)setsockopt(client, SOL_SOCKET, SO_KEEPALIVE,
                                 &enabled, sizeof(enabled));
                state->client_fd = client;
                state->link_up = true;
            }
        }
    }

    if (state->link_up == state->reported_link_up) {
        return IRIS_LINK_EVENT_NONE;
    }
    state->reported_link_up = state->link_up;
    return state->link_up ? IRIS_LINK_EVENT_CONNECTED
                          : IRIS_LINK_EVENT_DISCONNECTED;
}

static int tcp_read(iris_runtime_t *runtime, iris_transport_state_t *state,
                    uint8_t *buffer, size_t capacity)
{
    (void)runtime;
    if (state->client_fd < 0) {
        return -ENOTCONN;
    }
    const int result = recv(state->client_fd, buffer, capacity, MSG_DONTWAIT);
    if (result > 0) {
        return result;
    }
    if (result == 0) {
        close_client(state);
        return -ENOTCONN;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    close_client(state);
    return -errno;
}

static int tcp_transport_write(iris_runtime_t *runtime,
                               iris_transport_state_t *state,
                               const uint8_t *buffer, size_t length)
{
    (void)runtime;
    if (state->client_fd < 0) {
        return -ENOTCONN;
    }
    const int result = send(state->client_fd, buffer, length, MSG_DONTWAIT);
    if (result >= 0) {
        return result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    close_client(state);
    return -errno;
}

const iris_transport_ops_t g_iris_tcp_transport_ops = {
    .kind = ESP_IRIS_TRANSPORT_KIND_TCP,
    .name = "tcp",
    .start = tcp_start,
    .stop = tcp_stop,
    .poll = tcp_transport_poll,
    .disconnect = tcp_disconnect,
    .read = tcp_read,
    .write = tcp_transport_write,
};
