from machine import Pin, SPI, UART
import time
import gc9a01py
from fonts.romfonts import vga1_16x32 as font
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
max_radius = 80
pulse_speed = 0.2
text_base_y = 110

# -----------------------
# Couleur CO2
# -----------------------
def co2_color(ppm):
    if ppm < 800:
        return gc9a01py.GREEN
    elif ppm < 1200:
        return gc9a01py.MAGENTA
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
# Dessiner cercle avec des lignes
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
# Boucle principale
# -----------------------
angle_offset = 0

while True:
    co2 = read_co2()
    display.fill(gc9a01py.BLACK)

    if co2 is not None:
        color = co2_color(co2)
        txt = "{} ppm".format(co2)
        fill_ratio = min(max(co2 / 2000, 0), 1)
        print("CO2 =", co2, "ppm")
    else:
        color = gc9a01py.WHITE
        txt = "No data"
        fill_ratio = 0
        print("⚠️ Pas de réponse du capteur")

    # Cercles concentriques animés
    for r in range(20, max_radius, 10):
        pulse = int(5 * math.sin(angle_offset + r))
        draw_circle_lines(display, cx, cy, r + pulse, color)

    # Jauge circulaire simple (segments)
    steps = int(36 * fill_ratio)
    for i in range(steps):
        angle_deg = i * (360 / 36)
        rad = math.radians(angle_deg)
        x = int(cx + max_radius * math.cos(rad))
        y = int(cy + max_radius * math.sin(rad))
        display.fill_rect(x, y, 2, 2, color)

    # Texte CO2 qui pulse
    pulse_offset = int(5 * math.sin(angle_offset * 2))
    display.text(font, txt, 60, text_base_y + pulse_offset, color)

    angle_offset += pulse_speed
    time.sleep(0.05)
