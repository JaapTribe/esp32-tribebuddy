# TribeBuddy

A desk display for Claude usage. An ESP32 with a 2.8" touchscreen shows today's tokens and
estimated cost, per-model, per-day and per-hour charts, your plan limits (5-hour and weekly),
and a notification card when Claude Code or Claude Desktop needs you — or when any other app
or script posts to its webhook.

The board has no network connection: `feeder.py` runs on your computer, reads Claude Code's
local logs, and sends the numbers over USB serial.

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
arduino-cli lib install "Adafruit GFX Library" "Adafruit ILI9341" "Adafruit FT6206 Library" ArduinoJson
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
finishes:

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
   fixed domain assigned, like `discharge-gimmick-lard.ngrok-free.dev`; you can't pick the
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

- **Tap**: next page, or dismiss a notification. On the recent notifications page, tap a row to
  read the whole message, and tap again to go back; tap the heading to go to the next page.
- **Hold** (0.7 s or longer): back to the overview.
- Pages: overview, one per model, last 14 days, today by hour, recent notifications (the
  last 7, newest first, with how long ago; cleared when the board restarts).

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
cross) it asks again at the next start; until then taps only turn pages. Redo it with
`python3 feeder.py send calibrate --port auto` (stop the service first) if taps land on the
wrong row.

## Notes

- Token totals leave out cache reads (every request re-reads its context from cache, which
  would dwarf the rest); they're shown as their own field.
- Costs are estimates from public list prices and ignore plan discounts and subscriptions.
- The line protocol between feeder and board is documented at the top of `sketch.ino`.
