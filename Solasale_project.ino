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
// INFO (plain-text landing route — no dashboard served from the board)
// =====================================================

void handleInfo() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "ESP32 Solar Panel Cleaning motor API is running. See /status, /run, /stop, /speed.");
}

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
        handleInfo
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