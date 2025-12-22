import time
import math
import random
from machine import Pin, SPI, UART
import gc9a01py
from fonts.romfonts import vga1_16x32 as font

# -----------------------
# LCD setup
# -----------------------
spi = SPI(1, baudrate=40000000, polarity=1, phase=1)
dc = Pin(8, Pin.OUT)
cs = Pin(9, Pin.OUT)
rst = Pin(12, Pin.OUT)
bl = Pin(13, Pin.OUT)
bl.on()
lcd = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)
lcd.fill(gc9a01py.BLACK)

# -----------------------
# CO2 sensor
# -----------------------
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

# -----------------------
# Colors
# -----------------------
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

# -----------------------
# CO2 reading
# -----------------------
def read_mhz19():
    uart.write(cmd)
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

# -----------------------
# Animation parameters
# -----------------------
cx, cy = 120, 120
text_radius = 60
num_rings = 2
ring_spacing = 8
segments = 120
pulse_speed = 0.1

ring_lengths = [[0 for _ in range(segments)] for _ in range(num_rings)]

# -----------------------
# Draw one ring
# -----------------------
def draw_ring(lcd, cx, cy, radius, lengths, color):
    for i in range(segments):
        angle = 2*math.pi*i/segments
        x0 = int(cx + radius * math.cos(angle))
        y0 = int(cy + radius * math.sin(angle))
        l = lengths[i]
        x1 = int(x0 + l * math.cos(angle))
        y1 = int(y0 + l * math.sin(angle))
        lcd.line(x0, y0, x1, y1, rgb(*color))

# -----------------------
# Initial static text
# -----------------------
lcd.fill(gc9a01py.BLACK)
lcd.text(font, "CO2", cx-30, cy-28, rgb(255,255,255))

t = 0
last_ppm_value = None

while True:

    # ---- Lecture CO₂ ----
    co2_value = read_mhz19()
    if co2_value is None:
        co2_value = 0
    print("CO2 =", co2_value, "ppm")

    # ---- Affichage CO₂ (uniquement si changement, pour stabilité) ----
    if co2_value != last_ppm_value:
        lcd.fill_rect(cx-40, cy+10, 80, 40, gc9a01py.BLACK)
        lcd.text(font, str(co2_value), cx-30, cy+10, rgb(255,255,255))
        last_ppm_value = co2_value

    # ---- Définition des couleurs + longueurs ----
    if co2_value < 800:
        base_color = interp_color(GOOD_COLORS[0], GOOD_COLORS[1], random.random())
        max_len = 20
    elif co2_value < 1200:
        base_color = interp_color(MEDIUM_COLORS[0], MEDIUM_COLORS[1], random.random())
        max_len = 15
    else:
        base_color = BAD_COLOR
        max_len = 10

    # ---- Animation des anneaux ----
    for r in range(num_rings):
        radius = text_radius + r*(ring_spacing + max_len)
        for i in range(segments):
            target_len = max_len * (0.7 + 0.3*math.sin(t + i/5 + r))
            ring_lengths[r][i] += (target_len - ring_lengths[r][i]) * 0.2

        draw_ring(lcd, cx, cy, radius, ring_lengths[r], base_color)

    t += pulse_speed
    time.sleep(0.05)
