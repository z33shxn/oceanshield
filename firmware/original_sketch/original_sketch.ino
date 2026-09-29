#include <WiFi.h>
#include "ThingSpeak.h"

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

WiFiClient client;

// ThingSpeak channel details
unsigned long myChannelNumber = 0;   // your ThingSpeak channel ID
const char* myWriteAPIKey = "YOUR_THINGSPEAK_WRITE_KEY";

// Timer variables
unsigned long lastTime = 0;
unsigned long timerDelay = 30000;

// Sensor pins
const int memsPin = 34;        // MEMS sensor
const int ecgPin = 35;         // ECG sensor
const int waterLevelPin = 32;  // Digital water level sensor

int memsValue = 0;
int ecgValue = 0;
int waterLevelValue = 0;

// RFID variables
String incoming = "";
int idMatched = 1;
int idNotMatched = 2;
int result = 0;     // GLOBAL VARIABLE (VERY IMPORTANT)

void setup() {
  Serial.begin(9600);

  pinMode(memsPin, INPUT);
  pinMode(ecgPin, INPUT);
  pinMode(waterLevelPin, INPUT);

  WiFi.mode(WIFI_STA);
  ThingSpeak.begin(client);
}

void loop() {

  if ((millis() - lastTime) > timerDelay) {

    // Connect WiFi
    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("Connecting to WiFi");
      while (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(ssid, password);
        delay(5000);
        Serial.print(".");
      }
      Serial.println("\nConnected to WiFi!");
    }

    // Read sensors
    memsValue = analogRead(memsPin) / 4;
    ecgValue = analogRead(ecgPin) / 4;
    waterLevelValue = digitalRead(waterLevelPin);

    RFIDloop();   // Check RFID

    // Print values
    Serial.println("---- Sensor Readings ----");
    Serial.print("MEMS Value: "); Serial.println(memsValue);
    Serial.print("ECG Value: "); Serial.println(ecgValue);
    Serial.print("Water Level: "); Serial.println(waterLevelValue);
    Serial.print("RFID Result: "); Serial.println(result);
    Serial.println("--------------------------");

    // Send to ThingSpeak
    ThingSpeak.setField(1, memsValue);
    ThingSpeak.setField(2, ecgValue);
    ThingSpeak.setField(3, waterLevelValue);
    ThingSpeak.setField(4, result);   // RFID result uploaded

    int x = ThingSpeak.writeFields(myChannelNumber, myWriteAPIKey);

    if (x == 200) {
      Serial.println("Channel update successful.");
    } else {
      Serial.println("Problem updating channel. HTTP error code " + String(x));
    }

    lastTime = millis();
  }
}

void RFIDloop() 
{
  if (Serial.available()) {

    incoming = Serial.readStringUntil('\n');
    incoming.trim();

    if (incoming == "000000000000") {
      result = idMatched;     // = 1
    } else {
      result = idNotMatched;  // = 2
    }

    Serial.print("RFID Incoming: ");
    Serial.println(incoming);
  }
}