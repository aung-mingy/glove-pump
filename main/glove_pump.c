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
 * Input:
 *   GPIO3  glove sensor, ADC1 channel 3. Voltage divider:
 *          3.3 V --[13 kOhm]-- tap --[R_var]-- GND, so
 *          mv = 3300 * R / (13000 + R)  and  R = 13000 * mv / (3300 - mv).
 *          ~9 kOhm with the glove fully open (~1350 mV), ~20 kOhm fully closed
 *          (~2000 mV).
 *          Must be an ADC1 pin (GPIO0-GPIO4). The ESP32-C3's ADC2 is not
 *          supported by the ADC driver at all — SOC_ADC_DIG_SUPPORTED_UNIT() is
 *          true for unit 0 only, "ADC2 oneshot mode is no longer supported, due
 *          to hardware limitation" — so the A5/GPIO5 pin on a Super Mini cannot
 *          be read, however it is wired.
 *
 * Commands (case-insensitive):
 *   off | suction | compression   go to that state
 *   toggle                        off -> suction -> compression -> off
 *   set gpio 0|1|2 high|low       raw pin access; the two pumps can never be
 *                                 high together, and raising a pump sets the
 *                                 valve to match it
 *   status                        report state, pins and sensor, change nothing
 *
 * Reply: "OK <state> gpio0=<0|1> gpio1=<0|1> gpio2=<0|1> adc=<raw> mv=<mV>
 * r=<ohms>" or "ERR <reason>", where <state> is off | suction | compression |
 * raw. adc/mv/r are -1 if the sensor could not be read, r is capped at
 * 999999 ohms (open circuit / no sensor).
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
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#define PIN_VALVE GPIO_NUM_0
#define PIN_COMP  GPIO_NUM_1
#define PIN_SUCT  GPIO_NUM_2
#define PIN_SENSE GPIO_NUM_3

/* Sensor front end. ADC1 only: the C3's ADC2 is not supported by the ADC driver
 * (see the file header), so GPIO5/A5 is unusable even though the board labels it
 * "A5". GPIO0-GPIO4 are the ADC1 pins, and three of them are the pump outputs. */
#define SENSE_UNIT        ADC_UNIT_1
#define SENSE_CHANNEL     ADC_CHANNEL_3
#define SENSE_ATTEN       ADC_ATTEN_DB_12      /* the tap sits at 1.35-2.0 V */
#define SENSE_SERIES_OHMS 13000                /* the fixed resistor */
#define SENSE_VCC_MV      3300                 /* the 3.3 V rail, as wired */
#define SENSE_SAMPLES     8                    /* average: the ADC is noisy */
#define SENSE_OPEN_OHMS   999999               /* cap for "no sensor"/open */

/* Make the two wiring mistakes that cost an evening into build errors: an ADC
 * unit the driver doesn't support (silently reports r=-1 on the real chip), and
 * a sensor pin shared with a pump output (the pad would be driven, and
 * adc_oneshot_config_channel() would release it and leave it floating). */
#if !SOC_ADC_DIG_SUPPORTED_UNIT(SENSE_UNIT)
#error "SENSE_UNIT is not supported by the ADC driver on this target"
#endif
_Static_assert(PIN_SENSE != PIN_VALVE && PIN_SENSE != PIN_COMP && PIN_SENSE != PIN_SUCT,
               "the sensor and a pump cannot share a pin");

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

/* --- glove sensor: 3.3 V --[13 kOhm]-- tap --[R_var]-- GND, tap on GPIO3 --- */

static adc_oneshot_unit_handle_t sense_adc;
static adc_cali_handle_t sense_cali;
static bool sense_ready;
static bool sense_calibrated;

static void sense_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {
        .unit_id = SENSE_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit, &sense_adc);
    if (err != ESP_OK) {
        /* Name the error: "adc unit not supported" here means the pin is on an
         * ADC unit this chip's driver refuses (on the C3, that's ADC2 = GPIO5). */
        ESP_LOGW(TAG, "sensor: adc unit %d unavailable (%s) — reporting r=-1",
                 SENSE_UNIT + 1, esp_err_to_name(err));
        return;
    }
    adc_oneshot_chan_cfg_t chan = {
        .atten = SENSE_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(sense_adc, SENSE_CHANNEL, &chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sensor: channel %d config failed (%s) — reporting r=-1",
                 SENSE_CHANNEL, esp_err_to_name(err));
        return;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    /* ESP32-C3 uses curve fitting (ESP32 uses line fitting — the scheme header
     * is per-target). Needs the chip's eFuse calibration values. */
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = SENSE_UNIT,
        .chan = SENSE_CHANNEL,
        .atten = SENSE_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &sense_cali) == ESP_OK) {
        sense_calibrated = true;
    } else {
        ESP_LOGW(TAG, "sensor: no eFuse calibration — falling back to "
                      "raw * 3300 / 4095 mV");
    }
#endif
    sense_ready = true;
}

/* The variable resistor, from the tap voltage. Pure arithmetic, so the host test
 * checks it against the wiring: 1350 mV -> 9000 ohms (glove open),
 * 2000 mV -> 20000 ohms (glove closed). */
static int ohms_from_mv(int mv)
{
    if (mv <= 0) {
        return 0;                           /* tap at ground: R ~ 0 */
    }
    if (mv >= SENSE_VCC_MV) {
        return SENSE_OPEN_OHMS;             /* tap at the rail: open circuit */
    }
    long ohms = (long)SENSE_SERIES_OHMS * mv / (SENSE_VCC_MV - mv);
    return ohms > SENSE_OPEN_OHMS ? SENSE_OPEN_OHMS : (int)ohms;
}

/* Average of SENSE_SAMPLES reads. Both outputs are -1 if it can't be read. */
static void sense_read(int *raw_out, int *mv_out)
{
    *raw_out = -1;
    *mv_out = -1;
    if (!sense_ready) {
        return;
    }
    long sum = 0;
    for (int i = 0; i < SENSE_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(sense_adc, SENSE_CHANNEL, &raw) != ESP_OK) {
            return;
        }
        sum += raw;
    }
    int raw = (int)(sum / SENSE_SAMPLES);
    int mv;
    if (sense_calibrated) {
        if (adc_cali_raw_to_voltage(sense_cali, raw, &mv) != ESP_OK) {
            return;
        }
    } else {
        mv = raw * SENSE_VCC_MV / 4095;
    }
    *raw_out = raw;
    *mv_out = mv;
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
    int raw, mv;
    sense_read(&raw, &mv);
    reply("OK %s gpio0=%d gpio1=%d gpio2=%d adc=%d mv=%d r=%d",
          i < 0 ? "raw" : STATES[i].name,
          gpio_get_level(PIN_VALVE), gpio_get_level(PIN_COMP), gpio_get_level(PIN_SUCT),
          raw, mv, mv < 0 ? -1 : ohms_from_mv(mv));
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
    sense_init();

    write_pins(false, false, false);    /* power-on state: off */
    selftest();
    write_pins(false, false, false);
    struct { int raw, mv; } s;
    sense_read(&s.raw, &s.mv);
    ESP_LOGI(TAG, "sensor: adc=%d mv=%d r=%d", s.raw, s.mv,
             s.mv < 0 ? -1 : ohms_from_mv(s.mv));
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
