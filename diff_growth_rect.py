import time
import math
import random
import gc 
from machine import Pin, SPI, UART
import gc9a01py
from fonts.romfonts import vga1_16x32 as font

# -- Initialisation (inchangée) --
spi = SPI(1, baudrate=40000000, polarity=1, phase=1)
dc = Pin(8, Pin.OUT); cs = Pin(9, Pin.OUT); rst = Pin(12, Pin.OUT); bl = Pin(13, Pin.OUT)
bl.on()
lcd = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

def read_mhz19():
    uart.write(cmd); time.sleep(0.05)
    if uart.any() >= 9:
        d = uart.read(9)
        if d and len(d) == 9 and d[0] == 0xFF and d[1] == 0x86: return d[2]*256 + d[3]
    return None

def rgb(r,g,b): return gc9a01py.color565(int(r), int(g), int(b))
TRANSITION_COLORS = {
    "VERY_GOOD": ((100, 255, 100), (0, 255, 255)), 
    "GOOD": ((0, 255, 255),  (0, 0, 200)), 
    "MEDIUM": ((0, 0, 200),  (255, 0, 255)), 
    "BAD": ((255, 0, 0),  (255, 100, 0))
}
def interp_color(c1,c2,t):
    # Interpolation de couleur pour le dégradé
    return (int(c1[0]*(1-t)+c2[0]*t), int(c1[1]*(1-t)+c2[1]*t), int(c1[2]*(1-t)+c2[2]*t))


# -- Paramètres d'Animation --
cx, cy = 120, 120
text_size = 44 # Taille initiale du premier rectangle
num_rects = 8   # Nombre de rectangles concentriques
RECT_SPACING = 10 # Espacement entre les rectangles
RECT_THICKNESS = 2 # Épaisseur de la ligne (wireframe)

# Variables d'état
ring_activation_factor = [0.0] * num_rects


# -------------------------------------------------------------------------
# Fonction de Dessin (RECTANGLE FIL DE FER ALIGNÉ X/Y)
# -------------------------------------------------------------------------
def draw_wireframe_rect(lcd, cx, cy, rect_w, rect_h, col565):
    # Dessine un rectangle aligné X/Y centré sur (cx, cy)
    rect_w = int(rect_w)
    rect_h = int(rect_h)
    
    if rect_w < 2 or rect_h < 2:
        return
        
    half_w = rect_w // 2
    half_h = rect_h // 2
    
    x0, y0 = cx - half_w, cy - half_h
    x1, y1 = cx + half_w, cy + half_h
    
    # 1. Ligne supérieure
    lcd.line(x0, y0, x1, y0, col565)
    # 2. Ligne inférieure
    lcd.line(x0, y1, x1, y1, col565)
    # 3. Ligne gauche
    lcd.line(x0, y0, x0, y1, col565)
    # 4. Ligne droite
    lcd.line(x1, y0, x1, y1, col565)


# -- Boucle Principale --
lcd.fill(gc9a01py.BLACK); last_ppm_value = None; gc.collect() 

while True:
    lcd.fill_rect(cx - 120, cy - 120, 240, 240, gc9a01py.BLACK) 
    co2_value = read_mhz19()
    if co2_value is None: co2_value = 0
    print("CO2 =", co2_value, "ppm") 
    
    # ---- Détermination des paramètres d'état (Logique CO2) ----
    if co2_value < 600: 
        color_set = TRANSITION_COLORS["VERY_GOOD"]; target_active_rects = 2
    elif co2_value < 800: 
        color_set = TRANSITION_COLORS["GOOD"]; target_active_rects = 4
    elif co2_value < 1200: 
        color_set = TRANSITION_COLORS["MEDIUM"]; target_active_rects = 6
    else: 
        color_set = TRANSITION_COLORS["BAD"]; target_active_rects = 8
        
    target_active_rects = min(target_active_rects, num_rects)
    color_start = color_set[0]; color_end = color_set[1]

    lcd.text(font, str(co2_value), cx - (len(str(co2_value))*8), cy - 16, rgb(255, 255, 255))

    # --- Structure Rectangles Simples Excentriques ---
    for r in range(num_rects):
        
        # 1. GESTION DE L'APPARITION EXCENTRIQUE (Rythme)
        target_activation = 1.0 if r < target_active_rects else 0.0
        ring_activation_factor[r] += (target_activation - ring_activation_factor[r]) * 0.15 
        current_activation = ring_activation_factor[r]
        
        if current_activation < 0.005 and target_activation == 0.0: continue
        
        # 2. CALCUL DE LA TAILLE ET DE L'EXCENTRICITÉ
        
        # Taille de base du rectangle r (doit contenir les précédents)
        base_size = text_size + r * RECT_SPACING 
        
        # Facteur de pulsation (simule l'excentricité/rythme)
        # Chaque rectangle a une pulsation différente basée sur son index 'r'
        time_factor = time.time() * 0.5 + r * 0.5
        
        # Modulation de taille (0.8 à 1.2 du carré de base) pour simuler l'excentricité
        # Modulation de la largeur (W)
        pulse_factor_w = 1 + 0.2 * math.sin(time_factor) 
        # Modulation de la hauteur (H)
        pulse_factor_h = 1 + 0.2 * math.cos(time_factor) 
        
        # Taille du rectangle final (modulée par l'activation et la pulsation)
        rect_w = base_size * pulse_factor_w * current_activation
        rect_h = base_size * pulse_factor_h * current_activation
        
        # 3. CALCUL DE LA COULEUR (Dégradé)
        if target_active_rects > 1: t_interp = r / (target_active_rects - 1)
        else: t_interp = 0
        t_interp = max(0.0, min(1.0, t_interp)) 
        current_color_rgb = interp_color(color_start, color_end, t_interp)

        # Opacité de base (Dégradé d'alpha par rectangle)
        fade_outward_factor = r / (num_rects - 1)
        alpha_base = 1.0 
        if target_active_rects > num_rects / 2: alpha_base *= (0.6 + 0.4 * fade_outward_factor) 
        else: alpha_base *= (0.6 + 0.4 * (1 - fade_outward_factor))
            
        if alpha_base < 0.15: alpha_base = 0.15
        
        # Allumage/Extinction (Alpha)
        target_alpha = alpha_base * current_activation
        
        # 4. DESSIN
        # Utilisation de l'alpha pour moduler la couleur
        current_color_alpha = tuple([c * target_alpha for c in current_color_rgb])
        col565 = rgb(*current_color_alpha)
        
        draw_wireframe_rect(lcd, cx, cy, rect_w, rect_h, col565)
    
    time.sleep(0.03)