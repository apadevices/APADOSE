# Changelog

All notable changes to the APA-Dose library are documented here.  
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [3.17.4] — 2026-09-22

### Fixed

- **`begin()` was silently discarding a persisted `phDirection` on every boot.** For `DOSE_PH`
  pumps, `begin()` unconditionally set `phDirection = dir` (the compile-time argument) immediately
  after `loadConfiguration()` had already correctly restored the operator's last saved direction
  from EEPROM — throwing that restored value away every time. Worse: when the loaded direction
  differed from `dir`, `begin()` then called `saveConfiguration()`, permanently overwriting the
  operator's EEPROM-stored choice with the sketch's hardcoded default — so a direction changed at
  runtime via `setPhDirection()` (which already persisted correctly) would revert on the very next
  boot and then stay reverted, since EEPROM itself had been rewritten back to the hardcoded value.
  `phDirection` now follows the same "trust `loadConfiguration()` unless EEPROM was invalid or the
  dosing type changed" rule that `setpoint`/`proportionalBand` already used correctly — `dir` is now
  only ever a first-boot/type-change default, never re-applied over a live operator setting. No API
  change; `setPhDirection()`'s behavior and signature are unchanged, it just now actually survives
  a reboot as its own doc comment already claimed.

---

## [3.17.3] — 2026-05-27

### Changed

- `docs/API.md` — fixed pre-existing markdown formatting warnings: added language tags to all
  plain fenced code blocks (MD040), added blank lines after method headings and around fenced
  blocks (MD022/MD031), and added blank lines around lists and tables (MD032). No API changes.
- `README.md` — fixed MD040 (added `text` language to ASCII-art and diagram code blocks) and
  MD032 (added blank lines between section headers and bullet lists in Key Features).
- Added `.markdownlint.json` — suppresses MD024 (CHANGELOG duplicate sibling headings) and
  disables MD013 (line length) for this API reference document where long prose lines are normal.

---

## [3.17.2] — 2026-05-27

### Added

- **`alarmNeedsAcknowledgment()`** — new status query that returns `true` when the currently
  active alarm requires an explicit `acknowledgeAlarm()` call to clear. Returns `false` when
  no alarm is active or when the active alarm clears automatically.
  Latching alarms (return `true`): `ALARM_WRONG_DIRECTION`, `ALARM_INEFFECTIVE`,
  `ALARM_TANK_EMPTY`, `ALARM_OFA`.
  Auto-clearing alarms (return `false`): `ALARM_SAFETY_BAND`, `ALARM_SENSOR_FAULT`,
  `ALARM_DAILY_LIMIT`, `ALARM_OVER_SETPOINT`.
  Intended for GUI and display integrations that need to distinguish "show alert, require
  user action" from "show status, clear automatically" without hardcoding the alarm list.

---

## [3.17.1] — 2026-05-26

### Fixed

- **Tank estimation disabled by default** — `_tankCapacityL` now initialises to `0` (off) instead
  of 20. Previously, the feature was silently active for all users from first boot, which could
  fire `ALARM_TANK_EMPTY` after ~30 days without the user ever calling `setTankCapacity()`.
  Call `setTankCapacity(litres)` explicitly in `setup()` to enable the feature.
- **`resetToDefaults()` / `factoryReset()`** — `_tankCapacityL` resets to 0 (was 20); consumed
  counter and daily average reset to 0 as before.
- **README — Monitoring bullet rewritten** to clearly present the two independent paths (software
  estimation via `setTankCapacity()` and hardware sensor via `setTankEmptyCallback()`), their
  individual use, and how they strengthen each other when combined.
- **README / API.md / CHANGELOG** — removed "default 20 L" references from tank capacity
  documentation; corrected to "disabled by default".

---

## [3.17.0] — 2026-05-26

### Added

- **Tank level estimation** — software-only tank tracking without a hardware float switch.
  - `setTankCapacity(uint8_t liters)` — configure tank size (1–65 L); 0 disables the feature;
    disabled by default — call once in `setup()` to enable. Resets the consumed counter to zero (assumes tank is full). Safe to call from
    `loop()` — EEPROM is written only when the value changes or to persist the reset.
  - `getTankRemainingPct()` — returns 0–100 % of tank remaining, or 255 when disabled.
  - `getTankDaysUntilEmpty()` — returns a rolling 7-day estimate of days until empty, or 255
    when fewer than one full day of data is available.
  - `ALARM_TANK_EMPTY` fires when cumulative consumption reaches the configured capacity.
    If `setTankEmptyCallback()` is also registered, the hardware sensor is the sole alarm
    authority and estimation never fires the alarm — percentage display continues normally.
  - `acknowledgeAlarm()` resets `_tankConsumedMl` to zero when clearing `ALARM_TANK_EMPTY` —
    regardless of whether the alarm was triggered by the hardware sensor or by estimation.
  - Consumed counter and capacity are persisted to EEPROM at midnight (alongside dOFA); also
    saved immediately on `setTankCapacity()` and on `acknowledgeAlarm()` for `ALARM_TANK_EMPTY`.
  - Works with all pump types including sensor-less (flocculant, algaecide) and scheduled doses.
  - SRAM cost: 4 bytes per instance (`uint8_t _tankCapacityL`, `uint16_t _tankConsumedMl`,
    `uint8_t _dailyAvgDL`). EEPROM cost: 3 bytes per instance (`tankCapacityL`, `tankConsumedMl`
    in `ConfigData`). `_dailyAvgDL` is RAM-only; it rebuilds after one midnight.
- **`ConfigData` version bumped 5 → 6** — two new fields (`tankCapacityL`, `tankConsumedMl`)
  added to `ConfigData`. `sizeof(ConfigData)` increases from 22 to 25 bytes. Existing EEPROM
  (version 5) is detected as invalid and resets to safe defaults on first boot — setpoint,
  proportional band, and adaptive PB must be re-entered once after upgrading.
- **EEPROM address map updated** — per-instance layout with 25-byte blocks:
  pump 1: 192–216 · pump 2: 217–241 · pump 3: 242–266 · pump 4: 267–291.
- **Example `02_ph_and_cl` updated** — demonstrates `setTankCapacity()` for both pumps and
  prints `getTankRemainingPct()` / `getTankDaysUntilEmpty()` to Serial every 10 minutes.

---

## [3.16.4] — 2026-05-25

### Fixed

- **Over-setpoint diagram labels corrected** — in the "Dead-band and over-setpoint alarm correlation"
  diagram (README and API.md), the ruler labels at 7.30 and 7.50 were swapped. For a pH-PLUS pump
  (setpoint 7.4, dead-band 10 %): 7.30 = dead-band entry (SP − W), 7.50 = ALARM_OVER_SETPOINT
  threshold (SP + W). The arrow text above the ruler was already correct; only the labels
  underneath were transposed.

---

## [3.16.3] — 2026-05-25

### Added

- **`ALARM_OVER_SETPOINT`** — non-latching alarm that fires when the sensor has been on the wrong
  side of setpoint for more than 30 minutes (`OVER_SETPOINT_DELAY_MS = 1800000UL`). Notifies the
  operator that chemistry has drifted past target with no corrective action possible by the pump.
  - When a dead-band is configured, the alarm mirrors the same band width `W` on the opposite side
    of the setpoint — the threshold that starts dosing on one side is exactly the threshold that
    raises the alarm on the other.
  - With dead-band disabled (`W = 0`), any persistent over-setpoint reading triggers it.
  - Auto-clears the moment the sensor returns to the dosing zone — no `acknowledgeAlarm()` required.
  - SRAM cost: 4 bytes per instance + 1 bit in the flags bitfield.
  - New private method `checkOverSetpoint()` called from `manageProportionalDosing()` when idle.
  - Auto-clear injected into `readSensors()` so it runs unconditionally even while alarm is active.

---

## [3.16.2] — 2026-05-25

### Fixed

- **dOFA adaptation range capped at 14 days** — `setDOFAAdaptDays()` previously accepted 3–30;
  upper limit reduced to 14. N=30 adapts too slowly for seasonal pools (time constant ~30 days
  means spring-to-summer chemistry changes take 6+ weeks to track, causing false `ALARM_OFA`
  fires during legitimate heavy-use periods).
- **dOFA warm-up documentation corrected** — all docs, examples, and API reference previously
  claimed "~3–5 dosing days" warm-up. The actual behaviour is: `isDOFALearning()` becomes false
  at midnight of the **first qualifying day** (day with ≥5 min proportional run time) — typically
  day 2. The "3–5 days" figure was wrong everywhere it appeared; all occurrences fixed.
- README banner width 400 → 600 px.
- Dosing cycle ASCII diagram right-border alignment corrected (consistent 58-char inner width).

---

## [3.16.1] — 2026-05-25

### Changed

- **Dual license** — replaced MIT license with a dual-license model. Non-commercial
  use (personal, private, educational, hobby) remains free of charge under the same
  permissive terms. Commercial use (selling hardware with this library pre-installed,
  commercial pool maintenance services, integration into products sold to third parties)
  now requires a separate written Commercial License.
  Contact: **jaroslav@vazac.eu**
- Added commercial licensing notice to README.

---

## [3.16.0] — 2026-05-25

### Added

- **Dynamic OFA (dOFA) — self-learning over-feed alarm** — always active, zero configuration
  required. dOFA observes the normal proportional run time for THIS pool and fires `ALARM_OFA`
  when today's proportional run time exceeds 2× the EMA learned baseline (warning status at 1.5×).
  No limit to guess or set — the library builds the baseline from real daily usage.

  Algorithm: exponential moving average over N days (default N = 10, configurable 3–30 via
  `setDOFAAdaptDays()`). First qualifying day seeds the baseline directly (cold-start seeding —
  no false alarms during warm-up). Baseline is persisted to EEPROM at midnight and survives power
  cycles. A mid-day power cycle loses the day's accumulation (RAM only); the learned baseline is
  safe in EEPROM. Checks activate only after the baseline reaches DOFA_MIN_BASELINE_SEC (300 s,
  5 min) — `isDOFALearning()` returns true during warm-up (~3–5 dosing days).

  Both dOFA and fixed OFA (`setOFALimit()`) run independently and coexist. Both reuse `ALARM_OFA`.
  Whichever fires first controls. `acknowledgeAlarm()` resets both daily counters.

  Excluded from dOFA accumulation: `triggerManualDose()`, `triggerShock()`, `triggerPrime()`.
  Sensor-less pumps: dOFA is inert — proportional dosing never runs, counter stays 0.

  New public API:
  - `setDOFAAdaptDays(uint8_t days)` — EMA speed 3–30, default 10; call in `setup()`
  - `disableDOFA()` — suppress all dOFA checks for this instance
  - `getDOFAPct()` — today's proportional run as % of learned baseline (0–100; 0 = learning)
  - `isDOFALearning()` — true while baseline not yet established
  - `resetDOFA()` — clears baseline + daily counter; call at spring opening

  New constants: `DOFA_MIN_DAILY_SEC` (60 s), `DOFA_MIN_BASELINE_SEC` (300 s),
  `DOFA_WARN_FACTOR` (150 %), `DOFA_STOP_FACTOR` (200 %).

### Changed

- **EEPROM layout** — `ConfigData` gains a `uint16_t dofaLearnedSec` field; `sizeof(ConfigData)`
  grows from 20 to 22 bytes; `APA_DOSE_CONFIG_VERSION` bumped 4 → 5. Existing EEPROM data
  (version 4) fails validation, falls back to safe defaults, and is re-saved in the new format on
  the first boot — this is intentional and safe. EEPROM address comments in multi-pump examples
  updated to reflect the new 22-byte stride.

- **`factoryReset()` now resets dOFA** — clears the learned baseline, zeroes the daily counter,
  and re-enables dOFA if it had been disabled.

- **`acknowledgeAlarm()` for `ALARM_OFA` now resets both counters** — `_dailyPumpRunSec` (fixed
  OFA) and `_dofaDailyRunSec` (dOFA) are both zeroed; both `ofaWarningSent` and `dofaWarningSent`
  flags are cleared.

- **Flags bitfield** — two new bits added: `dofaDisabled` and `dofaWarningSent`; total 22 flags,
  still packed into 3 bytes (2 bits remaining before the next byte boundary).

- **SRAM footprint** — two-pump Uno sketch grows by approximately 10 bytes (5 B per instance:
  `_dofaLearnedSec` u16 + `_dofaDailyRunSec` u16 + `_dofaAdaptDays` u8); flags bitfield unchanged
  at 3 bytes; new baseline: ~857 B RAM / ~20 KB flash on Uno.

### Documentation

- README: dOFA added to **Key Features → Safety** section; OFA accumulation note in dosing cycle
  updated to mention dOFA counter; `ALARM_OFA` table row updated to include dOFA as a source;
  footprint bullet updated.
- API.md: new **Dynamic OFA (dOFA)** section alongside the fixed OFA section, with full API table,
  threshold table, warm-up note, RTC note, and spring-opening guidance.
- Examples 01, 02, 06: dOFA comment block added; `sizeof(ConfigData)` comments updated to 22 bytes;
  hardcoded EEPROM address strides in comments updated (212 → 214, 232 → 236, 252 → 258); example
  06 `printAlarmStatus()` now includes `printDOFA()` per pump.

---

## [3.15.2] — 2026-05-24

### Fixed

- **OFA alarm: `acknowledgeAlarm()` now resets the daily counter immediately** — previously ACK
  cleared the latch but left `_dailyPumpRunSec` unchanged, so `checkAlarmClearConditions()` could
  not clear the alarm until midnight reset the counter; dosing remained blocked for the rest of the
  day despite operator acknowledgment. Now `clearAlarm()` zeroes `_dailyPumpRunSec` and
  `ofaWarningSent` when clearing `ALARM_OFA`, matching VADOS OFA behaviour (ACK = clear alarm +
  reset counter; dosing resumes immediately). Midnight auto-reset is preserved as a fallback for
  unattended systems.
- **Manual doses excluded from OFA accumulation** — pump run time from `triggerManualDose()` was
  incorrectly counted toward the daily OFA limit; manual doses are now exempt (only proportional
  and shock dosing accumulate).
- **Manual doses no longer blocked by `ALARM_OFA`** — `triggerManualDose()` was blocking all
  manual operations when any latching alarm was active; manual doses are now permitted when only
  `ALARM_OFA` is set, allowing operator intervention without requiring ACK first.
- **Float absolute value: `abs()` replaced with `fabsf()`** — four calls to `abs()` on `float`
  operands replaced with `fabsf()`, the unambiguous single-precision function; on AVR the `abs()`
  macro can silently cast to `int` if the C++ overload from `<cmath>` loses resolution, producing
  wrong results for sub-integer differences. No `#include` change needed — `fabsf` is available
  through `Arduino.h` on all supported platforms. Flash cost: −106 bytes on Uno (more direct call).
- **`setScheduledDose()` input clamping** — `hour > 23` is clamped to 23, `minute > 59` to 59,
  `durationMs == 0` to 1000 ms (1 s minimum), and `intervalDays == 0` to 1 (already guarded);
  previously out-of-range hour/minute values were stored silently and the scheduled dose never
  fired, with no indication to the caller.
- **`collectSample()` unused-parameter warning suppressed** — the `prefix` parameter is used only
  inside `#ifdef APA_DOSE_DEBUG`; a `(void)prefix` statement added so release builds do not emit
  an unused-variable warning on GCC/Clang strict builds.

### Documentation

- **API.md — `setOFALimit()` / `getOFAPct()` section added** — both functions were absent from
  the API reference; new section documents parameters, 70 %/90 % thresholds, ACK reset behaviour,
  what counts toward the limit, how to choose a starting value, and a code example.
- **API.md — PWM pin requirement added to `setPumpRange()`** — board-specific PWM-capable pin
  lists added (Uno/Nano: 3 5 6 9 10 11; Mega: 2–13, 44–46; ESP/STM32 notes).
- **API.md — sensor smoothing requirement explained** — added note that the sensor callback must
  return a stable, smoothed value; noisy raw readings cause false `ALARM_WRONG_DIRECTION` and
  `ALARM_INEFFECTIVE` alarms; guidance on rolling average added.
- **API.md — `ALARM_OFA` exception documented in `triggerManualDose()` table** — blocked-conditions
  table and method comparison table updated to reflect that `ALARM_OFA` does not block manual doses.
- **README — `ALARM_OFA` added to alarm recovery table** — was missing from the alarm reference table.
- **README — OFA accumulation callout added after dosing cycle diagram** — explains that each dose
  pulse contributes to the daily OFA counter, with pointer to Safety Systems for details.
- **README — platform footprint table updated** — all five platform figures updated to reflect
  3.15.2 build (Uno: 19 712 B flash / 847 B RAM).
- **README — `extras/` folder added to Files tree** — `extras/apadose-banner.png` was referenced
  in the banner but missing from the repository file listing.
- **Examples — beginner clarity pass across 01, 02, 06**
  - `setPoolVolume()`: explicit "if your pool is ≤ 30 m³, leave this commented out" guidance added
    to examples 01, 02, and 06.
  - EEPROM address block in example 02: `sizeof(ConfigData)` = 20 bytes stated; addresses for a
    3rd and 4th pump shown explicitly.
  - `begin()` first-boot comment: default values named (setpoint 7.4, band 1.0 pH / 700 mV, 100 mV).
  - Tank empty callback: WHY comment added — dry-running a peristaltic pump damages the pump head.
  - `setEfficiencyThreshold()`: EMA jargon replaced with plain-language explanation.
  - `setCrossSettleMinutes()`: pH/ORP see-saw mechanism explained; Option J chlorine efficiency
    context added.
  - Inter-pump lockout note added to example 02 where two-pump users first encounter it.
  - OFA setup block in example 06: "optional, leave commented" made explicit; how-to-choose
    guidance and ACK-resets-immediately behaviour described.

---

## [3.15.1] — 2026-05-24

### Fixed / Documentation

- **README — OFA description rewritten for beginner clarity** — replaced technical phrasing
  ("cumulative daily pump run-time limit, latching, ACK required") with plain-language explanation;
  scaling, warning, stop, midnight reset, and `getOFAPct()` dashboard use all explained without
  assuming prior familiarity.
- **Example 06 (alarm management) — OFA coverage added**
  - `ALARM_OFA` added to alarm behaviour summary in header comment with full lifecycle explanation
  - `requiresAck()` updated to include `ALARM_OFA`
  - `printOFA()` helper added: prints pump OFA % in the periodic status report when enabled
  - `setup()` OFA configuration block added with pool-scaling explanation and ready-to-uncomment lines

---

## [3.15.0] — 2026-05-24

### Added

- **`setOFALimit(referenceMinutes)` — over-feed alarm (OFA)** — optional cumulative daily pump
  run-time guard. Sets a reference limit (minutes) for a 20 m³ pool; the library auto-scales the
  limit with `setPoolVolume()`. A status warning fires at 70 % of the daily limit; `ALARM_OFA`
  fires at 90 % (latching, `acknowledgeAlarm()` required). The alarm and counter reset
  automatically at midnight (RTC or millis roll-over). `getOFAPct()` returns today's consumption
  as 0–100 %. Disabled by default (`_ofaLimitMin = 0`). Applies to proportional dosing and shock
  mode; manual and prime doses are excluded.
  - RAM cost: +3 bytes per instance (two-pump sketch: 841 B → 847 B on Uno)
  - Flash cost: +558 bytes on Uno
  - New alarm constant: `ALARM_OFA`
  - New constants: `OFA_WARNING_PCT` (70), `OFA_STOP_PCT` (90)

---

## [3.14.3] — 2026-05-23

### Fixed

- **Stale pulse duration values in documentation** — ASCII diagrams, dosing zones tables, timing
  notes, and solenoid mode description in README and API.md all referenced the old 2–11 s range;
  updated to reflect the 10–180 s values introduced in 3.14.2. Expected cycle time note updated
  from 8–23 min to 8–26 min to account for the longer maximum pulse.

---

## [3.14.2] — 2026-05-23

### Changed

- **Proportional pulse durations increased across all zones** — previous values (2–11 s) were
  insufficient for real-world pool chemistry. New ranges match practical dosing requirements:

  | Zone | Error | Pulse duration | Rest |
  |------|-------|----------------|------|
  | 1 | 0–25 % | 10–30 s | 5 min |
  | 2 | 25–50 % | 30–60 s | 10 min |
  | 3 | 50–75 % | 60–120 s | 15 min |
  | 4 | 75–100 % | 180 s (flat) | 20 min |

  Rest periods are unchanged. Volume scaling (`setPoolVolume()`) still applies on top.

- **`FEEDBACK_PULSE_MAX_MS` derived from `ZONE4_PULSE_MS`** — replaced the hardcoded
  `14300UL` constant with `ZONE4_PULSE_MS × 5/3` (≈ ×1.67 = 300 s / 5 min). `ZONE4_PULSE_MS`
  is the single named anchor; the feedback cap updates automatically when zone 4 duration
  changes. Escalation path: 180 s → 234 s (×1.3) → 300 s cap (×1.3, capped).
  No API change, no EEPROM change.

---

## [3.14.1] — 2026-05-23

### Changed

- **`setScheduledDose()` — `threshold` default changed from `0.0f` to `NAN`** — the default
  behaviour is now "dose only when the sensor has drifted past the pump's own setpoint" rather
  than "always dose unconditionally". Pass `0.0f` explicitly to restore the always-dose
  behaviour; pass any finite value to use a specific override. For sensor-less pumps
  (algaecide, flocculant) the threshold is always ignored — they dose unconditionally
  regardless of this parameter.

---

## [3.14.0] — 2026-05-22

### Added

- **`setScheduledDose()` — RTC-based scheduled pre-dosing** — fires a fixed-duration manual
  dose at a configurable time of day, with optional interval and threshold controls.
  Requires an RTC callback registered via `setRTCCallback()`. All standard safety guards
  (filtration interlock, external stop, tank-empty check, daily dose limit, inter-pump
  lockout, active alarm block) are inherited automatically because the scheduled dose calls
  `triggerManualDose()` internally.
  - `hour` / `minute` — wall-clock time at which the dose fires
  - `durationMs` — pulse length in milliseconds (same as `triggerManualDose()`)
  - `intervalDays` (optional, default 1) — every N days; use `7` for weekly
  - `threshold` (optional, default 0.0) — if non-zero, dose is skipped when the sensor
    already reads in the safe direction (e.g. pH already low enough, ORP already high
    enough); ignored for sensor-less pumps (algaecide, flocculant)
  - Works for all pump types: pH, ORP, algaecide, flocculant — ideal for regular
    treatment chemicals that do not have a sensor to react to
  - SRAM cost: 13 bytes per instance; no EEPROM usage

---

## [3.13.3] — 2026-05-22

### Fixed

- **`library.properties` — library name corrected back to `APA-Dose`** — the 3.13.1 fix
  changed the name from `APA-Dose` to `APADOSE` believing the hyphen caused Library Manager
  indexing failures. The root cause was the wrong URL, not the name. The Library Manager
  rejected 3.13.2 because the registered library identity is `APA-Dose`. Name reverted;
  URL, category, and all other 3.13.1 fixes remain in place. No code, API, or EEPROM change.

---

## [3.13.2] — 2026-05-22

### Fixed

- **`ALARM_DAILY_LIMIT` was incorrectly latching** — the alarm fired correctly when the
  maximum daily dose count was reached, but it was marked as requiring manual ACK, so dosing
  stayed blocked overnight even after the daily counter reset at midnight. The alarm now
  auto-clears in both daily-reset paths (RTC midnight roll-over and 24 h millis fallback)
  the same moment `dailyDoseCount` is zeroed. No API change, no EEPROM change.

---

## [3.13.1] — 2026-05-22

### Fixed

- **`library.properties` — Arduino Library Manager discoverability** — three fields corrected
  so the Arduino Library Manager can index and surface new releases:
  - `name` changed from `APA-Dose` to `APADOSE` — removes the hyphen that was inconsistent
    with the library identifier used at registry registration
  - `url` corrected from a Facebook URL to the actual GitHub repository URL
    (`https://github.com/apadevices/APADOSE`) — Library Manager uses this field to locate
    GitHub releases when checking for version updates; the wrong URL caused every release
    after 3.8.3 to be invisible to the manager
  - `category` changed from `Other` to `Device Control` — more accurate classification
- **Version bumped to 3.13.1.** No code, API, or EEPROM change.

---

## [3.13.0] — 2026-05-22

### Added

- **Chemical tank empty sensor (`setTankEmptyCallback()`)** — optional callback-based hardware
  input for a float switch, capacitive sensor, or any dry-contact signal that indicates the
  chemical container is empty. Same pattern as `setExternalStopCallback()` — register before
  `begin()`, one callback per pump instance.

  ```cpp
  bool phTankEmpty() { return digitalRead(PIN_TANK_SENSOR) == LOW; }
  phPump.setTankEmptyCallback(phTankEmpty);  // call before begin()
  ```

  When the callback returns `true` at dose-start time:
  - `ALARM_TANK_EMPTY` fires immediately (latching — requires `acknowledgeAlarm()`)
  - Automatic and manual dosing are blocked
  - `triggerPrime()` is also blocked — no point running a dry pump
  - Checked at dose-start time only, not continuously — zero overhead during rest periods

  After the user refills and presses ACK, dosing resumes normally. If the tank is still empty,
  the alarm fires again on the next dose attempt.

- **`ALARM_TANK_EMPTY`** — new alarm enum value. Appears in `getCurrentAlarm()`,
  `getAlarmMessage()` (returns `"Tank empty!"`), `onAlarmTriggered` callback, and
  `getAlarmName()`. Like `ALARM_WRONG_DIRECTION`, `ALARM_INEFFECTIVE`, and
  `ALARM_DAILY_LIMIT`, it requires `acknowledgeAlarm()` to clear.

### Behaviour notes

- `setTankEmptyCallback()` costs 1 function pointer per instance (2 bytes AVR / 4 bytes
  ESP32/STM32). No SRAM overhead when unused (callback is `nullptr` by default).
- The callback is only invoked when the library is actually about to start a new dose cycle —
  not in the middle of a rest period, not while an alarm is already active, and not at boot.
- **Option E finding:** `wrongDirectionCount` already resets on any non-significantly-wrong-direction
  move (the `else` branch in `evaluateFeedback()` was already decoupled from the `effective`
  check in earlier refactoring). No code change was needed — the alarm is already tolerant of
  high-demand days where ORP drops slightly between correct doses.

---

## [3.12.0] — 2026-05-21

### Added

- **Dose efficiency tracking** — built-in delivery health monitor running after every automatic
  proportional dose. Uses an exponential moving average (EMA, α = 0.2, ~last 5 doses) of
  normalised delta per millisecond of pump run time to learn what normal delivery looks like
  for this specific pump, chemical, and pool — no chemistry constants, no user calibration.

- **`ALARM_INEFFECTIVE` — efficiency path** — when a dose achieves less than the configured
  threshold percent of the learned EMA baseline, `ALARM_INEFFECTIVE` fires with message
  `"Pump/supply fail"`. Default threshold 20 % (active out of the box after 3 warm-up doses).
  Catches empty tank, air lock, and pump failure after 1–3 bad doses instead of waiting for
  the existing 3-strike `failedAttempts` path (which remains as a cold-start safety net).

- **`setEfficiencyThreshold(uint8_t pct)`** — configures the alarm trigger level.
  `0` = alarm disabled, tracking still active. Default 20. Set in `setup()` like `setPhPump()` — not EEPROM-persisted.

- **`getEfficiencyThreshold()`** — returns the current threshold value.

- **`getDoseEffectiveness()`** — reworked from a signed-band proxy to the real EMA ratio:
  0–100 % of learned baseline. Returns 100 during cold-start (< 3 doses). Read any time
  for display; alarm fires automatically when threshold is exceeded.

- **`EFFICIENCY_EMA_ALPHA`** — new public constant (`0.2f`); exposed for documentation
  purposes; not intended for user tuning.

### Changed

- **`getDoseEffectiveness()`** return type changed `float → uint8_t` — now returns the real
  EMA ratio (0–100 %) instead of a signed proportional-band proxy.

- **`APA_DOSE_VERSION_MINOR`** corrected: was `10` (copy error from 3.10.x), now `12`.

### Documentation and examples

- **`examples/calibration/01_flow_rate_calibration/`** — new utility sketch for measuring pump
  flow rate (mL/min). Fill the container with 500 mL of actual chemical, type `run` to start
  the pump at full speed, type `stop` when the container empties. Calculates mL/min from
  elapsed time and prints the exact `setPumpFlowRate()` line to copy. Uses real chemical
  (not water) for accurate viscosity-matched results.

### Behaviour notes

- Tracking is **core infrastructure**, not optional — it runs regardless of threshold setting.
- Excludes manual, shock, and prime doses — only automatic proportional dose cycles update the EMA.
- Adaptive PB nudge is inherently frozen when the efficiency alarm fires (early return in
  `evaluateFeedback()` prevents the nudge path from running).
- `factoryReset()` resets EMA state (ema, count, ratio) via `resetToDefaults()`. Threshold is an integrator value set in `setup()` — not touched by factory reset.
- `acknowledgeAlarm()` does **not** reset EMA or count — baseline persists across alarm cycles.

---

## [3.11.0] — 2026-05-21

### Added

- **pH-first dosing priority (Option J)** — `clPump.setPhPump(&phPump)` registers a pH peer on the
  CL instance. When pH exceeds `CL_PH_MAX` (7.6), CL automatic dosing is suspended until pH drops
  back below threshold. Chlorine is mostly in the ineffective hypochlorite form above 7.6; dosing
  into high-pH water wastes chemical and produces misleading ORP feedback. Status message
  `"CL held: pH high"` fires once on activation and again each time the condition re-triggers.

- **Cross-settle coupling (Option A)** — `clPump.setCrossSettleMinutes(n)` adds a configurable
  hold on CL dosing after each pH dose completes. Prevents the pH/ORP see-saw: acid doses
  temporarily depress ORP during mixing, which can trigger a premature CL dose before chemistry
  has equilibrated. Status message `"CL held: settling"` fires during the hold window.
  `n = 0` (default) disables Option A; Option J remains active independently via `setPhPump()`.

  Both features require a linked pH pump instance and are off by default:
  ```cpp
  clPump.setPhPump(&phPump);          // J active immediately; A off until setCrossSettleMinutes
  clPump.setCrossSettleMinutes(15);   // A: hold CL 15 min after each pH dose
  ```

- **`getLastDosingEnd()`** — new public getter returning `lastDosingEnd` (millis() when last dose
  ended; 0 if never dosed). Used internally by the linked CL pump for Option A; also available
  for display or logging in user sketches.

### Changed

- **`SHOCK_PH_MIN` / `SHOCK_PH_MAX` renamed to `CL_PH_MIN` / `CL_PH_MAX`** — same values (7.0 / 7.6),
  same purpose. The old names implied shock-only; both shock and Option J enforce this pH range
  for the same chemical reason. No behaviour change in `triggerShock()`.

---

## [3.10.1] — 2026-05-20

### Fixed

- **API.md — priming rest period**: Removed incorrect claim that a minimum 5-minute rest is imposed after `triggerPrime()`. No rest period is imposed; normal dosing resumes immediately.
- **API.md — `setPoolVolume()` / `setDeadbandPct()` ordering**: Corrected examples to show both calls after `begin()`. On ESP8266/ESP32, calling them before `begin()` silently discards the values because `EEPROM.begin()` has not run yet. Both functions may also be called from `loop()` at runtime.
- **API.md — Setup Order table**: Added step 8 for `ApaDose::setPoolVolume()` and `ApaDose::setDeadbandPct()`.
- **API.md — `triggerManualDose()` return conditions**: Replaced incomplete 4-condition list with full 7-condition table matching the source code.
- **API.md — `CL_PLUS` constant**: Added `constexpr CL_PLUS` to the `ApaDoseDirection` section with explanation that it is an alias for `PH_PLUS`, not an enum member.
- **API.md — Quick Start `filterRunning()`**: Added missing `== HIGH` comparison.
- **API.md — EEPROM Layout write method**: Corrected `` `EEPROM.write()` `` to `` `EEPROM.put()` ``.
- **README.md — build table**: Updated flash/SRAM figures for all 5 platforms with actual measurements from example 02; corrected "~15 KB flash" feature bullet to "~17 KB flash".
- **Examples 01–04, 06–10**: Added commented-out `ApaDose::setPoolVolume()` block (and `setDeadbandPct()` for two-pump sketches) consistent with example 02.
- **keywords.txt**: Added `ApaDoseDirection`, 17 missing methods (`triggerShock`, `factoryReset`, `setPhDirection`, `getPhDirection`, `setPoolVolume`, `getPoolVolume`, `setDeadbandPct`, `getDeadbandPct`, `enableAdaptivePB`, `isAdaptivePBEnabled`, `isShockActive`, `getShockRemainingSeconds`, `getAdaptedPB`, `setExternalStopCallback`, `isExternalStopActive`, `isInExternalStopResumeDelay`, `isOutsideDosingWindow`), and 8 missing constants (`DOSE_PH`, `PH_PLUS`, `PH_MINUS`, `CL_PLUS`, `SHOCK_ORP_MILD`, `SHOCK_ORP_STANDARD`, `SHOCK_ORP_AGGRESSIVE`, `MAX_MANUAL_DOSE_MS`). Removed stale `DOSE_PH_PLUS` and `DOSE_PH_MINUS` (never existed in current API).

---

## [3.10.0] — 2026-05-20

### Added

- **Pool volume scaling** (`ApaDose::setPoolVolume(m3)`) — scales pulse duration, rest period, feedback
  pulse cap (`FEEDBACK_PULSE_MAX_MS`), and shock ORP rise window (`SHOCK_RISE_CHECK_MS`) proportionally
  to pool size. Reference: 20 m³. Valid range: 10–90 m³; 0 = off (backward compatible default).

  Without this, pools above ~30 m³ cannot converge to setpoint because every dose is too short relative
  to the water volume. With it, the same controller works correctly from a 10 m³ spa to a 90 m³ pool.

  ```cpp
  ApaDose::setPoolVolume(35);  // 35 m³ — scale 1.75×; call once in setup()
  ```

  - Saved to a 3-byte global EEPROM slot (addresses 189–191), outside per-instance `ConfigData`
  - **Survives `factoryReset()`** — pool size is a physical installation fact, not a tuning parameter
  - Clear explicitly with `ApaDose::setPoolVolume(0)` if needed
  - Affects: `calculateProportionalPulse()` (duration + rest), `applyFeedbackCorrections()` (cap), `manageShock()` (rise window)
  - Does NOT affect: `INTER_PUMP_LOCKOUT_MS`, `blackoutMinutes`, `triggerManualDose()`, `triggerPrime()`, PWM intensity

- **Dead-band** (`ApaDose::setDeadbandPct(pct)`) — suppresses proportional dosing when the sensor error
  is within a configurable percentage of the proportional band. Reduces unnecessary pump cycles when the
  pool is already close to setpoint.

  Unit is **% of proportional band** — dimensionless and type-agnostic. 10% on a pH pump (PB = 1.0)
  means ±0.10 pH; 10% on a chlorine pump (PB = 100 mV) means ±10 mV. The same number applies correctly
  to both types.

  ```cpp
  ApaDose::setDeadbandPct(10);  // 10 % — call once in setup()
  ```

  **Asymmetric hysteresis** prevents oscillation when the sensor straddles the boundary:
  - 0%: disabled (default)
  - 1–5%: symmetric (entry = exit)
  - 6–20%: exit threshold = entry − 5 percentage points; pump only resumes once error grows past the entry threshold again

  - Saved to the same global EEPROM slot as pool volume (no extra EEPROM cost)
  - **Cleared by `factoryReset()`** — dead-band is a tuning parameter; factory reset is the reliable escape hatch if dosing misbehaves after dead-band is set
  - Gate is in `shouldStartDosing()` — prevents wasted before-dose sensor sampling when within dead-band

### Fixed

- **`millis()` 49-day rollover** — six absolute timestamp comparisons converted to the
  rollover-safe signed-cast pattern `(long)(now - deadline) >= 0`. Affected paths:
  post-shock cooldown check in `update()`, post-shock cooldown guard in `triggerShock()`,
  and all three `FB_WAITING` / `FB_MEASURING_BEFORE` / `FB_MEASURING_AFTER` comparisons
  in the feedback sampling state machine.

- **SRAM reduction (−4 bytes per instance)** — `FeedbackState.feedbackCheckTime` field
  removed. The FB_WAITING deadline is now stored directly in `nextSampleTime`, which is
  structurally idle during that phase (both `FB_MEASURING_BEFORE` and `FB_MEASURING_AFTER`
  phase guards prevent it from being read). `startAfterDosingMeasurements()` naturally
  overwrites it when the after-measurement phase begins.

---

## [3.9.0] — 2026-05-20

### Added

- **Shock / super-chlorination mode** — `triggerShock()` doses the chlorine pump at full power
  continuously until ORP reaches a target value or a time ceiling expires, then automatically
  returns to normal proportional control. Designed for use after heavy bather load, algae
  treatment, storms, or any event that requires restoring chlorine faster than proportional
  dosing can achieve.

  Two overloads — call from any button, RTC schedule, or automation logic:

  ```cpp
  // Hobbyist — sensible defaults (max 4 h, 24 h cooldown)
  clPump.triggerShock(SHOCK_ORP_STANDARD, phPump.getProbeValue());

  // Pro — full control over duration and post-shock settling window
  clPump.triggerShock(780, 3, phPump.getProbeValue(), 48);
  //                  ^targetORP  ^maxHours  ^currentPH  ^cooldownHours
  ```

  Named ORP presets in `APADOSE.h` so users never need to remember raw mV values:

  | Constant | Value | When to use |
  |----------|-------|-------------|
  | `SHOCK_ORP_MILD` | 700 mV | Light event — post-rain, minor algae risk |
  | `SHOCK_ORP_STANDARD` | 750 mV | Weekly maintenance shock |
  | `SHOCK_ORP_AGGRESSIVE` | 800 mV | Heavy algae, high bather load |

  **New entry guards** — `triggerShock()` returns `false` (does nothing) when:
  - Called on a `DOSE_PH` instance (shock is chlorine-only)
  - No filter callback registered, or filter is currently off
  - External stop callback returns `true`
  - An alarm is active
  - A dose or prime is already running
  - `currentPH` is outside 7.0–7.6 (range where chlorine is most effective)
  - `targetORP` is outside 600–800 mV
  - `sensorValue` is already at or above `targetORP`
  - Post-shock cooldown has not yet elapsed (inter-shock interval guard)

  **Shock execution:**
  - Pump runs at `pumpMaxPWM` (continuous, not proportional) for up to `maxDurationHours` (default 4 h; clamped to 4 h maximum)
  - Stops early when ORP reaches `targetORP × 0.90` — a 10% early-stop margin compensates for chlorine mixing lag (ORP continues rising after dosing stops)
  - At 20 minutes: checks that ORP has risen at least 20 mV; if not, aborts with `ALARM_INEFFECTIVE` — catches an empty container or failed pump before wasting 4 hours
  - Aborts immediately if an alarm fires, the filter stops, or external stop activates

  **Inter-pump interlock:**
  - While shock is active on `clPump`, all other `ApaDose` instances (`phPump`, `flocPump`, …) are held — `"Held:shock active"` fires once per hold period
  - When shock ends all instances resume automatically — `"Dosing resumed"` fires

  **Post-shock cooldown:**
  - After shock completes, the safety band alarm is suppressed for the cooldown window (default 24 h, max 48 h) — ORP remains elevated while chlorine reacts, which would otherwise trigger a false `ALARM_SAFETY_BAND`
  - When the cooldown window expires, `"Post-shock normal"` fires and normal safety monitoring resumes
  - **With RTC:** cooldown uses wall-clock time and survives power cycles
  - **Without RTC:** cooldown uses `millis()` and resets on power cycle (acceptable for no-RTC installs)
  - `dailyDoseCount` is **not** incremented (shock bypasses the daily limit — it is an operator intervention, not automatic dosing); `dailyVolumeMl` **is** accumulated for chemical cost tracking

  **New public API:**

  ```cpp
  bool          triggerShock(uint16_t targetORP, float currentPH,
                             uint8_t cooldownHours = SHOCK_COOLDOWN_DEFAULT_HOURS);
  bool          triggerShock(uint16_t targetORP, uint8_t maxDurationHours, float currentPH,
                             uint8_t cooldownHours = SHOCK_COOLDOWN_DEFAULT_HOURS);
  bool          isShockActive()            const;  // true while shock dosing is running
  unsigned long getShockRemainingSeconds() const;  // seconds to time ceiling; 0 if not active
  ```

  **New constants in `APADOSE.h`:**

  | Constant | Value | Purpose |
  |----------|-------|---------|
  | `SHOCK_PH_MIN` | 7.0 | Minimum pH required before shock is permitted |
  | `SHOCK_PH_MAX` | 7.6 | Maximum pH; above this chlorine efficiency drops sharply |
  | `SHOCK_ORP_MIN` | 600 mV | Minimum allowed ORP target |
  | `SHOCK_ORP_MAX` | 800 mV | Maximum allowed ORP target (library's hard ceiling) |
  | `SHOCK_OVERSHOOT_MARGIN` | 0.10 | 10% early-stop margin for Cl mixing lag |
  | `SHOCK_MAX_DURATION_HOURS` | 4 | Hard ceiling on active shock dosing time |
  | `SHOCK_COOLDOWN_DEFAULT_HOURS` | 24 | Default post-shock safety band suppression window |
  | `SHOCK_COOLDOWN_MAX_HOURS` | 48 | Maximum cooldown; larger values silently clamped |
  | `SHOCK_RISE_CHECK_MS` | 20 min | ORP rise check window at shock start |
  | `SHOCK_RISE_MIN_MV` | 20 mV | Minimum ORP rise expected within rise check window |
  | `SHOCK_ORP_MILD` | 700 mV | Named preset — light shock |
  | `SHOCK_ORP_STANDARD` | 750 mV | Named preset — weekly maintenance |
  | `SHOCK_ORP_AGGRESSIVE` | 800 mV | Named preset — heavy algae / bather load |

  **SRAM cost:** 21 bytes per `DOSE_CL` instance + 1 byte shared static flag.  
  `DOSE_PH` instances pay no SRAM cost — members are present but `triggerShock()` exits immediately if called on a pH pump.

  **See also:** `examples/basic/02_ph_and_cl/` (hobbyist shock call with button trigger),
  `examples/advanced/05_multi_pump/` (pro shock call with RTC-based cooldown and serial feedback).

- **`factoryReset()` now stops active shock** — if a shock is in progress when `factoryReset()` is called, it is stopped cleanly before resetting all EEPROM fields. The post-shock cooldown is also cleared (full clean slate).

### Changed
- **Flags bitfield expanded from 14 to 16 flags** — still occupies 2 bytes exactly. Added: `shockActive` (this instance is running shock mode), `shockHoldSent` (rate-limits `"Held:shock active"` on non-shock instances during inter-pump hold).

### Removed
- **Post-prime rest period** — after `triggerPrime()` completes, no 5-minute rest is imposed before the next automatic dose or prime. Priming fills dry pipe only — it carries no chemical into the pool and needs no mixing wait. Previously the 5-minute guard prevented consecutive primes (e.g. a long-run outdoor installation requiring multiple priming passes), making the commissioning workflow unusable. The rest period after a proportional dose is unaffected.

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
