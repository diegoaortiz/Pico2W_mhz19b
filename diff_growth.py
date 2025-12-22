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

# -----------------------
# CO2 sensor
# -----------------------
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

def read_mhz19():
    uart.write(cmd)
    time.sleep(0.05)
    if uart.any() >= 9:
        d = uart.read(9)
        if d and len(d) == 9 and d[0] == 0xFF and d[1] == 0x86:
            return d[2]*256 + d[3]
    return None

# -----------------------
# Colors
# -----------------------
def rgb(r,g,b):
    return gc9a01py.color565(int(r), int(g), int(b))

VERY_GOOD_COLORS = [(180,255,180),(140,255,220)]
GOOD_COLORS = [(130,255,150),(100,255,255)]
MEDIUM_COLORS = [(140,140,255),(255,160,255)]
BAD_COLOR = (255,90,90)

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
num_rings = 12 # AUGMENTÉ: Six anneaux concentriques
segments = 120
pulse_speed = 0.25 
RING_THICKNESS = 1 
RING_SPACING = 8
MAX_LEN_FACTOR = 0.3 

ring_lengths = [[0 for _ in range(segments)] for _ in range(num_rings)]
ring_alphas = [[0 for _ in range(segments)] for _ in range(num_rings)]
ring_phases = [random.random()*2*math.pi for _ in range(num_rings)] 

# -----------------------
# Draw one ring with alpha
# -----------------------
def draw_ring_alpha(lcd, cx, cy, radius, lengths, alphas, color, thickness):
    for i in range(segments):
        col = tuple([c * alphas[i] for c in color])
        col565 = rgb(*col)
        angle = 2*math.pi*i/segments
        x0 = int(cx + radius * math.cos(angle))
        y0 = int(cy + radius * math.sin(angle))
        l = lengths[i]
        x1 = int(x0 + l * math.cos(angle))
        y1 = int(y0 + l * math.sin(angle))
        
        lcd.line(x0, y0, x1, y1, col565)
        
        if thickness > 1:
            dx = x1 - x0
            dy = y1 - y0
            if abs(dx) > abs(dy):
                for t in range(1, thickness):
                    lcd.line(x0, y0 + t, x1, y1 + t, col565)
            else:
                for t in range(1, thickness):
                    lcd.line(x0 + t, y0, x1 + t, y1, col565)


# -----------------------
# Initial state
# -----------------------
lcd.fill(gc9a01py.BLACK) 
last_ppm_value = None
t = 0

# -----------------------
# MAIN LOOP
# -----------------------
while True:

    # Effacement de la zone de l'animation pour la fluidité (sans flicker global)
    lcd.fill_rect(cx - 120, cy - 120, 240, 240, gc9a01py.BLACK) 
    
    # ---- Lecture CO₂ ----
    co2_value = read_mhz19()
    if co2_value is None:
        co2_value = 0

    # 1. AMÉLIORATION: Réactivation de l'affichage dans le terminal
    print("CO2 =", co2_value, "ppm") 

    # ---- Affichage CO₂ ----
    lcd.text(font, str(co2_value), cx - (len(str(co2_value))*8), cy - 16, rgb(255,255,255))
    last_ppm_value = co2_value
        

    # ---- Définition des couleurs, longueurs et complexité de la rampe ----
    if co2_value < 600: # very good
        base_color = interp_color(VERY_GOOD_COLORS[0], VERY_GOOD_COLORS[1], (math.sin(t)+1)/2)
        base_len = 10
        ramp_bumps = 3 # 3 bosses
        active_rings = 4
    elif co2_value < 800: # good
        base_color = interp_color(GOOD_COLORS[0], GOOD_COLORS[1], (math.sin(t)+1)/2)
        base_len = 15
        ramp_bumps = 4 # 4 bosses
        active_rings = 6
    elif co2_value < 1200: # medium
        base_color = interp_color(MEDIUM_COLORS[0], MEDIUM_COLORS[1], (math.sin(t)+1)/2)
        base_len = 20 # AUGMENTÉ la longueur
        ramp_bumps = 6 # 6 bosses plus complexes
        active_rings = 8
    else: # bad
        base_color = BAD_COLOR
        base_len = 25 # AUGMENTÉ la longueur
        ramp_bumps = 8 # 8 bosses, très déformé
        active_rings = 12 # Tous les anneaux actifs
        
    # S'assurer que le nombre d'anneaux actifs n'excède pas num_rings
    active_rings = min(active_rings, num_rings)


    # --- Anneaux Concentriques avec Rampe et Croissance Différentielle ---
    for r in range(num_rings):
        
        base_radius = text_radius + r * RING_SPACING
        
        # Croissance Différentielle
        radius = base_radius + math.sin(t + ring_phases[r]) * 5 
        
        # Opacité de base (décroissance avec l'éloignement)
        alpha_base = 0.8 - r * 0.1 
        if alpha_base < 0.1: alpha_base = 0.1

        for i in range(segments):
            
            # 3. AMÉLIORATION: Complexité de la Rampe Sinusoïdale (Basée sur ramp_bumps)
            # La fréquence des bosses est dynamique et dépend du niveau de CO2
            ramp_factor = 0.5 + 0.5 * math.sin(i / segments * 2 * math.pi * ramp_bumps + t * 0.8)
            
            target_len = base_len * MAX_LEN_FACTOR * ramp_factor

            # Transition de longueur 
            ring_lengths[r][i] += (target_len - ring_lengths[r][i]) * 0.25
            
            # Disparition (si l'anneau n'est plus actif)
            if r < active_rings:
                # Anneau actif : Maintient l'opacité de base
                ring_alphas[r][i] += (alpha_base - ring_alphas[r][i]) * 0.15
            else:
                # Anneau inactif : Disparition très rapide
                ring_alphas[r][i] *= 0.60 

        draw_ring_alpha(lcd, cx, cy, radius, ring_lengths[r], ring_alphas[r], base_color, RING_THICKNESS)
    
    t += pulse_speed
    time.sleep(0.03)