from machine import Pin, UART
import neopixel
import time
import math

# -----------------------
# UART0 MH-Z19B
# -----------------------
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

# -----------------------
# WS2812B
# -----------------------
NUM_LEDS = 6
PIN = 15
np = neopixel.NeoPixel(Pin(PIN), NUM_LEDS)

# -----------------------
# Fonctions utilitaires
# -----------------------
def read_co2():
    uart.write(cmd)
    time.sleep(0.1)
    if uart.any() >= 9:
        data = uart.read(9)
        if data and len(data) == 9 and data[0] == 0xFF and data[1] == 0x86:
            return data[2] * 256 + data[3]
    return None

def lerp(a, b, t):
    return int(a + (b - a) * t)

def lerp_color(c1, c2, t):
    r = lerp(c1[0], c2[0], t)
    g = lerp(c1[1], c2[1], t)
    b = lerp(c1[2], c2[2], t)
    return (r, g, b)

def co2_color_gradient(ppm, t):
    if ppm < 800:
        return lerp_color((0,255,0), (0,255,255), t)   # vert → cyan
    elif ppm < 1200:
        return lerp_color((0,0,255), (255,0,255), t)   # bleu → magenta
    else:
        pulse = int((math.sin(t*5)+1)/2*255)
        return (pulse, 0, 0)                           # rouge pulsé

def set_strip(color):
    for i in range(NUM_LEDS):
        np[i] = color
    np.write()

# -----------------------
# Boucle principale
# -----------------------
t = 0
while True:
    co2 = read_co2()
    if co2 is None:
        co2 = 0
        print("⚠️ Pas de réponse du capteur")
    else:
        print("CO2 =", co2, "ppm")

    color = co2_color_gradient(co2, t)
    set_strip(color)

    t += 0.05
    time.sleep(0.05)
