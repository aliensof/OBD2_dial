# OBD2 Dial

M5Stack Dial as a BLE OBD2 display: connects to a BLE ELM327 dongle, shows live gauges, reads and clears fault codes. Built for VW-group cars (Skoda Yeti, Seat Ibiza) but uses only standard OBD2.

## Use

**First run** — the Dial boots to the BLE SCAN page: plug the dongle into the car, tap to scan, rotate to highlight your dongle (OBD-looking devices are marked `*`), tap to choose it. The choice is remembered; from then on the Dial auto-connects to that dongle on every boot, until you pick a different one from the same page.

**Daily use:**

- **Rotate** to switch pages: RPM, Speed, Coolant, Intake, Load, Throttle, MAP, MAF, Short/Long fuel trim, Timing, Fuel level, Ambient, Run time, Distance-with-MIL, then Battery, Codes, BLE Scan. The gauge pages are filtered to the PIDs your car actually answers (asked once per connect via `0100`/`0120`/`0140`), so the rotation only holds pages that work.
- **CODES page**: tap for the menu — *Fault codes* (mode 03, read + scroll, e.g. `P0301 — Cyl 1 misfire`), *Pending codes* (mode 07, faults not yet confirmed by the MIL), *Freeze frame* (mode 02 — the sensor snapshot recorded when the fault stored), *Clear faults* (red confirm screen, tap to confirm), *Service reset* (VW-specific, not implemented yet — needs TP2.0, not standard OBD2). Hold to exit the menu.
- **Peaks**: each gauge shows its session high under the unit (`max 187`), or both ends for signed values like fuel trim (`-4 / 12`). Hold on any gauge to reset them; they also reset on reconnect.
- **Alarms**: coolant and battery voltage are polled in the background every 5s whatever page you're on. Out of range → three beeps and a full-screen warning, repeated at most once a minute. Thresholds are in `kWatch` in `src/main.cpp` — tune them on the car.
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
- Dongle must be **BLE** (ESP32-S3 has no Bluetooth Classic) — Classic-only ELM327s pair with Android phones but are invisible to the Dial. Known good: Vgate iCar Pro 2S / iCar Pro BLE 4.0.
- Notifications must be subscribed with **write-with-response** (`subscribe(true, cb, true)`). NimBLE defaults to a CCCD write *without* response, which the iCar Pro 2S silently drops: connect and discovery both report success, then every command times out with zero bytes. NimBLE's `subscribe()` also returns `true` when it can't find the CCCD at all, so its return value alone proves nothing.

Serial logging needs `-DARDUINO_USB_CDC_ON_BOOT=1` (the StampS3 board sets `ARDUINO_USB_MODE=1` but not this, so `Serial` would go to UART0, not the USB-C port). Build with `-DOBD2_TRACE` to log every command and response.

Design notes: `docs/superpowers/specs/2026-07-18-obd2-dial-design.md`

Current state, what's still unverified against a real car, and the planned
flash-logging design: `docs/next-steps.md`
