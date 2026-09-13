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
 *   GPIO3  glove sensor, ADC1 channel 3 — `A3` on a Super Mini. Voltage divider:
 *          3.3 V --[13 kOhm]-- tap --[R_var]-- GND, so
 *          mv = 3300 * R / (13000 + R)  and  R = 13000 * mv / (3300 - mv).
 *          ~9 kOhm with the glove fully open (~1350 mV), ~20 kOhm fully closed
 *          (~2000 mV).
 *          GPIO4 (`A4`) is the other free ADC1 pin and was tried first: the ADC
 *          read ~600 mV on a node a multimeter measured at 1.5 V. It is `MTMS`,
 *          so the JTAG pad config is the one difference that isn't electrical,
 *          hence A3.
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
 *   hold <ohms> | hold 12k        closed loop: pulse the pumps to keep the
 *   hold off                      sensor near a target the host sets. Any
 *                                 manual command takes the rig back
 *   status                        report state, pins, sensor and hold
 *
 * Reply: "OK <state> gpio0=<0|1> gpio1=<0|1> gpio2=<0|1> adc=<raw> mv=<mV>
 * r=<ohms> hold=<target|off|stalled> err=<signed ohms>" or "ERR <reason>", where
 * <state> is off | suction | compression | raw. adc/mv/r are -1 if the sensor
 * could not be read, r is capped at 999999 ohms (open circuit / no sensor).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
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
#include "freertos/semphr.h"
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
/* On the ESP32-C3, ADC1 channel N is GPIO N (soc/adc_channel.h), so the pin and
 * the channel must move together. Change one and you'd silently read a floating
 * pad: watch for mv=0/mv=3300, which looks exactly like bad wiring.
 * (cast: adc_channel_t vs gpio_num_t are different enums, and IDF builds with
 * -Werror=enum-compare) */
_Static_assert((int)SENSE_UNIT == (int)ADC_UNIT_1 && (int)SENSE_CHANNEL == (int)PIN_SENSE,
               "SENSE_CHANNEL must be the ADC1 channel for PIN_SENSE (C3: channel == GPIO)");

/* Hold: keep the sensor near a target the host sets. The pumps are on/off, so
 * this is a deadband plus dwell times, not PID - the leak does the work in the
 * other direction for free, so only one pump ever fires per target and a small
 * overshoot decays back on its own. All knobs in ms: a bite shorter than the
 * pump's spin-up is wasted, one longer than needed overshoots the band. */
#define HOLD_DEADBAND_OHMS 1000     /* +/- 1 kOhm, as asked for */
#define HOLD_MIN_OHMS      1000     /* sane targets only */
#define HOLD_MAX_OHMS      100000   /* above this the tap is within a few mV of the
                                     * rail, so the divider can't tell you anything */
#define HOLD_MIN_ON_MS     100      /* one bite must move LESS than the band and
                                     * more than the pump's spin-up: this is the
                                     * knob to turn on the real rig */
#define HOLD_MIN_OFF_MS    200      /* rest between bites: no chatter, no valve flap */
#define HOLD_MAX_ON_MS     400      /* one bite at most: caps the overshoot */
#define HOLD_STALL_MS      15000    /* no progress this long -> stop and say so */
#define HOLD_TICK_MS       50

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

/* --- hold: deadband + dwell, one direction at a time ---------------------- */

typedef enum { ACT_OFF, ACT_SUCTION, ACT_COMPRESSION } action_t;

typedef struct {
    int deadband;               /* ohms */
    int min_on_ms;
    int min_off_ms;
    int max_on_ms;
} hold_cfg_t;

typedef struct {
    action_t action;            /* what the pumps are doing now */
    long action_started_ms;     /* when action last became non-OFF */
    long last_change_ms;        /* when action last changed at all */
} hold_state_t;

/* Pure: no I/O, no clock, no globals, so the host test drives it directly.
 * err > 0 means R is above the target, i.e. more closed than asked, and suction
 * pulls R toward 9 kOhm; err < 0 wants compression. */
static action_t hold_next(const hold_cfg_t *cfg, const hold_state_t *st, int target,
                          int ohms, long now)
{
    int err = ohms - target;
    long ran = st->action == ACT_OFF ? 0 : now - st->action_started_ms;

    if (st->action != ACT_OFF) {
        if (ran < cfg->min_on_ms) {
            return st->action;                      /* finish the bite */
        }
        if (ran >= cfg->max_on_ms || abs(err) <= cfg->deadband) {
            return ACT_OFF;                         /* enough, or arrived */
        }
        if ((st->action == ACT_SUCTION) != (err > 0)) {
            return ACT_OFF;                         /* shot past the target */
        }
        return st->action;
    }
    if (abs(err) <= cfg->deadband || now - st->last_change_ms < cfg->min_off_ms) {
        return ACT_OFF;
    }
    return err > 0 ? ACT_SUCTION : ACT_COMPRESSION;
}

static void hold_apply(hold_state_t *st, action_t a, long now)
{
    if (a == st->action) {
        return;
    }
    if (a != ACT_OFF) {
        st->action_started_ms = now;
    }
    st->last_change_ms = now;
    st->action = a;
}

static const hold_cfg_t hold_cfg = { HOLD_DEADBAND_OHMS, HOLD_MIN_ON_MS, HOLD_MIN_OFF_MS,
                                     HOLD_MAX_ON_MS };
static hold_state_t hold_st;
static int hold_target;             /* 0 = not holding */
static bool hold_stalled;           /* stopped itself: needs a new hold or status */
static int hold_err;                /* last error while holding */
static int hold_best_err;           /* smallest |error| seen since the target was set */
static long hold_best_ms;           /* when it last improved */
static SemaphoreHandle_t hold_lock; /* guards the pins, hold state and the ADC read */

/* Index into STATES for an action (the table is off, suction, compression). */
static int action_state(action_t a)
{
    return a == ACT_SUCTION ? 1 : a == ACT_COMPRESSION ? 2 : 0;
}

/* Stop holding and drop the pumps. Caller must not hold the lock.
 * Does NOT touch the pins when no hold is running: a manual `set gpio 1 low`
 * must keep the valve where it is, and a no-op cancel would clear it. */
static void hold_cancel(void)
{
    if (!hold_lock) {
        return;
    }
    xSemaphoreTake(hold_lock, portMAX_DELAY);
    if (hold_target) {
        hold_target = 0;
        hold_err = 0;
        write_pins(false, false, false);
    }
    hold_stalled = false;
    hold_st.action = ACT_OFF;
    xSemaphoreGive(hold_lock);
}

/* One tick of the loop. Keeps the lock while touching the pins or the ADC, so
 * a command from the console can't interleave with a bite. */
static void hold_step(void)
{
    xSemaphoreTake(hold_lock, portMAX_DELAY);
    if (!hold_target) {
        xSemaphoreGive(hold_lock);
        return;                     /* manual mode: the console owns the pins */
    }
    long now = (long)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int raw, mv;
    sense_read(&raw, &mv);

    if (mv < 0) {
        ESP_LOGW(TAG, "hold off: sensor unreadable, pumps stopped");
        hold_target = 0;
        hold_stalled = true;
    } else {
        int ohms = ohms_from_mv(mv);
        hold_err = ohms - hold_target;
        if (abs(hold_err) < hold_best_err) {
            hold_best_err = abs(hold_err);
            hold_best_ms = now;
        } else if (abs(hold_err) > hold_cfg.deadband && hold_st.action != ACT_OFF &&
                   now - hold_best_ms > HOLD_STALL_MS) {
            /* Pumping, still outside the band, and getting nowhere: blocked line,
             * dead pump, kinked tube. Being *inside* the band is not a stall, it's
             * the loop working - holding there means the error stops improving. */
            ESP_LOGW(TAG, "hold stalled at %d ohms (target %d), pumps stopped",
                     ohms, hold_target);
            hold_target = 0;
            hold_stalled = true;
        }
    }
    if (hold_target) {
        hold_apply(&hold_st, hold_next(&hold_cfg, &hold_st, hold_target,
                                       ohms_from_mv(mv), now), now);
        const pump_state_t *s = &STATES[action_state(hold_st.action)];
        write_pins(s->valve, s->comp, s->suct);
    } else {
        hold_st.action = ACT_OFF;
        write_pins(false, false, false);
    }
    xSemaphoreGive(hold_lock);
}

static void hold_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(HOLD_TICK_MS));
        hold_step();
    }
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
    xSemaphoreTake(hold_lock, portMAX_DELAY);
    int i = current_state();
    int raw, mv;
    sense_read(&raw, &mv);
    char hold[12];
    if (hold_target) {
        snprintf(hold, sizeof hold, "%d", hold_target);
    } else {
        strcpy(hold, hold_stalled ? "stalled" : "off");
    }
    int err = hold_target ? hold_err : 0;
    int ohms = mv < 0 ? -1 : ohms_from_mv(mv);
    int valve = gpio_get_level(PIN_VALVE);
    int comp = gpio_get_level(PIN_COMP);
    int suct = gpio_get_level(PIN_SUCT);
    xSemaphoreGive(hold_lock);

    reply("OK %s gpio0=%d gpio1=%d gpio2=%d adc=%d mv=%d r=%d hold=%s err=%+d",
          i < 0 ? "raw" : STATES[i].name, valve, comp, suct, raw, mv, ohms, hold, err);
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

/* "12000" or "12k" -> ohms, or -1 if it isn't a number. */
static int parse_ohms(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) {
        return -1;
    }
    if (*end == 'k' || *end == 'K') {
        v *= 1000;
        end++;
    }
    return *end ? -1 : (int)v;
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
            hold_cancel();                  /* a manual move takes the rig back */
            go_to_state(i);
            return;
        }
    }

    if (!strcmp(tok[0], "toggle")) {
        /* Read the position BEFORE cancelling: hold_cancel() drops the pins, so
         * reading after it would always report "off" and toggle would be stuck
         * going to suction. */
        int i = current_state();
        hold_cancel();
        go_to_state(i < 0 ? 0 : (i + 1) % N_STATES);   /* raw -> off */
        return;
    }
    if (!strcmp(tok[0], "status")) {
        status();
        return;
    }
    if (!strcmp(tok[0], "hold")) {
        if (n == 2 && !strcmp(tok[1], "off")) {
            hold_cancel();
            status();
            return;
        }
        int ohms = n == 2 ? parse_ohms(tok[1]) : -1;
        if (ohms < HOLD_MIN_OHMS || ohms > HOLD_MAX_OHMS) {
            reply("ERR hold needs a target in ohms (hold 12000 | hold 12k | hold off)");
            return;
        }
        xSemaphoreTake(hold_lock, portMAX_DELAY);
        hold_target = ohms;
        hold_stalled = false;
        hold_err = 0;
        hold_best_err = INT_MAX;            /* so the stall guard starts fresh */
        hold_best_ms = (long)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        hold_st.action = ACT_OFF;
        hold_st.last_change_ms = (long)(xTaskGetTickCount() * portTICK_PERIOD_MS) -
                                 HOLD_MIN_OFF_MS;   /* the first bite needn't wait */
        xSemaphoreGive(hold_lock);
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
        hold_cancel();                  /* raw pin access takes the rig back too */
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

    hold_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(hold_lock ? ESP_OK : ESP_ERR_NO_MEM);

    write_pins(false, false, false);    /* power-on state: off */
    selftest();
    write_pins(false, false, false);
    struct { int raw, mv; } s;
    sense_read(&s.raw, &s.mv);
    ESP_LOGI(TAG, "sensor: adc=%d mv=%d r=%d", s.raw, s.mv,
             s.mv < 0 ? -1 : ohms_from_mv(s.mv));
    ESP_LOGI(TAG, "ready — commands: off | suction | compression | toggle | "
                  "set gpio 0|1|2 high|low | hold <ohms>|12k|off | status");
    status();

    /* The hold loop runs here, not on the host: if the laptop sleeps or the USB
     * re-enumerates, the glove must keep holding. 3 kB is enough for the ADC
     * read plus the log line - raise it if something heavy ever goes in. */
    xTaskCreate(hold_task, "hold", 3072, NULL, 5, NULL);

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
