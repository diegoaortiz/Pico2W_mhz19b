from machine import Pin, SPI, UART
import time
import gc9a01py
from fonts.romfonts import vga1_16x32 as font

# -----------------------
# UART0 MH-Z19B
# -----------------------
uart = UART(0, baudrate=9600, tx=Pin(0), rx=Pin(1))  # TX/RX sur GP0/GP1
cmd = b"\xFF\x01\x86\x00\x00\x00\x00\x00\x79"

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
display.fill(gc9a01py.color565(0, 0, 0))

# -----------------------
# Fonction lecture CO2
# -----------------------
def read_co2():
    uart.write(cmd)
    time.sleep(0.1)

    if uart.any() >= 9:
        data = uart.read(9)
        if data and len(data) == 9 and data[0] == 0xFF and data[1] == 0x86:
            return data[2] * 256 + data[3]
    return None

# -----------------------
# Boucle principale
# -----------------------
while True:
    co2 = read_co2()

    display.fill(gc9a01py.color565(0, 0, 0))  # fond noir

    if co2 is not None:
        txt = "CO2: {} ppm".format(co2)
        print("CO2 =", co2, "ppm")
    else:
        txt = "No data"
        print("⚠️ Pas de réponse du capteur")

    display.text(font, txt, 10, 100, gc9a01py.color565(255, 255, 255))
    time.sleep(1)
