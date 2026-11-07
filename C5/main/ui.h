// main/ui.h
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);
void ui_draw_menu(void);
int  ui_get_cursor(void);
void ui_move_up(void);
void ui_move_down(void);
const char *ui_get_selected_name(void);

// Submenus
void ui_enter_beacon_submenu(void);
void ui_enter_deauth_submenu(void);
void ui_enter_ble_submenu(void);
void ui_enter_flood_submenu(void);
void ui_enter_stats_submenu(void);
void ui_exit_to_main(void);
bool ui_in_submenu(void);
bool ui_in_deauth_submenu(void);
bool ui_in_ble_submenu(void);
bool ui_in_flood_submenu(void);
bool ui_in_stats_submenu(void);

// DEAUTH list
#define UI_DEAUTH_MAX 16
void        ui_set_deauth_list(const char **names, int count);
const char *ui_deauth_get(int idx);
int         ui_deauth_count(void);
int         ui_deauth_cursor(void);

// BLE list
#define UI_BLE_MAX 8
void        ui_set_ble_list(const char **names, int count);
const char *ui_ble_get(int idx);
int         ui_ble_count(void);
int         ui_ble_cursor(void);

// FLOOD list
#define UI_FLOOD_MAX 4
void        ui_set_flood_list(const char **names, int count);
const char *ui_flood_get(int idx);
int         ui_flood_count(void);
int         ui_flood_cursor(void);

// STATS list
#define UI_STATS_MAX 8
void        ui_set_stats_list(const char **names, const char **values, int count);
int         ui_stats_count(void);
int         ui_stats_cursor(void);

void ui_set_status(const char *text);
void ui_capture_boot_rotation(void);
void ui_set_rotation(bool portrait);

#ifdef __cplusplus
}
#endif