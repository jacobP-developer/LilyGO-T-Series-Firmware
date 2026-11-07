// main/main.cpp
#include "ui.h"
#include "input.h"
#include "serial_input.h"
#include "wifi_sniffer.h"
#include "ble_spam.h"
#include "beacon.h"
#include "portal.h"
#include "deauth.h"
#include "handshake.h"
#include "sd_card.h"
#include "status_led.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>
#include "lvgl.h"
#include "usb_protocol.h"   

// LovyanGFX — no Arduino wrapper
#include <LovyanGFX.hpp>

static const char *TAG = "main";

/* ---------------------------------------------------------------------------
 * Custom LGFX configuration for the T-Dongle-C5 ST7735 panel.
 *
 * Pin map (confirmed from LilyGO T-Dongle-C5 documentation):
 *   MOSI = GPIO 2
 *   MISO = GPIO 7
 *   SCK  = GPIO 6
 *   CS   = GPIO 23
 *   DC   = GPIO 3
 *   RST  = GPIO 1
 *
 * The panel is a 0.96-inch ST7735 IPS TFT, 80×160.
 * ------------------------------------------------------------------------- */
class LGFX_TDongleC5 : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7735S _panel_instance;
    lgfx::Bus_SPI       _bus_instance;

public:
    LGFX_TDongleC5()
    {
        // --- SPI bus configuration ---
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = 0;
            cfg.freq_write = 26000000;
            cfg.freq_read  = 16000000;
            cfg.spi_3wire  = false;
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk   = 6;
            cfg.pin_mosi   = 2;
            cfg.pin_miso   = 7;
            cfg.pin_dc     = 3;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        // --- Panel configuration ---
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 23;
            cfg.pin_rst          = 1;
            cfg.pin_busy         = -1;

            cfg.panel_width      = 80;
            cfg.panel_height     = 160;
            cfg.memory_width     = 132;
            cfg.memory_height    = 162;

            cfg.offset_x         = 26;
            cfg.offset_y         = 1;

            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = true;
            cfg.rgb_order        = true;   // BGR
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;

            _panel_instance.config(cfg);
        }

        setPanel(&_panel_instance);
    }
};

static LGFX_TDongleC5 display;

/* ---------------------------------------------------------------------------
 * LVGL tick
 * ------------------------------------------------------------------------- */
static esp_timer_handle_t s_lvgl_tick_timer = NULL;

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

/* ---------------------------------------------------------------------------
 * LVGL flush — LovyanGFX pushPixels
 * ------------------------------------------------------------------------- */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    display.startWrite();
    display.setAddrWindow(area->x1, area->y1, w, h);
    display.pushPixels((uint16_t *)px_map, w * h, true);
    display.endWrite();

    lv_display_flush_ready(disp);
}

/* ---------------------------------------------------------------------------
 * Display init
 * ------------------------------------------------------------------------- */
static esp_err_t lcd_init_manual(void)
{
    if (!display.init()) {
        ESP_LOGE(TAG, "display.init() failed");
        return ESP_FAIL;
    }

    // Landscape 160×80 — the panel is natively portrait 80×160.
    display.setRotation(1);

    ESP_LOGI(TAG, "LovyanGFX ST7735 panel ready (160x80 landscape)");
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Radio lifecycle (unchanged)
 * ------------------------------------------------------------------------- */
typedef enum {
    MODE_IDLE, MODE_WIFI_SNIFF, MODE_BLE_SPAM,
    MODE_BEACON_AP, MODE_BEACON_FLOOD, MODE_DEAUTH
} radio_mode_t;

static radio_mode_t active_mode = MODE_IDLE;
static bool deauth_scan_pending = false;

static int      last_ap_count = -1;
static int      last_cl_count = -1;
static uint32_t last_stats_ms = 0;

static uint8_t  s_spoofed_mac[6] = {0};
static uint8_t  s_deauth_target_bssid[6] = {0};
static bool     s_deauth_target_valid = false;

static volatile bool s_radios_ready = false;

static void randomize_base_mac(void)
{
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(esp_random() & 0xFF);
    mac[0] = (mac[0] & 0xFC) | 0x02;
    memcpy(s_spoofed_mac, mac, 6);
    esp_base_mac_addr_set(mac);
}

static void stop_all_radios(void)
{
    if (active_mode == MODE_WIFI_SNIFF)        wifi_sniffer_stop();
    else if (active_mode == MODE_BLE_SPAM)     ble_spam_stop();
    else if (active_mode == MODE_BEACON_AP)    beacon_stop();
    else if (active_mode == MODE_BEACON_FLOOD) beacon_flood_stop();
    else if (active_mode == MODE_DEAUTH)       deauth_stop();
    active_mode = MODE_IDLE;
    s_deauth_target_valid = false;
    last_ap_count = -1;
    last_cl_count = -1;
}

static void ensure_wifi_ready_for_mode(radio_mode_t mode)
{
    if (!s_radios_ready) {
        ui_set_status("INIT...");
        return;
    }

    bool needs_wifi =
        (mode == MODE_WIFI_SNIFF    ||
         mode == MODE_BEACON_AP     ||
         mode == MODE_BEACON_FLOOD  ||
         mode == MODE_DEAUTH);

    if (needs_wifi && !wifi_sniffer_is_initialized()) {
        if (wifi_sniffer_init() != ESP_OK) {
            ui_set_status("WIFI INIT FAIL");
            status_led_flash_error();
        }
    }
}

static void dispatch_module(const char *name)
{
    if (!s_radios_ready) { ui_set_status("INIT..."); return; }

    bool already_active =
        (strcmp(name, "SCAN")       == 0 && active_mode == MODE_WIFI_SNIFF) ||
        (strcmp(name, "BLE SPAM")   == 0 && active_mode == MODE_BLE_SPAM)  ||
        (strcmp(name, "WIFI CLONE") == 0 && active_mode == MODE_BEACON_AP);

    stop_all_radios();
    if (already_active) { ui_set_status("IDLE"); return; }

    if (strcmp(name, "SCAN") == 0) {
        ensure_wifi_ready_for_mode(MODE_WIFI_SNIFF);
        wifi_sniffer_start();
        active_mode = MODE_WIFI_SNIFF;
        ui_set_status("SCANNING");
    } else if (strcmp(name, "BLE SPAM") == 0) {
        ui_set_status("STUB");
    } else if (strcmp(name, "WIFI CLONE") == 0) {
        ensure_wifi_ready_for_mode(MODE_BEACON_AP);
        beacon_start();
        active_mode = MODE_BEACON_AP;
        ui_set_status("AP ON");
    } else if (strcmp(name, "DEAUTH") == 0) {
        ui_set_status("STUB");
    }
}

static void populate_deauth_ui(void)
{
    int n = deauth_network_count();
    const char *names[16];
    int cap = n > 16 ? 16 : n;
    for (int i = 0; i < cap; i++) names[i] = deauth_network_at(i)->ssid;
    ui_set_deauth_list(names, cap);
    ui_set_status("IDLE");
}

static void populate_ble_ui(void)
{
    int n = ble_spam_type_count();
    const char *names[BLE_SPAM_COUNT];
    for (int i = 0; i < n; i++) names[i] = ble_spam_type_name((ble_spam_type_t)i);
    ui_set_ble_list(names, n);
    ui_set_status("IDLE");
}

static void populate_flood_ui(void)
{
    int n = beacon_flood_type_count();
    const char *names[BEACON_FLOOD_COUNT];
    for (int i = 0; i < n; i++) names[i] = beacon_flood_type_name((beacon_flood_type_t)i);
    ui_set_flood_list(names, n);
    ui_set_status("IDLE");
}

static void populate_stats_ui(void)
{
    static char host_buf[48], mac_buf[48], heap_buf[48];
    static char up_buf[48], reset_buf[48], sd_buf[48], scan_buf[48];

    const char *host_name = status_led_is_connected()
                                ? status_led_get_host_name() : "NONE";
    snprintf(host_buf, sizeof(host_buf), "%s", host_name);

    snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             s_spoofed_mac[0], s_spoofed_mac[1], s_spoofed_mac[2],
             s_spoofed_mac[3], s_spoofed_mac[4], s_spoofed_mac[5]);

    size_t free_h = esp_get_free_heap_size();
    size_t large  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    snprintf(heap_buf, sizeof(heap_buf), "%uK free %uK large",
             (unsigned)(free_h / 1024), (unsigned)(large / 1024));

    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(up_buf, sizeof(up_buf), "%02u:%02u:%02u",
             (unsigned)(up_s / 3600),
             (unsigned)((up_s / 60) % 60),
             (unsigned)(up_s % 60));

    const char *rr;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   rr = "POWERON";   break;
    case ESP_RST_SW:        rr = "SW";        break;
    case ESP_RST_PANIC:     rr = "PANIC";     break;
    case ESP_RST_INT_WDT:   rr = "INT_WDT";   break;
    case ESP_RST_TASK_WDT:  rr = "TASK_WDT";  break;
    case ESP_RST_WDT:       rr = "WDT";       break;
    case ESP_RST_BROWNOUT:  rr = "BROWNOUT";  break;
    case ESP_RST_DEEPSLEEP: rr = "DEEPSLEEP"; break;
    case ESP_RST_EXT:       rr = "EXT";       break;
    default:                rr = "UNKNOWN";   break;
    }
    snprintf(reset_buf, sizeof(reset_buf), "%s", rr);
    snprintf(sd_buf, sizeof(sd_buf), "%s",
             sd_card_is_mounted() ? "MOUNTED" : "NONE");

    int aps = 0, cls = 0;
    wifi_sniffer_get_stats(&aps, &cls);
    snprintf(scan_buf, sizeof(scan_buf), "%d APs %d CLs", aps, cls);

    static const char *names_arr[] = {
        "HOST", "SPOOFED MAC", "FREE HEAP", "UPTIME",
        "RESET REASON", "SD CARD", "SCAN TOTALS",
    };
    const char *values_arr[] = {
        host_buf, mac_buf, heap_buf, up_buf, reset_buf, sd_buf, scan_buf,
    };
    ui_set_stats_list(names_arr, values_arr, 7);
}

static void deferred_radios_init(void *arg)
{
    (void)arg;

    if (wifi_sniffer_init() != ESP_OK) ESP_LOGE(TAG, "wifi_sniffer_init failed");
    if (beacon_init()       != ESP_OK) ESP_LOGE(TAG, "beacon_init failed");
    if (portal_init()       != ESP_OK) ESP_LOGE(TAG, "portal_init failed");
    if (deauth_init()       != ESP_OK) ESP_LOGE(TAG, "deauth_init failed");

    s_radios_ready = true;
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("main", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "=== M4NT1S boot start ===");

    lv_init();
    ESP_LOGI(TAG, "lv_init() done");

    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .arg      = NULL,
        .name     = "lvgl_tick"
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_lvgl_tick_timer, 2000));
    ESP_LOGI(TAG, "LVGL tick timer started (2 ms)");

    randomize_base_mac();
    status_led_init();

    esp_reset_reason_t rr = esp_reset_reason();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "reset_reason=%d largest_dma=%uK", (int)rr, (unsigned)(largest / 1024));

    char bootbuf[48];
    snprintf(bootbuf, sizeof(bootbuf), "R=%d L=%uK",
             (int)rr, (unsigned)(largest / 1024));
    ui_set_status(bootbuf);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s — erasing", esp_err_to_name(nvs_ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s — continuing", esp_err_to_name(nvs_ret));
    }

    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(e));
    }

    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(e));
    }

    esp_err_t lcd_ret = lcd_init_manual();
    if (lcd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s — continuing headless",
                 esp_err_to_name(lcd_ret));
    } else {
        lv_display_t *disp = lv_display_create(160, 80);
        if (disp) {
            lv_display_set_flush_cb(disp, lvgl_flush_cb);

            static lv_color_t buf[160 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);

            lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
            lv_display_set_default(disp);

            ui_capture_boot_rotation();
            ui_init();
            ESP_LOGI(TAG, "LCD and UI ready (160x80 landscape)");
        } else {
            ESP_LOGE(TAG, "lv_display_create failed");
        }
    }

    ESP_ERROR_CHECK(input_init());

    serial_input_init();
    usb_protocol_init(); 

    xTaskCreate(deferred_radios_init, "radios_init", 8192, NULL, 1, NULL);

    ESP_LOGI(TAG, "=== boot complete ===");

    input_event_t event;
    while (1) {
        lv_timer_handler();

        status_led_set_mode(active_mode == MODE_IDLE
                                ? LED_MODE_IDLE : LED_MODE_ACTIVE);

        if (active_mode == MODE_WIFI_SNIFF) {
            uint32_t t = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (t - last_stats_ms >= 1000) {
                last_stats_ms = t;
                int ap_c = 0, cl_c = 0;
                wifi_sniffer_get_stats(&ap_c, &cl_c);
                if (ap_c != last_ap_count || cl_c != last_cl_count) {
                    last_ap_count = ap_c;
                    last_cl_count = cl_c;
                    char b[32];
                    snprintf(b, sizeof(b), "APs:%d CL:%d", ap_c, cl_c);
                    ui_set_status(b);
                }
            }
        }

        if (deauth_scan_pending && deauth_scan_done()) {
            deauth_scan_pending = false;
            populate_deauth_ui();
        }

        if (active_mode == MODE_BLE_SPAM && !ble_spam_is_running()) {
            int last = ble_spam_last_error();
            char eb[48];
            snprintf(eb, sizeof(eb), "BLE ERR %d", last ? last : -1);
            ui_set_status(eb);
            status_led_flash_error();
            ui_draw_menu();
            active_mode = MODE_IDLE;
        }

        if (active_mode == MODE_DEAUTH && !deauth_is_running()) {
            bool captured = s_deauth_target_valid &&
                            handshake_is_usable(s_deauth_target_bssid);
            ui_set_status(captured ? "CAPTURED" : "DONE");
            s_deauth_target_valid = false;
            active_mode = MODE_IDLE;
            ui_draw_menu();
        }

        if (input_get_event(&event, pdMS_TO_TICKS(10)) == pdTRUE) {
            switch (event) {
                case INPUT_UP:   ui_move_up();   break;
                case INPUT_DOWN: ui_move_down(); break;

                case INPUT_ENTER: {
                    if (ui_in_stats_submenu()) { populate_stats_ui(); break; }

                    if (!s_radios_ready) {
                        ui_set_status("INIT...");
                        ui_draw_menu();
                        break;
                    }

                    if (ui_in_flood_submenu()) {
                        if (beacon_flood_is_running()) {
                            beacon_flood_stop();
                            active_mode = MODE_IDLE;
                            ui_set_status("IDLE");
                        } else if (ui_flood_count() > 0) {
                            int idx = ui_flood_cursor();
                            ensure_wifi_ready_for_mode(MODE_BEACON_FLOOD);
                            if (beacon_flood_start((beacon_flood_type_t)idx) == ESP_OK) {
                                active_mode = MODE_BEACON_FLOOD;
                                ui_set_status("FLOOD");
                            } else {
                                status_led_flash_error();
                            }
                        }
                        ui_draw_menu();
                        break;
                    }

                    if (ui_in_ble_submenu()) {
                        if (ble_spam_is_running()) {
                            ble_spam_stop();
                            active_mode = MODE_IDLE;
                            ui_set_status("IDLE");
                        } else if (ui_ble_count() > 0) {
                            int idx = ui_ble_cursor();
                            esp_err_t r = ble_spam_start((ble_spam_type_t)idx);
                            if (r == ESP_OK) {
                                active_mode = MODE_BLE_SPAM;
                                ui_set_status("SPAMMING");
                            } else {
                                char eb[48];
                                int last = ble_spam_last_error();
                                snprintf(eb, sizeof(eb), "BLE ERR %d",
                                         last ? last : (int)r);
                                ui_set_status(eb);
                                status_led_flash_error();
                            }
                        }
                        ui_draw_menu();
                        break;
                    }

                    if (ui_in_deauth_submenu()) {
                        if (deauth_is_running()) {
                            deauth_stop();
                            active_mode = MODE_IDLE;
                            s_deauth_target_valid = false;
                            ui_set_status("IDLE");
                        } else if (ui_deauth_count() > 0) {
                            int idx = ui_deauth_cursor();
                            ensure_wifi_ready_for_mode(MODE_DEAUTH);
                            esp_err_t r = deauth_start(idx);
                            if (r == ESP_OK) {
                                const deauth_network_t *n = deauth_network_at(idx);
                                if (n) {
                                    memcpy(s_deauth_target_bssid, n->bssid, 6);
                                    s_deauth_target_valid = true;
                                }
                                active_mode = MODE_DEAUTH;
                                ui_set_status("ATTACKING");
                            } else if (r == ESP_ERR_NOT_SUPPORTED) {
                                ui_set_status("PMF REQ");
                                status_led_flash_error();
                            } else {
                                status_led_flash_error();
                            }
                        }
                        ui_draw_menu();
                        break;
                    }

                    const char *name = ui_get_selected_name();

                    if (name && strcmp(name, "BEACON") == 0 && !ui_in_submenu()) {
                        ui_enter_beacon_submenu(); ui_draw_menu(); break;
                    }

                    if (name && strcmp(name, "DEAUTH") == 0 && !ui_in_submenu()) {
                        ensure_wifi_ready_for_mode(MODE_DEAUTH);
                        stop_all_radios();
                        ui_enter_deauth_submenu();
                        ui_draw_menu();
                        esp_err_t e2 = deauth_scan_start();
                        if (e2 == ESP_OK) deauth_scan_pending = true;
                        else {
                            ui_set_status("SCAN FAIL");
                            status_led_flash_error();
                            ui_draw_menu();
                        }
                        break;
                    }

                    if (name && strcmp(name, "BLE SPAM") == 0 && !ui_in_submenu()) {
                        stop_all_radios();
                        ui_enter_ble_submenu();
                        populate_ble_ui();
                        break;
                    }

                    if (name && strcmp(name, "WIFI FLOOD") == 0) {
                        ensure_wifi_ready_for_mode(MODE_BEACON_FLOOD);
                        stop_all_radios();
                        ui_enter_flood_submenu();
                        populate_flood_ui();
                        break;
                    }

                    if (name && strcmp(name, "STATS") == 0 && !ui_in_submenu()) {
                        stop_all_radios();
                        ui_enter_stats_submenu();
                        populate_stats_ui();
                        break;
                    }

                    if (name) dispatch_module(name);
                    ui_draw_menu();
                    break;
                }

                case INPUT_BACK:
                    stop_all_radios();
                    deauth_scan_pending = false;
                    ui_set_status("IDLE");
                    if (ui_in_submenu()) ui_exit_to_main();
                    ui_draw_menu();
                    break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}