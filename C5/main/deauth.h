// main/deauth.h
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEAUTH_MAX_NETWORKS 16

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;
    bool    pmf_required;   // WPA3-SAE or WPA2/WPA3-transition (PMF mandatory)
} deauth_network_t;

esp_err_t deauth_init(void);

// Scan (async). Poll deauth_scan_done() until true.
esp_err_t deauth_scan_start(void);
bool      deauth_scan_done(void);

int                      deauth_network_count(void);
const deauth_network_t  *deauth_network_at(int idx);

// Start / stop deauth on the given index (0-based, matches list order).
// Returns ESP_ERR_NOT_SUPPORTED if the target requires PMF.
esp_err_t deauth_start(int idx);
esp_err_t deauth_stop(void);
bool      deauth_is_running(void);

#ifdef __cplusplus
}
#endif