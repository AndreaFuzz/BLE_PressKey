# Hassinator – Always-On Bluetooth Keyboard (F9)

Ultra-small ESP32-C3 gadget that pairs as a BLE HID keyboard and fires a **single F9** key-press when you tap its lone button, then slips into deep-sleep to sip just a few µA.

---

## Table of Contents
1. [Why F9?](#why-f9)
2. [Tested Hardware](#tested-hardware)
3. [Feature Matrix](#feature-matrix)
4. [Wiring](#wiring)
5. [Configurable Variables](#configurable-variables)
6. [Build & Flash](#build--flash)
7. [Power-Saving Tips](#power-saving-tips)
8. [Usage Walk-Through](#usage-walk-through)
9. [How It Works](#how-it-works)
10. [Troubleshooting](#troubleshooting)
11. [License](#license)

---

## Why F9?
F9 is one of the few function keys that is **not mapped to anything by default** on Windows, macOS, or mainstream Linux desktops.  
If you need a different key, simply change `KEY_USAGE` (see below).

### Hot-key ideas  

| Platform | What to do with F9 |
|----------|-------------------|
| Windows 10/11 | Use *PowerToys → Keyboard Manager → Remap a key* to launch scripts or apps. |
| macOS | *System Settings → Keyboard → Keyboard Shortcuts* → bind F9 to a menu command or Automator workflow. |
| Linux DEs | Most environments expose *Settings → Keyboard → Shortcuts* for custom bindings. |

---

## Tested Hardware
| Item | Notes |
|------|-------|
| **Board** | ESP32-C3 Super Mini |
| **Button** | Momentary switch between **GPIO 4** and **GND** (also deep-sleep wake) |
| **LED** | Single WS2812 on-board, **GPIO 8** |

A CR2032 powers it for weeks thanks to deep-sleep current ≈ 5–6 µA (see tips).

---

## Feature Matrix
| Action | LED feedback | Result |
|--------|--------------|--------|
| Short press (< 3 s) | Half-white flash | Sends F9 then deep-sleep |
| Long press (≥ 3 s) | Solid red for 2 s | Factory-resets Bluetooth bonds, resumes advertising |
| Advertising | Blinks blue | Waiting for host to pair |
| Connected | Solid green | Ready for button press |
| 60 s unpaired | — | Auto-sleep to save battery |
| 5 s after first pair & idle | — | Auto-sleep if user never pressed button |

---

## Wiring

ESP32-C3 ──> Button ──> GND
GPIO 4

ESP32-C3 ──> WS2812 (on-board)
GPIO 8


*(Board already has pull-ups for GPIO 4 and the LED.)*

---

## Configurable Variables
Edit **`BLE_PressKey.ino`** (top of file):

```
static const char*  DEVICE_NAME = "Hassinator"; // BLE name shown to hosts
static const uint8_t KEY_USAGE  = 0x42;         // 0x42 = F9 (USB HID Usage Tbl.)
```

* Pick another usage from the official table: <https://usb.org/sites/default/files/hut1_4.pdf> (see “Keyboard/Keypad Page”, p.53 ff).

---

## Build & Flash

### 1  Install Arduino IDE ≥ 2.2 (or Arduino Studio)
<https://arduino.cc/en/software>

### 2  Add the ESP32 board support

* **Tools → Board → Boards Manager** → install **esp32 by espressif v3.2.0** or newer.

### 3  Libraries
| Library | Version tested |
|---------|----------------|
| NimBLE-Arduino | 2.2.3 |
| Adafruit_NeoPixel | 1.12.5 |

Install via **Tools → Manage Libraries**.

### 4  Board Options
* **Board:** *ESP32C3 Dev Module*  
* **USB CDC On Boot:** *Enabled* (optional for serial logs)

### 5  Compile & Upload

hit **Upload** in the IDE.

---

## Power-Saving Tips
1. **Snip the LED series resistor**  
   The Super Mini leaves the WS2812 data line held HIGH via a ~330 Ω resistor, leaking ≈ 300 µA. Removing it drops sleep current dramatically; the LED still works when awake.
2. **Disable debug logging**  
   Set `DEBUG_ENABLED` to `0` (default). This powers down the UART block after boot, saving 5–10 mA when active.
3. **Lower TX power**  
   Inside `setupBLE()` you may choose a lower `ESP_PWR_LVL_Nx` if the host is always close.

Combined tweaks bring deep-sleep to ≈ 5 µA on a good CR2032.

---

## Usage Walk-Through
1. **Power-on** → LED blinks blue (advertising).  
2. **Pair** from your phone/PC; look for `DEVICE_NAME`.  
3. LED turns **green**.  
4. **Tap** the button → half-white flash → *F9* sent → deep-sleep.  
5. **Hold 3 s** → LED red, bonds cleared, advertising restarts.

---

## How It Works (quick reference)
| Module | Responsibility |
|--------|----------------|
| **NimBLE-Arduino** | BLE stack, GATT server, HID service |
| **HID report map** | Defines a minimal 6-key-rollover keyboard (Report ID 1) |
| **Power watchdogs** | 60 s timer during advertising, 5 s idle timer after first link |
| **GPIO wakeup** | Deep-sleep resumes when button pulls GPIO 4 LOW |
| **LED state machine** | 5 modes (off, blink blue, solid green, solid red, half-white) |

---

## Troubleshooting
| Symptom | Check |
|---------|-------|
| Board never appears in Bluetooth scan | *Power:* CR2032 fresh? &nbsp;*Code:* Is `setupBLE()` called? &nbsp;*LED:* blue blink? |
| Key not received on host | Ensure host supports BLE HID (most do). Use a BLE scanner to confirm connection establishes. |
| Battery drains quickly | Remove LED resistor, disable debug, verify board actually enters deep-sleep (current < 10 µA). |
| Need a different key | Change `KEY_USAGE` and re-flash. Use HID Usage Table link above. |

---

## License
MIT © 2025 [AndreaFuzz](https://github.com/AndreaFuzz)  
Feel free to fork, tweak, and share.
