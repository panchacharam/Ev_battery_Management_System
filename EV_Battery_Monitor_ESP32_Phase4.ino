// ============================================================
// EV Battery Monitoring System — Phase 4
// ESP32: UART receiver (unchanged logic from Phase 3)
//        + Wi-Fi + HTTP web server + live dashboard
//
// Architecture:
//   Sensors -> Pico -> UART -> ESP32 -> Wi-Fi -> Web Dashboard
//
// Libraries required (both bundled with the ESP32 board package
// in Arduino IDE — Tools > Board > Boards Manager > "esp32" by
// Espressif Systems. No separate library install needed):
//   WiFi.h
//   WebServer.h
// ============================================================

#include <WiFi.h>
#include <WebServer.h>

// ============================================================
// WI-FI CREDENTIALS — EDIT THESE
// ============================================================

const char* ssid     = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

// ============================================================
// UART CONFIG (unchanged from Phase 3)
// ============================================================

#define RXD2 16
#define TXD2 17
#define UART_BAUD 115200
#define EXPECTED_FIELDS 12   // "DATA" + 11 data fields

String rxBuffer = "";

// ============================================================
// WEB SERVER
// ============================================================

WebServer server(80);

// ============================================================
// LATEST SENSOR VALUES (shared between UART parser and web server)
// ============================================================

float g_batteryVoltage    = 0;
float g_batteryPercentage = 0;
float g_current           = 0;
float g_temperature       = 0;
float g_humidity          = 0;
float g_ax                = 0;
float g_ay                = 0;
float g_az                = 0;
String g_activity         = "UNKNOWN";
String g_batteryStatus    = "UNKNOWN";
int   g_overallFault      = 0;

unsigned long lastPacketTime = 0;
unsigned long lastHeartbeat  = 0;
const unsigned long HEARTBEAT_INTERVAL_MS = 5000;

unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000;


// ============================================================
// DASHBOARD HTML/CSS/JS (served at "/")
// ============================================================

const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EV Battery Monitoring Dashboard</title>
<style>
  :root {
    --normal: #2ecc71;
    --low: #f1c40f;
    --critical: #e74c3c;
    --disconnected: #95a5a6;
    --bg: #0f1419;
    --card-bg: #1b222c;
    --text: #e8edf2;
    --muted: #8aa0b4;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    font-family: -apple-system, Segoe UI, Roboto, Arial, sans-serif;
    background: var(--bg);
    color: var(--text);
    padding: 16px;
  }
  h1 {
    font-size: 1.3rem;
    margin: 0 0 4px 0;
  }
  #last-update {
    color: var(--muted);
    font-size: 0.85rem;
    margin-bottom: 16px;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
    gap: 12px;
  }
  .card {
    background: var(--card-bg);
    border-radius: 12px;
    padding: 14px;
    box-shadow: 0 2px 6px rgba(0,0,0,0.3);
  }
  .card .label {
    font-size: 0.78rem;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .card .value {
    font-size: 1.5rem;
    font-weight: 600;
    margin-top: 4px;
  }
  .status-badge {
    display: inline-block;
    padding: 4px 10px;
    border-radius: 999px;
    font-weight: 600;
    font-size: 0.95rem;
  }
  .status-normal        { background: var(--normal); color: #06240f; }
  .status-low           { background: var(--low); color: #332700; }
  .status-critical       { background: var(--critical); color: #fff; }
  .status-disconnected   { background: var(--disconnected); color: #fff; }
  .status-fault {
    background: var(--critical);
    color: #fff;
    animation: blink 1s infinite;
  }
  @keyframes blink {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.4; }
  }
  .wide { grid-column: 1 / -1; }
</style>
</head>
<body>

  <h1>EV Battery Monitoring Dashboard</h1>
  <div id="last-update">Connecting...</div>

  <div class="grid">

    <div class="card">
      <div class="label">Battery Voltage</div>
      <div class="value" id="battery-voltage">--</div>
    </div>

    <div class="card">
      <div class="label">Battery Percentage</div>
      <div class="value" id="battery-percentage">--</div>
    </div>

    <div class="card">
      <div class="label">Current</div>
      <div class="value" id="current">--</div>
    </div>

    <div class="card">
      <div class="label">Temperature</div>
      <div class="value" id="temperature">--</div>
    </div>

    <div class="card">
      <div class="label">Humidity</div>
      <div class="value" id="humidity">--</div>
    </div>

    <div class="card">
      <div class="label">Accelerometer</div>
      <div class="value" id="accel" style="font-size:1.1rem;">--</div>
    </div>

    <div class="card">
      <div class="label">Activity</div>
      <div class="value" id="activity">--</div>
    </div>

    <div class="card">
      <div class="label">Battery Status</div>
      <div class="value"><span class="status-badge" id="battery-status">--</span></div>
    </div>

    <div class="card wide" id="fault-card">
      <div class="label">Overall Fault</div>
      <div class="value" id="fault-status">--</div>
    </div>

  </div>

<script>
  function statusClass(status) {
    switch (status) {
      case 'NORMAL': return 'status-normal';
      case 'LOW': return 'status-low';
      case 'CRITICAL': return 'status-critical';
      case 'DISCONNECTED': return 'status-disconnected';
      default: return '';
    }
  }

  async function updateDashboard() {
    try {
      const res = await fetch('/data');
      const data = await res.json();

      document.getElementById('battery-voltage').innerText = data.battery_voltage.toFixed(2) + ' V';
      document.getElementById('battery-percentage').innerText = data.battery_percentage.toFixed(1) + ' %';
      document.getElementById('current').innerText = data.current.toFixed(3) + ' A';
      document.getElementById('temperature').innerText = data.temperature.toFixed(0) + ' C';
      document.getElementById('humidity').innerText = data.humidity.toFixed(0) + ' %';
      document.getElementById('accel').innerText =
        'AX:' + data.ax.toFixed(2) + '  AY:' + data.ay.toFixed(2) + '  AZ:' + data.az.toFixed(2);
      document.getElementById('activity').innerText = data.activity;

      const battEl = document.getElementById('battery-status');
      battEl.innerText = data.battery_status;
      battEl.className = 'status-badge ' + statusClass(data.battery_status);

      const faultCard = document.getElementById('fault-card');
      const faultText = document.getElementById('fault-status');
      if (data.overall_fault == 1) {
        faultText.innerText = 'FAULT ACTIVE';
        faultCard.className = 'card wide status-fault';
      } else {
        faultText.innerText = 'No active faults';
        faultCard.className = 'card wide status-normal';
      }

      const ageSec = Math.round(data.data_age_ms / 1000);
      const lu = document.getElementById('last-update');
      if (data.data_age_ms < 5000) {
        lu.innerText = 'Live — last packet ' + ageSec + 's ago';
        lu.style.color = '#2ecc71';
      } else {
        lu.innerText = 'No data from Pico for ' + ageSec + 's — check UART wiring';
        lu.style.color = '#e74c3c';
      }

    } catch (err) {
      document.getElementById('last-update').innerText = 'Connection error reaching ESP32';
      document.getElementById('last-update').style.color = '#e74c3c';
    }
  }

  window.onload = updateDashboard;
  setInterval(updateDashboard, 1000);
</script>

</body>
</html>
)rawliteral";


// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial2.begin(UART_BAUD, SERIAL_8N1, RXD2, TXD2);

  Serial.println();
  Serial.println("Wi-Fi connecting");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // One-time blocking wait during setup only — does not affect the
  // non-blocking UART + web server loop that runs afterward.
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - wifiStart > 20000) {
      Serial.println();
      Serial.println("Wi-Fi still not connected after 20s — check credentials/signal.");
      wifiStart = millis(); // keep trying, reset the warning timer
    }
  }

  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.begin();

  Serial.println("HTTP server started.");
  Serial.println("Open the IP address above in a browser on the same Wi-Fi network.");

  lastPacketTime = millis();
  lastHeartbeat = millis();
  lastWifiCheck = millis();
}


// ============================================================
// MAIN LOOP — everything here is non-blocking
// ============================================================

void loop() {

  // ---- 1 & 2 & 3: Receive + parse UART, store latest values ----
  while (Serial2.available()) {
    char c = Serial2.read();

    if (c == '\n') {
      processPacket(rxBuffer);
      rxBuffer = "";
    } else if (c != '\r') {
      rxBuffer += c;
      if (rxBuffer.length() > 200) {
        rxBuffer = "";
      }
    }
  }

  // ---- 5 & 6: Serve webpage + data endpoint ----
  server.handleClient();

  // ---- 4: Maintain Wi-Fi connection (non-blocking check) ----
  unsigned long now = millis();
  if (now - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi dropped — attempting reconnect...");
      WiFi.reconnect();
    }
  }

  // ---- Troubleshooting heartbeat: flag if no UART data arriving ----
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    if (now - lastPacketTime >= HEARTBEAT_INTERVAL_MS) {
      Serial.println("No data received from Pico in the last 5 seconds...");
    }
  }
}


// ============================================================
// UART PACKET PARSER (same validation rules as Phase 3)
// ============================================================

void processPacket(String packet) {
  packet.trim();

  if (packet.length() == 0) {
    return;
  }

  if (!packet.startsWith("DATA,")) {
    Serial.println("Invalid packet");
    return;
  }

  String fields[EXPECTED_FIELDS];
  int fieldCount = 0;
  int startIdx = 0;

  while (fieldCount < EXPECTED_FIELDS) {
    int commaIdx = packet.indexOf(',', startIdx);
    if (commaIdx == -1) {
      fields[fieldCount++] = packet.substring(startIdx);
      break;
    } else {
      fields[fieldCount++] = packet.substring(startIdx, commaIdx);
      startIdx = commaIdx + 1;
    }
  }

  if (fieldCount != EXPECTED_FIELDS) {
    Serial.println("Invalid packet");
    return;
  }

  for (int i = 0; i < EXPECTED_FIELDS; i++) {
    if (fields[i].length() == 0) {
      Serial.println("Invalid packet");
      return;
    }
  }

  if (fields[0] != "DATA") {
    Serial.println("Invalid packet");
    return;
  }

  // ---- Store into the shared globals the web server reads from ----
  g_batteryVoltage    = fields[1].toFloat();
  g_batteryPercentage = fields[2].toFloat();
  g_current           = fields[3].toFloat();
  g_temperature       = fields[4].toFloat();
  g_humidity          = fields[5].toFloat();
  g_ax                = fields[6].toFloat();
  g_ay                = fields[7].toFloat();
  g_az                = fields[8].toFloat();
  g_activity          = fields[9];
  g_batteryStatus     = fields[10];
  g_overallFault      = fields[11].toInt();

  lastPacketTime = millis();

  Serial.println("========== EV BATTERY DATA ==========");
  Serial.print("Battery Voltage : "); Serial.print(g_batteryVoltage, 2); Serial.println(" V");
  Serial.print("Battery          : "); Serial.print(g_batteryPercentage, 1); Serial.println(" %");
  Serial.print("Current          : "); Serial.print(g_current, 3); Serial.println(" A");
  Serial.print("Temperature      : "); Serial.print(g_temperature, 0); Serial.println(" C");
  Serial.print("Humidity         : "); Serial.print(g_humidity, 0); Serial.println(" %");
  Serial.print("AX               : "); Serial.println(g_ax, 2);
  Serial.print("AY               : "); Serial.println(g_ay, 2);
  Serial.print("AZ               : "); Serial.println(g_az, 2);
  Serial.print("Activity         : "); Serial.println(g_activity);
  Serial.print("Battery Status   : "); Serial.println(g_batteryStatus);
  Serial.print("Overall Fault    : "); Serial.println(g_overallFault == 1 ? "YES" : "NO");
  Serial.println("======================================");
}


// ============================================================
// WEB HANDLERS
// ============================================================

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleData() {
  unsigned long dataAge = millis() - lastPacketTime;

  String json = "{";
  json += "\"battery_voltage\":" + String(g_batteryVoltage, 2) + ",";
  json += "\"battery_percentage\":" + String(g_batteryPercentage, 1) + ",";
  json += "\"current\":" + String(g_current, 3) + ",";
  json += "\"temperature\":" + String(g_temperature, 0) + ",";
  json += "\"humidity\":" + String(g_humidity, 0) + ",";
  json += "\"ax\":" + String(g_ax, 2) + ",";
  json += "\"ay\":" + String(g_ay, 2) + ",";
  json += "\"az\":" + String(g_az, 2) + ",";
  json += "\"activity\":\"" + g_activity + "\",";
  json += "\"battery_status\":\"" + g_batteryStatus + "\",";
  json += "\"overall_fault\":" + String(g_overallFault) + ",";
  json += "\"data_age_ms\":" + String(dataAge);
  json += "}";

  server.send(200, "application/json", json);
}
