// main/input.c
#include "input.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "input";
static QueueHandle_t input_queue = NULL;

#define DEBOUNCE_MS 50        // Increased from 20
#define LONG_PRESS_MS 1000

static void boot_button_task(void *arg) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_NUM_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Keep internal pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    int last_state = 1;
    int64_t press_start = 0;

    while (1) {
        int state = gpio_get_level(PIN_NUM_BOOT_BUTTON);
        if (state != last_state) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS)); // Debounce
            state = gpio_get_level(PIN_NUM_BOOT_BUTTON);
            if (state != last_state) {
                if (state == 0) { // Pressed
                    press_start = esp_timer_get_time() / 1000;
                } else { // Released
                    int64_t press_duration = (esp_timer_get_time() / 1000) - press_start;
                    input_event_t event;
                    if (press_duration > LONG_PRESS_MS) {
                        event = INPUT_ENTER;
                    } else {
                        event = INPUT_DOWN;
                    }
                    xQueueSend(input_queue, &event, 0);
                }
                last_state = state;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t input_init(void) {
    input_queue = xQueueCreate(10, sizeof(input_event_t));
    if (input_queue == NULL) return ESP_FAIL;
    xTaskCreate(boot_button_task, "boot_button", 2048, NULL, 5, NULL);
    return ESP_OK;
}

BaseType_t input_get_event(input_event_t *event, TickType_t timeout) {
    return xQueueReceive(input_queue, event, timeout);
}

void input_post_event(input_event_t event)
{
    if (input_queue != NULL) {
        xQueueSend(input_queue, &event, 0);
    }
}