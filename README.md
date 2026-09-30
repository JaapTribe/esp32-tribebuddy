# TribeBuddy

A desk display for Claude usage. An ESP32 with a 2.8" touchscreen shows today's tokens and
estimated cost, per-model, per-day and per-hour charts, your plan limits (5-hour and weekly),
and a notification card when Claude Code or Claude Desktop needs you.

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

- **Tap**: next page, or dismiss a notification.
- **Hold** (0.7 s or longer): back to the overview.
- Pages: overview, one per model, last 14 days, today by hour.

Board commands, sent with `python3 feeder.py send <command> --port auto` (stop the service first)
or typed in a serial monitor:

| Command    | Effect                                                   |
|------------|----------------------------------------------------------|
| `demo`     | toggle sample data                                       |
| `reset`    | clear all data                                           |
| `flip`     | rotate the picture 180°                                  |
| `contrast` | switch between high-contrast and the standard palette    |
| `gamma`    | cycle the panel's four gamma curves                      |

`flip`, `contrast` and `gamma` are remembered across restarts.

## Notes

- Token totals leave out cache reads (every request re-reads its context from cache, which
  would dwarf the rest); they're shown as their own field.
- Costs are estimates from public list prices and ignore plan discounts and subscriptions.
- The line protocol between feeder and board is documented at the top of `sketch.ino`.
