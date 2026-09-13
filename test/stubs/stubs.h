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
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
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

/* --- esp_adc (sensor front end) --------------------------------------- */
/* Mirrors IDF: adc_unit_t and adc_channel_t are 0-based enums (ADC_UNIT_1 == 0),
 * and soc_caps gates which units the driver will accept at all. The C3's real
 * caps are reproduced here: unit 0 only, 5 channels on unit 0 and 1 on unit 1 —
 * that's what makes an ADC2 pin a *driver* error on a C3, not a wiring problem. */
#define ADC_UNIT_1 0
#define ADC_UNIT_2 1
/* ADC1 has 5 channels on the C3 (GPIO0-GPIO4), ADC2 has 1 (GPIO5) — define the
 * whole range, or a stub that only knows the channels we happen to use will
 * reject a legal pin swap. */
#define ADC_CHANNEL_0 0
#define ADC_CHANNEL_1 1
#define ADC_CHANNEL_2 2
#define ADC_CHANNEL_3 3
#define ADC_CHANNEL_4 4
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_DEFAULT 12
#define ADC_ULP_MODE_DISABLE 0
#define SOC_ADC_DIG_SUPPORTED_UNIT(unit) ((unit) == 0)
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1

typedef int adc_unit_t;
typedef int adc_channel_t;
typedef int adc_atten_t;
typedef int adc_bitwidth_t;
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;

typedef struct { adc_unit_t unit_id; int ulp_mode; } adc_oneshot_unit_init_cfg_t;
typedef struct { adc_atten_t atten; adc_bitwidth_t bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,
                               adc_oneshot_unit_handle_t *out_unit);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t unit, adc_channel_t chan,
                                     const adc_oneshot_chan_cfg_t *cfg);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t unit, adc_channel_t chan, int *out_raw);
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                               adc_cali_handle_t *out_handle);
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *out_mv);

/* Real IDF has this; the firmware logs it so a failed ADC init names its cause. */
const char *esp_err_to_name(esp_err_t code);

typedef struct {
    uint32_t tx_buffer_size;
    uint32_t rx_buffer_size;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() \
    ((usb_serial_jtag_driver_config_t){ 256, 256 })

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg);
void usb_serial_jtag_vfs_use_driver(void);

#endif
