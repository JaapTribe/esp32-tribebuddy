// TribeBuddy · Claude token monitor · Tribe Agency edition
// Boards: ESP32 "Cheap Yellow Display" (ESP32-2432S028R) or ESP32-S3 + ILI9341 module (the
// Wokwi simulator). The pin profile is picked from the chip you compile for; see Pins below.
// Reads newline-delimited JSON over serial and shows it on an ILI9341 2.8" TFT (portrait).
// Home screen: a clock, one icon per app, and a one-line summary of each. Tap an icon to open
// the app; tap the header (logo) or hold anywhere to come back.
// Apps and their pages (tap = next page, or dismiss a notification):
//   Development: overview, Claude (sessions, limit forecast), recent notifications (tap one to
//                read it), Bitbucket pipelines, one page per model, usage per day, usage per hour
//                (today), cost per project (today).
//   3D Printer:  print status with the speed profile buttons, camera snapshot.
//   Jira:        your open issues (tap one to read it).
//   Calendar:    your upcoming meetings and reminders (tap one to read it).
//   Settings:    brightness, which apps show on the home screen, contrast (button on home).
//
// Line protocol (one JSON object per line, all fields optional; feeder.py sends these):
//   Overview: {"in":123456,"out":7890,"cr":2000000,"cw":150000,"cost":4.21,
//              "limit":5000000,"label":"TODAY","model":"claude-opus-4"}
//   Models:   {"models":[{"name":"claude-opus-4","in":1,"out":2,"cr":3,"cw":4,"cost":0.5}, ...]}
//   Days:     {"days":[{"d":"09-24","tok":123456,"cost":1.2}, ...]}   oldest first, last = today
//   Hours:    {"hours":[0,0,1200, ...24 values],"now":14}              index = local hour
//   Limits:   {"limits":{"5h":42,"5h_in":4320,"7d":61,"7d_in":190000,"7d_proj":88,"5h_full":2400}}
//             plan usage in percent and seconds until each window resets (counted down here;
//             0 = unknown). Replaces the "limit" bar. Greyed out after 10 minutes without an
//             update, dropped after 30. Optional forecast at the current pace: _proj = percent
//             at the reset, _full = seconds until 100% when that comes before the reset.
//   Projects: {"projects":[{"name":"tribe-website","tok":950545,"cost":21.96}, ...]}  today, max 6
//   Sessions: {"sessions":[{"name":"tribe-website","state":"waiting","for":120,"msg":"..."}, ...]}
//             Claude Code sessions, max 6. state: working / waiting / idle; for: seconds in it.
//   Time:     {"time":1790786407}  local time as seconds since 1970 (the board has no clock)
//   Pipelines: {"pipelines":[{"repo":"tribe-website","branch":"main","state":"failed","for":300,
//             "num":812,"by":"Jaap"}, ...]}  latest Bitbucket run per repo/branch, newest first, max 8.
//             state: running / paused (manual step) / passed / failed / stopped; for: seconds since.
//   Notify:   {"notify":{"title":"TribeBuddy","body":"Needs permission: Bash","src":"code",
//                        "app":"GitHub","prio":"high"}}
//             src: "code" (Claude Code hook), "desktop" (Claude Desktop / Cowork) or "hook"
//             (webhook). app: shown as the card's header instead of the src name. prio: "low"
//             (no blink, dim border), "normal" (default) or "high" (red border, longer blink,
//             shown before other queued ones). Shown as a card with a progress bar; it closes
//             when the bar is full (optional "ttl" in seconds, default 15, 0 = until tapped) or
//             on tap. Up to 4 are queued.
//   Printer:  {"printer":{"on":true,"name":"Bambu P1S","st":"RUNNING","job":"benchy","pct":42,
//             "left":83,"layer":120,"layers":300,"noz":219,"nozt":220,"bed":60,"bedt":60,
//             "spd":2,"err":0}}  on: feeder connected to the printer; st: gcode_state (IDLE,
//             PREPARE, RUNNING, PAUSE, FINISH, FAILED); left: minutes; spd: 1 silent,
//             2 standard, 3 sport, 4 ludicrous; err: print_error code (0 = none).
//   Camera:   {"snap":{"len":9876}} then {"snapd":"<base64>"} lines until len bytes are in: a
//             baseline JPEG, 240 wide, at most 180 high and SNAP_MAX bytes. {"snap_err":"..."}
//             when the feeder couldn't get one.
//   Jira:     {"jira":[{"k":"TRIB-12","s":"summary","st":"In Progress","c":"i","p":2}, ...],
//             "jira_total":9}  max 7; c: status category (n new, i in progress, d done);
//             p: priority 1 (highest) to 5 (lowest); jira_total: all matching issues.
//   Calendar: {"cal":[{"t":"Standup","l":"Meet","s":1790935200,"e":1790937000,"ad":0,"r":0}, ...]}
//             upcoming events, max 7; s/e: start/end in local seconds since 1970 (like "time");
//             ad: all day; r: a reminder (task with a due time). Notifications 5 minutes before
//             come from the feeder as normal "notify" lines.
//   Approve:  {"approve":{"id":"9f2c...","app":"tribe-website","tool":"Bash","detail":"rm -rf dist",
//             "ttl":25}}  a Claude Code permission request: a card with DENY / ALLOW, ahead of
//             notifications; ttl = seconds Claude Code waits. Answered {"ok":false} when the
//             board can't ask (touch not calibrated, 4 waiting): the terminal asks instead.
//             {"approve_cancel":"9f2c..."}: Claude Code stopped waiting, drop the card.
//   Settings: {"settings":{"bright":60,"apps":"dev,printer,jira,calendar","contrast":"high"}}
//             any of them; stored in flash, like changes on the Settings page.
// Events the board sends by itself (on taps): {"event":"speed","level":3} (set the printer's
//           speed profile), {"event":"snapshot"} (camera page open: send a new image),
//           {"event":"approve","id":"9f2c...","allow":true} (ALLOW / DENY tapped).
// Commands (plain text lines): "demo" toggles demo mode, "reset" clears all data,
//           "flip" turns the display 180 degrees, "contrast" toggles the high-contrast
//           palette, "gamma" cycles the panel's 4 gamma curves (all remembered across restarts),
//           "calibrate" runs the touch calibration again (XPT2046; also runs on first boot).
//
// Touch: XPT2046 (resistive) on the CYD; on the S3 profile FT6206 (capacitive, I2C; the
// simulator) is tried first, then XPT2046 (the TPM408-2.8 module).
// ESP32-S3 hardware: in Arduino IDE set Tools > "USB CDC On Boot" = Enabled so Serial is USB.

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_FT6206.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <TJpg_Decoder.h>
#include <mbedtls/base64.h>
#include "tribe_logo.h"

// ---- Pins ----
#if CONFIG_IDF_TARGET_ESP32
// ESP32-2432S028R "Cheap Yellow Display": display and touch each have their own SPI bus.
#define BOARD_NAME "cyd"
#define TFT_CS   15
#define TFT_DC    2
#define TFT_MOSI 13
#define TFT_SCK  14
#define TFT_MISO 12
#define TFT_RST  -1  // tied to EN
#define TFT_BL   21
#define TOUCH_CS  33
#define TOUCH_IRQ 36  // input-only pin, pulled up on the board
#define TOUCH_SCK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_OWN_BUS 1
#define HAS_FT6206 0  // GPIO 6-11 belong to the flash chip here; no I2C touch probe
// This CYD's panel (TPM408-2.8) is landscape-native: portrait needs the controller's
// row/column swap (rotation 1 or 3) while the drawing area stays 240x320.
#define PANEL_LANDSCAPE_NATIVE 1
#define DISPLAY_ROTATION 1  // portrait with the USB port at the top
#else
// ESP32-S3 + ILI9341 module (matches diagram.json); touch shares the display's SPI bus.
#define BOARD_NAME "s3"
#define TFT_CS   10
#define TFT_DC    9
#define TFT_MOSI 11
#define TFT_SCK  12
#define TFT_MISO 13
#define TFT_RST  14
#define TFT_BL   21
#define TOUCH_CS  15  // XPT2046 T_CS (T_CLK/T_DIN/T_DO share SCK/MOSI/MISO)
#define TOUCH_IRQ 16  // XPT2046 T_IRQ, or 255 if not wired
#define TOUCH_OWN_BUS 0
#define HAS_FT6206 1
#define CTP_SDA    8  // FT6206 (simulator)
#define CTP_SCL   18
#define PANEL_LANDSCAPE_NATIVE 0
#define DISPLAY_ROTATION 0  // portrait
#endif
// The "flip" serial command turns the picture 180 degrees; the choice is kept in flash.

// ---- Colors (RGB565) · Tribe Agency brand palette (tribeagency.nl) ----
#define C_PANEL  0x1A07  // #1D433B green
#define C_CARD   0x0903  // #0A201B darker green: notification card, more contrast for its text
#define C_ACCENT 0xCF2F  // #CFE578 green-light
#define C_DIM_STD 0x7C4B  // #7C8B5A army
#define C_DIM_HC  0xB633  // #B7C49A light sage: readable at an angle on TN panels
// "contrast" switches between high contrast (default, for TN panels seen at an angle) and the
// standard brand palette.
uint16_t C_BG = 0x0000;      // black      | standard: #001C0E green-dark (0x00E1)
uint16_t C_TEXT = 0xFFFF;    // white      | standard: #FEF6EF creme (0xFFBD)
uint16_t C_DIM = C_DIM_HC;   // light sage | standard: army; labels, status, bars
#define C_GREEN  0x4E8A
#define C_AMBER  0xFD20
#define C_RED    0xE8C4
#define C_BLUE   0x243F  // #2684FF: Jira app icon
#define C_INK    0x2104  // #212121: strokes on the printer app icon

// Adafruit_ILI9341 assumes a 240x320 panel and sets the drawing size from the rotation;
// on a landscape-native panel that size is swapped, so swap it back.
class Display : public Adafruit_ILI9341 {
 public:
  using Adafruit_ILI9341::Adafruit_ILI9341;
  void setRotation(uint8_t r) override {
    Adafruit_ILI9341::setRotation(r);
    if (PANEL_LANDSCAPE_NATIVE) std::swap(_width, _height);
  }
};
Display tft(&SPI, TFT_DC, TFT_CS, TFT_RST);
Adafruit_FT6206 ctp;
enum { TOUCH_NONE, TOUCH_FT6206, TOUCH_XPT2046 } touchType = TOUCH_NONE;
// XPT2046 raw -> screen: raw values of the calibration crosses at x 20/220 and y 20/300.
struct TouchCal {
  bool ok = false;
  bool swap = false;  // raw Y runs along screen x
  bool flip = false;  // display was flipped while calibrating
  int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
} touchCal;
const int CAL_X0 = 20, CAL_X1 = 220, CAL_Y0 = 20, CAL_Y1 = 300;

struct Usage {
  uint64_t in = 0, out = 0, cacheR = 0, cacheW = 0, limit = 0;
  float cost = 0;
  char label[16] = "WAITING";
  char model[40] = "-";
  bool valid = false;
} u;

struct ModelUsage {
  char name[40];
  uint64_t in, out, cacheR, cacheW;
  float cost;
};
const int MAX_MODELS = 6;
ModelUsage models[MAX_MODELS];
int nModels = 0;

struct DayUsage {
  char d[6];  // "MM-DD"
  uint64_t tok;
  float cost;
};
const int MAX_DAYS = 14;
DayUsage days[MAX_DAYS];
int nDays = 0;

// Plan limits: [0] = 5-hour session, [1] = weekly.
struct PlanLimits {
  int pct[2] = {-1, -1};         // -1 = not reported
  uint32_t resetIn[2] = {0, 0};  // seconds until the reset when received, 0 = unknown
  int proj[2] = {-1, -1};        // forecast: percent at the reset, -1 = too early to tell
  uint32_t full[2] = {0, 0};     // forecast: seconds until 100% (before the reset), 0 = not
  unsigned long rxAt = 0;
  bool valid = false;
} lim;
const unsigned long LIMITS_STALE_MS = 10 * 60 * 1000UL, LIMITS_DROP_MS = 30 * 60 * 1000UL;

struct ProjectUsage {
  char name[25];
  uint64_t tok;
  float cost;
};
ProjectUsage projects[MAX_MODELS];
int nProjects = 0;

struct Session {
  char name[25];
  char state;  // 'w' working, 'a' waiting (asked for something), 'i' idle
  uint32_t forS;  // seconds in that state when received
  char msg[61];
};
struct Pipeline {
  char repo[25];
  char branch[21];
  char state;     // 'r' running, 'p' paused (manual step), 's' passed, 'f' failed, 'x' stopped
  uint32_t forS;  // seconds since it started (running/paused) or finished, when received
  uint32_t num;
  char by[21];
};
const int MAX_PIPELINES = 8;
Pipeline pipelines[MAX_PIPELINES];
int nPipelines = 0;
unsigned long pipelinesAt = 0;
bool pipelinesValid = false;  // false: the feeder doesn't follow Bitbucket

// Bambu Lab printer, relayed by the feeder (it talks MQTT to the printer).
struct Printer {
  bool on = false;     // the feeder is connected to the printer
  char name[21] = "3D printer";
  char st[10] = "";    // gcode_state: IDLE, PREPARE, RUNNING, PAUSE, FINISH, FAILED
  char job[33] = "";
  int pct = -1, layer = 0, layers = 0;
  uint32_t left = 0;   // minutes remaining when received
  int noz = 0, nozT = 0, bed = 0, bedT = 0;
  int spd = 0;         // speed profile 1-4, 0 = unknown
  uint32_t err = 0;
  unsigned long rxAt = 0;
} prn;
bool printerValid = false;  // false: the feeder doesn't follow a printer
int spdPending = 0;         // speed level tapped, until the printer reports it (or 10 s pass)
unsigned long spdPendingAt = 0;

// Camera snapshot: the last complete JPEG, kept so the page can be redrawn.
const size_t SNAP_MAX = 24 * 1024;
uint8_t* snapBuf = nullptr;
size_t snapLen = 0, snapGot = 0;  // bytes announced / received of the image coming in
size_t snapReady = 0;             // bytes of the complete image in snapBuf, 0 = none
unsigned long snapAt = 0, snapAskedAt = 0;
char snapErr[48] = "";
const unsigned long SNAP_EVERY_MS = 20000;  // refresh while the camera page is open

struct JiraIssue {
  char key[16];
  char sum[101];
  char st[21];
  char cat;      // status category: 'n' new, 'i' in progress, 'd' done
  uint8_t prio;  // 1 highest .. 5 lowest
};
const int MAX_JIRA = 7;
JiraIssue jira[MAX_JIRA];
int nJira = 0, jiraTotal = 0;
bool jiraValid = false;  // false: the feeder doesn't follow Jira
int openJira = -1;       // the issue shown full screen, -1 = the list

// Calendar: upcoming events, times in local seconds since 1970 (like the clock).
struct CalEvent {
  char title[61];
  char loc[41];
  uint32_t start, end;
  bool allDay;
  bool todo;  // a reminder (task with a due time) rather than a meeting
};
const int MAX_CAL = 7;
CalEvent cal[MAX_CAL];
int nCal = 0;
bool calValid = false;  // false: the feeder doesn't follow a calendar
int openCal = -1;       // the event shown full screen, -1 = the list

// Claude Code permission requests, answered with ALLOW / DENY on the display.
struct Approval {
  char id[20];
  char app[25];
  char tool[24];
  char detail[160];
  uint16_t ttl;  // seconds until Claude Code stops waiting and asks in the terminal
};
const int MAX_APPROVALS = 4;
Approval approvals[MAX_APPROVALS];
int nApprovals = 0;              // approvals[0] is on screen while nApprovals > 0
unsigned long approvalShownAt = 0;
int approvalBarDrawn = 0;

// Settings (Settings page, or the feeder's .env), kept in flash.
uint8_t brightness = 100;  // backlight percent, 10-100
uint8_t appsShown = 0x0F;  // bit per entry of ALL_APPS: which icons the home screen shows

const int MAX_SESSIONS = 6;
Session sessions[MAX_SESSIONS];
int nSessions = 0;
unsigned long sessionsAt = 0;  // millis() when received
bool sessionsValid = false;

uint64_t hours[24] = {0};
int nowHour = -1;
bool hoursValid = false;

const int HIST = 44;
uint32_t hist[HIST] = {0};
uint64_t lastTotal = 0;

char lineBuf[2048];
size_t lineLen = 0;
unsigned long lastRx = 0;
bool demo = false;
unsigned long lastDemo = 0, lastStatus = 0;
unsigned long nextDemoNotice = 0;  // demo: when the next sample notification appears

// The home screen opens apps; page is the page within the open app.
enum { APP_HOME, APP_DEV, APP_PRINTER, APP_JIRA, APP_CAL, APP_SETTINGS };
int app = APP_HOME;
int page = 0;  // Development: 0 = overview, then Claude, recent, pipelines, models, days, ...

struct Notice {
  char title[40];
  char body[160];
  char src[8];
  char app[25];  // header text; empty = named after src
  uint8_t prio;  // PRIO_*
  uint16_t ttl;  // seconds until it closes itself, 0 = until tapped
};
enum { PRIO_LOW, PRIO_NORMAL, PRIO_HIGH };
const uint16_t NOTICE_TTL = 15;
const int MAX_NOTICES = 4;
Notice notices[MAX_NOTICES];
int nNotices = 0;             // notices[0] is on screen while nNotices > 0
unsigned long blinkUntil = 0;  // backlight blinks until this time after a new notice
unsigned long noticeShownAt = 0;  // when notices[0] appeared (its timer start)
int noticeBarDrawn = 0;           // filled pixels of the progress bar already on screen

// ---------- helpers ----------
// Clock: local time from the feeder, counted on with millis()
bool clockValid = false;
uint32_t clockBase = 0;
unsigned long clockAt = 0;
uint32_t nowEpoch() { return clockBase + (millis() - clockAt) / 1000; }

const char* PREFS_NS = "tribebuddy";

// Settings were kept under "deskbuddy" before the rename; carry them over once.
void migratePrefs() {
  Preferences old;
  if (!old.begin("deskbuddy", true)) return;  // read-only open fails when it never existed
  bool any = old.isKey("flip") || old.isKey("hicontrast") || old.isKey("gamma");
  if (any) {
    Preferences cur;
    cur.begin(PREFS_NS, false);
    if (old.isKey("flip"))       cur.putBool("flip", old.getBool("flip"));
    if (old.isKey("hicontrast")) cur.putBool("hicontrast", old.getBool("hicontrast"));
    if (old.isKey("gamma"))      cur.putUChar("gamma", old.getUChar("gamma"));
    cur.end();
  }
  old.end();
  if (any && old.begin("deskbuddy", false)) { old.clear(); old.end(); }
}
void setContrast(bool high) {
  C_BG = high ? 0x0000 : 0x00E1;
  C_TEXT = high ? 0xFFFF : 0xFFBD;
  C_DIM = high ? C_DIM_HC : C_DIM_STD;
}

// Totals leave out cache reads: every request re-reads the whole context from the cache, so
// they dwarf the real work. They're still shown as their own field.
uint64_t totalTokens() { return u.in + u.out + u.cacheW; }
uint64_t modelTotal(const ModelUsage& m) { return m.in + m.out + m.cacheW; }

// overview, Claude, recent, pipelines, models..., days, hours, projects
const int CLAUDE_PAGE = 1, RECENT_PAGE = 2, PIPELINES_PAGE = 3, FIRST_MODEL_PAGE = 4;
int recentPage() { return RECENT_PAGE; }
int daysPage() { return FIRST_MODEL_PAGE + nModels; }
int hoursPage() { return FIRST_MODEL_PAGE + 1 + nModels; }
int projectsPage() { return FIRST_MODEL_PAGE + 2 + nModels; }
// Pages in the open app: Development as above, the printer status + camera, Jira one.
int pageCount() {
  switch (app) {
    case APP_DEV:     return FIRST_MODEL_PAGE + 3 + nModels;
    case APP_PRINTER: return 2;
    default:          return 1;
  }
}

// What the current page shows, so updates for other pages don't redraw it.
enum { SHOWS_OVERVIEW = 1, SHOWS_MODELS = 2, SHOWS_DAYS = 4, SHOWS_HOURS = 8, SHOWS_RECENT = 16,
       SHOWS_CLAUDE = 32, SHOWS_PROJECTS = 64, SHOWS_PIPELINES = 128, SHOWS_HOME = 256,
       SHOWS_PRINTER = 512, SHOWS_CAMERA = 1024, SHOWS_JIRA = 2048, SHOWS_CAL = 4096,
       SHOWS_SETTINGS = 8192 };

static const char* DOW[7] ={"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};  // 1-1-1970: Thu

// A notification or permission card covers the page: page updates wait until it's gone.
bool covered() { return nNotices > 0 || nApprovals > 0; }

void fmtTokens(uint64_t n, char* buf, size_t len) {
  double v = (double)n;
  if (v >= 1e9)      snprintf(buf, len, "%.2fB", v / 1e9);
  else if (v >= 1e6) snprintf(buf, len, "%.2fM", v / 1e6);
  else if (v >= 1e4) snprintf(buf, len, "%.1fK", v / 1e3);
  else               snprintf(buf, len, "%u", (unsigned)n);
}

// Text is drawn with a background color and only the unused rest of the box is cleared,
// so values update in place without flashing.
void printAt(int x, int y, int w, int h, uint8_t size, uint16_t color, const char* s, uint16_t bg = C_BG) {
  tft.setTextSize(size);
  tft.setTextColor(color, bg);
  tft.setCursor(x, y);
  tft.print(s);
  int end = tft.getCursorX();
  if (end < x + w) tft.fillRect(end, y, x + w - end, h, bg);
}

void label(int x, int y, const char* s) {
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(x, y);
  tft.print(s);
}

void drawSplash() {
  tft.fillScreen(C_BG);
  tft.drawRGBBitmap((240 - TRIBELOGOLARGE_W) / 2, 100, tribeLogoLarge, TRIBELOGOLARGE_W, TRIBELOGOLARGE_H);
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT);
  tft.setCursor((240 - 17 * 12) / 2, 186);
  tft.print("Digital by nature");
  tft.setTextSize(1);
  tft.setTextColor(C_ACCENT);
  tft.setCursor((240 - 14 * 6) / 2, 216);
  tft.print("tribeagency.nl");
}

// ---------- screen (portrait 240x320) ----------
// Header: logo | page dots (100-164) | unread badge (168-186) | clock (190-220) | status dot
void drawHeader() {
  tft.fillRect(0, 0, 240, 28, C_PANEL);
  tft.drawRGBBitmap(6, 2, tribeLogoSmall, TRIBELOGOSMALL_W, TRIBELOGOSMALL_H);
  int n = pageCount();
  int step = n > 1 ? min(8, 64 / (n - 1)) : 0;  // squeeze when there are many model pages
  int x0 = 132 - (n - 1) * step / 2;
  for (int i = 0; i < n && n > 1; i++) {  // no dots for a single page (home, Jira)
    if (i == page) tft.fillCircle(x0 + i * step, 14, 2, C_ACCENT);
    else           tft.drawCircle(x0 + i * step, 14, 2, C_DIM);
  }
  drawUnreadBadge();
}

void drawStatus() {
  uint16_t c = C_DIM;
  const char* txt = "no data";
  unsigned long age = (millis() - lastRx) / 1000;
  if (u.valid) {
    if (age < 30)       { c = C_GREEN; txt = "live"; }
    else if (age < 120) { c = C_AMBER; txt = "stale"; }
    else                { c = C_RED;   txt = "offline"; }
  }
  if (demo) { c = C_ACCENT; txt = "demo"; }
  // The time when known (the dot's colour tells live/stale/offline), else the state in words
  char b[8];
  if (clockValid && !demo) {
    uint32_t t = nowEpoch();
    snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
  } else {
    snprintf(b, sizeof(b), "%5s", strcmp(txt, "offline") == 0 ? "off" : strcmp(txt, "no data") == 0 ? "--" : txt);
  }
  tft.fillCircle(231, 14, 4, c);
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setCursor(190, 10);
  tft.print(b);

  int shows = covered() ? 0 : pageShows();
  if (shows == SHOWS_HOME) drawHomeValues();           // clock, countdowns, badges
  if (shows == SHOWS_CAL) drawCalTicks();              // "in 12m" / "now"
  if (shows == SHOWS_CLAUDE) drawClaudeTicks();        // session timers, forecast
  if (shows == SHOWS_PIPELINES) drawPipelinesTicks();
  if (shows == SHOWS_PRINTER) drawPrinterTicks();      // time left, pending speed change
  if (shows == SHOWS_CAMERA) drawCameraTicks();        // image age, asks for a new one
  if (shows == SHOWS_RECENT) drawRecentValues();       // tick the ages
  if (shows == SHOWS_OVERVIEW) {
    char ago[24] = "";
    if (u.valid && !demo) snprintf(ago, sizeof(ago), "updated %lus ago", age);
    printAt(10, 104, 220, 8, 1, C_DIM, ago);
    if (lim.valid && !demo && millis() - lim.rxAt > LIMITS_DROP_MS) {
      lim.valid = false;  // long gone: back to the --limit bar
      drawOverviewValues();
    }
    if (lim.valid) drawPlanLimits(259);  // tick the countdowns
  }
}

// Horizontal bar: filled part in c, rest cleared, outline in C_DIM.
void drawHBar(int x, int y, int w, float frac, uint16_t c) {
  int fw = (int)((w - 2) * frac);
  tft.drawRect(x, y, w, 8, C_DIM);
  tft.fillRect(x + 1, y + 1, fw, 6, c);
  tft.fillRect(x + 1 + fw, y + 1, w - 2 - fw, 6, C_BG);
}

uint16_t levelColor(float frac) { return frac < 0.7 ? C_GREEN : frac < 0.9 ? C_AMBER : C_RED; }

void drawLimitBar(int y, uint64_t used) {
  if (u.limit == 0) { tft.fillRect(0, y, 240, 10, C_BG); return; }
  char b[8];
  float pct = min(1.0, (double)used / (double)u.limit);
  drawHBar(10, y + 1, 180, pct, levelColor(pct));
  snprintf(b, sizeof(b), "%3d%%", (int)(pct * 100));
  printAt(200, y + 1, 30, 8, 1, C_TEXT, b);
}

// "42m", "3h12m", "2d04h"
void fmtCountdown(uint32_t s, char* buf, size_t len) {
  if (s < 3600)       snprintf(buf, len, "%um", (unsigned)((s + 59) / 60));
  else if (s < 86400) snprintf(buf, len, "%uh%02um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
  else                snprintf(buf, len, "%ud%02uh", (unsigned)(s / 86400), (unsigned)(s % 86400 / 3600));
}

bool limitsStale() { return !demo && millis() - lim.rxAt > LIMITS_STALE_MS; }

// Two rows under the model: "5H [bar] 42%  3h12m" and "WK [bar] 61%  2d04h".
// Redrawn every second on the overview so the countdowns tick.
void drawPlanLimits(int y) {
  static const char* names[2] = {"5H", "WK"};
  unsigned long el = (millis() - lim.rxAt) / 1000;
  bool stale = limitsStale();  // feeder stopped sending them: show, but don't vouch for them
  for (int i = 0; i < 2; i++) {
    int ry = y + i * 11;
    char b[12];
    label(10, ry, names[i]);
    if (lim.pct[i] < 0) { printAt(30, ry, 200, 8, 1, C_DIM, "-"); continue; }
    float frac = lim.pct[i] / 100.0f;
    drawHBar(30, ry, 108, frac, stale ? C_DIM : levelColor(frac));
    snprintf(b, sizeof(b), "%3d%%", lim.pct[i]);
    printAt(144, ry, 30, 8, 1, stale ? C_DIM : C_TEXT, b);
    if (lim.resetIn[i] == 0)       strcpy(b, "");
    else if (el >= lim.resetIn[i]) strcpy(b, "reset");
    else                           fmtCountdown(lim.resetIn[i] - el, b, sizeof(b));
    char r[12];
    snprintf(r, sizeof(r), "%6s", b);  // right-aligned against the edge
    printAt(194, ry, 36, 8, 1, C_DIM, r);
  }
}

// Vertical bar chart: n values from x with bottom at yBottom, max height h.
// Each bar repaints only its own column (background above, color below).
void drawBars(const uint64_t* v, int n, int x, int yBottom, int h, int step, int barW, int highlight) {
  uint64_t mx = 1;
  for (int i = 0; i < n; i++) mx = max(mx, v[i]);
  for (int i = 0; i < n; i++) {
    int bh = (int)(v[i] * h / mx);
    if (v[i] > 0 && bh == 0) bh = 1;
    int bx = x + i * step;
    tft.fillRect(bx, yBottom - h, barW, h - bh, C_BG);
    tft.fillRect(bx, yBottom - bh, barW, bh, i == highlight ? C_ACCENT : C_DIM);
  }
}

// --- overview ---
void drawOverviewStatic() {
  label(10, 38, "TOTAL EXCL. CACHE READS");
  label(10, 126, "INPUT");
  label(125, 126, "OUTPUT");
  label(10, 160, "CACHE READ");
  label(125, 160, "CACHE WRITE");
  label(10, 200, "COST");
  label(10, 234, "MODEL");
  tft.drawFastHLine(10, 118, 220, C_PANEL);
  tft.drawFastHLine(10, 192, 220, C_PANEL);
  tft.drawFastHLine(10, 256, 220, C_PANEL);
  tft.drawFastHLine(10, 315, 219, C_PANEL);
}

void drawOverviewValues() {
  char b[24];
  fmtTokens(totalTokens(), b, sizeof(b));
  printAt(10, 50, 220, 32, 4, C_ACCENT, b);
  printAt(10, 86, 220, 16, 2, C_TEXT, u.label);

  fmtTokens(u.in, b, sizeof(b));     printAt(10, 136, 110, 16, 2, C_TEXT, b);
  fmtTokens(u.out, b, sizeof(b));    printAt(125, 136, 110, 16, 2, C_TEXT, b);
  fmtTokens(u.cacheR, b, sizeof(b)); printAt(10, 170, 110, 16, 2, C_TEXT, b);
  fmtTokens(u.cacheW, b, sizeof(b)); printAt(125, 170, 110, 16, 2, C_TEXT, b);

  snprintf(b, sizeof(b), "$%.2f", u.cost);
  printAt(10, 210, 220, 16, 2, C_TEXT, b);
  char m[37];
  strlcpy(m, u.model, sizeof(m));  // 36 chars fit in 220px at size 1
  printAt(10, 244, 220, 8, 1, C_TEXT, m);

  // Plan limits when the feeder reports them, else the optional daily token bar.
  static bool drewLimits = false;
  if (lim.valid != drewLimits) { tft.fillRect(0, 258, 240, 22, C_BG); drewLimits = lim.valid; }
  if (lim.valid) drawPlanLimits(259);
  else drawLimitBar(264, totalTokens());

  // Sparkline of tokens per update
  uint64_t v[HIST];
  for (int i = 0; i < HIST; i++) v[i] = hist[i];
  drawBars(v, HIST, 10, 314, 34, 5, 4, HIST - 1);
}

// --- per model ---
void drawModelStatic() {
  label(10, 146, "INPUT");
  label(125, 146, "OUTPUT");
  label(10, 180, "CACHE READ");
  label(125, 180, "CACHE WRITE");
  tft.drawFastHLine(10, 214, 220, C_PANEL);
  label(10, 224, "COST");
}

void drawModelValues(int idx) {
  const ModelUsage& m = models[idx];
  char b[40];
  snprintf(b, sizeof(b), "MODEL %d/%d", idx + 1, nModels);
  printAt(10, 38, 220, 8, 1, C_DIM, b);
  strlcpy(b, m.name, 19);  // 18 chars fit in 220px at size 2
  printAt(10, 50, 220, 16, 2, C_TEXT, b);

  uint64_t t = modelTotal(m);
  fmtTokens(t, b, sizeof(b));
  printAt(10, 74, 220, 32, 4, C_ACCENT, b);

  uint64_t all = 0;
  for (int i = 0; i < nModels; i++) all += modelTotal(models[i]);
  float share = all ? (float)t / (float)all : 0;
  snprintf(b, sizeof(b), "%d%% OF ALL MODELS", (int)(share * 100 + 0.5f));
  printAt(10, 112, 220, 8, 1, C_DIM, b);
  drawHBar(10, 124, 220, share, C_ACCENT);

  fmtTokens(m.in, b, sizeof(b));     printAt(10, 156, 110, 16, 2, C_TEXT, b);
  fmtTokens(m.out, b, sizeof(b));    printAt(125, 156, 110, 16, 2, C_TEXT, b);
  fmtTokens(m.cacheR, b, sizeof(b)); printAt(10, 190, 110, 16, 2, C_TEXT, b);
  fmtTokens(m.cacheW, b, sizeof(b)); printAt(125, 190, 110, 16, 2, C_TEXT, b);

  snprintf(b, sizeof(b), "$%.2f", m.cost);
  printAt(10, 234, 220, 16, 2, C_TEXT, b);
}

void drawNoData(const char* title) {
  label(10, 38, title);
  printAt(10, 150, 220, 16, 2, C_DIM, "No data yet");
  label(10, 174, "run feeder.py or type demo");
}

// --- per day ---
const int DAY_STEP = 220 / MAX_DAYS, DAY_BAR = DAY_STEP - 3;

void drawDaysStatic() {
  if (nDays == 0) { drawNoData("LAST 14 DAYS"); return; }
  tft.drawFastHLine(10, 287, DAY_STEP * MAX_DAYS - 3, C_PANEL);
}

void drawDaysValues() {
  if (nDays == 0) return;
  char b[40];
  snprintf(b, sizeof(b), "LAST %d DAYS", nDays);
  printAt(10, 38, 220, 8, 1, C_DIM, b);

  uint64_t tot = 0, v[MAX_DAYS];
  float cost = 0;
  for (int i = 0; i < nDays; i++) { tot += days[i].tok; cost += days[i].cost; v[i] = days[i].tok; }
  fmtTokens(tot, b, sizeof(b));
  printAt(10, 50, 220, 24, 3, C_ACCENT, b);
  snprintf(b, sizeof(b), "$%.2f", cost);
  printAt(10, 78, 220, 16, 2, C_TEXT, b);

  drawBars(v, nDays, 10, 286, 170, DAY_STEP, DAY_BAR, nDays - 1);  // today (last) highlighted
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  for (int i = 0; i < nDays; i++) {
    if (nDays > 7 && (nDays - 1 - i) % 2) continue;  // every other label when crowded
    tft.setCursor(10 + i * DAY_STEP + (DAY_BAR - 12) / 2, 292);
    tft.print(days[i].d + 3);  // day of month
  }

  const DayUsage& t = days[nDays - 1];
  char tk[16];
  fmtTokens(t.tok, tk, sizeof(tk));
  snprintf(b, sizeof(b), "TODAY %s  $%.2f", tk, t.cost);
  printAt(10, 308, 220, 8, 1, C_DIM, b);
}

// --- per hour ---
void drawHoursStatic() {
  if (!hoursValid) { drawNoData("TODAY BY HOUR"); return; }
  label(10, 38, "TODAY BY HOUR");
  tft.drawFastHLine(12, 287, 23 * 9 + 7, C_PANEL);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  for (int h = 0; h < 24; h += 6) {
    tft.setCursor(12 + h * 9, 292);
    tft.print(h);
  }
  tft.setCursor(12 + 23 * 9 - 3, 292);
  tft.print("23");
}

void drawHoursValues() {
  if (!hoursValid) return;
  char b[40];
  uint64_t tot = 0;
  int peak = 0;
  for (int i = 0; i < 24; i++) { tot += hours[i]; if (hours[i] > hours[peak]) peak = i; }
  fmtTokens(tot, b, sizeof(b));
  printAt(10, 50, 220, 24, 3, C_ACCENT, b);
  char pk[16];
  fmtTokens(hours[peak], pk, sizeof(pk));
  snprintf(b, sizeof(b), "PEAK %02d:00  %s", peak, pk);
  printAt(10, 82, 220, 8, 1, C_DIM, b);

  drawBars(hours, 24, 12, 286, 170, 9, 7, nowHour);
}

int drawWrapped(int x, int y, int maxW, int maxLines, int lineH, const char* s,
                const GFXfont* font = &FreeSans9pt7b);  // defined with the notification card

// --- recent notifications ---
// The last few notifications, newest first, kept after they're dismissed (RAM only: a restart
// empties the list).
struct Recent {
  char from[25];  // header text, see noticeFrom
  char title[40];
  char body[160];
  uint8_t prio;
  bool unread;       // not seen on the recent page yet
  unsigned long at;  // millis() when it came in
};
const int MAX_RECENT = 6;  // what fits on the page, leaving a strip below to turn the page
Recent recent[MAX_RECENT];
int nRecent = 0;
int openRecent = -1;  // the one shown full screen after a tap on its row, -1 = the list
const int RC_Y = 52, RC_STEP = 38;  // first row, row height

uint16_t prioColor(uint8_t prio) { return prio == PRIO_HIGH ? C_RED : prio == PRIO_LOW ? C_DIM : C_ACCENT; }

// Row under screen y on the list, or -1 (the heading and empty space turn the page instead).
int recentRowAt(int y) {
  if (y < RC_Y - 4) return -1;
  int i = (y - (RC_Y - 4)) / RC_STEP;
  return i < nRecent ? i : -1;
}

void remember(const Notice& n) {
  memmove(&recent[1], &recent[0], (MAX_RECENT - 1) * sizeof(Recent));
  Recent& r = recent[0];
  noticeFrom(n.app, n.src, r.from, sizeof(r.from));
  strlcpy(r.title, n.title, sizeof(r.title));
  strlcpy(r.body, n.body, sizeof(r.body));
  r.prio = n.prio;
  r.unread = true;
  r.at = millis();
  if (nRecent < MAX_RECENT) nRecent++;
  if (openRecent >= 0 && ++openRecent >= MAX_RECENT) openRecent = -1;  // follow the open one down
}

int unreadCount() {
  int n = 0;
  for (int i = 0; i < nRecent; i++) n += recent[i].unread;
  return n;
}

// Red badge with the number of unread notifications, hidden on the recent page itself.
void drawUnreadBadge() {
  int n = pageShows() == SHOWS_RECENT ? 0 : unreadCount();
  tft.fillRect(166, 5, 22, 18, C_PANEL);
  if (n == 0) return;
  tft.fillRoundRect(168, 7, 18, 14, 7, C_RED);
  char b[4];
  snprintf(b, sizeof(b), "%d", n);
  tft.setTextSize(1);
  tft.setTextColor(0xFFFF, C_RED);
  tft.setCursor(177 - strlen(b) * 3, 11);
  tft.print(b);
}

// Full screen: header, title (bold), the whole message.
void drawRecentDetail() {
  Recent& r = recent[openRecent];
  r.unread = false;
  tft.setTextSize(1);
  tft.setTextColor(prioColor(r.prio), C_BG);
  tft.setCursor(10, 38);
  tft.print(r.from);
  int y = 54;
  tft.setTextColor(C_TEXT);
  if (r.title[0]) y += 18 * drawWrapped(10, y, 220, 2, 18, r.title, &FreeSansBold9pt7b) + 6;
  drawWrapped(10, y, 220, (298 - y) / 18, 18, r.body[0] ? r.body : "(no message)");
  label(10, 308, "tap to go back");
}

// "now", "12m ago", then with the clock "14:32" (today) or "Tue 14:32"; without it "3h ago", "2d ago"
void fmtAgo(unsigned long ms, char* buf, size_t len) {
  unsigned long s = ms / 1000;
  if (s < 60)         snprintf(buf, len, "now");
  else if (s < 3600)  snprintf(buf, len, "%lum ago", s / 60);
  else if (clockValid) {
    uint32_t now = nowEpoch(), t = now - s;
    if (t / 86400 == now / 86400) snprintf(buf, len, "%02u:%02u", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
    else snprintf(buf, len, "%s %02u:%02u", DOW[t / 86400 % 7], (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
  }
  else if (s < 86400) snprintf(buf, len, "%luh ago", s / 3600);
  else                snprintf(buf, len, "%lud ago", s / 86400);
}

void drawRecentStatic() {
  if (openRecent >= 0) { drawRecentDetail(); return; }
  if (nRecent == 0) {
    label(10, 38, "RECENT NOTIFICATIONS");
    printAt(10, 150, 220, 16, 2, C_DIM, "Nothing yet");
    label(10, 174, "notifications show up here");
    return;
  }
  label(10, 38, "RECENT NOTIFICATIONS");
  if (touchCal.ok || touchType == TOUCH_FT6206) label(230 - 11 * 6, 38, "tap to read");
  for (int i = 0; i < nRecent; i++) {
    const Recent& r = recent[i];
    int y = RC_Y + i * RC_STEP;
    uint16_t c = prioColor(r.prio);
    tft.fillRect(10, y, 3, 28, c);  // priority stripe
    tft.setTextSize(1);
    if (r.unread) tft.fillCircle(20, y + 3, 2, C_TEXT);  // new since the last visit
    tft.setTextColor(c, C_BG);
    tft.setCursor(26, y);
    tft.print(r.from);
    tft.setTextColor(C_TEXT);
    drawWrapped(18, y + 10, 212, 1, 18, r.title[0] ? r.title : r.body);  // one line, "..." when longer
    if (i < nRecent - 1) tft.drawFastHLine(10, y + RC_STEP - 5, 220, C_PANEL);
  }
  if (touchCal.ok || touchType == TOUCH_FT6206) label(10, 308, "tap here for the next page >");
}

// Only the ages change on their own; redrawn every second from drawStatus.
void drawRecentValues() {
  char b[12], r[12];
  if (openRecent >= 0) {
    fmtAgo(millis() - recent[openRecent].at, b, sizeof(b));
    snprintf(r, sizeof(r), "%9s", b);
    printAt(230 - 9 * 6, 38, 9 * 6, 8, 1, C_DIM, r);
    return;
  }
  for (int i = 0; i < nRecent; i++) {
    fmtAgo(millis() - recent[i].at, b, sizeof(b));
    snprintf(r, sizeof(r), "%9s", b);  // right-aligned against the edge
    printAt(230 - 9 * 6, RC_Y + i * RC_STEP, 9 * 6, 8, 1, C_DIM, r);
  }
}

// --- Claude: sessions and the limit forecast ---
const int CS_Y = 52, CS_STEP = 30, CS_ROWS = 5;

uint16_t sessionColor(char st) { return st == 'a' ? C_AMBER : st == 'w' ? C_GREEN : C_DIM; }

void drawClaudeStatic() {
  label(10, 38, "CLAUDE CODE SESSIONS");
  tft.drawFastHLine(10, 212, 220, C_PANEL);
  label(10, 222, "PLAN LIMITS AT THIS PACE");
}

// Rows: dot + project + "waiting 3m" / what it's doing. Redrawn when new session data comes in.
void drawClaudeValues() {
  tft.fillRect(0, CS_Y - 2, 240, CS_ROWS * CS_STEP, C_BG);
  if (!sessionsValid || nSessions == 0) {
    printAt(10, CS_Y + 6, 220, 8, 1, C_DIM, sessionsValid ? "No sessions running" : "No session data yet");
    label(10, CS_Y + 20, "run feeder.py install-hooks, then");
    label(10, CS_Y + 32, "restart Claude Code");
  }
  for (int i = 0; i < min(nSessions, CS_ROWS); i++) {
    const Session& ss = sessions[i];
    int y = CS_Y + i * CS_STEP;
    tft.fillCircle(14, y + 3, 3, sessionColor(ss.state));
    char b[40];
    strlcpy(b, ss.name, 25);
    printAt(22, y, 140, 8, 1, C_TEXT, b);
    strlcpy(b, ss.msg, 35);  // 35 chars fit next to the dot
    printAt(22, y + 12, 208, 8, 1, C_DIM, b);
  }
  drawClaudeTicks();
}

// What counts on by itself: time in each state, the forecast countdown. Every second.
void drawClaudeTicks() {
  char b[40], t[16];
  unsigned long el = (millis() - sessionsAt) / 1000;
  for (int i = 0; i < min(nSessions, CS_ROWS); i++) {
    const Session& ss = sessions[i];
    uint32_t sec = ss.forS + el;
    if (sec < 60) snprintf(t, sizeof(t), "%us", (unsigned)sec);
    else fmtCountdown(sec, t, sizeof(t));
    snprintf(b, sizeof(b), "%s %s", ss.state == 'a' ? "waiting" : ss.state == 'w' ? "working" : "idle", t);
    char r[16];
    snprintf(r, sizeof(r), "%13s", b);
    printAt(230 - 13 * 6, CS_Y + i * CS_STEP, 13 * 6, 8, 1, sessionColor(ss.state), r);
  }

  static const char* names[2] = {"5H", "WK"};
  unsigned long lel = (millis() - lim.rxAt) / 1000;
  for (int i = 0; i < 2; i++) {
    int y = 238 + i * 16;
    uint16_t c = C_TEXT;
    if (!lim.valid || lim.pct[i] < 0) {
      snprintf(b, sizeof(b), "%s   -", names[i]);
      c = C_DIM;
    } else if (lim.full[i] > lel) {
      fmtCountdown(lim.full[i] - lel, t, sizeof(t));
      snprintf(b, sizeof(b), "%s %3d%%  full in %s!", names[i], lim.pct[i], t);
      c = C_RED;
    } else if (lim.full[i]) {
      snprintf(b, sizeof(b), "%s %3d%%  full about now", names[i], lim.pct[i]);
      c = C_RED;
    } else if (lim.proj[i] >= 0) {
      snprintf(b, sizeof(b), "%s %3d%%  -> %d%% at reset", names[i], lim.pct[i], lim.proj[i]);
      c = levelColor(lim.proj[i] / 100.0f);
    } else {
      snprintf(b, sizeof(b), "%s %3d%%  too early to tell", names[i], lim.pct[i]);
      c = C_DIM;
    }
    printAt(10, y, 220, 8, 1, limitsStale() ? C_DIM : c, b);
  }
  if (lim.valid) label(10, 276, "pace = average since the window started");
}

// --- Bitbucket pipelines ---
const int PL_Y = 52, PL_STEP = 30;

uint16_t pipelineColor(char st) {
  switch (st) {
    case 'r': return C_ACCENT;
    case 'p': return C_AMBER;
    case 's': return C_GREEN;
    case 'f': return C_RED;
    default:  return C_DIM;
  }
}

void drawPipelinesStatic() {
  label(10, 38, "PIPELINES");
  if (!pipelinesValid) {
    printAt(10, 150, 220, 16, 2, C_DIM, "Not set up");
    label(10, 174, "start the feeder with");
    label(10, 186, "--bitbucket-workspace (README)");
  } else if (nPipelines == 0) {
    printAt(10, 150, 220, 16, 2, C_DIM, "No runs yet");
    label(10, 174, "on the branches being followed");
  }
}

// Rows: dot + repo + status/time, and "main  #812  Jaap" below. Redrawn on new data.
void drawPipelinesValues() {
  if (!pipelinesValid || nPipelines == 0) return;
  for (int i = 0; i < nPipelines; i++) {
    const Pipeline& pl = pipelines[i];
    int y = PL_Y + i * PL_STEP;
    char b[48];
    strlcpy(b, pl.repo, 19);  // 18 chars leave room for the status (the feeder shortens to 18)
    printAt(22, y, 110, 8, 1, C_TEXT, b);
    snprintf(b, sizeof(b), "%s  #%u%s%s", pl.branch, (unsigned)pl.num, pl.by[0] ? "  " : "", pl.by);
    b[35] = 0;  // 35 chars fit next to the dot
    printAt(22, y + 12, 208, 8, 1, C_DIM, b);
  }
  drawPipelinesTicks();
}

// Every second: times, and the dot of a running pipeline blinks.
void drawPipelinesTicks() {
  if (!pipelinesValid) return;
  unsigned long el = (millis() - pipelinesAt) / 1000;
  bool on = (millis() / 1000) % 2;
  for (int i = 0; i < nPipelines; i++) {
    const Pipeline& pl = pipelines[i];
    int y = PL_Y + i * PL_STEP;
    uint16_t c = pipelineColor(pl.state);
    tft.fillCircle(14, y + 3, 3, pl.state == 'r' && !on ? C_BG : c);
    if (pl.state != 'r') tft.drawCircle(14, y + 3, 3, c);
    char t[16], b[24], r[24];
    uint32_t sec = pl.forS + el;
    if (pl.state == 'r' || pl.state == 'p') {
      if (sec < 60) snprintf(t, sizeof(t), "%us", (unsigned)sec);
      else fmtCountdown(sec, t, sizeof(t));
      snprintf(b, sizeof(b), "%s %s", pl.state == 'r' ? "running" : "paused", t);
    } else {
      fmtAgo(sec * 1000UL, t, sizeof(t));
      snprintf(b, sizeof(b), "%s %s", pl.state == 's' ? "ok" : pl.state == 'f' ? "FAILED" : "stopped", t);
    }
    snprintf(r, sizeof(r), "%16s", b);
    printAt(230 - 16 * 6, y, 16 * 6, 8, 1, c, r);
  }
}

// --- today by project ---
void drawProjectsStatic() {
  if (nProjects == 0) { drawNoData("TODAY BY PROJECT"); return; }
  label(10, 38, "TODAY BY PROJECT");
}

void drawProjectsValues() {
  if (nProjects == 0) return;
  float total = 0, mx = 0.01f;
  for (int i = 0; i < nProjects; i++) { total += projects[i].cost; mx = max(mx, projects[i].cost); }
  char b[40];
  snprintf(b, sizeof(b), "$%.2f", total);
  printAt(230 - 10 * 6, 38, 10 * 6, 8, 1, C_TEXT, b);  // right of the heading
  for (int i = 0; i < nProjects; i++) {
    const ProjectUsage& pr = projects[i];
    int y = 54 + i * 42;
    printAt(10, y, 150, 8, 1, C_TEXT, pr.name);
    snprintf(b, sizeof(b), "%9s", "");
    char c[16];
    snprintf(c, sizeof(c), "$%.2f", pr.cost);
    snprintf(b, sizeof(b), "%9s", c);
    printAt(230 - 9 * 6, y, 9 * 6, 8, 1, C_ACCENT, b);
    drawHBar(10, y + 12, 220, pr.cost / mx, C_ACCENT);
    fmtTokens(pr.tok, c, sizeof(c));
    snprintf(b, sizeof(b), "%s tokens", c);
    printAt(10, y + 24, 220, 8, 1, C_DIM, b);
  }
}

// --- 3D printer: status helpers (also used on the home screen and the camera page) ---
bool printerIs(const char* st) { return strcmp(prn.st, st) == 0; }
bool printerBusy() { return prn.on && (printerIs("RUNNING") || printerIs("PAUSE") || printerIs("PREPARE")); }

const char* printerWord(uint16_t& c) {
  if (!prn.on)                { c = C_DIM;    return "OFFLINE"; }
  if (printerIs("RUNNING"))   { c = C_GREEN;  return "PRINTING"; }
  if (printerIs("PREPARE"))   { c = C_ACCENT; return "PREPARING"; }
  if (printerIs("PAUSE"))     { c = C_AMBER;  return "PAUSED"; }
  if (printerIs("FINISH"))    { c = C_ACCENT; return "FINISHED"; }
  if (printerIs("FAILED"))    { c = C_RED;    return "FAILED"; }
  if (printerIs("SLICING"))   { c = C_ACCENT; return "SLICING"; }
  c = C_DIM;
  return "IDLE";
}

// Seconds left in the print, counted down since the last report.
uint32_t printerLeft() {
  uint32_t el = (millis() - prn.rxAt) / 1000, s = prn.left * 60;
  return printerIs("RUNNING") ? (s > el ? s - el : 0) : s;
}

// One line: "42%  1h23m left", "paused at 42%", "idle", ...
void printerSummary(char* buf, size_t len, uint16_t& c) {
  const char* w = printerWord(c);
  char t[12];
  if (!printerValid)            { c = C_DIM; snprintf(buf, len, "not set up"); }
  else if (!prn.on)             snprintf(buf, len, "offline");
  else if (printerIs("RUNNING")) {
    fmtCountdown(printerLeft(), t, sizeof(t));
    snprintf(buf, len, "%d%%  %s left", max(prn.pct, 0), t);
  }
  else if (printerIs("PAUSE"))  snprintf(buf, len, "paused at %d%%", max(prn.pct, 0));
  else {
    snprintf(buf, len, "%s", w);
    for (char* p = buf; *p; p++) *p = tolower(*p);
  }
}

// --- home screen ---
// Clock widget, a row of app icons (with badges, like a phone), a summary line per app and a
// Settings button. Which apps show is a setting (Settings page, or TRIBEBUDDY_APPS in .env).
const int HOME_ICON = 50, HOME_Y = 128, HOME_STEP = 60, MAX_HOME = 4;
const int HOME_SUM_Y = 212, HOME_SUM_STEP = 18;
const int ALL_APPS[MAX_HOME] = {APP_DEV, APP_PRINTER, APP_JIRA, APP_CAL};
const char* const APP_NAMES[MAX_HOME] = {"Dev", "Printer", "Jira", "Calendar"};
const char* const APP_KEYS[MAX_HOME] = {"dev", "printer", "jira", "calendar"};  // in the "apps" setting
const int SET_BTN_X = 166, SET_BTN_Y = 294, SET_BTN_W = 64, SET_BTN_H = 20;  // Settings button
int homeApp[MAX_HOME];  // the apps shown, in order
int nHome = 0;
char homeBadge[MAX_HOME][8];  // badge text on screen per icon, so icons redraw only on change
int calDayDrawn = -1;         // the calendar icon shows today's date

void layoutHome() {
  if (!(appsShown & 0x0F)) appsShown = 1;  // never an empty home screen
  nHome = 0;
  for (int i = 0; i < MAX_HOME; i++)
    if (appsShown & (1 << i)) homeApp[nHome++] = ALL_APPS[i];
}

int homeX(int i) { return 10 + i * HOME_STEP; }

void drawAppIcon(int i) {
  int x = homeX(i), y = HOME_Y, s = HOME_ICON;
  tft.fillRect(x, y - 9, HOME_STEP, s + 9, C_BG);  // icon plus the room its badge sticks out
  switch (homeApp[i]) {
    case APP_DEV:  // "</>" on the brand green
      tft.fillRoundRect(x, y, s, s, 12, C_PANEL);
      tft.setTextSize(2);
      tft.setTextColor(C_ACCENT);
      tft.setCursor(x + 7, y + 17);
      tft.print("</>");
      break;
    case APP_PRINTER:  // frame, gantry, nozzle and a part on the bed
      tft.fillRoundRect(x, y, s, s, 12, C_AMBER);
      tft.fillRect(x + 10, y + 9, 30, 3, C_INK);
      tft.fillRect(x + 10, y + 9, 3, 31, C_INK);
      tft.fillRect(x + 37, y + 9, 3, 31, C_INK);
      tft.fillRect(x + 10, y + 37, 30, 3, C_INK);
      tft.fillRect(x + 13, y + 17, 24, 2, C_INK);
      tft.fillTriangle(x + 21, y + 19, x + 29, y + 19, x + 25, y + 25, C_INK);
      tft.fillRect(x + 19, y + 30, 12, 7, 0xFFFF);
      break;
    case APP_JIRA:  // a checklist
      tft.fillRoundRect(x, y, s, s, 12, C_BLUE);
      for (int r = 0; r < 3; r++) {
        int ry = y + 11 + r * 10;
        tft.drawRect(x + 10, ry, 8, 8, 0xFFFF);
        tft.fillRect(x + 22, ry + 3, 18, 2, 0xFFFF);
        if (r < 2) {
          tft.drawLine(x + 11, ry + 4, x + 13, ry + 6, 0xFFFF);
          tft.drawLine(x + 13, ry + 6, x + 17, ry + 1, 0xFFFF);
        }
      }
      break;
    case APP_CAL: {  // a calendar sheet with today's weekday and date
      tft.fillRoundRect(x, y, s, s, 12, 0xFFFF);
      tft.fillRoundRect(x, y, s, 28, 12, C_RED);
      tft.fillRect(x, y + 16, s, 12, 0xFFFF);
      char dow[4] = "", day[4] = "--";
      if (clockValid) {
        uint32_t t = nowEpoch();
        strlcpy(dow, DOW[t / 86400 % 7], sizeof(dow));
        uint32_t z = t / 86400 + 719468, doe = z % 146097;  // day of month, as in fmtDate
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        snprintf(day, sizeof(day), "%u", (unsigned)(doy - (153 * mp + 2) / 5 + 1));
        calDayDrawn = t / 86400;
      }
      for (char* p = dow; *p; p++) *p = toupper(*p);
      tft.setTextSize(1);
      tft.setTextColor(0xFFFF);
      tft.setCursor(x + (s - strlen(dow) * 6) / 2, y + 5);
      tft.print(dow);
      tft.setTextSize(3);
      tft.setTextColor(C_INK);
      tft.setCursor(x + (s - strlen(day) * 18) / 2 + 1, y + 21);
      tft.print(day);
      break;
    }
  }
  const char* b = homeBadge[i];
  if (!b[0]) return;
  uint16_t bg = homeApp[i] == APP_PRINTER && b[0] != '!' ? C_GREEN : C_RED;
  int w = max(18, (int)strlen(b) * 6 + 8), cx = min(x + s - 4, x + HOME_STEP - w / 2);
  tft.fillRoundRect(cx - w / 2, y - 8, w, 18, 9, bg);
  tft.setTextSize(1);
  tft.setTextColor(0xFFFF, bg);
  tft.setCursor(cx - strlen(b) * 3, y - 3);
  tft.print(b);
}

int calNext();  // index of the first event that hasn't ended, or -1 (with the Calendar page)

// Badge per app: unread notifications, print progress (or "!" on a failure), open issues,
// minutes until the next meeting (within the hour).
void homeBadgeText(int i, char* b, size_t len) {
  b[0] = 0;
  switch (homeApp[i]) {
    case APP_DEV:
      if (unreadCount()) snprintf(b, len, "%d", unreadCount());
      break;
    case APP_PRINTER:
      if (!printerValid || !prn.on) break;
      if (printerIs("FAILED") || prn.err) snprintf(b, len, "!");
      else if (printerBusy() && prn.pct >= 0) snprintf(b, len, "%d%%", prn.pct);
      break;
    case APP_JIRA:
      if (jiraValid && jiraTotal > 0) snprintf(b, len, "%d", min(jiraTotal, 999));
      break;
    case APP_CAL: {
      int n = calNext();
      if (n < 0 || cal[n].allDay || !clockValid) break;
      uint32_t now = nowEpoch();
      if (cal[n].start > now && cal[n].start - now < 3600) snprintf(b, len, "%um", (unsigned)((cal[n].start - now + 59) / 60));
      break;
    }
  }
}

// "10:30", "Tmrw 09:00", "Tue 14:00" for an event time
void fmtWhen(uint32_t t, char* buf, size_t len) {
  uint32_t today = nowEpoch() / 86400, day = t / 86400;
  char hm[6];
  snprintf(hm, sizeof(hm), "%02u:%02u", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
  if (day == today)          snprintf(buf, len, "%s", hm);
  else if (day == today + 1) snprintf(buf, len, "Tmrw %s", hm);
  else                       snprintf(buf, len, "%s %s", DOW[day % 7], hm);
}

// One summary line per app on the home screen.
void homeSummary(int app, char* b, size_t len, uint16_t& c) {
  c = C_TEXT;
  switch (app) {
    case APP_DEV:
      if (lim.valid && lim.pct[0] >= 0) {
        snprintf(b, len, "5H %d%%  WK %d%%  $%.2f", lim.pct[0], max(lim.pct[1], 0), u.cost);
        c = limitsStale() ? C_DIM : levelColor(max(lim.pct[0], lim.pct[1]) / 100.0f);
      }
      else if (u.valid) snprintf(b, len, "$%.2f today", u.cost);
      else { snprintf(b, len, "no data yet"); c = C_DIM; }
      break;
    case APP_PRINTER:
      printerSummary(b, len, c);
      break;
    case APP_JIRA:
      if (!jiraValid)      { snprintf(b, len, "not set up"); c = C_DIM; }
      else if (!jiraTotal) { snprintf(b, len, "nothing open"); c = C_GREEN; }
      else if (nJira)      snprintf(b, len, "%d open, first: %s", jiraTotal, jira[0].key);
      else                 snprintf(b, len, "%d open", jiraTotal);
      break;
    case APP_CAL: {
      int n = calNext();
      uint32_t now = nowEpoch();
      if (!calValid)        { snprintf(b, len, "not set up"); c = C_DIM; }
      else if (n < 0)       { snprintf(b, len, "nothing planned"); c = C_DIM; }
      else if (cal[n].allDay) snprintf(b, len, "today: %s", cal[n].title);
      else if (cal[n].start <= now) { snprintf(b, len, "now: %s", cal[n].title); c = C_GREEN; }
      else if (cal[n].start - now < 3600) {
        snprintf(b, len, "in %um: %s", (unsigned)((cal[n].start - now + 59) / 60), cal[n].title);
        c = C_AMBER;
      } else {
        char w[12];
        fmtWhen(cal[n].start, w, sizeof(w));
        snprintf(b, len, "%s %s", w, cal[n].title);
      }
      break;
    }
  }
}

void drawHomeStatic() {
  static const char* const labels[] = {"", "CLAUDE", "PRINTER", "JIRA", "AGENDA"};  // by APP_*
  for (int i = 0; i < nHome; i++) {
    homeBadgeText(i, homeBadge[i], sizeof(homeBadge[i]));
    drawAppIcon(i);
    const char* name = APP_NAMES[0];
    for (int k = 0; k < MAX_HOME; k++) if (ALL_APPS[k] == homeApp[i]) name = APP_NAMES[k];
    tft.setTextSize(1);
    tft.setTextColor(C_TEXT, C_BG);
    tft.setCursor(homeX(i) + (HOME_ICON - (int)strlen(name) * 6) / 2, HOME_Y + HOME_ICON + 6);
    tft.print(name);
    label(10, HOME_SUM_Y + i * HOME_SUM_STEP, labels[homeApp[i]]);
  }
  tft.drawFastHLine(10, HOME_SUM_Y - 10, 220, C_PANEL);
  if (touchType == TOUCH_XPT2046 && !touchCal.ok) label(10, 300, "not calibrated: tap = Dev");
  else label(10, 300, "logo/hold: home");
  tft.drawRoundRect(SET_BTN_X, SET_BTN_Y, SET_BTN_W, SET_BTN_H, 6, C_DIM);
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(SET_BTN_X + (SET_BTN_W - 8 * 6) / 2, SET_BTN_Y + 6);
  tft.print("Settings");
}

// "Fri 2 Oct" (days-to-civil from howardhinnant.github.io/date_algorithms.html)
void fmtDate(uint32_t t, char* buf, size_t len) {
  static const char* mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  uint32_t z = t / 86400 + 719468, doe = z % 146097;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  snprintf(buf, len, "%s %u %s", DOW[t / 86400 % 7], (unsigned)d, mon[m - 1]);
}

void drawHomeTicks() {
  char b[16];
  bool known = clockValid && !demo;
  uint32_t t = nowEpoch();
  if (known) snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
  else strcpy(b, "--:--");
  printAt(45, 46, 150, 40, 5, known ? C_TEXT : C_DIM, b);
  if (known) fmtDate(t, b, sizeof(b));
  else b[0] = 0;
  char c[20];
  snprintf(c, sizeof(c), "%*s", (int)(9 + strlen(b) / 2), b);  // centred in 18 columns
  printAt(12, 96, 216, 16, 2, C_ACCENT, c);
}

void drawHomeValues() {
  for (int i = 0; i < nHome; i++) {
    char b[8];
    homeBadgeText(i, b, sizeof(b));
    bool newDay = homeApp[i] == APP_CAL && clockValid && (int)(nowEpoch() / 86400) != calDayDrawn;
    if (strcmp(b, homeBadge[i]) != 0 || newDay) { strcpy(homeBadge[i], b); drawAppIcon(i); }
  }
  for (int i = 0; i < nHome; i++) {
    char b[27];  // 26 characters fit right of the labels
    uint16_t c;
    homeSummary(homeApp[i], b, sizeof(b), c);
    printAt(70, HOME_SUM_Y + i * HOME_SUM_STEP, 160, 8, 1, c, b);
  }
  drawHomeTicks();
}

// App under a tap on the home screen (APP_SETTINGS for its button), or -1.
int homeAppAt(int x, int y) {
  if (x >= SET_BTN_X - 4 && y >= SET_BTN_Y - 6) return APP_SETTINGS;
  if (y < HOME_Y - 10 || y > HOME_Y + HOME_ICON + 18) return -1;  // icon and its label
  for (int i = 0; i < nHome; i++)
    if (x >= homeX(i) - 5 && x < homeX(i) + HOME_ICON + 5) return homeApp[i];
  return -1;
}

// --- 3D printer: status and speed profile ---
const int SB_X = 10, SB_Y = 194, SB_W = 52, SB_H = 38, SB_STEP = 56;  // speed buttons
int speedDrawn = -1;  // what the buttons show (level, pending, enabled), -1 = redraw

void drawNotSetUp(const char* title, const char* option) {
  label(10, 38, title);
  printAt(10, 150, 220, 16, 2, C_DIM, "Not set up");
  label(10, 174, "start the feeder with");
  label(10, 186, option);
}

void drawPrinterStatic() {
  speedDrawn = -1;
  if (!printerValid) { drawNotSetUp("3D PRINTER", "--printer-host (README)"); return; }
  tft.drawFastHLine(10, 126, 220, C_PANEL);
  label(10, 134, "NOZZLE");
  label(125, 134, "BED");
  tft.drawFastHLine(10, 170, 220, C_PANEL);
  label(10, 180, "SPEED");
  label(10, 304, "tap for the camera");
}

void drawSpeedButtons() {
  static const char* names[4] = {"Silent", "Std", "Sport", "Ludi"};
  static const char* pcts[4] = {"50%", "100%", "124%", "166%"};
  bool enabled = printerBusy() || demo;
  int key = prn.spd * 100 + spdPending * 10 + enabled;
  if (key == speedDrawn) return;
  speedDrawn = key;
  for (int i = 0; i < 4; i++) {
    int x = SB_X + i * SB_STEP, lvl = i + 1;
    bool active = prn.spd == lvl, pending = spdPending == lvl && !active;
    uint16_t bg = active ? C_ACCENT : C_BG;
    uint16_t fg = active ? C_CARD : enabled ? C_TEXT : C_DIM;
    tft.fillRoundRect(x, SB_Y, SB_W, SB_H, 6, bg);
    tft.drawRoundRect(x, SB_Y, SB_W, SB_H, 6, pending ? C_AMBER : active ? C_ACCENT : C_DIM);
    if (pending) tft.drawRoundRect(x + 1, SB_Y + 1, SB_W - 2, SB_H - 2, 5, C_AMBER);
    tft.setTextSize(1);
    tft.setTextColor(fg, bg);
    tft.setCursor(x + (SB_W - strlen(names[i]) * 6) / 2, SB_Y + 9);
    tft.print(names[i]);
    tft.setTextColor(active ? C_CARD : C_DIM, bg);
    tft.setCursor(x + (SB_W - strlen(pcts[i]) * 6) / 2, SB_Y + 22);
    tft.print(pcts[i]);
  }
  printAt(70, 180, 160, 8, 1, C_DIM, enabled ? (spdPending ? "    sending to the printer" : "")
                                             : "       only while printing");
}

void drawPrinterValues() {
  if (!printerValid) return;
  char b[40];
  uint16_t c;
  const char* w = printerWord(c);
  char name[21];
  strlcpy(name, prn.name, sizeof(name));
  for (char* p = name; *p; p++) *p = toupper(*p);
  printAt(10, 38, 130, 8, 1, C_DIM, name);
  snprintf(b, sizeof(b), "%10s", w);
  printAt(230 - 10 * 6, 38, 10 * 6, 8, 1, c, b);

  strlcpy(b, prn.job[0] ? prn.job : "No print job", 19);  // 18 chars fit at size 2
  printAt(10, 52, 220, 16, 2, prn.job[0] ? C_TEXT : C_DIM, b);
  if (prn.pct >= 0) snprintf(b, sizeof(b), "%d%%", prn.pct);
  else strcpy(b, "--");
  printAt(10, 76, 112, 32, 4, prn.on ? C_ACCENT : C_DIM, b);
  if (prn.layers) snprintf(b, sizeof(b), "layer %d/%d", prn.layer, prn.layers);
  else b[0] = 0;
  printAt(122, 78, 108, 8, 1, C_TEXT, b);
  float frac = prn.pct > 0 ? min(prn.pct, 100) / 100.0f : 0;
  drawHBar(10, 114, 220, frac, prn.on ? c : C_DIM);

  snprintf(b, sizeof(b), "%d/%dC", prn.noz, prn.nozT);
  printAt(10, 146, 110, 16, 2, prn.on ? C_TEXT : C_DIM, b);
  snprintf(b, sizeof(b), "%d/%dC", prn.bed, prn.bedT);
  printAt(125, 146, 105, 16, 2, prn.on ? C_TEXT : C_DIM, b);

  if (prn.err) snprintf(b, sizeof(b), "Error %04X %04X", (unsigned)(prn.err >> 16), (unsigned)(prn.err & 0xFFFF));
  else if (!prn.on) strcpy(b, "printer unreachable, last known state");
  else b[0] = 0;
  printAt(10, 248, 220, 8, 1, prn.err ? C_RED : C_DIM, b);
  drawPrinterTicks();
}

// Every second: time left and when it'll be done; a speed tap the printer didn't confirm expires.
void drawPrinterTicks() {
  if (!printerValid) return;
  if (spdPending && millis() - spdPendingAt > 10000) spdPending = 0;
  drawSpeedButtons();
  char b[40], t[12];
  b[0] = 0;
  if (printerBusy() && prn.left) {
    fmtCountdown(printerLeft(), t, sizeof(t));
    snprintf(b, sizeof(b), "%s left", t);
  }
  printAt(122, 92, 108, 8, 1, C_TEXT, b);
  b[0] = 0;
  if (printerBusy() && prn.left && clockValid) {
    uint32_t done = nowEpoch() + printerLeft();
    snprintf(b, sizeof(b), "done %02u:%02u", (unsigned)(done / 3600 % 24), (unsigned)(done / 60 % 60));
  }
  printAt(122, 102, 108, 8, 1, C_DIM, b);
}

int speedButtonAt(int x, int y) {
  if (y < SB_Y - 6 || y > SB_Y + SB_H + 6) return 0;
  for (int i = 0; i < 4; i++)
    if (x >= SB_X + i * SB_STEP - 2 && x < SB_X + i * SB_STEP + SB_W + 2) return i + 1;
  return 0;
}

// Asks the feeder to set the speed; the button stays amber until the printer reports it.
void setSpeed(int lvl) {
  if (demo) { prn.spd = lvl; drawSpeedButtons(); return; }
  if (!printerBusy() || lvl == prn.spd) return;
  Serial.printf("{\"event\":\"speed\",\"level\":%d}\n", lvl);
  spdPending = lvl;
  spdPendingAt = millis();
  drawPrinterTicks();
}

// --- 3D printer: camera ---
const int CAM_Y = 50, CAM_H = 180;  // image area: 240 wide, the feeder sends at most 180 high

bool jpgBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= CAM_Y + CAM_H) return false;  // stop decoding: below the image area
  tft.drawRGBBitmap(x, y, bitmap, w, min<int>(h, CAM_Y + CAM_H - y));
  return true;
}

void askSnapshot() {
  if (demo || !printerValid) return;
  snapAskedAt = millis();
  Serial.println("{\"event\":\"snapshot\"}");
}

void drawCameraImage() {
  if (!snapReady) {
    tft.fillRect(0, CAM_Y, 240, CAM_H, C_PANEL);
    const char* msg = demo ? "no camera in demo mode" : snapErr[0] ? "no image" : "loading...";
    tft.setTextSize(1);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.setCursor((240 - strlen(msg) * 6) / 2, CAM_Y + CAM_H / 2 - 4);
    tft.print(msg);
    return;
  }
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, snapBuf, snapReady);
  if (h < CAM_H) tft.fillRect(0, CAM_Y + h, 240, CAM_H - h, C_BG);
  TJpgDec.drawJpg(max(0, (240 - w) / 2), CAM_Y, snapBuf, snapReady);
}

void drawCameraStatic() {
  if (!printerValid) { drawNotSetUp("CAMERA", "--printer-host (README)"); return; }
  label(10, 38, "CAMERA");
  drawCameraImage();
  label(10, 304, "new image every 20 s; tap for status");
}

// Under the image: the print status in one line, and why there's no image if so.
void drawCameraInfo() {
  if (!printerValid) return;
  char b[40];
  uint16_t c;
  printerSummary(b, sizeof(b), c);
  printAt(10, CAM_Y + CAM_H + 10, 220, 8, 1, c, b);
  printAt(10, CAM_Y + CAM_H + 24, 220, 8, 1, C_RED, snapErr);
  drawCameraTicks();
}

void drawCameraTicks() {
  if (!printerValid) return;
  if (millis() - snapAskedAt > SNAP_EVERY_MS) askSnapshot();
  char b[16] = "", r[16];
  if (snapReady) fmtAgo(millis() - snapAt, b, sizeof(b));
  else if (snapLen) strcpy(b, "receiving");
  snprintf(r, sizeof(r), "%10s", b);
  printAt(230 - 10 * 6, 38, 10 * 6, 8, 1, C_DIM, r);
}

// --- Jira ---
const int JR_Y = 52, JR_STEP = 38;

uint16_t jiraPrioColor(uint8_t p) { return p <= 2 ? C_RED : p == 3 ? C_AMBER : C_DIM; }
uint16_t jiraCatColor(char cat) { return cat == 'i' ? C_ACCENT : cat == 'd' ? C_GREEN : C_DIM; }

void drawJiraStatic() {
  if (!jiraValid) drawNotSetUp("MY JIRA WORK", "--jira-site (README)");
}

void drawJiraDetail() {
  const JiraIssue& j = jira[openJira];
  tft.setTextSize(1);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setCursor(10, 38);
  tft.print(j.key);
  tft.setTextColor(jiraCatColor(j.cat), C_BG);
  tft.setCursor(230 - strlen(j.st) * 6, 38);
  tft.print(j.st);
  static const char* prios[5] = {"Highest", "High", "Medium", "Low", "Lowest"};
  tft.setTextColor(jiraPrioColor(j.prio), C_BG);
  tft.setCursor(10, 50);
  tft.printf("%s priority", prios[constrain(j.prio, 1, 5) - 1]);
  tft.setTextColor(C_TEXT);
  drawWrapped(10, 66, 220, 12, 18, j.sum[0] ? j.sum : "(no summary)", &FreeSansBold9pt7b);
  label(10, 308, "tap to go back");
}

// The list (rows like the recent page), or one issue full screen. Redrawn on new data.
void drawJiraValues() {
  if (!jiraValid) return;
  tft.fillRect(0, 34, 240, 286, C_BG);
  if (openJira >= 0) { drawJiraDetail(); return; }
  label(10, 38, "MY JIRA WORK");
  char b[16];
  snprintf(b, sizeof(b), "%d open", jiraTotal);
  printAt(230 - strlen(b) * 6, 38, strlen(b) * 6, 8, 1, C_TEXT, b);
  if (nJira == 0) {
    printAt(10, 150, 220, 16, 2, C_DIM, "Nothing open");
    label(10, 174, "no issues match the JQL");
    return;
  }
  for (int i = 0; i < nJira; i++) {
    const JiraIssue& j = jira[i];
    int y = JR_Y + i * JR_STEP;
    tft.fillRect(10, y, 3, 28, jiraPrioColor(j.prio));  // priority stripe
    tft.setTextSize(1);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setCursor(18, y);
    tft.print(j.key);
    char st[21];
    strlcpy(st, j.st, min<int>(sizeof(st), 35 - strlen(j.key)));  // what fits right of the key
    tft.setTextColor(jiraCatColor(j.cat), C_BG);
    tft.setCursor(230 - strlen(st) * 6, y);
    tft.print(st);
    tft.setTextColor(C_TEXT);
    drawWrapped(18, y + 10, 212, 1, 18, j.sum);
    if (i < nJira - 1) tft.drawFastHLine(10, y + JR_STEP - 5, 220, C_PANEL);
  }
}

int jiraRowAt(int y) {
  if (y < JR_Y - 4) return -1;
  int i = (y - (JR_Y - 4)) / JR_STEP;
  return i < nJira ? i : -1;
}

// --- Calendar ---
const int CL_Y = 54, CL_STEP = 37;

// The first meeting or reminder that hasn't ended; an all-day event only when there's none.
int calNext() {
  uint32_t now = nowEpoch();
  int allDay = -1;
  for (int i = 0; i < nCal; i++) {
    if (!(cal[i].end > now || (cal[i].end == cal[i].start && cal[i].start + 60 > now))) continue;
    if (!cal[i].allDay) return i;
    if (allDay < 0) allDay = i;
  }
  return allDay;
}

void drawCalStatic() {
  if (!calValid) drawNotSetUp("AGENDA", "--calendar-ics (README)");
}

void drawCalDetail() {
  const CalEvent& e = cal[openCal];
  char b[48], w1[12], w2[12];
  label(10, 38, e.todo ? "REMINDER" : "MEETING");
  tft.setTextColor(C_TEXT);
  int y = 54 + 18 * drawWrapped(10, 54, 220, 4, 18, e.title, &FreeSansBold9pt7b) + 8;
  if (e.allDay) snprintf(b, sizeof(b), "all day");
  else {
    fmtWhen(e.start, w1, sizeof(w1));
    snprintf(w2, sizeof(w2), "%02u:%02u", (unsigned)(e.end / 3600 % 24), (unsigned)(e.end / 60 % 60));
    if (e.end > e.start) snprintf(b, sizeof(b), "%s - %s", w1, w2);
    else snprintf(b, sizeof(b), "%s", w1);
  }
  printAt(10, y, 220, 8, 1, C_ACCENT, b);
  if (e.loc[0]) {
    tft.setTextColor(C_TEXT);
    drawWrapped(10, y + 14, 220, 4, 18, e.loc);
  }
  label(10, 308, "tap to go back");
}

// Rows: start (and end) time left, title, location; a stripe on what's on now / next.
void drawCalValues() {
  if (!calValid) return;
  tft.fillRect(0, 34, 240, 286, C_BG);
  if (openCal >= 0) { drawCalDetail(); return; }
  label(10, 38, "AGENDA");
  if (clockValid) {
    char d[16];
    fmtDate(nowEpoch(), d, sizeof(d));
    printAt(230 - strlen(d) * 6, 38, strlen(d) * 6, 8, 1, C_TEXT, d);
  }
  if (nCal == 0) {
    printAt(10, 150, 220, 16, 2, C_DIM, "Nothing planned");
    label(10, 174, "for the coming week");
    return;
  }
  uint32_t today = nowEpoch() / 86400;
  for (int i = 0; i < nCal; i++) {
    const CalEvent& e = cal[i];
    int y = CL_Y + i * CL_STEP;
    char t1[8], t2[8];
    uint32_t day = e.start / 86400;
    if (e.allDay) { strcpy(t1, day == today ? "today" : DOW[day % 7]); strcpy(t2, "all day"); }
    else {
      snprintf(t2, sizeof(t2), "%02u:%02u", (unsigned)(e.start / 3600 % 24), (unsigned)(e.start / 60 % 60));
      if (day == today) {
        strcpy(t1, t2);
        if (e.end > e.start) snprintf(t2, sizeof(t2), "%02u:%02u", (unsigned)(e.end / 3600 % 24), (unsigned)(e.end / 60 % 60));
        else t2[0] = 0;
      } else strcpy(t1, day == today + 1 ? "Tmrw" : DOW[day % 7]);
    }
    printAt(10, y, 42, 8, 1, C_ACCENT, t1);
    printAt(10, y + 12, 42, 8, 1, C_DIM, t2);  // "all day" ends at x 52, left of the title
    tft.setTextColor(e.todo ? C_AMBER : C_TEXT);
    drawWrapped(54, y - 3, 176, 1, 18, e.title);
    if (i < nCal - 1) tft.drawFastHLine(10, y + CL_STEP - 6, 220, C_PANEL);
  }
  drawCalTicks();
}

// Every second: "now" / "in 12m" per row, location next to it, the stripe on the current one.
void drawCalTicks() {
  if (!calValid || openCal >= 0) return;
  uint32_t now = nowEpoch();
  int next = calNext();
  for (int i = 0; i < nCal; i++) {
    const CalEvent& e = cal[i];
    int y = CL_Y + i * CL_STEP;
    char st[10] = "";
    uint16_t c = C_DIM;
    if (clockValid && !e.allDay) {
      if (e.start <= now && e.end > now)  { strcpy(st, "now"); c = C_GREEN; }
      else if (e.start > now && e.start - now < 3600) {
        snprintf(st, sizeof(st), "in %um", (unsigned)((e.start - now + 59) / 60));
        c = C_AMBER;
      }
      else if (e.end <= now && e.start < now) { strcpy(st, "done"); }
    }
    tft.fillRect(3, y - 2, 3, 26, i == next && clockValid ? (st[0] == 'n' ? C_GREEN : C_ACCENT) : C_BG);
    char loc[40];
    int room = (176 - (st[0] ? (int)strlen(st) * 6 + 6 : 0)) / 6;  // characters left of the status
    strlcpy(loc, e.loc, min<int>(sizeof(loc), room + 1));
    printAt(54, y + 16, 176, 8, 1, C_DIM, loc);
    if (st[0]) printAt(230 - strlen(st) * 6, y + 16, strlen(st) * 6, 8, 1, c, st);
  }
}

int calRowAt(int y) {
  if (y < CL_Y - 6) return -1;
  int i = (y - (CL_Y - 6)) / CL_STEP;
  return i < nCal ? i : -1;
}

// --- Settings: brightness, apps on the home screen, contrast ---
const int ST_BRIGHT_Y = 66, ST_APPS_Y = 124, ST_APP_STEP = 28, ST_CONTRAST_Y = 262;

void backlight(bool on) {
  uint32_t duty = on ? max(10, brightness * 255 / 100) : 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, duty);
#else
  ledcWrite(0, duty);
#endif
}

void initBacklight() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
#endif
}

bool highContrast() { return C_BG == 0x0000; }

void saveSettings() {
  Preferences prefs;
  prefs.begin(PREFS_NS, false);
  prefs.putUChar("bright", brightness);
  prefs.putUChar("apps", appsShown);
  prefs.putBool("hicontrast", highContrast());
  prefs.end();
}

void drawCheckbox(int x, int y, bool on) {
  tft.drawRoundRect(x, y, 18, 18, 4, on ? C_ACCENT : C_DIM);
  tft.fillRoundRect(x + 2, y + 2, 14, 14, 3, on ? C_ACCENT : C_BG);
}

void drawSettingsStatic() {
  label(10, 38, "SETTINGS");
  label(10, ST_BRIGHT_Y - 12, "BRIGHTNESS");
  for (int i = 0; i < 2; i++) {  // [-] and [+]
    int x = i ? 190 : 10;
    tft.drawRoundRect(x, ST_BRIGHT_Y, 40, 30, 6, C_DIM);
    tft.setTextSize(2);
    tft.setTextColor(C_TEXT, C_BG);
    tft.setCursor(x + 14, ST_BRIGHT_Y + 8);
    tft.print(i ? "+" : "-");
  }
  label(10, ST_APPS_Y - 14, "APPS ON THE HOME SCREEN");
  label(10, ST_CONTRAST_Y - 14, "DISPLAY");
  label(10, 304, "TRIBEBUDDY_* in .env win on reconnect");
}

void drawSettingsValues() {
  char b[8];
  tft.drawRect(58, ST_BRIGHT_Y + 8, 124, 14, C_DIM);
  int fw = 122 * brightness / 100;
  tft.fillRect(59, ST_BRIGHT_Y + 9, fw, 12, C_ACCENT);
  tft.fillRect(59 + fw, ST_BRIGHT_Y + 9, 122 - fw, 12, C_BG);
  snprintf(b, sizeof(b), "%4d%%", brightness);
  printAt(230 - 5 * 6, ST_BRIGHT_Y - 12, 5 * 6, 8, 1, C_TEXT, b);
  for (int i = 0; i < MAX_HOME; i++) {
    int y = ST_APPS_Y + i * ST_APP_STEP;
    drawCheckbox(10, y, appsShown & (1 << i));
    static const char* const full[MAX_HOME] = {"Development", "3D Printer", "Jira", "Calendar"};
    printAt(36, y + 5, 194, 8, 1, C_TEXT, full[i]);
  }
  drawCheckbox(10, ST_CONTRAST_Y, highContrast());
  printAt(36, ST_CONTRAST_Y + 1, 194, 8, 1, C_TEXT, "High contrast");
  printAt(36, ST_CONTRAST_Y + 12, 194, 8, 1, C_DIM, "for TN panels seen at an angle");
}

void onSettingsTap(int x, int y) {
  if (y >= ST_BRIGHT_Y - 4 && y < ST_BRIGHT_Y + 34) {
    if (x < 54)       brightness = max(10, brightness - 10);
    else if (x > 186) brightness = min(100, brightness + 10);
    else              brightness = constrain((x - 58) * 100 / 124 / 10 * 10 + 10, 10, 100);
    backlight(true);
  } else if (y >= ST_APPS_Y - 4 && y < ST_APPS_Y + MAX_HOME * ST_APP_STEP - 4) {
    int i = (y - ST_APPS_Y + 4) / ST_APP_STEP;
    uint8_t next = appsShown ^ (1 << i);
    if (next & 0x0F) appsShown = next;  // keep at least one app
    layoutHome();
  } else if (y >= ST_CONTRAST_Y - 4 && y < ST_CONTRAST_Y + 24) {
    setContrast(!highContrast());
    saveSettings();
    drawPage();
    return;
  } else return;
  saveSettings();
  drawSettingsValues();
}

int pageShows() {
  if (app == APP_HOME)                   return SHOWS_HOME;
  if (app == APP_PRINTER)                 return page == 0 ? SHOWS_PRINTER : SHOWS_CAMERA;
  if (app == APP_JIRA)                    return SHOWS_JIRA;
  if (app == APP_CAL)                     return SHOWS_CAL;
  if (app == APP_SETTINGS)                return SHOWS_SETTINGS;
  if (page == 0)                          return SHOWS_OVERVIEW;
  if (page == CLAUDE_PAGE)                return SHOWS_CLAUDE;
  if (page == RECENT_PAGE)                return SHOWS_RECENT;
  if (page == PIPELINES_PAGE)             return SHOWS_PIPELINES;
  if (page < FIRST_MODEL_PAGE + nModels)  return SHOWS_MODELS;
  if (page == daysPage())                 return SHOWS_DAYS;
  if (page == hoursPage())                return SHOWS_HOURS;
  return SHOWS_PROJECTS;
}

bool pageHasData() {
  switch (pageShows()) {
    case SHOWS_DAYS:     return nDays > 0;
    case SHOWS_HOURS:    return hoursValid;
    case SHOWS_PROJECTS: return nProjects > 0;
    case SHOWS_PIPELINES: return pipelinesValid && nPipelines > 0;
    case SHOWS_PRINTER:
    case SHOWS_CAMERA:   return printerValid;
    case SHOWS_JIRA:     return jiraValid;
    case SHOWS_CAL:      return calValid;
    default:             return true;
  }
}

bool drawnWithData = false;  // whether the static layer was drawn in the "has data" state

void drawContent() {
  switch (pageShows()) {
    case SHOWS_OVERVIEW: drawOverviewValues(); break;
    case SHOWS_MODELS:   drawModelValues(page - FIRST_MODEL_PAGE); break;
    case SHOWS_DAYS:     drawDaysValues(); break;
    case SHOWS_HOURS:    drawHoursValues(); break;
    case SHOWS_RECENT:   drawRecentValues(); break;
    case SHOWS_CLAUDE:   drawClaudeValues(); break;
    case SHOWS_PROJECTS: drawProjectsValues(); break;
    case SHOWS_PIPELINES: drawPipelinesValues(); break;
    case SHOWS_HOME:     drawHomeValues(); break;
    case SHOWS_PRINTER:  drawPrinterValues(); break;
    case SHOWS_CAMERA:   drawCameraInfo(); break;
    case SHOWS_JIRA:     drawJiraValues(); break;
    case SHOWS_CAL:      drawCalValues(); break;
    case SHOWS_SETTINGS: drawSettingsValues(); break;
  }
}

// Full redraw: only on page change, page count change, or no-data -> data.
void drawPage() {
  tft.fillScreen(C_BG);
  drawHeader();
  switch (pageShows()) {
    case SHOWS_OVERVIEW: drawOverviewStatic(); break;
    case SHOWS_MODELS:   drawModelStatic(); break;
    case SHOWS_DAYS:     drawDaysStatic(); break;
    case SHOWS_HOURS:    drawHoursStatic(); break;
    case SHOWS_RECENT:   drawRecentStatic(); break;
    case SHOWS_CLAUDE:   drawClaudeStatic(); break;
    case SHOWS_PROJECTS: drawProjectsStatic(); break;
    case SHOWS_PIPELINES: drawPipelinesStatic(); break;
    case SHOWS_HOME:     drawHomeStatic(); break;
    case SHOWS_PRINTER:  drawPrinterStatic(); break;
    case SHOWS_CAMERA:   drawCameraStatic(); break;
    case SHOWS_JIRA:     drawJiraStatic(); break;
    case SHOWS_CAL:      drawCalStatic(); break;
    case SHOWS_SETTINGS: drawSettingsStatic(); break;
  }
  drawnWithData = pageHasData();
  drawContent();
  drawStatus();
  drawOverlay();
}

// ---------- notifications ----------
int textWidth(const char* s) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Word-wraps s in FreeSans 9pt within maxW pixels, lineH apart from top y; the last line ends
// in "..." when the text doesn't fit. Returns the number of lines drawn.
int drawWrapped(int x, int y, int maxW, int maxLines, int lineH, const char* s, const GFXfont* font) {
  tft.setFont(font);
  char line[64];
  int lines = 0;
  while (*s && lines < maxLines) {
    while (*s == ' ') s++;
    if (!*s) break;
    int n = strlen(s), fit = 0;
    while (fit < n) {  // add words while the line fits
      int next = fit;
      while (next < n && s[next] == ' ') next++;
      while (next < n && s[next] != ' ') next++;
      if (next >= (int)sizeof(line) - 4) break;
      memcpy(line, s, next);
      line[next] = 0;
      if (textWidth(line) > maxW) break;
      fit = next;
    }
    if (fit == 0) {  // one word wider than the line: cut it
      fit = 1;
      while (fit < n && fit < (int)sizeof(line) - 4) {
        memcpy(line, s, fit + 1);
        line[fit + 1] = 0;
        if (textWidth(line) > maxW) break;
        fit++;
      }
    }
    const char* rest = s + fit;
    while (*rest == ' ') rest++;
    int len = fit;
    memcpy(line, s, len);
    line[len] = 0;
    if (lines == maxLines - 1 && *rest) {  // last line and more to come: ellipsis
      while (len > 0) {
        strcpy(line + len, "...");
        if (textWidth(line) <= maxW) break;
        len--;
      }
    }
    tft.setCursor(x, y + lines * lineH + 13);  // the cursor is the baseline with this font
    tft.print(line);
    s += fit;
    lines++;
  }
  tft.setFont();  // back to the built-in font
  return lines;
}

const int NC_X = 10, NC_Y = 44, NC_W = 220, NC_H = 200;               // notification card
const int NB_X = NC_X + 12, NB_Y = NC_Y + NC_H - 30, NB_W = NC_W - 24;  // its progress bar

int noticeBarFill() {
  unsigned long ttl = notices[0].ttl * 1000UL;
  unsigned long el = millis() - noticeShownAt;
  return el >= ttl ? NB_W : (int)(el * NB_W / ttl);
}

// "GITHUB ACTIONS", "CLAUDE CODE", ...: the app name, else named after the source.
void noticeFrom(const char* app, const char* src, char* buf, size_t len) {
  const char* from = app[0] ? app
                   : strcmp(src, "desktop") == 0 ? "Claude Desktop"
                   : strcmp(src, "code") == 0 ? "Claude Code" : "Webhook";
  strlcpy(buf, from, len);
  for (char* c = buf; *c; c++) *c = toupper(*c);
}

void drawNotice() {
  const Notice& n = notices[0];
  const int x = NC_X, y = NC_Y, w = NC_W, h = NC_H;
  uint16_t border = n.prio == PRIO_HIGH ? C_RED : n.prio == PRIO_LOW ? C_DIM : C_ACCENT;
  tft.fillRoundRect(x, y, w, h, 8, C_CARD);
  tft.drawRoundRect(x, y, w, h, 8, border);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, border);

  // Header: the app name (webhooks) or where it came from, uppercase; priority badge on the right
  char head[25];
  noticeFrom(n.app, n.src, head, sizeof(head));  // 24 chars leave room for the badge
  tft.setTextSize(1);
  tft.setTextColor(C_ACCENT, C_CARD);
  tft.setCursor(x + 12, y + 12);
  tft.print(head);
  if (n.prio != PRIO_NORMAL) {
    const char* badge = n.prio == PRIO_HIGH ? "HIGH" : "LOW";
    tft.setTextColor(n.prio == PRIO_HIGH ? C_RED : C_DIM, C_CARD);
    tft.setCursor(x + w - 12 - strlen(badge) * 6, y + 12);
    tft.print(badge);
  }

  int bodyY = y + 26, bodyLines = 8;  // without a title the message uses its space
  if (n.title[0]) {
    char t[18];
    strlcpy(t, n.title, sizeof(t));  // 17 chars fit at size 2
    tft.setTextSize(2);
    tft.setTextColor(C_TEXT, C_CARD);
    tft.setCursor(x + 12, y + 28);
    tft.print(t);
    bodyY = y + 50; bodyLines = 6;
  }
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT);  // custom fonts draw without a background; the card is filled already
  drawWrapped(x + 12, bodyY, w - 24, bodyLines, 18, n.body);

  if (n.ttl) {  // track, then the part that has already elapsed
    noticeBarDrawn = noticeBarFill();
    tft.fillRect(NB_X, NB_Y, NB_W, 4, C_BG);
    tft.fillRect(NB_X, NB_Y, noticeBarDrawn, 4, C_ACCENT);
  }

  tft.setTextColor(C_DIM, C_CARD);
  tft.setCursor(x + 12, y + h - 18);
  tft.print("tap to dismiss");
  if (nNotices > 1) {
    char more[12];
    snprintf(more, sizeof(more), "+%d more", nNotices - 1);
    tft.setCursor(x + w - 12 - strlen(more) * 6, y + h - 18);
    tft.print(more);
  }
}

uint8_t parsePrio(const char* p) {
  if (!p) return PRIO_NORMAL;
  if (strcmp(p, "high") == 0) return PRIO_HIGH;
  if (strcmp(p, "low") == 0) return PRIO_LOW;
  return PRIO_NORMAL;
}

void pushNotice(const char* title, const char* body, const char* src, uint16_t ttl,
                const char* app = "", uint8_t prio = PRIO_NORMAL) {
  if (nNotices == MAX_NOTICES) {  // drop the oldest queued one, keep the one on screen
    memmove(&notices[1], &notices[2], (MAX_NOTICES - 2) * sizeof(Notice));
    nNotices--;
  }
  Notice nw;
  strlcpy(nw.title, title, sizeof(nw.title));
  strlcpy(nw.body, body, sizeof(nw.body));
  strlcpy(nw.src, src, sizeof(nw.src));
  strlcpy(nw.app, app, sizeof(nw.app));
  nw.prio = prio;
  nw.ttl = ttl;
  // High priority goes straight after the card on screen, ahead of the other queued ones
  int at = nNotices;
  if (prio == PRIO_HIGH && nNotices > 1) {
    at = 1;
    while (at < nNotices && notices[at].prio == PRIO_HIGH) at++;  // keep high ones in order
    memmove(&notices[at + 1], &notices[at], (nNotices - at) * sizeof(Notice));
  }
  notices[at] = nw;
  remember(nw);
  drawUnreadBadge();
  nNotices++;
  if (nNotices == 1) noticeShownAt = millis();  // queued ones start their timer when shown
  if (prio != PRIO_LOW) blinkUntil = millis() + (prio == PRIO_HIGH ? 3000 : 1200);
  if (!nApprovals) drawNotice();  // redraws the card in place (also updates "+N more")
}

void dismissNotice(bool all) {
  if (nNotices == 0) return;
  if (all) nNotices = 0;
  else {
    memmove(&notices[0], &notices[1], (nNotices - 1) * sizeof(Notice));
    nNotices--;
  }
  noticeShownAt = millis();
  if (nNotices > 0) drawNotice();
  else drawPage();
}

// Grows the progress bar a few pixels at a time (no redraw) and closes the card when full.
void noticeTick() {
  static unsigned long last = 0;
  if (nNotices == 0 || nApprovals > 0 || notices[0].ttl == 0 || millis() - last < 50) return;
  last = millis();
  int fill = noticeBarFill();
  if (fill > noticeBarDrawn) {
    tft.fillRect(NB_X + noticeBarDrawn, NB_Y, fill - noticeBarDrawn, 4, C_ACCENT);
    noticeBarDrawn = fill;
  }
  if (fill >= NB_W) dismissNotice(false);
}

void blinkTick() {
  if (!blinkUntil) return;
  if ((long)(millis() - blinkUntil) >= 0) { blinkUntil = 0; backlight(true); return; }
  backlight((millis() / 150) % 2 == 0);
}


// ---------- Claude Code permission requests ----------
// A card like a notification with DENY / ALLOW buttons; it goes before notifications. The bar
// shows how long Claude Code keeps waiting (then it asks in the terminal and the feeder cancels).
const int AC_H = 250, AB_Y = NC_Y + AC_H - 44, AB_H = 34;  // card height, buttons
const int AB_DENY_X = NC_X + 12, AB_ALLOW_X = NC_X + 116, AB_W = 92;
const int AP_BAR_Y = AB_Y - 10;

int approvalBarFill() {
  unsigned long ttl = approvals[0].ttl * 1000UL, el = millis() - approvalShownAt;
  return el >= ttl ? NB_W : (int)(el * NB_W / ttl);
}

void drawApproval() {
  const Approval& a = approvals[0];
  const int x = NC_X, y = NC_Y, w = NC_W;
  tft.fillRoundRect(x, y, w, AC_H, 8, C_CARD);
  tft.drawRoundRect(x, y, w, AC_H, 8, C_AMBER);
  tft.drawRoundRect(x + 1, y + 1, w - 2, AC_H - 2, 7, C_AMBER);
  tft.setTextSize(1);
  tft.setTextColor(C_ACCENT, C_CARD);
  tft.setCursor(x + 12, y + 12);
  tft.print("CLAUDE CODE");
  char head[16];
  if (nApprovals > 1) snprintf(head, sizeof(head), "ASKS 1/%d", nApprovals);
  else strcpy(head, "ASKS");
  tft.setTextColor(C_AMBER, C_CARD);
  tft.setCursor(x + w - 12 - strlen(head) * 6, y + 12);
  tft.print(head);

  char t[18];
  strlcpy(t, a.app, sizeof(t));  // 17 chars fit at size 2
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT, C_CARD);
  tft.setCursor(x + 12, y + 28);
  tft.print(t);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_CARD);
  tft.setCursor(x + 12, y + 50);
  tft.print("wants to use ");
  tft.setTextColor(C_AMBER, C_CARD);
  tft.print(a.tool);
  tft.setTextColor(C_TEXT);
  drawWrapped(x + 12, y + 62, w - 24, 6, 18, a.detail[0] ? a.detail : "(no details)");

  approvalBarDrawn = approvalBarFill();
  tft.fillRect(NB_X, AP_BAR_Y, NB_W, 4, C_BG);
  tft.fillRect(NB_X, AP_BAR_Y, approvalBarDrawn, 4, C_AMBER);

  tft.drawRoundRect(AB_DENY_X, AB_Y, AB_W, AB_H, 6, C_RED);
  tft.drawRoundRect(AB_DENY_X + 1, AB_Y + 1, AB_W - 2, AB_H - 2, 5, C_RED);
  tft.fillRoundRect(AB_ALLOW_X, AB_Y, AB_W, AB_H, 6, C_GREEN);
  tft.setTextSize(2);
  tft.setTextColor(C_RED, C_CARD);
  tft.setCursor(AB_DENY_X + (AB_W - 4 * 12) / 2, AB_Y + 10);
  tft.print("DENY");
  tft.setTextColor(C_CARD, C_GREEN);
  tft.setCursor(AB_ALLOW_X + (AB_W - 5 * 12) / 2, AB_Y + 10);
  tft.print("ALLOW");
}

void drawOverlay() {
  if (nApprovals > 0) drawApproval();
  else if (nNotices > 0) drawNotice();
}

// The approval card went away: the next one, else the page (and any waiting notification,
// whose timer starts over).
void removeApproval(int i) {
  if (i < 0 || i >= nApprovals) return;
  memmove(&approvals[i], &approvals[i + 1], (nApprovals - i - 1) * sizeof(Approval));
  nApprovals--;
  if (i > 0) { drawApproval(); return; }  // the one on screen stays; only "1/N" changes
  approvalShownAt = noticeShownAt = millis();
  if (nApprovals > 0) drawApproval();
  else drawPage();
}

// Board can only ask with a known tap position (two buttons); otherwise the feeder lets the
// terminal ask right away.
bool pushApproval(JsonObject o) {
  if (!(touchType == TOUCH_FT6206 || touchCal.ok) || nApprovals >= MAX_APPROVALS) return false;
  Approval& a = approvals[nApprovals];
  strlcpy(a.id, o["id"] | "", sizeof(a.id));
  strlcpy(a.app, o["app"] | "Claude Code", sizeof(a.app));
  strlcpy(a.tool, o["tool"] | "?", sizeof(a.tool));
  strlcpy(a.detail, o["detail"] | "", sizeof(a.detail));
  a.ttl = constrain(o["ttl"] | 25, 1, 600);
  if (!a.id[0]) return false;
  if (++nApprovals == 1) approvalShownAt = millis();
  drawApproval();  // the new card, or "1/N" on the one shown
  blinkUntil = millis() + 2000;
  return true;
}

void answerApproval(bool allow) {
  Serial.printf("{\"event\":\"approve\",\"id\":\"%s\",\"allow\":%s}\n", approvals[0].id, allow ? "true" : "false");
  removeApproval(0);
}

// Grows the bar; drops the card a little after Claude Code stopped waiting (the feeder's
// cancel normally comes first).
void approvalTick() {
  static unsigned long last = 0;
  if (nApprovals == 0 || millis() - last < 100) return;
  last = millis();
  int fill = approvalBarFill();
  if (fill > approvalBarDrawn) {
    tft.fillRect(NB_X + approvalBarDrawn, AP_BAR_Y, fill - approvalBarDrawn, 4, C_AMBER);
    approvalBarDrawn = fill;
  }
  if (millis() - approvalShownAt > approvals[0].ttl * 1000UL + 3000) removeApproval(0);
}

// Partial redraw after new data, if it concerns the page on screen.
// The home screen summarises every app, so any update concerns it.
void refresh(int changed) {
  if (covered() || !(changed & pageShows() || pageShows() == SHOWS_HOME)) return;
  if (pageHasData() != drawnWithData) drawPage();
  else drawContent();
}

void leavePage() {
  if (pageShows() == SHOWS_RECENT) for (int i = 0; i < nRecent; i++) recent[i].unread = false;  // seen
  openRecent = -1;
  openJira = -1;
  openCal = -1;
}

void setPage(int p) {
  leavePage();
  int n = pageCount();
  page = ((p % n) + n) % n;
  drawPage();
  if (pageShows() == SHOWS_CAMERA && millis() - snapAskedAt > 3000) askSnapshot();
}

void openApp(int a) {
  leavePage();
  app = a;
  page = 0;
  drawPage();
}

void pushHistory() {
  uint64_t t = totalTokens();
  uint64_t d = (lastTotal > 0 && t >= lastTotal) ? t - lastTotal : 0;
  lastTotal = t;
  memmove(hist, hist + 1, (HIST - 1) * sizeof(uint32_t));
  hist[HIST - 1] = (uint32_t)min<uint64_t>(d, UINT32_MAX);
}

// ---------- input ----------
void resetAll() {
  u = Usage();
  memset(hist, 0, sizeof(hist));
  lastTotal = 0;
  nModels = 0;
  nDays = 0;
  memset(hours, 0, sizeof(hours));
  hoursValid = false;
  nowHour = -1;
  nNotices = 0;
  nRecent = 0;
  openRecent = -1;
  nProjects = 0;
  nSessions = 0;
  sessionsValid = false;
  nPipelines = 0;
  pipelinesValid = false;
  lim = PlanLimits();
  prn = Printer();
  printerValid = false;
  spdPending = 0;
  snapLen = snapGot = snapReady = 0;
  snapErr[0] = 0;
  nJira = jiraTotal = 0;
  jiraValid = false;
  openJira = -1;
  nCal = 0;
  calValid = false;
  openCal = -1;
  nApprovals = 0;
}

void handleLine(char* line) {
  if (strcmp(line, "demo") == 0) {
    demo = !demo;
    if (demo) { startDemo(); nextDemoNotice = 0; }
    Serial.printf("{\"demo\":%s}\n", demo ? "true" : "false");
    setPage(min(page, pageCount() - 1));
    return;
  }
  if (strcmp(line, "reset") == 0) { resetAll(); openApp(APP_HOME); return; }
  if (strcmp(line, "contrast") == 0 || strcmp(line, "gamma") == 0) {
    Preferences prefs;
    prefs.begin(PREFS_NS, false);
    if (line[0] == 'c') {
      bool hc = !prefs.getBool("hicontrast", true);
      prefs.putBool("hicontrast", hc);
      setContrast(hc);
      Serial.printf("{\"contrast\":\"%s\"}\n", hc ? "high" : "standard");
    } else {
      uint8_t g = prefs.getUChar("gamma", 1);
      g = g >= 8 ? 1 : g << 1;  // 1, 2, 4, 8
      prefs.putUChar("gamma", g);
      tft.sendCommand(0x26, &g, 1);  // GAMSET: selects gamma curve 1-4
      Serial.printf("{\"gamma\":%d}\n", g == 1 ? 1 : g == 2 ? 2 : g == 4 ? 3 : 4);
    }
    prefs.end();
    drawPage();
    return;
  }
  if (strcmp(line, "calibrate") == 0) {
    bool ok = calibrateTouch();
    Serial.printf("{\"calibrate\":%s}\n", ok ? "true" : touchType == TOUCH_XPT2046 ? "false" : "\"n/a\"");
    return;
  }
  if (strcmp(line, "flip") == 0) {
    Preferences prefs;
    prefs.begin(PREFS_NS, false);
    bool flipped = !prefs.getBool("flip", false);
    prefs.putBool("flip", flipped);
    prefs.end();
    tft.setRotation((DISPLAY_ROTATION + (flipped ? 2 : 0)) % 4);
    drawPage();
    Serial.printf("{\"flip\":%s}\n", flipped ? "true" : "false");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    // Report length and start of the line so the sender can see what got mangled.
    char head[25];
    size_t n = strlcpy(head, line, sizeof(head));
    for (size_t i = 0; i < min(n, sizeof(head) - 1); i++)
      if (head[i] == '"' || head[i] == '\\' || head[i] < 32) head[i] = '_';
    Serial.printf("{\"ok\":false,\"error\":\"%s\",\"len\":%u,\"head\":\"%s\"}\n",
                  err.c_str(), (unsigned)strlen(line), head);
    return;
  }

  if (doc["limits"].is<JsonObject>()) {
    JsonObject o = doc["limits"].as<JsonObject>();
    static const char* keys[2][4] = {{"5h", "5h_in", "5h_proj", "5h_full"}, {"7d", "7d_in", "7d_proj", "7d_full"}};
    for (int i = 0; i < 2; i++) {
      lim.pct[i] = o[keys[i][0]].isNull() ? -1 : constrain(o[keys[i][0]].as<int>(), 0, 100);
      lim.resetIn[i] = o[keys[i][1]] | 0UL;
      lim.proj[i] = o[keys[i][2]] | -1;
      lim.full[i] = o[keys[i][3]] | 0UL;
    }
    lim.rxAt = millis();
    lim.valid = true;
    refresh(SHOWS_OVERVIEW | SHOWS_CLAUDE);
    Serial.println("{\"ok\":true}");
    return;
  }

  if (doc["notify"].is<JsonObject>()) {
    JsonObject o = doc["notify"].as<JsonObject>();
    pushNotice(o["title"] | "", o["body"] | "", o["src"] | "code", o["ttl"] | NOTICE_TTL,
               o["app"] | "", parsePrio(o["prio"]));
    Serial.println("{\"ok\":true}");
    return;
  }

  if (doc["approve"].is<JsonObject>()) {
    if (pushApproval(doc["approve"].as<JsonObject>())) Serial.println("{\"ok\":true}");
    else Serial.println("{\"ok\":false,\"error\":\"can't ask: touch not calibrated, or too many waiting\"}");
    return;
  }
  if (doc["approve_cancel"].is<const char*>()) {  // Claude Code stopped waiting
    for (int i = 0; i < nApprovals; i++)
      if (strcmp(approvals[i].id, doc["approve_cancel"]) == 0) { removeApproval(i); break; }
    Serial.println("{\"ok\":true}");
    return;
  }
  // Settings from the feeder's .env: {"settings":{"bright":60,"apps":"dev,jira","contrast":"high"}}
  if (doc["settings"].is<JsonObject>()) {
    JsonObject o = doc["settings"].as<JsonObject>();
    if (!o["bright"].isNull()) { brightness = constrain(o["bright"].as<int>(), 10, 100); backlight(true); }
    if (o["apps"].is<const char*>()) {
      uint8_t mask = 0;
      char list[64];
      strlcpy(list, o["apps"], sizeof(list));
      for (char* tok = strtok(list, ", "); tok; tok = strtok(nullptr, ", "))
        for (int i = 0; i < MAX_HOME; i++) if (strcasecmp(tok, APP_KEYS[i]) == 0) mask |= 1 << i;
      if (mask) { appsShown = mask; layoutHome(); }
    }
    if (o["contrast"].is<const char*>()) setContrast(strcmp(o["contrast"], "standard") != 0);
    saveSettings();
    if (!covered()) drawPage();
    Serial.println("{\"ok\":true}");
    return;
  }

  // Camera image: announced, then base64 chunks; drawn when complete.
  if (doc["snap"].is<JsonObject>()) {
    size_t n = doc["snap"]["len"] | 0;
    if (!snapBuf) snapBuf = (uint8_t*)malloc(SNAP_MAX);
    if (n == 0 || n > SNAP_MAX || !snapBuf) {
      Serial.printf("{\"ok\":false,\"error\":\"image of %u bytes, max %u\"}\n", (unsigned)n, (unsigned)SNAP_MAX);
      return;
    }
    snapLen = n;
    snapGot = snapReady = 0;  // the buffer is overwritten from here
    snapErr[0] = 0;
    Serial.println("{\"ok\":true}");
    return;
  }
  if (doc["snapd"].is<const char*>()) {
    const char* d = doc["snapd"];
    size_t out = 0;
    if (!snapLen || mbedtls_base64_decode(snapBuf + snapGot, SNAP_MAX - snapGot, &out,
                                          (const unsigned char*)d, strlen(d)) != 0) {
      snapLen = snapGot = 0;
      Serial.println("{\"ok\":false,\"error\":\"unexpected or bad image data\"}");
      return;
    }
    snapGot += out;
    if (snapGot >= snapLen) {
      snapReady = snapLen;
      snapLen = snapGot = 0;
      snapAt = millis();
      if (pageShows() == SHOWS_CAMERA && !covered()) { drawCameraImage(); drawCameraInfo(); }
    }
    Serial.println("{\"ok\":true}");
    return;
  }
  if (doc["snap_err"].is<const char*>()) {
    strlcpy(snapErr, doc["snap_err"], sizeof(snapErr));
    snapLen = snapGot = 0;
    if (pageShows() == SHOWS_CAMERA && !covered()) { if (!snapReady) drawCameraImage(); drawCameraInfo(); }
    Serial.println("{\"ok\":true}");
    return;
  }

  if (demo) {  // drop the demo numbers; real ones follow if the feeder has them
    lim = PlanLimits();
    prn = Printer();
    printerValid = jiraValid = calValid = false;
    nJira = jiraTotal = nCal = 0;
  }
  int oldPages = pageCount();
  int changed = 0;  // SHOWS_* bits for the pages this line affects
  bool overview = false;

  if (doc["models"].is<JsonArray>()) {
    changed |= SHOWS_MODELS;
    nModels = 0;
    for (JsonObject m : doc["models"].as<JsonArray>()) {
      if (nModels >= MAX_MODELS) break;
      ModelUsage& x = models[nModels++];
      strlcpy(x.name, m["name"] | "?", sizeof(x.name));
      x.in = m["in"] | 0ULL;
      x.out = m["out"] | 0ULL;
      x.cacheR = m["cr"] | 0ULL;
      x.cacheW = m["cw"] | 0ULL;
      x.cost = m["cost"] | 0.0f;
    }
  }
  if (doc["days"].is<JsonArray>()) {
    changed |= SHOWS_DAYS;
    JsonArray a = doc["days"].as<JsonArray>();
    int skip = max(0, (int)a.size() - MAX_DAYS);  // keep the most recent
    nDays = 0;
    for (JsonObject d : a) {
      if (skip-- > 0) continue;
      DayUsage& x = days[nDays++];
      strlcpy(x.d, d["d"] | "--", sizeof(x.d));
      x.tok = d["tok"] | 0ULL;
      x.cost = d["cost"] | 0.0f;
    }
  }
  if (doc["pipelines"].is<JsonArray>()) {
    changed |= SHOWS_PIPELINES;
    nPipelines = 0;
    for (JsonObject o : doc["pipelines"].as<JsonArray>()) {
      if (nPipelines >= MAX_PIPELINES) break;
      Pipeline& x = pipelines[nPipelines++];
      strlcpy(x.repo, o["repo"] | "?", sizeof(x.repo));
      strlcpy(x.branch, o["branch"] | "?", sizeof(x.branch));
      const char* st = o["state"] | "stopped";
      x.state = strcmp(st, "running") == 0 ? 'r' : strcmp(st, "paused") == 0 ? 'p'
              : strcmp(st, "passed") == 0 ? 's' : strcmp(st, "failed") == 0 ? 'f' : 'x';
      x.forS = o["for"] | 0UL;
      x.num = o["num"] | 0UL;
      strlcpy(x.by, o["by"] | "", sizeof(x.by));
    }
    pipelinesAt = millis();
    pipelinesValid = true;
  }
  if (doc["projects"].is<JsonArray>()) {
    changed |= SHOWS_PROJECTS;
    nProjects = 0;
    for (JsonObject o : doc["projects"].as<JsonArray>()) {
      if (nProjects >= MAX_MODELS) break;
      ProjectUsage& x = projects[nProjects++];
      strlcpy(x.name, o["name"] | "?", sizeof(x.name));
      x.tok = o["tok"] | 0ULL;
      x.cost = o["cost"] | 0.0f;
    }
  }
  if (doc["sessions"].is<JsonArray>()) {
    changed |= SHOWS_CLAUDE;
    nSessions = 0;
    for (JsonObject o : doc["sessions"].as<JsonArray>()) {
      if (nSessions >= MAX_SESSIONS) break;
      Session& x = sessions[nSessions++];
      strlcpy(x.name, o["name"] | "?", sizeof(x.name));
      const char* st = o["state"] | "idle";
      x.state = strcmp(st, "waiting") == 0 ? 'a' : strcmp(st, "working") == 0 ? 'w' : 'i';
      x.forS = o["for"] | 0UL;
      strlcpy(x.msg, o["msg"] | "", sizeof(x.msg));
    }
    sessionsAt = millis();
    sessionsValid = true;
  }
  if (doc["printer"].is<JsonObject>()) {
    changed |= SHOWS_PRINTER | SHOWS_CAMERA;
    JsonObject o = doc["printer"].as<JsonObject>();
    prn.on = o["on"] | false;
    strlcpy(prn.name, o["name"] | "3D printer", sizeof(prn.name));
    strlcpy(prn.st, o["st"] | "", sizeof(prn.st));
    strlcpy(prn.job, o["job"] | "", sizeof(prn.job));
    prn.pct = o["pct"] | -1;
    prn.left = o["left"] | 0UL;
    prn.layer = o["layer"] | 0;
    prn.layers = o["layers"] | 0;
    prn.noz = o["noz"] | 0;
    prn.nozT = o["nozt"] | 0;
    prn.bed = o["bed"] | 0;
    prn.bedT = o["bedt"] | 0;
    prn.spd = o["spd"] | 0;
    prn.err = o["err"] | 0UL;
    prn.rxAt = millis();
    if (spdPending == prn.spd) spdPending = 0;  // confirmed
    printerValid = true;
  }
  if (doc["cal"].is<JsonArray>()) {
    changed |= SHOWS_CAL;
    CalEvent open = {};
    if (openCal >= 0) open = cal[openCal];  // keep it open if it's still there
    openCal = -1;
    nCal = 0;
    for (JsonObject o : doc["cal"].as<JsonArray>()) {
      if (nCal >= MAX_CAL) break;
      CalEvent& e = cal[nCal];
      strlcpy(e.title, o["t"] | "", sizeof(e.title));
      strlcpy(e.loc, o["l"] | "", sizeof(e.loc));
      e.start = o["s"] | 0UL;
      e.end = o["e"] | e.start;
      e.allDay = o["ad"] | false;
      e.todo = o["r"] | false;
      if (open.title[0] && e.start == open.start && strcmp(e.title, open.title) == 0) openCal = nCal;
      nCal++;
    }
    calValid = true;
  }
  if (doc["jira"].is<JsonArray>()) {
    changed |= SHOWS_JIRA;
    char openKey[16] = "";  // keep the open issue open if it's still there
    if (openJira >= 0) strlcpy(openKey, jira[openJira].key, sizeof(openKey));
    openJira = -1;
    nJira = 0;
    for (JsonObject o : doc["jira"].as<JsonArray>()) {
      if (nJira >= MAX_JIRA) break;
      JiraIssue& x = jira[nJira];
      strlcpy(x.key, o["k"] | "?", sizeof(x.key));
      strlcpy(x.sum, o["s"] | "", sizeof(x.sum));
      strlcpy(x.st, o["st"] | "", sizeof(x.st));
      x.cat = (o["c"] | "n")[0];
      x.prio = constrain(o["p"] | 3, 1, 5);
      if (openKey[0] && strcmp(openKey, x.key) == 0) openJira = nJira;
      nJira++;
    }
    jiraTotal = doc["jira_total"] | nJira;
    jiraValid = true;
  }
  if (!doc["time"].isNull()) {
    clockBase = doc["time"].as<uint32_t>();
    clockAt = millis();
    clockValid = true;
  }
  if (doc["hours"].is<JsonArray>()) {
    changed |= SHOWS_HOURS;
    int i = 0;
    for (JsonVariant v : doc["hours"].as<JsonArray>()) {
      if (i >= 24) break;
      hours[i++] = v.as<uint64_t>();
    }
    for (; i < 24; i++) hours[i] = 0;
    nowHour = doc["now"] | -1;
    hoursValid = true;
  }

  if (!doc["in"].isNull())    { u.in     = doc["in"].as<uint64_t>(); overview = true; }
  if (!doc["out"].isNull())   { u.out    = doc["out"].as<uint64_t>(); overview = true; }
  if (!doc["cr"].isNull())    { u.cacheR = doc["cr"].as<uint64_t>(); overview = true; }
  if (!doc["cw"].isNull())    { u.cacheW = doc["cw"].as<uint64_t>(); overview = true; }
  if (!doc["limit"].isNull()) { u.limit = doc["limit"].as<uint64_t>(); changed |= SHOWS_OVERVIEW; }
  if (!doc["cost"].isNull())  { u.cost  = doc["cost"].as<float>(); changed |= SHOWS_OVERVIEW; }
  if (doc["label"].is<const char*>()) { strlcpy(u.label, doc["label"], sizeof(u.label)); changed |= SHOWS_OVERVIEW; }
  if (doc["model"].is<const char*>()) { strlcpy(u.model, doc["model"], sizeof(u.model)); changed |= SHOWS_OVERVIEW; }

  u.valid = true;
  demo = false;
  lastRx = millis();
  if (overview) { pushHistory(); changed |= SHOWS_OVERVIEW; }

  if (pageCount() != oldPages) setPage(min(page, pageCount() - 1));  // dots changed
  else { refresh(changed); drawStatus(); }
  Serial.println("{\"ok\":true}");
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      lineBuf[lineLen] = 0;
      if (lineLen > 0) handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ---------- touch ----------
// Minimal XPT2046 reader: pressure, plus the position mapped with a 3-point calibration kept in
// flash. (The XPT2046_Touchscreen library can't be used next to Adafruit_FT6206: both define
// TS_Point.)
#if TOUCH_OWN_BUS
SPIClass touchSPI(HSPI);
SPIClass& tspi = touchSPI;
#else
SPIClass& tspi = SPI;
#endif

uint16_t xptRead(uint8_t cmd) {
  tspi.transfer(cmd);
  return tspi.transfer16(0) >> 3;  // 12-bit result
}

int xptPressure() {
  if (TOUCH_IRQ != 255 && digitalRead(TOUCH_IRQ) == HIGH) return 0;  // PENIRQ idles high
  tspi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  int z1 = xptRead(0xB1);  // Z1, power-down between conversions keeps PENIRQ enabled
  int z2 = xptRead(0xC1);  // Z2
  xptRead(0xD0);           // X with power-down, re-arms PENIRQ
  digitalWrite(TOUCH_CS, HIGH);
  tspi.endTransaction();
  return z1 + 4095 - z2;
}

void initTouch() {
#if HAS_FT6206
  Wire.begin(CTP_SDA, CTP_SCL);
  if (ctp.begin(40)) { touchType = TOUCH_FT6206; return; }
#endif
#if TOUCH_OWN_BUS
  touchSPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
#endif
  if (TOUCH_IRQ != 255) pinMode(TOUCH_IRQ, TOUCH_IRQ >= 34 ? INPUT : INPUT_PULLUP);  // 34-39: no pull-ups
  touchType = TOUCH_XPT2046;
}

bool touched() {
  switch (touchType) {
    case TOUCH_FT6206:  return ctp.touched();
    case TOUCH_XPT2046: return xptPressure() > 400;
    default:            return false;
  }
}

bool isFlipped() { return tft.getRotation() != DISPLAY_ROTATION; }

// Raw 12-bit position averaged over 4 samples; false when not pressed (or lifted meanwhile).
bool xptRaw(int& rx, int& ry) {
  if (xptPressure() <= 400) return false;
  long sx = 0, sy = 0;
  tspi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  xptRead(0x91);  // the first conversion after switching is noisy
  for (int i = 0; i < 4; i++) { sx += xptRead(0xD1); sy += xptRead(0x91); }
  xptRead(0xD0);  // power down, re-arms PENIRQ
  digitalWrite(TOUCH_CS, HIGH);
  tspi.endTransaction();
  if (xptPressure() <= 400) return false;
  rx = sx / 4;
  ry = sy / 4;
  return true;
}

// Screen position of the current touch; false when not touched or not calibrated.
bool touchPoint(int& sx, int& sy) {
  bool turned;  // measured in the other orientation than the screen is in now
  if (touchType == TOUCH_FT6206) {
    if (!ctp.touched()) return false;
    TS_Point p = ctp.getPoint();
    sx = 239 - p.x;  // FT6206 reports in panel coordinates, rotated 180 against rotation 0
    sy = 319 - p.y;
    turned = isFlipped();
  } else if (touchType == TOUCH_XPT2046 && touchCal.ok) {
    int rx, ry;
    if (!xptRaw(rx, ry)) return false;
    int a = touchCal.swap ? ry : rx, b = touchCal.swap ? rx : ry;
    sx = CAL_X0 + (long)(a - touchCal.x0) * (CAL_X1 - CAL_X0) / (touchCal.x1 - touchCal.x0);
    sy = CAL_Y0 + (long)(b - touchCal.y0) * (CAL_Y1 - CAL_Y0) / (touchCal.y1 - touchCal.y0);
    turned = isFlipped() != touchCal.flip;
  } else {
    return false;
  }
  if (turned) { sx = 239 - sx; sy = 319 - sy; }
  sx = constrain(sx, 0, 239);
  sy = constrain(sy, 0, 319);
  return true;
}

void loadTouchCal() {
  Preferences prefs;
  prefs.begin(PREFS_NS, true);
  touchCal.ok = prefs.getBool("tc_ok", false);
  touchCal.swap = prefs.getBool("tc_swap", false);
  touchCal.flip = prefs.getBool("tc_flip", false);
  touchCal.x0 = prefs.getInt("tc_x0", 0);
  touchCal.x1 = prefs.getInt("tc_x1", 0);
  touchCal.y0 = prefs.getInt("tc_y0", 0);
  touchCal.y1 = prefs.getInt("tc_y1", 0);
  prefs.end();
  if (touchCal.x0 == touchCal.x1 || touchCal.y0 == touchCal.y1) touchCal.ok = false;
}

// Waits for a tap and returns its averaged raw position; false after 30 s without one.
bool calTap(int& rx, int& ry) {
  unsigned long start = millis();
  while (xptPressure() > 400 && millis() - start < 30000) delay(10);  // let go first
  while (millis() - start < 30000) {
    int x, y;
    if (xptRaw(x, y)) {
      long ax = 0, ay = 0;
      int n = 0;
      unsigned long t = millis();
      while (millis() - t < 300) {  // average while the finger rests on it
        if (xptRaw(x, y)) { ax += x; ay += y; n++; }
        delay(10);
      }
      while (xptPressure() > 400) delay(10);
      if (n >= 3) { rx = ax / n; ry = ay / n; return true; }
    }
    delay(10);
  }
  return false;
}

// Three crosses: top left, top right, bottom right. Stored in flash; false when skipped/failed.
bool calibrateTouch() {
  if (touchType != TOUCH_XPT2046) return false;
  static const int tx[3] = {CAL_X0, CAL_X1, CAL_X1}, ty[3] = {CAL_Y0, CAL_Y0, CAL_Y1};
  int rx[3], ry[3];
  bool ok = true;
  for (int i = 0; i < 3 && ok; i++) {
    tft.fillScreen(C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_TEXT, C_BG);
    tft.setCursor(12, 140);
    tft.print("Touch setup");
    char b[40];
    snprintf(b, sizeof(b), "Tap the centre of the cross (%d/3)", i + 1);
    label(12, 166, b);
    label(12, 180, "(skipped after 30 s)");
    tft.drawFastHLine(tx[i] - 12, ty[i], 25, C_ACCENT);
    tft.drawFastVLine(tx[i], ty[i] - 12, 25, C_ACCENT);
    tft.drawCircle(tx[i], ty[i], 5, C_ACCENT);
    ok = calTap(rx[i], ry[i]);
  }
  if (ok) {
    // crosses 1 -> 2 differ only in screen x: the raw axis that moved most is screen x
    bool swap = abs(ry[1] - ry[0]) > abs(rx[1] - rx[0]);
    int x0 = swap ? ry[0] : rx[0], x1 = swap ? ry[1] : rx[1];
    int y0 = swap ? rx[1] : ry[1], y1 = swap ? rx[2] : ry[2];  // crosses 2 -> 3: only screen y
    ok = abs(x1 - x0) > 400 && abs(y1 - y0) > 400;  // too close together: a missed tap
    if (ok) {
      touchCal.ok = true;
      touchCal.swap = swap;
      touchCal.flip = isFlipped();
      touchCal.x0 = x0; touchCal.x1 = x1; touchCal.y0 = y0; touchCal.y1 = y1;
      Preferences prefs;
      prefs.begin(PREFS_NS, false);
      prefs.putBool("tc_ok", true);
      prefs.putBool("tc_swap", swap);
      prefs.putBool("tc_flip", touchCal.flip);
      prefs.putInt("tc_x0", x0); prefs.putInt("tc_x1", x1);
      prefs.putInt("tc_y0", y0); prefs.putInt("tc_y1", y1);
      prefs.end();
    }
  }
  drawPage();
  return ok;
}

// A short tap: dismiss the card on screen; on the home screen open the app tapped; the header
// goes home; open/close a notification or issue, set the printer speed, or turn the page.
// x/y are -1 when the position isn't known (not calibrated).
void onTap(int x, int y) {
  if (nApprovals > 0) {  // only the buttons count: a stray tap mustn't decide
    if (y >= AB_Y - 6 && y < AB_Y + AB_H + 8) {
      if (x >= AB_DENY_X - 4 && x < AB_DENY_X + AB_W + 4) answerApproval(false);
      else if (x >= AB_ALLOW_X - 4 && x < AB_ALLOW_X + AB_W + 4) answerApproval(true);
    }
    return;
  }
  if (nNotices > 0) { dismissNotice(false); return; }
  if (app != APP_HOME && y >= 0 && y < 30) { openApp(APP_HOME); return; }  // the "home button"
  switch (pageShows()) {
    case SHOWS_HOME: {
      int a = y >= 0 ? homeAppAt(x, y) : APP_DEV;  // no position: Development, the main app
      if (a >= 0) openApp(a);
      return;
    }
    case SHOWS_RECENT: {
      if (openRecent >= 0) { openRecent = -1; drawPage(); return; }
      int row = y >= 0 ? recentRowAt(y) : -1;
      if (row >= 0) { openRecent = row; drawPage(); return; }
      break;
    }
    case SHOWS_SETTINGS:
      if (y >= 0) onSettingsTap(x, y);
      return;
    case SHOWS_CAL: {
      if (openCal >= 0) { openCal = -1; drawPage(); return; }
      int row = y >= 0 ? calRowAt(y) : -1;
      if (row >= 0) { openCal = row; drawPage(); }
      return;
    }
    case SHOWS_JIRA: {
      if (openJira >= 0) { openJira = -1; drawPage(); return; }
      int row = y >= 0 ? jiraRowAt(y) : -1;
      if (row >= 0) { openJira = row; drawPage(); }
      return;  // a single page: nothing to turn to
    }
    case SHOWS_PRINTER: {
      int lvl = y >= 0 ? speedButtonAt(x, y) : 0;
      if (lvl) { setSpeed(lvl); return; }
      break;
    }
  }
  setPage(page + 1);
}

// Tap = onTap, hold >= 700 ms = back to the home screen. The position is read when the finger lands.
void pollTouch() {
  static bool down = false, held = false, hasPos = false;
  static int px = -1, py = -1;
  static unsigned long since = 0, lastPoll = 0;
  if (touchType == TOUCH_NONE || millis() - lastPoll < 20) return;
  lastPoll = millis();

  bool t = touched();
  if (t && !down) { down = true; held = false; since = millis(); hasPos = touchPoint(px, py); }
  else if (t && down && !hasPos && millis() - since < 200) hasPos = touchPoint(px, py);
  else if (t && down && !held && millis() - since >= 700) {
    held = true;
    nNotices = 0;
    openApp(APP_HOME);
  }
  else if (!t && down) {
    down = false;
    if (held || millis() - since < 30) return;
    onTap(hasPos ? px : -1, hasPos ? py : -1);
  }
}

// ---------- demo ----------
void startDemo() {
  static const char* names[] = {"claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-4-5"};
  resetAll();
  strlcpy(u.label, "DEMO", sizeof(u.label));
  strlcpy(u.model, names[0], sizeof(u.model));
  u.limit = 8000000;
  u.valid = true;
  lim.pct[0] = 37; lim.resetIn[0] = 2 * 3600 + 14 * 60;
  lim.pct[1] = 58; lim.resetIn[1] = 3 * 86400 + 5 * 3600;
  lim.rxAt = millis();
  lim.valid = true;
  nModels = 3;
  for (int i = 0; i < nModels; i++) {
    memset(&models[i], 0, sizeof(ModelUsage));
    strlcpy(models[i].name, names[i], sizeof(models[i].name));
  }
  nDays = MAX_DAYS;
  for (int i = 0; i < nDays; i++) {
    snprintf(days[i].d, sizeof(days[i].d), "09-%02d", 17 + i);
    days[i].tok = i == nDays - 1 ? 0 : random(500000, 6000000);
    days[i].cost = days[i].tok * 0.000004f;
  }
  nowHour = 14;
  for (int h = 0; h < nowHour; h++) hours[h] = (h >= 8) ? random(50000, 400000) : 0;
  hoursValid = true;

  prn.on = true;
  strlcpy(prn.name, "Bambu P1S", sizeof(prn.name));
  strlcpy(prn.st, "RUNNING", sizeof(prn.st));
  strlcpy(prn.job, "TribeBuddy case v3", sizeof(prn.job));
  prn.pct = 42; prn.layer = 118; prn.layers = 284; prn.left = 83;
  prn.noz = 219; prn.nozT = 220; prn.bed = 60; prn.bedT = 60; prn.spd = 2;
  prn.rxAt = millis();
  printerValid = true;

  static const char* const issues[][4] = {  // key, summary, status, category
    {"TRIB-1187", "Prijslijst wordt leeggemaakt na release 4.2", "In Progress", "i"},
    {"ZIN-342", "Inlogscherm toont verkeerde foutmelding bij verlopen wachtwoord", "To Do", "n"},
    {"GRP-88", "API rate limiting voor partnerkoppeling", "In Review", "i"},
    {"TRIB-1201", "Statuspagina koppelen aan UptimeRobot", "To Do", "n"},
  };
  nJira = 4;
  for (int i = 0; i < nJira; i++) {
    strlcpy(jira[i].key, issues[i][0], sizeof(jira[i].key));
    strlcpy(jira[i].sum, issues[i][1], sizeof(jira[i].sum));
    strlcpy(jira[i].st, issues[i][2], sizeof(jira[i].st));
    jira[i].cat = issues[i][3][0];
    jira[i].prio = i + 1;
  }
  jiraTotal = 9;
  jiraValid = true;

  uint32_t now = clockValid ? nowEpoch() : 1790946000;  // demo without a clock: a Friday, 11:00
  static const char* const events[][2] = {{"Standup", "Google Meet"}, {"Sprintreview ZIN", "Vergaderruimte 1"},
                                          {"Lunch", ""}, {"Release GRP naar productie", ""}};
  static const int32_t at[][2] = {{-600, 300}, {720, 4320}, {7200, 9000}, {90000, 90000}};  // from now
  nCal = 4;
  for (int i = 0; i < nCal; i++) {
    memset(&cal[i], 0, sizeof(CalEvent));
    strlcpy(cal[i].title, events[i][0], sizeof(cal[i].title));
    strlcpy(cal[i].loc, events[i][1], sizeof(cal[i].loc));
    cal[i].start = now + at[i][0] - now % 300;
    cal[i].end = now + at[i][1] - now % 300;
    cal[i].todo = i == 3;
  }
  calValid = true;
}

// A random example notification every 20-40 s while demo mode runs.
void demoNotice() {
  static const char* const samples[][5] = {  // title, body, src, app, prio
    {"tribebuddy", "Claude needs your permission to use Bash", "code"},
    {"tribebuddy", "Finished - waiting for you", "code"},
    {"bespeak-frontend", "Claude is waiting for your input", "code"},
    {"tribe-website", "Claude needs your permission to use Edit", "code"},
    {"Cowork", "Your task \"Quarterly report\" is ready to review", "desktop"},
    {"Cowork", "Claude needs your approval to send an email to 3 recipients", "desktop"},
    {"Claude", "Research on ISO 9001 audit checklist is complete", "desktop"},
    {"Deploy failed", "tribe-website: build failed on main (step: test)", "hook", "GitHub Actions", "high"},
    {"", "Backup of the Drive finished, 2,431 files", "hook", "Backup", "low"},
    {"", "Site down: bespeak.nl returned 502 for 2 minutes", "hook", "UptimeRobot", "high"},
  };
  const int n = sizeof(samples) / sizeof(samples[0]);
  int i = random(0, n);
  uint8_t prio = parsePrio(samples[i][4]);
  pushNotice(samples[i][0], samples[i][1], samples[i][2], NOTICE_TTL,
             samples[i][3] ? samples[i][3] : "", prio);
}

void demoTick() {
  if (!nextDemoNotice) nextDemoNotice = millis() + 8000;  // first one soon after starting
  if ((long)(millis() - nextDemoNotice) >= 0) {
    demoNotice();
    nextDemoNotice = millis() + random(20000, 40000);
  }
  uint32_t burst = random(0, 5) == 0 ? 0 : random(2000, 60000);
  int mi = random(0, 10) < 6 ? 0 : random(1, 3);
  ModelUsage& m = models[mi];
  m.in += burst / 10;     u.in += burst / 10;
  m.out += burst / 4;     u.out += burst / 4;
  m.cacheR += burst / 2;  u.cacheR += burst / 2;
  m.cacheW += burst / 20; u.cacheW += burst / 20;
  float c = burst * 0.000004f;
  m.cost += c;            u.cost += c;
  uint64_t add = burst / 10 + burst / 4 + burst / 2 + burst / 20;
  days[nDays - 1].tok += add;
  days[nDays - 1].cost += c;
  hours[nowHour] += add;
  if (totalTokens() > u.limit) { startDemo(); }
  pushHistory();
  refresh(SHOWS_OVERVIEW | SHOWS_MODELS | SHOWS_DAYS | SHOWS_HOURS);
}

// ---------- main ----------
void setup() {
  Serial.setRxBufferSize(4096);  // model/day lines are longer than the default 256 bytes
  Serial.begin(115200);
  initBacklight();
  backlight(true);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);  // keep the touch chip quiet until it's used

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin(40000000);
  migratePrefs();
  Preferences prefs;
  prefs.begin(PREFS_NS, true);
  bool flipped = prefs.getBool("flip", false);
  setContrast(prefs.getBool("hicontrast", true));
  uint8_t gamma = prefs.getUChar("gamma", 1);
  brightness = constrain(prefs.getUChar("bright", 100), 10, 100);
  appsShown = prefs.getUChar("apps", 0x0F);
  prefs.end();
  backlight(true);
  layoutHome();
  tft.setRotation((DISPLAY_ROTATION + (flipped ? 2 : 0)) % 4);  // portrait 240x320
  tft.setTextWrap(false);  // text too long for its spot is cut off at the edge, not wrapped onto the next line
  tft.sendCommand(0x26, &gamma, 1);
  // ILI9341 reports 0x9341 here; anything else (0x0000/0xFFFF) means another controller or no MISO
  uint16_t dispId = (tft.readcommand8(0xD3, 2) << 8) | tft.readcommand8(0xD3, 3);

  TJpgDec.setCallback(jpgBlock);  // camera images
  drawSplash();
  initTouch();
  loadTouchCal();
  delay(1500);
  drawPage();
  Serial.printf("{\"ready\":true,\"board\":\"%s\",\"display\":\"%04X\",\"touch\":\"%s\","
                "\"hint\":\"send JSON lines or type demo\"}\n", BOARD_NAME, dispId,
                touchType == TOUCH_FT6206 ? "ft6206" : touchType == TOUCH_XPT2046 ? "xpt2046" : "none");
  // Tapping a notification needs the touch position: calibrate once (skipped after 30 s,
  // asked again next boot; "calibrate" redoes it).
  if (touchType == TOUCH_XPT2046 && !touchCal.ok)
    Serial.printf("{\"calibrate\":%s}\n", calibrateTouch() ? "true" : "false");
}

void loop() {
  readSerial();
  pollTouch();
  blinkTick();
  noticeTick();
  approvalTick();
  if (demo && millis() - lastDemo > 1000) { lastDemo = millis(); demoTick(); }
  if (millis() - lastStatus > 1000) { lastStatus = millis(); drawStatus(); }
}
