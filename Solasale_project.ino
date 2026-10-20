#include <WiFi.h>
#include <WebServer.h>

// ==================================================
// SOLARA : ระบบทำความสะอาดแผงโซลาร์เซลล์
// ราง (A4988 + NEMA17) + แปรง (BTS7960) + ปั๊ม (Relay)
// ==================================================

// ==================================================
// 1. ตั้งค่า Wi-Fi
// ==================================================
const char* WIFI_SSID = "true_home2G_248";
const char* WIFI_PASSWORD = "7u393g45";

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

// Relay และปุ่มกด
#define RELAY_PIN        5
#define START_BUTTON_PIN 21
#define STOP_BUTTON_PIN  13

// ==================================================
// 3. ตั้งค่าที่ต้องตรวจสอบกับอุปกรณ์จริง
// ==================================================

// ถ้ารีเลย์เปิดเมื่อ GPIO เป็น LOW ให้ใช้ true
const bool RELAY_ACTIVE_LOW = false;

// ถ้ารางวิ่งผิดทิศ ให้เปลี่ยน HIGH เป็น LOW
const uint8_t DIR_RIGHT_LEVEL = HIGH;

// ปุ่มและ Limit Switch ใช้ INPUT_PULLUP
// NO ต่อระหว่าง GPIO กับ GND  (กด/ทำงาน = LOW)

// ความเร็วราง หน่วย steps/second
int speedSPS = 400;

// Limit Switch ต้องถูกกดค้างต่อเนื่องอย่างน้อยกี่ ms ถึงจะนับว่า "ชนจริง"
// ช่วยกัน noise จากสเต็ปเปอร์ที่ทำให้อ่านค่า Limit ผิดแล้วรางเด้งกลับก่อนถึงสุดทาง
const unsigned long LIMIT_CONFIRM_MS = 20;

// ---- ลำดับการวิ่งเมื่อกด START ----
// จำนวน "เที่ยว" ที่รางวิ่ง แต่ละเที่ยววิ่งจนชน Limit แล้วเด้งกลับ
//   3 = ชนขวา -> เด้งไปชนซ้าย -> เด้งกลับมาชนขวา (ค่าเริ่มต้น)
//   2 = ชนขวา -> เด้งไปชนซ้าย แล้วจบ (ไป-กลับ 1 รอบ)
const uint8_t SWEEP_LEGS = 3;

// ป้องกันกรณี Limit Switch เสีย/สายหลุด: ถ้าวิ่งเที่ยวเดียวนานเกินเวลานี้
// จะหยุดทุกอุปกรณ์ทันที (ปรับให้มากกว่าเวลาที่รางวิ่งสุดทางจริง)
const unsigned long LEG_TIMEOUT_MS = 360000UL;   // 6 นาที

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
long legStartPos = 0;           // ตำแหน่ง (steps) ตอนเริ่มเที่ยว
long lastLegSteps = 0;          // จำนวน steps ที่เที่ยวก่อนหน้าวิ่งไปก่อนชน Limit
unsigned long limitSinceMs = 0; // เวลาที่เริ่มเห็น Limit ถูกกด (0 = ยังไม่เห็น)

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
    // เปิดใช้งานทั้งสองช่องของ BTS7960
    digitalWrite(R_EN_PIN, HIGH);
    digitalWrite(L_EN_PIN, HIGH);

    // สั่ง PWM เต็มกำลังไปทางเดียว: RPWM = 100%, LPWM = 0%
    // ใช้ analogWrite เพื่อกำหนด duty cycle ชัดเจนบน ESP32
    analogWrite(LPWM_PIN, 0);
    analogWrite(RPWM_PIN, 255);
  } else {
    // หยุดสัญญาณขับมอเตอร์ทั้งสองด้าน
    analogWrite(RPWM_PIN, 0);
    analogWrite(LPWM_PIN, 0);
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
  legStartPos = positionSteps;
  limitSinceMs = 0;

  cycleMessage = "Pass " + String(legsDone + 1) + "/" +
                 String(SWEEP_LEGS) +
                 (toRight ? ": moving right" : ": moving left");

  // แสดงว่าเที่ยวก่อนหน้าวิ่งไปกี่ steps ก่อนชน Limit (ไว้ตรวจสอบหน้างาน)
  if (legsDone > 0) {
    cycleMessage += " (prev leg " + String(lastLegSteps) + " steps)";
  }

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
    // ต้องเห็น Limit ถูกกดค้างต่อเนื่อง LIMIT_CONFIRM_MS ก่อน
    // ถ้าเป็นแค่สัญญาณรบกวนชั่วขณะ รางจะวิ่งต่อไป
    if (limitSinceMs == 0) {
      limitSinceMs = millis();
      if (limitSinceMs == 0) limitSinceMs = 1;
    }

    if (millis() - limitSinceMs < LIMIT_CONFIRM_MS) {
      return;
    }

    stopRail();
    lastLegSteps = labs(positionSteps - legStartPos);
    limitSinceMs = 0;
    legsDone++;

    if (legsDone >= SWEEP_LEGS) {
      finishCycle();
      cycleMessage += " (last leg " + String(lastLegSteps) + " steps)";
      return;
    }

    // เด้งกลับไปทิศตรงข้าม
    if (!beginLeg(!movingRight)) {
      faultStop("Fault: cannot reverse at limit");
    }
    return;
  }

  // Limit ไม่ถูกกดแล้ว: ล้างตัวจับเวลา
  limitSinceMs = 0;

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
:root{color-scheme:dark;--bg:#080d19;--panel:#111a2b;--panel2:#172238;--line:#293750;--muted:#91a0b8;--text:#f3f6fc;--green:#36d399;--blue:#60a5fa;--red:#fb7185;--amber:#fbbf24}
*{box-sizing:border-box}
body{margin:0;font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",Arial,sans-serif;background:radial-gradient(ellipse at 15% 0%,#17365b 0,transparent 38%),radial-gradient(ellipse at 100% 15%,#193c36 0,transparent 32%),var(--bg);color:var(--text);min-height:100vh}
.container{max-width:1050px;margin:0 auto;padding:28px 20px 34px}
.header{display:flex;align-items:center;justify-content:space-between;gap:18px;margin-bottom:24px;padding:22px 24px;border:1px solid var(--line);border-radius:22px;background:linear-gradient(120deg,rgba(21,35,58,.96),rgba(15,25,42,.88));box-shadow:0 18px 55px #0003}
.header h1{margin:0;font-size:clamp(23px,4vw,32px);letter-spacing:-.7px}.header p{margin:7px 0 0;color:var(--muted);font-size:14px}.header:after{content:'SOLARA  /  ESP32';font-size:11px;font-weight:800;letter-spacing:1.5px;color:#a5f3fc;border:1px solid #28516a;background:#0d2639;border-radius:12px;padding:12px 14px;white-space:nowrap}
.status-card,.card,.info{background:linear-gradient(145deg,rgba(20,31,51,.98),rgba(13,22,38,.98));border:1px solid var(--line);border-radius:20px;box-shadow:0 12px 32px #0002}
.status-card{padding:24px;margin-bottom:16px;text-align:left;display:flex;align-items:center;justify-content:space-between;gap:16px}.status-card:before{content:'SYSTEM STATUS';font-size:12px;letter-spacing:1.5px;color:var(--muted);font-weight:800;order:0}.status{display:inline-flex;align-items:center;justify-content:center;padding:10px 17px;border-radius:12px;font-size:14px;font-weight:800;letter-spacing:.5px;order:1}.status.stopped{background:#3b1d2b;color:#fda4af;border:1px solid #713247}.status.run{background:#103b32;color:#6ee7b7;border:1px solid #1c6a55}.msg{color:var(--muted);font-size:13px;margin-top:0;order:2;text-align:right;flex:1}
.info-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px;margin-bottom:16px}.info{padding:20px 22px;text-align:left}.info-title{color:var(--muted);font-size:11px;letter-spacing:1.5px;font-weight:800;margin-bottom:12px}.info-value{font-size:clamp(25px,4vw,34px);font-weight:800;letter-spacing:-.7px}
.card{padding:22px;margin-bottom:16px}.card h2{margin:0 0 8px;font-size:18px;letter-spacing:-.25px}.card p{color:var(--muted);font-size:13px;line-height:1.65;margin:0 0 16px}.grid2{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.grid3{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin-top:12px}
button{border:1px solid #34435d;border-radius:13px;min-height:52px;padding:12px 14px;font-size:14px;font-weight:800;cursor:pointer;color:#edf4ff;transition:transform .15s,filter .15s, border-color .15s;background:linear-gradient(135deg,#25334b,#1a263b)}button:hover{filter:brightness(1.14);border-color:#607798}button:active{transform:scale(.98)}button.start,button.forward{background:linear-gradient(135deg,#087f5b,#10b981);border-color:#21b98a;color:#fff}button.reverse{background:linear-gradient(135deg,#1d4ed8,#3b82f6);border-color:#538dff;color:#fff}button.stop{background:linear-gradient(135deg,#be123c,#f43f5e);border-color:#fb7185;color:#fff}.full{width:100%;margin-top:12px;font-size:15px}.speed-value{text-align:left;font-size:28px;font-weight:800;margin:18px 0 15px}.speed-value span:last-child{color:var(--muted);font-size:13px;font-weight:600}input[type=range]{width:100%;accent-color:var(--green);cursor:pointer;height:8px}.row{display:flex;justify-content:space-between;gap:16px;padding:13px 0;border-bottom:1px solid #26334a;font-size:13px}.row:last-child{border-bottom:none}.row span{color:var(--muted)}.row b{text-align:right;color:#e7eefb}.card small{color:#8292aa;display:block;margin-top:13px;font-size:11px;line-height:1.5}.footer{text-align:center;color:#6e7e97;font-size:11px;letter-spacing:1.2px;margin-top:25px;padding:18px}
@media(max-width:600px){.container{padding:14px 12px 24px}.header{padding:18px;align-items:flex-start;flex-direction:column}.header:after{padding:8px 10px}.status-card{align-items:flex-start;flex-direction:column;gap:10px}.status-card:before{order:0}.status{order:1}.msg{order:2;text-align:left}.card{padding:17px}.info{padding:16px}.grid2{gap:9px}button{padding:10px 8px;font-size:13px;min-height:50px}}
</style>
</head>

<body>
<div class="container">

  <div class="header">
    <h1>☀️ Solar Panel Cleaning</h1>
    <p>ESP32 Web Control Dashboard</p>
  </div>

  <div class="status-card">
    <div id="status" class="status stopped">🔴 STOPPED</div>
    <div class="msg" id="message">--</div>
  </div>

  <div class="info-grid">
    <div class="info">
      <div class="info-title">RAIL</div>
      <div id="rail" class="info-value">---</div>
    </div>
    <div class="info">
      <div class="info-title">POSITION (steps)</div>
      <div id="pos" class="info-value">0</div>
    </div>
  </div>

  <div class="card">
    <h2>🤖 Automatic Control</h2>
    <p>กด START: ปั๊ม + แปรงทำงาน รางวิ่งชน Limit แล้วเด้งกลับไปชนอีกฝั่ง และเด้งกลับ</p>
    <div class="grid2">
      <button class="start" onclick="cmd('/start')">▶ START ALL</button>
      <button class="stop" onclick="cmd('/stop')">⛔ STOP ALL</button>
    </div>
    <small>STOP ALL เป็นคำสั่งผ่านซอฟต์แวร์ ไม่ใช่วงจรหยุดฉุกเฉินแบบตัดกำลัง</small>
  </div>

  <div class="card">
    <h2>🎮 Manual Control</h2>
    <div class="grid2">
      <button class="reverse" onclick="cmd('/run?dir=-1')">◀ Rail Left</button>
      <button class="forward" onclick="cmd('/run?dir=1')">Rail Right ▶</button>
    </div>
    <button class="stop full" onclick="cmd('/railstop')">■ Rail Stop</button>
    <div class="grid3">
      <button onclick="cmd('/brush?on=1')">Brush ON</button>
      <button onclick="cmd('/brush?on=0')">Brush OFF</button>
      <span></span>
      <button onclick="cmd('/pump?on=1')">Pump ON</button>
      <button onclick="cmd('/pump?on=0')">Pump OFF</button>
    </div>
  </div>

  <div class="card">
    <h2>⚡ Rail Speed</h2>
    <div class="speed-value">
      <span id="speedVal">400</span>
      <span style="font-size:15px;">steps/sec</span>
    </div>
    <input type="range" min="100" max="800" step="50" value="400" id="speed"
      oninput="speedVal.textContent=this.value"
      onchange="cmd('/speed?value='+this.value)">
  </div>

  <div class="card">
    <h2>📊 System Status</h2>
    <div class="row"><span>Cycle</span><b id="cycle">--</b></div>
    <div class="row"><span>Passes done</span><b id="legs">--</b></div>
    <div class="row"><span>Brush</span><b id="brush">--</b></div>
    <div class="row"><span>Pump</span><b id="pump">--</b></div>
    <div class="row"><span>Left Limit</span><b id="left">--</b></div>
    <div class="row"><span>Right Limit</span><b id="right">--</b></div>
  </div>

  <div class="footer">SOLARA · ESP32 Solar Panel Cleaner</div>
</div>

<script>
async function cmd(path) {
  try {
    const r = await fetch(path);
    const t = await r.text();
    if (!r.ok) alert(t);
    await refreshStatus();
  } catch(e) {
    alert('เชื่อมต่อ ESP32 ไม่ได้');
  }
}

const $ = id => document.getElementById(id);

async function refreshStatus() {
  try {
    const r = await fetch('/status');
    const s = await r.json();

    const st = $('status');
    if (s.running) { st.textContent = '🟢 RUNNING'; st.className = 'status run'; }
    else           { st.textContent = '🔴 STOPPED'; st.className = 'status stopped'; }

    $('message').textContent = s.message;
    $('rail').textContent  = s.rail;
    $('pos').textContent   = s.position;
    $('cycle').textContent = s.cycle;
    $('legs').textContent  = s.legsDone + ' / ' + s.legsTotal;
    $('brush').textContent = s.brush;
    $('pump').textContent  = s.pump;
    $('left').textContent  = s.left;
    $('right').textContent = s.right;
    $('speedVal').textContent = s.speed;
    $('speed').value = s.speed;
  } catch(e) {
    $('message').textContent = 'Connection lost';
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

  initButton(startButton);
  initButton(stopButton);

  stepIntervalUs = 1000000UL / speedSPS;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println();
  Serial.println("================================");
  Serial.println(" SOLARA CLEANING DASHBOARD");
  Serial.println("================================");
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

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
}
