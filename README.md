# SOLARA — Solar Panel Cleaning Dashboard

Static, single-file dashboard (`index.html`). No build step, no dependencies to install.

## Deploy to Cloudflare Pages

**Option A — Dashboard (easiest)**
1. Go to the Cloudflare dashboard → **Workers & Pages** → **Create** → **Pages** → **Upload assets**.
2. Drag this whole folder (`index.html` + `_headers`) into the upload area.
3. Give the project a name and deploy. You'll get a `*.pages.dev` URL immediately.

**Option B — Wrangler CLI**
```bash
npm install -g wrangler
wrangler pages deploy . --project-name=solara-dashboard
```
Run this from inside this folder.

**Option C — Git**
Push this folder to a GitHub/GitLab repo, then in Cloudflare Pages choose
**Connect to Git**, pick the repo, and leave the build command empty
(output directory = `/`).

## Connecting the ESP32

Open the deployed site → **ตั้งค่า (Settings)** → enter the ESP32's IP address →
**ทดสอบ (Test)**. The IP is saved in the browser (localStorage) so it's
remembered next time you open the dashboard on the same device/browser.

`Solasale_project.ino` (included in this folder) is the matching firmware. It
drives the rail stepper (A4988), brush motor (BTS7960), pump relay, two limit
switches and the physical START/STOP buttons. It joins Wi-Fi (`WIFI_SSID` / `WIFI_PASSWORD` at the top of
the file, unchanged from the original firmware) as a station and serves:

| Method | Path                | Purpose                                                        |
|--------|---------------------|----------------------------------------------------------------|
| GET    | `/status`           | cycle, message, rail, brush, pump, limits, position, speed, legs |
| GET    | `/start`            | START ALL — pump + brush ON, auto sweep (see below)            |
| GET    | `/stop`             | STOP ALL — rail, brush and pump off                            |
| GET    | `/run?dir=1\|-1`    | manual rail right / left (refused while the auto cycle runs)   |
| GET    | `/railstop`         | stop the rail only                                             |
| GET    | `/brush?on=1\|0`    | manual brush                                                   |
| GET    | `/pump?on=1\|0`     | manual pump                                                    |
| GET    | `/speed?value=N`    | rail speed, 100–800 steps/sec                                  |
| GET    | `/schedule?...`     | save the auto schedule (see below)                             |
| GET    | `/ldrauto?on=1\|0` | enable / disable auto-start from the LDR comparison (saved in flash) |

### Auto schedule (time + weekdays)

10 slots per day. Each slot has an enable flag, a start time (Thai time, NTP) and
a **weekday mask**, so a slot can run every day or only on chosen days. Both the
website (menu **ตั้งเวลาทำงาน**) and the ESP32's own page have the day pickers
(จ อ พ พฤ ศ ส อา, with quick buttons: ทุกวัน / จ–ศ / ส–อา).

`GET /schedule?master=1&e0=1&s0=08:00&d0=62&e1=1&s1=17:30&d1=127 ...`

| Param | Meaning |
|-------|---------|
| `master` | `1`/`0` — whole schedule on/off |
| `e<i>`   | slot *i* (0–9) enabled `1`/`0` |
| `s<i>`   | start time `HH:MM` |
| `d<i>`   | weekday bitmask 0–127: bit0 = Sun, bit1 = Mon … bit6 = Sat (`127` = every day, `62` = Mon–Fri, `65` = Sat+Sun). Omitted = keep old value |
| `cancel=1` | turn the whole schedule off |

`/status` returns `d0`…`d9` alongside `e<i>` / `s<i>`. Two slots may share the same
time only if their days don't overlap. Saved days live in flash (key `dy<i>`);
firmware older than this version has no `dy` keys, so existing slots default to
every day. **Re-flash `Solasale_project.ino`** after updating `index.html`, otherwise
the old firmware ignores the website's day selection.

### LDR light sensors (GPIO 34 + GPIO 35)

Two LDRs are read on ADC1 (works with Wi-Fi). Values are the **raw ADC reading
(0-4095): more light = lower value, darker = higher value**. `/status` adds
`ldr34`, `ldr35`, `ldrRisePct`, `ldrThresholdPct`, `ldrMaxRef`, `ldrAuto`,
`ldrState`, `ldrMessage`, and both dashboards show them.

**Auto-start rule:** when the LDR 35 value is more than 30 % **higher** than LDR 34
(LDR 35 is darker) the firmware runs START (same as the button). To avoid false
triggers and endless re-runs:

- the condition must hold for `LDR_HOLD_MS` (5 s);
- LDR 34 must read at most `LDR_MAX_REF_RAW` (3800) so it never fires at night;
- the % is calculated against at least `LDR_MIN_REF_RAW` (100) so strong sun (values near 0) can't cause a huge %;
- after any cycle ends (or is stopped) it waits `LDR_COOLDOWN_MS` (30 min) before it may fire again;
- it never fires while the machine is already running or being driven manually;
- the dashboard checkbox (or `/ldrauto?on=0`) switches the rule off; the choice survives reboot.

Tunables near the top of the `.ino`: `LDR_RISE_PERCENT`, `LDR_MAX_REF_RAW`,
`LDR_MIN_REF_RAW`, `LDR_HOLD_MS`, `LDR_COOLDOWN_MS`.

### What START does (web button or physical button)

1. Pump relay and brush turn ON immediately.
2. The rail runs until it hits a limit switch, then bounces back to the opposite
   limit, then bounces back again. Default is 3 legs: right → left → right.
   (If the right limit is already pressed at start, it begins by going left.)
3. When the last leg hits its limit, rail, brush and pump all turn OFF.

Tunables near the top of the `.ino`:

- `SWEEP_LEGS` — number of legs (`2` = one round trip, `3` = default, `4` = two round trips).
- `LEG_TIMEOUT_MS` — safety cut-off: if a leg runs this long without reaching a
  limit (broken switch / loose wire) everything is switched off and the status
  shows `Fault: limit not reached (timeout)`. Set it above the time the rail
  really needs to travel end to end at your slowest speed.
- `DIR_RIGHT_LEVEL` — flip `HIGH`/`LOW` if the rail goes the wrong way.
- `RELAY_ACTIVE_LOW` — set `false` if your relay board is active-HIGH.

Flash it with the Arduino IDE (board: an ESP32 dev board, e.g. "ESP32 Dev
Module"). After it connects, open the Serial Monitor at 115200 baud to read
the IP address it was assigned — that's what you type into the dashboard's
Settings page. The ESP32 also serves its own simple dashboard at that IP.

> **Do not publish the `.ino`.** It contains your Wi-Fi password. When deploying
> to Cloudflare Pages upload only `index.html` and `_headers` (Option A above);
> with Wrangler/Git, move `Solasale_project.ino` out of the deployed folder first.

This logic lives in the `Device` object near the top of the `<script>` block
in `index.html` — edit it if you change the firmware's routes.

### ⚠️ Important: HTTPS vs. local ESP32 (mixed content)

Cloudflare Pages serves everything over **https**. Browsers block a page
loaded over https from calling plain **http** devices on your local network
(this is the browser's own "mixed content" security rule — it has nothing to
do with Cloudflare or this code). Because of that, the deployed dashboard
**cannot talk to an ESP32 on your LAN directly.**

Pick one of these to actually close the loop:

1. **Cloudflare Tunnel** (recommended) — run `cloudflared` on a machine on the
   same network as the ESP32, exposing it at `https://esp32.yourdomain.com`.
   Point the dashboard's Settings field at that hostname instead of a raw IP.
2. **A small backend/proxy you control** (e.g. a Cloudflare Worker or any
   server with a public https URL) that forwards requests to the ESP32 and
   that the dashboard calls instead of the ESP32 directly.
3. **Self-host the dashboard** on the same local network as the ESP32, served
   over plain http (e.g. from a Raspberry Pi) — then there's no mixed-content
   issue, but the dashboard is only reachable on that network.

Until one of these is in place, the dashboard will show **mock data** for
battery/sensors/history, since the ESP32 calls will fail and it falls back
automatically.
