/* ============================================================================
 *  OceanShield / Seafarer Safety Monitor  -  ESP32 firmware  (CORRECTED)
 * ----------------------------------------------------------------------------
 *  Fixes vs. the original sketch:
 *   1. RFID moved OFF Serial/UART0 (the USB port) onto Serial2 (GPIO16/17).
 *      In the original, RFID and the USB debug console fought over the same
 *      pins, which is why field4 has been stuck at 0 for months.
 *   2. Debug console raised to 115200 so ESP32 boot messages are readable.
 *   3. WiFi.begin() is now called ONCE per attempt, not every 5 s inside the
 *      retry loop (repeated calls abort the association in progress).
 *   4. Removed "analogRead()/4". The ESP32 ADC is 12-bit (0..4095); dividing
 *      by 4 threw away resolution AND floored any small reading to exactly 0.
 *   5. field2 now carries a real HEART RATE in BPM, computed from an 6 s
 *      window of ECG samples at ~500 Hz, instead of one random instantaneous
 *      ADC sample taken every 30 s (which is physically meaningless).
 *   6. AD8232 leads-off detection (LO+/LO-) is honoured -> BPM 0 when the
 *      electrodes are not on the body.
 *   7. RFID is polled continuously instead of once every 30 s.
 *   8. A boot-time SENSOR SELF TEST prints raw values so you can see at a
 *      glance whether anything is actually reaching the pins.
 *   9. ThingSpeak error codes are decoded into plain English.
 * ==========================================================================*/

#include <WiFi.h>
#include "ThingSpeak.h"

// ------------------------- WiFi -------------------------------------------
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
WiFiClient  client;

// ------------------------- ThingSpeak -------------------------------------
unsigned long myChannelNumber = 0;   // your ThingSpeak channel ID
const char*   myWriteAPIKey   = "YOUR_THINGSPEAK_WRITE_KEY";

// ------------------------- Timing -----------------------------------------
const unsigned long timerDelay = 30000;   // free ThingSpeak needs >= 15000 ms
unsigned long lastTime = 0;

// ------------------------- Pins -------------------------------------------
// NOTE: GPIO34/35 are INPUT-ONLY and have NO internal pull-up/pull-down.
//       They are ADC1 channels, which is correct - ADC2 pins (GPIO0,2,4,
//       12-15,25-27) do NOT work while WiFi is running.
const int memsPin    = 34;   // ADC1_CH6  - MEMS / vibration analog out
const int ecgPin     = 35;   // ADC1_CH7  - AD8232 OUTPUT
const int waterPin   = 32;   // ADC1_CH4  - YL-69/LM393 water probe, ANALOG out (AO)
const int ecgLOplus  = 25;   // AD8232 LO+   (leads-off detect)
const int ecgLOminus = 26;   // AD8232 LO-   (leads-off detect)

// ------------------------- RFID on UART2 ----------------------------------
HardwareSerial RFIDSerial(2);
const int   RFID_RX_PIN = 16;               // <-- EM-18 TX connects HERE
const int   RFID_TX_PIN = 17;               // unused by the EM-18
const char* ALLOWED_TAG = "000000000000";

// !!  The EM-18 runs at 5 V and its TX idles at 5 V. GPIO16 is NOT 5 V
// !!  tolerant. Put a divider on that line: EM18_TX --[1k]--+--> GPIO16
// !!                                                        |
// !!                                                      [2k]
// !!                                                        |
// !!                                                       GND

// ------------------------- State ------------------------------------------
int memsValue  = 0;      // raw 0..4095
int heartRate  = 0;      // BPM, 0 = no valid reading
int waterValue = 0;      // raw 0..4095 (dry probe low, wet probe high)
int rfidResult = 0;      // 0 = no scan, 1 = authorised, 2 = unknown tag

// ------------------------- Forward decls ----------------------------------
void  connectWiFi();
void  sensorSelfTest();
int   readHeartRate();
void  pollRFID();
const char* tsError(int code);

// ==========================================================================
void setup() {
  Serial.begin(115200);                       // debug console (NOT the RFID)
  delay(300);
  Serial.println("\n\n=== Seafarer Safety Monitor booting ===");

  RFIDSerial.begin(9600, SERIAL_8N1, RFID_RX_PIN, RFID_TX_PIN);
  RFIDSerial.setTimeout(50);                  // don't block the loop

  pinMode(waterPin,   INPUT);
  pinMode(ecgLOplus,  INPUT);
  pinMode(ecgLOminus, INPUT);

  analogReadResolution(12);                   // 0..4095
  analogSetPinAttenuation(memsPin, ADC_11db); // full 0..~3.3 V range
  analogSetPinAttenuation(ecgPin,  ADC_11db);
  analogSetPinAttenuation(waterPin, ADC_11db);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                       // stop modem sleep dropouts
  ThingSpeak.begin(client);

  sensorSelfTest();                           // <-- read this output first!
  connectWiFi();
}

// ==========================================================================
void loop() {
  pollRFID();                                 // continuous, not once per 30 s

  if (millis() - lastTime < timerDelay) return;
  lastTime = millis();

  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi this cycle - skipping upload.");
    return;
  }

  memsValue  = analogRead(memsPin);           // raw 0..4095, NO /4
  int ecgRaw = analogRead(ecgPin);            // raw sample, diagnostics only
  waterValue = analogRead(waterPin);          // analog since the Aug-2026 rewiring
  heartRate  = readHeartRate();               // blocks ~7.5 s
  pollRFID();

  Serial.println("---- Sensor Readings ----");
  Serial.printf("MEMS   (raw 0-4095) : %d\n", memsValue);
  Serial.printf("ECG    (raw 0-4095) : %d\n", ecgRaw);
  Serial.printf("Heart rate (BPM)    : %d\n", heartRate);
  Serial.printf("Water (raw 0-4095)  : %d\n", waterValue);
  Serial.printf("RFID result         : %d\n", rfidResult);
  Serial.println("--------------------------");

  if (memsValue == 0 && heartRate == 0 && waterValue == 0) {
    Serial.println(">> ALL SENSORS READ ZERO. This is a wiring/power fault,");
    Serial.println(">> not a ThingSpeak fault. See sensorSelfTest() notes.");
  }

  // ---- REMOTE DIAGNOSTICS ------------------------------------------------
  // This status string rides along with every upload and is readable from
  // anywhere on the internet, with NO USB cable attached, via:
  //   https://api.thingspeak.com/channels/<CHANNEL_ID>/status.json?api_key=<READKEY>
  // It carries the raw unscaled pin values, the AD8232 leads-off flags, WiFi
  // signal strength and uptime - everything needed to tell a wiring fault
  // apart from a code fault while the board sits on someone else's desk.
  char status[140];
  snprintf(status, sizeof(status),
           "M=%d E=%d W=%d LO=%d%d RSSI=%d up=%lus",
           memsValue, ecgRaw, waterValue,
           digitalRead(ecgLOplus), digitalRead(ecgLOminus),
           (int)WiFi.RSSI(), millis() / 1000UL);
  ThingSpeak.setStatus(status);

  ThingSpeak.setField(1, memsValue);
  ThingSpeak.setField(2, heartRate);
  ThingSpeak.setField(3, waterValue);
  ThingSpeak.setField(4, rfidResult);

  int x = ThingSpeak.writeFields(myChannelNumber, myWriteAPIKey);
  if (x == 200) {
    Serial.println("ThingSpeak update OK.");
  } else {
    Serial.printf("ThingSpeak FAILED. code=%d  (%s)\n", x, tsError(x));
  }

  rfidResult = 0;         // clear so an old scan doesn't latch forever
}

// ==========================================================================
void sensorSelfTest() {
  Serial.println("\n--- SENSOR SELF TEST (raw, unscaled) ---");
  for (int i = 0; i < 10; i++) {
    Serial.printf("  MEMS(GPIO%d)=%4d  ECG(GPIO%d)=%4d  WATER(GPIO%d)=%4d  "
                  "LO+=%d LO-=%d\n",
                  memsPin,  analogRead(memsPin),
                  ecgPin,   analogRead(ecgPin),
                  waterPin, analogRead(waterPin),
                  digitalRead(ecgLOplus), digitalRead(ecgLOminus));
    delay(200);
  }
  Serial.println("  Interpretation:");
  Serial.println("   * numbers move around a bit      -> pin + ADC are healthy");
  Serial.println("   * stuck at exactly 0             -> no signal reaching pin:");
  Serial.println("       - is the sensor VCC actually powered?");
  Serial.println("       - is there a COMMON GND between power board and ESP32?");
  Serial.println("       - is the signal wire seated on the right GPIO?");
  Serial.println("   * jumper 3V3 -> GPIO34 should read ~4095. If it still");
  Serial.println("     reads 0, that input pin is damaged - move to GPIO36/39.");
  Serial.println("----------------------------------------\n");
}

// ==========================================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Connecting to WiFi \"%s\" ", ssid);
  WiFi.disconnect();
  WiFi.begin(ssid, password);          // called ONCE - not inside the loop

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n  connected. IP=%s  RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("\n  FAILED (status=%d). Checks: is \"%s\" 2.4 GHz? "
                  "ESP32 cannot see 5 GHz. Captive portal? Wrong password?\n",
                  WiFi.status(), ssid);
  }
}

// ==========================================================================
// Sample the AD8232 fast, find R-peaks, return beats per minute.
int readHeartRate() {
  if (digitalRead(ecgLOplus) || digitalRead(ecgLOminus)) {
    Serial.println("  ECG: electrodes not attached (leads-off) -> BPM 0");
    return 0;
  }

  const uint32_t CALIB_MS      = 1500;
  const uint32_t MEAS_MS       = 6000;
  const uint32_t REFRACTORY_MS = 300;      // caps detection at ~200 BPM

  // Pass 1: learn the signal range so the threshold adapts to the person.
  int vMin = 4095, vMax = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < CALIB_MS) {
    int v = analogRead(ecgPin);
    if (v < vMin) vMin = v;
    if (v > vMax) vMax = v;
    delayMicroseconds(2000);               // ~500 Hz
  }

  if (vMax - vMin < 100) {
    Serial.printf("  ECG: flat signal (min=%d max=%d) -> BPM 0\n", vMin, vMax);
    return 0;
  }

  const int threshold = vMin + (int)((vMax - vMin) * 0.65f);

  // Pass 2: count threshold crossings.
  uint32_t lastBeat = 0, sumIntervals = 0;
  int  beats = 0;
  bool above = false;

  t0 = millis();
  while (millis() - t0 < MEAS_MS) {
    int v = analogRead(ecgPin);
    uint32_t now = millis();
    if (!above && v > threshold && (now - lastBeat) > REFRACTORY_MS) {
      above = true;
      if (lastBeat != 0) { sumIntervals += (now - lastBeat); beats++; }
      lastBeat = now;
    } else if (above && v < threshold) {
      above = false;
    }
    delayMicroseconds(2000);
  }

  if (beats < 2) {
    Serial.println("  ECG: fewer than 2 clean beats -> BPM 0");
    return 0;
  }

  int bpm = 60000 / (sumIntervals / beats);
  if (bpm < 30 || bpm > 220) {
    Serial.printf("  ECG: implausible BPM %d -> 0\n", bpm);
    return 0;
  }
  return bpm;
}

// ==========================================================================
void pollRFID() {
  while (RFIDSerial.available()) {
    String raw = RFIDSerial.readStringUntil('\n');
    raw.trim();

    // EM-18 variants wrap the tag in STX/ETX and add a checksum.
    // Keep only hex characters.
    String tag = "";
    for (unsigned int i = 0; i < raw.length(); i++) {
      char c = raw[i];
      if (isxdigit((int)c)) tag += (char)toupper(c);
    }
    if (tag.length() < 10) continue;

    rfidResult = tag.equals(ALLOWED_TAG) ? 1 : 2;
    Serial.printf("  RFID tag %s -> %s\n", tag.c_str(),
                  rfidResult == 1 ? "AUTHORISED" : "UNKNOWN");
  }
}

// ==========================================================================
const char* tsError(int code) {
  switch (code) {
    case 200:  return "OK";
    case 400:  return "bad API key";
    case 404:  return "bad URL / channel not found";
    case -101: return "value out of range";
    case -201: return "invalid field number";
    case -210: return "setField() never called before writeFields()";
    case -301: return "could not connect to ThingSpeak (no internet route)";
    case -302: return "unexpected failure during write";
    case -303: return "could not parse response";
    case -304: return "timed out waiting for server";
    case -401: return "point not inserted (rate limited - wait 15 s)";
    case 0:    return "no response at all - check WiFi / captive portal";
    default:   return "unknown";
  }
}
