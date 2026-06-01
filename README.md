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

> L'affichage est rendu avec **LVGL v9** (anneaux « pulse » anti-aliasés animés par le moteur `lv_anim`). `Arduino_GFX` ne sert que de driver de flush bas niveau pour le GC9A01. La config LVGL est dans `include/lv_conf.h` (activée via `-DLV_CONF_INCLUDE_SIMPLE`).

## Build & Flash (PlatformIO)

Le firmware est en **C++ / Arduino** (core earlephilhower) géré par **PlatformIO**.

```bash
# Compiler
pio run

# Compiler + flasher le Pico 2W (en BOOTSEL, ou via USB si déjà flashé)
pio run --target upload

# Moniteur série (115200 baud)
pio device monitor
```

Pour le tout premier flash : maintenir **BOOTSEL**, brancher l'USB, relâcher quand le lecteur **RPI-RP2** apparaît, puis `pio run -t upload`.

> Les anciens fichiers MicroPython (`main.py`, `code.py`, etc.) sont obsolètes : le firmware actif est `src/main.cpp`.

## Configuration WiFi (portail captif)

À la première mise sous tension (ou après un reset), si aucun identifiant WiFi n'est enregistré, l'appareil démarre un **point d'accès ouvert** et un **portail captif** :

1. L'écran affiche `Config WiFi`, le nom du réseau (`alba-XXXXXX`) et l'IP (`192.168.4.1`).
2. Sur un téléphone/PC, rejoindre le réseau WiFi **`alba-XXXXXX`** (ouvert, sans mot de passe).
3. La page de configuration s'ouvre automatiquement (sinon aller sur `http://192.168.4.1`).
4. Choisir son réseau dans la liste scannée, saisir le mot de passe, **Connecter**.
5. L'appareil enregistre les identifiants (LittleFS) et redémarre connecté.

Une fois connecté, l'IP locale de l'appareil sert une **page de statut** (CO2 en direct, réseau, signal, bouton « Oublier le WiFi »).

### Réinitialiser le WiFi

- Depuis la page de statut : bouton **« Oublier le WiFi »**, ou
- Via le moniteur série : envoyer la touche **`r`**.

L'appareil efface la config et redémarre en mode configuration.

## CO2 Levels

| Level | Color | Action |
|-------|-------|--------|
| < 600 ppm | 🟢 Green | Excellent |
| 600-800 ppm | 🔵 Cyan | Good |
| 800-1200 ppm | 🟣 Purple | Open windows |
| > 1200 ppm | 🔴 Red | Ventilate! |
