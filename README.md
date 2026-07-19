# OBD2 Dial

M5Stack Dial as a BLE OBD2 display: connects to a BLE ELM327 dongle, shows live gauges, reads and clears fault codes. Built for VW-group cars (Skoda Yeti, Seat Ibiza) but uses only standard OBD2.

## Use

**First run** — the Dial boots to the BLE SCAN page: plug the dongle into the car, tap to scan, rotate to highlight your dongle (OBD-looking devices are marked `*`), tap to choose it. The choice is remembered; from then on the Dial auto-connects to that dongle on every boot, until you pick a different one from the same page.

**Daily use:**

- **Rotate** to switch pages: RPM, Speed, Coolant, Battery, Intake, Load, Codes, BLE Scan.
- **CODES page**: tap for the menu — *Fault codes* (read + scroll, e.g. `P0301 — Cyl 1 misfire`), *Clear faults* (red confirm screen, tap to confirm), *Service reset* (VW-specific, not implemented yet — needs TP2.0, not standard OBD2). Hold to exit the menu.
- **Connect screen**: tap to skip into demo mode (browse the UI without a car; values show `--`).
- Ignition off / dongle out of range → gauges show `--` and the Dial keeps retrying.

## Build & flash

```sh
pio run -t upload        # build + flash over USB-C
pio device monitor       # serial log
```

Host test for the response parser:

```sh
cd test/host && g++ -std=c++11 -I../../src parse_check.cpp ../../src/parse.cpp -o parse_check && ./parse_check
```

## Hardware notes

- The M5Dial library's encoder driver never gets interrupts (its ESP32 pin table stops at GPIO39; the encoder is on 40/41) — `src/main.cpp` counts quadrature edges with its own ISR instead.
- NimBLE's default 30s connect timeout freezes a single-loop UI; it's capped at 5s.
- Dongle must be **BLE** (ESP32-S3 has no Bluetooth Classic). Known good: Vgate iCar Pro BLE.

Design notes: `docs/superpowers/specs/2026-07-18-obd2-dial-design.md`
