# APA-Dose Library — API Reference

**Version**: 3.16.2  
**File**: `APADOSE.h` / `APADOSE.cpp`

---

## Quick Start — pH pump

```cpp
#include "APADOSE.h"

ApaDose phPump(PIN_PH_PUMP);

float getpH()         { return phSensor.getPH(); }   // must return a stable, smoothed reading — see note below
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }

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
8. setEfficiencyThreshold()     optional — per-pump delivery alarm threshold (default 20; pass 0 to disable)
                                call any time after begin(); takes effect on the next evaluated dose
   ApaDose::setPoolVolume()     optional — system-wide pool size scaling (static, affects all instances)
   ApaDose::setDeadbandPct()    optional — system-wide dosing dead-band (static, affects all instances)
                                both must come AFTER begin() — EEPROM must be initialised first
                                can also be called at runtime from loop() — takes effect immediately
9. clPump.setPhPump(&phPump)    optional — pH-first priority (J) + cross-settle coupling (A)
   clPump.setCrossSettleMinutes() optional — enables Option A; call after setPhPump()
                                both must come AFTER both pump begin() calls; pH-only setups omit this
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

> **PWM pin required.** The pump pin passed to the constructor must support PWM (`analogWrite()`). On Arduino Uno/Nano these are pins **3, 5, 6, 9, 10, 11** — marked with a `~` on the board silkscreen. On Mega: 2–13 and 44–46. On ESP8266/ESP32 all GPIO pins support PWM. On STM32 consult your board's pinout. Connecting to a non-PWM pin will cause the pump to run at full speed or not at all.

A 10% minimum floor above `minPWM` is applied to every dose to overcome pipe and hose resistance.

**Solenoid / time-proportional mode:** setting `minPWM == maxPWM` (e.g. `255, 255`) locks PWM at that fixed level. Proportionality then comes entirely from pulse duration — the solenoid opens fully for 10–180 s scaled to the error percentage. All feedback, safety, and alarm systems function identically in this mode.

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

### `setPhPump()` / `setCrossSettleMinutes()`
```cpp
void setPhPump(ApaDose* phPump);
void setCrossSettleMinutes(uint8_t minutes);
```
Links a CL pump instance to a pH pump peer, enabling two optional features. **Call AFTER both `begin()` calls.**

**Option J — pH-first dosing priority** (activated by `setPhPump()` alone):  
CL automatic dosing is suspended whenever the pH pump's reading exceeds `CL_PH_MAX` (7.6). Above this pH, an increasing fraction of free chlorine converts to the ineffective hypochlorite ion — dosing wastes chemical and produces misleading ORP feedback. Normal CL dosing resumes automatically once pH drops back below 7.6.  
Status: `"CL held: pH high"` fires once when the hold activates.

**Option A — cross-settle coupling** (activated by `setCrossSettleMinutes(n)`, `n > 0`):  
CL dosing is held for `n` minutes after each pH dose completes, giving chemistry time to equilibrate. Acid doses temporarily depress ORP during mixing; without this hold a premature CL dose can fire before the reading stabilises, creating a pH/ORP see-saw.  
Status: `"CL held: settling"` fires during the hold window.  
`n = 0` (default) disables Option A; Option J remains active independently.

Both features are off by default. Neither requires any change to the pH pump instance.

```cpp
// After phPump.begin() and clPump.begin():
clPump.setPhPump(&phPump);          // Option J active immediately
clPump.setCrossSettleMinutes(15);   // Option A: 15 min hold after pH doses
```

| `setPhPump` called | `setCrossSettleMinutes` | Result |
|--------------------|------------------------|--------|
| No | Any | J and A both inactive — CL runs independently |
| Yes | 0 (default) | J active — CL held when pH > 7.6 |
| Yes | > 0 | J + A active — CL also held N min after each pH dose |

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
void    setEfficiencyThreshold(uint8_t pct);  // delivery alarm threshold: 0 = off; default 20 (active out of box)
uint8_t getEfficiencyThreshold() const;
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
1. If shock is active — stops the pump and clears the post-shock cooldown (full clean slate).
2. If a dose is active — stops the pump immediately (same as a mid-dose filter dropout).
3. If priming is active — stops the pump immediately.
4. If an alarm is active — clears it (fires `onAlarmCleared` if registered).
5. Resets per-instance EEPROM fields to defaults for the current dosing type:

| Field | After reset | Rationale |
|-------|-------------|-----------|
| `setpoint` | pH 7.4 / ORP 700 mV | |
| `proportionalBand` | pH 1.0 / ORP 100 mV | |
| `phDirection` | `PH_PLUS` | |
| `nudgePct` | 0 (adaptive PB disabled) | |
| `adaptedPB` | 0.0f (learned value discarded) | |
| `_efficiencyEma` / `_efficiencyCount` / `_lastEfficiencyPct` | 0.0 / 0 / 100 (baseline cleared, ratio reset to 100) | Learned delivery baseline is installation-specific; reset clears history so a fresh baseline builds from the next 3 doses |
| `_efficiencyThresholdPct` | **Not touched** | Integrator value — set in `setup()` like `setPhPump()`; not EEPROM-stored |
| **dead-band** (`s_deadbandPct`) | **0 (disabled)** | Tuning parameter — must be the reliable escape hatch when dosing misbehaves after dead-band was set |
| **pool volume** (`s_poolVolume`) | **Not touched** | Physical installation fact — the pool does not change size on reset; clear explicitly with `ApaDose::setPoolVolume(0)` |

6. Saves the reset values to EEPROM.
7. Sends `"Factory reset"` via `onStatusMessage`.

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
| `triggerManualDose()` | `pumpMaxPWM` | Yes — blocked by most alarms; `ALARM_OFA` is the exception (manual doses still permitted) | No | Yes |
| `triggerPrime()` | configurable | No — bypasses all guards | No | No |

`triggerManualDose()` returns `false` — and does nothing — when any of the following conditions are true:

| Condition | Guard |
|-----------|-------|
| A dose or prime is already running | `flags.dosingActive \|\| flags.primingActive` |
| An alarm is active — except `ALARM_OFA` | `flags.alarmActive && alarm != ALARM_OFA` |
| `durationMs` is 0 | zero duration is rejected |
| Daily dose limit reached | `maxDailyDoses > 0 && dailyDoseCount >= maxDailyDoses` |
| Filter not running | only when a `FilterCallback` was registered in `begin()` |
| External stop active | external stop callback returns `true` |
| External stop resume delay | 5-minute settling time after external stop cleared |
| Inter-pump lockout active | 90 s since the last dose on any instance has not elapsed |  
If `durationMs` exceeds `MAX_MANUAL_DOSE_MS` (5 minutes), it is silently clamped and a `"Dose capped:5min"` status message is sent — the dose still runs at the capped duration.  
The optional `restMs` parameter (default 20 min) sets the mixing wait before the next proportional dose. Pass `0` only for sensor-less pumps where `maxDailyDoses` already prevents double-dosing.

`triggerPrime()` returns `false` if dosing or priming is already active, or if `durationMs` is 0.

`triggerPrime()` is for filling the pipe on first install — it ignores alarms, filter state, blackout, and dosing window. The optional `pwm` parameter lets you prime at a lower speed to protect dry joints; passing `0` (default) runs at `pumpMaxPWM`. The value is clamped to `[pumpMinPWM, pumpMaxPWM]`.

```cpp
pump.triggerPrime(10000);       // 10 s at full speed (default)
pump.triggerPrime(10000, 100);  // 10 s at PWM 100 — gentle fill on a dry pipe
```

**No rest period is imposed after priming.** Priming fills dry pipe only — no chemical reaches the pool water, so consecutive primes are allowed immediately and automatic proportional dosing may start as soon as priming ends. If a proportional dose had completed before the prime and its rest period is still running, that rest period continues independently — it is unrelated to priming.

---

## Scheduled Dosing

```cpp
void setScheduledDose(uint8_t hour, uint8_t minute,
                      unsigned long durationMs,
                      uint8_t intervalDays = 1,
                      float   threshold    = 0.0f);
```

Arms a recurring dose that fires automatically at the configured wall-clock time. Requires `setRTCCallback()`. If no RTC callback is registered, the call is stored but never triggers.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `hour` | `uint8_t` | — | Hour of day to fire (0–23). Values above 23 are clamped to 23. |
| `minute` | `uint8_t` | — | Minute of hour to fire (0–59). Values above 59 are clamped to 59. |
| `durationMs` | `unsigned long` | — | Dose duration in milliseconds. Clamped to `MAX_MANUAL_DOSE_MS` (5 min). Values of 0 are clamped to 1000 ms (1 s minimum). |
| `intervalDays` | `uint8_t` | `1` | Fire every N days. `1` = daily, `7` = weekly, `14` = fortnightly. `0` is treated as `1`. |
| `threshold` | `float` | `NAN` | Condition for the dose to fire. `NAN` (default) = use the pump's own setpoint. `0.0` = always dose. Any finite value = explicit override. For `PH_MINUS` pumps: skips if reading ≤ threshold. For `PH_PLUS` / `DOSE_CL` pumps: skips if reading ≥ threshold. Ignored for sensor-less pumps. |

The library tracks the interval with a day-of-month counter that ticks once per calendar day (from the RTC). The dose is suppressed on interval days without resetting the countdown.

**All standard guards apply.** The scheduled dose calls `triggerManualDose()` internally, so it is blocked by: active alarm (except `ALARM_OFA`), filter off, external stop, tank empty, daily limit, inter-pump lockout, or a dose/prime already running. The daily dose counter is incremented when the dose fires.

```cpp
// Algaecide every day at 09:00 for 30 s (sensor-less — threshold ignored, always doses)
algiPump.setScheduledDose(9, 0, 30UL * 1000UL);

// Flocculant every 7 days at 08:30 for 60 s (sensor-less — threshold ignored, always doses)
flocPump.setScheduledDose(8, 30, 60UL * 1000UL, 7);

// pH acid daily at 07:00 — doses only when pH has drifted past setpoint (default threshold)
phPump.setScheduledDose(7, 0, 10UL * 1000UL);

// pH acid daily — explicit threshold: only dose if pH > 7.4 regardless of setpoint
phPump.setScheduledDose(7, 0, 10UL * 1000UL, 1, 7.4f);

// pH acid daily — always dose at 07:00, no sensor condition
phPump.setScheduledDose(7, 0, 10UL * 1000UL, 1, 0.0f);
```

---

## Shock / Super-Chlorination

Shock mode doses the chlorine pump at full power (`pumpMaxPWM`) until ORP reaches a target or a time ceiling expires, then automatically returns to normal proportional control. Use it after heavy bather load, algae events, storms, or any situation where proportional dosing is too slow to restore chlorine.

**Available on `DOSE_CL` instances only. Requires a `FilterCallback` registered in `begin()`.**

### `triggerShock()` — hobbyist overload

```cpp
bool triggerShock(uint16_t targetORP, float currentPH,
                  uint8_t cooldownHours = SHOCK_COOLDOWN_DEFAULT_HOURS);
```

Simplest form — uses sensible defaults (4 h maximum duration, 24 h post-shock cooldown).

```cpp
// Weekly maintenance shock — target 750 mV, current pH from the pH pump
clPump.triggerShock(SHOCK_ORP_STANDARD, phPump.getProbeValue());

// Aggressive shock after heavy algae — 48 h cooldown (large pool, slow ORP recovery)
clPump.triggerShock(SHOCK_ORP_AGGRESSIVE, phPump.getProbeValue(), 48);
```

**Named ORP presets:**

| Constant | Value | When to use |
|----------|-------|-------------|
| `SHOCK_ORP_MILD` | 700 mV | Light event — post-rain, minor algae risk |
| `SHOCK_ORP_STANDARD` | 750 mV | Weekly maintenance shock |
| `SHOCK_ORP_AGGRESSIVE` | 800 mV | Heavy algae or high bather load |

### `triggerShock()` — pro overload

```cpp
bool triggerShock(uint16_t targetORP, uint8_t maxDurationHours, float currentPH,
                  uint8_t cooldownHours = SHOCK_COOLDOWN_DEFAULT_HOURS);
```

Full control over duration and cooldown window.

| Parameter | Type | Constraints | Description |
|-----------|------|-------------|-------------|
| `targetORP` | `uint16_t` | 600–800 mV | ORP target in mV; use named presets for clarity |
| `maxDurationHours` | `uint8_t` | 1–4; clamped to 4 h | Maximum active dosing time before auto-stop |
| `currentPH` | `float` | 7.0–7.6 | Current pH; pass `phPump.getProbeValue()` |
| `cooldownHours` | `uint8_t` | 1–48; clamped to 48 h | Post-shock safety band suppression window; default 24 h |

```cpp
// Pro: 3 h maximum, 48 h cooldown (RTC-backed, survives power cycle)
clPump.triggerShock(780, 3, phPump.getProbeValue(), 48);
```

### Return value and entry guards

Both overloads return `false` (do nothing) when any guard fails:

| # | Condition | Reason |
|---|-----------|--------|
| 1 | `dosingType != DOSE_CL` | Shock is chlorine-only |
| 2 | No `FilterCallback` registered | No filter visibility — shock not safe |
| 3 | Filter not running | Never dose into stagnant water |
| 4 | External stop active | Operator or system has blocked dosing |
| 5 | Alarm active | Underlying problem must be resolved first |
| 6 | Dose or prime already running | State machine conflict |
| 7 | `currentPH < CL_PH_MIN` (7.0) or `> CL_PH_MAX` (7.6) | pH out of effective chlorine range |
| 8 | `targetORP < 600` or `> 800` mV | Outside permitted shock target range |
| 9 | `sensorValue >= targetORP` | ORP already at or above target |
| 10 | Post-shock cooldown active | Inter-shock interval not yet elapsed |

### What shock does

**On start:**
- `shockEffectiveStop` = `sensorValue + (targetORP − sensorValue) × 0.90` — pump stops 10% before the target to account for Cl mixing lag; ORP continues rising after dosing stops
- `shockRiseTarget` = `sensorValue + 20 mV` — ORP must rise at least 20 mV within the first 20 minutes
- All other `ApaDose` instances are held: `"Held:shock active"` fires once per instance; `"Dosing resumed"` fires when shock ends
- `dailyDoseCount` is **not** incremented (shock is operator intervention, not automatic dosing); `dailyVolumeMl` **is** accumulated

**Each `update()` cycle during shock:**
1. Alarm fired → abort: `"Shock:alarm fired"`
2. Filter stopped → abort: `"Shock:filter off"`
3. External stop → abort: `"Shock:ext stop"`
4. At 20 minutes: ORP < `shockRiseTarget` → abort with `ALARM_INEFFECTIVE`: `"Shock:no ORP rise"` (empty container / failed pump detected)
5. `sensorValue >= shockEffectiveStop` → complete: `"Shock done:target"`
6. Time ceiling reached → complete: `"Shock done:timeout"`

**Safety backstop:** the safety band check at 850 mV (`ORP_SETPOINT_MAX`) fires independently via `readSensors()` and stops everything if ORP reaches a dangerous level during shock.

### Post-shock cooldown

After shock ends, the safety band alarm is suppressed while ORP normalizes:

- **With RTC:** cooldown measured in wall-clock hours — **survives power cycles**
- **Without RTC:** cooldown measured with `millis()` — resets on power cycle (acceptable for no-RTC installs)
- Maximum: 48 h (`SHOCK_COOLDOWN_MAX_HOURS`); values above this are silently clamped
- When cooldown expires: `"Post-shock normal"` fires and normal safety monitoring resumes
- The post-shock cooldown also guards the minimum inter-shock interval — `triggerShock()` returns `false` while cooldown is active, preventing a second shock before the pool has stabilized

**Alarm behavior during and after shock:**

| Alarm | During shock | Post-shock cooldown |
|-------|-------------|---------------------|
| `ALARM_SAFETY_BAND` | Replaced by absolute 850 mV ceiling | Suppressed |
| `ALARM_SENSOR_FAULT` | Active | Active |
| `ALARM_INEFFECTIVE` | Fires on ORP-rise check failure | Active |
| `ALARM_WRONG_DIRECTION` | N/A (no proportional dosing) | Active |
| `ALARM_DAILY_LIMIT` | N/A | Active |

### Status queries

```cpp
bool          isShockActive()            const;  // true while shock dosing is running
unsigned long getShockRemainingSeconds() const;  // seconds to time ceiling; 0 if not active
```

### Status messages

| Event | Message |
|-------|---------|
| Shock started | `"Shock started"` |
| Alarm fires during shock | `"Shock:alarm fired"` |
| Filter off during shock | `"Shock:filter off"` |
| External stop during shock | `"Shock:ext stop"` |
| ORP did not rise in 20 min | `"Shock:no ORP rise"` + `ALARM_INEFFECTIVE` |
| ORP reached effective stop | `"Shock done:target"` |
| Time ceiling hit | `"Shock done:timeout"` |
| Other instance held during shock | `"Held:shock active"` |
| Other instance resumed after shock | `"Dosing resumed"` |
| Cooldown window expired | `"Post-shock normal"` |

### pH guidance

pH must be 7.0–7.6 before shock (`CL_PH_MIN` / `CL_PH_MAX`). This is not bureaucracy — above pH 7.6, an increasing fraction of free chlorine converts to the ineffective hypochlorite ion (OCl⁻); at pH 7.8 roughly 60% of your chlorine dose is wasted. Adjust pH first, then shock.

| pH | Approx. fraction of active HOCl | Shock effective? |
|----|----------------------------------|------------------|
| 7.0 | ~75% | Excellent |
| 7.2 | ~65% | Good |
| 7.4 | ~55% | Acceptable |
| 7.6 | ~45% | Marginal — `CL_PH_MAX` ceiling |
| 7.8 | ~37% | Not permitted |

### SRAM cost

21 bytes per `DOSE_CL` instance + 1 byte shared static (`shockModeActive`). `DOSE_PH` instances pay no runtime cost — shock member variables are present (class definition is shared) but `triggerShock()` exits at guard 1 without touching them.

### pH dosing hold after shock (M6 consideration)

ORP can continue rising for 30–60 minutes after shock ends as dissolved chlorine continues reacting. Professional systems impose a hold on pH dosing during this period to prevent incorrect pH corrections against a still-rising ORP baseline. This feature is not implemented in v3.9 — flagged for M6.

> See **`examples/basic/02_ph_and_cl/`** for the hobbyist shock pattern (button trigger, `SHOCK_ORP_STANDARD`).  
> See **`examples/advanced/05_multi_pump/`** for the pro pattern (RTC-based cooldown, serial feedback, `isShockActive()` display).

---

## Pool Volume and Dead-band

Both are **static shared values** — one call affects all pump instances simultaneously. Call after `begin()` — from `setup()` or from anywhere in `loop()` at runtime. The new value takes effect immediately: `setPoolVolume()` applies on the next dose calculation; `setDeadbandPct()` applies on the very next `update()` tick.

> **Must be called after `begin()`** — on ESP8266/ESP32, `EEPROM.begin()` is called inside `begin()`. Calling `setPoolVolume()` or `setDeadbandPct()` before any `begin()` on those platforms silently discards the write because the EEPROM RAM buffer has not been allocated yet. On AVR the order does not matter, but calling after `begin()` is correct on all platforms.

### Pool volume scaling

```cpp
static bool    ApaDose::setPoolVolume(uint8_t m3);  // 10–90 m³; 0 = off; false if out of range
static uint8_t ApaDose::getPoolVolume();
```

The library was calibrated on a **20 m³** reference pool (`REFERENCE_VOLUME_M3`). Without a pool volume set, all timing parameters use the reference values. With a pool volume set, pulse duration, rest period, feedback pulse cap, and shock ORP rise window all scale proportionally:

```
scale = poolVolume / 20
```

| Pool | Scale | Effect |
|------|-------|--------|
| 10 m³ | 0.50× | Shorter doses, shorter rest — reacts faster in a smaller water volume |
| 20 m³ | 1.00× | Reference — unchanged behaviour |
| 35 m³ | 1.75× | Longer doses, longer rest — chemical needs more time to mix and take effect |
| 50 m³ | 2.50× | Without scaling, pools above ~30 m³ often never converge to setpoint |
| 90 m³ | 4.50× | Maximum; large commercial pool |

`setPoolVolume(0)` disables scaling (returns to reference behaviour). This is the default — the library behaves identically to v3.9 and earlier for systems that do not call `setPoolVolume()`.

**Persistence:** saved to a 3-byte global EEPROM slot at `APA_GLOBAL_EEPROM_ADDR` (189–191), outside `ConfigData`. **Survives `factoryReset()`** — the pool does not change size when a pump's tuning is reset. To clear it explicitly: `ApaDose::setPoolVolume(0)`.

**Multi-pump note:** call once — all instances read the same value.

```cpp
void setup() {
  phPump.begin(...);
  clPump.begin(...);
  ApaDose::setPoolVolume(35);   // call AFTER begin() — EEPROM must be initialised first (ESP requirement)
}
```

---

### Dead-band

```cpp
static bool    ApaDose::setDeadbandPct(uint8_t pct);  // 0–20 % of PB; 0 = off; false if > 20
static uint8_t ApaDose::getDeadbandPct();
```

Suppresses dosing when the sensor error is small — reduces unnecessary pump cycles when the pool is already close to setpoint and sensor noise could otherwise trigger constant small doses.

The unit is **percent of proportional band** — dimensionless and type-agnostic. The same setting means the same thing for pH and ORP because PB already normalizes the sensor scale:

| Setting | pH (PB = 1.0) | ORP (PB = 100 mV) |
|---------|--------------|-------------------|
| 5% | ±0.05 pH | ±5 mV |
| 10% | ±0.10 pH | ±10 mV |
| 15% | ±0.15 pH | ±15 mV |
| 20% | ±0.20 pH | ±20 mV |

**Asymmetric hysteresis** — the entry and exit thresholds are deliberately different to prevent oscillation when the sensor straddles the dead-band boundary:

| Entry (`pct`) | Exit threshold | Hysteresis gap |
|--------------|----------------|----------------|
| 0% | — | disabled |
| 1–5% | 0% (same as entry) | none — symmetric |
| 6–20% | `pct − 5%` | 5 percentage points |

- **Entering** the dead-band requires the error to **drop below the entry threshold**.
- **Leaving** the dead-band (re-enabling dosing) requires the error to **grow back past the entry threshold**, not just the exit threshold. This prevents the pump from toggling on and off while the sensor wanders within the gap.

**Persistence:** saved to the same global slot as pool volume. **Cleared by `factoryReset()`** — dead-band is a tuning parameter. If dosing misbehaves after setting a dead-band value, `factoryReset()` is the reliable escape hatch. To clear it without a full reset: `ApaDose::setDeadbandPct(0)`.

```cpp
void setup() {
  phPump.begin(...);
  clPump.begin(...);
  ApaDose::setDeadbandPct(10);   // call AFTER begin() — 10 % of PB: pH ±0.10 entry / ±0.05 exit, ORP ±10/±5 mV
}
```

**What is NOT affected by dead-band:**
- Safety band alarm (`readSensors()`) — always active
- Shock mode — runs before the proportional block, unaffected
- `triggerManualDose()` / `triggerPrime()` — user-initiated, unaffected

---

## Over-Feed Alarm (OFA)

```cpp
void    setOFALimit(uint8_t referenceMinutes);  // 0 = disabled (default); reference for 20 m³ pool
uint8_t getOFAPct() const;                      // today's usage as 0–100 %; 0 if OFA disabled
```

Protects against chemical over-dosing by tracking how many minutes each pump has run today. Disabled by default — call `setOFALimit()` once in `setup()` to enable.

**How it works:**

`referenceMinutes` sets the daily pump run-time ceiling for the reference pool size (20 m³). If `ApaDose::setPoolVolume()` was called, the limit scales automatically with pool size — a 40 m³ pool gets twice the reference limit, an 80 m³ pool four times.

| Threshold | Effect |
|-----------|--------|
| 70 % of limit | Status message via `onStatusMessage` — dosing continues normally |
| 90 % of limit | `ALARM_OFA` fires — dosing stops until acknowledged |

**Alarm recovery:** `acknowledgeAlarm()` resets the daily counter immediately and dosing resumes. At midnight (or after 24 h without an RTC) the counter resets automatically so unattended systems recover the next day without operator attention.

**What counts toward OFA:** proportional dosing pulses and shock dosing.

**What does NOT count:** `triggerManualDose()` (manual doses are intentional operator actions, always permitted even under `ALARM_OFA`) and `triggerPrime()`.

**Choosing a starting limit:** 20–30 minutes is a typical starting point for a 20 m³ residential pool. Watch `getOFAPct()` over the first week — if it consistently reaches 80–90 % while pool chemistry is good, the limit is too tight; raise it. If it barely reaches 30 %, the limit is generous and you have headroom.

```cpp
void setup() {
  phPump.begin(...);
  clPump.begin(...);
  phPump.setOFALimit(30);   // 30-min reference for 20 m³ — scales with setPoolVolume()
  clPump.setOFALimit(30);   // set independently per pump
}

void loop() {
  // Show today's OFA usage on a dashboard row (returns 0 when OFA is disabled)
  uint8_t phOFA = phPump.getOFAPct();
  if (phOFA > 0) {
    Serial.print(F("pH OFA today: ")); Serial.print(phOFA); Serial.println(F("%"));
  }
}
```

`setOFALimit()` is per pump instance — set different limits for pH and chlorine if needed. Call after `begin()`.

---

## Dynamic OFA (dOFA)

```cpp
void    setDOFAAdaptDays(uint8_t days);  // EMA speed: 3–14 days, default 10; call in setup()
void    disableDOFA();                   // suppress all dOFA checks for this instance
uint8_t getDOFAPct() const;              // today's proportional run vs baseline (0–100 %); 0 = learning
bool    isDOFALearning() const;          // true until the first qualifying day (typically day 2)
void    resetDOFA();                     // clear baseline and daily counter; call at spring opening
```

Self-learning over-feed protection that requires zero configuration. dOFA observes the normal proportional run time for THIS pool and fires `ALARM_OFA` when today's run time is abnormally high — no limit to guess, no calibration, nothing to set.

**Always active by default.** Works alongside fixed OFA (`setOFALimit()`): both run independently and both reuse `ALARM_OFA`. Whichever fires first controls. Pressing ACK resets both daily counters.

**How dOFA learns:**

Each midnight (or after 24 h without an RTC), if at least 60 seconds of proportional run time was accumulated, the EMA baseline is updated:

```
First qualifying day → baseline = today's run time (cold-start seed)
Subsequent days      → baseline = baseline × (N-1)/N + today × 1/N
```

Where N = `_dofaAdaptDays` (default 10). The baseline is written to EEPROM at midnight and survives power cycles. A mid-day power cycle loses the day's accumulation (RAM only) but the learned baseline is safe.

**Alarm thresholds:**

| Threshold | Effect |
|-----------|--------|
| 150 % of baseline | Status message `"dOFA:150% warning"` — dosing continues (e.g. normal 10 min/day → warning above 15 min) |
| 200 % of baseline | `ALARM_OFA` fires — dosing stops until acknowledged (e.g. normal 10 min/day → alarm above 20 min) |

Checks are suppressed until the baseline reaches at least 300 seconds (5 min). `isDOFALearning()` returns `true` and `getDOFAPct()` returns 0 during this warm-up.

**What counts toward dOFA:** proportional dosing pulses only.

**What does NOT count:** `triggerManualDose()`, `triggerShock()`, and `triggerPrime()`.

**Warm-up protection:** During the first dosing day (until the baseline seeds at midnight) the pool is guarded by `ALARM_INEFFECTIVE`, `ALARM_WRONG_DIRECTION`, `ALARM_SAFETY_BAND`, the daily dose limit, and optional fixed OFA.

**RTC note:** RTC improves dOFA accuracy after reboots — without one, a power cycle mid-day restarts the 24-hour window, which can slightly skew the learned baseline over time. For stable systems without frequent reboots the millis fallback is adequate.

**Spring opening:** Call `resetDOFA()` on each pump after a seasonal shutdown of more than a few weeks — the library then re-learns current chemistry rather than using last year's baseline. Call it once on the first startup of the season; dOFA re-learns automatically from that point forward.

```cpp
void setup() {
  phPump.begin(...);
  clPump.begin(...);

  // Spring opening — restart dOFA on both pumps after seasonal shutdown
  // phPump.resetDOFA();
  // clPump.resetDOFA();

  // Optional tuning — speed up learning for first install (default 10)
  // phPump.setDOFAAdaptDays(5);
  // clPump.setDOFAAdaptDays(5);
}

void loop() {
  phPump.update();
  clPump.update();

  // Dashboard: show dOFA warm-up state or today's usage
  if (phPump.isDOFALearning()) {
    lcd.print(F("pH dOFA: warming up"));
  } else {
    Serial.print(F("pH dOFA today: "));
    Serial.print(phPump.getDOFAPct());
    Serial.println(F("%"));
  }
}
```

`resetDOFA()` clears the learned baseline, zeroes the daily counter, and re-enables dOFA if it was disabled. Call on each pump instance individually. After reset, `isDOFALearning()` returns `true` until the first qualifying day — dOFA is active again from day 2.

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
unsigned long    getLastDosingTime()            const;  // millis() when last dose ended (same as getLastDosingEnd)
unsigned long    getLastDosingEnd()             const;  // millis() when last dose ended; 0 if never dosed — used by linked CL pump for Option A
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

> **Sensor smoothing:** The library calls your sensor callback once per sample interval and uses the raw value returned. If your sensor library returns noisy, unfiltered readings, that noise is visible to the dosing logic — a momentary spike can trigger a dose, and erratic readings after a dose will confuse the feedback comparison, leading to false `ALARM_WRONG_DIRECTION` or `ALARM_INEFFECTIVE` alarms. Use a sensor library that applies internal averaging or exponential smoothing (such as APAPHX2), or apply smoothing inside your callback before returning the value. A simple rolling average over 5–10 readings is sufficient for most analog pH/ORP circuits.

The library validates every sensor reading with `isfinite()`.

> **Temperature compensation:** pH sensor readings are temperature-dependent (~0.003 pH/°C). The **APAPHX** and **APAPHX2** libraries apply the Passco 2001 formula to deliver a stable, temperature-compensated value automatically. If using a different sensor library, apply temperature compensation inside your sensor callback so APA-Dose always receives a corrected reading. If the callback returns NaN or infinity:
- The first bad reading sends `"Sensor:bad value"` once via `onStatusMessage`
- The last known good value is kept — dosing and safety checks continue against it
- The flag clears automatically when a finite value is received; the message fires again on the next transition to bad
- Any feedback sample that is non-finite is silently skipped and rescheduled — the average is never corrupted by a bad read

This protects against transient sensor glitches and disconnected probes that return rail voltages through the calibration math.

### Stale sensor timeout

If the sensor callback returns only non-finite values for `SENSOR_STALE_MS` (30 minutes) without recovery, automatic dosing is suspended and `ALARM_SENSOR_FAULT` fires with message `"Stale>30min"` via `onAlarmTriggered`. The alarm clears automatically as soon as the next finite reading arrives — no `acknowledgeAlarm()` required.

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
bool    hasDoseHistory()           const;  // false until first full dose+feedback cycle
float   getLastDoseSensorBefore()  const;  // averaged sensor value before last dose
float   getLastDoseSensorAfter()   const;  // averaged sensor value after last dose
uint8_t getDoseEffectiveness()     const;  // 0–100: last dose vs EMA baseline; 100 before baseline established
```

`getDoseEffectiveness()` returns the last dose's delivery ratio as a percentage of the learned EMA baseline. 100 means the dose delivered at or above the system's normal level; values below the configured threshold trigger `ALARM_INEFFECTIVE`. Returns 100 until the baseline is established (first 3 automatic proportional doses).

| Value | Meaning |
|-------|---------|
| `100` | Delivery at or above learned baseline — normal |
| `50`  | Delivered 50% of what is normal for this system |
| `15`  | Below default 20% threshold — `ALARM_INEFFECTIVE` fires |
| `0`   | No delivery detected |

```cpp
if (phPump.hasDoseHistory()) {
  Serial.print("Before:   ");  Serial.println(phPump.getLastDoseSensorBefore());
  Serial.print("After:    ");  Serial.println(phPump.getLastDoseSensorAfter());
  Serial.print("Delivery: ");  Serial.print(phPump.getDoseEffectiveness()); Serial.println("%");
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

// Semantic alias for DOSE_CL pumps — identical to PH_PLUS internally
constexpr ApaDoseDirection CL_PLUS = PH_PLUS;
```

`CL_PLUS` is a named alias for `PH_PLUS`. Use it when calling `begin()` on a chlorine pump so intent is clear in the code — `CL_PLUS` and `PH_PLUS` are the same value. Direction is not used in chlorine control logic; the alias exists purely for readability.

```cpp
// pH pump — direction matters
phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20);  // acid pump

// Chlorine pump — use CL_PLUS alias; PH_PLUS would also compile and work
clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20);
```

### `ApaDoseAlarm`
```cpp
enum ApaDoseAlarm {
  ALARM_NONE,
  ALARM_WRONG_DIRECTION,  // sensor moved opposite way 3×  — wrong chemical?
  ALARM_INEFFECTIVE,      // no change after 3 dosing attempts
  ALARM_SAFETY_BAND,      // sensor beyond setpoint ± safety band
  ALARM_INVALID_PARAM,    // configuration value out of valid range
  ALARM_DAILY_LIMIT,      // maximum daily dose count reached — auto-clears at midnight / 24 h
  ALARM_SENSOR_FAULT      // sensor reading out of range or NaN for >2 min, or no reading for >30 min
};
```

### Alarm behavior

| Alarm | Trigger | Recovery |
|-------|---------|----------|
| `ALARM_WRONG_DIRECTION` | sensor moves opposite way on 3 consecutive cycles | Fix chemical or wiring → `acknowledgeAlarm()` |
| `ALARM_INEFFECTIVE` | EMA delivery ratio drops below threshold (default 20%), 3 consecutive failed feedback cycles, or ORP did not rise during shock | Fix pump or supply → `acknowledgeAlarm()` |
| `ALARM_SAFETY_BAND` | sensor beyond `min(band × 1.5, hardCap)` from setpoint | Automatic when sensor recovers |
| `ALARM_INVALID_PARAM` | bad configuration value | Automatic rejection, no change applied |
| `ALARM_DAILY_LIMIT` | `maxDailyDoses` reached | Automatic at midnight (RTC) or after 24 h (millis) |

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
| 0–25% | min+10% floor | 10–30 s | 5 min |
| 25–50% | proportional | 30–60 s | 10 min |
| 50–75% | proportional | 60–120 s | 15 min |
| 75–100% | max | 180 s | 20 min |

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

Write method: `EEPROM.put()` — works on all supported platforms; ESP8266/ESP32 `EEPROM.begin()` and `EEPROM.commit()` are called automatically.  
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
