/**
 * @file rgb_led.c
 * @brief RGB LED (WS2812) control implementation
 */
#include "rgb_led.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_TAG "RGB_LED"
#define LED_GPIO 21  // Cardputer Advanced v1.2
#define LED_RESOLUTION_HZ 10000000  // 10MHz
#define LED_STRIP_LED_COUNT 1

static rmt_channel_handle_t led_chan = NULL;
static rmt_encoder_handle_t led_encoder = NULL;
static led_mode_t current_mode = LED_OFF;
static uint8_t current_color[3] = {0, 0, 0};
static bool led_initialized = false;
static TaskHandle_t led_task_handle = NULL;

// WS2812 timing encoder
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_led_strip_encoder_t;

static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                    const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = led_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    
    switch (led_encoder->state) {
    case 0: // send RGB data
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1; // switch to reset code
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state = RMT_ENCODING_MEM_FULL;
            goto out;
        }
    // fall-through
    case 1: // send reset code
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET;
            state = RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state = RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_led_strip_encoder(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

esp_err_t rmt_new_led_strip_encoder(rmt_encoder_handle_t *ret_encoder)
{
    rmt_led_strip_encoder_t *led_encoder = calloc(1, sizeof(rmt_led_strip_encoder_t));
    if (!led_encoder) return ESP_ERR_NO_MEM;
    
    led_encoder->base.encode = rmt_encode_led_strip;
    led_encoder->base.del = rmt_del_led_strip_encoder;
    led_encoder->base.reset = rmt_led_strip_encoder_reset;
    
    // WS2812 timing
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 0.3 * LED_RESOLUTION_HZ / 1000000, // T0H=0.3us
            .level1 = 0,
            .duration1 = 0.9 * LED_RESOLUTION_HZ / 1000000, // T0L=0.9us
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 0.9 * LED_RESOLUTION_HZ / 1000000, // T1H=0.9us
            .level1 = 0,
            .duration1 = 0.3 * LED_RESOLUTION_HZ / 1000000, // T1L=0.3us
        },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder));
    
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder));
    
    uint32_t reset_ticks = LED_RESOLUTION_HZ / 1000000 * 50 / 2; // 50us reset
    led_encoder->reset_code = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };
    
    *ret_encoder = &led_encoder->base;
    return ESP_OK;
}

esp_err_t rgb_led_init(void)
{
    if (led_initialized) return ESP_OK;
    
    ESP_LOGI(LED_TAG, "Initializing RGB LED on GPIO %d", LED_GPIO);
    
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));
    
    led_initialized = true;
    
    // Start LED task
    xTaskCreate(rgb_led_task, "rgb_led_task", 2048, NULL, 5, &led_task_handle);
    
    return ESP_OK;
}

void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!led_initialized) return;
    
    // WS2812 expects GRB order
    uint8_t led_data[3] = {g, r, b};
    current_color[0] = r;
    current_color[1] = g;
    current_color[2] = b;
    
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    rmt_transmit(led_chan, led_encoder, led_data, sizeof(led_data), &tx_config);
}

void rgb_led_set_mode(led_mode_t mode)
{
    current_mode = mode;
    
    switch (mode) {
    case LED_OFF:
        rgb_led_set_color(0, 0, 0);
        break;
    case LED_SOLID_YELLOW:
        rgb_led_set_color(255, 255, 0);
        break;
    case LED_SOLID_BLUE:
        rgb_led_set_color(0, 0, 255);
        break;
    case LED_SOLID_GREEN:
        rgb_led_set_color(0, 255, 0);
        break;
    case LED_BLINK_BLUE:
    case LED_BLINK_GREEN:
        // Handled by task
        break;
    }
}

void rgb_led_task(void *arg)
{
    bool blink_state = false;
    
    while (1) {
        if (current_mode == LED_BLINK_BLUE || current_mode == LED_BLINK_GREEN) {
            blink_state = !blink_state;
            if (blink_state) {
                if (current_mode == LED_BLINK_BLUE) {
                    rgb_led_set_color(0, 0, 255);
                } else {
                    rgb_led_set_color(0, 255, 0);
                }
            } else {
                rgb_led_set_color(0, 0, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));  // Blink every 500ms
    }
}

void rgb_led_cleanup(void)
{
    ESP_LOGI(LED_TAG, "Cleaning up RGB LED task...");
    
    // Turn off LED first
    rgb_led_set_color(0, 0, 0);
    current_mode = LED_OFF;
    
    // Delete the task if running
    if (led_task_handle != NULL) {
        vTaskDelete(led_task_handle);
        led_task_handle = NULL;
        ESP_LOGI(LED_TAG, "RGB LED task deleted (freed 2KB stack)");
    }
}
