// main/portal.h
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t portal_init(void);
esp_err_t portal_start(void);
esp_err_t portal_stop(void);
bool      portal_is_running(void);

#ifdef __cplusplus
}
#endif