# Changelog

All notable changes to the APA-Dose library are documented here.  
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [3.3.0] — 2026-05-15

### Fixed
- **`EEPROM.update()` not available on ESP32/ESP8266** — replaced with `EEPROM.write()` which
  works on all platforms; ESP32/ESP8266 `EEPROM.begin()` and `EEPROM.commit()` guards already
  present. Without this fix, ESP builds would fail to compile.
- **`triggerManualDose()` collision in `FB_MEASURING_BEFORE`** — when before-dose sampling
  completed while a manual dose was already active, `manageFeedbackSampling()` called
  `startDosingPulse()` unconditionally, overwriting the manual dose PWM and `dosingStartTime`,
  and double-incrementing `dailyDoseCount`. Fixed: `if (flags.dosingActive) return` guard added
  before `startDosingPulse()` in `manageFeedbackSampling()`.
- **Solenoid valve support** — `setPumpRange()` rejected `minPWM == maxPWM` due to the
  `minPWM >= maxPWM` guard, making time-proportional mode for solenoids impossible to configure.
  Guard relaxed to `minPWM > maxPWM`; equal values are now valid and documented.

### Changed
- **Dosing pulse durations tuned** — all four tiers raised to deliver more chemical per pulse
  and reduce the number of correction cycles needed for a given deviation:

  | Error % of band | Old duration | New duration |
  |:---------------:|:------------:|:------------:|
  | 0 – 25 %        | 1 – 3 s      | 2 – 4 s      |
  | 25 – 50 %       | 3 – 6 s      | 4 – 7 s      |
  | 50 – 75 %       | 6 – 10 s     | 7 – 10 s     |
  | 75 – 100 %      | 10 s fixed   | 11 s fixed   |

- **Version bumped to 3.3.0.**

### Added
- **Inter-pump chemical lockout** — static 90 s gap enforced between any two pump instances
  completing a dose. Applies to both automatic and manual doses; blocks `shouldStartDosing()`
  and `triggerManualDose()` until the timer expires. Priming is exempt (tube filling, no chemical
  injected). Prevents back-to-back acid + chlorine injection at the same inlet.
- **Volume tracking** — `setPumpFlowRate(float mlPerMin)` (optional, default 450 mL/min) enables
  `getDailyVolumeMl()` and `getLastDoseVolumeMl()`. Volume is calculated from actual pulse duration
  and PWM intensity relative to max flow; resets at midnight with RTC.
- `LICENSE` — MIT license, © 2026 APADevices (@kecup)
- `keywords.txt` — Arduino IDE syntax colouring for all public types, methods, and constants
- `library.properties` — Arduino / PlatformIO package metadata (name, version, author,
  maintainer, description, architectures)

### Documentation
- `README.md` completely rewritten as a library description and how-to guide: ASCII proportional
  band diagram (pH-PLUS orientation), six-phase dosing cycle flowchart, dosing zones table,
  safety feature reference, alarm handling guide with callback and polling examples, quick-start
  code, multi-pump EEPROM layout, setup order, solenoid mode notes, platform support table,
  APA ecosystem section, disclaimer
- All 10 example sketches: author line added — `kecup@vazac.eu (APA Devices)`
- Examples 01–02: solenoid valve note added near `setPumpRange()` explaining time-proportional mode
- All 10 examples: one-directional pH warning added after `setDosingType()` — pH+ and pH−
  must not run simultaneously on the same pool
- `README.md` Safety Systems: filtration interlock and filter-off notification documented as
  requiring a `FilterCallback`; startup blackout and daily dose limit documented as opt-in
  `begin()` parameters; "ORP ceiling" row expanded to "Setpoint range enforcement" covering
  both pH (6.8–7.8) and ORP (400–850 mV) bounds
- `README.md` Setup Order: `begin()` parameter table added covering both overloads, all four
  parameters with types, defaults, and required vs optional marking
- `README.md` Diagnostic output: Arduino IDE enable path documented — uncomment the prepared
  `// #define APA_DOSE_DEBUG` line in `APADOSE.h`
- `README.md` Multi-Pump section: `triggerManualDose()` subsection added with button-triggered
  and RTC-scheduled examples; `triggerPrime()` subsection added explaining maintenance use,
  safety guard bypass, and post-prime 5-minute rest
- `README.md` Key Features: expanded from 11 to 21 entries organised in five categories
  (proportional control, safety, flexibility, monitoring, engineering); section moved to top
  of README for immediate visibility
- `README.md` APA Ecosystem: expanded with APAPHX-Board v2 hardware layer, full stack table,
  and marketing description positioning APA Devices against proprietary pool controllers
- `README.md` temperature compensation note: updated to reference Passco 2001 formula used
  by APAPHX and APAPHX2 libraries; same correction applied to `docs/API.md`
- `docs/API.md` `begin()` signatures: `maxDailyDoses` parameter added to both overloads
  (was present in the parameter table but missing from the function signatures)
- `docs/API.md` EEPROM layout: write method corrected from `EEPROM.update()` to
  `EEPROM.write()` to match the 3.3.0 fix for ESP32/ESP8266 compatibility

---

## [3.2.1] — 2026-05-14

### Fixed
- **False `ALARM_SAFETY_BAND` at boot** — the constructor defaulted `sensorValue` to
  `PH_SETPOINT_DEFAULT (7.4)` for every pump type; an ORP pump with setpoint 700 mV
  saw a 692.6 mV error on the very first `checkSafetyConditions()` call, immediately
  firing a safety alarm before any real reading was received.
  `begin()` now primes `sensorValue = setpoint` (zero error) and then attempts one
  synchronous sensor read; if the first reading is finite it is used immediately,
  otherwise the zero-error fallback suppresses the false alarm until `update()` reads
  the sensor normally.
- **Spurious motor pulse at filter dropout** — in the `FB_MEASURING_BEFORE` sampling
  phase, completing the before-dose average called `startDosingPulse()` without first
  confirming the filter was still running. A sub-millisecond motor blip could occur
  on the same `loop()` iteration that detected filter loss. A filter guard is now
  applied immediately before `startDosingPulse()` inside `manageFeedbackSampling()`.
- **Float truncation in `calculateProportionalPulse()`** — pulse duration was computed
  via `map((long)errorPercent, ...)`, which truncates the float to integer before
  interpolating and loses sub-percent precision. Replaced with explicit float linear
  interpolation `(unsigned long)(startMs + (t / 25.0f) * rangeMs)` in each tier;
  results are identical at integer percent values and correct for fractional values.
- **`ORP_SETPOINT_MAX` lowered from 900 mV to 850 mV** — free chlorine in pool water
  becomes harmful to bathers above ~850 mV; the previous ceiling of 900 mV allowed
  setpoints in the hazardous range.
- **Stale sensor timeout** — when the sensor callback returned `NaN` or `infinity`
  continuously (cable fault, ADC power loss), `sensorValue` remained frozen at the
  last good reading with no timeout, allowing indefinite automatic dosing against a
  stale value. `SENSOR_STALE_MS` (30 min, `APADOSE.h`) is now tracked via
  `lastGoodSensorTime`; if no finite reading arrives within that window, automatic
  dosing is suspended and a single `"Sensor:stale>30min"` status message is sent.
  Dosing resumes automatically on the next finite reading.
- **`ORP_FEEDBACK_THRESHOLD` raised from 5 mV to 10 mV** — 5 mV was below the noise
  floor of typical ORP electrodes, causing false `ALARM_INEFFECTIVE` reports after
  legitimate chlorine doses that moved ORP by 6–9 mV.

### Changed
- Version bumped to 3.2.1 (`APA_DOSE_VERSION`, `APA_DOSE_VERSION_PATCH` added).

---

## [3.2.0] — 2026-05-14

### Changed (SRAM optimisation — ~39 bytes saved per instance, ~156 bytes on a 4-pump setup)
- **Sensor profile fields removed** — seven `float` instance members (`sensorSetpointMin/Max/Default`,
  `sensorBandMin/Max/Default`, `sensorFeedbackThreshold`) were SRAM copies of compile-time
  constants. Replaced with `isOrpProfile()` inline helper that reads the `constexpr` values
  directly from flash at the few call sites that need them. Saves **28 bytes per instance**.
- **`setSensorRange()` and `setSensorBand()` removed** — only existed to write the fields above;
  no example sketch used them. Removed from the public API.
- **`loadDefaultRanges()` removed** — internal helper that populated the removed fields.
- **11 `bool` members packed into a 2-byte bitfield struct `flags`** — `dosingActive`,
  `blackoutMessageSent`, `dosingWindowEnabled`, `manualDoseActive`, `primingActive`,
  `configurationValid`, `lastDoseDataValid`, `filterOffAlarmSent`, `sensorValueBad` (class
  level) plus `alarmActive` and `alarmNeedsAck` (previously in `AlarmState`) — 11 bytes → 2
  bytes. Saves **9 bytes per instance**.
- **`AlarmState` cleaned up** — `alarmActive` and `userAcknowledgmentRequired` moved into the
  `flags` bitfield as `flags.alarmActive` / `flags.alarmNeedsAck`.
- **`FeedbackPhase` enum replaces three `FeedbackState` bools** — `measuringBefore`,
  `measuringAfter`, `waitingForFeedback` were mutually exclusive; replaced with a single
  `FeedbackPhase phase` (`uint8_t` enum: `FB_IDLE / FB_MEASURING_BEFORE / FB_WAITING /
  FB_MEASURING_AFTER`). Saves **2 bytes per instance**.
- **Version bumped to 3.2** (`APA_DOSE_VERSION`, `APA_DOSE_VERSION_MAJOR/MINOR`).

### Migration note
`setSensorRange()` and `setSensorBand()` are removed. Sensor validation ranges are
fixed per dosing type and can no longer be overridden at runtime. Users who relied on
these methods should use `DOSE_PH_PLUS / DOSE_PH_MINUS / DOSE_CL` with the built-in
profiles, or fork the header constants if genuinely non-standard ranges are required.

---

## [3.1.7] — 2026-05-14

### Fixed
- **Non-finite sensor values (NaN / infinity) were silently swallowed** — `shouldStartDosing()`
  returned false for NaN (no dosing), but the safety band check also evaluated false, so the
  alarm system never fired; the library appeared healthy while flying blind.
  Three guard points added:
  - **`readSensors()`** — validates each periodic read with `isfinite()`; on the first bad
    reading a `"Sensor:bad value"` status message is sent once and the last known good value
    is retained. The flag clears automatically when a finite value is received again.
  - **Before-dose feedback sampling** — non-finite sample skipped; next sample rescheduled
    after `SAMPLE_INTERVAL` so averaging is never corrupted.
  - **After-dose feedback sampling** — same protection; evaluation only proceeds when the
    full sample set is finite.

---

## [3.1.6] — 2026-05-14

### Fixed
- **Extended filter-off was completely silent** — when the filtration pump failed or was off
  for maintenance, the library quietly blocked dosing with no user notification; a filter
  failure could go undetected indefinitely.
  A `"Filter off>30min"` status message now fires once via `onStatusMessage` after
  `FILTER_OFF_ALARM_MS` (30 minutes) of continuous filter-off detected during `update()`.
  The timer resets as soon as the filter comes back on. The threshold is a named constant
  in `APADOSE.h` so it can be adjusted without touching the implementation.

---

## [3.1.5] — 2026-05-14

### Fixed
- **`triggerManualDose()` duration ceiling** — previously accepted any `durationMs` with no
  upper bound; a single call could run the pump for hours, risking severe over-dosing.
  Requests above `MAX_MANUAL_DOSE_MS` (5 minutes, defined in `APADOSE.h`) are now clamped
  to the ceiling and a `"Dose capped:5min"` status message is sent — the dose still executes
  at the capped duration so automation code is not silently broken.

---

## [3.1.4] — 2026-05-14

### Fixed
- **Safety band — dual protection against dangerous water chemistry** — previous formula
  `proportionalBand × 1.1` caused the safety zone to scale linearly with band width; at the
  maximum band (2.0 pH / 250 mV ORP) the alarm would not fire until pH 5.2 / ORP 425 mV,
  both hazardous for swimmers and equipment.
  New formula: `min(proportionalBand × 1.5, hardCap)` where `hardCap` is a non-configurable
  type-specific limit (`PH_SAFETY_HARD_CAP = 1.0` pH unit, `ORP_SAFETY_HARD_CAP = 150` mV).
  - Tight bands still get proportional scaling (50% buffer beyond control band edge).
  - Wide bands are capped — pH alarm fires at worst ±1.0 from setpoint (6.4–8.4 for SP 7.4),
    ORP alarm fires at worst ±150 mV (550–850 mV for SP 700).
  - All three constants exported from `APADOSE.h` for reference: `PH_SAFETY_HARD_CAP`,
    `ORP_SAFETY_HARD_CAP`, `SAFETY_BAND_MULTIPLIER`.

---

## [3.1.3] — 2026-05-14

### Fixed
- **Post-prime rest period** — previously `triggerPrime()` did not update `lastDosingEnd` on
  completion, allowing the proportional controller to fire immediately after a prime that had
  pushed chemical into the pool. Now sets `lastDosingEnd = millis()` on prime completion and
  ensures `currentPulse.restPeriod` is at least 5 minutes (honoring any longer rest left over
  from a preceding proportional dose).

### Added
- **`APA_DOSE_STATUS_BUFFER_SIZE`** — `constexpr size_t` (96) exported from `APADOSE.h`;
  use as the minimum buffer size for `getSystemStatus()`. Worst-case output is ~80 chars;
  the constant adds a 16-byte margin.
- **`APA_DOSE_DEBUG` documented in API.md** — full table of per-cycle diagnostic messages,
  build flag usage, and flash/RAM cost note.

---

## [3.1.2] — 2026-05-14

### Changed (memory — Uno compatibility)
- **All string literals moved to flash (PROGMEM)** via `F()` macro + a 20-byte RAM buffer
  that exists only for the duration of the callback call. Static strings no longer occupy SRAM.
  Estimated SRAM saving: **~350–500 bytes** (the full set of status/alarm literals).
- **All status and alarm messages shortened to ≤ 19 characters** — fit one row of a 16×2 or
  20×4 LCD with no truncation. See mapping in `src/APADOSE.cpp` sendStatus calls.
- **`AlarmState.alarmMessage[80]` → `[20]`** — 60 bytes saved per instance (240 bytes on a
  4-pump setup).
- **`FeedbackState` counters narrowed** — `failedAttempts`, `sampleCount`, `targetSamples`,
  `wrongDirectionCount` changed from `int` to `uint8_t`; 4 bytes saved per instance.
- **`dailyDoseCount` narrowed** — `int` → `uint8_t`; 1 byte saved per instance.
- **`getFailedAttempts()` / `getDailyDoseCount()`** return type changed to `uint8_t`
  (backward compatible — implicit promotion to `int` at call site).
- **`-Wreorder` warnings fixed** — constructor initializer list reordered to match class
  member declaration order.
- **`-Wformat` warnings fixed** — `float` arguments in `snprintf` now cast to `double`
  to match `%f` / `%.2f` expected type on strict-C99 targets.
- **`FeedbackState` zero-initialization** — replaced fragile positional aggregate
  `{0,0,...}` with `memset(&feedback, 0, sizeof(feedback))`.
- **`printLibraryInfo()` strings moved to flash** — `F()` applied; function body reduced.

---

## [3.1.1] — 2026-05-14

### Fixed
- **`triggerManualDose()` now respects the filtration interlock** — previously the filter state was
  only checked mid-dose (stop guard), not at the moment a manual dose was requested; a scheduled
  dose could start into stagnant water if the filter was off. Now returns `false` immediately when
  `filterPumpRunning` returns `false`, consistent with proportional dosing behavior.
- **`ConfigData` struct marked `__attribute__((packed))`** — without packing, the compiler may insert
  padding bytes between `version` (uint8_t) and `setpoint` (float); those undefined bytes were
  included in the checksum loop, causing potential false checksum mismatches across compiler versions
  or platforms. The packed struct has no padding, checksum covers only real data.
  **EEPROM note**: config format version bumped from 1 → 2 (`APA_DOSE_CONFIG_VERSION`). Any existing
  v1 EEPROM config will be rejected on first boot and reset to type defaults — users will need to
  re-enter their setpoints once.
- **Manual dose now enforces a rest period before the next proportional dose** — previously
  `triggerManualDose()` stored `restPeriod = 0`, allowing the proportional controller to fire again
  immediately after a manual dose without any mixing wait. Fixed by adding an optional `restMs`
  parameter (default 20 min) that is stored in `currentPulse.restPeriod` and respected by the
  standard rest-period guard.

### Changed
- `triggerManualDose(unsigned long durationMs)` →
  `triggerManualDose(unsigned long durationMs, unsigned long restMs = 20UL * 60UL * 1000UL)`.
  Backward compatible — existing calls unchanged. Sensor-less pumps that want no rest between manual
  doses (e.g., flocculant where `maxDailyDoses = 1` already prevents double-dosing) can pass `0`
  explicitly.
- `APA_DOSE_CONFIG_VERSION` extracted as a named `constexpr uint8_t` in the header — future struct
  changes only require updating this one constant.

---

## [3.1.0] — 2026-05-14

### Added
- **Per-instance EEPROM addressing** — `ApaDose(uint8_t pumpPin, int eepromAddress = APA_DOSE_EEPROM_ADDRESS)`;
  each pump in a multi-pump setup passes a unique address so configurations never overwrite each other.
  Single-pump sketches require no change — the default (192) is unchanged.
  Suggested spacing: `sizeof(ConfigData)` bytes (≈20 bytes) between instances.
- **`getLastDoseSensorBefore()`** — averaged sensor value collected before the last proportional dose
- **`getLastDoseSensorAfter()`** — averaged sensor value collected after the last rest period
- **`getDoseEffectiveness()`** — signed percentage of band: positive = correct direction, negative = wrong direction.
  Formula: `(after − before) / proportionalBand × 100`, sign-normalized so positive always means
  "dose moved sensor in the expected direction" regardless of pump type
- **`hasDoseHistory()`** — returns `false` until the first complete dose + feedback cycle has run;
  guard before calling the diagnostic getters

### Fixed
- `loadConfiguration()` and `saveConfiguration()` now use the per-instance `eepromBaseAddress`
  instead of the global constant — closes the silent EEPROM corruption bug in multi-pump setups

---

## [3.0.0] — 2026-05-14

### Added
- **`DOSE_CL` dosing type** — full ORP/chlorine pump support with dedicated sensor profile
  (setpoint 400–900 mV, default 700 mV; band 50–250 mV, default 100 mV; threshold 5 mV)
- **Sensor-less pump support** — `begin(nullptr, filterCallback, ...)` accepted; proportional
  dosing and sensor alarms skipped, filtration interlock remains active; use with
  `triggerManualDose()` for flocculant/algaecide scheduling
- **RTC integration** — `setRTCCallback(ApaDoseTimeCallback)` wires a DS3231 or any RTC;
  enables `setDosingWindow(startHour, endHour)` and automatic daily dose counter reset at midnight
- **Daily dose limit** — optional 4th parameter of `begin()`: `uint8_t maxDailyDoses = 0`
  (0 = unlimited); triggers latching `ALARM_DAILY_LIMIT` when reached; also blocks
  `triggerManualDose()` when limit is reached
- **`ALARM_DAILY_LIMIT`** — new latching alarm type requiring `acknowledgeAlarm()` to clear
- **`getAlarmMessage()`** — public getter returning the current alarm message string
- **`getMaxDailyDoses()`** — public getter for the configured daily dose ceiling
- **`triggerPrime(durationMs, uint8_t pwm = 0)`** — optional PWM parameter; `0` sentinel
  uses `pumpMaxPWM`; explicit values clamped to `[pumpMinPWM, pumpMaxPWM]`
- **10 example sketches** spanning four levels:
  - `basic/01_single_ph` — minimum correct single pH- pump setup
  - `basic/02_ph_and_cl` — pH + chlorine on shared filter
  - `intermediate/03_serial_diagnostics` — full serial command interface
  - `intermediate/04_lcd_display` — 20×4 LCD with alarm priority stack
  - `advanced/05_multi_pump` — 4-pump setup with DS3231 + scheduled flocculant/algaecide
  - `advanced/06_alarm_management` — complete alarm/ACK reference
  - `expert/07_apa_serial` — APAPHX2_ADS1115 + DS18B20 + APA-Dose, serial output
  - `expert/08_apa_lcd` — APAPHX2_ADS1115 + DS18B20 + APA-Dose, 20×4 LCD
  - `expert/09_apa_ds2482_serial` — APAPHX2_ADS1115 + DS2482 + APA-Dose, serial output
  - `expert/10_apa_ds2482_lcd` — APAPHX2_ADS1115 + DS2482 + APA-Dose, 20×4 LCD
- `docs/Milestone3.md` — complete milestone documentation with architecture summary,
  setup patterns, known limitations, and deferred items

### Fixed
- **Safety band formula** — was `proportionalBand + 0.1f` (meaningless margin for ORP);
  corrected to `proportionalBand * 1.1f` (10% beyond control band, scales correctly for
  both pH and ORP)
- **First-boot type ordering** — `setDosingType()` called before `begin()` could silently
  keep a cross-type setpoint/band from a previous EEPROM write (e.g., pH setpoint 7.4 used
  as ORP setpoint); fix: when new values fall outside the incoming type's valid range,
  `setDosingType()` resets them to the new type's defaults before saving
- **Blackout status message** — previously fired in `begin()` before callbacks were
  registered; moved to first `update()` tick so `onStatusMessage` is always wired by then

### Changed
- `begin()` signature extended with optional 4th parameter `uint8_t maxDailyDoses = 0`
  (backward compatible — existing calls unchanged)
- `triggerPrime(durationMs)` extended with optional `uint8_t pwm = 0` (backward compatible)
- `ALARM_DAILY_LIMIT` added to the set of alarms that require `acknowledgeAlarm()` to clear
  (alongside `ALARM_WRONG_DIRECTION` and `ALARM_INEFFECTIVE`)

---

## [2.2.0] — 2026-05

### Added
- **Startup blackout** — optional 3rd parameter of `begin()`: `uint8_t blackoutMinutes = 0`;
  blocks new dosing after power-on to prevent double-dosing after a mid-rest-period reboot;
  `isInStartupBlackout()` exposes state for display; auto-fires status message on expiry
- **Filtration pump interlock** — `FilterCallback` parameter in `begin()`; two guards
  inside `manageProportionalDosing()` stop active dosing and block new dosing when filter
  is not running; omitting the callback preserves backward-compatible behaviour
- **Calibrated PWM range** — `setPumpRange(uint8_t minPWM, uint8_t maxPWM)`; PWM floor
  at `minPWM + 10%` of range ensures pump always overcomes pipe resistance; feedback
  boost corrections capped at `pumpMaxPWM`

### Fixed
- **Double pH filtering** removed — internal 80/20 EMA applied on top of the already-smoothed
  value from the external sensor library caused sluggish tracking; `readSensors()` now passes
  the callback value through unchanged
- **EEPROM address conflict** — configuration storage moved from address `0` to `192` to avoid
  collision with Arduino system usage (0–127) and the companion APAPHX2_ADS1115 library (128–177)
- **EEPROM write wear** — `EEPROM.put()` replaced with byte-by-byte `EEPROM.update()` to skip
  physical writes when stored bytes already match

### Changed
- **Library renamed** from `PoolDosing` / `pool_dosing_system` to `APA-Dose` / `ApaDose`
- **Constructor simplified** from `PoolDosing(phSensorPin, phPumpPin, enablePin)` to
  `ApaDose(pumpPin)` — sensor pin was never used; enable pin removed (single-MOSFET hardware)
- **`begin()` consolidates callbacks** — `setPHCallback()` and `setFilterCallback()` removed;
  both passed directly to `begin(PHReadCallback, FilterCallback, uint8_t blackoutMinutes)`
- All enum and constant names prefixed: `DOSE_PH_PLUS/MINUS`, `ALARM_NONE/WRONG_DIRECTION/
  INEFFECTIVE/SAFETY_BAND/INVALID_PARAM/SENSOR_FAULT`, `ApaDoseType`, `ApaDoseAlarm`

---

## [2.1.0] — 2026-02

### Added
- **EEPROM persistent configuration** — `setpoint`, `proportionalBand`, and `dosingType`
  survive power loss; checksum + magic-number validation; `resetToDefaults()` for factory reset
- **Configuration API** — `getCurrentSetpoint()`, `getCurrentProportionalBand()`,
  `getCurrentDosingType()`, `isConfigurationValid()`

---

## [2.0.0] — 2026-02

### Added
- **Comprehensive alarm system** — `WRONG_DIRECTION`, `DOSING_INEFFECTIVE`,
  `OUT_OF_SAFETY_BAND`, `INVALID_PARAMETER` alarm types
- **Callback interface** — `onAlarmTriggered(AlarmType, const char*)`,
  `onAlarmCleared(AlarmType, const char*)` for hardware-agnostic notifications
- **Flexible configuration** — `setPHSetpoint()`, `setProportionalBand()`, `setDosingType()`
  with runtime validation
- **Automatic safety band** — calculated as `proportionalBand + 0.1` (replaced in 3.0.0)
- **Wrong direction detection** — requires 2–3 measurement confirmations before alarm fires
- **Dosing ineffective detection** — 3 consecutive failed feedback checks → alarm

---

## [1.0.0] — 2026-02

### Added
- **Proportional pulse dosing** — 4 error zones mapped to PWM intensity + pulse duration
  + rest period; non-blocking state machine throughout
- **Feedback control** — before/after sensor averaging (2 samples before, 3 after);
  trend detection with 0.05 pH threshold; escalating corrections (attempt 1: +30% PWM,
  attempt 2: +50% PWM + 2× pulse, attempt 3: ALARM)
- **Single pH pump** — user selects pH+ (base) or pH- (acid) at configuration time
- **Non-blocking architecture** — all timing via `millis()`; no `delay()` anywhere;
  parallel operation of dosing, sampling, and alarm logic
