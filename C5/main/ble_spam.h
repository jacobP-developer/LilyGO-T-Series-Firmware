// main/ble_spam.h
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_SPAM_APPLEJUICE = 0,
    BLE_SPAM_SOURAPPLE,
    BLE_SPAM_SAMSUNG,
    BLE_SPAM_WINDOWS,
    BLE_SPAM_FASTPAIR,
    BLE_SPAM_COUNT
} ble_spam_type_t;

esp_err_t ble_spam_init(void);
esp_err_t ble_spam_start(ble_spam_type_t type);
esp_err_t ble_spam_stop(void);
bool      ble_spam_is_running(void);

/* Stops NimBLE and deinitialises the Bluetooth stack. Call this before any
 * dual-band Wi-Fi scan — 5 GHz scans fail with ESP_ERR_TIMEOUT when
 * Bluetooth is running (IDFGH-15407 / esp-idf#16059). After this call,
 * ble_spam_is_resident() returns false and ble_spam_start() will bring
 * NimBLE back up from scratch. */
esp_err_t ble_spam_release_radio(void);

/* True once NimBLE has been brought up in this boot. */
bool ble_spam_is_resident(void);

/* Diagnostic:
 *   >0    = esp_err_t returned by nimble_port_init() or xTaskCreate
 *           (257 = ESP_ERR_NO_MEM)
 *   -2    = host sync never completed
 *   -1001 = precheck failed: largest contiguous DMA block < threshold
 *   -1002 = NimBLE bring-up failed after precheck passed
 *   -1003 = xTaskCreate for spam task failed
 *   0     = no error recorded. */
int       ble_spam_last_error(void);
void      ble_spam_clear_error(void);

const char *ble_spam_type_name(ble_spam_type_t type);
int         ble_spam_type_count(void);

#ifdef __cplusplus
}
#endif