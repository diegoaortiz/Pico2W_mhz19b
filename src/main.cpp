/*
 * CO2 Display with BLE WiFi Provisioning
 * Pico 2W + MH-Z19B + GC9A01 Round LCD
 *
 * Features:
 * - Smooth breathing animation for CO2 display
 * - BLE-based WiFi provisioning
 * - Stores WiFi credentials in flash
 * - Broadcasts CO2 data via BLE every minute
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

// WiFi and BLE
#include <LittleFS.h>
#include <WiFi.h>

// BTstack for BLE on Pico W
#include <BTstackLib.h>
#include <SPI.h>

// ============================================================
// PIN CONFIGURATION - Pico 2W
// ============================================================

#define TFT_SCK 10
#define TFT_MOSI 11
#define TFT_CS 9
#define TFT_DC 8
#define TFT_RST 12
#define TFT_BL 13

#define MHZ_TX 0
#define MHZ_RX 1

// ============================================================
// DISPLAY SETUP
// ============================================================

Arduino_DataBus *bus = new Arduino_RPiPicoSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI,
                                              -1 /* MISO */, spi1);
Arduino_GC9A01 *gfx =
    new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

Arduino_Canvas *canvas = nullptr;

// ============================================================
// CONFIGURATION
// ============================================================

#define SCREEN_W 240
#define SCREEN_H 240
#define CENTER_X 120
#define CENTER_Y 120

// Animation parameters
#define NUM_RINGS 12
#define TEXT_RADIUS 42
#define RING_THICKNESS 8
#define MIN_INTENSITY 0.35f
#define WAVE_SPEED 0.8f
#define PHASE_OFFSET 0.18f
#define ANIM_SPEED 0.03f

// Colors
#define BLACK 0x0000
#define WHITE 0xFFFF

// WiFi config file
#define WIFI_CONFIG_FILE "/wifi.cfg"

// BLE UUIDs (same as aircarto-app expects)
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_DEVICE_INFO_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_WIFI_NETWORKS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define CHAR_WIFI_CONFIG_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define CHAR_STATUS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define CHAR_CO2_DATA_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"

// Provisioning Status codes (renamed to avoid BTstackLib conflict)
enum ProvisionStatus {
  PROV_IDLE = 0,
  PROV_CONNECTING = 1,
  PROV_CONNECTED = 2,
  PROV_FAILED = 3,
  PROV_WRONG_PASSWORD = 4
};

// Device modes
enum DeviceMode { MODE_STARTUP, MODE_CONFIG, MODE_NORMAL };

// ============================================================
// GLOBALS
// ============================================================

uint16_t co2Value = 0;
float tAnim = 0.0f;
unsigned long lastCO2Read = 0;
unsigned long lastBLEBroadcast = 0;
bool startupComplete = false;

DeviceMode currentMode = MODE_STARTUP;
ProvisionStatus provStatus = PROV_IDLE;

// WiFi credentials
String storedSSID = "";
String storedPassword = "";

// BLE state
bool bleConnected = false;
String wifiNetworksJson = "";
bool wifiScanRequested = false;
bool wifiConfigReceived = false;
String pendingSSID = "";
String pendingPassword = "";

// Chip ID for device name
String chipId = "";

const uint8_t CO2_CMD[] = {0xFF, 0x01, 0x86, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x79};

// ============================================================
// COLOR HELPERS
// ============================================================

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void interpColor(const uint8_t *c1, const uint8_t *c2, float t,
                 uint8_t *result) {
  result[0] = (uint8_t)(c1[0] * (1.0f - t) + c2[0] * t);
  result[1] = (uint8_t)(c1[1] * (1.0f - t) + c2[1] * t);
  result[2] = (uint8_t)(c1[2] * (1.0f - t) + c2[2] * t);
}

void getCO2Colors(uint16_t co2, uint8_t *startRGB, uint8_t *endRGB) {
  if (co2 < 600) {
    startRGB[0] = 100;
    startRGB[1] = 255;
    startRGB[2] = 100;
    endRGB[0] = 0;
    endRGB[1] = 255;
    endRGB[2] = 255;
  } else if (co2 < 800) {
    startRGB[0] = 0;
    startRGB[1] = 255;
    startRGB[2] = 255;
    endRGB[0] = 0;
    endRGB[1] = 0;
    endRGB[2] = 200;
  } else if (co2 < 1200) {
    startRGB[0] = 0;
    startRGB[1] = 0;
    startRGB[2] = 200;
    endRGB[0] = 255;
    endRGB[1] = 0;
    endRGB[2] = 255;
  } else {
    startRGB[0] = 255;
    startRGB[1] = 0;
    startRGB[2] = 0;
    endRGB[0] = 255;
    endRGB[1] = 100;
    endRGB[2] = 0;
  }
}

float getBreathSpeed(uint16_t co2) {
  if (co2 < 600)
    return 0.5f;
  if (co2 < 800)
    return 0.9f;
  if (co2 < 1200)
    return 1.4f;
  return 2.2f;
}

// ============================================================
// WIFI CREDENTIAL STORAGE
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

// ============================================================
// WIFI CONNECTION
// ============================================================

bool connectToWifi(const String &ssid, const String &password,
                   int timeoutMs = 10000) {
  Serial.printf("Connecting to WiFi: %s\n", ssid.c_str());

  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected! IP: %s\n",
                  WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.println("WiFi connection failed");
    WiFi.disconnect();
    return false;
  }
}

String scanWifiNetworks() {
  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("Found %d networks\n", n);

  String json = "[";
  for (int i = 0; i < n && i < 15; i++) { // Limit to 15 networks
    if (i > 0)
      json += ",";
    json += "{\"s\":\"";
    json += WiFi.SSID(i);
    json += "\",\"r\":";
    json += String(WiFi.RSSI(i));
    json += ",\"e\":";
    json += (WiFi.encryptionType(i) != ENC_TYPE_NONE) ? "1" : "0";
    json += "}";
  }
  json += "]";

  WiFi.scanDelete();
  return json;
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

    if (response[0] == 0xFF && response[1] == 0x86) {
      uint16_t co2 = (response[2] << 8) | response[3];
      if (co2 > 300 && co2 < 5000) {
        return co2;
      }
    }
  }

  return co2Value > 0 ? co2Value : 0;
}

// ============================================================
// DRAW RING
// ============================================================

void drawRing(Arduino_GFX *target, int16_t cx, int16_t cy, int16_t radius,
              const uint8_t *colorRGB, float alpha) {
  if (alpha < 0.001f || radius <= 0)
    return;

  uint8_t r = (uint8_t)(colorRGB[0] * alpha);
  uint8_t g = (uint8_t)(colorRGB[1] * alpha);
  uint8_t b = (uint8_t)(colorRGB[2] * alpha);
  uint16_t color = rgb565(r, g, b);

  int thickness = RING_THICKNESS;
  int outerR = radius + thickness / 2;
  int innerR = radius - thickness / 2;
  if (innerR < 0)
    innerR = 0;

  for (int y = -outerR; y <= outerR; y++) {
    int outerX = (int)sqrtf((float)(outerR * outerR - y * y));
    int innerX = (y >= -innerR && y <= innerR)
                     ? (int)sqrtf((float)(innerR * innerR - y * y))
                     : 0;

    if (cx - outerX >= 0 && cx - innerX <= SCREEN_W) {
      target->drawFastHLine(cx - outerX, cy + y, outerX - innerX, color);
    }
    if (cx + innerX >= 0 && cx + outerX <= SCREEN_W) {
      target->drawFastHLine(cx + innerX, cy + y, outerX - innerX, color);
    }
  }
}

// ============================================================
// DRAW FRAMES
// ============================================================

void drawBreathingRings(Arduino_GFX *target, float anim, uint8_t *startRGB,
                        uint8_t *endRGB, float breathSpeed) {
  for (int ring = NUM_RINGS - 1; ring >= 0; ring--) {
    float wavePhase = (anim * WAVE_SPEED) - (ring * 0.40f);
    float waveOpacity = cosf(wavePhase);
    float waveAlphaPeak = waveOpacity > 0 ? waveOpacity * waveOpacity : 0;
    float waveAlpha = MIN_INTENSITY + (1.0f - MIN_INTENSITY) * waveAlphaPeak;

    if (waveAlpha < 0.001f)
      continue;

    float tInterp = (float)ring / (NUM_RINGS - 1);
    uint8_t radialColor[3];
    interpColor(startRGB, endRGB, tInterp, radialColor);

    int startRadius = TEXT_RADIUS + ring * 2;
    int endRadius = 118 - ring * 2;
    float phase = anim * breathSpeed - ring * PHASE_OFFSET;
    float wave = 0.5f + 0.5f * sinf(phase);
    int animatedRadius = startRadius + (int)(wave * (endRadius - startRadius));

    drawRing(target, CENTER_X, CENTER_Y, animatedRadius, radialColor,
             waveAlpha);
  }
}

void drawStartupFrame(float anim) {
  Arduino_GFX *target =
      (canvas != nullptr) ? (Arduino_GFX *)canvas : (Arduino_GFX *)gfx;

  target->fillScreen(BLACK);

  uint8_t startRGB[3] = {0, 255, 255};
  uint8_t endRGB[3] = {0, 0, 200};
  float breathSpeed = 0.9f;

  drawBreathingRings(target, anim, startRGB, endRGB, breathSpeed);

  // Draw "neuma" text centered
  target->setTextColor(WHITE);
  target->setTextSize(3);
  int16_t x1, y1;
  uint16_t w, h;
  target->getTextBounds("neuma", 0, 0, &x1, &y1, &w, &h);
  target->setCursor(CENTER_X - w / 2, CENTER_Y - h / 2 - 8);
  target->print("neuma");

  // Draw "mini" below
  target->setTextSize(2);
  target->getTextBounds("mini", 0, 0, &x1, &y1, &w, &h);
  target->setCursor(CENTER_X - w / 2, CENTER_Y + 12);
  target->print("mini");

  if (canvas != nullptr) {
    canvas->flush();
  }
}

void drawConfigFrame(float anim) {
  Arduino_GFX *target =
      (canvas != nullptr) ? (Arduino_GFX *)canvas : (Arduino_GFX *)gfx;

  target->fillScreen(BLACK);

  // Use purple/pink colors for config mode
  uint8_t startRGB[3] = {128, 0, 255};
  uint8_t endRGB[3] = {255, 0, 128};
  float breathSpeed = 0.6f;

  drawBreathingRings(target, anim, startRGB, endRGB, breathSpeed);

  // Draw "neuma" text centered
  target->setTextColor(WHITE);
  target->setTextSize(2);
  int16_t x1, y1;
  uint16_t w, h;
  target->getTextBounds("neuma mini", 0, 0, &x1, &y1, &w, &h);
  target->setCursor(CENTER_X - w / 2, CENTER_Y - 30);
  target->print("neuma mini");

  // Draw config message
  target->setTextSize(1);
  target->getTextBounds("aircarto app", 0, 0, &x1, &y1, &w, &h);
  target->setCursor(CENTER_X - w / 2, CENTER_Y + 5);
  target->print("aircarto app");

  target->getTextBounds("> Configurer", 0, 0, &x1, &y1, &w, &h);
  target->setCursor(CENTER_X - w / 2, CENTER_Y + 20);
  target->print("> Configurer");

  if (canvas != nullptr) {
    canvas->flush();
  }
}

void drawNormalFrame() {
  Arduino_GFX *target =
      (canvas != nullptr) ? (Arduino_GFX *)canvas : (Arduino_GFX *)gfx;

  target->fillScreen(BLACK);

  uint8_t startRGB[3], endRGB[3];
  getCO2Colors(co2Value, startRGB, endRGB);
  float breathSpeed = getBreathSpeed(co2Value);

  drawBreathingRings(target, tAnim, startRGB, endRGB, breathSpeed);

  // Draw CO2 text centered
  target->setTextColor(WHITE);
  target->setTextSize(3);

  String co2Text = String(co2Value);
  int16_t x1, y1;
  uint16_t w, h;
  target->getTextBounds(co2Text, 0, 0, &x1, &y1, &w, &h);
  target->setCursor(CENTER_X - w / 2, CENTER_Y - h / 2);
  target->print(co2Text);

  if (canvas != nullptr) {
    canvas->flush();
  }
}

// ============================================================
// BLE SERVICE (Using BTstack)
// ============================================================

// Value handles for our characteristics (assigned by BTstack)
static uint16_t deviceInfoHandle = 0;
static uint16_t wifiNetworksHandle = 0;
static uint16_t wifiConfigHandle = 0;
static uint16_t statusHandle = 0;
static uint16_t co2DataHandle = 0;

// BLE UUIDs as UUID objects
static UUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static UUID deviceInfoUUID("beb5483e-36e1-4688-b7f5-ea07361b26a8");
static UUID wifiNetworksUUID("beb5483e-36e1-4688-b7f5-ea07361b26a9");
static UUID wifiConfigUUID("beb5483e-36e1-4688-b7f5-ea07361b26aa");
static UUID statusUUID("beb5483e-36e1-4688-b7f5-ea07361b26ab");
static UUID co2DataUUID("beb5483e-36e1-4688-b7f5-ea07361b26ac");

// Device info JSON
static String deviceInfoJson;

// BLE Callbacks
void bleDeviceConnectedCallback(BLEStatus status, BLEDevice *device) {
  (void)device;
  if (status == BLE_STATUS_OK) {
    Serial.println("[BLE] Device connected!");
    bleConnected = true;

    // Trigger WiFi scan when connected
    wifiScanRequested = true;
  }
}

void bleDeviceDisconnectedCallback(BLEDevice *device) {
  (void)device;
  Serial.println("[BLE] Device disconnected");
  bleConnected = false;
}

uint16_t bleGattReadCallback(uint16_t value_handle, uint8_t *buffer,
                             uint16_t buffer_size) {
  Serial.printf("[BLE] Read request for handle: %d\n", value_handle);

  if (value_handle == deviceInfoHandle) {
    // Return device info JSON
    uint16_t len = deviceInfoJson.length();
    if (buffer && buffer_size >= len) {
      memcpy(buffer, deviceInfoJson.c_str(), len);
    }
    return len;
  }

  if (value_handle == wifiNetworksHandle) {
    // Return WiFi networks JSON
    uint16_t len = wifiNetworksJson.length();
    if (buffer && buffer_size >= len) {
      memcpy(buffer, wifiNetworksJson.c_str(), len);
    }
    return len;
  }

  if (value_handle == statusHandle) {
    // Return provisioning status
    if (buffer && buffer_size >= 1) {
      buffer[0] = (uint8_t)provStatus;
    }
    return 1;
  }

  if (value_handle == co2DataHandle) {
    // Return CO2 value as string
    String co2Str = String(co2Value);
    uint16_t len = co2Str.length();
    if (buffer && buffer_size >= len) {
      memcpy(buffer, co2Str.c_str(), len);
    }
    return len;
  }

  return 0;
}

int bleGattWriteCallback(uint16_t value_handle, uint8_t *buffer,
                         uint16_t size) {
  Serial.printf("[BLE] Write request for handle: %d, size: %d\n", value_handle,
                size);

  if (value_handle == wifiConfigHandle) {
    // Parse WiFi config JSON: {"ssid":"xxx","password":"yyy"}
    String config = String((char *)buffer, size);
    Serial.printf("[BLE] WiFi config received: %s\n", config.c_str());

    // Simple JSON parsing (avoiding ArduinoJson dependency)
    int ssidStart = config.indexOf("\"ssid\":\"") + 8;
    int ssidEnd = config.indexOf("\"", ssidStart);
    int pwdStart = config.indexOf("\"password\":\"") + 12;
    int pwdEnd = config.indexOf("\"", pwdStart);

    if (ssidStart > 8 && ssidEnd > ssidStart) {
      pendingSSID = config.substring(ssidStart, ssidEnd);
      if (pwdStart > 12 && pwdEnd > pwdStart) {
        pendingPassword = config.substring(pwdStart, pwdEnd);
      } else {
        pendingPassword = "";
      }
      wifiConfigReceived = true;
      Serial.printf("[BLE] Parsed SSID: %s\n", pendingSSID.c_str());
    }
    return 0;
  }

  return 0;
}

void setupBLE() {
  Serial.println("[BLE] Setting up BLE...");

  // Get chip ID for device name
  uint8_t mac[6];
  WiFi.macAddress(mac);
  chipId = String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
  chipId.toUpperCase();

  String deviceName = "neuma-" + chipId;
  Serial.printf("[BLE] Device name: %s\n", deviceName.c_str());

  // Prepare device info JSON
  deviceInfoJson = "{\"chipId\":\"" + chipId +
                   "\",\"version\":\"1.0.0\",\"name\":\"" + deviceName + "\"}";

  // Set BLE callbacks
  BTstack.setBLEDeviceConnectedCallback(bleDeviceConnectedCallback);
  BTstack.setBLEDeviceDisconnectedCallback(bleDeviceDisconnectedCallback);
  BTstack.setGATTCharacteristicRead(bleGattReadCallback);
  BTstack.setGATTCharacteristicWrite(bleGattWriteCallback);

  // Setup GATT Database
  BTstack.addGATTService(&serviceUUID);

  // Device Info - Read only
  deviceInfoHandle = BTstack.addGATTCharacteristicDynamic(&deviceInfoUUID,
                                                          ATT_PROPERTY_READ, 0);

  // WiFi Networks - Read only (device scans and reports available networks)
  wifiNetworksHandle = BTstack.addGATTCharacteristicDynamic(
      &wifiNetworksUUID, ATT_PROPERTY_READ, 0);

  // WiFi Config - Write only (app sends SSID/password)
  wifiConfigHandle = BTstack.addGATTCharacteristicDynamic(
      &wifiConfigUUID, ATT_PROPERTY_WRITE, 0);

  // Status - Read + Notify (connection status updates)
  statusHandle = BTstack.addGATTCharacteristicDynamic(
      &statusUUID, ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY, 0);

  // CO2 Data - Read + Notify (CO2 readings)
  co2DataHandle = BTstack.addGATTCharacteristicDynamic(
      &co2DataUUID, ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY, 0);

  // Set device name for advertising
  BTstack.setBLEAdvertisementCallback([](BLEAdvertisement *) {});

  // Initialize BTstack and start advertising
  BTstack.setup(deviceName.c_str());
  BTstack.startAdvertising();

  Serial.println("[BLE] BLE initialized and advertising!");
}

void updateProvStatus(ProvisionStatus status) {
  provStatus = status;
  Serial.printf("[BLE] Provisioning Status updated: %d\n", status);
  // TODO: Send notification to connected device
}

// ============================================================
// STARTUP ANIMATION
// ============================================================

void showStartupAnimation() {
  Serial.println("Starting boot animation...");

  float localAnim = 0.0f;
  uint16_t currentCO2 = 0;
  uint16_t previousCO2 = 0;
  unsigned long firstValidReadTime = 0;
  unsigned long startTime = millis();
  bool sensorResponding = false;

  unsigned long elapsed = millis() - startTime;
  while (elapsed < 30000) {
    drawStartupFrame(localAnim);

    localAnim += 0.06f;
    delay(16);

    if (elapsed % 1000 < 20) {
      previousCO2 = currentCO2;
      currentCO2 = readCO2();

      if (currentCO2 > 0) {
        if (!sensorResponding) {
          sensorResponding = true;
          firstValidReadTime = millis();
          Serial.printf("First valid reading: %d ppm\n", currentCO2);
        }

        if (previousCO2 > 0 && previousCO2 != currentCO2) {
          Serial.printf("Sensor ready: %d -> %d ppm\n", previousCO2,
                        currentCO2);
          break;
        }

        if (sensorResponding && (millis() - firstValidReadTime) > 15000) {
          Serial.println("15s elapsed with stable reading, proceeding...");
          break;
        }
      }
    }
    elapsed = millis() - startTime;
  }

  if (currentCO2 == 0) {
    Serial.println("WARNING: No valid CO2 data! Check sensor.");
    co2Value = 400;
  } else {
    co2Value = currentCO2;
  }
  Serial.println("Boot animation complete!");
}

// ============================================================
// CONFIG MODE
// ============================================================

void runConfigMode() {
  Serial.println("Entering configuration mode...");
  currentMode = MODE_CONFIG;

  // Do initial WiFi scan so networks are ready when app connects
  Serial.println("[WiFi] Doing initial network scan...");
  wifiNetworksJson = scanWifiNetworks();
  Serial.printf("[WiFi] Found networks: %s\n", wifiNetworksJson.c_str());

  // Start BLE
  setupBLE();

  unsigned long lastWifiScan = millis();

  while (currentMode == MODE_CONFIG) {
    // Draw config screen
    drawConfigFrame(tAnim);
    tAnim += ANIM_SPEED;

    // Handle BLE events (simplified - actual implementation needs BTstack
    // callbacks)

    // Check if we received WiFi config
    if (wifiConfigReceived) {
      wifiConfigReceived = false;
      Serial.printf("Received WiFi config: SSID=%s\n", pendingSSID.c_str());

      updateProvStatus(PROV_CONNECTING);

      if (connectToWifi(pendingSSID, pendingPassword, 15000)) {
        updateProvStatus(PROV_CONNECTED);
        saveWifiCredentials(pendingSSID, pendingPassword);
        storedSSID = pendingSSID;
        storedPassword = pendingPassword;

        delay(2000); // Give app time to see success
        currentMode = MODE_NORMAL;
      } else {
        // Check if it was a password issue
        if (WiFi.status() == WL_CONNECT_FAILED) {
          updateProvStatus(PROV_WRONG_PASSWORD);
        } else {
          updateProvStatus(PROV_FAILED);
        }
      }
    }

    // Periodic WiFi scan if connected via BLE (every 30s or on request)
    if (wifiScanRequested ||
        (bleConnected && millis() - lastWifiScan > 30000)) {
      wifiScanRequested = false;
      lastWifiScan = millis();
      wifiNetworksJson = scanWifiNetworks();
      Serial.printf("[BLE] WiFi networks updated: %d chars\n",
                    wifiNetworksJson.length());
    }

    // Process BLE events
    BTstack.loop();
    delay(16);
  }
}

// ============================================================
// NORMAL MODE
// ============================================================

void runNormalMode() {
  Serial.println("Entering normal mode...");
  currentMode = MODE_NORMAL;

  while (currentMode == MODE_NORMAL) {
    unsigned long frameStart = millis();

    // Read CO2 every 3 seconds
    if (millis() - lastCO2Read > 3000) {
      co2Value = readCO2();
      lastCO2Read = millis();
      Serial.printf("CO2: %d ppm\n", co2Value);
    }

    // Broadcast CO2 via BLE every 60 seconds
    if (millis() - lastBLEBroadcast > 60000) {
      lastBLEBroadcast = millis();
      Serial.printf("[BLE] CO2 broadcast: %d ppm\n", co2Value);
      // CO2 value is served via BLE read callback
    }

    drawNormalFrame();

    tAnim += ANIM_SPEED;

    // Process BLE events
    BTstack.loop();

    unsigned long frameTime = millis() - frameStart;
    if (frameTime < 20) {
      delay(20 - frameTime);
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

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    while (1)
      ;
  }

  // Try canvas for double buffering
  canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx);
  if (canvas->begin()) {
    Serial.println("Double buffering enabled!");
  } else {
    delete canvas;
    canvas = nullptr;
    Serial.println("Direct rendering mode");
  }

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }

  // Show startup animation while sensor warms up
  showStartupAnimation();

  // Check for stored WiFi credentials
  bool hasCredentials = loadWifiCredentials();

  if (hasCredentials) {
    Serial.println("Found stored WiFi credentials, attempting connection...");

    // Show connecting animation briefly
    drawStartupFrame(tAnim);

    if (connectToWifi(storedSSID, storedPassword, 10000)) {
      Serial.println("WiFi connected from stored credentials!");
      currentMode = MODE_NORMAL;
    } else {
      Serial.println("Stored WiFi failed, entering config mode");
      currentMode = MODE_CONFIG;
    }
  } else {
    Serial.println("No stored WiFi credentials, entering config mode");
    currentMode = MODE_CONFIG;
  }
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  if (currentMode == MODE_CONFIG) {
    runConfigMode();
  } else if (currentMode == MODE_NORMAL) {
    runNormalMode();
  }
}
