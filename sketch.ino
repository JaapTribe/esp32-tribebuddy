// TribeBuddy · Claude token monitor · Tribe Agency edition
// Boards: ESP32 "Cheap Yellow Display" (ESP32-2432S028R) or ESP32-S3 + ILI9341 module (the
// Wokwi simulator). The pin profile is picked from the chip you compile for; see Pins below.
// Reads newline-delimited JSON over serial and shows it on an ILI9341 2.8" TFT (portrait).
// Touch: tap = next page (or dismiss a notification), hold = back to overview.
// Pages: overview, one page per model, usage per day, usage per hour (today).
//
// Line protocol (one JSON object per line, all fields optional; feeder.py sends these):
//   Overview: {"in":123456,"out":7890,"cr":2000000,"cw":150000,"cost":4.21,
//              "limit":5000000,"label":"TODAY","model":"claude-opus-4"}
//   Models:   {"models":[{"name":"claude-opus-4","in":1,"out":2,"cr":3,"cw":4,"cost":0.5}, ...]}
//   Days:     {"days":[{"d":"09-24","tok":123456,"cost":1.2}, ...]}   oldest first, last = today
//   Hours:    {"hours":[0,0,1200, ...24 values],"now":14}              index = local hour
//   Limits:   {"limits":{"5h":42,"5h_in":4320,"7d":61,"7d_in":190000}}  plan usage in percent and
//             seconds until each window resets (counted down here; 0 = unknown). Replaces the
//             "limit" bar. Greyed out after 10 minutes without an update, dropped after 30.
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
//           palette, "gamma" cycles the panel's 4 gamma curves (all remembered across restarts).
//
// Touch: XPT2046 (resistive) on the CYD; on the S3 profile FT6206 (capacitive, I2C; the
// simulator) is tried first, then XPT2046 (the TPM408-2.8 module).
// ESP32-S3 hardware: in Arduino IDE set Tools > "USB CDC On Boot" = Enabled so Serial is USB.

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
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
  unsigned long rxAt = 0;
  bool valid = false;
} lim;
const unsigned long LIMITS_STALE_MS = 10 * 60 * 1000UL, LIMITS_DROP_MS = 30 * 60 * 1000UL;

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

int pageCount() { return 3 + nModels; }
int daysPage() { return 1 + nModels; }
int hoursPage() { return 2 + nModels; }

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
void drawHeader() {
  tft.fillRect(0, 0, 240, 28, C_PANEL);
  tft.drawRGBBitmap(6, 2, tribeLogoSmall, TRIBELOGOSMALL_W, TRIBELOGOSMALL_H);
  // Page dots between the logo and the status
  int n = pageCount();
  int x0 = 128 - (n - 1) * 4;
  for (int i = 0; i < n; i++) {
    if (i == page) tft.fillCircle(x0 + i * 8, 14, 2, C_ACCENT);
    else           tft.drawCircle(x0 + i * 8, 14, 2, C_DIM);
  }
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
  char b[8];
  snprintf(b, sizeof(b), "%7s", txt);  // fixed width, right-aligned: overwrites the old text
  tft.fillCircle(226, 14, 5, c);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.setCursor(216 - 7 * 6, 10);
  tft.print(b);

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

// What the current page shows, so updates for other pages don't redraw it.
enum { SHOWS_OVERVIEW = 1, SHOWS_MODELS = 2, SHOWS_DAYS = 4, SHOWS_HOURS = 8 };
int pageShows() {
  if (page == 0)           return SHOWS_OVERVIEW;
  if (page <= nModels)     return SHOWS_MODELS;
  if (page == daysPage())  return SHOWS_DAYS;
  return SHOWS_HOURS;
}

bool pageHasData() {
  switch (pageShows()) {
    case SHOWS_DAYS:  return nDays > 0;
    case SHOWS_HOURS: return hoursValid;
    default:          return true;
  }
}

bool drawnWithData = false;  // whether the static layer was drawn in the "has data" state

void drawContent() {
  switch (pageShows()) {
    case SHOWS_OVERVIEW: drawOverviewValues(); break;
    case SHOWS_MODELS:   drawModelValues(page - 1); break;
    case SHOWS_DAYS:     drawDaysValues(); break;
    case SHOWS_HOURS:    drawHoursValues(); break;
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
int drawWrapped(int x, int y, int maxW, int maxLines, int lineH, const char* s) {
  tft.setFont(&FreeSans9pt7b);
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

void drawNotice() {
  const Notice& n = notices[0];
  const int x = NC_X, y = NC_Y, w = NC_W, h = NC_H;
  uint16_t border = n.prio == PRIO_HIGH ? C_RED : n.prio == PRIO_LOW ? C_DIM : C_ACCENT;
  tft.fillRoundRect(x, y, w, h, 8, C_CARD);
  tft.drawRoundRect(x, y, w, h, 8, border);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, border);

  // Header: the app name (webhooks) or where it came from, uppercase; priority badge on the right
  char head[25];
  const char* from = n.app[0] ? n.app
                   : strcmp(n.src, "desktop") == 0 ? "Claude Desktop"
                   : strcmp(n.src, "code") == 0 ? "Claude Code" : "Webhook";
  strlcpy(head, from, sizeof(head));  // 24 chars leave room for the badge
  for (char* c = head; *c; c++) *c = toupper(*c);
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
    static const char* keys[2][2] = {{"5h", "5h_in"}, {"7d", "7d_in"}};
    for (int i = 0; i < 2; i++) {
      lim.pct[i] = o[keys[i][0]].isNull() ? -1 : constrain(o[keys[i][0]].as<int>(), 0, 100);
      lim.resetIn[i] = o[keys[i][1]] | 0UL;
    }
    lim.rxAt = millis();
    lim.valid = true;
    refresh(SHOWS_OVERVIEW);
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
// Minimal XPT2046 reader: only the pressure (Z) is needed, so no calibration.
// (The XPT2046_Touchscreen library can't be used next to Adafruit_FT6206: both define TS_Point.)
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

// Tap = next page (or dismiss the notification on screen), hold >= 700 ms = back to overview. Position is ignored,
// so no calibration or orientation mapping is needed.
void pollTouch() {
  static bool down = false, held = false;
  static unsigned long since = 0, lastPoll = 0;
  if (touchType == TOUCH_NONE || millis() - lastPoll < 20) return;
  lastPoll = millis();

  bool t = touched();
  if (t && !down) { down = true; held = false; since = millis(); }
  else if (t && down && !held && millis() - since >= 700) {
    held = true;
    nNotices = 0;
    setPage(0);
  }
  else if (!t && down) {
    down = false;
    if (held || millis() - since < 30) return;
    if (nNotices > 0) dismissNotice(false);
    else setPage(page + 1);
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
  delay(1500);
  drawPage();
  Serial.printf("{\"ready\":true,\"board\":\"%s\",\"display\":\"%04X\",\"touch\":\"%s\","
                "\"hint\":\"send JSON lines or type demo\"}\n", BOARD_NAME, dispId,
                touchType == TOUCH_FT6206 ? "ft6206" : touchType == TOUCH_XPT2046 ? "xpt2046" : "none");
}

void loop() {
  readSerial();
  pollTouch();
  blinkTick();
  noticeTick();
  if (demo && millis() - lastDemo > 1000) { lastDemo = millis(); demoTick(); }
  if (millis() - lastStatus > 1000) { lastStatus = millis(); drawStatus(); }
}
