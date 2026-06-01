/*
 * CO2 Display with WiFi Captive-Portal Provisioning
 * Pico 2W + MH-Z19B + GC9A01 Round LCD
 *
 * Rendering: LVGL v9 (declarative, anti-aliased, animation-engine driven)
 *   - Arduino_GFX is used only as the low-level flush driver for the GC9A01.
 *   - Breathing "pulse" rings are LVGL objects animated by lv_anim
 *     (size + opacity, ease-out, phase-offset for a wave effect).
 *   - Ring color + pulse speed are driven by the CO2 level.
 *
 * WiFi provisioning: open Access-Point + captive portal (inspired by
 *   ModuleAir_V4), credentials stored in flash (LittleFS).
 *
 * Control flow is cooperative: a single non-blocking loop drives LVGL,
 * the web server, the CO2 sensor and the LED ring together.
 */

#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// WiFi + captive portal + storage
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// ============================================================
// PIN CONFIGURATION - Pico 2W
// ============================================================

#define TFT_SCK 10
#define TFT_MOSI 11
#define TFT_CS 13
#define TFT_DC 14
#define TFT_RST 15
#define TFT_BL 12

#define LED_PIN 22
#define LED_COUNT 24
#define LED_BRIGHTNESS 90

#define MHZ_TX 0
#define MHZ_RX 1

// ============================================================
// DISPLAY (Arduino_GFX = LVGL flush backend)
// ============================================================

#define SCREEN_W 240
#define SCREEN_H 240

Arduino_DataBus *bus = new Arduino_RPiPicoSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI,
                                              -1 /* MISO */, spi1);
Arduino_GC9A01 *gfx =
    new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

Adafruit_NeoPixel ledRing(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
bool ledRingReady = false;

// LVGL draw buffer (partial render, 40 lines high)
static lv_display_t *lvDisplay = nullptr;
static uint8_t lvDrawBuf[SCREEN_W * 40 * (LV_COLOR_DEPTH / 8)];

// ============================================================
// ANIMATION CONFIGURATION
// ============================================================

#define NUM_RINGS 4
#define RING_MIN_D 64    // min diameter (px)
#define RING_MAX_D 232   // max diameter (px, kept inside the round bezel)
#define RING_BORDER 6    // ring stroke width (px)

// ============================================================
// CONFIGURATION
// ============================================================

#define WIFI_CONFIG_FILE "/wifi.cfg"

// Captive-portal network configuration (mirrors ModuleAir_V4)
#define AP_IP_OCT_1 192
#define AP_IP_OCT_2 168
#define AP_IP_OCT_3 4
#define AP_IP_OCT_4 1
#define DNS_PORT 53
#define WEB_PORT 80

// Device modes
enum DeviceMode { MODE_STARTUP, MODE_CONFIG, MODE_NORMAL };

// ============================================================
// GLOBALS
// ============================================================

uint16_t co2Value = 0;
unsigned long lastCO2Read = 0;

DeviceMode currentMode = MODE_STARTUP;

// WiFi credentials
String storedSSID = "";
String storedPassword = "";

// AP / captive portal state
String chipId = "";
String apSSID = "";
WebServer server(WEB_PORT);
DNSServer dnsServer;

// Current pulse theme (driven by CO2 level)
static uint16_t currentDurMs = 2000;
static uint8_t ledRGB[3] = {0, 200, 255};
static int currentLevel = -1; // -1 forces first theme apply

const uint8_t CO2_CMD[] = {0xFF, 0x01, 0x86, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x79};

// LVGL UI objects
static lv_obj_t *rings[NUM_RINGS];
static lv_obj_t *lblTitle;
static lv_obj_t *lblValue;
static lv_obj_t *lblSub;

// ============================================================
// CO2 -> COLOR / SPEED MAPPING
// ============================================================

void getCO2Colors(uint16_t co2, uint8_t *startRGB, uint8_t *endRGB) {
  if (co2 < 600) {
    startRGB[0] = 100; startRGB[1] = 255; startRGB[2] = 100;
    endRGB[0] = 0;     endRGB[1] = 255;   endRGB[2] = 255;
  } else if (co2 < 800) {
    startRGB[0] = 0;   startRGB[1] = 255; startRGB[2] = 255;
    endRGB[0] = 0;     endRGB[1] = 0;     endRGB[2] = 200;
  } else if (co2 < 1200) {
    startRGB[0] = 0;   startRGB[1] = 0;   startRGB[2] = 200;
    endRGB[0] = 255;   endRGB[1] = 0;     endRGB[2] = 255;
  } else {
    startRGB[0] = 255; startRGB[1] = 0;   startRGB[2] = 0;
    endRGB[0] = 255;   endRGB[1] = 100;   endRGB[2] = 0;
  }
}

int co2Level(uint16_t co2) {
  if (co2 < 600) return 0;
  if (co2 < 800) return 1;
  if (co2 < 1200) return 2;
  return 3;
}

uint16_t getRingDuration(uint16_t co2) {
  switch (co2Level(co2)) {
    case 0: return 2600; // calm
    case 1: return 2000;
    case 2: return 1500;
    default: return 1000; // urgent
  }
}

// ============================================================
// LVGL DISPLAY DRIVER
// ============================================================

static uint32_t lvMillisCb(void) { return millis(); }

static void lvFlushCb(lv_display_t *disp, const lv_area_t *area,
                      uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);
}

// ============================================================
// LVGL ANIMATION CALLBACKS
// ============================================================

static void ringSizeCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  lv_obj_set_size(o, v, v);
  lv_obj_center(o);
}

static void ringOpaCb(void *var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

// (Re)start the breathing animations with the given pulse duration.
// Starting an animation with the same var+exec_cb replaces the previous
// one, so this can be called freely whenever the speed changes.
static void startRingAnims(uint16_t durMs) {
  uint16_t step = durMs / NUM_RINGS;
  for (int i = 0; i < NUM_RINGS; i++) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rings[i]);
    lv_anim_set_exec_cb(&a, ringSizeCb);
    lv_anim_set_values(&a, RING_MIN_D, RING_MAX_D);
    lv_anim_set_duration(&a, durMs);
    lv_anim_set_delay(&a, step * i);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, rings[i]);
    lv_anim_set_exec_cb(&b, ringOpaCb);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&b, durMs);
    lv_anim_set_delay(&b, step * i);
    lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&b, lv_anim_path_linear);
    lv_anim_start(&b);
  }
}

// ============================================================
// LVGL UI
// ============================================================

void buildUI() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Concentric breathing rings (transparent fill, colored AA border)
  for (int i = 0; i < NUM_RINGS; i++) {
    lv_obj_t *r = lv_obj_create(scr);
    lv_obj_remove_style_all(r);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, RING_BORDER, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x00C8FF), 0);
    lv_obj_set_style_border_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(
        r, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(r, RING_MIN_D, RING_MIN_D);
    lv_obj_center(r);
    rings[i] = r;
  }

  // Labels (created after rings so they render on top)
  lblTitle = lv_label_create(scr);
  lv_obj_set_style_text_color(lblTitle, lv_color_white(), 0);
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_14, 0);
  lv_label_set_text(lblTitle, "");
  lv_obj_align(lblTitle, LV_ALIGN_CENTER, 0, -52);

  lblValue = lv_label_create(scr);
  lv_obj_set_style_text_color(lblValue, lv_color_white(), 0);
  lv_obj_set_style_text_font(lblValue, &lv_font_montserrat_48, 0);
  lv_label_set_text(lblValue, "");
  lv_obj_align(lblValue, LV_ALIGN_CENTER, 0, 0);

  lblSub = lv_label_create(scr);
  lv_obj_set_style_text_color(lblSub, lv_color_white(), 0);
  lv_obj_set_style_text_font(lblSub, &lv_font_montserrat_14, 0);
  lv_label_set_text(lblSub, "");
  lv_obj_align(lblSub, LV_ALIGN_CENTER, 0, 50);

  startRingAnims(currentDurMs);
}

// Apply a color gradient across the rings + set pulse speed + LED color.
void applyTheme(const uint8_t *startRGB, const uint8_t *endRGB,
                uint16_t durMs) {
  for (int i = 0; i < NUM_RINGS; i++) {
    uint8_t mix = (NUM_RINGS > 1) ? (uint8_t)(255 - (255 * i) / (NUM_RINGS - 1))
                                  : 255;
    lv_color_t cs = lv_color_make(startRGB[0], startRGB[1], startRGB[2]);
    lv_color_t ce = lv_color_make(endRGB[0], endRGB[1], endRGB[2]);
    lv_color_t c = lv_color_mix(cs, ce, mix);
    lv_obj_set_style_border_color(rings[i], c, 0);
  }
  ledRGB[0] = endRGB[0];
  ledRGB[1] = endRGB[1];
  ledRGB[2] = endRGB[2];

  if (durMs != currentDurMs) {
    currentDurMs = durMs;
    startRingAnims(durMs);
  }
}

void setStartupUI() {
  uint8_t s[3] = {0, 255, 255};
  uint8_t e[3] = {0, 0, 200};
  applyTheme(s, e, 2000);

  lv_label_set_text(lblTitle, "");
  lv_obj_set_style_text_font(lblValue, &lv_font_montserrat_48, 0);
  lv_label_set_text(lblValue, "neuma");
  lv_obj_align(lblValue, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_style_text_font(lblSub, &lv_font_montserrat_28, 0);
  lv_label_set_text(lblSub, "mini");
  lv_obj_align(lblSub, LV_ALIGN_CENTER, 0, 34);
}

void setConfigUI() {
  uint8_t s[3] = {128, 0, 255};
  uint8_t e[3] = {255, 0, 128};
  applyTheme(s, e, 2600);

  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_14, 0);
  lv_label_set_text(lblTitle, "Config WiFi");
  lv_obj_align(lblTitle, LV_ALIGN_CENTER, 0, -40);

  lv_obj_set_style_text_font(lblValue, &lv_font_montserrat_20, 0);
  lv_label_set_text(lblValue, apSSID.c_str());
  lv_obj_align(lblValue, LV_ALIGN_CENTER, 0, 0);

  lv_obj_set_style_text_font(lblSub, &lv_font_montserrat_14, 0);
  String ip = WiFi.softAPIP().toString();
  lv_label_set_text(lblSub, ip.c_str());
  lv_obj_align(lblSub, LV_ALIGN_CENTER, 0, 36);
}

void updateCO2Label() {
  lv_label_set_text(lblValue, String(co2Value).c_str());
}

void setNormalUI() {
  lv_label_set_text(lblTitle, "");
  lv_obj_set_style_text_font(lblValue, &lv_font_montserrat_48, 0);
  lv_obj_align(lblValue, LV_ALIGN_CENTER, 0, -4);
  lv_obj_set_style_text_font(lblSub, &lv_font_montserrat_14, 0);
  lv_label_set_text(lblSub, "ppm");
  lv_obj_align(lblSub, LV_ALIGN_CENTER, 0, 40);

  currentLevel = -1; // force theme apply on next refresh
  updateCO2Label();
}

// Refresh theme/speed when the CO2 level changes; always refresh the number.
void refreshNormal() {
  int lvl = co2Level(co2Value);
  if (lvl != currentLevel) {
    currentLevel = lvl;
    uint8_t s[3], e[3];
    getCO2Colors(co2Value, s, e);
    applyTheme(s, e, getRingDuration(co2Value));
  }
  updateCO2Label();
}

// ============================================================
// LED RING (breathing wave synchronized with the pulse speed)
// ============================================================

void updateLeds() {
  if (!ledRingReady)
    return;

  static unsigned long lastShow = 0;
  if (millis() - lastShow < 33) // ~30 fps
    return;
  lastShow = millis();

  float base = (float)(millis() % currentDurMs) / (float)currentDurMs;
  for (uint16_t i = 0; i < LED_COUNT; i++) {
    float p = base + (float)i / LED_COUNT;
    float breath = 0.5f + 0.5f * sinf(p * 2.0f * (float)M_PI);
    uint8_t r = (uint8_t)(ledRGB[0] * breath);
    uint8_t g = (uint8_t)(ledRGB[1] * breath);
    uint8_t b = (uint8_t)(ledRGB[2] * breath);
    ledRing.setPixelColor(i, ledRing.Color(r, g, b));
  }
  ledRing.show();
}

// ============================================================
// WIFI CREDENTIAL STORAGE (LittleFS)
// ============================================================

bool loadWifiCredentials() {
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS");
    return false;
  }
  if (!LittleFS.exists(WIFI_CONFIG_FILE)) {
    Serial.println("No WiFi config file found");
    return false;
  }
  File file = LittleFS.open(WIFI_CONFIG_FILE, "r");
  if (!file) {
    Serial.println("Failed to open WiFi config file");
    return false;
  }
  storedSSID = file.readStringUntil('\n');
  storedSSID.trim();
  storedPassword = file.readStringUntil('\n');
  storedPassword.trim();
  file.close();
  Serial.printf("Loaded WiFi: SSID=%s\n", storedSSID.c_str());
  return storedSSID.length() > 0;
}

bool saveWifiCredentials(const String &ssid, const String &password) {
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS");
    return false;
  }
  File file = LittleFS.open(WIFI_CONFIG_FILE, "w");
  if (!file) {
    Serial.println("Failed to create WiFi config file");
    return false;
  }
  file.println(ssid);
  file.println(password);
  file.close();
  Serial.printf("Saved WiFi: SSID=%s\n", ssid.c_str());
  return true;
}

bool deleteWifiConfig() {
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS");
    return false;
  }
  if (LittleFS.exists(WIFI_CONFIG_FILE)) {
    LittleFS.remove(WIFI_CONFIG_FILE);
    Serial.println("WiFi configuration deleted, restarting...");
    delay(1000);
    rp2040.reboot();
    return true;
  }
  Serial.println("No WiFi config file found");
  return false;
}

// ============================================================
// WIFI CONNECTION
// ============================================================

bool connectToWifi(const String &ssid, const String &password,
                   int timeoutMs = 10000) {
  Serial.printf("Connecting to WiFi: %s\n", ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeoutMs) {
    lv_timer_handler(); // keep the animation alive while connecting
    updateLeds();
    delay(5);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected! IP: %s\n",
                  WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi connection failed");
  WiFi.disconnect();
  return false;
}

// ============================================================
// CO2 READING
// ============================================================

uint16_t readCO2() {
  while (Serial1.available()) {
    Serial1.read();
  }

  Serial1.write(CO2_CMD, 9);

  unsigned long startWait = millis();
  while (Serial1.available() < 9 && millis() - startWait < 200) {
    delay(10);
  }

  if (Serial1.available() >= 9) {
    uint8_t response[9];
    Serial1.readBytes(response, 9);

    // Validate header + checksum (MH-Z19B: byte8 = 0xFF - sum(byte1..7) + 1)
    if (response[0] == 0xFF && response[1] == 0x86) {
      uint8_t checksum = 0;
      for (int i = 1; i < 8; i++)
        checksum += response[i];
      checksum = (uint8_t)(0xFF - checksum + 1);

      if (checksum == response[8]) {
        uint16_t co2 = (response[2] << 8) | response[3];
        if (co2 > 300 && co2 < 5000) {
          return co2;
        }
      }
    }
  }

  return co2Value > 0 ? co2Value : 0;
}

// ============================================================
// WEB SERVER (captive portal config + STA status page)
// ============================================================

static const char CONFIG_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>neuma mini - Config WiFi</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:system-ui,sans-serif;
background:#1a1a2e;color:#e0e0e0;display:flex;justify-content:center;padding:18px}
.card{width:100%;max-width:420px}
h1{margin:.2em 0;font-size:1.5em;color:#4fc3f7}
.sub{margin:.2em 0 1em;color:#9aa}
.nets{margin-bottom:14px}
.net{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;
margin:6px 0;background:#24243e;border-radius:8px;cursor:pointer;transition:.15s}
.net:hover{background:#33335a}
.rssi{color:#7a8;font-size:.85em}
input{width:100%;padding:11px;margin:6px 0;border:1px solid #44445e;border-radius:8px;
background:#16162a;color:#fff;font-size:1em}
.pw{position:relative}.pw button{position:absolute;right:6px;top:8px;width:auto;margin:0;
background:none;border:none;color:#9aa;font-size:1.2em;padding:4px}
button{width:100%;padding:12px;margin:8px 0 0;border:none;border-radius:8px;
background:#4fc3f7;color:#06121c;font-weight:600;font-size:1em;cursor:pointer}
.rescan{background:#33335a;color:#cde}
.muted{color:#778;font-size:.8em;text-align:center;margin-top:14px}
</style></head><body>
<div class="card">
<h1>neuma mini</h1>
<p class="sub">Configuration du WiFi</p>
<div id="nets" class="nets">Recherche des reseaux...</div>
<form action="/save" method="POST">
<input id="ssid" name="ssid" placeholder="Nom du reseau (SSID)" required>
<div class="pw"><input id="pw" name="password" type="password" placeholder="Mot de passe">
<button type="button" onclick="tog()">&#128065;</button></div>
<button type="submit">Connecter</button>
</form>
<button class="rescan" onclick="scan()">&#8635; Rescanner</button>
<p class="muted">Reseau ouvert temporaire pour la configuration</p>
</div>
<script>
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;')}
function scan(){document.getElementById('nets').innerHTML='Recherche...';
fetch('/scan').then(r=>r.json()).then(render).catch(()=>{document.getElementById('nets').innerHTML='Erreur de scan'})}
function render(l){var h='';if(!l.length){h='Aucun reseau trouve';}
l.sort((a,b)=>b.rssi-a.rssi);
l.forEach(function(n){h+='<div class="net" onclick="pick(this)" data-s="'+esc(n.ssid)+'">'+
'<span>'+esc(n.ssid)+(n.encrypted?' &#128274;':'')+'</span>'+
'<span class="rssi">'+n.rssi+' dBm</span></div>'});
document.getElementById('nets').innerHTML=h}
function pick(el){document.getElementById('ssid').value=el.getAttribute('data-s');
document.getElementById('pw').focus()}
function tog(){var p=document.getElementById('pw');p.type=p.type=='password'?'text':'password'}
scan();
</script></body></html>)HTML";

void handleScan() {
  Serial.println("[Web] /scan");
  int n = WiFi.scanNetworks();
  if (n < 0)
    n = 0;

  String json = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i > 0)
      json += ",";
    json += "{\"ssid\":\"";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += ssid;
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI(i));
    json += ",\"encrypted\":";
    json += (WiFi.encryptionType(i) != ENC_TYPE_NONE) ? "true" : "false";
    json += "}";
  }
  json += "]";

  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  ssid.trim();

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID manquant");
    return;
  }

  Serial.printf("[Web] /save SSID=%s (pwd len=%d)\n", ssid.c_str(),
                password.length());
  saveWifiCredentials(ssid, password);

  String page =
      "<!DOCTYPE html><html lang=\"fr\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>neuma mini</title>"
      "<style>body{font-family:system-ui,sans-serif;background:#1a1a2e;"
      "color:#e0e0e0;text-align:center;padding:40px}h1{color:#4fc3f7}</style>"
      "</head><body><h1>neuma mini</h1>"
      "<p>Connexion a <b>" + ssid + "</b>...</p>"
      "<p>L'appareil redemarre. Vous pouvez fermer cette page.</p>"
      "</body></html>";
  server.send(200, "text/html", page);

  delay(2000);
  rp2040.reboot();
}

void handleReset() {
  Serial.println("[Web] /reset");
  server.send(200, "text/html",
              "<!DOCTYPE html><html><body style='font-family:sans-serif;"
              "background:#1a1a2e;color:#eee;text-align:center;padding:40px'>"
              "<h2>WiFi oublie</h2><p>Redemarrage en mode configuration...</p>"
              "</body></html>");
  delay(1000);
  deleteWifiConfig();
}

void handleStatus() {
  String level;
  if (co2Value < 600)       level = "Excellent";
  else if (co2Value < 800)  level = "Bon";
  else if (co2Value < 1200) level = "Aerez";
  else                      level = "Ventilez !";

  String page =
      "<!DOCTYPE html><html lang=\"fr\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<meta http-equiv=\"refresh\" content=\"5\">"
      "<title>neuma mini</title>"
      "<style>body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;"
      "text-align:center;padding:24px}h1{color:#4fc3f7;margin:.2em}"
      ".co2{font-size:3.2em;font-weight:700;margin:.1em}.lvl{color:#7a8;font-size:1.1em}"
      "table{margin:18px auto;border-collapse:collapse}td{padding:5px 12px;text-align:left}"
      "td:first-child{color:#9aa}form{margin-top:20px}"
      "button{padding:11px 18px;border:none;border-radius:8px;background:#b33;color:#fff;"
      "font-size:1em;cursor:pointer}</style></head><body>"
      "<h1>neuma mini</h1>"
      "<div class=\"co2\">" + String(co2Value) + " <span style='font-size:.4em'>ppm</span></div>"
      "<div class=\"lvl\">" + level + "</div>"
      "<table>"
      "<tr><td>Reseau</td><td>" + storedSSID + "</td></tr>"
      "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>"
      "<tr><td>Signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>"
      "<tr><td>Appareil</td><td>" + apSSID + "</td></tr>"
      "</table>"
      "<form action=\"/reset\" method=\"POST\">"
      "<button type=\"submit\">Oublier le WiFi</button></form>"
      "</body></html>";
  server.send(200, "text/html", page);
}

void handleRoot() {
  if (currentMode == MODE_CONFIG) {
    server.send_P(200, "text/html", CONFIG_HTML);
  } else {
    handleStatus();
  }
}

void handleCaptiveRedirect() {
  String url = "http://" + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", url, true);
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  if (currentMode == MODE_CONFIG) {
    handleCaptiveRedirect();
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void registerRoutes() {
  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/generate_204", handleCaptiveRedirect);
  server.on("/gen_204", handleCaptiveRedirect);
  server.on("/hotspot-detect.html", handleCaptiveRedirect);
  server.on("/connecttest.txt", handleCaptiveRedirect);
  server.on("/fwlink", handleCaptiveRedirect);
  server.onNotFound(handleNotFound);
}

// ============================================================
// MODE TRANSITIONS
// ============================================================

void startConfigMode() {
  Serial.println("Entering configuration mode (WiFi AP)...");
  currentMode = MODE_CONFIG;

  WiFi.mode(WIFI_AP_STA); // AP + STA so we can also scan networks
  IPAddress apIP(AP_IP_OCT_1, AP_IP_OCT_2, AP_IP_OCT_3, AP_IP_OCT_4);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, subnet);
  WiFi.softAP(apSSID.c_str()); // open network

  Serial.printf("[AP] SSID: %s\n", apSSID.c_str());
  Serial.printf("[AP] IP:   %s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  registerRoutes();
  server.begin();
  Serial.println("[Web] HTTP server started");

  setConfigUI();
}

void startNormalMode() {
  Serial.println("Entering normal mode...");
  currentMode = MODE_NORMAL;

  registerRoutes();
  server.begin();
  Serial.printf("[Web] Status page at http://%s/\n",
                WiFi.localIP().toString().c_str());

  setNormalUI();
  lastCO2Read = millis();
}

// ============================================================
// STARTUP PHASE (sensor warmup while the boot animation plays)
// ============================================================

static unsigned long startupStart = 0;
static unsigned long startupLastRead = 0;
static unsigned long firstValidRead = 0;
static uint16_t startupPrevCO2 = 0;
static bool sensorResponding = false;

void finishStartup() {
  if (co2Value == 0) {
    Serial.println("WARNING: No valid CO2 data! Check sensor.");
    co2Value = 400;
  }
  Serial.println("Boot animation complete!");

  if (loadWifiCredentials()) {
    Serial.println("Stored credentials found, connecting...");
    if (connectToWifi(storedSSID, storedPassword, 10000)) {
      startNormalMode();
      return;
    }
    Serial.println("Stored WiFi failed, entering config mode");
  } else {
    Serial.println("No stored WiFi credentials, entering config mode");
  }
  startConfigMode();
}

void handleStartup() {
  unsigned long elapsed = millis() - startupStart;

  if (millis() - startupLastRead > 1000) {
    startupLastRead = millis();
    startupPrevCO2 = co2Value;
    uint16_t c = readCO2();
    if (c > 0) {
      co2Value = c;
      if (!sensorResponding) {
        sensorResponding = true;
        firstValidRead = millis();
        Serial.printf("First valid reading: %d ppm\n", c);
      }
      // Sensor is "ready" once the reading changes, or after a stable window
      if (startupPrevCO2 > 0 && startupPrevCO2 != c) {
        Serial.printf("Sensor ready: %d -> %d ppm\n", startupPrevCO2, c);
        finishStartup();
        return;
      }
      if (millis() - firstValidRead > 15000) {
        Serial.println("15s stable reading, proceeding...");
        finishStartup();
        return;
      }
    }
  }

  if (elapsed > 30000) {
    Serial.println("Startup timeout, proceeding...");
    finishStartup();
  }
}

// ============================================================
// SERIAL RESET HELPER
// ============================================================

void checkSerialReset() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == 'r' || cmd == 'R') {
      Serial.println("\n*** RESET WiFi Configuration ***");
      deleteWifiConfig();
    }
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial1.setTX(MHZ_TX);
  Serial1.setRX(MHZ_RX);
  Serial1.begin(9600);

  ledRing.begin();
  ledRing.setBrightness(LED_BRIGHTNESS);
  ledRing.clear();
  ledRing.show();
  ledRingReady = true;

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    while (1)
      ;
  }
  gfx->fillScreen(0x0000);

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }

  // Derive device id / AP SSID from MAC
  uint8_t mac[6];
  WiFi.macAddress(mac);
  chipId = String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
  chipId.toUpperCase();
  apSSID = "neuma-" + chipId;
  Serial.printf("Device: %s\n", apSSID.c_str());

  // --- LVGL ---
  lv_init();
  lv_tick_set_cb(lvMillisCb);
  lvDisplay = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(lvDisplay, lvFlushCb);
  lv_display_set_buffers(lvDisplay, lvDrawBuf, NULL, sizeof(lvDrawBuf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  buildUI();
  setStartupUI();

  currentMode = MODE_STARTUP;
  startupStart = millis();
}

// ============================================================
// LOOP (cooperative: LVGL + web + sensor + LEDs)
// ============================================================

void loop() {
  lv_timer_handler();
  updateLeds();

  switch (currentMode) {
  case MODE_STARTUP:
    handleStartup();
    break;

  case MODE_CONFIG:
    dnsServer.processNextRequest();
    server.handleClient();
    checkSerialReset();
    break;

  case MODE_NORMAL:
    server.handleClient();
    checkSerialReset();
    if (millis() - lastCO2Read > 3000) {
      co2Value = readCO2();
      lastCO2Read = millis();
      Serial.printf("CO2: %d ppm\n", co2Value);
      refreshNormal();
    }
    break;
  }

  delay(5);
}
