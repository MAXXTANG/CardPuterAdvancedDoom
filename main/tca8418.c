#include "tca8418.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "TCA8418";

static void tca8418_isr_handler(void* arg);

/**
 * @brief Initialize the TCA8418 keyboard expander.
 * @param dev TCA8418 device structure
 * @param bus_handle I2C bus handle
 * @param i2c_addr I2C address of the device
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_init(tca8418_dev_t *dev, i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr) {
    if (!dev || !bus_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize device structure
    dev->bus_handle = bus_handle;
    dev->i2c_addr = i2c_addr;
    dev->initialized = false;
    
    // Configure I2C device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false
    };
    
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure GPIO defaults
    // Set all GPIO pins to INPUT
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_DIR_1, 0x00));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_DIR_2, 0x00));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_DIR_3, 0x00));
    
    // Add all pins to key events
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPI_EM_1, 0xFF));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPI_EM_2, 0xFF));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPI_EM_3, 0xFF));
    
    // Set all pins to FALLING interrupts
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_INT_LVL_1, 0x00));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_INT_LVL_2, 0x00));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_INT_LVL_3, 0x00));
    
    // Enable interrupts for all pins
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_INT_EN_1, 0xFF));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_INT_EN_2, 0xFF));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_GPIO_INT_EN_3, 0xFF));
    
    dev->initialized = true;
    ESP_LOGI(TAG, "TCA8418 initialized successfully");
    return ESP_OK;
}

/**
 * @brief Configure the keypad matrix size.
 * @param dev TCA8418 device structure
 * @param rows Number of rows (1-8)
 * @param columns Number of columns (1-10)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_config_matrix(tca8418_dev_t *dev, uint8_t rows, uint8_t columns) {
    if (!dev || rows > 8 || columns > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Skip zero size matrix
    if (rows == 0 || columns == 0) {
        return ESP_OK;
    }
    
    // Configure rows (KP_GPIO_1 register)
    uint8_t row_mask = 0x00;
    for (int r = 0; r < rows; r++) {
        row_mask <<= 1;
        row_mask |= 1;
    }
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_KP_GPIO_1, row_mask));
    
    // Configure columns (KP_GPIO_2 and KP_GPIO_3 registers)
    uint8_t col_mask = 0x00;
    for (int c = 0; c < columns && c < 8; c++) {
        col_mask <<= 1;
        col_mask |= 1;
    }
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_KP_GPIO_2, col_mask));
    
    if (columns > 8) {
        if (columns == 9) {
            col_mask = 0x01;
        } else {
            col_mask = 0x03;
        }
        ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_KP_GPIO_3, col_mask));
    }
    
    return ESP_OK;
}

/**
 * @brief Get the number of key events available in the buffer.
 * @param dev TCA8418 device structure
 * @param count Number of events available
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_get_event_count(tca8418_dev_t *dev, uint8_t *count) {
    if (!dev || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t event_count;
    ESP_ERROR_CHECK(tca8418_read_register(dev, TCA8418_REG_KEY_LCK_EC, &event_count));
    
    // Lower 4 bits contain the event count
    *count = event_count & 0x0F;
    return ESP_OK;
}

/**
 * @brief Read a key event from the buffer.
 * @param dev TCA8418 device structure
 * @param event Key event structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_read_event(tca8418_dev_t *dev, uint8_t *keycode) {
    if (!dev || !keycode) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_ERROR_CHECK(tca8418_read_register(dev, TCA8418_REG_KEY_EVENT_A, keycode));

    return ESP_OK;
}

/**
 * @brief Flush all events from the buffer.
 * @param dev TCA8418 device structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_flush_events(tca8418_dev_t *dev) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t count = 0, event = 0;
    // Flush key events
    while (tca8418_read_event(dev, &event) == ESP_OK && event != 0) {
        count++;
    }
    
    // Flush GPIO events by reading interrupt status registers
    uint8_t dummy;
    tca8418_read_register(dev, TCA8418_REG_GPIO_INT_STAT_1, &dummy);
    tca8418_read_register(dev, TCA8418_REG_GPIO_INT_STAT_2, &dummy);
    tca8418_read_register(dev, TCA8418_REG_GPIO_INT_STAT_3, &dummy);
    
    // Clear interrupt status
    return tca8418_write_register(dev, TCA8418_REG_INT_STAT, 0x03);
}

/**
 * @brief Read GPIO pin value.
 * @param dev TCA8418 device structure
 * @param pin Pin to read
 * @param value GPIO value (0 or 1)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_gpio_read(tca8418_dev_t *dev, tca8418_pin_t pin, uint8_t *value) {
    if (!dev || !value || pin >= TCA8418_PIN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t reg = TCA8418_REG_GPIO_DAT_STAT_1 + (pin / 8);
    uint8_t mask = (1 << (pin % 8));
    
    uint8_t reg_value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, reg, &reg_value));
    
    *value = (reg_value & mask) ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief Write GPIO pin value.
 * @param dev TCA8418 device structure
 * @param pin Pin to write
 * @param value GPIO value (0 or 1)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_gpio_write(tca8418_dev_t *dev, tca8418_pin_t pin, uint8_t value) {
    if (!dev || pin >= TCA8418_PIN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t reg = TCA8418_REG_GPIO_DAT_OUT_1 + (pin / 8);
    uint8_t mask = (1 << (pin % 8));
    
    uint8_t reg_value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, reg, &reg_value));
    
    if (value == 0) {
        reg_value &= ~mask;
    } else {
        reg_value |= mask;
    }
    
    return tca8418_write_register(dev, reg, reg_value);
}

/**
 * @brief Set GPIO pin mode.
 * @param dev TCA8418 device structure
 * @param pin Pin to configure
 * @param mode GPIO mode
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_gpio_set_mode(tca8418_dev_t *dev, tca8418_pin_t pin, tca8418_gpio_mode_t mode) {
    if (!dev || pin >= TCA8418_PIN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t idx = pin / 8;
    uint8_t mask = (1 << (pin % 8));
    
    // Set GPIO direction (0 = INPUT, 1 = OUTPUT)
    uint8_t dir_reg = TCA8418_REG_GPIO_DIR_1 + idx;
    uint8_t dir_value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, dir_reg, &dir_value));
    
    if (mode == TCA8418_GPIO_OUTPUT) {
        dir_value |= mask;
    } else {
        dir_value &= ~mask;
    }
    ESP_ERROR_CHECK(tca8418_write_register(dev, dir_reg, dir_value));
    
    // Set pull-up configuration (0 = enabled, 1 = disabled)
    uint8_t pull_reg = TCA8418_REG_GPIO_PULL_1 + idx;
    uint8_t pull_value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, pull_reg, &pull_value));
    
    if (mode == TCA8418_GPIO_INPUT_PULLUP) {
        pull_value &= ~mask;
    } else {
        pull_value |= mask;
    }
    ESP_ERROR_CHECK(tca8418_write_register(dev, pull_reg, pull_value));
    
    return ESP_OK;
}

/**
 * @brief Set GPIO interrupt mode.
 * @param dev TCA8418 device structure
 * @param pin Pin to configure
 * @param mode Interrupt mode
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_gpio_set_irq_mode(tca8418_dev_t *dev, tca8418_pin_t pin, tca8418_irq_mode_t mode) {
    if (!dev || pin >= TCA8418_PIN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t idx = pin / 8;
    uint8_t mask = (1 << (pin % 8));
    
    // Set interrupt level (0 = FALLING, 1 = RISING)
    uint8_t lvl_reg = TCA8418_REG_GPIO_INT_LVL_1 + idx;
    uint8_t lvl_value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, lvl_reg, &lvl_value));
    
    if (mode == TCA8418_IRQ_RISING) {
        lvl_value |= mask;
    } else {
        lvl_value &= ~mask;
    }
    ESP_ERROR_CHECK(tca8418_write_register(dev, lvl_reg, lvl_value));
    
    // Enable interrupt
    uint8_t int_reg = TCA8418_REG_GPIO_INT_EN_1 + idx;
    uint8_t int_value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, int_reg, &int_value));
    int_value |= mask;
    ESP_ERROR_CHECK(tca8418_write_register(dev, int_reg, int_value));
    
    return ESP_OK;
}

/**
 * @brief Enable all interrupts.
 * @param dev TCA8418 device structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_enable_interrupts(tca8418_dev_t *dev) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, TCA8418_REG_CFG, &value));
    value |= (TCA8418_CFG_GPI_IEN | TCA8418_CFG_KE_IEN);
    return tca8418_write_register(dev, TCA8418_REG_CFG, value);
}

/**
 * @brief Disable all interrupts.
 * @param dev TCA8418 device structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_disable_interrupts(tca8418_dev_t *dev) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, TCA8418_REG_CFG, &value));
    value &= ~(TCA8418_CFG_GPI_IEN | TCA8418_CFG_KE_IEN);
    return tca8418_write_register(dev, TCA8418_REG_CFG, value);
}

/**
 * @brief Enable matrix overflow.
 * @param dev TCA8418 device structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_enable_overflow(tca8418_dev_t *dev) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, TCA8418_REG_CFG, &value));
    value |= TCA8418_CFG_OVR_FLOW_M;
    return tca8418_write_register(dev, TCA8418_REG_CFG, value);
}

/**
 * @brief Disable matrix overflow.
 * @param dev TCA8418 device structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_disable_overflow(tca8418_dev_t *dev) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t value;
    ESP_ERROR_CHECK(tca8418_read_register(dev, TCA8418_REG_CFG, &value));
    value &= ~TCA8418_CFG_OVR_FLOW_M;
    return tca8418_write_register(dev, TCA8418_REG_CFG, value);
}

/**
 * @brief Enable key debounce.
 * @param dev TCA8418 device structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_enable_debounce(tca8418_dev_t *dev) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_DEBOUNCE_DIS_1, 0x00));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_DEBOUNCE_DIS_2, 0x00));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_DEBOUNCE_DIS_3, 0x00));
    return ESP_OK;
}

/**
 * @brief Disable key debounce.
 * @param dev TCA8418 device structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_disable_debounce(tca8418_dev_t *dev) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_DEBOUNCE_DIS_1, 0xFF));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_DEBOUNCE_DIS_2, 0xFF));
    ESP_ERROR_CHECK(tca8418_write_register(dev, TCA8418_REG_DEBOUNCE_DIS_3, 0xFF));
    return ESP_OK;
}

/**
 * @brief Read from a TCA8418 register.
 * @param dev TCA8418 device structure
 * @param reg Register address
 * @param value Register value output
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_read_register(tca8418_dev_t *dev, tca8418_reg_t reg, uint8_t *value) {
    if (!dev || !value || !dev->dev_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t reg_addr = (uint8_t)reg;
    return i2c_master_transmit_receive(dev->dev_handle, &reg_addr, 1, value, 1, 1000);
}

/**
 * @brief Write to a TCA8418 register.
 * @param dev TCA8418 device structure
 * @param reg Register address
 * @param value Value to write
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tca8418_write_register(tca8418_dev_t *dev, tca8418_reg_t reg, uint8_t value) {
    if (!dev || !dev->dev_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t data[2] = {(uint8_t)reg, value};
    return i2c_master_transmit(dev->dev_handle, data, sizeof(data), 1000);
}

// Interrupt configuration functions
esp_err_t tca8418_config_interrupt_pin(tca8418_dev_t *dev, gpio_num_t int_pin, 
                                      gpio_int_type_t int_type, 
                                      gpio_pullup_t pull_up_en, 
                                      gpio_pulldown_t pull_down_en) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->int_pin = int_pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << int_pin),
        .mode = GPIO_MODE_INPUT,
        .intr_type = int_type,
        .pull_up_en = pull_up_en,
        .pull_down_en = pull_down_en,
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure interrupt pin: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Interrupt pin GPIO%d configured successfully", int_pin);
    return ESP_OK;
}

esp_err_t tca8418_install_isr_service(tca8418_dev_t *dev, int intr_flags) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->int_pin == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "Interrupt pin not configured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gpio_install_isr_service(intr_flags);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means ISR service is already installed, which is fine
        ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add(dev->int_pin, tca8418_isr_handler, dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ISR service installed successfully");
    return ESP_OK;
}

esp_err_t tca8418_set_interrupt_callback(tca8418_dev_t *dev, 
                                        tca8418_interrupt_callback_t callback, 
                                        void* arg) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->int_callback = callback;
    dev->int_callback_arg = arg;

    ESP_LOGI(TAG, "Interrupt callback set successfully");
    return ESP_OK;
}

esp_err_t tca8418_enable_interrupt_pin(tca8418_dev_t *dev) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->int_pin == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "Interrupt pin not configured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gpio_intr_enable(dev->int_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable interrupt pin: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Interrupt pin enabled");
    return ESP_OK;
}

esp_err_t tca8418_disable_interrupt_pin(tca8418_dev_t *dev) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->int_pin == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gpio_intr_disable(dev->int_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable interrupt pin: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Interrupt pin disabled");
    return ESP_OK;
}

esp_err_t tca8418_clear_interrupt(tca8418_dev_t *dev) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Read the interrupt status register to clear interrupts
    uint8_t int_status;
    esp_err_t ret = tca8418_read_register(dev, TCA8418_REG_INT_STAT, &int_status);
    if (ret != ESP_OK) {
        return ret;
    }

    // If there are key events, read them to clear the interrupt
    if (int_status & TCA8418_STAT_K_INT) {
        uint8_t event = 0;
        while (tca8418_read_event(dev, &event) == ESP_OK && event != 0) {
            // Events are read and discarded to clear the FIFO
        }
    }

    // Clear GPIO interrupts by reading the status registers
    uint8_t gpio_int_status;
    tca8418_read_register(dev, TCA8418_REG_GPIO_INT_STAT_1, &gpio_int_status);
    tca8418_read_register(dev, TCA8418_REG_GPIO_INT_STAT_2, &gpio_int_status);
    tca8418_read_register(dev, TCA8418_REG_GPIO_INT_STAT_3, &gpio_int_status);

    ESP_LOGD(TAG, "Interrupt cleared, status was: 0x%02x", int_status);
    return ESP_OK;
}

// ISR handler function
static void tca8418_isr_handler(void* arg) {
    tca8418_dev_t *dev = (tca8418_dev_t*)arg;
    
    if (dev == NULL || dev->int_callback == NULL) {
        return;
    }

    // Call the user-provided callback
    dev->int_callback(dev->int_callback_arg);
}