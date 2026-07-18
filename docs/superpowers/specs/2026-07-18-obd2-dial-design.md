# OBD2 Dial — design

M5Stack Dial firmware that connects over BLE to an ELM327 OBD2 dongle and shows live engine data plus read/clear of fault codes. Cars: VW group (Skoda Yeti, Seat Ibiza) — standard OBD2 modes 01/03/04 only, no VW-specific protocols in phase 1.

## Stack

- PlatformIO, Arduino framework, board `m5stack-stamps3` (the Dial's StampS3).
- `M5Dial` library (display, touch, encoder, buzzer), `NimBLE-Arduino` 1.4.x (BLE central).
- ELM327 is plain text over a BLE UART-style service: write `010C\r`, read notification bytes until `>`.

## Components

- `src/parse.{h,cpp}` — pure functions, no Arduino deps: clean ELM responses, extract PID bytes, decode mode-03 DTC lists (CAN and K-line framing), DTC code → short description table. Host-testable.
- `src/elm327.{h,cpp}` — BLE client: scan (known service UUIDs fff0/ffe0/18f0 or OBD-ish name), connect, auto-discover the notify+write characteristic pair, AT init (`ATZ,E0,L0,S0,H0,SP0`), blocking `cmd()` with timeout, typed getters (rpm, speed, coolant, intake, load, battery volts, DTC count/read/clear). Auto-reconnect on drop.
- `src/ui.{h,cpp}` — full-screen canvas drawing: connecting screen, gauge page (label / big value / unit / progress arc around the round edge), DTC summary, DTC code page, clear-confirm screen.
- `src/main.cpp` — state machine and input glue.

## UI / interaction

- Rotate encoder → switch pages: RPM, Speed, Coolant, Battery, Intake temp, Engine load, DTC.
- Gauge pages poll their PID ~3x/s; failed reads show `--`. Arc turns red near limits (rpm, coolant).
- DTC page shows code count + CHECK ENGINE if MIL on. Tap (touch or button) → read codes; rotate scrolls one code per screen with description. Touch-hold → "Clear codes?" confirm screen; tap confirms (mode 04 + beep), anything else cancels.
- No car/dongle → "Scanning…" screen, retry loop forever. Dongle connected but ignition off → gauges show `--`.

## Testing

One host-compiled check (`test/host/parse_check.cpp`, plain g++ + asserts) covering response cleaning, PID byte extraction, and DTC decoding for CAN and K-line framings. Hardware behavior verified on the car.

## Deferred (phase 2+)

Freeze frame, VW-specific UDS data, multi-frame DTC responses (>3 codes on K-line / >2 ECUs), settings, themes.
