// main/beacon.h
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BEACON_FLOOD_FREE_WIFI = 0,
    BEACON_FLOOD_JOKE,
    BEACON_FLOOD_COUNT
} beacon_flood_type_t;

esp_err_t beacon_init(void);

// WIFI CLONE: connectable open AP + captive portal
esp_err_t beacon_start(void);
esp_err_t beacon_stop(void);
bool      beacon_is_running(void);

// WIFI FLOOD: non-connectable beacon spam, selectable profile
esp_err_t beacon_flood_start(beacon_flood_type_t type);
esp_err_t beacon_flood_stop(void);
bool      beacon_flood_is_running(void);
const char *beacon_flood_type_name(beacon_flood_type_t type);
int         beacon_flood_type_count(void);

#ifdef __cplusplus
}
#endif