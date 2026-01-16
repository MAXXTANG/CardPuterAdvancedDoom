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
#include "tca8418.h"
#include <queue>

#define KEY_TAG "KEYBOARD"

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

extern "C" void init_wad(void)
{
    doom_iwad = (unsigned char*)doom_iwad_builtin;
    doom_iwad_len = sizeof(doom_iwad_builtin);
}

extern "C" void app_main(void)
{
    memcheck("Start");

    kb_init();

    canvas_init();

    init_wad();

    printf("Launch DOOM\n");
    doom_main(0, 0);
}
