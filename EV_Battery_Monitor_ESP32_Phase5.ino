// ============================================================
// EV Battery Monitoring System — Phase 5
// ESP32: UART receiver + Wi-Fi + HTTP web server + live dashboard
//        + CSV data logging to LittleFS (NEW in Phase 5)
//
// Architecture:
//   Sensors -> Pico -> UART -> ESP32 -> Wi-Fi -> Web Dashboard
//                                  \-> LittleFS /battery_data.csv
//
// Phase 5 additions (everything else is your Phase 4 code):
//   - LittleFS mount (never formats automatically)
//   - /battery_data.csv with a header written only for a new file
//   - one validated row about every 5 s, timestamped with millis()
//   - /download route + "Download CSV Dataset" button
//   - "Data Logging" card on the dashboard (status, rows, free space)
//
// Libraries (all bundled with the ESP32 board package, no installs):
//   WiFi.h, WebServer.h, FS.h, LittleFS.h
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>

// ============================================================
// WI-FI CREDENTIALS — EDIT THESE
// ============================================================

const char* ssid     = "OPPOA785G";
const char* password = "bala@2005";

// ============================================================
// UART CONFIG (unchanged from Phase 3)
// ============================================================

#define RXD2 16
#define TXD2 17
#define UART_BAUD 115200
#define EXPECTED_FIELDS 12   // "DATA" + 11 data fields

String rxBuffer = "";

// ============================================================
// PHASE 5 — CSV LOGGING CONFIG
// ============================================================

#define CSV_PATH   "/battery_data.csv"
#define CSV_HEADER "timestamp_ms,battery_voltage_v,battery_percentage,current_a,temperature_c,humidity_pct,ax_g,ay_g,az_g,activity,battery_status,overall_fault"

const unsigned long LOG_INTERVAL_MS = 5000;   // one row about every 5 s
const size_t MIN_FREE_BYTES = 8192;           // stop logging below this much free space

// Plausibility limits used ONLY to decide whether a row is logged.
// (The live dashboard still shows whatever the Pico sent, as in Phase 4.)
const float LIM_V_MAX    = 30.0;
const float LIM_I_ABS    = 30.0;
const float LIM_T_MIN    = -40.0;
const float LIM_T_MAX    = 125.0;
const float LIM_ACC_ABS  = 16.0;

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
// PHASE 5 — LOGGING STATE
// ============================================================

bool     fsReady         = false;           // LittleFS mounted?
bool     headerOk        = false;           // CSV exists and has its header
String   logState        = "STORAGE ERROR"; // ACTIVE / STORAGE ERROR / FILE ERROR / WRITE ERROR / STORAGE FULL
uint32_t rowsInFile      = 0;               // rows currently in the CSV (persisted + new)
uint32_t rowsSession     = 0;               // rows written since this boot
uint32_t logErrors       = 0;               // failed write attempts since boot
size_t   fsFreeBytes     = 0;               // cached, refreshed on every log write
String   pendingRow      = "";              // newest validated row waiting to be written
bool     pendingRowReady = false;
unsigned long lastLogMs  = 0;


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
  .log-details {
    color: var(--muted);
    font-size: 0.9rem;
    margin-top: 8px;
  }
  .btn {
    display: inline-block;
    margin-top: 12px;
    padding: 10px 16px;
    border-radius: 8px;
    background: #3498db;
    color: #fff;
    font-weight: 600;
    text-decoration: none;
  }
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

    <div class="card wide" id="log-card">
      <div class="label">Data Logging</div>
      <div class="value"><span class="status-badge" id="log-status">--</span></div>
      <div class="log-details" id="log-details">--</div>
      <a class="btn" id="download-btn" href="/download" download="battery_data.csv">Download CSV Dataset</a>
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

      // ---- Phase 5: data logging card ----
      const logBadge = document.getElementById('log-status');
      logBadge.innerText = data.log_state;
      if (data.logging_active) {
        logBadge.className = 'status-badge status-normal';
      } else if (data.log_state === 'STORAGE FULL') {
        logBadge.className = 'status-badge status-low';
      } else {
        logBadge.className = 'status-badge status-critical';
      }
      document.getElementById('log-details').innerText = data.fs_ready
        ? data.rows_in_file + ' rows saved  |  ' + data.rows_session + ' this session  |  ' +
          data.fs_free_kb.toFixed(0) + ' KB free'
        : 'Storage unavailable';
      const dl = document.getElementById('download-btn');
      dl.style.opacity = data.fs_ready ? '1' : '0.4';
      dl.style.pointerEvents = data.fs_ready ? 'auto' : 'none';

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
// FORWARD DECLARATIONS
// ============================================================

void processPacket(String packet);
void handleRoot();
void handleData();
void handleDownload();
void initLogging();
bool ensureHeader();
void ensureTrailingNewline();
uint32_t countCsvRows();
bool strictFloat(const String &s, float &out);
bool safeToken(const String &s);
bool buildLogRow(const String fields[], unsigned long ts, String &rowOut);
void logPendingRow();


// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  // Larger RX buffer so no Pico packet is lost while the ESP32 is busy
  // streaming a CSV download. Must be set BEFORE begin().
  Serial2.setRxBufferSize(1024);
  Serial2.begin(UART_BAUD, SERIAL_8N1, RXD2, TXD2);

  // ---- Phase 5: mount LittleFS and prepare the CSV (never formats) ----
  Serial.println();
  initLogging();

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
  server.on("/download", HTTP_GET, handleDownload);   // Phase 5
  server.begin();

  Serial.println("HTTP server started.");
  Serial.println("Open the IP address above in a browser on the same Wi-Fi network.");

  lastPacketTime = millis();
  lastHeartbeat = millis();
  lastWifiCheck = millis();
  lastLogMs = millis() - LOG_INTERVAL_MS;   // first valid row may be logged right away
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

  unsigned long now = millis();

  // ---- Phase 5: write the newest validated row every LOG_INTERVAL_MS ----
  if (fsReady && pendingRowReady && (now - lastLogMs >= LOG_INTERVAL_MS)) {
    lastLogMs = now;
    logPendingRow();
  }

  // ---- 4: Maintain Wi-Fi connection (non-blocking check) ----
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
// UART PACKET PARSER (same validation rules as Phase 3 / 4)
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

  // ---- Phase 5: strict validation, then queue a CSV row ----
  // The dashboard above already shows the packet exactly as in Phase 4.
  // A row is queued for logging only if every field passes the strict checks.
  String row;
  if (buildLogRow(fields, lastPacketTime, row)) {
    pendingRow = row;
    pendingRowReady = true;
  } else {
    Serial.println("CSV: packet failed validation, not logged");
  }
}


// ============================================================
// PHASE 5 — VALIDATION
// ============================================================

// Accepts only plain decimal numbers. Unlike String::toFloat(), garbage
// such as "abc" is rejected instead of silently becoming 0.0.
bool strictFloat(const String &s, float &out) {
  size_t n = s.length();
  if (n == 0 || n > 15) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s.charAt(i);
    bool ok = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E';
    if (!ok) return false;
  }
  char *endPtr = nullptr;
  double v = strtod(s.c_str(), &endPtr);
  if (endPtr == s.c_str() || *endPtr != '\0') return false;
  if (v != v || v > 1.0e30 || v < -1.0e30) return false;   // NaN or overflow (e.g. 1e999)
  out = (float)v;
  return true;
}

// Text fields (activity / battery status): letters, digits, '_' and '-' only,
// so nothing can break the CSV layout.
bool safeToken(const String &s) {
  size_t n = s.length();
  if (n == 0 || n > 20) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s.charAt(i);
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// Builds one CSV line from the fields exactly as received from the Pico.
// The Pico sends -1 for temperature/humidity when the DHT11 read fails;
// that is "no reading", so those two cells are left empty instead of -1.
bool buildLogRow(const String fields[], unsigned long ts, String &rowOut) {
  float v, pct, cur, t, h, ax, ay, az;

  if (!strictFloat(fields[1], v)   || !strictFloat(fields[2], pct) ||
      !strictFloat(fields[3], cur) || !strictFloat(fields[4], t)   ||
      !strictFloat(fields[5], h)   || !strictFloat(fields[6], ax)  ||
      !strictFloat(fields[7], ay)  || !strictFloat(fields[8], az)) {
    return false;
  }

  if (!safeToken(fields[9]) || !safeToken(fields[10])) return false;
  if (!(fields[11] == "0" || fields[11] == "1")) return false;

  if (v < 0.0 || v > LIM_V_MAX) return false;
  if (pct < 0.0 || pct > 100.0) return false;
  if (cur < -LIM_I_ABS || cur > LIM_I_ABS) return false;
  if (ax < -LIM_ACC_ABS || ax > LIM_ACC_ABS) return false;
  if (ay < -LIM_ACC_ABS || ay > LIM_ACC_ABS) return false;
  if (az < -LIM_ACC_ABS || az > LIM_ACC_ABS) return false;

  bool tempMissing = (t == -1.0f);
  bool humMissing  = (h == -1.0f);
  if (!tempMissing && (t < LIM_T_MIN || t > LIM_T_MAX)) return false;
  if (!humMissing  && (h < 0.0 || h > 100.0)) return false;

  rowOut  = String(ts);
  rowOut += ",";  rowOut += fields[1];
  rowOut += ",";  rowOut += fields[2];
  rowOut += ",";  rowOut += fields[3];
  rowOut += ",";  rowOut += (tempMissing ? String("") : fields[4]);
  rowOut += ",";  rowOut += (humMissing  ? String("") : fields[5]);
  rowOut += ",";  rowOut += fields[6];
  rowOut += ",";  rowOut += fields[7];
  rowOut += ",";  rowOut += fields[8];
  rowOut += ",";  rowOut += fields[9];
  rowOut += ",";  rowOut += fields[10];
  rowOut += ",";  rowOut += fields[11];
  rowOut += "\n";
  return true;
}


// ============================================================
// PHASE 5 — LITTLEFS / CSV
// ============================================================

// Header is written ONLY when the file is missing or completely empty.
// An existing file with data is never touched here.
bool ensureHeader() {
  if (LittleFS.exists(CSV_PATH)) {
    File f = LittleFS.open(CSV_PATH, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    f.close();
    if (sz > 0) return true;               // existing data: keep it as is
  }
  File w = LittleFS.open(CSV_PATH, FILE_WRITE);   // new or empty file only
  if (!w) return false;
  size_t n = w.print(CSV_HEADER "\n");
  w.close();
  return n == (strlen(CSV_HEADER) + 1);
}

// If power was lost in the middle of a row, the last line has no '\n'.
// Add one so the next row does not get glued onto the broken line.
void ensureTrailingNewline() {
  File f = LittleFS.open(CSV_PATH, FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  if (sz == 0) { f.close(); return; }
  f.seek(sz - 1);
  int last = f.read();
  f.close();
  if (last != '\n') {
    File a = LittleFS.open(CSV_PATH, FILE_APPEND);
    if (a) {
      a.print("\n");
      a.close();
      Serial.println("CSV: last line was incomplete, closed it with a newline");
    }
  }
}

// Number of data rows already stored (lines minus the header line).
uint32_t countCsvRows() {
  File f = LittleFS.open(CSV_PATH, FILE_READ);
  if (!f) return 0;
  uint32_t newlines = 0;
  uint8_t buf[512];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '\n') newlines++;
    }
  }
  f.close();
  return newlines > 0 ? newlines - 1 : 0;
}

void initLogging() {
  fsReady = LittleFS.begin(false);          // false = NEVER format automatically
  if (!fsReady) {
    logState = "STORAGE ERROR";
    Serial.println("LittleFS mount FAILED - CSV logging disabled (dashboard still works).");
    return;
  }
  Serial.println("LittleFS mounted (existing data is kept).");

  headerOk = ensureHeader();
  if (!headerOk) {
    logState = "FILE ERROR";
    Serial.println("Could not create/open " CSV_PATH " - will retry when logging.");
  } else {
    ensureTrailingNewline();
    rowsInFile = countCsvRows();
    logState = "ACTIVE";
    Serial.print("CSV ready: ");
    Serial.print(CSV_PATH);
    Serial.print("   rows already saved: ");
    Serial.println((unsigned long)rowsInFile);
  }
  fsFreeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
}

// Appends the pending row. Opens and closes the file each time so nothing
// stays half-written, and never waits (no delay()).
void logPendingRow() {
  pendingRowReady = false;                  // one attempt per row, no endless retries

  fsFreeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (fsFreeBytes < MIN_FREE_BYTES) {
    logState = "STORAGE FULL";
    Serial.println("CSV: storage almost full, logging paused");
    return;
  }

  if (!headerOk) {
    headerOk = ensureHeader();
    if (!headerOk) {
      logState = "FILE ERROR";
      logErrors++;
      Serial.println("CSV: could not create the file");
      return;
    }
  }

  File f = LittleFS.open(CSV_PATH, FILE_APPEND);
  if (!f) {
    logState = "FILE ERROR";
    logErrors++;
    Serial.println("CSV: could not open file for append");
    return;
  }
  size_t written = f.print(pendingRow);
  f.close();

  if (written != pendingRow.length()) {
    logState = "WRITE ERROR";
    logErrors++;
    Serial.println("CSV: write incomplete");
    return;
  }

  rowsInFile++;
  rowsSession++;
  logState = "ACTIVE";
  Serial.print("CSV: row saved (rows in file: ");
  Serial.print((unsigned long)rowsInFile);
  Serial.println(")");
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
  json += "\"data_age_ms\":" + String(dataAge) + ",";

  // ---- Phase 5 fields ----
  json += "\"fs_ready\":" + String(fsReady ? "true" : "false") + ",";
  json += "\"logging_active\":" + String((fsReady && logState == "ACTIVE") ? "true" : "false") + ",";
  json += "\"log_state\":\"" + logState + "\",";
  json += "\"rows_in_file\":" + String((unsigned long)rowsInFile) + ",";
  json += "\"rows_session\":" + String((unsigned long)rowsSession) + ",";
  json += "\"log_errors\":" + String((unsigned long)logErrors) + ",";
  json += "\"fs_free_kb\":" + String((float)fsFreeBytes / 1024.0f, 1);
  json += "}";

  server.send(200, "application/json", json);
}

// Streams /battery_data.csv to the browser as a file download.
void handleDownload() {
  if (!fsReady) {
    server.send(503, "text/plain", "Storage not available");
    return;
  }
  if (!LittleFS.exists(CSV_PATH)) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  File f = LittleFS.open(CSV_PATH, FILE_READ);
  if (!f) {
    server.send(500, "text/plain", "Could not open CSV file");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"battery_data.csv\"");
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "text/csv");
  f.close();
}
