# Pisces Moon OS - Technical Requirements Document (TRD)
**Version:** Alpha 0.2
**Project Lead:** Eric
**Architecture Target:** Hardware-Agnostic / Multi-Platform

## 1. Project Philosophy & Vision
Pisces Moon OS is a custom, highly optimized, hacker-centric operating system. It is designed to bridge the gap between low-power microcontrollers (ESP32) and lightweight x86 architecture (Alpine Linux). 
* **The "Run Anywhere" Mandate:** The OS architecture must be scalable. It should boot on dedicated maker hardware (LilyGo T-Deck), portable pocket devices (T-Embed), x86 tablets (Fujitsu Q508), and potentially locked consumer hardware (Amazon Fire Tablets) via bootloader bypasses or sideloaded environments.
* **UI/UX Paradigm:** The interface relies on a "Retro-Cyberdeck" aesthetic, prioritizing high-contrast visuals, immediate feedback, and rapid task-switching over heavy graphical animations.

## 2. Hardware Profiles & Constraints
The OS must dynamically adapt to the physical constraints of its host hardware.

### Profile A: LilyGo T-Deck Plus (ESP32-S3 N8R8)
* **Memory:** 8MB Octal PSRAM (Must be initialized via `qio_opi`).
* **Display Constraint (The Bezel Factor):** Hardware LCD is 320x240. The physical plastic enclosure obscures the bottom ~18 pixels. All UI rendering MUST be constrained to a visible "Safe Zone" of 320x222.
* **Input:** GT911 Capacitive Touch. Requires dynamic NVS (Preferences.h) multi-point matrix calibration to counteract digitizer drift and factory initialization flaws.
* **Bus Conflicts:** LoRa (SX1262) and MicroSD share the SPI bus. LoRa CS (Pin 9) must be pulled HIGH immediately on boot to prevent SD mounting failures. I2C Bus (SDA 18 / SCL 17) requires strict "Safety Guards" to prevent infinite polling loops if the AXP2101 PMU or Keyboard co-processor hang.

### Profile B: Fujitsu Stylistic Q508 (x86 Tablet)
* **OS Foundation:** Alpine Linux (chosen for `musl libc` efficiency and low overhead).
* **Display:** 10.1" WUXGA. Requires scaling translation from the 320x222 T-Deck standard to native tablet resolution using SDL2 or Python/Qt.
* **Input:** Wacom Digitizer (`xf86-input-wacom`).
* **Networking:** Requires user-space SPI drivers (e.g., via FT232H USB-to-SPI bridge) to interface with external LoRa modules, mimicking the ESP32 hardware architecture on x86.

## 3. Core Software Subsystems

### 3.1 Display & Calibration Engine
* **Dynamic Matrix:** The OS must not rely on hardcoded pixel offsets for touch. It must calculate an Affine Transformation matrix via user-prompted crosshairs and store the min/max X and Y values in Non-Volatile Storage (NVS).
* **Delta-Drawing (Anti-Strobe):** To prevent full-screen flashing, touch feedback (e.g., the red tracking dot) must use differential drawing (erasing the previous coordinate and drawing the new one) without triggering a full UI refresh.

### 3.2 The WarDrive Logger (Background Tasking)
* **Concurrency:** On ESP32 systems, the WarDrive subsystem must operate on Core 0 (background) via `TaskHandle_t`, allowing the UI to remain responsive at 60Hz on Core 1.
* **Data Capture:** Must passively log SSID, BSSID, RSSI, and Encryption Type.
* **Storage:** Data must be formatted and appended to a `.csv` or `.wigle` file on the mounted SD Card asynchronously.

### 3.3 Power Management (PMU)
* **Fail-Safe Polling:** The OS must detect I2C availability. If the PMU chip (e.g., AXP2101) fails to answer a `Wire.beginTransmission` heartbeat, the OS must flag `pmu_online = false` and cease all battery percentage polling to prevent CPU blocking.

## 4. Development & Deployment Guidelines
* **Single Source of Truth:** This document, alongside hardware schematics, serves as the definitive architecture guide for AI-assisted generation in NotebookLM.
* **Dependency Management:** Upstream libraries (e.g., TFT_eSPI) that override manufacturer-specific initialization sequences (e.g., LilyGo's July 2024 ST7789 patch) must be manually overridden or bypassed entirely in the codebase.