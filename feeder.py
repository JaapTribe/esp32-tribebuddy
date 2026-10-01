#!/usr/bin/env python3
"""TribeBuddy feeder: streams Claude usage and notifications to the display.

Usage: Claude Code writes every API response (model, timestamp, token usage) to
~/.claude/projects/**/*.jsonl. This script totals that per model, per day and per
hour (local time) and sends it over serial in the line protocol sketch.ino expects.

Plan limits: the 5-hour and weekly usage of your Claude plan (what /usage shows), read with
Claude Code's own login every --limits-interval seconds. The endpoint is undocumented and
may change; --no-limits turns this off. On macOS the first read asks for Keychain access
("security wants to use ... Claude Code-credentials"): choose Always Allow.

Notifications, shown as a card on the display until tapped:
  * Claude Code: a Notification/Stop hook runs `feeder.py hook`, which queues the event
    in ~/.cache/tribebuddy/notify.jsonl for the running feeder. Set up with install-hooks.
  * Claude Desktop / Cowork (macOS only): read from the macOS notification database.
    That needs Full Disk Access for the app running the feeder (Terminal, iTerm, ...);
    without it the feeder prints a hint and carries on with the rest.
  * Anything else: POST to the webhook, http://localhost:8787/notify (see WebhookListener).

    pip install pyserial
    python3 feeder.py install-hooks            # once: add the Claude Code hooks
    python3 feeder.py                          # Wokwi simulator (rfc2217://localhost:4000)
    python3 feeder.py --port /dev/ttyACM0      # real board on Linux
    python3 feeder.py --port /dev/cu.usbmodem1101 --limit 5000000
    python3 feeder.py --dry-run --once         # print the lines instead of sending
    python3 feeder.py uninstall-hooks          # remove the hooks again
    python3 feeder.py send gamma --port ...    # send a command: demo, reset, flip, contrast, gamma, calibrate
    python3 feeder.py --port auto              # find the board by its USB chip, reconnect on replug
    python3 feeder.py install-service          # start at login (macOS LaunchAgent / systemd --user)
    python3 feeder.py uninstall-service

Costs are estimates from public list prices; they ignore plan discounts and subscriptions.
Works on Linux and macOS (anything with Python 3.8+).
"""

import argparse
import base64
import fnmatch
import getpass
import glob
import json
import os
import plistlib
import re
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
from collections import defaultdict
from datetime import datetime, timedelta, timezone

# USD per million tokens: (input, output, cache read). Cache writes are billed at
# 1.25x input (5-minute TTL) or 2x input (1-hour TTL). Most specific prefix first.
PRICES = [
    ("claude-fable-5-1", (10.00, 50.00, 0.25)),
    ("claude-mythos-5-1", (10.00, 50.00, 0.25)),
    ("claude-fable-5", (10.00, 50.00, 1.00)),
    ("claude-mythos-5", (10.00, 50.00, 1.00)),
    ("claude-opus-5-5", (4.00, 20.00, 0.20)),
    ("claude-opus-5", (5.00, 25.00, 0.50)),
    ("claude-opus-4-8", (5.00, 25.00, 0.50)),
    ("claude-opus-4-7", (5.00, 25.00, 0.50)),
    ("claude-opus-4-6", (5.00, 25.00, 0.50)),
    ("claude-opus-4-5", (5.00, 25.00, 0.50)),
    ("claude-opus-4", (15.00, 75.00, 1.50)),  # Opus 4 / 4.1
    ("claude-sonnet-5-5", (2.00, 10.00, 0.20)),
    ("claude-sonnet-5", (2.00, 10.00, 0.20)),
    ("claude-sonnet-4", (3.00, 15.00, 0.30)),  # Sonnet 4 / 4.5 / 4.6
    ("claude-3-7-sonnet", (3.00, 15.00, 0.30)),
    ("claude-haiku-4-5", (1.00, 5.00, 0.10)),
    ("claude-3-5-haiku", (0.80, 4.00, 0.08)),
    ("claude-3-haiku", (0.25, 1.25, 0.03)),
]
FALLBACK_PRICE = (3.00, 15.00, 0.30)

MAX_MODELS = 6  # must match sketch.ino
LINE_GAP = 0.02  # extra pause between lines; the board's reply is the real pacing
REPLY_TIMEOUT = 2.0  # seconds to wait for the board's {"ok":...} after each line
TICK = 1.0  # main loop period; usage is rescanned every --interval seconds

DESKTOP_APPS = ["com.anthropic.claudefordesktop"]  # Claude Desktop (chat, Cowork, Code tab)
NOTIFY_DB = os.path.expanduser("~/Library/Group Containers/group.com.apple.usernoted/db2/db")
SPOOL = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")),
                     "tribebuddy", "notify.jsonl")
SETTINGS = os.path.join(os.environ.get("CLAUDE_CONFIG_DIR", os.path.expanduser("~/.claude")),
                        "settings.json")
HOOK_EVENTS = ["Notification", "Stop", "SessionStart", "SessionEnd", "UserPromptSubmit", "PostToolUse"]
NOTIFY_EVENTS = {"Notification", "Stop"}  # these also put a card on the display
# Approving permission requests on the display (install-hooks --approve and feeder --approve):
CACHE_DIR = os.path.dirname(SPOOL)
STATE_FILE = os.path.join(CACHE_DIR, "feeder.json")  # heartbeat: is a feeder with a board there?
APPROVALS = os.path.join(CACHE_DIR, "approvals")     # <id>.req from the hook, <id>.res back
APPROVE_WAIT = 25  # seconds the hook waits for a tap before the terminal asks instead

_ASCII = {"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-",
          "\u2014": "-", "\u2026": "...", "\u00a0": " ", "\u2022": "*"}


def to_ascii(text, limit):
    """The display font is ASCII only: fold accents and typographic punctuation."""
    text = "".join(_ASCII.get(c, c) for c in str(text or ""))
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    text = " ".join(text.split())
    return text[:limit]


def shorten(name, limit):
    """Long names lose their middle, so similar ones stay apart: tijd-voor-to..-backend."""
    name = to_ascii(name, 200)
    if len(name) <= limit:
        return name
    head = (limit - 2) // 2
    return name[:head] + ".." + name[-(limit - 2 - head):]


def notice(title, body, src, app=None, prio=None, ttl=None):
    n = {"title": to_ascii(title, 39), "body": to_ascii(body, 159), "src": src}
    if app:
        n["app"] = to_ascii(app, 24)
    if prio and prio != "normal":
        n["prio"] = prio
    if ttl is not None:
        n["ttl"] = max(0, min(int(ttl), 65535))
    return {"notify": n}


def normalize_model(model):
    """'us.anthropic.claude-opus-4-1-20250805-v1:0' -> 'claude-opus-4-1'."""
    m = model.split(".")[-1] if "anthropic." in model else model
    m = re.sub(r"(-v\d+(:\d+)?)$", "", m)
    m = re.sub(r"-\d{8}$", "", m)
    m = re.sub(r"@\d{8}$", "", m)
    return m


def price_for(model):
    for prefix, p in PRICES:
        if model.startswith(prefix):
            return p
    return FALLBACK_PRICE


def cost_of(model, usage, speed):
    p_in, p_out, p_read = price_for(model)
    cc = usage.get("cache_creation") or {}
    cw_total = usage.get("cache_creation_input_tokens", 0) or 0
    cw_1h = cc.get("ephemeral_1h_input_tokens", 0) or 0
    cw_5m = cc.get("ephemeral_5m_input_tokens", cw_total - cw_1h) or 0
    usd = (
        (usage.get("input_tokens", 0) or 0) * p_in
        + (usage.get("output_tokens", 0) or 0) * p_out
        + (usage.get("cache_read_input_tokens", 0) or 0) * p_read
        + cw_5m * p_in * 1.25
        + cw_1h * p_in * 2.0
    ) / 1e6
    return usd * 2 if speed == "fast" else usd  # fast mode: premium pricing (estimate)


class Usage:
    """Totals keyed by (local date, hour, model)."""

    def __init__(self):
        self.buckets = defaultdict(lambda: [0, 0, 0, 0, 0.0])  # in, out, cr, cw, cost
        self.projects = defaultdict(lambda: [0, 0.0])  # (local date, project folder): tokens, cost
        self.seen = set()
        self.offsets = {}
        self.last_model = "-"
        self.last_ts = None

    def scan(self, roots):
        for root in roots:
            for path in glob.glob(os.path.join(root, "**", "*.jsonl"), recursive=True):
                self._read_file(path)

    def _read_file(self, path):
        try:
            size = os.path.getsize(path)
        except OSError:
            return
        start = self.offsets.get(path, 0)
        if size < start:  # truncated or rewritten
            start = 0
        if size == start:
            return
        with open(path, "rb") as f:
            f.seek(start)
            data = f.read()
        # Only consume complete lines; a partial last line is re-read next time.
        end = data.rfind(b"\n") + 1
        self.offsets[path] = start + end
        for raw in data[:end].splitlines():
            self._ingest(raw)

    def _ingest(self, raw):
        if b'"usage"' not in raw:
            return
        try:
            e = json.loads(raw)
        except ValueError:
            return
        msg = e.get("message") or {}
        usage = msg.get("usage")
        model = msg.get("model")
        ts = e.get("timestamp")
        if not usage or not model or not ts or model.startswith("<"):  # e.g. <synthetic>
            return
        # One response is logged as several lines (one per content block); count it once.
        key = (msg.get("id"), e.get("requestId"))
        if key != (None, None):
            if key in self.seen:
                return
            self.seen.add(key)

        try:
            when = datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone()
        except ValueError:
            return
        model = normalize_model(model)
        b = self.buckets[(when.date(), when.hour, model)]
        b[0] += usage.get("input_tokens", 0) or 0
        b[1] += usage.get("output_tokens", 0) or 0
        b[2] += usage.get("cache_read_input_tokens", 0) or 0
        b[3] += usage.get("cache_creation_input_tokens", 0) or 0
        cost = cost_of(model, usage, usage.get("speed"))
        b[4] += cost
        project = os.path.basename((e.get("cwd") or "").rstrip("/")) or "?"
        pr = self.projects[(when.date(), project)]
        pr[0] += (usage.get("input_tokens", 0) or 0) + (usage.get("output_tokens", 0) or 0) \
            + (usage.get("cache_creation_input_tokens", 0) or 0)
        pr[1] += cost
        if self.last_ts is None or when > self.last_ts:
            self.last_ts, self.last_model = when, model

    def messages(self, n_days, limit):
        now = datetime.now(timezone.utc).astimezone()
        today = now.date()

        per_model = defaultdict(lambda: [0, 0, 0, 0, 0.0])
        per_day = defaultdict(lambda: [0, 0.0])
        per_hour = [0] * 24
        first_day = today - timedelta(days=n_days - 1)
        for (day, hour, model), b in self.buckets.items():
            tok = b[0] + b[1] + b[3]  # excludes cache reads (re-read context), like the display
            if day >= first_day:
                per_day[day][0] += tok
                per_day[day][1] += b[4]
            if day == today:
                per_hour[hour] += tok
                pm = per_model[model]
                for i in range(5):
                    pm[i] += b[i]

        tot = [sum(m[i] for m in per_model.values()) for i in range(5)]
        overview = {
            "in": tot[0], "out": tot[1], "cr": tot[2], "cw": tot[3],
            "cost": round(tot[4], 2), "label": "TODAY", "model": self.last_model,
        }
        if limit:
            overview["limit"] = limit

        ranked = sorted(per_model.items(), key=lambda kv: -(kv[1][0] + kv[1][1] + kv[1][3]))[:MAX_MODELS]
        models = [
            {"name": name, "in": m[0], "out": m[1], "cr": m[2], "cw": m[3], "cost": round(m[4], 2)}
            for name, m in ranked
        ]
        days = []
        for i in range(n_days):
            d = first_day + timedelta(days=i)
            tok, cost = per_day.get(d, (0, 0.0))
            days.append({"d": d.strftime("%m-%d"), "tok": tok, "cost": round(cost, 2)})

        ranked_p = sorted(((name, v) for (day, name), v in self.projects.items() if day == today),
                          key=lambda kv: -kv[1][1])[:MAX_MODELS]
        projects = [{"name": to_ascii(name, 24), "tok": v[0], "cost": round(v[1], 2)} for name, v in ranked_p]

        return [
            overview,
            {"models": models},
            {"days": days},
            {"hours": per_hour, "now": now.hour},
            {"projects": projects},
            clock_message(),
        ]


def clock_message():
    """{"time": seconds since 1970 in local time}: the board has no clock of its own."""
    now = datetime.now().astimezone()
    return {"time": int(now.timestamp() + now.utcoffset().total_seconds())}


class HookSpool:
    """Events queued by `feeder.py hook` (one JSON object per line): session updates go to
    `sessions`, Notification/Stop come back as notifications."""

    def __init__(self, sessions, path=SPOOL):
        self.path, self.sessions = path, sessions
        try:
            self.offset = os.path.getsize(path)  # skip anything queued before we started
        except OSError:
            self.offset = 0

    def poll(self):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return []
        if size < self.offset:
            self.offset = 0
        if size == self.offset:
            return []
        with open(self.path, "rb") as f:
            f.seek(self.offset)
            data = f.read()
        end = data.rfind(b"\n") + 1
        self.offset += end
        out = []
        for raw in data[:end].splitlines():
            try:
                n = json.loads(raw)
                if n.get("event"):
                    self.sessions.update(n)
                if n.get("title") or n.get("body"):  # older lines have no "event"
                    out.append(notice(n.get("title"), n.get("body"), n.get("src", "code")))
            except (ValueError, AttributeError):
                pass
        if size > 256 * 1024 and self.offset == size:  # keep the spool small
            try:
                open(self.path, "w").close()
                self.offset = 0
            except OSError:
                pass
        return out


class DesktopNotifications:
    """Claude Desktop notifications from the macOS notification database (undocumented).

    Needs Full Disk Access for the process running the feeder. The database is copied
    (with its WAL) before reading so the live one is never locked.
    """

    def __init__(self, apps=DESKTOP_APPS, path=NOTIFY_DB):
        self.apps, self.path = apps, path
        self.enabled = sys.platform == "darwin"
        self.last_id = None
        self.tmp = tempfile.mkdtemp(prefix="tribebuddy-")
        if self.enabled:
            self.poll()  # establishes last_id so old notifications aren't replayed

    def _query(self, sql, args=()):
        for suffix in ("", "-wal", "-shm"):
            src = self.path + suffix
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(self.tmp, "db" + suffix))
        con = sqlite3.connect(os.path.join(self.tmp, "db"))
        try:
            return con.execute(sql, args).fetchall()
        finally:
            con.close()

    def poll(self):
        if not self.enabled:
            return []
        marks = ",".join("?" * len(self.apps))
        try:
            rows = self._query(
                "SELECT r.rec_id, r.data FROM record r JOIN app a ON a.app_id = r.app_id "
                f"WHERE a.identifier IN ({marks}) AND r.rec_id > ? ORDER BY r.rec_id",
                (*self.apps, self.last_id or 0))
        except (OSError, sqlite3.Error) as e:
            self.enabled = False
            print("Claude Desktop notifications disabled: can't read the macOS notification "
                  f"database ({e}).\nGive Full Disk Access (System Settings > Privacy & Security) "
                  "to the app that runs the feeder: your terminal app, or when it runs as a service, "
                  f"this Python:\n  {os.path.realpath(sys.executable)}\nthen restart the feeder.",
                  file=sys.stderr)
            return []
        first = self.last_id is None
        out = []
        for rec_id, data in rows:
            self.last_id = max(self.last_id or 0, rec_id)
            if first:
                continue
            try:
                req = plistlib.loads(data).get("req", {})
            except Exception:
                continue
            title = req.get("titl") or "Claude"
            body = " - ".join(x for x in (req.get("subt"), req.get("body")) if x)
            out.append(notice(title, body, "desktop"))
        if first and self.last_id is None:
            self.last_id = 0
        return out


class PlanLimits:
    """Plan usage limits (5-hour session, weekly) as shown by Claude Code's /usage.

    Uses the OAuth token Claude Code stores (macOS Keychain, else ~/.claude/.credentials.json)
    against an undocumented endpoint, so it may break without notice. The token is only sent
    to api.anthropic.com. It is never refreshed here: refresh tokens rotate, and refreshing
    would log Claude Code out. An expired token is skipped until Claude Code renews it.
    """

    URL = "https://api.anthropic.com/api/oauth/usage"
    KEYCHAIN_SERVICE = "Claude Code-credentials"
    CREDENTIALS = os.path.join(os.environ.get("CLAUDE_CONFIG_DIR", os.path.expanduser("~/.claude")),
                               ".credentials.json")
    WINDOWS = [("five_hour", "5h"), ("seven_day", "7d")]
    LENGTH = {"5h": 5 * 3600, "7d": 7 * 86400}  # window lengths, for the forecast

    def __init__(self, interval, log):
        self.interval, self.log = interval, log
        self.windows = None  # {"5h": (percent, resets_at or None), ...} from the last good poll
        self.good_at = 0.0   # monotonic time of that poll
        self.next_poll = 0.0
        self.last_error = None
        self.thread = None   # the fetch runs in the background: a Keychain prompt or a slow
        self.result = None   # network must not hold up notifications

    def _token(self):
        raw = None
        if sys.platform == "darwin":
            r = subprocess.run(["security", "find-generic-password", "-s", self.KEYCHAIN_SERVICE, "-w"],
                               capture_output=True, text=True, timeout=10)
            raw = r.stdout.strip() if r.returncode == 0 else None
        if not raw and os.path.exists(self.CREDENTIALS):
            with open(self.CREDENTIALS) as f:
                raw = f.read()
        if not raw:
            raise RuntimeError("no Claude Code login found (run `claude` and log in with your plan)")
        oauth = json.loads(raw).get("claudeAiOauth") or {}
        if not oauth.get("accessToken"):
            raise RuntimeError("Claude Code isn't logged in with a Claude plan (API keys have no plan limits)")
        if oauth.get("expiresAt") and oauth["expiresAt"] / 1000 < time.time():
            raise RuntimeError("Claude Code's login token has expired; it renews the next time you use Claude Code")
        return oauth["accessToken"]

    def _fetch(self):
        import urllib.request
        req = urllib.request.Request(self.URL, headers={
            "Authorization": f"Bearer {self._token()}",
            "anthropic-beta": "oauth-2025-04-20",
            "User-Agent": "tribebuddy-feeder",
        })
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)

    def _run(self):
        try:
            self.result = (self._fetch(), None)
        except Exception as e:  # network, HTTP error, keychain, bad JSON
            self.result = (None, e)

    def poll(self, wait=False):
        """Starts a fetch every `interval` seconds and picks up its result; True when new
        numbers came in. `wait` blocks until the fetch is done (for --once)."""
        if self.thread is None and time.monotonic() >= self.next_poll:
            self.next_poll = time.monotonic() + self.interval
            self.result = None
            self.thread = threading.Thread(target=self._run, daemon=True)
            self.thread.start()
        if self.thread is None:
            return False
        if wait:
            self.thread.join()
        if self.thread.is_alive():
            return False
        self.thread = None
        data, e = self.result
        if e is None:
            windows = self._parse(data)
            if not windows:
                e = RuntimeError("unexpected response (no utilization); the endpoint may have changed")
        if e is not None:  # keep the last numbers until they go stale (see message)
            err = f"{e.code} {e.reason}" if hasattr(e, "code") else str(e)
            if getattr(e, "code", None) == 429:
                self.next_poll = time.monotonic() + max(self.interval, 600)
            if err != self.last_error:
                self.log(f"plan limits unavailable: {err}")
                self.last_error = err
            return False
        if self.last_error:
            self.log("plan limits available again")
        self.last_error = None
        self.windows, self.good_at = windows, time.monotonic()
        return True

    def _parse(self, data):
        windows = {}
        if not isinstance(data, dict):
            return windows
        for key, short in self.WINDOWS:
            w = data.get(key)
            if not isinstance(w, dict) or w.get("utilization") is None:
                continue
            reset = None
            if w.get("resets_at"):
                try:
                    reset = datetime.fromisoformat(w["resets_at"].replace("Z", "+00:00"))
                except ValueError:
                    pass
            try:
                windows[short] = (float(w["utilization"]), reset)
            except (TypeError, ValueError):
                pass
        return windows

    def message(self):
        """{"limits":{"5h":42,"5h_in":4320,...}}: percent used and seconds until the reset."""
        # Numbers from 3 failed polls ago are stale: stop sending them (the board then greys
        # them out and later falls back to the --limit bar).
        if not self.windows or time.monotonic() - self.good_at > 3 * self.interval + 60:
            return None
        now = datetime.now(timezone.utc)
        out = {}
        for short, (pct, reset) in self.windows.items():
            out[short] = round(max(0.0, min(pct, 100.0)))
            # 0 = reset time unknown; a reset that has passed is sent as 1 so the board says "reset"
            left = int((reset - now).total_seconds()) if reset else 0
            out[short + "_in"] = max(1, left) if reset else 0
            # Forecast at the pace since the window started: percent at the reset, and seconds
            # until 100% when that comes first. Too early in the window (<10%) to say.
            elapsed = self.LENGTH[short] - left
            if reset and left > 0 and elapsed > self.LENGTH[short] * 0.1 and pct > 0:
                rate = pct / elapsed  # percent per second
                out[short + "_proj"] = min(999, round(pct + rate * left))
                if pct < 100 and pct + rate * left >= 100:
                    out[short + "_full"] = int((100 - pct) / rate)
        return {"limits": out}


class WebhookListener:
    """Generic webhook: any app or script can put a notification on the display.

        curl -d "Deploy finished" "http://localhost:8787/notify?app=CI"
        curl -H "Content-Type: application/json" -d '{"app":"GitHub","title":"Deploy failed",
             "message":"tribe-website: build failed on main","priority":"high"}' localhost:8787/notify

    Accepts JSON, form fields or a plain-text body (the message), plus query parameters.
    Fields (first alias found wins):
      app      app, source, src, sender, app_name  - card header
      message  message, body, text, msg, content   - required (or a title)
      title    title, subject                      - optional big line
      priority priority, prio, level, severity     - low / normal / high, or 1-5 (ntfy style)
      ttl      seconds on screen, 0 = until tapped (default: --notify-seconds)
    Listens on localhost only unless a token is set (Authorization: Bearer <token>,
    X-Token: <token> or ?token=<token>). Requests carrying a browser Origin header are refused,
    so web pages you visit can't post to it.
    """

    MAX_BODY = 16 * 1024
    MAX_PENDING = 20
    ALIASES = {
        "app": ("app", "source", "src", "sender", "app_name"),
        "message": ("message", "body", "text", "msg", "content"),
        "title": ("title", "subject"),
        "priority": ("priority", "prio", "level", "severity"),
        "ttl": ("ttl",),
    }
    KNOWN = {k for names in ALIASES.values() for k in names}
    HIGH = {"high", "urgent", "critical", "max", "emergency", "error", "4", "5"}
    LOW = {"low", "min", "info", "debug", "1", "2"}

    def __init__(self, host, port, token, log):
        import http.server
        self.token, self.log = token, log
        self.pending, self.lock = [], threading.Lock()
        listener = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):  # quiet: delivered notifications are logged by the feeder
                pass

            def reply(self, code, obj):
                data = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path.split("?")[0] in ("/", "/health"):
                    self.reply(200, {"ok": True, "service": "tribebuddy"})
                else:
                    self.reply(404, {"ok": False, "error": "POST to /notify"})

            def do_POST(self):
                code, obj = listener.handle(self)
                self.reply(code, obj)

        self.server = http.server.ThreadingHTTPServer((host, port), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @classmethod
    def field(cls, data, name):
        for key in cls.ALIASES[name]:
            v = data.get(key)
            if isinstance(v, list):  # parse_qs gives lists
                v = v[0] if v else None
            if v is not None and str(v).strip() != "":
                return v
        return None

    @classmethod
    def priority(cls, value):
        v = str(value or "").strip().lower()
        return "high" if v in cls.HIGH else "low" if v in cls.LOW else "normal"

    def _authorized(self, req, query):
        if not self.token:
            return True
        import hmac
        auth = req.headers.get("Authorization", "")
        given = (auth[7:] if auth.lower().startswith("bearer ") else None) \
            or req.headers.get("X-Token") or (query.get("token") or [None])[0]
        return bool(given) and hmac.compare_digest(given.encode(), self.token.encode())

    def handle(self, req):
        """Parses one POST; returns (HTTP status, JSON reply)."""
        from urllib.parse import parse_qs, urlsplit
        url = urlsplit(req.path)
        if url.path not in ("/", "/notify"):
            return 404, {"ok": False, "error": "POST to /notify"}
        if req.headers.get("Origin"):
            return 403, {"ok": False, "error": "browser requests are not accepted"}
        query = parse_qs(url.query)
        if not self._authorized(req, query):
            return 401, {"ok": False, "error": "missing or wrong token"}
        try:
            length = int(req.headers.get("Content-Length") or 0)
        except ValueError:
            return 400, {"ok": False, "error": "bad Content-Length"}
        if length > self.MAX_BODY:
            return 413, {"ok": False, "error": f"body over {self.MAX_BODY} bytes"}
        raw = req.rfile.read(length).decode("utf-8", errors="replace") if length else ""
        ctype = (req.headers.get("Content-Type") or "").split(";")[0].strip().lower()

        data = dict(query)
        data.pop("token", None)
        if ctype == "application/json" or (not ctype and raw.lstrip().startswith("{")):
            try:
                body = json.loads(raw or "{}")
            except ValueError:
                return 400, {"ok": False, "error": "invalid JSON"}
            if not isinstance(body, dict):
                return 400, {"ok": False, "error": "JSON body must be an object"}
            data.update(body)
        elif ctype == "application/x-www-form-urlencoded" and any(
                k in self.KNOWN for k in parse_qs(raw)):
            data.update(parse_qs(raw))
        elif raw.strip():  # plain text, or `curl -d "some text"` (sent as a form without fields)
            data["message"] = raw

        message, title = self.field(data, "message"), self.field(data, "title")
        if message is None and title is None:
            return 400, {"ok": False, "error": "a message (or title) is required"}
        prio = self.priority(self.field(data, "priority"))
        ttl = self.field(data, "ttl")
        try:
            ttl = int(ttl) if ttl is not None else None  # None: --notify-seconds, with the progress bar
        except (TypeError, ValueError):
            return 400, {"ok": False, "error": "ttl must be a number of seconds"}
        n = notice(title or "", message or "", "hook", self.field(data, "app") or "Webhook", prio, ttl)
        with self.lock:
            if len(self.pending) >= self.MAX_PENDING:
                return 503, {"ok": False, "error": "too many pending notifications"}
            self.pending.append(n)
        return 202, {"ok": True}

    def poll(self):
        with self.lock:
            out, self.pending = self.pending, []
        return out


class BitbucketPipelines:
    """Pipelines on chosen branches across a whole Bitbucket Cloud workspace, via the REST API.

    Bitbucket has no workspace-wide pipeline list, and a repository's updated_on doesn't change
    on a push, so: the first scan checks every repository once (fills the page, no
    notifications). After that, every `interval` seconds, repositories with a run in the last
    HOT hours (any branch) or one still going are checked, plus a few others in rotation, within
    `budget` requests an hour (Bitbucket allows about 1000). A repository that wakes up after a
    quiet spell is therefore noticed within one rotation; from then on it's checked every scan.
    A changed state gives a notification; `message()` is the latest run per repo and branch for
    the Pipelines page. Runs in a thread.

    Auth: an Atlassian API token with scopes read:repository:bitbucket and read:pipeline:bitbucket
    plus your Atlassian e-mail (Basic auth), or a workspace/repository access token (no e-mail;
    sent as Bearer).
    """

    API = "https://api.bitbucket.org/2.0"
    HOT = 12 * 3600      # repos with a run this recent are checked every scan
    REPO_REFRESH = 1800  # seconds between fetching the list of repositories
    MIN_ROTATION = 3     # repos per scan always kept for the rotation
    MAX_ROWS = 8         # what the page shows
    PRIO = {"running": "low", "paused": "normal", "passed": "low", "failed": "high", "stopped": "low"}
    WORDS = {"running": "running", "paused": "waiting for a manual step", "passed": "passed",
             "failed": "FAILED", "stopped": "stopped"}

    def __init__(self, workspace, token, email, branches, interval, log, budget=600):
        self.workspace, self.branches, self.interval, self.log = workspace, branches, interval, log
        self.budget = budget  # API requests per hour
        self.repos, self.repos_at = [], 0.0
        self.activity = {}    # repo -> time of its newest run (any branch)
        self.cursor = 0       # position in the rotation
        self.auth = ("Basic " + base64.b64encode(f"{email}:{token}".encode()).decode()) if email \
            else f"Bearer {token}"
        self.state = {}       # (repo, branch) -> latest run
        self.notices, self.changed = [], False
        self.lock = threading.Lock()
        self.first = True     # the first scan fills the page without notifications
        self.last_error = None
        threading.Thread(target=self._loop, daemon=True).start()

    def _get(self, path, **params):
        import urllib.parse
        import urllib.request
        url = f"{self.API}/{path}?{urllib.parse.urlencode(params)}"
        req = urllib.request.Request(url, headers={"Authorization": self.auth, "Accept": "application/json",
                                                   "User-Agent": "tribebuddy-feeder"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.load(r)

    def _branch_ok(self, ref):
        return any(fnmatch.fnmatchcase(ref, pat) for pat in self.branches)

    @staticmethod
    def _status(p):
        st = p.get("state") or {}
        if st.get("name") == "COMPLETED":
            res = (st.get("result") or {}).get("name")
            return {"SUCCESSFUL": "passed", "FAILED": "failed", "ERROR": "failed"}.get(res, "stopped")
        if (st.get("stage") or {}).get("name") == "PAUSED":
            return "paused"
        return "running"  # PENDING, IN_PROGRESS

    @staticmethod
    def _time(text):
        """Seconds since 1970, or None. Bitbucket writes 5 to 9 decimals ("21.059759143Z");
        Python before 3.11 only reads exactly 3 or 6, so pad/cut them to 6."""
        if not text:
            return None
        t = re.sub(r"\.(\d+)", lambda m: "." + (m.group(1) + "000000")[:6], str(text).replace("Z", "+00:00"))
        try:
            return datetime.fromisoformat(t).timestamp()
        except ValueError:
            return None

    def _loop(self):
        while True:
            try:
                self._scan()
                if self.last_error:
                    self.log("Bitbucket pipelines available again")
                self.last_error = None
                wait = self.interval
            except Exception as e:
                err = f"{e.code} {e.reason}" if hasattr(e, "code") else str(e)
                hint = {401: " (check the token, and --bitbucket-email for an API token)",
                        403: " (the token needs read:repository and read:pipeline)",
                        404: " (check --bitbucket-workspace)"}.get(getattr(e, "code", None), "")
                if err != self.last_error:
                    self.log(f"Bitbucket pipelines unavailable: {err}{hint}")
                    self.last_error = err
                wait = max(self.interval, 600) if getattr(e, "code", None) == 429 else self.interval
            time.sleep(wait)

    def _repo_list(self):
        slugs, page = [], 1
        while page <= 20:  # 2000 repositories at most
            v = self._get(f"repositories/{self.workspace}", pagelen=100, page=page, fields="values.slug,next")
            slugs += [r["slug"] for r in v.get("values", [])]
            if not v.get("next"):
                break
            page += 1
        return slugs

    def _scan(self):
        now = time.time()
        if not self.repos or now - self.repos_at > self.REPO_REFRESH:
            self.repos, self.repos_at = self._repo_list(), now
        if self.first:
            todo = list(self.repos)  # one full sweep to fill the page
        else:
            per_scan = max(self.MIN_ROTATION + 1, int(self.budget * self.interval / 3600))
            busy = {repo for (repo, _), run in self.state.items() if run["state"] in ("running", "paused")}
            hot = sorted((r for r in self.repos if r in busy or now - self.activity.get(r, 0) < self.HOT),
                         key=lambda r: -self.activity.get(r, 0))[:per_scan - self.MIN_ROTATION]
            cold = [r for r in self.repos if r not in hot]  # hot ones over the cap rotate too
            todo = list(hot)
            for _ in range(min(per_scan - len(hot), len(cold))):
                todo.append(cold[self.cursor % len(cold)])
                self.cursor += 1
        for slug in todo:
            self._check(slug)
        self.first = False

    def _check(self, slug):
        runs = self._get(f"repositories/{self.workspace}/{slug}/pipelines/", sort="-created_on", pagelen=20,
                         fields="values.uuid,values.build_number,values.state,values.target.type,"
                                "values.target.ref_name,values.created_on,values.completed_on,"
                                "values.creator.display_name").get("values", [])
        if runs:
            self.activity[slug] = max(self._time(p.get("created_on")) or 0 for p in runs)
        seen = set()
        for p in runs:  # newest first: the first per branch is the latest
            target = p.get("target") or {}
            ref = target.get("ref_name")
            if target.get("type") != "pipeline_ref_target" or not ref or not self._branch_ok(ref) or ref in seen:
                continue
            seen.add(ref)
            status = self._status(p)
            started = self._time(p.get("created_on")) or time.time()
            done = self._time(p.get("completed_on")) if status in ("passed", "failed", "stopped") else None
            self._update({"repo": slug, "branch": ref, "state": status, "num": p.get("build_number"),
                          "by": (p.get("creator") or {}).get("display_name") or "",
                          "since": done or started, "uuid": p.get("uuid")})

    def _update(self, run):
        key = (run["repo"], run["branch"])
        with self.lock:
            old = self.state.get(key)
            if old and old["uuid"] == run["uuid"] and old["state"] == run["state"]:
                return
            self.state[key] = run
            self.changed = True
            if self.first:
                return
            url = f"https://bitbucket.org/{self.workspace}/{run['repo']}/pipelines/results/{run['num']}"
            short = "FAILED" if run["state"] == "failed" else run["state"]  # the card title fits ~17 chars
            self.notices.append(notice(f"{run['branch']} {short}",
                                       f"Pipeline #{run['num']} {self.WORDS[run['state']]} on {run['branch']}"
                                       + (f", started by {run['by']}" if run["by"] else "") + f". {url}",
                                       "hook", shorten(run["repo"], 24), self.PRIO[run["state"]]))

    def poll(self):
        """Notifications for runs that changed since the last call."""
        with self.lock:
            out, self.notices = self.notices, []
        return out

    def message(self):
        with self.lock:
            runs = sorted(self.state.values(), key=lambda r: -r["since"])[:self.MAX_ROWS]
            self.changed = False
        now = time.time()
        return {"pipelines": [{"repo": shorten(r["repo"], 18), "branch": shorten(r["branch"], 20),
                               "state": r["state"], "for": max(0, int(now - r["since"])),
                               "num": r["num"] or 0, "by": to_ascii(r["by"], 20)} for r in runs]}


class NgrokTunnel:
    """Runs `ngrok http` in front of the webhook so cloud services (Jira, Bitbucket) can reach it.

    Set up once: install ngrok (brew install ngrok), `ngrok config add-authtoken <token>` from
    dashboard.ngrok.com. Free accounts get one fixed domain assigned (not chosen: custom names
    are paid, ERR_NGROK_313); it's under Universal Gateway > Domains on the dashboard. Restarted when it exits; stopped with the feeder.
    """

    SEARCH = ["/opt/homebrew/bin", "/usr/local/bin", os.path.expanduser("~/.local/bin"), "/snap/bin"]

    def __init__(self, port, domain, token, log):
        self.port, self.domain, self.token, self.log = port, domain, token, log
        self.proc, self.url, self.restart_at = None, None, 0.0
        self.backoff, self.last_error = 10, None
        # launchd starts services with a minimal PATH, so look in the usual install places too
        self.binary = shutil.which("ngrok") or shutil.which("ngrok", path=os.pathsep.join(self.SEARCH))
        if not self.binary:
            raise RuntimeError("ngrok not found: brew install ngrok (or see ngrok.com/download)")
        import atexit
        atexit.register(self.stop)

    def start(self):
        cmd = [self.binary, "http", f"127.0.0.1:{self.port}", "--log", "stdout", "--log-format", "json"]
        if self.domain:
            cmd += ["--url", self.domain if "://" in self.domain else "https://" + self.domain]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, text=True)
        threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()

    def _read(self, proc):
        for raw in proc.stdout:
            try:
                e = json.loads(raw)
            except ValueError:
                continue
            if e.get("msg") == "started tunnel" and e.get("url"):
                self.url, self.backoff, self.last_error = e["url"], 10, None
                self.log(f"tunnel up: {self.url}/notify  (send the token as "
                         f"'Authorization: Bearer ...' or ?token=...)")
                if not self.domain:
                    self.log("ngrok picked this URL; pass it as --tunnel-domain to pin it")
            elif e.get("lvl") in ("eror", "crit") and e.get("err") not in (None, "<nil>"):
                err = str(e["err"]).strip().splitlines()[0]  # ngrok errors run over several lines
                code = re.search(r"ERR_NGROK_\d+", str(e["err"]))
                err += f" ({code.group(0)})" if code else ""
                if err != self.last_error:  # each distinct error once
                    self.log(f"ngrok: {err}")
                    self.last_error = err

    def tick(self):
        """Restarts ngrok after it exits (network change, auth problem, ...): 10 s, doubling to 5 min."""
        if self.proc and self.proc.poll() is not None:
            if self.url or self.backoff == 10:
                self.log(f"tunnel down (ngrok exited with {self.proc.returncode}); retrying, "
                         "backing off to every 5 minutes while it keeps failing")
            self.proc, self.url = None, None
            self.restart_at = time.monotonic() + self.backoff
            self.backoff = min(self.backoff * 2, 300)
        if self.proc is None and time.monotonic() >= self.restart_at:
            self.start()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ---------- Claude Code hooks ----------
def hook_main():
    """Run by Claude Code: queue the hook event on stdin for the running feeder (session state,
    and a notification for Notification/Stop).

    Never fails and prints nothing, so it can't disturb the Claude Code session.
    """
    try:
        e = json.load(sys.stdin)
        project = os.path.basename((e.get("cwd") or "").rstrip("/")) or "Claude Code"
        event = e.get("hook_event_name")
        if event == "PermissionRequest":
            approval_hook(e, project)
            return
        if event not in HOOK_EVENTS or (event == "Stop" and e.get("stop_hook_active")):
            return
        rec = {"event": event, "session": e.get("session_id"), "project": project, "ts": time.time()}
        if event == "Notification":
            rec.update(title=project, body=e.get("message") or "Needs your attention", src="code")
        elif event == "Stop":
            rec.update(title=project, body="Finished - waiting for you", src="code")
        elif event == "UserPromptSubmit":
            rec["prompt"] = to_ascii(e.get("prompt") or e.get("user_prompt"), 80)
        os.makedirs(os.path.dirname(SPOOL), exist_ok=True)
        with open(SPOOL, "a") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass


def tool_summary(tool_input):
    """The part of a tool call worth reading before allowing it: the command, file, URL, ..."""
    inp = tool_input if isinstance(tool_input, dict) else {}
    for key in ("command", "file_path", "url", "notebook_path", "path", "pattern", "query", "prompt"):
        if inp.get(key):
            return str(inp[key])
    return json.dumps(inp)


def approval_hook(e, project):
    """PermissionRequest: ask on the display and print the decision. Printing nothing (feeder not
    running, no board, no tap within APPROVE_WAIT) lets the terminal ask as usual."""
    try:
        with open(STATE_FILE) as f:
            state = json.load(f)
    except (OSError, ValueError):
        return
    if not state.get("approve") or not state.get("board") or time.time() - state.get("ts", 0) > 5:
        return
    os.makedirs(APPROVALS, mode=0o700, exist_ok=True)
    rid = secrets.token_hex(8)
    req, res = os.path.join(APPROVALS, rid + ".req"), os.path.join(APPROVALS, rid + ".res")
    with open(req + ".tmp", "w") as f:
        json.dump({"project": project, "tool": e.get("tool_name") or "?",
                   "detail": tool_summary(e.get("tool_input")), "ts": time.time()}, f)
    os.replace(req + ".tmp", req)
    try:
        end = time.time() + APPROVE_WAIT
        while time.time() < end:
            if os.path.exists(res):
                with open(res) as f:
                    answer = json.load(f)
                if "allow" in answer:  # else the board couldn't ask: terminal
                    behavior = "allow" if answer["allow"] is True else "deny"
                    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest",
                                                             "decision": {"behavior": behavior}}}))
                return
            time.sleep(0.2)
    finally:
        for path in (req, res):
            try:
                os.remove(path)
            except OSError:
                pass


class Approvals:
    """Feeder side of approving on the display: shows requests the hook queued, and writes the
    answer when the board reports a tap on ALLOW or DENY. Only the board can answer; the id is
    random and must be one this feeder sent."""

    def __init__(self, log):
        self.log, self.sent = log, {}  # id -> request
        os.makedirs(APPROVALS, mode=0o700, exist_ok=True)
        for name in os.listdir(APPROVALS):  # left over from a crash
            try:
                os.remove(os.path.join(APPROVALS, name))
            except OSError:
                pass

    def poll(self):
        """Board messages: new requests to show, and cancels for ones the hook gave up on."""
        try:
            ids = {n[:-4] for n in os.listdir(APPROVALS) if n.endswith(".req")}
        except OSError:
            return []
        out = []
        for rid in sorted(ids - self.sent.keys()):
            try:
                with open(os.path.join(APPROVALS, rid + ".req")) as f:
                    r = json.load(f)
            except (OSError, ValueError):
                continue
            self.sent[rid] = r
            out.append({"approve": {"id": rid, "app": to_ascii(r.get("project"), 24),
                                    "tool": to_ascii(r.get("tool"), 23), "detail": to_ascii(r.get("detail"), 159)}})
        for rid in [k for k in self.sent if k not in ids]:
            del self.sent[rid]
            out.append({"approve_cancel": rid})
        return out

    def _answer(self, rid, answer):
        path = os.path.join(APPROVALS, rid + ".res")
        try:
            with open(path + ".tmp", "w") as f:
                json.dump(answer, f)
            os.replace(path + ".tmp", path)
        except OSError:
            pass

    def decide(self, rid, allow):
        r = self.sent.get(rid)
        if r is None:
            return
        self._answer(rid, {"allow": bool(allow)})
        self.log(f"{'ALLOWED' if allow else 'denied'} on the display: {r.get('project')} "
                 f"{r.get('tool')}: {str(r.get('detail'))[:80]}")

    def unavailable(self, rid):
        """The board couldn't ask (no touch position, unplugged): let the terminal ask now."""
        self._answer(rid, {})

    def drop_all(self):
        for rid in list(self.sent):
            self.unavailable(rid)


class Sessions:
    """What each Claude Code session is doing, from the hook events.

    working: after a prompt or a tool call; waiting: Claude asked for something (permission,
    input); idle: finished its turn. SessionEnd removes it; one without events for 12 hours
    is dropped too (closed without SessionEnd, e.g. a killed terminal).
    """

    MAX_AGE = 12 * 3600

    def __init__(self):
        self.s = {}  # session id -> {"name", "state", "since", "msg", "last"}
        self.changed = False

    def update(self, rec):
        sid, event, now = rec.get("session"), rec.get("event"), rec.get("ts") or time.time()
        if not sid:
            return
        if event == "SessionEnd":
            self.changed |= self.s.pop(sid, None) is not None
            return
        cur = self.s.setdefault(sid, {"name": rec.get("project") or "?", "state": "idle",
                                      "since": now, "msg": "started", "last": now})
        state, msg = {
            "SessionStart": ("idle", "started"),
            "UserPromptSubmit": ("working", rec.get("prompt") or "working"),
            "PostToolUse": ("working", None),  # keep the prompt as the description
            "Notification": ("waiting", rec.get("body")),
            "Stop": ("idle", "finished"),
        }.get(event, (cur["state"], None))
        if state != cur["state"]:
            cur["state"], cur["since"] = state, now
        if msg:
            cur["msg"] = msg
        cur["last"] = now
        self.changed = True

    def message(self):
        now = time.time()
        for sid in [k for k, v in self.s.items() if now - v["last"] > self.MAX_AGE]:
            del self.s[sid]
        order = {"waiting": 0, "working": 1, "idle": 2}
        ranked = sorted(self.s.values(), key=lambda v: (order[v["state"]], -v["last"]))[:6]
        self.changed = False
        return {"sessions": [{"name": to_ascii(v["name"], 24), "state": v["state"],
                              "for": max(0, int(now - v["since"])), "msg": to_ascii(v["msg"], 60)}
                             for v in ranked]}


def _our_hook(entry):
    return any(h.get("args", [])[-1:] == ["hook"] and "feeder.py" in " ".join(h.get("args", []))
               for h in entry.get("hooks", []))


def _load_settings():
    if not os.path.exists(SETTINGS):
        return {}
    with open(SETTINGS) as f:
        text = f.read()
    return json.loads(text) if text.strip() else {}


def _save_settings(settings):
    if os.path.exists(SETTINGS):
        shutil.copyfile(SETTINGS, SETTINGS + ".tribebuddy.bak")
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    tmp = SETTINGS + ".tmp"
    with open(tmp, "w") as f:
        json.dump(settings, f, indent=2)
        f.write("\n")
    os.replace(tmp, SETTINGS)


def install_hooks(argv=()):
    approve = "--approve" in argv
    settings = _load_settings()
    hooks = settings.setdefault("hooks", {})
    cmd = {"type": "command", "command": sys.executable,
           "args": [os.path.abspath(__file__), "hook"]}  # exec form: no shell, spaces in paths are fine
    events = {e: {**cmd, "async": True, "timeout": 10} for e in HOOK_EVENTS}
    if approve:  # waits for the tap, so not async
        events["PermissionRequest"] = {**cmd, "timeout": APPROVE_WAIT + 15}
    for event in set(events) | {"PermissionRequest"}:
        lst = [x for x in hooks.get(event, []) if not _our_hook(x)]  # replace an older install
        if event in events:
            lst.append({"hooks": [events[event]]})
        if lst:
            hooks[event] = lst
        else:
            hooks.pop(event, None)
    _save_settings(settings)
    print(f"Added TribeBuddy hooks ({', '.join(sorted(events))}) to {SETTINGS}")
    if approve:
        print("Approving on the display also needs the feeder started with --approve.")
    print("Running Claude Code sessions pick them up after /hooks or a restart.")


def uninstall_hooks():
    settings = _load_settings()
    hooks = settings.get("hooks", {})
    for event in list(hooks):
        hooks[event] = [x for x in hooks[event] if not _our_hook(x)]
        if not hooks[event]:
            del hooks[event]
    if not hooks:
        settings.pop("hooks", None)
    _save_settings(settings)
    print(f"Removed TribeBuddy hooks from {SETTINGS}")


# USB vendor ids of the USB-serial chips on ESP32 boards, for --port auto.
BOARD_VIDS = {0x1A86: "CH340", 0x10C4: "CP210x", 0x0403: "FTDI", 0x303A: "Espressif USB"}
SIM_PORT = "rfc2217://localhost:4000"


def _serial():
    try:
        import serial  # pyserial
        import serial.tools.list_ports  # noqa: F401
    except ImportError:
        sys.exit(f"pyserial is missing: {sys.executable} -m pip install pyserial")
    return serial


def find_board():
    """First connected serial port with a known ESP32 USB chip, or None."""
    for p in sorted(_serial().tools.list_ports.comports(), key=lambda p: p.device):
        if p.vid in BOARD_VIDS and not p.device.startswith("/dev/tty."):  # macOS: use cu.*
            return p.device
    return None


def open_port(url, baud):
    serial = _serial()
    local = "://" not in url
    # exclusive: a second feeder (or `send`) gets "busy" instead of interleaving its lines
    port = serial.serial_for_url(url, baudrate=baud, timeout=1, do_not_open=True,
                                 **({"exclusive": True} if local and os.name == "posix" else {}))
    if local:
        # USB-serial boards (CYD) wire DTR/RTS to EN/IO0; keep them released so opening the
        # port doesn't reset the board (or hold it in the bootloader).
        port.dtr = False
        port.rts = False
    port.open()
    # If the board did reset anyway, wait for its {"ready":...} line before sending.
    end = time.monotonic() + 2.5
    while time.monotonic() < end:
        if b'"ready"' in port.readline():
            break
    return port


# ---------- start at login ----------
SERVICE_LABEL = "nl.tribeagency.tribebuddy"
LAUNCH_AGENT = os.path.expanduser(f"~/Library/LaunchAgents/{SERVICE_LABEL}.plist")
MAC_LOG = os.path.expanduser("~/Library/Logs/tribebuddy.log")
# The service runs a copy from here: on macOS a background agent can't read ~/Documents
# (where the project usually lives) without an extra privacy permission.
SERVICE_DIR = (os.path.expanduser("~/Library/Application Support/TribeBuddy") if sys.platform == "darwin"
               else os.path.join(os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")),
                                 "tribebuddy"))
SYSTEMD_UNIT = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")),
                            "systemd", "user", "tribebuddy.service")


def install_service(extra):
    """Runs the feeder at login with --port auto (plus any extra feeder options)."""
    _serial()  # the service uses this same Python, so pyserial must be installed for it
    os.makedirs(SERVICE_DIR, exist_ok=True)
    script = os.path.join(SERVICE_DIR, "feeder.py")
    if os.path.abspath(__file__) != script:
        shutil.copyfile(os.path.abspath(__file__), script)
    cmd = [sys.executable, script, "--port", "auto", *extra]
    # A service doesn't see your shell's variables: take the TRIBEBUDDY_* ones along (tokens,
    # tunnel domain, ...). The file is made readable for you only.
    env = {k: v for k, v in sorted(os.environ.items()) if k.startswith("TRIBEBUDDY_")}
    if sys.platform == "darwin":
        os.makedirs(os.path.dirname(LAUNCH_AGENT), exist_ok=True)
        with open(LAUNCH_AGENT, "wb") as f:
            plistlib.dump({
                "Label": SERVICE_LABEL,
                "ProgramArguments": cmd,
                "WorkingDirectory": SERVICE_DIR,
                "EnvironmentVariables": {"PYTHONUNBUFFERED": "1", **env},
                "RunAtLoad": True,
                "KeepAlive": True,
                "ThrottleInterval": 10,
                "StandardOutPath": MAC_LOG,
                "StandardErrorPath": MAC_LOG,
            }, f)
        os.chmod(LAUNCH_AGENT, 0o600)
        domain = f"gui/{os.getuid()}"
        subprocess.run(["launchctl", "bootout", domain, LAUNCH_AGENT], capture_output=True)
        subprocess.run(["launchctl", "bootstrap", domain, LAUNCH_AGENT], check=True)
        print(f"Installed {LAUNCH_AGENT}; the feeder runs now and at every login.")
        if env:
            print("Took along: " + ", ".join(env) + " (run install-service again after changing them)")
        print(f"It runs a copy in {SERVICE_DIR}: run install-service again after changing feeder.py.")
        print(f"Log: {MAC_LOG}   (tail -f {MAC_LOG})")
        print("For Claude Desktop notifications, give Full Disk Access to this Python:\n  "
              + os.path.realpath(sys.executable))
    elif sys.platform.startswith("linux"):
        os.makedirs(os.path.dirname(SYSTEMD_UNIT), exist_ok=True)
        with open(SYSTEMD_UNIT, "w") as f:
            f.write("[Unit]\nDescription=TribeBuddy feeder (Claude usage display)\n\n"
                    "[Service]\n"
                    f"ExecStart={' '.join(json.dumps(c) for c in cmd)}\n"
                    "Environment=PYTHONUNBUFFERED=1\n"
                    + "".join(f"Environment={json.dumps(f'{k}={v}')}\n" for k, v in env.items())
                    + "Restart=always\nRestartSec=10\n\n"
                    "[Install]\nWantedBy=default.target\n")
        os.chmod(SYSTEMD_UNIT, 0o600)
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
        subprocess.run(["systemctl", "--user", "enable", "--now", "tribebuddy.service"], check=True)
        print(f"Installed {SYSTEMD_UNIT}; the feeder runs now and at every login.")
        if env:
            print("Took along: " + ", ".join(env) + " (run install-service again after changing them)")
        print(f"It runs a copy in {SERVICE_DIR}: run install-service again after changing feeder.py.")
        print("Log: journalctl --user -u tribebuddy -f")
        print(f"If the board isn't found: sudo usermod -aG dialout {getpass.getuser()} (then log in again)")
    else:
        sys.exit("install-service supports macOS and Linux")


def uninstall_service():
    if sys.platform == "darwin":
        subprocess.run(["launchctl", "bootout", f"gui/{os.getuid()}", LAUNCH_AGENT], capture_output=True)
        if os.path.exists(LAUNCH_AGENT):
            os.remove(LAUNCH_AGENT)
        print("Removed the TribeBuddy LaunchAgent.")
    elif sys.platform.startswith("linux"):
        subprocess.run(["systemctl", "--user", "disable", "--now", "tribebuddy.service"], capture_output=True)
        if os.path.exists(SYSTEMD_UNIT):
            os.remove(SYSTEMD_UNIT)
        subprocess.run(["systemctl", "--user", "daemon-reload"], capture_output=True)
        print("Removed the TribeBuddy systemd user service.")


def send_command(argv):
    ap = argparse.ArgumentParser(prog="feeder.py send")
    ap.add_argument("command", choices=["demo", "reset", "flip", "contrast", "gamma", "calibrate"])
    ap.add_argument("--port", default=SIM_PORT, help="serial port, 'auto', or pyserial URL")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args(argv)
    dev = find_board() if args.port == "auto" else args.port
    if not dev:
        sys.exit("no board found")
    port = open_port(dev, args.baud)
    port.write(args.command.encode() + b"\n")
    port.flush()
    if args.command == "calibrate":
        print("Tap the crosses on the display...")
    end = time.monotonic() + (100 if args.command == "calibrate" else 2)  # 3 taps, 30 s each
    while time.monotonic() < end:  # print the board's reply, e.g. {"gamma":2}
        reply = port.readline().decode(errors="replace").strip()
        if reply.startswith("{") and '"ready"' not in reply:
            print(reply)
            break
    port.close()


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "send":
        send_command(sys.argv[2:])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "install-service":
        install_service(sys.argv[2:])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "uninstall-service":
        uninstall_service()
        return
    if len(sys.argv) > 1 and sys.argv[1] in ("hook", "install-hooks", "uninstall-hooks"):
        if sys.argv[1] == "install-hooks":
            install_hooks(sys.argv[2:])
        else:
            {"hook": hook_main, "uninstall-hooks": uninstall_hooks}[sys.argv[1]]()
        return

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=SIM_PORT,
                    help="serial port, 'auto' (find the board by its USB chip), or pyserial URL "
                         "(default: Wokwi simulator)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--interval", type=float, default=10, help="seconds between updates")
    ap.add_argument("--days", type=int, default=14, help="days in the per-day chart (max 14)")
    ap.add_argument("--limit", type=int, default=0, help="daily token limit for the progress bar")
    ap.add_argument("--projects", action="append",
                    help="Claude Code projects dir (repeatable; default ~/.claude/projects)")
    ap.add_argument("--once", action="store_true", help="send one update and exit")
    ap.add_argument("--dry-run", action="store_true", help="print lines instead of sending")
    ap.add_argument("--notify-seconds", type=int, default=15,
                    help="how long a notification stays on the display (0 = until tapped)")
    ap.add_argument("--no-desktop", action="store_true",
                    help="don't read Claude Desktop notifications (macOS)")
    ap.add_argument("--limits-interval", type=float, default=120,
                    help="seconds between plan limit checks (5-hour and weekly)")
    ap.add_argument("--webhook-port", type=int, default=8787,
                    help="port of the notification webhook (0 = off); POST to /notify")
    ap.add_argument("--webhook-host", default="127.0.0.1",
                    help="address the webhook listens on; anything but localhost needs a token")
    ap.add_argument("--webhook-token", default=os.environ.get("TRIBEBUDDY_WEBHOOK_TOKEN"),
                    help="shared secret for the webhook (or set TRIBEBUDDY_WEBHOOK_TOKEN)")
    ap.add_argument("--tunnel", choices=["ngrok"], default=os.environ.get("TRIBEBUDDY_TUNNEL") or None,
                    help="make the webhook reachable from the internet (Jira, Bitbucket); needs a token")
    ap.add_argument("--tunnel-domain", default=os.environ.get("TRIBEBUDDY_TUNNEL_DOMAIN"),
                    help="your ngrok domain, e.g. discharge-gimmick-lard.ngrok-free.dev (Dashboard > Domains)")
    ap.add_argument("--bitbucket-workspace", default=os.environ.get("TRIBEBUDDY_BITBUCKET_WORKSPACE"),
                    help="Bitbucket Cloud workspace to follow pipelines in (e.g. tribeagency)")
    ap.add_argument("--bitbucket-token", default=os.environ.get("TRIBEBUDDY_BITBUCKET_TOKEN"),
                    help="Atlassian API token (read:repository, read:pipeline) or a workspace access token "
                         "(or set TRIBEBUDDY_BITBUCKET_TOKEN)")
    ap.add_argument("--bitbucket-email", default=os.environ.get("TRIBEBUDDY_BITBUCKET_EMAIL"),
                    help="your Atlassian e-mail, needed with an API token (not with an access token)")
    ap.add_argument("--bitbucket-branches", default="develop,acceptance,main",
                    help="branches to follow, comma-separated; wildcards like release/* work")
    ap.add_argument("--bitbucket-interval", type=float, default=60, help="seconds between pipeline checks")
    ap.add_argument("--bitbucket-budget", type=int, default=600,
                    help="Bitbucket API requests per hour at most (Bitbucket allows about 1000)")
    ap.add_argument("--approve", action="store_true",
                    help="let Claude Code permission requests be allowed/denied by tapping the display "
                         "(also needs install-hooks --approve)")
    ap.add_argument("--no-limits", action="store_true",
                    help="don't show plan limits (skips reading Claude Code's login token)")
    args = ap.parse_args()

    roots = args.projects or [os.path.join(
        os.environ.get("CLAUDE_CONFIG_DIR", os.path.expanduser("~/.claude")), "projects")]
    n_days = max(1, min(args.days, 14))
    port = None  # opened (and reopened after an unplug) in the main loop
    usage = Usage()
    sessions = Sessions()
    spool = HookSpool(sessions)
    desktop = DesktopNotifications() if not args.no_desktop else None

    def log(text):
        print(time.strftime("[%H:%M:%S] ") + text, flush=True)

    def wait_reply():
        """The board answers every line with {"ok":...}; skip anything else (e.g. boot text)."""
        end = time.monotonic() + REPLY_TIMEOUT
        while time.monotonic() < end:
            raw = port.readline().decode(errors="replace").strip()
            if raw.startswith('{"ok"'):
                return raw
            board_event(raw)
            if '"ready"' in raw:
                log("board restarted")
        return None

    def board_event(raw):
        """Lines the board sends by itself: {"event":"approve","id":...,"allow":true}."""
        if not raw.startswith('{"event"') or not approvals:
            return
        try:
            ev = json.loads(raw)
        except ValueError:
            return
        if ev.get("event") == "approve" and isinstance(ev.get("id"), str):
            approvals.decide(ev["id"], ev.get("allow") is True)

    def read_events():
        """Taps arrive between our own lines: read whatever the board sent meanwhile."""
        nonlocal port
        try:
            while port is not None and port.in_waiting:
                board_event(port.readline().decode(errors="replace").strip())
        except OSError:
            pass  # unplugged: the next send notices and reconnects

    def write_state():
        """Heartbeat for the PermissionRequest hook: only ask on the display when it's there."""
        try:
            os.makedirs(CACHE_DIR, exist_ok=True)
            with open(STATE_FILE + ".tmp", "w") as f:
                json.dump({"ts": time.time(), "approve": bool(approvals), "board": port is not None}, f)
            os.replace(STATE_FILE + ".tmp", STATE_FILE)
        except OSError:
            pass

    def connect():
        """Opens the board's port; None (and a log line once) while it isn't there."""
        nonlocal waiting
        dev = find_board() if args.port == "auto" else args.port
        try:
            if dev:
                p = open_port(dev, args.baud)
                log(f"connected to {dev}")
                waiting = False
                return p
        except OSError as e:  # busy, unplugged, simulator not running
            if not waiting:
                log(f"can't open {dev}: {e}")
        if not waiting:
            log("waiting for the board..." if args.port == "auto" else f"retrying {dev} every 5s...")
            waiting = True
        return None

    def send(msgs):
        """Sends each line and waits for the board's reply; returns the number rejected."""
        nonlocal port
        failed = 0
        for msg in msgs:
            if "notify" in msg:
                msg["notify"].setdefault("ttl", max(0, min(args.notify_seconds, 65535)))
            line = json.dumps(msg, separators=(",", ":"))
            if args.dry_run:
                print(line, flush=True)
                continue
            if port is None:  # disconnected earlier in this batch
                failed += 1
                continue
            try:
                port.reset_input_buffer()
                port.write(line.encode() + b"\n")
                port.flush()
                reply = wait_reply()
            except OSError:  # unplugged (pyserial's SerialException is an OSError)
                log("board disconnected")
                try:
                    port.close()
                except OSError:
                    pass
                port = None
                failed += 1
                continue
            if reply is None:
                failed += 1
                log(f"no reply from the board ({len(line)} byte line: {line[:60]}...)")
            elif '"ok":false' in reply:
                failed += 1
                log(f"board rejected a {len(line)} byte line starting {line[:40]!r}: {reply}")
            time.sleep(LINE_GAP)
        return failed

    def fmt_tok(n):
        return f"{n / 1e6:.2f}M" if n >= 1e6 else f"{n / 1e3:.1f}K" if n >= 1e4 else str(n)

    limits = PlanLimits(max(30.0, args.limits_interval), log) if not args.no_limits else None
    webhook = None
    if args.webhook_port:
        if args.webhook_host not in ("127.0.0.1", "localhost", "::1") and not args.webhook_token:
            sys.exit(f"--webhook-host {args.webhook_host} is reachable from other machines: "
                     "set --webhook-token (or TRIBEBUDDY_WEBHOOK_TOKEN) too")
        try:
            webhook = WebhookListener(args.webhook_host, args.webhook_port, args.webhook_token, log)
        except OSError as e:  # port taken, e.g. by a second feeder
            log(f"webhook off: can't listen on {args.webhook_host}:{args.webhook_port} ({e})")
    pipelines = None
    if args.bitbucket_workspace:
        if not args.bitbucket_token:
            sys.exit("--bitbucket-workspace needs --bitbucket-token (or TRIBEBUDDY_BITBUCKET_TOKEN)")
        branches = [b.strip() for b in args.bitbucket_branches.split(",") if b.strip()]
        pipelines = BitbucketPipelines(args.bitbucket_workspace, args.bitbucket_token, args.bitbucket_email,
                                       branches, max(30.0, args.bitbucket_interval), log,
                                       budget=max(60, args.bitbucket_budget))
        log(f"following Bitbucket pipelines in {args.bitbucket_workspace} on {', '.join(branches)}")
    tunnel = None
    if args.tunnel:
        if not args.webhook_port:
            sys.exit("--tunnel needs the webhook (--webhook-port)")
        if not args.webhook_token:
            import secrets
            sys.exit("--tunnel puts the webhook on the internet: set --webhook-token (or "
                     "TRIBEBUDDY_WEBHOOK_TOKEN) too, for example:\n  --webhook-token "
                     + secrets.token_urlsafe(24))
        if webhook:
            try:
                tunnel = NgrokTunnel(args.webhook_port, args.tunnel_domain, args.webhook_token, log)
            except RuntimeError as e:
                log(f"tunnel off: {e}")
    sources = ["Claude Code hooks"] + (["Claude Desktop"] if desktop and desktop.enabled else []) \
        + ([f"webhook http://{args.webhook_host}:{args.webhook_port}/notify"] if webhook else [])
    log(f"TribeBuddy feeder -> {'stdout (dry run)' if args.dry_run else args.port}")
    log(f"usage every {args.interval:g}s from {', '.join(roots)}; "
        f"notifications from {' + '.join(sources)}. Ctrl+C to stop.")

    approvals = Approvals(log) if args.approve else None
    next_scan = 0.0
    waiting = False
    while True:
        if tunnel:
            tunnel.tick()
        if approvals:
            write_state()
            if port is None and not args.dry_run:
                approvals.drop_all()  # no board: the terminal asks
        if not args.dry_run and port is None:
            port = connect()
            if port is None:
                if args.once:
                    sys.exit(1)
                time.sleep(5)
                continue
            next_scan = 0.0  # fresh connection (or a restarted board): send everything now
        read_events()
        if approvals:
            for m in approvals.poll():  # one at a time: a rejected one goes back to the terminal
                if send([m]) and "approve" in m:
                    approvals.unavailable(m["approve"]["id"])
        notes = spool.poll() + (desktop.poll() if desktop else []) + (webhook.poll() if webhook else []) \
            + (pipelines.poll() if pipelines else [])
        if notes:
            failed = send(notes)
            if not args.dry_run:
                for n in notes:
                    m = n["notify"]
                    log(f"notification ({m.get('app') or m['src']}, {m.get('prio', 'normal')}): "
                        f"{m['title']} - {m['body']}"
                        + (" [not delivered]" if failed else ""))
        if sessions.changed and time.monotonic() < next_scan:
            send([sessions.message()])  # a session changed state: show it now
        if pipelines and pipelines.changed and time.monotonic() < next_scan:
            send([pipelines.message()])
        lim = limits.message() if limits and limits.poll(wait=args.once) else None
        if lim and time.monotonic() < next_scan:
            send([lim])  # new numbers between usage scans
            if not args.dry_run:
                log("plan limits: " + ", ".join(f"{k} {v}%" for k, v in lim["limits"].items()
                                                if "_" not in k))
        if time.monotonic() >= next_scan:
            usage.scan(roots)
            msgs = usage.messages(n_days, args.limit) + [sessions.message()] \
                + ([pipelines.message()] if pipelines else [])
            lim = limits.message() if limits else None
            failed = send(msgs + ([lim] if lim else []))  # limits resent too, e.g. after a replug
            if not args.dry_run:
                o = msgs[0]
                tok = o["in"] + o["out"] + o["cw"]
                log(f"usage: {fmt_tok(tok)} tokens today excl. cache reads (${o['cost']:.2f}), "
                    f"{len(msgs[1]['models'])} model(s) -> "
                    + ("board ok" if not failed else f"{failed} of {len(msgs) + bool(lim)} lines failed"))
            next_scan = time.monotonic() + args.interval
        if args.once:
            break
        time.sleep(TICK)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
