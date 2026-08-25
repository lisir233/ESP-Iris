#include "esp_iris_state.h"

bool iris_lifecycle_transition(esp_iris_lifecycle_t current,
                               esp_iris_lifecycle_t requested,
                               esp_iris_lifecycle_t *out)
{
    if (out == NULL) {
        return false;
    }
    bool allowed = current == requested;
    switch (current) {
    case ESP_IRIS_LIFECYCLE_STOPPED:
        allowed = allowed || requested == ESP_IRIS_LIFECYCLE_STARTING;
        break;
    case ESP_IRIS_LIFECYCLE_STARTING:
        allowed = allowed || requested == ESP_IRIS_LIFECYCLE_RUNNING ||
                  requested == ESP_IRIS_LIFECYCLE_FAILED;
        break;
    case ESP_IRIS_LIFECYCLE_RUNNING:
        allowed = allowed || requested == ESP_IRIS_LIFECYCLE_STOPPING ||
                  requested == ESP_IRIS_LIFECYCLE_FAILED;
        break;
    case ESP_IRIS_LIFECYCLE_STOPPING:
        allowed = allowed || requested == ESP_IRIS_LIFECYCLE_STOPPED ||
                  requested == ESP_IRIS_LIFECYCLE_FAILED;
        break;
    case ESP_IRIS_LIFECYCLE_FAILED:
        allowed = allowed || requested == ESP_IRIS_LIFECYCLE_STARTING ||
                  requested == ESP_IRIS_LIFECYCLE_STOPPING ||
                  requested == ESP_IRIS_LIFECYCLE_STOPPED;
        break;
    default:
        allowed = false;
        break;
    }
    if (allowed) {
        *out = requested;
    }
    return allowed;
}

bool iris_session_transition(iris_session_state_t current,
                             iris_session_event_t event,
                             iris_session_state_t *out)
{
    if (out == NULL) {
        return false;
    }
    iris_session_state_t requested = current;
    bool allowed = true;
    switch (event) {
    case IRIS_SESSION_EVENT_LINK_UP:
        allowed = current == IRIS_SESSION_DISCONNECTED;
        requested = IRIS_SESSION_NEGOTIATING;
        break;
    case IRIS_SESSION_EVENT_AUTHENTICATED:
        allowed = current == IRIS_SESSION_NEGOTIATING;
        requested = IRIS_SESSION_READY;
        break;
    case IRIS_SESSION_EVENT_LINK_DOWN:
        allowed = current == IRIS_SESSION_DISCONNECTED ||
                  current == IRIS_SESSION_NEGOTIATING ||
                  current == IRIS_SESSION_READY;
        requested = IRIS_SESSION_DISCONNECTED;
        break;
    default:
        allowed = false;
        break;
    }
    if (allowed) {
        *out = requested;
    }
    return allowed;
}
