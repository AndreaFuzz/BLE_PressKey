# Hassinator – Always-On Bluetooth Keyboard (F9)

Ultra-small ESP32-C3 gadget that pairs as a BLE HID keyboard and sends **F9** (configurable) when you press a single button, then drops into deep sleep to save power.

---

## Why F9?

F9 is one of the few function keys that is **not mapped to anything by default** on Windows, macOS or mainstream Linux desktops, so it is normally safe to use.  
If you want another key, change `KEY_USAGE` (see “Configurable Variables”).

### Hot-key ideas

* **Windows 10/11:** Use *PowerToys → Keyboard Manager → Remap a key* to launch a program or run a shortcut when F9 is received.  
* **macOS:** Use *System Settings → Keyboard → Keyboard Shortcuts → App Shortcuts* and bind F9 to a menu item.  
* **Linux:** Most desktop environments let you bind actions in *Settings → Keyboard → Shortcuts*.

---

## Tested Hardware

* **Board:** ESP32-C3 Super Mini  
  On-board WS2812 LED on **GPIO 8**.  
* **Button:** Momentary switch between **GPIO 4** and **GND**  
  GPIO 4 doubles as the deep-sleep wake-up pin.

Runs for weeks on a CR2032 coin cell.

---

## Quick Features

| Action                       | Result                                  |
| ---------------------------- | --------------------------------------- |
| Short press (< 3 s)          | Sends F9 → LED half-white → deep sleep  |
| Long press (≥ 3 s)           | Factory reset → LED solid red           |
| Advertising (not paired)     | LED blinks blue                         |
| Connected                    | LED solid green                         |

---

## Wiring

```
ESP32-C3  ──>  Button  ──>  GND
GPIO 4

ESP32-C3  ──>  WS2812 single LED (on-board)
GPIO 8
```

---

## Configurable Variables (top of `BLE_PressKey.ino`)

```
static const char*  DEVICE_NAME = "Hassinator"; // rename here
static const uint8_t KEY_USAGE  = 0x42;         // 0x42 = F9
```

* Full HID usage table (to pick another key): <https://usb.org/sites/default/files/hut1_4.pdf> (page 53 onward).

---

## Development Environment

### 1  Install Arduino IDE / Arduino Studio

* Download Arduino IDE ≥ 2.2 from <https://arduino.cc>.
* If you prefer *Arduino Studio* (the alternative UI), install it; menus are identical.

### 2  Add the ESP32 core

1. Open **Preferences**  
2. Paste this URL into *Additional Boards Manager URLs*:  
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Open **Tools → Board → Boards Manager**, search *esp32*, install version **3.2.0** or newer.

### 3  Install libraries

* **Sketch → Include Library → Manage Libraries…**

| Library            | Version tested |
| ------------------ | -------------- |
| NimBLE-Arduino     | 2.2.3          |
| Adafruit NeoPixel  | latest         |

### 4  Board & Port

* **Tools → Board → ESP32C3 Dev Module**
* **Tools → USB CDC On Boot:** *Enabled* (optional easier serial)
* Plug the board → Select the COM port under **Tools → Port**.

### 5  Compile & Upload

```
arduino-cli compile --fqbn esp32:esp32:esp32c3
arduino-cli upload   --fqbn esp32:esp32:esp32c3 -p COM5
```

Or just click **Upload → Serial Monitor** inside the IDE.

---

## Power-Saving Tips

1. **Remove the LED series resistor**  
   The Super Mini holds the WS2812 data line HIGH via a ~330 Ω resistor, leaking ~300 µA. Snip or desolder it; the LED still works while awake.

2. **Disable serial logging**  
   Set `DEBUG_ENABLED` to `0`. Avoids powering the UART block after boot and saves 5–10 mA.

With both tweaks the sleep current drops to ≈ 5–6 µA.

---

## Usage

1. Power up → LED blinks blue.  
2. Pair from your host; device name is `DEVICE_NAME`.  
3. LED turns green when connected.  
4. Tap the button → key sent → deep sleep.  
5. Hold the button 3 s → bonds cleared → advertising restarts.

---

## License

MIT — see [LICENSE](LICENSE).  
Copyright © 2025 [AndreaFuzz](https://github.com/AndreaFuzz/)
