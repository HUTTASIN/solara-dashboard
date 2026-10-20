#include <WiFi.h>
#include <WebServer.h>

// ==================================================
// SOLARA : ระบบทำความสะอาดแผงโซลาร์เซลล์
// ราง (A4988 + NEMA17) + แปรง (BTS7960) + ปั๊ม (Relay)
// ==================================================

// ==================================================
// 1. ตั้งค่า Wi-Fi
// ==================================================
const char* ssid = "Kat";
const char* password = "1234567890";

WebServer server(80);

// ==================================================
// 2. กำหนดขาอุปกรณ์
// ==================================================

// A4988 + NEMA17 : ระบบราง
#define STEP_PIN 25
#define DIR_PIN  26
#define EN_PIN   27

// BTS7960 : มอเตอร์แปรง
#define RPWM_PIN 32
#define LPWM_PIN 33
#define R_EN_PIN 18
#define L_EN_PIN 19

// Limit Switch
#define LIMIT_LEFT_PIN  22
#define LIMIT_RIGHT_PIN 23

// Relay, LED และปุ่มกด
#define RELAY_PIN        5
#define LED_PIN          17
#define START_BUTTON_PIN 21
#define STOP_BUTTON_PIN  13

// ==================================================
// 3. ตั้งค่าที่ต้องตรวจสอบกับอุปกรณ์จริง
// ==================================================

// ถ้ารีเลย์เปิดเมื่อ GPIO เป็น LOW ให้ใช้ true
const bool RELAY_ACTIVE_LOW = true;

// ถ้ารางวิ่งผิดทิศ ให้เปลี่ยน HIGH เป็น LOW
const uint8_t DIR_RIGHT_LEVEL = HIGH;

// ปุ่มและ Limit Switch ใช้ INPUT_PULLUP
// NO ต่อระหว่าง GPIO กับ GND  (กด/ทำงาน = LOW)

// ความเร็วราง หน่วย steps/second
int speedSPS = 400;

// ---- ลำดับการวิ่งเมื่อกด START ----
// จำนวน "เที่ยว" ที่รางวิ่ง แต่ละเที่ยววิ่งจนชน Limit แล้วเด้งกลับ
//   3 = ชนขวา -> เด้งไปชนซ้าย -> เด้งกลับมาชนขวา (ค่าเริ่มต้น)
//   2 = ชนขวา -> เด้งไปชนซ้าย แล้วจบ (ไป-กลับ 1 รอบ)
const uint8_t SWEEP_LEGS = 3;

// ป้องกันกรณี Limit Switch เสีย/สายหลุด: ถ้าวิ่งเที่ยวเดียวนานเกินเวลานี้
// จะหยุดทุกอุปกรณ์ทันที (ปรับให้มากกว่าเวลาที่รางวิ่งสุดทางจริง)
const unsigned long LEG_TIMEOUT_MS = 120000UL;

// ==================================================
// 4. ตัวแปรสถานะ
// ==================================================

enum CycleState {
  IDLE,
  MOVING_RIGHT,
  MOVING_LEFT,
  DONE,
  STOPPED
};

CycleState cycleState = IDLE;

bool railMoving = false;
bool railToRight = true;
bool brushOn = false;
bool pumpOn = false;
bool stepPulseHigh = false;

long positionSteps = 0;

unsigned long stepIntervalUs = 2500;
unsigned long lastStepUs = 0;
unsigned long pulseHighUs = 0;

uint8_t legsDone = 0;           // จำนวนเที่ยวที่ชน Limit แล้ว
unsigned long legStartMs = 0;   // เวลาเริ่มเที่ยวปัจจุบัน

String cycleMessage = "Ready";

// ==================================================
// 5. ฟังก์ชันอ่านปุ่มแบบ Debounce
// ==================================================

struct Button {
  uint8_t pin;
  bool stableState;
  bool lastReading;
  unsigned long lastChange;
};

Button startButton = { START_BUTTON_PIN, HIGH, HIGH, 0 };
Button stopButton  = { STOP_BUTTON_PIN,  HIGH, HIGH, 0 };

void initButton(Button &b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.stableState = digitalRead(b.pin);
  b.lastReading = b.stableState;
  b.lastChange = millis();
}

bool pressedEvent(Button &b) {
  bool reading = digitalRead(b.pin);
  unsigned long now = millis();

  if (reading != b.lastReading) {
    b.lastReading = reading;
    b.lastChange = now;
  }

  if (now - b.lastChange >= 30) {
    if (reading != b.stableState) {
      b.stableState = reading;

      if (b.stableState == LOW) {
        return true;
      }
    }
  }

  return false;
}

// ==================================================
// 6. ฟังก์ชันอุปกรณ์
// ==================================================

bool leftLimitPressed() {
  return digitalRead(LIMIT_LEFT_PIN) == LOW;
}

bool rightLimitPressed() {
  return digitalRead(LIMIT_RIGHT_PIN) == LOW;
}

bool cycleRunning() {
  return cycleState == MOVING_RIGHT ||
         cycleState == MOVING_LEFT;
}

void setPump(bool on) {
  pumpOn = on;

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  }
}

void setBrush(bool on) {
  brushOn = on;

  if (on) {
    digitalWrite(R_EN_PIN, HIGH);
    digitalWrite(L_EN_PIN, HIGH);

    // หมุนแปรงทิศทางเดียวด้วยความเร็วเต็ม
    digitalWrite(LPWM_PIN, LOW);
    digitalWrite(RPWM_PIN, HIGH);
  } else {
    digitalWrite(RPWM_PIN, LOW);
    digitalWrite(LPWM_PIN, LOW);
  }
}

void stopRail() {
  railMoving = false;

  digitalWrite(STEP_PIN, LOW);
  stepPulseHigh = false;

  // A4988 ENABLE เป็น Active LOW
  digitalWrite(EN_PIN, HIGH);
}

bool startRail(bool toRight) {
  // ห้ามเคลื่อนที่เข้าหา Limit ที่กดอยู่แล้ว
  if (toRight && rightLimitPressed()) {
    return false;
  }

  if (!toRight && leftLimitPressed()) {
    return false;
  }

  railToRight = toRight;

  digitalWrite(
    DIR_PIN,
    toRight ? DIR_RIGHT_LEVEL :
              (DIR_RIGHT_LEVEL == HIGH ? LOW : HIGH)
  );

  delayMicroseconds(5);

  digitalWrite(EN_PIN, LOW);
  railMoving = true;
  stepPulseHigh = false;
  lastStepUs = micros();

  return true;
}

// ==================================================
// 7. หยุดทุกอุปกรณ์
// ==================================================

void stopAll() {
  stopRail();
  setBrush(false);
  setPump(false);

  cycleState = STOPPED;
  cycleMessage = "Stopped by user";
}

// หยุดทุกอย่างพร้อมแจ้งสาเหตุความผิดพลาด
void faultStop(const char* msg) {
  stopAll();
  cycleMessage = msg;
}

// ==================================================
// 8. จบรอบอัตโนมัติ
// ==================================================

void finishCycle() {
  stopRail();
  setBrush(false);
  setPump(false);

  cycleState = DONE;
  cycleMessage = "Cleaning completed";
}

// ==================================================
// 9. เริ่มรอบอัตโนมัติ
// ==================================================

// เริ่มวิ่งเที่ยวหนึ่งไปยังทิศที่กำหนด
bool beginLeg(bool toRight) {
  if (!startRail(toRight)) {
    return false;
  }

  cycleState = toRight ? MOVING_RIGHT : MOVING_LEFT;
  legStartMs = millis();

  cycleMessage = "Pass " + String(legsDone + 1) + "/" +
                 String(SWEEP_LEGS) +
                 (toRight ? ": moving right" : ": moving left");

  return true;
}

bool startCycle() {
  if (cycleRunning()) {
    return false;
  }

  // หาก Limit ทั้งสองข้างทำงานพร้อมกัน ไม่อนุญาตให้เริ่มรอบ
  if (leftLimitPressed() && rightLimitPressed()) {
    faultStop("Fault: both limits pressed");
    return false;
  }

  stopRail();

  // เปิดปั๊มและแปรงทันทีที่กด START
  setPump(true);
  setBrush(true);

  legsDone = 0;

  // ปกติวิ่งไปทางขวาก่อน
  // หากเริ่มต้นโดยกด Limit ขวาอยู่ ให้เริ่มจากถอยไปทางซ้าย
  bool firstToRight = !rightLimitPressed();

  if (beginLeg(firstToRight)) {
    return true;
  }

  faultStop("Cannot start rail");
  return false;
}

// ==================================================
// 10. ควบคุมลำดับการทำงาน
// ==================================================
// วิ่งจนชน Limit ปลายทาง -> เด้งกลับทิศตรงข้าม -> ...
// ครบ SWEEP_LEGS เที่ยวแล้วจบรอบ

void updateCycle() {
  if (!cycleRunning()) {
    return;
  }

  bool movingRight = (cycleState == MOVING_RIGHT);

  bool atTarget = movingRight ? rightLimitPressed()
                              : leftLimitPressed();

  if (atTarget) {
    stopRail();
    legsDone++;

    if (legsDone >= SWEEP_LEGS) {
      finishCycle();
      return;
    }

    // เด้งกลับไปทิศตรงข้าม
    if (!beginLeg(!movingRight)) {
      faultStop("Fault: cannot reverse at limit");
    }
    return;
  }

  // ป้องกัน Limit เสีย: วิ่งนานเกินกำหนดโดยไม่ชน
  if (millis() - legStartMs > LEG_TIMEOUT_MS) {
    faultStop("Fault: limit not reached (timeout)");
  }
}

// ==================================================
// 11. สร้าง STEP pulse โดยไม่ใช้ delay ยาว
// ==================================================

void updateStepper() {
  if (!railMoving) {
    return;
  }

  unsigned long now = micros();

  // ปิด pulse หลัง HIGH ประมาณ 3 us
  if (stepPulseHigh) {
    if ((unsigned long)(now - pulseHighUs) >= 3) {
      digitalWrite(STEP_PIN, LOW);
      stepPulseHigh = false;
    }
    return;
  }

  if ((unsigned long)(now - lastStepUs) >= stepIntervalUs) {
    digitalWrite(STEP_PIN, HIGH);

    pulseHighUs = now;
    lastStepUs = now;
    stepPulseHigh = true;

    positionSteps += railToRight ? 1 : -1;
  }
}

// ==================================================
// 12. ป้องกันรางชน Limit ในโหมด Manual
// ==================================================

void updateManualLimits() {
  if (cycleRunning() || !railMoving) {
    return;
  }

  if (railToRight && rightLimitPressed()) {
    stopRail();
    cycleMessage = "Manual stopped at right limit";
  }

  if (!railToRight && leftLimitPressed()) {
    stopRail();
    cycleMessage = "Manual stopped at left limit";
  }
}

// ==================================================
// 13. ไฟแสดงสถานะ
// ==================================================

void updateStatusLED() {
  bool active = railMoving || brushOn || pumpOn;

  digitalWrite(LED_PIN, active ? HIGH : LOW);
}

// ==================================================
// 14. แปลงสถานะเป็นข้อความ
// ==================================================

String stateName() {
  switch (cycleState) {
    case IDLE:         return "READY";
    case MOVING_RIGHT: return "MOVING RIGHT";
    case MOVING_LEFT:  return "MOVING LEFT";
    case DONE:         return "COMPLETED";
    case STOPPED:      return "STOPPED";
  }

  return "UNKNOWN";
}

// ==================================================
// 15. Dashboard (หน้าเว็บที่ ESP32 เสิร์ฟเอง)
// ==================================================

const char DASHBOARD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="th">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Solar Cleaning Control</title>

<style>
body {
  font-family: Arial, sans-serif;
  background: #eef2f7;
  color: #182334;
  margin: 0;
  padding: 16px;
}
.container {
  max-width: 720px;
  margin: auto;
}
h2 { text-align: center; }
.card {
  background: white;
  border-radius: 14px;
  padding: 16px;
  margin: 12px 0;
  box-shadow: 0 2px 8px #0001;
}
button {
  border: 0;
  border-radius: 9px;
  padding: 13px 12px;
  margin: 4px;
  font-size: 15px;
  font-weight: bold;
  cursor: pointer;
}
.start { background: #198754; color: white; }
.stop { background: #dc3545; color: white; }
.control { background: #dbeafe; color: #12345a; }
button:active { opacity: .7; }
.grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 8px;
}
.full { width: 100%; }
.value { font-weight: bold; }
small { color: #667085; }
</style>
</head>

<body>
<div class="container">
  <h2>Solar Panel Cleaning</h2>

  <div class="card">
    <h3>Automatic Control</h3>
    <p>กด START: ปั๊ม + แปรงทำงาน รางวิ่งชน Limit แล้วเด้งกลับไปชนอีกฝั่ง และเด้งกลับ</p>
    <button class="start full" onclick="cmd('/start')">
      START ALL
    </button>
    <button class="stop full" onclick="cmd('/stop')">
      STOP ALL
    </button>
    <small>
      STOP ALL เป็นคำสั่งผ่านซอฟต์แวร์ ไม่ใช่วงจร
      หยุดฉุกเฉินแบบตัดกำลัง
    </small>
  </div>

  <div class="card">
    <h3>Manual Control</h3>
    <div class="grid">
      <button class="control" onclick="cmd('/run?dir=1')">
        Rail Right
      </button>
      <button class="control" onclick="cmd('/run?dir=-1')">
        Rail Left
      </button>
      <button class="control" onclick="cmd('/railstop')">
        Rail Stop
      </button>
      <button class="control" onclick="cmd('/brush?on=1')">
        Brush ON
      </button>
      <button class="control" onclick="cmd('/brush?on=0')">
        Brush OFF
      </button>
      <button class="control" onclick="cmd('/pump?on=1')">
        Pump ON
      </button>
      <button class="control" onclick="cmd('/pump?on=0')">
        Pump OFF
      </button>
    </div>
  </div>

  <div class="card">
    <h3>Rail Speed</h3>
    <label>Steps/second: <span id="speedVal">400</span></label>
    <input type="range" min="100" max="800" value="400"
      id="speed" class="full"
      oninput="speedVal.textContent=this.value"
      onchange="cmd('/speed?value='+this.value)">
  </div>

  <div class="card">
    <h3>System Status</h3>
    <p>Cycle: <span class="value" id="cycle">--</span></p>
    <p>Message: <span class="value" id="message">--</span></p>
    <p>Rail: <span class="value" id="rail">--</span></p>
    <p>Brush: <span class="value" id="brush">--</span></p>
    <p>Pump: <span class="value" id="pump">--</span></p>
    <p>Left Limit: <span class="value" id="left">--</span></p>
    <p>Right Limit: <span class="value" id="right">--</span></p>
    <p>Position estimate: <span class="value" id="pos">--</span> steps</p>
    <button class="control full" onclick="refreshStatus()">
      Refresh Status
    </button>
  </div>
</div>

<script>
async function cmd(path) {
  try {
    const r = await fetch(path);
    const t = await r.text();
    console.log(t);
    await refreshStatus();
  } catch(e) {
    alert('เชื่อมต่อ ESP32 ไม่ได้');
  }
}

async function refreshStatus() {
  try {
    const r = await fetch('/status');
    const s = await r.json();

    document.getElementById('cycle').textContent = s.cycle;
    document.getElementById('message').textContent = s.message;
    document.getElementById('rail').textContent = s.rail;
    document.getElementById('brush').textContent = s.brush;
    document.getElementById('pump').textContent = s.pump;
    document.getElementById('left').textContent = s.left;
    document.getElementById('right').textContent = s.right;
    document.getElementById('pos').textContent = s.position;
    document.getElementById('speedVal').textContent = s.speed;
    document.getElementById('speed').value = s.speed;
  } catch(e) {
    document.getElementById('message').textContent =
      'Connection lost';
  }
}

setInterval(refreshStatus, 1000);
refreshStatus();
</script>
</body>
</html>
)rawliteral";

// ==================================================
// 16. Web endpoints
// ==================================================

// อนุญาตให้ Dashboard บน Cloudflare (index.html) เรียก ESP32 ข้ามโดเมนได้
void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Cache-Control", "no-store");
}

void reply(int code, const char* type, const String &body) {
  sendCorsHeaders();
  server.send(code, type, body);
}

void handleRoot() {
  sendCorsHeaders();
  server.send_P(200, "text/html; charset=utf-8", DASHBOARD);
}

void handleStart() {
  bool ok = startCycle();

  reply(ok ? 200 : 409,
        "text/plain; charset=utf-8",
        ok ? String("Cycle started") : cycleMessage);
}

void handleStop() {
  stopAll();
  reply(200, "text/plain; charset=utf-8", "All stopped");
}

void handleManualRun() {
  if (cycleRunning()) {
    reply(409, "text/plain; charset=utf-8",
          "Automatic cycle is running");
    return;
  }

  int dir = server.arg("dir").toInt();

  if (dir != 1 && dir != -1) {
    reply(400, "text/plain", "Invalid direction");
    return;
  }

  bool toRight = (dir == 1);

  if (startRail(toRight)) {
    cycleMessage = toRight ?
      "Manual moving right" : "Manual moving left";

    reply(200, "text/plain", "Rail started");
  } else {
    reply(409, "text/plain",
          "Limit switch blocks this direction");
  }
}

void handleRailStop() {
  stopRail();
  cycleMessage = "Rail stopped";
  reply(200, "text/plain", "Rail stopped");
}

void handleBrush() {
  if (cycleRunning()) {
    reply(409, "text/plain", "Automatic cycle is running");
    return;
  }

  bool on = server.arg("on") == "1";
  setBrush(on);

  reply(200, "text/plain", on ? "Brush ON" : "Brush OFF");
}

void handlePump() {
  if (cycleRunning()) {
    reply(409, "text/plain", "Automatic cycle is running");
    return;
  }

  bool on = server.arg("on") == "1";
  setPump(on);

  reply(200, "text/plain", on ? "Pump ON" : "Pump OFF");
}

void handleSpeed() {
  int value = server.arg("value").toInt();

  speedSPS = constrain(value, 100, 800);
  stepIntervalUs = 1000000UL / speedSPS;

  reply(200, "text/plain", String(speedSPS));
}

void handleStatus() {
  String json = "{";

  json += "\"cycle\":\"" + stateName() + "\",";
  json += "\"message\":\"" + cycleMessage + "\",";
  json += "\"rail\":\"";
  json += railMoving ? (railToRight ? "RIGHT" : "LEFT") : "STOPPED";
  json += "\",";
  json += "\"brush\":\"" + String(brushOn ? "ON" : "OFF") + "\",";
  json += "\"pump\":\"" + String(pumpOn ? "ON" : "OFF") + "\",";
  json += "\"left\":\"";
  json += leftLimitPressed() ? "PRESSED" : "FREE";
  json += "\",";
  json += "\"right\":\"";
  json += rightLimitPressed() ? "PRESSED" : "FREE";
  json += "\",";
  json += "\"position\":" + String(positionSteps) + ",";
  json += "\"speed\":" + String(speedSPS) + ",";

  // ฟิลด์เพิ่มเติมสำหรับ Dashboard บน Cloudflare (index.html)
  json += "\"running\":";
  json += (railMoving || brushOn || pumpOn) ? "true," : "false,";
  json += "\"direction\":";
  json += railMoving ? (railToRight ? "1," : "-1,") : "0,";
  json += "\"legsDone\":" + String(legsDone) + ",";
  json += "\"legsTotal\":" + String(SWEEP_LEGS);

  json += "}";

  reply(200, "application/json", json);
}

// ตอบ CORS preflight (OPTIONS) และ 404
void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    sendCorsHeaders();
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "*");
    server.send(204);
    return;
  }

  reply(404, "text/plain", "Not found");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/start", HTTP_GET, handleStart);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/run", HTTP_GET, handleManualRun);
  server.on("/railstop", HTTP_GET, handleRailStop);
  server.on("/brush", HTTP_GET, handleBrush);
  server.on("/pump", HTTP_GET, handlePump);
  server.on("/speed", HTTP_GET, handleSpeed);
  server.on("/status", HTTP_GET, handleStatus);
  server.onNotFound(handleNotFound);

  server.begin();
}

// ==================================================
// 17. SETUP
// ==================================================

void setup() {
  Serial.begin(115200);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  pinMode(RPWM_PIN, OUTPUT);
  pinMode(LPWM_PIN, OUTPUT);
  pinMode(R_EN_PIN, OUTPUT);
  pinMode(L_EN_PIN, OUTPUT);

  pinMode(LIMIT_LEFT_PIN, INPUT_PULLUP);
  pinMode(LIMIT_RIGHT_PIN, INPUT_PULLUP);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  // เริ่มต้นโดยปิดอุปกรณ์ทั้งหมด
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(EN_PIN, HIGH);

  digitalWrite(RPWM_PIN, LOW);
  digitalWrite(LPWM_PIN, LOW);
  digitalWrite(R_EN_PIN, HIGH);
  digitalWrite(L_EN_PIN, HIGH);

  digitalWrite(
    RELAY_PIN,
    RELAY_ACTIVE_LOW ? HIGH : LOW
  );

  digitalWrite(LED_PIN, LOW);

  initButton(startButton);
  initButton(stopButton);

  stepIntervalUs = 1000000UL / speedSPS;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to Wi-Fi");

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < 20000) {
    delay(300);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Dashboard IP: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection failed.");
    Serial.println("Physical buttons remain available; retrying in background.");
  }

  // เปิดเว็บเซิร์ฟเวอร์เสมอ เพื่อให้ใช้งานได้ทันทีเมื่อ Wi-Fi กลับมา
  setupWebServer();

  cycleState = IDLE;
  cycleMessage = "Ready";
}

// ==================================================
// 18. LOOP
// ==================================================

void loop() {
  server.handleClient();

  // STOP มีลำดับความสำคัญสูงกว่า START
  if (pressedEvent(stopButton)) {
    stopAll();
  } else if (pressedEvent(startButton)) {
    startCycle();
  }

  updateCycle();
  updateManualLimits();
  updateStepper();
  updateStatusLED();
}
