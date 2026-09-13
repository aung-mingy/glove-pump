/* Command parser + pump/valve rules + the glove sensor, host-native (no ESP32).
 *
 *   gcc -Wall -Wextra -o /tmp/test_glove_pump test/test_glove_pump.c -Itest/stubs && /tmp/test_glove_pump
 *
 * Links the real main/glove_pump.c against stubbed GPIO and ADC, so it exercises
 * the production code, not a copy of it.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "stubs.h"                      /* types + fakes, before the firmware */

/* --- fake GPIO -------------------------------------------------------- */
static int level[3];
static int input_enabled[3];

/* Mirrors gpio_config(): the mode decides whether the input buffer is on, and
 * an output-only pad (GPIO_MODE_OUTPUT, no input bit) reads back 0 for ever. */
esp_err_t gpio_config(const gpio_config_t *cfg)
{
    for (int pin = 0; pin < 3; pin++) {
        if (cfg->pin_bit_mask & (1ULL << pin)) {
            input_enabled[pin] = (cfg->mode & GPIO_MODE_DEF_INPUT) != 0;
        }
    }
    return ESP_OK;
}
int gpio_set_level(gpio_num_t pin, int l)
{
    level[pin] = l;
    return 0;
}
int gpio_get_level(gpio_num_t pin)
{
    return input_enabled[pin] ? level[pin] : 0;
}
void vTaskDelay(TickType_t t) { (void)t; }
void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t m) { (void)m; }

/* --- fake FreeRTOS: one tick is one ms, the mutex is a no-op ---------- */
static TickType_t fake_ticks;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) { (void)m; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return pdTRUE; }
TickType_t xTaskGetTickCount(void) { return fake_ticks; }
void vTaskDelayUntil(TickType_t *prev, TickType_t period) { *prev += period; fake_ticks += period; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)out;
    return pdPASS;                      /* the harness drives hold_step() itself */
}
/* app_main() is compiled but never called here, so these only need to link. */
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *c) { (void)c; return ESP_OK; }
void usb_serial_jtag_vfs_use_driver(void) {}

/* --- fake ADC --------------------------------------------------------- */
static int fake_raw;                    /* what the sensor "reads" */

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c,
                               adc_oneshot_unit_handle_t *out)
{
    (void)c;
    *out = (void *)1;
    return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch,
                                     const adc_oneshot_chan_cfg_t *c)
{
    (void)h; (void)ch; (void)c;
    return ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *out_raw)
{
    (void)h; (void)ch;
    *out_raw = fake_raw;
    return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c,
                                               adc_cali_handle_t *out)
{
    (void)c;
    *out = (void *)1;
    return ESP_OK;
}
/* Identity: "raw" is already mV, so a test can set the tap voltage directly and
 * check the divider maths against the real wiring numbers. */
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *out_mv)
{
    (void)h;
    *out_mv = raw;
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "stub error";
}

#include "../main/glove_pump.c"

/* --- capture the reply ------------------------------------------------- */
static int cap[2];
static char out[256];

static void reply_reset(void)
{
    while (read(cap[0], out, sizeof(out)) > 0) {
        /* drain anything left over */
    }
    out[0] = 0;
}

/* Read until a whole line has arrived: one non-blocking read can return a
 * partial chunk (or nothing), which turns into a bogus mismatch later on. */
static void reply_grab(void)
{
    fflush(stdout);
    size_t used = 0;
    while (used < sizeof(out) - 1) {
        ssize_t n = read(cap[0], out + used, sizeof(out) - 1 - used);
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
        out[used] = 0;
        if (strchr(out, '\n')) {
            break;
        }
    }
    out[strcspn(out, "\r\n")] = 0;
}

#define SEND(cmd) do {                                                  \
        reply_reset();                                                  \
        char b[sizeof(cmd)];                                            \
        memcpy(b, cmd, sizeof(cmd));                                    \
        handle(b);                                                      \
        reply_grab();                                                   \
    } while (0)

/* Every reply carries the sensor and hold fields too. fake_raw is 1350 from here
 * on, so mv=1350 (glove fully open) and r=9000 — 13000 * 1350 / (3300 - 1350). */
#define SENSOR " adc=1350 mv=1350 r=9000"
#define HOLD   " hold=off err=+0"

/* The one hardware rule, checked after every command: the pumps are never both
 * high. The valve is free — `set gpio 0 …` moves it on its own. */
#define EXPECT(cmd, want) do {                                          \
        SEND(cmd);                                                      \
        if (strcmp(out, want)) {                                        \
            fprintf(stderr, "\n  got  <%s>\n  want <%s>\n", out, want); \
        }                                                               \
        assert(!strcmp(out, want));                                     \
        assert(!(level[PIN_COMP] && level[PIN_SUCT]));                  \
    } while (0)

int main(void)
{
    pipe(cap);
    int terminal = dup(STDOUT_FILENO);      /* replies go to the pipe, PASS to the tty */
    dup2(cap[1], STDOUT_FILENO);
    fcntl(cap[0], F_SETFL, O_NONBLOCK);

    /* The firmware's own pin config: output-only mode would make every readback
     * 0 and fail the boot selftest on hardware. */
    gpio_config(&gpio_pins_cfg);
    hold_lock = xSemaphoreCreateMutex();
    write_pins(false, false, false);
    assert(gpio_get_level(PIN_VALVE) == 0 && gpio_get_level(PIN_COMP) == 0 &&
           gpio_get_level(PIN_SUCT) == 0);

    /* --- the glove sensor --------------------------------------------- */
    sense_init();
    assert(sense_ready && sense_calibrated);

    /* The wiring, both ends, plus saturation: 3.3 V --[13k]-- tap --[R]-- GND. */
    assert(ohms_from_mv(1350) == 9000);         /* glove fully open, per the user */
    assert(ohms_from_mv(2000) == 20000);        /* fully closed */
    assert(ohms_from_mv(0) == 0);
    assert(ohms_from_mv(-1) == 0);
    assert(ohms_from_mv(3300) == SENSE_OPEN_OHMS);   /* tap on the rail */
    assert(ohms_from_mv(3250) == 845000);            /* still a real number... */
    assert(ohms_from_mv(3299) == SENSE_OPEN_OHMS);   /* ...and the cap bites here
                                                      * (13k * 3299 / 1 would be
                                                      * 43 MOhm) */
    /* Round trip through the divider for a sweep of resistances: the inverse
     * must match what the wiring actually produces, to within the 1 mV the ADC
     * reports. */
    for (int r = 500; r <= 60000; r += 500) {
        int mv = (int)(((long)SENSE_VCC_MV * r + (SENSE_SERIES_OHMS + r) / 2) /
                       (SENSE_SERIES_OHMS + r));
        int back = ohms_from_mv(mv);
        assert(abs(back - r) * 100 <= r);       /* <= 1% */
    }

    fake_raw = 1350;
    reply_reset();
    SEND("status");                             /* startup state: off */
    assert(!strcmp(out, "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD));

    /* The three named states */
    EXPECT("suction\n", "OK suction gpio0=0 gpio1=0 gpio2=1" SENSOR HOLD);
    EXPECT("compression\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("COMPRESSION\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("off\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);

    /* toggle cycles off -> suction -> compression -> off */
    EXPECT("toggle\n", "OK suction gpio0=0 gpio1=0 gpio2=1" SENSOR HOLD);
    EXPECT("toggle\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("toggle\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);

    /* raw pin commands: raising a pump drops the other one and nothing else —
     * the valve is not touched, so these read back as "raw" combinations */
    EXPECT("set gpio 1 high\n", "OK raw gpio0=0 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 2 high\n", "OK suction gpio0=0 gpio1=0 gpio2=1" SENSOR HOLD);
    EXPECT("set gpio 2 low\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);

    /* the valve moves only when it is set, and stays put through pump changes:
     * that is the handle for checking which pump a valve level actually routes */
    EXPECT("set gpio 0 high\n", "OK raw gpio0=1 gpio1=0 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 2 high\n", "OK raw gpio0=1 gpio1=0 gpio2=1" SENSOR HOLD);
    EXPECT("set gpio 1 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 1 low\n", "OK raw gpio0=1 gpio1=0 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 0 low\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 0 high\n", "OK raw gpio0=1 gpio1=0 gpio2=0" SENSOR HOLD);

    /* valve low with compression running is now reachable too */
    EXPECT("set gpio 0 low\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 1 high\n", "OK raw gpio0=0 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 0 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("off\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);

    /* both pumps can never be high: each raise drops the other, and the valve
     * is left exactly where it was set */
    EXPECT("set gpio 0 high\n", "OK raw gpio0=1 gpio1=0 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 1 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 2 high\n", "OK raw gpio0=1 gpio1=0 gpio2=1" SENSOR HOLD);
    EXPECT("set gpio 1 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("set gpio 0 low\n", "OK raw gpio0=0 gpio1=1 gpio2=0" SENSOR HOLD);
    EXPECT("off\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);

    /* toggle out of a raw combination goes to off */
    EXPECT("set gpio 0 high\n", "OK raw gpio0=1 gpio1=0 gpio2=0" SENSOR HOLD);
    EXPECT("toggle\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR HOLD);

    EXPECT("set gpio 3 high\n", "ERR pin must be 0, 1 or 2");
    EXPECT("set gpio 1 sideways\n", "ERR level must be high or low");
    EXPECT("set gpio 1\n", "ERR unknown command");
    EXPECT("frobnicate\n", "ERR unknown command");

    /* the sensor reading follows the hardware, and rides on every reply */
    fake_raw = 2000;                            /* fully closed */
    reply_reset();
    SEND("status");
    assert(!strcmp(out, "OK off gpio0=0 gpio1=0 gpio2=0 adc=2000 mv=2000 r=20000 hold=off err=+0"));
    fake_raw = 0;                               /* tap shorted to ground */
    SEND("status");
    assert(strstr(out, " mv=0 r=0"));
    fake_raw = 3300;                            /* tap at the rail: no sensor */
    SEND("status");
    assert(strstr(out, " mv=3300 r=999999"));
    fake_raw = 1350;

    reply_reset();
    SEND("\r\n");                            /* half of a CRLF: silence, no state change */
    assert(!strcmp(out, ""));
    assert(level[PIN_VALVE] == 0 && level[PIN_COMP] == 0 && level[PIN_SUCT] == 0);

    /* every state in the table is reachable and reads back as itself */
    for (int i = 0; i < N_STATES; i++) {
        write_pins(STATES[i].valve, STATES[i].comp, STATES[i].suct);
        assert(current_state() == i);
        assert(!(level[PIN_COMP] && level[PIN_SUCT]));
    }
    /* hold maps actions to table rows by index, so pin the names down */
    assert(!strcmp(STATES[action_state(ACT_OFF)].name, "off"));
    assert(!strcmp(STATES[action_state(ACT_SUCTION)].name, "suction"));
    assert(!strcmp(STATES[action_state(ACT_COMPRESSION)].name, "compression"));

    /* --- hold: the decision table, driven directly --------------------- */
    const int TARGET = 12000;
    hold_state_t st = { 0 };
    long t = 100000;
    assert(hold_next(&hold_cfg, &st, TARGET, 12000, t) == ACT_OFF);   /* in band */
    assert(hold_next(&hold_cfg, &st, TARGET, 11300, t) == ACT_OFF);
    assert(hold_next(&hold_cfg, &st, TARGET, 12999, t) == ACT_OFF);
    assert(hold_next(&hold_cfg, &st, TARGET, 13500, t) == ACT_SUCTION);      /* too closed */
    assert(hold_next(&hold_cfg, &st, TARGET, 10500, t) == ACT_COMPRESSION);  /* too open */

    /* a bite runs to min_on even if the band is reached sooner... */
    hold_apply(&st, ACT_SUCTION, t);
    assert(hold_next(&hold_cfg, &st, TARGET, 12000, t + 50) == ACT_SUCTION);
    assert(hold_next(&hold_cfg, &st, TARGET, 12000, t + 99) == ACT_SUCTION);
    /* ...then stops */
    assert(hold_next(&hold_cfg, &st, TARGET, 12000, t + 100) == ACT_OFF);
    /* and min_off keeps it off even when it is wanted again */
    hold_apply(&st, ACT_OFF, t + 100);
    assert(hold_next(&hold_cfg, &st, TARGET, 13500, t + 200) == ACT_OFF);
    assert(hold_next(&hold_cfg, &st, TARGET, 13500, t + 300) == ACT_SUCTION);
    /* max_on ends a bite that is still far from the target */
    hold_apply(&st, ACT_SUCTION, t + 1000);
    assert(hold_next(&hold_cfg, &st, TARGET, 25000, t + 1100) == ACT_SUCTION);
    assert(hold_next(&hold_cfg, &st, TARGET, 25000, t + 1400) == ACT_OFF);
    /* and crossing the target cancels it instead of pumping the wrong way */
    hold_apply(&st, ACT_SUCTION, t + 2000);
    assert(hold_next(&hold_cfg, &st, TARGET, 11000, t + 2100) == ACT_OFF);

    /* --- hold: a simulated glove, through the real hold_step() ---------- */
    /* R responds to the pumps and leaks back toward closed when they are off --
     * exactly the situation that needs a controller. Counts pump duty and checks
     * the pin invariant on every tick. Rates are per 50 ms tick: a bite moves
     * 400 ohms, the leak 40, so one bite stays well inside the 1 kOhm band. */
    const int BITE = 400, LEAK = 40;
    fake_ticks = 0;
    hold_lock = xSemaphoreCreateMutex();
    int r = 20000;                      /* start closed, target 12k */
    int duty = 0, ticks = 0, worst = 0;
    reply_reset();
    SEND("hold 12k\n");
    assert(strstr(out, "hold=12000"));
    for (int i = 0; i < 400; i++) {     /* 20 s at 50 ms */
        fake_raw = (int)((3300L * r + (SENSE_SERIES_OHMS + r) / 2) /
                         (SENSE_SERIES_OHMS + r));
        fake_ticks += HOLD_TICK_MS;
        hold_step();
        assert(!(level[PIN_COMP] && level[PIN_SUCT]));      /* the invariant, live */
        assert(!level[PIN_COMP] || level[PIN_VALVE]);       /* valve follows */
        assert(!level[PIN_SUCT] || !level[PIN_VALVE]);
        if (level[PIN_SUCT]) {
            r -= BITE;                  /* suction opens the glove: R falls */
            duty++;
        } else if (level[PIN_COMP]) {
            r += BITE;
            duty++;
        } else {
            r += LEAK;                  /* leak: air creeps in, glove closes */
        }
        if (r < 8000) r = 8000;         /* mechanical end stops */
        if (r > 21000) r = 21000;
        ticks++;
        if (i > 150) {                  /* after settling it holds: the band, plus
                                         * one tick of leak before the next bite
                                         * notices, plus the mV quantisation the
                                         * firmware actually sees (~25 ohms here) */
            assert(abs(r - TARGET) <= hold_cfg.deadband + LEAK + 50);
            if (abs(r - TARGET) > worst) {
                worst = abs(r - TARGET);
            }
        }
    }
    assert(duty > 0 && duty < ticks);    /* it held by pumping, not by luck */
    fprintf(stderr, "hold: settled %d ohms (target %d), pumps on %d/%d ticks, worst error %d ohms\n",
           r, TARGET, duty, ticks, worst);
    SEND("hold off\n");
    assert(hold_target == 0);

    /* a pump that moves nothing must end with the pumps off, not pumping for ever */
    fake_ticks = 0;
    hold_target = TARGET;
    hold_stalled = false;
    hold_best_err = INT_MAX;
    hold_best_ms = 0;
    hold_st.action = ACT_OFF;
    for (int i = 0; i < 500; i++) {      /* 25 s of a dead pump */
        fake_raw = 1350;                 /* sensor fine: 9 kOhm, far from 12 kOhm */
        fake_ticks += HOLD_TICK_MS;
        hold_step();
    }
    assert(hold_target == 0 && hold_stalled);
    assert(level[PIN_COMP] == 0 && level[PIN_SUCT] == 0 && level[PIN_VALVE] == 0);

    /* an unreadable sensor stops the loop rather than pumping blind */
    fake_ticks = 0;
    hold_target = TARGET;
    hold_stalled = false;
    hold_best_err = INT_MAX;
    hold_best_ms = 0;
    hold_st.action = ACT_OFF;
    fake_raw = -1;
    fake_ticks += HOLD_TICK_MS;
    hold_step();
    assert(hold_target == 0 && hold_stalled);
    assert(level[PIN_COMP] == 0 && level[PIN_SUCT] == 0);
    fake_raw = 1350;

    /* manual commands take the rig back from the loop */
    hold_target = TARGET;
    SEND("off\n");
    assert(hold_target == 0);
    hold_target = TARGET;
    SEND("set gpio 1 high\n");
    assert(hold_target == 0);
    SEND("off\n");
    SEND("status\n");
    assert(strstr(out, "hold=off"));
    fake_raw = 1350;

    fflush(stdout);
    dup2(terminal, STDOUT_FILENO);
    printf("PASS glove-pump states + pump/valve rules + sensor\n");
    return 0;
}
