/*
 * Custom 128x32 status screen for yk_do52pro.
 *
 * Row 1: left/right battery              Row 2: BLE profiles + USB
 * Row 3: active layer + shift / caps lock
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
#include <zmk/events/keycode_state_changed.h>

#define ROW_1_Y 0
#define ROW_2_Y 11
#define ROW_3_Y 21

static lv_obj_t *battery_label;
static lv_obj_t *profile_label;
static lv_obj_t *usb_label;
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
        SET_TEXT_FMT(battery_label, "L%3u%% R%3u%%", state.central, state.peripheral);
    } else {
        SET_TEXT_FMT(battery_label, "L%3u%% R --", state.central);
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
    // Per profile: "<active marker><index><connection marker>"
    // markers: '>' active, '*' connected, '.' paired, '-' unused
    char text[ZMK_BLE_PROFILE_COUNT * 3 + 1] = {};
    size_t offset = 0;

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        char active = (state.selected_transport == ZMK_TRANSPORT_BLE && state.active_profile == i)
                          ? '>'
                          : ' ';
        char status = state.profile_connected[i] ? '*' : (state.profile_open[i] ? '-' : '.');
        offset += snprintf(text + offset, sizeof(text) - offset, "%c%u%c", active, i + 1, status);
    }

    SET_TEXT(profile_label, text);

#if IS_ENABLED(CONFIG_ZMK_USB)
    char usb_status;
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
    SET_TEXT_FMT(usb_label, "%cU%c",
                 state.selected_transport == ZMK_TRANSPORT_USB ? '>' : ' ', usb_status);
#endif
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

/* -------------------------------------------------------- shift / caps lock */

struct caps_state {
    bool shift;
    bool caps_lock;
};

static uint8_t held_shifts = 0;

static void caps_update_cb(struct caps_state state) {
    if (state.shift && state.caps_lock) {
        SET_TEXT(caps_label, "SFT CAPS");
    } else if (state.shift) {
        SET_TEXT(caps_label, "SFT");
    } else if (state.caps_lock) {
        SET_TEXT(caps_label, "CAPS");
    } else {
        SET_TEXT(caps_label, "");
    }
}

static struct caps_state caps_get_state(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);
    if (kc != NULL && kc->usage_page == HID_USAGE_KEY) {
        uint8_t bit = 0;
        if (kc->keycode == HID_USAGE_KEY_KEYBOARD_LEFTSHIFT) {
            bit = BIT(0);
        } else if (kc->keycode == HID_USAGE_KEY_KEYBOARD_RIGHTSHIFT) {
            bit = BIT(1);
        }

        if (bit) {
            if (kc->state) {
                held_shifts |= bit;
            } else {
                held_shifts &= ~bit;
            }
        }
    }

    return (struct caps_state){
        .shift = held_shifts != 0,
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        .caps_lock = (zmk_hid_indicators_get_current_profile() & HID_INDICATOR_CAPS_LOCK) != 0,
#else
        .caps_lock = false,
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_caps, struct caps_state, caps_update_cb, caps_get_state)
ZMK_SUBSCRIPTION(widget_caps, zmk_keycode_state_changed);
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(widget_caps, zmk_hid_indicators_changed);
#endif

/* ------------------------------------------------------------------- screen */

static lv_obj_t *make_label(lv_obj_t *screen, lv_align_t align, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *label = lv_label_create(screen);
    if (!label) {
        LOG_ERR("Failed to allocate status screen label");
        return NULL;
    }

    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_label_set_text(label, "");
    lv_obj_align(label, align, x, y);
    return label;
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    battery_label = make_label(screen, LV_ALIGN_TOP_LEFT, 0, ROW_1_Y);
    profile_label = make_label(screen, LV_ALIGN_TOP_LEFT, 0, ROW_2_Y);
    usb_label = make_label(screen, LV_ALIGN_TOP_RIGHT, 0, ROW_2_Y);
    layer_label = make_label(screen, LV_ALIGN_TOP_LEFT, 0, ROW_3_Y);
    caps_label = make_label(screen, LV_ALIGN_TOP_RIGHT, 0, ROW_3_Y);

    widget_batteries_init();
#if IS_ENABLED(CONFIG_ZMK_BLE)
    widget_outputs_init();
#endif
    widget_layer_init();
    widget_caps_init();

    return screen;
}
