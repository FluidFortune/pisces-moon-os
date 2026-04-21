# Six Problems That Didn't Exist Until We Built Something Complex Enough to Find Them

*On the engineering of Pisces Moon OS — and what it means when your crash dump points nowhere near the cause.*

---

There is a category of engineering problem that is particularly difficult to solve: the problem that does not exist in the documentation because nobody has ever built anything complex enough to trigger it.

These are not obscure problems. They are not edge cases in the theoretical sense. They are problems that were always present in the hardware, waiting. They simply required a project sufficiently complex — running enough subsystems simultaneously, under enough real-world load — to surface them.

Building the first known documented dual-core persistent background tasking OS for field intelligence on the ESP32-S3 hardware class meant running that hardware harder than it had ever been run. The Ghost Engine collecting wardrive data on Core 0 while the AI terminal runs on Core 1 while the LoRa radio handles mesh communications while the GPS maintains a fix while the display renders at 60 frames per second — this is not what the hardware was designed for, in the sense that no previous project had attempted it. The hardware was always capable. Nobody had asked it to do all of this at once.

Six engineering problems emerged from that complexity. None had documented solutions for this platform before this project encountered and solved them. What follows is an account of each: what the symptom was, why it was difficult to diagnose, and what the solution turned out to be.

---

## Problem 1: The SPI Bus Conflict

**The symptom:** The device rebooted without warning during normal operation. The crash dump — the ESP32-S3's equivalent of a blue screen — pointed at a memory address that changed on every reboot. No consistent error. No reproducible cause. Just: running fine, then not running.

**Why it was hard:** The crash dump tells you where the crash occurred in the code. It does not tell you why the crash occurred in the hardware. When the address changes every time, a developer reading the dump sees what appears to be a random memory fault in a different module on each occurrence. The symptom points nowhere near the cause.

**The actual cause:** The MicroSD card and the SX1262 LoRa radio module share the ESP32-S3's SPI bus — the same physical wires for data transmission. They use separate chip-select signals, but the underlying MOSI, MISO, and CLK lines are shared. Only one device may transmit at any given moment.

In single-function firmware, this constraint is invisible. If the firmware only uses the SD card, the LoRa radio is idle and there is nothing to collide. In a general-purpose OS running the Ghost Engine — which writes wardrive data to the SD card continuously — and the LoRa mesh radio simultaneously, both devices compete for the same lines at the same microsecond. The result is a hardware fatal exception.

This problem had no documented solution for this hardware because no previous ESP32-S3 project had operated both devices under simultaneous sustained load. The problem could not have been found without first building something complex enough to trigger it.

**The solution:** The SPI Bus Treaty — a formal behavioral protocol governing every component of the operating system and every third-party ELF module developed for the platform.

Four rules:

*Hit and run.* All SD card operations follow the pattern: open file, write data, close file, release bus immediately. No component may hold an SD card file open across multiple operations or across a time delay.

*No extended holds.* No operation may hold the SPI bus for extended periods. This explicitly prohibits on-the-fly file encryption during write operations, in-place editing of large files, and SD card formatting during normal operation.

*Radio traffic management.* A shared boolean flag signals when the WiFi radio is in use for an HTTP request. The wardriving task checks this flag before initiating a scan, preventing the Ghost Engine from switching the radio to scan mode while the AI terminal is mid-request.

*Metadata-only destructive operations.* The Nuke security function cannot use SD card format operations — those hold the bus for several seconds. It deletes only the index files the OS uses to locate data. A metadata delete takes milliseconds. A format takes seconds. The Treaty dictates the former.

The SPI Bus Treaty belongs to the same architectural class as Unix filesystem locking conventions from the 1970s, the Apollo Guidance Computer's priority scheduling protocol from 1969, and Nintendo's N64 RSP time budget from 1996 — all cases where competing subsystems sharing a hardware resource required formal behavioral rules rather than hardware redesign. It is the first documented instance of this solution class applied to the ESP32-S3 SPI bus architecture.

---

## Problem 2: Memory Exhaustion Under Simultaneous Workloads

**The symptom:** Crashes in different subsystems — the wardriving engine, the AI client, the BLE scanner — all appearing as different bugs with different error signatures. No consistent pattern. Each crash looked like an isolated problem in its own module.

**Why it was hard:** When SRAM runs out, the failure doesn't announce itself as "out of memory." It manifests wherever the next allocation fails — which could be anywhere in the codebase. The crash in the AI client and the crash in the wardriving engine looked like two different bugs. They were the same bug.

**The actual cause:** The ESP32-S3 has 320 kilobytes of fast internal SRAM. For single-function firmware, this is generous. For a general-purpose OS running a wardriving engine scanning 80 WiFi access points simultaneously, a Gemini AI client parsing large JSON responses, a BLE scanner tracking 100+ devices in a dense urban environment, a GPS reader, a LoRa mesh radio stack, and a 60fps user interface simultaneously — 320 kilobytes is not enough.

The device has 8 megabytes of additional PSRAM — external RAM connected over a high-speed bus. This is the solution. The problem: the default configuration routes all heap allocations to internal SRAM. PSRAM exists on the hardware but is not automatically used.

No previous ESP32-S3 project had generated workloads complex enough to exhaust the internal SRAM heap. The problem did not appear in the documentation because it had not appeared in practice.

**The solution:** A single compiler flag — `-DCONFIG_SPIRAM_USE_MALLOC=1` — instructs the ESP32's memory management layer to automatically redirect heap allocations above a size threshold to PSRAM. Internal SRAM is reserved for small, fast, time-critical allocations. Everything else goes to the 8MB PSRAM pool transparently, without requiring any changes to application or library code.

The flag exists in Espressif's SDK documentation. The contribution is not discovering it — it is being the first to build something that required it, recognizing that SRAM exhaustion was the failure mode across apparently unrelated crashes, and applying the solution in a general-purpose OS context where multiple libraries and subsystems share the heap simultaneously.

---

## Problem 3: Dual-Core Task Synchronization

**The symptom:** The hardware watchdog timer fired, rebooting the device, from what appeared to be a random location in the code — sometimes the system timer interrupt, sometimes a display refresh function, sometimes an unrelated application module. The crash appeared random. It was not random.

**Why it was hard:** The failure is intermittent and context-dependent. In a bench environment with a single nearby WiFi network, the Ghost Engine writes to the SD card infrequently — the collision window rarely opens. In downtown Los Angeles with 80+ access points in range, the Ghost Engine writes multiple times per second. The collision becomes near-certain during any concurrent Core 1 SD operation.

The crash appeared in random locations because the heap corruption — caused by the collision — propagated to whatever data structure the heap walker happened to be traversing when it reached the corrupted entry. The symptom pointed nowhere near the cause. Identifying the root cause required correlating crash frequency with WiFi network density across multiple field sessions.

**The actual cause:** Core 0 (Ghost Engine writing wardrive CSV to SD card) and Core 1 (file browser or system statistics read accessing SD card) collided within the same narrow timing window. The collision corrupted SdFat's internal linked list data structures. The heap walker subsequently traversed the corrupted list, entered an infinite loop, and triggered the Core 1 hardware watchdog timer.

**The solution:** Two mechanisms working in concert.

A FreeRTOS mutex — created before either core starts, ensuring both cores have a valid handle before any SD access occurs. Every SD write in the Ghost Engine and every SD read in any Core 1 application acquires the mutex with a timeout, executes the operation, and releases immediately. The cores cannot execute SD operations simultaneously.

A wifi_in_use boolean flag addressing the equivalent race condition on the WiFi radio. The Ghost Engine's scanner checks this flag before switching the radio to scan mode. The AI client sets it true before initiating a request and clears it on completion.

Validated under sustained operation in downtown Los Angeles: 40+ simultaneous WiFi access points, continuous Ghost Engine logging, concurrent Core 1 application use. Zero crashes attributable to dual-core conflict after implementation.

---

## Problem 4: Dense RF Environment Instability

**The symptom:** Three apparently unrelated symptoms — GPS timeouts, intermittent display corruption, and random reboots — appearing together in high-density urban environments but not in the lab. Each looked like a different bug.

**Why it was hard:** This problem cannot be discovered in a laboratory. The bench test environment never exceeds twenty nearby Bluetooth devices. Downtown Los Angeles routinely exceeds one hundred. A bug that only manifests in real-world field deployment, and manifests as apparently random crashes whose symptoms point nowhere near the actual cause, requires field testing to discover and careful cross-session correlation to diagnose.

The GPS timeout and display corruption were both consequences of stack overflow corruption propagating to unrelated data structures. Neither was the primary failure.

**The actual cause:** BLE advertisement callbacks run in interrupt service routine context, which has a fixed, small stack. In a dense RF environment — 150 or more devices simultaneously advertising — callbacks fire faster than the system can process them. Stack overflow. The overflow corrupts whatever memory happens to be adjacent to the stack: a GPS sentence buffer, a display refresh counter, a SdFat internal state variable. The system crashes in a way that appears completely unrelated to its actual cause.

**The solution:** Three fixes, all required simultaneously. Removing any one restores the crash condition.

BLE task stack size increased to accommodate the maximum expected concurrent callback depth under real-world urban RF density.

Scan result buffer given a hard size limit. When the buffer is full, new scan results are discarded rather than processed. The system stops accepting data it cannot handle rather than attempting to process an unbounded stream.

GPS UART receive buffer increased from 128 bytes to 512 bytes, with explicit drain loops added immediately before and after the blocking WiFi scan call. During the scan — which blocks Core 1 for 2-4 seconds — the GPS module outputs NMEA sentences that were filling and overflowing the small buffer, causing GPS fix loss.

Validated in downtown Los Angeles: sustained operation, no crashes, GPS fix maintained, BLE and WiFi scanning continuous.

---

## Problem 5: Security Architecture Under Hardware Timing Constraints

**The symptom:** The device crashed during the Nuke sequence — the security function designed to render sensitive Ghost Partition data permanently unreachable in a panic scenario.

**Why it was hard:** This is not a solvable problem within its original framing. The standard engineering solution for securing data at rest is AES encryption. AES encryption operations hold the SPI bus for extended periods during write operations. Extended SPI bus holds trigger LoRa radio interrupt collisions — Problem 1. The most widely deployed data security tool available causes the device to crash under the constraints of this hardware architecture.

If the requirement is "encrypt all Ghost Partition data at rest using AES," and AES write operations violate the SPI Bus Treaty, and Treaty violations cause system crashes, then the requirement and the hardware constraint are irreconcilable. No amount of implementation optimization resolves this — it is a structural conflict.

**The solution:** Reframe the requirement.

The question is not "how do we encrypt the data." The question is "what does the adversary actually need to access the data, and how do we eliminate that without violating the bus timing constraint."

The OS maintains index files that describe the structure and location of all data in the Ghost Partition. These index files are small — kilobytes, not megabytes — and deleting them takes milliseconds: a single small write operation that completes within the SPI Bus Treaty's hold budget. Without the index files, all Ghost Partition data is permanently unreachable through any Pisces Moon OS interface. The raw bytes remain on the SD card, but there is no navigable path to them.

Against the realistic threat model — casual inspection, card reader forensics without specialized equipment, time-constrained seizure scenarios — this provides complete protection. Against a Tier 3 adversary with laboratory-grade sector analysis capability and significant analysis time, it provides meaningful delay. The threat model is documented explicitly and honestly in the project documentation.

A device that crashes attempting to encrypt data is less secure than a device that deletes index files in milliseconds and returns to normal operation. The Nuke function must work to be useful.

---

## Problem 6: GPS Module Hardware Variation

**The symptom:** GPS does not work. No error output. The code compiles without warnings. It executes without exceptions. The GPS simply produces no useful result.

**Why it was hard:** Silent failure without error output is the hardest category of embedded hardware problem to diagnose. In the absence of an error message, a developer must eliminate every other possible cause before arriving at the hardware variation hypothesis — a hypothesis that is difficult to generate without prior knowledge that this type of manufacturing variation occurs.

**The actual cause:** LilyGO manufactured the T-Deck Plus with two different GPS modules across production batches. Both modules are physically compatible with the same board footprint. Both output standard NMEA sentences. They operate at different UART baud rates. Code that correctly initializes the GPS on one batch fails silently on the other.

The variation is undocumented in LilyGO's official product documentation. It is not noted in any community forum. It is not visible from physical inspection of the hardware.

**The solution:** Baud rate auto-detection. An auto-detection system attempts GPS initialization at each supported baud rate in sequence, checks for valid NMEA sentence structure at each rate, and latches to the first configuration that produces valid output. The device determines which GPS module variant it has by attempting to communicate with it in both dialects and using whichever one responds correctly.

This is the first documented solution to the T-Deck Plus GPS baud rate variation problem. It was discovered and implemented because a production device from a different manufacturing batch than the development device failed to acquire GPS fix for reasons that had no other explanation after all software causes were eliminated.

---

## What These Six Problems Have In Common

All six were always present in the hardware. They were discovered because this project was the first software on this hardware class complex enough to trigger them.

The SPI Bus Treaty, the PSRAM heap redirect, the dual-core mutex, the BLE stack sizing, the metadata deletion security architecture, the GPS auto-detection — all of these are now in the public record. Any developer who builds general-purpose software for the ESP32-S3 and runs these subsystems simultaneously starts from documented solutions rather than rediscovering the problems from scratch.

That is the point of publishing them. Not to claim credit for cleverness. To make the next build easier for whoever comes next.

The solutions are in the repository. The repository is public. The code is AGPL-3.0.

---

*Pisces Moon OS: [github.com/FluidFortune/pisces-moon-os](https://github.com/FluidFortune/pisces-moon-os)*

*The Sovereignty White Paper: [fluidfortune.com/sovereignty.html](https://fluidfortune.com/sovereignty.html)*

*Next in this series: The Philosophy — why computing sovereignty matters more than any feature a cloud vendor can offer.*
