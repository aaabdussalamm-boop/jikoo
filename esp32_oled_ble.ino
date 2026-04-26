/*
 * ESP32 OLED BLE Controller Firmware
 * Supports: SSD1306 (0.96") and SH1106 (1.3") OLED displays
 * Features: BLE receive (text, emoticons, bitmap, clock), Serial commands
 *
 * Libraries required:
 *   - Adafruit GFX Library
 *   - Adafruit SSD1306
 *   - U8g2 (for SH1106)
 *   - NimBLE-Arduino (lightweight BLE)
 *
 * Board: ESP32 Dev Module
 * Flash Size: 4MB
 */

#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <Adafruit_GFX.h>

// ===================== SCREEN TYPE CONFIG =====================
// This string will be replaced by the web flasher at offset search
// DO NOT CHANGE the format of SCREEN_TYPE_PLACEHOLDER
#define SCREEN_CONFIG "SCREEN_TYPE_PLACEHOLDER"   // replaced by flasher: "SSD1306" or "SH1106"

#if defined(USE_SSD1306) || true  // default include both, switch at runtime
#include <Adafruit_SSD1306.h>
#endif

// ===================== OLED CONFIG =====================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDRESS  0x3C

// ===================== BLE UUIDs =====================
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789012"
#define CHAR_RECEIVE_UUID   "12345678-1234-1234-1234-123456789013"  // controller -> ESP32
#define CHAR_STATUS_UUID    "12345678-1234-1234-1234-123456789014"  // ESP32 -> controller

// ===================== GLOBALS =====================
String btName = "ESP32-OLED";
String screenType = "SSD1306"; // default, overridden at boot

Adafruit_SSD1306* display_ssd = nullptr;

// SH1106 raw driver (SSD1306 compatible with page offset)
bool isRunning = false;
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pStatusChar = nullptr;

// Display queue
struct DisplayCmd {
  String type;     // "text", "emoticon", "bitmap", "clock", "clear"
  String content;
  int duration;    // ms, 0 = persistent
};

DisplayCmd currentCmd;
unsigned long cmdStartTime = 0;
bool cmdActive = false;

// Clock
bool clockMode = false;
String clockTime = "";

// ===================== SCREEN HELPERS =====================
void initDisplay() {
  Wire.begin(21, 22); // SDA=21, SCL=22 (default ESP32)
  
  if (screenType == "SH1106") {
    // SH1106 uses same I2C but with 2-pixel offset
    // We use SSD1306 driver with SH1106 workaround
    display_ssd = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
    display_ssd->begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, false, false);
    // SH1106 adjustment: set column start offset
  } else {
    display_ssd = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
    if (!display_ssd->begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
      Serial.println("SSD1306 init failed!");
    }
  }
  
  display_ssd->clearDisplay();
  display_ssd->setTextColor(SSD1306_WHITE);
  display_ssd->setTextSize(1);
  display_ssd->setCursor(0, 0);
  display_ssd->println("ESP32 OLED Ready");
  display_ssd->println("BLE: " + btName);
  display_ssd->display();
}

void clearScreen() {
  display_ssd->clearDisplay();
  display_ssd->display();
}

void showText(const String& text, int textSize = 1) {
  display_ssd->clearDisplay();
  display_ssd->setTextSize(textSize);
  display_ssd->setTextColor(SSD1306_WHITE);
  display_ssd->setCursor(0, 0);
  
  // Word wrap
  int lineHeight = 8 * textSize;
  int maxLines = SCREEN_HEIGHT / lineHeight;
  int maxChars = SCREEN_WIDTH / (6 * textSize);
  
  String remaining = text;
  int line = 0;
  while (remaining.length() > 0 && line < maxLines) {
    String chunk = remaining.substring(0, maxChars);
    remaining = remaining.substring(chunk.length());
    display_ssd->setCursor(0, line * lineHeight);
    display_ssd->print(chunk);
    line++;
  }
  display_ssd->display();
}

void showClock(const String& timeStr) {
  display_ssd->clearDisplay();
  
  // Large clock display
  display_ssd->setTextSize(3);
  display_ssd->setTextColor(SSD1306_WHITE);
  
  // Center the time
  int16_t x1, y1;
  uint16_t w, h;
  display_ssd->getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int cx = (SCREEN_WIDTH - w) / 2;
  int cy = (SCREEN_HEIGHT / 2) - (h / 2) - 4;
  
  display_ssd->setCursor(cx, cy);
  display_ssd->print(timeStr);
  
  // Small label
  display_ssd->setTextSize(1);
  display_ssd->setCursor(48, 52);
  display_ssd->print("LIVE CLOCK");
  
  display_ssd->display();
}

// Draw emoticons using pixel art
void drawEmoticon(const String& name) {
  display_ssd->clearDisplay();
  
  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  int r = 25;
  
  // Draw circle (face)
  display_ssd->drawCircle(cx, cy, r, SSD1306_WHITE);
  
  if (name == "smile") {
    // Eyes
    display_ssd->fillCircle(cx - 8, cy - 8, 3, SSD1306_WHITE);
    display_ssd->fillCircle(cx + 8, cy - 8, 3, SSD1306_WHITE);
    // Smile arc (manual pixels)
    for (int i = -10; i <= 10; i++) {
      int y = cy + 8 + (i * i) / 14;
      display_ssd->drawPixel(cx + i, y, SSD1306_WHITE);
    }
  } else if (name == "sad") {
    // Eyes
    display_ssd->fillCircle(cx - 8, cy - 8, 3, SSD1306_WHITE);
    display_ssd->fillCircle(cx + 8, cy - 8, 3, SSD1306_WHITE);
    // Sad arc
    for (int i = -10; i <= 10; i++) {
      int y = cy + 14 - (i * i) / 14;
      display_ssd->drawPixel(cx + i, y, SSD1306_WHITE);
    }
  } else if (name == "heart") {
    display_ssd->clearDisplay();
    // Heart shape centered
    for (int y = -15; y <= 15; y++) {
      for (int x = -18; x <= 18; x++) {
        float nx = (float)x / 18.0f;
        float ny = (float)y / 15.0f;
        // Heart equation
        float val = (nx*nx + ny*ny - 1);
        if (val * val * val - nx*nx*ny*ny*ny <= 0) {
          display_ssd->drawPixel(cx + x, cy - y + 5, SSD1306_WHITE);
        }
      }
    }
  } else if (name == "thumbsup") {
    display_ssd->clearDisplay();
    // Thumb up icon
    // Fist
    display_ssd->fillRect(cx - 10, cy - 5, 20, 18, SSD1306_WHITE);
    // Thumb
    display_ssd->fillRect(cx - 10, cy - 22, 8, 18, SSD1306_WHITE);
    // Thumb tip rounded
    display_ssd->fillCircle(cx - 6, cy - 22, 4, SSD1306_WHITE);
    // Knuckle lines
    display_ssd->drawLine(cx - 4, cy - 5, cx - 4, cy + 13, SSD1306_BLACK);
    display_ssd->drawLine(cx + 2, cy - 5, cx + 2, cy + 13, SSD1306_BLACK);
    display_ssd->drawLine(cx + 8, cy - 5, cx + 8, cy + 13, SSD1306_BLACK);
  }
  
  display_ssd->display();
}

// Draw bitmap (128x64, 1bpp, raw bytes, 1024 bytes)
void drawBitmap(const uint8_t* bitmap, int len) {
  if (len < 1024) return;
  display_ssd->clearDisplay();
  display_ssd->drawBitmap(0, 0, bitmap, 128, 64, SSD1306_WHITE);
  display_ssd->display();
}

// ===================== BLE CALLBACKS =====================
class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) override {
    Serial.println("BLE Client connected");
    showText("BLE Connected!\nReady for\ncommands...", 1);
  }
  void onDisconnect(NimBLEServer* pServer) override {
    Serial.println("BLE Client disconnected");
    showText("BLE Disconnected\nWaiting...", 1);
    pServer->startAdvertising();
  }
};

// BLE data buffer for large messages (bitmap chunking)
static String bleBuffer = "";
static bool receivingBitmap = false;
static std::vector<uint8_t> bitmapBuffer;

class CharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    String data = String(val.c_str());
    Serial.println("BLE RX: " + data);
    
    processCommand(data);
  }
};

void processCommand(const String& raw) {
  // Protocol: TYPE:CONTENT:DURATION
  // Types: TEXT, EMOTICON, BITMAP_START, BITMAP_DATA, BITMAP_END, CLOCK, CLEAR
  
  int firstColon = raw.indexOf(':');
  if (firstColon < 0) return;
  
  String type = raw.substring(0, firstColon);
  String rest = raw.substring(firstColon + 1);
  type.trim();
  type.toUpperCase();
  
  if (type == "TEXT") {
    int lastColon = rest.lastIndexOf(':');
    String content = rest;
    int duration = 0;
    if (lastColon > 0) {
      content = rest.substring(0, lastColon);
      duration = rest.substring(lastColon + 1).toInt();
    }
    currentCmd = {"text", content, duration};
    cmdStartTime = millis();
    cmdActive = (duration > 0);
    clockMode = false;
    showText(content, content.length() < 20 ? 2 : 1);
    
  } else if (type == "EMOTICON") {
    int lastColon = rest.lastIndexOf(':');
    String name = rest;
    int duration = 3000;
    if (lastColon > 0) {
      name = rest.substring(0, lastColon);
      duration = rest.substring(lastColon + 1).toInt();
    }
    name.toLowerCase();
    name.trim();
    currentCmd = {"emoticon", name, duration};
    cmdStartTime = millis();
    cmdActive = true;
    clockMode = false;
    drawEmoticon(name);
    
  } else if (type == "BITMAP_START") {
    bitmapBuffer.clear();
    receivingBitmap = true;
    bleBuffer = "";
    Serial.println("Bitmap transfer started");
    
  } else if (type == "BITMAP_DATA") {
    if (receivingBitmap) {
      // Base64 encoded chunk
      bleBuffer += rest;
    }
    
  } else if (type == "BITMAP_END") {
    if (receivingBitmap) {
      receivingBitmap = false;
      // Decode base64
      decodeAndShowBitmap(bleBuffer);
      bleBuffer = "";
    }
    
  } else if (type == "CLOCK") {
    // CLOCK:HH:MM:SS or CLOCK:HH:MM
    clockMode = true;
    clockTime = rest;
    cmdActive = false;
    showClock(clockTime);
    
  } else if (type == "CLEAR") {
    clockMode = false;
    cmdActive = false;
    clearScreen();
    
  } else if (type == "NOTIFY") {
    // Notification from phone: NOTIFY:AppName:Message:DURATION
    int c1 = rest.indexOf(':');
    int lastC = rest.lastIndexOf(':');
    String appName = rest.substring(0, c1);
    String message = rest.substring(c1 + 1, lastC);
    int dur = rest.substring(lastC + 1).toInt();
    
    display_ssd->clearDisplay();
    display_ssd->setTextSize(1);
    display_ssd->setTextColor(SSD1306_WHITE);
    // App name header
    display_ssd->fillRect(0, 0, 128, 10, SSD1306_WHITE);
    display_ssd->setTextColor(SSD1306_BLACK);
    display_ssd->setCursor(2, 1);
    display_ssd->print(appName.substring(0, 21));
    display_ssd->setTextColor(SSD1306_WHITE);
    // Message body
    display_ssd->setCursor(0, 14);
    display_ssd->print(message.substring(0, 84)); // max ~84 chars
    display_ssd->display();
    
    currentCmd = {"notify", message, dur};
    cmdStartTime = millis();
    cmdActive = (dur > 0);
    clockMode = false;
  }
}

// Base64 decode
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int b64val(char c) {
  for (int i = 0; i < 64; i++) if (b64chars[i] == c) return i;
  return -1;
}

void decodeAndShowBitmap(const String& b64) {
  std::vector<uint8_t> decoded;
  int len = b64.length();
  for (int i = 0; i + 3 < len; i += 4) {
    int v0 = b64val(b64[i]);
    int v1 = b64val(b64[i+1]);
    int v2 = b64val(b64[i+2]);
    int v3 = b64val(b64[i+3]);
    if (v0 < 0 || v1 < 0) break;
    decoded.push_back((v0 << 2) | (v1 >> 4));
    if (v2 >= 0) decoded.push_back(((v1 & 0xF) << 4) | (v2 >> 2));
    if (v3 >= 0) decoded.push_back(((v2 & 0x3) << 6) | v3);
  }
  if (decoded.size() >= 1024) {
    drawBitmap(decoded.data(), decoded.size());
  }
}

// ===================== SERIAL COMMAND HANDLER =====================
void handleSerialCommand(const String& cmd) {
  // Commands from web flasher / serial terminal
  // SET_NAME:NewName
  // EMOTICON:smile:3000
  // TEXT:Hello World:5000
  // CLEAR
  
  Serial.println("Serial CMD: " + cmd);
  
  if (cmd.startsWith("SET_NAME:")) {
    btName = cmd.substring(9);
    btName.trim();
    Serial.println("BT Name set to: " + btName);
    // Note: BLE name change requires restart
    display_ssd->clearDisplay();
    display_ssd->setTextSize(1);
    display_ssd->setCursor(0, 0);
    display_ssd->println("BT Name Changed:");
    display_ssd->println(btName);
    display_ssd->println("Restarting...");
    display_ssd->display();
    delay(2000);
    // Save name to preferences and restart
    // (simplified: just restart with new advertising name)
    NimBLEDevice::deinit(true);
    delay(500);
    NimBLEDevice::init(btName.c_str());
    setupBLE();
    showText("BLE Ready\n" + btName, 1);
    
  } else if (cmd.startsWith("EMOTICON:")) {
    processCommand(cmd);
  } else if (cmd.startsWith("TEXT:")) {
    processCommand(cmd);
  } else if (cmd == "CLEAR") {
    clearScreen();
    cmdActive = false;
    clockMode = false;
  } else {
    Serial.println("Unknown command: " + cmd);
  }
}

// ===================== BLE SETUP =====================
void setupBLE() {
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  NimBLEService* pService = pServer->createService(SERVICE_UUID);
  
  NimBLECharacteristic* pReceiveChar = pService->createCharacteristic(
    CHAR_RECEIVE_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pReceiveChar->setCallbacks(new CharCallbacks());
  
  pStatusChar = pService->createCharacteristic(
    CHAR_STATUS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  pStatusChar->setValue("READY");
  
  pService->start();
  
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->start();
  
  Serial.println("BLE advertising as: " + btName);
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32 OLED BLE Controller");
  Serial.println("Screen: " + String(SCREEN_CONFIG));
  
  // Determine screen type from compiled config
  if (String(SCREEN_CONFIG).indexOf("SH1106") >= 0) {
    screenType = "SH1106";
  } else {
    screenType = "SSD1306";
  }
  Serial.println("Using screen: " + screenType);
  
  initDisplay();
  
  // Init BLE
  NimBLEDevice::init(btName.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max power
  setupBLE();
  
  Serial.println("Ready! Commands:");
  Serial.println("  SET_NAME:<name>");
  Serial.println("  EMOTICON:<smile|sad|heart|thumbsup>:<duration_ms>");
  Serial.println("  TEXT:<message>:<duration_ms>");
  Serial.println("  CLEAR");
  
  showText("ESP32 OLED\nBLE Ready!\n" + btName, 1);
}

// ===================== LOOP =====================
String serialBuffer = "";

void loop() {
  // Handle serial input
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        handleSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }
  
  // Handle clock update (receive via BLE every second)
  // Clock is updated by BLE CLOCK command
  
  // Handle timed display commands
  if (cmdActive && currentCmd.duration > 0) {
    if (millis() - cmdStartTime >= (unsigned long)currentCmd.duration) {
      cmdActive = false;
      if (clockMode) {
        showClock(clockTime);
      } else {
        clearScreen();
      }
    }
  }
  
  delay(10);
}
