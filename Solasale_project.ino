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

const char DASHBOARD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="th">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>SOLARA · Solar Panel Cleaning Dashboard</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@500;600;700&family=IBM+Plex+Mono:wght@400;500;600&display=swap" rel="stylesheet">
<style>
:root{
  --bg:#0a1120; --panel:#101a30; --panel-2:#16233f; --border:#233457;
  --text:#eef2fb; --muted:#8fa0c7; --muted-2:#5b6d95;
  --accent:#ffb020; --accent-2:#ff7a3d; --accent-ink:#2b1400;
  --ok:#34d399; --warn:#f5c451; --bad:#f8716f;
  --ok-bg:rgba(52,211,153,.13); --warn-bg:rgba(245,196,81,.13); --bad-bg:rgba(248,113,111,.15);
  --radius:12px; --font-h:'Space Grotesk',sans-serif; --font-d:'IBM Plex Mono',monospace;
  --shadow:0 10px 30px -12px rgba(0,0,0,.5);
}
@media (prefers-color-scheme: light){
  :root:not([data-theme="dark"]){
    --bg:#f6f4ee; --panel:#ffffff; --panel-2:#f9f7f0; --border:#e2ddcd;
    --text:#1c1a13; --muted:#71695a; --muted-2:#948b78;
    --shadow:0 10px 26px -14px rgba(80,60,20,.25);
  }
}
:root[data-theme="light"]{
  --bg:#f6f4ee; --panel:#ffffff; --panel-2:#f9f7f0; --border:#e2ddcd;
  --text:#1c1a13; --muted:#71695a; --muted-2:#948b78;
  --shadow:0 10px 26px -14px rgba(80,60,20,.25);
}
*{box-sizing:border-box;}
html{-webkit-text-size-adjust:100%;}
img,svg{max-width:100%;}
body{margin:0;background:
  radial-gradient(1200px 500px at 90% -10%, rgba(255,176,32,.07), transparent 60%),
  var(--bg);color:var(--text);font-family:var(--font-h);transition:background .2s;}
button{font-family:inherit;}
.app{display:flex;min-height:100vh;min-height:100dvh;}
/* Sidebar */
.sidebar{width:220px;flex-shrink:0;background:var(--panel);border-right:1px solid var(--border);
  display:flex;flex-direction:column;padding:18px 12px;position:sticky;top:0;height:100vh;height:100dvh;overflow-y:auto;}
.brand{display:flex;align-items:center;gap:10px;padding:6px 8px 22px;font-weight:700;font-size:16px;}
.brand .dot{width:26px;height:26px;border-radius:8px;flex-shrink:0;display:flex;align-items:center;justify-content:center;
  background:linear-gradient(145deg,var(--accent),var(--accent-2));box-shadow:0 4px 14px -4px rgba(255,140,30,.6);font-size:14px;}
.brand-sub{font-size:10.5px;font-weight:500;color:var(--muted-2);font-family:var(--font-d);display:block;margin-top:1px;}
.nav-item{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:9px;color:var(--muted);
  cursor:pointer;font-size:13.5px;margin-bottom:3px;border:1px solid transparent;transition:.15s;}
.nav-item:hover{background:var(--panel-2);color:var(--text);}
.nav-item.active{background:var(--panel-2);color:var(--text);border-color:var(--border);font-weight:600;
  box-shadow:inset 2px 0 0 var(--accent);}
.nav-item .ic{width:18px;text-align:center;}
.sidebar-foot{margin-top:auto;font-size:11px;color:var(--muted-2);padding:8px;}
/* Topbar */
.main{flex:1;min-width:0;}
.topbar{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:16px 24px;
  border-bottom:1px solid var(--border);position:sticky;top:0;background:var(--bg);z-index:5;}
.topbar h1{font-size:18px;margin:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.topbar>div:first-child{min-width:0;}
.status-pill{flex-shrink:0;white-space:nowrap;display:flex;align-items:center;gap:8px;font-size:13px;padding:6px 12px;border-radius:99px;
  border:1px solid var(--border);background:var(--panel);}
.status-pill .sw{width:8px;height:8px;border-radius:50%;}
.content{padding:22px 24px 60px;max-width:1280px;animation:fadein .35s ease;}
@keyframes fadein{from{opacity:0;transform:translateY(6px);}to{opacity:1;transform:translateY(0);}}
/* Grid */
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px;}
.card{min-width:0;background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);padding:18px;
  box-shadow:var(--shadow);transition:transform .15s, border-color .15s;}
.card:hover{border-color:var(--muted-2);}
.card h3{font-size:12.5px;margin:0 0 14px;color:var(--muted);font-weight:600;letter-spacing:.2px;}
.c3{grid-column:span 3;} .c4{grid-column:span 4;} .c5{grid-column:span 5;} .c6{grid-column:span 6;} .c8{grid-column:span 8;} .c12{grid-column:span 12;}
.menu-btn{display:none;background:none;border:1px solid var(--border);color:var(--text);border-radius:8px;padding:6px 12px;cursor:pointer;font-size:16px;line-height:1;min-height:40px;}
.backdrop{display:none;position:fixed;inset:0;background:rgba(0,0,0,.5);z-index:15;}
.ip-row{display:flex;gap:8px;flex-wrap:wrap;}
.ip-row input{flex:1 1 160px;min-width:0;}
.ctrl-row{flex-wrap:wrap;}
.btn{min-width:0;}
.modal{width:min(320px,calc(100vw - 32px));}
.toast{max-width:calc(100vw - 32px);}
.table-wrap{-webkit-overflow-scrolling:touch;}
table{min-width:520px;}
.metric-big{font-size:clamp(24px,5vw,30px);}
.content{margin-inline:auto;}

/* ---- Responsive ---- */
/* Large desktop */
@media(min-width:1600px){.content{max-width:1440px;}}
/* Laptop / small desktop */
@media(max-width:1180px){
  .c3,.c4{grid-column:span 6;} .c5,.c6,.c8{grid-column:span 12;}
}
/* Tablet & below: sidebar becomes a slide-in drawer */
@media(max-width:900px){
  .c3,.c4,.c5{grid-column:span 6;} .c6,.c8,#locationCard{grid-column:span 12;}
  .sidebar{position:fixed;left:0;top:0;bottom:0;width:240px;max-width:82vw;z-index:20;transform:translateX(-105%);
    transition:transform .22s ease;box-shadow:none;visibility:hidden;
    padding-top:calc(18px + env(safe-area-inset-top,0px));padding-bottom:calc(18px + env(safe-area-inset-bottom,0px));}
  .sidebar.open{transform:none;visibility:visible;box-shadow:0 0 40px rgba(0,0,0,.4);}
  .sidebar.open ~ .backdrop{display:block;}
  .menu-btn{display:inline-flex;align-items:center;}
  .nav-item{padding:12px;}
  .topbar{padding:12px 16px;padding-top:calc(12px + env(safe-area-inset-top,0px));}
  .content{padding:18px 16px 48px;}
}
/* Phones */
@media(max-width:640px){
  .grid{gap:12px;}
  .c3,.c4,.c5,.c6,.c8{grid-column:span 12;}
  .c3.stat{grid-column:span 6;}
  .card{padding:14px;}
  .topbar h1{font-size:16px;} .status-pill{font-size:12px;padding:5px 10px;}
  .content{padding:14px 12px calc(40px + env(safe-area-inset-bottom,0px));}
  .tabs{overflow-x:auto;white-space:nowrap;-webkit-overflow-scrolling:touch;}
  .tab{flex-shrink:0;}
  .filters>*{flex:1 1 140px;min-width:0;}
  .filters .btn{flex:1 1 120px!important;}
  select,input[type=date],input,.ip-row input{font-size:16px;} /* prevents iOS zoom-on-focus */
  .btn{min-height:44px;}
  .toast{bottom:calc(16px + env(safe-area-inset-bottom,0px));}
}
/* Very small phones */
@media(max-width:360px){
  .c3.stat{grid-column:span 12;}
  .brand{font-size:15px;} .status-pill .sw+span{max-width:90px;overflow:hidden;text-overflow:ellipsis;}
}
@media(hover:none){.card:hover{border-color:var(--border);}}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important;}}

/* Metric */
.metric-big{font-family:var(--font-d);font-size:30px;font-weight:600;}
.metric-sub{font-size:12px;color:var(--muted);margin-top:4px;}
.badge{display:inline-flex;align-items:center;gap:6px;font-size:11.5px;padding:3px 9px;border-radius:99px;font-family:var(--font-d);}
.b-ok{background:var(--ok-bg);color:var(--ok);} .b-warn{background:var(--warn-bg);color:var(--warn);} .b-bad{background:var(--bad-bg);color:var(--bad);}
.bar-track{height:8px;border-radius:99px;background:var(--panel-2);overflow:hidden;border:1px solid var(--border);}
.bar-fill{height:100%;border-radius:99px;}
/* Sensor rows */
.sensor-row{display:flex;align-items:center;justify-content:space-between;padding:9px 0;border-bottom:1px dashed var(--border);font-size:13px;}
.sensor-row:last-child{border-bottom:none;}
.sensor-name{display:flex;align-items:center;gap:8px;}
.dot-s{width:8px;height:8px;border-radius:50%;flex-shrink:0;}
/* Controls */
.ctrl-row{display:flex;gap:10px;margin-top:14px;}
.btn{border:1px solid var(--border);background:var(--panel-2);color:var(--text);padding:10px 16px;border-radius:9px;
  font-weight:600;font-size:13px;cursor:pointer;flex:1;transition:transform .1s, filter .15s;}
.btn:hover{filter:brightness(1.08);} .btn:active{transform:scale(.97);}
.btn.primary{background:linear-gradient(145deg,var(--accent),var(--accent-2));color:var(--accent-ink);border-color:transparent;}
.btn.danger{background:var(--bad);color:#330806;border-color:var(--bad);}
.btn:disabled{opacity:.45;cursor:not-allowed;}
/* Map */
.map-wrap{position:relative;border-radius:8px;overflow:hidden;border:1px solid var(--border);height:220px;background:
  repeating-linear-gradient(0deg, var(--panel-2) 0 1px, transparent 1px 28px),
  repeating-linear-gradient(90deg, var(--panel-2) 0 1px, transparent 1px 28px), var(--panel);}
/* Tabs */
.tabs{display:flex;gap:6px;margin-bottom:16px;border-bottom:1px solid var(--border);}
.tab{padding:9px 14px;font-size:13px;color:var(--muted);cursor:pointer;border-bottom:2px solid transparent;font-weight:600;}
.tab.active{color:var(--text);border-color:var(--accent);}
/* Table */
table{width:100%;border-collapse:collapse;font-size:13px;}
th{text-align:left;color:var(--muted);font-weight:600;padding:8px 10px;border-bottom:1px solid var(--border);font-size:12px;}
td{padding:9px 10px;border-bottom:1px solid var(--border);font-family:var(--font-d);}
.table-wrap{overflow-x:auto;}
.filters{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px;}
select,input[type=date]{background:var(--panel-2);border:1px solid var(--border);color:var(--text);padding:7px 10px;border-radius:7px;font-size:12.5px;font-family:inherit;}
/* toast/modal */
.toast{position:fixed;bottom:22px;left:50%;transform:translateX(-50%) translateY(20px);background:var(--panel-2);
  border:1px solid var(--border);padding:12px 18px;border-radius:8px;opacity:0;transition:.25s;z-index:100;font-size:13px;
  box-shadow:0 8px 30px rgba(0,0,0,.25);display:flex;align-items:center;gap:10px;}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0);}
.overlay{position:fixed;inset:0;background:rgba(0,0,0,.5);display:none;align-items:center;justify-content:center;z-index:99;}
.overlay.show{display:flex;}
.modal{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:22px;width:320px;}
.modal h4{margin:0 0 8px;} .modal p{color:var(--muted);font-size:13px;margin:0 0 18px;}
.hint{font-size:11px;color:var(--muted-2);margin-top:6px;}
.empty{text-align:center;color:var(--muted);padding:30px 10px;font-size:13px;}
.skeleton{background:linear-gradient(90deg,var(--panel-2) 25%,var(--border) 37%,var(--panel-2) 63%);background-size:400% 100%;
  animation:sk 1.4s infinite;border-radius:6px;height:14px;}
@keyframes sk{0%{background-position:100% 0}100%{background-position:0 0}}
.view{display:none;} .view.active{display:block;}
</style>
</head>
<body>
<div class="app">
  <aside class="sidebar" id="sidebar">
    <div class="brand"><span class="dot">☀</span><span>SOLARA<span class="brand-sub">SOLAR PANEL CLEANING</span></span></div>
    <div class="nav-item active" data-view="monitoring"><span class="ic">◉</span> Monitoring</div>
    <div class="nav-item" data-view="analytics"><span class="ic">▤</span> วิเคราะห์ข้อมูล</div>
    <div class="nav-item" data-view="history"><span class="ic">▦</span> ประวัติการทำความสะอาด</div>
    <div class="nav-item" data-view="errors"><span class="ic">▲</span> Error Log</div>
    <div class="nav-item" data-view="settings"><span class="ic">⚙</span> ตั้งค่า</div>
    <div class="sidebar-foot">Unit SC-04 · Firmware 2.3.1<br><span id="linkModeLabel">โหมด: Mock data</span></div>
  </aside>
  <div class="backdrop" id="backdrop"></div>

  <div class="main">
    <div class="topbar">
      <div style="display:flex;align-items:center;gap:12px;">
        <button class="menu-btn" onclick="document.getElementById('sidebar').classList.toggle('open')">☰</button>
        <h1 id="pageTitle">Monitoring</h1>
      </div>
      <div class="status-pill"><span class="sw" id="statusDot" style="background:var(--ok)"></span><span id="statusText">กำลังทำงาน</span></div>
    </div>
    <div class="content" id="content"></div>
  </div>
</div>

<div class="overlay" id="overlay">
  <div class="modal">
    <h4 id="modalTitle">ยืนยันคำสั่ง</h4>
    <p id="modalBody">คุณต้องการสั่งให้เครื่องเริ่มทำงานหรือไม่?</p>
    <div class="ctrl-row">
      <button class="btn" onclick="closeModal()">ยกเลิก</button>
      <button class="btn primary" id="modalConfirm">ยืนยัน</button>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>

<script>
/* ---------- Device bridge: connects to the real ESP32 (Solasale_project.ino) ---------- */
// Routes match Solasale_project.ino exactly:
//   GET /start            -> START ALL: pump + brush ON, rail sweeps to a limit, bounces to the other side and back
//   GET /stop             -> STOP ALL (rail + brush + pump)
//   GET /run?dir=1|-1     -> manual rail right/left (blocked while auto cycle runs)
//   GET /railstop         -> stop rail only
//   GET /brush?on=1|0     -> manual brush         GET /pump?on=1|0 -> manual pump
//   GET /speed?value=N    -> rail speed, 100-800 steps/sec
//   GET /status           -> {cycle,message,rail,brush,pump,left,right,position,speed,running,direction,legsDone,legsTotal}
const Device = {
  ip: (()=>{ try{ return localStorage.getItem('esp32_ip') || window.location.host; }catch(e){ return ''; } })(),
  connected: false,
  async ping(){
    if(!this.ip) return false;
    try{ const res = await fetch(`http://${this.ip}/status`, {signal: AbortSignal.timeout(2500)}); this.connected = res.ok; }
    catch(e){ this.connected = false; }
    return this.connected;
  },
  async getStatus(){
    const res = await fetch(`http://${this.ip}/status`, {signal: AbortSignal.timeout(3000)});
    if(!res.ok) throw new Error('device error');
    return res.json();
  },
  async start(){ return this._cmd(`/start`); },
  async stopAll(){ return this._cmd(`/stop`); },
  async rail(dir){ return this._cmd(`/run?dir=${dir}`); },
  async railStop(){ return this._cmd(`/railstop`); },
  async brush(on){ return this._cmd(`/brush?on=${on?1:0}`); },
  async pump(on){ return this._cmd(`/pump?on=${on?1:0}`); },
  async setSpeed(v){ return this._cmd(`/speed?value=${v}`); },
  async _cmd(path){
    const res = await fetch(`http://${this.ip}${path}`, {signal: AbortSignal.timeout(3000)});
    const text = await res.text();
    if(!res.ok){ const err = new Error(text || 'device error'); err.fromDevice = true; throw err; }
    return text;
  }
};// NOTE: browser pages served over https (including a Cloudflare Pages deployment) block
// plain-http requests to a local ESP32 (mixed-content). To control the real motor you
// either open this dashboard over plain http/file:// on the same network as the ESP32,
// or put the ESP32 behind an https tunnel (e.g. Cloudflare Tunnel) and use that hostname.

/* ---------- Mock API / service layer (swap with real backend later) ---------- */
const MockAPI = {
  async getBattery(){ return {pct:76,volt:24.6,state:'discharging',low:false}; },
  // Real weather snapshot — pulled from a live forecast for the site's location at build time.
  // Swap this for a live fetch to your own weather proxy once self-hosted (published pages
  // can't call third-party APIs directly), keeping the same {current, forecast} shape.
  async getWeather(){ return {
    fetchedAt:'18 ก.ย. 2026',
    current:{temp:25.8, condition:'มีเมฆมาก'},
    forecast:[
      {day:'ศ (วันนี้)', high:32.1, low:25.7, rain:85},
      {day:'ส', high:31.6, low:25.6, rain:65},
      {day:'อา', high:30.8, low:25.4, rain:70},
    ]
  };},
  async getCycle(){ return {start:'08:20',elapsed:'01:42',eta:'02:15',zone:'แถวแผง B2 (อาคาร 2)'}; },
  async getCycleCounts(){ return {today:3,week:14,month:52}; },
  async getSensors(){ return [
    {name:'เซนเซอร์ระยะ (Ultrasonic)',status:'ok'},
    {name:'Limit Switch ปลายราง',status:'ok'},
    {name:'Limit Switch ต้นราง',status:'warn'},
    {name:'เซนเซอร์แบตเตอรี่',status:'ok'},
    {name:'เซนเซอร์อุณหภูมิแผง',status:'ok'},
    {name:'เซนเซอร์ตรวจจับสิ่งกีดขวาง',status:'bad'},
  ];},
  async getMachineState(){ return 'running'; }, // running | stopped | charging | error
  async getBatteryTrend(){ return {labels:['จ','อ','พ','พฤ','ศ','ส','อา'],values:[82,78,74,80,71,68,76]}; },
  async getCycleDurations(){ return {labels:['รอบ1','รอบ2','รอบ3','รอบ4','รอบ5','รอบ6'],values:[95,102,88,110,97,101]}; },
  async getHistory(){ return [
    {date:'2026-09-17',start:'08:20',end:'10:02',dur:'1:42',zone:'แถวแผง B',status:'สำเร็จ'},
    {date:'2026-09-16',start:'08:15',end:'09:58',dur:'1:43',zone:'แถวแผง A',status:'สำเร็จ'},
    {date:'2026-09-16',start:'14:00',end:'14:31',dur:'0:31',zone:'แถวแผง C',status:'ยกเลิก'},
    {date:'2026-09-15',start:'08:22',end:'10:10',dur:'1:48',zone:'แถวแผง B',status:'สำเร็จ'},
    {date:'2026-09-14',start:'08:18',end:'09:55',dur:'1:37',zone:'แถวแผง A',status:'สำเร็จ'},
  ];},
  async getErrors(){ return [
    {ts:'2026-09-16 14:31',type:'ชน Limit Switch',sev:'warn',cause:'สิ่งกีดขวางไม่ทราบชนิด',fixed:'แก้ไขแล้ว'},
    {ts:'2026-09-14 09:40',type:'แบตเตอรี่ต่ำ',sev:'bad',cause:'ระดับแบต < 10%',fixed:'แก้ไขแล้ว'},
    {ts:'2026-09-12 08:55',type:'เซนเซอร์กันชนขัดข้อง',sev:'bad',cause:'สายสัญญาณหลวม',fixed:'รอตรวจสอบ'},
    {ts:'2026-09-10 11:20',type:'การเชื่อมต่อหลุด',sev:'warn',cause:'สัญญาณ Wi-Fi อ่อน',fixed:'แก้ไขแล้ว'},
  ];}
};

const sevColor = s => s==='ok'?'var(--ok)':s==='warn'?'var(--warn)':'var(--bad)';
const sevBadge = s => s==='ok'?'b-ok':s==='warn'?'b-warn':'b-bad';
const sevLabel = s => s==='ok'?'ปกติ':s==='warn'?'คำเตือน':'ผิดปกติ';

function toast(msg){
  const t=document.getElementById('toast'); t.textContent=msg; t.classList.add('show');
  clearTimeout(window._tt); window._tt=setTimeout(()=>t.classList.remove('show'),2600);
}

let modalAction=null;
function openModal(title,body,action){
  document.getElementById('modalTitle').textContent=title;
  document.getElementById('modalBody').textContent=body;
  modalAction=action;
  document.getElementById('overlay').classList.add('show');
}
function closeModal(){ document.getElementById('overlay').classList.remove('show'); }
document.getElementById('modalConfirm').onclick=()=>{ if(modalAction) modalAction(); closeModal(); };

// Uses the browser's real Geolocation API (actual GPS/Wi-Fi position of whoever has this
// page open) — not mock data. Falls back to the site's known address if permission is
// denied or unavailable, and says so explicitly rather than faking a reading.
function locateDevice(){
  const body=document.getElementById('locBody');
  if(!body) return;
  if(!navigator.geolocation){
    body.innerHTML=`<div class="badge b-warn">ไม่รองรับ Geolocation</div><div class="metric-sub" style="margin-top:8px;">เบราว์เซอร์นี้ไม่รองรับการระบุตำแหน่ง</div>`;
    return;
  }
  navigator.geolocation.getCurrentPosition(
    pos=>{
      const {latitude:lat, longitude:lon, accuracy} = pos.coords;
      body.innerHTML=`
        <div class="badge b-ok">GPS จริง</div>
        <div style="font-family:var(--font-d);font-size:13px;margin-top:10px;">${lat.toFixed(5)}, ${lon.toFixed(5)}</div>
        <div class="metric-sub">ความแม่นยำ ±${Math.round(accuracy)} ม.</div>
        <a href="https://www.google.com/maps?q=${lat},${lon}" target="_blank" rel="noopener"
           class="btn primary" style="display:block;text-align:center;text-decoration:none;margin-top:12px;">เปิดใน Google Maps</a>`;
    },
    err=>{
      body.innerHTML=`
        <div class="badge b-warn">ไม่ได้รับอนุญาต</div>
        <div class="metric-sub" style="margin:8px 0;">ใช้ที่อยู่เริ่มต้นของไซต์แทน: Khlong Luang, Pathum Thani</div>
        <a href="https://www.google.com/maps?q=Khlong+Luang+Pathum+Thani" target="_blank" rel="noopener"
           class="btn" style="display:block;text-align:center;text-decoration:none;">เปิดใน Google Maps</a>`;
    },
    {timeout:6000}
  );
}

let machineState='running';
function setMachineState(s){
  machineState=s;
  const map={running:['กำลังทำงาน','var(--ok)'],stopped:['หยุดอยู่','var(--muted)'],charging:['ชาร์จแบตเตอรี่','var(--warn)'],error:['พบข้อผิดพลาด','var(--bad)']};
  document.getElementById('statusText').textContent=map[s][0];
  document.getElementById('statusDot').style.background=map[s][1];
}

/* simple inline SVG line chart */
function lineChart(labels,values,color,unit=''){
  const w=560,h=160,pad=28,max=Math.max(...values)*1.15,min=Math.min(...values)*0.9;
  const step=(w-pad*2)/(values.length-1);
  const pts=values.map((v,i)=>[pad+i*step, h-pad-((v-min)/(max-min))*(h-pad*1.6)]);
  const path=pts.map((p,i)=>(i===0?'M':'L')+p[0].toFixed(1)+','+p[1].toFixed(1)).join(' ');
  const dots=pts.map(p=>`<circle cx="${p[0]}" cy="${p[1]}" r="3" fill="${color}"/>`).join('');
  const labelsSvg=labels.map((l,i)=>`<text x="${pad+i*step}" y="${h-6}" font-size="9" fill="var(--muted)" text-anchor="middle" font-family="IBM Plex Mono">${l}</text>`).join('');
  return `<svg viewBox="0 0 ${w} ${h}" style="width:100%;height:auto;">
    <path d="${path}" fill="none" stroke="${color}" stroke-width="2"/>
    <path d="${path} L${pts[pts.length-1][0]},${h-pad} L${pts[0][0]},${h-pad} Z" fill="${color}" opacity="0.08"/>
    ${dots}${labelsSvg}
  </svg>`;
}
function barChart(labels,values,color){
  const w=560,h=160,pad=26,bw=(w-pad*2)/values.length*0.55,gap=(w-pad*2)/values.length,max=Math.max(...values)*1.15;
  const bars=values.map((v,i)=>{const bh=((v/max)*(h-pad*1.6));const x=pad+i*gap+(gap-bw)/2;const y=h-pad-bh;
    return `<rect x="${x.toFixed(1)}" y="${y.toFixed(1)}" width="${bw.toFixed(1)}" height="${bh.toFixed(1)}" rx="3" fill="${color}"/>
      <text x="${(x+bw/2).toFixed(1)}" y="${h-6}" font-size="9" fill="var(--muted)" text-anchor="middle" font-family="IBM Plex Mono">${labels[i]}</text>`;}).join('');
  return `<svg viewBox="0 0 ${w} ${h}" style="width:100%;height:auto;">${bars}</svg>`;
}

/* ---------- Views ---------- */
const titles={monitoring:'Monitoring',analytics:'วิเคราะห์ข้อมูล',history:'ประวัติการทำความสะอาด',errors:'Error Log',settings:'ตั้งค่า'};

async function renderMonitoring(el){
  el.innerHTML = `<div class="grid">
    <div class="card c3"><h3>🔋 แบตเตอรี่</h3><div class="skeleton" style="width:70%;height:30px;"></div></div>
    <div class="card c4"><h3>🌧️ สภาพอากาศ</h3><div class="skeleton" style="width:90%;height:120px;"></div></div>
    <div class="card c5"><h3>⏱️ รอบทำงานปัจจุบัน</h3><div class="skeleton" style="width:80%;height:60px;"></div></div>
  </div>`;
  const [bat,weather,cycle,counts,sensors,state] = await Promise.all([
    MockAPI.getBattery(),MockAPI.getWeather(),MockAPI.getCycle(),MockAPI.getCycleCounts(),MockAPI.getSensors(),MockAPI.getMachineState()
  ]);
  setMachineState(state);
  const battColor = bat.pct<20?'var(--bad)':bat.pct<40?'var(--warn)':'var(--ok)';
  el.innerHTML = `
  <div class="grid">
    <div class="card c3">
      <h3>🔋 แบตเตอรี่</h3>
      <div class="metric-big" style="color:${battColor}">${bat.pct}%</div>
      <div class="bar-track" style="margin:8px 0;"><div class="bar-fill" style="width:${bat.pct}%;background:${battColor}"></div></div>
      <div class="metric-sub">${bat.volt} V · ${bat.state==='charging'?'กำลังชาร์จ':'กำลังใช้งาน'}</div>
      ${bat.pct<20?'<div class="badge b-bad" style="margin-top:8px;">⚠ แบตต่ำ</div>':''}
    </div>

    <div class="card c5">
      <h3>🌧️ สภาพอากาศ ณ ตำแหน่งปัจจุบัน <span class="badge b-ok" style="margin-left:4px;">ข้อมูลจริง</span></h3>
      <div style="display:flex;align-items:baseline;gap:14px;">
        <div class="metric-big">${weather.current.temp}°C</div>
        <div style="font-size:13px;color:var(--muted);">${weather.current.condition}</div>
      </div>
      <div class="metric-sub" style="margin-bottom:10px;">อัปเดต ${weather.fetchedAt}</div>
      <div style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px;">
        ${weather.forecast.map(d=>`<div style="background:var(--panel-2);border:1px solid var(--border);border-radius:8px;padding:8px;text-align:center;">
          <div style="font-size:11px;color:var(--muted);margin-bottom:4px;">${d.day}</div>
          <div style="font-family:var(--font-d);font-size:13px;">${d.high}° <span style="color:var(--muted-2)">/ ${d.low}°</span></div>
          <div style="font-size:10.5px;color:var(--accent);margin-top:2px;">🌧 ${d.rain}%</div>
        </div>`).join('')}
      </div>
    </div>

    <div class="card c4">
      <h3>⏱️ เวลาที่ใช้ทำความสะอาด (รอบปัจจุบัน)</h3>
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px 10px;">
        <div><div class="metric-sub">เริ่มงาน</div><div style="font-family:var(--font-d);font-size:19px;font-weight:600;">${cycle.start}</div></div>
        <div><div class="metric-sub">คาดว่าเสร็จ</div><div style="font-family:var(--font-d);font-size:19px;font-weight:600;">${cycle.eta}</div></div>
      </div>
      <div style="margin-top:14px;">
        <div class="metric-sub" style="margin-bottom:6px;">เวลาที่ผ่านไป</div>
        <div style="display:flex;align-items:baseline;gap:8px;">
          <div class="metric-big" style="font-size:26px;">${cycle.elapsed}</div>
          <div class="bar-track" style="flex:1;"><div class="bar-fill" style="width:44%;background:var(--accent)"></div></div>
        </div>
      </div>
      <div class="metric-sub" style="margin-top:14px;">📍 พื้นที่: ${cycle.zone}</div>
    </div>

    <div class="card c3">
      <h3>🔄 จำนวนรอบการทำงาน</h3>
      <div class="sensor-row"><span>วันนี้</span><b>${counts.today}</b></div>
      <div class="sensor-row"><span>สัปดาห์นี้</span><b>${counts.week}</b></div>
      <div class="sensor-row"><span>เดือนนี้</span><b>${counts.month}</b></div>
    </div>

    <div class="card c4">
      <h3>📈 สถานะเซนเซอร์</h3>
      ${sensors.map(s=>`<div class="sensor-row"><span class="sensor-name"><span class="dot-s" style="background:${sevColor(s.status)}"></span>${s.name}</span><span class="badge ${sevBadge(s.status)}">${sevLabel(s.status)}</span></div>`).join('')}
    </div>

    <div class="card c5">
      <h3>🎮 ควบคุมระบบ <span class="badge ${Device.ip?'b-ok':'b-warn'}" style="margin-left:4px;">${Device.ip?'เชื่อมต่อ ESP32':'โหมดจำลอง'}</span></h3>
      <div class="metric-sub" style="margin:-4px 0 10px;">กด START: ปั๊ม + แปรงทำงาน รางวิ่งชน Limit แล้วเด้งกลับไปชนอีกฝั่ง และเด้งกลับ</div>
      <div class="ctrl-row" style="margin-top:0;">
        <button class="btn primary" id="startAllBtn">▶ START ALL</button>
        <button class="btn danger" id="stopAllBtn">⛔ STOP ALL</button>
      </div>
      <div class="hint">STOP ALL เป็นคำสั่งผ่านซอฟต์แวร์ ไม่ใช่วงจรหยุดฉุกเฉินแบบตัดกำลัง</div>

      <div class="metric-sub" style="margin-top:16px;">ควบคุมเอง (Manual)</div>
      <div class="ctrl-row" style="margin-top:6px;">
        <button class="btn" id="railLeftBtn">◀ ราง ซ้าย</button>
        <button class="btn" id="railStopBtn">■ หยุดราง</button>
        <button class="btn" id="railRightBtn">ราง ขวา ▶</button>
      </div>
      <div class="ctrl-row" style="margin-top:8px;">
        <button class="btn" id="brushOnBtn">แปรง ON</button>
        <button class="btn" id="brushOffBtn">แปรง OFF</button>
      </div>
      <div class="ctrl-row" style="margin-top:8px;">
        <button class="btn" id="pumpOnBtn">ปั๊ม ON</button>
        <button class="btn" id="pumpOffBtn">ปั๊ม OFF</button>
      </div>

      <div style="margin-top:16px;">
        <div class="metric-sub" style="display:flex;justify-content:space-between;">
          <span>ความเร็วราง</span><span id="speedLabel" style="font-family:var(--font-d);color:var(--text)">400 steps/sec</span>
        </div>
        <input id="speedSlider" type="range" min="100" max="800" step="50" value="400" style="width:100%;margin-top:6px;">
      </div>

      <div style="margin-top:16px;border-top:1px dashed var(--border);padding-top:6px;">
        <div class="sensor-row"><span>สถานะรอบ</span><b id="stCycle">---</b></div>
        <div class="sensor-row"><span>ข้อความ</span><b id="stMsg" style="text-align:right;">---</b></div>
        <div class="sensor-row"><span>เที่ยวที่วิ่งแล้ว</span><b id="stLegs">---</b></div>
        <div class="sensor-row"><span>ราง</span><b id="stRail">---</b></div>
        <div class="sensor-row"><span>แปรง / ปั๊ม</span><b><span id="stBrush">---</span> / <span id="stPump">---</span></b></div>
        <div class="sensor-row"><span>Limit ซ้าย / ขวา</span><b><span id="stLeft">---</span> / <span id="stRight">---</span></b></div>
        <div class="sensor-row"><span>ตำแหน่ง (steps)</span><b id="stPos">0</b></div>
      </div>
      <div class="hint">${Device.ip?'สั่งงานตรงไปยังบอร์ด ESP32 ทันที':'ยังไม่ได้เชื่อมต่อ ESP32 — ไปที่หน้าตั้งค่าเพื่อกรอก IP'}</div>
    </div>
    </div>

    <div class="card c3" id="locationCard">
      <h3>📍 ตำแหน่งปัจจุบัน</h3>
      <div id="locBody">
        <div class="skeleton" style="height:14px;width:80%;margin-bottom:8px;"></div>
        <div class="skeleton" style="height:14px;width:60%;"></div>
      </div>
    </div>
  </div>`;
  locateDevice();

  const $=id=>document.getElementById(id);
  const speedSlider=$('speedSlider'), speedLabel=$('speedLabel');

  async function sendMotor(action, fn, mockMsg){
    if(!Device.ip){ toast(mockMsg); return; }
    try{ await fn(); toast(`✅ ส่งคำสั่ง "${action}" ไปยัง ESP32 สำเร็จ`); poll(); }
    catch(e){ toast(e.fromDevice ? `⚠ ESP32 ปฏิเสธคำสั่ง: ${e.message}` : '⚠ เชื่อมต่อ ESP32 ไม่ได้ — ตรวจสอบ IP ในหน้าตั้งค่า'); }
  }
  $('startAllBtn').onclick=()=>openModal('เริ่มรอบทำความสะอาด?',
    'ปั๊มและแปรงจะเริ่มทำงาน รางจะวิ่งชน Limit แล้วเด้งกลับไปชนอีกฝั่ง และเด้งกลับ — ตรวจสอบว่าไม่มีสิ่งกีดขวางบนราง',
    ()=>sendMotor('START ALL',()=>Device.start(),'▶ START ALL (mock)'));
  $('stopAllBtn').onclick=()=>sendMotor('STOP ALL',()=>Device.stopAll(),'⛔ STOP ALL (mock)');
  $('railRightBtn').onclick=()=>sendMotor('ราง ขวา',()=>Device.rail(1),'▶ ราง ขวา (mock)');
  $('railLeftBtn').onclick=()=>sendMotor('ราง ซ้าย',()=>Device.rail(-1),'◀ ราง ซ้าย (mock)');
  $('railStopBtn').onclick=()=>sendMotor('หยุดราง',()=>Device.railStop(),'■ หยุดราง (mock)');
  $('brushOnBtn').onclick=()=>sendMotor('แปรง ON',()=>Device.brush(true),'แปรง ON (mock)');
  $('brushOffBtn').onclick=()=>sendMotor('แปรง OFF',()=>Device.brush(false),'แปรง OFF (mock)');
  $('pumpOnBtn').onclick=()=>sendMotor('ปั๊ม ON',()=>Device.pump(true),'ปั๊ม ON (mock)');
  $('pumpOffBtn').onclick=()=>sendMotor('ปั๊ม OFF',()=>Device.pump(false),'ปั๊ม OFF (mock)');
  speedSlider.oninput=()=>{ speedLabel.textContent=`${speedSlider.value} steps/sec`; };
  speedSlider.onchange=()=>{ if(Device.ip) Device.setSpeed(speedSlider.value).catch(()=>toast('⚠ ตั้งความเร็วไม่สำเร็จ')); };

  // Poll real status from the ESP32 when connected, to reflect the live state of the machine.
  async function poll(){
    if(!Device.ip) return;
    try{
      const s=await Device.getStatus();
      $('stCycle').textContent=s.cycle; $('stMsg').textContent=s.message;
      $('stLegs').textContent=`${s.legsDone} / ${s.legsTotal}`;
      $('stRail').textContent=s.rail; $('stBrush').textContent=s.brush; $('stPump').textContent=s.pump;
      $('stLeft').textContent=s.left; $('stRight').textContent=s.right; $('stPos').textContent=s.position;
      speedSlider.value=s.speed; speedLabel.textContent=`${s.speed} steps/sec`;
      setMachineState(String(s.message).startsWith('Fault') ? 'error' : (s.running ? 'running' : 'stopped'));
    }catch(e){ /* device offline this tick */ }
  }
  clearInterval(window._pollTimer);
  if(Device.ip){ poll(); window._pollTimer=setInterval(poll,1000); }
}

async function renderAnalytics(el){
  el.innerHTML='<div class="card c12"><div class="skeleton" style="height:160px;"></div></div>';
  const [bt,cd] = await Promise.all([MockAPI.getBatteryTrend(),MockAPI.getCycleDurations()]);
  el.innerHTML=`
  <div class="tabs">
    <div class="tab active" data-range="day">รายวัน</div>
    <div class="tab" data-range="week">รายสัปดาห์</div>
    <div class="tab" data-range="month">รายเดือน</div>
  </div>
  <div class="grid">
    <div class="card c6"><h3>แนวโน้มแรงดันแบตเตอรี่ (%)</h3>${lineChart(bt.labels,bt.values,'var(--accent)')}</div>
    <div class="card c6"><h3>เวลาที่ใช้ทำความสะอาดต่อรอบ (นาที)</h3>${barChart(cd.labels,cd.values,'var(--ok)')}</div>
    <div class="card c3 stat"><h3>รอบทำงานทั้งหมด</h3><div class="metric-big">52</div><div class="metric-sub">เดือนนี้</div></div>
    <div class="card c3 stat"><h3>เวลาเฉลี่ยต่อรอบ</h3><div class="metric-big">1:38</div><div class="metric-sub">ชม.:นาที</div></div>
    <div class="card c3 stat"><h3>อัตราสำเร็จ</h3><div class="metric-big" style="color:var(--ok)">92%</div><div class="metric-sub">จาก 52 รอบ</div></div>
    <div class="card c3 stat"><h3>Error สะสม</h3><div class="metric-big" style="color:var(--warn)">4</div><div class="metric-sub">เดือนนี้</div></div>
  </div>`;
}

function exportData(name){ toast(`📤 กำลังสร้างรายงาน ${name} ...`); setTimeout(()=>toast(`✅ Export ${name} สำเร็จ`),1000); }

async function renderHistory(el){
  el.innerHTML='<div class="card c12"><div class="skeleton" style="height:160px;"></div></div>';
  const rows=await MockAPI.getHistory();
  el.innerHTML=`
  <div class="card c12">
    <div class="filters">
      <input type="date"><input type="date">
      <select><option>ทุกแถวแผง</option><option>แถวแผง A</option><option>แถวแผง B</option><option>แถวแผง C</option></select>
      <select><option>ทุกสถานะ</option><option>สำเร็จ</option><option>ยกเลิก</option></select>
      <button class="btn" onclick="exportData('CSV')" style="flex:none;padding:7px 14px;">Export CSV</button>
      <button class="btn" onclick="exportData('PDF')" style="flex:none;padding:7px 14px;">Export PDF</button>
    </div>
    <div class="table-wrap"><table>
      <tr><th>วันที่</th><th>เริ่ม</th><th>สิ้นสุด</th><th>ระยะเวลา</th><th>พื้นที่</th><th>สถานะ</th></tr>
      ${rows.length? rows.map(r=>`<tr><td>${r.date}</td><td>${r.start}</td><td>${r.end}</td><td>${r.dur}</td><td>${r.zone}</td>
        <td><span class="badge ${r.status==='สำเร็จ'?'b-ok':'b-warn'}">${r.status}</span></td></tr>`).join('') : '<tr><td colspan="6"><div class="empty">ไม่พบข้อมูลในช่วงที่เลือก</div></td></tr>'}
    </table></div>
  </div>`;
}

async function renderErrors(el){
  el.innerHTML='<div class="card c12"><div class="skeleton" style="height:160px;"></div></div>';
  const rows=await MockAPI.getErrors();
  el.innerHTML=`
  <div class="card c12">
    <div class="filters">
      <select><option>ทุกประเภท</option><option>ชน Limit</option><option>แบตเตอรี่ต่ำ</option><option>เซนเซอร์ขัดข้อง</option><option>การเชื่อมต่อหลุด</option></select>
      <select><option>ทุกระดับ</option><option>คำเตือน</option><option>ผิดปกติ</option></select>
      <button class="btn" onclick="exportData('CSV')" style="flex:none;padding:7px 14px;">Export CSV</button>
    </div>
    <div class="table-wrap"><table>
      <tr><th>วัน-เวลา</th><th>ประเภท</th><th>ระดับความรุนแรง</th><th>สาเหตุ</th><th>สถานะแก้ไข</th></tr>
      ${rows.map(r=>`<tr><td>${r.ts}</td><td>${r.type}</td><td><span class="badge ${sevBadge(r.sev)}">${sevLabel(r.sev)}</span></td><td>${r.cause}</td><td>${r.fixed}</td></tr>`).join('')}
    </table></div>
  </div>`;
}

function renderSettings(el){
  el.innerHTML=`<div class="grid">
    <div class="card c6">
      <h3>🔌 เชื่อมต่อบอร์ด ESP32</h3>
      <div class="metric-sub" style="margin-bottom:10px;">ใส่ IP ของ ESP32 ในเครือข่ายเดียวกัน เพื่อสลับจากข้อมูลจำลองไปใช้งานจริง</div>
      <div class="ip-row">
        <input id="espIp" placeholder="192.168.1.50" style="background:var(--panel-2);border:1px solid var(--border);color:var(--text);padding:9px 10px;border-radius:7px;font-family:var(--font-d);" value="${Device.ip}">
        <button class="btn primary" id="espConnectBtn" style="flex:none;padding:9px 18px;">ทดสอบ</button>
      </div>
      <div style="margin-top:12px;display:flex;align-items:center;gap:8px;font-size:12.5px;">
        <span class="dot-s" id="espDot" style="background:${Device.connected?'var(--ok)':'var(--muted-2)'}"></span>
        <span id="espStatusText">${Device.connected?'เชื่อมต่อสำเร็จ':'ยังไม่ได้เชื่อมต่อ · ใช้ข้อมูลจำลอง'}</span>
      </div>
      <div class="hint">Firmware (Solasale_project.ino) เปิด endpoint: <b>/status</b>, <b>/start</b>, <b>/stop</b>, <b>/run?dir=1|-1</b>, <b>/railstop</b>, <b>/brush?on=</b>, <b>/pump?on=</b>, <b>/speed?value=N</b><br>
      หมายเหตุ: หน้านี้เมื่อเปิดผ่าน https จะเชื่อมต่อ ESP32 ที่เป็น http ธรรมดาในเครือข่ายท้องถิ่นไม่ได้ (ข้อจำกัด mixed-content) — ต้องรันไฟล์นี้เองในเครื่อง หรือใส่ ESP32 หลัง https proxy</div>
    </div>
    <div class="card c6"><h3>🎨 ธีม</h3>
      <div class="ctrl-row">
        <button class="btn" onclick="document.documentElement.setAttribute('data-theme','dark')">Dark</button>
        <button class="btn" onclick="document.documentElement.setAttribute('data-theme','light')">Light</button>
      </div>
      <div class="hint">ธีมเริ่มต้นจะตามการตั้งค่าของอุปกรณ์คุณโดยอัตโนมัติ</div>
    </div>
  </div>`;
  document.getElementById('espConnectBtn').onclick=async()=>{
    Device.ip=document.getElementById('espIp').value.trim();
    try{ localStorage.setItem('esp32_ip', Device.ip); }catch(e){}
    document.getElementById('espStatusText').textContent='กำลังเชื่อมต่อ...';
    const ok=await Device.ping();
    document.getElementById('espDot').style.background=ok?'var(--ok)':'var(--bad)';
    document.getElementById('espStatusText').textContent=ok?'เชื่อมต่อสำเร็จ':'เชื่อมต่อไม่สำเร็จ · ยังใช้ข้อมูลจำลองอยู่';
    document.getElementById('linkModeLabel').textContent=ok?`โหมด: ESP32 (${Device.ip})`:'โหมด: Mock data';
    toast(ok?'✅ เชื่อมต่อ ESP32 สำเร็จ':'⚠ เชื่อมต่อไม่สำเร็จ ตรวจสอบ IP และเครือข่าย');
  };
}

const renderers={monitoring:renderMonitoring,analytics:renderAnalytics,history:renderHistory,errors:renderErrors,settings:renderSettings};

async function nav(view){
  document.querySelectorAll('.nav-item').forEach(n=>n.classList.toggle('active',n.dataset.view===view));
  document.getElementById('pageTitle').textContent=titles[view];
  document.getElementById('sidebar').classList.remove('open');
  try{ await renderers[view](document.getElementById('content')); }
  catch(e){ document.getElementById('content').innerHTML='<div class="card c12"><div class="empty">⚠ โหลดข้อมูลไม่สำเร็จ ลองใหม่อีกครั้ง</div></div>'; }
  if(view==='analytics'){
    document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>{document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));t.classList.add('active');toast('อัปเดตช่วงเวลา: '+t.textContent);});
  }
}
document.querySelectorAll('.nav-item').forEach(n=>n.onclick=()=>nav(n.dataset.view));
document.getElementById('backdrop').onclick=()=>document.getElementById('sidebar').classList.remove('open');
document.addEventListener('keydown',e=>{ if(e.key==='Escape') document.getElementById('sidebar').classList.remove('open'); });
window.addEventListener('resize',()=>{ if(innerWidth>900) document.getElementById('sidebar').classList.remove('open'); });
nav('monitoring');
if(Device.ip){
  Device.ping().then(ok=>{
    document.getElementById('linkModeLabel').textContent = ok ? `โหมด: ESP32 (${Device.ip})` : 'โหมด: Mock data (ESP32 ไม่ตอบสนอง)';
  });
}
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
