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
# Paramètres d'Animation
# -----------------------
cx, cy = 120, 120 
EXCENTRIC_RADIUS = 20 # Amplitude de l'oscillation excentrique
text_radius = 42
RING_SPACING = 10
num_rings = 12
segments = 120 
ANIM_SPEED = 0.02 
WAVE_SPEED = 0.5 # Vitesse à laquelle l'onde se propage
WAVE_LENGTH_FACTOR = 0.8 # Longueur d'onde (plus grand = plus d'anneaux visibles en même temps)
t_anim = 0 
MAX_SCREEN_COORD = 239
RING_THICKNESS_FAST = RING_SPACING + 2 
MIN_INTENSITY = 0.3 # Intensité minimale pour la couleur stable

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
def draw_ring_optimized(lcd, ax, ay, radius, lengths, alphas, color_rgb, thickness, mode, wave_alpha):
    """Dessine un anneau par segments/points, centré sur (ax, ay)."""
    
    if mode == "FAST_CIRCLE":
        thickness = RING_THICKNESS_FAST 
        
    for i in range(segments):
        
        angle = 2*math.pi*i/segments
        
        if mode == "COMPLEX_ANIM":
            # Pour COMPLEX_ANIM, on utilise le fading d'activation ET l'onde.
            alpha = alphas[i] * wave_alpha 
            l = lengths[i]
            if alpha < 0.001: continue
        else: # FAST_CIRCLE
            # Pour FAST_CIRCLE, l'opacité est gérée uniquement par l'onde (wave_alpha)
            alpha = wave_alpha 
            l = 0       
            
        # Simplification de la couleur: on module par l'alpha du segment (ou l'onde)
        col_r = int(color_rgb[0] * alpha)
        col_g = int(color_rgb[1] * alpha)
        col_b = int(color_rgb[2] * alpha)
        col565 = rgb(col_r, col_g, col_b)
        
        # Le centre utilisé ici est le centre animé (ax, ay)
        x0 = int(ax + radius * math.cos(angle))
        y0 = int(ay + radius * math.sin(angle))
        
        if mode == "FAST_CIRCLE":
            # Dessiner un point épais. 
            if 0 <= x0 < MAX_SCREEN_COORD and 0 <= y0 < MAX_SCREEN_COORD:
                lcd.fill_rect(x0 - thickness//2, y0 - thickness//2, thickness, thickness, col565)
        
        else: # COMPLEX_ANIM (Lignes animées)
            x1 = int(x0 + l * math.cos(angle))
            y1 = int(ay + l * math.sin(angle)) # Correction ay ici aussi? Non, ay est le centre.
            
            # Dessin de la ligne principale
            if 0 <= x0 <= MAX_SCREEN_COORD and 0 <= y0 <= MAX_SCREEN_COORD and \
               0 <= x1 <= MAX_SCREEN_COORD and 0 <= y1 <= MAX_SCREEN_COORD:
                lcd.line(x0, y0, x1, y1, col565)


# -----------------------
# MAIN LOOP
# -----------------------
while True:
    t_anim += ANIM_SPEED 
    
    co2_value = read_mhz19()
    if co2_value is None: co2_value = 0

    # 1. DÉFINITION DE L'ÉTAT ET DES COULEURS 
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
    
    # 3. CALCUL DE L'EXCENTRICITÉ
    
    # Excentricité : Le centre de l'animation oscille visiblement
    ax = int(cx + EXCENTRIC_RADIUS * math.sin(t_anim))
    ay = int(cy + EXCENTRIC_RADIUS * math.cos(t_anim * 0.7)) 
    
    # 4. MISE À JOUR (COMPLEX_ANIM seulement)
    if STATE == "COMPLEX_ANIM":
        for r in range(num_rings):
            target_activation = 1.0 if r < target_active_rings else 0.0
            ring_activation_factor[r] += (target_activation - ring_activation_factor[r]) * SMOOTHING_FACTOR
            current_activation = ring_activation_factor[r]
            
            if current_activation < 0.005 and target_activation == 0.0: continue

            for i in range(segments):
                target_len = (MIN_RING_LEN + 15 * current_activation) * current_activation 
                ring_lengths[r][i] += (target_len - ring_lengths[r][i]) * 0.25
                ring_alphas[r][i] = current_activation

    # 5. DESSIN EXCENTRIQUE + ONDE SORTANTE (du plus grand au plus petit)
    for r in range(num_rings - 1, -1, -1):
        
        # Le fading des anneaux est maintenant géré par l'onde temporelle
        
        # 5a. Calcul de l'onde (sinus)
        # La phase dépend du temps (t_anim * WAVE_SPEED) et du rayon (r * WAVE_LENGTH_FACTOR)
        wave_phase = t_anim * WAVE_SPEED - r * WAVE_LENGTH_FACTOR
        
        # L'opacité de l'onde va de 0.0 à 1.0, puis retombe. On utilise (sin + 1)/2 pour l'effet pulsant.
        # On peut moduler la fonction sinus pour forcer le début à 0 (l'onde part du centre)
        wave_factor = math.sin(wave_phase) 
        wave_alpha = max(0.0, wave_factor) # Seule la partie positive est visible
        
        # Pour le mode rapide, nous assurons une intensité de base pour les anneaux actifs
        if STATE == "FAST_CIRCLE" and r < target_active_rings:
             # Intensité minimum (MIN_INTENSITY) + l'onde
            wave_alpha = MIN_INTENSITY + (1.0 - MIN_INTENSITY) * wave_alpha

        # L'activation classique gère toujours le nombre d'anneaux actifs.
        current_activation = ring_activation_factor[r] if STATE == "COMPLEX_ANIM" else (1.0 if r < target_active_rings else 0.0)
        
        if current_activation < 0.005: continue
        
        # Si l'onde est presque nulle, on peut sauter le dessin pour gagner du temps
        if wave_alpha < 0.001 and STATE == "FAST_CIRCLE": continue
            
        # 5b. Dégradé Radial
        if target_active_rings > 1: t_interp = r / (target_active_rings - 1)
        else: t_interp = 0
        t_interp = max(0.0, min(1.0, t_interp)) 
        
        radial_color_rgb = interp_color(color_start_rgb, color_end_rgb, t_interp)
        final_color_rgb = radial_color_rgb 

        base_radius = text_radius + r * RING_SPACING
        
        # Dessin de l'anneau avec le facteur d'onde appliqué à l'opacité (wave_alpha)
        draw_ring_optimized(lcd, ax, ay, base_radius + RING_SPACING, ring_lengths[r], ring_alphas[r], final_color_rgb, THICKNESS, STATE, wave_alpha)


    # 6. AFFICHAGE DE LA VALEUR CO2 (SANS FOND NOIR)
    lcd.text(font, str(co2_value), cx - (len(str(co2_value))*8), cy - 16, rgb(255, 255, 255))
    
    print(f"CO2 = {co2_value} ppm. Mode: {STATE}. Onde: {wave_alpha:.2f}. Centre: ({ax}, {ay})") 
    
    time.sleep(0.05)