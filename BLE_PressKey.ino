/**
 * 
 // Hassinator – Always-On Bluetooth Keyboard
 // Copyright (c) 2025 AndreaFuzz <https://github.com/AndreaFuzz/>
 // SPDX-License-Identifier: MIT
 *
 * with NeoPixel LED, battery service, factory reset
 *
 * - Built for ESP32-C3 Super Mini (Arduino-ESP32 ≥ 3.2.0 + NimBLE-Arduino v2.2.3)
 * - Button on GPIO4 (hold ≥ 3 s → factory reset, short press → key press + deep-sleep)
 * - On-board NeoPixel on GPIO8
 * - Battery service fixed at 100 % (Windows requirement for HID-over-GATT)
 * - LED states:
 *     · BLINK BLUE   : advertising
 *     · SOLID GREEN  : connected
 *     · SOLID RED    : factory reset
 *     · HALF WHITE   : sending key
 */

/* ======== Easy-to-edit settings ======================================== */
static const char*  DEVICE_NAME = "Hassinator"; // Change this to rename the peripheral
static const uint8_t KEY_USAGE  = 0x42;         // 0x42 = F9; see USB HID Usage Tables
/* ======================================================================= */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEServer.h>
#include <Preferences.h>
#include "driver/gpio.h"
#include <Adafruit_NeoPixel.h>

// ================= Debug Flag ====================
#ifndef DEBUG_ENABLED            // set to 1 for serial logs while testing, 0 when deploying to save power
#define DEBUG_ENABLED 1
#endif

#if DEBUG_ENABLED
  #define LOG(fmt, ...)  Serial.printf("[%lu ms] " fmt "\n", millis(), ##__VA_ARGS__)
  #define DEBUG_INIT()   do { Serial.begin(115200); delay(100); } while (0)
#else
  #define LOG(...)       ((void)0)
  #define DEBUG_INIT()   ((void)0)
#endif

// ================= Pins & Timings ================
static constexpr gpio_num_t  BUTTON_PIN   = GPIO_NUM_4; // press = GND
static constexpr int         WS2812_PIN   = 8;
static constexpr int         WS2812_CNT   = 1;
static constexpr uint32_t    FACTORY_MS   = 3000; // 3 seconds → factory reset
static constexpr uint32_t    LED_BLINK_MS = 400;  // blink toggle interval

// ================ LED Enums/Vars =================
enum LEDMode {
  LED_OFF,
  LED_BLINK_BLUE,   // advertising
  LED_SOLID_GREEN,  // connected
  LED_SOLID_RED,    // factory reset
  LED_HALF_WHITE,   // sending key
};

Adafruit_NeoPixel  neo(WS2812_CNT, WS2812_PIN, NEO_GRB + NEO_KHZ800);
LEDMode            currentLEDMode  = LED_OFF;
uint32_t           lastBlinkToggle = 0;
bool               ledOn           = false;

void setPixelColor(uint8_t r, uint8_t g, uint8_t b) {
  neo.clear();
  neo.setPixelColor(0, neo.Color(r, g, b));
  neo.show();
}

void setLEDMode(LEDMode mode) {
  currentLEDMode = mode;
  lastBlinkToggle = millis();
  ledOn = false;
}

void updateLED() {
  switch (currentLEDMode) {
    case LED_OFF:
      setPixelColor(0, 0, 0);
      break;

    case LED_BLINK_BLUE: {
      uint32_t now = millis();
      if (now - lastBlinkToggle >= LED_BLINK_MS) {
        lastBlinkToggle = now;
        ledOn = !ledOn;
        if (ledOn) setPixelColor(0, 0, 255);
        else       setPixelColor(0, 0, 0);
      }
    } break;

    case LED_SOLID_GREEN:
      setPixelColor(0, 255, 0);
      break;

    case LED_SOLID_RED:
      setPixelColor(255, 0, 0);
      break;

    case LED_HALF_WHITE:
      setPixelColor(128, 128, 128);
      break;
  }
}

// =============== Globals =========================
Preferences           prefs;
NimBLEHIDDevice*      hid                 = nullptr;
NimBLECharacteristic* inputRpt           = nullptr;
bool                  deviceConnected     = false;
bool                  factoryResetPending = false;
bool                  pendingKey          = false;
unsigned long         pressStart          = 0;

// HID Keyboard report w/ Report ID = 1
static const uint8_t keyboardReportMap[] = {
  0x05,0x01, 0x09,0x06, 0xA1,0x01,
    0x85,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x03,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65,
    0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00,
  0xC0
};

// ================ Factory Reset ==================
void eraseAllBonds() {
  if (NimBLEDevice::deleteAllBonds()) {
    LOG("[RESET] all Bluetooth bonds erased");
  } else {
    LOG("[RESET] no bonds found or deletion failed");
  }
}

void doFactoryReset() {
  LOG("[RESET] factory reset");
  setLEDMode(LED_SOLID_RED);
  unsigned long t0 = millis();
  while (millis() - t0 < 2000) { updateLED(); delay(25); }

  eraseAllBonds();
  setLEDMode(LED_BLINK_BLUE);
  NimBLEDevice::startAdvertising();
  factoryResetPending = false;
}

// ================ Send Key =======================
void sendKey() {
  delay(100);
  LOG("[KEY] sending usage 0x%02X", KEY_USAGE);
  setLEDMode(LED_HALF_WHITE);

  uint8_t report[8] = {0};
  /* press */
  report[2] = KEY_USAGE;
  inputRpt->setValue(report, sizeof(report));
  inputRpt->notify();
  delay(15);
  /* release */
  memset(report, 0, sizeof(report));
  inputRpt->setValue(report, sizeof(report));
  inputRpt->notify();

  LOG("[KEY] sent");
  if (deviceConnected) setLEDMode(LED_SOLID_GREEN);
  else                 setLEDMode(LED_BLINK_BLUE);
}

// ================ Server Callbacks ===============
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    deviceConnected = true;
    LOG("[LINK] up");
    setLEDMode(LED_SOLID_GREEN);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    deviceConnected = false;
    LOG("[LINK] down (reason=%d)", reason);
    setLEDMode(LED_BLINK_BLUE);
  }
};

// ============== Bluetooth & Setup ======================
void setupBLE() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_N0);
  NimBLEDevice::setSecurityAuth(true, false, true);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  hid = new NimBLEHIDDevice(server);
  hid->setManufacturer("MyCompany");
  hid->setHidInfo(0x0111, 0x00);
  hid->setReportMap((uint8_t*)keyboardReportMap, sizeof(keyboardReportMap));
  inputRpt = hid->getInputReport(1);
  hid->startServices();

  // Battery service (100 %)
  auto* batt = server->createService(NimBLEUUID((uint16_t)0x180F));
  uint8_t lvl = 100;
  batt->createCharacteristic(NimBLEUUID((uint16_t)0x2A19),
    NIMBLE_PROPERTY::READ)->setValue(&lvl, 1);
  batt->start();

  auto adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->setAppearance(0x03C4); // Keyboard
  adv->addServiceUUID(hid->getHidService()->getUUID());
  adv->setMinInterval(32);
  adv->setMaxInterval(48);
  adv->start();
  LOG("[ADV] start");
}

// ================= Deep Sleep ====================
void enterDeepSleep() {
  LOG("[POWER] entering deep sleep…");
  /* turn LED off explicitly */
  neo.clear();
  neo.show();
  currentLEDMode = LED_OFF;

  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);

  gpio_hold_en(BUTTON_PIN);

  esp_deep_sleep_enable_gpio_wakeup((1ULL << BUTTON_PIN),
                                    ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// ============ Arduino Setup ======================
void setup() {
  DEBUG_INIT();
  LOG("[Boot] starting up…");
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  neo.begin();
  neo.setBrightness(128);             // 50 % global brightness
  neo.clear();
  neo.show();

  setupBLE();
  setLEDMode(LED_BLINK_BLUE);
}

// ============ Main Loop ==========================
void loop() {
  updateLED();

  /* Handle pending key once connected */
  if (pendingKey && deviceConnected) {
    pendingKey = false;
    sendKey();
    delay(100); // allow host to process keystroke
    enterDeepSleep();
  }

  /* ===== Button handling ===== */
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  static bool wasPressed = false;

  if (pressed && !wasPressed) {         // button down
    wasPressed = true;
    pressStart = millis();
    LOG("[BTN] down");
  }

  if (!pressed && wasPressed) {         // button released
    wasPressed = false;
    uint32_t heldMs = millis() - pressStart;
    LOG("[BTN] up after %lu ms", heldMs);
    pressStart = 0;

    if (!factoryResetPending && heldMs < FACTORY_MS) {
      /* short press → send F9 (only when linked) */
      if (deviceConnected) {
        sendKey();
        delay(100);
        enterDeepSleep();
      } else {
        LOG("[KEY] waiting for link before sending");
        pendingKey = true;
        setLEDMode(LED_BLINK_BLUE);
        NimBLEDevice::startAdvertising();
      }
    }
  }

  /* Long-press factory reset */
  if (pressed && wasPressed && !factoryResetPending) {
    if ((millis() - pressStart) >= FACTORY_MS) {
      factoryResetPending = true;
      doFactoryReset();
    }
  }

  delay(20);
}
