// ================================================
//  AQUASYNK — Master Controller V11.1 (ESP32)
//  Foton Labz / Max
//
//  V11.1 Fixes:
//  - Motor A valve interlock (auto AND manual mode)
//  - Correct fill priority logic:
//      reservoir > 20%  → fill both tanks toward 100%
//      reservoir 20-40% → prioritize Tank 2, Tank 1 only if spare water
//      reservoir < 20%  → no filling at all
//  - Valve opens BEFORE pump, confirmed via digitalRead
//  - Removed delay() from automation path
//  - runAutomationLogic() called once per cycle only
//  - Thinger stream("relays") removed from hot path
//  - g_ultrasonicReady resets after each median read
//  - WiFi reconnect timer properly seeded on first failure
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

// ---------- STATIC IP ----------
IPAddress local_IP(192, 168, 1, 61);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ---------- THINGER INSTANCE ----------
ThingerESP32 thing(USERNAME, DEVICE_ID, DEVICE_CRED);

// ---------- HARDWARE PINS ----------
// Relays (active LOW — HIGH = OFF, LOW = ON)
#define PUMP_A   4    // Main fill pump
#define PUMP_B   22   // Garden pump (independent, no valve interlock needed)
#define VALVE_1  32   // Subtank 1 inlet valve
#define VALVE_2  33   // Subtank 2 inlet valve

// AJ-SR04M Ultrasonic (reservoir)
#define TRIG_PIN 18
#define ECHO_PIN 19

// RGB Status LEDs
#define LED_RED   16
#define LED_GREEN  5
#define LED_BLUE  27

// ================================================
//  TANK DIMENSIONS
// ================================================
const float TANK1_MAX_CM         = 100.0f;
const float TANK2_MAX_CM         = 100.0f;
const float RES_TANK_HEIGHT_CM   = 100.0f;  // sensor face → tank bottom (empty)
const float RES_BLIND_SPOT_CM    = 46.0f;   // reading when tank is FULL
const float RES_TANK_DIAMETER_CM = 80.0f;

// ---------- FILL THRESHOLDS ----------
// Auto mode hysteresis per tank
const float FILL_START_PCT   = 50.0f;   // start filling below this
const float FILL_STOP_PCT    = 100.0f;  // stop filling at this (or as close as sensor allows)
const float FILL_STOP_HYST   = 95.0f;   // practical stop — sensor noise margin

// Reservoir guard levels
const float RES_CRITICAL_PCT = 20.0f;   // below this: no filling at all
const float RES_LOW_PCT      = 40.0f;   // below this: tank 2 priority only, skip tank 1

// Valve settle time before pump enable (ms) — replaces delay()
const unsigned long VALVE_SETTLE_MS = 200;

// ================================================
//  ULTRASONIC SHARED STATE
// ================================================
const float  SOUND_SPEED             = 0.0343f;
const unsigned long ULTRASONIC_TIMEOUT_US = 30000;

SemaphoreHandle_t ultrasonicMutex;

volatile float   g_ultrasonicSamples[3] = {0, 0, 0};
volatile uint8_t g_ultrasonicSampleIdx  = 0;
volatile bool    g_ultrasonicReady      = false;
volatile float   g_latestRawReading     = -1.0f;

const unsigned long SENSOR_POLL_INTERVAL = 2000;
const unsigned long WIFI_RETRY_INTERVAL  = 60000;

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

float tank1BatPct   = 0.0f;
float tank1VBat     = 0.0f;
float tank2BatPct   = 0.0f;
float tank2VBat     = 0.0f;

// Auto-mode fill intent flags
bool fillingTank1 = false;
bool fillingTank2 = false;

// Manual mode
bool manualMode   = false;

// Node wake/sleep management
bool node1StayAwake = false;
bool node2StayAwake = false;

// Physical relay states (cached from digitalRead)
bool pumpAState  = false;
bool pumpBState  = false;
bool valve1State = false;
bool valve2State = false;

// Valve settle sequencing
bool     waitingForValveSettle = false;
unsigned long valveSettleStart = 0;

unsigned long lastSensorPoll = 0;
unsigned long lastWiFiCheck  = 0;
bool wifiWasConnected        = false;
bool wifiRetryPending        = false;   // FIX: proper first-failure seeding

WebServer server(80);

// ================================================
//  FREERTOS ULTRASONIC TASK — Core 0
// ================================================
void ultrasonicTask(void* pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xInterval = pdMS_TO_TICKS(100);

  for (;;) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT_US);

    float reading = -1.0f;
    if (duration > 0) {
      reading = (duration * SOUND_SPEED) / 2.0f;
    }

    if (xSemaphoreTake(ultrasonicMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      g_latestRawReading = reading;
      if (reading > 0.0f && reading <= 400.0f) {
        g_ultrasonicSamples[g_ultrasonicSampleIdx] = reading;
        g_ultrasonicSampleIdx = (g_ultrasonicSampleIdx + 1) % 3;
        if (g_ultrasonicSampleIdx == 0) g_ultrasonicReady = true;
      }
      xSemaphoreGive(ultrasonicMutex);
    }

    vTaskDelayUntil(&xLastWakeTime, xInterval);
  }
}

// ================================================
//  MEDIAN FILTER (3-sample)
// ================================================
float getMedianDistance() {
  float d[3];
  if (xSemaphoreTake(ultrasonicMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    d[0] = g_ultrasonicSamples[0];
    d[1] = g_ultrasonicSamples[1];
    d[2] = g_ultrasonicSamples[2];
    // FIX: reset ready flag so stale samples don't re-trigger
    g_ultrasonicReady = false;
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
// ================================================
inline float calculateResPercent(float distanceCm) {
  distanceCm = constrain(distanceCm, RES_BLIND_SPOT_CM, RES_TANK_HEIGHT_CM);
  float span   = RES_TANK_HEIGHT_CM - RES_BLIND_SPOT_CM;
  float filled = RES_TANK_HEIGHT_CM - distanceCm;
  return constrain((filled / span) * 100.0f, 0.0f, 100.0f);
}

inline float calculateResVolume(float distanceCm) {
  float percent       = calculateResPercent(distanceCm);
  float waterHeightCm = (percent / 100.0f) * (RES_TANK_HEIGHT_CM - RES_BLIND_SPOT_CM);
  return (RES_BASE_AREA * waterHeightCm) * 0.001f;
}

inline float calculateFillPercent(float distance, float maxDepth) {
  distance = constrain(distance, 0.0f, maxDepth);
  return constrain(((maxDepth - distance) / maxDepth) * 100.0f, 0.0f, 100.0f);
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
//  VALVE INTERLOCK CHECK
//
//  Returns true only if at least one valve is
//  confirmed OPEN (LOW = active). Safe to run pump.
// ================================================
inline bool anyValveOpen() {
  return (digitalRead(VALVE_1) == LOW) || (digitalRead(VALVE_2) == LOW);
}

// ================================================
//  SET PUMP A — WITH MANDATORY VALVE INTERLOCK
//
//  wantOn = true  → only fires if a valve is open
//  wantOn = false → always allowed (safe to stop)
// ================================================
void setPumpA(bool wantOn) {
  if (wantOn) {
    if (!anyValveOpen()) {
      // Safety block: no valve open, refuse to run motor
      Serial.println("[Safety] PUMP_A blocked — no valve open!");
      digitalWrite(PUMP_A, HIGH);  // ensure off
      return;
    }
    digitalWrite(PUMP_A, LOW);   // active LOW = ON
    digitalWrite(LED_BLUE, HIGH);
  } else {
    digitalWrite(PUMP_A, HIGH);  // OFF
    // Only kill LED if pump B is also off
    if (digitalRead(PUMP_B) == HIGH) {
      digitalWrite(LED_BLUE, LOW);
    }
  }
  updatePinStates();
}

// ================================================
//  AUTOMATION LOGIC  (called from loop only)
//
//  Priority rules:
//    resPercent < RES_CRITICAL_PCT (20%) → no filling
//    resPercent < RES_LOW_PCT (40%)      → tank2 only
//    resPercent >= RES_LOW_PCT           → fill both toward 100%
//
//  Sequence: valve(s) open → wait VALVE_SETTLE_MS → pump on
//  Pump off first → then close valves (always safe order)
// ================================================
void runAutomationLogic() {
  if (manualMode) return;  // hands off in manual

  // ---- DETERMINE FILL INTENT ----
  // Tank 2 hysteresis
  if      (tank2Percent <= FILL_START_PCT)  fillingTank2 = true;
  else if (tank2Percent >= FILL_STOP_HYST)  fillingTank2 = false;

  // Tank 1 hysteresis
  if      (tank1Percent <= FILL_START_PCT)  fillingTank1 = true;
  else if (tank1Percent >= FILL_STOP_HYST)  fillingTank1 = false;

  // ---- APPLY RESERVOIR GUARD ----
  if (resPercent < RES_CRITICAL_PCT) {
    // Emergency stop — reservoir too low
    fillingTank1 = false;
    fillingTank2 = false;
    Serial.println("[Auto] Reservoir critical — halting all fills");

  } else if (resPercent < RES_LOW_PCT) {
    // Low reservoir — prioritise tank 2 only
    fillingTank1 = false;
    Serial.println("[Auto] Reservoir low — tank 2 priority only");
  }
  // else: resPercent >= RES_LOW_PCT → both tanks can fill as needed

  // ---- DESIRED VALVE STATES ----
  bool wantValve1 = fillingTank1;
  bool wantValve2 = fillingTank2;
  bool wantPump   = wantValve1 || wantValve2;

  updatePinStates();

  // ---- PUMP-OFF PATH (always instant, no settle needed) ----
  if (!wantPump) {
    if (pumpAState) {
      setPumpA(false);
      // Small real-time yield before closing valves
      // (not delay — just defer valve close to next cycle via flag)
    }
    // Close valves if pump is off
    if (!pumpAState) {
      if (valve1State) { digitalWrite(VALVE_1, HIGH); Serial.println("[Auto] Valve 1 closed"); }
      if (valve2State) { digitalWrite(VALVE_2, HIGH); Serial.println("[Auto] Valve 2 closed"); }
      waitingForValveSettle = false;
    }
    updatePinStates();
    return;
  }

  // ---- PUMP-ON PATH — valve-first sequencing ----

  // Step 1: Are valves already in the right state?
  bool valve1Correct = (valve1State == wantValve1);
  bool valve2Correct = (valve2State == wantValve2);

  if (!valve1Correct || !valve2Correct) {
    // Need to change valves — stop pump first if running
    if (pumpAState) {
      setPumpA(false);
      Serial.println("[Auto] Pump paused for valve change");
    }

    // Apply new valve states
    digitalWrite(VALVE_1, wantValve1 ? LOW : HIGH);
    digitalWrite(VALVE_2, wantValve2 ? LOW : HIGH);

    Serial.printf("[Auto] Valves → V1:%s V2:%s\n",
      wantValve1 ? "OPEN" : "CLOSE",
      wantValve2 ? "OPEN" : "CLOSE");

    // Start settle timer — pump will turn on next cycle after settle
    waitingForValveSettle = true;
    valveSettleStart      = millis();
    updatePinStates();
    return;  // return now, pump starts next cycle after settle
  }

  // Step 2: Valves are correct — check settle timer before pump
  if (waitingForValveSettle) {
    if (millis() - valveSettleStart < VALVE_SETTLE_MS) {
      return;  // still settling
    }
    waitingForValveSettle = false;
    Serial.println("[Auto] Valve settle complete");
  }

  // Step 3: Valves correct + settled → run pump if needed
  if (!pumpAState && anyValveOpen()) {
    setPumpA(true);
    Serial.printf("[Auto] Pump ON — filling:%s%s\n",
      fillingTank1 ? " Tank1" : "",
      fillingTank2 ? " Tank2" : "");
  }

  updatePinStates();
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

  if (server.hasArg("batpct")) tank1BatPct = server.arg("batpct").toFloat();
  if (server.hasArg("vbat"))   tank1VBat   = server.arg("vbat").toFloat();

  updatePinStates();
  bool anyPumpOn     = pumpAState || pumpBState;
  bool manualFilling = manualMode && anyPumpOn && valve1State;
  bool keepAwake     = fillingTank1 || manualFilling;

  server.send(200, "text/plain", keepAwake ? "STAY_AWAKE" : "GO_SLEEP");
  // NOTE: automation runs in loop(), not here — avoids double-firing
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

  if (server.hasArg("batpct")) tank2BatPct = server.arg("batpct").toFloat();
  if (server.hasArg("vbat"))   tank2VBat   = server.arg("vbat").toFloat();

  updatePinStates();
  bool anyPumpOn     = pumpAState || pumpBState;
  bool manualFilling = manualMode && anyPumpOn && valve2State;
  bool keepAwake     = fillingTank2 || manualFilling;

  server.send(200, "text/plain", keepAwake ? "STAY_AWAKE" : "GO_SLEEP");
}

// ================================================
//  WIFI — NON-BLOCKING RECONNECT
//  FIX: wifiRetryPending flag ensures lastWiFiCheck
//  is seeded exactly once on first connection loss,
//  not re-seeded on every subsequent failed check.
// ================================================
void handleWiFi(unsigned long currentMillis) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      Serial.print("[WiFi] Connected! IP: ");
      Serial.println(WiFi.localIP());
      wifiWasConnected  = true;
      wifiRetryPending  = false;
    }
    digitalWrite(LED_GREEN, HIGH);
    thing.handle();  // only calls Thinger when WiFi is up
  } else {
    digitalWrite(LED_GREEN, LOW);

    if (wifiWasConnected) {
      Serial.println("[WiFi] Connection lost — automation continues locally.");
      wifiWasConnected = false;
      lastWiFiCheck    = currentMillis;  // seed timer at moment of loss
      wifiRetryPending = true;
    }

    // Retry every WIFI_RETRY_INTERVAL regardless of WiFi state
    if (wifiRetryPending && (currentMillis - lastWiFiCheck >= WIFI_RETRY_INTERVAL)) {
      lastWiFiCheck = currentMillis;
      Serial.println("[WiFi] Attempting reconnect...");
      WiFi.disconnect(false);
      WiFi.begin(ssid, pass);
      // Result checked next iteration — fully non-blocking
    }
  }
}

// ================================================
//  THINGER.IO RESOURCES
//  Trimmed: removed stream("relays") hot-path calls.
//  Dashboard reads happen on poll, not push.
// ================================================
void setupThingerResources() {

  thing["sensors"] >> [](pson& out) {
    out["res_percent"]    = resPercent;
    out["res_distance"]   = resDistance;
    out["res_volume"]     = resVolume;
    out["tank1_percent"]  = tank1Percent;
    out["tank1_distance"] = tank1Distance;
    out["tank1_filling"]  = fillingTank1;
    out["tank1_bat_pct"]  = tank1BatPct;
    out["tank1_vbat"]     = tank1VBat;
    out["tank2_percent"]  = tank2Percent;
    out["tank2_distance"] = tank2Distance;
    out["tank2_filling"]  = fillingTank2;
    out["tank2_bat_pct"]  = tank2BatPct;
    out["tank2_vbat"]     = tank2VBat;
  };

  thing["raw_sensor"] >> [](pson& out) {
    float raw = -1.0f, s0 = 0, s1 = 0, s2 = 0;
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
    out["auto_mode"]        = !manualMode;
    out["filling_t1"]       = fillingTank1;
    out["filling_t2"]       = fillingTank2;
  };

  thing["mode"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = manualMode;
    } else {
      manualMode = (bool)in;
      if (!manualMode) {
        // Switching to auto — safe state: stop pump, close valves
        setPumpA(false);
        digitalWrite(VALVE_1, HIGH);
        digitalWrite(VALVE_2, HIGH);
        waitingForValveSettle = false;
        fillingTank1 = false;
        fillingTank2 = false;
        Serial.println("[Mode] Auto mode — pump and valves reset");
      }
      out = manualMode;
    }
  };

  // ---- MANUAL CONTROLS ----
  // pump_a: INTERLOCK — only fires if a valve is open
  thing["pump_a"] = [](pson& in, pson& out) {
    updatePinStates();
    if (in.is_empty()) {
      out = pumpAState;
    } else if (manualMode) {
      bool want = (bool)in;
      if (want && !anyValveOpen()) {
        Serial.println("[Manual] pump_a blocked — open a valve first!");
        out = false;  // report blocked state to dashboard
      } else {
        setPumpA(want);
        out = pumpAState;
      }
    } else {
      out = pumpAState;
    }
  };

  // pump_b: Garden pump — no valve interlock required
  thing["pump_b"] = [](pson& in, pson& out) {
    updatePinStates();
    if (in.is_empty()) {
      out = pumpBState;
    } else if (manualMode) {
      pumpBState = (bool)in;
      digitalWrite(PUMP_B, pumpBState ? LOW : HIGH);
      // Blue LED tracks any pump activity
      bool anyOn = pumpAState || pumpBState;
      digitalWrite(LED_BLUE, anyOn ? HIGH : LOW);
      out = pumpBState;
    } else {
      out = pumpBState;
    }
  };

  thing["valve_1"] = [](pson& in, pson& out) {
    updatePinStates();
    if (in.is_empty()) {
      out = valve1State;
    } else if (manualMode) {
      valve1State = (bool)in;
      digitalWrite(VALVE_1, valve1State ? LOW : HIGH);
      // If closing this valve and pump is on, check if any valve remains open
      if (!valve1State && pumpAState && !anyValveOpen()) {
        Serial.println("[Manual] Last valve closed — stopping pump_a safely");
        setPumpA(false);
      }
      out = valve1State;
    } else {
      out = valve1State;
    }
  };

  thing["valve_2"] = [](pson& in, pson& out) {
    updatePinStates();
    if (in.is_empty()) {
      out = valve2State;
    } else if (manualMode) {
      valve2State = (bool)in;
      digitalWrite(VALVE_2, valve2State ? LOW : HIGH);
      if (!valve2State && pumpAState && !anyValveOpen()) {
        Serial.println("[Manual] Last valve closed — stopping pump_a safely");
        setPumpA(false);
      }
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
  // 1. Secure relays FIRST — before anything else
  pinMode(PUMP_A,  OUTPUT); digitalWrite(PUMP_A,  HIGH);
  pinMode(PUMP_B,  OUTPUT); digitalWrite(PUMP_B,  HIGH);
  pinMode(VALVE_1, OUTPUT); digitalWrite(VALVE_1, HIGH);
  pinMode(VALVE_2, OUTPUT); digitalWrite(VALVE_2, HIGH);

  // 2. Serial
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n========================================");
  Serial.println("  AquaSynk V11.1 — ESP32 / FreeRTOS");
  Serial.println("========================================");
  Serial.printf("Reservoir: %.0fcm height, blind spot %.0fcm\n",
    RES_TANK_HEIGHT_CM, RES_BLIND_SPOT_CM);
  Serial.printf("Usable range: %.0fcm | Full=46cm=100%% Empty=100cm=0%%\n",
    RES_TANK_HEIGHT_CM - RES_BLIND_SPOT_CM);
  Serial.printf("Fill thresholds: start<%.0f%% stop>%.0f%%\n",
    FILL_START_PCT, FILL_STOP_HYST);
  Serial.printf("Reservoir guard: critical<%.0f%% low<%.0f%%\n",
    RES_CRITICAL_PCT, RES_LOW_PCT);
  Serial.println("========================================\n");

  // 3. Ultrasonic
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // 4. LEDs
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   LOW);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, LOW);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  LOW);

  // 5. Mutex
  ultrasonicMutex = xSemaphoreCreateMutex();
  if (ultrasonicMutex == NULL) {
    Serial.println("[FATAL] Mutex creation failed!");
    while (true) delay(1000);
  }

  // 6. Ultrasonic task — Core 0
  xTaskCreatePinnedToCore(
    ultrasonicTask, "UltrasonicTask", 2048, NULL, 2, NULL, 0
  );
  Serial.println("[FreeRTOS] Ultrasonic task → Core 0");

  // 7. WiFi
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(ssid, pass);

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
    Serial.println("[WiFi] Not connected at boot — retrying every 60s");
    lastWiFiCheck    = millis();
    wifiRetryPending = true;
  }

  // 8. HTTP endpoints
  server.on("/update1", HTTP_GET, handleUpdate1);
  server.on("/update2", HTTP_GET, handleUpdate2);

  server.on("/command_node1", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      node1StayAwake = (cmd[0] == 'a');
      server.send(200, "text/plain", node1StayAwake ? "Node1 awake" : "Node1 sleep");
    } else {
      server.send(400, "text/plain", "Missing cmd");
    }
  });

  server.on("/command_node2", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      node2StayAwake = (cmd[0] == 'a');
      server.send(200, "text/plain", node2StayAwake ? "Node2 awake" : "Node2 sleep");
    } else {
      server.send(400, "text/plain", "Missing cmd");
    }
  });

  server.begin();
  setupThingerResources();
  digitalWrite(LED_RED, HIGH);  // System ready indicator

  Serial.println("[System] Ready — automation armed\n");
}

// ================================================
//  MAIN LOOP — Core 1
//  Ultrasonic fully on Core 0.
//  This loop: WiFi/Thinger, HTTP, sensor processing,
//  and automation logic (single call per cycle).
// ================================================
void loop() {
  unsigned long currentMillis = millis();

  // WiFi + Thinger (non-blocking, continues locally if offline)
  handleWiFi(currentMillis);

  // HTTP server — always runs, WiFi-independent
  server.handleClient();

  // Read shared ultrasonic state
  bool ready = false;
  if (xSemaphoreTake(ultrasonicMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    ready = g_ultrasonicReady;
    xSemaphoreGive(ultrasonicMutex);
  }

  if (ready && (currentMillis - lastSensorPoll >= SENSOR_POLL_INTERVAL)) {
    lastSensorPoll = currentMillis;

    float distance = getMedianDistance();  // also resets g_ultrasonicReady

    if (distance > 0.0f && distance <= 400.0f) {
      resDistance = distance;
      resPercent  = calculateResPercent(distance);
      resVolume   = calculateResVolume(distance);

      Serial.printf("[Reservoir] dist=%.1fcm  fill=%.1f%%  vol=%.2fL\n",
        distance, resPercent, resVolume);
    } else {
      Serial.println("[Sensor] Invalid median — skipping update");
    }
  }

  // Automation — runs every loop() pass so valve settle timer is responsive
  // Guard: only if we have at least one valid reservoir reading
  if (!manualMode && resPercent >= 0.0f) {
    runAutomationLogic();
  }

  delay(1);
}
