/*
 * Custom status screen for yk_do52pro, on a 32x128 portrait canvas (see
 * display_rotate.h).
 *
 *   battery left / battery right / USB + BT1 / BT2 + BT3 / BT4 + BT5 /
 *   caps lock / caps word / layer
 *
 * Every field is fixed width and sits at a fixed position, so nothing shifts
 * around as the state changes.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>

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

#include "display_rotate.h"

#define BATT_L_Y 1
#define BATT_R_Y 15
#define RULE_CONN_Y 30
#define CONN_ROW_Y(row) (34 + (row) * 13)
#define RULE_STATUS_Y 75
#define STATUS_ROW_Y(row) (79 + (row) * 11)
#define RULE_LAYER_Y 104
#define LAYER_Y 108

#define CONN_CELLS (1 + ZMK_BLE_PROFILE_COUNT)

// Two columns per row, plus the narrow L/R gutter on the battery rows.
#define COL_W (CANVAS_W / 2)
#define BATT_PREFIX_W 6

static lv_obj_t *battery_l_label;
static lv_obj_t *battery_r_label;
static lv_obj_t *conn_labels[CONN_CELLS];
static lv_obj_t *layer_label;
static lv_obj_t *num_lock_label;
static lv_obj_t *caps_lock_label;
static lv_obj_t *scroll_lock_label;
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
    SET_TEXT_FMT(battery_l_label, "%3u%%", state.central);
    if (state.peripheral_valid) {
        SET_TEXT_FMT(battery_r_label, "%3u%%", state.peripheral);
    } else {
        SET_TEXT(battery_r_label, "  --");
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

// Increasing visual weight: unused < paired < connected < selected.
#define SYM_UNUSED ' '
#define SYM_PAIRED 'o'
#define SYM_CONNECTED '*'
#define SYM_SELECTED '#'
#define SYM_SELECTED_UNUSED '-'
#define SYM_SELECTED_PAIRED 'O'

static void output_update_cb(struct output_state state) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    char usb_status;
    if (state.selected_transport == ZMK_TRANSPORT_USB) {
        usb_status = SYM_SELECTED;
    } else if (state.preferred_transport == ZMK_TRANSPORT_USB) {
        usb_status = state.usb_state == ZMK_USB_CONN_NONE ? SYM_SELECTED_UNUSED
                                                         : SYM_SELECTED_PAIRED;
    } else {
        switch (state.usb_state) {
        case ZMK_USB_CONN_HID:
            usb_status = SYM_CONNECTED;
            break;
        case ZMK_USB_CONN_POWERED:
            usb_status = SYM_PAIRED;
            break;
        default:
            usb_status = SYM_UNUSED;
            break;
        }
    }
    SET_TEXT_FMT(conn_labels[0], "U%c", usb_status);
#else
    SET_TEXT_FMT(conn_labels[0], "U%c", SYM_UNUSED);
#endif

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        char status;
        if (state.selected_transport == ZMK_TRANSPORT_BLE && state.active_profile == i) {
            status = SYM_SELECTED;
        } else if (state.preferred_transport == ZMK_TRANSPORT_BLE &&
                   state.active_profile == i) {
            status = state.profile_open[i] ? SYM_SELECTED_UNUSED : SYM_SELECTED_PAIRED;
        } else if (state.profile_connected[i]) {
            status = SYM_CONNECTED;
        } else {
            status = state.profile_open[i] ? SYM_UNUSED : SYM_PAIRED;
        }
        SET_TEXT_FMT(conn_labels[i + 1], "%u%c", i + 1, status);
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
    SET_TEXT(scroll_lock_label, state.scroll_lock ? "SL" : "");
    SET_TEXT(caps_word_label, state.caps_word ? "CW" : "");
}

static struct caps_state caps_get_state(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    const zmk_hid_indicators_t indicators = zmk_hid_indicators_get_current_profile();
#endif

    return (struct caps_state){
        .caps_word = caps_word_is_active(),
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

    lv_obj_t *prefix;

    prefix = make_label(screen, &lv_font_montserrat_10, 0, BATT_L_Y, BATT_PREFIX_W,
                        LV_TEXT_ALIGN_LEFT);
    SET_TEXT(prefix, "L");
    battery_l_label = make_label(screen, &lv_font_montserrat_10, BATT_PREFIX_W, BATT_L_Y,
                                 CANVAS_W - BATT_PREFIX_W, LV_TEXT_ALIGN_RIGHT);

    prefix = make_label(screen, &lv_font_montserrat_10, 0, BATT_R_Y, BATT_PREFIX_W,
                        LV_TEXT_ALIGN_LEFT);
    SET_TEXT(prefix, "R");
    battery_r_label = make_label(screen, &lv_font_montserrat_10, BATT_PREFIX_W, BATT_R_Y,
                                 CANVAS_W - BATT_PREFIX_W, LV_TEXT_ALIGN_RIGHT);

    make_rule(screen, RULE_CONN_Y);

    for (size_t i = 0; i < CONN_CELLS; i++) {
        const bool right = (i % 2) != 0;
        conn_labels[i] = make_label(screen, &lv_font_montserrat_10, right ? COL_W : 0,
                                    CONN_ROW_Y(i / 2), COL_W,
                                    right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
    }

    make_rule(screen, RULE_STATUS_Y);

    num_lock_label =
        make_label(screen, &lv_font_montserrat_8, 0, STATUS_ROW_Y(0), COL_W, LV_TEXT_ALIGN_LEFT);
    caps_lock_label = make_label(screen, &lv_font_montserrat_8, COL_W, STATUS_ROW_Y(0), COL_W,
                                 LV_TEXT_ALIGN_RIGHT);
    scroll_lock_label =
        make_label(screen, &lv_font_montserrat_8, 0, STATUS_ROW_Y(1), COL_W, LV_TEXT_ALIGN_LEFT);
    caps_word_label = make_label(screen, &lv_font_montserrat_8, COL_W, STATUS_ROW_Y(1), COL_W,
                                 LV_TEXT_ALIGN_RIGHT);

    make_rule(screen, RULE_LAYER_Y);

    layer_label =
        make_label(screen, &lv_font_montserrat_10, 0, LAYER_Y, CANVAS_W, LV_TEXT_ALIGN_LEFT);

    widget_batteries_init();
#if IS_ENABLED(CONFIG_ZMK_BLE)
    widget_outputs_init();
#endif
    widget_layer_init();
    widget_caps_init();

    return screen;
}
