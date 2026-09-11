/* Command parser + mutual-exclusion test, host-native (no ESP32 attached).
 *
 *   gcc -Wall -o /tmp/test_glove_pump test/test_glove_pump.c -Itest/stubs && /tmp/test_glove_pump
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

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}
int gpio_set_level(gpio_num_t pin, int l)
{
    level[pin] = l;
    return 0;
}
int gpio_get_level(gpio_num_t pin)
{
    return level[pin];
}
void vTaskDelay(TickType_t t) { (void)t; }
void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t m) { (void)m; }

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

/* Every command must leave the pair complementary, whatever it returned. */
#define EXPECT(cmd, want) do {                                  \
        SEND(cmd);                                              \
        assert(!strcmp(out, want));                             \
        assert(level[1] != level[2]);                           \
    } while (0)

int main(void)
{
    pipe(cap);
    int terminal = dup(STDOUT_FILENO);      /* replies go to the pipe, PASS to the tty */
    dup2(cap[1], STDOUT_FILENO);
    fcntl(cap[0], F_SETFL, O_NONBLOCK);

    drive(1);                               /* power-on state */
    reply_reset();
    SEND("status");
    assert(!strcmp(out, "OK gpio1=1 gpio2=0"));

    EXPECT("toggle\n", "OK gpio1=0 gpio2=1");
    EXPECT("toggle\n", "OK gpio1=1 gpio2=0");

    EXPECT("set gpio 2 high\n", "OK gpio1=0 gpio2=1");
    EXPECT("SET GPIO 2 HIGH\n", "OK gpio1=0 gpio2=1");
    /* "low" on one pin forces the other high — the invariant wins over intent */
    EXPECT("set gpio 2 low\n", "OK gpio1=1 gpio2=0");
    EXPECT("set gpio 1 low\n", "OK gpio1=0 gpio2=1");
    EXPECT("set gpio 1 high\n", "OK gpio1=1 gpio2=0");

    EXPECT("set gpio 3 high\n", "ERR pin must be 1 or 2");
    EXPECT("set gpio 1 sideways\n", "ERR level must be high or low");
    EXPECT("frobnicate\n", "ERR unknown command");

    reply_reset();
    SEND("\r\n");                            /* half of a CRLF: silence, no state change */
    assert(!strcmp(out, ""));
    assert(level[1] == 1 && level[2] == 0);

    /* every reachable state was complementary, and only ever 0 or 1 */
    assert((level[1] == 0 || level[1] == 1) && (level[2] == 0 || level[2] == 1));

    fflush(stdout);
    dup2(terminal, STDOUT_FILENO);
    printf("PASS glove-pump parser + mutual exclusion\n");
    return 0;
}
