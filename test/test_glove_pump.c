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

static void reply_grab(void)
{
    fflush(stdout);
    ssize_t n = read(cap[0], out, sizeof(out) - 1);
    out[n > 0 ? (size_t)n : 0] = 0;
    if (n > 0) {
        out[strcspn(out, "\r\n")] = 0;
    }
}

#define SEND(cmd) do { char b[sizeof(cmd)]; memcpy(b, cmd, sizeof(cmd)); handle(b); reply_grab(); } while (0)

/* Every reply carries the sensor fields too. fake_raw is 1350 from here on, so
 * mv=1350 (glove fully open) and r=9000 — 13000 * 1350 / (3300 - 1350). */
#define SENSOR " adc=1350 mv=1350 r=9000"

/* The two hardware rules, checked after every command: the pumps are never both
 * high, and the valve is never left on the other pump's path. */
#define EXPECT(cmd, want) do {                                          \
        SEND(cmd);                                                      \
        assert(!strcmp(out, want));                                     \
        assert(!(level[PIN_COMP] && level[PIN_SUCT]));                  \
        assert(!(level[PIN_COMP] && !level[PIN_VALVE]));                \
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
    assert(!strcmp(out, "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR));

    /* The three named states */
    EXPECT("suction\n", "OK suction gpio0=0 gpio1=0 gpio2=1" SENSOR);
    EXPECT("compression\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR);
    EXPECT("COMPRESSION\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR);
    EXPECT("off\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR);

    /* toggle cycles off -> suction -> compression -> off */
    EXPECT("toggle\n", "OK suction gpio0=0 gpio1=0 gpio2=1" SENSOR);
    EXPECT("toggle\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR);
    EXPECT("toggle\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR);

    /* raw pin commands: raising a pump pairs the valve and drops the other pump */
    EXPECT("set gpio 1 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR);
    EXPECT("set gpio 2 high\n", "OK suction gpio0=0 gpio1=0 gpio2=1" SENSOR);
    EXPECT("set gpio 2 low\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR);

    /* lowering is local: the valve keeps its position, so a valve-only
     * combination reads back as "raw" */
    EXPECT("set gpio 1 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0" SENSOR);
    EXPECT("set gpio 1 low\n", "OK raw gpio0=1 gpio1=0 gpio2=0" SENSOR);
    EXPECT("set gpio 0 low\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR);
    EXPECT("set gpio 0 high\n", "OK raw gpio0=1 gpio1=0 gpio2=0" SENSOR);

    /* toggle out of a raw combination goes to off */
    EXPECT("toggle\n", "OK off gpio0=0 gpio1=0 gpio2=0" SENSOR);

    EXPECT("set gpio 3 high\n", "ERR pin must be 0, 1 or 2");
    EXPECT("set gpio 1 sideways\n", "ERR level must be high or low");
    EXPECT("set gpio 1\n", "ERR unknown command");
    EXPECT("frobnicate\n", "ERR unknown command");

    /* the sensor reading follows the hardware, and rides on every reply */
    fake_raw = 2000;                            /* fully closed */
    reply_reset();
    SEND("status");
    assert(!strcmp(out, "OK off gpio0=0 gpio1=0 gpio2=0 adc=2000 mv=2000 r=20000"));
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

    fflush(stdout);
    dup2(terminal, STDOUT_FILENO);
    printf("PASS glove-pump states + pump/valve rules + sensor\n");
    return 0;
}
