// main/font6x10_lvgl.c
#include "font6x10_lvgl.h"
#include "font.h"

// The font6x10 array in font.h is actually 10 COLUMNS wide x 6 ROWS tall,
// column-major: each byte is one column, bits 0..5 of that byte are rows
// 0..5 (bit 0 = top).  Despite the name and the comment, this is NOT 6x10
// row-major.
//
// We render columns 0..5 only (columns 6..9 are unused by every glyph in
// this set), producing a compact 6x6 monospace glyph.
//
// LVGL A1 format wants row-major, MSB-first, padded to a byte boundary:
//   6 pixels per row = 1 byte per row (2 bits padding at the end)
//   6 rows = 6 bytes per glyph

static uint8_t converted[95][6];
static bool    converted_ready = false;

static void convert_all(void)
{
    if (converted_ready) return;
    for (int i = 0; i < 95; i++) {
        for (int row = 0; row < 6; row++) {
            uint8_t byte = 0;
            for (int col = 0; col < 6; col++) {
                if (font6x10[i][col] & (1 << row)) {
                    byte |= (1 << (7 - col));   // MSB = leftmost pixel
                }
            }
            converted[i][row] = byte;
        }
    }
    converted_ready = true;
}

static bool get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out,
                          uint32_t unicode_letter, uint32_t unicode_letter_next)
{
    (void)font; (void)unicode_letter_next;
    if (unicode_letter < 32 || unicode_letter > 126) return false;
    dsc_out->adv_w     = 6;
    dsc_out->box_w     = 6;
    dsc_out->box_h     = 6;
    dsc_out->ofs_x     = 0;
    dsc_out->ofs_y     = 0;
    dsc_out->format    = LV_FONT_GLYPH_FORMAT_A1;
    dsc_out->gid.index = unicode_letter - 32;
    return true;
}

static const void *get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc,
                                    lv_draw_buf_t *draw_buf)
{
    (void)draw_buf;
    convert_all();
    uint32_t idx = g_dsc->gid.index;
    if (idx >= 95) return NULL;
    return converted[idx];
}

const lv_font_t font6x10_lvgl = {
    .get_glyph_dsc    = get_glyph_dsc,
    .get_glyph_bitmap = get_glyph_bitmap,
    .line_height      = 8,     // 6px glyph + 2px gap between rows
    .base_line        = 6,
};