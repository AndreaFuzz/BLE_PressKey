/**
 *  Hassinator – Always-On Bluetooth Keyboard
 *  2025 AndreaFuzz  ·  SPDX-License-Identifier: MIT
 *
 *  ────────────────────────────────────────────────────────────────────
 *  Hardware
 *    • ESP32-C3 Super Mini  (Arduino-ESP32 ≥ 3.2.0  +  NimBLE-Arduino 2.2.3)
 *    • Momentary button on GPIO-4, pulled-up internally (LOW when pressed)
 *    • Single NeoPixel (RGB LED) on GPIO-8
 *
 *  Behaviour recap
 *    • Short press        → send one F9 keystroke via HID-over-GATT, then sleep
 *    • 3-second long press→ factory-reset all Bluetooth bonds,   then sleep
 *
 *  Battery-saving rules
 *    1. 60-second advertising watchdog  
 *       If still unpaired after 60 s (LED blinking blue), go to deep-sleep.
 *    2. 5-second idle watchdog after the very first successful pairing  
 *       If no button press occurs within 5 s of a new link (LED solid green),
 *       assume the host just wanted to pair and let the MCU sleep.
 *
 *  LED legend
 *    · BLINK BLUE   advertising / waiting to pair
 *    · SOLID GREEN  connected
 *    · SOLID RED    factory-reset in progress
 *    · HALF WHITE   sending key report
 *  ────────────────────────────────────────────────────────────────────
 */

#include <stdint.h>  // ensures uint8_t is known before we use it

/* ======== Easy-to-edit identifiers ==================================== */
static const char*  DEVICE_NAME = "Hassinator";   // BLE peripheral name
static const uint8_t KEY_USAGE  = 0x42;           // USB-HID usage code for F9
/* ======================================================================= */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEServer.h>
#include <Preferences.h>
#include "driver/gpio.h"
#include <Adafruit_NeoPixel.h>

/* ================= Debug output ======================================== */
#ifndef DEBUG_ENABLED                 // 1 = serial logs, 0 = max battery
  #define DEBUG_ENABLED 0
#endif

#if DEBUG_ENABLED
  #define LOG(fmt, ...)  Serial.printf("[%lu ms] " fmt "\n", millis(), ##__VA_ARGS__)
  #define DEBUG_INIT()   do { Serial.begin(115200); delay(100); } while (0)
#else
  #define LOG(...)       ((void)0)
  #define DEBUG_INIT()   ((void)0)
#endif

/* ================= GPIO & timing constants ============================ */
static constexpr gpio_num_t BUTTON_PIN   = GPIO_NUM_4; // LOW when pressed
static constexpr int        WS2812_PIN   = 8;
static constexpr int        WS2812_CNT   = 1;

static constexpr uint32_t   FACTORY_MS   = 3000;  // ≥3 s hold → factory reset
static constexpr uint32_t   LED_BLINK_MS = 400;   // 0.4 s on / 0.4 s off

/* --- POWER-SAVE --------------------------------------------------------- */
/* 60 000 ms watchdog: if still unpaired (advertising) after this period,
 * cut current consumption to zero by entering deep-sleep.                */
static constexpr uint32_t   ADV_TIMEOUT_MS  = 60000;

/* 5 000 ms idle timer: starts the moment the very first connection is
 * established.  If the user never presses the button, we assume they only
 * wanted to pair; sleep to preserve battery.  Any button press resets it. */
static constexpr uint32_t   CONNECT_IDLE_MS = 5000;
/* ---------------------------------------------------------------------- */

/* ====================== LED helper ===================================== */
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

inline void setPixelColor(uint8_t r, uint8_t g, uint8_t b) {
  neo.clear(); neo.setPixelColor(0, neo.Color(r, g, b)); neo.show();
}
inline void setLEDMode(LEDMode mode) {
  currentLEDMode  = mode;
  lastBlinkToggle = millis();
  ledOn           = false;
}
void updateLED() {
  switch (currentLEDMode) {
    case LED_OFF:          setPixelColor(0,0,0); break;
    case LED_BLINK_BLUE: {
      uint32_t now = millis();
      if (now - lastBlinkToggle >= LED_BLINK_MS) {
        lastBlinkToggle = now;
        ledOn = !ledOn;
        setPixelColor(0,0, ledOn ? 255 : 0);
      }
    } break;
    case LED_SOLID_GREEN:  setPixelColor(0,255,0);     break;
    case LED_SOLID_RED:    setPixelColor(255,0,0);     break;
    case LED_HALF_WHITE:   setPixelColor(128,128,128); break;
  }
}

/* ====================== Globals ======================================== */
Preferences           prefs;
NimBLEHIDDevice*      hid                 = nullptr;
NimBLECharacteristic* inputRpt           = nullptr;
bool                  deviceConnected     = false;
bool                  factoryResetPending = false;
bool                  pendingKey          = false;
unsigned long         pressStart          = 0;

/* Timers used by the power-save rules */
unsigned long         advStartTime        = 0;  // millis() when adverts began
unsigned long         connStartTime       = 0;  // millis() when link came up

/* ====================== HID report map ================================= */
static const uint8_t keyboardReportMap[] = {
  0x05,0x01, 0x09,0x06, 0xA1,0x01,
    0x85,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x03,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65,
    0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00,
  0xC0
};

/* ====================== Factory-reset helpers ========================== */
void eraseAllBonds() {
  if (NimBLEDevice::deleteAllBonds())
    LOG("[RESET] all Bluetooth bonds erased");
  else
    LOG("[RESET] no bonds found or deletion failed");
}
void doFactoryReset() {
  LOG("[RESET] factory reset");
  setLEDMode(LED_SOLID_RED);
  unsigned long t0 = millis();
  while (millis() - t0 < 2000) { updateLED(); delay(25); }

  eraseAllBonds();                        // remove stored pairings
  setLEDMode(LED_BLINK_BLUE);
  NimBLEDevice::startAdvertising();       // reopen for pairing
  factoryResetPending = false;
  advStartTime = millis();                // restart 60 s advert watchdog
}

/* ====================== Keystroke routine ============================== */
void sendKey() {
  /* Press-and-release sequence for F9 (usage 0x42) */
  delay(100);
  LOG("[KEY] sending usage 0x%02X", KEY_USAGE);
  setLEDMode(LED_HALF_WHITE);

  uint8_t report[8] = {0};
  report[2] = KEY_USAGE;                         // press
  inputRpt->setValue(report, sizeof(report));
  inputRpt->notify();
  delay(15);
  memset(report, 0, sizeof(report));             // release
  inputRpt->setValue(report, sizeof(report));
  inputRpt->notify();

  LOG("[KEY] sent");
  setLEDMode(deviceConnected ? LED_SOLID_GREEN
                             : LED_BLINK_BLUE);
}

/* ====================== BLE server callbacks =========================== */
class ServerCB : public NimBLEServerCallbacks {
  void onConnect   (NimBLEServer*, NimBLEConnInfo&) override {
    deviceConnected = true;
    LOG("[LINK] up");
    setLEDMode(LED_SOLID_GREEN);
    connStartTime = millis();            // start 5 s idle countdown
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    deviceConnected = false;
    LOG("[LINK] down (reason=%d)", reason);
    setLEDMode(LED_BLINK_BLUE);
    NimBLEDevice::startAdvertising();    // ready for next host
    advStartTime = millis();             // new 60 s advert watchdog
  }
};

/* ====================== BLE stack setup ================================ */
void setupBLE() {
  NimBLEDevice::init(DEVICE_NAME);

#if defined(ESP_PWR_LVL_N2)              // –6 dBm on most ESP32 cores
  NimBLEDevice::setPower(ESP_PWR_LVL_N2);
#else
  NimBLEDevice::setPower(ESP_PWR_LVL_N6); // fallback enum name
#endif
  NimBLEDevice::setSecurityAuth(true, false, true); // bonding, no MITM/PIN

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  /* HID service -------------------------------------------------------- */
  hid = new NimBLEHIDDevice(server);
  hid->setManufacturer("MyCompany");
  hid->setHidInfo(0x0111, 0x00);                   // HID 1.11
  hid->setReportMap((uint8_t*)keyboardReportMap, sizeof(keyboardReportMap));
  inputRpt = hid->getInputReport(1);
  hid->startServices();

  /* Battery service (always 100 %) ------------------------------------ */
  auto* batt = server->createService(NimBLEUUID((uint16_t)0x180F));
  uint8_t lvl = 100;
  batt->createCharacteristic(NimBLEUUID((uint16_t)0x2A19),
                             NIMBLE_PROPERTY::READ)->setValue(&lvl, 1);
  batt->start();

  /* Advertising -------------------------------------------------------- */
  auto adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->setAppearance(0x03C4);                      // Generic Keyboard
  adv->addServiceUUID(hid->getHidService()->getUUID());
  adv->setMinInterval(32); adv->setMaxInterval(48);
  adv->start();
  LOG("[ADV] start");

  advStartTime = millis();                         // arm advert watchdog
}

/* ====================== Deep-sleep wrapper ============================= */
void enterDeepSleep() {
  LOG("[POWER] entering deep sleep…");
  neo.clear(); neo.show();                         // LED off
  currentLEDMode = LED_OFF;

  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);                      // shut down BLE stack

  gpio_hold_en(BUTTON_PIN);                        // hold state during sleep
  esp_deep_sleep_enable_gpio_wakeup((1ULL<<BUTTON_PIN),
                                    ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();                          // Z-z-z-z…
}

/* ====================== Arduino setup ================================= */
void setup() {
  DEBUG_INIT();
  LOG("[Boot] starting up…");
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  neo.begin(); neo.setBrightness(128); neo.clear(); neo.show();

  setupBLE();
  setLEDMode(LED_BLINK_BLUE);                      // waiting for host
}

/* ====================== Main loop ===================================== */
void loop() {
  updateLED();                                     // animate LED modes

  /* -------- Advert watchdog: sleep if still unpaired after 60 s -------- */
  if (!deviceConnected && (millis() - advStartTime >= ADV_TIMEOUT_MS)) {
    LOG("[POWER] advert timeout → sleep");
    enterDeepSleep();
  }

  /* -------- Post-pair idle: sleep 5 s after first link ---------------- */
  if (deviceConnected &&
      !pendingKey &&
      (digitalRead(BUTTON_PIN) == HIGH) &&          // button idle
      (millis() - connStartTime >= CONNECT_IDLE_MS)) {
    LOG("[POWER] idle after pair → sleep");
    enterDeepSleep();
  }

  /* -------- Deferred key (button pressed before pairing) -------------- */
  if (pendingKey && deviceConnected) {
    pendingKey = false;
    sendKey();
    delay(100);                                    // allow host to process
    enterDeepSleep();
  }

  /* -------- Button state machine ------------------------------------- */
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  static bool wasPressed = false;

  if (pressed && !wasPressed) {                    // button went down
    wasPressed = true;
    pressStart = millis();
    LOG("[BTN] down");
    connStartTime = millis();                      // reset idle timer
  }

  if (!pressed && wasPressed) {                    // button went up
    wasPressed = false;
    uint32_t heldMs = millis() - pressStart;
    LOG("[BTN] up after %lu ms", heldMs);
    pressStart = 0;

    if (!factoryResetPending && heldMs < FACTORY_MS) {
      /* Short tap → send F9 (only when linked) */
      if (deviceConnected) {
        sendKey();
        delay(100);
        enterDeepSleep();
      } else {
        LOG("[KEY] waiting for link before sending");
        pendingKey = true;
        setLEDMode(LED_BLINK_BLUE);
        NimBLEDevice::startAdvertising();
        advStartTime = millis();                   // restart 60 s watchdog
      }
    }
  }

  /* -------- Long-press factory reset ---------------------------------- */
  if (pressed && wasPressed && !factoryResetPending) {
    if ((millis() - pressStart) >= FACTORY_MS) {
      factoryResetPending = true;
      doFactoryReset();
    }
  }

  delay(20);                                       // ≈50 Hz loop
}
