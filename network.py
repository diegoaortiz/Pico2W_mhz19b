import network
import time

# Informations de votre réseau
SSID = 'Ent-Cabucelle'
PASSWORD = 'Entreprise13'

# Initialisation et connexion
wlan = network.WLAN(network.STA_IF)
wlan.active(True)

if not wlan.isconnected():
    print('Connexion au réseau...')
    wlan.connect(SSID, PASSWORD)
    
    # Attendre la connexion
    max_wait = 10
    while max_wait > 0:
        if wlan.status() < 0 or wlan.status() >= 3:
            break
        max_wait -= 1
        print('.', end='')
        time.sleep(1)

if wlan.isconnected():
    # Succès
    status = wlan.ifconfig()
    print('\nConnexion réussie!')
    print('Adresse IP de la carte:', status[0])
else:
    # Échec
    print('\nÉchec de la connexion Wi-Fi.')

# La variable 'status[0]' contient l'adresse IP nécessaire pour le SSH.