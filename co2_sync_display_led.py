from machine import Pin, SPI, UART
import neopixel
import time, math
import gc9a01py
from fonts.romfonts import vga1_16x32 as font

# ========================
# HARDWARE CONFIG
# ========================

# --- CO2 Sensor (MH-Z19B) ---
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
CMD = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

# --- WS2812 Ring ---
NUM_LEDS = 24
LED_PIN = 15
np = neopixel.NeoPixel(Pin(LED_PIN), NUM_LEDS)
BRIGHTNESS = 0.6   # Global scaling

# --- GC9A01 Display ---
spi = SPI(1, baudrate=20000000, polarity=1, phase=1,
          sck=Pin(10), mosi=Pin(11))
dc = Pin(8, Pin.OUT)
cs = Pin(9, Pin.OUT)
rst = Pin(12, Pin.OUT)
bl = Pin(13, Pin.OUT)
bl.on()

display = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)
display.fill(0)

# ========================
# HELPERS
# ========================

def read_co2(timeout=0.1):
    uart.write(CMD)
    time.sleep(timeout)
    if uart.any() >= 9:
        d = uart.read(9)
        if d and len(d) == 9 and d[0] == 0xFF and d[1] == 0x86:
            return d[2]*256 + d[3]
    return None

def lerp(a,b,t):
    return int(a + (b-a)*t)

def lerp_color(a,b,t):
    return (lerp(a[0],b[0],t), lerp(a[1],b[1],t), lerp(a[2],b[2],t))

def scale(col, s):
    return (int(col[0]*s), int(col[1]*s), int(col[2]*s))

# Color zones
GOOD_A = (0,255,0)
GOOD_B = (0,255,255)
MID_A  = (0,0,255)
MID_B  = (255,0,255)
BAD    = (255,0,0)

def co2_color(ppm, t):
    if ppm < 800:
        return lerp_color(GOOD_A, GOOD_B, t)
    elif ppm < 1200:
        return lerp_color(MID_A, MID_B, t)
    else:
        # pulsed red
        pulse = (math.sin(t*6.28)+1)/2
        v = int(100 + pulse*155)
        return (v,0,0)

# ========================
# LED RING SYNCHRO
# ========================

def ring_set(color):
    for i in range(NUM_LEDS):
        fade = 1 - (i / (NUM_LEDS - 1))
        col = scale((int(color[0]*fade), int(color[1]*fade), int(color[2]*fade)), BRIGHTNESS)
        np[i] = col
    np.write()

# ========================
# DISPLAY SYNCHRO
# ========================

def draw_screen(ppm, color, t):
    display.fill(0)

    # --- Concentric animated rings ---
    cx, cy = 120, 120
    max_r = 110

    for i in range(5):
        r = int(max_r * (i+1)/5)
        mod = (math.sin(t*0.5 + i*0.6) + 1)/2  # wave
        c = lerp_color((0,0,0), color, mod)
        display.circle(cx, cy, r, gc9a01py.color565(*c))

    # --- CO₂ text ---
    txt = "CO2: {} ppm".format(ppm)
    display.text(font, txt, 20, 100, gc9a01py.color565(*color))

    display.show()

# ========================
# MAIN LOOP
# ========================

t = 0.0
while True:
    ppm = read_co2()
    if ppm is None:
        ppm = 0

    # Wave phase
    phase = (math.sin(t*0.1) + 1)/2

    col = co2_color(ppm, phase)

    # LED ring sync
    ring_set(col)

    # Display sync
    draw_screen(ppm, col, t)

    # Speed depending on CO₂ quality
    if ppm < 800:
        t += 0.03   # smooth
    elif ppm < 1200:
        t += 0.06   # medium
    else:
        t += 0.15   # fast pulse

    time.sleep(0.03)
