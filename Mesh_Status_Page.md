# The Current State of Mesh on Pisces Moon

**Last updated:** May 20, 2026
**Status:** Alpha — proven working, features in active development

---

## TL;DR

Pisces Moon Mesh is a working LoRa-based peer-to-peer messenger that runs on all three Pisces Moon-supported devices (T-Deck Plus, T-LoRa Pager, and M5Stack Cardputer ADV with the Cap LoRa-1262). It uses the same radio settings as the Meshtastic LongFast preset, so Pisces Moon devices can hear Meshtastic packets and Meshtastic devices can hear Pisces packets at the radio level. **However, the two systems do not yet exchange decodable text messages with each other.** Read on for why, what works, and what's coming.

Pisces Moon Mesh started as a proof-of-concept while the rest of the operating system was being built. It is now evolving into completed architecture, but it is still in **alpha**. Features are working, but the feature set is intentionally limited while the foundation hardens.

---

## What works today

**Pisces-to-Pisces messaging on shared channels.** Two or more Pisces Moon devices on the same channel can send and receive text messages over LoRa, peer-to-peer, with no infrastructure. No router, no AP, no cell tower, no internet. This is the core of the system and it is confirmed working across all three supported devices.

**Four channels available:**
- `#LongFast` (channel 0) — the public Meshtastic-compatible lane (see Meshtastic interop section below)
- `#local` (channel 1) — Pisces-exclusive, plaintext, walkie-talkie style
- `#emergency` (channel 2) — Pisces-exclusive, plaintext
- `#pisces` (channel 3) — Pisces-exclusive, plaintext

**Per-device channel switching:**
- T-Deck Plus: Tab key
- T-LoRa Pager: rotary wheel (scroll up = previous channel, scroll down = next channel)
- Cardputer ADV: Tab key

**SD card transcript.** All sent and received messages are saved to `/mesh_logs/messages.csv` on the SD card in WiGLE-friendly CSV format. The transcript persists across sleep, app exit, and reboot. A crash will not lose written messages.

**Heard-node visibility.** When a Pisces device receives an undecodable packet on `#LongFast` (either a real Meshtastic packet whose encrypted payload Pisces cannot yet decrypt, or a packet from a device running incompatible firmware), the Mesh app displays a `~node` system-style row showing that raw or encrypted traffic was heard. This converts silent failures into actionable diagnostics: you can see whether Meshtastic devices are in range even if you can't yet read their messages.

**Meshtastic-compatible packet header on `#LongFast`.** Pisces packets on channel 0 now use the Meshtastic 16-byte header structure (flags, channel hash, nextHop, relayNode fields) with the LongFast channel hash `0x02`. Pisces packets on the LongFast lane look like legitimate Meshtastic packets at the header layer, even though the payload format is still Pisces-specific.

**Concurrent with wardrive on Cardputer ADV.** Despite the Cardputer's no-PSRAM hardware constraints, the v1.2.0 architecture supports running the wardrive app and Mesh Messenger sessions in the same boot session (not simultaneously visible — wardrive is the background scanner, Mesh is the foreground app — but they share the radio and SD bus cleanly via the SPI Bus Treaty).

**Compact input bar on Cardputer.** The 240×135 display uses a two-row input layout: channel/status on the top row, typed text on the bottom row. Long typed text rolls left and shows the newest tail of the buffer, prefixed with a `<` marker when older characters have scrolled off-screen.

---

## What does not work today

**Stock Meshtastic message decode.** Pisces will hear stock Meshtastic packets on `#LongFast` at both the RF and header layers. It cannot yet decrypt the AES-256-CTR encrypted text payloads or parse the Protocol Buffer (protobuf) structure that Meshtastic uses to wrap them. When Pisces hears such a packet, it displays a `~node` row indicating raw or encrypted traffic was heard — you'll know Meshtastic devices are in range, but you won't see their text yet.

**Stock Meshtastic message send.** Pisces' outgoing messages on `#LongFast` use Pisces' plaintext payload format inside the Meshtastic-compatible header. Stock Meshtastic devices will demodulate the RF and recognize the header, but will reject the payload because it isn't AES-encrypted protobuf. They will not display Pisces messages in their text view.

**Position beacons / node info exchange.** Stock Meshtastic devices broadcast periodic position beacons and node-info announcements so the network knows who is where. Pisces Moon does not yet implement these. Other Pisces devices will see Pisces nodes; Meshtastic devices will not learn Pisces nodes exist beyond the heard-packet signal.

**Multi-hop routing.** Stock Meshtastic supports flood-routing messages across multiple hops to reach distant nodes. Pisces Moon currently operates as a single-hop direct-broadcast system. Your message reaches everyone in radio range, but it does not get rebroadcast by intermediate nodes to extend coverage.

**Channel encryption on Pisces-internal channels.** Pisces channels 1-3 (`#local`, `#emergency`, `#pisces`) are plaintext by design. If two Pisces devices on `#local` are within radio range of a third device with a LoRa receiver tuned to the right parameters, the third device can read those messages. This is acceptable for the intended walkie-talkie use case, but should not be confused with the encryption Meshtastic uses on its private channels.

---

## How Pisces Mesh is similar to Meshtastic

**Same radio modulation.** The `#LongFast` channel on Pisces Moon uses the same LoRa physical-layer parameters as Meshtastic's LongFast preset: 906.875 MHz center frequency, BW 250 kHz, SF 11, coding rate 4/5, with the same sync word. At the layer of LoRa physical-layer modulation, Pisces and Meshtastic are interoperable — packets sent by one are demodulated cleanly by the other.

**Same packet header on `#LongFast`.** Pisces uses the 16-byte Meshtastic header structure (flags, channel hash, nextHop, relayNode fields) with the LongFast channel hash `0x02`. At the packet-header layer, Pisces packets on `#LongFast` look like legitimate Meshtastic packets.

**Same regional band.** Pisces Moon uses 902-928 MHz in US deployments (915 MHz LongFast center) and supports configuration for the EU 868 MHz band. This matches Meshtastic's regional band defaults.

**Same kind of mesh philosophy.** Both projects use unlicensed LoRa to provide off-grid peer-to-peer messaging without cellular, WiFi, or internet infrastructure. Both target hobbyist, prepper, hiker, and off-grid communication use cases.

**Same hardware family.** Both run on ESP32-based LoRa-capable devices. Pisces Moon currently supports T-Deck Plus, T-LoRa Pager, and the Cardputer ADV with the Cap LoRa-1262 expansion. Meshtastic supports a wider hardware lineup including dedicated Heltec and LilyGO LoRa boards.

---

## How Pisces Mesh differs from Meshtastic

**Pisces is a full operating system; Meshtastic is a single-purpose firmware.** When you flash a device with Meshtastic, the device becomes a Meshtastic node and does nothing else. When you flash Pisces Moon, you get a multi-app operating system with wardriving, GPS logging, file management, games, an audio recorder, and a Mesh Messenger as one of many apps. The Mesh feature is a piece of a larger handheld computing experience.

**Pisces channels are plaintext walkie-talkie style.** Channels 1-3 in Pisces Mesh do not use the encryption layer that Meshtastic applies to its default channel. This is intentional for the use case Pisces is targeting — quick, low-friction messaging between people who know each other, with no key exchange, no setup, no "did you get the channel invite." Think of it as a digital walkie-talkie for a family, a group of friends, or a small team. Most users do not need protocol-enforced encryption for "are you ready for dinner?"

**Pisces does not yet implement the full Meshtastic protocol.** As of May 20, 2026, the Meshtastic-compatible work in Pisces Moon is limited to radio-layer compatibility and "heard nodes" visibility. Full message decode/encode, node info beacons, and multi-hop routing on the Meshtastic protocol are on the roadmap but not yet implemented.

**Pisces is alpha; Meshtastic is mature.** Meshtastic has been in active development since 2019, has a large community, and ships polished firmware with extensive documentation. Pisces Mesh started as a proof-of-concept and is still hardening. If you need a mature off-grid messenger today, install Meshtastic. If you want to experiment with what a handheld OS can do with a mesh layer integrated into a larger system, Pisces Moon is the project.

---

## When will Pisces and Meshtastic talk to each other?

Codex (one of the AI collaborators working on Pisces Moon alongside Claude) flagged in May 2026 that the right way to handle this is a layered approach. As of v1.2.0:

1. **DONE in v1.2.0:** ensure radio-layer compatibility on `#LongFast` so we can see Meshtastic packets in our area. This included a coding-rate fix from 4/8 to 4/5 to match the actual LongFast preset.
2. **DONE in v1.2.0:** add the Meshtastic 16-byte packet header (flags, channel hash, nextHop, relayNode) so Pisces packets on `#LongFast` are correctly framed for the wire.
3. **DONE in v1.2.0:** add a "heard nodes" surface so users can see undecodable Meshtastic packets and confirm radio settings are correct.
4. **Planned for a future release:** add Meshtastic packet decryption (AES-256-CTR with the well-known LongFast PSK) so we can read Meshtastic text messages.
5. **Planned for a future release:** add Meshtastic protobuf parsing for full structured message handling, then protobuf encoding for sending Meshtastic-compatible messages back.
6. **Planned for a future release:** implement position beacons, node info exchange, and multi-hop routing for full Meshtastic interoperability.

The first three layers shipped in v1.2.0. The remaining layers are real work but they are roadmap work, not blocker work. The walkie-talkie-style Pisces channels are usable today and may be all that some users ever need.

---

## Why we mention Meshtastic at all

Meshtastic is the dominant project in this space and the de facto reference implementation for LoRa mesh messaging on ESP32-class hardware. Honoring radio-layer compatibility with Meshtastic means Pisces Moon users have a path to participating in the wider mesh community as Pisces development progresses. It would be possible to build a Pisces-only mesh that ignores Meshtastic entirely, but that would be choosing isolation over interoperability for no good reason.

Meshtastic is also AGPL-licensed software, like Pisces Moon. Where Pisces Moon's mesh layer takes inspiration from or follows Meshtastic's protocol conventions, credit is given in the project license and source comments. See the Pisces Moon LICENSE document for details.

---

## How to test Mesh on Pisces Moon today

1. Flash Pisces Moon OS v1.2.0 onto two or more supported devices (T-Deck Plus, T-LoRa Pager, or Cardputer ADV with Cap LoRa-1262).
2. Open the Mesh Messenger app from the launcher.
3. Both devices default to `#LongFast` (channel 0).
4. Type a message on one device, press Enter to send.
5. The message should appear on the other device within 1-2 seconds.
6. Switch channels with:
   - **Tab key** on T-Deck Plus and Cardputer ADV
   - **Rotary wheel** on T-LoRa Pager (scroll up = previous, scroll down = next)

If you have a stock Meshtastic device, send a message from it on the default LongFast channel. You should see a `~node` row appear in the Pisces Moon Mesh view indicating raw or encrypted traffic was heard. The message content will not display because Pisces cannot yet decrypt or parse Meshtastic-format payloads — but the `~node` row confirms your radio settings are correct and Meshtastic traffic is reaching you.

---

## In summary

Pisces Mesh is a real, working, peer-to-peer LoRa messenger. It runs on three different devices. It writes durable transcripts to SD card. It is part of a larger handheld operating system, not a single-purpose firmware. It shares the LongFast radio settings with Meshtastic but does not yet share the application-layer protocol that would make full message interop possible.

We are honest about what works and what does not. The walkie-talkie use case is solid today. The full Meshtastic interop story is in active development. Watch the changelog at fluidfortune.com for updates as the layers come online.

---

**Pisces Moon OS — Fluid Fortune — fluidfortune.com**
**AGPL-3.0-or-later**

*This page will be updated as Pisces Mesh features evolve. The date at the top reflects the most recent revision.*
