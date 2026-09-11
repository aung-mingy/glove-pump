/*
 * glove-pump — ESP32-C3 pump/valve controller over the native USB serial port.
 *
 * Outputs (high = device enabled):
 *   GPIO0  valve              low  = suction pump -> output
 *                             high = compression pump -> output
 *   GPIO1  compression pump
 *   GPIO2  suction pump
 *
 * Three states. The valve always follows the running pump, so these are the
 * only pump-running combinations that exist:
 *
 *   off          valve 0  comp 0  suct 0
 *   suction      valve 0  comp 0  suct 1
 *   compression  valve 1  comp 1  suct 0
 *
 * Commands (case-insensitive):
 *   off | suction | compression   go to that state
 *   toggle                        off -> suction -> compression -> off
 *   set gpio 0|1|2 high|low       raw pin access; the two pumps can never be
 *                                 high together, and raising a pump sets the
 *                                 valve to match it
 *   status                        report state and pin levels, change nothing
 *
 * Reply: "OK <state> gpio0=<0|1> gpio1=<0|1> gpio2=<0|1>" or "ERR <reason>",
 * where <state> is off | suction | compression | raw.
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

#define PIN_VALVE GPIO_NUM_0
#define PIN_COMP  GPIO_NUM_1
#define PIN_SUCT  GPIO_NUM_2

static const char *TAG = "glove-pump";

typedef struct {
    const char *name;
    bool valve, comp, suct;
} pump_state_t;

static const pump_state_t STATES[] = {
    { "off",         false, false, false },
    { "suction",     false, false, true  },
    { "compression", true,  true,  false },
};
#define N_STATES ((int)(sizeof(STATES) / sizeof(STATES[0])))

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

/* Break-before-make: everything low, then the valve, then the pumps — the valve
 * is never switched while a pump is running, and the two pumps are never both
 * high, not even for microseconds. */
static void write_pins(bool valve, bool comp, bool suct)
{
    gpio_set_level(PIN_VALVE, 0);
    gpio_set_level(PIN_COMP, 0);
    gpio_set_level(PIN_SUCT, 0);
    gpio_set_level(PIN_VALVE, valve);
    gpio_set_level(PIN_COMP, comp);
    gpio_set_level(PIN_SUCT, suct);
}

/* Index into STATES matching the pads, or -1 for a combination the named states
 * don't cover (reachable only by raw "set gpio 0 ..."). */
static int current_state(void)
{
    bool valve = gpio_get_level(PIN_VALVE);
    bool comp = gpio_get_level(PIN_COMP);
    bool suct = gpio_get_level(PIN_SUCT);
    for (int i = 0; i < N_STATES; i++) {
        if (STATES[i].valve == valve && STATES[i].comp == comp && STATES[i].suct == suct) {
            return i;
        }
    }
    return -1;
}

static void status(void)
{
    int i = current_state();
    reply("OK %s gpio0=%d gpio1=%d gpio2=%d", i < 0 ? "raw" : STATES[i].name,
          gpio_get_level(PIN_VALVE), gpio_get_level(PIN_COMP), gpio_get_level(PIN_SUCT));
}

static void go_to_state(int i)
{
    write_pins(STATES[i].valve, STATES[i].comp, STATES[i].suct);
    status();
}

/* Raw pin access, for bench testing. Raising a pump enforces both hardware
 * rules; lowering one is local (the valve keeps its position). */
static void set_pin(int pin, bool high)
{
    if (high && (pin == PIN_COMP || pin == PIN_SUCT)) {
        gpio_set_level(pin == PIN_COMP ? PIN_SUCT : PIN_COMP, 0);
        gpio_set_level(PIN_VALVE, pin == PIN_COMP);   /* valve follows the pump */
    }
    gpio_set_level(pin, high);
}

/* One runnable check on the rules that matter, on real hardware. Walks the
 * named states, then the raw pump path, and checks the pads really land there.
 * Runs in microseconds at boot; delete the call in app_main() if the load must
 * not see a power-on blip. */
static void selftest(void)
{
    for (int i = 0; i < N_STATES; i++) {
        write_pins(STATES[i].valve, STATES[i].comp, STATES[i].suct);
        bool valve = gpio_get_level(PIN_VALVE);
        bool comp = gpio_get_level(PIN_COMP);
        bool suct = gpio_get_level(PIN_SUCT);
        if (valve != STATES[i].valve || comp != STATES[i].comp || suct != STATES[i].suct) {
            ESP_LOGE(TAG, "SELFTEST FAIL %s: got valve=%d comp=%d suct=%d, want %d/%d/%d",
                     STATES[i].name, valve, comp, suct,
                     STATES[i].valve, STATES[i].comp, STATES[i].suct);
            return;
        }
        if (comp && suct) {
            ESP_LOGE(TAG, "SELFTEST FAIL %s: both pumps high", STATES[i].name);
            return;
        }
    }
    for (int pin = PIN_COMP; pin <= PIN_SUCT; pin++) {
        set_pin(pin, true);
        if (gpio_get_level(PIN_COMP) && gpio_get_level(PIN_SUCT)) {
            ESP_LOGE(TAG, "SELFTEST FAIL raw gpio%d: both pumps high", pin);
            return;
        }
    }
    ESP_LOGI(TAG, "selftest PASS — off/suction/compression land on their pins");
}

/* GPIO number, or -1 if the token isn't one. */
static int parse_pin(const char *s)
{
    if (!strcmp(s, "0") || !strcmp(s, "gpio0") || !strcmp(s, "gpio_0")) {
        return PIN_VALVE;
    }
    if (!strcmp(s, "1") || !strcmp(s, "gpio1") || !strcmp(s, "gpio_1")) {
        return PIN_COMP;
    }
    if (!strcmp(s, "2") || !strcmp(s, "gpio2") || !strcmp(s, "gpio_2")) {
        return PIN_SUCT;
    }
    return -1;
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

    for (int i = 0; i < N_STATES; i++) {
        if (!strcmp(tok[0], STATES[i].name)) {
            go_to_state(i);
            return;
        }
    }

    if (!strcmp(tok[0], "toggle")) {
        int i = current_state();
        go_to_state(i < 0 ? 0 : (i + 1) % N_STATES);   /* raw -> off */
        return;
    }
    if (!strcmp(tok[0], "status")) {
        status();
        return;
    }
    if (!strcmp(tok[0], "set") && n == 4 && !strcmp(tok[1], "gpio")) {
        int pin = parse_pin(tok[2]);
        if (pin < 0) {
            reply("ERR pin must be 0, 1 or 2");
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
        set_pin(pin, high);
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
    .pin_bit_mask = (1ULL << PIN_VALVE) | (1ULL << PIN_COMP) | (1ULL << PIN_SUCT),
    .mode = GPIO_MODE_INPUT_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_config(&gpio_pins_cfg));

    console_init();

    write_pins(false, false, false);    /* power-on state: off */
    selftest();
    write_pins(false, false, false);
    ESP_LOGI(TAG, "ready — commands: off | suction | compression | toggle | "
                  "set gpio 0|1|2 high|low | status");
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
