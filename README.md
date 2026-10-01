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

The dashboard expects the ESP32 to serve plain JSON over HTTP:

| Method | Path          | Purpose                                   |
|--------|---------------|--------------------------------------------|
| GET    | `/api/status` | battery, sensors, machine state           |
| POST   | `/api/start`  | begin a cleaning cycle                    |
| POST   | `/api/stop`   | stop the motor immediately                |

This logic lives in the `Device` object near the top of the `<script>` block
in `index.html` — edit it to match your firmware's actual response shape.

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
