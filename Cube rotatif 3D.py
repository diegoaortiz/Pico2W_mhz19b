from machine import Pin, SPI
import gc9a01py
import math
import time

# -----------------------
# SPI écran GC9A01
# -----------------------
spi = SPI(1, baudrate=20000000, polarity=1, phase=1,
          sck=Pin(10), mosi=Pin(11))
dc = Pin(8, Pin.OUT)
cs = Pin(9, Pin.OUT)
rst = Pin(12, Pin.OUT)
bl = Pin(13, Pin.OUT)
bl.on()

display = gc9a01py.GC9A01(spi, dc=dc, cs=cs, reset=rst)
display.fill(gc9a01py.BLACK)

# -----------------------
# Cube 3D
# -----------------------
size = 40
cx, cy = 120, 120  # centre écran

# Sommets du cube (x, y, z)
vertices = [
    [-1, -1, -1],
    [-1, -1,  1],
    [-1,  1, -1],
    [-1,  1,  1],
    [ 1, -1, -1],
    [ 1, -1,  1],
    [ 1,  1, -1],
    [ 1,  1,  1]
]

# Arêtes du cube (indices des sommets)
edges = [
    [0,1],[0,2],[0,4],
    [1,3],[1,5],
    [2,3],[2,6],
    [3,7],
    [4,5],[4,6],
    [5,7],
    [6,7]
]

# -----------------------
# Projection 3D → 2D
# -----------------------
def project(x, y, z, angle_x, angle_y):
    # rotation X
    y, z = y*math.cos(angle_x) - z*math.sin(angle_x), y*math.sin(angle_x) + z*math.cos(angle_x)
    # rotation Y
    x, z = x*math.cos(angle_y) + z*math.sin(angle_y), -x*math.sin(angle_y) + z*math.cos(angle_y)
    # perspective
    f = 100
    z += 4  # éviter division par 0
    px = int(cx + size * f * x / z)
    py = int(cy + size * f * y / z)
    return px, py

# -----------------------
# Boucle principale
# -----------------------
angle_x = 0
angle_y = 0

while True:
    display.fill(gc9a01py.BLACK)

    # Projeter tous les sommets
    points = [project(x, y, z, angle_x, angle_y) for x, y, z in vertices]

    # Dessiner les arêtes
    for edge in edges:
        x0, y0 = points[edge[0]]
        x1, y1 = points[edge[1]]
        display.line(x0, y0, x1, y1, gc9a01py.CYAN)

    angle_x += 0.03
    angle_y += 0.05
    time.sleep(0.05)
