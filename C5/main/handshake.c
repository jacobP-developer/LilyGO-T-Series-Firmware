// main/handshake.c
#include "handshake.h"
#include "sd_card.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "handshake";

#define MAX_BSSIDS      4
#define MAX_FRAME_LEN   256
#define MAX_MSGS        4

typedef struct {
    uint8_t  bssid[6];
    bool     used;
    uint8_t  msg_mask;
    uint8_t  frames[MAX_MSGS][MAX_FRAME_LEN];
    uint16_t frame_lens[MAX_MSGS];
    int8_t   rssi;
    uint8_t  channel;
    uint32_t first_seen_us;
} handshake_slot_t;

static handshake_slot_t s_slots[MAX_BSSIDS];

static handshake_slot_t *find_slot(const uint8_t *bssid, bool create)
{
    handshake_slot_t *empty = NULL;
    for (int i = 0; i < MAX_BSSIDS; i++) {
        if (s_slots[i].used &&
            memcmp(s_slots[i].bssid, bssid, 6) == 0) {
            return &s_slots[i];
        }
        if (!s_slots[i].used && empty == NULL) empty = &s_slots[i];
    }
    if (create && empty) {
        memset(empty, 0, sizeof(*empty));
        memcpy(empty->bssid, bssid, 6);
        empty->used = true;
        empty->first_seen_us = (uint32_t)(esp_timer_get_time() / 1000);
        return empty;
    }
    return NULL;
}

static int classify_eapol(const uint8_t *eapol, size_t eapol_len)
{
    if (eapol_len < 4 + 1 + 2) return 0;
    if (eapol[1] != 0x03) return 0;

    uint16_t key_info = ((uint16_t)eapol[5] << 8) | eapol[6];

    bool ack     = (key_info & 0x0080) != 0;
    bool mic     = (key_info & 0x0100) != 0;
    bool install = (key_info & 0x0040) != 0;
    bool secure  = (key_info & 0x0200) != 0;

    if (ack && !mic && !install) return 1;
    if (!ack && mic && !install) return (secure ? 4 : 2);
    if (ack && mic && install) return 3;

    return 0;
}

static const uint8_t *extract_eapol(const uint8_t *frame, size_t len,
                                    size_t *eapol_len)
{
    if (len < 24) return NULL;

    uint8_t fc0 = frame[0];
    uint8_t type    = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;

    if (type != 0x02) return NULL;
    if (frame[1] & 0x40) return NULL;

    size_t hdr = (subtype >= 8) ? 26 : 24;
    if (len < hdr + 8) return NULL;

    const uint8_t *llc = frame + hdr;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03 ||
        llc[3] != 0x00 || llc[4] != 0x00 || llc[5] != 0x00 ||
        llc[6] != 0x88 || llc[7] != 0x8E) {
        return NULL;
    }

    *eapol_len = len - (hdr + 8);
    return llc + 8;
}

esp_err_t handshake_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    return ESP_OK;
}

void handshake_feed(const uint8_t *frame, size_t len, int8_t rssi, uint8_t channel)
{
    if (!frame || len < 24) return;

    uint8_t bssid_candidates[2][6];
    memcpy(bssid_candidates[0], frame + 16, 6);
    memcpy(bssid_candidates[1], frame + 4, 6);

    for (int c = 0; c < 2; c++) {
        if (bssid_candidates[c][0] & 0x01) continue;

        size_t eapol_len = 0;
        const uint8_t *eapol = extract_eapol(frame, len, &eapol_len);
        if (!eapol) return;

        int msg = classify_eapol(eapol, eapol_len);
        if (msg < 1 || msg > 4) return;

        handshake_slot_t *slot = find_slot(bssid_candidates[c], true);
        if (!slot) return;

        int idx = msg - 1;
        if (slot->msg_mask & (1 << idx)) return;

        size_t copy = len;
        if (copy > MAX_FRAME_LEN) copy = MAX_FRAME_LEN;
        memcpy(slot->frames[idx], frame, copy);
        slot->frame_lens[idx] = (uint16_t)copy;
        slot->msg_mask |= (1 << idx);
        slot->rssi = rssi;
        slot->channel = channel;
        return;
    }
}

bool handshake_is_usable(const uint8_t *bssid)
{
    handshake_slot_t *slot = find_slot(bssid, false);
    if (!slot) return false;
    return (slot->msg_mask & 0x03) == 0x03 ||
           (slot->msg_mask & 0x0C) == 0x0C;
}

uint8_t handshake_message_mask(const uint8_t *bssid)
{
    handshake_slot_t *slot = find_slot(bssid, false);
    return slot ? slot->msg_mask : 0;
}

void handshake_reset(const uint8_t *bssid)
{
    handshake_slot_t *slot = find_slot(bssid, false);
    if (slot) memset(slot, 0, sizeof(*slot));
}

// ---------------------------------------------------------------------------
// PCAP writer — mounts SD lazily on first write.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} pcap_global_header_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcap_packet_header_t;

static const uint8_t radiotap_hdr[8] = {
    0x00, 0x00,
    0x08, 0x00,
    0x00, 0x00, 0x00, 0x00
};

esp_err_t handshake_dump_pcap(const uint8_t *bssid, const char *ssid)
{
    if (!handshake_is_usable(bssid)) return ESP_ERR_NOT_FOUND;

    handshake_slot_t *slot = find_slot(bssid, false);
    if (!slot) return ESP_ERR_NOT_FOUND;

    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(sd_ret));
        return sd_ret;
    }

    mkdir("/sdcard/handshakes", 0777);

    char path[96];
    uint32_t epoch = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(path, sizeof(path),
             "/sdcard/handshakes/%02x%02x%02x%02x%02x%02x_%lu.pcap",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
             (unsigned long)epoch);

    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;

    pcap_global_header_t gh = {
        .magic = 0xA1B2C3D4,
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = 65535,
        .network = 127,
    };
    fwrite(&gh, sizeof(gh), 1, f);

    for (int i = 0; i < MAX_MSGS; i++) {
        if (!(slot->msg_mask & (1 << i))) continue;
        uint32_t total = (uint32_t)(sizeof(radiotap_hdr) + slot->frame_lens[i]);
        pcap_packet_header_t ph = {
            .ts_sec = epoch,
            .ts_usec = 0,
            .incl_len = total,
            .orig_len = total,
        };
        fwrite(&ph, sizeof(ph), 1, f);
        fwrite(radiotap_hdr, sizeof(radiotap_hdr), 1, f);
        fwrite(slot->frames[i], slot->frame_lens[i], 1, f);
    }

    fclose(f);

    ESP_LOGI(TAG, "Handshake saved: %s", path);
    return ESP_OK;
}