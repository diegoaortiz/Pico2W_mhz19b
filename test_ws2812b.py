from machine import Pin
import neopixel
import time

NUM_LEDS = 24
PIN = 15

np = neopixel.NeoPixel(Pin(PIN), NUM_LEDS)

# Test simple : allume chaque LED en rouge, puis vert, puis bleu
colors = [(255,0,0), (0,255,0), (0,0,255)]

while True:
    for color in colors:
        for i in range(NUM_LEDS):
            np[i] = color
        np.write()
        time.sleep(0.5)
