/* Shared fakes so main/glove_pump.c links as a native host binary.
 * Not compiled into the firmware. */
#ifndef STUBS_H
#define STUBS_H

#include <stdint.h>
#include <stdio.h>

typedef int gpio_num_t;
typedef int esp_err_t;

#define ESP_OK 0
#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_MODE_DEF_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *cfg);
int gpio_set_level(gpio_num_t pin, int level);
int gpio_get_level(gpio_num_t pin);

#define ESP_ERROR_CHECK(expr) do {                                  \
        if ((expr) != ESP_OK) {                                     \
            printf("ESP_ERROR_CHECK failed: %s\n", #expr);          \
        }                                                           \
    } while (0)

typedef int TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(TickType_t ticks);

typedef enum { ESP_LINE_ENDINGS_CRLF, ESP_LINE_ENDINGS_CR, ESP_LINE_ENDINGS_LF } esp_line_endings_t;
void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t mode);

typedef struct {
    uint32_t tx_buffer_size;
    uint32_t rx_buffer_size;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() \
    ((usb_serial_jtag_driver_config_t){ 256, 256 })

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg);
void usb_serial_jtag_vfs_use_driver(void);

#endif
