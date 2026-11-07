// main/beacon.c
#include "beacon.h"
#include "portal.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "beacon";

// ---------------------------------------------------------------------------
// WIFI CLONE — connectable open AP with captive portal
// ---------------------------------------------------------------------------
static bool ap_running = false;
static esp_netif_t *ap_netif = NULL;

#define CLONE_SSID "FreeWiFi"
#define CLONE_CHANNEL 6

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ui_set_status("CONNECTED");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ui_set_status("AP ON");
    }
}

esp_err_t beacon_init(void)
{
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
        if (ap_netif == NULL) return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    return ESP_OK;
}

esp_err_t beacon_start(void)
{
    if (ap_running) return ESP_OK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = CLONE_SSID,
            .ssid_len = strlen(CLONE_SSID),
            .channel = CLONE_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
            .pmf_cfg = { .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    portal_start();  // non-fatal
    ap_running = true;
    return ESP_OK;
}

esp_err_t beacon_stop(void)
{
    if (!ap_running) return ESP_OK;
    portal_stop();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    ap_running = false;
    return ESP_OK;
}

bool beacon_is_running(void) { return ap_running; }

// ---------------------------------------------------------------------------
// WIFI FLOOD — spoofed beacon frames, selectable profile
// ---------------------------------------------------------------------------
#define FLOOD_MAX_SSIDS  50
#define FLOOD_CHANNEL    6
#define MAX_BEACON_LEN   128

static const char *joke_static_ssids[] = {
    "Ultra fast wifi fully real",
    "Johnny Sin's hotspot",
    "M4NT1S",
    "Anal probe connection",
    "Secret Admin Page",
    "Minecraft server WiFi",
    "No noodle all peas",
};
#define JOKE_STATIC_COUNT (sizeof(joke_static_ssids) / sizeof(joke_static_ssids[0]))

static uint8_t  s_frames[FLOOD_MAX_SSIDS][MAX_BEACON_LEN];
static size_t   s_lengths[FLOOD_MAX_SSIDS];
static int      s_flood_count    = 0;
static volatile bool s_flood_running = false;
static TaskHandle_t  s_flood_task    = NULL;

static size_t build_beacon_frame(uint8_t *buf, const char *ssid, uint8_t seq,
                                 uint8_t mac_suffix, uint8_t channel)
{
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;

    size_t i = 0;
    buf[i++] = 0x80; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x00;
    memset(&buf[i], 0xFF, 6); i += 6;
    buf[i++] = 0x02; buf[i++] = 0x00; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x00; buf[i++] = mac_suffix;
    buf[i++] = 0x02; buf[i++] = 0x00; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x00; buf[i++] = mac_suffix;
    buf[i++] = (seq << 4) & 0xF0;
    buf[i++] = 0x00;
    memset(&buf[i], 0, 8); i += 8;
    buf[i++] = 0x64; buf[i++] = 0x00;
    buf[i++] = 0x21; buf[i++] = 0x04;
    buf[i++] = 0x00;
    buf[i++] = (uint8_t)ssid_len;
    memcpy(&buf[i], ssid, ssid_len); i += ssid_len;
    buf[i++] = 0x01; buf[i++] = 0x08;
    buf[i++] = 0x82; buf[i++] = 0x84; buf[i++] = 0x8B; buf[i++] = 0x96;
    buf[i++] = 0x0C; buf[i++] = 0x12; buf[i++] = 0x18; buf[i++] = 0x24;
    buf[i++] = 0x03; buf[i++] = 0x01; buf[i++] = channel;

    return i;
}

static void flood_task(void *arg)
{
    (void)arg;
    while (s_flood_running) {
        for (int i = 0; i < s_flood_count && s_flood_running; i++) {
            esp_wifi_80211_tx(WIFI_IF_STA, s_frames[i], (int)s_lengths[i], false);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    s_flood_task = NULL;
    vTaskDelete(NULL);
}

static void build_free_wifi_profile(void)
{
    int n = 0;
    for (int i = 0; i < 50; i++) {
        char ssid[32];
        snprintf(ssid, sizeof(ssid), "Free-WiFi %d", i + 1);
        s_lengths[n] = build_beacon_frame(s_frames[n], ssid,
                                          (uint8_t)(n & 0x0F),
                                          (uint8_t)(n + 1),
                                          FLOOD_CHANNEL);
        n++;
    }
    s_flood_count = n;
}

static void build_joke_profile(void)
{
    int n = 0;
    for (int i = 0; i < 10; i++) {
        char ssid[32];
        snprintf(ssid, sizeof(ssid), "Mobile Hotspot No Virus %d", i + 1);
        s_lengths[n] = build_beacon_frame(s_frames[n], ssid,
                                          (uint8_t)(n & 0x0F),
                                          (uint8_t)(n + 1),
                                          FLOOD_CHANNEL);
        n++;
    }
    for (int i = 0; i < (int)JOKE_STATIC_COUNT; i++) {
        s_lengths[n] = build_beacon_frame(s_frames[n], joke_static_ssids[i],
                                          (uint8_t)(n & 0x0F),
                                          (uint8_t)(n + 1),
                                          FLOOD_CHANNEL);
        n++;
    }
    s_flood_count = n;
}

esp_err_t beacon_flood_start(beacon_flood_type_t type)
{
    if (s_flood_running) return ESP_OK;

    if (type == BEACON_FLOOD_JOKE) {
        build_joke_profile();
    } else {
        build_free_wifi_profile();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta_cfg = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(FLOOD_CHANNEL, WIFI_SECOND_CHAN_NONE));

    s_flood_running = true;
    xTaskCreate(flood_task, "beacon_flood", 4096, NULL, 5, &s_flood_task);
    return ESP_OK;
}

esp_err_t beacon_flood_stop(void)
{
    if (!s_flood_running) return ESP_OK;
    s_flood_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    return ESP_OK;
}

bool beacon_flood_is_running(void) { return s_flood_running; }

const char *beacon_flood_type_name(beacon_flood_type_t type)
{
    switch (type) {
    case BEACON_FLOOD_FREE_WIFI: return "Free-WiFi";
    case BEACON_FLOOD_JOKE:      return "Joke";
    default:                     return "?";
    }
}

int beacon_flood_type_count(void) { return BEACON_FLOOD_COUNT; }