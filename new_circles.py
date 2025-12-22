import time
import math
import random
import gc 
from machine import Pin, SPI, UART
import gc9a01py
from fonts.romfonts import vga1_16x32 as font

# -----------------------
# LCD setup (Identique)
# -----------------------
spi = SPI(1, baudrate=40000000, polarity=1, phase=1)
dc = Pin(8, Pin.OUT)
cs = Pin(9, Pin.OUT)
rst = Pin(12, Pin.OUT)
bl = Pin(13, Pin.OUT)
bl.on()
lcd = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)

# -----------------------
# CO2 sensor (Identique)
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
# Couleurs et Interpolation
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

def interp_color_with_intensity(rgb_color, intensity):
    return (
        int(rgb_color[0] * intensity),
        int(rgb_color[1] * intensity),
        int(rgb_color[2] * intensity)
    )

# -----------------------
# Paramètres d'Animation
# -----------------------
cx, cy = 120, 120 # Centre de l'écran (pour le texte)
EXCENTRIC_RADIUS = 5 # Amplitude de l'oscillation excentrique
text_radius = 42
RING_SPACING = 10
num_rings = 12
segments = 120 
PULSE_SPEED = 0.02 
t_anim = 0 
MAX_SCREEN_COORD = 239
RING_THICKNESS_FAST = RING_SPACING + 2 

# Variables d'état pour les anneaux complexes (BAD/MEDIUM)
ring_lengths = [[0 for _ in range(segments)] for _ in range(num_rings)]
ring_alphas = [[0 for _ in range(segments)] for _ in range(num_rings)]
ring_activation_factor = [0.0] * num_rings
SMOOTHING_FACTOR = 0.15 
MIN_RING_LEN = 2
MAX_LEN_FACTOR = 0.3


# -----------------------
# Draw one ring (Optimisé)
# -----------------------
def draw_ring_optimized(lcd, ax, ay, radius, lengths, alphas, color_rgb, thickness, mode):
    """Dessine un anneau par segments/points, centré sur (ax, ay)."""
    
    if mode == "FAST_CIRCLE":
        thickness = RING_THICKNESS_FAST 
        
    for i in range(segments):
        
        angle = 2*math.pi*i/segments
        
        if mode == "COMPLEX_ANIM":
            alpha = alphas[i]
            l = lengths[i]
            if alpha < 0.001: continue
        else:
            alpha = 1.0 
            l = 0       
            
        current_color_rgb = interp_color_with_intensity(color_rgb, alpha)
        col565 = rgb(*current_color_rgb)
        
        x0 = int(ax + radius * math.cos(angle))
        y0 = int(ay + radius * math.sin(angle))
        
        if mode == "FAST_CIRCLE":
            # Dessiner un point épais. 
            if 0 <= x0 < MAX_SCREEN_COORD and 0 <= y0 < MAX_SCREEN_COORD:
                lcd.fill_rect(x0 - thickness//2, y0 - thickness//2, thickness, thickness, col565)
        
        else: # COMPLEX_ANIM (Lignes animées)
            x1 = int(x0 + l * math.cos(angle))
            y1 = int(y0 + l * math.sin(angle))
            
            # Dessin de la ligne principale
            if 0 <= x0 <= MAX_SCREEN_COORD and 0 <= y0 <= MAX_SCREEN_COORD and \
               0 <= x1 <= MAX_SCREEN_COORD and 0 <= y1 <= MAX_SCREEN_COORD:
                lcd.line(x0, y0, x1, y1, col565)


# -----------------------
# MAIN LOOP
# -----------------------
while True:
    t_anim += PULSE_SPEED
    
    co2_value = read_mhz19()
    if co2_value is None: co2_value = 0

    # 1. DÉFINITION DE L'ÉTAT ET DES COULEURS (Identique)
    if co2_value < 800:
        STATE = "FAST_CIRCLE"
        if co2_value < 600:
            color_set = TRANSITION_COLORS["VERY_GOOD"]; target_active_rings = 10; THICKNESS = 10
        else:
            color_set = TRANSITION_COLORS["GOOD"]; target_active_rings = 8; THICKNESS = 10
    else:
        STATE = "COMPLEX_ANIM"
        if co2_value < 1200:
            color_set = TRANSITION_COLORS["MEDIUM"]; target_active_rings = 6; THICKNESS = 1 
        else:
            color_set = TRANSITION_COLORS["BAD"]; target_active_rings = 4; THICKNESS = 1
            
    color_start_rgb = color_set[0]
    color_end_rgb = color_set[1]
    target_active_rings = min(target_active_rings, num_rings)
    
    # 2. EFFACEMENT DE L'ÉCRAN
    lcd.fill_rect(0, 0, 240, 240, gc9a01py.BLACK) 
    
    # 3. CALCUL DE LA PULSATION ET DE L'EXCENTRICITÉ
    
    # Pulsation (pour l'intensité)
    PULSE_MODULATION = (math.sin(t_anim) + 1.0) * 0.5 
    MIN_INTENSITY_FACTOR = 0.3 
    dynamic_intensity_factor = MIN_INTENSITY_FACTOR + (1.0 - MIN_INTENSITY_FACTOR) * PULSE_MODULATION

    # Excentricité (pour le décalage du centre d'animation)
    ax = int(cx + EXCENTRIC_RADIUS * math.sin(t_anim))
    ay = int(cy + EXCENTRIC_RADIUS * math.cos(t_anim * 0.5)) # Vitesse différente pour un mouvement moins régulier
    
    # 4. MISE À JOUR (COMPLEX_ANIM seulement)
    if STATE == "COMPLEX_ANIM":
        for r in range(num_rings):
            target_activation = 1.0 if r < target_active_rings else 0.0
            ring_activation_factor[r] += (target_activation - ring_activation_factor[r]) * SMOOTHING_FACTOR
            current_activation = ring_activation_factor[r]
            
            if current_activation < 0.005 and target_activation == 0.0: continue

            for i in range(segments):
                target_len = (MIN_RING_LEN + 15 * PULSE_MODULATION) * current_activation 
                ring_lengths[r][i] += (target_len - ring_lengths[r][i]) * 0.25
                ring_alphas[r][i] = current_activation * dynamic_intensity_factor

    # 5. DESSIN EXCENTRIQUE (du plus grand au plus petit)
    for r in range(num_rings - 1, -1, -1):
        
        current_activation = ring_activation_factor[r] if STATE == "COMPLEX_ANIM" else (1.0 if r < target_active_rings else 0.0)
        
        if current_activation < 0.005: continue
            
        # Dégradé Radial
        if target_active_rings > 1: t_interp = r / (target_active_rings - 1)
        else: t_interp = 0
        t_interp = max(0.0, min(1.0, t_interp)) 
        
        radial_color_rgb = interp_color(color_start_rgb, color_end_rgb, t_interp)
        
        if STATE == "FAST_CIRCLE":
            final_color_rgb = interp_color_with_intensity(radial_color_rgb, dynamic_intensity_factor)
        else:
            final_color_rgb = radial_color_rgb

        base_radius = text_radius + r * RING_SPACING
        
        # Le centre utilisé ici est l'animation excentrique (ax, ay)
        draw_ring_optimized(lcd, ax, ay, base_radius + RING_SPACING, ring_lengths[r], ring_alphas[r], final_color_rgb, THICKNESS, STATE)


    # 6. AFFICHAGE DE LA VALEUR CO2 (SANS FOND NOIR)
    # Les coordonnées restent centrées sur cx, cy
    lcd.text(font, str(co2_value), cx - (len(str(co2_value))*8), cy - 16, rgb(255, 255, 255))
    
    print(f"CO2 = {co2_value} ppm. Mode: {STATE}. Intensité: {dynamic_intensity_factor:.2f}. Centre: ({ax}, {ay})") 
    
    time.sleep(0.05)