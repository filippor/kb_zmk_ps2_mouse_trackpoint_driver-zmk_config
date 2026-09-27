/*
 * The 128x32 SSD1306 is mounted vertically with what would be its right edge on
 * top. Neither LVGL's software rotation (no I1 case) nor the SSD1306 driver can
 * do that, so LVGL renders a 32x128 canvas and this flush callback rotates it
 * onto the panel.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <lvgl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "display_rotate.h"

// LVGL prefixes indexed draw buffers with a two entry palette.
#define PALETTE_SIZE 8
#define CANVAS_STRIDE ROUND_UP(DIV_ROUND_UP(CANVAS_W, 8), CONFIG_LV_DRAW_BUF_STRIDE_ALIGN)

static const struct device *const panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uint8_t canvas_buf[PALETTE_SIZE + CANVAS_STRIDE * CANVAS_H]
    __aligned(CONFIG_LV_DRAW_BUF_ALIGN);
static uint8_t panel_buf[PANEL_W * PANEL_H / 8];

static bool set_bit_lights_pixel = true;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    ARG_UNUSED(area);

    const uint8_t *src = px_map + PALETTE_SIZE;

    memset(panel_buf, set_bit_lights_pixel ? 0x00 : 0xff, sizeof(panel_buf));

    for (int32_t cy = 0; cy < CANVAS_H; cy++) {
        const uint8_t *row = src + cy * CANVAS_STRIDE;
        const int32_t panel_x = PANEL_W - 1 - cy;

        for (int32_t cx = 0; cx < CANVAS_W; cx++) {
            if (!((row[cx / 8] >> (7 - (cx % 8))) & 1)) {
                continue;
            }

            // Panel is vertically tiled, LSB first: byte = x + (y / 8) * width, bit = y % 8.
            uint8_t *byte = &panel_buf[panel_x + (cx / 8) * PANEL_W];
            if (set_bit_lights_pixel) {
                *byte |= BIT(cx % 8);
            } else {
                *byte &= ~BIT(cx % 8);
            }
        }
    }

    const struct display_buffer_descriptor desc = {
        .buf_size = sizeof(panel_buf),
        .width = PANEL_W,
        .height = PANEL_H,
        .pitch = PANEL_W,
    };

    display_write(panel, 0, 0, &desc, panel_buf);
    lv_display_flush_ready(disp);
}

void zmk_display_rotate_init(void) {
    lv_display_t *disp = lv_display_get_default();

    if (disp == NULL || !device_is_ready(panel)) {
        LOG_ERR("No display to rotate");
        return;
    }

    struct display_capabilities caps;
    display_get_capabilities(panel, &caps);

    if ((caps.screen_info & SCREEN_INFO_MONO_VTILED) == 0 ||
        (caps.screen_info & SCREEN_INFO_MONO_MSB_FIRST) != 0) {
        LOG_ERR("Panel is not LSB-first vertically tiled mono");
        return;
    }

    set_bit_lights_pixel = caps.current_pixel_format == PIXEL_FORMAT_MONO10;

    lv_display_set_resolution(disp, CANVAS_W, CANVAS_H);
    lv_display_set_buffers(disp, canvas_buf, NULL, sizeof(canvas_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);
}
