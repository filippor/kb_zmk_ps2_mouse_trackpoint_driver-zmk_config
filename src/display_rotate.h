#pragma once

#define PANEL_W 128
#define PANEL_H 32

// The panel is mounted with its right edge up, so the canvas is portrait.
#define CANVAS_W PANEL_H
#define CANVAS_H PANEL_W

/**
 * Switch the default LVGL display to a portrait canvas whose contents get
 * rotated onto the landscape panel on flush. Call before creating any widgets.
 */
void zmk_display_rotate_init(void);
