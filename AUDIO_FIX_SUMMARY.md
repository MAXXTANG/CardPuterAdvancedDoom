# Audio Fix for M5Stack Cardputer Advanced v1.2 (ES8311 Codec)

## Changes Made

### 1. Updated I2S Pin Configuration
**File:** `components/GBADoom/source/i_sound.c`

Changed from legacy v1.1 pins to Cardputer Advanced v1.2 pins:

| Signal | Old (v1.1) | New (v1.2) |
|--------|-----------|-----------|
| **BCLK** | GPIO 41 | GPIO 1 ✅ |
| **LRCK/WS** | GPIO 43 | GPIO 2 ✅ |
| **DIN** | GPIO 42 | GPIO 3 ✅ |

### 2. Added ES8311 Codec Initialization
**File:** `components/GBADoom/source/i_sound.c`

Added `ES8311_Init()` function that:
- Communicates with ES8311 via I2C (address 0x18)
- Resets and powers on the codec
- Configures it as I2S slave (ESP32-S3 is master)
- Enables DAC output
- Sets initial volume to ~75%
- Enables audio output

### 3. Shared I2C Bus Configuration
**File:** `main/main.cpp`

Changed `i2c_bus_handle_` from `static` to global scope to allow sharing between:
- TCA8418 keyboard chip
- ES8311 audio codec

Both devices now use the same I2C bus (SDA=GPIO 8, SCL=GPIO 9).

### 4. Integration Point

The ES8311 initialization is called in `I_InitSound()` immediately after I2S driver installation:

```c
i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pin_cfg);

// Initialize ES8311 codec via I2C
esp_err_t codec_ret = ES8311_Init();
if (codec_ret != ESP_OK) {
    ESP_LOGE(SOUND_TAG, "ES8311 initialization failed! Audio will not work.");
}
```

## Audio Configuration

- **Sample Rate:** 11025 Hz (optimal for Doom SFX)
- **Bit Depth:** 16-bit
- **Channel Format:** Mono (I2S_CHANNEL_FMT_ONLY_RIGHT)
- **Communication Format:** I2S_COMM_FORMAT_I2S_MSB
- **I2S Port:** I2S_NUM_0

## ES8311 I2C Address Note

The code uses `0x18` as the I2C address. If audio still doesn't work, try changing to `0x19` in the `ES8311_I2C_ADDR` definition, as ES8311 address depends on pin strapping.

## Testing

After flashing the updated firmware:
1. Monitor serial output for "ES8311 codec initialized successfully"
2. If you see I2C errors, the codec address might need adjustment
3. Audio should now work for both SFX and music
4. Keyboard functionality remains unchanged (same I2C bus)

## Troubleshooting

If audio is still silent:
1. Check serial logs for ES8311 initialization errors
2. Verify I2C address (try 0x19 if 0x18 fails)
3. Verify I2S pins match your hardware schematic
4. Check if keyboard still works (validates I2C bus)
5. Increase ES8311 volume by changing `0x40` to `0x20` or `0x00` in ES8311_Init()
