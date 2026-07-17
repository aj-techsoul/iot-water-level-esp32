/**
 * ESP32 Water Tank Monitoring System
 * 
 * Hardware Configuration:
 * - Board: ESP32 DEVKIT V1
 * - Sensor: HC-SR04 (Trig: GPIO 5, Echo: GPIO 18)
 * - Green LED: GPIO 2
 * - Yellow LED: GPIO 4
 * - Red LED: GPIO 15
 * - Passive Buzzer: GPIO 19
 * 
 * Target Environment:
 * - Arduino IDE 2.x
 * - ESP32 Arduino Core 3.3.x (uses new non-deprecated LEDC/Tone APIs)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ==========================================
// Pins
// ==========================================
#define PIN_TRIG 5
#define PIN_ECHO 18

#define PIN_LED_GREEN 2
#define PIN_LED_YELLOW 4
#define PIN_LED_RED 15

#define PIN_BUZZER 19

// ==========================================
// Variables
// ==========================================
// WiFi & Configuration Constants
const char* AP_SSID = "WaterTank-TechSoul";
const char* AP_PASS = "12345678";

// Configuration Settings (Loaded from Preferences)
String configSSID = "";
String configPass = "";
float thresholdHigh = 8.0;   // Distance in cm representing HIGH water (default: 8cm)
float thresholdLow = 35.0;  // Distance in cm representing LOW water (default: 35cm)

// System States
enum WaterState {
  STATE_HIGH,
  STATE_MEDIUM,
  STATE_LOW,
  STATE_ERROR
};

enum SystemMode {
  MODE_AUTO,
  MODE_TEST_HIGH,
  MODE_TEST_MEDIUM,
  MODE_TEST_LOW,
  MODE_TEST_ERROR
};

enum BuzzerMode {
  BUZZ_NONE,
  BUZZ_SUCCESS,
  BUZZ_AMBULANCE,
  BUZZ_ERROR,
  BUZZ_TEST
};

enum LEDMode {
  LED_NORMAL,
  LED_TEST
};

// State Variables
WaterState currentWaterState = STATE_MEDIUM;
WaterState previousWaterState = STATE_MEDIUM;
SystemMode systemMode = MODE_AUTO;
BuzzerMode currentBuzzerMode = BUZZ_NONE;
LEDMode currentLEDMode = LED_NORMAL;

float currentDistance = -1.0;
float currentPercentage = -1.0;
int consecutiveErrorCount = 0;
bool wifiModeSTA = false;

// Non-blocking Timers
unsigned long lastSensorReadTime = 0;
const unsigned long SENSOR_READ_INTERVAL = 1000; // Measure once per second

// LED Timer Variables
unsigned long lastLEDBlinkTime = 0;
bool ledBlinkState = false;
unsigned long ledTestStartTime = 0;

// Buzzer Sequencer Variables
int buzzerStep = 0;
int buzzerPlayCount = 0;
unsigned long lastBuzzerActionTime = 0;
unsigned long buzzerTestStartTime = 0;
int buzzerTestStep = 0;

// Software Reboot Scheduling
bool restartRequested = false;
unsigned long restartTimer = 0;

// Web Server
WebServer server(80);

// Forward Declarations
void triggerStateBuzzer(WaterState state, bool force);
const char* getStateString(WaterState state);

// ==========================================
// Preferences
// ==========================================
Preferences preferences;

void loadConfigurations() {
  preferences.begin("tank-settings", true); // Open namespace in read-only mode
  configSSID = preferences.getString("ssid", "");
  configPass = preferences.getString("pass", "");
  thresholdHigh = preferences.getFloat("high", 8.0);
  thresholdLow = preferences.getFloat("low", 35.0);
  preferences.end();
  
  Serial.println("--- Configurations Loaded ---");
  Serial.print("WiFi SSID : "); Serial.println(configSSID);
  Serial.print("High Threshold: "); Serial.print(thresholdHigh); Serial.println(" cm");
  Serial.print("Low Threshold : "); Serial.print(thresholdLow); Serial.println(" cm");
  Serial.println("-----------------------------");
}

void saveConfigurations(const String& ssid, const String& pass, float high, float low) {
  preferences.begin("tank-settings", false); // Open namespace in read/write mode
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putFloat("high", high);
  preferences.putFloat("low", low);
  preferences.end();
  
  Serial.println("Configurations saved successfully.");
}

// ==========================================
// Web Pages
// ==========================================
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Water Tank Monitor</title>
  <style>
    :root {
      --bg-grad: linear-gradient(135deg, #0f172a 0%, #1e293b 100%);
      --card-bg: rgba(30, 41, 59, 0.65);
      --card-border: rgba(255, 255, 255, 0.08);
      --primary: #0ea5e9;
      --success: #10b981;
      --warning: #f59e0b;
      --danger: #ef4444;
      --text: #f8fafc;
      --text-muted: #94a3b8;
    }
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    }
    body {
      background: var(--bg-grad);
      color: var(--text);
      min-height: 100vh;
      padding: 20px;
      display: flex;
      justify-content: center;
      align-items: center;
      background-attachment: fixed;
    }
    .container {
      width: 100%;
      max-width: 960px;
      display: grid;
      grid-template-columns: 1.2fr 1fr;
      gap: 20px;
    }
    @media (max-width: 768px) {
      .container {
        grid-template-columns: 1fr;
      }
    }
    .header {
      grid-column: 1 / -1;
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      backdrop-filter: blur(12px);
      border-radius: 16px;
      padding: 16px 24px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 15px;
    }
    .header-logo {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .header-logo h1 {
      font-size: 22px;
      font-weight: 700;
      letter-spacing: -0.5px;
    }
    .header-logo h1 span {
      color: var(--primary);
    }
    .wifi-badge {
      display: flex;
      align-items: center;
      gap: 6px;
      font-size: 13px;
      background: rgba(14, 165, 233, 0.15);
      border: 1px solid rgba(14, 165, 233, 0.3);
      padding: 6px 12px;
      border-radius: 20px;
      color: #7dd3fc;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      backdrop-filter: blur(12px);
      border-radius: 16px;
      padding: 22px;
      box-shadow: 0 4px 30px rgba(0, 0, 0, 0.25);
      display: flex;
      flex-direction: column;
    }
    .card-title {
      font-size: 16px;
      font-weight: 600;
      color: var(--primary);
      margin-bottom: 18px;
      display: flex;
      align-items: center;
      gap: 8px;
      border-bottom: 1px solid rgba(255,255,255,0.06);
      padding-bottom: 8px;
    }
    .dashboard-content {
      display: grid;
      grid-template-columns: 110px 1fr;
      gap: 20px;
    }
    @media (max-width: 480px) {
      .dashboard-content {
        grid-template-columns: 1fr;
        justify-items: center;
      }
    }
    /* Visual Tank styling */
    .tank-wrapper {
      width: 100px;
      height: 180px;
      border: 4px solid #475569;
      border-radius: 16px;
      position: relative;
      background: rgba(15, 23, 42, 0.8);
      overflow: hidden;
      box-shadow: inset 0 0 12px rgba(0,0,0,0.6);
    }
    .water-fill {
      width: 100%;
      position: absolute;
      bottom: 0;
      background: linear-gradient(180deg, #38bdf8 0%, #0284c7 100%);
      transition: height 0.8s cubic-bezier(0.4, 0, 0.2, 1);
    }
    .water-wave {
      position: absolute;
      top: -6px;
      left: 0;
      width: 200%;
      height: 12px;
      background: radial-gradient(circle, transparent 20%, #38bdf8 21%);
      background-size: 10px 12px;
      animation: wave 2s linear infinite;
    }
    @keyframes wave {
      0% { transform: translateX(0); }
      100% { transform: translateX(-50%); }
    }
    .tank-text {
      position: absolute;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -50%);
      font-size: 20px;
      font-weight: 700;
      text-shadow: 0 2px 4px rgba(0,0,0,0.8);
      z-index: 5;
    }
    .metrics {
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      gap: 10px;
    }
    .metric-item {
      display: flex;
      justify-content: space-between;
      align-items: center;
      background: rgba(15, 23, 42, 0.4);
      padding: 10px 14px;
      border-radius: 8px;
    }
    .metric-label {
      font-size: 12px;
      color: var(--text-muted);
    }
    .metric-value {
      font-size: 16px;
      font-weight: 600;
    }
    .badge {
      padding: 4px 10px;
      border-radius: 6px;
      font-size: 11px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    .badge-high { background: rgba(16, 185, 129, 0.15); color: var(--success); border: 1px solid rgba(16, 185, 129, 0.3); }
    .badge-medium { background: rgba(245, 158, 11, 0.15); color: var(--warning); border: 1px solid rgba(245, 158, 11, 0.3); }
    .badge-low { background: rgba(239, 68, 68, 0.15); color: var(--danger); border: 1px solid rgba(239, 68, 68, 0.3); }
    .badge-error { background: rgba(239, 68, 68, 0.3); color: var(--danger); border: 1px solid var(--danger); animation: pulse 1s infinite; }
    
    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
    
    /* Buttons */
    .btn-group {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(80px, 1fr));
      gap: 8px;
      margin-top: 10px;
    }
    .btn {
      padding: 8px 12px;
      border-radius: 8px;
      border: 1px solid rgba(255, 255, 255, 0.1);
      background: rgba(255, 255, 255, 0.05);
      color: var(--text);
      font-size: 11px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s;
    }
    .btn:hover {
      background: rgba(255, 255, 255, 0.1);
      transform: translateY(-1px);
    }
    .btn-active {
      background: var(--primary) !important;
      color: #0f172a !important;
      border-color: var(--primary) !important;
    }
    .btn-action {
      background: rgba(14, 165, 233, 0.15);
      color: #7dd3fc;
      border-color: rgba(14, 165, 233, 0.3);
    }
    .btn-action:hover {
      background: var(--primary);
      color: #0f172a;
    }
    .btn-danger {
      background: rgba(239, 68, 68, 0.15);
      color: #fca5a5;
      border-color: rgba(239, 68, 68, 0.3);
    }
    .btn-danger:hover {
      background: var(--danger);
      color: white;
    }
    
    /* Hardware status indicators */
    .hw-status {
      display: flex;
      justify-content: space-around;
      background: rgba(15, 23, 42, 0.3);
      padding: 12px;
      border-radius: 10px;
      margin-top: 15px;
    }
    .hw-item {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 5px;
      font-size: 11px;
      color: var(--text-muted);
    }
    .led-dot {
      width: 14px;
      height: 14px;
      border-radius: 50%;
      background: #475569;
      box-shadow: inset 0 2px 4px rgba(0,0,0,0.4);
      transition: all 0.3s;
    }
    .led-dot.active-green { background: var(--success); box-shadow: 0 0 8px var(--success), 0 0 16px var(--success); }
    .led-dot.active-yellow { background: var(--warning); box-shadow: 0 0 8px var(--warning), 0 0 16px var(--warning); }
    .led-dot.active-red { background: var(--danger); box-shadow: 0 0 8px var(--danger), 0 0 16px var(--danger); }
    
    /* Forms */
    .form-group {
      margin-bottom: 12px;
    }
    .form-group label {
      display: block;
      font-size: 12px;
      color: var(--text-muted);
      margin-bottom: 4px;
    }
    .form-group input {
      width: 100%;
      padding: 8px 12px;
      border-radius: 8px;
      border: 1px solid rgba(255, 255, 255, 0.1);
      background: rgba(15, 23, 42, 0.5);
      color: white;
      font-size: 13px;
    }
    .form-group input:focus {
      outline: none;
      border-color: var(--primary);
    }
    .form-row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }
    
    .console-card {
      grid-column: 1 / -1;
    }
    .console {
      background: rgba(15, 23, 42, 0.8);
      border: 1px solid var(--card-border);
      border-radius: 8px;
      padding: 10px;
      font-family: monospace;
      font-size: 11px;
      height: 90px;
      overflow-y: auto;
      color: #38bdf8;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div class="header-logo">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="color: var(--primary);"><path d="M12 22a7 7 0 0 0 5-2.1c2-2 3-5.1 3-8.9a1 1 0 0 0-.2-.6l-7-8.1a1 1 0 0 0-1.6 0l-7 8.1a1 1 0 0 0-.2.6c0 3.8 1 6.9 3 8.9a7 7 0 0 0 5 2.1z"/></svg>
        <h1>SmartTank<span>IoT</span></h1>
      </div>
      <div class="wifi-badge">
        <span id="wifiStatus">Connecting...</span>
      </div>
    </div>

    <!-- Live Dashboard -->
    <div class="card">
      <div class="card-title">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><line x1="9" y1="3" x2="9" y2="21"/><line x1="15" y1="3" x2="15" y2="21"/><line x1="3" y1="9" x2="21" y2="9"/><line x1="3" y1="15" x2="21" y2="15"/></svg>
        Live Monitoring Dashboard
      </div>
      <div class="dashboard-content">
        <div class="tank-wrapper">
          <div class="water-fill" id="waterLevelBar" style="height: 0%;">
            <div class="water-wave"></div>
          </div>
          <div class="tank-text" id="tankPercentText">--%</div>
        </div>
        <div class="metrics">
          <div class="metric-item">
            <span class="metric-label">Current Distance</span>
            <span class="metric-value" id="distVal">-- cm</span>
          </div>
          <div class="metric-item">
            <span class="metric-label">Water Percentage</span>
            <span class="metric-value" id="pctVal">-- %</span>
          </div>
          <div class="metric-item">
            <span class="metric-label">System State</span>
            <span id="stateBadge" class="badge">UNKNOWN</span>
          </div>
          <div class="metric-item">
            <span class="metric-label">Active Thresholds</span>
            <span class="metric-value" id="thresholds" style="font-size: 11px; color: var(--text-muted);">--</span>
          </div>
        </div>
      </div>

      <div class="hw-status">
        <div class="hw-item">
          <div class="led-dot green" id="ledG"></div>
          <span>Green LED</span>
        </div>
        <div class="hw-item">
          <div class="led-dot yellow" id="ledY"></div>
          <span>Yellow LED</span>
        </div>
        <div class="hw-item">
          <div class="led-dot red" id="ledR"></div>
          <span>Red LED</span>
        </div>
        <div class="hw-item">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-bottom: 2px;"><path d="M12 2v20M17 5H9.5a3.5 3.5 0 0 0 0 7h5a3.5 3.5 0 0 1 0 7H6"/></svg>
          <span id="buzzerStatus" style="font-weight: 600; color: white; font-size: 12px;">--</span>
          <span>Buzzer Mode</span>
        </div>
      </div>
    </div>

    <!-- Configuration Panel -->
    <div class="card" style="justify-content: space-between;">
      <div>
        <div class="card-title">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
          System Configuration
        </div>
        <form id="settingsForm" onsubmit="saveSettings(event)">
          <div class="form-group">
            <label>WiFi SSID</label>
            <input type="text" id="inputSSID" required placeholder="SSID">
          </div>
          <div class="form-group">
            <label>WiFi Password</label>
            <input type="password" id="inputPass" required placeholder="Password">
          </div>
          <div class="form-row">
            <div class="form-group">
              <label>High Level Threshold (cm)</label>
              <input type="number" step="0.1" id="inputHigh" required placeholder="e.g. 8">
            </div>
            <div class="form-group">
              <label>Low Level Threshold (cm)</label>
              <input type="number" step="0.1" id="inputLow" required placeholder="e.g. 35">
            </div>
          </div>
          <button type="submit" class="btn btn-primary" style="width: 100%; margin-top: 5px;">Save Settings & Restart</button>
        </form>
      </div>

      <div style="margin-top: 15px;">
        <div style="font-size: 11px; text-transform: uppercase; color: var(--text-muted); margin-bottom: 6px; letter-spacing: 0.5px;">System Properties</div>
        <div style="font-size: 12px; display: grid; grid-template-columns: 1fr 1fr; gap: 6px;">
          <div>IP: <span id="ipAddress" style="color: white; font-weight: 500;">--</span></div>
          <div>Uptime: <span id="uptime" style="color: white; font-weight: 500;">--</span></div>
          <div>Mode: <span id="systemMode" style="color: white; font-weight: 500;">--</span></div>
          <div>Last Update: <span id="lastUpdate" style="color: white; font-weight: 500;">--</span></div>
        </div>
      </div>
    </div>

    <!-- Simulation & Diagnostic Controls -->
    <div class="card" style="grid-column: 1 / -1;">
      <div class="card-title">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="5 3 19 12 5 21 5 3"/></svg>
        Simulation & Diagnostic Panel
      </div>
      <div>
        <div style="font-size: 12px; color: var(--text-muted); margin-bottom: 8px;">Test State Simulation (Overrides Physical Sensor)</div>
        <div class="btn-group">
          <button id="btn-auto" onclick="triggerEndpoint('/test/auto')" class="btn">AUTO</button>
          <button id="btn-high" onclick="triggerEndpoint('/test/high')" class="btn">TEST HIGH</button>
          <button id="btn-medium" onclick="triggerEndpoint('/test/medium')" class="btn">TEST MEDIUM</button>
          <button id="btn-low" onclick="triggerEndpoint('/test/low')" class="btn">TEST LOW</button>
          <button id="btn-error" onclick="triggerEndpoint('/test/error')" class="btn">TEST ERROR</button>
        </div>
      </div>
      <div style="margin-top: 15px;">
        <div style="font-size: 12px; color: var(--text-muted); margin-bottom: 8px;">Hardware Diagnostics</div>
        <div class="btn-group">
          <button onclick="triggerEndpoint('/test/led')" class="btn btn-action">Test LEDs Cycle</button>
          <button onclick="triggerEndpoint('/test/buzzer')" class="btn btn-action">Test Buzzer Melodies</button>
          <button onclick="triggerEndpoint('/restart')" class="btn btn-danger" style="margin-bottom: 0;">Force Reboot ESP</button>
        </div>
      </div>
    </div>

    <!-- Log Console -->
    <div class="card console-card">
      <div class="card-title" style="margin-bottom: 8px;">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
        Live System Event Logs
      </div>
      <div class="console" id="statusConsole">
        <div>[System Ready] Awaiting updates...</div>
      </div>
    </div>
  </div>

  <script>
    function logMsg(msg, isErr = false) {
      const consoleEl = document.getElementById('statusConsole');
      const time = new Date().toLocaleTimeString();
      const style = isErr ? 'style="color: var(--danger)"' : '';
      consoleEl.innerHTML += `<div ${style}>[${time}] ${msg}</div>`;
      consoleEl.scrollTop = consoleEl.scrollHeight;
    }

    async function triggerEndpoint(url) {
      try {
        const response = await fetch(url);
        const result = await response.json();
        logMsg(result.message);
        
        if (url.includes('/test/')) {
          const modeToken = url.split('/').pop();
          updateActiveModeButtons(modeToken);
        }
        updateDashboard();
      } catch (err) {
        logMsg("Failed to trigger: " + url, true);
      }
    }

    function updateActiveModeButtons(activeMode) {
      const btns = {
        'auto': 'btn-auto',
        'high': 'btn-high',
        'medium': 'btn-medium',
        'low': 'btn-low',
        'error': 'btn-error'
      };
      for (const [key, id] of Object.entries(btns)) {
        const btn = document.getElementById(id);
        if (btn) {
          if (key === activeMode) {
            btn.classList.add('btn-active');
          } else {
            btn.classList.remove('btn-active');
          }
        }
      }
    }

    async function saveSettings(e) {
      e.preventDefault();
      const ssid = document.getElementById('inputSSID').value;
      const pass = document.getElementById('inputPass').value;
      const high = parseFloat(document.getElementById('inputHigh').value);
      const low = parseFloat(document.getElementById('inputLow').value);
      
      if (high >= low) {
        logMsg("Validation error: High threshold must be less than Low threshold.", true);
        return;
      }
      
      const url = `/save?ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}&high=${high}&low=${low}`;
      try {
        logMsg("Saving configurations and staging reboot...");
        const response = await fetch(url);
        const result = await response.json();
        
        if (response.ok) {
          logMsg(result.message);
          logMsg("ESP32 is rebooting. Switch network or reconnect to AP!");
        } else {
          logMsg("Save error: " + result.message, true);
        }
      } catch (err) {
        logMsg("Error during save API request.", true);
      }
    }

    async function loadSettings() {
      try {
        const response = await fetch('/settings');
        const data = await response.json();
        document.getElementById('inputSSID').value = data.ssid || '';
        document.getElementById('inputPass').value = data.pass || '';
        document.getElementById('inputHigh').value = data.high;
        document.getElementById('inputLow').value = data.low;
        logMsg("Configurations loaded from memory.");
      } catch (err) {
        logMsg("Failed to prefill settings values.", true);
      }
    }

    async function updateDashboard() {
      try {
        const response = await fetch('/status');
        const data = await response.json();
        
        // Update labels
        const hasValidDistance = data.distance >= 0;
        document.getElementById('distVal').innerText = hasValidDistance ? data.distance.toFixed(1) + ' cm' : 'SENSOR ERROR';
        document.getElementById('pctVal').innerText = data.percentage >= 0 ? data.percentage.toFixed(1) + ' %' : 'N/A';
        
        // Update tank graphic
        const pct = Math.max(0, Math.min(100, data.percentage));
        const waterBar = document.getElementById('waterLevelBar');
        const tankText = document.getElementById('tankPercentText');
        
        if (data.percentage >= 0) {
          waterBar.style.height = pct + '%';
          tankText.innerText = pct.toFixed(0) + '%';
        } else {
          waterBar.style.height = '0%';
          tankText.innerText = 'ERR';
        }
        
        // Update badges
        const stateEl = document.getElementById('stateBadge');
        stateEl.innerText = data.state;
        stateEl.className = 'badge badge-' + data.state.toLowerCase();
        
        document.getElementById('thresholds').innerText = `High: ${data.high_threshold} cm | Low: ${data.low_threshold} cm`;
        
        // Network metrics
        document.getElementById('wifiStatus').innerText = data.wifi_ssid + " (" + (data.wifi_mode.includes("STA") ? "STA" : "AP") + ")";
        document.getElementById('ipAddress').innerText = data.wifi_ip;
        document.getElementById('systemMode').innerText = data.system_mode;
        
        // Uptime Formatter
        const ms = data.uptime;
        let sec = Math.floor(ms / 1000);
        let min = Math.floor(sec / 60);
        let hr = Math.floor(min / 60);
        sec %= 60;
        min %= 60;
        document.getElementById('uptime').innerText = `${hr}h ${min}m ${sec}s`;
        
        // Hardware Indicators
        updateLEDIndicator('ledG', 'green', data.led_green);
        updateLEDIndicator('ledY', 'yellow', data.led_yellow);
        updateLEDIndicator('ledR', 'red', data.led_red);
        
        document.getElementById('buzzerStatus').innerText = data.buzzer_mode;
        document.getElementById('lastUpdate').innerText = new Date().toLocaleTimeString();
        
        // Ensure active simulation mode buttons are styled correctly
        const activeToken = data.system_mode.replace("TEST ", "").toLowerCase();
        updateActiveModeButtons(activeToken);
        
      } catch (err) {
        // Suppress warning console noise during connection drops/reboots
      }
    }

    function updateLEDIndicator(id, color, state) {
      const el = document.getElementById(id);
      if (state) {
        el.className = `led-dot active-${color}`;
      } else {
        el.className = 'led-dot';
      }
    }

    window.onload = () => {
      loadSettings();
      updateDashboard();
      setInterval(updateDashboard, 1000);
    };
  </script>
</body>
</html>
)rawhtml";

// ==========================================
// Sensor
// ==========================================
float readDistanceCM() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  
  // Measure the Echo high pulse duration. Timeout in 30ms (approx. 515cm max distance)
  unsigned long duration = pulseIn(PIN_ECHO, HIGH, 30000);
  
  if (duration == 0) {
    // Timeout or echo line disconnected
    return -1.0;
  }
  
  float distance = (duration * 0.0343) / 2.0;
  
  // Basic sensor validity constraints for HC-SR04
  if (distance < 2.0 || distance > 400.0) {
    return -1.0;
  }
  
  return distance;
}

// ==========================================
// LEDs
// ==========================================
void initLEDs() {
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_RED, LOW);
}

void triggerLEDTest() {
  currentLEDMode = LED_TEST;
  ledTestStartTime = millis();
  Serial.println("LED hardware test diagnostic initiated.");
}

void updateLEDs() {
  unsigned long now = millis();
  
  if (currentLEDMode == LED_TEST) {
    unsigned long elapsed = now - ledTestStartTime;
    if (elapsed < 1000) {
      digitalWrite(PIN_LED_GREEN, HIGH);
      digitalWrite(PIN_LED_YELLOW, LOW);
      digitalWrite(PIN_LED_RED, LOW);
    } else if (elapsed < 2000) {
      digitalWrite(PIN_LED_GREEN, LOW);
      digitalWrite(PIN_LED_YELLOW, HIGH);
      digitalWrite(PIN_LED_RED, LOW);
    } else if (elapsed < 3000) {
      digitalWrite(PIN_LED_GREEN, LOW);
      digitalWrite(PIN_LED_YELLOW, LOW);
      digitalWrite(PIN_LED_RED, HIGH);
    } else {
      // Diagnostic complete, restore default control state
      digitalWrite(PIN_LED_RED, LOW);
      currentLEDMode = LED_NORMAL;
      Serial.println("LED hardware test diagnostic completed.");
    }
    return;
  }
  
  // Normal state transitions matching the water level
  if (currentWaterState == STATE_HIGH) {
    digitalWrite(PIN_LED_GREEN, HIGH);
    digitalWrite(PIN_LED_YELLOW, LOW);
    digitalWrite(PIN_LED_RED, LOW);
  } 
  else if (currentWaterState == STATE_MEDIUM) {
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_YELLOW, HIGH);
    digitalWrite(PIN_LED_RED, LOW);
  } 
  else if (currentWaterState == STATE_LOW) {
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_YELLOW, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
  } 
  else if (currentWaterState == STATE_ERROR) {
    // All LEDs blink simultaneously at 500ms intervals
    if (now - lastLEDBlinkTime >= 500) {
      ledBlinkState = !ledBlinkState;
      lastLEDBlinkTime = now;
    }
    digitalWrite(PIN_LED_GREEN, ledBlinkState ? HIGH : LOW);
    digitalWrite(PIN_LED_YELLOW, ledBlinkState ? HIGH : LOW);
    digitalWrite(PIN_LED_RED, ledBlinkState ? HIGH : LOW);
  }
}

// ==========================================
// Buzzer
// ==========================================
void initBuzzer() {
  // ESP32 Arduino Core 3.3.x does not require ledcSetup/ledcAttachPin.
  // We rely on the native tone() API, which handles LEDC channel routing dynamically.
  pinMode(PIN_BUZZER, OUTPUT);
  noTone(PIN_BUZZER);
}

void triggerStateBuzzer(WaterState state, bool force) {
  // Prevent overriding the active manual diagnostic test mode unless forced
  if (currentBuzzerMode == BUZZ_TEST && !force) {
    return;
  }
  
  if (state == STATE_HIGH) {
    currentBuzzerMode = BUZZ_SUCCESS;
    buzzerStep = 0;
    buzzerPlayCount = 0;
    lastBuzzerActionTime = millis();
    Serial.println("Buzzer: Scheduled SUCCESS melody (x3 times).");
  } 
  else if (state == STATE_LOW) {
    currentBuzzerMode = BUZZ_AMBULANCE;
    buzzerStep = 0;
    buzzerPlayCount = 0;
    lastBuzzerActionTime = millis();
    Serial.println("Buzzer: Scheduled AMBULANCE siren (x3 times).");
  } 
  else if (state == STATE_ERROR) {
    currentBuzzerMode = BUZZ_ERROR;
    buzzerStep = 0;
    lastBuzzerActionTime = millis();
    Serial.println("Buzzer: Scheduled continuous ERROR alarm.");
  } 
  else {
    currentBuzzerMode = BUZZ_NONE;
    noTone(PIN_BUZZER);
  }
}

void triggerBuzzerTest() {
  currentBuzzerMode = BUZZ_TEST;
  buzzerTestStep = 0;
  buzzerStep = 0;
  lastBuzzerActionTime = millis();
  Serial.println("Buzzer: Hardware test diagnostic initiated.");
}

void updateBuzzer() {
  unsigned long now = millis();
  
  if (currentBuzzerMode == BUZZ_NONE) {
    noTone(PIN_BUZZER);
    return;
  }
  
  int note = 0;
  
  if (currentBuzzerMode == BUZZ_SUCCESS) {
    // 4-Note Success Melody: C5 -> Gap -> E5 -> Gap -> G5 -> Gap -> C6 -> Gap
    const int notes[] = {523, 0, 659, 0, 784, 0, 1047, 0};
    const int durations[] = {150, 30, 150, 30, 150, 30, 250, 100};
    const int totalSteps = 8;
    
    if (now - lastBuzzerActionTime >= (unsigned long)durations[buzzerStep]) {
      buzzerStep++;
      lastBuzzerActionTime = now;
      if (buzzerStep >= totalSteps) {
        buzzerStep = 0;
        buzzerPlayCount++;
        if (buzzerPlayCount >= 3) {
          currentBuzzerMode = BUZZ_NONE;
          noTone(PIN_BUZZER);
          Serial.println("Buzzer: SUCCESS melody play count completed.");
          return;
        }
      }
    }
    note = notes[buzzerStep];
  } 
  else if (currentBuzzerMode == BUZZ_AMBULANCE) {
    // Ambulance Siren: Alternates high and low frequencies (800Hz / 600Hz)
    const int notes[] = {800, 600};
    const int durations[] = {500, 500};
    const int totalSteps = 2;
    
    if (now - lastBuzzerActionTime >= (unsigned long)durations[buzzerStep]) {
      buzzerStep++;
      lastBuzzerActionTime = now;
      if (buzzerStep >= totalSteps) {
        buzzerStep = 0;
        buzzerPlayCount++;
        if (buzzerPlayCount >= 3) {
          currentBuzzerMode = BUZZ_NONE;
          noTone(PIN_BUZZER);
          Serial.println("Buzzer: AMBULANCE siren play count completed.");
          return;
        }
      }
    }
    note = notes[buzzerStep];
  } 
  else if (currentBuzzerMode == BUZZ_ERROR) {
    // Rapid error beeping: 1000Hz (150ms) -> Silent (150ms)
    const int notes[] = {1000, 0};
    const int durations[] = {150, 150};
    const int totalSteps = 2;
    
    if (now - lastBuzzerActionTime >= (unsigned long)durations[buzzerStep]) {
      buzzerStep++;
      lastBuzzerActionTime = now;
      if (buzzerStep >= totalSteps) {
        buzzerStep = 0;
      }
    }
    note = notes[buzzerStep];
  } 
  else if (currentBuzzerMode == BUZZ_TEST) {
    // Test Sequence execution: Success (1 time) -> Ambulance (1 time) -> Error alarm (3 seconds)
    if (buzzerTestStep == 0) {
      const int notes[] = {523, 0, 659, 0, 784, 0, 1047, 0};
      const int durations[] = {150, 30, 150, 30, 150, 30, 250, 100};
      const int totalSteps = 8;
      
      if (now - lastBuzzerActionTime >= (unsigned long)durations[buzzerStep]) {
        buzzerStep++;
        lastBuzzerActionTime = now;
        if (buzzerStep >= totalSteps) {
          buzzerStep = 0;
          buzzerTestStep = 1; // Move to Ambulance Siren
          Serial.println("Buzzer Test: Completed Success. Starting Ambulance...");
        }
      }
      note = notes[buzzerStep];
    } 
    else if (buzzerTestStep == 1) {
      const int notes[] = {800, 600};
      const int durations[] = {500, 500};
      const int totalSteps = 2;
      
      if (now - lastBuzzerActionTime >= (unsigned long)durations[buzzerStep]) {
        buzzerStep++;
        lastBuzzerActionTime = now;
        if (buzzerStep >= totalSteps) {
          buzzerStep = 0;
          buzzerTestStep = 2; // Move to Error Beeping
          buzzerTestStartTime = now;
          Serial.println("Buzzer Test: Completed Ambulance. Starting Error Beeps...");
        }
      }
      note = notes[buzzerStep];
    } 
    else if (buzzerTestStep == 2) {
      const int notes[] = {1000, 0};
      const int durations[] = {150, 150};
      const int totalSteps = 2;
      
      if (now - lastBuzzerActionTime >= (unsigned long)durations[buzzerStep]) {
        buzzerStep++;
        lastBuzzerActionTime = now;
        if (buzzerStep >= totalSteps) {
          buzzerStep = 0;
        }
      }
      
      // Stop the test and return to normal mode after 3 seconds
      if (now - buzzerTestStartTime >= 3000) {
        currentBuzzerMode = BUZZ_NONE;
        noTone(PIN_BUZZER);
        Serial.println("Buzzer: Hardware test diagnostic completed. Restoring state...");
        triggerStateBuzzer(currentWaterState, true);
        return;
      }
      note = notes[buzzerStep];
    }
  }
  
  if (note > 0) {
    tone(PIN_BUZZER, note);
  } else {
    noTone(PIN_BUZZER);
  }
}

// ==========================================
// States & Transitions
// ==========================================
const char* getStateString(WaterState state) {
  switch (state) {
    case STATE_HIGH: return "HIGH";
    case STATE_MEDIUM: return "MEDIUM";
    case STATE_LOW: return "LOW";
    case STATE_ERROR: return "ERROR";
  }
  return "UNKNOWN";
}

void updateSystemState() {
  WaterState newState;
  
  if (systemMode == MODE_AUTO) {
    // Transition to error if sensor registers 3 consecutive fail readings
    if (consecutiveErrorCount >= 3) {
      newState = STATE_ERROR;
    } else {
      if (currentDistance <= thresholdHigh) {
        newState = STATE_HIGH;
      } else if (currentDistance >= thresholdLow) {
        newState = STATE_LOW;
      } else {
        newState = STATE_MEDIUM;
      }
    }
  } else {
    // Mode is manually overridden via API Simulation commands
    switch (systemMode) {
      case MODE_TEST_HIGH:   newState = STATE_HIGH;   break;
      case MODE_TEST_MEDIUM: newState = STATE_MEDIUM; break;
      case MODE_TEST_LOW:    newState = STATE_LOW;    break;
      case MODE_TEST_ERROR:  newState = STATE_ERROR;  break;
      default:               newState = STATE_MEDIUM; break;
    }
  }
  
  // Transition Action trigger
  if (newState != currentWaterState) {
    previousWaterState = currentWaterState;
    currentWaterState = newState;
    
    Serial.print("System state changed: ");
    Serial.println(getStateString(currentWaterState));
    
    // Automatically trigger state-specific sound alarms/melodies
    triggerStateBuzzer(currentWaterState, false);
  }
}

// ==========================================
// WiFi
// ==========================================
void startAP() {
  WiFi.mode(WIFI_AP);
  
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  
  WiFi.softAP(AP_SSID, AP_PASS);
  wifiModeSTA = false;
  
  Serial.println("WiFi: Initiated Access Point (AP) mode.");
  Serial.print("SSID  : "); Serial.println(AP_SSID);
  Serial.print("IP Address: "); Serial.println(WiFi.softAPIP());
}

void setupWiFi() {
  if (configSSID.length() == 0) {
    Serial.println("WiFi: No saved network configurations. Falling back to AP...");
    startAP();
    return;
  }
  
  Serial.print("WiFi: Attempting to connect to Station (STA): ");
  Serial.println(configSSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(configSSID.c_str(), configPass.c_str());
  
  unsigned long startAttemptTime = millis();
  const unsigned long wifiTimeout = 15000; // 15-second connection deadline
  
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTimeout) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiModeSTA = true;
    Serial.print("WiFi: Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi: Connection timeout. Staging fail-safe Access Point...");
    startAP();
  }
}

// ==========================================
// Server Routes
// ==========================================
void handleRoot() {
  server.send(200, "text/html", HTML_PAGE);
}

void handleStatus() {
  String json;
  json.reserve(512);
  
  String stateStr = getStateString(currentWaterState);
  
  String buzzStr;
  switch (currentBuzzerMode) {
    case BUZZ_NONE:      buzzStr = "NONE";      break;
    case BUZZ_SUCCESS:   buzzStr = "SUCCESS";   break;
    case BUZZ_AMBULANCE: buzzStr = "AMBULANCE"; break;
    case BUZZ_ERROR:     buzzStr = "ERROR";     break;
    case BUZZ_TEST:      buzzStr = "TEST";      break;
  }
  
  String modeStr;
  switch (systemMode) {
    case MODE_AUTO:        modeStr = "AUTO";        break;
    case MODE_TEST_HIGH:   modeStr = "TEST HIGH";   break;
    case MODE_TEST_MEDIUM: modeStr = "TEST MEDIUM"; break;
    case MODE_TEST_LOW:    modeStr = "TEST LOW";    break;
    case MODE_TEST_ERROR:  modeStr = "TEST ERROR";  break;
  }
  
  String wifiSsid = wifiModeSTA ? configSSID : String(AP_SSID);
  String wifiIp = wifiModeSTA ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String wifiModeStr = wifiModeSTA ? "STA" : "AP";
  
  json += "{";
  json += "\"distance\":" + String(currentDistance, 2) + ",";
  json += "\"percentage\":" + String(currentPercentage, 1) + ",";
  json += "\"state\":\"" + stateStr + "\",";
  json += "\"high_threshold\":" + String(thresholdHigh, 2) + ",";
  json += "\"low_threshold\":" + String(thresholdLow, 2) + ",";
  json += "\"wifi_ssid\":\"" + wifiSsid + "\",";
  json += "\"wifi_ip\":\"" + wifiIp + "\",";
  json += "\"wifi_mode\":\"" + wifiModeStr + "\",";
  json += "\"led_green\":" + String(digitalRead(PIN_LED_GREEN)) + ",";
  json += "\"led_yellow\":" + String(digitalRead(PIN_LED_YELLOW)) + ",";
  json += "\"led_red\":" + String(digitalRead(PIN_LED_RED)) + ",";
  json += "\"buzzer_mode\":\"" + buzzStr + "\",";
  json += "\"system_mode\":\"" + modeStr + "\",";
  json += "\"uptime\":" + String(millis()) + "";
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleSettings() {
  String json;
  json.reserve(256);
  json += "{";
  json += "\"ssid\":\"" + configSSID + "\",";
  json += "\"pass\":\"" + configPass + "\",";
  json += "\"high\":" + String(thresholdHigh, 2) + ",";
  json += "\"low\":" + String(thresholdLow, 2) + "";
  json += "}";
  server.send(200, "application/json", json);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("pass") && server.hasArg("high") && server.hasArg("low")) {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");
    float newHigh = server.arg("high").toFloat();
    float newLow = server.arg("low").toFloat();
    
    // Bounds & logic validation checks
    if (newHigh <= 0 || newLow <= 0) {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Thresholds must be positive values.\"}");
      return;
    }
    if (newHigh >= newLow) {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"High threshold must be less than Low threshold.\"}");
      return;
    }
    
    saveConfigurations(newSsid, newPass, newHigh, newLow);
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Configurations saved. Restarting...\"}");
    
    restartRequested = true;
    restartTimer = millis();
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing parameters.\"}");
  }
}

// REST endpoints for simulated states
void handleTestAuto() {
  systemMode = MODE_AUTO;
  consecutiveErrorCount = 0;
  updateSystemState();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"System switched to AUTOMATIC sensor tracking.\"}");
}

void handleTestHigh() {
  systemMode = MODE_TEST_HIGH;
  updateSystemState();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Simulation: System set to HIGH Water state.\"}");
}

void handleTestMedium() {
  systemMode = MODE_TEST_MEDIUM;
  updateSystemState();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Simulation: System set to MEDIUM Water state.\"}");
}

void handleTestLow() {
  systemMode = MODE_TEST_LOW;
  updateSystemState();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Simulation: System set to LOW Water state.\"}");
}

void handleTestError() {
  systemMode = MODE_TEST_ERROR;
  updateSystemState();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Simulation: System set to SENSOR ERROR state.\"}");
}

// REST endpoints for hardware testing
void handleTestLed() {
  triggerLEDTest();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Diagnostic: LED testing sequence started.\"}");
}

void handleTestBuzzer() {
  triggerBuzzerTest();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Diagnostic: Buzzer melody sequence started.\"}");
}

void handleRestart() {
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Rebooting hardware...\"}");
  restartRequested = true;
  restartTimer = millis();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/save", HTTP_GET, handleSave); // Allow simple GET request save submissions
  
  // Test/Simulation Endpoints
  server.on("/test/auto", HTTP_GET, handleTestAuto);
  server.on("/test/high", HTTP_GET, handleTestHigh);
  server.on("/test/medium", HTTP_GET, handleTestMedium);
  server.on("/test/low", HTTP_GET, handleTestLow);
  server.on("/test/error", HTTP_GET, handleTestError);
  
  // Diagnostic Endpoints
  server.on("/test/led", HTTP_GET, handleTestLed);
  server.on("/test/buzzer", HTTP_GET, handleTestBuzzer);
  server.on("/restart", HTTP_GET, handleRestart);
  
  server.begin();
  Serial.println("HTTP: Web server initiated.");
}

// ==========================================
// Setup
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Initiating Water Tank IoT Monitor ---");
  
  // Initialize Hardware
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  
  initLEDs();
  initBuzzer();
  
  // Load settings & start networking
  loadConfigurations();
  setupWiFi();
  setupWebServer();
  
  Serial.println("System operational. Beginning live loops.");
}

// ==========================================
// Loop
// ==========================================
void loop() {
  unsigned long now = millis();
  
  // Handle HTTP Server clients
  server.handleClient();
  
  // Non-blocking background hardware updates
  updateLEDs();
  updateBuzzer();
  
  // Periodic non-blocking sensor readings
  if (now - lastSensorReadTime >= SENSOR_READ_INTERVAL) {
    lastSensorReadTime = now;
    
    if (systemMode == MODE_AUTO) {
      float dist = readDistanceCM();
      
      if (dist < 0) {
        // Read fail
        consecutiveErrorCount++;
        Serial.print("Sensor error count: ");
        Serial.println(consecutiveErrorCount);
        
        if (consecutiveErrorCount >= 3) {
          currentDistance = -1.0;
          currentPercentage = -1.0;
        }
      } else {
        // Successful reading
        consecutiveErrorCount = 0;
        currentDistance = dist;
        
        // Calculate fill percentage based on height limits
        // Distance is measured from top: smaller distance = fuller tank.
        // Clamp distance within thresholds
        float clampedDist = constrain(currentDistance, thresholdHigh, thresholdLow);
        float totalRange = thresholdLow - thresholdHigh;
        
        if (totalRange > 0) {
          currentPercentage = ((thresholdLow - clampedDist) / totalRange) * 100.0;
        } else {
          currentPercentage = 0.0;
        }
      }
    } else {
      // In simulation mode, ignore physical sensor values
      consecutiveErrorCount = 0;
      
      // Simulate level parameters depending on simulated state
      switch (systemMode) {
        case MODE_TEST_HIGH:
          currentDistance = thresholdHigh;
          currentPercentage = 100.0;
          break;
        case MODE_TEST_MEDIUM:
          currentDistance = (thresholdHigh + thresholdLow) / 2.0;
          currentPercentage = 50.0;
          break;
        case MODE_TEST_LOW:
          currentDistance = thresholdLow;
          currentPercentage = 0.0;
          break;
        case MODE_TEST_ERROR:
          currentDistance = -1.0;
          currentPercentage = -1.0;
          break;
        default:
          break;
      }
    }
    
    // Evaluate system water state and alert levels
    updateSystemState();
  }
  
  // Handle scheduled software reboot
  if (restartRequested && (now - restartTimer >= 2000)) {
    Serial.println("Rebooting ESP32 hardware now...");
    ESP.restart();
  }
}
