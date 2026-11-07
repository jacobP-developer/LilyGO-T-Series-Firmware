// main/input.h
#ifndef M4NT1S_INPUT_H
#define M4NT1S_INPUT_H

typedef enum {
    INPUT_UP = 0,
    INPUT_DOWN,
    INPUT_ENTER,
    INPUT_BACK
} input_event_t;

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t input_init(void);
BaseType_t input_get_event(input_event_t *event, TickType_t timeout);
void input_post_event(input_event_t event);   // <-- common section

#ifdef __cplusplus
}
#endif

#endif // M4NT1S_INPUT_H