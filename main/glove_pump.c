/*
 * glove-pump — ESP32-C3 dual-GPIO controller over the native USB serial port.
 *
 * GPIO1 / GPIO2 are a complementary pair: exactly one is high at all times.
 * The host sends line commands; every command answers with the resulting state.
 *
 *   toggle                 swap which pin is high
 *   set gpio 1 high        force GPIO1 high (=> GPIO2 low)
 *   set gpio 1 low         force GPIO1 low  (=> GPIO2 high, invariant > intent)
 *   set gpio 2 high|low    same for GPIO2
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

#include "driver/gpio.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_A GPIO_NUM_1
#define PIN_B GPIO_NUM_2

static const char *TAG = "glove-pump";

/* Which pin is currently driven high: 1 => PIN_A, 2 => PIN_B. */
static int high_side = 1;

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

/* Drive exactly one pin high. Break-before-make: both drop first, so the pair
 * is never both-high even for a few microseconds — matters if this ever drives
 * an H-bridge. */
static void drive(int side)
{
    gpio_set_level(PIN_A, 0);
    gpio_set_level(PIN_B, 0);
    gpio_set_level(side == 1 ? PIN_A : PIN_B, 1);
    high_side = side;
}

static void set_high_side(int side)
{
    drive(side);
    status();
}

/* One runnable check on the invariant that matters, on real hardware.
 * Runs in ~microseconds at boot. Toggles the lines a few times, so delete the
 * call in app_main() if the load must not see any blip at power-on. */
static void selftest(void)
{
    for (int i = 0; i < 3; i++) {
        for (int side = 1; side <= 2; side++) {
            drive(side);
            int a = gpio_get_level(PIN_A), b = gpio_get_level(PIN_B);
            if (a == b) {
                ESP_LOGE(TAG, "SELFTEST FAIL side=%d gpio1=%d gpio2=%d", side, a, b);
                return;
            }
        }
    }
    ESP_LOGI(TAG, "selftest PASS gpio1/gpio2 complementary");
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
        set_high_side(high_side == 1 ? 2 : 1);
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
        /* Complementary pair: asking for low on one pin means the other goes
         * high. There is no state where both are low. */
        set_high_side(high ? pin : (pin == 1 ? 2 : 1));
        return;
    }
    reply("ERR unknown command");
}

void app_main(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_A) | (1ULL << PIN_B),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* Accept a bare CR (terminal Enter), a bare LF (scripts) or CRLF. With the
     * CRLF default a lone CR would stall the console read. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);

    drive(1);                   /* known power-on state: GPIO1 high, GPIO2 low */
    selftest();
    drive(1);
    ESP_LOGI(TAG, "ready — commands: toggle | set gpio 1|2 high|low | status");
    status();

    char line[80];
    while (fgets(line, sizeof(line), stdin)) {
        handle(line);
    }

    /* stdin closed: nothing to control any more. Idle instead of spinning. */
    ESP_LOGW(TAG, "console read failed — idling");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
