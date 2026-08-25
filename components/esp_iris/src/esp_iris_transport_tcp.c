#include "esp_iris_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

static void close_client(iris_runtime_t *runtime)
{
    if (runtime->transport.client_fd >= 0) {
        close(runtime->transport.client_fd);
        runtime->transport.client_fd = -1;
    }
    runtime->transport.link_up = false;
}

static esp_err_t open_listener(iris_runtime_t *runtime)
{
    if (runtime->transport.listen_fd >= 0) {
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
    runtime->transport.listen_fd = fd;
    return ESP_OK;
}

esp_err_t iris_transport_start(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime->transport.listen_fd = -1;
    runtime->transport.client_fd = -1;
    runtime->transport.driver_started = true;
    runtime->transport.link_up = false;
    runtime->transport.reported_link_up = false;
    /* socket() asserts inside lwIP when tcpip_init has not run yet. Initialize
     * only the global TCP/IP core; the product still owns interfaces, Wi-Fi,
     * addressing and reconnect policy. esp_netif_init() is idempotent. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        runtime->transport.driver_started = false;
        return err;
    }
    /* An IP interface may still be created after esp_iris_start(). Binding
     * INADDR_ANY is safe now and listener creation remains retryable. */
    (void)open_listener(runtime);
    return ESP_OK;
}

void iris_transport_stop(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    close_client(runtime);
    if (runtime->transport.listen_fd >= 0) {
        close(runtime->transport.listen_fd);
        runtime->transport.listen_fd = -1;
    }
    runtime->transport.driver_started = false;
    runtime->transport.reported_link_up = false;
}

iris_link_event_t iris_transport_poll(iris_runtime_t *runtime)
{
    if (runtime->transport.listen_fd < 0) {
        (void)open_listener(runtime);
    }
    if (runtime->transport.listen_fd >= 0) {
        struct sockaddr_storage peer;
        socklen_t peer_size = sizeof(peer);
        const int client = accept(runtime->transport.listen_fd,
                                  (struct sockaddr *)&peer, &peer_size);
        if (client >= 0) {
            if (runtime->transport.client_fd >= 0) {
                close(client);
            } else {
                const int enabled = 1;
                (void)fcntl(client, F_SETFL, O_NONBLOCK);
                (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                                 &enabled, sizeof(enabled));
                (void)setsockopt(client, SOL_SOCKET, SO_KEEPALIVE,
                                 &enabled, sizeof(enabled));
                runtime->transport.client_fd = client;
                runtime->transport.link_up = true;
            }
        }
    }

    if (runtime->transport.link_up == runtime->transport.reported_link_up) {
        return IRIS_LINK_EVENT_NONE;
    }
    runtime->transport.reported_link_up = runtime->transport.link_up;
    return runtime->transport.link_up ? IRIS_LINK_EVENT_CONNECTED
                                      : IRIS_LINK_EVENT_DISCONNECTED;
}

int iris_transport_read(iris_runtime_t *runtime, uint8_t *buffer,
                        size_t capacity)
{
    if (runtime->transport.client_fd < 0) {
        return -ENOTCONN;
    }
    const int result = recv(runtime->transport.client_fd, buffer, capacity,
                            MSG_DONTWAIT);
    if (result > 0) {
        return result;
    }
    if (result == 0) {
        close_client(runtime);
        return -ENOTCONN;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    close_client(runtime);
    return -errno;
}

int iris_transport_write(iris_runtime_t *runtime, const uint8_t *buffer,
                         size_t length)
{
    if (runtime->transport.client_fd < 0) {
        return -ENOTCONN;
    }
    const int result = send(runtime->transport.client_fd, buffer, length,
                            MSG_DONTWAIT);
    if (result >= 0) {
        return result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    close_client(runtime);
    return -errno;
}

esp_iris_transport_kind_t iris_transport_kind(void)
{
    return ESP_IRIS_TRANSPORT_KIND_TCP;
}

const char *iris_transport_name(void)
{
    return "tcp";
}
