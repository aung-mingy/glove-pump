/* Command parser + pump/valve rules, host-native (no ESP32 attached).
 *
 *   gcc -Wall -Wextra -o /tmp/test_glove_pump test/test_glove_pump.c -Itest/stubs && /tmp/test_glove_pump
 *
 * Links the real main/glove_pump.c against stubbed GPIO, so it exercises the
 * production parser, not a copy of it.
 */
#include <assert.h>
#include <fcntl.h>
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
    assert(gpio_config(&gpio_pins_cfg) == ESP_OK);
    write_pins(false, false, false);
    assert(gpio_get_level(PIN_VALVE) == 0 && gpio_get_level(PIN_COMP) == 0 &&
           gpio_get_level(PIN_SUCT) == 0);

    reply_reset();
    SEND("status");                         /* startup state: off */
    assert(!strcmp(out, "OK off gpio0=0 gpio1=0 gpio2=0"));

    /* the three named states */
    EXPECT("suction\n", "OK suction gpio0=0 gpio1=0 gpio2=1");
    EXPECT("compression\n", "OK compression gpio0=1 gpio1=1 gpio2=0");
    EXPECT("COMPRESSION\n", "OK compression gpio0=1 gpio1=1 gpio2=0");
    EXPECT("off\n", "OK off gpio0=0 gpio1=0 gpio2=0");

    /* toggle cycles off -> suction -> compression -> off */
    EXPECT("toggle\n", "OK suction gpio0=0 gpio1=0 gpio2=1");
    EXPECT("toggle\n", "OK compression gpio0=1 gpio1=1 gpio2=0");
    EXPECT("toggle\n", "OK off gpio0=0 gpio1=0 gpio2=0");

    /* raw pin commands: raising a pump pairs the valve and drops the other pump */
    EXPECT("set gpio 1 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0");
    EXPECT("set gpio 2 high\n", "OK suction gpio0=0 gpio1=0 gpio2=1");
    EXPECT("set gpio 2 low\n", "OK off gpio0=0 gpio1=0 gpio2=0");

    /* lowering is local: the valve keeps its position, so a valve-only
     * combination reads back as "raw" */
    EXPECT("set gpio 1 high\n", "OK compression gpio0=1 gpio1=1 gpio2=0");
    EXPECT("set gpio 1 low\n", "OK raw gpio0=1 gpio1=0 gpio2=0");
    EXPECT("set gpio 0 low\n", "OK off gpio0=0 gpio1=0 gpio2=0");
    EXPECT("set gpio 0 high\n", "OK raw gpio0=1 gpio1=0 gpio2=0");

    /* toggle out of a raw combination goes to off */
    EXPECT("toggle\n", "OK off gpio0=0 gpio1=0 gpio2=0");

    EXPECT("set gpio 3 high\n", "ERR pin must be 0, 1 or 2");
    EXPECT("set gpio 1 sideways\n", "ERR level must be high or low");
    EXPECT("set gpio 1\n", "ERR unknown command");
    EXPECT("frobnicate\n", "ERR unknown command");

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
    printf("PASS glove-pump states + pump/valve rules\n");
    return 0;
}
