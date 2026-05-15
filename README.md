# APA-Dose Library

**Autonomous proportional chemical dosing for swimming pool automation**  
Part of the **APA Devices** product family.

**Version 3.3.0** &nbsp;·&nbsp; AVR &nbsp;·&nbsp; ESP &nbsp;·&nbsp; STM32 &nbsp;·&nbsp; No external dependencies

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

                                                         SP
          │◄──────────── Proportional band ─────────────►│
  pH  ────┼───────────────┬───────────┬───────────────────┼────
         6.4             6.65        6.9                 7.4
                                                          │
       100 %             75 %       25 %             threshold
      max dose          long dose  short dose          (idle)
          │
       ALARM─┘
     (safety band)

  Pump PWM:   pumpMaxPWM  ◄─────────────────────  min+10%  off
  Pulse time: 11 s        ◄─────────────────────  2 s      off
  Rest period: 20 min     ◄─────────────────────  5 min    —
```

*For a pH-MINUS pump the direction is mirrored — dosing starts when pH rises above the setpoint.*  
*The chlorine (ORP) pump uses the same logic as pH-PLUS — dosing starts when ORP falls below the setpoint.*

> **One direction per pump.** Each `ApaDose` instance controls one chemical direction — either raising pH (`DOSE_PH_PLUS`) or lowering it (`DOSE_PH_MINUS`). Running both a pH+ and a pH- pump on the same pool at the same time is **not supported** and will cause the two pumps to fight each other. Choose the chemical type that matches your water — install only that one pump for pH control.

### The dosing cycle

Every automatic dose passes through six phases:

```
  ┌─────────────────────────────────────────────────────────┐
  │                                                         │
  │  ① SAMPLE BEFORE     2 readings × 30 s apart            │
  │        │             averaged → before-dose value       │
  │        ▼                                                │
  │  ② CALCULATE PULSE                                      │
  │        │   error %  =  |setpoint − reading| / band      │
  │        │   PWM      ∝  error %   (proportional)         │
  │        │   time     ∝  error %   (2 – 11 s)             │
  │        │   rest     ∝  error %   (5 – 20 min)           │
  │        ▼                                                │
  │  ③ RUN PUMP          analogWrite(PWM) for pulse time    │
  │        │                                                │
  │        ▼                                                │
  │  ④ REST              chemical mixes into pool water     │
  │        │             (5 – 20 min, proportional to dose) │
  │        ▼                                                │
  │  ⑤ SAMPLE AFTER      3 readings × 30 s apart            │
  │        │             averaged → after-dose value        │
  │        ▼                                                │
  │  ⑥ EVALUATE FEEDBACK  did the sensor move correctly?    │
  │        ├─ yes ──► reset fail counter, repeat cycle      │
  │        └─ no  ──► boost next dose (+30 % / +50 % PWM)   │
  │                   alarm after 3 consecutive failures    │
  └────────────────────────┬────────────────────────────────┘
                           │ repeat
                           ▼
```

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

All safety features are always active — no configuration required.

| Feature | Behaviour |
|---------|-----------|
| **Filtration interlock** | Dosing is blocked when the filter pump is off. A running dose stops immediately if the filter cuts out mid-dose. |
| **Startup blackout** | Optional delay (0 – 60 min) after boot before the first dose. Prevents overdosing after a warm restart when the water chemistry is still in transition. |
| **Safety band** | If the sensor drifts beyond `min(band × 1.5, hardCap)` from setpoint, `ALARM_SAFETY_BAND` fires and dosing stops. Clears automatically when the sensor recovers. Hard caps: **±1.0 pH** · **±150 mV ORP**. |
| **Wrong direction detection** | If the sensor moves the wrong way on 3 consecutive cycles, `ALARM_WRONG_DIRECTION` fires. Catches wrong chemical installed or reversed pump wiring before significant harm occurs. |
| **Ineffective dose detection** | If the sensor shows no response after 3 dose attempts, `ALARM_INEFFECTIVE` fires. Catches empty container, blocked tube, or failed pump. |
| **Manual dose ceiling** | `triggerManualDose()` clamps duration to 5 minutes regardless of what is passed. Prevents runaway from automation code errors. |
| **Daily dose limit** | Optional maximum doses per day. `ALARM_DAILY_LIMIT` fires when reached and requires human acknowledgment before dosing resumes. |
| **Stale sensor timeout** | If the sensor callback returns no valid value for 30 continuous minutes, automatic dosing suspends and a single warning message is sent. Resumes automatically on the next valid reading. Prevents dosing against a frozen stale value after a sensor cable fault. |
| **NaN / infinity guard** | Every sensor reading is validated before use. A single bad value sends one status message but never corrupts averaging, never triggers a false alarm, and never crashes the state machine. |
| **Filter-off notification** | If the filter stays off for 30 minutes, a single `"Filter off>30min"` status message fires. The operator is reminded that circulation has stopped. |
| **ORP ceiling** | ORP setpoint maximum is 850 mV. Above this level free chlorine becomes harmful to bathers. Setpoints above 850 mV are rejected. |
| **Inter-pump lockout** | After any pump instance completes a dose, all other instances wait 90 s before starting. Prevents back-to-back injection of incompatible chemicals at the same inlet (acid + chlorine → chlorine gas). |

---

## Alarm System

Alarms stop the pump immediately. Each alarm is reported through the `onAlarmTriggered` callback and can also be polled at any time.

| Alarm | Trigger | Recovery |
|-------|---------|----------|
| `ALARM_WRONG_DIRECTION` | Sensor moved wrong way 3× in a row | Fix chemical or wiring → `acknowledgeAlarm()` |
| `ALARM_INEFFECTIVE` | No sensor response after 3 doses | Fix pump or supply → `acknowledgeAlarm()` |
| `ALARM_SAFETY_BAND` | Sensor beyond safety band | Automatic when sensor returns to safe range |
| `ALARM_DAILY_LIMIT` | Max daily doses reached | `acknowledgeAlarm()` |
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
  phPump.begin(getpH, filterRunning, 20);
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
  phPump.setPumpRange(65, 255);         // 65 = PWM where YOUR pump starts — measure it
  phPump.setDosingType(DOSE_PH_MINUS);  // acid pump (lowers pH); use DOSE_PH_PLUS for a base pump
  phPump.setCallbacks(onAlarm);
  if (!phPump.begin(getpH, filterRunning, 20)) {
    // begin() returns false when EEPROM data is corrupt — defaults are loaded automatically
    Serial.println("EEPROM invalid — defaults loaded");
  }
}

void loop() {
  // update() is non-blocking — call every iteration; tolerates occasional ms-level loop delays
  phPump.update();
}
```

> **Temperature compensation:** pH sensor readings are temperature-dependent (~0.003 pH/°C). The **APAPHX2_ADS1115** library applies NTC correction automatically before returning the value. If using a different sensor library, apply temperature compensation inside your `getSensorValue()` callback so APA-Dose always receives a corrected reading.

### pH + chlorine — two independent pumps

```cpp
#include <APADOSE.h>

ApaDose phPump(PIN_PH_PUMP);
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + 14);  // each instance uses 14 EEPROM bytes

float getpH()         { return phSensor.getPH(); }
float getORP()        { return orpSensor.getORP(); }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY); }

void onAlarm(ApaDoseAlarm alarm, const char* msg) { Serial.println(msg); }

void setup() {
  phPump.setPumpRange(65, 255);
  phPump.setCallbacks(onAlarm);
  phPump.begin(getpH, filterRunning, 20);

  clPump.setPumpRange(65, 255);
  clPump.setDosingType(DOSE_CL);   // must be called before begin() on first boot
  clPump.setCallbacks(onAlarm);
  clPump.begin(getORP, filterRunning, 20);
}

void loop() {
  phPump.update();
  clPump.update();
}
```

---

## Multi-Pump Setup — Up to Four Pumps

Each `ApaDose` instance is a fully independent state machine with its own dosing cycle, feedback loop, alarm state, and EEPROM block. Instances never interfere with each other.

```cpp
#include <APADOSE.h>

ApaDose phPump  (PIN_PH,   APA_DOSE_EEPROM_ADDRESS);
ApaDose clPump  (PIN_CL,   APA_DOSE_EEPROM_ADDRESS +     sizeof(ConfigData));
ApaDose flocPump(PIN_FLOC, APA_DOSE_EEPROM_ADDRESS + 2 * sizeof(ConfigData));
ApaDose algiPump(PIN_ALGI, APA_DOSE_EEPROM_ADDRESS + 3 * sizeof(ConfigData));

void loop() {
  phPump.update();
  clPump.update();
  flocPump.update();  // sensor-less — manual / scheduled dosing only
  algiPump.update();  // sensor-less — manual / scheduled dosing only
}
```

Each pump's EEPROM configuration block is 14 bytes. The four-pump layout above occupies addresses 192 – 248, safely clear of the APAPHX2 sensor library (128 – 177).

### Sensor-less pumps (flocculant / algaecide)

Pumps with no sensor are initialised by passing `nullptr` as the sensor callback. Proportional dosing never fires — control these pumps entirely with `triggerManualDose()`:

```cpp
flocPump.begin(nullptr, filterRunning, 0, 1);  // no sensor · no blackout · max 1 dose/day
algiPump.begin(nullptr, filterRunning, 0, 1);
```

The filtration interlock and daily dose limit remain active even without a sensor.

> See **`examples/advanced/05_multi_pump/`** for the complete four-pump example.

---

## Setup Order

Call setup methods in this order — order matters on first boot:

```
1. setPumpRange()      calibrate motor dead band (optional, default 50–255)
                       set min == max (e.g. 255, 255) for solenoid valves — see below
2. setPumpFlowRate()   optional — pump output at max PWM in mL/min (default 450)
                       enables getDailyVolumeMl() / getLastDoseVolumeMl() volume tracking
3. setDosingType()     REQUIRED for CL pumps — must be before begin() on first boot
4. setRTCCallback()    optional — enables time-based scheduling
5. setDosingWindow()   optional — restrict dosing to specific hours
6. setCallbacks()      register alarm/status callbacks BEFORE begin()
7. begin()             connect sensor, load EEPROM, start library
                       returns false if EEPROM data was corrupt — defaults are used, safe to continue
```

**EEPROM writes** happen only when a value changes via `setProbeSetpoint()`, `setProportionalBand()`, or `setDosingType()`, and once at `begin()` if no valid config was found. Call `forceConfigurationSave()` after a batch of changes to guarantee persistence before the next power cycle — useful in web-UI or serial configuration flows where multiple values are updated in quick succession.

**Diagnostic output** — define `APA_DOSE_DEBUG` before building to print per-cycle messages on `Serial` (sensor readings, PWM value, pulse duration, feedback result). Enable in `platformio.ini`:

```ini
build_flags = -D APA_DOSE_DEBUG
```

### Peristaltic pumps (default)

`setPumpRange(minPWM, maxPWM)` with `min < max` — both PWM speed and pulse duration scale with error. This is the primary use case.

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
Serial.print(phPump.getDailyVolumeMl(), 1);   // total mL dosed today
Serial.print(phPump.getLastDoseVolumeMl(), 1); // mL in the last dose
```

`getDailyVolumeMl()` resets at midnight when an RTC callback is registered, in sync with `getDailyDoseCount()`. Without an RTC it accumulates for the session.

Full API reference: [`docs/API.md`](docs/API.md)

---

## Key Features

- **Proportional control** — PWM and pulse duration both scale with error; true proportional, not on/off bang-bang
- **Closed-loop feedback** — before/after sensor averaging confirms every dose; automatically boosts if ineffective
- **Full alarm system** — wrong direction, ineffective dose, safety band, daily limit, stale sensor — all built-in
- **Filtration interlock** — dosing blocked and stopped when filter is off; no chemical injected into stagnant water
- **1 to 4 independent pumps** — each a full class instance, isolated state machine, separate EEPROM block
- **Memory efficient** — full two-pump sketch compiles to ~12 KB flash (38%) and ~530 bytes RAM (26%) on an Uno; ~150 bytes RAM per instance; 12 boolean flags packed into 2 bytes; sensor profiles stored in flash, zero SRAM copies
- **EEPROM persistence** — setpoint, band, and type survive power loss; magic number + checksum validation on every boot
- **RTC scheduling** — optional daily dosing window and automatic dose counter reset; works without RTC
- **Non-blocking** — pure `millis()` state machine; tolerates occasional millisecond-level loop delays; safe to call every `loop()` iteration alongside any other code
- **Universal** — AVR (including Uno), ESP8266, ESP32, STM32 — same source, no `#ifdef` in user code
- **No external dependencies** — only `<Arduino.h>` and `<EEPROM.h>`; no APA libraries or any other third-party library required

---

## Platform Support

| Platform | Tested boards | Notes |
|----------|--------------|-------|
| **AVR** | Uno, Mega, Nano, Pro Mini | Full support; fits on Uno (32 KB flash / 2 KB SRAM) |
| **ESP8266** | NodeMCU, Wemos D1 Mini | EEPROM `begin()` / `commit()` handled automatically |
| **ESP32** | ESP32-DevKit, S2, S3, C3 | EEPROM flash emulation handled automatically |
| **STM32** | Nucleo, Blue Pill (STM32duino) | Full support |

Verified build sizes (two-pump dev sketch, release mode, clean build):

| Board | Flash | RAM |
|-------|-------|-----|
| Arduino Uno (ATmega328P) | 12,666 B / 31,500 B (39%) | 562 B / 2,048 B (27%) |
| Arduino Mega 2560 | 13,718 B / 253,952 B (5%) | 562 B / 8,192 B (7%) |
| ESP32-DevKit | 289,561 B / 1,310,720 B (22%) | 22,024 B / 327,680 B (7%) |
| NodeMCU v2 (ESP8266) | 275,927 B / 1,044,464 B (26%) | 28,616 B / 81,920 B (35%) |

ESP flash totals include the full Arduino framework (WiFi stack, OS); the library itself adds a few KB on top of a bare sketch.

**Dependencies:** `<Arduino.h>` and `<EEPROM.h>` only.  
No APA libraries. No sensor libraries. No communication libraries. No other third-party code.

---

## APA Ecosystem

APA-Dose works standalone with **any** sensor that returns a `float`. For a complete household pool automation solution, it combines naturally with the other APA libraries:

| Library | Role |
|---------|------|
| **APA-Dose** *(this library)* | Dosing pump control — pH up, pH down, chlorine/ORP |
| **APAPHX2_ADS1115** | Dual ADS1115-based pH + ORP sensor board (non-blocking, calibrated) |
| **DS2482** | DS2482-800 I²C-to-1-Wire bridge for DS18B20 water temperature sensors |

Together these three libraries cover the complete sensor → dosing → temperature monitoring stack for a household pool — with no dependencies between them beyond what each one is designed to provide.

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
