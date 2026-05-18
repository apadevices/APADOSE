# APA-Dose Library

**Autonomous proportional chemical dosing for swimming pool automation**  
Part of the **APA Devices** product family.

**Version 3.8.3** &nbsp;·&nbsp; AVR &nbsp;·&nbsp; ESP &nbsp;·&nbsp; STM32 &nbsp;·&nbsp; No required dependencies

---

## Key Features

**Proportional dosing control**
- **True proportional output** — both PWM speed and pulse duration scale continuously with error; never bang-bang on/off
- **Closed-loop feedback** — 2 sensor readings averaged before each dose, 3 after; verifies the water actually moved toward setpoint
- **Adaptive dose correction** — first failed dose gets +30 % PWM boost; second gets +50 % PWM and doubled pulse time; alarm fires only after three consecutive failures — no human intervention needed between attempts
- **Dose effectiveness reporting** — `getDoseEffectiveness()`, `getLastDoseSensorBefore()`, `getLastDoseSensorAfter()` expose a percentage score and raw before/after sensor averages for display or logging; check `hasDoseHistory()` first — all three return `0.0` until the first complete dose+feedback cycle
- **Adaptive proportional band** — optional self-learning mode: after each feedback cycle the library nudges the effective band up when the sensor overshot, down when it undershot, converging toward the pool's true chemical response; disabled by default, enabled with `enableAdaptivePB(nudgePct)` (1–25% per cycle); learned value is EEPROM-persistent per pump

**Safety**
- **Full alarm system** — wrong direction, ineffective dose, safety band, daily dose limit, sensor fault (`ALARM_SENSOR_FAULT`) — all built-in and reported via callback or polling
- **Filtration interlock** — dosing blocked the instant the filter stops; a running dose halts immediately; no chemical ever injected into stagnant water
- **External stop** — optional callback from any external system (maintenance mode, backwash, cover) blocks all dosing immediately; a mandatory 5-minute settling time applies after the signal clears before dosing resumes
- **Setpoint range enforcement** — pH 6.8 – 7.8 and ORP 400 – 850 mV enforced on every write; out-of-range values rejected before reaching EEPROM
- **Inter-pump chemical lockout** — 90-second enforced gap after any pump instance doses; prevents incompatible chemicals meeting at the same pipe inlet

**Flexibility**
- **1 to 4 independent pumps** — each instance is a full isolated state machine with its own dosing cycle, feedback loop, alarm state, and EEPROM block
- **Sensor-less pump support** — pass `nullptr` as the sensor callback for flocculant or algaecide pumps; filtration interlock, daily limit, and priming all remain active
- **Solenoid valve support** — set `min == max` in `setPumpRange()` for time-proportional on/off control; no other code changes needed
- **Manual dosing** — `triggerManualDose()` for button or RTC-triggered doses; duration clamped to 5 minutes regardless of what is passed; all safety guards apply
- **Pipe priming** — `triggerPrime()` fills dry pipes on installation or after a container swap; bypasses all safety guards so it works even under an active alarm
- **Dosing window** — restrict automatic dosing to a configurable daily hour range via `setDosingWindow()`; manual doses and priming are unaffected

**Monitoring**
- **Chemical volume tracking** — `getDailyVolumeMl()` and `getLastDoseVolumeMl()` estimate consumption from actual pulse duration and PWM intensity; resets at midnight when an RTC is connected
- **Dose counter** — `getDailyDoseCount()` tracks combined automatic and manual doses per day; resets every 24h — at real midnight with an RTC, every 24h from boot without one
- **Rest period queries** — `getSecondsUntilNextDose()` returns seconds remaining in the current rest period (0 when ready to dose); `getSecondsSinceLastDose()` returns seconds elapsed since the last dose completed (0 if no dose yet this session) — both are useful for dashboards and LCD status rows
- **System status snapshot** — `getSystemStatus(buf, size)` fills a caller-supplied buffer with a single-line summary of the current state (active alarms, dosing phase, sensor value, daily dose count); size `APA_DOSE_STATUS_BUFFER_SIZE` (96) is sufficient for the longest output

**Engineering**
- **EEPROM persistence** — setpoint, band, and dosing type survive power loss; magic-number and checksum validation on every boot with automatic fallback to safe defaults
- **RTC scheduling** — optional: daily counter reset at midnight, dosing window by hour; library works fully without an RTC
- **Non-blocking** — pure `millis()` state machine; zero `delay()` calls; safe to call every `loop()` iteration alongside any other code
- **Universal hardware support** — AVR (Uno through Mega), ESP8266, ESP32, STM32 — same source, no `#ifdef` in user code
- **Minimal footprint** — two-pump sketch: ~14 KB flash / 734 B RAM on Uno; ~290 B RAM per additional instance; 13 boolean flags packed into 2 bytes
- **No required dependencies** — the library itself needs only `<Arduino.h>` and `<EEPROM.h>`; RTClib (+ Adafruit BusIO) is required only when using an RTC for scheduling — not needed without one

---

## Installation

**Arduino IDE** — Download the repository as a `.zip` and install via *Sketch → Include Library → Add .ZIP Library*.

**PlatformIO** — Copy the library folder into your project's `lib/` directory, then add to `platformio.ini`:

```ini
[env:your_board]
platform  = atmelavr          ; or espressif32, ststm32
board     = uno               ; your target board
framework = arduino
lib_extra_dirs = lib          ; folder containing the APA-DOSING_LIB directory

build_flags =
    ; -D APA_DOSE_DEBUG       ; uncomment for per-cycle diagnostic output on Serial
```

No additional dependencies — only the standard `<Arduino.h>` and `<EEPROM.h>` are required.

---

## What It Does

APA-Dose controls dosing pumps to keep a pool's pH or ORP (chlorine) at a target setpoint. It reads the current sensor value, calculates how far it is from the target, and runs the pump for a proportional duration — short pulses for small deviations, longer pulses when the water is further off. After each dose it waits for the chemical to mix, reads the sensor again, and verifies the dose was effective before starting the next cycle.

The library is fully **non-blocking**. All timing uses `millis()`. Zero `delay()` calls. Multiple independent pump instances run side-by-side in a single `loop()`.

---

## How Proportional Dosing Works

### The setpoint and proportional band

The user sets a **setpoint** (target value) and a **proportional band** (control range). The band defines the sensor range over which the pump output scales from minimum to maximum. Error is calculated as the distance from the setpoint, expressed as a percentage of the band.

```
pH-PLUS pump example  (setpoint 7.4,  band 1.0 pH)
Doses when pH falls below setpoint — raises pH toward target.

                                                          SP
          ──────────── Proportional band ───────────────►│
  pH  ────┼─────────────┬──────────┬─────────┬───────────┼────
         6.4           6.65       6.9       7.15         7.4
          │                                               │
        100 %          75 %      50 %      25 %       threshold
       max dose       long dose  mid dose  short dose   (idle)
          │
       ALARM─┘
     (safety band)

  Pump PWM:    pumpMaxPWM  ◄──────────────────────  min+10%   off
  Pulse time:  11 s        ◄──────────────────────  2 s        off
  Rest period: 20 min      ◄──────────────────────  5 min       —
```

```
pH-MINUS pump example  (setpoint 7.4,  band 1.0 pH)
Doses when pH rises above setpoint — lowers pH toward target.

  SP
  │◄─────────────────── Proportional band ─────────────────
  ┼───────────┬─────────┬──────────┬─────────────────────  pH
 7.4         7.65      7.9       8.15                     8.4
  │                                                        │
threshold    25 %      50 %      75 %                   100 %
  (idle)   short dose  mid dose  long dose              max dose
                                                           │
                                                        ALARM─┘
                                                      (safety band)

  Pump PWM:    off  min+10%  ──────────────────────►  pumpMaxPWM
  Pulse time:   —   2 s      ──────────────────────►  11 s
  Rest period:  —   5 min    ──────────────────────►  20 min
```

*The chlorine (ORP) pump follows the pH-PLUS pattern — dosing starts when ORP falls below setpoint.*

> **One direction per pump.** Each `ApaDose` instance controls one chemical direction — either raising pH (`PH_PLUS`) or lowering it (`PH_MINUS`). Running both a pH+ and a pH- pump on the same pool at the same time is **not supported** and will cause the two pumps to fight each other. Choose the direction that matches your water — install only that one pump for pH control.

### The dosing cycle

Every automatic dose passes through six phases:

```
  ┌─────────────────────────────────────────────────────────┐
  │                                                         │
  │  ① SAMPLE BEFORE     2 readings × 30 s apart           │
  │        │             averaged → before-dose value       │
  │        ▼                                               │
  │  ② CALCULATE PULSE                                      │
  │        │   error %  =  |setpoint − reading| / band      │
  │        │   PWM      ∝  error %   (proportional)         │
  │        │   time     ∝  error %   (2 – 11 s)             │
  │        │   rest     ∝  error %   (5 – 20 min)           │
  │        ▼                                               │
  │  ③ RUN PUMP          analogWrite(PWM) for pulse time    │
  │        │                                               │
  │        ▼                                               │
  │  ④ REST              chemical mixes into pool water     │
  │        │             (5 – 20 min, proportional to dose) │
  │        ▼                                               │
  │  ⑤ SAMPLE AFTER      3 readings × 30 s apart           │
  │        │             averaged → after-dose value        │
  │        ▼                                               │
  │  ⑥ EVALUATE FEEDBACK  did the sensor move correctly?   │
  │        ├─ yes ──► reset fail counter, repeat cycle      │
  │        └─ no  ──► boost next dose (+30 % / +50 % PWM)   │
  │                   alarm after 3 consecutive failures     │
  └────────────────────────┬────────────────────────────────┘
                           │ repeat
                           ▼
```

> **Expected timing:** A full cycle takes **8 – 23 minutes** depending on how far the sensor is from setpoint — pre-sampling alone is 1 minute, rest is 5 – 20 minutes, post-sampling is 1.5 minutes. Add any startup blackout on top. Seeing nothing on Serial for several minutes after boot is normal. Enable `APA_DOSE_DEBUG` in `platformio.ini` (`build_flags = -D APA_DOSE_DEBUG`) to print per-cycle progress and confirm the library is running.

### Dosing zones

| Error (% of band) | PWM output | Pulse duration | Rest period |
|:-----------------:|:----------:|:--------------:|:-----------:|
| 0 – 25 %          | proportional | 2 – 4 s      | 5 min       |
| 25 – 50 %         | proportional | 4 – 7 s      | 10 min      |
| 50 – 75 %         | proportional | 7 – 10 s     | 15 min      |
| 75 – 100 %        | pumpMaxPWM   | 11 s         | 20 min      |

A 10 % minimum floor above `pumpMinPWM` is always applied to overcome pipe resistance.

---

## Safety Systems

Most safety features are always active with no configuration required. Two features are opt-in by passing non-zero values to `begin()`: startup blackout and daily dose limit. The filtration interlock and filter-off notification are only active when a `FilterCallback` is supplied to `begin()` — omit it (use the no-filter overload) only when your hardware has no filter pump signal.

| Feature | Behaviour |
|---------|-----------|
| **Filtration interlock** | Dosing is blocked when the filter pump is off. A running dose stops immediately if the filter cuts out mid-dose. **Requires a `FilterCallback` passed to `begin()`; inactive when the no-filter overload is used.** |
| **External stop** | An optional callback registered via `setExternalStopCallback()` can block all dosing from any external system — maintenance mode, backwash cycle, pool cover, or a signal from a filtration controller. A running dose stops the instant the callback returns `true`. After the signal clears, a **mandatory 5-minute settling time** (`EXTERNAL_STOP_RESUME_MS`) must pass before the next dose is allowed — this prevents a brief dose from firing while an operator is still toggling between filtration modes or water is still flowing through a diverted outlet. Priming is exempt. |
| **Startup blackout** | Optional delay (0 – 60 min) after boot before the first dose. Prevents overdosing after a warm restart when the water chemistry is still in transition. Enabled by passing a non-zero `blackoutMinutes` to `begin()`; default is 0 (disabled). |
| **Safety band** | If the sensor drifts beyond `min(band × 1.5, hardCap)` from setpoint, `ALARM_SAFETY_BAND` fires and dosing stops. The check runs every 10 s via the sensor read cycle — not only when a dose is about to start. Clears automatically when the sensor recovers. Hard caps: **±1.0 pH** · **±150 mV ORP**. |
| **Wrong direction detection** | If the sensor moves the wrong way on 3 consecutive cycles, `ALARM_WRONG_DIRECTION` fires. Catches wrong chemical installed or reversed pump wiring before significant harm occurs. |
| **Ineffective dose detection** | If the sensor shows no response after 3 dose attempts, `ALARM_INEFFECTIVE` fires. Catches empty container, blocked tube, or failed pump. |
| **Manual dose ceiling** | `triggerManualDose()` clamps duration to 5 minutes regardless of what is passed. Prevents runaway from automation code errors. |
| **Daily dose limit** | Optional maximum doses per day. Enabled by passing a non-zero `maxDailyDoses` to `begin()`; default is 0 (no limit). `ALARM_DAILY_LIMIT` fires when reached and requires human acknowledgment before dosing resumes. The counter resets every 24 h — at real midnight when an RTC callback is registered, every 24 h from boot without one. |
| **Stale sensor / sensor fault** | If the sensor callback returns invalid or out-of-range values continuously for 2 minutes, or returns no valid value at all for 30 minutes, `ALARM_SENSOR_FAULT` fires and dosing stops. Clears automatically when the sensor recovers — no acknowledgment required. Prevents dosing against a frozen or disconnected sensor. |
| **NaN / infinity guard** | Every sensor reading is validated before use. A single bad value sends one status message but never corrupts averaging, never triggers a false alarm, and never crashes the state machine. |
| **Filter-off notification** | If the filter stays off for 30 minutes, a single `"Filter off>30min"` status message fires. The operator is reminded that circulation has stopped. **Active only when a `FilterCallback` is provided to `begin()`.** |
| **Setpoint range enforcement** | Both pH and ORP setpoints are rejected if outside safe operating bounds. pH: **6.8 – 7.8** (floor prevents dangerously acidic water; ceiling prevents scaling and chlorine inefficiency). ORP: **400 – 850 mV** (floor prevents under-chlorination; ceiling prevents harmful free-chlorine levels for bathers). Out-of-range values are rejected silently and reported via `ALARM_INVALID_PARAM`. |
| **Inter-pump lockout** | After any pump instance completes a dose, all other instances wait 90 s before starting. Prevents back-to-back injection of incompatible chemicals at the same inlet (acid + chlorine → chlorine gas). |

---

## Alarm System

Alarms stop the pump immediately. Each alarm is reported through the `onAlarmTriggered` callback and can also be polled at any time.

| Alarm | Trigger | Recovery |
|-------|---------|----------|
| `ALARM_WRONG_DIRECTION` | Sensor moved wrong way 3× in a row | Fix chemical or wiring → `acknowledgeAlarm()` |
| `ALARM_INEFFECTIVE` | No sensor response after 3 doses | Fix pump or supply → `acknowledgeAlarm()` |
| `ALARM_SAFETY_BAND` | Sensor beyond safety band | Automatic when sensor returns to safe range |
| `ALARM_DAILY_LIMIT` | Max daily doses reached | `acknowledgeAlarm()` (counter auto-resets next midnight / 24 h) |
| `ALARM_SENSOR_FAULT` | Invalid/out-of-range readings for 2 min, or no valid reading for 30 min | Automatic when sensor recovers — no acknowledgment needed |
| `ALARM_INVALID_PARAM` | Bad configuration value | Rejected silently — no alarm stays active |

### Receiving alarms via callback

Register an alarm callback **before** calling `begin()`:

```cpp
void onAlarm(ApaDoseAlarm alarm, const char* message) {
  // message is max 19 chars — fits one row of a 16×2 LCD
  Serial.println(message);
  digitalWrite(PIN_BUZZER, HIGH);
}

void onAlarmCleared(ApaDoseAlarm alarm, const char* message) {
  digitalWrite(PIN_BUZZER, LOW);
}

void setup() {
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20);
}
```

### Polling alarm state

Alarm state can also be polled without callbacks — useful for display code:

```cpp
void loop() {
  phPump.update();

  if (phPump.isAlarmActive()) {
    lcd.print(phPump.getAlarmMessage());  // same text as the callback
    if (buttonPressed())
      phPump.acknowledgeAlarm();
  }
}
```

> See **`examples/advanced/06_alarm_management/`** for a complete alarm UI with LCD and an acknowledge button.  
> See **`examples/expert/07_apa_serial/`** for a serial interface with full alarm reporting.

---

## Quick Start

### Single pH pump

On first boot (blank EEPROM) the library starts with these built-in defaults. They are saved to EEPROM and survive all subsequent power cycles until changed via `setProbeSetpoint()` / `setProportionalBand()`.

| Parameter | pH default | ORP / CL default |
|-----------|:----------:|:----------------:|
| Setpoint  | 7.4        | 700 mV           |
| Band      | 1.0 pH     | 100 mV           |

> **Hardware note:** `PIN_PH_PUMP` must be a PWM-capable pin (marked `~` on Arduino boards). Connect the pump through an N-channel MOSFET or a relay module — the library drives it with `analogWrite()`. A plain relay works too; set `setPumpRange(255, 255)` for on/off mode.

```cpp
#include <APADOSE.h>

ApaDose phPump(PIN_PH_PUMP);  // must be a PWM-capable pin (~)

// Replace with your pH sensor library's read call.
// Must return a float. Called by the library every ~30 s during sampling.
float getpH()         { return phSensor.getPH(); }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }

void onAlarm(ApaDoseAlarm alarm, const char* msg) {
  Serial.println(msg);
}

void setup() {
  phPump.setPumpRange(65, 255);  // 65 = PWM where YOUR pump starts — measure it
  phPump.setCallbacks(onAlarm);
  // PH_MINUS = acid pump (lowers pH); use PH_PLUS for a base pump (raises pH)
  // 20 min startup blackout · max 6 doses/day (pass 0 for either to disable)
  // Normal on first install — returns false only when no valid config exists yet or EEPROM is corrupt
  if (!phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6))
    Serial.println("No saved config — defaults loaded");
}

void loop() {
  // update() is non-blocking — call every iteration; tolerates occasional ms-level loop delays
  phPump.update();
}
```

> **Temperature compensation:** pH sensor readings are temperature-dependent (~0.003 pH/°C). The **APAPHX** and **APAPHX2** libraries apply the Passco 2001 formula to deliver a stable, temperature-compensated value automatically. If using a different sensor library, apply temperature compensation inside your `getSensorValue()` callback so APA-Dose always receives a corrected reading.

### pH + chlorine — two independent pumps

```cpp
#include <APADOSE.h>

ApaDose phPump(PIN_PH_PUMP);
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData));  // each instance uses sizeof(ConfigData) bytes

float getpH()         { return phSensor.getPH(); }
float getORP()        { return orpSensor.getORP(); }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }

void onAlarm(ApaDoseAlarm alarm, const char* msg) { Serial.println(msg); }

void setup() {
  phPump.setPumpRange(65, 255);
  phPump.setCallbacks(onAlarm);
  // PH_MINUS = acid pump (lowers pH); use PH_PLUS for a base pump (raises pH)
  if (!phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20))
    Serial.println("No saved config — defaults loaded");

  clPump.setPumpRange(65, 255);
  clPump.setCallbacks(onAlarm);
  if (!clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20))
    Serial.println("No saved config — defaults loaded");
}

void loop() {
  phPump.update();
  clPump.update();
}
```

> **Two pumps:** After one pump doses, the other waits 90 seconds before it can start — see [Inter-pump lockout](#multi-pump-setup--up-to-four-pumps) below.

---

## Multi-Pump Setup — Up to Four Pumps

Each `ApaDose` instance is a fully independent state machine with its own dosing cycle, feedback loop, alarm state, and EEPROM block. Instances never interfere with each other.

```cpp
#include <APADOSE.h>

ApaDose phPump  (PIN_PH,   APA_DOSE_EEPROM_ADDRESS);
ApaDose clPump  (PIN_CL,   APA_DOSE_EEPROM_ADDRESS +     sizeof(ConfigData));  // EEPROM 212
ApaDose flocPump(PIN_FLOC, APA_DOSE_EEPROM_ADDRESS + 2 * sizeof(ConfigData));  // EEPROM 232
ApaDose algiPump(PIN_ALGI, APA_DOSE_EEPROM_ADDRESS + 3 * sizeof(ConfigData));  // EEPROM 252

void loop() {
  phPump.update();
  clPump.update();
  flocPump.update();  // sensor-less — manual / scheduled dosing only
  algiPump.update();  // sensor-less — manual / scheduled dosing only
}
```

Each pump's EEPROM configuration block is 20 bytes (`sizeof(ConfigData)`). The four-pump layout above occupies addresses 192 – 271, safely clear of the APAPHX2 sensor library (128 – 177).

> **Inter-pump lockout:** After any pump instance completes a dose, all other instances wait 90 seconds before starting their next dose. This prevents acid and chlorine from being injected back-to-back at the same pipe inlet — mixing them there produces chlorine gas. If your second pump seems slow to react after the first one doses, this is why — it is working as intended.

### Sensor-less pumps (flocculant / algaecide)

Pumps with no sensor are initialised by passing `nullptr` as the sensor callback. Proportional dosing never fires — control these pumps entirely with `triggerManualDose()`:

```cpp
// type and dir are required by the API signature but have no effect here:
// proportional dosing never runs without a sensor, so the library never consults them.
// Any valid combination compiles and works — DOSE_PH / PH_PLUS is the conventional placeholder.
flocPump.begin(nullptr, filterRunning, DOSE_PH, PH_PLUS, 0, 1);  // no sensor · no blackout · max 1 dose/day
algiPump.begin(nullptr, filterRunning, DOSE_PH, PH_PLUS, 0, 1);
```

The filtration interlock and daily dose limit remain active even without a sensor.

### Manual and scheduled dosing

`triggerManualDose(durationMs, restMs)` triggers a single dose at `pumpMaxPWM`. Duration is in milliseconds and is **clamped to 5 minutes** — requests above this ceiling are silently reduced and a `"Dose capped:5min"` status message is sent. The optional `restMs` (default 20 min) sets the mixing wait before the next proportional cycle resumes. Each call increments the daily dose counter.

```cpp
// Button-triggered dose — safe to call every loop(); returns false immediately if blocked
if (buttonPressed())
  flocPump.triggerManualDose(30UL * 1000UL);          // 30 s at pumpMaxPWM

// RTC-scheduled weekly dose — call once when the condition is met
if (t.weekday == 1 && t.hour == 8 && !weeklyFlocDone) {
  if (flocPump.triggerManualDose(60UL * 1000UL))      // 60 s at pumpMaxPWM; returns false if blocked
    weeklyFlocDone = true;
}
```

`triggerManualDose()` returns `false` and does nothing when: a dose or prime is already running, an alarm is active, the filter is off (when a `FilterCallback` is provided), the external stop callback returns `true` or its 5-minute resume delay is still active, the daily dose limit is reached, or the 90-second inter-pump lockout is still in effect.

### Priming — filling dry pipes

`triggerPrime(durationMs, pwm)` runs the pump for exactly `durationMs` milliseconds to fill a dry pipe after first installation or a chemical container swap. `pwm` is optional — pass 0 (default) to use `pumpMaxPWM`, or any value between `pumpMinPWM` and `pumpMaxPWM` for a slower fill.

**Priming bypasses all safety guards** — no filter check, no alarm state, no daily dose limit, no inter-pump lockout. Use it only during maintenance, never from automated scheduling code.

```cpp
// Fill the pipe after swapping the acid container — 20 s at full speed
phPump.triggerPrime(20UL * 1000UL);

// Slower prime if pipe or fittings are fragile
phPump.triggerPrime(20UL * 1000UL, 160);              // 160 / 255 PWM
```

A `"Prime done"` status message fires when the duration elapses. Priming does not count toward the daily dose limit. After priming completes the library imposes a minimum 5-minute rest before the next automatic dose starts on that pump instance.

`triggerPrime()` returns `false` if a dose or another prime is already running.

> See **`examples/advanced/05_multi_pump/`** for the complete four-pump example.

---

## Setup Order

Call setup methods in this order — order matters on first boot:

```
1. setPumpRange()               calibrate motor dead band (optional, default 50–255)
                                set min == max (e.g. 255, 255) for solenoid valves — see below
2. setPumpFlowRate()            optional — pump output at max PWM in mL/min (default 450)
                                enables getDailyVolumeMl() / getLastDoseVolumeMl() volume tracking
3. setRTCCallback()             optional — enables time-based scheduling
4. setDosingWindow()            optional — restrict dosing to specific hours
5. setExternalStopCallback()    optional — block dosing from external conditions (maintenance, backwash…)
6. setCallbacks()               register alarm/status callbacks BEFORE begin()
7. begin()                      connect sensor, set type + direction, load EEPROM, start library
                                returns false if EEPROM data was corrupt — defaults are used, safe to continue
```

### `begin()` parameters

Two overloads are available. Use the full form when a filter pump signal is wired; use the no-filter shorthand when it is not:

```cpp
// Full form — filtration interlock and filter-off notification active
pump.begin(sensorReader, filter, type, dir, blackoutMinutes, maxDailyDoses);

// No-filter shorthand — interlock disabled
pump.begin(sensorReader, type, dir, blackoutMinutes, maxDailyDoses);
```

| # | Parameter | Type | Required | Default | Description |
|---|-----------|------|:--------:|---------|-------------|
| 1 | `sensorReader` | `SensorReadCallback` | Yes | — | Callback returning the current sensor value as `float`. Pass `nullptr` for sensor-less pumps (flocculant, algaecide) — proportional dosing is then disabled. |
| 2 | `filter` | `FilterCallback` | No | *(omitted)* | Callback returning `true` when the filter pump is running. Present only in the full form; omit it by using the no-filter overload. |
| 3 | `type` | `ApaDoseType` | Yes | — | Chemical type: `DOSE_PH` (pH pump) or `DOSE_CL` (chlorine/ORP pump). Always overwrites the EEPROM-stored value — hardware type is authoritative. |
| 4 | `dir` | `ApaDoseDirection` | Yes | — | pH dosing direction: `PH_MINUS` (acid, lowers pH) or `PH_PLUS` (base, raises pH). For `DOSE_CL` pumps use `CL_PLUS` — a named alias for `PH_PLUS` that makes intent clear; direction is not used in chlorine control logic. |
| 5 | `blackoutMinutes` | `uint8_t` | No | `0` | Startup delay before the first dose, in minutes. Accepted range 0 – 60; 0 disables the blackout. |
| 6 | `maxDailyDoses` | `uint8_t` | No | `0` | Maximum combined automatic and manual doses per day. 0 = no limit. |

Parameters 5 and 6 are positional and must be supplied in order when used. Trailing defaults can be omitted — `pump.begin(getSensor, filterRunning, DOSE_PH, PH_MINUS, 20)` sets a 20-minute blackout and leaves the daily limit unrestricted.

**EEPROM writes** happen on every call to `setProbeSetpoint()`, `setProportionalBand()`, or `setDosingType()`, and once at `begin()` if no valid config was found. On AVR, `EEPROM.put()` uses `EEPROM.update()` internally and skips bytes that already match, limiting wear. On ESP8266/ESP32, `EEPROM.commit()` is called per save — flash writes are more expensive; call `forceConfigurationSave()` after a batch of changes to guarantee persistence before the next power cycle rather than calling setters one at a time in a configuration flow.

**Diagnostic output** — define `APA_DOSE_DEBUG` before building to print per-cycle messages on `Serial` (sensor readings, PWM value, pulse duration, feedback result).

**PlatformIO** — add to `platformio.ini`:

```ini
build_flags = -D APA_DOSE_DEBUG
```

**Arduino IDE** — open `APADOSE.h` in the library folder and uncomment the prepared line near the top of the file:

```cpp
// #define APA_DOSE_DEBUG   ← remove the leading // to enable
```

Remember to comment it back out before releasing production firmware — the define adds ~200 bytes of flash and runtime `snprintf` overhead on every dosing cycle.

### Peristaltic pumps (default)

`setPumpRange(minPWM, maxPWM)` with `min < max` — both PWM speed and pulse duration scale with error. This is the primary use case.

> **Finding your pump's minimum PWM:** Run **`examples/calibration/00_pump_calibration/`** before writing your main sketch. It is an interactive Serial utility: type a PWM value, the pump runs for 3 seconds, repeat until the shaft turns — then type `done` and it prints the exact `setPumpRange()` line to copy. Run it once per pump; the threshold is specific to each motor and supply voltage.

### Solenoid valves — time-proportional mode

Solenoids are binary devices and cannot be speed-controlled by PWM. Set `min == max` to lock PWM at a fixed level and let pulse duration carry all the proportionality:

```cpp
pump.setPumpRange(255, 255);  // solenoid: always full-on; time varies with error
```

With `min == max`, the dosing zones table still applies — the solenoid opens for 2–11 s proportional to the error percentage. Feedback, safety, and alarm systems work identically.

### Volume tracking

Call `setPumpFlowRate()` with your pump's measured output at max PWM to enable chemical consumption monitoring:

```cpp
phPump.setPumpFlowRate(420.0);  // measured 420 mL/min for this specific pump
```

The default is 450 mL/min if not called. Volume per dose is estimated from actual pulse duration and PWM intensity relative to maximum. Read back with:

```cpp
Serial.print(phPump.getDailyVolumeMl(), 1);    // total mL dosed today
Serial.print(phPump.getLastDoseVolumeMl(), 1);  // mL in the last dose
Serial.print(phPump.getDailyDoseCount());        // number of doses today
```

`getDailyVolumeMl()` resets at midnight when an RTC callback is registered, in sync with `getDailyDoseCount()`. Without an RTC both counters reset every 24 h from boot.

### Adaptive proportional band

Call `enableAdaptivePB(nudgePct)` after `begin()` to switch from a fixed control band to a self-learning one. After each successful feedback cycle the library nudges the effective band toward the pool's true chemical response — wider when the sensor overshot, narrower when it undershot.

```cpp
phPump.enableAdaptivePB(5);  // learn at 5% per feedback cycle (1–25 allowed)

// Read back the current effective band at any time:
Serial.print(F("Band: "));
Serial.println(phPump.getAdaptedPB());  // learned or fixed, whichever is active
```

| Call | Behaviour |
|------|-----------|
| `enableAdaptivePB(5)` | Enable at 5% nudge rate; seeds from fixed band on first enable |
| `enableAdaptivePB(0)` | Disable; learned value is discarded; reverts to fixed band |
| `getAdaptedPB()` | Current effective band (learned when on, fixed when off) |
| `isAdaptivePBEnabled()` | `true` when nudgePct > 0 |

The learned value is saved to EEPROM after every adjustment and restored on reboot. It is per-instance — each pump learns independently.

Full API reference: [`docs/API.md`](docs/API.md)

---

## Platform Support

| Platform | Tested boards | Notes |
|----------|--------------|-------|
| **AVR** | Uno, Mega, Nano, Pro Mini | Full support; fits on Uno (32 KB flash / 2 KB SRAM) |
| **ESP8266** | NodeMCU, Wemos D1 Mini | EEPROM `begin()` / `commit()` handled automatically |
| **ESP32** | ESP32-DevKit, S2, S3, C3 | EEPROM flash emulation handled automatically |
| **STM32** | Nucleo, Blue Pill (STM32duino) | Verified on Blue Pill F103C8 (STM32duino) |

Verified build sizes (`examples/basic/02_ph_and_cl` — two-pump sketch, release mode, clean build):

| Board | Flash | RAM |
|-------|-------|-----|
| Arduino Uno (ATmega328P) | 14,194 B / 32,256 B (44%) | 734 B / 2,048 B (36%) |
| Arduino Mega 2560 | 15,260 B / 253,952 B (6%) | 734 B / 8,192 B (9%) |
| ESP32-DevKit | 290,293 B / 1,310,720 B (22%) | 22,056 B / 327,680 B (7%) |
| NodeMCU v2 (ESP8266) | 276,355 B / 1,044,464 B (26%) | 28,804 B / 81,920 B (35%) |
| Blue Pill (STM32F103C8T6) | 26,952 B / 65,536 B (41%) | 2,612 B / 20,480 B (13%) |

ESP flash totals include the full Arduino framework (WiFi stack, OS); the library itself adds a few KB on top of a bare sketch.

**Dependencies:** `<Arduino.h>` and `<EEPROM.h>` only.  
No APA libraries. No sensor libraries. No communication libraries. No other third-party code.

---

## APA Ecosystem

APA-Dose works standalone with **any** sensor that returns a `float`. For a complete household pool automation solution, it pairs with dedicated APA Devices hardware and libraries to cover every layer of the control stack.

### Build a complete pool controller — no proprietary black box required

Professional pool controllers cost hundreds to thousands of euros, lock you into a closed system, and offer no visibility into what they actually do. The APA Devices ecosystem gives you the same closed-loop proportional dosing, safety interlocks, and alarm logic on hardware you own and software you can read — for a fraction of the price.

Everything you need, layer by layer:

| Layer | Component | What it provides |
|-------|-----------|-----------------|
| **Sensor hardware** | **APAPHX-Board v2** | Ready-made dual-channel pH + ORP measurement board based on ADS1115; I²C, calibrated, temperature-compensated |
| **Sensor firmware** | **APAPHX2_ADS1115** | Non-blocking driver for the APAPHX-Board v2; applies Passco 2001 formula for stable temperature-compensated readings; returns a single `float` |
| **Dosing control** | **APA-Dose** *(this library)* | Proportional pump control, closed-loop feedback, full alarm system, safety interlocks — all the intelligence |
| **Temperature** | **DS2482** | DS2482-800 I²C-to-1-Wire bridge for DS18B20 water temperature sensors; or use any DS18B20 library or raw sensor of your choice |
| **Microcontroller** | Your choice | Any Arduino-compatible board — Uno, Mega, ESP32, STM32 |

Pair an **APAPHX-Board v2** with the **APAPHX2_ADS1115** library and this **APA-Dose** library, add a microcontroller, wire your dosing pumps — and you have a complete, calibrated, safe pool dosing system. Temperature monitoring is optional but straightforward with the DS2482 library or any DS18B20 solution you already have.

No subscriptions. No cloud dependency. No proprietary protocol. Full source code. You control everything.

> See **`examples/expert/`** for complete sketches combining all three libraries.

---

## Files

```
APA-DOSING_LIB/
├── src/
│   ├── APADOSE.h            Main header — all public types and API
│   └── APADOSE.cpp          Implementation
├── docs/
│   └── API.md               Full API reference
├── examples/
│   ├── calibration/         00_pump_calibration  (run first — finds setPumpRange() value)
│   ├── basic/               01_single_ph · 02_ph_and_cl
│   ├── intermediate/        03_serial_diagnostics · 04_lcd_display
│   ├── advanced/            05_multi_pump · 06_alarm_management
│   └── expert/              07–10  combined APAPHX2 + DS2482 sketches
├── LICENSE
├── keywords.txt
├── library.properties
└── CHANGELOG.md
```

---

## Disclaimer

> **This library was developed for private household pool automation only.**
>
> It is **not** designed, tested, or approved for commercial pools, public swimming facilities, spas, or any installation subject to health and safety regulation.
>
> The library is provided **as-is, without warranty of any kind, and without any support**.
>
> Chemical dosing systems carry real safety risks. Incorrect configuration, wiring errors, or sensor failure can result in severe over-dosing that may harm bathers, damage equipment, or create hazardous conditions. The user assumes full responsibility for installation, configuration, safe operation, and compliance with all applicable local regulations.

---

**Author:** kecup@vazac.eu  
**© APA Devices**
