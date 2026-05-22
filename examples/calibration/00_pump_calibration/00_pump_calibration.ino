/*
 * APA-Dose Example — 00: Pump PWM Calibration  [UTILITY]
 *
 * Finds the minimum PWM value where your dosing pump starts spinning.
 * Peristaltic pumps have a dead band: PWM values below a threshold spin
 * the motor driver but produce no shaft rotation or flow. Dosing at these
 * values silently skips doses. setPumpRange(minPWM, 255) tells the library
 * where real flow begins so it never commands the pump below that point.
 *
 * Run this sketch once per pump before writing your main sketch.
 * Each pump has its own threshold — motor model and supply voltage both affect it.
 *
 * Hardware:
 *   PIN_PUMP   any PWM-capable pin (~)   MOSFET gate → your dosing pump
 *
 * Wiring note: the pump does not need to be connected to chemical tubing
 * during calibration — run it dry in short bursts.
 *
 * Instructions:
 *   1. Set PIN_PUMP below to match your wiring.
 *   2. Upload and open Serial Monitor at 115200 baud.
 *   3. Type a number (0–255) + Enter — pump runs for 3 s then stops.
 *   4. Start low (try 30, 40, 50...) and increase until the shaft turns.
 *   5. Type 'done' — the sketch prints your result and the exact
 *      setPumpRange() line to copy into your main sketch.
 *   6. Repeat for each pump.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <Arduino.h>

const uint8_t  PIN_PUMP = 9;      // ← change to your pump pin (must be PWM-capable, ~)
const uint32_t RUN_MS   = 3000;   // how long the pump runs per test, in milliseconds

uint8_t lastTested = 0;

// Round up to the nearest multiple of 5 — gives a clean, stable value with a small safety margin.
uint8_t roundUpToFive(uint8_t v) {
  return ((uint8_t)((v + 4) / 5)) * 5;
}

void printResult() {
  uint8_t rounded = roundUpToFive(lastTested);
  Serial.println();
  Serial.println(F("--- Result ---"));
  Serial.print(F("Minimum PWM found : "));
  Serial.println(lastTested);
  Serial.print(F("Rounded up (use this): "));
  Serial.println(rounded);
  Serial.println();
  Serial.print(F("Copy into your main sketch:  setPumpRange("));
  Serial.print(rounded);
  Serial.println(F(", 255);"));
  Serial.println();
  Serial.println(F("Run this sketch again for your next pump (change PIN_PUMP at the top)."));
}

void testPWM(uint8_t pwm) {
  Serial.print(F("Running PWM "));
  Serial.print(pwm);
  Serial.print(F(" for "));
  Serial.print(RUN_MS / 1000);
  Serial.println(F(" s..."));

  analogWrite(PIN_PUMP, pwm);
  delay(RUN_MS);
  analogWrite(PIN_PUMP, 0);

  Serial.println(F("Stopped."));
  Serial.println(F("Shaft moving? → type 'done'   Not yet? → type a higher value"));
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PUMP, OUTPUT);
  analogWrite(PIN_PUMP, 0);

  Serial.println();
  Serial.println(F("=== APA-Dose Pump PWM Calibration ==="));
  Serial.println(F("Type a PWM value (0-255) + Enter to test it."));
  Serial.println(F("Type 'done' when the pump shaft just started turning."));
  Serial.println(F("Suggestion: start at 30, then 40, 50, 60..."));
  Serial.println();
}

void loop() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();

  if (input.length() == 0) return;

  if (input.equalsIgnoreCase("done")) {
    if (lastTested == 0) {
      Serial.println(F("Test at least one value first."));
    } else {
      printResult();
    }
    return;
  }

  int value = input.toInt();
  if (value <= 0 || value > 255) {
    Serial.println(F("Enter a number between 1 and 255, or 'done'."));
    return;
  }

  lastTested = (uint8_t)value;
  testPWM(lastTested);
}
