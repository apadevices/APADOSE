# Changelog

All notable changes to the APA-Dose library are documented here.  
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [3.8.3] — 2026-05-18

### Changed
- **README — `ALARM_SENSOR_FAULT` added to alarm table** — was listed in `APADOSE.h` and API.md
  but entirely absent from the README alarm table. Row added with trigger conditions (2 min
  of bad readings, or 30 min without any valid reading) and auto-recovery behaviour.
- **README — Safety band row updated** — now states that the safety band check runs every 10 s
  via the sensor read cycle; previous wording implied it only fired when a dose was about to start.
- **README — Stale sensor row rewritten** — old wording ("a single warning message is sent")
  was inaccurate since `ALARM_SENSOR_FAULT` was implemented. Updated to describe the two-path
  trigger (2 min bad readings, 30 min stale) and auto-recovery with no acknowledgment required.
- **README — Daily dose limit row** — added that the counter auto-resets every 24 h (at real
  midnight with an RTC, every 24 h from boot without one).
- **README — Alarm table daily limit row** — added auto-reset note to `ALARM_DAILY_LIMIT` recovery column.
- **README — Monitoring section** — added `getSecondsUntilNextDose()` and `getSecondsSinceLastDose()`
  to the Key Features monitoring bullet; both were public API since 3.8.2 but undocumented in the README.
- **README — Dose counter bullet** — updated to reflect 24 h millis-based fallback reset without RTC.
- **README — Volume tracking paragraph** — corrected "accumulates for the session" to "resets
  every 24 h from boot" to reflect the millis-based daily reset introduced in 3.8.2.
- **README — Key Features safety bullet** — "stale sensor" replaced with `ALARM_SENSOR_FAULT`
  so the alarm name is consistent with the alarm table and API reference.
- **README — Platform memory table updated** — all five platforms re-measured from a clean build of
  `examples/basic/02_ph_and_cl` (two-pump sketch, matching the original table basis).
  Flash and RAM numbers updated to reflect features added since 3.4.2:
  Uno 14,194 B / 734 B; Mega 15,260 B / 734 B; ESP32 290,293 B / 22,056 B;
  ESP8266 276,355 B / 28,804 B. Blue Pill (STM32F103C8T6) row added:
  26,952 B flash / 2,612 B RAM — verified clean build.
- **README — Key Features minimal footprint bullet updated** — two-pump figures refreshed to
  ~14 KB flash / 734 B RAM on Uno; per-instance RAM overhead updated to ~290 B.
- **Version bumped to 3.8.3.** No code, API, or EEPROM change.

---

## [3.8.2] — 2026-05-18

### Fixed
- **`EEPROM.put()` instead of `EEPROM.write()` in `saveConfiguration()`** — `EEPROM.write()`
  writes one byte at a time and silently truncates multi-byte fields; replaced with
  `EEPROM.put()` which serialises the full `ConfigData` struct correctly on all platforms.
- **Post-prime rest period always enforces 5 minutes** — previously the 5-minute guard used
  `if (restPeriod == 0)` which only fired on first boot; subsequent primes inherited the last
  proportional dose's rest period. Now unconditionally set to 5 minutes after every prime.
- **`isExternalStopActive()` returns cached state** — was calling the user callback directly
  inside the getter; now returns `flags.externalStopSent` set by the last `update()` call.
- **`isOutsideDosingWindow()` returns cached state** — was performing an RTC I²C read inside
  the getter; now returns `flags.outsideDosingWindow` cached each `update()` cycle.
- **`ALARM_SENSOR_FAULT` implemented** — was declared in the enum but never triggered.
  Now fires after 2 minutes of continuous out-of-range or NaN/Inf sensor readings, or after
  30 minutes without any valid reading (stale timeout). Clears automatically on recovery.
  Range check added to both before- and after-dose sampling blocks in `manageFeedbackSampling()`
  to prevent corrupted samples reaching `evaluateFeedback()` before the alarm fires.

### Changed
- **`dosesUp()` extracted as private helper** — the condition
  `dosingType == DOSE_CL || phDirection == PH_PLUS` was duplicated in three places;
  consolidated into a single inline helper used throughout.
- **`BEFORE_SAMPLES` / `AFTER_SAMPLES` typed as `constexpr uint8_t`** — were typed as `int`,
  inconsistent with the `uint8_t` fields they are compared against.
- **`FEEDBACK_PULSE_MAX_MS` named constant** — replaced magic `15000UL` with
  `FEEDBACK_PULSE_MAX_MS = 14300UL` (11 s × 1.3). Feedback correction now boosts PWM by 50%
  and pulse duration by 30% (capped at 14 300 ms) after two or more failed attempts.
- **`collectSample()` helper** — duplicate ~18-line BEFORE/AFTER sampling blocks in
  `manageFeedbackSampling()` consolidated into a single private `bool collectSample(unsigned long, char)`.
- **`calculateProportionalPulse()` zone table reformatted** — four 150+ character single-line
  branches broken into readable multi-line blocks; no logic change.
- **`eepromBaseAddress` typed as `uint16_t`** — was `int`, allowing negative values that would
  corrupt arbitrary EEPROM addresses. Constructor parameter and `APA_DOSE_EEPROM_ADDRESS`
  constant updated to match.
- **`ALARM_SENSOR_FAULT` removed from dead-code** — enum entry now active; `getAlarmName()`
  and `checkAlarmClearConditions()` handle it correctly.
- **Sensor callback type warning added to API.md** — documents that the `SensorReadCallback`
  must return values matching the pump's `dosingType`; the library cannot detect a mismatched
  callback since pH values (0–14) fall within the valid ORP hardware range (−1500 to +1500 mV).

### Skipped
- Internal structs (`DosingPulse`, `ConfigData`, `FeedbackState`, `AlarmState`, `FeedbackPhase`)
  remain visible in the public header intentionally — `sizeof(ConfigData)` is used by
  multi-pump sketches to calculate per-instance EEPROM offsets automatically.

---

## [3.8.1] — 2026-05-17

### Fixed
- **`factoryReset()` did not reset the feedback state machine** — if the library was in a
  sampling phase (`FB_MEASURING_BEFORE` or `FB_MEASURING_AFTER`) when `factoryReset()` was
  called, the next `update()` would resume the half-finished sampling cycle against the new
  default setpoint and fire a spurious proportional dose with corrupted before-sample data.
  Fixed: `memset(&feedback, 0, sizeof(feedback))` added before `resetToDefaults()`.
- **Dead `readSensor != nullptr` guard in stale-sensor check** — `manageProportionalDosing()`
  already returns at an earlier `if (readSensor == nullptr)` check, so the same condition
  on the stale-sensor branch was always `true`. Removed the redundant prefix.
- **`enableAdaptivePB()` parameter name shadowed class member in header** — declaration used
  `nudgePct` as the parameter name, which shadowed the private member of the same name and
  was inconsistent with the implementation (which correctly uses `pct`). Renamed to `pct`.
- **Snake_case variable names in `evaluateFeedback()`** — `actual_shift` / `expected_shift`
  renamed to `actualShift` / `expectedShift` to match the camelCase convention used
  throughout the file.

### Changed
- **Version bumped to 3.8.1.** No API or EEPROM change.

---

## [3.8.0] — 2026-05-17

### Added
- **`factoryReset()`** — public method that resets all five EEPROM-stored user settings to
  their type-default values in one call. Safe to call at any time: if a dose is active it is
  stopped immediately; if priming is active it is stopped immediately; any active alarm is
  cleared. After reset, defaults are saved to EEPROM and `"Factory reset"` is sent via
  `onStatusMessage`. `dosingType` is not touched — it is always re-applied by `begin()` on
  the next boot.

  | Field | Value after reset |
  |-------|------------------|
  | `setpoint` | pH 7.4 / ORP 700 mV |
  | `proportionalBand` | pH 1.0 / ORP 100 mV |
  | `phDirection` | `PH_PLUS` |
  | `nudgePct` | 0 (adaptive PB disabled) |
  | `adaptedPB` | 0.0f (learned value discarded) |

- **Version bumped to 3.8.0.** No EEPROM struct change — config version remains 4.

---

## [3.7.0] — 2026-05-17

### Added
- **Adaptive proportional band** — `enableAdaptivePB(uint8_t nudgePct)` (1–25%) enables a
  self-learning control band that converges toward the pool's actual chemical response over time.
  After each effective feedback cycle (correct direction, above threshold), the library compares
  the actual sensor shift to the expected shift and nudges `adaptedPB` up when the sensor
  overshot, down when it undershot. The learned band is clamped to `[0.2×PB, 3.0×PB]` and
  saved to EEPROM after every adjustment. Disabled by default (`nudgePct = 0`); calling
  `enableAdaptivePB(0)` discards the learned value and reverts to the fixed band.
- **`getAdaptedPB()`** — returns the current effective proportional band: the learned value when
  adaptive mode is active, the fixed `proportionalBand` otherwise.
- **`isAdaptivePBEnabled()`** — returns `true` when `nudgePct > 0`.
- **`APA_DOSE_CONFIG_VERSION` bumped to 4** — `ConfigData` gains `nudgePct` (uint8_t) and
  `adaptedPB` (float), growing from 15 → 20 bytes. Existing v3 EEPROM configs fail the version
  check on first boot and reset to defaults automatically — no code change needed.

### Changed
- **`sizeof(ConfigData)` is now 20 bytes** on all platforms (was 15). Multi-pump EEPROM
  addresses shift: second pump 206 → 212; third 220 → 232; fourth 234 → 252.
  Update hard-coded addresses in existing multi-pump sketches, or use `sizeof(ConfigData)`:
  ```cpp
  // Before (3.6.0)
  ApaDose clPump(PIN_CL, APA_DOSE_EEPROM_ADDRESS + 15);

  // After (3.7.0)
  ApaDose clPump(PIN_CL, APA_DOSE_EEPROM_ADDRESS + 20);
  // or — preferred, always correct regardless of version:
  ApaDose clPump(PIN_CL, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData));
  ```
- **Version bumped to 3.7.0.**

---

## [3.6.0] — 2026-05-17

### Added
- **`ApaDoseDirection` enum** — new enum `{ PH_PLUS, PH_MINUS }` separates dosing direction
  from chemical type. pH direction is now independent of `ApaDoseType`: pass it to `begin()`
  as the fourth parameter and change it at runtime with `setPhDirection()`.
- **`setPhDirection(ApaDoseDirection)`** — changes acid/base direction at runtime for `DOSE_PH`
  pumps after `begin()` has run. Returns `false` if dosing is active or if called on a `DOSE_CL`
  pump. Saves to EEPROM immediately.
- **`getPhDirection()`** — returns the currently active `ApaDoseDirection`.
- **`APA_DOSE_CONFIG_VERSION` bumped to 3** — `ConfigData` gains a `phDirection` byte (now
  15 bytes). Existing v2 EEPROM configs fail checksum on first boot and reset to defaults
  automatically — no code change needed.

### Changed
- **`ApaDoseType` simplified** — values `DOSE_PH_PLUS`, `DOSE_PH_MINUS`, and `DOSE_CL` replaced
  by `DOSE_PH` and `DOSE_CL`. Direction is now expressed separately via `ApaDoseDirection`.
  **Breaking change** — all existing sketches must update `ApaDoseType` usage.
- **`begin()` signature** — `ApaDoseDirection dir` added as the fourth parameter (after `type`,
  before `blackoutMinutes`). Both overloads updated. **Breaking change** — all sketches must
  add the direction argument to their `begin()` call. For `DOSE_CL` pumps pass `PH_PLUS` as a
  placeholder — it is stored but has no effect on control logic.
  ```cpp
  // Before (3.5.0)
  phPump.begin(getpH, filterRunning, DOSE_PH_MINUS, 20, 6);
  clPump.begin(getORP, filterRunning, DOSE_CL, 20, 12);

  // After (3.6.0)
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6);
  clPump.begin(getORP, filterRunning, DOSE_CL, PH_PLUS, 20, 12);
  ```
- **`sizeof(ConfigData)` is now 15 bytes** on all platforms (was 14).
- **All 10 examples updated** to use the new `begin()` signature and new enum names.
- **Version bumped to 3.6.0.**

---

## [3.5.0] — 2026-05-17

### Fixed
- **`begin()` ESP EEPROM ordering bug** — calling `setDosingType()` before `begin()` on
  ESP8266/ESP32 silently discarded the type because `EEPROM.begin()` had not been called yet,
  so `saveConfiguration()` wrote nothing. On first boot this worked by accident (EEPROM invalid,
  `resetToDefaults()` used the in-RAM type); on subsequent boots the EEPROM-stored type was
  correct only because it had been saved during a previous boot's `begin()`. Any scenario that
  relied on a pre-`begin()` type change was unreliable.
- **Dosing type now an explicit `begin()` parameter** — type is set after `EEPROM.begin()` runs
  and always takes precedence over the EEPROM-stored value (hardware type is authoritative).
  If the type changes between boots, setpoint and band are clamped to the new type's valid range
  and re-saved automatically.

### Changed
- **`begin()` signature** — `ApaDoseType type` is now the third parameter (after `filter`,
  before `blackoutMinutes`). Both overloads updated. **Breaking change** — all existing sketches
  must add the type argument to their `begin()` call.
  ```cpp
  // Before (3.4.x)
  phPump.setDosingType(DOSE_PH_MINUS);
  phPump.begin(getpH, filterRunning, 20, 6);

  // After (3.5.0)
  phPump.begin(getpH, filterRunning, DOSE_PH_MINUS, 20, 6);
  ```
- **`setDosingType()` role changed** — no longer called during setup; use it only for runtime
  type changes after `begin()` has run (e.g. swapping chemical in the field).
- **All 10 examples updated** to use the new `begin()` signature.
- **Version bumped to 3.5.0.**

---

## [3.4.2] — 2026-05-16

### Fixed
- **`enum ApaDoseType` underlying type** — the enum was untyped, causing the compiler to choose
  a platform-dependent width: 1 byte on AVR/ESP8266, 4 bytes on ESP32/STM32. As a result,
  `sizeof(ConfigData)` was 14 on AVR/ESP8266 but 17 on ESP32/STM32, producing wrong EEPROM
  addresses for any pump beyond the first in multi-pump setups compiled for 32-bit targets.
  Fixed: `enum ApaDoseType : uint8_t` forces 1-byte width on all platforms; `sizeof(ConfigData)`
  is now 14 bytes everywhere. Existing EEPROM configs on affected 32-bit targets will fail
  the checksum check on first boot and reset to defaults automatically — no code change needed.
- **`begin()` always returned `true`** — `loadConfiguration()` result was never propagated;
  every call returned `true` regardless of EEPROM validity. Fixed: return value now reflects
  whether EEPROM data was valid (`true`) or corrupt/blank and reset to defaults (`false`).
  Code that checked the return value (documented since 3.1.1) now works correctly.
- **`printLibraryInfo()` hardcoded version string** — the function printed a literal `"3.1.2"`
  instead of the `APA_DOSE_VERSION` macro, so the displayed version was always wrong after
  any version bump. Fixed: now prints `F("APA-Dose v" APA_DOSE_VERSION)` via adjacent string
  literal concatenation — correct at compile time for every release.

### Added
- **`isInExternalStopResumeDelay()`** — returns `true` during the mandatory 5-minute settling
  wait that follows the external stop signal clearing. Complements `isExternalStopActive()` so
  display code can show a distinct "resume delay" state rather than showing idle with no
  explanation for why dosing has not restarted.
- **`isOutsideDosingWindow()`** — returns `true` when the dosing window is enabled and the
  current RTC hour falls outside it. Allows display code or logging to distinguish "window
  blocked" from other idle reasons without duplicating the hour comparison logic.

### Changed
- **Version bumped to 3.4.2.**

---

## [3.4.1] — 2026-05-16

### Added
- **External stop resume delay** — after the `ExternalStopCallback` clears (returns `false`),
  a mandatory 5-minute settling time (`EXTERNAL_STOP_RESUME_MS`) now applies before any new
  dose is allowed. This prevents a brief dose from firing while a pool operator is still
  switching between filtration modes, or while water is still flowing through a diverted outlet
  (e.g. backwash pipe). During the delay, `triggerManualDose()` is also blocked. Priming is
  exempt. Status messages: `"ExtStop cleared"` when the signal drops, `"Dosing resumed"` when
  the delay expires. If the stop re-activates during the settling window, the timer resets.

### Changed
- **Memory optimisation** — `startupBlackoutMs` (`unsigned long`, 4 B) replaced by
  `startupBlackoutMinutes` (`uint8_t`, 1 B); multiplication to milliseconds is done inline
  at the two call sites. Saves 3 bytes of RAM per instance (6 B for a two-pump sketch on AVR).
  No API or behaviour change.
- **Version bumped to 3.4.1.**

---

## [3.4.0] — 2026-05-16

### Added
- **External stop callback** — `setExternalStopCallback(ExternalStopCallback cb)` registers an
  optional function that blocks all dosing (automatic and manual) while it returns `true`.
  Priming (`triggerPrime()`) is exempt. Designed for pool-side conditions where injecting
  chemistry into non-circulating or diverted water is unsafe: maintenance/vacuuming mode,
  backwash cycle, pool cover closed, or any signal from an external controller.
  - While blocked during an active dose: pump stops immediately, `"Stop:ext request"` is sent
    via `onStatusMessage`.
  - While blocked with no active dose: `"ExtStop active"` is sent once; new doses and
    `triggerManualDose()` calls are rejected.
  - When the callback returns `false` again: `"ExtStop cleared"` is sent and normal dosing resumes.
- **`isExternalStopActive()`** — live status query; returns the current result of the registered
  external stop callback (or `false` when none is registered).

### Changed
- **Version bumped to 3.4.0.**

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
  Suggested spacing: `sizeof(ConfigData)` bytes between instances.
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
