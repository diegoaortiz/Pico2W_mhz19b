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
# Dessiner cercle approximatif
# -----------------------
def draw_circle(display, x0, y0, radius, color):
    for angle in range(0, 360, 5):
        rad = math.radians(angle)
        x = int(x0 + radius * math.cos(rad))
        y = int(y0 + radius * math.sin(rad))
        display.fill_rect(x, y, 1, 1, color)

# -----------------------
# Boucle principale
# -----------------------
angle_offset = 0

while True:
    co2 = read_co2()
    display.fill(gc9a01py.BLACK)

    if co2 is not None:
        color = co2_color(co2)
