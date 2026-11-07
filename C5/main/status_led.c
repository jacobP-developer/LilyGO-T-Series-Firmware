// main/status_led.c
#include "status_led.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

// APA102 on the T-Dongle-C5: DI on GPIO5, CI on GPIO4.
#define LED_GPIO_DI            5
#define LED_GPIO_CI            4

#define APA_BRIGHTNESS         3
#define LED_SOFTWARE_BRIGHTNESS_PCT   15
#define HEARTBEAT_TIMEOUT_MS   6000
#define ERROR_FLASH_MS         3000
#define HOST_NAME_MAX_LEN      32

static volatile led_mode_t s_mode            = LED_MODE_IDLE;
static volatile uint32_t   s_last_hb_ms      = 0;
static volatile bool       s_ever_connected  = false;
static volatile uint32_t   s_error_until_ms  = 0;
static volatile bool       s_running         = false;

static char s_host_name[HOST_NAME_MAX_LEN + 1] = "NONE";

static void apa_gpios_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GPIO_DI) | (1ULL << LED_GPIO_CI),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(LED_GPIO_DI, 0);
    gpio_set_level(LED_GPIO_CI, 0);
}

static inline void apa_bit(uint8_t bit)
{
    gpio_set_level(LED_GPIO_DI, bit ? 1 : 0);
    gpio_set_level(LED_GPIO_CI, 1);
    esp_rom_delay_us(1);
    gpio_set_level(LED_GPIO_CI, 0);
}

static void apa_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) apa_bit((b >> i) & 1);
}

static uint8_t scale_channel(uint8_t value)
{
    return (uint8_t)(((uint32_t)value * LED_SOFTWARE_BRIGHTNESS_PCT) / 100);
}

static void apa_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    r = scale_channel(r);
    g = scale_channel(g);
    b = scale_channel(b);

    for (int i = 0; i < 32; i++) apa_bit(0);

    apa_byte((uint8_t)(0xE0 | (APA_BRIGHTNESS & 0x1F)));
    apa_byte(b);
    apa_byte(g);
    apa_byte(r);

    for (int i = 0; i < 32; i++) apa_bit(1);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool currently_connected(void)
{
    if (!s_ever_connected) return false;
    return (now_ms() - s_last_hb_ms) <= HEARTBEAT_TIMEOUT_MS;
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t last_packed = 0xFFFFFFFF;

    while (s_running) {
        uint8_t r = 0, g = 0, b = 0;
        uint32_t now = now_ms();

        if (now < s_error_until_ms) {
            r = 100; g = 0;   b = 0;
        } else if (s_mode == LED_MODE_ACTIVE) {
            r = 0;   g = 0;   b = 100;
        } else if (!currently_connected()) {
            r = 80;  g = 80;  b = 0;
        } else {
            r = 0;   g = 80;  b = 0;
        }

        uint32_t packed = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        if (packed != last_packed) {
            apa_set_rgb(r, g, b);
            last_packed = packed;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

esp_err_t status_led_init(void)
{
    apa_gpios_init();
    s_running = true;
    xTaskCreate(led_task, "status_led", 2048, NULL, 3, NULL);
    return ESP_OK;
}

void status_led_set_mode(led_mode_t mode)
{
    s_mode = mode;
}

void status_led_flash_error(void)
{
    s_error_until_ms = now_ms() + ERROR_FLASH_MS;
}

void status_led_notify_heartbeat(void)
{
    s_last_hb_ms     = now_ms();
    s_ever_connected = true;
}

bool status_led_is_connected(void)
{
    return currently_connected();
}

void status_led_set_host_name(const char *name)
{
    if (!name) return;
    strncpy(s_host_name, name, HOST_NAME_MAX_LEN);
    s_host_name[HOST_NAME_MAX_LEN] = '\0';
}

const char *status_led_get_host_name(void)
{
    return s_host_name;
}