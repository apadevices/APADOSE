# APA-Dose Library — API Reference

**Version**: 3.8.3  
**File**: `APADOSE.h` / `APADOSE.cpp`

---

## Quick Start — pH pump

```cpp
#include "APADOSE.h"

ApaDose phPump(PIN_PH_PUMP);

float getpH()         { return phSensor.getPH(); }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY); }

void onAlarm(ApaDoseAlarm alarm, const char* msg) { Serial.println(msg); }

void setup() {
  phPump.setPumpRange(65, 255);
  phPump.setCallbacks(onAlarm);                                    // register callbacks BEFORE begin()
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20);  // sensor + filter + type + dir + 20 min blackout
}

void loop() {
  phPump.update();
}
```

## Quick Start — sensor-less pump (floc / algaecide)

```cpp
ApaDose flocPump(PIN_FLOC_PUMP);

void setup() {
  flocPump.setPumpRange(65, 255);
  flocPump.setCallbacks(onAlarm);
  flocPump.begin(nullptr, filterRunning, DOSE_PH, PH_PLUS, 0, 1);  // no sensor, max 1 dose/day
}

// dose manually from your schedule logic:
void doFlocDose() {
  flocPump.triggerManualDose(30000);  // 30 s at full speed
}
```

With `nullptr` sensor: proportional dosing, feedback, and sensor alarms are all disabled. The filtration interlock, daily dose limit, and alarm system remain fully active.

---

## Quick Start — chlorine pump

```cpp
ApaDose clPump(PIN_CL_PUMP);

float getORP() { return orpSensor.getORP(); }

void setup() {
  clPump.setPumpRange(65, 255);
  clPump.setCallbacks(onAlarm);
  clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20);
}
```

---

## Setup Order

Call setup methods in this order:

```
1. setPumpRange()               calibrate motor dead band
2. setPumpFlowRate()            optional — pump output at max PWM in mL/min (default 450)
3. setRTCCallback()             optional — RTC for scheduling
4. setDosingWindow()            optional — restrict dosing hours
5. setExternalStopCallback()    optional — block dosing from external systems (maintenance, backwash…)
6. setCallbacks()               register alarm/status callbacks BEFORE begin()
7. begin()                      connect sensor + type + start library
```

`setCallbacks()` must come before `begin()` to receive the startup blackout message and any boot-time status output.

Dosing type is now passed directly to `begin()` — no separate `setDosingType()` call is needed during setup. Use `setDosingType()` only for runtime type changes after `begin()` has run.

---

## Constructor

```cpp
ApaDose(uint8_t pumpPin, int eepromAddress = APA_DOSE_EEPROM_ADDRESS);
```

One instance per pump. Name it after the chemical it controls: `phPump`, `clPump`, `flocPump`.  
Single hardware pin per pump, switched through a MOSFET (`analogWrite` only).

`eepromAddress` is the EEPROM base address where this instance stores its configuration.  
Single-pump sketches can omit it — the default (192) is used.  
Multi-pump sketches **must** pass a unique address per pump; space them by `sizeof(ConfigData)` bytes (20 bytes on all platforms):

```cpp
ApaDose phPump  (PIN_PH_PUMP);                                            // EEPROM 192 (default)
ApaDose clPump  (PIN_CL_PUMP,   APA_DOSE_EEPROM_ADDRESS +     sizeof(ConfigData));  // EEPROM 212
ApaDose flocPump(PIN_FLOC_PUMP, APA_DOSE_EEPROM_ADDRESS + 2 * sizeof(ConfigData));  // EEPROM 232
ApaDose algiPump(PIN_ALGI_PUMP, APA_DOSE_EEPROM_ADDRESS + 3 * sizeof(ConfigData));  // EEPROM 252
```

> The constant `APA_DOSE_EEPROM_ADDRESS` (192) is defined in `APADOSE.h`. The companion
> APAPHX2_ADS1115 sensor library occupies addresses 128–177. Addresses 178–191 are a safety gap.

---

## Setup Methods

### `setPumpRange()`
```cpp
void setPumpRange(uint8_t minPWM, uint8_t maxPWM);
```
Calibrates the PWM range for this specific pump motor.
- `minPWM` — PWM level where the motor actually starts spinning (measure for your pump)
- `maxPWM` — maximum allowed PWM, usually `255`
- Default if not called: `minPWM = 50`, `maxPWM = 255`
- `minPWM > maxPWM` is rejected silently.

A 10% minimum floor above `minPWM` is applied to every dose to overcome pipe and hose resistance.

**Solenoid / time-proportional mode:** setting `minPWM == maxPWM` (e.g. `255, 255`) locks PWM at that fixed level. Proportionality then comes entirely from pulse duration — the solenoid opens fully for 2–11 s scaled to the error percentage. All feedback, safety, and alarm systems function identically in this mode.

---

### `setPumpFlowRate()`
```cpp
void setPumpFlowRate(float mlPerMin);
```
Sets the pump's output rate at maximum PWM, used to calculate dosed volume.

- `mlPerMin` — measured flow rate in mL/min at max PWM (must be > 0; ignored otherwise)
- Default if not called: `450.0` mL/min
- Call before `begin()`, after `setPumpRange()`

Volume per dose is estimated as: `(pwm / 255) × (mlPerMin / 60000) × actual_duration_ms`. The PWM scaling assumes flow is roughly proportional to speed — adequate for peristaltic pumps. For solenoids, set flow rate to the fully-open value and use `setPumpRange(255, 255)`.

Enables `getDailyVolumeMl()` and `getLastDoseVolumeMl()`. If `setPumpFlowRate()` is never called the default (450 mL/min) is used and volume values are still returned — just less accurate for pumps that differ significantly from that rate.

---

### `setDosingType()`
```cpp
bool setDosingType(ApaDoseType newType);
```
Changes the chemical type at runtime and loads the matching sensor profile (pH or ORP).

Use this for runtime type changes **after `begin()` has run** — for example, switching from `DOSE_PH` to `DOSE_CL`. To change pH pump direction (acid ↔ base) at runtime, use `setPhDirection()` instead. The type and direction for initial setup are passed as parameters to `begin()`.

If the current setpoint or band fall outside the new type's valid range, they are automatically reset to that type's defaults and saved to EEPROM.

Returns `false` if dosing is currently active.

---

### `setRTCCallback()`
```cpp
void setRTCCallback(RTCReadCallback rtcReader);
```
Connects an external RTC. When registered:
- The daily dose counter resets automatically when the day changes.
- Dosing windows (`setDosingWindow()`) become active.

---

### `setDosingWindow()`
```cpp
void setDosingWindow(uint8_t startHour, uint8_t endHour);
```
Restricts dosing to a time window. `startHour` inclusive, `endHour` exclusive (both 0–23).  
Example: `setDosingWindow(8, 20)` allows dosing 08:00–19:59 only.  
Has no effect without an RTC callback registered.

---

### `setExternalStopCallback()`
```cpp
void setExternalStopCallback(ExternalStopCallback cb);
```
Registers an optional callback that blocks all dosing (automatic and manual) while it returns `true`. Priming (`triggerPrime()`) is not affected.

Typical use cases: pool maintenance mode, backwash cycle, pool cover closed, or any external condition where injecting chemistry into a non-circulating or diverted water flow is unsafe.

**Call before `begin()`.**

```cpp
bool maintenanceMode = false;
bool backwashRunning = false;

bool dosingBlocked() {
  return maintenanceMode || backwashRunning;
}

phPump.setExternalStopCallback(dosingBlocked);
```

**Behaviour:**

| Condition | Action |
|-----------|--------|
| Callback returns `true` while a dose is running | Pump stops immediately; `"Stop:ext request"` sent via `onStatusMessage` |
| Callback returns `true`, no dose running | `"ExtStop active"` sent once; new doses and manual triggers blocked |
| Callback returns `false` after being `true` | `"ExtStop cleared"` sent; **5-minute settling time begins** |
| Settling time in progress | Dosing and manual triggers still blocked; no repeated messages |
| Settling time expires | `"Dosing resumed"` sent; normal dosing resumes |
| Callback returns `true` again during settling | Settling timer resets; full stop re-applies |

The 5-minute settling time (`EXTERNAL_STOP_RESUME_MS`) is hardcoded. It prevents a brief dose from firing while an operator is still switching between filtration modes or while water is still flowing through a diverted outlet.

`isExternalStopActive()` returns the cached state evaluated by the last `update()` call.

---

### `setCallbacks()`
```cpp
void setCallbacks(AlarmCallback alarmTriggered,
                  AlarmCallback alarmCleared   = nullptr,
                  StatusCallback statusMessage = nullptr);
```
All parameters optional. **Call before `begin()`** to receive startup messages.

---

### `begin()`
```cpp
bool begin(SensorReadCallback sensorReader, FilterCallback filter, ApaDoseType type, ApaDoseDirection dir, uint8_t blackoutMinutes = 0, uint8_t maxDailyDoses = 0);
bool begin(SensorReadCallback sensorReader, ApaDoseType type, ApaDoseDirection dir, uint8_t blackoutMinutes = 0, uint8_t maxDailyDoses = 0);
```

Connects the library to external hardware and activates EEPROM restore.

| Parameter | Description |
|-----------|-------------|
| `sensorReader` | Function returning the current sensor value (pH float or ORP mV float). Must match the pump's `dosingType` — see Callback Signatures for details. Pass `nullptr` for sensor-less pumps (floc, algaecide) — manual dosing only, filtration interlock still active. |
| `filter` | Function returning `true` when filtration pump is running. Omit if unavailable. |
| `type` | Chemical type: `DOSE_PH` or `DOSE_CL`. Always takes precedence over the EEPROM-stored value. If type changed since last boot, setpoint and band are clamped to the new type's valid range and re-saved. |
| `dir` | pH dosing direction: `PH_PLUS` (base, raises pH) or `PH_MINUS` (acid, lowers pH). For `DOSE_CL` pumps use `CL_PLUS` (an alias for `PH_PLUS`) — direction is stored but not used in chlorine control logic. Takes precedence over the EEPROM-stored direction. |
| `blackoutMinutes` | Block dosing for N minutes after boot (0 = disabled, max 60). Values above 60 clamped. |
| `maxDailyDoses` | Maximum automatic+manual doses per day (0 = no limit). Triggers `ALARM_DAILY_LIMIT` when reached. Not saved to EEPROM — set in code each boot. Counter resets every 24 h automatically — with RTC at real midnight, without RTC every 24 h from boot. |

**Usage examples:**
```cpp
phPump.begin(getpH, DOSE_PH, PH_MINUS);                           // sensor + type + dir, no limits
phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS);            // + filter
phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20);        // + 20 min blackout
phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6);     // + max 6 doses/day
clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20, 12);    // chlorine pump
```

---

## Main Loop

```cpp
void update();
```
Call every `loop()` iteration. Never blocks. Runs the full state machine: sensor read, dosing control, feedback evaluation, alarm checks.

---

## Configuration

All changes are immediately saved to EEPROM.

```cpp
bool setProbeSetpoint(float value);         // pH: 6.8–7.8 default 7.4 | ORP: 400–850 default 700
bool setProportionalBand(float value);      // pH: 0.5–2.0 default 1.0 | ORP: 50–250 default 100
bool setDosingType(ApaDoseType type);       // runtime type change (DOSE_PH / DOSE_CL); set via begin() on startup
bool setPhDirection(ApaDoseDirection dir);  // runtime direction change (PH_PLUS / PH_MINUS); DOSE_PH only
void enableAdaptivePB(uint8_t nudgePct);    // 0 = disable (forgets learned value); 1–25 = nudge rate % per cycle
void forceConfigurationSave();              // write to EEPROM immediately
void acknowledgeAlarm();                    // clears alarms that require user acknowledgment
void factoryReset();                        // force-stop any active dose/prime, reset all EEPROM fields to type-defaults, clear alarm, save
```

`setProbeSetpoint()` and `setProportionalBand()` return `false` if the value is out of range or dosing is currently active.

### `factoryReset()`
```cpp
void factoryReset();
```
Resets all EEPROM-stored user settings to their type-default values in a single call.

**Sequence:**
1. If a dose is active — stops the pump immediately (same as a mid-dose filter dropout).
2. If priming is active — stops the pump immediately.
3. If an alarm is active — clears it (fires `onAlarmCleared` if registered).
4. Resets the five EEPROM fields to defaults for the current dosing type:

| Field | After reset |
|-------|-------------|
| `setpoint` | pH 7.4 / ORP 700 mV |
| `proportionalBand` | pH 1.0 / ORP 100 mV |
| `phDirection` | `PH_PLUS` |
| `nudgePct` | 0 (adaptive PB disabled) |
| `adaptedPB` | 0.0f (learned value discarded) |

5. Saves the reset values to EEPROM.
6. Sends `"Factory reset"` via `onStatusMessage`.

`dosingType` is not touched — it is always overridden by the `begin()` parameter on the next boot.

```cpp
// Example: factory reset on long button press
if (longPress())
  phPump.factoryReset();
```

---

## Manual Control

```cpp
bool triggerManualDose(unsigned long durationMs,
                       unsigned long restMs = 20UL * 60UL * 1000UL);  // 0 = no rest
bool triggerPrime(unsigned long durationMs, uint8_t pwm = 0);          // 0 = use pumpMaxPWM
```

| Method | Speed | Respects alarms | Runs feedback | Counts as dose |
|--------|-------|-----------------|---------------|----------------|
| `triggerManualDose()` | `pumpMaxPWM` | Yes — blocked if alarm active | No | Yes |
| `triggerPrime()` | configurable | No — bypasses all guards | No | No |

`triggerManualDose()` returns `false` if dosing or priming is already active, `durationMs` is 0, the filter is not running, or the daily dose limit has been reached.  
If `durationMs` exceeds `MAX_MANUAL_DOSE_MS` (5 minutes), it is silently clamped and a `"Dose capped:5min"` status message is sent — the dose still runs at the capped duration.  
The optional `restMs` parameter (default 20 min) sets the mixing wait before the next proportional dose. Pass `0` only for sensor-less pumps where `maxDailyDoses` already prevents double-dosing.

`triggerPrime()` returns `false` if dosing or priming is already active, or if `durationMs` is 0.

`triggerPrime()` is for filling the pipe on first install — it ignores alarms, filter state, blackout, and dosing window. The optional `pwm` parameter lets you prime at a lower speed to protect dry joints; passing `0` (default) runs at `pumpMaxPWM`. The value is clamped to `[pumpMinPWM, pumpMaxPWM]`.

```cpp
pump.triggerPrime(10000);       // 10 s at full speed (default)
pump.triggerPrime(10000, 100);  // 10 s at PWM 100 — gentle fill on a dry pipe
```

After priming completes the library applies a minimum 5-minute rest before the next proportional dose. If a previous proportional dose established a longer rest period, that period is honored instead. This prevents the controller from firing immediately after a prime that pushed chemical into the pool.

---

## Status Queries

```cpp
float            getProbeValue()                const;  // current sensor reading (pH or ORP mV) — "probe" = pool chemical electrode
float            getCurrentSetpoint()           const;
float            getCurrentProportionalBand()   const;
ApaDoseType      getCurrentDosingType()         const;  // DOSE_PH or DOSE_CL
ApaDoseDirection getPhDirection()               const;  // PH_PLUS or PH_MINUS; always PH_PLUS for DOSE_CL
ApaDoseAlarm     getCurrentAlarm()              const;
const char*   getAlarmMessage()                 const;  // alarm text; empty string if no alarm
bool             isAlarmActive()                const;
bool             isDosingActive()               const;
bool             isPrimingActive()              const;
bool             isInStartupBlackout()          const;
bool             isExternalStopActive()         const;  // true if external stop was active at last update()
bool             isInExternalStopResumeDelay()  const;  // true during the mandatory 5-min settling wait after external stop clears
bool             isOutsideDosingWindow()        const;  // true if dosing window enabled and current hour is outside it (requires RTC callback)
bool             isConfigurationValid()         const;
unsigned long    getLastDosingTime()            const;  // millis() when last dose ended
unsigned long    getSecondsSinceLastDose()      const;  // seconds elapsed since last dose ended; 0 if no dose yet
unsigned long    getSecondsUntilNextDose()      const;  // seconds remaining in rest period; 0 if eligible now
uint8_t          getFailedAttempts()            const;  // consecutive ineffective doses
uint8_t          getDailyDoseCount()            const;  // doses today; resets only with RTC connected
uint8_t          getMaxDailyDoses()             const;  // ceiling set in begin(); 0 = no limit
float            getDailyVolumeMl()             const;  // total mL dosed today; resets at midnight with RTC
float            getLastDoseVolumeMl()          const;  // mL dosed in the last completed dose
float            getAdaptedPB()                 const;  // current effective PB: learned value when adaptive enabled, fixed proportionalBand otherwise
bool             isAdaptivePBEnabled()          const;  // true when nudgePct > 0
```

These three queries cover all internal blocking states that are not derivable from the user's own callbacks:

| Query | Returns `true` when… | Requires |
|-------|----------------------|----------|
| `isInStartupBlackout()` | Still within the startup delay after boot | non-zero `blackoutMinutes` in `begin()` |
| `isExternalStopActive()` | External stop callback currently returns `true` | `setExternalStopCallback()` |
| `isInExternalStopResumeDelay()` | External stop cleared but 5-min settling not yet elapsed | `setExternalStopCallback()` |
| `isOutsideDosingWindow()` | Dosing window enabled and current hour is outside it | `setDosingWindow()` + RTC callback |

`getDailyDoseCount()` increments on every dose (manual or automatic). Reset behaviour depends on whether an RTC callback is registered:

| RTC registered | Reset trigger |
|----------------|---------------|
| Yes | When the RTC reports a new calendar day (real midnight) |
| No | Every 24 hours from the last reset (starting from `begin()`) |

Without an RTC the reset is not synchronized to clock time — it occurs 24 hours after boot, then every 24 hours thereafter. For dose-limit safety purposes this is equivalent: the counter never accumulates indefinitely.

`getDailyVolumeMl()` follows the same reset schedule as `getDailyDoseCount()`. `getLastDoseVolumeMl()` always reflects the last completed dose regardless of RTC. Both values are 0.0 until the first dose completes.

```cpp
// Example: print daily consumption summary
Serial.print(F("Today: "));
Serial.print(phPump.getDailyDoseCount());
Serial.print(F(" doses, "));
Serial.print(phPump.getDailyVolumeMl(), 1);
Serial.println(F(" mL"));
```

`getSecondsUntilNextDose()` returns `0` when the rest period has elapsed — it does not guarantee a dose will fire immediately. Other conditions (filter off, alarm, external stop, outside dosing window) can still block dosing and are queryable via the existing state getters.

```cpp
// Example: dashboard status line
if (phPump.isAlarmActive()) {
  Serial.println(F("ALARM"));
} else if (phPump.getSecondsUntilNextDose() > 0) {
  Serial.print(F("Rest: "));
  Serial.print(phPump.getSecondsUntilNextDose() / 60);
  Serial.println(F(" min"));
} else {
  Serial.println(F("Ready"));
}
```

### Inter-pump chemical lockout

After any `ApaDose` instance completes a dose, **all instances** wait 90 seconds before the next automatic or manual dose can start. The timer is stored in a `static` variable shared across all instances.

This prevents back-to-back injection of incompatible chemicals — for example, acid (pH-) dosed immediately before chlorine at the same inlet can release chlorine gas. The 90-second window is long enough for residual chemical to clear a typical pool return line.

`triggerPrime()` is exempt — priming fills tubing before any chemical enters the water and does not reset the lockout timer.

---

### Sensor value validation

The library validates every sensor reading with `isfinite()`.

> **Temperature compensation:** pH sensor readings are temperature-dependent (~0.003 pH/°C). The **APAPHX** and **APAPHX2** libraries apply the Passco 2001 formula to deliver a stable, temperature-compensated value automatically. If using a different sensor library, apply temperature compensation inside your sensor callback so APA-Dose always receives a corrected reading. If the callback returns NaN or infinity:
- The first bad reading sends `"Sensor:bad value"` once via `onStatusMessage`
- The last known good value is kept — dosing and safety checks continue against it
- The flag clears automatically when a finite value is received; the message fires again on the next transition to bad
- Any feedback sample that is non-finite is silently skipped and rescheduled — the average is never corrupted by a bad read

This protects against transient sensor glitches and disconnected probes that return rail voltages through the calibration math.

### Stale sensor timeout

If the sensor callback returns only non-finite values for `SENSOR_STALE_MS` (30 minutes) without recovery, automatic dosing is suspended and a single `"Sensor:stale>30min"` message is sent via `onStatusMessage`. Dosing resumes automatically as soon as the next finite reading arrives — no `acknowledgeAlarm()` required, no alarm is raised.

This guards against a cable fault or ADC power loss that freezes `sensorValue` at a stale reading while allowing the pump to dose indefinitely against it. The 30-minute window is intentionally longer than `FILTER_OFF_ALARM_MS` so a simultaneous filter failure is noticed first.

---

### Filter-off notification

When a `FilterCallback` is registered, the library tracks continuous filter-off time using `millis()`. After `FILTER_OFF_ALARM_MS` (30 minutes) of uninterrupted filter-off, a single `"Filter off>30min"` message is sent via `onStatusMessage`. The timer resets as soon as the filter comes back on, so the message fires again if the filter goes off for another 30-minute stretch. No RTC is required — timing is purely `millis()`-based.

---

### External stop

When an `ExternalStopCallback` is registered via `setExternalStopCallback()`, the callback is evaluated every `update()` cycle. While it returns `true`, no dose starts and any active auto or manual dose is stopped immediately. Priming is exempt. See `setExternalStopCallback()` for the full behaviour table and example.

---

## Dose diagnostics

Available after the first complete proportional dose + feedback cycle.  
Manual doses and priming do not populate these values.

```cpp
bool  hasDoseHistory()           const;  // false until first full dose+feedback cycle
float getLastDoseSensorBefore()  const;  // averaged sensor value before last dose
float getLastDoseSensorAfter()   const;  // averaged sensor value after last dose
float getDoseEffectiveness()     const;  // signed % of band (see below)
```

`getDoseEffectiveness()` returns a signed percentage of `proportionalBand`:

| Value | Meaning |
|-------|---------|
| `+100%` | Sensor moved by exactly one full band — excellent dose |
| `+20%`  | Sensor moved 20% of band — mild but correct direction |
| `0%`    | No measurable change |
| `-10%`  | Sensor moved in wrong direction (10% of band) |

The sign is normalized: **positive always means correct direction** regardless of pump type.  
For `DOSE_PH` pumps with `PH_MINUS` direction the raw change is sign-flipped internally so a falling pH still returns a positive number.

```cpp
if (phPump.hasDoseHistory()) {
  Serial.print("Before: ");  Serial.println(phPump.getLastDoseSensorBefore());
  Serial.print("After:  ");  Serial.println(phPump.getLastDoseSensorAfter());
  Serial.print("Effect: ");  Serial.print(phPump.getDoseEffectiveness()); Serial.println("%");
}
```

---

## Adaptive Proportional Band

The adaptive PB feature lets the library learn the pool's chemical response over time and automatically adjust the control band for better accuracy. It is **disabled by default** — fixed `proportionalBand` is used.

### How it works

After each proportional dose + feedback cycle, the library compares the actual sensor shift to the expected shift (setpoint − reading before dose). If the chemical moved the sensor **more than expected**, the band is widened so future pulses are shorter. If it moved **less than expected**, the band is narrowed so future pulses are longer. Only "effective" doses — correct direction, above feedback threshold — trigger a nudge.

The learned band (`adaptedPB`) is clamped to `[0.2 × proportionalBand, 3.0 × proportionalBand]` and saved to EEPROM after every adjustment. On `setProportionalBand()` the fixed band updates immediately; the learned band scales proportionally on the next nudge. On `enableAdaptivePB(0)` the learned value is discarded — the next `enableAdaptivePB(n)` seeds from the current fixed band.

### API

```cpp
void  enableAdaptivePB(uint8_t nudgePct);  // 0=disable; 1–25 = nudge rate % per cycle
float getAdaptedPB()        const;          // current effective PB (learned or fixed)
bool  isAdaptivePBEnabled() const;          // true when nudgePct > 0
```

`nudgePct` is clamped to 25. A value of 5 (5% per cycle) is a reasonable starting point — it converges in ~10–20 cycles without overshooting.

### Usage

```cpp
void setup() {
  phPump.setPumpRange(65, 255);
  phPump.setCallbacks(onAlarm);
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6);
  phPump.enableAdaptivePB(5);  // learn at 5% per cycle
}

// Read back in loop() or on a timer:
Serial.print(F("Effective band: "));
Serial.println(phPump.getAdaptedPB());
```

Adaptive PB is per-instance — each pump learns independently. It applies to proportional dosing only; manual doses are sized by the same effective band but do not trigger a nudge.

---

## Diagnostics

```cpp
void getSystemStatus(char* buffer, size_t bufferSize) const;  // human-readable snapshot
static const char* getVersion();      // returns version string, e.g. "3.4.1"
static void        printLibraryInfo(); // prints "APA-Dose vX.Y.Z" + "APA Devices" to Serial
```

`getSystemStatus()` writes a single-line summary: sensor value, setpoint, band, type, dosing state, and current alarm. Use `APA_DOSE_STATUS_BUFFER_SIZE` (96) as the minimum buffer size:

```cpp
char buf[APA_DOSE_STATUS_BUFFER_SIZE];
phPump.getSystemStatus(buf, sizeof(buf));
Serial.println(buf);
// Sensor:7.42 SP:7.40 Band:1.00 Type:pH- Dosing:NO Alarm:NONE
```

---

## Debug output (`APA_DOSE_DEBUG`)

Define `APA_DOSE_DEBUG` before the include, or set it as a build flag, to enable per-cycle diagnostic messages routed through the `StatusCallback`:

```ini
; platformio.ini
build_flags = -D APA_DOSE_DEBUG
```

When enabled, the status callback receives additional messages for each stage of the dosing cycle:

| Message | When |
|---------|------|
| `B1/2:7.42` | Before-dose sample N of M: reading |
| `Bavg:7.42` | Averaged before-dose value |
| `A1/3:7.39` | After-dose sample N of M: reading |
| `Aavg:7.39` | Averaged after-dose value |
| `Err:60% P:180 D:8` | Error %, calculated PWM, pulse duration (s) |
| `Dose pH- P:180` | Dose started: type and PWM |
| `Corr attempt 2` | Feedback correction applied |
| `Prime P:255` | Priming started at PWM level |
| `Sampling before...` | Before-dose sampling phase started |
| `Sampling after...` | After-dose sampling phase started |

Disabled by default — saves ~200 bytes of flash and eliminates `snprintf` overhead at runtime.

---

## Enumerations

### `ApaDoseType`
```cpp
enum ApaDoseType : uint8_t {
  DOSE_PH,  // pH pump — direction (acid/base) set separately via begin() or setPhDirection()
  DOSE_CL   // Chlorine/ORP pump — doses when ORP is below setpoint
};
```

### `ApaDoseDirection`
```cpp
enum ApaDoseDirection : uint8_t {
  PH_PLUS,   // Base chemical — doses when pH is below setpoint (raises pH)
  PH_MINUS   // Acid chemical — doses when pH is above setpoint (lowers pH)
};
```

### `ApaDoseAlarm`
```cpp
enum ApaDoseAlarm {
  ALARM_NONE,
  ALARM_WRONG_DIRECTION,  // sensor moved opposite way 3×  — wrong chemical?
  ALARM_INEFFECTIVE,      // no change after 3 dosing attempts
  ALARM_SAFETY_BAND,      // sensor beyond setpoint ± safety band
  ALARM_INVALID_PARAM,    // configuration value out of valid range
  ALARM_DAILY_LIMIT,      // maximum daily dose count reached — requires human check
  ALARM_SENSOR_FAULT      // sensor reading out of range or NaN for >2 min, or no reading for >30 min
};
```

### Alarm behavior

| Alarm | Trigger | Recovery |
|-------|---------|----------|
| `ALARM_WRONG_DIRECTION` | sensor moves opposite way on 3 consecutive cycles | Fix chemical or wiring → `acknowledgeAlarm()` |
| `ALARM_INEFFECTIVE` | 3 failed dosing attempts in a row | `acknowledgeAlarm()` |
| `ALARM_SAFETY_BAND` | sensor beyond `min(band × 1.5, hardCap)` from setpoint | Automatic when sensor recovers |
| `ALARM_INVALID_PARAM` | bad configuration value | Automatic rejection, no change applied |
| `ALARM_DAILY_LIMIT` | `maxDailyDoses` reached | `acknowledgeAlarm()` |

**`ALARM_WRONG_DIRECTION` on high-bather-load days:** On a heavily used pool, chlorine demand can exceed what each dose delivers — ORP may drop after dosing even though the correct chemical is present. The 3-consecutive-cycle threshold (with ~20-minute rest periods between each) gives roughly one hour of tolerance before the alarm fires, which covers most short demand spikes. If this alarm fires on a busy day:

1. Check the chemical tank is not empty and contains the correct product.
2. Check the pump is primed and delivering (prime briefly, verify flow).
3. If both are fine, the pool demand exceeds the current dosing capacity — consider increasing the proportional band or switching to manual dose to top up, then `acknowledgeAlarm()` to resume automatic control.

Display alarm details on demand:
```cpp
if (phPump.isAlarmActive()) {
  Serial.println(phPump.getAlarmMessage());  // same text that came through the callback
}
```

---

## Callback Signatures

```cpp
typedef float      (*SensorReadCallback)();               // return current sensor value
typedef bool       (*FilterCallback)();                   // return true if filter running
typedef bool       (*ExternalStopCallback)();             // return true to block all dosing (except priming); 5-min resume delay applies after clearance
typedef void       (*AlarmCallback)(ApaDoseAlarm, const char*);
typedef void       (*StatusCallback)(const char*);
typedef ApaDoseTime (*RTCReadCallback)();                 // return current date/time
```

> **Warning — sensor callback must match pump type:**  
> The `SensorReadCallback` must return values appropriate for the pump's `dosingType`:
> - `DOSE_PH` pump → callback must return pH (valid control range 6.8–7.8, hardware bounds 0–14)
> - `DOSE_CL` pump → callback must return ORP in mV (valid control range 400–850 mV, hardware bounds −1500 to +1500 mV)
>
> **The library cannot detect a mismatched callback.** pH values (0–14) fall entirely within the valid ORP hardware range (−1500 to +1500 mV), so a pH sensor wired to a `DOSE_CL` pump passes all range checks silently. The first visible symptom is `ALARM_SAFETY_BAND`: with a 700 mV ORP setpoint, a pH reading of 7.4 is 692 mV below target — the safety band fires almost immediately, but the alarm text gives no indication of the root cause. Always verify that each pump's `begin()` call uses the correct sensor callback for its `dosingType`.

### `ApaDoseTime` struct
```cpp
struct ApaDoseTime {
  uint8_t  hour;    // 0-23
  uint8_t  minute;  // 0-59 (unused internally — include for completeness)
  uint8_t  second;  // 0-59 (unused internally)
  uint8_t  day;     // 1-31 — used for daily counter reset
  uint8_t  month;   // 1-12 (unused internally)
  uint16_t year;    // e.g. 2026 (unused internally)
};
```

Only `hour` and `day` are read by the library. Fill all fields from your RTC for forward compatibility.

**RTC callback example (DS3231):**
```cpp
ApaDoseTime getRTC() {
  DateTime now = rtc.now();
  return { now.hour(), now.minute(), now.second(),
           now.day(), now.month(), now.year() };
}
```

---

## Configuration Ranges

| Parameter | pH pumps | ORP / CL pump |
|-----------|----------|----------------|
| Setpoint min | 6.8 | 400 mV |
| Setpoint max | 7.8 | 850 mV |
| Setpoint default | 7.4 | 700 mV |
| Band min | 0.5 | 50 mV |
| Band max | 2.0 | 250 mV |
| Band default | 1.0 | 100 mV |
| Feedback threshold | 0.05 pH | 10 mV |

Safety band uses dual protection — no user configuration needed:

```
safetyBand = min(proportionalBand × 1.5, hardCap)
```

| Type | Hard cap | Worst-case safety zone (SP = default) |
|------|----------|---------------------------------------|
| pH pumps | ±1.0 pH | 6.4 – 8.4 (SP 7.4) |
| ORP / CL | ±150 mV | 550 – 850 mV (SP 700) |

The `× 1.5` multiplier gives a 50% buffer beyond the control band edge for tight bands. The hard cap overrides this for wide bands, preventing dangerous water chemistry regardless of the configured proportional band. Both limits are exported as named constants (`PH_SAFETY_HARD_CAP`, `ORP_SAFETY_HARD_CAP`, `SAFETY_BAND_MULTIPLIER`) in `APADOSE.h`.

---

## Dosing Zones

Sensor error is mapped to pulse intensity and duration:

| Error % of band | PWM | Pulse duration | Rest period |
|-----------------|-----|----------------|-------------|
| 0–25% | min+10% floor | 2–4 s | 5 min |
| 25–50% | proportional | 4–7 s | 10 min |
| 50–75% | proportional | 7–10 s | 15 min |
| 75–100% | max | 11 s | 20 min |

After failed attempts, `applyFeedbackCorrections()` boosts PWM by 30% (1 failure) or 50% + double duration (2+ failures), capped at `pumpMaxPWM`.

---

## EEPROM Layout

```
Address 0–127    Arduino / user application
Address 128–144  APAPHX2_ADS1115 — pH calibration
Address 145–160  APAPHX2_ADS1115 internal gap
Address 161–177  APAPHX2_ADS1115 — ORP calibration
Address 178–191  safety gap
Address 192+     APA-Dose configuration (this library); sizeof(ConfigData) = 20 bytes
Address 212+     second pump instance; 232+ third; 252+ fourth
```

Write method: `EEPROM.write()` — works on all supported platforms; ESP8266/ESP32 `EEPROM.begin()` and `EEPROM.commit()` are called automatically.  
Validation: 2-byte magic number `0xABCD` + version byte + additive checksum.  
On invalid EEPROM: factory defaults loaded and written automatically.

---

## Non-Blocking Design

```cpp
void loop() {
  phPump.update();    // APA-Dose state machine — fast, never blocks
  clPump.update();    // second independent pump
  phSensor.update();  // your sensor library
  ui.update();        // your display/UI code
}
```

All timing uses `millis()`. Zero `delay()` calls anywhere in the library.
