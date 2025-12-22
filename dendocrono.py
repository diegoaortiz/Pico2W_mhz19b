import time
import math
import random      # ← AJOUTE ÇA
from machine import Pin, SPI, UART
import gc9a01py


# --- LCD setup ---
spi = SPI(1, baudrate=40000000, polarity=1, phase=1)
dc = Pin(8, Pin.OUT)
cs = Pin(9, Pin.OUT)
rst = Pin(12, Pin.OUT)
bl = Pin(13, Pin.OUT)
bl.on()
lcd = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)

# --- CO2 sensor ---
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))

# --- Colors ---
def rgb(r,g,b):
    return gc9a01py.color565(r,g,b)

GOOD_COLORS   = [(128,255,128),(128,255,255)]
MEDIUM_COLORS = [(128,128,255),(255,128,255)]
BAD_COLOR     = (255,128,128)

def interp_color(c1,c2,t):
    r = int(c1[0]*(1-t)+c2[0]*t)
    g = int(c1[1]*(1-t)+c2[1]*t)
    b = int(c1[2]*(1-t)+c2[2]*t)
    return (r,g,b)

# --- CO2 reading ---
def read_mhz19():
    uart.write(b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79")
    time.sleep(0.05)
    if uart.any():
        d = uart.read(9)
        if d and len(d)==9 and d[0]==0xFF and d[1]==0x86:
            return d[2]*256 + d[3]
    return None

def get_state(ppm):
    if ppm < 800: return "good"
    if ppm < 1200: return "medium"
    return "bad"

# --- Draw circle ---
def draw_circle(lcd, cx, cy, radius, color):
    x = radius
    y = 0
    err = 0
    while x >= y:
        lcd.pixel(int(cx + x), int(cy + y), rgb(*color))
        lcd.pixel(int(cx + y), int(cy + x), rgb(*color))
        lcd.pixel(int(cx - y), int(cy + x), rgb(*color))
        lcd.pixel(int(cx - x), int(cy + y), rgb(*color))
        lcd.pixel(int(cx - x), int(cy - y), rgb(*color))
        lcd.pixel(int(cx - y), int(cy - x), rgb(*color))
        lcd.pixel(int(cx + y), int(cy - x), rgb(*color))
        lcd.pixel(int(cx + x), int(cy - y), rgb(*color))
        y += 1
        if err <= 0:
            err += 2*y + 1
        if err > 0:
            x -= 1
            err -= 2*x + 1

# --- Dendrochronologie params ---
cx, cy = 120, 120
max_radius = 150
ring_width = 3  # épaisseur d’un anneau
current_radius = 10  # départ du centre

while True:
    ppm = read_mhz19()
    if ppm is None:
        continue

    state = get_state(ppm)

    # Couleur de l’anneau selon état
    t = random.random()
    if state=="good":
        color = interp_color(GOOD_COLORS[0], GOOD_COLORS[1], t)
    elif state=="medium":
        color = interp_color(MEDIUM_COLORS[0], MEDIUM_COLORS[1], t)
    else:
        color = BAD_COLOR

    # Dessiner un anneau
    for r in range(current_radius, current_radius+ring_width):
        draw_circle(lcd, cx, cy, r, color)

    # CO2 dans le terminal
    print("CO2 =", ppm, "ppm | état:", state)

    # avancer le rayon
    current_radius += ring_width
    if current_radius + ring_width > max_radius:
        lcd.fill(rgb(0,0,0))
        current_radius = 10

    time.sleep(0.5)
