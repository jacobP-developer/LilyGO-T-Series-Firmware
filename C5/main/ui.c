// main/ui.c
#include "ui.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

typedef enum { SCR_MAIN, SCR_BEACON, SCR_DEAUTH, SCR_BLE, SCR_FLOOD, SCR_STATS } screen_t;

// --- main menu ---------------------------------------------------------------
#define MAIN_COUNT 5
static const char *main_names[MAIN_COUNT] = { "SCAN", "DEAUTH", "BEACON", "BLE SPAM", "STATS" };
static const char *main_art[MAIN_COUNT] = {
    "       |\n     | |\n   | | |\n | | | |\n -------",
    "xx    xx\n xx  xx\n  xxxx\n xx  xx\nxx    xx",
    "   .-.\n  / | \\\n /__|__\\\n   |||\n   |||",
    "    /|\n   / |\n   \\ |\n    \\|\n     |",
    "     /\n    /\n   /\n  /\n_/",
};

// --- beacon submenu ----------------------------------------------------------
#define BEACON_COUNT 2
static const char *beacon_names[BEACON_COUNT] = { "WIFI CLONE", "WIFI FLOOD" };
static const char *beacon_art[BEACON_COUNT] = {
    "  .-.\n /___\\\n  | |\n  | |\n =====",
    ")))  )))\n(((  (((\n)))  )))\n(((  (((\n)))  )))",
};

// --- list screens ------------------------------------------------------------
static char list_names[UI_DEAUTH_MAX][33];
static int  list_count  = 0;
static int  list_cursor = 0;

static char ble_names[UI_BLE_MAX][33];
static int  ble_count  = 0;
static int  ble_cursor = 0;

static char flood_names[UI_FLOOD_MAX][33];
static int  flood_count  = 0;
static int  flood_cursor = 0;

static char stats_names[UI_STATS_MAX][33];
static char stats_values[UI_STATS_MAX][48];
static int  stats_count  = 0;
static int  stats_cursor = 0;

// --- status buffers ----------------------------------------------------------
#define STATUS_LEN 48
static char main_status[MAIN_COUNT][STATUS_LEN]     = { "IDLE", "STUB", "IDLE", "IDLE", "IDLE" };
static char beacon_status[BEACON_COUNT][STATUS_LEN] = { "IDLE", "IDLE" };
static char deauth_status[STATUS_LEN] = "IDLE";
static char ble_status[STATUS_LEN]    = "IDLE";
static char flood_status[STATUS_LEN]  = "IDLE";
static char stats_status[STATUS_LEN]  = "STATS";

// --- state ------------------------------------------------------------------
static screen_t current_screen = SCR_MAIN;
static int main_page = 0, beacon_page = 0;

static lv_obj_t *lbl_left_arrow, *lbl_page_name, *lbl_right_arrow;
static lv_obj_t *lbl_art, *lbl_status;
static lv_obj_t *lbl_list_prev, *lbl_list_curr, *lbl_list_next;
static lv_obj_t *hdr_row, *body_box;   // flex containers: header strip / body area
static bool initialized = false;

static volatile bool s_rotation_pending = false;
static volatile bool s_rotation_wanted  = false;
static lv_display_rotation_t s_boot_rotation = LV_DISPLAY_ROTATION_0;
static bool s_boot_rotation_valid = false;

// helpers --------------------------------------------------------------------
static const char **names(void) { return current_screen == SCR_MAIN ? main_names : beacon_names; }
static const char **art(void)   { return current_screen == SCR_MAIN ? main_art   : beacon_art;   }
static char *status_buf(void) {
    if (current_screen == SCR_MAIN)   return main_status[main_page];
    if (current_screen == SCR_BEACON) return beacon_status[beacon_page];
    if (current_screen == SCR_DEAUTH) return deauth_status;
    if (current_screen == SCR_BLE)    return ble_status;
    if (current_screen == SCR_STATS)  return stats_status;
    return flood_status;
}

static bool list_screen(void) {
    return current_screen == SCR_DEAUTH ||
           current_screen == SCR_BLE    ||
           current_screen == SCR_FLOOD  ||
           current_screen == SCR_STATS;
}

static int list_active_count(void) {
    if (current_screen == SCR_DEAUTH) return list_count;
    if (current_screen == SCR_BLE)    return ble_count;
    if (current_screen == SCR_STATS)  return stats_count;
    return flood_count;
}

static int list_active_cursor(void) {
    if (current_screen == SCR_DEAUTH) return list_cursor;
    if (current_screen == SCR_BLE)    return ble_cursor;
    if (current_screen == SCR_STATS)  return stats_cursor;
    return flood_cursor;
}

static const char *list_name_at(int i) {
    if (i < 0) return NULL;
    if (current_screen == SCR_DEAUTH) return (i < list_count) ? list_names[i] : NULL;
    if (current_screen == SCR_BLE)    return (i < ble_count)  ? ble_names[i]  : NULL;
    if (current_screen == SCR_STATS)  return (i < stats_count)? stats_names[i] : NULL;
    return (i < flood_count) ? flood_names[i] : NULL;
}

// rotation -------------------------------------------------------------------
void ui_capture_boot_rotation(void) {
    if (s_boot_rotation_valid) return;
    lv_display_t *disp = lv_display_get_default();
    if (disp) { s_boot_rotation = lv_display_get_rotation(disp); s_boot_rotation_valid = true; }
}
static void apply_rotation_async(void *arg) {
    (void)arg;
    lv_display_t *disp = lv_display_get_default();
    if (disp && s_boot_rotation_valid) {
        lv_display_rotation_t target = s_rotation_wanted
            ? (lv_display_rotation_t)(((int)s_boot_rotation + 3) % 4) : s_boot_rotation;
        lv_display_set_rotation(disp, target);
    }
    // Clear the flag BEFORE the labels are freed so ui_set_status() cannot
    // touch a dangling pointer if another task gets scheduled in between.
    initialized = false;
    lbl_art = lbl_left_arrow = lbl_page_name = lbl_right_arrow = NULL;
    lbl_list_prev = lbl_list_curr = lbl_list_next = lbl_status = NULL;
    hdr_row = body_box = NULL;
    lv_obj_clean(lv_scr_act());
    ui_init();
    s_rotation_pending = false;
}
void ui_set_rotation(bool portrait) {
    s_rotation_wanted = portrait;
    if (!s_rotation_pending) { s_rotation_pending = true; lv_async_call(apply_rotation_async, NULL); }
}

// status ---------------------------------------------------------------------
void ui_set_status(const char *text) {
    if (!text) return;
    char *buf = status_buf();
    strncpy(buf, text, STATUS_LEN - 1);
    buf[STATUS_LEN - 1] = '\0';
    // `initialized` matters: apply_rotation_async() frees every label and then
    // rebuilds, so lbl_status is briefly dangling.
    if (initialized && lbl_status && !list_screen()) lv_label_set_text(lbl_status, buf);
}

// DEAUTH list ----------------------------------------------------------------
void ui_set_deauth_list(const char **names_in, int count) {
    list_count = count > UI_DEAUTH_MAX ? UI_DEAUTH_MAX : count;
    for (int i = 0; i < list_count; i++) {
        strncpy(list_names[i], names_in[i], 32);
        list_names[i][32] = '\0';
    }
    list_cursor = 0;
    ui_draw_menu();
}
const char *ui_deauth_get(int idx) {
    if (idx < 0 || idx >= list_count) return NULL;
    return list_names[idx];
}
int ui_deauth_count(void)  { return list_count; }
int ui_deauth_cursor(void) { return list_cursor; }

// BLE list -------------------------------------------------------------------
void ui_set_ble_list(const char **names_in, int count) {
    ble_count = count > UI_BLE_MAX ? UI_BLE_MAX : count;
    for (int i = 0; i < ble_count; i++) {
        strncpy(ble_names[i], names_in[i], 32);
        ble_names[i][32] = '\0';
    }
    ble_cursor = 0;
    ui_draw_menu();
}
const char *ui_ble_get(int idx) {
    if (idx < 0 || idx >= ble_count) return NULL;
    return ble_names[idx];
}
int ui_ble_count(void)  { return ble_count; }
int ui_ble_cursor(void) { return ble_cursor; }

// FLOOD list -----------------------------------------------------------------
void ui_set_flood_list(const char **names_in, int count) {
    flood_count = count > UI_FLOOD_MAX ? UI_FLOOD_MAX : count;
    for (int i = 0; i < flood_count; i++) {
        strncpy(flood_names[i], names_in[i], 32);
        flood_names[i][32] = '\0';
    }
    flood_cursor = 0;
    ui_draw_menu();
}
const char *ui_flood_get(int idx) {
    if (idx < 0 || idx >= flood_count) return NULL;
    return flood_names[idx];
}
int ui_flood_count(void)  { return flood_count; }
int ui_flood_cursor(void) { return flood_cursor; }

// STATS list -----------------------------------------------------------------
void ui_set_stats_list(const char **names_in, const char **values_in, int count) {
    stats_count = count > UI_STATS_MAX ? UI_STATS_MAX : count;
    for (int i = 0; i < stats_count; i++) {
        strncpy(stats_names[i], names_in[i], 32);
        stats_names[i][32] = '\0';
        strncpy(stats_values[i], values_in[i], 47);
        stats_values[i][47] = '\0';
    }
    // Clamp cursor to new count without resetting it, so a refresh keeps position.
    if (stats_cursor >= stats_count) stats_cursor = (stats_count > 0) ? stats_count - 1 : 0;
    ui_draw_menu();
}
int ui_stats_count(void)  { return stats_count; }
int ui_stats_cursor(void) { return stats_cursor; }

// ---------------------------------------------------------------------------
// Layout
//
// The panel is 80x160 native and the same UI has to work in both orientations
// (80x160 portrait, 160x80 landscape). Absolute lv_obj_align() positioning is
// what produced the overlap: the 5-line ASCII art block is ~85 px tall at
// montserrat_14, which is more than a landscape screen leaves after the header
// and status rows, and centered labels with no width limit drew straight over
// their neighbours. Everything is now a flex column sized from the real
// display metrics, with fonts and line spacing picked so every block fits the
// space that actually exists.
// ---------------------------------------------------------------------------
#define UI_ART_LINES 5

static const lv_font_t *F_BIG   = &lv_font_montserrat_14;
static const lv_font_t *F_SMALL = &lv_font_montserrat_10;

static int line_h(const lv_font_t *f) { return (int)lv_font_get_line_height(f); }

// Bare container: no style, no padding, no scrolling, flows its children.
static lv_obj_t *ui_make_box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_row(o, 0, 0);
    lv_obj_set_style_pad_column(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// Sizes every block from the current display geometry. Called from ui_init()
// and at the end of ui_draw_menu(), so a rotation rebuild and a live text
// change both end up with a consistent, non-overlapping layout.
static void ui_apply_metrics(void)
{
    if (!initialized || !hdr_row || !body_box) return;

    lv_display_t *disp = lv_display_get_default();
    int w = disp ? (int)lv_display_get_horizontal_resolution(disp) : 80;
    int h = disp ? (int)lv_display_get_vertical_resolution(disp) : 160;

    // Narrow (portrait) screens need the small font for the title: "WIFI
    // FLOOD" at montserrat_14 is wider than the gap between the arrows.
    const lv_font_t *title_f = (w < 120) ? F_SMALL : F_BIG;
    int hdr_h    = line_h(title_f) + 2;
    int status_h = line_h(F_SMALL) + 2;

    lv_obj_set_style_text_font(lbl_page_name,   title_f, 0);
    lv_obj_set_style_text_font(lbl_left_arrow,  title_f, 0);
    lv_obj_set_style_text_font(lbl_right_arrow, title_f, 0);
    lv_obj_set_height(hdr_row, hdr_h);
    lv_obj_set_height(lbl_status, status_h);

    int body_h = h - hdr_h - status_h;
    if (body_h < UI_ART_LINES) body_h = UI_ART_LINES;

    // ASCII art: biggest font that fits, then pull the line spacing in so
    // UI_ART_LINES lines can never exceed the body box.
    const lv_font_t *art_f = F_BIG;
    if (UI_ART_LINES * line_h(art_f) > body_h) art_f = F_SMALL;
    int ls = 0;
    if (UI_ART_LINES * line_h(art_f) > body_h) {
        ls = (body_h / UI_ART_LINES) - line_h(art_f);
        if (ls > 0) ls = 0;
    }
    lv_obj_set_style_text_font(lbl_art, art_f, 0);
    lv_obj_set_style_text_line_space(lbl_art, ls, 0);

    // Three single-line rows (prev / current / next) share the same box.
    const lv_font_t *list_f = (3 * line_h(F_BIG) <= body_h) ? F_BIG : F_SMALL;
    int row_h = line_h(list_f);
    if (3 * row_h > body_h) row_h = body_h / 3;
    lv_obj_t *rows[3] = { lbl_list_prev, lbl_list_curr, lbl_list_next };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_text_font(rows[i], list_f, 0);
        lv_obj_set_height(rows[i], row_h);
    }
}

// init -----------------------------------------------------------------------
void ui_init(void) {
    if (initialized) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Root is a flex column: header / body / status. Nothing is positioned
    // with lv_obj_align() any more, so the rows cannot overlap in either
    // orientation.
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t g_bright = lv_color_make(0, 255, 0);
    lv_color_t g_mid    = lv_color_make(0, 200, 0);
    lv_color_t g_dim    = lv_color_make(0, 120, 0);

    hdr_row = ui_make_box(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(hdr_row, 2, 0);   // breathing room next to the arrows
    lv_obj_set_flex_align(hdr_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    body_box = ui_make_box(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(body_box, 1);
    lv_obj_set_flex_align(body_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_left_arrow = lv_label_create(hdr_row);
    lv_label_set_text(lbl_left_arrow, "<");
    lv_obj_set_style_text_font(lbl_left_arrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_left_arrow, g_mid, 0);

    lbl_page_name = lv_label_create(hdr_row);
    lv_obj_set_style_text_font(lbl_page_name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_page_name, g_bright, 0);
    lv_obj_set_style_text_align(lbl_page_name, LV_TEXT_ALIGN_CENTER, 0);
    // Ellipsise instead of running under the arrows.
    lv_label_set_long_mode(lbl_page_name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(lbl_page_name, 1);

    lbl_right_arrow = lv_label_create(hdr_row);
    lv_label_set_text(lbl_right_arrow, ">");
    lv_obj_set_style_text_font(lbl_right_arrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_right_arrow, g_mid, 0);

    lbl_art = lv_label_create(body_box);
    lv_obj_set_style_text_font(lbl_art, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_art, g_bright, 0);
    lv_obj_set_style_text_align(lbl_art, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(lbl_art, 0, 0);
    lv_obj_set_width(lbl_art, LV_PCT(100));
    // CLIP, not the default WRAP: the art carries its own newlines, and
    // wrapping a 10-character ASCII line in two on an 80 px wide screen is
    // exactly what pushed it into the header and the status row.
    lv_label_set_long_mode(lbl_art, LV_LABEL_LONG_CLIP);

    lbl_list_prev = lv_label_create(body_box);
    lv_obj_set_style_text_font(lbl_list_prev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_list_prev, g_mid, 0);
    lv_obj_set_style_text_align(lbl_list_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_list_prev, LV_PCT(100));
    lv_label_set_long_mode(lbl_list_prev, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(lbl_list_prev, LV_OBJ_FLAG_HIDDEN);

    lbl_list_curr = lv_label_create(body_box);
    lv_obj_set_style_text_font(lbl_list_curr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_list_curr, lv_color_black(), 0);
    lv_obj_set_style_bg_color(lbl_list_curr, g_bright, 0);
    lv_obj_set_style_bg_opa(lbl_list_curr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(lbl_list_curr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_list_curr, LV_PCT(100));
    lv_label_set_long_mode(lbl_list_curr, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(lbl_list_curr, LV_OBJ_FLAG_HIDDEN);

    lbl_list_next = lv_label_create(body_box);
    lv_obj_set_style_text_font(lbl_list_next, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_list_next, g_mid, 0);
    lv_obj_set_style_text_align(lbl_list_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_list_next, LV_PCT(100));
    lv_label_set_long_mode(lbl_list_next, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(lbl_list_next, LV_OBJ_FLAG_HIDDEN);

    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_status, g_dim, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_status, LV_PCT(100));
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);

    initialized = true;
    ui_apply_metrics();
    ui_draw_menu();
}

// draw -----------------------------------------------------------------------
void ui_draw_menu(void) {
    if (!initialized) return;

    if (list_screen()) {
        lv_obj_add_flag(lbl_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_list_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_list_curr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_list_next, LV_OBJ_FLAG_HIDDEN);

        const char *title;
        if      (current_screen == SCR_DEAUTH) title = "DEAUTH";
        else if (current_screen == SCR_BLE)    title = "BLE SPAM";
        else if (current_screen == SCR_STATS)  title = "STATS";
        else                                   title = "WIFI FLOOD";
        lv_label_set_text(lbl_page_name, title);

        int n = list_active_count();
        int c = list_active_cursor();
        if (n == 0) {
            lv_label_set_text(lbl_list_prev, "");
            lv_label_set_text(lbl_list_curr, "(no targets)");
            lv_label_set_text(lbl_list_next, "");
        } else {
            const char *p = list_name_at(c - 1);
            const char *x = list_name_at(c);
            const char *q = list_name_at(c + 1);
            lv_label_set_text(lbl_list_prev, p ? p : "");
            lv_label_set_text(lbl_list_curr, x ? x : "");
            lv_label_set_text(lbl_list_next, q ? q : "");
        }

        char st[80];
        if (current_screen == SCR_STATS && c < stats_count) {
            snprintf(st, sizeof(st), "%s", stats_values[c]);
        } else if (n == 0) {
            snprintf(st, sizeof(st), "%s", status_buf());
        } else {
            snprintf(st, sizeof(st), "%d/%d %s", c + 1, n, status_buf());
        }
        lv_label_set_text(lbl_status, st);
    } else {
        lv_obj_clear_flag(lbl_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_list_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_list_curr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_list_next, LV_OBJ_FLAG_HIDDEN);

        int idx = (current_screen == SCR_MAIN) ? main_page : beacon_page;
        lv_label_set_text(lbl_page_name, names()[idx]);
        lv_label_set_text(lbl_art,       art()[idx]);
        lv_label_set_text(lbl_status,    status_buf());
    }

    // Flex owns the placement now; this only re-picks fonts and row heights
    // for the current orientation.
    ui_apply_metrics();
}

// navigation -----------------------------------------------------------------
int ui_get_cursor(void) {
    if (current_screen == SCR_MAIN)   return main_page;
    if (current_screen == SCR_BEACON) return beacon_page;
    if (current_screen == SCR_DEAUTH) return list_cursor;
    if (current_screen == SCR_BLE)    return ble_cursor;
    if (current_screen == SCR_STATS)  return stats_cursor;
    return flood_cursor;
}
void ui_move_up(void) {
    if (current_screen == SCR_MAIN) {
        main_page = (main_page - 1 + MAIN_COUNT) % MAIN_COUNT;
    } else if (current_screen == SCR_BEACON) {
        beacon_page = (beacon_page - 1 + BEACON_COUNT) % BEACON_COUNT;
    } else if (current_screen == SCR_DEAUTH && list_cursor > 0) {
        list_cursor--;
    } else if (current_screen == SCR_BLE && ble_cursor > 0) {
        ble_cursor--;
    } else if (current_screen == SCR_STATS && stats_cursor > 0) {
        stats_cursor--;
    } else if (current_screen == SCR_FLOOD && flood_cursor > 0) {
        flood_cursor--;
    }
    ui_draw_menu();
}
void ui_move_down(void) {
    if (current_screen == SCR_MAIN) {
        main_page = (main_page + 1) % MAIN_COUNT;
    } else if (current_screen == SCR_BEACON) {
        beacon_page = (beacon_page + 1) % BEACON_COUNT;
    } else if (current_screen == SCR_DEAUTH && list_cursor < list_count - 1) {
        list_cursor++;
    } else if (current_screen == SCR_BLE && ble_cursor < ble_count - 1) {
        ble_cursor++;
    } else if (current_screen == SCR_STATS && stats_cursor < stats_count - 1) {
        stats_cursor++;
    } else if (current_screen == SCR_FLOOD && flood_cursor < flood_count - 1) {
        flood_cursor++;
    }
    ui_draw_menu();
}
const char *ui_get_selected_name(void) {
    if (current_screen == SCR_MAIN)   return main_names[main_page];
    if (current_screen == SCR_BEACON) return beacon_names[beacon_page];
    return NULL;
}

// submenu transitions --------------------------------------------------------
void ui_enter_beacon_submenu(void) { current_screen = SCR_BEACON; beacon_page = 0; }
void ui_enter_deauth_submenu(void) {
    current_screen = SCR_DEAUTH; list_cursor = 0; list_count = 0;
    strncpy(deauth_status, "SCANNING...", STATUS_LEN - 1);
    deauth_status[STATUS_LEN - 1] = '\0';
}
void ui_enter_ble_submenu(void) {
    current_screen = SCR_BLE; ble_cursor = 0; ble_count = 0;
    strncpy(ble_status, "SELECT PAYLOAD", STATUS_LEN - 1);
    ble_status[STATUS_LEN - 1] = '\0';
}
void ui_enter_flood_submenu(void) {
    current_screen = SCR_FLOOD; flood_cursor = 0; flood_count = 0;
    strncpy(flood_status, "SELECT TYPE", STATUS_LEN - 1);
    flood_status[STATUS_LEN - 1] = '\0';
}
void ui_enter_stats_submenu(void) {
    current_screen = SCR_STATS; stats_cursor = 0; stats_count = 0;
    strncpy(stats_status, "STATS", STATUS_LEN - 1);
    stats_status[STATUS_LEN - 1] = '\0';
}
void ui_exit_to_main(void) { current_screen = SCR_MAIN; }

bool ui_in_submenu(void)        { return current_screen != SCR_MAIN; }
bool ui_in_deauth_submenu(void) { return current_screen == SCR_DEAUTH; }
bool ui_in_ble_submenu(void)    { return current_screen == SCR_BLE; }
bool ui_in_flood_submenu(void)  { return current_screen == SCR_FLOOD; }
bool ui_in_stats_submenu(void)  { return current_screen == SCR_STATS; }