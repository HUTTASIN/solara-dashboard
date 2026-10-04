#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "time.h"

// =====================================================
// ESP32 STEPPER MOTOR DASHBOARD
// =====================================================

#define STEP_PIN 25
#define DIR_PIN  26

// =====================================================
// WIFI
// =====================================================

const char* WIFI_SSID = "true_home2G_248";
const char* WIFI_PASSWORD = "7u393g45";

WebServer server(80);

// =====================================================
// MOTOR VARIABLES
// =====================================================

bool motorRunning = false;

int motorDirection = 1;
//  1 = Forward
// -1 = Reverse

int speedSPS = 500;

long position = 0;

bool stepState = LOW;

unsigned long lastStepMicros = 0;

// =====================================================
// CYCLE TRACKING (real counts, persisted across reboots)
// =====================================================
Preferences prefs;

time_t cycleStartEpoch = 0;   // 0 = no cycle running right now
unsigned long totalCycles = 0;
unsigned long todayCount = 0, weekCount = 0, monthCount = 0;
String todayKey, weekKey, monthKey;   // e.g. "2026-09-18", "2026-W38", "2026-09"

String makeTodayKey(struct tm &t){ char b[11]; sprintf(b,"%04d-%02d-%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday); return String(b); }
String makeMonthKey(struct tm &t){ char b[8];  sprintf(b,"%04d-%02d",t.tm_year+1900,t.tm_mon+1); return String(b); }
String makeWeekKey(struct tm &t){ char b[9]; int wk=(t.tm_yday - t.tm_wday + 10)/7; sprintf(b,"%04d-W%02d",t.tm_year+1900,wk); return String(b); }

// Call this whenever a new cleaning cycle starts. Rolls today/week/month
// counters over automatically when the real calendar date has moved on,
// and persists everything to flash so counts survive a reboot/power cut.
void registerCycleStart(){
    cycleStartEpoch = time(nullptr);
    struct tm t; localtime_r(&cycleStartEpoch, &t);

    String tk = makeTodayKey(t), wk = makeWeekKey(t), mk = makeMonthKey(t);
    if (tk != todayKey) { todayKey = tk; todayCount = 0; }
    if (wk != weekKey)  { weekKey  = wk; weekCount  = 0; }
    if (mk != monthKey) { monthKey = mk; monthCount = 0; }

    todayCount++; weekCount++; monthCount++; totalCycles++;

    prefs.putString("todayKey", todayKey); prefs.putULong("todayCount", todayCount);
    prefs.putString("weekKey",  weekKey);  prefs.putULong("weekCount",  weekCount);
    prefs.putString("monthKey", monthKey); prefs.putULong("monthCount", monthCount);
    prefs.putULong("totalCycles", totalCycles);
}

// =====================================================
// DASHBOARD HTML
// =====================================================

const char MAIN_page[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
content="width=device-width, initial-scale=1">

<title>ESP32 Stepper Control</title>

<style>

* {
    box-sizing: border-box;
}

body {

    margin: 0;

    font-family:
    Arial,
    Helvetica,
    sans-serif;

    background:
    linear-gradient(
        135deg,
        #111827,
        #1f2937
    );

    color: white;

    min-height: 100vh;

}

.container {

    max-width: 650px;

    margin: auto;

    padding: 20px;

}


/* HEADER */

.header {

    text-align: center;

    margin-bottom: 20px;

}

.header h1 {

    margin: 0;

    font-size: 28px;

}

.header p {

    color: #9ca3af;

    margin-top: 6px;

}


/* STATUS */

.status-card {

    background: #111827;

    border: 1px solid #374151;

    border-radius: 20px;

    padding: 20px;

    margin-bottom: 15px;

    text-align: center;

}

.status {

    display: inline-block;

    padding: 8px 20px;

    border-radius: 30px;

    font-size: 18px;

    font-weight: bold;

}

.status.stop {

    background: #3f1d1d;

    color: #ff6b6b;

}

.status.run {

    background: #123c28;

    color: #4ade80;

}


/* INFO */

.info-grid {

    display: grid;

    grid-template-columns:
    repeat(2, 1fr);

    gap: 12px;

    margin-bottom: 15px;

}

.info {

    background: #1f2937;

    border: 1px solid #374151;

    border-radius: 16px;

    padding: 18px;

    text-align: center;

}

.info-title {

    color: #9ca3af;

    font-size: 14px;

    margin-bottom: 8px;

}

.info-value {

    font-size: 25px;

    font-weight: bold;

}


/* CONTROL CARD */

.card {

    background: #111827;

    border: 1px solid #374151;

    border-radius: 20px;

    padding: 20px;

    margin-bottom: 15px;

}

.card h2 {

    margin-top: 0;

    font-size: 20px;

}


/* DIRECTION BUTTONS */

.direction {

    display: grid;

    grid-template-columns:
    1fr 1fr;

    gap: 12px;

}

button {

    border: none;

    border-radius: 15px;

    min-height: 65px;

    font-size: 18px;

    font-weight: bold;

    cursor: pointer;

    color: white;

    transition: 0.15s;

}

button:active {

    transform: scale(0.96);

}


.forward {

    background:
    linear-gradient(
        135deg,
        #16a34a,
        #22c55e
    );

}

.reverse {

    background:
    linear-gradient(
        135deg,
        #2563eb,
        #3b82f6
    );

}

.stop {

    background:
    linear-gradient(
        135deg,
        #dc2626,
        #ef4444
    );

    width: 100%;

    margin-top: 12px;

    font-size: 22px;

}


/* ACTIVE BUTTON */

.active {

    box-shadow:
    0 0 0 4px
    rgba(255,255,255,0.2);

}


/* SPEED */

.speed-value {

    text-align: center;

    font-size: 28px;

    font-weight: bold;

    margin-bottom: 10px;

}

input[type=range] {

    width: 100%;

    height: 8px;

    cursor: pointer;

}


/* FOOTER */

.footer {

    text-align: center;

    color: #6b7280;

    font-size: 13px;

    margin-top: 20px;

}


/* MOBILE */

@media(max-width: 500px) {

    .container {

        padding: 12px;

    }

    .header h1 {

        font-size: 24px;

    }

    button {

        min-height: 70px;

    }

}

</style>

</head>


<body>


<div class="container">


<!-- HEADER -->

<div class="header">

<h1>⚙️ Stepper Motor</h1>

<p>ESP32 Web Control Dashboard</p>

</div>


<!-- STATUS -->

<div class="status-card">

<div id="status"
class="status stop">

🔴 STOPPED

</div>

</div>


<!-- INFORMATION -->

<div class="info-grid">


<div class="info">

<div class="info-title">
DIRECTION
</div>

<div id="direction"
class="info-value">

---

</div>

</div>


<div class="info">

<div class="info-title">
POSITION
</div>

<div id="position"
class="info-value">

0

</div>

</div>


</div>


<!-- MOTOR CONTROL -->

<div class="card">

<h2>🎮 Motor Control</h2>


<div class="direction">


<button
id="forwardBtn"
class="forward"
type="button">

▶ FORWARD

</button>


<button
id="reverseBtn"
class="reverse"
type="button">

◀ REVERSE

</button>


</div>


<button
id="stopBtn"
class="stop"
type="button">

⛔ STOP

</button>


</div>


<!-- SPEED -->

<div class="card">

<h2>⚡ Motor Speed</h2>


<div class="speed-value">

<span id="speedValue">
500
</span>

<span style="font-size:15px;">
steps/sec
</span>

</div>


<input
id="speedSlider"
type="range"
min="50"
max="2000"
step="50"
value="500">


</div>


<div class="footer">

ESP32 Stepper Motor Controller

</div>


</div>


<script>


const forwardBtn =
document.getElementById(
    "forwardBtn"
);

const reverseBtn =
document.getElementById(
    "reverseBtn"
);

const stopBtn =
document.getElementById(
    "stopBtn"
);

const speedSlider =
document.getElementById(
    "speedSlider"
);

const speedValue =
document.getElementById(
    "speedValue"
);

const statusElement =
document.getElementById(
    "status"
);

const directionElement =
document.getElementById(
    "direction"
);

const positionElement =
document.getElementById(
    "position"
);


// =====================================================
// FORWARD
// =====================================================

forwardBtn.addEventListener(
    "click",
    function() {

        fetch("/run?dir=1");

    }
);


// =====================================================
// REVERSE
// =====================================================

reverseBtn.addEventListener(
    "click",
    function() {

        fetch("/run?dir=-1");

    }
);


// =====================================================
// STOP
// =====================================================

stopBtn.addEventListener(
    "click",
    function() {

        fetch("/stop");

    }
);


// =====================================================
// SPEED
// =====================================================

speedSlider.addEventListener(
    "input",
    function() {

        speedValue.innerText =
            this.value;

        fetch(
            "/speed?value="
            + this.value
        );

    }
);


// =====================================================
// UPDATE DASHBOARD
// =====================================================

function updateStatus() {

    fetch("/status")

    .then(
        response => response.json()
    )

    .then(
        data => {


            // STATUS

            if(data.running) {

                statusElement.innerHTML =
                    "🟢 RUNNING";

                statusElement.className =
                    "status run";

            }

            else {

                statusElement.innerHTML =
                    "🔴 STOPPED";

                statusElement.className =
                    "status stop";

            }


            // POSITION

            positionElement.innerText =
                data.position;


            // DIRECTION

            if(data.direction === 1) {

                directionElement.innerText =
                    "FORWARD ▶";

            }

            else if(data.direction === -1) {

                directionElement.innerText =
                    "◀ REVERSE";

            }

            else {

                directionElement.innerText =
                    "---";

            }


            // SPEED

            speedValue.innerText =
                data.speed;


            speedSlider.value =
                data.speed;


            // ACTIVE BUTTON

            forwardBtn.classList.remove(
                "active"
            );

            reverseBtn.classList.remove(
                "active"
            );


            if(data.running) {

                if(data.direction === 1) {

                    forwardBtn.classList.add(
                        "active"
                    );

                }

                else {

                    reverseBtn.classList.add(
                        "active"
                    );

                }

            }

        }

    )

    .catch(
        error => {

            console.log(error);

        }
    );

}


// Update every 300 ms

setInterval(
    updateStatus,
    300
);


updateStatus();


</script>


</body>

</html>

)rawliteral";


// =====================================================
// HOME
// =====================================================

void handleRoot() {

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(
        200,
        "text/html",
        MAIN_page
    );

}


// =====================================================
// RUN MOTOR
// =====================================================

void handleRun() {

    if(!server.hasArg("dir")) {

        server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(
            400,
            "text/plain",
            "Missing direction"
        );

        return;

    }


    motorDirection =
        server.arg("dir").toInt();


    if(
        motorDirection != 1 &&
        motorDirection != -1
    ) {

        motorDirection = 1;

    }


    // Set direction

    if(motorDirection == 1) {

        digitalWrite(
            DIR_PIN,
            HIGH
        );

    }

    else {

        digitalWrite(
            DIR_PIN,
            LOW
        );

    }


    // Start continuous movement — a new "cycle" begins only when we were
    // previously stopped (so holding/re-pressing forward doesn't double count).
    if (!motorRunning) {
        registerCycleStart();
    }
    motorRunning = true;


    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(
        200,
        "text/plain",
        "Motor Running"
    );

}


// =====================================================
// STOP MOTOR
// =====================================================

void handleStop() {

    motorRunning = false;


    digitalWrite(
        STEP_PIN,
        LOW
    );


    stepState = LOW;


    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(
        200,
        "text/plain",
        "Motor Stopped"
    );

}


// =====================================================
// SET SPEED
// =====================================================

void handleSpeed() {

    if(!server.hasArg("value")) {

        server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(
            400,
            "text/plain",
            "Missing speed"
        );

        return;

    }


    speedSPS =
        server.arg("value").toInt();


    // Limit speed

    if(speedSPS < 50)
        speedSPS = 50;

    if(speedSPS > 2000)
        speedSPS = 2000;


    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(
        200,
        "text/plain",
        "Speed Updated"
    );

}


// =====================================================
// STATUS
// =====================================================

void handleStatus() {

    String json = "{";


    json += "\"running\":";
    json +=
        motorRunning
        ? "true"
        : "false";


    json += ",";


    json += "\"direction\":";
    json += motorDirection;


    json += ",";


    json += "\"position\":";
    json += position;


    json += ",";


    json += "\"speed\":";
    json += speedSPS;

    json += ",";
    json += "\"cycleStartEpoch\":";
    json += (unsigned long)cycleStartEpoch;

    json += ",";
    json += "\"cycleElapsedSec\":";
    json += cycleStartEpoch ? (unsigned long)(time(nullptr) - cycleStartEpoch) : 0;

    json += ",";
    json += "\"todayCount\":"; json += todayCount;
    json += ",";
    json += "\"weekCount\":";  json += weekCount;
    json += ",";
    json += "\"monthCount\":"; json += monthCount;
    json += ",";
    json += "\"totalCycles\":"; json += totalCycles;

    json += "}";


    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(
        200,
        "application/json",
        json
    );

}


// =====================================================
// STEPPER ENGINE
// =====================================================

void updateStepper() {


    if(!motorRunning) {

        return;

    }


    unsigned long now =
        micros();


    // Calculate pulse interval

    unsigned long interval =
        1000000UL /
        speedSPS;


    // Toggle STEP

    if(
        now - lastStepMicros
        >= interval / 2
    ) {

        lastStepMicros =
            now;


        stepState =
            !stepState;


        digitalWrite(
            STEP_PIN,
            stepState
        );


        // Count position
        // on rising edge

        if(stepState == HIGH) {

            position +=
                motorDirection;

        }

    }

}


// =====================================================
// SETUP
// =====================================================

void setup() {


    Serial.begin(
        115200
    );


    pinMode(
        STEP_PIN,
        OUTPUT
    );


    pinMode(
        DIR_PIN,
        OUTPUT
    );


    digitalWrite(
        STEP_PIN,
        LOW
    );


    digitalWrite(
        DIR_PIN,
        LOW
    );


    // =================================================
    // CONNECT TO HOME WI-FI
    // =================================================

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.println();
    Serial.println("================================");
    Serial.println(" ESP32 STEPPER DASHBOARD");
    Serial.println("================================");
    Serial.print("Connecting to Wi-Fi: ");
    Serial.println(WIFI_SSID);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(400);
        Serial.print(".");
        // After 20s of trying, restart and try again rather than hang forever.
        if (millis() - wifiStart > 20000) {
            Serial.println();
            Serial.println("Wi-Fi connect timed out, restarting...");
            ESP.restart();
        }
    }

    Serial.println();
    Serial.println("Wi-Fi connected!");
    Serial.print("Dashboard: http://");
    Serial.println(WiFi.localIP());

    // Get the real date/time from the internet (needed for today/week/month
    // cycle counts to roll over on the correct calendar day). GMT+7 = Thailand.
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
    Serial.print("Syncing time");
    time_t now = time(nullptr);
    unsigned long ntpStart = millis();
    while (now < 100000 && millis() - ntpStart < 10000) { delay(300); Serial.print("."); now = time(nullptr); }
    Serial.println();

    // Reload persisted cycle counts so they survive a reboot/power cut.
    prefs.begin("cycles", false);
    todayKey  = prefs.getString("todayKey", "");
    weekKey   = prefs.getString("weekKey", "");
    monthKey  = prefs.getString("monthKey", "");
    todayCount  = prefs.getULong("todayCount", 0);
    weekCount   = prefs.getULong("weekCount", 0);
    monthCount  = prefs.getULong("monthCount", 0);
    totalCycles = prefs.getULong("totalCycles", 0);
    // If the saved date keys don't match today's real date, the counters are
    // stale from a previous day — registerCycleStart() will roll them over
    // automatically the next time a cycle actually starts.


    // =================================================
    // WEB ROUTES
    // =================================================

    server.on(
        "/",
        handleRoot
    );


    server.on(
        "/run",
        handleRun
    );


    server.on(
        "/stop",
        handleStop
    );


    server.on(
        "/speed",
        handleSpeed
    );


    server.on(
        "/status",
        handleStatus
    );


    server.begin();


    Serial.println(
        "Web Server Started"
    );

}


// =====================================================
// LOOP
// =====================================================

void loop() {


    server.handleClient();


    updateStepper();

}