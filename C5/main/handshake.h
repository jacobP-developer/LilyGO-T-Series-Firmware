// main/handshake.h
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t handshake_init(void);

// Feed a promiscuous data frame. Called from the Wi-Fi RX callback.
void handshake_feed(const uint8_t *frame, size_t len, int8_t rssi, uint8_t channel);

// True if a usable pair (M1+M2 or M3+M4) is captured for this BSSID.
bool handshake_is_usable(const uint8_t *bssid);

// Writes /sdcard/handshakes/<bssid>_<epoch>.pcap if usable.
// Returns ESP_OK on success, ESP_ERR_NOT_FOUND if no usable handshake.
esp_err_t handshake_dump_pcap(const uint8_t *bssid, const char *ssid);

// Diagnostic: which messages (bitmask) are captured for this BSSID.
uint8_t handshake_message_mask(const uint8_t *bssid);

// Clear tracking state for a BSSID.
void handshake_reset(const uint8_t *bssid);

#ifdef __cplusplus
}
#endif