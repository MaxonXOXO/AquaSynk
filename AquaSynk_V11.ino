// ================================================
//  AQUASYNK — Master Controller V11 (ESP32)
//  Ported from ESP8266 by Max / Foton Labz
//
//  V11 Changes:
//  - Ultrasonic moved to FreeRTOS task on Core 0
//    (no more pulseIn() blocking WiFi/Thinger on Core 1)
//  - Blind spot correction: sensor reads 46cm at full tank.
//    RES_BLIND_SPOT_CM accounts for this — 100% is now
//    correctly reported when distance = 46cm.
//  - Non-blocking WiFi reconnect every 60s via millis()
//    (other tasks never pause during reconnect)
// ================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ThingerESP32.h>

// ---------- THINGER.IO CREDENTIALS ----------
#define USERNAME    "Hydra_Mesh"
#define DEVICE_ID   "Hydra"
#define DEVICE_CRED "hydramesh"

// ---------- WIFI CREDENTIALS ----------
const char* ssid = "Thunderbird 242";
const char* pass = "9947666371";

// ---------- STATIC IP — UPDATE YOURSELF ----------
IPAddress local_IP(192, 168, 1, 61);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ---------- THINGER INSTANCE ----------
ThingerESP32 thing(USERNAME, DEVICE_ID, DEVICE_CRED);

// ---------- HARDWARE PINS (ESP32 GPIO) ----------
// Relays (active LOW)
#define PUMP_A   4    // GPIO4  — safe output
#define PUMP_B   22   // GPIO22 — moved from 14 (PWM boot glitch)
#define VALVE_1  32   // GPIO32 — safe output
#define VALVE_2  33   // GPIO33 — safe output

// AJ-SR04 Ultrasonic
#define TRIG_PIN 18   // GPIO18 — Safe, silent pin
#define ECHO_PIN 19   // GPIO19 — Safe, silent pin

// LEDs
#define LED_RED   16  // GPIO16
#define LED_GREEN  5  // GPIO5
#define LED_BLUE  27  // GPIO27 — moved from GPIO2 (strapping pin)

// ================================================
//  TANK DIMENSIONS
//
//  RES_TANK_HEIGHT_CM  : Physical distance from sensor
//                        face to tank bottom (empty = max cm)
//
//  RES_BLIND_SPOT_CM   : Distance read when tank is FULL.
//                        Your tank reads 46cm at full — that's
//                        the sensor's blind zone + air gap above water.
//                        Set this to the value you measured.
//
//  Effective range     : BLIND_SPOT (100%) → TANK_HEIGHT (0%)
//                        54cm of usable sensor travel.
// ================================================
const float TANK1_MAX_CM          = 100.0;
const float TANK2_MAX_CM          = 100.0;
const float RES_TANK_HEIGHT_CM    = 100.0;  // sensor-to-bottom distance
const float RES_BLIND_SPOT_CM     = 46.0;   // reading when tank is FULL
const float RES_TANK_DIAMETER_CM  = 80.0;

// ================================================
//  ULTRASONIC — SHARED STATE (FreeRTOS protected)
// ================================================
const float  SOUND_SPEED          = 0.0343f;
const unsigned long ULTRASONIC_TIMEOUT_US = 30000;  // 30ms timeout for pulseIn

// Shared between ultrasonic task (Core 0) and main loop (Core 1)
// Protected by mutex — always lock before read/write
SemaphoreHandle_t ultrasonicMutex;

volatile float  g_ultrasonicSamples[3]  = {0, 0, 0};
volatile uint8_t g_ultrasonicSampleIdx  = 0;
volatile bool   g_ultrasonicReady       = false;
volatile float  g_latestRawReading      = -1.0f;

// Timing
const unsigned long SENSOR_POLL_INTERVAL = 2000;   // median update interval (ms)
const unsigned long WIFI_RETRY_INTERVAL  = 60000;  // 1-minute reconnect attempt

const float RES_RADIUS_CM = RES_TANK_DIAMETER_CM / 2.0f;
const float RES_BASE_AREA  = PI * RES_RADIUS_CM * RES_RADIUS_CM;

// ---------- SYSTEM STATE ----------
float resDistance   = 0.0f;
float resPercent    = 0.0f;
float resVolume     = 0.0f;
float tank1Distance = 0.0f;
float tank1Percent  = 0.0f;
float tank2Distance = 0.0f;
float tank2Percent  = 0.0f;

bool fillingTank1   = false;
bool fillingTank2   = false;
bool manualMode     = false;
bool node1StayAwake = false;
bool node2StayAwake = false;
bool node1Filling   = false;
bool node2Filling   = false;

bool pumpAState  = false;
bool pumpBState  = false;
bool valve1State = false;
bool valve2State = false;

unsigned long lastSensorPoll  = 0;
unsigned long lastWiFiCheck   = 0;
bool wifiWasConnected         = false;

WebServer server(80);

// ================================================
//  FREERTOS ULTRASONIC TASK — Runs on Core 0
//
//  Samples every 100ms. pulseIn() is blocking but
//  isolated on Core 0, so WiFi/Thinger on Core 1
//  are never starved.
//
//  Writes to g_ultrasonicSamples[] under mutex.
// ================================================
void ultrasonicTask(void* pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xInterval = pdMS_TO_TICKS(100);  // 100ms period

  for (;;) {
    // Fire trigger
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    // Read echo — this blocks up to 30ms, but only on Core 0
    long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT_US);

    float reading = -1.0f;
    if (duration > 0) {
      reading = (duration * SOUND_SPEED) / 2.0f;
    }

    // Write to shared buffer under mutex
    if (xSemaphoreTake(ultrasonicMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      g_latestRawReading = reading;

      if (reading > 0.0f && reading <= 400.0f) {
        g_ultrasonicSamples[g_ultrasonicSampleIdx] = reading;
        g_ultrasonicSampleIdx = (g_ultrasonicSampleIdx + 1) % 3;
        if (g_ultrasonicSampleIdx == 0) g_ultrasonicReady = true;
      }
      xSemaphoreGive(ultrasonicMutex);
    }

    // Wait until next 100ms tick (vTaskDelayUntil keeps period accurate)
    vTaskDelayUntil(&xLastWakeTime, xInterval);
  }
}

// ================================================
//  MEDIAN SORT (3-sample, branchless swap)
// ================================================
float getMedianDistance() {
  float d[3];

  if (xSemaphoreTake(ultrasonicMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    d[0] = g_ultrasonicSamples[0];
    d[1] = g_ultrasonicSamples[1];
    d[2] = g_ultrasonicSamples[2];
    xSemaphoreGive(ultrasonicMutex);
  } else {
    return -1.0f;
  }

  if (d[0] > d[1]) { float t = d[0]; d[0] = d[1]; d[1] = t; }
  if (d[1] > d[2]) { float t = d[1]; d[1] = d[2]; d[2] = t; }
  if (d[0] > d[1]) { float t = d[0]; d[0] = d[1]; d[1] = t; }
  return d[1];
}

// ================================================
//  CALCULATIONS
//
//  Reservoir percent:
//    distance = RES_BLIND_SPOT_CM  → 100% (full, water near sensor)
//    distance = RES_TANK_HEIGHT_CM → 0%  (empty, max sensor range)
//
//  Effective span = TANK_HEIGHT - BLIND_SPOT = 54cm
// ================================================
inline float calculateResPercent(float distanceCm) {
  // Clamp to valid sensor range
  distanceCm = constrain(distanceCm, RES_BLIND_SPOT_CM, RES_TANK_HEIGHT_CM);
  float span    = RES_TANK_HEIGHT_CM - RES_BLIND_SPOT_CM;   // 54cm
  float filled  = RES_TANK_HEIGHT_CM - distanceCm;          // how far from empty
  return constrain((filled / span) * 100.0f, 0.0f, 100.0f);
}

inline float calculateResVolume(float distanceCm) {
  // Water height above bottom = tank height minus distance
  // But we correct for blind spot: actual water height uses the percent
  float percent        = calculateResPercent(distanceCm);
  float waterHeightCm  = (percent / 100.0f) * (RES_TANK_HEIGHT_CM - RES_BLIND_SPOT_CM);
  return (RES_BASE_AREA * waterHeightCm) * 0.001f;  // cm³ → litres
}

inline float calculateFillPercent(float distance, float maxDepth) {
  distance = constrain(distance, 0.0f, maxDepth);
  float percent = ((maxDepth - distance) / maxDepth) * 100.0f;
  return constrain(percent, 0.0f, 100.0f);
}

// ================================================
//  PIN STATE CACHE
// ================================================
inline void updatePinStates() {
  pumpAState  = (digitalRead(PUMP_A)  == LOW);
  pumpBState  = (digitalRead(PUMP_B)  == LOW);
  valve1State = (digitalRead(VALVE_1) == LOW);
  valve2State = (digitalRead(VALVE_2) == LOW);
}

// ================================================
//  AUTOMATION LOGIC
// ================================================
void runAutomationLogic() {
  if      (tank1Percent <= 45.0f) fillingTank1 = true;
  else if (tank1Percent >= 90.0f) fillingTank1 = false;

  if      (tank2Percent <= 45.0f) fillingTank2 = true;
  else if (tank2Percent >= 90.0f) fillingTank2 = false;

  // Reservoir too low — stop all filling
  if (resPercent <= 20.0f) {
    fillingTank1 = false;
    fillingTank2 = false;
  }

  updatePinStates();

  bool pumpNeedsToRun = fillingTank1 || fillingTank2;
  bool pumpIsRunning  = pumpAState  || pumpBState;

  static bool lastLEDState = false;
  if (pumpIsRunning != lastLEDState) {
    digitalWrite(LED_BLUE, pumpIsRunning ? HIGH : LOW);
    lastLEDState = pumpIsRunning;
  }

  if (pumpNeedsToRun && !pumpIsRunning) {
    digitalWrite(VALVE_1, fillingTank1 ? LOW : HIGH);
    digitalWrite(VALVE_2, fillingTank2 ? LOW : HIGH);
    delay(50);
    digitalWrite(PUMP_A, LOW);

  } else if (!pumpNeedsToRun && pumpIsRunning) {
    digitalWrite(PUMP_A, HIGH);
    digitalWrite(PUMP_B, HIGH);
    delay(50);
    digitalWrite(VALVE_1, HIGH);
    digitalWrite(VALVE_2, HIGH);

  } else if (pumpIsRunning) {
    if (valve1State != fillingTank1) digitalWrite(VALVE_1, fillingTank1 ? LOW : HIGH);
    if (valve2State != fillingTank2) digitalWrite(VALVE_2, fillingTank2 ? LOW : HIGH);
  }
}

// ================================================
//  HTTP HANDLERS
// ================================================
void handleUpdate1() {
  if (!server.hasArg("distance")) {
    server.send(400, "text/plain", "Missing distance");
    return;
  }

  tank1Distance = server.arg("distance").toFloat();
  tank1Percent  = server.hasArg("percent")
                    ? server.arg("percent").toFloat()
                    : calculateFillPercent(tank1Distance, TANK1_MAX_CM);

  updatePinStates();
  bool anyPumpOn     = pumpAState || pumpBState;
  bool manualFilling = manualMode && anyPumpOn && valve1State;
  bool keepAwake     = fillingTank1 || manualFilling;

  server.send(200, "text/plain", keepAwake ? "STAY_AWAKE" : "GO_SLEEP");

  if (!manualMode) runAutomationLogic();
}

void handleUpdate2() {
  if (!server.hasArg("distance")) {
    server.send(400, "text/plain", "Missing distance");
    return;
  }

  tank2Distance = server.arg("distance").toFloat();
  tank2Percent  = server.hasArg("percent")
                    ? server.arg("percent").toFloat()
                    : calculateFillPercent(tank2Distance, TANK2_MAX_CM);

  updatePinStates();
  bool anyPumpOn     = pumpAState || pumpBState;
  bool manualFilling = manualMode && anyPumpOn && valve2State;
  bool keepAwake     = fillingTank2 || manualFilling;

  server.send(200, "text/plain", keepAwake ? "STAY_AWAKE" : "GO_SLEEP");

  if (!manualMode) runAutomationLogic();
}

// ================================================
//  WIFI — NON-BLOCKING RECONNECT
//
//  Called every loop(). Checks status, attempts
//  reconnect only after WIFI_RETRY_INTERVAL (60s).
//  Never blocks — all other tasks keep running.
// ================================================
void handleWiFi(unsigned long currentMillis) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      Serial.print("[WiFi] Reconnected! IP: ");
      Serial.println(WiFi.localIP());
      wifiWasConnected = true;
    }
    digitalWrite(LED_GREEN, HIGH);
    thing.handle();
  } else {
    digitalWrite(LED_GREEN, LOW);

    if (wifiWasConnected) {
      Serial.println("[WiFi] Connection lost. Will retry every 60s.");
      wifiWasConnected = false;
      lastWiFiCheck    = currentMillis;  // start retry timer now
    }

    if (currentMillis - lastWiFiCheck >= WIFI_RETRY_INTERVAL) {
      lastWiFiCheck = currentMillis;
      Serial.println("[WiFi] Attempting reconnect...");
      WiFi.disconnect(false);  // disconnect without clearing credentials
      WiFi.begin(ssid, pass);  // non-blocking — result checked next iteration
    }
  }
}

// ================================================
//  THINGER.IO RESOURCES
// ================================================
void setupThingerResources() {

  thing["sensors"] >> [](pson& out) {
    out["res_percent"]    = resPercent;
    out["res_distance"]   = resDistance;
    out["res_volume"]     = resVolume;
    out["tank1_percent"]  = tank1Percent;
    out["tank1_distance"] = tank1Distance;
    out["tank1_filling"]  = fillingTank1;
    out["tank2_percent"]  = tank2Percent;
    out["tank2_distance"] = tank2Distance;
    out["tank2_filling"]  = fillingTank2;
  };

  thing["raw_sensor"] >> [](pson& out) {
    float raw = -1.0f;
    float s0 = 0, s1 = 0, s2 = 0;
    bool  ready = false;

    if (xSemaphoreTake(ultrasonicMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      raw   = g_latestRawReading;
      s0    = g_ultrasonicSamples[0];
      s1    = g_ultrasonicSamples[1];
      s2    = g_ultrasonicSamples[2];
      ready = g_ultrasonicReady;
      xSemaphoreGive(ultrasonicMutex);
    }

    out["raw_distance_cm"] = raw;
    out["sample_0"]        = s0;
    out["sample_1"]        = s1;
    out["sample_2"]        = s2;
    out["samples_ready"]   = ready;
    out["percent_fill"]    = resPercent;
    out["tank_height_cm"]  = RES_TANK_HEIGHT_CM;
    out["blind_spot_cm"]   = RES_BLIND_SPOT_CM;
  };

  thing["relays"] >> [](pson& out) {
    out["pump_a"]  = (digitalRead(PUMP_A)  == LOW);
    out["pump_b"]  = (digitalRead(PUMP_B)  == LOW);
    out["valve_1"] = (digitalRead(VALVE_1) == LOW);
    out["valve_2"] = (digitalRead(VALVE_2) == LOW);
  };

  thing["health"] >> [](pson& out) {
    out["ram"]              = ESP.getFreeHeap();
    out["min_free_heap"]    = ESP.getMinFreeHeap();
    out["max_alloc_heap"]   = ESP.getMaxAllocHeap();
    out["wifi"]             = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    out["last_sensor_poll"] = lastSensorPoll;
  };

  thing["mode"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = manualMode;
    } else {
      manualMode = (bool)in;
      if (!manualMode) runAutomationLogic();
      out = manualMode;
    }
  };

  thing["pump_a"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = pumpAState;
    } else if (manualMode) {
      pumpAState = (bool)in;
      digitalWrite(PUMP_A, pumpAState ? LOW : HIGH);
      digitalWrite(LED_BLUE, pumpAState ? HIGH : LOW);
      thing.stream("relays");
      out = pumpAState;
    } else {
      out = pumpAState;
    }
  };

  thing["pump_b"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = pumpBState;
    } else if (manualMode) {
      pumpBState = (bool)in;
      digitalWrite(PUMP_B, pumpBState ? LOW : HIGH);
      digitalWrite(LED_BLUE, pumpBState ? HIGH : LOW);
      thing.stream("relays");
      out = pumpBState;
    } else {
      out = pumpBState;
    }
  };

  thing["valve_1"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = valve1State;
    } else if (manualMode) {
      valve1State = (bool)in;
      digitalWrite(VALVE_1, valve1State ? LOW : HIGH);
      thing.stream("relays");
      out = valve1State;
    } else {
      out = valve1State;
    }
  };

  thing["valve_2"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = valve2State;
    } else if (manualMode) {
      valve2State = (bool)in;
      digitalWrite(VALVE_2, valve2State ? LOW : HIGH);
      thing.stream("relays");
      out = valve2State;
    } else {
      out = valve2State;
    }
  };
}

// ================================================
//  SETUP
// ================================================
void setup() {
  // 1. SECURE RELAYS FIRST — before any delays
  pinMode(PUMP_A,  OUTPUT); digitalWrite(PUMP_A,  HIGH);
  pinMode(PUMP_B,  OUTPUT); digitalWrite(PUMP_B,  HIGH);
  pinMode(VALVE_1, OUTPUT); digitalWrite(VALVE_1, HIGH);
  pinMode(VALVE_2, OUTPUT); digitalWrite(VALVE_2, HIGH);

  // 2. Serial
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n========================================");
  Serial.println("  AquaSynk V11 — ESP32 / FreeRTOS");
  Serial.println("========================================");
  Serial.printf("Tank Height   : %.0fcm\n",  RES_TANK_HEIGHT_CM);
  Serial.printf("Blind Spot    : %.0fcm\n",  RES_BLIND_SPOT_CM);
  Serial.printf("Usable Range  : %.0fcm  (%.0f%% travel)\n",
    RES_TANK_HEIGHT_CM - RES_BLIND_SPOT_CM,
    RES_TANK_HEIGHT_CM - RES_BLIND_SPOT_CM);
  Serial.println("Full  = 46cm = 100% fill");
  Serial.println("Empty = 100cm = 0% fill");
  Serial.println("========================================\n");

  // 3. Ultrasonic pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // 4. LEDs
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   LOW);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, LOW);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  LOW);

  // 5. Create mutex before starting task
  ultrasonicMutex = xSemaphoreCreateMutex();
  if (ultrasonicMutex == NULL) {
    Serial.println("[FATAL] Mutex creation failed!");
    while (true) delay(1000);
  }

  // 6. Launch ultrasonic task on Core 0
  //    Stack: 2048 words is plenty for this simple task
  //    Priority 2 — above idle, below most things
  xTaskCreatePinnedToCore(
    ultrasonicTask,     // function
    "UltrasonicTask",   // name
    2048,               // stack words
    NULL,               // parameter
    2,                  // priority
    NULL,               // handle (not needed)
    0                   // Core 0
  );
  Serial.println("[FreeRTOS] Ultrasonic task started on Core 0");

  // 7. WiFi (non-blocking connect attempt at boot)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(ssid, pass);

  // Boot-time wait — 5 seconds max, non-blocking style
  Serial.print("[WiFi] Connecting");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 5000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
    wifiWasConnected = true;
  } else {
    Serial.println("[WiFi] Not connected at boot — will retry every 60s");
    lastWiFiCheck = millis();
  }

  // 8. HTTP endpoints
  server.on("/update1", HTTP_GET, handleUpdate1);
  server.on("/update2", HTTP_GET, handleUpdate2);

  server.on("/command_node1", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd[0] == 'a') { node1StayAwake = true;  server.send(200, "text/plain", "Node1 awake"); }
      else               { node1StayAwake = false; node1Filling = false; server.send(200, "text/plain", "Node1 sleep"); }
    }
  });

  server.on("/command_node2", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd[0] == 'a') { node2StayAwake = true;  server.send(200, "text/plain", "Node2 awake"); }
      else               { node2StayAwake = false; node2Filling = false; server.send(200, "text/plain", "Node2 sleep"); }
    }
  });

  server.begin();
  setupThingerResources();
  digitalWrite(LED_RED, HIGH);

  Serial.println("[System] Ready!\n");
}

// ================================================
//  MAIN LOOP — Core 1 only
//  Ultrasonic is now fully on Core 0.
//  This loop handles: WiFi/Thinger, HTTP server,
//  median processing, and automation logic.
// ================================================
void loop() {
  unsigned long currentMillis = millis();

  // WiFi + Thinger (non-blocking reconnect built in)
  handleWiFi(currentMillis);

  // HTTP server
  server.handleClient();

  // Read shared ultrasonic state, update system every 2s
  bool ready = false;
  if (xSemaphoreTake(ultrasonicMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    ready = g_ultrasonicReady;
    xSemaphoreGive(ultrasonicMutex);
  }

  if (ready && (currentMillis - lastSensorPoll >= SENSOR_POLL_INTERVAL)) {
    lastSensorPoll = currentMillis;

    float distance = getMedianDistance();

    if (distance > 0.0f && distance <= 400.0f) {
      resDistance = distance;
      resPercent  = calculateResPercent(distance);
      resVolume   = calculateResVolume(distance);

      Serial.printf("[Sensor] dist=%.1fcm  fill=%.1f%%  vol=%.2fL\n",
        distance, resPercent, resVolume);

      if (!manualMode) runAutomationLogic();
    } else {
      Serial.println("[Sensor] Invalid median — skipping");
    }
  }

  delay(1);
}
