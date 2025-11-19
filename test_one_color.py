from machine import Pin
import neopixel
import time

NUM_LEDS = 24
PIN = 15
np = neopixel.NeoPixel(Pin(PIN), NUM_LEDS)

# Allume toutes les LED en rouge faible (évite pics de courant)
for i in range(NUM_LEDS):
    np[i] = (64, 0, 0)
np.write()
print("Écrit: rouge faible sur toutes les LEDs")
