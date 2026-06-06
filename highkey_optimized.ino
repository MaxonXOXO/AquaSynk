// ================================================
//  AQUASYNK — Master Controller V8 OPTIMIZED
//  Platform: Thinger.io 
//  Board: ESP8266
// ================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ThingerESP8266.h>

// ---------- THINGER.IO CREDENTIALS ----------
#define USERNAME    "Hydra_Mesh"
#define DEVICE_ID   "Hydra"
#define DEVICE_CRED "hydramesh"

// ---------- WIFI CREDENTIALS ----------
const char* ssid = "Thunderbird 242";
const char* pass = "9947666371";

// ---------- STATIC IP ----------
IPAddress local_IP(192, 168, 1, 68);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ---------- THINGER INSTANCE ----------
ThingerESP8266 thing(USERNAME, DEVICE_ID, DEVICE_CRED);


// ---------- HARDWARE PINS ----------
#define PUMP_A  D2   // GPIO4  - Relay 1
#define PUMP_B  D5   // GPIO14 - Relay 2
#define VALVE_1 D6   // GPIO12 - Relay 3
#define VALVE_2 D7   // GPIO13 - Relay 4

// AJ-SR04 ULTRASONIC (Optimized for this sensor)
#define TRIG_PIN D3  // GPIO0
#define ECHO_PIN D8  // GPIO15

// LED PINS
#define LED_RED   D0 // GPIO16
#define LED_GREEN D1 // GPIO5
#define LED_BLUE  D4 // GPIO2

// ---------- AJ-SR04 OPTIMIZED CONSTANTS ----------
const float TANK1_MAX_CM     = 95.0;
const float TANK2_MAX_CM     = 95.0;
const float RES_TANK_HEIGHT_CM = 110.0;
const float RES_TANK_DIAMETER_CM = 80.0;

float ultrasonicSamples[3] = {0, 0, 0};
uint8_t ultrasonicSampleIdx = 0;
unsigned long lastUltrasonicSample = 0;
bool ultrasonicReady = false;


// AJ-SR04 Specific: 20cm blind spot + optimized timing
const float RES_SENSOR_OFFSET_CM = 20.0;
const float SOUND_SPEED = 0.0343;  // cm/µs at 20°C
const unsigned long ULTRASONIC_TIMEOUT = 23200; // ~400cm max range for AJ-SR04
const unsigned long SENSOR_POLL_INTERVAL = 3000; // 3s interval (tanks fill slowly)
const unsigned long WIFI_RETRY_INTERVAL = 15000; // 15s WiFi retry

// Pre-calculated values (compile-time optimization)
const float RES_RADIUS_CM = RES_TANK_DIAMETER_CM / 2.0;
const float RES_BASE_AREA = PI * RES_RADIUS_CM * RES_RADIUS_CM;

// ---------- STATE VARIABLES ----------
float resDistance   = 0.0;
float resPercent    = 0.0;
float resVolume     = 0.0;
float tank1Distance = 0.0;
float tank1Percent  = 0.0;
float tank2Distance = 0.0;
float tank2Percent  = 0.0;

bool fillingTank1 = false;
bool fillingTank2 = false;
bool manualMode   = false;
bool node1StayAwake = false;
bool node2StayAwake = false;
bool node1Filling = false;
bool node2Filling = false;

// Cached pin states (reduces digitalRead calls)
bool pumpAState = false;
bool pumpBState = false;
bool valve1State = false;
bool valve2State = false;

unsigned long lastSensorPoll = 0;
unsigned long lastWiFiCheck = 0;

ESP8266WebServer server(80);

// ================================================
//  INLINE MATH HELPERS (Faster execution)
// ================================================

inline float calculateResVolume(float distanceCm) {
  float actualDistance = distanceCm - RES_SENSOR_OFFSET_CM;
  float waterHeightCm = RES_TANK_HEIGHT_CM - actualDistance;
  
  if (waterHeightCm < 0.0) waterHeightCm = 0.0;
  else if (waterHeightCm > RES_TANK_HEIGHT_CM) waterHeightCm = RES_TANK_HEIGHT_CM;
  
  return (RES_BASE_AREA * waterHeightCm) * 0.001; // Combined multiply
}

inline float calculateResPercent(float distanceCm) {
  float actualDistance = distanceCm - RES_SENSOR_OFFSET_CM;
  float waterHeightCm = RES_TANK_HEIGHT_CM - actualDistance;
  
  if (waterHeightCm < 0.0) return 0.0;
  if (waterHeightCm > RES_TANK_HEIGHT_CM) return 100.0;
  
  return (waterHeightCm / RES_TANK_HEIGHT_CM) * 100.0;
}

inline float calculateFillPercent(float distance, float maxDepth) {
  if (distance >= maxDepth) return 0.0;
  if (distance <= 0.0) return 100.0;
  
  float percent = ((maxDepth - distance) / maxDepth) * 100.0;
  return (percent > 100.0) ? 100.0 : ((percent < 0.0) ? 0.0 : percent);
}

// ================================================
//  OPTIMIZED SENSOR READ
// ================================================

inline float readAJSR04() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT);
  return (duration > 0) ? (duration * SOUND_SPEED) * 0.5 : -1.0;
}

float getMedianDistance() {
  float d[3] = {ultrasonicSamples[0], ultrasonicSamples[1], ultrasonicSamples[2]};
  if (d[0] > d[1]) { float t = d[0]; d[0] = d[1]; d[1] = t; }
  if (d[1] > d[2]) { float t = d[1]; d[1] = d[2]; d[2] = t; }
  if (d[0] > d[1]) { float t = d[0]; d[0] = d[1]; d[1] = t; }
  return d[1];
}

// ================================================
//  CACHED PIN STATE UPDATER
// ================================================

inline void updatePinStates() {
  pumpAState = (digitalRead(PUMP_A) == LOW);
  pumpBState = (digitalRead(PUMP_B) == LOW);
  valve1State = (digitalRead(VALVE_1) == LOW);
  valve2State = (digitalRead(VALVE_2) == LOW);
}

// ================================================
//  OPTIMIZED AUTOMATION LOGIC
// ================================================

void runAutomationLogic() {
  // Hysteresis logic
  if (tank1Percent <= 50.0) fillingTank1 = true;
  else if (tank1Percent >= 95.0) fillingTank1 = false;
  
  if (tank2Percent <= 50.0) fillingTank2 = true;
  else if (tank2Percent >= 95.0) fillingTank2 = false;
  
  // Reservoir protection
  if (resPercent <= 25.0) {
    fillingTank1 = false;
    fillingTank2 = false;
  }
  
  // Update cached pin states once
  updatePinStates();
  
  bool pumpNeedsToRun = fillingTank1 || fillingTank2;
  bool pumpIsRunning = pumpAState || pumpBState;
  
  // LED update (only when state changes)
  static bool lastLEDState = false;
  if (pumpIsRunning != lastLEDState) {
    digitalWrite(LED_BLUE, pumpIsRunning ? HIGH : LOW);
    lastLEDState = pumpIsRunning;
  }
  
  // Pump control with minimal delays
  if (pumpNeedsToRun && !pumpIsRunning) {
    // Set valves first, then pump
    digitalWrite(VALVE_1, fillingTank1 ? LOW : HIGH);
    digitalWrite(VALVE_2, fillingTank2 ? LOW : HIGH);
    delayMicroseconds(500); // Reduced from 100ms delay
    digitalWrite(PUMP_A, LOW);
    
  } else if (!pumpNeedsToRun && pumpIsRunning) {
    // Stop pump first, then close valves
    digitalWrite(PUMP_A, HIGH);
    digitalWrite(PUMP_B, HIGH);
    delayMicroseconds(500);
    digitalWrite(VALVE_1, HIGH);
    digitalWrite(VALVE_2, HIGH);
    
  } else if (pumpIsRunning) {
    // Adjust valves while running
    if (valve1State != fillingTank1) digitalWrite(VALVE_1, fillingTank1 ? LOW : HIGH);
    if (valve2State != fillingTank2) digitalWrite(VALVE_2, fillingTank2 ? LOW : HIGH);
  }
}

// ================================================
//  HTTP HANDLERS (Optimized string handling)
// ================================================

void handleUpdate1() {
  if (!server.hasArg("distance")) {
    server.send(400, "text/plain", "Missing distance");
    return;
  }
  
  tank1Distance = server.arg("distance").toFloat();
  
  if (server.hasArg("percent")) {
    tank1Percent = server.arg("percent").toFloat();
  } else {
    tank1Percent = calculateFillPercent(tank1Distance, TANK1_MAX_CM);
  }
  
  updatePinStates(); // Single read for all states
  bool anyPumpOn = pumpAState || pumpBState;
  bool manualFilling = manualMode && anyPumpOn && valve1State;
  bool keepAwake = fillingTank1 || manualFilling;
  
  server.send(200, "text/plain", keepAwake ? "STAY_AWAKE" : "GO_SLEEP");
  
  if (!manualMode) runAutomationLogic();
}

void handleUpdate2() {
  if (!server.hasArg("distance")) {
    server.send(400, "text/plain", "Missing distance");
    return;
  }
  
  tank2Distance = server.arg("distance").toFloat();
  
  if (server.hasArg("percent")) {
    tank2Percent = server.arg("percent").toFloat();
  } else {
    tank2Percent = calculateFillPercent(tank2Distance, TANK2_MAX_CM);
  }
  
  updatePinStates();
  bool anyPumpOn = pumpAState || pumpBState;
  bool manualFilling = manualMode && anyPumpOn && valve2State;
  bool keepAwake = fillingTank2 || manualFilling;
  
  server.send(200, "text/plain", keepAwake ? "STAY_AWAKE" : "GO_SLEEP");
  
  if (!manualMode) runAutomationLogic();
}

// ================================================
//  THINGER.IO RESOURCE SETUP (Optimized)
// ================================================

void setupThingerResources() {
  
  // Combined sensor data
  thing["sensors"] >> [](pson& out) {
    out["res_percent"] = resPercent;
    out["res_distance"] = resDistance;
    out["res_volume"] = resVolume;
    out["tank1_percent"] = tank1Percent;
    out["tank1_distance"] = tank1Distance;
    out["tank1_filling"] = fillingTank1;
    out["tank2_percent"] = tank2Percent;
    out["tank2_distance"] = tank2Distance;
    out["tank2_filling"] = fillingTank2;
  };
  
  // NEW: Raw ultrasonic sensor readings (fixed syntax)
  thing["raw_sensor"] >> [](pson& out) {
    out["raw_distance_cm"] = resDistance;  // The actual median distance reading in cm
    
    // Add individual sample readings
    out["sample_0"] = ultrasonicSamples[0];
    out["sample_1"] = ultrasonicSamples[1];
    out["sample_2"] = ultrasonicSamples[2];
    
    out["samples_ready"] = ultrasonicReady;  // Whether we have full sample buffer
    out["sample_index"] = ultrasonicSampleIdx; // Current write position in buffer
    
    // Add calculated values for reference
    float actualHeight = RES_TANK_HEIGHT_CM - (resDistance - RES_SENSOR_OFFSET_CM);
    if (actualHeight < 0) actualHeight = 0;
    if (actualHeight > RES_TANK_HEIGHT_CM) actualHeight = RES_TANK_HEIGHT_CM;
    
    out["actual_water_height_cm"] = actualHeight;
    out["sensor_offset_cm"] = RES_SENSOR_OFFSET_CM;
    out["tank_total_height_cm"] = RES_TANK_HEIGHT_CM;
    out["sound_speed_used"] = SOUND_SPEED;
  };
  
  // Relay states
  thing["relays"] >> [](pson& out) {
    out["pump_a"] = (digitalRead(PUMP_A) == LOW);
    out["pump_b"] = (digitalRead(PUMP_B) == LOW);
    out["valve_1"] = (digitalRead(VALVE_1) == LOW);
    out["valve_2"] = (digitalRead(VALVE_2) == LOW);
  };
  
  // System health
  thing["health"] >> [](pson& out) {
    out["ram"] = ESP.getFreeHeap();
    out["frag"] = ESP.getHeapFragmentation();
    out["max_block"] = ESP.getMaxFreeBlockSize();
    out["wifi"] = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    out["last_sensor_poll"] = lastSensorPoll;
  };
  
  // Mode control
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
//  OPTIMIZED SETUP
// ================================================

void setup() {
  // Hardware init (batch pin configuration)
  digitalWrite(PUMP_A, HIGH); pinMode(PUMP_A, OUTPUT);  
  digitalWrite(PUMP_B, HIGH); pinMode(PUMP_B, OUTPUT);  
  digitalWrite(VALVE_1, HIGH); pinMode(VALVE_1, OUTPUT); 
  digitalWrite(VALVE_2, HIGH); pinMode(VALVE_2, OUTPUT);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_RED, OUTPUT);   digitalWrite(LED_RED, LOW);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, LOW);
  pinMode(LED_BLUE, OUTPUT);  digitalWrite(LED_BLUE, LOW);
  
  // WiFi optimization
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);;
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(ssid, pass);
  
  // HTTP endpoints
  server.on("/update1", HTTP_GET, handleUpdate1);
  server.on("/update2", HTTP_GET, handleUpdate2);
  
  server.on("/command_node1", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd[0] == 'a') { // "awake"
        node1StayAwake = true;
        server.send(200, "text/plain", "Node1 awake");
      } else { // "sleep"
        node1StayAwake = false;
        node1Filling = false;
        server.send(200, "text/plain", "Node1 sleep");
      }
    }
  });
  
  server.on("/command_node2", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd[0] == 'a') {
        node2StayAwake = true;
        server.send(200, "text/plain", "Node2 awake");
      } else {
        node2StayAwake = false;
        node2Filling = false;
        server.send(200, "text/plain", "Node2 sleep");
      }
    }
  });
  
  server.begin();
  setupThingerResources();
  digitalWrite(LED_RED, HIGH);
}

// ================================================
//  SUPER-OPTIMIZED MAIN LOOP
// ================================================

void loop() {
  unsigned long currentMillis = millis();
  
  // Non-blocking WiFi management
  if (WiFi.status() != WL_CONNECTED) {
  digitalWrite(LED_GREEN, LOW);
  if (currentMillis - lastWiFiCheck >= WIFI_RETRY_INTERVAL) {
    lastWiFiCheck = currentMillis;
    WiFi.reconnect();
  }
} else {
  digitalWrite(LED_GREEN, HIGH);
  thing.handle();
}
  
  // Always handle HTTP (even without WiFi for local access)
  server.handleClient();
  
  // Timed sensor operations
  if (currentMillis - lastUltrasonicSample >= 100) {  // one sample every 100ms
  lastUltrasonicSample = currentMillis;
  
  float reading = readAJSR04();
  if (reading > 0.0) {
    ultrasonicSamples[ultrasonicSampleIdx] = reading;
    ultrasonicSampleIdx = (ultrasonicSampleIdx + 1) % 3;
    if (ultrasonicSampleIdx == 0) ultrasonicReady = true; // full ring buffer
  }

  if (!ultrasonicReady && currentMillis > 10000) {
  // Fill with last known good or zero, don't leave system blind
    ultrasonicReady = true;
  }

}

if (ultrasonicReady && (currentMillis - lastSensorPoll >= SENSOR_POLL_INTERVAL)) {
  lastSensorPoll = currentMillis;
  
  float distance = getMedianDistance();
  resDistance = distance;
  resPercent = calculateResPercent(distance);
  resVolume = calculateResVolume(distance);
  
  if (!manualMode) runAutomationLogic();
  
    digitalWrite(LED_GREEN, (WiFi.status() == WL_CONNECTED) ? HIGH : LOW);
  }
  
  // Feed watchdog and background tasks
  yield();
}