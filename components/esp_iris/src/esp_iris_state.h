#pragma once

#include <stdbool.h>

#include "esp_iris.h"

typedef enum {
    IRIS_SESSION_DISCONNECTED = 0,
    IRIS_SESSION_NEGOTIATING = 1,
    IRIS_SESSION_READY = 2,
} iris_session_state_t;

typedef enum {
    IRIS_SESSION_EVENT_LINK_UP = 0,
    IRIS_SESSION_EVENT_AUTHENTICATED = 1,
    IRIS_SESSION_EVENT_LINK_DOWN = 2,
} iris_session_event_t;

bool iris_lifecycle_transition(esp_iris_lifecycle_t current,
                               esp_iris_lifecycle_t requested,
                               esp_iris_lifecycle_t *out);
bool iris_session_transition(iris_session_state_t current,
                             iris_session_event_t event,
                             iris_session_state_t *out);
