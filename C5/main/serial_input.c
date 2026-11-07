// main/serial_input.c
#include "serial_input.h"
#include "input.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_input";

typedef enum {
    ST_IDLE,
    ST_ESC,
    ST_CSI,
} parser_state_t;

static void serial_input_task(void *arg)
{
    (void)arg;
    parser_state_t state = ST_IDLE;
    uint8_t byte;

    while (1) {
        int n = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));

        if (n <= 0) {
            // Idle; reset parser if it was mid-sequence
            if (state != ST_IDLE) state = ST_IDLE;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        switch (state) {
        case ST_IDLE:
            if (byte == 0x1B) {
                state = ST_ESC;
            } else if (byte == '\r' || byte == '\n') {
                input_post_event(INPUT_ENTER);
            }
            // Any other byte ignored
            break;

        case ST_ESC:
            if (byte == '[' || byte == 'O') {
                state = ST_CSI;
            } else {
                // Bare ESC = BACK
                input_post_event(INPUT_BACK);
                state = ST_IDLE;
            }
            break;

        case ST_CSI:
            switch (byte) {
            case 'A': input_post_event(INPUT_UP);   break;
            case 'B': input_post_event(INPUT_DOWN); break;
            case 'C': /* right — reserved */        break;
            case 'D': input_post_event(INPUT_BACK); break;
            default:  break;
            }
            state = ST_IDLE;
            break;
        }
    }
}

esp_err_t serial_input_init(void)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ret = xTaskCreate(serial_input_task, "serial_input",
                                 4096, NULL, 5, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}