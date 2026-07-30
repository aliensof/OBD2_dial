# Where this stands — 2026-07-30

Picking this up later? Read this first. The short version: two commits of new
features are flashed to the Dial but **none of them have been tested against a
real car**, and the bench can't get past BLE discovery.

## What was added

`7591378` — supported-PID mask, 10 more gauges, pending codes, freeze frame

- `0100`/`0120`/`0140` read once per connect; the gauge rotation is filtered to
  PIDs the car actually answers (`buildPages()` in `src/main.cpp`).
- Gauges moved from a switch to the `kPids` table in `src/elm327.cpp`. Adding a
  PID is one row.
- New gauges: throttle, MAP, MAF, short/long fuel trim, timing advance, fuel
  level, ambient temp, run time, distance-with-MIL.
- CODES menu gained *Pending codes* (mode 07) and *Freeze frame* (mode 02).

`3d28367` — multi-frame DTCs, background alarms, peak hold

- `joinFrames()` in `src/parse.cpp` splices ELM327's `0:`/`1:` continuation
  lines before parsing. Before this, **every code in a multi-frame reply was
  silently dropped** — a car with 3+ faults showed 2.
- `kWatch` in `src/main.cpp` polls coolant and battery every 5s on any page;
  out of range gives 3 beeps and a full-screen warning, max once a minute.
- Each gauge keeps a session high (both ends for signed values). Hold to reset.

## Verified vs. not

Confirmed: firmware builds, host parser tests pass (`test/host/parse_check.cpp`
covers multi-frame, mode 07, and raw DTC byte decoding), binary flashed with
`-DOBD2_TRACE` present in the ELF.

**Not confirmed — every one of these needs a real car:**

- MAF scaling (`/100`) and fuel-trim offset (`-128`). Standard formulas, but
  VW-group ECUs are where scaling surprises turn up.
- Freeze-frame framing. The code matches `42<pid>` and skips one frame byte.
  Some dongles echo the frame number, some don't. If freeze frame reads empty
  on a car that *has* a stored code, this is why.
- `kWatch` thresholds (110 °C coolant, 11.5 V) are guesses for a warm
  VW-group engine.
- The alarm path has no test behind it at all — it needs a running engine.

## Blocker: BLE discovery fails on the bench

Three flashes in a row, same signature:

```
[elm] link up, mtu=255
[ble] 4 services
[ble] svc 0x18f0
[ble] no service with notify+write found      <- no "[ble] chr" lines at all
[elm] connect FAILED (not in range / wrong addr type)   <- every retry after
```

The link comes up and the OBD service `0x18f0` is found, but **zero
characteristics enumerate under it**. This is in code untouched by either
commit, so it predates them.

Most likely a stuck dongle, not firmware. Before blaming the code:

1. Unplug the dongle from the OBD port ~10s, replug.
2. Make sure no phone app holds the BLE link — these accept one central at a
   time, and that produces exactly this "connects, no characteristics" pattern.

If it still enumerates zero characteristics with a freshly power-cycled dongle
and nothing else paired, it's a real discovery bug in `findUartChars()` and
jumps the queue over every feature below.

## Capturing the trace

The device is already flashed with trace on. **Nothing is logged on the
device** — no code writes to flash yet, so the laptop must be attached during
the drive or the log is lost.

```sh
~/.platformio/penv/bin/pio device monitor -p /dev/cu.usbmodem14201 \
    --filter time | tee ~/obd2-session.log
```

`--filter time` timestamps each line, which is what gives us per-command BLE
latency. Note `pio` is not on PATH — it lives at `~/.platformio/penv/bin/pio`.

Five minutes idling in the driveway with a few throttle blips is enough; no
actual driving needed for the timing and scaling questions.

### What to grep for

```sh
grep -E "supported|gauge pages" ~/obd2-session.log     # PID mask worked?
grep -A1 "> 0210\|> 0206\|> 0207" ~/obd2-session.log   # MAF + trim scaling
grep -A2 "> 020200" ~/obd2-session.log                 # freeze-frame framing
```

- MAF: expect ~2–5 g/s warm idle, 15–25 cruising. Off by ~100× means the
  divisor is wrong for this ECU.
- Trims: expect within ±10%. Reading ~128 or ~-100 means the offset didn't apply.
- Round-trip time between a `[cmd] >` line and its reply sets the logging
  sample rate below.

## Next: flash logging (designed, not built)

The partition table (`default_8MB.csv`) already reserves **1.5 MB of SPIFFS at
`0x670000`, currently unused**. No SD card needed — the M5Dial has no slot
anyway.

Design decisions already settled:

- **Fixed-size binary records**, `uint32 ms | int16 × N values` ≈ 34 bytes.
  JSON is 3–5× larger (keys repeated every sample) and pickle is a Python-only
  format nothing on the ESP32 can write. 1.5 MB ≈ 44,000 samples ≈ 12 h at 1 Hz.
- **True ring buffer, not delete-and-rewrite.** Preallocate one file filling the
  partition, write at a cursor that wraps to 0, keep the cursor in NVS
  (`Preferences` is already open for the dongle address). Deleting the oldest
  file triggers SPIFFS garbage-collection pauses and burns erase cycles;
  overwriting in place avoids both and makes "oldest gets overwritten" free.
- **The constraint is BLE, not flash.** Every PID is a round trip. At ~50 ms
  each, 15 PIDs don't fit in a 1 s sample, and polling everything would also
  slow the gauge on screen. The trace timings decide the rate and how many PIDs
  get logged. Conservative starting point: 1 Hz, 8 PIDs.
- If running untethered, **flash without `-DOBD2_TRACE`**. It prints ~3 lines/s
  to USB CDC; behaviour with no host attached is unverified on this core
  version and some versions block on a full buffer, which would stutter the UI.

## Queue after that

1. **Readiness monitors** — bytes B–D of PID 01, already fetched and discarded.
   The "will it pass inspection" answer.
2. **VIN** (mode 09 PID 02) — nearly free now that multi-frame reassembly exists.
3. **Tune `kWatch`** once one drive shows this car's real warm coolant temp and
   charging voltage.

Deliberately skipped, with reasons: RTC timestamps (no coin cell, clock resets
every power-down — use `millis()` plus engine run time PID `0x1F`), RFID
(onboard, unused, solves nothing here), WiFi dashboard (shares one radio with
BLE; don't add contention while the BLE link is still unproven), Grove Port A
sensors (only worth real hardware for something the ECU doesn't expose, like
oil temp or EGT).
