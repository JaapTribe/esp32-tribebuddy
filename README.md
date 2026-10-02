# TribeBuddy

A desk display with a phone-style home screen and four apps:

- **Development**: today's Claude tokens and estimated cost, per-model, per-day and per-hour
  charts, your plan limits (5-hour and weekly), Claude Code sessions and Bitbucket Pipelines.
- **3D Printer**: a Bambu Lab printer's (P1S) print status, its speed profile, and a camera
  snapshot.
- **Jira**: your open Jira issues.
- **Calendar**: your agenda, with a notification 5 minutes before each meeting or reminder.

It can also ask you to **allow or deny Claude Code permission requests** with a tap, and has a
**Settings** page (brightness, which apps show, contrast) that `.env` can set too.

On top of any app, a notification card shows up when Claude Code or Claude Desktop needs you,
a pipeline fails, a print finishes — or when any other app or script posts to its webhook.

The board has no network connection: `feeder.py` runs on your computer, reads Claude Code's
local logs, talks to Bitbucket, Jira and the printer, and sends it all over USB serial.

## What you need

- **Board**, one of:
  - ESP32 "Cheap Yellow Display" (ESP32-2432S028R) — display and touch on one board. [Hackerstore - ESP Yellow Display](https://www.hackerstore.nl/Artikel/1642)
  - ESP32-S3 with an ILI9341 2.8" module (wired as in `diagram.json`), or the
    [Wokwi](https://wokwi.com) simulator, no hardware needed.
- A USB data cable (not a charge-only one).
- macOS or Linux with Python 3.8+ and Claude Code installed.
- [arduino-cli](https://arduino.github.io/arduino-cli/) (or the Arduino IDE).

## 1. Flash the board

Install the ESP32 core and the libraries (once):

```sh
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit GFX Library" "Adafruit ILI9341" "Adafruit FT6206 Library" ArduinoJson TJpg_Decoder
```

arduino-cli wants the sketch in a folder named after the `.ino`, so compile a copy:

```sh
mkdir -p /tmp/sketch && cp sketch.ino tribe_logo.h /tmp/sketch/
```

**Cheap Yellow Display** — find the port with `arduino-cli board list`, then:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir build-cyd /tmp/sketch
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=460800 \
  -p /dev/cu.usbserial-XXXX --input-dir build-cyd /tmp/sketch
```

Use 460800 baud: the board's USB chip fails at the default 921600
(`Unable to verify flash chip connection`). Stop the feeder first if it runs as a service
(see step 4); it keeps the port open.

**ESP32-S3** — compile with `--fqbn esp32:esp32:esp32s3`. On real hardware set
"USB CDC On Boot" to Enabled (Arduino IDE: Tools menu) so serial runs over USB.

**Wokwi simulator** — compile the S3 build into `build/`, which `wokwi.toml` points to:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3 --output-dir build /tmp/sketch
```

Then start the simulation (Wokwi for VS Code: "Wokwi: Start Simulator"). It exposes the
serial port on `rfc2217://localhost:4000`, the feeder's default.

After flashing, the board shows the Tribe logo and then "No data yet". Type `demo` in a
serial monitor (115200 baud) to see sample data.

## 2. Install the feeder

```sh
python3 -m pip install pyserial
python3 feeder.py --dry-run --once     # prints what it would send; checks your logs are found
```

## 3. Run it

```sh
python3 feeder.py --port auto          # real board: found by its USB chip, reconnects on replug
python3 feeder.py                      # Wokwi simulator
```

Useful options: `--interval 10` (seconds between updates), `--limit 5000000` (daily token bar,
used when plan limits are unavailable), `--notify-seconds 15` (0 = until tapped),
`--no-limits`, `--no-desktop`. See `python3 feeder.py --help`.

## 4. Start it at login (optional)

```sh
python3 feeder.py install-service      # macOS LaunchAgent or systemd --user service
python3 feeder.py uninstall-service
```

The service runs a copy of `feeder.py` (macOS: `~/Library/Application Support/TribeBuddy`),
so run `install-service` again after changing the script. Logs: `~/Library/Logs/tribebuddy.log`
on macOS, `journalctl --user -u tribebuddy -f` on Linux. On Linux you may need
`sudo usermod -aG dialout $USER` (then log in again) to open the serial port.

## 5. Notifications

**Claude Code** — add the hooks once; they queue an event when Claude needs permission or
finishes, and keep the sessions on the Claude page up to date (session start/end, prompt, tool
use). Run it again after updating `feeder.py` to pick up new hooks:

```sh
python3 feeder.py install-hooks        # writes to ~/.claude/settings.json (backup kept)
python3 feeder.py uninstall-hooks
```

Running sessions pick them up after `/hooks` or a restart. The hooks point to this folder's
`feeder.py`: run `install-hooks` again if you move the folder.

**Claude Desktop / Cowork** (macOS) — read from the macOS notification database, which needs
Full Disk Access (System Settings > Privacy & Security) for the app running the feeder: your
terminal, or for the service the Python that `install-service` prints. Restart the feeder after
granting it.

**Webhook** — anything that can make an HTTP request can put a card on the display: CI,
monitoring, home automation, a cron script. The feeder listens on `http://localhost:8787/notify`.

```sh
curl -d "Backup finished" "http://localhost:8787/notify?app=Backup&priority=low"

curl -H "Content-Type: application/json" http://localhost:8787/notify -d '{
  "app": "GitHub Actions",
  "title": "Deploy failed",
  "message": "tribe-website: build failed on main",
  "priority": "high"
}'
```

| Field      | Also accepted as                      | Shown as                                   |
|------------|---------------------------------------|--------------------------------------------|
| `app`      | `source`, `src`, `sender`, `app_name` | card header (default "Webhook")            |
| `message`  | `body`, `text`, `msg`, `content`      | card text (required, unless a title is set)|
| `title`    | `subject`                             | large line above the text (optional)       |
| `priority` | `prio`, `level`, `severity`           | `low`, `normal` (default) or `high`        |
| `ttl`      |                                       | seconds on screen, 0 = until tapped        |

- Send JSON, form fields, or plain text (the text becomes the message); query parameters work
  for every field.
- Priority also takes 1–5 (1–2 low, 3 normal, 4–5 high) and words like `urgent`, `critical`,
  `error` (high) or `info`, `debug` (low).
- **High**: red border, longer blink, shown before other waiting cards.
  **Low**: dim border, no blink.
- Up to 4 cards wait on the board; texts longer than the card are cut off with "...".

By default only this computer can post to it. To accept posts from your network, set a token:

```sh
python3 feeder.py --port auto --webhook-host 0.0.0.0 --webhook-token <secret>
curl -H "Authorization: Bearer <secret>" -d "Hello" http://<your-mac>:8787/notify
```

The token can also be sent as `X-Token: <secret>` or `?token=<secret>`, or set with the
`TRIBEBUDDY_WEBHOOK_TOKEN` environment variable. Requests from web browsers (with an `Origin`
header) are always refused, so websites you visit can't post to it. Use `--webhook-port` to
change the port, `--webhook-port 0` to turn the webhook off. For the service, pass the same
options to `install-service`.

**From the internet (Jira, Bitbucket, …)** — cloud services can't reach your computer, so the
feeder can start an [ngrok](https://ngrok.com) tunnel in front of the webhook. Set up once:

1. `brew install ngrok` and create a free account at [dashboard.ngrok.com](https://dashboard.ngrok.com/signup).
2. `ngrok config add-authtoken <token>` (the token is on the dashboard under "Your Authtoken").
3. Look up your domain on the dashboard (Universal Gateway > Domains). Free accounts get one
   fixed domain assigned, like `example-name-here.ngrok-free.dev`; you can't pick the
   name (that's a paid feature, `ERR_NGROK_313`).

Then run the feeder with a token (it refuses to open a tunnel without one, and suggests a
random one):

```sh
python3 feeder.py --port auto --tunnel ngrok --tunnel-domain <your-domain>.ngrok-free.dev \
  --webhook-token <secret>
```

The log shows `tunnel up: https://<your-domain>.ngrok-free.dev/notify`. Give services that URL and
the token: as an `Authorization: Bearer <secret>` header where they allow custom headers (Jira
Automation's "Send web request"), otherwise as `?token=<secret>` in the URL. With a token set,
local posts need it too. For the service:
`python3 feeder.py install-service --tunnel ngrok --tunnel-domain … --webhook-token …`.
If ngrok stops (network change, sleep) the feeder restarts it: after 10 seconds, backing off
to every 5 minutes while it keeps failing.

## Bitbucket Pipelines

The feeder can follow Bitbucket Pipelines across a whole workspace through the Bitbucket API —
no webhooks or `bitbucket-pipelines.yml` changes per repository, and no tunnel needed.

1. Create an API token at [id.atlassian.com](https://id.atlassian.com/manage-profile/security/api-tokens)
   → "Create API token with scopes" → Bitbucket, with the scopes `read:repository:bitbucket` and
   `read:pipeline:bitbucket`. (A workspace access token works too; then leave out the e-mail.)
2. Run the feeder with it:

```sh
python3 feeder.py --port auto --bitbucket-workspace <workspace> \
  --bitbucket-email <you@company.com> --bitbucket-token <token>
```

The workspace is the part after `bitbucket.org/` in your repository URLs. By default it follows
`develop`, `acceptance` and `main`; change that with `--bitbucket-branches develop,main,release/*`.
These can also go in `.env` (see "Settings in a .env file" below):
`TRIBEBUDDY_BITBUCKET_TOKEN`, `TRIBEBUDDY_BITBUCKET_EMAIL` and `TRIBEBUDDY_BITBUCKET_WORKSPACE`.

What you get:

- The **Pipelines** page: the latest run per repo and branch, newest first — a blinking dot while
  it runs, amber when it waits for a manual step (a deploy), green passed, red failed, grey stopped.
- A notification when a run changes: running (low), waiting for a manual step (normal), passed
  (low), **failed (high)**, stopped (low). Tap it on the recent page for the link to the run.

Bitbucket has no workspace-wide list of pipelines, so the feeder checks repositories one by one,
within `--bitbucket-budget` requests an hour (default 600; Bitbucket allows about 1000):

- At start it checks every repository once to fill the page (one request per repository, a
  minute or two for ~170). That first round gives no notifications.
- Then every minute (`--bitbucket-interval`): repositories with a run in the last 12 hours (any
  branch) or one still going, plus others in rotation with what's left of the budget.

A repository with recent activity is followed within a minute. One that has been quiet for over
12 hours is picked up by the rotation: with ~170 repositories and many active ones, that can
take up to an hour; raise `--bitbucket-budget` to make it faster. Pull request pipelines aren't
followed, and branches must match exactly (`release/acceptance` needs `release/*` or its own
name in `--bitbucket-branches`).

## 3D printer (Bambu Lab)

The 3D Printer app shows a Bambu Lab printer on your network (made for the P1S; the P1P, X1 and
A1 speak the same protocol). The feeder talks to the printer directly — MQTT over TLS on port
8883, and the camera on port 6000 — so no Bambu account, cloud or extra Python package is needed.

1. On the printer's screen, look up the **IP address** and **access code** (Settings > WLAN /
   Network) and the **serial number** (Settings > Device; also in Bambu Studio and Handy). Give
   the printer a fixed IP address in your router so it doesn't change.
2. For the camera, turn on **LAN Only Liveview** (Settings > Network) on newer firmware. You
   don't need LAN-only mode itself.
3. For the camera image the feeder scales the picture down: on macOS with the built-in `sips`,
   elsewhere with Pillow (`python3 -m pip install pillow`).
4. Run the feeder with it:

```sh
python3 feeder.py --port auto --printer-host 192.168.1.50 --printer-serial 01P00A000000000 \
  --printer-code 12345678
```

Or set `TRIBEBUDDY_PRINTER_HOST`, `TRIBEBUDDY_PRINTER_SERIAL` and `TRIBEBUDDY_PRINTER_CODE`.
`--printer-name` changes the name on the display (default "Bambu P1S").

What you get:

- **Status page**: state (printing / preparing / paused / finished / failed / idle / offline), the
  job, progress, layer, time left and when it'll be done, nozzle and bed temperature, and error
  codes.
- **Speed profile**: four buttons — Silent (50%), Standard (100%), Sport (124%), Ludicrous
  (166%). Tap one while a print runs; it turns amber until the printer confirms the change. The
  buttons need a calibrated touchscreen (see Touch calibration).
  Printers on 2025 firmware or newer only accept this with **LAN Only Mode** and **Developer
  Mode** on (printer: Settings > Network / General); otherwise the printer refuses the change
  (error 84033543) and the display shows "Speed not changed". Status and the camera work either
  way. LAN Only Mode turns off the Bambu cloud: Bambu Handy and printing over the internet stop
  working; Bambu Studio and Orca on your network keep working.
- **Camera page** (tap the status page): a snapshot, refreshed every 20 seconds while the page
  is open.
- Notifications when a print starts (low), pauses (normal), finishes (normal) or **fails (high)**.
- On the home screen: the progress as a badge on the icon, and a summary line.

The printer uses a self-signed certificate, so the feeder doesn't verify it; the access code
travels encrypted but only use this on a network you trust. The camera serves one viewer at a
time: while Bambu Studio or Handy shows the live view, snapshots may fail.

## Jira

The Jira app shows your open issues, by default with:

```
assignee = currentUser() AND statusCategory != Done ORDER BY priority
```

1. Create an API token at [id.atlassian.com](https://id.atlassian.com/manage-profile/security/api-tokens)
   ("Create API token", without scopes). The Bitbucket token from above doesn't work for Jira:
   it only has Bitbucket scopes.
2. Run the feeder with it:

```sh
python3 feeder.py --port auto --jira-site tribeagency.atlassian.net \
  --jira-email <you@company.com> --jira-token <token>
```

Or set `TRIBEBUDDY_JIRA_SITE`, `TRIBEBUDDY_JIRA_EMAIL` (falls back to the Bitbucket e-mail) and
`TRIBEBUDDY_JIRA_TOKEN`. Change the search with `--jira-jql "..."` (or `TRIBEBUDDY_JIRA_JQL`); it
runs every 2 minutes (`--jira-interval`).

The page shows the first 7 issues: key, status (coloured: grey to do, green-light in progress), summary, and a
priority stripe (red highest/high, amber medium, grey low). Tap an issue to read the whole
summary; tap again to go back. The home screen shows the number of open issues as a badge.

## Calendar

The Calendar app shows your next 7 meetings and reminders (today and the coming week), with what's
on now and a countdown to the next one, and puts a notification on the display **5 minutes
before** each one. It reads your calendar as an iCal (ICS) feed, so no OAuth app or login is
needed:

- **Google Calendar**: calendar.google.com > Settings > (your calendar, under "Settings for my
  calendars") > Integrate calendar > **Secret address in iCal format**. If it isn't there, your
  Google Workspace admin has switched it off.
- **Outlook / Microsoft 365**: Settings > Calendar > Shared calendars > Publish a calendar > ICS.

Use the **secret** address (it contains `private-`): the public one (`.../public/basic.ics`) gives
`404 Not Found` unless the calendar is shared publicly. Treat that address like a password (anyone with it can read your calendar); the feeder never
logs it. Put it in `.env` (several calendars: separate them with commas):

```sh
TRIBEBUDDY_CALENDAR_ICS=https://calendar.google.com/calendar/ical/.../basic.ics
```

- Recurring meetings, moved or cancelled occurrences, and all-day events are handled. Meetings
  you declined are left out (matched on `TRIBEBUDDY_CALENDAR_EMAIL`, default your Jira/Bitbucket
  e-mail).
- **Reminders**: tasks with a due time in the feed (VTODO, e.g. from Outlook or iCloud) notify
  too. Google Tasks and Google's reminders aren't in Google's iCal feed; add them as events (a
  0-minute event works) to get notified.
- Change the warning time with `TRIBEBUDDY_CALENDAR_NOTIFY=10` (minutes; 0 = no notifications).
  The feed is downloaded every 5 minutes, so a meeting you add shows up within 5 minutes.
- Tap an event for the whole title and location. The home screen icon shows today's date, and a
  badge with the minutes until the next meeting within the hour.

## Approving Claude Code on the display

When Claude Code asks for permission (to run a command, edit a file, ...), the display can show
a card with the project, the tool and the command, and **DENY** / **ALLOW** buttons. Set up once:

```sh
python3 feeder.py install-hooks --approve      # adds the PermissionRequest hook
```

and add `TRIBEBUDDY_APPROVE=1` to `.env` (or start the feeder with `--approve`), then run
`install-service` again.

- Claude Code waits 25 seconds for a tap (the amber bar); after that, or when the feeder or
  board isn't there, it asks in the terminal as usual. Answering in the terminal meanwhile works
  too: the card then disappears.
- The card goes before notifications, and only the two buttons count: a tap elsewhere does
  nothing, so a stray tap can't allow anything.
- It needs the touch position: with an uncalibrated touchscreen the terminal asks instead.
- Turn it off with `install-hooks` (without `--approve`) or `uninstall-hooks`.

## Settings

Tap **Settings** at the bottom of the home screen:

- **Brightness**: `-` / `+`, or tap the bar.
- **Apps on the home screen**: tick the ones you want (at least one).
- **High contrast**: the palette for TN panels seen at an angle (same as the `contrast` command).

They're kept in the board's flash. To set them from your computer instead, put them in `.env`:

```sh
TRIBEBUDDY_BRIGHTNESS=60                     # 10-100
TRIBEBUDDY_APPS=dev,printer,jira,calendar    # which apps (always in this order)
TRIBEBUDDY_CONTRAST=high                     # or standard
```

The feeder sends these each time it connects to the board, so they win over a change made on
the display at the next reconnect or restart. Leave one out of `.env` to control it from the
display only.

## Settings in a .env file

Tokens and other settings can be kept out of the command line in a `.env` file next to
`feeder.py`. It's git-ignored, so tokens don't end up in the repository. Start from the example:

```sh
cp .env.example .env      # then fill in what you use
```

```sh
TRIBEBUDDY_TUNNEL=ngrok
TRIBEBUDDY_TUNNEL_DOMAIN=<your-domain>.ngrok-free.dev
TRIBEBUDDY_WEBHOOK_TOKEN=<secret>
TRIBEBUDDY_BITBUCKET_WORKSPACE=<workspace>
TRIBEBUDDY_BITBUCKET_EMAIL=<you@company.com>
TRIBEBUDDY_BITBUCKET_TOKEN=<api token>
TRIBEBUDDY_PRINTER_HOST=192.168.1.50
TRIBEBUDDY_PRINTER_SERIAL=<serial number>
TRIBEBUDDY_PRINTER_CODE=<access code>
TRIBEBUDDY_JIRA_SITE=tribeagency.atlassian.net
TRIBEBUDDY_JIRA_TOKEN=<api token>
TRIBEBUDDY_CALENDAR_ICS=<secret iCal address>
TRIBEBUDDY_APPROVE=1
TRIBEBUDDY_BRIGHTNESS=60
```

One `KEY=value` per line; `#` starts a comment, and `export ` in front and quotes around the
value are allowed. The same variables also work from your shell (e.g. `~/.zshrc`), but `.env`
wins over them; a command-line option wins over both.

The service doesn't read `.env` itself: `install-service` takes every `TRIBEBUDDY_*` setting
(from `.env` and your shell) along into the service (macOS: the LaunchAgent file, Linux: the
systemd unit; both readable for you only). After changing one, run `install-service` again.

## Plan limits

The `5H` and `WK` rows show how much of your Claude plan's 5-hour and weekly limit you've used,
the same numbers as `/usage` in Claude Code, with a countdown to each reset.

- The feeder reads Claude Code's login (macOS Keychain, or `~/.claude/.credentials.json`) and
  sends it only to `api.anthropic.com`. The first time, macOS asks whether `security` may use
  "Claude Code-credentials": choose **Always Allow**.
- You need to be logged in to Claude Code with a Claude plan; API keys have no plan limits.
- The endpoint is undocumented and may change. If it stops working the feeder logs
  `plan limits unavailable: ...` and keeps going; the display falls back to the `--limit` bar.

## Using the display

- **Home screen**: the time and date, an icon per app (with a badge: unread notifications,
  print progress, open Jira issues, minutes to the next meeting), a summary line per app, and
  the Settings button. Tap an icon to open the app.
- **Tap the header** (the logo) or **hold** (0.7 s or longer) anywhere: back to the home screen.
- **Tap** elsewhere: next page in the app, or dismiss a notification. On the recent notifications
  page, tap a row to read the whole message, and tap again to go back; tap the heading to go to
  the next page.
- **3D Printer** app: the status page with the speed buttons, then the camera page (see
  "3D printer" above).
- **Jira** app: one page with your issues (see "Jira" above).
- **Calendar** app: one page with your agenda (see "Calendar" above).
- **Development** app pages, in this order:
  - **Overview**: today's tokens and cost, plan limits.
  - **Claude**: your Claude Code sessions (waiting / working / idle, for how long, what about),
    and the plan limits at the current pace ("-> 38% at reset", or "full in 1d04h!" in red when
    you'd run out before the reset).
  - **Recent notifications**: the last 7, newest first (cleared when the board restarts).
  - **Pipelines**: the latest Bitbucket Pipelines run per repo and branch (see below).
  - One page per model, the last 14 days, today by hour.
  - **Today by project**: cost and tokens per project folder.
- Header: page dots, a red badge with the number of unread notifications (cleared when you've
  seen the recent page), the time, and the status dot (green live, amber stale, red offline).

Board commands, sent with `python3 feeder.py send <command> --port auto` (stop the service first)
or typed in a serial monitor:

| Command    | Effect                                                   |
|------------|----------------------------------------------------------|
| `demo`     | toggle sample data                                       |
| `reset`    | clear all data                                           |
| `flip`     | rotate the picture 180°                                  |
| `contrast` | switch between high-contrast and the standard palette    |
| `gamma`    | cycle the panel's four gamma curves                      |
| `calibrate`| redo the touch calibration (tap three crosses)           |

`flip`, `contrast` and `gamma` are remembered across restarts.

**Touch calibration** (Cheap Yellow Display): the first time the board starts it asks you to tap
three crosses, so it knows where you tap. It's stored in flash. If you skip it (30 seconds per
cross) it asks again at the next start; until then taps only turn pages (on the home screen a
tap opens Development, and the printer's speed buttons don't work). Redo it with
`python3 feeder.py send calibrate --port auto` (stop the service first) if taps land on the
wrong row.

## Notes

- Token totals leave out cache reads (every request re-reads its context from cache, which
  would dwarf the rest); they're shown as their own field.
- Costs are estimates from public list prices and ignore plan discounts and subscriptions.
- The line protocol between feeder and board is documented at the top of `sketch.ino`.
