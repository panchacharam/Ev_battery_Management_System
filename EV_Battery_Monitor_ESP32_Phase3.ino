// ============================================================
// EV Battery Monitoring System — Phase 3
// ESP32 UART receiver (Arduino IDE)
// Receives DATA,... packets from the Raspberry Pi Pico over
// Serial2 and prints a formatted block to the USB Serial Monitor.
// ============================================================

#define RXD2 16
#define TXD2 17

#define UART_BAUD 115200
#define EXPECTED_FIELDS 12   // "DATA" + 11 data fields

String rxBuffer = "";

unsigned long lastPacketTime = 0;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL_MS = 5000;


void setup() {
  Serial.begin(115200);
  Serial2.begin(UART_BAUD, SERIAL_8N1, RXD2, TXD2);

  Serial.println("ESP32 UART receiver ready.");
  Serial.println("Waiting for DATA packets from Pico...");

  lastPacketTime = millis();
  lastHeartbeat = millis();
}


void loop() {

  // ---- Read Serial2 one character at a time, building a line ----
  while (Serial2.available()) {
    char c = Serial2.read();

    if (c == '\n') {
      processPacket(rxBuffer);
      rxBuffer = "";
    } else if (c != '\r') {
      rxBuffer += c;

      // Safety: if a newline never arrives, don't grow forever
      if (rxBuffer.length() > 200) {
        rxBuffer = "";
      }
    }
  }

  // ---- Troubleshooting heartbeat: flag if nothing has arrived ----
  unsigned long now = millis();
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    if (now - lastPacketTime >= HEARTBEAT_INTERVAL_MS) {
      Serial.println("No data received from Pico in the last 5 seconds...");
    }
  }
}


void processPacket(String packet) {
  packet.trim();

  // 1. Ignore empty packets
  if (packet.length() == 0) {
    return;
  }

  // 2. Ignore packets that don't start with DATA,
  if (!packet.startsWith("DATA,")) {
    Serial.println("Invalid packet");
    return;
  }

  // ---- Split into fields on commas ----
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

  // 3. Detect incomplete/malformed packets (wrong field count)
  if (fieldCount != EXPECTED_FIELDS) {
    Serial.println("Invalid packet");
    return;
  }

  // 4. Avoid crashing if a field is missing/empty
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

  // ---- Parse fields into the right types ----
  float batteryVoltage    = fields[1].toFloat();
  float batteryPercentage = fields[2].toFloat();
  float current           = fields[3].toFloat();
  float temperature       = fields[4].toFloat();
  float humidity          = fields[5].toFloat();
  float ax                = fields[6].toFloat();
  float ay                = fields[7].toFloat();
  float az                = fields[8].toFloat();
  String activity         = fields[9];
  String batteryStatus    = fields[10];
  int overallFault        = fields[11].toInt();

  // Valid packet received — reset the "no data" heartbeat timer
  lastPacketTime = millis();

  // ---- Print formatted block ----
  Serial.println("========== EV BATTERY DATA ==========");

  Serial.print("Battery Voltage : ");
  Serial.print(batteryVoltage, 2);
  Serial.println(" V");

  Serial.print("Battery          : ");
  Serial.print(batteryPercentage, 1);
  Serial.println(" %");

  Serial.print("Current          : ");
  Serial.print(current, 3);
  Serial.println(" A");

  Serial.print("Temperature      : ");
  Serial.print(temperature, 0);
  Serial.println(" C");

  Serial.print("Humidity         : ");
  Serial.print(humidity, 0);
  Serial.println(" %");

  Serial.print("AX               : ");
  Serial.println(ax, 2);

  Serial.print("AY               : ");
  Serial.println(ay, 2);

  Serial.print("AZ               : ");
  Serial.println(az, 2);

  Serial.print("Activity         : ");
  Serial.println(activity);

  Serial.print("Battery Status   : ");
  Serial.println(batteryStatus);

  Serial.print("Overall Fault    : ");
  Serial.println(overallFault == 1 ? "YES" : "NO");

  Serial.println("======================================");
}
