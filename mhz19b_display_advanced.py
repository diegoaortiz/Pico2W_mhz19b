from machine import Pin, SPI, UART
import gc9a01py
from fonts.romfonts import vga1_16x32 as font
import time
import math

# -----------------------
# UART0 MH-Z19B
# -----------------------
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

# -----------------------
# SPI écran GC9A01
# -----------------------
spi = SPI(1, baudrate=20000000, polarity=1, phase=1,
          sck=Pin(10), mosi=Pin(11))
dc = Pin(8, Pin.OUT)
cs = Pin(9, Pin.OUT)
rst = Pin(12, Pin.OUT)
bl = Pin(13, Pin.OUT)
bl.on()

display = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)
display.fill(gc9a01py.BLACK)

# -----------------------
# Paramètres visuels
# -----------------------
cx, cy = 120, 120
max_radius = 70
pulse_speed = 0.05
segments = 60

# -----------------------
# Gradient color functions
# -----------------------
def lerp_color(c1, c2, t):
    """Interpole entre deux couleurs 16-bit"""
    r1 = (c1 >> 11) & 0x1F
    g1 = (c1 >> 5) & 0x3F
    b1 = c1 & 0x1F
    r2 = (c2 >> 11) & 0x1F
    g2 = (c2 >> 5) & 0x3F
    b2 = c2 & 0x1F
    r = int(r1 + (r2 - r1) * t)
    g = int(g1 + (g2 - g1) * t)
    b = int(b1 + (b2 - b1) * t)
    return (r << 11) | (g << 5) | b

def co2_color_gradient(ppm, t):
    if ppm < 800:
        return lerp_color(gc9a01py.GREEN, gc9a01py.CYAN, t)
    elif ppm < 1200:
        return lerp_color(gc9a01py.BLUE, gc9a01py.MAGENTA, t)
    else:
        return gc9a01py.RED

# -----------------------
# Lecture CO2
# -----------------------
def read_co2():
    uart.write(cmd)
    time.sleep(0.1)
    if uart.any() >= 9:
        data = uart.read(9)
        if data and len(data) == 9 and data[0] == 0xFF and data[1] == 0x86:
            return data[2] * 256 + data[3]
    return None

# -----------------------
# Cercle approximatif avec lignes
# -----------------------
def draw_circle_lines(display, cx, cy, radius, color, segments=36):
    points = []
    for i in range(segments + 1):
        angle = 2 * math.pi * i / segments
        x = int(cx + radius * math.cos(angle))
        y = int(cy + radius * math.sin(angle))
        points.append((x, y))
    for i in range(len(points) - 1):
        x0, y0 = points[i]
        x1, y1 = points[i + 1]
        display.line(x0, y0, x1, y1, color)

# -----------------------
# Courbe type Bezier
# -----------------------
def draw_bezier_circle(display, cx, cy, radius, t, segments=36, amplitude=5):
    points = []
    for i in range(segments + 1):
        angle = 2 * math.pi * i / segments
        # sinus pour créer une vague
        r = radius + math.sin(angle*3 + t) * amplitude
        x = int(cx + r * math.cos(angle))
        y = int(cy + r * math.sin(angle))
        points.append((x, y))
    for i in range(len(points) - 1):
        x0, y0 = points[i]
        x1, y1 = points[i + 1]
        display.line(x0, y0, x1, y1, co2_color_gradient(co2_value, t))

# -----------------------
# Boucle principale
# -----------------------
t = 0
while True:
    co2_value = read_co2()
    display.fill(gc9a01py.BLACK)

    if co2_value is None:
        co2_value = 0
        txt = "No data"
    else:
        txt = "{} ppm".format(co2_value)
        print("CO2 =", co2_value, "ppm")

    # Animation: 3 cercles concentriques avec courbes Bezier
    for i, rad in enumerate([30, 50, 70]):
        amp = 3 if co2_value < 800 else 6 if co2_value < 1200 else 10
        draw_bezier_circle(display, cx, cy, rad, t + i, amplitude=amp)

    # Texte CO2 qui pulse légèrement
    pulse_offset = int(5 * math.sin(t * 1.5))
    display.text(font, txt, 60, 110 + pulse_offset, co2_color_gradient(co2_value, t))

    t += pulse_speed
    time.sleep(0.05)
