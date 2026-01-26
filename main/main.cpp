/**
 * @file cardputer.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.6
 * @date 2023-10-12
 *
 * @copyright Copyright (c) 2023
 *
 */


#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "spi_display.hpp"
#include "esp_mac.h"


unsigned short * __sprite_data;

LGFX_Sprite * doom_canvas;

bool scale = true;

extern "C" void __update_sprite() {
    if(scale) {
        doom_canvas->pushRotateZoom(240.0f/2.0f,135.0f/2.0f,0.0f,1.0f,(135.0f/160.0f));
    } else {
        doom_canvas->pushSprite(0,0);
    }
        
}

extern "C" void __set_sprite_pallete(unsigned int i, unsigned char r, unsigned char g, unsigned char b) {
    doom_canvas->setPaletteColor(i,r,g,b);
}

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/gpio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "tca8418.h"
#include <queue>

#define KEY_TAG "KEYBOARD"

// TFT Color definitions (RGB565)
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_BLUE    0x001F
#define TFT_YELLOW  0xFFE0

// Helper function for millis()
static inline uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#define BOARD_IIC_BUS_PORT                  I2C_NUM_0
#define BOARD_IIC_BUS_SDA                   GPIO_NUM_8
#define BOARD_IIC_BUS_SCL                   GPIO_NUM_9
#define KEYBOARD_GPIO_PIN                   GPIO_NUM_11

std::queue<uint8_t> evt_queue;
tca8418_dev_t* _tca8418 = nullptr;
EventGroupHandle_t _event_group;
i2c_master_bus_handle_t i2c_bus_handle_;  // Shared I2C bus for keyboard and ES8311 codec
bool _isr_flag = false;
static const EventBits_t INTERRUPT_EVENT_BIT = (1 << 0);

struct KeyEventRaw_t {
    bool state = false;     ///< Key state (true = pressed, false = released)
    uint8_t row = 0;        ///< Physical row position
    uint8_t col = 0;        ///< Physical column position
};

KeyEventRaw_t _key_raw;


static void IRAM_ATTR gpio_isr_handler(void* arg) {
    if (_event_group) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xEventGroupSetBitsFromISR(_event_group, 
                                 INTERRUPT_EVENT_BIT, 
                                 &xHigherPriorityTaskWoken);
        
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

void kb_init(void)
{
    ESP_LOGI(KEY_TAG, "Initializing keyboard");

    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = static_cast<i2c_port_t>(BOARD_IIC_BUS_PORT),
        .sda_io_num = BOARD_IIC_BUS_SDA,
        .scl_io_num = BOARD_IIC_BUS_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_handle_);
    
    // Allocate and initialize TCA8418 device
    _tca8418 = (tca8418_dev_t*)malloc(sizeof(tca8418_dev_t));
    if (_tca8418 == nullptr) {
        ESP_LOGE(KEY_TAG, "Failed to allocate TCA8418 device memory");
        return;
    }
    
    // Initialize TCA8418 with I2C bus (you need to provide the bus handle)
    // Replace NULL with your actual I2C bus handle
    esp_err_t ret = tca8418_init(_tca8418, i2c_bus_handle_, TCA8418_DEFAULT_ADDR);
    if (ret != ESP_OK) {
        ESP_LOGE(KEY_TAG, "Failed to initialize TCA8418: %s", esp_err_to_name(ret));
        free(_tca8418);
        _tca8418 = nullptr;
        return;
    }
    
    // Configure 7x8 keypad matrix (adjust dimensions as needed)
    ret = tca8418_config_matrix(_tca8418, 7, 8);
    if (ret != ESP_OK) {
        ESP_LOGE(KEY_TAG, "Failed to configure keypad matrix");
        free(_tca8418);
        _tca8418 = nullptr;
        return;
    }
    
    // Flush any existing events
    tca8418_flush_events(_tca8418);
    
    _event_group = xEventGroupCreate();
    if (_event_group == NULL) {
        ESP_LOGE(KEY_TAG, "Failed to create event group");
        return;
    }

    // Configure interrupt pin
    ret = tca8418_config_interrupt_pin(_tca8418, KEYBOARD_GPIO_PIN, 
                                    GPIO_INTR_ANYEDGE, 
                                    GPIO_PULLUP_DISABLE, 
                                    GPIO_PULLDOWN_DISABLE);
    if (ret != ESP_OK) {
        ESP_LOGE(KEY_TAG, "Failed to configure interrupt pin");
        free(_tca8418);
        _tca8418 = nullptr;
        return;
    }
    
    // Install ISR service
    ret = tca8418_install_isr_service(_tca8418, ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK) {
        ESP_LOGE(KEY_TAG, "Failed to install ISR service");
        free(_tca8418);
        _tca8418 = nullptr;
        return;
    }
    
    // Set interrupt callback
    ret = tca8418_set_interrupt_callback(_tca8418, gpio_isr_handler, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(KEY_TAG, "Failed to set interrupt callback");
        free(_tca8418);
        _tca8418 = nullptr;
        return ;
    }
    
    // Enable interrupts
    ret = tca8418_enable_interrupts(_tca8418);
    if (ret != ESP_OK) {
        ESP_LOGE(KEY_TAG, "Failed to enable interrupts");
        free(_tca8418);
        _tca8418 = nullptr;
        return;
    }
    
    // Enable interrupt pin
    ret = tca8418_enable_interrupt_pin(_tca8418);
    if (ret != ESP_OK) {
        ESP_LOGE(KEY_TAG, "Failed to enable interrupt pin");
        free(_tca8418);
        _tca8418 = nullptr;
        return;
    }
    ESP_LOGI(KEY_TAG, "Keyboard initialized successfully");
    return;
}

KeyEventRaw_t get_key_event_raw(uint8_t eventRaw) {
    KeyEventRaw_t ret;
    ret.state = (eventRaw & 0x80) == 0;  // TCA8418 uses bit 7 for release flag
    
    // Decode row and column from key code (adjust decoding as needed)
    uint16_t buffer = eventRaw & 0x7F;  // Key code is in lower 7 bits
    if (buffer > 0) {
        buffer--;
        ret.row = buffer / 10;  // Adjust based on your matrix dimensions
        ret.col = buffer % 10;  // Adjust based on your matrix dimensions
    }
    
    return ret;
}

void remap(KeyEventRaw_t& key) {
    // Remap physical positions to logical layout
    // Col
    uint8_t col = 0;
    col = key.row * 2;
    if (key.col > 3) col++;

    // Row
    uint8_t row = 0;
    row = (key.col + 4) % 4;

    key.row = row;
    key.col = col;
}

char kb_state[14*4];

bool keyboard_update() {
    if (_tca8418 == nullptr) return false;

    // 检查是否有中断标志（由 ISR 设置）
    EventBits_t event_bits = xEventGroupGetBits(_event_group);
    if ((event_bits & INTERRUPT_EVENT_BIT) == 0) {
        return false;
    }

    uint8_t tca_event = 0;
    bool has_processed = false;

    // 循环读取直到 FIFO 为空
    while (tca8418_read_event(_tca8418, &tca_event) == ESP_OK && tca_event != 0) {
        KeyEventRaw_t current_raw = get_key_event_raw(tca_event);
        remap(current_raw);

        uint8_t idx = current_raw.row * 14 + current_raw.col;
        if (idx < 14 * 4) {
            // 这里是关键：只根据硬件事件更新特定按键的状态
            kb_state[idx] = current_raw.state ? 0 : 1;
        }
        has_processed = true;
    }

    // 清除硬件中断状态寄存器，否则 INT 引脚会一直拉低
    tca8418_write_register(_tca8418, TCA8418_REG_INT_STAT, 1);
    // 清除事件组标志
    xEventGroupClearBits(_event_group, INTERRUPT_EVENT_BIT);

    return has_processed;
}

#define K_BKSP  0x01
#define K_TAB   0x02
#define K_FN    0x03
#define K_SHIFT 0x04
#define K_ENT   0x05
#define K_CTRL  0x06
#define K_OPT   0x07
#define K_ALT   0x08

uint8_t keymap[] = {
    '`',    '1',     '2',    '3',    '4',    '5',    '6',    '7',    '8',    '9',    '0',    '-',    '=',    K_BKSP,
    K_TAB,  'q',     'w',    'e',    'r',    't',    'y',    'u',    'i',    'o',    'p',    '[',    ']',    '\\',
    K_FN,   K_SHIFT, 'a',    's',    'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',    '\'',   K_ENT,
    K_CTRL, K_OPT,   K_ALT,  'z',    'x',    'c',    'v',    'b',    'n',    'm',    ',',    '.',    '/',    ' '    
};

void check_evts() {
    static uint8_t old_state[14*4];

    keyboard_update();

    for (int i = 0; i < 14 * 4; i++)
    {
        uint8_t v = keymap[i];
        if (old_state[i] != kb_state[i])
        {
            evt_queue.push((kb_state[i] << 7) | (v & 0x7f));
        }
    }
    memcpy(old_state, kb_state, 14 * 4);
}

extern "C" unsigned char __get_event() {
    check_evts();
    if(evt_queue.empty()) {
        return 0;
    } else {
        uint8_t evt = evt_queue.front();
        printf("Send event 0x%02x (%s '%c')\n", evt, evt&0x80?"DOWN":"UP  ", evt&0x7f);
        evt_queue.pop(); 
        return evt;
    }

}

extern "C" int doom_main(int argc, const char * argv);

#include "doom_iwad.h"
#include "rgb_led.h"
#include "i_net_serial.h"
#include "version.h"

extern const unsigned char doom_iwad_builtin[4697587UL];

void memcheck(const char *tag) {
    uint32_t free_heap = esp_get_free_heap_size();

    uint32_t max_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    printf("--- Memory Check [%s] ---\n", tag);
    printf("Total Free Heap: %lu bytes\n", free_heap);
    printf("Largest Free Block: %lu bytes\n", max_block);
}

void canvas_init()
{
    LGFX_Cardputer* _display = new LGFX_Cardputer;
    _display->init();
    _display->setRotation(1);
    doom_canvas = new LGFX_Sprite(_display);
    doom_canvas->setColorDepth(lgfx::color_depth_t::palette_8bit);
    doom_canvas->createPalette();
    doom_canvas->createSprite(240, 160);
    __sprite_data = (unsigned short*)doom_canvas->getBuffer();
}

// Splash screen function - displays version info and waits for any key
bool show_splash_screen(void)
{
    LGFX_Cardputer* display = (LGFX_Cardputer*)doom_canvas->getParent();
    
    // Clear screen to black
    display->fillScreen(TFT_BLACK);
    
    // Version in top left (grey)
    display->setTextColor(0x7BEF); // Grey RGB565
    display->setTextSize(1);
    display->setCursor(5, 5);
    display->print("Version: " VERSION_STRING);
    
    // Large centered "Cardputer ADV Doom" in yellow
    display->setTextColor(TFT_YELLOW, TFT_BLACK);
    display->setTextSize(3);
    display->setTextDatum(TC_DATUM); // Top-center alignment for multi-line centering
    
    // Calculate vertical positions (adjusted for 135px screen height)
    int center_x = display->width() / 2;
    int current_y = 30;  // Start a bit higher
    
    // Split title into two lines
    display->drawString("Cardputer", center_x, current_y);
    current_y += 25;  // Reduced spacing
    display->drawString("ADV Doom", center_x, current_y);
    current_y += 35;  // Reduced spacing
    
    // "Created by Szilamer" in white (same size as version)
    display->setTextColor(TFT_WHITE, TFT_BLACK);
    display->setTextSize(1);
    display->drawString("Created by Szilamer", center_x, current_y);
    current_y += 15;
    
    // "Tested by: Tommy P. and Andrew P." on one line to save space
    display->drawString("Tested by: Tommy P. and Andrew P.", center_x, current_y);
    current_y += 15;
    
    // Reset text datum to default (top-left)
    display->setTextDatum(TL_DATUM);
    
    // Wait for any key press (key down event)
    printf("Splash screen: waiting for any key press...\n");
    uint32_t start_time = millis();
    const uint32_t timeout_ms = 30000; // 30 second timeout
    
    while (1) {
        // Check for timeout
        if (millis() - start_time >= timeout_ms) {
            printf("Splash screen timeout, continuing...\n");
            return true; // Continue anyway
        }
        
        // Update keyboard and check for events
        keyboard_update();
        uint8_t evt = __get_event();
        
        if (evt != 0) {
            bool key_down = (evt & 0x80) != 0;
            if (key_down) {
                printf("Key pressed, continuing to NET? screen\n");
                // Brief visual feedback (optional)
                display->fillScreen(TFT_BLACK);
                return true;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void init_wad(void)
{
    doom_iwad = (unsigned char*)doom_iwad_builtin;
    doom_iwad_len = sizeof(doom_iwad_builtin);
}

// Multiplayer role selection UI (UART Serial version - NO WiFi)
// Get current day from BM8563 RTC (1-31)
static uint8_t get_rtc_day()
{
    uint8_t day = 1;
    // BM8563 I2C address (0x51) and day register (0x04)
    const uint8_t rtc_addr = 0x51;
    const uint8_t day_reg = 0x04;
    
    // Create device handle for RTC
    i2c_master_dev_handle_t rtc_handle;
    i2c_device_config_t rtc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = rtc_addr,
        .scl_speed_hz = 100000,  // Standard speed
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle_, &rtc_cfg, &rtc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE("RTC", "Failed to add BM8563 device: %s", esp_err_to_name(ret));
        return 1;  // Default to day 1 on error
    }
    
    // Write register address to read
    ret = i2c_master_transmit(rtc_handle, &day_reg, 1, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE("RTC", "Failed to write register address: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(rtc_handle);
        return 1;
    }
    
    // Read day value
    uint8_t bcd_data;
    ret = i2c_master_receive(rtc_handle, &bcd_data, 1, 1000);
    i2c_master_bus_rm_device(rtc_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE("RTC", "Failed to read day: %s", esp_err_to_name(ret));
        return 1;
    }
    
    // BM8563 stores day in BCD format (bits 0-5, 1-31)
    // Convert BCD to decimal: tens = (bcd_data >> 4) & 0x03, ones = bcd_data & 0x0F
    uint8_t tens = (bcd_data >> 4) & 0x03;  // Day tens (0-3)
    uint8_t ones = bcd_data & 0x0F;         // Day ones (0-9)
    day = tens * 10 + ones;
    
    // Validate range
    if (day < 1 || day > 31) {
        ESP_LOGW("RTC", "Invalid day value %d, defaulting to 1", day);
        day = 1;
    }
    
    ESP_LOGI("RTC", "Read day from BM8563: %d", day);
    return day;
}

// Read MAC address without enabling WiFi
static void get_mac_address(uint8_t mac[6])
{
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE("MAC", "Failed to read MAC: %s", esp_err_to_name(ret));
        memset(mac, 0, 6);
    } else {
        ESP_LOGI("MAC", "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

bool multiplayer_role_selection(void)
{
    LGFX_Cardputer* display = (LGFX_Cardputer*)doom_canvas->getParent();
    
    // Initialize RGB LED
    rgb_led_init();
    rgb_led_set_mode(LED_SOLID_YELLOW);
    
    // Clear screen to dark grey
    display->fillScreen(0x4208); // Dark grey RGB565
    
    // Draw large yellow "NET?"
    display->setTextColor(TFT_YELLOW, 0x4208);
    display->setTextSize(4);
    display->setCursor(80, 40);
    display->print("NET?");
    
    // Instructions
    display->setTextSize(1);
    display->setCursor(10, 100);
    display->setTextColor(TFT_WHITE, 0x4208);
    display->print("Automated role negotiation...");
    display->setCursor(40, 115);
    display->print("(Connect Grove cable first)");
    
    // Progress bar at bottom
    const int bar_width = 200;
    const int bar_height = 10;
    const int bar_x = 20;
    const int bar_y = 130;
    
    uint32_t start_time = millis();
    uint32_t timeout_ms = 15000; // 15 seconds for negotiation
    
    bool mode_selected = false;
    bool negotiation_complete = false;
    
    // Get local MAC and RTC day
    uint8_t local_mac[6];
    get_mac_address(local_mac);
    uint8_t local_day = get_rtc_day();
    
    ESP_LOGI("NET", "Local MAC: %02X:%02X:%02X:%02X:%02X:%02X, Day: %d", 
             local_mac[0], local_mac[1], local_mac[2], local_mac[3], local_mac[4], local_mac[5], local_day);
    
    uint8_t remote_mac[6];
    uint8_t remote_day;
    int auto_is_master;
    
    while (!mode_selected) {
        uint32_t elapsed = millis() - start_time;
        
        // Update progress bar
        int progress = (elapsed * bar_width) / timeout_ms;
        display->fillRect(bar_x, bar_y, progress, bar_height, TFT_YELLOW);
        
        // Check for timeout
        if (elapsed >= timeout_ms) {
            ESP_LOGW("NET", "Negotiation timeout - no remote device found");
            display->fillScreen(0x0000); // Black
            rgb_led_set_mode(LED_OFF);
            return false; // Single player
        }
        
        // Try automated negotiation - use longer timeout per call for better reliability
        // Each call tries multiple pin configs (200ms each), so 2000ms = ~10 config attempts
        if (!negotiation_complete) {
            esp_err_t neg_ret = serial_net_auto_negotiate(local_mac, local_day, remote_mac, &remote_day, &auto_is_master, 2000);
            
            if (neg_ret == ESP_OK) {
                negotiation_complete = true;
                // Determine role based on negotiation result
                is_multiplayer = true;
                is_master = auto_is_master;
                
                ESP_LOGI("NET", "Negotiation succeeded: Role = %s (Day %d, Local MAC higher? %d)", 
                         is_master ? "MASTER" : "SLAVE", local_day, mac_compare(local_mac, remote_mac));
                
                // Clear screen and show role with appropriate color
                display->fillScreen(0x0000);
                if (is_master) {
                    display->setTextColor(TFT_GREEN, 0x0000);
                    display->setTextSize(8);
                    display->setCursor(60, 40);
                    display->print("M");
                    display->setTextSize(2);
                    display->setCursor(60, 120);
                    display->print("MASTER");
                    rgb_led_set_mode(LED_SOLID_GREEN);
                } else {
                    display->setTextColor(TFT_RED, 0x0000);
                    display->setTextSize(8);
                    display->setCursor(60, 40);
                    display->print("S");
                    display->setTextSize(2);
                    display->setCursor(60, 120);
                    display->print("SLAVE");
                    rgb_led_set_mode(LED_SOLID_BLUE);
                }
                
                // Wait for 1 second to show role (reduced from 2 seconds for faster sync)
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                // Now initialize UART with determined role
                display->fillScreen(0x0000);
                display->setTextColor(TFT_WHITE, 0x0000);
                display->setTextSize(2);
                display->setCursor(30, 60);
                display->print("Initializing...");
                
                if (serial_net_init() == ESP_OK) {
                    display->setTextSize(1);
                    display->setCursor(30, 90);
                    if (is_master) {
                        display->print("Syncing with slave...");
                    } else {
                        display->print("Waiting for master...");
                    }
                    
                    // Perform handshake
                    int handshake_timeout = is_master ? 5000 : 10000;
                    if (serial_net_handshake(handshake_timeout) == ESP_OK) {
                        // Success - flash green
                        display->fillScreen(0x07E0); // Green
                        rgb_led_set_mode(LED_SOLID_GREEN);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        display->fillScreen(0x0000);
                        mode_selected = true;
                        return true;
                    } else {
                        // Handshake failed
                        display->fillScreen(TFT_RED);
                        display->setTextSize(2);
                        display->setCursor(20, 60);
                        display->print("SYNC FAIL");
                        display->setTextSize(1);
                        display->setCursor(20, 90);
                        display->print("Check Grove cable!");
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        display->fillScreen(0x0000);
                        rgb_led_set_mode(LED_OFF);
                        serial_net_deinit();
                        is_multiplayer = false;
                        return false;
                    }
                } else {
                    // UART init failed
                    display->fillScreen(TFT_RED);
                    display->setTextSize(2);
                    display->setCursor(20, 60);
                    display->print("UART FAIL");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    display->fillScreen(0x0000);
                    rgb_led_set_mode(LED_OFF);
                    is_multiplayer = false;
                    return false;
                }
            } else if (neg_ret != ESP_ERR_TIMEOUT) {
                // Some other error
                ESP_LOGE("NET", "Negotiation error: %d", neg_ret);
            }
        }
        
        // Also allow manual override via Enter key for single player
        keyboard_update();
        uint8_t evt = __get_event();
        if (evt != 0) {
            bool key_down = (evt & 0x80) != 0;
            char key = evt & 0x7F;
            
            if (key_down && (key == '\r' || key == '\n')) {
                // Enter - single player
                display->fillScreen(0x0000);
                rgb_led_set_mode(LED_OFF);
                return false;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    return false;
}

extern "C" void app_main(void)
{
    memcheck("Start");

    kb_init();

    canvas_init();

    init_wad();
    
    // Show splash screen (version info, credits) and wait for any key
    show_splash_screen();
    
    // Multiplayer role selection (UART Serial - NO WiFi overhead)
    bool mp_enabled = multiplayer_role_selection();
    
    if (mp_enabled) {
        printf("=== MULTIPLAYER MODE (%s) via UART Serial ===\n", is_master ? "MASTER" : "SLAVE");
        
        // Log memory - should be nearly full heap since NO WiFi!
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t max_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        printf("[MP] Free heap: %lu bytes\n", free_heap);
        printf("[MP] Largest free block: %lu bytes\n", max_block);
        printf("[MP] UART uses minimal RAM - full heap available for Doom!\n");
        fflush(stdout);
        
        // Cleanup RGB LED task to reclaim 2KB stack memory
        rgb_led_cleanup();

        // Pre-game startup sync: wait for both devices before entering doom_main
        if (Serial_StartupSync(10000) != ESP_OK) {
            printf("STARTUP SYNC FAILED - continuing anyway (risk of desync)\n");
        }
        
    } else {
        printf("=== SINGLE PLAYER MODE ===\n");
        
        // Cleanup LED task even in single player mode
        rgb_led_cleanup();
    }
    
    // Final memory check before doom_main()
    printf(">>> Final Memory Check <<<\n");
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t max_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    printf("Free heap before doom_main(): %lu bytes\n", free_heap);
    printf("Largest free block: %lu bytes\n", max_block);
    fflush(stdout);

    printf(">>> Calling doom_main() <<<\n");
    fflush(stdout);
    doom_main(0, 0);
    
    // doom_main() should never return
    printf("ERROR: doom_main() returned unexpectedly!\n");
}
