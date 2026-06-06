  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <Wire.h>
  #include <Adafruit_VL53L0X.h>

  extern "C" {
    #include "user_interface.h"
  }

  // ================================================
  // CONFIGURATION
  // ================================================
  char ssid[] = "Thunderbird 242";
  char pass[] = "9947666371";0-=
  String masterIP = "192.168.1.61";
  String endpoint = "/update2";

  // Tank Dimensions (Rectangular)
  const float TANK_HEIGHT_CM = 55.0; 
  const float TANK_LENGTH_CM = 186.0;
  const float TANK_WIDTH_CM  = 62.0;

  // Timers
  const unsigned long NORMAL_SLEEP_US  = 12 * 60 * 1000000UL; // 10 minutes deep sleep
  const unsigned long FILL_INTERVAL_MS = 10000;               // 10 seconds awake loop

  // Hardware Pins
  const int BAT_PIN = A0;  // Battery voltage divider

  // Battery divider (100k + 100k)
  const float R1          = 100000.0;
  const float R2          = 100000.0;
  const float ADC_REF     = 3.3;
  const float ADC_MAX     = 1023.0;
  const float BAT_MAX     = 4.2;
  const float BAT_MIN     = 3.0;

  // Initialize the VL53L0X sensor object
  Adafruit_VL53L0X lox = Adafruit_VL53L0X();

  // ================================================
  // BATTERY
  // ================================================
  float readBatteryVoltage() {
    // Take 5 samples and average for stability
    int sum = 0;
    for (int i = 0; i < 5; i++) {
      sum += analogRead(BAT_PIN);
      delay(10);
    }
    float raw     = sum / 5.0;
    float vA0     = (raw / ADC_MAX) * ADC_REF;
    float vBat    = vA0 * ((R1 + R2) / R2);  // Scale back up
    return vBat;
  }

  float batteryPercent(float vBat) {
    float pct = ((vBat - BAT_MIN) / (BAT_MAX - BAT_MIN)) * 100.0;
    return constrain(pct, 0.0, 100.0);
  }

  // ================================================
  // RADIO CONTROL (Power Saving)
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
  // SENSOR (VL53L0X)
  // ================================================
  bool readSensor(float &distance, float &waterHeight, float &percent) {
    radioOff(); // Shut down WiFi radio to minimize voltage sag during laser burst

    float sum = 0;
    int valid = 0;
    VL53L0X_RangingMeasurementData_t measure;

    // Take 5 readings to average out any laser bounce noise
    for (int i = 0; i < 5; i++) {
      lox.rangingTest(&measure, false); 
      
      // RangeStatus != 4 means we got a valid reading back
      if (measure.RangeStatus != 4) {
        float d = measure.RangeMilliMeter / 10.0; // Convert mm to cm
        Serial.printf("  [S%d] %.1f cm\n", i + 1, d);
        
        // Basic bounds check (ignore crazy outliers)
        if (d > 0 && d <= TANK_HEIGHT_CM + 20) {
          sum += d;
          valid++;
        }
      } else {
        Serial.printf("  [S%d] Out of range\n", i + 1);
      }
      delay(50); 
    }

    radioOn(); // Bring WiFi back online

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
    // Volume of a rectangle = (L * W * H) / 1000 to get Liters
    return (TANK_LENGTH_CM * TANK_WIDTH_CM * h) / 1000.0;
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
  // HTTP
  // ================================================
  String sendToMaster(float distance, float volume, float percent,
                      float vBat, float batPct) {
    if (!connectWiFi()) return "ERROR";

    WiFiClient client;
    HTTPClient http;

    String url = "http://" + masterIP + endpoint +
                "?distance=" + String(distance, 1) +
                "&volume="   + String(volume, 1)   +
                "&percent="  + String(percent, 1)  +
                "&vbat="     + String(vBat, 2)     +
                "&batpct="   + String(batPct, 1);

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
  void goDeepSleep(unsigned long us) {
    Serial.printf("💤 Sleeping %lu min...\n", us / 60000000UL);
    Serial.flush();
    ESP.deepSleep(us);
  }

  // ================================================
  // FILL MONITORING LOOP
  // ================================================
  void fillMonitorLoop(float vBat, float batPct) {
    Serial.println("🔔 FILL MODE — monitoring every 10s");

    while (true) {
      float distance, waterHeight, percent;

      if (!readSensor(distance, waterHeight, percent)) {
        Serial.println("⚠️ Sensor failed during fill — retrying");
      } else {
        Serial.printf("  Fill: %.1fcm gap | %.1fcm water | %.1f%%\n",
                      distance, waterHeight, percent);

        if (percent >= 99.0) {
          Serial.println("✅ Tank full — exiting fill mode");
          return;
        }

        String cmd = sendToMaster(distance, calculateVolume(waterHeight),
                                  percent, vBat, batPct);
        
        if (cmd == "GO_SLEEP") {
          Serial.println("💤 Master GO_SLEEP — exiting fill mode");
          return;
        }
      }

      Serial.println("  ⏳ Next fill read in 10s...");
      delay(FILL_INTERVAL_MS);
    }
  }

  // ================================================
  // SETUP
  // ================================================
  void setup() {
    Serial.begin(115200);
    Serial.println("\n== TANK NODE 2 WAKE (VL53L0X) ==");

    // Read battery first (no radio needed, quick)
    float vBat   = readBatteryVoltage();
    float batPct = batteryPercent(vBat);
    Serial.printf("🔋 Battery: %.2fV | %.1f%%\n", vBat, batPct);

    // Critical battery — long sleep, don't even bother with sensor/WiFi
    if (vBat < 3.3) {
      Serial.println("⚠️ Critical battery! Sleeping 30min.");
      goDeepSleep(30 * 60 * 1000000UL);
      return;
    }

    // Low battery — extended sleep interval to save power
    if (vBat < 3.5) {
      Serial.println("⚠️ Low battery. Sleeping 10min.");
      goDeepSleep(10 * 60 * 1000000UL);
      return;
    }

    // Initialize I2C. Default for Wemos D1 Mini is D2 (SDA), D1 (SCL).
    Wire.begin(); 

    // Boot VL53L0X
    if (!lox.begin()) {
      Serial.println("❌ CRITICAL ERROR: Failed to boot VL53L0X!");
      Serial.println("🛑 Sleeping to prevent battery drain. Check wiring (D1=SCL, D2=SDA).");
      goDeepSleep(NORMAL_SLEEP_US);
      return;
    }

    // Read sensor
    float distance, waterHeight, percent;
    if (!readSensor(distance, waterHeight, percent)) {
      Serial.println("⚠️ Sensor failed to read valid data — retrying next wake");
      goDeepSleep(NORMAL_SLEEP_US);
      return;
    }

    Serial.printf("📏 Gap:%.1fcm | Water:%.1fcm | %.1f%% | Vol:%.1fL\n",
                  distance, waterHeight, percent, calculateVolume(waterHeight));

    // Send to master
    String cmd = sendToMaster(distance, calculateVolume(waterHeight),
                              percent, vBat, batPct);

    // Handle Fill Mode logic
    if (cmd == "STAY_AWAKE") {
      fillMonitorLoop(vBat, batPct);
    }

    // Safe to sleep
    goDeepSleep(NORMAL_SLEEP_US);
  }

  // ================================================
  // LOOP (Never runs in Deep Sleep architecture)
  // ================================================
  void loop() {}