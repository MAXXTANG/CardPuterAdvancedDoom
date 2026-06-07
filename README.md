# CardPuter ADV — Doom

Doom running on the **M5Stack CardPuter ADV** (ESP32-S3).

Port of [prBoom for GBA](https://github.com/doomhack/GBADoom) by doomhack, adapted for CardPuter ADV by [zspuspoki](https://github.com/zspuspoki/CardPuterAdvancedDoom), originally based on [romalik/m5cardputer_doom](https://github.com/romalik/m5cardputer_doom).

---

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32-S3, 240 MHz, 8 MB flash |
| Keyboard | TCA8418 I2C controller |
| Audio | ES8311 codec (SFX + music) |
| Display | ST7789 135×240 LCD |

---

## Flashing (pre-built binary)

The app embeds the full Doom IWAD (~4.7 MB) and requires a **custom 6 MB partition table**. Flashing only the app binary (e.g. via M5Burner) will result in a black screen. You must flash `partition-table.bin` and `cardputer_doom.bin` together.

### Step 1 — Install esptool

```bash
pip install esptool
```

### Step 2 — Download binaries

Download from [Releases](../../releases):
- `partition-table.bin`
- `cardputer_doom.bin`

### Step 3 — Enter Download Mode

- Power switch on the side → **OFF**
- Hold the **G0** key
- Switch power → **ON**, then release G0

### Step 4 — Flash

**macOS / Linux:**
```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 write_flash \
  0x8000 partition-table.bin \
  0x10000 cardputer_doom.bin
```

**Windows:**
```bash
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash ^
  0x8000 partition-table.bin ^
  0x10000 cardputer_doom.bin
```

Replace the port with your actual device (`ls /dev/cu.*` on macOS, Device Manager on Windows).

---

## Building from source

Requires [ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/index.html).

```bash
git clone --recursive https://github.com/MAXXTANG/CardPuterAdvancedDoom
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

To create a merged binary (flash at 0x0):
```bash
bash merge_firmware.sh
esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 write_flash 0x0 build/merged-flash.bin
```

---

## Controls

| Key | Action |
|-----|--------|
| `;` | Forward |
| `.` | Backward |
| `l` | Turn right |
| `'` | Turn left |
| `opt` | Fire |
| `ctrl` | Strafe left |
| `alt` | Strafe right |
| `fn` | Use / open |
| `tab` | Map |
| `1`–`7` | Weapon select |

## Cheats

| Code | Effect |
|------|--------|
| `iddqd` | God mode |
| `idkfa` | All keys, weapons, armor |
| `idc<x><y>` | Jump to level (e.g. `idc14` → E1M4) |

---

## Status

- [x] Keyboard input (TCA8418)
- [x] Sound effects
- [x] Music
- [x] Save & Load (SD card)
- [x] Cheats
- [ ] Load WAD from SD card
- [ ] Fix minor music issues
