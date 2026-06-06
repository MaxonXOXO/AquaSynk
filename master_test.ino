// ================================================
//  MARINE GUARDIAN — Master Controller V7
//  Platform: Thinger.io (ported from Blynk V6)
//  Board: ESP8266
// ================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ThingerESP8266.h>

// ---------- THINGER.IO CREDENTIALS ----------
#define USERNAME    "Hydra_Mesh"
#define DEVICE_ID   "Hydra"
#define DEVICE_CRED "hydramesh"   // From Thinger.io device page

// ---------- WIFI CREDENTIALS ----------
const char* ssid = "Thunderbird 242";
const char* pass = "9947666371";
unsigned long prevLoopTime = 0;
unsigned long lastWiFiCheckTime = 0; // NEW: Timer for WiFi reconnection

// Node control variables
bool node1StayAwake = false;
bool node2StayAwake = false;
bool node1Filling = false;
bool node2Filling = false;

// ---------- STATIC IP ----------
IPAddress local_IP(192, 168, 1, 68);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ---------- THINGER INSTANCE ----------
ThingerESP8266 thing(USERNAME, DEVICE_ID, DEVICE_CRED);

// ---------- HARDWARE PINS ----------
// RELAYS (Active LOW)
#define PUMP_A  D2   // GPIO4  - Relay 1 - Main Pump
#define PUMP_B  D5   // GPIO14 - Relay 2 - Backup Pump
#define VALVE_1 D6   // GPIO12 - Relay 3 - Routing to Tank 1
#define VALVE_2 D7   // GPIO13 - Relay 4 - Routing to Tank 2

// ULTRASONIC SENSOR
#define TRIG_PIN D3  // GPIO0
#define ECHO_PIN D8  // GPIO15

// LED PINS (Active HIGH)
#define LED_RED   D0 // GPIO16 - System Initialized
#define LED_GREEN D1 // GPIO5  - Server/Data Status
#define LED_BLUE  D4 // GPIO2  - Pump Active

// ---------- CONSTANTS ----------
const float TANK1_MAX_CM     = 95.0; 
const float TANK2_MAX_CM     = 95.0; 

// Reservoir Specifications
const float RES_TANK_HEIGHT_CM = 110.0;
const float RES_TANK_DIAMETER_CM = 80.0;
const float RES_SENSOR_OFFSET_CM = 20.0; // The AJ-SR04 blind spot gap
const float RES_RADIUS_CM = RES_TANK_DIAMETER_CM / 2.0;
const float RES_BASE_AREA = PI * RES_RADIUS_CM * RES_RADIUS_CM;

// ---------- STATE VARIABLES ----------
float resDistance   = 0;
float resPercent    = 0;
float resVolume     = 0;

float tank1Distance = 0;
float tank1Percent  = 0;

float tank2Distance = 0;
float tank2Percent  = 0;

bool fillingTank1 = false;
bool fillingTank2 = false;
bool manualMode   = false;

unsigned long prevLoopTime = 0;

ESP8266WebServer server(80);

// ================================================
//  MATH HELPERS
// ================================================

float calculateResVolume(float distanceCm) {
  float actualDistance = distanceCm - RES_SENSOR_OFFSET_CM;
  float waterHeightCm = RES_TANK_HEIGHT_CM - actualDistance;
  
  if (waterHeightCm < 0) waterHeightCm = 0;
  if (waterHeightCm > RES_TANK_HEIGHT_CM) waterHeightCm = RES_TANK_HEIGHT_CM;
  
  // Use the pre-calculated area, saving CPU cycles
  float volumeCm3 = RES_BASE_AREA * waterHeightCm;
  return volumeCm3 / 1000.0; 
}

float calculateResPercent(float distanceCm) {
  float actualDistance = distanceCm - RES_SENSOR_OFFSET_CM;
  float waterHeightCm = RES_TANK_HEIGHT_CM - actualDistance;
  
  if (waterHeightCm < 0) return 0.0;
  if (waterHeightCm > RES_TANK_HEIGHT_CM) return 100.0;
  
  return (waterHeightCm / RES_TANK_HEIGHT_CM) * 100.0;
}


float calculateFillPercent(float distance, float maxDepth) {
  if (distance >= maxDepth) return 0.0;
  if (distance <= 0)        return 100.0;
  float percent = ((maxDepth - distance) / maxDepth) * 100.0;
  if (percent > 100.0) percent = 100.0;
  if (percent < 0.0)   percent = 0.0;
  return percent;
}

// ================================================
//  AUTOMATION LOGIC
// ================================================

void runAutomationLogic() {
  if (tank1Percent <= 50.0) fillingTank1 = true;
  if (tank1Percent >= 95.0) fillingTank1 = false;

  if (tank2Percent <= 50.0) fillingTank2 = true;
  if (tank2Percent >= 95.0) fillingTank2 = false;

  if (resPercent <= 25.0) {
    fillingTank1 = false;
    fillingTank2 = false;
    Serial.println("[LOGIC] ALERT: Reservoir below 25%. Suspending refills.");
  }

  // LOGGING ONLY - We let the HTTP handlers handle the state changes 
  // so we don't accidentally clear the wake state before sending the GO_SLEEP command.
  if (fillingTank1 && !node1Filling) {
    Serial.println("[LOGIC] Tank 1 filling started - waiting for node check-in to wake it up");
  }
  if (!fillingTank1 && node1Filling) {
    Serial.println("[LOGIC] Tank 1 filling complete - waiting for node check-in to put it to sleep");
  }
  
  if (fillingTank2 && !node2Filling) {
    Serial.println("[LOGIC] Tank 2 filling started - waiting for node check-in to wake it up");
  }
  if (!fillingTank2 && node2Filling) {
    Serial.println("[LOGIC] Tank 2 filling complete - waiting for node check-in to put it to sleep");
  }

  // Pump control logic
  bool pumpNeedsToRun = fillingTank1 || fillingTank2;
  bool pumpIsRunning  = (digitalRead(PUMP_A) == LOW);

  digitalWrite(LED_BLUE, pumpIsRunning ? HIGH : LOW);

  if (pumpNeedsToRun && !pumpIsRunning) {
    if (fillingTank1) digitalWrite(VALVE_1, LOW);
    if (fillingTank2) digitalWrite(VALVE_2, LOW);
    delay(100);
    digitalWrite(PUMP_A, LOW);
    Serial.println("[LOGIC] Pump Started.");

  } else if (!pumpNeedsToRun && pumpIsRunning) {
    digitalWrite(PUMP_A, HIGH);
    delay(100);
    digitalWrite(VALVE_1, HIGH);
    digitalWrite(VALVE_2, HIGH);
    Serial.println("[LOGIC] All Tanks Full. Pump Stopped.");

  } else if (pumpIsRunning) {
    digitalWrite(VALVE_1, fillingTank1 ? LOW : HIGH);
    digitalWrite(VALVE_2, fillingTank2 ? LOW : HIGH);
  }
}

void updateRelayStateResource() {
  // Correct Thinger.io method: 
  // Trigger a stream event so the dashboard requests the fresh state
  // from the lambda defined in setupThingerResources()
  thing.stream("relay_states");
  Serial.println("[UPDATE] Relay state change pushed to dashboard");
}

// ================================================
//  HTTP SERVER (Receiving Sub-Node Data)
// ================================================

void handleUpdate1() {
  if (server.hasArg("distance")) {
    tank1Distance = server.arg("distance").toFloat();
    
    if (server.hasArg("percent")) {
      tank1Percent = server.arg("percent").toFloat();
    } else {
      tank1Percent = calculateFillPercent(tank1Distance, TANK1_MAX_CM);
    }
    
    float volume = server.hasArg("volume") ? server.arg("volume").toFloat() : 0.0;
    
    // --- MANUAL MODE CHECK ---
    // Is any pump ON (LOW) and is Valve 1 OPEN (LOW)?
    bool anyPumpOn = (digitalRead(PUMP_A) == LOW) || (digitalRead(PUMP_B) == LOW);
    bool manualFilling = (manualMode && anyPumpOn && (digitalRead(VALVE_1) == LOW));
    
    // Keep awake if Auto mode wants to fill OR if physically filling in Manual mode
    bool keepAwake = fillingTank1 || manualFilling;

    Serial.printf("[Node1] %.2fcm | %.1f%% | %.1fL | KeepAwake:%s\n", 
                  tank1Distance, tank1Percent, volume, keepAwake ? "YES" : "NO");
    
    // EXPLICIT COMMANDS
    if (keepAwake) {
      server.send(200, "text/plain", "STAY_AWAKE");
    } else {
      server.send(200, "text/plain", "GO_SLEEP");
    }
    
    if (!manualMode) runAutomationLogic();
  } else {
    server.send(400, "text/plain", "Missing distance");
  }
}

void handleUpdate2() {
  if (server.hasArg("distance")) {
    tank2Distance = server.arg("distance").toFloat();
    
    if (server.hasArg("percent")) {
      tank2Percent = server.arg("percent").toFloat();
    } else {
      tank2Percent = calculateFillPercent(tank2Distance, TANK2_MAX_CM);
    }
    
    float volume = server.hasArg("volume") ? server.arg("volume").toFloat() : 0.0;
    
    // --- MANUAL MODE CHECK ---
    // Is any pump ON (LOW) and is Valve 2 OPEN (LOW)?
    bool anyPumpOn = (digitalRead(PUMP_A) == LOW) || (digitalRead(PUMP_B) == LOW);
    bool manualFilling = (manualMode && anyPumpOn && (digitalRead(VALVE_2) == LOW));
    
    // Keep awake if Auto mode wants to fill OR if physically filling in Manual mode
    bool keepAwake = fillingTank2 || manualFilling;

    Serial.printf("[Node2] %.2fcm | %.1f%% | %.1fL | KeepAwake:%s\n", 
                  tank2Distance, tank2Percent, volume, keepAwake ? "YES" : "NO");
    
    // EXPLICIT COMMANDS
    if (keepAwake) {
      server.send(200, "text/plain", "STAY_AWAKE");
    } else {
      server.send(200, "text/plain", "GO_SLEEP");
    }
    
    if (!manualMode) runAutomationLogic();
  } else {
    server.send(400, "text/plain", "Missing distance");
  }
}
//  THINGER.IO RESOURCE DEFINITIONS

void setupThingerResources() {

  // ── READ-ONLY: Sensor Data ──────────────────────

  thing["reservoir"] >> [](pson& out) {
    out["percent"]  = resPercent;
    out["distance"] = resDistance;
    out["volume"]   = resVolume;
  };

  thing["tank1"] >> [](pson& out) {
    out["percent"]  = tank1Percent;
    out["distance"] = tank1Distance;
    out["filling"]  = fillingTank1;
  };

  thing["tank2"] >> [](pson& out) {
    out["percent"]  = tank2Percent;
    out["distance"] = tank2Distance;
    out["filling"]  = fillingTank2;
  };

  thing["all_levels"] >> [](pson& out) {
    out["reservoir"] = resPercent;
    out["tank1"]     = tank1Percent;
    out["tank2"]     = tank2Percent;
  };

  // ── READ-ONLY: Relay / Physical States ─────────

  thing["relay_states"] >> [](pson& out) {
    out["pump_a"]  = (digitalRead(PUMP_A)  == LOW);  // true = ON
    out["pump_b"]  = (digitalRead(PUMP_B)  == LOW);
    out["valve_1"] = (digitalRead(VALVE_1) == LOW);  // true = OPEN
    out["valve_2"] = (digitalRead(VALVE_2) == LOW);
  };

  // ── READ-ONLY: System Health ─────────
  thing["system_health"] >> [](pson& out) {
    out["free_ram"] = ESP.getFreeHeap();
  };
  // ── READ/WRITE: Mode Switch ───────────

  thing["mode"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = manualMode; 
    } else {
      manualMode = (bool)in;
      if (manualMode) {
        Serial.println("[MODE] --- MANUAL OVERRIDE ENGAGED ---");
      } else {
        Serial.println("[MODE] --- AUTO MODE RESUMED ---");
        runAutomationLogic();
      }
      out = manualMode; 
    }
  };

  // ── WRITE: Manual Pump A Control ──────

  thing["pump_a"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = (digitalRead(PUMP_A) == LOW);
    } else {
      if (manualMode) {
        bool state = (bool)in;
        digitalWrite(PUMP_A, state ? LOW : HIGH);
        digitalWrite(LED_BLUE, state ? HIGH : LOW);
        Serial.printf("[MANUAL] Pump A: %s\n", state ? "ON" : "OFF");
        
        updateRelayStateResource();
      } else {
        Serial.println("[MANUAL] Ignored — not in manual mode.");
      }
      out = (digitalRead(PUMP_A) == LOW); 
    }
  };

  // ── WRITE: Manual Pump B Control ──────

  thing["pump_b"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = (digitalRead(PUMP_B) == LOW);
    } else {
      if (manualMode) {
        bool state = (bool)in;
        digitalWrite(PUMP_B, state ? LOW : HIGH);
        digitalWrite(LED_BLUE, state ? HIGH : LOW);
        Serial.printf("[MANUAL] Pump B: %s\n", state ? "ON" : "OFF");
        
        updateRelayStateResource();
      } else {
        Serial.println("[MANUAL] Ignored — not in manual mode.");
      }
      out = (digitalRead(PUMP_B) == LOW); 
    }
  };

  // ── WRITE: Manual Valve 1 Control ─────

  thing["valve_1"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = (digitalRead(VALVE_1) == LOW);
    } else {
      if (manualMode) {
        bool state = (bool)in;
        digitalWrite(VALVE_1, state ? LOW : HIGH);
        digitalWrite(LED_BLUE, state ? HIGH : LOW);
        Serial.printf("[MANUAL] Valve 1: %s\n", state ? "ON" : "OFF");
        
        updateRelayStateResource();
      } else {
        Serial.println("[MANUAL] Ignored — not in manual mode.");
      }
      out = (digitalRead(VALVE_1) == LOW); 
    }
  };

  // ── WRITE: Manual Valve 2 Control ─────

  thing["valve_2"] = [](pson& in, pson& out) {
    if (in.is_empty()) {
      out = (digitalRead(VALVE_2) == LOW);
    } else {
      if (manualMode) {
        bool state = (bool)in;
        digitalWrite(VALVE_2, state ? LOW : HIGH);
        digitalWrite(LED_BLUE, state ? HIGH : LOW);
        Serial.printf("[MANUAL] Valve 2: %s\n", state ? "ON" : "OFF");
        
        updateRelayStateResource();
      } else {
        Serial.println("[MANUAL] Ignored — not in manual mode.");
      }
      out = (digitalRead(VALVE_2) == LOW); 
    }
  };
}

// ================================================
//  SETUP
// ================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n[BOOT] Master Controller V7 — Thinger.io");

  // Relays (Active LOW — start OFF)
  digitalWrite(PUMP_A,  HIGH); pinMode(PUMP_A,  OUTPUT); 
  digitalWrite(PUMP_B,  HIGH); pinMode(PUMP_B,  OUTPUT); 
  digitalWrite(VALVE_1, HIGH); pinMode(VALVE_1, OUTPUT); 
  digitalWrite(VALVE_2, HIGH); pinMode(VALVE_2, OUTPUT);
  // Ultrasonic
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // LEDs
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   LOW);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, LOW);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  LOW);

  // --- OPTIMIZED WIFI SETUP ---
  WiFi.persistent(false);                 // Prevent flash memory wear from constant WiFi writes
  WiFi.setSleepMode(WIFI_NONE_SLEEP);     // Keeps radio on: prevents dropped HTTP packets from nodes
  WiFi.mode(WIFI_STA);                    // Explicitly set to Station mode
  WiFi.setAutoReconnect(true);            // Tell the core to handle basic reconnections

  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(ssid, pass);
  
  Serial.println("[WiFi] Connection initiated (running in background)...");
  // NOTICE: The blocking while() loop is gone. 
  // The ESP will now boot instantly and run the automation even if the router is down.

  // HTTP Sub-node endpoints
  server.on("/update1", HTTP_GET, handleUpdate1);
  server.on("/update2", HTTP_GET, handleUpdate2);
  
  server.on("/command_node1", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd == "awake") {
        node1StayAwake = true;
        server.send(200, "text/plain", "Node1 awake mode ON");
      } else if (cmd == "sleep") {
        node1StayAwake = false;
        node1Filling = false;
        server.send(200, "text/plain", "Node1 sleep mode ON");
      }
    }
  });

  server.on("/command_node2", HTTP_GET, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd == "awake") {
        node2StayAwake = true;
        server.send(200, "text/plain", "Node2 awake mode ON");
      } else if (cmd == "sleep") {
        node2StayAwake = false;
        node2Filling = false;
        server.send(200, "text/plain", "Node2 sleep mode ON");
      }
    }
  });

  server.begin();
  Serial.println("[HTTP] Sub-node server started.");

  setupThingerResources();

  digitalWrite(LED_RED, HIGH);
  Serial.println("[BOOT] READY.");
}

// ================================================
//  LOOP
// ================================================

void loop() {
  unsigned long currentMillis = millis();

  // --- NON-BLOCKING WIFI MANAGER ---
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_GREEN, LOW); // LED off when disconnected
    
    // Try to reconnect every 10 seconds without freezing the code
    if (currentMillis - lastWiFiCheckTime >= 10000) {
      Serial.println("[WiFi] Connection lost! Attempting to reconnect...");
      WiFi.disconnect();
      WiFi.begin(ssid, pass);
      lastWiFiCheckTime = currentMillis;
    }
  } else {
    // We are connected. Run network tasks.
    thing.handle();
    server.handleClient();
    digitalWrite(LED_GREEN, HIGH);
  }

  // Master sensor poll + cloud push every 1.5s
  if (currentMillis - prevLoopTime >= 1500) {
    prevLoopTime = currentMillis;

    digitalWrite(LED_GREEN, LOW); // Flicker

    // Read ultrasonic
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 25000);

    if (duration > 0) {
      resDistance = (duration * 0.0343) / 2.0;
      resPercent  = calculateResPercent(resDistance);
      resVolume   = calculateResVolume(resDistance);
    } else {
      Serial.println("[MASTER SENSOR] Out of range or no echo.");
    }

    if (!manualMode) {
      runAutomationLogic();
    }

    digitalWrite(LED_GREEN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);

  Serial.printf("[LOOP] Res: %.1f%% | T1: %.1f%% | T2: %.1f%% | Mode: %s\n",
      resPercent, tank1Percent, tank2Percent, manualMode ? "MANUAL" : "AUTO");
  }

  // OPTIMIZATION: Feed the background Watchdog timer
  yield(); 
}