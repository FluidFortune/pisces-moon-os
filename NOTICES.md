# Pisces Moon OS — Third-Party Notices and Attributions

Pisces Moon OS is licensed under the GNU Affero General Public License,
version 3 or (at your option) any later version. The full text of the
AGPL is included in the LICENSE file in the root of this repository.

This NOTICES file acknowledges third-party software, protocols, and
projects that Pisces Moon either incorporates, derives architectural
inspiration from, or interoperates with at the wire-protocol level.

---

## Meshtastic

Pisces Moon's Mesh Messenger app interoperates at the LoRa radio layer
with the Meshtastic project (https://meshtastic.org). Specifically,
Pisces Moon's `#LongFast` channel uses the same LoRa physical-layer
settings as Meshtastic's LongFast preset (spreading factor, bandwidth,
coding rate, sync word, and frequency band) to enable cross-project
radio-layer compatibility.

Meshtastic is open-source software licensed under the GNU General
Public License, version 3. The Meshtastic protocol, packet header
format, and channel-naming conventions referenced in Pisces Moon's
Mesh Messenger documentation are designed by the Meshtastic project
and its contributors.

Pisces Moon does not include Meshtastic source code, firmware, or
binaries. Pisces Moon is an independent implementation that adopts
the LoRa physical-layer parameters of Meshtastic's LongFast preset
for interoperability purposes. Where Pisces Moon's documentation
discusses Meshtastic protocol behavior (such as the use of the
default-channel pre-shared key for LongFast payload encryption),
this is descriptive of Meshtastic's published protocol, not derived
code.

Pisces Moon is not affiliated with, endorsed by, or sponsored by
the Meshtastic project. Trademarks referenced in this notice belong
to their respective owners.

For the Meshtastic project, source, and documentation, see:
- https://meshtastic.org
- https://github.com/meshtastic

---

## Espressif ESP-IDF and Arduino-ESP32

Pisces Moon is built on the Espressif ESP-IDF framework, distributed
under the Apache License 2.0, and the Arduino-ESP32 core, distributed
under the GNU Lesser General Public License version 2.1.

- https://github.com/espressif/esp-idf
- https://github.com/espressif/arduino-esp32

---

## RadioLib

Pisces Moon uses the RadioLib library for SX1262 LoRa radio control,
distributed under the MIT License.

- https://github.com/jgromes/RadioLib

---

## NimBLE-Arduino

Pisces Moon uses NimBLE-Arduino for Bluetooth Low Energy observer
functionality in the wardrive app, distributed under the Apache
License 2.0.

- https://github.com/h2zero/NimBLE-Arduino

---

## SdFat

Pisces Moon uses the SdFat library for SD card access, distributed
under the MIT License.

- https://github.com/greiman/SdFat

---

## TinyGPSPlus

Pisces Moon uses the TinyGPSPlus library for NMEA parsing,
distributed under the GNU Lesser General Public License version 2.1.

- https://github.com/mikalhart/TinyGPSPlus

---

## Arduino_GFX

Pisces Moon uses the Arduino_GFX library for display rendering on
T-Deck Plus, T-LoRa Pager, and Cardputer ADV, distributed under
the BSD 3-Clause License.

- https://github.com/moononournation/Arduino_GFX

---

## M5Cardputer / M5Unified

Pisces Moon uses the M5Cardputer and M5Unified libraries for
Cardputer ADV keyboard and unified peripheral access, distributed
under the MIT License.

- https://github.com/m5stack/M5Cardputer
- https://github.com/m5stack/M5Unified

---

## ArduinoJson

Pisces Moon uses the ArduinoJson library for JSON parsing and
generation, distributed under the MIT License.

- https://github.com/bblanchon/ArduinoJson

---

## Hardware acknowledgments

Pisces Moon supports the following hardware. These manufacturers
are not affiliated with Pisces Moon and their inclusion in this
notice is for hardware support documentation only:

- LilyGO T-Deck Plus and T-LoRa Pager (LilyGO, https://lilygo.cc)
- M5Stack Cardputer ADV and Cap LoRa-1262 (M5Stack, https://m5stack.com)
- ELECROW CrowPanel Advanced 7" (ELECROW, https://elecrow.com)

---

This NOTICES file is non-exhaustive. The Pisces Moon source code
includes additional credit comments in individual files where
specific implementations are inspired by or adapted from third-party
work. All such adapted work is compatible with the AGPL-3.0-or-later
license under which Pisces Moon is distributed.

---

**Pisces Moon OS — Fluid Fortune — fluidfortune.com**
**AGPL-3.0-or-later**
