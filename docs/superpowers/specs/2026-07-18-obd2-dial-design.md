# OBD2 Dial — design

M5Stack Dial firmware that connects over BLE to an ELM327 OBD2 dongle and shows live engine data plus read/clear of fault codes. Cars: VW group (Skoda Yeti, Seat Ibiza) — standard OBD2 modes 01/03/04 only, no VW-specific protocols in phase 1.

## Stack

- PlatformIO, Arduino framework, board `m5stack-stamps3` (the Dial's StampS3).
- `M5Dial` library (display, touch, encoder, buzzer), `NimBLE-Arduino` 1.4.x (BLE central).
- ELM327 is plain text over a BLE UART-style service: write `010C\r`, read notification bytes until `>`.

## Components

- `src/parse.{h,cpp}` — pure functions, no Arduino deps: clean ELM responses, extract PID bytes, decode mode-03 DTC lists (CAN and K-line framing), DTC code → short description table. Host-testable.
- `src/elm327.{h,cpp}` — BLE client: the user picks their dongle once from a scan list (OBD-looking devices — service fff0/ffe0/18f0 or OBD-ish name — are marked `*`); the address is persisted in NVS and connected to directly (5s timeout) from then on. Auto-discovers the notify+write characteristic pair, AT init (`ATZ,E0,L0,S0,H0,SP0`), blocking `cmd()` with timeout, typed getters (rpm, speed, coolant, intake, load, battery volts, DTC count/read/clear). Auto-reconnect on drop.
- `src/ui.{h,cpp}` — full-screen canvas drawing: connecting screen, gauge page (label / big value / unit / progress arc around the round edge), DTC summary, DTC code page, clear-confirm screen.
- `src/main.cpp` — state machine and input glue.

## UI / interaction

- Rotate encoder → switch pages: RPM, Speed, Coolant, Battery, Intake temp, Engine load, Codes, BLE Scan.
- Gauge pages poll their PID ~3x/s; failed reads show `--`. Arc turns red near limits (rpm, coolant).
- Codes page shows fault count + CHECK ENGINE if MIL on. Tap → menu (Fault codes / Clear faults / Service reset): fault codes scroll one per screen with description; clear shows a red confirm screen (tap confirms, mode 04 + beep); service reset is a stub — it needs VW TP2.0, not standard OBD2. Hold exits the menu.
- BLE Scan page: tap to scan, rotate to highlight, tap to save a device as "my dongle" (persisted; replaces the previous choice). First boot lands here until a dongle is chosen.
- Connect screen: tap enters demo mode (browse UI without hardware). Dongle out of range → 2s tap window + 5s connect attempt, looping. Connected but ignition off → gauges show `--`.

## Hardware findings

- M5Dial lib's encoder driver gets no interrupts on the dial's GPIO 40/41 (pin table stops at 39) → own quadrature ISR in `main.cpp`.
- NimBLE default connect timeout (30s) blocks the loop → capped at 5s.
- iCar Pro 2S (service 18F0, notify 2AF0 / write 2AF1) ignores a CCCD write sent *without* response, so `subscribe()` must pass `response=true`. Symptom was maximally misleading: link up, services found, subscribe "OK", writes accepted, and every read timing out empty. NimBLE's `subscribe()` also returns true when no CCCD exists — `findUartChars` now requires the 0x2902 descriptor before accepting a service.
- Serial needs `-DARDUINO_USB_CDC_ON_BOOT=1`; the StampS3 board sets `ARDUINO_USB_MODE=1` only, so `Serial` lands on UART0 (GPIO43/44), not USB-C. `-DOBD2_TRACE` enables per-command logging.

## Testing

One host-compiled check (`test/host/parse_check.cpp`, plain g++ + asserts) covering response cleaning, PID byte extraction, and DTC decoding for CAN and K-line framings. Hardware behavior verified on the car.

## Deferred (phase 2+)

Freeze frame, VW-specific UDS data, multi-frame DTC responses (>3 codes on K-line / >2 ECUs), settings, themes.
