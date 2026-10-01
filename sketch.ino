// TribeBuddy · Claude token monitor · Tribe Agency edition
// Boards: ESP32 "Cheap Yellow Display" (ESP32-2432S028R) or ESP32-S3 + ILI9341 module (the
// Wokwi simulator). The pin profile is picked from the chip you compile for; see Pins below.
// Reads newline-delimited JSON over serial and shows it on an ILI9341 2.8" TFT (portrait).
// Touch: tap = next page (or dismiss a notification; on the recent page, open the one tapped),
// hold = back to overview.
// Pages: overview, Claude (sessions, limit forecast), recent notifications, Bitbucket pipelines,
// one page per model,
// usage per day, usage per hour (today), cost per project (today).
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

int page = 0;  // 0 = overview, 1..nModels = models, then days, then hours

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
int pageCount() { return FIRST_MODEL_PAGE + 3 + nModels; }

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
  for (int i = 0; i < n; i++) {
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

  if (page == CLAUDE_PAGE && nNotices == 0) drawClaudeTicks();  // session timers, forecast
  if (page == PIPELINES_PAGE && nNotices == 0) drawPipelinesTicks();

  if (page == recentPage() && nNotices == 0) drawRecentValues();  // tick the ages
  if (page == 0 && nNotices == 0) {
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
const int MAX_RECENT = 7;  // what fits on the page
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
  int n = page == RECENT_PAGE ? 0 : unreadCount();
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
  static const char* dow[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};  // 1-1-1970: Thu
  unsigned long s = ms / 1000;
  if (s < 60)         snprintf(buf, len, "now");
  else if (s < 3600)  snprintf(buf, len, "%lum ago", s / 60);
  else if (clockValid) {
    uint32_t now = nowEpoch(), t = now - s;
    if (t / 86400 == now / 86400) snprintf(buf, len, "%02u:%02u", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
    else snprintf(buf, len, "%s %02u:%02u", dow[t / 86400 % 7], (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
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

// What the current page shows, so updates for other pages don't redraw it.
enum { SHOWS_OVERVIEW = 1, SHOWS_MODELS = 2, SHOWS_DAYS = 4, SHOWS_HOURS = 8, SHOWS_RECENT = 16,
       SHOWS_CLAUDE = 32, SHOWS_PROJECTS = 64, SHOWS_PIPELINES = 128 };
int pageShows() {
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
  }
  drawnWithData = pageHasData();
  drawContent();
  drawStatus();
  if (nNotices > 0) drawNotice();
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
  drawNotice();  // redraws the card in place (also updates "+N more")
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
  if (nNotices == 0 || notices[0].ttl == 0 || millis() - last < 50) return;
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
  if ((long)(millis() - blinkUntil) >= 0) { blinkUntil = 0; digitalWrite(TFT_BL, HIGH); return; }
  digitalWrite(TFT_BL, (millis() / 150) % 2 ? LOW : HIGH);
}

// Partial redraw after new data, if it concerns the page on screen.
void refresh(int changed) {
  if (nNotices > 0 || !(changed & pageShows())) return;
  if (pageHasData() != drawnWithData) drawPage();
  else drawContent();
}

void setPage(int p) {
  if (page == RECENT_PAGE) for (int i = 0; i < nRecent; i++) recent[i].unread = false;  // seen
  openRecent = -1;
  int n = pageCount();
  page = ((p % n) + n) % n;
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
}

void handleLine(char* line) {
  if (strcmp(line, "demo") == 0) {
    demo = !demo;
    if (demo) { startDemo(); nextDemoNotice = 0; }
    Serial.printf("{\"demo\":%s}\n", demo ? "true" : "false");
    setPage(min(page, pageCount() - 1));
    return;
  }
  if (strcmp(line, "reset") == 0) { resetAll(); setPage(0); return; }
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
  if (demo) lim = PlanLimits();  // drop the demo numbers; real ones follow if the feeder has them
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

// A short tap: dismiss the card on screen, open/close a notification on the recent page, or turn
// the page. x/y are -1 when the position isn't known (not calibrated).
void onTap(int x, int y) {
  if (nNotices > 0) { dismissNotice(false); return; }
  if (page == recentPage()) {
    if (openRecent >= 0) { openRecent = -1; drawPage(); return; }
    int row = y >= 0 ? recentRowAt(y) : -1;
    if (row >= 0) { openRecent = row; drawPage(); return; }
  }
  setPage(page + 1);
}

// Tap = onTap, hold >= 700 ms = back to overview. The position is read when the finger lands.
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
    setPage(0);
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
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
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
  prefs.end();
  tft.setRotation((DISPLAY_ROTATION + (flipped ? 2 : 0)) % 4);  // portrait 240x320
  tft.sendCommand(0x26, &gamma, 1);
  // ILI9341 reports 0x9341 here; anything else (0x0000/0xFFFF) means another controller or no MISO
  uint16_t dispId = (tft.readcommand8(0xD3, 2) << 8) | tft.readcommand8(0xD3, 3);

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
  if (demo && millis() - lastDemo > 1000) { lastDemo = millis(); demoTick(); }
  if (millis() - lastStatus > 1000) { lastStatus = millis(); drawStatus(); }
}
