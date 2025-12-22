import time
import math
import random
from machine import Pin, SPI, UART
import gc9a01py
from fonts.romfonts import vga1_16x32 as font

# --- LCD setup ---
spi = SPI(1, baudrate=40000000, polarity=1, phase=1)
dc = Pin(8, Pin.OUT)
cs = Pin(9, Pin.OUT)
rst = Pin(12, Pin.OUT)
bl = Pin(13, Pin.OUT)
bl.on()
lcd = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)
lcd.fill(gc9a01py.BLACK)

# --- CO2 sensor ---
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

# --- Colors ---
def rgb(r,g,b):
    return gc9a01py.color565(r,g,b)

GOOD_COLORS   = [(128,255,128),(128,255,255)]
MEDIUM_COLORS = [(128,128,255),(255,128,255)]
BAD_COLOR     = (255,128,128)
TEXT_COLOR    = rgb(255,255,255)

def interp_color(c1,c2,t):
    r = int(c1[0]*(1-t)+c2[0]*t)
    g = int(c1[1]*(1-t)+c2[1]*t)
    b = int(c1[2]*(1-t)+c2[2]*t)
    return (r,g,b)

# --- CO2 reading ---
def read_mhz19():
    uart.write(cmd)
    time.sleep(0.05)
    if uart.any() >= 9:
        d = uart.read(9)
        if d and len(d)==9 and d[0]==0xFF and d[1]==0x86:
            return d[2]*256 + d[3]
    return None

# --- Draw circle with color gradient (colors close to base color) ---
def draw_circle_gradient(lcd, cx, cy, radius, base_color, alpha, segments=90):
    points = []
    for i in range(segments+1):
        angle = 2*math.pi*i/segments
        r = radius
        x = int(cx + r * math.cos(angle))
        y = int(cy + r * math.sin(angle))
        points.append((x, y, angle))
    for i in range(len(points)-1):
        x0, y0, a0 = points[i]
        x1, y1, a1 = points[i+1]
        # dégradé subtil : variation légère de chaque composante
        factor = 0.7 + 0.3 * math.sin(a0*5 + alpha*2)
        color = tuple(min(int(c*factor),255) for c in base_color)
        lcd.line(x0, y0, x1, y1, rgb(*color))

# --- Parameters ---
cx, cy = 120, 120
text_radius = 60
start_radius = text_radius + 5
max_radius = 130
num_rings = 5
ring_width = 14
pulse_speed = 0.25  # plus rapide

# Anneaux
rings = []
for i in range(num_rings):
    rings.append({
        'radius': start_radius + i*ring_width,
        'phase': i * 0.5
    })

t = 0
while True:
    co2_value = read_mhz19()
    if co2_value is None:
        co2_value = 0
        txt = "No data"
    else:
        txt = "{} ppm".format(co2_value)
        print("CO2 =", co2_value, "ppm")

    lcd.fill(gc9a01py.BLACK)

    # --- Texte fixe ---
    lcd.text(font, txt, cx - len(txt)*8, cy - 16, TEXT_COLOR)

    # --- Cercle blanc autour du texte ---
    for w in range(3):
        draw_circle_gradient(lcd, cx, cy, text_radius + w, (255,255,255), 1)

    # --- Couleur selon CO2 ---
    if co2_value < 800:
        base_color = interp_color(GOOD_COLORS[0], GOOD_COLORS[1], random.random())
    elif co2_value < 1200:
        base_color = interp_color(MEDIUM_COLORS[0], MEDIUM_COLORS[1], random.random())
    else:
        base_color = BAD_COLOR

    # --- Anneaux animés avec respiration + gradient subtile ---
    for ring in rings:
        radius = ring['radius'] + math.sin(t + ring['phase'])*6
        alpha = 0.4 + 0.6*math.sin(t + ring['phase'])**2
        for w in range(ring_width):
            draw_circle_gradient(lcd, cx, cy, radius + w, base_color, alpha)
        # avancer l’anneau
        ring['radius'] += 1.0  # plus rapide
        if ring['radius'] > max_radius:
            ring['radius'] = start_radius

    t += pulse_speed
    time.sleep(0.02)  # boucle plus rapide
