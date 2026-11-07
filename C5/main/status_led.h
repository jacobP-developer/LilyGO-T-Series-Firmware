// main/status_led.h
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_MODE_IDLE = 0,     // green when connected, yellow when not
    LED_MODE_ACTIVE,       // blue (any module running)
} led_mode_t;

esp_err_t status_led_init(void);

/* Set the running/idle state. Called from app_main each loop iteration. */
void status_led_set_mode(led_mode_t mode);

/* Flash red for 3 s, then return to the current state. */
void status_led_flash_error(void);

/* Called from serial_input when a heartbeat command arrives. */
void status_led_notify_heartbeat(void);

/* True if a heartbeat arrived within the last 6 s. */
bool status_led_is_connected(void);

/* Host-supplied device name (up to 32 chars). Defaults to "NONE". */
void        status_led_set_host_name(const char *name);
const char *status_led_get_host_name(void);

#ifdef __cplusplus
}
#endif