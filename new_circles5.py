import time
import math
import random
import gc 
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

# -----------------------
# CO2 sensor 
# -----------------------
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

last_co2_value = 400 

def read_mhz19():
    global last_co2_value 
    uart.write(cmd)
    time.sleep(0.05)
    
    if uart.any() >= 9:
        d = uart.read(9)
        if d and len(d) == 9 and d[0] == 0xFF and d[1] == 0x86:
            co2 = d[2]*256 + d[3]
            if co2 > 300: 
                last_co2_value = co2
                return co2
    return last_co2_value

# -----------------------
# Colors & utils
# -----------------------
def rgb(r,g,b):
    return gc9a01py.color565(int(r), int(g), int(b))

TRANSITION_COLORS = {
    "VERY_GOOD": ((100, 255, 100), (0, 255, 255)),
    "GOOD": ((0, 255, 255), (0, 0, 200)), 
    "MEDIUM": ((0, 0, 200), (255, 0, 255)), 
    "BAD": ((255, 0, 0), (255, 100, 0))
}

def interp_color(c1,c2,t):
    return (
        int(c1[0]*(1-t)+c2[0]*t),
        int(c1[1]*(1-t)+c2[1]*t),
        int(c1[2]*(1-t)+c2[2]*t)
    )

# -----------------------
# Animation parameters
# -----------------------
cx, cy = 120, 120 
text_radius = 42

num_rings = 12
segments = 120 

ANIM_SPEED = 0.02 
WAVE_SPEED = 1.0          # Vitesse onde interne
PHASE_OFFSET = 0.35       # Décalage entre anneaux : crée la vague
MAX_SCREEN_COORD = 239
RING_THICKNESS = 10 
MIN_INTENSITY = 0.35 

smooth_active_rings = float(num_rings)
SMOOTHING_FACTOR = 0.1 

t_anim = 0.0

# -----------------------
# Draw ring
# -----------------------
def draw_ring_optimized(lcd, ax, ay, radius, color_rgb, thickness, wave_alpha):
    if wave_alpha < 0.001 or radius <= 0:
        return

    col_r = int(color_rgb[0] * wave_alpha)
    col_g = int(color_rgb[1] * wave_alpha)
    col_b = int(color_rgb[2] * wave_alpha)
    col565 = rgb(col_r, col_g, col_b)
        
    for i in range(segments):
        angle = 2 * math.pi * i / segments
        x0 = int(ax + radius * math.cos(angle))
        y0 = int(ay + radius * math.sin(angle))
        if 0 <= x0 <= MAX_SCREEN_COORD and 0 <= y0 <= MAX_SCREEN_COORD:
            lcd.fill_rect(x0 - thickness//2, y0 - thickness//2, thickness, thickness, col565)

# -----------------------
# MAIN LOOP
# -----------------------
while True:
    t_anim += ANIM_SPEED
    co2_value = read_mhz19()

    if co2_value < 600:
        color_set = TRANSITION_COLORS["VERY_GOOD"]; target_active_rings = 10; breath_speed = 0.6
    elif co2_value < 800:
        color_set = TRANSITION_COLORS["GOOD"]; target_active_rings = 10; breath_speed = 0.9
    elif co2_value < 1200:
        color_set = TRANSITION_COLORS["MEDIUM"]; target_active_rings = 10; breath_speed = 1.4
    else:
        color_set = TRANSITION_COLORS["BAD"]; target_active_rings = 4; breath_speed = 2.0
            
    color_start_rgb = color_set[0]
    color_end_rgb = color_set[1]
    
    smooth_active_rings += (target_active_rings - smooth_active_rings) * SMOOTHING_FACTOR
    
    lcd.fill_rect(0, 0, 240, 240, gc9a01py.BLACK)
    
    ax = cx
    ay = cy

    # Rayon minimal = cercle du CO₂
    MIN_RADIUS = text_radius + 8       # juste autour du texte
    MAX_RADIUS = 118                   # bord de l'écran

    for r in range(num_rings):

        # ondulation d'intensité
        wave_phase = (t_anim * WAVE_SPEED) + r * 0.3
        wave_alpha_peak = max(0, math.cos(wave_phase))**2
        
        current_activation = max(0.0, min(1.0, smooth_active_rings - r))
        wave_alpha = MIN_INTENSITY + (1 - MIN_INTENSITY) * wave_alpha_peak * current_activation
        if wave_alpha < 0.001:
            continue

        # dégradé de couleur
        t_interp = r / (num_rings - 1)
        radial_color_rgb = interp_color(color_start_rgb, color_end_rgb, t_interp)

        # -------------------------------------------------
        # 🟦 ANIMATION FIXE SUR TOUT L’ESPACE (CORRECTION)
        # -------------------------------------------------
        # Chaque anneau oscille sur toute la zone :
        # MIN_RADIUS → MAX_RADIUS
        #
        # Mais décalé en phase pour donner l’effet ondulant.
        # -------------------------------------------------

        phase = t_anim * breath_speed + r * PHASE_OFFSET
        wave = 0.5 + 0.5 * math.sin(phase)   # 0..1

        animated_radius = MIN_RADIUS + wave * (MAX_RADIUS - MIN_RADIUS)

        draw_ring_optimized(
            lcd, ax, ay,
            animated_radius,
            radial_color_rgb,
            RING_THICKNESS,
            wave_alpha
        )

    lcd.text(font, str(co2_value), cx - (len(str(co2_value))*8), cy - 16, rgb(255, 255, 255))
    
    time.sleep(0.05)
