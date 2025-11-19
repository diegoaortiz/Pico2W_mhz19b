# mhz19b_ws2812b_sync.py
from machine import Pin, UART
import neopixel
import time
import math

# -------- HW config --------
NUM_LEDS = 24
LED_PIN = 15
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
np = neopixel.NeoPixel(Pin(LED_PIN), NUM_LEDS)
CMD = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

# -------- utils --------
def read_co2(timeout=0.1):
    uart.write(CMD)
    time.sleep(timeout)
    if uart.any() >= 9:
        d = uart.read(9)
        if d and len(d) == 9 and d[0] == 0xFF and d[1] == 0x86:
            return d[2]*256 + d[3]
    return None

def lerp(a,b,t):
    return int(a + (b-a)*t)

def lerp_color(c1,c2,t):
    return (lerp(c1[0],c2[0],t), lerp(c1[1],c2[1],t), lerp(c1[2],c2[2],t))

# color zones:
GOOD_A = (0,255,0)    # green
GOOD_B = (0,255,255)  # cyan
MID_A  = (0,0,255)    # blue
MID_B  = (255,0,255)  # magenta
BAD    = (255,0,0)    # red

# brightness scaling to protect power
BRIGHTNESS = 0.6  # 0..1 -> multiply channels

def scale_color(col, s):
    return (int(col[0]*s), int(col[1]*s), int(col[2]*s))

def co2_color(ppm, t):
    # t in [0,1] local phase for gradient/pulse
    if ppm < 800:
        return lerp_color(GOOD_A, GOOD_B, t)
    elif ppm < 1200:
        return lerp_color(MID_A, MID_B, t)
    else:
        # pulsed red
        pulse = (math.sin(t*6.28) + 1)/2  # 0..1
        val = int(128 + pulse*127)
        return (val, 0, 0)

def set_gradient(center_color):
    # simple gradient from center_color to a dimmer variant along strip
    for i in range(NUM_LEDS):
        pos = i / (NUM_LEDS-1)
        # mix center_color with black for tail
        c = (int(center_color[0]*(1-pos)), int(center_color[1]*(1-pos)), int(center_color[2]*(1-pos)))
        np[i] = scale_color(c, BRIGHTNESS)
    np.write()

# -------- main loop --------
t = 0.0
try:
    while True:
        ppm = read_co2()
        if ppm is None:
            print("No CO2 data")
            ppm = 0
        else:
            print("CO2:", ppm, "ppm")

        # choose phase for smooth transitions
        phase = (math.sin(t*0.5)+1)/2  # slow wave 0..1

        color = co2_color(ppm, phase)

        # faster pulse when bad
        if ppm >= 1200:
            # amplify time for pulse
            t += 0.2
        else:
            t += 0.04

        set_gradient(color)
        time.sleep(0.06)

except KeyboardInterrupt:
    # clear on exit
    for i in range(NUM_LEDS):
        np[i] = (0,0,0)
    np.write()
    print("Stopped")
