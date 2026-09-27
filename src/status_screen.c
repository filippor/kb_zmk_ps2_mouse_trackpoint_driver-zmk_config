#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <lvgl.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/display/widgets/output_status.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/split/central.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

static struct zmk_widget_output_status output_status_widget;
static struct zmk_widget_layer_status layer_status_widget;
static lv_obj_t *battery_label;
static lv_obj_t *caps_label;

struct batteries_state {
    uint8_t left;
    uint8_t right;
};

static const char *battery_symbol(uint8_t level) {
    if (level > 95) {
        return LV_SYMBOL_BATTERY_FULL;
    } else if (level > 65) {
        return LV_SYMBOL_BATTERY_3;
    } else if (level > 35) {
        return LV_SYMBOL_BATTERY_2;
    } else if (level > 5) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void batteries_update_cb(struct batteries_state state) {
    if (!battery_label) {
        return;
    }
    // Right reads 0 until the peripheral has connected and reported
    lv_label_set_text_fmt(battery_label, "L%s R%s", battery_symbol(state.left),
                          state.right ? battery_symbol(state.right) : "-");
}

static struct batteries_state batteries_get_state(const zmk_event_t *eh) {
    struct batteries_state state = {.left = zmk_battery_state_of_charge()};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev && ev->source == 0) {
        state.right = ev->state_of_charge;
    } else {
        zmk_split_central_get_peripheral_battery_level(0, &state.right);
    }
#endif
    return state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_batteries, struct batteries_state, batteries_update_cb,
                            batteries_get_state)
ZMK_SUBSCRIPTION(widget_batteries, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(widget_batteries, zmk_peripheral_battery_state_changed);
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(caps_word), okay)
static const struct device *const caps_word_dev = DEVICE_DT_GET(DT_NODELABEL(caps_word));
#endif

static bool caps_word_active(void) {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(caps_word), okay)
    // No public getter upstream; first field of its private data is `bool active`
    return *(const bool *)caps_word_dev->data;
#else
    return false;
#endif
}

static bool caps_lock_active(void) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    return zmk_hid_indicators_get_current_profile() & BIT(HID_USAGE_LED_CAPS_LOCK - 1);
#else
    return false;
#endif
}

static void caps_update_work_cb(struct k_work *work) {
    if (!caps_label) {
        return;
    }

    bool lock = caps_lock_active();
    bool word = caps_word_active();

    lv_label_set_text(caps_label, lock && word ? "CAPS+W" : lock ? "CAPS" : word ? "WORD" : "");
}

static K_WORK_DELAYABLE_DEFINE(caps_update_work, caps_update_work_cb);

static int caps_listener_cb(const zmk_event_t *eh) {
    if (zmk_display_is_initialized()) {
        // Delay so caps word has handled the same event before we sample it
        k_work_reschedule_for_queue(zmk_display_work_q(), &caps_update_work, K_MSEC(20));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(widget_caps, caps_listener_cb);
ZMK_SUBSCRIPTION(widget_caps, zmk_position_state_changed);
ZMK_SUBSCRIPTION(widget_caps, zmk_keycode_state_changed);
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(widget_caps, zmk_hid_indicators_changed);
#endif

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    const lv_font_t *small = lv_theme_get_font_small(screen);

    zmk_widget_output_status_init(&output_status_widget, screen);
    lv_obj_align(zmk_widget_output_status_obj(&output_status_widget), LV_ALIGN_TOP_LEFT, 0, 0);

    battery_label = lv_label_create(screen);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    widget_batteries_init();

    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_layer_status_obj(&layer_status_widget), small,
                               LV_PART_MAIN);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_BOTTOM_LEFT, 0, 0);

    caps_label = lv_label_create(screen);
    lv_obj_set_style_text_font(caps_label, small, LV_PART_MAIN);
    lv_obj_align(caps_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(caps_label, "");

    return screen;
}
