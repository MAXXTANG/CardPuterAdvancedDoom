# Build and Test Checklist for Audio Fix

## Pre-Build Checklist

- [x] Updated I2S pins to GPIO 1, 2, 3 in `i_sound.c`
- [x] Added ES8311 codec initialization function
- [x] Made I2C bus handle global in `main.cpp`
- [x] Added ES8311 register definitions
- [x] Integrated ES8311_Init() call in I_InitSound()

## Build Instructions

```bash
# 1. Clean previous build (recommended)
idf.py fullclean

# 2. Build the project
idf.py build

# 3. Flash to device
idf.py -p /dev/ttyACM0 flash monitor
# Or on Windows:
# idf.py -p COM3 flash monitor
```

## Expected Serial Output

When successful, you should see these logs:

```
I (xxxx) KEYBOARD: Initializing keyboard
I (xxxx) KEYBOARD: Keyboard initialized successfully
...
I (xxxx) DOOM_SOUND: Initializing ES8311 codec...
I (xxxx) DOOM_SOUND: ES8311 codec initialized successfully
I (xxxx) DOOM: I_InitSound: pre-cached all sound data
I (xxxx) DOOM: I_InitSound: sound module ready
```

## Potential Issues and Solutions

### Issue 1: ES8311 I2C Address Error
**Error:** `Failed to add ES8311 to I2C bus` or `ES8311 reset failed`

**Solution:** Change I2C address in `i_sound.c`:
```c
#define ES8311_I2C_ADDR  0x19  // Try 0x19 instead of 0x18
```

### Issue 2: I2C Bus Not Initialized
**Error:** `I2C bus handle is NULL! Initialize keyboard first.`

**Solution:** Ensure keyboard initialization happens before audio:
- Check `kb_init()` is called in `main.cpp` before Doom starts
- Verify `i2c_bus_handle_` is not static in `main.cpp`

### Issue 3: Audio Still Silent (I2C Success)
**Possible causes:**
1. Volume too low
2. Wrong I2S pins
3. ES8311 needs power cycle

**Solutions:**
```c
// In ES8311_Init(), try:
write_buf[1] = 0x00;  // Maximum volume instead of 0x40

// Verify pins match your hardware:
pin_cfg.bck_io_num = 1;       // Double-check schematic
pin_cfg.data_out_num = 3;     // Verify with hardware
pin_cfg.ws_io_num = 2;        // Confirm pin routing
```

### Issue 4: Distorted Audio
**Solution:** Reduce volume:
```c
write_buf[1] = 0x60;  // or 0x80 for quieter audio
```

## Testing Steps

1. **Start Game**
   - Listen for startup sound when game launches

2. **Test SFX**
   - Walk around (should hear footsteps)
   - Shoot weapon (should hear gunfire)
   - Open doors (should hear door sounds)

3. **Test Music**
   - Music should play in menu and during gameplay
   - Should be OPL synthesized music

4. **Test Volume**
   - Sound should be clear, not distorted
   - If too loud, reduce volume in ES8311_Init()

## Verification Commands

```bash
# Monitor serial output
idf.py monitor

# Check component sizes
idf.py size-components

# View build configuration
idf.py menuconfig
```

## Common Build Errors

### Error: `i2c_master_bus_handle_t` undeclared
**Cause:** Missing include
**Fix:** Already added `#include "driver/i2c_master.h"`

### Error: `i2c_bus_handle_` undefined reference
**Cause:** Still declared as `static` in main.cpp
**Fix:** Already changed to non-static

### Error: Cannot find `i2c_master_transmit`
**Cause:** Old ESP-IDF version (< 5.0)
**Fix:** If using ESP-IDF 4.4.x, you'll need to use legacy I2C API:
```c
// Replace i2c_master_transmit with:
i2c_cmd_handle_t cmd = i2c_cmd_link_create();
i2c_master_start(cmd);
i2c_master_write_byte(cmd, (ES8311_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
i2c_master_write_byte(cmd, write_buf[0], true);
i2c_master_write_byte(cmd, write_buf[1], true);
i2c_master_stop(cmd);
i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
i2c_cmd_link_delete(cmd);
```

## Success Indicators

✅ Build completes without errors
✅ Flash and boot successful
✅ Keyboard still works (I2C bus shared)
✅ Serial log shows "ES8311 codec initialized successfully"
✅ Game makes sounds (footsteps, gunfire, etc.)
✅ Music plays (OPL synthesis)
✅ No audio distortion or crackling

## Next Steps After Success

1. **Optimize volume** - Adjust ES8311_DAC_VOLUME register if needed
2. **Test gameplay** - Play a full level to ensure audio stability
3. **Test save/load** - Ensure SD card features still work
4. **Share results** - Document any additional findings

## Rollback Plan

If something breaks:
```bash
# Restore original pins (emergency only):
# In i_sound.c, change back to:
pin_cfg.bck_io_num = 41;
pin_cfg.data_out_num = 42;
pin_cfg.ws_io_num = 43;
# And comment out ES8311_Init() call
```

## Notes
- The ES8311 is much better quality than MAX98357A
- Sample rate 11025 Hz is correct for Doom SFX
- I2C bus is shared safely between keyboard and codec
- Both devices work independently on same bus
