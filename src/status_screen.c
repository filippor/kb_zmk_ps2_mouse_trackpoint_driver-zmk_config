/*
 * Custom status screen for yk_do52pro, on a 32x128 portrait canvas (see
 * display_rotate.h).
 *
 *   layer / lock and macro status / USB + BLE profiles / battery left + right
 *
 * Every field is fixed width and sits at a fixed position, so nothing shifts
 * around as the state changes.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>
#include <widgets/canvas/lv_canvas.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/hid.h>

#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
#include <zmk/dynamic_macros.h>
#include <zmk/events/dynamic_macros_changed.h>
#endif

#include <zmk/behavior_queue.h>
#include <zmk/events/behavior_queue_state_changed.h>

#include "display_rotate.h"

#define LAYER_Y 1
#define RULE_LAYER_Y 14
#define STATUS_ROW_Y(row) (17 + (row) * 11)
#define RULE_STATUS_Y 48
#define STATUS_DIVIDER_H (STATUS_ROW_Y(1) - STATUS_ROW_Y(0) + 10)
#define CONN_ROW_Y(row) (RULE_STATUS_Y + 3 + (row) * 13)
#define RULE_BATT_Y 89
#define BATT_L_Y 92
#define BATT_R_Y (BATT_L_Y + 11)

#define CONN_CELLS (1 + ZMK_BLE_PROFILE_COUNT)

// Two columns per row, plus the narrow L/R gutter on the battery rows.
#define COL_W (CANVAS_W / 2)
#define BATT_PREFIX_W 5
#define BATT_VALUE_X BATT_PREFIX_W
#define BATT_VALUE_W 18
#define BATT_UNIT_X (BATT_VALUE_X + BATT_VALUE_W)
#define BATT_UNIT_W (CANVAS_W - BATT_UNIT_X)
#define CONN_ICON_W 10
#define CONN_ICON_H 10
#define MACRO_ICON_W 17
#define MACRO_ICON_H 10
#define STATUS_DIVIDER_W CANVAS_W
#define MODIFIER_ICON_W CANVAS_W
#define MODIFIER_ICON_H 5
#define MODIFIER_ICON_Y 40
#define MODIFIER_SQUARE_SIZE 5
#define MODIFIER_SQUARE_GAP 1

static lv_obj_t *battery_l_label;
static lv_obj_t *battery_r_label;
static lv_obj_t *battery_l_unit_label;
static lv_obj_t *battery_r_unit_label;
static lv_obj_t *conn_labels[CONN_CELLS];
static lv_obj_t *conn_icons[CONN_CELLS];
static LV_ATTRIBUTE_MEM_ALIGN uint8_t conn_icon_buffers[CONN_CELLS]
    [LV_DRAW_BUF_SIZE(CONN_ICON_W, CONN_ICON_H, LV_COLOR_FORMAT_I1)];
static lv_obj_t *layer_label;
static lv_obj_t *num_lock_label;
static lv_obj_t *caps_lock_label;
static lv_obj_t *macro_recording_label;
static lv_obj_t *macro_status_icon;
static LV_ATTRIBUTE_MEM_ALIGN uint8_t macro_icon_buffer[LV_DRAW_BUF_SIZE(
    MACRO_ICON_W, MACRO_ICON_H, LV_COLOR_FORMAT_I1)];
static lv_obj_t *status_divider_icon;
static LV_ATTRIBUTE_MEM_ALIGN uint8_t status_divider_buffer[LV_DRAW_BUF_SIZE(
    STATUS_DIVIDER_W, STATUS_DIVIDER_H, LV_COLOR_FORMAT_I1)];
static lv_obj_t *modifier_icon;
static LV_ATTRIBUTE_MEM_ALIGN uint8_t modifier_icon_buffer[LV_DRAW_BUF_SIZE(
    MODIFIER_ICON_W, MODIFIER_ICON_H, LV_COLOR_FORMAT_I1)];
static lv_obj_t *caps_word_label;

// LVGL returns NULL when its pool is exhausted; faulting here would kill USB too.
#define SET_TEXT(label, text)                                                                      \
    do {                                                                                           \
        if (label) {                                                                               \
            lv_label_set_text(label, text);                                                        \
        }                                                                                          \
    } while (0)

#define SET_TEXT_FMT(label, ...)                                                                   \
    do {                                                                                           \
        if (label) {                                                                               \
            lv_label_set_text_fmt(label, __VA_ARGS__);                                             \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ battery */

struct battery_state {
    uint8_t central;
    uint8_t peripheral;
    bool peripheral_valid;
};

static void battery_update_cb(struct battery_state state) {
    SET_TEXT_FMT(battery_l_label, "%3u", state.central);
    if (state.peripheral_valid) {
        SET_TEXT_FMT(battery_r_label, "%3u", state.peripheral);
    } else {
        SET_TEXT(battery_r_label, " --");
    }
}

static struct battery_state battery_get_state(const zmk_event_t *eh) {
    struct battery_state state = {
        .central = zmk_battery_state_of_charge(),
        .peripheral = 0,
        .peripheral_valid = false,
    };

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint8_t level = 0;
    if (zmk_split_central_get_peripheral_battery_level(0, &level) == 0) {
        state.peripheral = level;
        state.peripheral_valid = true;
    }
#endif

    return state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_batteries, struct battery_state, battery_update_cb,
                            battery_get_state)
ZMK_SUBSCRIPTION(widget_batteries, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(widget_batteries, zmk_peripheral_battery_state_changed);
#endif

/* ------------------------------------------------------- BLE profiles + USB */

static void draw_icon_pixel(lv_draw_buf_t *buffer, int x, int y) {
    uint8_t *pixel = lv_draw_buf_goto_xy(buffer, x, y);
    *pixel |= BIT(7 - (x & 7));
}

static void draw_status_cross(lv_obj_t *screen) {
    status_divider_icon = lv_canvas_create(screen);
    if (!status_divider_icon) {
        return;
    }

    lv_canvas_set_buffer(status_divider_icon, status_divider_buffer, STATUS_DIVIDER_W,
                         STATUS_DIVIDER_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(status_divider_icon, 0, lv_color32_make(0, 0, 0, 0));
    lv_canvas_set_palette(status_divider_icon, 1,
                          lv_color_to_32(lv_obj_get_style_text_color(screen, LV_PART_MAIN),
                                         LV_OPA_COVER));

    lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(status_divider_icon);
    memset(lv_draw_buf_goto_xy(buffer, 0, 0), 0, buffer->header.stride * STATUS_DIVIDER_H);
    for (int x = 2; x < STATUS_DIVIDER_W; x += 4) {
        draw_icon_pixel(buffer, x, STATUS_ROW_Y(1) - STATUS_ROW_Y(0) - 1);
    }
    for (int y = 1; y < STATUS_DIVIDER_H; y += 4) {
        draw_icon_pixel(buffer, COL_W, y);
    }

    lv_obj_set_pos(status_divider_icon, 0, STATUS_ROW_Y(0));
}

static void set_modifier_icon(zmk_mod_flags_t modifiers) {
    if (!modifier_icon) {
        return;
    }

    lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(modifier_icon);
    memset(lv_draw_buf_goto_xy(buffer, 0, 0), 0, buffer->header.stride * MODIFIER_ICON_H);

    const zmk_mod_flags_t modifier_masks[] = {
        MOD_LGUI | MOD_RGUI,
        MOD_RALT,
        MOD_LALT,
        MOD_LCTL | MOD_RCTL,
        MOD_LSFT | MOD_RSFT,
    };
    const int total_width = 5 * MODIFIER_SQUARE_SIZE + 4 * MODIFIER_SQUARE_GAP;
    const int start_x = (MODIFIER_ICON_W - total_width) / 2;

    for (size_t i = 0; i < ARRAY_SIZE(modifier_masks); i++) {
        const bool pressed = (modifiers & modifier_masks[i]) != 0;
        const int x0 = start_x + i * (MODIFIER_SQUARE_SIZE + MODIFIER_SQUARE_GAP);
        for (int y = 0; y < MODIFIER_SQUARE_SIZE; y++) {
            for (int x = 0; x < MODIFIER_SQUARE_SIZE; x++) {
                if (pressed || x == 0 || x == MODIFIER_SQUARE_SIZE - 1 || y == 0 ||
                    y == MODIFIER_SQUARE_SIZE - 1) {
                    draw_icon_pixel(buffer, x0 + x, y);
                }
            }
        }
    }

    lv_obj_invalidate(modifier_icon);
}

#if IS_ENABLED(CONFIG_ZMK_BLE)

struct output_state {
    uint8_t active_profile;
    bool profile_open[ZMK_BLE_PROFILE_COUNT];
    bool profile_connected[ZMK_BLE_PROFILE_COUNT];
    enum zmk_transport selected_transport;
    enum zmk_transport preferred_transport;
#if IS_ENABLED(CONFIG_ZMK_USB)
    enum zmk_usb_conn_state usb_state;
#endif
};

static void set_conn_icon(size_t index, bool active, bool paired, bool connected, bool selected) {
    lv_obj_t *icon = conn_icons[index];
    if (!icon) {
        return;
    }

    lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(icon);
    memset(lv_draw_buf_goto_xy(buffer, 0, 0), 0, buffer->header.stride * CONN_ICON_H);

    for (int y = 0; y < CONN_ICON_H; y++) {
        for (int x = 0; x < CONN_ICON_W; x++) {
            bool square = selected && (x == 0 || x == 9 || y == 0 || y == 9);
            bool large_circle = active &&
                                ((y == 0 || y == 9) ? (x >= 4 && x <= 5)
                                 : (y == 1 || y == 8) ? (x == 2 || x == 3 || x == 6 || x == 7)
                                 : (y == 2 || y == 7) ? (x == 1 || x == 8)
                                 : (y >= 3 && y <= 6) && (x == 0 || x == 9));
            bool small_circle = paired &&
                                ((y == 2 || y == 7) ? (x >= 3 && x <= 6)
                                 : (y >= 3 && y <= 6) && (x == 2 || x == 7));
            bool dot = connected && x >= 3 && x <= 6 && y >= 3 && y <= 6;

            if (square || large_circle || small_circle || dot) {
                draw_icon_pixel(buffer, x, y);
            }
        }
    }
    lv_obj_invalidate(icon);
}

static void output_update_cb(struct output_state state) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    set_conn_icon(0, state.selected_transport == ZMK_TRANSPORT_USB,
                  state.usb_state != ZMK_USB_CONN_NONE, state.usb_state == ZMK_USB_CONN_HID,
                  state.preferred_transport == ZMK_TRANSPORT_USB);
#endif
    SET_TEXT(conn_labels[0], "U");

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        bool active_profile = state.active_profile == i;
        set_conn_icon(i + 1,
                      state.selected_transport == ZMK_TRANSPORT_BLE && active_profile,
                      !state.profile_open[i], state.profile_connected[i],
                      state.preferred_transport == ZMK_TRANSPORT_BLE && active_profile);
        SET_TEXT_FMT(conn_labels[i + 1], "%u", i + 1);
    }
}

static struct output_state output_get_state(const zmk_event_t *eh) {
    struct output_state state = {
        .active_profile = zmk_ble_active_profile_index(),
        .selected_transport = zmk_endpoint_get_selected().transport,
        .preferred_transport = zmk_endpoint_get_preferred_transport(),
#if IS_ENABLED(CONFIG_ZMK_USB)
        .usb_state = zmk_usb_get_conn_state(),
#endif
    };

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        state.profile_open[i] = zmk_ble_profile_is_open(i);
        state.profile_connected[i] = zmk_ble_profile_is_connected(i);
    }

    return state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_outputs, struct output_state, output_update_cb,
                            output_get_state)
ZMK_SUBSCRIPTION(widget_outputs, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(widget_outputs, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(widget_outputs, zmk_usb_conn_state_changed);
#endif

#endif /* CONFIG_ZMK_BLE */

/* -------------------------------------------------------------------- layer */

struct layer_state {
    zmk_keymap_layer_index_t index;
    const char *name;
};

static void layer_update_cb(struct layer_state state) {
    bool invert = state.name &&
                  (strcmp(state.name, "Game") == 0 || strcmp(state.name, "GaMe") == 0);
    zmk_display_rotate_set_inverted(invert);

    if (state.name && state.name[0] != '\0') {
        SET_TEXT(layer_label, state.name);
    } else {
        SET_TEXT_FMT(layer_label, "L%u", state.index);
    }
}

static struct layer_state layer_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_state){
        .index = index,
        .name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer, struct layer_state, layer_update_cb, layer_get_state)
ZMK_SUBSCRIPTION(widget_layer, zmk_layer_state_changed);

/* ------------------------------------------------- lock states / caps word */

struct caps_state {
    bool caps_word;
    bool caps_lock;
    bool num_lock;
    bool scroll_lock;
    zmk_mod_flags_t modifiers;
};

static bool caps_word_is_active(void) {
    static const struct device *caps_word_dev = NULL;

    if (caps_word_dev == NULL) {
        caps_word_dev = zmk_behavior_get_binding("caps_word");
        if (caps_word_dev == NULL) {
            return false;
        }
    }

    // Mirrors struct behavior_caps_word_data in ZMK core, whose only member is the flag.
    return caps_word_dev->data != NULL && *(const bool *)caps_word_dev->data;
}

static void caps_update_cb(struct caps_state state) {
    SET_TEXT(num_lock_label, state.num_lock ? "BN" : "");
    SET_TEXT(caps_lock_label, state.caps_lock ? "CL" : "");
    SET_TEXT(caps_word_label, state.caps_word ? "CW" : "");
    set_modifier_icon(state.modifiers);
}

static struct caps_state caps_get_state(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    const zmk_hid_indicators_t indicators = zmk_hid_indicators_get_current_profile();
#endif

    return (struct caps_state){
        .caps_word = caps_word_is_active(),
        .modifiers = zmk_hid_get_explicit_mods(),
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        .caps_lock = (indicators & HID_INDICATOR_CAPS_LOCK) != 0,
        .num_lock = (indicators & HID_INDICATOR_NUM_LOCK) != 0,
        .scroll_lock = (indicators & HID_INDICATOR_SCROLL_LOCK) != 0,
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_caps, struct caps_state, caps_update_cb, caps_get_state)
ZMK_SUBSCRIPTION(widget_caps, zmk_position_state_changed);
ZMK_SUBSCRIPTION(widget_caps, zmk_keycode_state_changed);
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(widget_caps, zmk_hid_indicators_changed);
#endif

struct macro_status_state {
    int recording_count;
    bool scheduled;
};

static void macro_status_update_cb(struct macro_status_state state) {
    if (state.recording_count > 0) {
        SET_TEXT_FMT(macro_recording_label, "%d", state.recording_count);
    } else {
        SET_TEXT(macro_recording_label, "");
    }

    if (!macro_status_icon) {
        return;
    }

    lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(macro_status_icon);
    memset(lv_draw_buf_goto_xy(buffer, 0, 0), 0, buffer->header.stride * MACRO_ICON_H);
    for (int y = 0; y < MACRO_ICON_H; y++) {
        for (int x = 0; x < MACRO_ICON_W; x++) {
            bool circle = state.recording_count > 0 &&
                          ((y == 1 || y == 7) ? (x >= 7 && x <= 9)
                           : (y == 2 || y == 6) ? (x >= 6 && x <= 10)
                                                  : (y >= 3 && y <= 5) && (x >= 5 && x <= 11));
            bool play = state.scheduled && x >= 0 && x <= 4 && y >= 1 && y <= 7 &&
                        x <= (y <= 4 ? y - 1 : 7 - y);
            if (circle || play) {
                draw_icon_pixel(buffer, x, y);
            }
        }
    }
    lv_obj_invalidate(macro_status_icon);
}

static struct macro_status_state macro_status_get_state(const zmk_event_t *eh) {
    return (struct macro_status_state){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
        .recording_count = zmk_recording_macro_count(),
#endif
        .scheduled = zmk_behavior_queue_work_is_scheduled(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_macro_status, struct macro_status_state, macro_status_update_cb,
                            macro_status_get_state)
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
ZMK_SUBSCRIPTION(widget_macro_status, zmk_dynamic_macros_changed);
#endif
ZMK_SUBSCRIPTION(widget_macro_status, zmk_behavior_queue_state_changed);

/* ------------------------------------------------------------------- screen */

// Fixed box per field, so a label's contents never drift with its own width.
static lv_obj_t *make_label(lv_obj_t *screen, const lv_font_t *font, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, lv_text_align_t text_align) {
    lv_obj_t *label = lv_label_create(screen);
    if (!label) {
        LOG_ERR("Failed to allocate status screen label");
        return NULL;
    }

    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, text_align, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, "");
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *make_rule(lv_obj_t *screen, lv_coord_t y) {
    lv_obj_t *rule = lv_obj_create(screen);
    if (!rule) {
        LOG_ERR("Failed to allocate status screen rule");
        return NULL;
    }

    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, CANVAS_W, 1);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rule, lv_obj_get_style_text_color(screen, LV_PART_MAIN),
                              LV_PART_MAIN);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, y);
    return rule;
}

lv_obj_t *zmk_display_status_screen(void) {
    zmk_display_rotate_init();

    lv_obj_t *screen = lv_obj_create(NULL);

    // The mono theme puts a 1px border and padding on every object, including the screen.
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    layer_label =
        make_label(screen, &lv_font_montserrat_10, 0, LAYER_Y, CANVAS_W, LV_TEXT_ALIGN_LEFT);
    make_rule(screen, RULE_LAYER_Y);

    draw_status_cross(screen);
    caps_lock_label     = make_label(screen, &lv_font_montserrat_8, 0    , STATUS_ROW_Y(0), COL_W, LV_TEXT_ALIGN_LEFT);
    caps_word_label     = make_label(screen, &lv_font_montserrat_8, COL_W, STATUS_ROW_Y(0), COL_W, LV_TEXT_ALIGN_RIGHT);
    macro_status_icon   = lv_canvas_create(screen);
    if (macro_status_icon) {
        lv_canvas_set_buffer(macro_status_icon, macro_icon_buffer, MACRO_ICON_W, MACRO_ICON_H,
                             LV_COLOR_FORMAT_I1);
        lv_canvas_set_palette(macro_status_icon, 0, lv_color32_make(0, 0, 0, 0));
        lv_canvas_set_palette(macro_status_icon, 1,
                              lv_color_to_32(lv_obj_get_style_text_color(screen, LV_PART_MAIN),
                                             LV_OPA_COVER));
        lv_obj_set_pos(macro_status_icon, 0, STATUS_ROW_Y(1));
    }
    macro_recording_label = make_label(screen, &lv_font_montserrat_8, 12, STATUS_ROW_Y(1), 5, LV_TEXT_ALIGN_LEFT);
    num_lock_label  = make_label(screen, &lv_font_montserrat_8, 17, STATUS_ROW_Y(1), CANVAS_W - 17, LV_TEXT_ALIGN_RIGHT);
    make_rule(screen, RULE_STATUS_Y);

    for (size_t i = 0; i < CONN_CELLS; i++) {
        const bool right = (i % 2) != 0;
        lv_coord_t x = right ? COL_W : 0;
        conn_labels[i] = make_label(screen, &lv_font_montserrat_8, right ? x + CONN_ICON_W : x,
                                    CONN_ROW_Y(i / 2), 6, LV_TEXT_ALIGN_LEFT);
        conn_icons[i] = lv_canvas_create(screen);
        if (conn_icons[i]) {
            lv_canvas_set_buffer(conn_icons[i], conn_icon_buffers[i], CONN_ICON_W, CONN_ICON_H,
                                 LV_COLOR_FORMAT_I1);
            lv_canvas_set_palette(conn_icons[i], 0, lv_color32_make(0, 0, 0, 0));
            lv_canvas_set_palette(conn_icons[i], 1,
                                  lv_color_to_32(lv_obj_get_style_text_color(screen, LV_PART_MAIN),
                                                 LV_OPA_COVER));
            lv_obj_set_pos(conn_icons[i], right ? x : x + 6, CONN_ROW_Y(i / 2));
        }
    }

    make_rule(screen, RULE_BATT_Y);
    lv_obj_t *prefix = make_label(screen, &lv_font_montserrat_10, 0, BATT_L_Y, BATT_PREFIX_W,
                                  LV_TEXT_ALIGN_LEFT);
    SET_TEXT(prefix, "L");
    battery_l_label = make_label(screen, &lv_font_montserrat_10, BATT_VALUE_X, BATT_L_Y,
                                 BATT_VALUE_W, LV_TEXT_ALIGN_RIGHT);
    battery_l_unit_label = make_label(screen, &lv_font_montserrat_8, BATT_UNIT_X, BATT_L_Y,
                                      BATT_UNIT_W, LV_TEXT_ALIGN_LEFT);
    SET_TEXT(battery_l_unit_label, "%");

    prefix = make_label(screen, &lv_font_montserrat_10, 0, BATT_R_Y, BATT_PREFIX_W,
                        LV_TEXT_ALIGN_LEFT);
    SET_TEXT(prefix, "R");
    battery_r_label = make_label(screen, &lv_font_montserrat_10, BATT_VALUE_X, BATT_R_Y,
                                 BATT_VALUE_W, LV_TEXT_ALIGN_RIGHT);
    battery_r_unit_label = make_label(screen, &lv_font_montserrat_8, BATT_UNIT_X, BATT_R_Y,
                                      BATT_UNIT_W, LV_TEXT_ALIGN_LEFT);
    SET_TEXT(battery_r_unit_label, "%");

    modifier_icon = lv_canvas_create(screen);
    if (modifier_icon) {
        lv_canvas_set_buffer(modifier_icon, modifier_icon_buffer, MODIFIER_ICON_W, MODIFIER_ICON_H,
                             LV_COLOR_FORMAT_I1);
        lv_canvas_set_palette(modifier_icon, 0, lv_color32_make(0, 0, 0, 0));
        lv_canvas_set_palette(modifier_icon, 1,
                              lv_color_to_32(lv_obj_get_style_text_color(screen, LV_PART_MAIN),
                                             LV_OPA_COVER));
        lv_obj_set_pos(modifier_icon, 0, MODIFIER_ICON_Y);
    }

    widget_batteries_init();
#if IS_ENABLED(CONFIG_ZMK_BLE)
    widget_outputs_init();
#endif
    widget_layer_init();
    widget_caps_init();
    widget_macro_status_init();

    return screen;
}
