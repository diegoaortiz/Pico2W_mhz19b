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

# COULEURS DE TRANSITION
TRANSITION_COLORS = {
    "VERY_GOOD": ((100, 255, 100), (0, 255, 255)),
    "GOOD": ((0, 255, 255),  (0, 0, 200)), 
    "MEDIUM": ((0, 0, 200),  (255, 0, 255)), 
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
num_rings = 8
segments = 120
RING_THICKNESS = 2 
RING_SPACING = 10
# MAX_LEN_FACTOR n'est plus utilisé pour la rampe
MIN_LEN = 2 
MAX_LEN_STATIC = 20 # Longueur fixe pour un cercle visible

ring_lengths = [[0 for _ in range(segments)] for _ in range(num_rings)]
ring_alphas = [[0.001 for _ in range(segments)] for _ in range(num_rings)] 
ring_activation_factor = [0.0] * num_rings

# Désactivation des précalculs de rampe

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
gc.collect() 

# -----------------------
# MAIN LOOP
# -----------------------
while True:

    # Effacement de la zone de l'animation pour la fluidité
    lcd.fill_rect(cx - 120, cy - 120, 240, 240, gc9a01py.BLACK) 
    
    # ---- Lecture CO₂ ----
    co2_value = read_mhz19()
    if co2_value is None:
        co2_value = 0

    print("CO2 =", co2_value, "ppm") 
    
    # ---- Définition des paramètres d'état ----
    if co2_value < 600: # very good
        color_set = TRANSITION_COLORS["VERY_GOOD"]
        target_active_rings = 2
    elif co2_value < 800: # good
        color_set = TRANSITION_COLORS["GOOD"]
        target_active_rings = 4
    elif co2_value < 1200: # medium
        color_set = TRANSITION_COLORS["MEDIUM"]
        target_active_rings = 6
    else: # bad
        color_set = TRANSITION_COLORS["BAD"]
        target_active_rings = 8
        
    target_active_rings = min(target_active_rings, num_rings)
    
    color_start = color_set[0]
    color_end = color_set[1]

    # ---- Affichage CO₂ sur LCD (Couleur Fixe BLANC) ----
    lcd.text(font, str(co2_value), cx - (len(str(co2_value))*8), cy - 16, rgb(255, 255, 255))
    last_ppm_value = co2_value
        

    # --- Anneaux Concentriques (Cercles) avec Allumage/Extinction Excentrique ---
    for r in range(num_rings):
        
        # 1. GESTION DE L'APPARITION EXCENTRIQUE
        
        target_activation = 1.0 if r < target_active_rings else 0.0
        
        # Augmentation/diminution du facteur d'activation
        ring_activation_factor[r] += (target_activation - ring_activation_factor[r]) * 0.15 
        
        current_activation = ring_activation_factor[r]
        
        # Si complètement inactif, ne pas dessiner
        if current_activation < 0.005 and target_activation == 0.0:
             for i in range(segments):
                 ring_alphas[r][i] = 0.001 
             continue
        
        # 2. COULEUR ET OPACITÉ LOCALE
        
        if target_active_rings > 1:
            t_interp = r / (target_active_rings - 1)
        else:
            t_interp = 0
        t_interp = max(0.0, min(1.0, t_interp)) 
        current_color = interp_color(color_start, color_end, t_interp)

        base_radius = text_radius + r * RING_SPACING
        radius = base_radius # Rayon fixe (Cercle parfait)
        
        # Opacité de base (dégradé statique)
        fade_outward_factor = r / (num_rings - 1)
        alpha_base = 1.0 
        
        if target_active_rings > num_rings / 2: 
            alpha_base *= (0.6 + 0.4 * fade_outward_factor) 
        else:
            alpha_base *= (0.6 + 0.4 * (1 - fade_outward_factor))
            
        if alpha_base < 0.15: alpha_base = 0.15
        
        # 3. MISE À JOUR ET DESSIN
        
        for i in range(segments): 
            
            # Forme = CERCLE PARFAIT (longueur statique et uniforme)
            # L'activation module la longueur statique pour le clignotement
            target_len = MAX_LEN_STATIC * current_activation 

            # DESSIN IMMÉDIAT: La longueur est stable
            ring_lengths[r][i] = target_len 
            
            # Allumage/Extinction (Alpha)
            target_alpha = alpha_base * current_activation
            ring_alphas[r][i] += (target_alpha - ring_alphas[r][i]) * 0.15 
            
            if target_alpha < 0.001:
                 ring_alphas[r][i] = 0.001
            

        draw_ring_alpha(lcd, cx, cy, radius, ring_lengths[r], ring_alphas[r], current_color, RING_THICKNESS)
    
    time.sleep(0.03)