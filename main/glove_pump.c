/*
 * glove-pump — ESP32-C3 dual-GPIO controller over the native USB serial port.
 *
 * GPIO1 / GPIO2 may both be low, but never both high. Startup is both low.
 * The host sends line commands; every command answers with the resulting state.
 *
 *   toggle                 next state: both low -> GPIO1 high -> GPIO2 high -> both low
 *   set gpio 1 high        GPIO1 high (GPIO2 is lowered first if it was high)
 *   set gpio 2 high        same for GPIO2
 *   set gpio 1 low         GPIO1 low, GPIO2 untouched (both-low = off)
 *   status                 report state without changing anything
 *
 * Reply: "OK gpio1=<0|1> gpio2=<0|1>" or "ERR <reason>".
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_A GPIO_NUM_1
#define PIN_B GPIO_NUM_2

static const char *TAG = "glove-pump";

static void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void reply(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void status(void)
{
    reply("OK gpio1=%d gpio2=%d", gpio_get_level(PIN_A), gpio_get_level(PIN_B));
}

/* Current state, read from the pads: 0 = both low (off), 1 = GPIO1 high,
 * 2 = GPIO2 high. Both-high is not a state. */
static int state(void)
{
    if (gpio_get_level(PIN_A)) {
        return 1;
    }
    return gpio_get_level(PIN_B) ? 2 : 0;
}

/* Drive to a state. Break-before-make: both drop first, so the pair is never
 * both high — not even for a few microseconds. */
static void drive(int side)
{
    gpio_set_level(PIN_A, 0);
    gpio_set_level(PIN_B, 0);
    if (side == 1) {
        gpio_set_level(PIN_A, 1);
    } else if (side == 2) {
        gpio_set_level(PIN_B, 1);
    }
}

/* One runnable check on the invariant that matters, on real hardware. Walks
 * every state and checks the pads actually land there. Runs in microseconds at
 * boot; delete the call in app_main() if the load must not see a power-on blip. */
static void selftest(void)
{
    static const struct { int side, a, b; } walk[] = {
        { 0, 0, 0 }, { 1, 1, 0 }, { 2, 0, 1 }, { 0, 0, 0 }, { 2, 0, 1 }, { 1, 1, 0 },
    };
    for (size_t i = 0; i < sizeof(walk) / sizeof(walk[0]); i++) {
        drive(walk[i].side);
        int a = gpio_get_level(PIN_A), b = gpio_get_level(PIN_B);
        if (a != walk[i].a || b != walk[i].b) {
            ESP_LOGE(TAG, "SELFTEST FAIL side=%d got gpio1=%d gpio2=%d want %d/%d",
                     walk[i].side, a, b, walk[i].a, walk[i].b);
            return;
        }
    }
    ESP_LOGI(TAG, "selftest PASS gpio1/gpio2 land on every state, never both high");
}

static int parse_pin(const char *s)
{
    if (!strcmp(s, "1") || !strcmp(s, "gpio1") || !strcmp(s, "gpio_1")) {
        return 1;
    }
    if (!strcmp(s, "2") || !strcmp(s, "gpio2") || !strcmp(s, "gpio_2")) {
        return 2;
    }
    return 0;
}

static void handle(char *line)
{
    for (char *p = line; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    char *tok[4];
    int n = 0;
    for (char *p = strtok(line, " \t\r\n"); p && n < 4; p = strtok(NULL, " \t\r\n")) {
        tok[n++] = p;
    }
    if (n == 0) {
        return;                     /* blank line — e.g. half of a CRLF */
    }

    if (!strcmp(tok[0], "toggle")) {
        drive((state() + 1) % 3);       /* off -> GPIO1 -> GPIO2 -> off */
        status();
        return;
    }
    if (!strcmp(tok[0], "status")) {
        status();
        return;
    }
    if (!strcmp(tok[0], "set") && n == 4 && !strcmp(tok[1], "gpio")) {
        int pin = parse_pin(tok[2]);
        if (pin == 0) {
            reply("ERR pin must be 1 or 2");
            return;
        }
        bool high;
        if (!strcmp(tok[3], "high")) {
            high = true;
        } else if (!strcmp(tok[3], "low")) {
            high = false;
        } else {
            reply("ERR level must be high or low");
            return;
        }
        if (high) {
            drive(pin);                 /* raising one drops the other first */
        } else {
            /* Lowering is local: the other pin keeps its level, so both-low
             * (off) is reachable and a lowered pin is a no-op otherwise. */
            gpio_set_level(pin == 1 ? PIN_A : PIN_B, 0);
        }
        status();
        return;
    }
    reply("ERR unknown command");
}

/* Console setup, following ESP-IDF's examples/system/console/advanced for
 * CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG. The driver install is not optional: the
 * no-driver VFS read path polls the RX FIFO once and returns EWOULDBLOCK, so a
 * blocking fgets() sees EOF immediately and the command loop exits at boot. */
static void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();

    /* Bare CR (terminal Enter), bare LF (scripts) and CRLF all end a line. In
     * the default CRLF mode a lone CR stalls: the VFS blocks looking ahead for
     * the LF that never comes. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);

    /* stdin starts non-blocking, which makes the driver read poll rather than
     * wait. Clear it so fgets() actually blocks for the next line. */
    fcntl(fileno(stdin), F_SETFL, 0);
    fcntl(fileno(stdout), F_SETFL, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
}

/* GPIO_MODE_INPUT_OUTPUT, not GPIO_MODE_OUTPUT: an output-only pin has its
 * input buffer disabled (gpio_config -> gpio_input_disable), so
 * gpio_get_level() reads back 0 for ever. The boot selftest and status() report
 * the real pad level, so both directions must be enabled. */
static const gpio_config_t gpio_pins_cfg = {
    .pin_bit_mask = (1ULL << PIN_A) | (1ULL << PIN_B),
    .mode = GPIO_MODE_INPUT_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_config(&gpio_pins_cfg));

    console_init();

    drive(0);                   /* power-on state: both pins low (off) */
    selftest();
    drive(0);
    ESP_LOGI(TAG, "ready — commands: toggle | set gpio 1|2 high|low | status");
    status();

    char line[80];
    while (1) {
        if (fgets(line, sizeof(line), stdin)) {
            handle(line);
            continue;
        }
        /* EOF or read error: drop the error state and wait. A host that closes
         * the port must not leave the pump dead until the next reset. */
        clearerr(stdin);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
