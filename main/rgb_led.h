/**
 * @file rgb_led.h
 * @brief RGB LED (WS2812) control for M5Stack Cardputer Advanced
 * @note LED is on GPIO 21 for Cardputer Advanced v1.2
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    LED_OFF = 0,
    LED_SOLID_YELLOW,
    LED_SOLID_BLUE,
    LED_SOLID_GREEN,
    LED_BLINK_BLUE,
    LED_BLINK_GREEN
} led_mode_t;

/**
 * @brief Initialize RGB LED
 * @return ESP_OK on success
 */
esp_err_t rgb_led_init(void);

/**
 * @brief Set LED mode
 * @param mode LED mode from led_mode_t
 */
void rgb_led_set_mode(led_mode_t mode);

/**
 * @brief Set LED to specific RGB color
 * @param r Red (0-255)
 * @param g Green (0-255)  
 * @param b Blue (0-255)
 */
void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief LED update task (handles blinking)
 */
void rgb_led_task(void *arg);

/**
 * @brief Stop LED task and turn off LED to reclaim resources
 * Call before heavy initialization to free stack memory
 */
void rgb_led_cleanup(void);

#ifdef __cplusplus
}
#endif
