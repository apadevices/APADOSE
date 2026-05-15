/*
 * APA-Dose Library - Implementation
 *
 * Version: 3.3.0
 * Author: kecup@vazac.eu (APA Devices)
 * Date: May 2026
 */

#include "APADOSE.h"

unsigned long ApaDose::lastAnyDoseEnd = 0;

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

ApaDose::ApaDose(uint8_t pumpPin, int eepromAddress)
  : pumpPin(pumpPin),
    pumpMinPWM(50), pumpMaxPWM(255),
    setpoint(PH_SETPOINT_DEFAULT), proportionalBand(PH_BAND_DEFAULT),
    dosingType(DOSE_PH_PLUS),
    flags{},
    dosingStartTime(0), lastDosingEnd(0),
    filterOffStart(0),
    sensorValue(PH_SETPOINT_DEFAULT),
    onAlarmTriggered(nullptr), onAlarmCleared(nullptr), onStatusMessage(nullptr),
    readSensor(nullptr), filterPumpRunning(nullptr), readRTCTime(nullptr),
    startupBlackoutMs(0), startupTime(0),
    dosingWindowStart(0), dosingWindowEnd(0),
    lastKnownDay(255), dailyDoseCount(0), maxDailyDoses(0),
    lastSensorRead(0), lastGoodSensorTime(0),
    primingStartTime(0), primingDuration(0),
    eepromBaseAddress(eepromAddress),
    lastDoseSensorBefore(0.0f), lastDoseSensorAfter(0.0f),
    pumpFlowRateMlPerMin(450.0f), dailyVolumeMl(0.0f), lastDoseVolumeMl(0.0f)
{
  currentPulse = {0, 0, 0};
  memset(&feedback, 0, sizeof(feedback));
  memset(&alarm,    0, sizeof(alarm));
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool ApaDose::begin(SensorReadCallback sensorReader, FilterCallback filter,
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

  blackoutMinutes   = constrain(blackoutMinutes, 0, 60);
  startupBlackoutMs = (unsigned long)blackoutMinutes * 60UL * 1000UL;
  if (startupBlackoutMs > 0) {
    startupTime               = millis();
    flags.blackoutMessageSent = false;
  }

  flags.configurationValid = loadConfiguration();
  if (!flags.configurationValid) {
    resetToDefaults();
    saveConfiguration();
    flags.configurationValid = true;
  }

  // Prime sensorValue to setpoint so checkSafetyConditions() sees zero error on the
  // very first update() call before any real reading has been received.
  sensorValue          = setpoint;
  lastGoodSensorTime   = millis();

  if (readSensor != nullptr) {
    float v = readSensor();
    if (isfinite(v)) {
      sensorValue        = v;
      lastGoodSensorTime = millis();
    }
  } else {
    sendStatus(onStatusMessage, F("No sensor-manual"));
  }

  return true;
}

bool ApaDose::begin(SensorReadCallback sensorReader,
                    uint8_t blackoutMinutes, uint8_t maxDailyDosesLimit) {
  return begin(sensorReader, nullptr, blackoutMinutes, maxDailyDosesLimit);
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
    return;
  }
  dosingWindowStart         = startHour;
  dosingWindowEnd           = endHour;
  flags.dosingWindowEnabled = true;
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

// ---------------------------------------------------------------------------
// Main update loop
// ---------------------------------------------------------------------------

void ApaDose::update() {
  if (flags.primingActive) {
    if (millis() - primingStartTime >= primingDuration) {
      analogWrite(pumpPin, 0);
      flags.primingActive     = false;
      lastDosingEnd           = millis();
      if (currentPulse.restPeriod == 0)
        currentPulse.restPeriod = 5UL * 60UL * 1000UL;
      sendStatus(onStatusMessage, F("Prime done"));
    }
    return;
  }

  if (readSensor != nullptr && millis() - lastSensorRead >= 10000UL) {
    readSensors();
    lastSensorRead = millis();
  }

  manageFeedbackSampling();
  manageProportionalDosing();
}

void ApaDose::readSensors() {
  if (readSensor == nullptr) return;
  float raw = readSensor();
  if (!isfinite(raw)) {
    if (!flags.sensorValueBad) {
      flags.sensorValueBad = true;
      sendStatus(onStatusMessage, F("Sensor:bad value"));
    }
    return;
  }
  flags.sensorValueBad    = false;
  flags.sensorStaleWarned = false;
  sensorValue             = raw;
  lastGoodSensorTime      = millis();
}

// ---------------------------------------------------------------------------
// Proportional dosing state machine
// ---------------------------------------------------------------------------

void ApaDose::manageProportionalDosing() {
  unsigned long now = millis();

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

  if (startupBlackoutMs > 0) {
    if ((now - startupTime) < startupBlackoutMs) {
      if (!flags.blackoutMessageSent) {
        sendStatus(onStatusMessage, F("Blackout active"));
        flags.blackoutMessageSent = true;
      }
      return;
    }
    startupBlackoutMs = 0;
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
        feedback.phase             = FB_WAITING;
        feedback.feedbackCheckTime = now + currentPulse.restPeriod;
      }
    }
    return;
  }

  if (feedback.phase == FB_WAITING && now >= feedback.feedbackCheckTime) {
    startAfterDosingMeasurements();
  }

  if (lastDosingEnd > 0 && now - lastDosingEnd < currentPulse.restPeriod) return;

  if (filterPumpRunning != nullptr && !filterPumpRunning()) return;

  if (readSensor == nullptr) return;

  if (readRTCTime != nullptr) {
    ApaDoseTime t = readRTCTime();
    if (t.day != lastKnownDay) {
      dailyDoseCount = 0;
      dailyVolumeMl  = 0.0f;
      lastKnownDay   = t.day;
    }
    if (flags.dosingWindowEnabled &&
        (t.hour < dosingWindowStart || t.hour >= dosingWindowEnd)) return;
  }

  if (readSensor != nullptr &&
      lastGoodSensorTime > 0 &&
      (millis() - lastGoodSensorTime) >= SENSOR_STALE_MS) {
    if (!flags.sensorStaleWarned) {
      sendStatus(onStatusMessage, F("Sensor:stale>30min"));
      flags.sensorStaleWarned = true;
    }
    return;
  }

  checkSafetyConditions();
  if (flags.alarmActive) return;

  if (maxDailyDoses > 0 && dailyDoseCount >= maxDailyDoses) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Limit:%d/%d doses", dailyDoseCount, maxDailyDoses);
    triggerAlarm(ALARM_DAILY_LIMIT, msg);
    return;
  }

  if (shouldStartDosing() && feedback.phase == FB_IDLE) {
    startBeforeDosingMeasurements();
  }
}

// ---------------------------------------------------------------------------
// Feedback sampling (non-blocking)
// ---------------------------------------------------------------------------

void ApaDose::manageFeedbackSampling() {
  unsigned long now = millis();

  if (feedback.phase == FB_MEASURING_BEFORE && now >= feedback.nextSampleTime) {
    if (readSensor != nullptr) {
      float reading = readSensor();
      if (!isfinite(reading)) {
        feedback.nextSampleTime = now + SAMPLE_INTERVAL;
      } else {
        feedback.sampleSum += reading;
        feedback.sampleCount++;

#ifdef APA_DOSE_DEBUG
        if (onStatusMessage) {
          char msg[20];
          snprintf(msg, sizeof(msg), "B%d/%d:%.2f",
                   feedback.sampleCount, feedback.targetSamples, (double)reading);
          onStatusMessage(msg);
        }
#endif

        if (feedback.sampleCount >= feedback.targetSamples) {
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
          DosingPulse pulse = calculateProportionalPulse();
          if (pulse.pwmIntensity > 0) startDosingPulse(pulse);
        } else {
          feedback.nextSampleTime = now + SAMPLE_INTERVAL;
        }
      }
    }
  }

  if (feedback.phase == FB_MEASURING_AFTER && now >= feedback.nextSampleTime) {
    if (readSensor != nullptr) {
      float reading = readSensor();
      if (!isfinite(reading)) {
        feedback.nextSampleTime = now + SAMPLE_INTERVAL;
      } else {
        feedback.sampleSum += reading;
        feedback.sampleCount++;

#ifdef APA_DOSE_DEBUG
        if (onStatusMessage) {
          char msg[20];
          snprintf(msg, sizeof(msg), "A%d/%d:%.2f",
                   feedback.sampleCount, feedback.targetSamples, (double)reading);
          onStatusMessage(msg);
        }
#endif

        if (feedback.sampleCount >= feedback.targetSamples) {
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
        } else {
          feedback.nextSampleTime = now + SAMPLE_INTERVAL;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Dosing logic helpers
// ---------------------------------------------------------------------------

bool ApaDose::shouldStartDosing() {
  if (lastAnyDoseEnd != 0 && millis() - lastAnyDoseEnd < INTER_PUMP_LOCKOUT_MS) return false;
  float threshold = isOrpProfile() ? ORP_FEEDBACK_THRESHOLD : PH_FEEDBACK_THRESHOLD;
  if (dosingType == DOSE_PH_PLUS || dosingType == DOSE_CL)
    return sensorValue < (setpoint - threshold);
  return sensorValue > (setpoint + threshold);
}

DosingPulse ApaDose::calculateProportionalPulse() {
  DosingPulse pulse = {0, 0, 0};

  float sensorError  = abs(setpoint - sensorValue);
  float errorPercent = constrain((sensorError / proportionalBand) * 100.0f, 0.0f, 100.0f);

  float effectiveRange = pumpMaxPWM - pumpMinPWM;
  float minDosePWM     = pumpMinPWM + 0.10f * effectiveRange;
  float rawPWM         = pumpMinPWM + (errorPercent / 100.0f) * effectiveRange;
  pulse.pwmIntensity   = (uint8_t)constrain(max(rawPWM, minDosePWM), 0.0f, 255.0f);

  if      (errorPercent <= 25.0f) { pulse.pulseDuration = (unsigned long)(2000.0f + (errorPercent        / 25.0f) * 2000.0f); pulse.restPeriod =  5UL * 60UL * 1000UL; }
  else if (errorPercent <= 50.0f) { pulse.pulseDuration = (unsigned long)(4000.0f + ((errorPercent - 25.0f) / 25.0f) * 3000.0f); pulse.restPeriod = 10UL * 60UL * 1000UL; }
  else if (errorPercent <= 75.0f) { pulse.pulseDuration = (unsigned long)(7000.0f + ((errorPercent - 50.0f) / 25.0f) * 3000.0f); pulse.restPeriod = 15UL * 60UL * 1000UL; }
  else                            { pulse.pulseDuration = 11000UL;                                                                  pulse.restPeriod = 20UL * 60UL * 1000UL; }

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
    const char* dir = (dosingType == DOSE_PH_PLUS) ? "pH+" :
                      (dosingType == DOSE_PH_MINUS) ? "pH-" : "CL";
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

  analogWrite(pumpPin, 0);
  flags.dosingActive = false;
  lastDosingEnd      = now;
  lastAnyDoseEnd     = now;
}

DosingPulse ApaDose::applyFeedbackCorrections(DosingPulse p) {
  if (feedback.failedAttempts == 1) {
    p.pwmIntensity = (uint8_t)min((int)pumpMaxPWM, (int)(p.pwmIntensity * 1.3f));
  } else if (feedback.failedAttempts >= 2) {
    p.pwmIntensity  = (uint8_t)min((int)pumpMaxPWM, (int)(p.pwmIntensity * 1.5f));
    p.pulseDuration = min(15000UL, p.pulseDuration * 2);
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
  float change    = feedback.valueAfterDose - feedback.valueBeforeDose;
  float threshold = isOrpProfile() ? ORP_FEEDBACK_THRESHOLD : PH_FEEDBACK_THRESHOLD;

  bool wrongDirection = false;
  if (dosingType == DOSE_PH_PLUS || dosingType == DOSE_CL) {
    if (change < -threshold) {
      feedback.wrongDirectionCount++;
      if (feedback.wrongDirectionCount >= 3) {
        wrongDirection = true;
        char buf[20];
        FSTR_TO_BUF(buf, (dosingType == DOSE_CL ? F("Wrong chem-ORP?")
                                                : F("Wrong chem-pH+?")), 19);
        buf[19] = '\0';
        triggerAlarm(ALARM_WRONG_DIRECTION, buf);
      }
    } else { feedback.wrongDirectionCount = 0; }
  } else {
    if (change > threshold) {
      feedback.wrongDirectionCount++;
      if (feedback.wrongDirectionCount >= 3) {
        wrongDirection = true;
        char buf[20];
        FSTR_TO_BUF(buf, F("Wrong chem-pH-?"), 19);
        buf[19] = '\0';
        triggerAlarm(ALARM_WRONG_DIRECTION, buf);
      }
    } else { feedback.wrongDirectionCount = 0; }
  }

  if (wrongDirection) return;

  bool effective = (dosingType == DOSE_PH_PLUS || dosingType == DOSE_CL)
                   ? (change >  threshold)
                   : (change < -threshold);

  if (onStatusMessage) {
    char msg[20];
    snprintf(msg, sizeof(msg), "Fb:%.2f->%.2f %s",
             (double)feedback.valueBeforeDose, (double)feedback.valueAfterDose,
             effective ? "OK" : "BAD");
    onStatusMessage(msg);
  }

  if (effective) {
    feedback.failedAttempts = 0;
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
  float safetyBand  = getEffectiveSafetyBand();
  float sensorError = abs(setpoint - sensorValue);

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
                         type == ALARM_DAILY_LIMIT);

  feedback.failedAttempts = 0;
  feedback.phase          = FB_IDLE;

  if (onAlarmTriggered) onAlarmTriggered(type, message);
}

void ApaDose::checkAlarmClearConditions() {
  if (flags.alarmNeedsAck) return;

  bool canClear = false;
  switch (alarm.currentAlarm) {
    case ALARM_SAFETY_BAND:
      canClear = (abs(setpoint - sensorValue) < getEffectiveSafetyBand());
      break;
    default:
      canClear = true;
      break;
  }

  if (canClear) clearAlarm();
}

void ApaDose::clearAlarm() {
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

void ApaDose::acknowledgeAlarm() {
  if (flags.alarmActive) {
    flags.alarmNeedsAck = false;
    checkAlarmClearConditions();
  }
}

void ApaDose::forceConfigurationSave() {
  saveConfiguration();
}

// ---------------------------------------------------------------------------
// Manual control
// ---------------------------------------------------------------------------

bool ApaDose::triggerManualDose(unsigned long durationMs, unsigned long restMs) {
  if (flags.dosingActive || flags.primingActive || flags.alarmActive) return false;
  if (durationMs == 0) return false;
  if (maxDailyDoses > 0 && dailyDoseCount >= maxDailyDoses) return false;
  if (filterPumpRunning != nullptr && !filterPumpRunning()) return false;
  if (lastAnyDoseEnd != 0 && millis() - lastAnyDoseEnd < INTER_PUMP_LOCKOUT_MS) return false;

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

float        ApaDose::getProbeValue()              const { return sensorValue; }
float        ApaDose::getCurrentSetpoint()         const { return setpoint; }
float        ApaDose::getCurrentProportionalBand() const { return proportionalBand; }
ApaDoseType  ApaDose::getCurrentDosingType()       const { return dosingType; }
ApaDoseAlarm ApaDose::getCurrentAlarm()            const { return alarm.currentAlarm; }
bool         ApaDose::isAlarmActive()              const { return flags.alarmActive; }
bool         ApaDose::isDosingActive()             const { return flags.dosingActive; }
bool         ApaDose::isPrimingActive()            const { return flags.primingActive; }
bool         ApaDose::isInStartupBlackout()        const { return startupBlackoutMs > 0 && (millis() - startupTime) < startupBlackoutMs; }
bool         ApaDose::isConfigurationValid()       const { return flags.configurationValid; }
uint8_t      ApaDose::getDailyDoseCount()          const { return dailyDoseCount; }
uint8_t      ApaDose::getMaxDailyDoses()           const { return maxDailyDoses; }
unsigned long ApaDose::getLastDosingTime()         const { return lastDosingEnd; }
uint8_t      ApaDose::getFailedAttempts()          const { return feedback.failedAttempts; }
const char*  ApaDose::getAlarmMessage()            const { return alarm.alarmMessage; }

float ApaDose::getDailyVolumeMl()        const { return dailyVolumeMl; }
float ApaDose::getLastDoseVolumeMl()     const { return lastDoseVolumeMl; }

bool  ApaDose::hasDoseHistory()          const { return flags.lastDoseDataValid; }
float ApaDose::getLastDoseSensorBefore() const { return lastDoseSensorBefore; }
float ApaDose::getLastDoseSensorAfter()  const { return lastDoseSensorAfter; }

float ApaDose::getDoseEffectiveness() const {
  if (!flags.lastDoseDataValid) return 0.0f;
  float change = lastDoseSensorAfter - lastDoseSensorBefore;
  if (dosingType == DOSE_PH_MINUS) change = -change;
  return (change / proportionalBand) * 100.0f;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void ApaDose::getSystemStatus(char* buffer, size_t bufferSize) const {
  const char* t = (dosingType == DOSE_PH_PLUS) ? "pH+" :
                  (dosingType == DOSE_PH_MINUS) ? "pH-" : "CL";
  snprintf(buffer, bufferSize,
    "Sensor:%.2f SP:%.2f Band:%.2f Type:%s Dosing:%s Alarm:%s",
    (double)sensorValue, (double)setpoint, (double)proportionalBand,
    t,
    flags.dosingActive ? "YES" : "NO",
    flags.alarmActive  ? getAlarmName(alarm.currentAlarm) : "NONE");
}

const char* ApaDose::getVersion() { return APA_DOSE_VERSION; }

void ApaDose::printLibraryInfo() {
  Serial.println(F("APA-Dose v3.3.0"));
  Serial.println(F("APA Devices"));
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
  setpoint         = config.setpoint;
  proportionalBand = config.proportionalBand;
  return true;
}

void ApaDose::saveConfiguration() {
  ConfigData config;
  config.magicNumber      = APA_DOSE_MAGIC_NUMBER;
  config.version          = APA_DOSE_CONFIG_VERSION;
  config.setpoint         = setpoint;
  config.proportionalBand = proportionalBand;
  config.dosingType       = dosingType;
  config.checksum         = calculateChecksum(config);

  const uint8_t* data = (const uint8_t*)&config;
  for (size_t i = 0; i < sizeof(ConfigData); i++)
    EEPROM.write(eepromBaseAddress + i, data[i]);
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
}

bool ApaDose::validateConfiguration(const ConfigData& config) {
  if (config.magicNumber != APA_DOSE_MAGIC_NUMBER ||
      config.version     != APA_DOSE_CONFIG_VERSION) return false;
  if (config.checksum    != calculateChecksum(config)) return false;
  if (config.dosingType  != DOSE_PH_PLUS &&
      config.dosingType  != DOSE_PH_MINUS &&
      config.dosingType  != DOSE_CL) return false;

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
}
