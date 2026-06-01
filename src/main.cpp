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
#include <WS2812FX.h> // pulls in Adafruit_NeoPixel as its driver
#include <SerialPIO.h>
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

// MH-Z19 on GP6 / GP7. These pins are NOT on a hardware UART, so we drive a
// PIO-emulated serial port (SerialPIO). GP6 = TX, GP7 = RX.
// Wire crossed: sensor TX -> GP7 (Pico RX), sensor RX -> GP6 (Pico TX).
#define MHZ_TX 6
#define MHZ_RX 7
SerialPIO mhzSerial(MHZ_TX, MHZ_RX);

// ============================================================
// DISPLAY (Arduino_GFX = LVGL flush backend)
// ============================================================

#define SCREEN_W 240
#define SCREEN_H 240

Arduino_DataBus *bus = new Arduino_RPiPicoSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI,
                                              -1 /* MISO */, spi1);
Arduino_GC9A01 *gfx =
    new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

// LED ring driven by WS2812FX (built-in BREATH effect = the soft "standby"
// heartbeat). Its driver is Adafruit_NeoPixel, the same path that already
// works on this board. Colour is set from the CO2 level; the breath cadence is
// fixed (slow & gentle) so the screen carries the urgency via its pulse speed.
WS2812FX ws2812fx(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
bool ledRingReady = false;

// Full-screen LVGL draw buffer (RENDER_MODE_FULL): LVGL composes the whole
// frame, then we push it in one go. This avoids the visible "section by
// section" band refresh of partial rendering. Must be aligned (LVGL asserts).
static lv_display_t *lvDisplay = nullptr;
static uint8_t lvDrawBuf[SCREEN_W * SCREEN_H * (LV_COLOR_DEPTH / 8)]
    __attribute__((aligned(64)));

// ============================================================
// ANIMATION CONFIGURATION
// ============================================================

// Single soft radial "glow" that breathes (heartbeat), instead of many
// discrete ring strokes. Animated by growing/shrinking the gradient radius.
#define GLOW_CX 120        // center (screen is 240x240)
#define GLOW_CY 120
#define GLOW_RMIN 34       // tight bright core radius (px)
#define GLOW_RMAX 168      // fully expanded glow radius (px)

// ============================================================
// CONFIGURATION
// ============================================================

#define WIFI_CONFIG_FILE "/wifi.cfg"

// --- Feature flags ---------------------------------------------------------
// SKIP_WIFI=1 bypasses the whole WiFi/captive-portal flow and jumps straight
// to the live CO2 display. Needed on a plain Pico 2 (no radio) and handy while
// working on the display/animation. Set to 0 to re-enable provisioning.
#define SKIP_WIFI 1
// CO2_DEBUG=1 prints the raw 9-byte MH-Z19 UART frames so we can tell a wiring
// problem (no bytes / wrong header) from a warm-up (out-of-range value) issue.
#define CO2_DEBUG 1

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
unsigned long lastCO2Request = 0;

DeviceMode currentMode = MODE_STARTUP;

// WiFi credentials
String storedSSID = "";
String storedPassword = "";

// AP / captive portal state
String chipId = "";
String apSSID = "";
WebServer server(WEB_PORT);
DNSServer dnsServer;
String cachedScanJson = "[]"; // networks scanned once before the AP comes up

// Current pulse theme (driven by CO2 level). currentDurMs is the breathing
// period, used by breathLevel() which both the glow and the LED ring sample.
static uint16_t currentDurMs = 6000;
static int currentLevel = -1; // -1 forces first theme apply

const uint8_t CO2_CMD[] = {0xFF, 0x01, 0x86, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x79};

// LVGL UI objects
static lv_obj_t *glow;          // full-screen object carrying the radial glow
static lv_style_t glowStyle;
static lv_grad_dsc_t glowGrad;  // referenced by glowStyle; modified in place
static lv_obj_t *lblTitle;
static lv_obj_t *lblValue;
static lv_obj_t *lblSub;

// Forward declarations
void finishStartup();
void onCO2Updated();

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
    case 0: return 7000; // calm, very slow breath
    case 1: return 6000;
    case 2: return 5000;
    default: return 4000; // worst air: a bit quicker, still gentle
  }
}

// ============================================================
// LVGL DISPLAY DRIVER
// ============================================================

static uint32_t lvMillisCb(void) { return millis(); }

static void lvLogCb(lv_log_level_t level, const char *buf) {
  (void)level;
  Serial.print(buf);
}

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

// Set the radius (in px) of the glow's outer/transparent edge by moving the
// end circle of the radial gradient, then invalidate so it re-renders.
static void setGlowRadius(int32_t r) {
  glowGrad.params.radial.end.x = GLOW_CX;
  glowGrad.params.radial.end.y = GLOW_CY;
  glowGrad.params.radial.end_extent.x = GLOW_CX + r;
  glowGrad.params.radial.end_extent.y = GLOW_CY;
  lv_obj_invalidate(glow);
}

// Tint the glow with a two-colour radial gradient: bright "start" colour at the
// core, shifting to the "end" colour as it fades out at the edge. Only the
// colours change here; the opacity ramp (cover -> mid -> transparent) is set
// once in buildUI() and is what gives the soft breathing halo.
static void setGlowColors(const uint8_t *startRGB, const uint8_t *endRGB) {
  if (glowGrad.stops_count >= 3) {
    glowGrad.stops[0].color =
        lv_color_make(startRGB[0], startRGB[1], startRGB[2]);
    glowGrad.stops[1].color = lv_color_make(endRGB[0], endRGB[1], endRGB[2]);
    glowGrad.stops[2].color = lv_color_make(endRGB[0], endRGB[1], endRGB[2]);
  }
  lv_obj_invalidate(glow);
}

// THE breathing oscillator: a single stateless function of time, sampled by
// BOTH the screen glow and the LED ring, so they pulse in sync by construction
// (no shared mutable state, no cross-engine bridge). 0 -> 1 -> 0 (soft cosine)
// over currentDurMs. Changing currentDurMs just changes the breathing rate.
float breathLevel() {
  float t = (float)(millis() % currentDurMs) / (float)currentDurMs;
  return 0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI);
}

// LVGL timer (native scheduler): sample the oscillator and resize the glow.
static void glowBreathCb(lv_timer_t *timer) {
  (void)timer;
  setGlowRadius(GLOW_RMIN + (int32_t)(breathLevel() * (GLOW_RMAX - GLOW_RMIN)));
}

// ============================================================
// LVGL UI
// ============================================================

void buildUI() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // One full-screen object filled with a radial gradient: bright tinted core
  // fading to transparent at the edge. Animating the gradient radius makes it
  // breathe as a single soft mass (no discrete ring strokes).
  static const lv_color_t gcol[3] = {
      LV_COLOR_MAKE(0, 200, 255), LV_COLOR_MAKE(0, 200, 255),
      LV_COLOR_MAKE(0, 200, 255)};
  static const lv_opa_t gopa[3] = {LV_OPA_COVER, 150, LV_OPA_TRANSP};
  lv_grad_init_stops(&glowGrad, gcol, gopa, NULL, 3);
  lv_grad_radial_init(&glowGrad, GLOW_CX, GLOW_CY, GLOW_CX + GLOW_RMIN, GLOW_CY,
                      LV_GRAD_EXTEND_PAD);
  lv_grad_radial_set_focal(&glowGrad, GLOW_CX, GLOW_CY, 0);

  lv_style_init(&glowStyle);
  lv_style_set_bg_grad(&glowStyle, &glowGrad);
  lv_style_set_bg_opa(&glowStyle, LV_OPA_COVER);
  lv_style_set_border_width(&glowStyle, 0);
  lv_style_set_radius(&glowStyle, 0);
  lv_style_set_pad_all(&glowStyle, 0);

  glow = lv_obj_create(scr);
  lv_obj_remove_style_all(glow);
  lv_obj_add_style(glow, &glowStyle, 0);
  lv_obj_set_size(glow, SCREEN_W, SCREEN_H);
  lv_obj_center(glow);
  lv_obj_remove_flag(
      glow, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

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

  // Drive the glow breathing from the shared oscillator via LVGL's scheduler.
  lv_timer_create(glowBreathCb, 16, NULL); // ~60 fps
}

// Apply a CO2 theme: two-colour glow gradient on screen + LED breath colour +
// breathing rate. start/end are the CO2 indicator colours.
void applyTheme(const uint8_t *startRGB, const uint8_t *endRGB,
                uint16_t durMs) {
  setGlowColors(startRGB, endRGB);
  // LED ring breathes from off to the dominant CO2 colour.
  ws2812fx.setColor(ws2812fx.Color(startRGB[0], startRGB[1], startRGB[2]));
  currentDurMs = durMs; // breathLevel() picks up the new rate immediately
}

void setStartupUI() {
  uint8_t s[3] = {0, 255, 255};
  uint8_t e[3] = {0, 0, 200};
  applyTheme(s, e, 4000);

  lv_label_set_text(lblTitle, "");
  lv_obj_set_style_text_font(lblValue, &lv_font_montserrat_48, 0);
  lv_label_set_text(lblValue, "alba");
  lv_obj_align(lblValue, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(lblSub, "");
}

void setConfigUI() {
  uint8_t s[3] = {128, 0, 255};
  uint8_t e[3] = {255, 0, 128};
  applyTheme(s, e, 4500);

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
// LED RING (custom WS2812FX mode: breath locked to the screen glow)
// ============================================================

// Custom WS2812FX effect: fill the whole ring with the segment colour scaled by
// breathLevel() -- the exact same oscillator the screen glow samples -- so the
// ring and the glow pulse in perfect lock-step. A small floor keeps the ring
// softly lit at the trough (gentle "leger" look) instead of fully off.
uint16_t breathSyncMode(void) {
  WS2812FX::Segment *seg = ws2812fx.getSegment();
  float level = 0.12f + 0.88f * breathLevel(); // 0.12 .. 1.0
  uint32_t c = seg->colors[0];
  uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * level);
  uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * level);
  uint8_t b = (uint8_t)((c & 0xFF) * level);
  uint32_t scaled = ws2812fx.Color(r, g, b);
  for (uint16_t i = seg->start; i <= seg->stop; i++)
    ws2812fx.setPixelColor(i, scaled);
  return 16; // re-render ~60 fps; brightness tracks the glow continuously
}

// Drive the WS2812FX engine (which runs breathSyncMode above).
void updateLeds() {
  if (ledRingReady)
    ws2812fx.service();
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

// Non-blocking MH-Z19B reader. requestCO2() fires a measurement command;
// pollCO2() (called every loop) collects the 9-byte reply across iterations
// and updates co2Value + calls onCO2Updated() when a valid frame arrives.
// This keeps the animation from stalling on the UART (the old blocking read
// froze the screen ~200 ms every read).
static bool co2Pending = false;
static unsigned long co2RequestTime = 0;

void requestCO2() {
  while (mhzSerial.available())
    mhzSerial.read(); // flush stale bytes
  mhzSerial.write(CO2_CMD, 9);
  co2Pending = true;
  co2RequestTime = millis();
}

void pollCO2() {
  if (!co2Pending)
    return;

  if (mhzSerial.available() >= 9) {
    uint8_t r[9];
    mhzSerial.readBytes(r, 9); // returns immediately, bytes already buffered
    co2Pending = false;

#if CO2_DEBUG
    Serial.printf("[CO2] RX: %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
#endif

    // Validate header + checksum (MH-Z19B: byte8 = 0xFF - sum(byte1..7) + 1)
    if (r[0] == 0xFF && r[1] == 0x86) {
      uint8_t cs = 0;
      for (int i = 1; i < 8; i++)
        cs += r[i];
      cs = (uint8_t)(0xFF - cs + 1);
      if (cs == r[8]) {
        uint16_t c = (r[2] << 8) | r[3];
        if (c > 300 && c < 5000) {
          co2Value = c;
          onCO2Updated();
        }
#if CO2_DEBUG
        else
          Serial.printf("[CO2] value %u out of range (warming up?)\n", c);
#endif
      }
#if CO2_DEBUG
      else
        Serial.println("[CO2] checksum mismatch");
#endif
    }
#if CO2_DEBUG
    else
      Serial.println("[CO2] bad header -> check TX/RX wiring (crossed?) & 9600 baud");
#endif
  } else if (millis() - co2RequestTime > 300) {
#if CO2_DEBUG
    Serial.printf("[CO2] no reply (%d bytes) -> check wiring/power/5V\n",
                  mhzSerial.available());
#endif
    co2Pending = false; // no/short reply, drop and retry next cycle
  }
}

// ============================================================
// WEB SERVER (captive portal config + STA status page)
// ============================================================

static const char CONFIG_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>alba - Config WiFi</title>
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
<h1>alba</h1>
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

// Scan networks (must be in STA mode) and return them as a JSON array.
String scanToJson() {
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
  return json;
}

// The page fetches /scan: serve the list captured before the AP started
// (a live scan is unreliable while the cyw43 is in AP mode).
void handleScan() {
  server.send(200, "application/json", cachedScanJson);
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
      "<title>alba</title>"
      "<style>body{font-family:system-ui,sans-serif;background:#1a1a2e;"
      "color:#e0e0e0;text-align:center;padding:40px}h1{color:#4fc3f7}</style>"
      "</head><body><h1>alba</h1>"
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
      "<title>alba</title>"
      "<style>body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;"
      "text-align:center;padding:24px}h1{color:#4fc3f7;margin:.2em}"
      ".co2{font-size:3.2em;font-weight:700;margin:.1em}.lvl{color:#7a8;font-size:1.1em}"
      "table{margin:18px auto;border-collapse:collapse}td{padding:5px 12px;text-align:left}"
      "td:first-child{color:#9aa}form{margin-top:20px}"
      "button{padding:11px 18px;border:none;border-radius:8px;background:#b33;color:#fff;"
      "font-size:1em;cursor:pointer}</style></head><body>"
      "<h1>alba</h1>"
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

  // 1) Scan available networks in STA mode first, cache the list for the page.
  Serial.println("[WiFi] Scanning networks before AP...");
  WiFi.mode(WIFI_STA);
  cachedScanJson = scanToJson();
  Serial.printf("[WiFi] Scan done (%d bytes)\n", cachedScanJson.length());

  // 2) Bring up an open Access-Point only (reliable on the cyw43; AP+STA is
  //    flaky and can leave the SSID invisible).
  IPAddress apIP(AP_IP_OCT_1, AP_IP_OCT_2, AP_IP_OCT_3, AP_IP_OCT_4);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, subnet);
  bool apOk = WiFi.softAP(apSSID.c_str()); // open network (no password)

  Serial.printf("[AP] softAP(%s) -> %s\n", apSSID.c_str(),
                apOk ? "OK" : "FAILED");
  Serial.printf("[AP] IP: %s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  registerRoutes();
  server.begin();
  Serial.println("[Web] HTTP server started");

  setConfigUI();
}

void startNormalMode() {
  Serial.println("Entering normal mode...");
  currentMode = MODE_NORMAL;

#if !SKIP_WIFI
  registerRoutes();
  server.begin();
  Serial.printf("[Web] Status page at http://%s/\n",
                WiFi.localIP().toString().c_str());
#endif

  setNormalUI();
  lastCO2Request = millis();
}

// ============================================================
// STARTUP PHASE (sensor warmup while the boot animation plays)
// ============================================================

static unsigned long startupStart = 0;
static bool startupSensorSeen = false;
static unsigned long startupFirstSeen = 0;
static uint16_t startupPrevCO2 = 0;

void finishStartup() {
  if (co2Value == 0) {
    Serial.println("WARNING: No valid CO2 data! Check sensor.");
    co2Value = 400;
  }
  Serial.println("Boot animation complete!");

#if SKIP_WIFI
  Serial.println("WiFi disabled (SKIP_WIFI) -> live CO2 display");
  startNormalMode();
  return;
#else
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
#endif
}

// Called by pollCO2() whenever a fresh, valid reading lands.
void onCO2Updated() {
  if (currentMode == MODE_STARTUP) {
    if (!startupSensorSeen) {
      startupSensorSeen = true;
      startupFirstSeen = millis();
      startupPrevCO2 = co2Value;
      Serial.printf("First valid reading: %d ppm\n", co2Value);
      return;
    }
    if (co2Value != startupPrevCO2) {
      Serial.printf("Sensor ready: %d -> %d ppm\n", startupPrevCO2, co2Value);
      finishStartup();
      return;
    }
    if (millis() - startupFirstSeen > 15000) {
      Serial.println("15s stable reading, proceeding...");
      finishStartup();
      return;
    }
    startupPrevCO2 = co2Value;
  } else if (currentMode == MODE_NORMAL) {
    Serial.printf("CO2: %d ppm\n", co2Value);
    refreshNormal();
  }
}

// Only responsibility left here: the overall startup timeout.
void handleStartup() {
  if (millis() - startupStart > 30000) {
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
  // Give the USB-CDC link time to enumerate so the early boot logs aren't lost.
  // The PlatformIO monitor does not reset the Pico, so without this wait the
  // first prints often disappear. Bounded so the board still boots headless.
  unsigned long serialT0 = millis();
  while (!Serial && millis() - serialT0 < 3000)
    delay(10);
  delay(100);

  mhzSerial.begin(9600); // PIO-emulated UART on GP6/GP7 (pins set in ctor)

  ws2812fx.init();
  ws2812fx.setBrightness(LED_BRIGHTNESS);
  // Register our screen-synced breath as custom mode 0 and run it on the whole
  // ring. Colour is updated per CO2 level via ws2812fx.setColor() in applyTheme.
  ws2812fx.setCustomMode(breathSyncMode);
  ws2812fx.setSegment(0, 0, LED_COUNT - 1, FX_MODE_CUSTOM_0,
                      ws2812fx.Color(0, 200, 255), 1000, false);
  ws2812fx.start();
  ledRingReady = true;

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  if (!gfx->begin(62500000)) { // higher SPI clock -> faster flush, smoother
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

  // Derive device id / AP SSID from the board's unique chip id (a hex string,
  // valid this early in setup unlike WiFi.macAddress() which needs the radio).
  String fullId = String(rp2040.getChipID());
  chipId = fullId.substring(fullId.length() >= 6 ? fullId.length() - 6 : 0);
  chipId.toUpperCase();
  apSSID = "alba-" + chipId;
  Serial.printf("Device: %s\n", apSSID.c_str());

  // --- LVGL ---
  lv_init();
  lv_log_register_print_cb(lvLogCb);
  lv_tick_set_cb(lvMillisCb);
  lvDisplay = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(lvDisplay, lvFlushCb);
  lv_display_set_buffers(lvDisplay, lvDrawBuf, NULL, sizeof(lvDrawBuf),
                         LV_DISPLAY_RENDER_MODE_FULL);

  buildUI();
  setStartupUI();

  currentMode = MODE_STARTUP;
  startupStart = millis();
  requestCO2(); // kick off the first (non-blocking) sensor reading
  lastCO2Request = millis();
  Serial.println("Setup complete, entering loop");
}

// ============================================================
// LOOP (cooperative: LVGL + web + sensor + LEDs)
// ============================================================

void loop() {
  lv_timer_handler();
  updateLeds();
  pollCO2();

  // Fire a new (non-blocking) sensor request periodically.
  unsigned long reqInterval = (currentMode == MODE_STARTUP) ? 1500 : 3000;
  if (!co2Pending && millis() - lastCO2Request > reqInterval) {
    requestCO2();
    lastCO2Request = millis();
  }

  switch (currentMode) {
  case MODE_STARTUP:
    handleStartup();
    break;

  case MODE_CONFIG:
#if !SKIP_WIFI
    dnsServer.processNextRequest();
    server.handleClient();
    checkSerialReset();
#endif
    break;

  case MODE_NORMAL:
#if !SKIP_WIFI
    server.handleClient();
    checkSerialReset();
#endif
    break;
  }

  delay(5);
}
