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
# Draw circle with alpha
# -----------------------
def draw_circle_alpha(lcd, cx, cy, radius, color, alpha, segments=60):
    points = []
    for i in range(segments+1):
        angle = 2*math.pi*i/segments
        x = int(cx + radius*math.cos(angle))
        y = int(cy + radius*math.sin(angle))
        points.append((x,y))
    r = int(color[0]*alpha)
    g = int(color[1]*alpha)
    b = int(color[2]*alpha)
    color565 = rgb(r,g,b)
    for i in range(len(points)-1):
        x0,y0 = points[i]
        x1,y1 = points[i+1]
        lcd.line(x0,y0,x1,y1,color565)

# -----------------------
# Dendrochronologie params
# -----------------------
cx, cy = 120, 120
min_radius = 0
max_radius = 120
ring_width = 80  # anneau plus épais
pulse_speed = 0.05
num_rings = 12
ring_spacing = 5  # petit espace entre anneaux

# -----------------------
# Main loop
# -----------------------
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
    # texte au centre
    display_color = interp_color(GOOD_COLORS[0], GOOD_COLORS[1], 0.5)
    lcd.text(font, txt, 60, 110, rgb(*display_color))

    # Couleur dynamique selon ppm
    if co2_value < 800:
        base_color = interp_color(GOOD_COLORS[0], GOOD_COLORS[1], random.random())
    elif co2_value < 1200:
        base_color = interp_color(MEDIUM_COLORS[0], MEDIUM_COLORS[1], random.random())
    else:
        base_color = BAD_COLOR

    # Dessin des anneaux avec respiration
    for i in range(num_rings):
        radius = max_radius/num_rings * (i+1) + math.sin(t + i*0.5)*5
        alpha = 0.3 + 0.7*math.sin(t + i*0.3)**2  # effet allumage/extinction
        draw_circle_alpha(lcd, cx, cy, radius, base_color, alpha)

    t += pulse_speed
    time.sleep(0.05)
