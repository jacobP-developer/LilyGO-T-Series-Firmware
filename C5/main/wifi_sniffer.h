// main/wifi_sniffer.h
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_sniffer_init(void);
esp_err_t wifi_sniffer_deinit(void);
bool      wifi_sniffer_is_initialized(void);

void wifi_sniffer_start(void);
void wifi_sniffer_stop(void);
void wifi_sniffer_set_channel(uint8_t channel);

// Re-register the MGMT filter and RX callback on the Wi-Fi driver.
// Used to recover driver state after another module (DEAUTH) has
// overwritten them. Safe to call while promiscuous is disabled.
esp_err_t wifi_sniffer_rearm(void);

// Current totals. Either pointer may be NULL.
void wifi_sniffer_get_stats(int *ap_count, int *client_count);

#ifdef __cplusplus
}
#endif