from machine import Pin
import neopixel
import time

NUM_LEDS = 24
PIN = 15
np = neopixel.NeoPixel(Pin(PIN), NUM_LEDS)

def clear():
    for i in range(NUM_LEDS):
        np[i] = (0,0,0)
    np.write()

try:
    while True:
        for i in range(NUM_LEDS):
            clear()
            np[i] = (0,150,0)   # pixel vert visible mais pas max
            np.write()
            print("LED", i)
            time.sleep(0.12)
except KeyboardInterrupt:
    clear()
    print("Stopped")
