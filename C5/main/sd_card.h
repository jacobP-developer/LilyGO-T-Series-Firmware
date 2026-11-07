// main/sd_card.h
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_card_init(void);   // idempotent — safe to call repeatedly
bool      sd_card_is_mounted(void);

#ifdef __cplusplus
}
#endif