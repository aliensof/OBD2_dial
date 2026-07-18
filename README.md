# OBD2 Dial

M5Stack Dial as a BLE OBD2 display: connects to a BLE ELM327 dongle, shows live gauges, reads and clears fault codes. Built for VW-group cars (Skoda Yeti, Seat Ibiza) but uses only standard OBD2.

## Use

1. Plug the BLE dongle into the car's OBD2 port, ignition on.
2. Power the Dial — it scans, connects, and beeps when linked.
3. **Rotate** to switch pages: RPM, Speed, Coolant, Battery, Intake, Load, Fault codes.
4. On the fault-codes page: **tap** to read codes, rotate to scroll them, **touch-and-hold** to clear (with confirm).

## Build & flash

```sh
pio run -t upload        # build + flash over USB-C
pio device monitor       # serial log
```

Host test for the response parser:

```sh
cd test/host && g++ -std=c++11 -I../../src parse_check.cpp ../../src/parse.cpp -o parse_check && ./parse_check
```

Design notes: `docs/superpowers/specs/2026-07-18-obd2-dial-design.md`
