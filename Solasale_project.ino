#include <WiFi.h>
#include <WebServer.h>

// ==================================================
// SOLARA : ระบบทำความสะอาดแผงโซลาร์เซลล์
// ราง (A4988 + NEMA17) + แปรง (BTS7960) + ปั๊ม (Relay)
// ==================================================

// ==================================================
// 1. ตั้งค่า Wi-Fi
// ==================================================
const char* WIFI_SSID = "Kat";
const char* WIFI_PASSWORD = "1234567890";

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

const char DASHBOARD[] PROGMEM =
  "<!doctype html>\n"
  "<html lang=\"th\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>SOLARA Dashboard</title>\n"
  "<style>\n"
  ":root{color-scheme:dark;--bg:#08111f;--panel:#111e30;--line:#263950;--text:#eaf2ff;--muted:#93a7c2;--green:#2de0a3;--blue:#65a8ff;--red:#ff647c;--gold:#ffc86b}*{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at 10% 0%,#153454 0,transparent 45%),var(--bg);font:15px system-ui,'Segoe UI',sans-serif;color:var(--text);min-height:100vh}.wrap{max-width:1000px;margin:auto;padding:24px}.head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:22px}.brand{display:flex;align-items:center;gap:12px}.logo{width:48px;height:48px;border-radius:15px;background:linear-gradient(135deg,#2de0a3,#258bff);display:grid;place-items:center;color:#071422;font-size:25px;font-weight:900}.eyebrow{font-size:11px;letter-spacing:2px;color:var(--green);font-weight:800}.title{font-size:25px;font-weight:800;letter-spacing:.5px}.pill{border:1px solid var(--line);background:#102137;padding:9px 13px;border-radius:30px;color:var(--muted);font-size:12px}.led{display:inline-block;width:8px;height:8px;background:var(--gold);border-radius:50%;margin-right:7px}.hero{padding:24px;border:1px solid #29415b;border-radius:22px;background:linear-gradient(120deg,#132b42,#101b2c);display:flex;justify-content:space-between;align-items:center;gap:20px;margin-bottom:16px}.hero h1{font-size:clamp(24px,4vw,36px);margin:7px 0}.hero p{color:var(--muted);margin:0}.hero-icon{font-size:54px;filter:drop-shadow(0 0 20px #2de0a344)}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px}.card{background:#101d2e;border:1px solid var(--line);border-radius:18px;padding:19px;min-width:0}.label{font-size:12px;color:var(--muted);margin-bottom:9px}.value{font-size:24px;font-weight:800;overflow-wrap:anywhere}.sub{font-size:12px;color:var(--muted);margin-top:7px}.section{font-size:14px;font-weight:800;margin:22px 0 12px}.controls{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.btn{border:1px solid #304760;border-radius:12px;background:#1a2d43;color:var(--text);padding:13px 12px;font-size:14px;font-weight:750;cursor:pointer;transition:.15s}.btn:hover{filter:brightness(1.15);transform:translateY(-1px)}.green{background:linear-gradient(135deg,#2de0a3,#13a97d);color:#061a19;border:0}.blue{background:linear-gradient(135deg,#65a8ff,#477de5);color:#071629;border:0}.red{background:#3a1c2a;color:#ff8a9b;border-color:#693044}.wide{grid-column:1/-1}.range{width:100%;accent-color:var(--green);margin-top:14px}.row{display:flex;justify-content:space-between;align-items:center;gap:12px}.tag{padding:5px 9px;border-radius:8px;background:#19382f;color:var(--green);font-size:11px;font-weight:800}.tag.warn{background:#3b2d1a;color:var(--gold)}.tag.off{background:#273345;color:#aebdd0}.footer{color:#71849c;text-align:center;font-size:11px;padding:25px 0 8px}#notice{font-size:12px;color:var(--muted);min-height:20px;margin-top:12px}@media(max-width:700px){.wrap{padding:15px}.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.hero{padding:18px}.hero-icon{font-size:40px}.title{font-size:21px}.card{padding:15px}}@media(max-width:430px){.grid{grid-template-columns:1fr 1fr}.value{font-size:20px}.pill{font-size:10px}.controls .btn{padding:12px 8px}}\n"
  "</style></head><body><main class=\"wrap\">\n"
  "<header class=\"head\"><div class=\"brand\"><div class=\"logo\">S</div><div><div class=\"eyebrow\">SMART SOLAR CARE</div><div class=\"title\">SOLARA <span style=\"color:#65a8ff\">CONTROL</span></div></div></div><div class=\"pill\"><span class=\"led\" id=\"led\"></span><span id=\"connection\">กำลังเชื่อมต่อ</span></div></header>\n"
  "<section class=\"hero\"><div><div class=\"eyebrow\">CLEANING SYSTEM / LIVE PANEL</div><h1>Solar Panel Cleaning</h1><p>แดชบอร์ดควบคุมระบบทำความสะอาดแผงโซลาร์เซลล์</p></div><div class=\"hero-icon\">☀️</div></section>\n"
  "<div class=\"grid\"><section class=\"card\"><div class=\"label\">สถานะระบบ</div><div class=\"value\" id=\"cycle\">--</div><div class=\"sub\" id=\"message\">กำลังอ่านสถานะ...</div></section><section class=\"card\"><div class=\"label\">รางเลื่อน</div><div class=\"value\" id=\"rail\">--</div><div class=\"sub\">ตำแหน่ง <span id=\"position\">0</span> steps</div></section><section class=\"card\"><div class=\"label\">ความเร็วราง</div><div class=\"value\"><span id=\"speedVal\">400</span><small style=\"font-size:12px;color:#93a7c2\"> steps/s</small></div><input id=\"speed\" class=\"range\" type=\"range\" min=\"100\" max=\"800\" step=\"50\" value=\"400\" oninput=\"speedVal.textContent=this.value\" onchange=\"send('/speed?value='+this.value)\"><div class=\"sub\">ปรับความเร็ว 100–800</div></section><section class=\"card\"><div class=\"row\"><div class=\"label\">มอเตอร์แปรง</div><span class=\"tag off\" id=\"brushTag\">OFF</span></div><div class=\"value\" id=\"brush\">ปิด</div><div class=\"sub\">BTS7960</div></section><section class=\"card\"><div class=\"row\"><div class=\"label\">ปั๊มน้ำ</div><span class=\"tag off\" id=\"pumpTag\">OFF</span></div><div class=\"value\" id=\"pump\">ปิด</div><div class=\"sub\">Relay output</div></section><section class=\"card\"><div class=\"label\">Limit Switch</div><div class=\"row\"><span>ซ้าย <b id=\"left\">--</b></span><span>ขวา <b id=\"right\">--</b></span></div><div class=\"sub\">สถานะสวิตช์ปลายราง</div></section></div>\n"
  "<div class=\"section\">การควบคุมระบบ</div><section class=\"card\"><div class=\"controls\"><button class=\"btn green\" onclick=\"send('/start')\">▶ เริ่มอัตโนมัติ</button><button class=\"btn red\" onclick=\"send('/stop')\">■ หยุดทั้งหมด</button><button class=\"btn blue\" onclick=\"send('/run?dir=1')\">→ เลื่อนรางขวา</button><button class=\"btn blue\" onclick=\"send('/run?dir=-1')\">← เลื่อนรางซ้าย</button><button class=\"btn\" onclick=\"send('/railstop')\">หยุดราง</button><button class=\"btn\" onclick=\"toggle('/brush?on=','brush')\">เปิด/ปิดแปรง</button><button class=\"btn wide\" onclick=\"toggle('/pump?on=','pump')\">เปิด/ปิดปั๊มน้ำ</button></div><div id=\"notice\">พร้อมใช้งานเมื่อเชื่อมต่อ ESP32</div></section>\n"
  "<div class=\"footer\">SOLARA · ESP32 LOCAL DASHBOARD · STATUS REFRESH EVERY 1.5s</div></main>\n"
  "<script>\n"
  "async function send(path){const n=document.getElementById('notice');try{const r=await fetch(path,{cache:'no-store'});const t=await r.text();n.textContent=r.ok?'✓ '+t:'⚠ '+t;if(!r.ok)n.style.color='#ff8a9b';else n.style.color='#2de0a3';await refresh()}catch(e){n.textContent='เชื่อมต่อ ESP32 ไม่สำเร็จ';n.style.color='#ff8a9b'}}\n"
  "async function toggle(path,key){const on=document.getElementById(key).textContent==='ปิด'?1:0;await send(path+on)}\n"
  "async function refresh(){try{const r=await fetch('/status',{cache:'no-store'});if(!r.ok)throw 0;const d=await r.json();document.getElementById('connection').textContent='ESP32 Online';document.getElementById('led').style.background='#2de0a3';document.getElementById('cycle').textContent=d.cycle||'--';document.getElementById('message').textContent=d.message||'';document.getElementById('rail').textContent=d.rail==='STOPPED'?'หยุด':d.rail==='RIGHT'?'กำลังไปขวา':'กำลังไปซ้าย';document.getElementById('position').textContent=d.position??0;document.getElementById('speedVal').textContent=d.speed??400;document.getElementById('speed').value=d.speed??400;document.getElementById('brush').textContent=d.brush==='ON'?'เปิด':'ปิด';document.getElementById('pump').textContent=d.pump==='ON'?'เปิด':'ปิด';document.getElementById('brushTag').textContent=d.brush||'OFF';document.getElementById('pumpTag').textContent=d.pump||'OFF';document.getElementById('brushTag').className='tag '+(d.brush==='ON'?'':'off');document.getElementById('pumpTag').className='tag '+(d.pump==='ON'?'':'off');document.getElementById('left').textContent=d.left==='PRESSED'?'กดอยู่':'ปกติ';document.getElementById('right').textContent=d.right==='PRESSED'?'กดอยู่':'ปกติ'}catch(e){document.getElementById('connection').textContent='ESP32 Offline';document.getElementById('led').style.background='#ff647c'}}refresh();setInterval(refresh,1500);\n"
  "</script></body></html>\n";

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
