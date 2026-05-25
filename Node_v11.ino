#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

extern "C" {
  #include "user_interface.h"
}

// ================================================
// CONFIGURATION
// ================================================
char ssid[] = "Thunderbird 242";
char pass[] = "9947666371";
String masterIP = "192.168.1.61";

const float TANK_HEIGHT_CM   = 95.0;
const float TANK_DIAMETER_CM = 80.0;
const float TANK_RADIUS_CM   = TANK_DIAMETER_CM / 2.0;

const unsigned long NORMAL_SLEEP_US  = 2 * 60 * 1000000UL;  // 2 min deep sleep
const unsigned long FILL_INTERVAL_MS = 10000;                // 10s in fill mode

String endpoint = "/update1";  // /update2 for Tank 2

const int TRIG_PIN = D2;
const int ECHO_PIN = D1;

// ================================================
// RADIO CONTROL
// ================================================
void radioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifi_fpm_set_sleep_type(MODEM_SLEEP_T);
  wifi_fpm_open();
  wifi_fpm_do_sleep(0xFFFFFFF);
  delay(300);
}

void radioOn() {
  wifi_fpm_do_wakeup();
  wifi_fpm_close();
  WiFi.mode(WIFI_STA);
  delay(100);
}

// ================================================
// SENSOR
// ================================================
float readUltrasonicRaw() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(4);

  noInterrupts();
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  interrupts();

  if (duration == 0) return -1.0;
  return (duration / 2.0) * 0.0343;
}

// Returns true if reading succeeded, fills out params
bool readSensor(float &distance, float &waterHeight, float &percent) {
  radioOff();  // Kill radio before touching sensor

  float sum = 0;
  int valid = 0;

  for (int i = 0; i < 5; i++) {
    float d = readUltrasonicRaw();
    Serial.printf("  [S%d] %.1f cm\n", i + 1, d);
    if (d > 0 && d <= TANK_HEIGHT_CM + 10) {
      sum += d;
      valid++;
    }
    delay(100);
  }

  radioOn();  // Restore radio after sensing

  if (valid == 0) return false;

  distance    = sum / valid;
  waterHeight = constrain(TANK_HEIGHT_CM - distance, 0, TANK_HEIGHT_CM);
  percent     = (waterHeight / TANK_HEIGHT_CM) * 100.0;
  return true;
}

// ================================================
// VOLUME
// ================================================
float calculateVolume(float h) {
  return (PI * TANK_RADIUS_CM * TANK_RADIUS_CM * h) / 1000.0;
}

// ================================================
// WiFi
// ================================================
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.begin(ssid, pass);
  Serial.print("WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ✓ " + WiFi.localIP().toString());
    return true;
  }

  Serial.println(" ✗ failed");
  return false;
}

// ================================================
// HTTP — returns "STAY_AWAKE", "GO_SLEEP", or "ERROR"
// ================================================
String sendToMaster(float distance, float volume, float percent) {
  if (!connectWiFi()) return "ERROR";

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + masterIP + endpoint +
               "?distance=" + String(distance, 1) +
               "&volume="   + String(volume, 1) +
               "&percent="  + String(percent, 1);

  Serial.println("→ " + url);
  http.begin(client, url);
  http.setTimeout(5000);

  int code = http.GET();
  String resp = "";
  if (code == 200) {
    resp = http.getString();
  } else {
    Serial.printf("HTTP %d\n", code);
    resp = "ERROR";
  }
  http.end();

  resp.trim();
  resp.toUpperCase();
  Serial.println("← " + resp);
  return resp;
}

// ================================================
// DEEP SLEEP
// ================================================
void goDeepSleep() {
  Serial.println("💤 Deep sleep 2 min...");
  Serial.flush();
  ESP.deepSleep(NORMAL_SLEEP_US);
  // Execution stops here — ESP reboots after sleep
}

// ================================================
// FILL MONITORING LOOP
// Runs when master says STAY_AWAKE
// Radio OFF → Sense → Radio ON → Send → 10s wait → repeat
// Exits when master says GO_SLEEP or tank hits 100%
// ================================================
void fillMonitorLoop() {
  Serial.println("🔔 FILL MODE — monitoring every 10s");

  while (true) {
    float distance, waterHeight, percent;

    // Read sensor (radio killed inside readSensor)
    if (!readSensor(distance, waterHeight, percent)) {
      Serial.println("⚠️ Sensor failed during fill — retrying next cycle");
    } else {
      Serial.printf("  Fill: %.1fcm gap | %.1fcm water | %.1f%%\n",
                    distance, waterHeight, percent);

      // Safety: auto-exit if tank full
      if (percent >= 99.0) {
        Serial.println("✅ Tank full — exiting fill mode");
        return;  // Back to normal loop → will deep sleep
      }

      // Send and check master command
      // Radio was re-enabled inside readSensor already
      String cmd = sendToMaster(distance, calculateVolume(waterHeight), percent);

      if (cmd == "GO_SLEEP") {
        Serial.println("💤 Master says GO_SLEEP — exiting fill mode");
        return;  // Back to normal loop → will deep sleep
      }
      // STAY_AWAKE or ERROR → keep monitoring
    }

    // Wait 10s before next fill measurement
    // Radio can be active here since we're just waiting
    Serial.println("  ⏳ Next fill read in 10s...");
    delay(FILL_INTERVAL_MS);
  }
}

// ================================================
// SETUP — runs on every wake from deep sleep
// ================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n== TANK NODE WAKE ==");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  float distance, waterHeight, percent;

  // Read sensor — radio killed and restored inside
  if (!readSensor(distance, waterHeight, percent)) {
    Serial.println("Sensor failed — sleeping and retrying");
    goDeepSleep();
    return;
  }

  Serial.printf("📏 Gap:%.1fcm | Water:%.1fcm | %.1f%% | Vol:%.1fL\n",
                distance, waterHeight, percent, calculateVolume(waterHeight));

  // Send to master
  String cmd = sendToMaster(distance, calculateVolume(waterHeight), percent);

  if (cmd == "STAY_AWAKE") {
    fillMonitorLoop();    // Blocks here until fill done or master says sleep
  }

  // GO_SLEEP, ERROR, or fill loop exited → deep sleep
  goDeepSleep();
}

// ================================================
// LOOP — never runs (deep sleep reboots into setup)
// ================================================
void loop() {}