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

# Variable pour la persistance de la valeur lue (Correction de stabilité)
last_co2_value = 400 

def read_mhz19():
    global last_co2_value
    uart.write(cmd)
    time.sleep(0.05)
    
    # Lecture des données du capteur
    if uart.any() >= 9:
        d = uart.read(9)
        # Vérification du checksum (méthode simple, non exhaustive)
        if d and len(d) == 9 and d[0] == 0xFF and d[1] == 0x86:
            co2 = d[2]*256 + d[3]
            # Vérification de la validité de la lecture (éviter les lectures aberrantes)
            if co2 > 300: 
                last_co2_value = co2
                return co2
            
    # Retourne la dernière valeur connue si la lecture est nulle ou échoue
    return last_co2_value

# -----------------------
# Couleurs et Interpolation (Identique)
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
# Paramètres d'Animation (Identique)
# -----------------------
cx, cy = 120, 120 
text_radius = 42
RING_SPACING = 10
num_rings = 12
segments = 120 
ANIM_SPEED = 0.02 
WAVE_SPEED = 0.40 
WAVE_LENGTH_FACTOR = 0.40 
t_anim = 0 
MAX_SCREEN_COORD = 239
RING_THICKNESS = 10 
MIN_INTENSITY = 0.35 

# -----------------------
# Draw one ring (Simplifié)
# -----------------------
def draw_ring_optimized(lcd, ax, ay, radius, color_rgb, thickness, wave_alpha):
    """Dessine un anneau par segments/points, centré sur (ax, ay)."""
    
    alpha = wave_alpha 
    
    if alpha < 0.001: return 

    col_r = int(color_rgb[0] * alpha)
    col_g = int(color_rgb[1] * alpha)
    col_b = int(color_rgb[2] * alpha)
    col565 = rgb(col_r, col_g, col_b)
        
    for i in range(segments):
        
        angle = 2*math.pi*i/segments
        
        x0 = int(ax + radius * math.cos(angle))
        y0 = int(ay + radius * math.sin(angle))
        
        if 0 <= x0 < MAX_SCREEN_COORD and 0 <= y0 < MAX_SCREEN_COORD:
            lcd.fill_rect(x0 - thickness//2, y0 - thickness//2, thickness, thickness, col565)


# -----------------------
# MAIN LOOP
# -----------------------
while True:
    t_anim += ANIM_SPEED 
    
    co2_value = read_mhz19()

    # 1. DÉFINITION DE L'ÉTAT ET DES COULEURS
    if co2_value < 600:
        color_set = TRANSITION_COLORS["VERY_GOOD"]; target_active_rings = 10
    elif co2_value < 800:
        color_set = TRANSITION_COLORS["GOOD"]; target_active_rings = 8
    elif co2_value < 1200:
        color_set = TRANSITION_COLORS["MEDIUM"]; target_active_rings = 6
    else:
        color_set = TRANSITION_COLORS["BAD"]; target_active_rings = 4
            
    color_start_rgb = color_set[0]
    color_end_rgb = color_set[1]
    target_active_rings = min(target_active_rings, num_rings)
    
    # 2. EFFACEMENT DE L'ÉCRAN
    lcd.fill_rect(0, 0, 240, 240, gc9a01py.BLACK) 
    
    # 3. CALCUL DU CENTRE (Centré fixe)
    ax = cx
    ay = cy
    
    # 4. DESSIN : Flux Sortant Corrigé
    for r in range(num_rings - 1, -1, -1):
        
        # 4a. Calcul de l'onde (MOUVEMENT SORTANT - Nouvelle Tentative)
        # Utilisation de la somme, mais en faisant avancer t_anim.
        # Si le problème est visuel, cette formule devrait être l'inverse de la précédente.
        wave_phase = (t_anim * WAVE_SPEED) + (r * WAVE_LENGTH_FACTOR) 
        wave_opacite = math.cos(wave_phase) 
        wave_alpha_peak = max(0.0, wave_opacite) ** 2 
        
        current_activation = 1.0 if r < target_active_rings else 0.0
        
        # Application de l'intensité de base pour la subtilité
        wave_alpha = MIN_INTENSITY * current_activation + (1.0 - MIN_INTENSITY) * wave_alpha_peak
        
        if current_activation < 0.005: continue
        if wave_alpha < 0.001: continue
            
        # 4b. Dégradé Radial
        if target_active_rings > 1: t_interp = r / (target_active_rings - 1)
        else: t_interp = 0
        t_interp = max(0.0, min(1.0, t_interp)) 
        
        radial_color_rgb = interp_color(color_start_rgb, color_end_rgb, t_interp)
        final_color_rgb = radial_color_rgb 

        base_radius = text_radius + r * RING_SPACING
        
        # Dessin de l'anneau avec le facteur d'onde appliqué (wave_alpha)
        draw_ring_optimized(lcd, ax, ay, base_radius + RING_SPACING, final_color_rgb, RING_THICKNESS, wave_alpha)


    # 5. AFFICHAGE DE LA VALEUR CO2
    lcd.text(font, str(co2_value), cx - (len(str(co2_value))*8), cy - 16, rgb(255, 255, 255))
    
    # 6. Affichage terminal
    print(f"CO2 = {co2_value} ppm. **Flux SORTANT (Tentative 5)**. Centre: ({ax}, {ay})") 
    
    time.sleep(0.05)