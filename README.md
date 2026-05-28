# CO2 Display - Pico + MH-Z19B + GC9A01

Affichage CO2 avec animation wave sur écran rond 1.28" (240x240).

## Hardware

- **Board** : Raspberry Pi Pico 2W ou Waveshare RP2040-LCD-1.28
- **Display** : GC9A01 240x240 round LCD
- **Sensor** : MH-Z19B CO2 sensor

## Wiring

| Pico | GC9A01 Display |
|------|----------------|
| GP10 | SCK |
| GP11 | MOSI |
| GP13 | CS |
| GP14 | DC |
| GP15 | RST |
| GP12 | BL |

| Pico | LED ring / WS2812 |
|------|-------------------|
| GP22 | DIN |
| 5V   | 5V |
| GND  | GND |

| Pico | MH-Z19B |
|------|---------|
| GP0  | TX |
| GP1  | RX |
| 5V   | VIN |
| GND  | GND |

> Le firmware Arduino/PlatformIO utilise `Adafruit NeoPixel` pour le ruban LED. Ajuster `LED_COUNT` dans `src/main.cpp` si l'anneau n'a pas 24 LEDs.

## Flash Instructions

### 1. Download Firmware

Go to: https://github.com/russhughes/gc9a01_mpy/tree/main/firmware

Download `.uf2` for your board:
- `RPI_PICO_W/` for Pico 2W
- `WAVESHARE_RP2040_LCD_1.28/` for Waveshare integrated board

### 2. Flash the Pico

1. **Disconnect** USB
2. Hold **BOOTSEL** button
3. Reconnect USB (keep holding BOOTSEL)
4. Release BOOTSEL when **RPI-RP2** drive appears
5. Drag & drop the `.uf2` file
6. Pico auto-reboots

### 3. Upload Code

Use **Thonny** (recommended):
1. Open Thonny
2. `Run → Configure interpreter → MicroPython (Raspberry Pi Pico)`
3. Copy `main.py` to Pico
4. Restart

## CO2 Levels

| Level | Color | Action |
|-------|-------|--------|
| < 600 ppm | 🟢 Green | Excellent |
| 600-800 ppm | 🔵 Cyan | Good |
| 800-1200 ppm | 🟣 Purple | Open windows |
| > 1200 ppm | 🔴 Red | Ventilate! |
