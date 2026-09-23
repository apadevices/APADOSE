/*
 * APA-Dose Library - Implementation
 *
 * Version: 3.17.8
 * Author: kecup@vazac.eu (APA Devices)
 * Date: September 2026
 */

#include "APADOSE.h"

unsigned long ApaDose::lastAnyDoseEnd  = 0;
bool          ApaDose::shockModeActive = false;
uint8_t       ApaDose::s_poolVolume    = 0;
uint8_t       ApaDose::s_deadbandPct   = 0;

// ---------------------------------------------------------------------------
// Platform compatibility
// On AVR, F() strings live in a separate address space (Harvard architecture)
// and require strncpy_P to read. On ESP and STM32 the flash is memory-mapped,
// so a plain strncpy via the cast pointer works correctly.
// ---------------------------------------------------------------------------

#ifdef __AVR__
  #define FSTR_TO_BUF(dst, src, n) strncpy_P((dst), (PGM_P)(src), (n))
#else
  #define FSTR_TO_BUF(dst, src, n) strncpy((dst), (const char*)(src), (n))
#endif

// ---------------------------------------------------------------------------
// File-scope PROGMEM helpers
// All status/alarm strings are stored in flash (F() macro) and copied to a
// 20-byte RAM buffer only at the moment the callback fires.
// Max message length is 19 chars + null — fits one row of a 16×2 or 20×4 LCD.
// ---------------------------------------------------------------------------

static void sendStatus(StatusCallback cb, const __FlashStringHelper* msg) {
  if (!cb) return;
  char buf[20];
  FSTR_TO_BUF(buf, msg, 19);
  buf[19] = '\0';
  cb(buf);
}

static void sendCleared(AlarmCallback cb, ApaDoseAlarm prev,
                        const __FlashStringHelper* msg) {
  if (!cb) return;
  char buf[20];
  FSTR_TO_BUF(buf, msg, 19);
  buf[19] = '\0';
  cb(prev, buf);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ApaDose::ApaDose(uint8_t pumpPin, uint16_t eepromAddress)
  : pumpPin(pumpPin),
    pumpMinPWM(50), pumpMaxPWM(255),
    setpoint(PH_SETPOINT_DEFAULT), proportionalBand(PH_BAND_DEFAULT),
    dosingType(DOSE_PH), phDirection(PH_PLUS),
    flags{},
    dosingStartTime(0), lastDosingEnd(0),
    filterOffStart(0), externalStopClearedAt(0),
    sensorValue(PH_SETPOINT_DEFAULT),
    onAlarmTriggered(nullptr), onAlarmCleared(nullptr), onStatusMessage(nullptr),
    readSensor(nullptr), filterPumpRunning(nullptr), externalStop(nullptr), tankEmpty(nullptr), readRTCTime(nullptr),
    startupBlackoutMinutes(0), startupTime(0),
    dosingWindowStart(0), dosingWindowEnd(0),
    lastKnownDay(255), dailyDoseCount(0), maxDailyDoses(0),
    lastSensorRead(0), lastGoodSensorTime(0), lastDailyReset(0),
    primingStartTime(0), primingDuration(0),
    eepromBaseAddress(eepromAddress),
    lastDoseSensorBefore(0.0f), lastDoseSensorAfter(0.0f),
    pumpFlowRateMlPerMin(450.0f), dailyVolumeMl(0.0f), lastDoseVolumeMl(0.0f),
    _dailyPumpRunSec(0), _ofaLimitMin(0),
    _dofaLearnedSec(0), _dofaDailyRunSec(0), _dofaAdaptDays(10),
    _overSetpointSince(0),
    _tankCapacityL(0), _tankConsumedMl(0), _dailyAvgDL(0),
    _schedHour(0), _schedMinute(0), _schedDurationMs(0),
    _schedThreshold(NAN), _schedIntervalDays(1),
    _schedDaysRemaining(0), _schedLastSeenDay(255),
    nudgePct(0), adaptedPB(0.0f),
    shockStartTime(0), postShockCooldownEnd(0),
    shockEffectiveStop(0), shockRiseTarget(0),
    shockMaxDurationHours(0), shockCooldownHours(0)
{
  currentPulse = {0, 0, 0};
  memset(&feedback,        0, sizeof(feedback));
  memset(&alarm,           0, sizeof(alarm));
  memset(&postShockEndRTC, 0, sizeof(postShockEndRTC));
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool ApaDose::begin(SensorReadCallback sensorReader, FilterCallback filter,
                    ApaDoseType type, ApaDoseDirection dir,
                    uint8_t blackoutMinutes, uint8_t maxDailyDosesLimit) {
  readSensor        = sensorReader;
  filterPumpRunning = filter;
  maxDailyDoses     = maxDailyDosesLimit;

  pinMode(pumpPin, OUTPUT);
  analogWrite(pumpPin, 0);

#if defined(ESP8266) || defined(ESP32)
  // ESP EEPROM is flash-emulated: allocate a RAM buffer covering this instance's data block.
  // 512 bytes covers up to ~22 pump instances at the default base address.
  EEPROM.begin(max(512, eepromBaseAddress + (int)sizeof(ConfigData)));
#endif

  startupBlackoutMinutes = constrain(blackoutMinutes, 0, 60);
  if (startupBlackoutMinutes > 0) {
    startupTime               = millis();
    flags.blackoutMessageSent = false;
  }

  bool eepromValid = loadConfiguration();
  ApaDoseType      loadedType = dosingType;
  ApaDoseDirection loadedDir  = phDirection;

  dosingType = type;
  bool orp   = isOrpProfile();

  if (!eepromValid) {
    phDirection = (type == DOSE_PH) ? dir : PH_PLUS;  // direction meaningless for DOSE_CL
    resetToDefaults();
    saveConfiguration();
  } else if (loadedType != type) {
    // Type changed: old direction is meaningless for the new type, fall back to the
    // caller's default. Also clamp setpoint/band to the new type's valid range.
    phDirection = (type == DOSE_PH) ? dir : PH_PLUS;
    if (setpoint < (orp ? ORP_SETPOINT_MIN : PH_SETPOINT_MIN) ||
        setpoint > (orp ? ORP_SETPOINT_MAX : PH_SETPOINT_MAX))
      setpoint = orp ? ORP_SETPOINT_DEFAULT : PH_SETPOINT_DEFAULT;
    if (proportionalBand < (orp ? ORP_BAND_MIN : PH_BAND_MIN) ||
        proportionalBand > (orp ? ORP_BAND_MAX : PH_BAND_MAX))
      proportionalBand = orp ? ORP_BAND_DEFAULT : PH_BAND_DEFAULT;
    saveConfiguration();
  } else {
    // EEPROM valid and type unchanged -- trust what loadConfiguration() already restored,
    // exactly like setpoint/proportionalBand above. phDirection is runtime-owned via
    // setPhDirection() (which already persists correctly); begin()'s own `dir` argument
    // is only ever a first-boot/type-change default, never a value re-applied on every
    // boot over a live operator setting.
    phDirection = (type == DOSE_PH) ? loadedDir : PH_PLUS;
  }
  flags.configurationValid = true;

  lastDailyReset = millis();

  // Prime sensorValue to setpoint so checkSafetyConditions() sees zero error on the
  // very first update() call before any real reading has been received.
  sensorValue          = setpoint;
  lastGoodSensorTime   = millis();

  // Load shared global slot (pool volume + dead-band) — same 3 bytes regardless of instance.
  {
    uint8_t valid = 0;
    EEPROM.get(APA_GLOBAL_EEPROM_ADDR + 2, valid);
    if (valid == APA_GLOBAL_VALID_BYTE) {
      EEPROM.get(APA_GLOBAL_EEPROM_ADDR,     s_poolVolume);
      EEPROM.get(APA_GLOBAL_EEPROM_ADDR + 1, s_deadbandPct);
    }
  }

  if (readSensor != nullptr) {
    float v     = readSensor();
    bool  valid = isfinite(v) && (isOrpProfile()
      ? (v >= ORP_SENSOR_MIN && v <= ORP_SENSOR_MAX)
      : (v >= PH_SENSOR_MIN  && v <= PH_SENSOR_MAX));
    if (valid) {
      sensorValue        = v;
      lastGoodSensorTime = millis();
    }
  } else {
    sendStatus(onStatusMessage, F("No sensor-manual"));
  }

  return eepromValid;
}

bool ApaDose::begin(SensorReadCallback sensorReader,
                    ApaDoseType type, ApaDoseDirection dir,
                    uint8_t blackoutMinutes, uint8_t maxDailyDosesLimit) {
  return begin(sensorReader, nullptr, type, dir, blackoutMinutes, maxDailyDosesLimit);
}

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

void ApaDose::setCallbacks(AlarmCallback alarmTriggered, AlarmCallback alarmCleared,
                           StatusCallback statusMessage) {
  onAlarmTriggered = alarmTriggered;
  onAlarmCleared   = alarmCleared;
  onStatusMessage  = statusMessage;
}

void ApaDose::setRTCCallback(RTCReadCallback rtcReader) {
  readRTCTime = rtcReader;
}

void ApaDose::setDosingWindow(uint8_t startHour, uint8_t endHour) {
  if (startHour >= endHour || endHour > 23) {
    flags.dosingWindowEnabled = false;
    flags.outsideDosingWindow = false;
    return;
  }
  dosingWindowStart         = startHour;
  dosingWindowEnd           = endHour;
  flags.dosingWindowEnabled = true;
}

void ApaDose::setExternalStopCallback(ExternalStopCallback cb) {
  externalStop = cb;
}

void ApaDose::setTankEmptyCallback(TankEmptyCallback cb) {
  tankEmpty = cb;
}

void ApaDose::setPumpRange(uint8_t minPWM, uint8_t maxPWM) {
  if (minPWM > maxPWM) return;
  // min == max is valid: PWM is fixed at that value and proportionality
  // comes from pulse duration only (time-proportional mode for solenoids).
  pumpMinPWM = minPWM;
  pumpMaxPWM = maxPWM;
}

void ApaDose::setPumpFlowRate(float mlPerMin) {
  if (mlPerMin > 0.0f) pumpFlowRateMlPerMin = mlPerMin;
}

void ApaDose::setTankCapacity(uint8_t liters) {
  _tankCapacityL  = (liters > 65) ? 65 : liters;
  _tankConsumedMl = 0;  // new capacity call = tank is full now
  saveConfiguration();
}

// ---------------------------------------------------------------------------
// Main update loop
// ---------------------------------------------------------------------------

void ApaDose::update() {
  if (flags.primingActive) {
    if (millis() - primingStartTime >= primingDuration) {
      analogWrite(pumpPin, 0);
      flags.primingActive = false;
      sendStatus(onStatusMessage, F("Prime done"));
    }
    return;
  }

  // Sensor read runs before shock check — live ORP needed for target comparison and safety ceiling.
  if (readSensor != nullptr && millis() - lastSensorRead >= 10000UL) {
    readSensors();
    lastSensorRead = millis();
  }

  if (flags.shockActive) {
    manageShock();
    return;
  }

  // Post-shock cooldown expiry — millis path
  if (postShockCooldownEnd > 0 && (long)(millis() - postShockCooldownEnd) >= 0) {
    postShockCooldownEnd = 0;
    sendStatus(onStatusMessage, F("Post-shock normal"));
  }
  // Post-shock cooldown expiry — RTC path
  if (postShockEndRTC.year != 0 && readRTCTime != nullptr) {
    ApaDoseTime now = readRTCTime();
    if (toApproxHours(now) >= toApproxHours(postShockEndRTC) + shockCooldownHours) {
      memset(&postShockEndRTC, 0, sizeof(postShockEndRTC));
      sendStatus(onStatusMessage, F("Post-shock normal"));
    }
  }

  manageFeedbackSampling();
  manageScheduledDose();
  manageProportionalDosing();
}

void ApaDose::readSensors() {
  if (readSensor == nullptr) return;
  float raw   = readSensor();
  bool  valid = isfinite(raw) && (isOrpProfile()
    ? (raw >= ORP_SENSOR_MIN && raw <= ORP_SENSOR_MAX)
    : (raw >= PH_SENSOR_MIN  && raw <= PH_SENSOR_MAX));
  if (!valid) {
    if (!flags.sensorValueBad) {
      flags.sensorValueBad = true;
      sendStatus(onStatusMessage, F("Sensor:bad value"));
    }
    if (!flags.alarmActive && millis() - lastGoodSensorTime >= SENSOR_FAULT_MS) {
      char buf[20];
      FSTR_TO_BUF(buf, isfinite(raw) ? F("Fault:range") : F("Fault:NaN/Inf"), 19);
      buf[19] = '\0';
      triggerAlarm(ALARM_SENSOR_FAULT, buf);
    }
    return;
  }
  flags.sensorValueBad    = false;
  flags.sensorStaleWarned = false;
  sensorValue             = raw;
  lastGoodSensorTime      = millis();

  // ALARM_OVER_SETPOINT auto-clears on every fresh reading — no ACK needed.
  // Runs unconditionally so the alarm clears even while update()'s alarm-guard is active.
  if (flags.alarmActive && alarm.currentAlarm == ALARM_OVER_SETPOINT) {
    float delta = dosesUp() ? (setpoint - sensorValue) : (sensorValue - setpoint);
    if (delta >= -(s_deadbandPct / 100.0f) * proportionalBand) clearAlarm();
  }

  if (!flags.alarmActive) {
    if (flags.shockActive) {
      // During shock: standard safety band suspended — absolute ORP ceiling is the backstop.
      if (sensorValue > SHOCK_ORP_MAX) {
        char msg[20];
        snprintf(msg, sizeof(msg), "ORP:%.0f>max", (double)sensorValue);
        triggerAlarm(ALARM_SAFETY_BAND, msg);
      }
    } else if (postShockCooldownEnd > 0 || postShockEndRTC.year != 0) {
      // Post-shock cooldown: safety band suppressed — ORP normalizing after shock.
    } else {
      if (fabsf(setpoint - sensorValue) >= getEffectiveSafetyBand()) {
        char msg[20];
        snprintf(msg, sizeof(msg), "OOB:%.2f SP:%.2f",
                 (double)sensorValue, (double)setpoint);
        triggerAlarm(ALARM_SAFETY_BAND, msg);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Proportional dosing state machine
// ---------------------------------------------------------------------------

void ApaDose::manageProportionalDosing() {
  unsigned long now = millis();

  // Inter-pump shock interlock — hold this instance while another is shocking.
  if (ApaDose::shockModeActive && !flags.shockActive) {
    if (flags.dosingActive) stopDosingPulse();
    if (!flags.shockHoldSent) {
      sendStatus(onStatusMessage, F("Held:shock active"));
      flags.shockHoldSent = true;
    }
    return;
  }
  if (!ApaDose::shockModeActive && flags.shockHoldSent) {
    flags.shockHoldSent = false;
    sendStatus(onStatusMessage, F("Dosing resumed"));
  }

  if (filterPumpRunning != nullptr) {
    if (!filterPumpRunning()) {
      if (filterOffStart == 0) filterOffStart = now;
      if (!flags.filterOffAlarmSent && (now - filterOffStart) >= FILTER_OFF_ALARM_MS) {
        sendStatus(onStatusMessage, F("Filter off>30min"));
        flags.filterOffAlarmSent = true;
      }
    } else {
      filterOffStart           = 0;
      flags.filterOffAlarmSent = false;
    }
  }

  if (externalStop != nullptr) {
    if (externalStop()) {
      externalStopClearedAt = 0;  // cancel any pending resume timer
      if (flags.dosingActive) {
        stopDosingPulse();
        feedback.phase = FB_IDLE;
        sendStatus(onStatusMessage, F("Stop:ext request"));
        flags.externalStopSent = true;
      } else if (!flags.externalStopSent) {
        sendStatus(onStatusMessage, F("ExtStop active"));
        flags.externalStopSent = true;
      }
      return;
    } else {
      if (flags.externalStopSent) {
        sendStatus(onStatusMessage, F("ExtStop cleared"));
        flags.externalStopSent = false;
        externalStopClearedAt = now;
      }
      if (externalStopClearedAt != 0) {
        if (now - externalStopClearedAt < EXTERNAL_STOP_RESUME_MS) return;
        sendStatus(onStatusMessage, F("Dosing resumed"));
        externalStopClearedAt = 0;
      }
    }
  }

  if (startupBlackoutMinutes > 0) {
    if ((now - startupTime) < (unsigned long)startupBlackoutMinutes * 60000UL) {
      if (!flags.blackoutMessageSent) {
        sendStatus(onStatusMessage, F("Blackout active"));
        flags.blackoutMessageSent = true;
      }
      return;
    }
    startupBlackoutMinutes = 0;
    sendStatus(onStatusMessage, F("Dosing enabled"));
  }

  if (flags.alarmActive) {
    if (flags.dosingActive) stopDosingPulse();
    return;
  }

  if (flags.dosingActive) {
    if (filterPumpRunning != nullptr && !filterPumpRunning()) {
      stopDosingPulse();
      sendStatus(onStatusMessage, F("Stop:filter off"));
      return;
    }
    if (now - dosingStartTime >= currentPulse.pulseDuration) {
      stopDosingPulse();
      if (flags.manualDoseActive) {
        flags.manualDoseActive = false;
        sendStatus(onStatusMessage, F("Manual done"));
      } else {
        sendStatus(onStatusMessage, F("Dose done-resting"));
        feedback.phase            = FB_WAITING;
        feedback.nextSampleTime   = now + currentPulse.restPeriod;  // deadline snapshot; nextSampleTime is idle during FB_WAITING
      }
    }
    return;
  }

  if (feedback.phase == FB_WAITING && (long)(now - feedback.nextSampleTime) >= 0) {
    startAfterDosingMeasurements();
  }

  if (lastDosingEnd > 0 && now - lastDosingEnd < currentPulse.restPeriod) return;

  if (filterPumpRunning != nullptr && !filterPumpRunning()) return;

  if (readSensor == nullptr) return;

  if (readRTCTime != nullptr) {
    ApaDoseTime t = readRTCTime();
    if (t.day != lastKnownDay) {
      bool eepromDirty = false;

      // dOFA midnight EMA update — must run before zeroing _dofaDailyRunSec
      if (!flags.dofaDisabled && _dofaDailyRunSec >= DOFA_MIN_DAILY_SEC) {
        if (_dofaLearnedSec == 0) {
          // Cold-start seeding: require at least DOFA_MIN_BASELINE_SEC so the seeded
          // baseline is guaranteed to be ≥ the alarm-activation floor. Days with
          // 60–299 s of run time are skipped — baseline stays 0 and isDOFALearning()
          // stays true until a more representative day arrives.
          if (_dofaDailyRunSec >= DOFA_MIN_BASELINE_SEC) {
            _dofaLearnedSec = _dofaDailyRunSec;
            eepromDirty     = true;
          }
        } else {
          uint32_t upd = ((uint32_t)_dofaLearnedSec * (_dofaAdaptDays - 1) + _dofaDailyRunSec) / _dofaAdaptDays;
          _dofaLearnedSec = (upd > 65535U) ? 65535U : (uint16_t)upd;
          eepromDirty     = true;
        }
      }

      // Tank daily EMA update — N=7 rolling average in decilitres
      if (_tankCapacityL > 0) {
        uint8_t dl = (uint8_t)constrain(dailyVolumeMl / 100.0f, 0.0f, 255.0f);
        if (dl > 0) {
          _dailyAvgDL = (_dailyAvgDL == 0) ? dl
                      : (uint8_t)(((uint16_t)_dailyAvgDL * 6U + dl) / 7U);
        }
        eepromDirty = true;  // _tankConsumedMl must persist across power cycles
      }

      if (eepromDirty) saveConfiguration();

      _dofaDailyRunSec      = 0;
      flags.dofaWarningSent = false;

      dailyDoseCount       = 0;
      dailyVolumeMl        = 0.0f;
      _dailyPumpRunSec     = 0;
      flags.ofaWarningSent = false;
      lastKnownDay         = t.day;
      lastDailyReset       = millis();
      // Midnight auto-clears ALARM_OFA and ALARM_DAILY_LIMIT without operator ACK —
      // intentional: unattended systems must recover overnight without manual intervention.
      if (alarm.currentAlarm == ALARM_DAILY_LIMIT ||
          alarm.currentAlarm == ALARM_OFA) clearAlarm();
    }
    flags.outsideDosingWindow = flags.dosingWindowEnabled &&
                                (t.hour < dosingWindowStart || t.hour >= dosingWindowEnd);
    if (flags.outsideDosingWindow) return;
  } else {
    if (millis() - lastDailyReset >= 24UL * 60UL * 60UL * 1000UL) {
      bool eepromDirty = false;

      // dOFA midnight EMA update — must run before zeroing _dofaDailyRunSec
      if (!flags.dofaDisabled && _dofaDailyRunSec >= DOFA_MIN_DAILY_SEC) {
        if (_dofaLearnedSec == 0) {
          // Cold-start seeding: require at least DOFA_MIN_BASELINE_SEC so the seeded
          // baseline is guaranteed to be ≥ the alarm-activation floor. Days with
          // 60–299 s of run time are skipped — baseline stays 0 and isDOFALearning()
          // stays true until a more representative day arrives.
          if (_dofaDailyRunSec >= DOFA_MIN_BASELINE_SEC) {
            _dofaLearnedSec = _dofaDailyRunSec;
            eepromDirty     = true;
          }
        } else {
          uint32_t upd = ((uint32_t)_dofaLearnedSec * (_dofaAdaptDays - 1) + _dofaDailyRunSec) / _dofaAdaptDays;
          _dofaLearnedSec = (upd > 65535U) ? 65535U : (uint16_t)upd;
          eepromDirty     = true;
        }
      }

      // Tank daily EMA update — N=7 rolling average in decilitres
      if (_tankCapacityL > 0) {
        uint8_t dl = (uint8_t)constrain(dailyVolumeMl / 100.0f, 0.0f, 255.0f);
        if (dl > 0) {
          _dailyAvgDL = (_dailyAvgDL == 0) ? dl
                      : (uint8_t)(((uint16_t)_dailyAvgDL * 6U + dl) / 7U);
        }
        eepromDirty = true;  // _tankConsumedMl must persist across power cycles
      }

      if (eepromDirty) saveConfiguration();

      _dofaDailyRunSec      = 0;
      flags.dofaWarningSent = false;

      dailyDoseCount       = 0;
      dailyVolumeMl        = 0.0f;
      _dailyPumpRunSec     = 0;
      flags.ofaWarningSent = false;
      lastDailyReset       = millis();
      // Midnight auto-clears ALARM_OFA and ALARM_DAILY_LIMIT without operator ACK —
      // intentional: unattended systems must recover overnight without manual intervention.
      if (alarm.currentAlarm == ALARM_DAILY_LIMIT ||
          alarm.currentAlarm == ALARM_OFA) clearAlarm();
    }
  }

  if (lastGoodSensorTime > 0 &&
      (millis() - lastGoodSensorTime) >= SENSOR_STALE_MS) {
    if (!flags.sensorStaleWarned) {
      flags.sensorStaleWarned = true;
      char buf[20];
      FSTR_TO_BUF(buf, F("Stale>30min"), 19);
      buf[19] = '\0';
      triggerAlarm(ALARM_SENSOR_FAULT, buf);
    }
    return;
  }

  checkSafetyConditions();
  if (flags.alarmActive) return;

  if (tankEmpty != nullptr && tankEmpty()) {
    char buf[20];
    FSTR_TO_BUF(buf, F("Tank empty!"), 19);
    buf[19] = '\0';
    triggerAlarm(ALARM_TANK_EMPTY, buf);
    return;
  }

  // Tank estimation alarm — only when no hw sensor registered (hw sensor takes priority).
  if (_tankCapacityL > 0 && tankEmpty == nullptr) {
    if (_tankConsumedMl >= (uint16_t)_tankCapacityL * 1000U) {
      char buf[20];
      FSTR_TO_BUF(buf, F("Tank empty!"), 19);
      buf[19] = '\0';
      triggerAlarm(ALARM_TANK_EMPTY, buf);
      return;
    }
  }

  if (maxDailyDoses > 0 && dailyDoseCount >= maxDailyDoses) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Limit:%d/%d doses", dailyDoseCount, maxDailyDoses);
    triggerAlarm(ALARM_DAILY_LIMIT, msg);
    return;
  }

  if (shouldStartDosing() && feedback.phase == FB_IDLE) {
    _overSetpointSince      = 0;
    flags.overSetpointFired = false;
    startBeforeDosingMeasurements();
  } else if (feedback.phase == FB_IDLE) {
    checkOverSetpoint();
  }
}

// ---------------------------------------------------------------------------
// Feedback sampling (non-blocking)
// ---------------------------------------------------------------------------

bool ApaDose::collectSample(unsigned long now, char prefix) {
  (void)prefix;  // used only in APA_DOSE_DEBUG builds
  if (readSensor == nullptr) return false;
  float reading = readSensor();
  bool  valid   = isfinite(reading) && (isOrpProfile()
    ? (reading >= ORP_SENSOR_MIN && reading <= ORP_SENSOR_MAX)
    : (reading >= PH_SENSOR_MIN  && reading <= PH_SENSOR_MAX));
  if (!valid) {
    feedback.nextSampleTime = now + SAMPLE_INTERVAL;
    return false;
  }
  feedback.sampleSum += reading;
  feedback.sampleCount++;
#ifdef APA_DOSE_DEBUG
  if (onStatusMessage) {
    char msg[20];
    snprintf(msg, sizeof(msg), "%c%d/%d:%.2f",
             prefix, feedback.sampleCount, feedback.targetSamples, (double)reading);
    onStatusMessage(msg);
  }
#endif
  if (feedback.sampleCount < feedback.targetSamples) {
    feedback.nextSampleTime = now + SAMPLE_INTERVAL;
    return false;
  }
  return true;
}

void ApaDose::manageFeedbackSampling() {
  unsigned long now = millis();

  if (feedback.phase == FB_MEASURING_BEFORE && (long)(now - feedback.nextSampleTime) >= 0) {
    if (collectSample(now, 'B')) {
      feedback.valueBeforeDose = feedback.sampleSum / feedback.targetSamples;
      feedback.phase           = FB_IDLE;
      lastDoseSensorBefore     = feedback.valueBeforeDose;
#ifdef APA_DOSE_DEBUG
      if (onStatusMessage) {
        char msg[20];
        snprintf(msg, sizeof(msg), "Bavg:%.2f", (double)feedback.valueBeforeDose);
        onStatusMessage(msg);
      }
#endif
      if (flags.dosingActive) return;
      if (filterPumpRunning != nullptr && !filterPumpRunning()) return;
      if (externalStop      != nullptr && externalStop())       return;
      if (externalStopClearedAt != 0)                           return;
      // Re-check the same interlock shouldStartDosing() checked when this
      // "before" measurement phase began -- collectSample() takes real time
      // (several sample intervals), so the coupled peer's dosing state and the
      // inter-pump lockout must be re-verified right before actually starting
      // the pump, not just when we started watching. Without this, a peer that
      // began dosing DURING the sampling window was never caught -- the actual
      // pulse-start call site had no interlock of its own at all.
      if (lastAnyDoseEnd != 0 && millis() - lastAnyDoseEnd < INTER_PUMP_LOCKOUT_MS) return;
      if (_linkedPhPump != nullptr && _linkedPhPump->flags.dosingActive) return;
      if (_linkedPeer    != nullptr && _linkedPeer->flags.dosingActive)  return;
      // Options J/A can also go stale during the sampling window (pH crossed
      // CL_PH_MAX, or a fresh pH dose both started and ended, resetting the
      // settle timer) without ever setting _linkedPhPump->flags.dosingActive
      // long enough for the check above to catch it -- re-verify explicitly.
      if (!checkPhCoupling()) return;
      DosingPulse pulse = calculateProportionalPulse();
      if (pulse.pwmIntensity > 0) startDosingPulse(pulse);
    }
  }

  if (feedback.phase == FB_MEASURING_AFTER && (long)(now - feedback.nextSampleTime) >= 0) {
    if (collectSample(now, 'A')) {
      feedback.valueAfterDose  = feedback.sampleSum / feedback.targetSamples;
      feedback.phase           = FB_IDLE;
      lastDoseSensorAfter      = feedback.valueAfterDose;
      flags.lastDoseDataValid  = true;
#ifdef APA_DOSE_DEBUG
      if (onStatusMessage) {
        char msg[20];
        snprintf(msg, sizeof(msg), "Aavg:%.2f", (double)feedback.valueAfterDose);
        onStatusMessage(msg);
      }
#endif
      evaluateFeedback();
    }
  }
}

// ---------------------------------------------------------------------------
// Dosing logic helpers
// ---------------------------------------------------------------------------

bool ApaDose::shouldStartDosing() {
  if (lastAnyDoseEnd != 0 && millis() - lastAnyDoseEnd < INTER_PUMP_LOCKOUT_MS) return false;

  // Never start while a coupled pH<->CL peer is CURRENTLY dosing -- lastAnyDoseEnd
  // above only guards the 90s after a dose ENDS, so two pumps whose trigger
  // conditions become true close together (neither having finished yet) could
  // otherwise both start in the same window. Checked both directions: the CL
  // side via _linkedPhPump (set by this instance's own setPhPump() call), the pH
  // side via _linkedPeer (set automatically, reverse of the peer's setPhPump()).
  if (_linkedPhPump != nullptr && _linkedPhPump->flags.dosingActive) return false;
  if (_linkedPeer    != nullptr && _linkedPeer->flags.dosingActive)  return false;

  if (!checkPhCoupling()) return false;

  if (s_deadbandPct > 0) {
    uint8_t exitPct = (s_deadbandPct > 5) ? (s_deadbandPct - 5) : 0;
    float entryAbs  = (s_deadbandPct / 100.0f) * proportionalBand;
    float exitAbs   = (exitPct       / 100.0f) * proportionalBand;
    float delta     = dosesUp() ? (setpoint - sensorValue) : (sensorValue - setpoint);
    if (flags.deadbandSatisfied) {
      // Currently suppressed — only re-enable when error exceeds entry threshold again.
      if (delta >= entryAbs) flags.deadbandSatisfied = false;
      else return false;
    }
    if (exitPct > 0 && delta < exitAbs) {
      flags.deadbandSatisfied = true;
      return false;
    }
    if (delta < entryAbs) return false;
  }
  float threshold = isOrpProfile() ? ORP_FEEDBACK_THRESHOLD : PH_FEEDBACK_THRESHOLD;
  if (dosesUp()) return sensorValue < (setpoint - threshold);
  return sensorValue > (setpoint + threshold);
}

// Options J (pH-first priority) + A (cross-settle) — shared by shouldStartDosing() (gates
// entering the "before" sampling phase) and manageFeedbackSampling()'s pulse-commit point
// (gates actually firing, after sampling took real time). A single helper means both
// checkpoints can never drift out of sync again -- the same class of gap already found once
// between shouldStartDosing() and the commit point for the plain interlock checks.
bool ApaDose::checkPhCoupling() {
  if (_linkedPhPump == nullptr) return true;

  // Option J — pH-first priority: suspend CL when pH too high for effective chlorination
  if (_linkedPhPump->getProbeValue() > CL_PH_MAX) {
    if (!flags.phHoldSent) {
      sendStatus(onStatusMessage, F("CL held: pH high"));
      flags.phHoldSent = true;
    }
    flags.settleHoldSent = false;
    return false;
  }
  if (flags.phHoldSent) {
    sendStatus(onStatusMessage, F("CL resumed: pH OK"));
    flags.phHoldSent = false;
  }

  // Option A — cross-settle: hold CL after a pH dose to let chemistry equilibrate
  if (_crossSettleMinutes > 0) {
    unsigned long phLastDose = _linkedPhPump->getLastDosingEnd();
    if (phLastDose != 0) {
      unsigned long settleMs = (unsigned long)_crossSettleMinutes * 60000UL;
      if ((long)(millis() - phLastDose) < (long)settleMs) {
        if (!flags.settleHoldSent) {
          sendStatus(onStatusMessage, F("CL held: settling"));
          flags.settleHoldSent = true;
        }
        return false;
      }
    }
    if (flags.settleHoldSent) {
      sendStatus(onStatusMessage, F("CL resumed: settled"));
      flags.settleHoldSent = false;
    }
  }

  return true;
}

DosingPulse ApaDose::calculateProportionalPulse() {
  DosingPulse pulse = {0, 0, 0};

  float effectivePB  = (nudgePct > 0 && adaptedPB > 0.0f) ? adaptedPB : proportionalBand;
  float sensorError  = fabsf(setpoint - sensorValue);
  float errorPercent = constrain((sensorError / effectivePB) * 100.0f, 0.0f, 100.0f);

  float effectiveRange = pumpMaxPWM - pumpMinPWM;
  float minDosePWM     = pumpMinPWM + 0.10f * effectiveRange;
  float rawPWM         = pumpMinPWM + (errorPercent / 100.0f) * effectiveRange;
  pulse.pwmIntensity   = (uint8_t)constrain(max(rawPWM, minDosePWM), 0.0f, 255.0f);

  if (errorPercent <= 25.0f) {
    pulse.pulseDuration = (unsigned long)(10000.0f + (errorPercent / 25.0f) * 20000.0f);
    pulse.restPeriod    =  5UL * 60UL * 1000UL;
  } else if (errorPercent <= 50.0f) {
    pulse.pulseDuration = (unsigned long)(30000.0f + ((errorPercent - 25.0f) / 25.0f) * 30000.0f);
    pulse.restPeriod    = 10UL * 60UL * 1000UL;
  } else if (errorPercent <= 75.0f) {
    pulse.pulseDuration = (unsigned long)(60000.0f + ((errorPercent - 50.0f) / 25.0f) * 60000.0f);
    pulse.restPeriod    = 15UL * 60UL * 1000UL;
  } else {
    pulse.pulseDuration = ZONE4_PULSE_MS;
    pulse.restPeriod    = 20UL * 60UL * 1000UL;
  }

  float vs = volumeScale();
  pulse.pulseDuration = (unsigned long)(pulse.pulseDuration * vs);
  pulse.restPeriod    = (unsigned long)(pulse.restPeriod    * vs);

#ifdef APA_DOSE_DEBUG
  if (onStatusMessage) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Err:%.0f%% P:%d D:%lu",
             (double)errorPercent, pulse.pwmIntensity, pulse.pulseDuration / 1000UL);
    onStatusMessage(msg);
  }
#endif

  return pulse;
}

void ApaDose::startDosingPulse(DosingPulse pulse) {
  pulse = applyFeedbackCorrections(pulse);

  currentPulse       = pulse;
  flags.dosingActive = true;
  dosingStartTime    = millis();
  dailyDoseCount++;

  analogWrite(pumpPin, pulse.pwmIntensity);

#ifdef APA_DOSE_DEBUG
  if (onStatusMessage) {
    char msg[20];
    const char* dir = (dosingType == DOSE_CL)  ? "CL" :
                      (phDirection == PH_PLUS) ? "pH+" : "pH-";
    snprintf(msg, sizeof(msg), "Dose %s P:%d", dir, pulse.pwmIntensity);
    onStatusMessage(msg);
  }
#endif
}

void ApaDose::stopDosingPulse() {
  unsigned long now            = millis();
  unsigned long actualDuration = now - dosingStartTime;
  lastDoseVolumeMl = (currentPulse.pwmIntensity / 255.0f)
                   * (pumpFlowRateMlPerMin / 60000.0f)
                   * (float)actualDuration;
  dailyVolumeMl   += lastDoseVolumeMl;

  if (_tankCapacityL > 0) {
    uint32_t total  = (uint32_t)_tankConsumedMl + (uint16_t)min(65535.0f, lastDoseVolumeMl);
    _tankConsumedMl = (total > 65535U) ? 65535U : (uint16_t)total;
  }

  analogWrite(pumpPin, 0);
  flags.dosingActive = false;
  lastDosingEnd      = now;
  lastAnyDoseEnd     = now;

  if (!flags.manualDoseActive) {
    accumulateAndCheckOFA(actualDuration);
    accumulateAndCheckDOFA(actualDuration);
  }
}

DosingPulse ApaDose::applyFeedbackCorrections(DosingPulse p) {
  if (feedback.failedAttempts == 1) {
    p.pwmIntensity = (uint8_t)min((int)pumpMaxPWM, (int)(p.pwmIntensity * 1.3f));
  } else if (feedback.failedAttempts >= 2) {
    p.pwmIntensity  = (uint8_t)min((int)pumpMaxPWM, (int)(p.pwmIntensity * 1.5f));
    p.pulseDuration = min((unsigned long)(FEEDBACK_PULSE_MAX_MS * volumeScale()), (unsigned long)(p.pulseDuration * 1.3f));
  }

#ifdef APA_DOSE_DEBUG
  if (feedback.failedAttempts > 0 && onStatusMessage) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Corr attempt %d", feedback.failedAttempts + 1);
    onStatusMessage(msg);
  }
#endif

  return p;
}

void ApaDose::startBeforeDosingMeasurements() {
#ifdef APA_DOSE_DEBUG
  sendStatus(onStatusMessage, F("Sampling before..."));
#endif
  feedback.phase          = FB_MEASURING_BEFORE;
  feedback.sampleSum      = 0.0f;
  feedback.sampleCount    = 0;
  feedback.targetSamples  = BEFORE_SAMPLES;
  feedback.nextSampleTime = millis();
}

void ApaDose::startAfterDosingMeasurements() {
#ifdef APA_DOSE_DEBUG
  sendStatus(onStatusMessage, F("Sampling after..."));
#endif
  feedback.phase          = FB_MEASURING_AFTER;
  feedback.sampleSum      = 0.0f;
  feedback.sampleCount    = 0;
  feedback.targetSamples  = AFTER_SAMPLES;
  feedback.nextSampleTime = millis();
}

void ApaDose::evaluateFeedback() {
  // Dose efficiency EMA — always first; core dosing infrastructure, not optional.
  // Tracks delivery health across all auto proportional doses. Alarm fires when a dose
  // achieves less than _efficiencyThresholdPct% of the learned baseline (0 = alarm disabled).
  {
    unsigned long doseDurationMs = lastDosingEnd - dosingStartTime;
    if (doseDurationMs > 0) {
      float delta      = fabsf(feedback.valueAfterDose - feedback.valueBeforeDose);
      float normalized = delta / (float)doseDurationMs;
      if (_efficiencyCount < 255) _efficiencyCount++;
      _efficiencyEma = (_efficiencyCount == 1)
        ? normalized
        : (EFFICIENCY_EMA_ALPHA * normalized + (1.0f - EFFICIENCY_EMA_ALPHA) * _efficiencyEma);
      _lastEfficiencyPct = (_efficiencyEma > 0.0f)
        ? (uint8_t)constrain((normalized / _efficiencyEma) * 100.0f, 0.0f, 100.0f)
        : 100;
      if (_efficiencyCount > 3 && _efficiencyThresholdPct > 0 &&
          _lastEfficiencyPct < _efficiencyThresholdPct) {
        char buf[20];
        FSTR_TO_BUF(buf, F("Pump/supply fail"), 19);
        buf[19] = '\0';
        triggerAlarm(ALARM_INEFFECTIVE, buf);
        return;
      }
    }
  }

  float change    = feedback.valueAfterDose - feedback.valueBeforeDose;
  float threshold = isOrpProfile() ? ORP_FEEDBACK_THRESHOLD : PH_FEEDBACK_THRESHOLD;

  if (dosesUp()) {
    if (change < -threshold) {
      feedback.wrongDirectionCount++;
      if (feedback.wrongDirectionCount >= 3) {
        char buf[20];
        FSTR_TO_BUF(buf, (dosingType == DOSE_CL ? F("Wrong chem-ORP?")
                                                : F("Wrong chem-pH+?")), 19);
        buf[19] = '\0';
        triggerAlarm(ALARM_WRONG_DIRECTION, buf);
        return;
      }
    } else { feedback.wrongDirectionCount = 0; }
  } else {
    if (change > threshold) {
      feedback.wrongDirectionCount++;
      if (feedback.wrongDirectionCount >= 3) {
        char buf[20];
        FSTR_TO_BUF(buf, F("Wrong chem-pH-?"), 19);
        buf[19] = '\0';
        triggerAlarm(ALARM_WRONG_DIRECTION, buf);
        return;
      }
    } else { feedback.wrongDirectionCount = 0; }
  }

  bool effective = dosesUp() ? (change > threshold) : (change < -threshold);

  if (onStatusMessage) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Fb:%.2f->%.2f %s",
             (double)feedback.valueBeforeDose, (double)feedback.valueAfterDose,
             effective ? "OK" : "BAD");
    onStatusMessage(msg);
  }

  if (effective) {
    feedback.failedAttempts = 0;

    // Adaptive PB nudge — only on confirmed effective doses (correct direction, > threshold)
    if (nudgePct > 0) {
      float actualShift   = fabsf(feedback.valueAfterDose - feedback.valueBeforeDose);
      float expectedShift = fabsf(setpoint - feedback.valueBeforeDose);
      if (expectedShift > 0.0f) {
        if (actualShift > expectedShift)
          adaptedPB *= (1.0f + nudgePct / 100.0f);  // overshot → widen (less aggressive)
        else
          adaptedPB *= (1.0f - nudgePct / 100.0f);  // undershot → narrow (more aggressive)
        adaptedPB = constrain(adaptedPB, 0.2f * proportionalBand, 3.0f * proportionalBand);
        saveConfiguration();
      }
    }
  } else {
    feedback.failedAttempts++;
    if (feedback.failedAttempts >= 3) {
      char buf[20];
      FSTR_TO_BUF(buf, F("Pump/supply fail"), 19);
      buf[19] = '\0';
      triggerAlarm(ALARM_INEFFECTIVE, buf);
    }
  }
}

void ApaDose::checkSafetyConditions() {
  if (postShockCooldownEnd > 0 || postShockEndRTC.year != 0) return;

  float safetyBand  = getEffectiveSafetyBand();
  float sensorError = fabsf(setpoint - sensorValue);

  if (sensorError >= safetyBand) {
    char msg[20];
    snprintf(msg, sizeof(msg), "OOB:%.2f SP:%.2f",
             (double)sensorValue, (double)setpoint);
    triggerAlarm(ALARM_SAFETY_BAND, msg);
    return;
  }

  if (flags.alarmActive) checkAlarmClearConditions();
}

// ---------------------------------------------------------------------------
// Alarm management
// ---------------------------------------------------------------------------

void ApaDose::triggerAlarm(ApaDoseAlarm type, const char* message) {
  if (flags.alarmActive && alarm.currentAlarm == type) return;

  if (flags.dosingActive) stopDosingPulse();

  alarm.currentAlarm  = type;
  flags.alarmActive   = true;
  strncpy(alarm.alarmMessage, message, sizeof(alarm.alarmMessage) - 1);
  alarm.alarmMessage[sizeof(alarm.alarmMessage) - 1] = '\0';
  flags.alarmNeedsAck = (type == ALARM_WRONG_DIRECTION ||
                         type == ALARM_INEFFECTIVE    ||
                         type == ALARM_TANK_EMPTY     ||
                         type == ALARM_OFA);

  feedback.failedAttempts = 0;
  feedback.phase          = FB_IDLE;

  if (onAlarmTriggered) onAlarmTriggered(type, message);
}

void ApaDose::checkAlarmClearConditions() {
  if (flags.alarmNeedsAck) return;

  bool canClear = false;
  switch (alarm.currentAlarm) {
    case ALARM_SAFETY_BAND:
      canClear = (fabsf(setpoint - sensorValue) < getEffectiveSafetyBand());
      break;
    case ALARM_SENSOR_FAULT:
      canClear = !flags.sensorValueBad && !flags.sensorStaleWarned;
      break;
    case ALARM_OFA:
      canClear = true;  // ACK is sufficient; counter resets in clearAlarm()
      break;
    case ALARM_OVER_SETPOINT: {
      // Same condition readSensors() already uses for its own dedicated auto-clear
      // (line ~315) -- must match exactly, or a consumer that polls acknowledgeAlarm()
      // for every non-ACK alarm (a reasonable, documented pattern; see the
      // ALARM_SAFETY_BAND case above, which genuinely needs that poll since it has no
      // other re-check path) will clear this alarm unconditionally the instant it's
      // called, regardless of whether the reading is still on the wrong side of
      // setpoint. Found 2026-09-23 via real hardware: fired correctly at 30 min, then
      // cleared ~1s later while pH was still 0.26 above setpoint, because this case was
      // missing and fell through to the unconditional `default`.
      float delta = dosesUp() ? (setpoint - sensorValue) : (sensorValue - setpoint);
      canClear = (delta >= -(s_deadbandPct / 100.0f) * proportionalBand);
      break;
    }
    default:
      canClear = true;
      break;
  }

  if (canClear) clearAlarm();
}

void ApaDose::clearAlarm() {
  if (alarm.currentAlarm == ALARM_OVER_SETPOINT) {
    _overSetpointSince      = 0;
    flags.overSetpointFired = false;
  }
  if (alarm.currentAlarm == ALARM_TANK_EMPTY) {
    // ACK = tank refilled — reset consumed counter to start tracking a fresh full tank.
    _tankConsumedMl = 0;
    saveConfiguration();
  }
  if (alarm.currentAlarm == ALARM_OFA) {
    // ACK resets both counters regardless of which system fired the alarm (fixed OFA or dOFA).
    // Wiping _dofaDailyRunSec means today's partial dOFA run is lost — the EMA update at
    // midnight is skipped for this day. Accepted trade-off: ACK = "operator acknowledges today
    // was abnormal; start fresh." The baseline adapts naturally over subsequent normal days.
    _dailyPumpRunSec      = 0;
    flags.ofaWarningSent  = false;
    _dofaDailyRunSec      = 0;
    flags.dofaWarningSent = false;
  }
  ApaDoseAlarm previous  = alarm.currentAlarm;
  alarm.currentAlarm     = ALARM_NONE;
  flags.alarmActive      = false;
  flags.alarmNeedsAck    = false;
  alarm.alarmMessage[0]  = '\0';
  feedback.failedAttempts      = 0;
  feedback.wrongDirectionCount = 0;

  sendCleared(onAlarmCleared, previous, F("Alarm cleared"));
}

const char* ApaDose::getAlarmName(ApaDoseAlarm type) {
  switch (type) {
    case ALARM_WRONG_DIRECTION: return "Wrong direction";
    case ALARM_INEFFECTIVE:     return "Dose ineffective";
    case ALARM_SAFETY_BAND:     return "Safety band!";
    case ALARM_INVALID_PARAM:   return "Invalid param";
    case ALARM_DAILY_LIMIT:     return "Daily limit";
    case ALARM_SENSOR_FAULT:    return "Sensor fault";
    case ALARM_TANK_EMPTY:      return "Tank empty";
    case ALARM_OFA:             return "OFA limit";
    case ALARM_OVER_SETPOINT:   return "Over setpoint";
    default:                    return "Unknown";
  }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

bool ApaDose::setProbeSetpoint(float newSetpoint) {
  if (flags.dosingActive) return false;
  bool orp = isOrpProfile();
  if (newSetpoint < (orp ? ORP_SETPOINT_MIN : PH_SETPOINT_MIN) ||
      newSetpoint > (orp ? ORP_SETPOINT_MAX : PH_SETPOINT_MAX)) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Bad SP:%.2f", (double)newSetpoint);
    if (onStatusMessage)       onStatusMessage(msg);
    else if (onAlarmTriggered) onAlarmTriggered(ALARM_INVALID_PARAM, msg);
    return false;
  }
  setpoint = newSetpoint;
  saveConfiguration();
  return true;
}

bool ApaDose::setProportionalBand(float newBand) {
  if (flags.dosingActive) return false;
  bool orp = isOrpProfile();
  if (newBand < (orp ? ORP_BAND_MIN : PH_BAND_MIN) ||
      newBand > (orp ? ORP_BAND_MAX : PH_BAND_MAX)) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Bad band:%.2f", (double)newBand);
    if (onStatusMessage)       onStatusMessage(msg);
    else if (onAlarmTriggered) onAlarmTriggered(ALARM_INVALID_PARAM, msg);
    return false;
  }
  proportionalBand = newBand;
  saveConfiguration();
  return true;
}

bool ApaDose::setDosingType(ApaDoseType newType) {
  if (flags.dosingActive) return false;
  dosingType = newType;
  bool orp   = isOrpProfile();
  if (setpoint         < (orp ? ORP_SETPOINT_MIN : PH_SETPOINT_MIN) ||
      setpoint         > (orp ? ORP_SETPOINT_MAX : PH_SETPOINT_MAX))
    setpoint         = orp ? ORP_SETPOINT_DEFAULT : PH_SETPOINT_DEFAULT;
  if (proportionalBand < (orp ? ORP_BAND_MIN : PH_BAND_MIN) ||
      proportionalBand > (orp ? ORP_BAND_MAX : PH_BAND_MAX))
    proportionalBand = orp ? ORP_BAND_DEFAULT : PH_BAND_DEFAULT;
  saveConfiguration();
  return true;
}

bool ApaDose::setPhDirection(ApaDoseDirection newDir) {
  if (flags.dosingActive) return false;
  if (dosingType == DOSE_CL)  return false;
  phDirection = newDir;
  saveConfiguration();
  return true;
}

void ApaDose::enableAdaptivePB(uint8_t pct) {
  nudgePct = (pct > 25) ? 25 : pct;
  if (nudgePct == 0) {
    adaptedPB = 0.0f;  // forget learned value — revert to fixed PB
  } else if (adaptedPB <= 0.0f || !isfinite(adaptedPB)) {
    adaptedPB = proportionalBand;  // seed from current fixed PB on first enable
  }
  saveConfiguration();
}

void ApaDose::acknowledgeAlarm() {
  if (flags.alarmActive) {
    flags.alarmNeedsAck = false;
    checkAlarmClearConditions();
  }
}

void ApaDose::forceConfigurationSave() {
  saveConfiguration();
}

void ApaDose::factoryReset() {
  if (flags.shockActive)   stopShock(F("Shock stopped"));  // pump off, counters cleared
  if (flags.dosingActive)  stopDosingPulse();
  if (flags.primingActive) { analogWrite(pumpPin, 0); flags.primingActive = false; }
  if (flags.alarmActive)   clearAlarm();
  // Clear post-shock cooldown — full clean slate on factory reset.
  postShockCooldownEnd = 0;
  memset(&postShockEndRTC, 0, sizeof(postShockEndRTC));
  // Zero currentPulse so its restPeriod cannot block the first dose after reset.
  // stopDosingPulse() sets lastDosingEnd; without this zero, the old restPeriod
  // would keep dosing blocked for minutes even on a fresh start.
  currentPulse = {0, 0, 0};
  // Reset feedback state machine so a half-finished sampling cycle cannot resume
  // against new default values and fire a spurious dose on the next update().
  memset(&feedback, 0, sizeof(feedback));
  // Dead-band is a tuning parameter — must be the reliable escape hatch when dosing
  // misbehaves after dead-band is set. Pool volume is an installation parameter
  // (physical pool size) — intentionally NOT cleared here.
  s_deadbandPct = 0;
  saveGlobalSlot();
  resetToDefaults();
  saveConfiguration();
  sendStatus(onStatusMessage, F("Factory reset"));
}

// ---------------------------------------------------------------------------
// Shock / super-chlorination
// ---------------------------------------------------------------------------

bool ApaDose::triggerShock(uint16_t targetORP, float currentPH,
                           uint8_t cooldownHours) {
  return triggerShock(targetORP, SHOCK_MAX_DURATION_HOURS, currentPH, cooldownHours);
}

bool ApaDose::triggerShock(uint16_t targetORP, uint8_t maxDurationHours, float currentPH,
                           uint8_t cooldownHours) {
  if (dosingType != DOSE_CL)                              return false;  // CL instances only
  if (filterPumpRunning == nullptr)                        return false;  // filter callback required
  if (!filterPumpRunning())                                return false;  // filter must be running
  if (externalStop != nullptr && externalStop())           return false;  // external stop active
  if (flags.alarmActive)                                   return false;  // active alarm blocks shock
  if (flags.dosingActive || flags.primingActive)           return false;  // something already running
  if (currentPH  < CL_PH_MIN  || currentPH  > CL_PH_MAX)  return false;  // pH 7.0–7.6 required
  if (targetORP  < (uint16_t)SHOCK_ORP_MIN ||
      targetORP  > (uint16_t)SHOCK_ORP_MAX)               return false;  // ORP target 600–800 mV
  if (sensorValue >= (float)targetORP)                     return false;  // ORP already at or above target

  // Inter-shock interval guard — cooldown check (millis path)
  if (postShockCooldownEnd > 0 && (long)(millis() - postShockCooldownEnd) < 0) return false;
  // Inter-shock interval guard — cooldown check (RTC path)
  if (postShockEndRTC.year != 0 && readRTCTime != nullptr) {
    ApaDoseTime now = readRTCTime();
    if (toApproxHours(now) < toApproxHours(postShockEndRTC) + shockCooldownHours) return false;
  }

  float gap             = (float)targetORP - sensorValue;
  shockEffectiveStop    = (uint16_t)(sensorValue + gap * (1.0f - SHOCK_OVERSHOOT_MARGIN));
  shockRiseTarget       = (uint16_t)(sensorValue + SHOCK_RISE_MIN_MV);
  shockMaxDurationHours = min(maxDurationHours, SHOCK_MAX_DURATION_HOURS);
  shockCooldownHours    = min(cooldownHours,    SHOCK_COOLDOWN_MAX_HOURS);
  shockStartTime        = millis();
  flags.shockActive        = true;
  ApaDose::shockModeActive = true;

  analogWrite(pumpPin, pumpMaxPWM);

  feedback.failedAttempts      = 0;
  feedback.wrongDirectionCount = 0;
  feedback.phase               = FB_IDLE;

  sendStatus(onStatusMessage, F("Shock started"));
  return true;
}

// ---------------------------------------------------------------------------
// Over-setpoint protection
// ---------------------------------------------------------------------------

void ApaDose::checkOverSetpoint() {
  // dosesUp() true → pump raises value (PH_PLUS, CL).
  // delta > 0: reading on correct side (needs dosing). delta < 0: reading past setpoint wrong way.
  float delta   = dosesUp() ? (setpoint - sensorValue) : (sensorValue - setpoint);
  float mirrorW = (s_deadbandPct / 100.0f) * proportionalBand;

  if (delta >= -mirrorW) {
    // Within mirror zone or on correct side — reset timer and flag.
    _overSetpointSince      = 0;
    flags.overSetpointFired = false;
    return;
  }

  // Reading is past the mirror threshold on the wrong side of the setpoint.
  // Start the timer on first detection; fire alarm after OVER_SETPOINT_DELAY_MS.
  unsigned long now = millis();
  if (_overSetpointSince == 0) _overSetpointSince = now;

  if (!flags.overSetpointFired &&
      (now - _overSetpointSince) >= OVER_SETPOINT_DELAY_MS) {
    char buf[20];
    FSTR_TO_BUF(buf, dosesUp() ? F("OverSP:too high") : F("OverSP:too low"), 19);
    buf[19] = '\0';
    triggerAlarm(ALARM_OVER_SETPOINT, buf);
    flags.overSetpointFired = true;
  }
}

void ApaDose::manageShock() {
  unsigned long now = millis();

  if (flags.alarmActive) {
    stopShock(F("Shock:alarm fired"));
    return;
  }
  if (filterPumpRunning != nullptr && !filterPumpRunning()) {
    stopShock(F("Shock:filter off"));
    return;
  }
  if (externalStop != nullptr && externalStop()) {
    stopShock(F("Shock:ext stop"));
    return;
  }

  // ORP rise check — one-shot at SHOCK_RISE_CHECK_MS scaled by pool volume (larger pools need longer)
  if (shockRiseTarget != 0 && now - shockStartTime >= (unsigned long)(SHOCK_RISE_CHECK_MS * volumeScale())) {
    if ((uint16_t)sensorValue >= shockRiseTarget) {
      shockRiseTarget = 0;  // ORP is rising — check done, never fires again
    } else {
      char buf[20];
      FSTR_TO_BUF(buf, F("Shock:no ORP rise"), 19);
      buf[19] = '\0';
      stopShock(F("Shock:no ORP rise"));
      triggerAlarm(ALARM_INEFFECTIVE, buf);
      return;
    }
  }

  if ((uint16_t)sensorValue >= shockEffectiveStop) {
    stopShock(F("Shock done:target"));
    return;
  }
  if (now - shockStartTime >= (unsigned long)shockMaxDurationHours * 3600000UL) {
    stopShock(F("Shock done:timeout"));
    return;
  }
}

void ApaDose::stopShock(const __FlashStringHelper* msg) {
  analogWrite(pumpPin, 0);

  unsigned long now     = millis();
  unsigned long elapsed = now - shockStartTime;
  lastDoseVolumeMl = (pumpMaxPWM / 255.0f) * (pumpFlowRateMlPerMin / 60000.0f) * (float)elapsed;
  dailyVolumeMl   += lastDoseVolumeMl;

  if (_tankCapacityL > 0) {
    uint32_t total  = (uint32_t)_tankConsumedMl + (uint16_t)min(65535.0f, lastDoseVolumeMl);
    _tankConsumedMl = (total > 65535U) ? 65535U : (uint16_t)total;
  }

  lastDosingEnd            = now;
  lastAnyDoseEnd           = now;
  flags.shockActive        = false;
  ApaDose::shockModeActive = false;

  accumulateAndCheckOFA(elapsed);

  if (readRTCTime != nullptr) {
    postShockEndRTC      = readRTCTime();  // shock-end wall clock; cooldown checked via toApproxHours
    postShockCooldownEnd = 0;
  } else {
    postShockCooldownEnd = now + (unsigned long)shockCooldownHours * 3600000UL;
    memset(&postShockEndRTC, 0, sizeof(postShockEndRTC));
  }

  feedback.failedAttempts      = 0;
  feedback.wrongDirectionCount = 0;
  feedback.phase               = FB_IDLE;

  sendStatus(onStatusMessage, msg);
}

uint32_t ApaDose::toApproxHours(ApaDoseTime t) {
  return (uint32_t)t.year * 8760UL + (uint32_t)t.month * 720UL +
         (uint32_t)t.day  * 24UL   + (uint32_t)t.hour;
}

// ---------------------------------------------------------------------------
// Scheduled dose (C-pred)
// ---------------------------------------------------------------------------

void ApaDose::setScheduledDose(uint8_t hour, uint8_t minute,
                                unsigned long durationMs,
                                uint8_t intervalDays,
                                float   threshold) {
  _schedHour          = (hour   > 23) ? 23 : hour;
  _schedMinute        = (minute > 59) ? 59 : minute;
  _schedDurationMs    = (durationMs == 0) ? 1000UL : durationMs;
  _schedThreshold     = threshold;
  _schedIntervalDays  = (intervalDays == 0) ? 1 : intervalDays;
  _schedDaysRemaining = 0;
  _schedLastSeenDay   = 255;
}

void ApaDose::manageScheduledDose() {
  if (_schedDurationMs == 0)  return;
  if (readRTCTime == nullptr) return;

  ApaDoseTime t = readRTCTime();

  // Day-tick: advance countdown once per calendar day
  if (t.day != _schedLastSeenDay) {
    if (_schedLastSeenDay != 255 && _schedDaysRemaining > 0)
      _schedDaysRemaining--;
    _schedLastSeenDay = t.day;
  }

  // Fire only when countdown reaches zero and the clock matches the configured time
  if (_schedDaysRemaining == 0 &&
      t.hour == _schedHour && t.minute == _schedMinute) {
    float eff = isnan(_schedThreshold) ? setpoint : _schedThreshold;
    bool condMet = (eff == 0.0f) ||
                   (readSensor == nullptr) ||
                   (dosesUp() ? sensorValue < eff : sensorValue > eff);
    if (condMet && triggerManualDose(_schedDurationMs)) {
      _schedDaysRemaining = _schedIntervalDays;
    }
  }
}

bool ApaDose::isShockActive() const {
  return flags.shockActive;
}

unsigned long ApaDose::getShockRemainingSeconds() const {
  if (!flags.shockActive) return 0;
  unsigned long elapsed = millis() - shockStartTime;
  unsigned long ceiling = (unsigned long)shockMaxDurationHours * 3600000UL;
  if (elapsed >= ceiling) return 0;
  return (ceiling - elapsed) / 1000UL;
}

// ---------------------------------------------------------------------------
// Manual control
// ---------------------------------------------------------------------------

bool ApaDose::triggerManualDose(unsigned long durationMs, unsigned long restMs) {
  if (flags.dosingActive || flags.primingActive) return false;
  if (flags.alarmActive && alarm.currentAlarm != ALARM_OFA) return false;
  if (durationMs == 0) return false;
  if (maxDailyDoses > 0 && dailyDoseCount >= maxDailyDoses) return false;
  if (filterPumpRunning != nullptr && !filterPumpRunning()) return false;
  if (externalStop      != nullptr && externalStop())       return false;
  if (externalStopClearedAt != 0)                           return false;
  if (lastAnyDoseEnd != 0 && millis() - lastAnyDoseEnd < INTER_PUMP_LOCKOUT_MS) return false;
  if (_linkedPhPump != nullptr && _linkedPhPump->flags.dosingActive) return false;  // same live-peer check as shouldStartDosing()
  if (_linkedPeer    != nullptr && _linkedPeer->flags.dosingActive)  return false;

  if (durationMs > MAX_MANUAL_DOSE_MS) {
    durationMs = MAX_MANUAL_DOSE_MS;
    sendStatus(onStatusMessage, F("Dose capped:5min"));
  }

  flags.manualDoseActive = true;
  currentPulse           = {pumpMaxPWM, durationMs, restMs};
  flags.dosingActive     = true;
  dosingStartTime        = millis();
  dailyDoseCount++;

  analogWrite(pumpPin, pumpMaxPWM);
  sendStatus(onStatusMessage, F("Manual dose start"));
  return true;
}

bool ApaDose::triggerPrime(unsigned long durationMs, uint8_t pwm) {
  if (flags.dosingActive || flags.primingActive) return false;
  if (durationMs == 0) return false;
  if (flags.alarmActive && alarm.currentAlarm == ALARM_TANK_EMPTY) return false;

  uint8_t primePWM    = (pwm == 0) ? pumpMaxPWM
                                   : (uint8_t)constrain(pwm, pumpMinPWM, pumpMaxPWM);
  flags.primingActive = true;
  primingStartTime    = millis();
  primingDuration     = durationMs;

  analogWrite(pumpPin, primePWM);
#ifdef APA_DOSE_DEBUG
  if (onStatusMessage) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Prime P:%d", primePWM);
    onStatusMessage(msg);
  }
#endif
  return true;
}

// ---------------------------------------------------------------------------
// Status queries
// ---------------------------------------------------------------------------

float            ApaDose::getProbeValue()              const { return sensorValue; }
float            ApaDose::getCurrentSetpoint()         const { return setpoint; }
float            ApaDose::getCurrentProportionalBand() const { return proportionalBand; }
ApaDoseType      ApaDose::getCurrentDosingType()       const { return dosingType; }
ApaDoseDirection ApaDose::getPhDirection()             const { return phDirection; }
ApaDoseAlarm ApaDose::getCurrentAlarm()              const { return alarm.currentAlarm; }
bool         ApaDose::isAlarmActive()               const { return flags.alarmActive; }
bool         ApaDose::alarmNeedsAcknowledgment()    const { return flags.alarmNeedsAck; }
bool         ApaDose::isDosingActive()             const { return flags.dosingActive; }
bool         ApaDose::isPrimingActive()            const { return flags.primingActive; }
bool         ApaDose::isInStartupBlackout()        const { return startupBlackoutMinutes > 0 && (millis() - startupTime) < (unsigned long)startupBlackoutMinutes * 60000UL; }
bool         ApaDose::isExternalStopActive()            const { return flags.externalStopSent; }
bool         ApaDose::isInExternalStopResumeDelay()     const { return externalStopClearedAt != 0 && (millis() - externalStopClearedAt) < EXTERNAL_STOP_RESUME_MS; }
bool         ApaDose::isOutsideDosingWindow()           const { return flags.outsideDosingWindow; }
bool         ApaDose::isConfigurationValid()            const { return flags.configurationValid; }
uint8_t      ApaDose::getDailyDoseCount()          const { return dailyDoseCount; }
uint8_t      ApaDose::getMaxDailyDoses()           const { return maxDailyDoses; }
unsigned long ApaDose::getLastDosingTime()         const { return lastDosingEnd; }
unsigned long ApaDose::getLastDosingEnd()          const { return lastDosingEnd; }
unsigned long ApaDose::getSecondsSinceLastDose()   const { return lastDosingEnd == 0 ? 0 : (millis() - lastDosingEnd) / 1000UL; }

void ApaDose::setPhPump(ApaDose* phPump) {
  _linkedPhPump = phPump;
  if (phPump != nullptr) phPump->_linkedPeer = this;   // reverse link -- see _linkedPeer's own comment
}
void ApaDose::setCrossSettleMinutes(uint8_t minutes) { _crossSettleMinutes = minutes; }

void ApaDose::setEfficiencyThreshold(uint8_t pct) {
  _efficiencyThresholdPct = pct;
}
uint8_t ApaDose::getEfficiencyThreshold() const { return _efficiencyThresholdPct; }

void ApaDose::setOFALimit(uint8_t referenceMinutes) {
  _ofaLimitMin         = referenceMinutes;
  flags.ofaWarningSent = false;
}

uint8_t ApaDose::getOFAPct() const {
  if (_ofaLimitMin == 0) return 0;
  uint32_t limitSec = (s_poolVolume == 0)
    ? (uint32_t)_ofaLimitMin * 60U
    : (uint32_t)_ofaLimitMin * 60U * s_poolVolume / REFERENCE_VOLUME_M3;
  if (limitSec == 0) return 0;
  return (uint8_t)min(100UL, (uint32_t)_dailyPumpRunSec * 100UL / limitSec);
}

void ApaDose::accumulateAndCheckOFA(unsigned long durationMs) {
  if (_ofaLimitMin == 0) return;

  uint16_t addSec      = (uint16_t)min(durationMs / 1000UL, (unsigned long)65535U);
  uint32_t newTotal    = (uint32_t)_dailyPumpRunSec + addSec;
  _dailyPumpRunSec     = (newTotal > 65535U) ? 65535U : (uint16_t)newTotal;

  uint32_t limitSec = (s_poolVolume == 0)
    ? (uint32_t)_ofaLimitMin * 60U
    : (uint32_t)_ofaLimitMin * 60U * s_poolVolume / REFERENCE_VOLUME_M3;
  if (limitSec == 0) return;

  uint8_t pct = (uint8_t)min(100UL, (uint32_t)_dailyPumpRunSec * 100UL / limitSec);
  if (pct >= OFA_STOP_PCT) {
    char buf[20];
    FSTR_TO_BUF(buf, F("OFA:limit reached"), 19);
    buf[19] = '\0';
    triggerAlarm(ALARM_OFA, buf);
  } else if (pct >= OFA_WARNING_PCT && !flags.ofaWarningSent) {
    sendStatus(onStatusMessage, F("OFA:70% warning"));
    flags.ofaWarningSent = true;
  }
}

void ApaDose::accumulateAndCheckDOFA(unsigned long durationMs) {
  if (flags.dofaDisabled) return;

  uint32_t total   = (uint32_t)_dofaDailyRunSec + durationMs / 1000UL;
  _dofaDailyRunSec = (total > 65535U) ? 65535U : (uint16_t)total;

  if (_dofaLearnedSec < DOFA_MIN_BASELINE_SEC) return;

  uint32_t stopThresh = (uint32_t)_dofaLearnedSec * DOFA_STOP_FACTOR / 100;
  uint32_t warnThresh = (uint32_t)_dofaLearnedSec * DOFA_WARN_FACTOR / 100;

  if ((uint32_t)_dofaDailyRunSec > stopThresh) {
    char buf[20];
    FSTR_TO_BUF(buf, F("dOFA:limit"), 19);
    buf[19] = '\0';
    triggerAlarm(ALARM_OFA, buf);
  } else if ((uint32_t)_dofaDailyRunSec > warnThresh && !flags.dofaWarningSent) {
    sendStatus(onStatusMessage, F("dOFA:150% warning"));
    flags.dofaWarningSent = true;
  }
}

void ApaDose::setDOFAAdaptDays(uint8_t days) {
  _dofaAdaptDays = constrain(days, 3, 14);
}

void ApaDose::disableDOFA() {
  flags.dofaDisabled = true;
}

uint8_t ApaDose::getDOFAPct() const {
  if (flags.dofaDisabled || _dofaLearnedSec == 0) return 0;
  return (uint8_t)min(100UL, (uint32_t)_dofaDailyRunSec * 100UL / _dofaLearnedSec);
}

bool ApaDose::isDOFALearning() const {
  return !flags.dofaDisabled && _dofaLearnedSec == 0;
}

void ApaDose::resetDOFA() {
  _dofaLearnedSec       = 0;
  _dofaDailyRunSec      = 0;
  flags.dofaDisabled    = false;
  flags.dofaWarningSent = false;
  saveConfiguration();
}

unsigned long ApaDose::getSecondsUntilNextDose() const {
  if (lastDosingEnd == 0) return 0;
  unsigned long elapsed = millis() - lastDosingEnd;
  if (elapsed >= currentPulse.restPeriod) return 0;
  return (currentPulse.restPeriod - elapsed) / 1000UL;
}
uint8_t      ApaDose::getFailedAttempts()          const { return feedback.failedAttempts; }
const char*  ApaDose::getAlarmMessage()            const { return alarm.alarmMessage; }

float ApaDose::getDailyVolumeMl()        const { return dailyVolumeMl; }
float ApaDose::getLastDoseVolumeMl()     const { return lastDoseVolumeMl; }

uint8_t ApaDose::getTankRemainingPct() const {
  if (_tankCapacityL == 0) return 255;
  uint16_t capacityMl = (uint16_t)_tankCapacityL * 1000U;
  if (_tankConsumedMl >= capacityMl) return 0;
  return (uint8_t)(100U - ((uint32_t)_tankConsumedMl * 100U / capacityMl));
}

uint8_t ApaDose::getTankDaysUntilEmpty() const {
  if (_tankCapacityL == 0 || _dailyAvgDL == 0) return 255;
  uint16_t capacityMl  = (uint16_t)_tankCapacityL * 1000U;
  uint16_t remainingMl = (_tankConsumedMl < capacityMl) ? (capacityMl - _tankConsumedMl) : 0U;
  if (remainingMl == 0) return 0;
  uint16_t days = remainingMl / ((uint16_t)_dailyAvgDL * 100U);
  return (days > 254U) ? 254U : (uint8_t)days;  // 255 reserved for "no data"
}

bool  ApaDose::hasDoseHistory()          const { return flags.lastDoseDataValid; }
float ApaDose::getLastDoseSensorBefore() const { return lastDoseSensorBefore; }
float ApaDose::getLastDoseSensorAfter()  const { return lastDoseSensorAfter; }

uint8_t ApaDose::getDoseEffectiveness() const { return _lastEfficiencyPct; }

float ApaDose::getAdaptedPB()        const { return (nudgePct > 0 && adaptedPB > 0.0f) ? adaptedPB : proportionalBand; }
bool  ApaDose::isAdaptivePBEnabled() const { return nudgePct > 0; }

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void ApaDose::getSystemStatus(char* buffer, size_t bufferSize) const {
  const char* t = (dosingType == DOSE_CL)  ? "CL" :
                  (phDirection == PH_PLUS) ? "pH+" : "pH-";
  snprintf(buffer, bufferSize,
    "Sensor:%.2f SP:%.2f Band:%.2f Type:%s Dosing:%s Alarm:%s",
    (double)sensorValue, (double)setpoint, (double)proportionalBand,
    t,
    flags.dosingActive ? "YES" : "NO",
    flags.alarmActive  ? getAlarmName(alarm.currentAlarm) : "NONE");
}

const char* ApaDose::getVersion() { return APA_DOSE_VERSION; }

void ApaDose::printLibraryInfo() {
  Serial.println(F("APA-Dose v" APA_DOSE_VERSION));
  Serial.println(F("APA Devices"));
}

// ---------------------------------------------------------------------------
// Pool volume + dead-band (static — shared across all instances)
// ---------------------------------------------------------------------------

void ApaDose::saveGlobalSlot() {
  EEPROM.put(APA_GLOBAL_EEPROM_ADDR,     s_poolVolume);
  EEPROM.put(APA_GLOBAL_EEPROM_ADDR + 1, s_deadbandPct);
  EEPROM.put(APA_GLOBAL_EEPROM_ADDR + 2, APA_GLOBAL_VALID_BYTE);
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
}

bool ApaDose::setPoolVolume(uint8_t m3) {
  if (m3 != 0 && (m3 < 10 || m3 > 90)) return false;
  s_poolVolume = m3;
  saveGlobalSlot();
  return true;
}
uint8_t ApaDose::getPoolVolume() { return s_poolVolume; }

bool ApaDose::setDeadbandPct(uint8_t pct) {
  if (pct > 20) return false;
  s_deadbandPct = pct;
  saveGlobalSlot();
  return true;
}
uint8_t ApaDose::getDeadbandPct() { return s_deadbandPct; }

float ApaDose::volumeScale() {
  if (s_poolVolume == 0) return 1.0f;
  return (float)s_poolVolume / (float)REFERENCE_VOLUME_M3;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

float ApaDose::getEffectiveSafetyBand() const {
  float dynamic = proportionalBand * SAFETY_BAND_MULTIPLIER;
  float hardCap = isOrpProfile() ? ORP_SAFETY_HARD_CAP : PH_SAFETY_HARD_CAP;
  return min(dynamic, hardCap);
}

// ---------------------------------------------------------------------------
// EEPROM
// ---------------------------------------------------------------------------

bool ApaDose::loadConfiguration() {
  ConfigData config;
  EEPROM.get(eepromBaseAddress, config);
  if (!validateConfiguration(config)) return false;
  dosingType       = config.dosingType;
  phDirection      = config.phDirection;
  setpoint         = config.setpoint;
  proportionalBand = config.proportionalBand;
  nudgePct         = config.nudgePct;
  adaptedPB        = config.adaptedPB;
  _dofaLearnedSec  = config.dofaLearnedSec;
  _tankCapacityL   = config.tankCapacityL;
  _tankConsumedMl  = config.tankConsumedMl;
  // Guard against a corrupt adaptedPB that passed the checksum
  if (nudgePct > 0 && (adaptedPB <= 0.0f || !isfinite(adaptedPB)))
    adaptedPB = proportionalBand;
  return true;
}

void ApaDose::saveConfiguration() {
  ConfigData config;
  config.magicNumber      = APA_DOSE_MAGIC_NUMBER;
  config.version          = APA_DOSE_CONFIG_VERSION;
  config.setpoint         = setpoint;
  config.proportionalBand = proportionalBand;
  config.dosingType       = dosingType;
  config.phDirection      = phDirection;
  config.nudgePct         = nudgePct;
  config.adaptedPB        = adaptedPB;
  config.dofaLearnedSec   = _dofaLearnedSec;
  config.tankCapacityL    = _tankCapacityL;
  config.tankConsumedMl   = _tankConsumedMl;
  config.checksum         = calculateChecksum(config);

  EEPROM.put(eepromBaseAddress, config);
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
}

bool ApaDose::validateConfiguration(const ConfigData& config) {
  if (config.magicNumber != APA_DOSE_MAGIC_NUMBER ||
      config.version     != APA_DOSE_CONFIG_VERSION) return false;
  if (config.checksum    != calculateChecksum(config)) return false;
  if (config.dosingType  != DOSE_PH &&
      config.dosingType  != DOSE_CL) return false;
  if (config.phDirection != PH_PLUS &&
      config.phDirection != PH_MINUS) return false;
  if (config.nudgePct > 25) return false;

  float spMin, spMax, bandMin, bandMax;
  if (config.dosingType == DOSE_CL) {
    spMin = ORP_SETPOINT_MIN; spMax = ORP_SETPOINT_MAX;
    bandMin = ORP_BAND_MIN;   bandMax = ORP_BAND_MAX;
  } else {
    spMin = PH_SETPOINT_MIN;  spMax = PH_SETPOINT_MAX;
    bandMin = PH_BAND_MIN;    bandMax = PH_BAND_MAX;
  }

  if (config.setpoint         < spMin   || config.setpoint         > spMax)   return false;
  if (config.proportionalBand < bandMin || config.proportionalBand > bandMax)  return false;
  return true;
}

uint16_t ApaDose::calculateChecksum(const ConfigData& config) {
  uint16_t       sum  = 0;
  const uint8_t* data = (const uint8_t*)&config;
  for (size_t i = 0; i < sizeof(ConfigData) - sizeof(config.checksum); i++)
    sum += data[i];
  return sum;
}

void ApaDose::resetToDefaults() {
  bool orp         = isOrpProfile();
  setpoint         = orp ? ORP_SETPOINT_DEFAULT : PH_SETPOINT_DEFAULT;
  proportionalBand = orp ? ORP_BAND_DEFAULT     : PH_BAND_DEFAULT;
  phDirection      = PH_PLUS;
  nudgePct         = 0;
  adaptedPB        = 0.0f;
  // EMA learned state cleared — threshold is an integrator value set in setup(), not reset here
  _efficiencyEma     = 0.0f;
  _efficiencyCount   = 0;
  _lastEfficiencyPct = 100;
  // dOFA — reset learned baseline; _dofaAdaptDays intentionally kept (set in setup())
  _dofaLearnedSec       = 0;
  _dofaDailyRunSec      = 0;
  flags.dofaDisabled    = false;
  flags.dofaWarningSent = false;
  // Over-setpoint protection
  _overSetpointSince      = 0;
  flags.overSetpointFired = false;
  // Tank level estimation — disabled by default; user must call setTankCapacity() to activate
  _tankCapacityL  = 0;
  _tankConsumedMl = 0;
  _dailyAvgDL     = 0;
}
