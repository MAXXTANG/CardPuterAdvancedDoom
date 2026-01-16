# ES8311 Register Reference for Cardputer Advanced v1.2

## Quick Reference

### I2C Address
- **Default in code:** `0x18`
- **Alternative:** `0x19` (if 0x18 doesn't work)

The ES8311 I2C address is determined by pin strapping. Try both if audio doesn't work.

## Key Registers Used in ES8311_Init()

### 1. ES8311_CHIP_CONTROL1 (0x00) - Reset
```c
write_buf[1] = 0x3F;  // Full chip reset
```

### 2. ES8311_CHIP_POWER (0x02) - Power Management
```c
write_buf[1] = 0x00;  // Power on all blocks
// Alternative: 0xFF = Power down all
```

### 3. ES8311_SYSTEM_CONTROL (0x0D) - Clock & Mode
```c
write_buf[1] = 0x01;  // Slave mode, MCLK from BCLK
// Bit 0: 0=Master, 1=Slave
// Bit 4-5: MCLK source
```

### 4. ES8311_DAC_CONTROL17 (0x17) - DAC Configuration
```c
write_buf[1] = 0xB8;  // Enable DAC, configure gain
// Bit 7: DAC enable (1=enable)
// Bit 0-6: Various DAC settings
```

### 5. ES8311_DAC_VOLUME (0x32) - Volume Control
```c
write_buf[1] = 0x40;  // ~75% volume
```

**Volume Scale:**
- `0x00` = Maximum volume (0 dB)
- `0x40` = ~75% volume (~-12 dB)
- `0x80` = ~50% volume (~-24 dB)
- `0xFF` = Mute

**To adjust volume, change this line in ES8311_Init():**
```c
write_buf[0] = ES8311_DAC_VOLUME;
write_buf[1] = 0x20;  // Louder (85% volume)
// or
write_buf[1] = 0x60;  // Quieter (65% volume)
```

### 6. ES8311_GPIO_CONTROL (0x44) - Output Enable
```c
write_buf[1] = 0x00;  // Normal operation, output enabled
```

## Troubleshooting Audio Issues

### If I2C Fails
1. **Check I2C address:** Try changing `ES8311_I2C_ADDR` from `0x18` to `0x19`
2. **Verify I2C bus:** Ensure keyboard is working (proves I2C bus is OK)
3. **Check logs:** Look for "Failed to add ES8311 to I2C bus" error

### If I2C Works But No Sound
1. **Increase volume:** Change volume register to `0x20` or `0x00`
2. **Check I2S pins:** Verify GPIO 1, 2, 3 match your schematic
3. **Check sample rate:** Should be 11025 Hz for Doom
4. **Power cycle:** Some ES8311 chips need power cycle after init

### If Sound is Distorted
1. **Lower volume:** Change to `0x60` or `0x80`
2. **Check DMA buffers:** Already set to 4 buffers of 64 samples
3. **Check bit depth:** Should be 16-bit (SAMPLESIZE*8)

## Advanced Configuration

### Changing Sample Rate
In `i_sound.c`, change:
```c
#define SAMPLERATE  22050  // Double the quality (more CPU/memory)
// or
#define SAMPLERATE  11025  // Current setting (optimal)
// or
#define SAMPLERATE  8000   // Lower quality (save resources)
```

### Adding Volume Control Function
Add this function to `i_sound.c` for runtime volume control:

```c
void ES8311_SetVolume(uint8_t volume) {
    // volume: 0-100 (percent)
    uint8_t write_buf[2];
    uint8_t vol_reg = 0xFF - ((volume * 0xFF) / 100);
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    
    i2c_master_dev_handle_t es8311_handle;
    i2c_master_bus_add_device(i2c_bus_handle_, &dev_cfg, &es8311_handle);
    
    write_buf[0] = ES8311_DAC_VOLUME;
    write_buf[1] = vol_reg;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    i2c_master_bus_rm_device(es8311_handle);
}
```

### Channel Configuration
Current: Mono (right channel only)
```c
.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
```

For stereo (requires double the data):
```c
.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
```

## Pin Summary

| Signal | GPIO | ES8311 Pin | Direction |
|--------|------|------------|-----------|
| BCLK   | 1    | BCLK       | Output → ES8311 |
| LRCK/WS| 2    | LRCK       | Output → ES8311 |
| DIN    | 3    | SDIN       | Output → ES8311 |
| SDA    | 8    | SDA        | I2C Data |
| SCL    | 9    | SCL        | I2C Clock |

## Common ES8311 I2C Addresses
- `0x18` (0011000) - Most common
- `0x19` (0011001) - Alternative strapping
- `0x32` (0110010) - 8-bit addressing variant
- `0x33` (0110011) - 8-bit addressing variant

ESP-IDF uses 7-bit addressing, so use `0x18` or `0x19`.
