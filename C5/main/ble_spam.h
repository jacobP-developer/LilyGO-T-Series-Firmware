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

/* True once NimBLE has been brought up in this boot. Once true, the
 * Wi-Fi driver cannot be reinitialised without a reboot — the S3 has
 * no PSRAM and NimBLE's buffers fragment the internal DMA heap. The UI
 * uses this to refuse WiFi mode selections with a "REBOOT" message. */
bool ble_spam_is_resident(void);

/* Diagnostic:
 *   >0    = esp_err_t returned by nimble_port_init() or xTaskCreate
 *           (257 = ESP_ERR_NO_MEM)
 *   -2    = host sync never completed
 *   -1001 = precheck failed: largest contiguous DMA block < threshold
 *           (happens when Wi-Fi was initialised earlier and fragmented
 *           the heap — reboot to recover)
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