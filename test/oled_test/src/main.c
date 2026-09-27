#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(oled_test, LOG_LEVEL_INF);

#define WIDTH 128
#define HEIGHT 32

static const struct device *const display = DEVICE_DT_GET(DT_NODELABEL(oled));
static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct i2c_dt_spec oled_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(oled));
static uint8_t frame[WIDTH * HEIGHT / 8];

static int oled_cmd(const uint8_t *cmd, size_t len) {
    return i2c_burst_write_dt(&oled_i2c, 0x00, cmd, len);
}

static void try_n(const char *name, const uint8_t *cmd, size_t len, bool single_msg) {
    uint8_t buf[32] = {0x00};
    uint32_t fail = 0;
    int first_err = 0;
    int64_t t0 = k_uptime_get();

    memcpy(buf + 1, cmd, len);
    for (int i = 0; i < 20; i++) {
        int ret = single_msg ? i2c_write_dt(&oled_i2c, buf, len + 1) : oled_cmd(cmd, len);

        if (ret) {
            first_err = first_err ?: ret;
            fail++;
        }
    }
    LOG_INF("%-24s len=%2u fail=%2u/20 err=%d (%lld ms)", name, len, fail, first_err,
            k_uptime_delta(&t0));
}

static void raw_test(void) {
    static const uint8_t nops[16] = {[0 ... 15] = 0xe3};
    static const uint8_t window[] = {0x20, 0x00, 0x21, 0x00, 0x7f, 0x22, 0x00, 0x03};
    static const size_t lens[] = {1, 2, 4, 6, 7, 8, 9, 12, 16};
    static const uint8_t off[] = {0xae};

    oled_cmd(off, 1);

    for (size_t i = 0; i < ARRAY_SIZE(lens); i++) {
        try_n("NOP x len", nops, lens[i], false);
    }

    try_n("window (burst)", window, sizeof(window), false);
    try_n("window (single msg)", window, sizeof(window), true);
    try_n("20 00", window, 2, false);
    try_n("21 00 7f", window + 2, 3, false);
    try_n("22 00 03", window + 5, 3, false);
    try_n("20 00 21 00 7f", window, 5, false);
    try_n("21 00 7f 22 00 03", window + 2, 6, false);

    for (size_t i = 0; i < sizeof(window); i++) {
        char name[16];

        snprintk(name, sizeof(name), "byte %02x", window[i]);
        try_n(name, &window[i], 1, false);
    }
}

static void wait_for_terminal(void) {
    const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;

    for (int i = 0; i < 100 && !dtr; i++) {
        uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr);
        k_msleep(100);
    }
}

static int draw(uint8_t pattern_even, uint8_t pattern_odd) {
    const struct display_buffer_descriptor desc = {
        .buf_size = sizeof(frame),
        .width = WIDTH,
        .height = HEIGHT,
        .pitch = WIDTH,
    };

    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (i & 1) ? pattern_odd : pattern_even;
    }
    return display_write(display, 0, 0, &desc, frame);
}

int main(void) {
    usb_enable(NULL);
    wait_for_terminal();
    LOG_INF("OLED test start");

    // nice!nano v2 external VCC switch
    gpio_pin_configure(gpio0, 13, GPIO_OUTPUT_ACTIVE);
    k_msleep(200);

    raw_test();

    int ret = device_init(display);
    LOG_INF("display init: %d", ret);
    if (ret) {
        return 0;
    }

    display_blanking_off(display);

    static const struct {
        const char *name;
        uint8_t even, odd;
    } patterns[] = {
        {"white", 0xFF, 0xFF},
        {"black", 0x00, 0x00},
        {"checker", 0xAA, 0x55},
        {"stripes", 0xFF, 0x00},
    };
    uint32_t ok = 0, fail = 0;

    for (uint32_t n = 0;; n++) {
        for (size_t p = 0; p < ARRAY_SIZE(patterns); p++) {
            int64_t t0 = k_uptime_get();
            ret = draw(patterns[p].even, patterns[p].odd);
            int64_t dt = k_uptime_delta(&t0);

            ret ? fail++ : ok++;
            LOG_INF("[%u] %-8s ret=%d (%lld ms)  ok=%u fail=%u", n, patterns[p].name, ret, dt,
                    ok, fail);
            k_msleep(1000);
        }
    }
    return 0;
}
