# AquaSynk — Intelligent Distributed Water Monitoring & Automation System 

![Theme1](https://i.postimg.cc/rFpMTXFT/Screenshot-2026-05-26-002951.png)

![Theme2](https://i.postimg.cc/ncLpxbcz/Screenshot-2026-05-26-002959.png)

## Overview

**AquaSynk** is a distributed IoT-based water monitoring and smart reservoir automation platform designed for real-time tank monitoring, autonomous water routing, intelligent pump/valve control, and low-power remote telemetry.

The project combines:

- A high-performance **ESP32 master controller**
- Multiple **ESP8266 ultrasonic sensor nodes**
- A fully custom-built **Progressive Web App (PWA)** dashboard
- Real-time cloud telemetry using **Thinger.io**
- Local HTTP communication between nodes
- Deep-sleep optimized sensor architecture
- Advanced UI/UX with animated real-time visualization

The system is engineered for:
- Smart buildings
- Water distribution systems
- Reservoir management
- Multi-tank automation
- Industrial monitoring
- Remote telemetry applications

---

# System Architecture

```text
                ┌────────────────────┐
                │   ESP8266 Node (x) │
                │ Ultrasonic Sensor  │
                └─────────┬──────────┘              
                          │ HTTP
                          ▼
                ┌────────────────────┐
                │                    │
                │   ESP32 MASTER     │
                │    AquaSynk V11    │
                │                    │
                └─────────┬──────────┘
                          │
         ┌────────────────┼────────────────┐
         ▼                ▼                ▼

   Pump Control      Valve Control     Thinger.io

                          │
                          ▼

                Progressive Web App
                Real-Time Dashboard
```

---

# Core Design Philosophy

AquaSynk was designed around four major engineering goals:

## 1. Distributed Sensor Topology

Instead of directly wiring every sensor into one controller, the system distributes sensing operations across independent ESP8266 nodes.

Benefits:
- Modular scalability
- Reduced wiring complexity
- Lower sensor noise
- Fault isolation
- Easier maintenance
- Remote deployment capability

---

## 2. Non-Blocking Real-Time Operation

Traditional ultrasonic sensing using `pulseIn()` blocks execution and causes WiFi instability.

AquaSynk solves this using:

- FreeRTOS task separation
- Dedicated ultrasonic processing core
- Mutex-protected shared state
- Non-blocking WiFi reconnect logic

This allows:
- Stable networking
- Responsive dashboard updates
- Continuous telemetry
- Reliable automation

---

## 3. Ultra Low Power Remote Nodes

Sensor nodes aggressively optimize power consumption through:

- Deep sleep
- Radio shutdown before sensing
- Wake → Sense → Transmit → Sleep cycle
- Dynamic stay-awake behavior during filling

This enables long-term remote deployment with minimal energy usage.

---

## 4. High-End Human Interface Design

The frontend is not a generic IoT dashboard.

AquaSynk implements:
- Glassmorphism
- Skeuomorphic controls
- Animated water simulation
- Real-time flow visualization
- Responsive mobile-first design
- Offline-capable PWA behavior
- Dark/light adaptive themes

The dashboard behaves more like a modern industrial control panel than a basic telemetry page.

---

# Hardware Stack

## Master Controller

| Component | Purpose |
|---|---|
| ESP32 | Main processing & automation |
| Relay Modules | Pump and valve switching |
| AJ-SR04 Ultrasonic | Main reservoir monitoring |
| LEDs | System state indicators |

---

## Sensor Nodes

| Component | Purpose |
|---|---|
| ESP8266 | Remote sensor node |
| Ultrasonic Sensor | Tank level sensing |
| Deep Sleep Logic | Battery optimization |

---

# Software Stack

## Firmware

### ESP32 Master Controller
- FreeRTOS
- WebServer
- ThingerESP32
- WiFi
- HTTP endpoints
- Automation engine

### ESP8266 Nodes
- ESP8266WiFi
- ESP8266HTTPClient
- Deep sleep management
- Radio power optimization

---

## Frontend Stack

| Technology | Usage |
|---|---|
| HTML5 | Structure |
| CSS3 | Advanced UI styling |
| Vanilla JavaScript | Real-time logic |
| Service Workers | Offline support |
| Web App Manifest | PWA installation |
| SVG Animation | Pipeline visualization |

---

# Major Features

# Real-Time Reservoir Monitoring

The master ESP32 continuously monitors:
- Reservoir depth
- Water percentage
- Estimated volume
- Tank state
- Sensor validity

---

# Distributed Tank Monitoring

Remote ESP8266 nodes:
- Independently measure water level
- Calculate percentages locally
- Send telemetry to the master controller
- Enter deep sleep when idle

---

# Intelligent Automation

The system supports:
- Automatic refill logic
- Pump routing
- Valve sequencing
- Manual override mode
- Autonomous fill state management

---

# FreeRTOS-Based Sensor Isolation

The ESP32 implementation separates ultrasonic sensing into a dedicated task.

This prevents:
- WiFi starvation
- Cloud disconnects
- UI freezing
- Relay timing issues

A mutex-protected shared state system ensures thread safety.

---

# Blind Spot Compensation

Ultrasonic sensors have a physical dead zone near the sensor face.

AquaSynk compensates using:

```cpp
const float RES_BLIND_SPOT_CM = 46.0; //Or However you configure your tanks
```

This allows:
- Accurate 100% calibration
- Correct full-tank reporting
- Real-world deployment precision

---

# Deep Sleep Optimization

ESP8266 nodes implement:
- Radio shutdown before sensing
- Modem sleep
- Deep sleep cycling
- Adaptive wake intervals

Normal operation:
- Wake every 2 minutes
- Sense
- Transmit
- Sleep

During active filling:
- Stay awake
- Increase update frequency

---

# Progressive Web App (PWA)

The frontend is fully installable as a mobile/desktop application.

Features include:
- Home screen installation
- Offline asset caching
- Standalone app mode
- Splash screen support
- Native-app behavior

---

# Advanced UI System

The dashboard includes:

## 3D Glass Tank Rendering
Custom CSS-rendered cylindrical tanks with:
- Dynamic water fill
- Ripple simulation
- Frosted glass effects
- Responsive scaling

## Animated Pipeline System
SVG-based flow routing visualization showing:
- Pump activity
- Valve activity
- Water routing
- Live system state

## Responsive Industrial Layout
- Mobile-first design
- Tablet optimization
- Desktop dashboard grid
- Adaptive scaling

---

# Offline Capability

Using service workers, AquaSynk:
- Caches UI assets
- Supports offline loading
- Reduces bandwidth usage
- Improves responsiveness

The service worker intentionally bypasses caching for live Thinger.io telemetry calls.

---

# Networking Architecture

## Communication Flow

### Sensor Nodes → ESP32
Communication uses:
- HTTP GET requests
- Local network transport
- Lightweight query-based telemetry

Example:

```text
/update1?distance=23.1&volume=120.2&percent=74.5
```

---

## ESP32 → Dashboard

The ESP32 hosts:
- Real-time dashboard
- Device state APIs
- Local monitoring interface

---

## ESP32 → Cloud

Cloud telemetry is handled through:
- Thinger.io integration
- WiFi reconnect management
- Non-blocking transmission

---

# Core Engineering Optimizations

# 1. FreeRTOS Task Separation

Ultrasonic processing runs independently from:
- WiFi stack
- Dashboard serving
- Automation logic

This dramatically improves reliability.

---

# 2. Mutex-Protected Shared State

Shared ultrasonic variables use:
```cpp
SemaphoreHandle_t ultrasonicMutex;
```

This prevents:
- Race conditions
- Corrupted readings
- Concurrent memory access issues

---

# 3. Non-Blocking WiFi Reconnection

WiFi reconnect attempts use `millis()` timing instead of blocking loops.

Benefits:
- Continuous automation
- Stable UI
- Better uptime

---

# 4. Radio Isolation During Sensing

ESP8266 nodes disable WiFi radio before ultrasonic reads.

This reduces:
- Sensor noise
- Timing instability
- RF interference

---

# Frontend Technical Highlights

## UI Design Language

The dashboard combines:
- Glassmorphism
- Soft shadows
- Skeuomorphic controls
- Animated gradients
- Dynamic SVG visualization

---

## Theme Engine

Supports:
- Light mode
- Dark mode
- Persistent theme behavior
- Dynamic CSS variable switching

---

## Responsive Scaling

The interface dynamically adapts for:
- Phones
- Tablets
- Desktop monitors

---

# File Structure

```text
AquaSynk/
│
├── AquaSynk_V11.ino     # ESP32 master controller
├── Node_v11.ino         # ESP8266 sensor node
├── index.html           # Main PWA dashboard
├── manifest.json        # PWA configuration
├── sw.js                # Service worker
│
└── icons/
    ├── icon-72x72.png
    ├── icon-96x96.png
    ├── icon-128x128.png
    ├── icon-144x144.png
    ├── icon-152x152.png
    ├── icon-192x192.png
    ├── icon-384x384.png
    └── icon-512x512.png
```

---

# Current Capabilities

- Multi-tank monitoring
- Reservoir visualization
- Real-time automation
- Deep-sleep telemetry nodes
- PWA dashboard
- Offline support
- Cloud telemetry
- Manual control mode
- Animated flow system
- Dark/light themes
- Responsive industrial UI

---

# Future Expansion Ideas

Potential upgrades:
- MQTT architecture
- OTA firmware updates
- AI-based water usage prediction
- Historical analytics
- Notification engine
- Solar-powered autonomous nodes
- LoRa communication
- Battery telemetry
- Pressure sensor fusion
- Leak detection algorithms

---

# Engineering Highlights

## Why AquaSynk Is Different

Most hobby IoT water systems:
- Use blocking code
- Have poor UI
- Lack scalability
- Ignore power optimization
- Fail under network instability

AquaSynk approaches the problem more like a professional industrial automation platform.

The project emphasizes:
- Real engineering architecture
- Reliability
- Scalability
- User experience
- Embedded optimization
- Distributed system design

---

# Credits

Designed & Developed by:

## Max
Electronics & Embedded Systems Developer

With:
## Foton Labz

---

# License

This project is open-source and available under the MIT License.

---

# Repository Notes

Before deployment:
- Update WiFi credentials
- Configure static IPs
- Set Thinger.io credentials
- Calibrate tank dimensions
- Adjust blind spot compensation values

---

# Final Notes

AquaSynk is not just a water level monitor.

It is a distributed embedded monitoring ecosystem combining:
- Embedded systems
- Real-time networking
- Industrial-style automation
- Modern frontend engineering
- Power-optimized IoT architecture

into one cohesive platform.
