/*
 * Custom 128x32 status screen for yk_do52pro.
 *
 * Row 1: active layer                      | left/right battery
 * Row 2: USB + BLE profile connection block | caps lock / caps word
 *
 * Every field is fixed width and every connection cell has a fixed x, so
 * nothing shifts around as the state changes.
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

#define ROW_1_Y 0
#define ROW_2_Y 17

// x where the caps indicator starts, so it never shifts with its own width.
#define CAPS_X 98

#define CONN_CELLS (1 + ZMK_BLE_PROFILE_COUNT)

static const lv_coord_t conn_cell_x[] = {0, 16, 32, 48, 64, 80};
BUILD_ASSERT(ARRAY_SIZE(conn_cell_x) == CONN_CELLS);

static lv_obj_t *battery_label;
static lv_obj_t *conn_labels[CONN_CELLS];
static lv_obj_t *layer_label;
static lv_obj_t *caps_label;

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
    if (state.peripheral_valid) {
        SET_TEXT_FMT(battery_label, "%3u %3u", state.central, state.peripheral);
    } else {
        SET_TEXT_FMT(battery_label, "%3u  --", state.central);
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
#if IS_ENABLED(CONFIG_ZMK_USB)
    enum zmk_usb_conn_state usb_state;
#endif
};

static void output_update_cb(struct output_state state) {
    // Each cell is "<U|profile><status>": '<' selected, '*' connected, '.' paired, '-' unused.
#if IS_ENABLED(CONFIG_ZMK_USB)
    char usb_status;
    if (state.selected_transport == ZMK_TRANSPORT_USB) {
        usb_status = '<';
    } else {
        switch (state.usb_state) {
        case ZMK_USB_CONN_HID:
            usb_status = '*';
            break;
        case ZMK_USB_CONN_POWERED:
            usb_status = '.';
            break;
        default:
            usb_status = '-';
            break;
        }
    }
    SET_TEXT_FMT(conn_labels[0], "U%c", usb_status);
#else
    SET_TEXT(conn_labels[0], "U-");
#endif

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        char status;
        if (state.selected_transport == ZMK_TRANSPORT_BLE && state.active_profile == i) {
            status = '<';
        } else {
            status = state.profile_connected[i] ? '*' : (state.profile_open[i] ? '-' : '.');
        }
        SET_TEXT_FMT(conn_labels[i + 1], "%u%c", i + 1, status);
    }
}

static struct output_state output_get_state(const zmk_event_t *eh) {
    struct output_state state = {
        .active_profile = zmk_ble_active_profile_index(),
        .selected_transport = zmk_endpoint_get_selected().transport,
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

#define LAYER_NAME_MAX 6

static void layer_update_cb(struct layer_state state) {
    if (state.name && state.name[0] != '\0') {
        SET_TEXT_FMT(layer_label, "%.*s", LAYER_NAME_MAX, state.name);
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

/* ---------------------------------------------------- caps word / caps lock */

struct caps_state {
    bool caps_word;
    bool caps_lock;
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
    if (state.caps_lock && state.caps_word) {
        SET_TEXT(caps_label, "CA CW");
    } else if (state.caps_lock) {
        SET_TEXT(caps_label, "CAPS");
    } else if (state.caps_word) {
        SET_TEXT(caps_label, "CW");
    } else {
        SET_TEXT(caps_label, "");
    }
}

static struct caps_state caps_get_state(const zmk_event_t *eh) {
    return (struct caps_state){
        .caps_word = caps_word_is_active(),
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        .caps_lock = (zmk_hid_indicators_get_current_profile() & HID_INDICATOR_CAPS_LOCK) != 0,
#else
        .caps_lock = false,
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

static lv_obj_t *make_label(lv_obj_t *screen, const lv_font_t *font, lv_align_t align, lv_coord_t x,
                            lv_coord_t y) {
    lv_obj_t *label = lv_label_create(screen);
    if (!label) {
        LOG_ERR("Failed to allocate status screen label");
        return NULL;
    }

    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_label_set_text(label, "");
    lv_obj_align(label, align, x, y);
    return label;
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    // The mono theme puts a 1px border and padding on every object, including the screen.
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    layer_label = make_label(screen, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 0, ROW_1_Y);
    battery_label = make_label(screen, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, 0, ROW_1_Y);

    for (size_t i = 0; i < CONN_CELLS; i++) {
        conn_labels[i] =
            make_label(screen, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, conn_cell_x[i], ROW_2_Y);
    }

    caps_label = make_label(screen, &lv_font_montserrat_8, LV_ALIGN_TOP_LEFT, CAPS_X, ROW_2_Y);

    widget_batteries_init();
#if IS_ENABLED(CONFIG_ZMK_BLE)
    widget_outputs_init();
#endif
    widget_layer_init();
    widget_caps_init();

    return screen;
}
