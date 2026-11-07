// main/wifi_sniffer.c
#include "wifi_sniffer.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "wifi_sniffer";

static bool sniffing = false;
static bool initialized = false;
static esp_netif_t *sta_netif = NULL;
static esp_timer_handle_t hop_timer = NULL;

// 2.4 GHz: all channels. 5 GHz: UNII-1 + UNII-3 (skip DFS).
static const uint8_t hop_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48,
    149, 153, 157, 161, 165,
};
#define HOP_COUNT (sizeof(hop_channels) / sizeof(hop_channels[0]))
static uint8_t hop_idx = 0;

// ---- scan tables ----
#define SCAN_MAX_APS          64
#define SCAN_MAX_CLIENTS      128
#define SCAN_SSID_MAX         32

// Security bits
#define SEC_OPEN      (1u << 0)
#define SEC_WEP       (1u << 1)
#define SEC_WPA       (1u << 2)
#define SEC_WPA2_PSK  (1u << 3)
#define SEC_WPA2_ENT  (1u << 4)
#define SEC_WPA3_SAE  (1u << 5)
#define SEC_OWE       (1u << 6)
#define SEC_WPS       (1u << 7)
#define SEC_PMF_CAP   (1u << 8)
#define SEC_PMF_REQ   (1u << 9)

#define AP_HIDDEN     (1u << 0)
#define AP_EVIL_TWIN  (1u << 1)
#define CLIENT_ASSOC  (1u << 0)

typedef struct {
    uint8_t  bssid[6];
    char     ssid[SCAN_SSID_MAX + 1];
    uint8_t  ssid_len;
    uint8_t  channel;
    int8_t   rssi_last;
    int8_t   rssi_min;
    int8_t   rssi_max;
    uint16_t security;
    uint16_t capabilities;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint16_t beacon_count;
    uint8_t  flags;
    uint8_t  used;
} scan_ap_t;

typedef struct {
    uint8_t  mac[6];
    uint8_t  bssid[6];
    char     probed_ssid[SCAN_SSID_MAX + 1];
    uint8_t  probed_ssid_len;
    int8_t   rssi;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint16_t frames;
    uint8_t  flags;
    uint8_t  used;
} scan_client_t;

static scan_ap_t     s_aps[SCAN_MAX_APS];
static scan_client_t s_clients[SCAN_MAX_CLIENTS];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---- IE parsing ----
static const uint8_t *find_ie(const uint8_t *ies, size_t len, uint8_t id,
                              size_t *out_len)
{
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t ie_id  = ies[i];
        uint8_t ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) return NULL;
        if (ie_id == id) {
            if (out_len) *out_len = ie_len;
            return &ies[i + 2];
        }
        i += 2 + ie_len;
    }
    return NULL;
}

static void parse_ssid_ie(const uint8_t *ies, size_t len,
                          char *out, uint8_t *out_len)
{
    *out_len = 0;
    out[0] = '\0';
    size_t s_len = 0;
    const uint8_t *ssid = find_ie(ies, len, 0, &s_len);
    if (!ssid || s_len == 0 || s_len > SCAN_SSID_MAX) return;
    bool all_zero = true;
    for (size_t i = 0; i < s_len; i++) {
        if (ssid[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return;
    memcpy(out, ssid, s_len);
    out[s_len] = '\0';
    *out_len = (uint8_t)s_len;
}

static uint16_t classify_rsn(const uint8_t *rsn, size_t len)
{
    if (len < 8) return 0;
    uint16_t sec = 0;
    size_t off = 6;
    if (off + 2 > len) return 0;
    uint16_t pair_count = rsn[off] | (rsn[off + 1] << 8);
    off += 2 + (size_t)pair_count * 4;
    if (off + 2 > len) return sec;
    uint16_t akm_count = rsn[off] | (rsn[off + 1] << 8);
    off += 2;
    for (uint16_t i = 0; i < akm_count && off + 4 <= len; i++) {
        uint8_t akm = rsn[off + 3];
        switch (akm) {
        case 1: case 3: case 5:  sec |= SEC_WPA2_ENT; break;
        case 2: case 4: case 6:  sec |= SEC_WPA2_PSK; break;
        case 8: case 9:          sec |= SEC_WPA3_SAE; break;
        case 18:                 sec |= SEC_OWE;      break;
        }
        off += 4;
    }
    if (off + 2 <= len) {
        uint16_t caps = rsn[off] | (rsn[off + 1] << 8);
        if (caps & 0x0040) sec |= SEC_PMF_CAP;
        if (caps & 0x0080) sec |= SEC_PMF_REQ;
    }
    return sec;
}

static bool find_wpa_ie(const uint8_t *ies, size_t len)
{
    size_t i = 0;
    while (i + 2 + 4 <= len) {
        uint8_t ie_id = ies[i], ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) return false;
        if (ie_id == 221 && ie_len >= 4) {
            const uint8_t *v = &ies[i + 2];
            if (v[0]==0x00 && v[1]==0x50 && v[2]==0xF2 && v[3]==0x01)
                return true;
        }
        i += 2 + ie_len;
    }
    return false;
}

static bool find_wps_ie(const uint8_t *ies, size_t len)
{
    size_t i = 0;
    while (i + 2 + 4 <= len) {
        uint8_t ie_id = ies[i], ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) return false;
        if (ie_id == 221 && ie_len >= 4) {
            const uint8_t *v = &ies[i + 2];
            if (v[0]==0x00 && v[1]==0x50 && v[2]==0xF2 && v[3]==0x04)
                return true;
        }
        i += 2 + ie_len;
    }
    return false;
}

static uint16_t classify_security(const uint8_t *ies, size_t len)
{
    uint16_t sec = 0;
    size_t rsn_len = 0;
    const uint8_t *rsn = find_ie(ies, len, 48, &rsn_len);
    if (rsn) sec |= classify_rsn(rsn, rsn_len);
    if (find_wpa_ie(ies, len)) sec |= SEC_WPA;
    if (find_wps_ie(ies, len)) sec |= SEC_WPS;
    if (sec == 0) sec = SEC_OPEN;
    return sec;
}

// ---- table management ----
static scan_ap_t *find_ap(const uint8_t *bssid)
{
    for (int i = 0; i < SCAN_MAX_APS; i++) {
        if (s_aps[i].used && memcmp(s_aps[i].bssid, bssid, 6) == 0)
            return &s_aps[i];
    }
    return NULL;
}

static scan_ap_t *alloc_ap(void)
{
    for (int i = 0; i < SCAN_MAX_APS; i++) {
        if (!s_aps[i].used) return &s_aps[i];
    }
    scan_ap_t *oldest = &s_aps[0];
    for (int i = 1; i < SCAN_MAX_APS; i++) {
        if (s_aps[i].last_seen_ms < oldest->last_seen_ms)
            oldest = &s_aps[i];
    }
    return oldest;
}

static scan_client_t *find_client(const uint8_t *mac)
{
    for (int i = 0; i < SCAN_MAX_CLIENTS; i++) {
        if (s_clients[i].used && memcmp(s_clients[i].mac, mac, 6) == 0)
            return &s_clients[i];
    }
    return NULL;
}

static scan_client_t *alloc_client(void)
{
    for (int i = 0; i < SCAN_MAX_CLIENTS; i++) {
        if (!s_clients[i].used) return &s_clients[i];
    }
    scan_client_t *oldest = &s_clients[0];
    for (int i = 1; i < SCAN_MAX_CLIENTS; i++) {
        if (s_clients[i].last_seen_ms < oldest->last_seen_ms)
            oldest = &s_clients[i];
    }
    return oldest;
}

// ---- frame handlers ----
static void handle_beacon(const uint8_t *frame, size_t len,
                          int8_t rssi, uint8_t channel)
{
    if (len < 24 + 12) return;
    const uint8_t *bssid = &frame[16];
    if (bssid[0] & 0x01) return;

    const uint8_t *ies = frame + 24 + 12;
    size_t ies_len = len - 24 - 12;

    uint32_t t = now_ms();
    scan_ap_t *ap = find_ap(bssid);
    if (!ap) {
        ap = alloc_ap();
        memset(ap, 0, sizeof(*ap));
        memcpy(ap->bssid, bssid, 6);
        ap->first_seen_ms = t;
        ap->rssi_min = rssi;
        ap->rssi_max = rssi;
        ap->used = true;
    }

    ap->channel = channel;
    ap->rssi_last = rssi;
    if (rssi < ap->rssi_min) ap->rssi_min = rssi;
    if (rssi > ap->rssi_max) ap->rssi_max = rssi;
    ap->last_seen_ms = t;
    ap->beacon_count++;
    ap->capabilities = frame[24 + 10] | (frame[24 + 11] << 8);

    char ssid[SCAN_SSID_MAX + 1];
    uint8_t ssid_len;
    parse_ssid_ie(ies, ies_len, ssid, &ssid_len);

    if (ssid_len > 0) {
        if (ap->ssid_len != ssid_len ||
            memcmp(ap->ssid, ssid, ssid_len) != 0) {
            memcpy(ap->ssid, ssid, ssid_len);
            ap->ssid[ssid_len] = '\0';
            ap->ssid_len = ssid_len;
            ap->flags &= ~AP_HIDDEN;
        }
    } else if (ap->ssid_len == 0) {
        ap->flags |= AP_HIDDEN;
    }

    ap->security = classify_security(ies, ies_len);
}

static void handle_probe_request(const uint8_t *frame, size_t len,
                                 int8_t rssi, uint8_t channel)
{
    (void)channel;
    if (len < 24) return;
    const uint8_t *client_mac = &frame[10];
    if (client_mac[0] & 0x01) return;

    const uint8_t *ies = frame + 24;
    size_t ies_len = len - 24;

    char ssid[SCAN_SSID_MAX + 1];
    uint8_t ssid_len;
    parse_ssid_ie(ies, ies_len, ssid, &ssid_len);

    uint32_t t = now_ms();
    scan_client_t *cl = find_client(client_mac);
    if (!cl) {
        cl = alloc_client();
        memset(cl, 0, sizeof(*cl));
        memcpy(cl->mac, client_mac, 6);
        memset(cl->bssid, 0xFF, 6);
        cl->first_seen_ms = t;
        cl->used = true;
    }
    cl->last_seen_ms = t;
    cl->rssi = rssi;
    cl->frames++;

    if (ssid_len > 0) {
        memcpy(cl->probed_ssid, ssid, ssid_len);
        cl->probed_ssid[ssid_len] = '\0';
        cl->probed_ssid_len = ssid_len;
    }
}

static void handle_assoc_request(const uint8_t *frame, size_t len,
                                 int8_t rssi, uint8_t channel)
{
    (void)channel;
    if (len < 24) return;
    const uint8_t *client_mac = &frame[10];
    const uint8_t *bssid      = &frame[4];
    if (client_mac[0] & 0x01) return;

    uint32_t t = now_ms();
    scan_client_t *cl = find_client(client_mac);
    if (!cl) {
        cl = alloc_client();
        memset(cl, 0, sizeof(*cl));
        memcpy(cl->mac, client_mac, 6);
        cl->first_seen_ms = t;
        cl->used = true;
    }
    memcpy(cl->bssid, bssid, 6);
    cl->flags |= CLIENT_ASSOC;
    cl->last_seen_ms = t;
    cl->rssi = rssi;
    cl->frames++;
}

// ---- promiscuous callback ----
static void wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    size_t len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;

    int8_t  rssi    = pkt->rx_ctrl.rssi;
    uint8_t channel = pkt->rx_ctrl.channel;
    uint8_t subtype = (frame[0] >> 4) & 0x0F;

    switch (subtype) {
    case 8: handle_beacon(frame, len, rssi, channel); break;
    case 5: handle_beacon(frame, len, rssi, channel); break;  // probe resp
    case 4: handle_probe_request(frame, len, rssi, channel); break;
    case 0: handle_assoc_request(frame, len, rssi, channel); break;
    }
}

// ---- timer / lifecycle ----
static void channel_hop_cb(void *arg)
{
    (void)arg;
    if (!sniffing) return;
    hop_idx = (hop_idx + 1) % HOP_COUNT;
    // Some 5GHz channels may be rejected by country policy; ignore errors.
    esp_wifi_set_channel(hop_channels[hop_idx], WIFI_SECOND_CHAN_NONE);
}

esp_err_t wifi_sniffer_rearm(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    esp_err_t e = esp_wifi_set_promiscuous_filter(&filter);
    if (e != ESP_OK) return e;
    return esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);
}

esp_err_t wifi_sniffer_init(void)
{
    if (initialized) return ESP_OK;

    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == NULL) return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) { if (sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif = NULL; } return e; }

    e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e != ESP_OK) { esp_wifi_deinit(); if (sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif = NULL; } return e; }

    e = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (e != ESP_OK) { esp_wifi_deinit(); if (sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif = NULL; } return e; }

    // C5 is dual-band; force AUTO so 5GHz channels are addressable.
    // (Older IDF may not expose this symbol on non-5G chips; safe to ignore.)
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    e = esp_wifi_set_promiscuous_filter(&filter);
    if (e != ESP_OK) { esp_wifi_deinit(); if (sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif = NULL; } return e; }

    e = esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);
    if (e != ESP_OK) { esp_wifi_deinit(); if (sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif = NULL; } return e; }

    if (hop_timer == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = channel_hop_cb,
            .arg      = NULL,
            .name     = "wifi_hop"
        };
        e = esp_timer_create(&timer_args, &hop_timer);
        if (e != ESP_OK) { esp_wifi_deinit(); if (sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif = NULL; } return e; }
    }

    initialized = true;
    return ESP_OK;
}

esp_err_t wifi_sniffer_deinit(void)
{
    if (!initialized) return ESP_OK;
    if (sniffing) wifi_sniffer_stop();
    if (hop_timer) {
        esp_timer_stop(hop_timer);
        esp_timer_delete(hop_timer);
        hop_timer = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();

    if (sta_netif) {
        esp_netif_destroy_default_wifi(sta_netif);
        sta_netif = NULL;
    }

    initialized = false;
    return ESP_OK;
}

bool wifi_sniffer_is_initialized(void) { return initialized; }

void wifi_sniffer_start(void)
{
    if (sniffing) return;
    if (!initialized) {
        if (wifi_sniffer_init() != ESP_OK) return;
    }
    memset(s_aps, 0, sizeof(s_aps));
    memset(s_clients, 0, sizeof(s_clients));

    wifi_sniffer_rearm();

    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
        return;
    }
    e = esp_wifi_set_promiscuous(true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "promiscuous on: %s", esp_err_to_name(e));
        return;
    }
    hop_idx = 0;
    esp_wifi_set_channel(hop_channels[0], WIFI_SECOND_CHAN_NONE);
    esp_timer_start_periodic(hop_timer, 250000);
    sniffing = true;
}

void wifi_sniffer_stop(void)
{
    if (!sniffing) return;
    esp_timer_stop(hop_timer);
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    sniffing = false;
}

void wifi_sniffer_set_channel(uint8_t channel)
{
    // Accept both 2.4 (1–14) and 5 GHz channels
    if (channel < 1) return;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void wifi_sniffer_get_stats(int *ap_count, int *client_count)
{
    int aps = 0, cls = 0;
    for (int i = 0; i < SCAN_MAX_APS; i++)     if (s_aps[i].used)     aps++;
    for (int i = 0; i < SCAN_MAX_CLIENTS; i++) if (s_clients[i].used) cls++;
    if (ap_count)     *ap_count     = aps;
    if (client_count) *client_count = cls;
}