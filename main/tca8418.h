#ifndef TCA8418_H
#define TCA8418_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TCA8418 Keypad Matrix and GPIO Expander Driver
 * 
 * This driver provides an interface for the TCA8418 keypad matrix controller
 * and GPIO expander. The TCA8418 supports up to 10x8 keypad matrix scanning
 * and provides 18 GPIO pins with interrupt capabilities.
 */

// Library version
#define TCA8418_LIBRARY_VERSION "1.0.0"

// I2C address
#define TCA8418_DEFAULT_ADDR 0x34  ///< Default I2C address of TCA8418

// Register addresses
typedef enum {
    TCA8418_REG_CFG = 0x01,              ///< Configuration register
    TCA8418_REG_INT_STAT = 0x02,         ///< Interrupt status register
    TCA8418_REG_KEY_LCK_EC = 0x03,       ///< Key lock and event counter register
    TCA8418_REG_KEY_EVENT_A = 0x04,      ///< Key event register A (first event)
    TCA8418_REG_KEY_EVENT_B = 0x05,      ///< Key event register B
    TCA8418_REG_KEY_EVENT_C = 0x06,      ///< Key event register C
    TCA8418_REG_KEY_EVENT_D = 0x07,      ///< Key event register D
    TCA8418_REG_KEY_EVENT_E = 0x08,      ///< Key event register E
    TCA8418_REG_KEY_EVENT_F = 0x09,      ///< Key event register F
    TCA8418_REG_KEY_EVENT_G = 0x0A,      ///< Key event register G
    TCA8418_REG_KEY_EVENT_H = 0x0B,      ///< Key event register H
    TCA8418_REG_KEY_EVENT_I = 0x0C,      ///< Key event register I
    TCA8418_REG_KEY_EVENT_J = 0x0D,      ///< Key event register J (last event)
    TCA8418_REG_KP_LCK_TIMER = 0x0E,     ///< Keypad lock timer register
    TCA8418_REG_UNLOCK_1 = 0x0F,         ///< Unlock sequence register 1
    TCA8418_REG_UNLOCK_2 = 0x10,         ///< Unlock sequence register 2
    TCA8418_REG_GPIO_INT_STAT_1 = 0x11,  ///< GPIO interrupt status bank 1
    TCA8418_REG_GPIO_INT_STAT_2 = 0x12,  ///< GPIO interrupt status bank 2
    TCA8418_REG_GPIO_INT_STAT_3 = 0x13,  ///< GPIO interrupt status bank 3
    TCA8418_REG_GPIO_DAT_STAT_1 = 0x14,  ///< GPIO data status bank 1
    TCA8418_REG_GPIO_DAT_STAT_2 = 0x15,  ///< GPIO data status bank 2
    TCA8418_REG_GPIO_DAT_STAT_3 = 0x16,  ///< GPIO data status bank 3
    TCA8418_REG_GPIO_DAT_OUT_1 = 0x17,   ///< GPIO data output bank 1
    TCA8418_REG_GPIO_DAT_OUT_2 = 0x18,   ///< GPIO data output bank 2
    TCA8418_REG_GPIO_DAT_OUT_3 = 0x19,   ///< GPIO data output bank 3
    TCA8418_REG_GPIO_INT_EN_1 = 0x1A,    ///< GPIO interrupt enable bank 1
    TCA8418_REG_GPIO_INT_EN_2 = 0x1B,    ///< GPIO interrupt enable bank 2
    TCA8418_REG_GPIO_INT_EN_3 = 0x1C,    ///< GPIO interrupt enable bank 3
    TCA8418_REG_KP_GPIO_1 = 0x1D,        ///< Keypad/GPIO selection bank 1
    TCA8418_REG_KP_GPIO_2 = 0x1E,        ///< Keypad/GPIO selection bank 2
    TCA8418_REG_KP_GPIO_3 = 0x1F,        ///< Keypad/GPIO selection bank 3
    TCA8418_REG_GPI_EM_1 = 0x20,         ///< GPI event mode bank 1
    TCA8418_REG_GPI_EM_2 = 0x21,         ///< GPI event mode bank 2
    TCA8418_REG_GPI_EM_3 = 0x22,         ///< GPI event mode bank 3
    TCA8418_REG_GPIO_DIR_1 = 0x23,       ///< GPIO direction bank 1 (input/output)
    TCA8418_REG_GPIO_DIR_2 = 0x24,       ///< GPIO direction bank 2
    TCA8418_REG_GPIO_DIR_3 = 0x25,       ///< GPIO direction bank 3
    TCA8418_REG_GPIO_INT_LVL_1 = 0x26,   ///< GPIO interrupt level/edge bank 1
    TCA8418_REG_GPIO_INT_LVL_2 = 0x27,   ///< GPIO interrupt level/edge bank 2
    TCA8418_REG_GPIO_INT_LVL_3 = 0x28,   ///< GPIO interrupt level/edge bank 3
    TCA8418_REG_DEBOUNCE_DIS_1 = 0x29,   ///< Debounce disable bank 1
    TCA8418_REG_DEBOUNCE_DIS_2 = 0x2A,   ///< Debounce disable bank 2
    TCA8418_REG_DEBOUNCE_DIS_3 = 0x2B,   ///< Debounce disable bank 3
    TCA8418_REG_GPIO_PULL_1 = 0x2C,      ///< GPIO pull-up configuration bank 1
    TCA8418_REG_GPIO_PULL_2 = 0x2D,      ///< GPIO pull-up configuration bank 2
    TCA8418_REG_GPIO_PULL_3 = 0x2E,      ///< GPIO pull-up configuration bank 3
} tca8418_reg_t;

// Configuration register bits
#define TCA8418_CFG_AI              (0x80)  ///< Auto-increment register pointer
#define TCA8418_CFG_GPI_E_CGF       (0x40)  ///< GPI event mode configuration
#define TCA8418_CFG_OVR_FLOW_M      (0x20)  ///< Overflow mode enable
#define TCA8418_CFG_INT_CFG         (0x10)  ///< Interrupt configuration (level/edge)
#define TCA8418_CFG_OVR_FLOW_IEN    (0x08)  ///< Overflow interrupt enable
#define TCA8418_CFG_K_LCK_IEN       (0x04)  ///< Key lock interrupt enable
#define TCA8418_CFG_GPI_IEN         (0x02)  ///< GPI interrupt enable
#define TCA8418_CFG_KE_IEN          (0x01)  ///< Key event interrupt enable

// Interrupt status register bits
#define TCA8418_STAT_CAD_INT        (0x10)  ///< Control-alt-del interrupt status
#define TCA8418_STAT_OVR_FLOW_INT   (0x08)  ///< Overflow interrupt status
#define TCA8418_STAT_K_LCK_INT      (0x04)  ///< Key lock interrupt status
#define TCA8418_STAT_GPI_INT        (0x02)  ///< GPI interrupt status
#define TCA8418_STAT_K_INT          (0x01)  ///< Key event interrupt status

// Key lock and event counter register bits
#define TCA8418_LCK_EC_K_LCK_EN     (0x40)  ///< Key lock enable
#define TCA8418_LCK_EC_LCK_2        (0x20)  ///< Keypad lock status 2
#define TCA8418_LCK_EC_LCK_1        (0x10)  ///< Keypad lock status 1
#define TCA8418_LCK_EC_KLEC_3       (0x08)  ///< Key event counter bit 3
#define TCA8418_LCK_EC_KLEC_2       (0x04)  ///< Key event counter bit 2
#define TCA8418_LCK_EC_KLEC_1       (0x02)  ///< Key event counter bit 1
#define TCA8418_LCK_EC_KLEC_0       (0x01)  ///< Key event counter bit 0

/**
 * @brief TCA8418 pin definitions
 * 
 * The TCA8418 provides 18 pins that can be configured as rows (0-7),
 * columns (0-9), or GPIO pins.
 */
typedef enum {
    TCA8418_ROW0 = 0,    ///< Row 0 / GPIO 0
    TCA8418_ROW1,        ///< Row 1 / GPIO 1
    TCA8418_ROW2,        ///< Row 2 / GPIO 2
    TCA8418_ROW3,        ///< Row 3 / GPIO 3
    TCA8418_ROW4,        ///< Row 4 / GPIO 4
    TCA8418_ROW5,        ///< Row 5 / GPIO 5
    TCA8418_ROW6,        ///< Row 6 / GPIO 6
    TCA8418_ROW7,        ///< Row 7 / GPIO 7
    TCA8418_COL0,        ///< Column 0 / GPIO 8
    TCA8418_COL1,        ///< Column 1 / GPIO 9
    TCA8418_COL2,        ///< Column 2 / GPIO 10
    TCA8418_COL3,        ///< Column 3 / GPIO 11
    TCA8418_COL4,        ///< Column 4 / GPIO 12
    TCA8418_COL5,        ///< Column 5 / GPIO 13
    TCA8418_COL6,        ///< Column 6 / GPIO 14
    TCA8418_COL7,        ///< Column 7 / GPIO 15
    TCA8418_COL8,        ///< Column 8 / GPIO 16
    TCA8418_COL9,        ///< Column 9 / GPIO 17
    TCA8418_PIN_COUNT    ///< Total number of pins available
} tca8418_pin_t;

/**
 * @brief GPIO pin mode configuration
 */
typedef enum {
    TCA8418_GPIO_INPUT,          ///< Input mode without pull-up
    TCA8418_GPIO_INPUT_PULLUP,   ///< Input mode with pull-up resistor
    TCA8418_GPIO_OUTPUT          ///< Output mode
} tca8418_gpio_mode_t;

/**
 * @brief GPIO interrupt trigger mode
 */
typedef enum {
    TCA8418_IRQ_FALLING,         ///< Interrupt on falling edge
    TCA8418_IRQ_RISING,          ///< Interrupt on rising edge
    TCA8418_IRQ_ANYEDGE,         ///< Interrupt on both edges
    TCA8418_IRQ_LOW_LEVEL,       ///< Interrupt on low level
    TCA8418_IRQ_HIGH_LEVEL       ///< Interrupt on high level
} tca8418_irq_mode_t;

/**
 * @brief Interrupt callback function type
 */
typedef void (*tca8418_interrupt_callback_t)(void* arg);

/**
 * @brief TCA8418 device structure
 * 
 * This structure holds the device context including I2C handles
 * and initialization status.
 */
typedef struct {
    i2c_master_bus_handle_t bus_handle;  ///< I2C master bus handle
    i2c_master_dev_handle_t dev_handle;  ///< I2C device handle
    uint8_t i2c_addr;                    ///< I2C device address
    bool initialized;                    ///< Device initialization status
    gpio_num_t int_pin;                  ///< Interrupt pin number
    tca8418_interrupt_callback_t int_callback; ///< Interrupt callback function
    void* int_callback_arg;              ///< Argument for interrupt callback
} tca8418_dev_t;

// Device initialization and configuration
esp_err_t tca8418_init(tca8418_dev_t *dev, i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr);
esp_err_t tca8418_config_matrix(tca8418_dev_t *dev, uint8_t rows, uint8_t columns);

// Key event management functions
esp_err_t tca8418_get_event_count(tca8418_dev_t *dev, uint8_t *count);
esp_err_t tca8418_read_event(tca8418_dev_t *dev, uint8_t *keycode);
esp_err_t tca8418_flush_events(tca8418_dev_t *dev);

// GPIO control functions
esp_err_t tca8418_gpio_read(tca8418_dev_t *dev, tca8418_pin_t pin, uint8_t *value);
esp_err_t tca8418_gpio_write(tca8418_dev_t *dev, tca8418_pin_t pin, uint8_t value);
esp_err_t tca8418_gpio_set_mode(tca8418_dev_t *dev, tca8418_pin_t pin, tca8418_gpio_mode_t mode);
esp_err_t tca8418_gpio_set_irq_mode(tca8418_dev_t *dev, tca8418_pin_t pin, tca8418_irq_mode_t mode);

// Device configuration functions
esp_err_t tca8418_enable_interrupts(tca8418_dev_t *dev);
esp_err_t tca8418_disable_interrupts(tca8418_dev_t *dev);
esp_err_t tca8418_enable_overflow(tca8418_dev_t *dev);
esp_err_t tca8418_disable_overflow(tca8418_dev_t *dev);
esp_err_t tca8418_enable_debounce(tca8418_dev_t *dev);
esp_err_t tca8418_disable_debounce(tca8418_dev_t *dev);

// Interrupt configuration functions
esp_err_t tca8418_config_interrupt_pin(tca8418_dev_t *dev, gpio_num_t int_pin, 
                                      gpio_int_type_t int_type, 
                                      gpio_pullup_t pull_up_en, 
                                      gpio_pulldown_t pull_down_en);
esp_err_t tca8418_install_isr_service(tca8418_dev_t *dev, int intr_flags);
esp_err_t tca8418_set_interrupt_callback(tca8418_dev_t *dev, 
                                        tca8418_interrupt_callback_t callback, 
                                        void* arg);
esp_err_t tca8418_enable_interrupt_pin(tca8418_dev_t *dev);
esp_err_t tca8418_disable_interrupt_pin(tca8418_dev_t *dev);
esp_err_t tca8418_clear_interrupt(tca8418_dev_t *dev);

// Low-level register access functions
esp_err_t tca8418_read_register(tca8418_dev_t *dev, tca8418_reg_t reg, uint8_t *value);
esp_err_t tca8418_write_register(tca8418_dev_t *dev, tca8418_reg_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif // TCA8418_H