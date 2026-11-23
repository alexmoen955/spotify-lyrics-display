// ============================================================================
//  Spotify Now Playing + Synced Lyrics  —  ESP32 WROOM + 1.44" 128x128 ST7735
//
//  - Polls Spotify's "currently playing" endpoint
//  - Downloads album art (JPEG) and decodes it on the ESP32
//  - Fetches time-synced lyrics from LRCLIB (free, no key needed)
//  - Renders flicker-free at ~30 fps into an off-screen canvas:
//      * Lyrics view: blurred, slowly drifting album-colour background,
//        smoothly scrolling lyrics, current line highlighted
//      * Art view: full-screen album art with title + progress overlay
//      * Idle view: clock
//  - Networking runs on core 0, drawing on core 1, so lyrics never stutter
//
//  Press the BOOT button to switch between lyrics and album-art view.
//  All settings are in config.h.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <SHA2Builder.h>
#include <esp_random.h>
#include "qrcode.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <vector>
#include <algorithm>
#include <time.h>
#include "config.h"
#include "version.h"
#include "logger.h"
#include "settings.h"
#include "device.h"

// ---------------------------------------------------------------- constants --
static const int W = 128, H = 128;
static const int ART = 128;          // decoded album art size
static const int THUMB = 36;         // thumbnail in lyrics view
static const int GRID = 4;           // colour grid used for the blurred background
static const int LY_TOP = 44;        // lyrics area starts here
static const int LY_CENTER = LY_TOP + (H - LY_TOP) / 2;
static const int LYR_W = 118;        // max width of a lyric row
static const int MAX_ROWS = 4;       // max wrapped rows per lyric line
static const int LINE_GAP = 5;
static const uint32_t FRAME_MS = 33;
static const size_t MAX_JPG = 120 * 1024;
static const char* USER_AGENT = "SpotifyLyricsDisplay/" FW_VERSION " (ESP32)";

#define FONT_TITLE  u8g2_font_helvB08_tf
#define FONT_SUB    u8g2_font_helvR08_tf
#define FONT_LYRIC  u8g2_font_helvB10_tf
#define FONT_TINY   u8g2_font_tom_thumb_4x6_tf
#define FONT_CLOCK  u8g2_font_logisoso28_tn

// ------------------------------------------------------------ colour utils --
struct RGB { uint8_t r, g, b; };

static inline uint16_t rgb565(int r, int g, int b) {
  r = r < 0 ? 0 : (r > 255 ? 255 : r);
  g = g < 0 ? 0 : (g > 255 ? 255 : g);
  b = b < 0 ? 0 : (b > 255 ? 255 : b);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline RGB unpack(uint16_t c) {
  return { (uint8_t)(((c >> 11) & 31) * 255 / 31),
           (uint8_t)(((c >> 5) & 63) * 255 / 63),
           (uint8_t)((c & 31) * 255 / 31) };
}
// t = 0 -> a, t = 255 -> b
static inline uint16_t mix(uint16_t a, uint16_t b, int t) {
  if (t <= 0) return a;
  if (t >= 255) return b;
  RGB x = unpack(a), y = unpack(b);
  return rgb565(x.r + ((y.r - x.r) * t >> 8), x.g + ((y.g - x.g) * t >> 8), x.b + ((y.b - x.b) * t >> 8));
}
// k = 256 keeps the colour, 0 = black
static inline uint16_t scale565(uint16_t c, int k) {
  int r = ((c >> 11) & 31) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
  return (r << 11) | (g << 5) | b;
}
static const uint16_t WHITE = 0xFFFF, BLACK = 0x0000;

// ------------------------------------------------- canvas with clip rectangle --
class ClipCanvas : public GFXcanvas16 {
 public:
  ClipCanvas(uint16_t w, uint16_t h) : GFXcanvas16(w, h) { resetClip(); }
  void setClip(int16_t x, int16_t y, int16_t w, int16_t h) { cx0 = x; cy0 = y; cx1 = x + w; cy1 = y + h; }
  void resetClip() { cx0 = 0; cy0 = 0; cx1 = W; cy1 = H; }
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
    GFXcanvas16::drawPixel(x, y, c);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override {
    if (y < cy0 || y >= cy1) return;
    int16_t x1 = x + w;
    if (x < cx0) x = cx0;
    if (x1 > cx1) x1 = cx1;
    if (x1 <= x) return;
    GFXcanvas16::drawFastHLine(x, y, x1 - x, c);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override {
    if (x < cx0 || x >= cx1) return;
    int16_t y1 = y + h;
    if (y < cy0) y = cy0;
    if (y1 > cy1) y1 = cy1;
    if (y1 <= y) return;
    GFXcanvas16::drawFastVLine(x, y, y1 - y, c);
  }
 private:
  int16_t cx0, cy0, cx1, cy1;
};

WiFiMulti* g_wifiMulti = nullptr;   // rebuilt from the saved networks (settings.h)

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
ClipCanvas canvas(W, H);
U8G2_FOR_ADAFRUIT_GFX u8f;

// ------------------------------------------------------------ shared state --
// Written by the network task (core 0), read by the render loop (core 1).
// Always touch it while holding `mux`.
struct TrackInfo {
  bool active = false;     // something is playing / paused
  bool playing = false;
  char id[48] = "";
  char title[160] = "";
  char artist[160] = "";
  uint32_t durationMs = 0, progressMs = 0, stampMs = 0;
};
enum LyricsState : uint8_t { LYR_NONE, LYR_LOADING, LYR_SYNCED, LYR_NOT_FOUND, LYR_INSTRUMENTAL };
struct LyricLine { uint32_t t; String text; };
struct ArtSet {
  bool has = false;
  uint16_t thumb[THUMB * THUMB];
  RGB grid[GRID * GRID];     // darkened colour grid for the background
  uint16_t accent = 0x07E0;
};

SemaphoreHandle_t mux;
TrackInfo g_track;
std::vector<LyricLine> g_lyrics;
LyricsState g_lyrState = LYR_NONE;
uint32_t g_lyrVersion = 0;
ArtSet g_art[2];
int g_artFront = 0;
bool g_statusMode = true;
char g_status[96] = "Starting...";
uint32_t g_lastActiveMs = 0;
volatile bool g_forcePoll = false;
uint32_t g_artVersion = 0;

// --- Wi-Fi / setup hotspot (written by the network task) ---
volatile bool g_wifiUp = false;
volatile bool g_hotspot = false;
volatile bool g_setupDone = false;
volatile bool g_wifiListChanged = false;
uint32_t g_hotspotOffAt = 0;
char g_ipStr[20] = "";
std::vector<String> g_savedSsids;
enum TryState : uint8_t { TRY_IDLE, TRY_TRYING, TRY_OK, TRY_FAIL };
volatile bool g_tryPending = false;
TryState g_tryState = TRY_IDLE;
String g_trySsid, g_tryPass, g_tryMsg;

// --- Spotify login (PKCE: no client secret needed) ---
enum SpState : uint8_t { SP_NEEDS_CLIENT, SP_NEEDS_LOGIN, SP_CONNECTING, SP_CONNECTED, SP_ERROR };
SpState g_spState = SP_NEEDS_CLIENT;
String g_clientId, g_spUser, g_spMsg;
String g_pkceVerifier, g_pkceNonce, g_authCode;
volatile bool g_authPending = false;

// --- Firmware updates from GitHub releases ---
enum UpdState : uint8_t { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_AVAILABLE, UPD_INSTALLING, UPD_ERROR, UPD_DONE };
UpdState g_updState = UPD_IDLE;
String g_updLatest, g_updUrl, g_updMsg;
volatile int g_updProgress = 0;
volatile bool g_updCheckReq = false, g_updInstallReq = false;

volatile uint32_t g_resetHoldMs = 0;   // how long BOOT has been held (for the on-screen countdown)

static void lock() { xSemaphoreTake(mux, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(mux); }
static void setStatus(const char* s) {
  lock();
  strlcpy(g_status, s, sizeof(g_status));
  unlock();
  LOG.println(s);
}

static void logMem(const char* tag) {
  LOG.printf("mem %-14s free %6u  largest %6u  min %6u\n", tag, (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
}

static uint32_t positionOf(const TrackInfo& t, uint32_t now) {
  if (!t.playing) return t.progressMs;
  uint32_t p = t.progressMs + (now - t.stampMs);
  return (t.durationMs && p > t.durationMs) ? t.durationMs : p;
}

// ============================================================================
//                               TEXT HELPERS
// ============================================================================

// Replace typographic characters the font doesn't have with ASCII look-alikes.
static String sanitize(const char* s) {
  String o;
  o.reserve(strlen(s) + 4);
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint32_t cp; int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
    else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
    else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0x10000; n = 4; }
    else { p++; continue; }
    switch (cp) {
      case 0x2018: case 0x2019: case 0x201B: case 0x2032: case 0x00B4: o += '\''; break;
      case 0x201C: case 0x201D: case 0x201E: case 0x2033: o += '"'; break;
      case 0x2010: case 0x2011: case 0x2013: case 0x2014: case 0x2212: o += '-'; break;
      case 0x2026: o += "..."; break;
      case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: o += ' '; break;
      case 0x200B: case 0xFEFF: break;  // zero-width junk
      case '\r': case '\t': o += ' '; break;
      default:
        for (int i = 0; i < n; i++) o += (char)p[i];
    }
    p += n;
  }
  o.trim();
  return o;
}

static String urlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String o;
  o.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// "Song - Remastered 2011" / "Song (feat. X)" -> "Song"  (used for the fallback search)
static String cleanTitle(const String& t) {
  String s = t;
  int cut = s.indexOf(" - ");
  if (cut > 0) s = s.substring(0, cut);
  cut = s.indexOf(" (feat");
  if (cut > 0) s = s.substring(0, cut);
  cut = s.indexOf(" [feat");
  if (cut > 0) s = s.substring(0, cut);
  s.trim();
  return s;
}

// --- word wrap (uses the font currently selected on u8f) ---
struct Row { uint16_t start, len; };

static int textWidth(const char* s, int len) {
  static char buf[256];
  if (len > 255) len = 255;
  memcpy(buf, s, len);
  buf[len] = 0;
  return u8f.getUTF8Width(buf);
}

static int wrapText(const char* s, int maxW, Row* rows, int maxRows) {
  int n = strlen(s), count = 0, pos = 0;
  while (pos < n && count < maxRows) {
    while (pos < n && s[pos] == ' ') pos++;
    if (pos >= n) break;
    int lineEnd = pos, scan = pos;
    while (scan < n) {
      int wEnd = scan;
      while (wEnd < n && s[wEnd] != ' ') wEnd++;
      if (textWidth(s + pos, wEnd - pos) > maxW) break;
      lineEnd = wEnd;
      scan = wEnd;
      while (scan < n && s[scan] == ' ') scan++;
    }
    if (lineEnd == pos) {  // a single word wider than the row: hard-break it
      int e = pos, lastOk = pos;
      while (e < n && s[e] != ' ') {
        int nx = e + 1;
        while (nx < n && ((uint8_t)s[nx] & 0xC0) == 0x80) nx++;
        if (textWidth(s + pos, nx - pos) > maxW) break;
        lastOk = e = nx;
      }
      if (lastOk == pos) {
        lastOk = pos + 1;
        while (lastOk < n && ((uint8_t)s[lastOk] & 0xC0) == 0x80) lastOk++;
      }
      lineEnd = lastOk;
    }
    rows[count++] = { (uint16_t)pos, (uint16_t)(lineEnd - pos) };
    pos = lineEnd;
  }
  return count;
}

// ============================================================================
//                               LRC PARSING
// ============================================================================
static void parseLRC(const char* s, std::vector<LyricLine>& out) {
  out.clear();
  const char* p = s;
  while (*p) {
    const char* eol = strchr(p, '\n');
    if (!eol) eol = p + strlen(p);
    std::vector<uint32_t> stamps;
    const char* q = p;
    while (q < eol && *q == '[') {
      const char* close = (const char*)memchr(q, ']', eol - q);
      if (!close) break;
      // [mm:ss.xx] or [mm:ss.xxx] or [mm:ss]
      if (isdigit((uint8_t)q[1])) {
        int mm = atoi(q + 1);
        const char* colon = (const char*)memchr(q, ':', close - q);
        if (colon) {
          int ss = atoi(colon + 1);
          uint32_t ms = 0;
          const char* dot = (const char*)memchr(colon, '.', close - colon);
          if (!dot) dot = (const char*)memchr(colon, ',', close - colon);
          if (dot) {
            int digits = 0;
            for (const char* d = dot + 1; d < close && isdigit((uint8_t)*d) && digits < 3; d++, digits++)
              ms = ms * 10 + (*d - '0');
            while (digits++ < 3) ms *= 10;
          }
          stamps.push_back((uint32_t)mm * 60000UL + (uint32_t)ss * 1000UL + ms);
        }
      }
      q = close + 1;
    }
    if (!stamps.empty()) {
      String text;
      text.reserve(eol - q);
      for (const char* c = q; c < eol; c++) text += *c;
      String clean = sanitize(text.c_str());
      for (uint32_t t : stamps) out.push_back({ t, clean });
    }
    p = *eol ? eol + 1 : eol;
  }
  std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
  // An "intro" line (rendered as animated dots) until the first lyric starts.
  if (!out.empty() && out[0].t > 800) out.insert(out.begin(), LyricLine{ 0, String() });
  if (out.size() > 400) out.resize(400);
}

// ============================================================================
//                           ALBUM ART PROCESSING
// ============================================================================
static uint16_t* g_decodeTarget = nullptr;

static bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  for (int j = 0; j < h; j++) {
    int yy = y + j;
    if (yy < 0 || yy >= ART) continue;
    for (int i = 0; i < w; i++) {
      int xx = x + i;
      if (xx < 0 || xx >= ART) continue;
      g_decodeTarget[yy * ART + xx] = data[j * w + i];
    }
  }
  return true;
}

static void deriveDefault(ArtSet& s) {
  s.has = false;
  for (int gy = 0; gy < GRID; gy++)
    for (int gx = 0; gx < GRID; gx++) {
      int t = (gx + gy) * 255 / (2 * (GRID - 1));
      s.grid[gy * GRID + gx] = { (uint8_t)(55 + (10 - 55) * t / 255), (uint8_t)(25 + (65 - 25) * t / 255), (uint8_t)(85 + (75 - 85) * t / 255) };
    }
  for (int y = 0; y < THUMB; y++)
    for (int x = 0; x < THUMB; x++) {
      int t = (x + y) * 255 / (2 * THUMB);
      s.thumb[y * THUMB + x] = mix(rgb565(120, 60, 170), rgb565(30, 150, 140), t);
    }
  s.accent = rgb565(110, 230, 170);
}

static void deriveArt(ArtSet& s, const uint16_t* a) {
  // 1) background colour grid: average each cell, then darken
  const int cell = ART / GRID;
  for (int gy = 0; gy < GRID; gy++)
    for (int gx = 0; gx < GRID; gx++) {
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int y = gy * cell; y < (gy + 1) * cell; y += 2)
        for (int x = gx * cell; x < (gx + 1) * cell; x += 2) {
          RGB c = unpack(a[y * ART + x]);
          r += c.r; g += c.g; b += c.b; n++;
        }
      s.grid[gy * GRID + gx] = { (uint8_t)(r / n * 42 / 100), (uint8_t)(g / n * 42 / 100), (uint8_t)(b / n * 42 / 100) };
    }
  // 2) accent colour: dominant vivid hue (12-bin hue histogram weighted by saturation*value)
  float wsum[12] = {0}, rs[12] = {0}, gs[12] = {0}, bs[12] = {0};
  for (int y = 2; y < ART; y += 4)
    for (int x = 2; x < ART; x += 4) {
      RGB c = unpack(a[y * ART + x]);
      int mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
      if (mx < 60 || mx == mn) continue;
      float d = mx - mn, sat = d / mx, h;
      if (mx == c.r) h = fmodf((c.g - c.b) / d, 6.0f);
      else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
      else h = (c.r - c.g) / d + 4.0f;
      h *= 60.0f;
      if (h < 0) h += 360.0f;
      int bin = ((int)(h / 30.0f)) % 12;
      float w = sat * sat * (mx / 255.0f);
      wsum[bin] += w; rs[bin] += c.r * w; gs[bin] += c.g * w; bs[bin] += c.b * w;
    }
  int best = 0;
  float total = 0;
  for (int i = 0; i < 12; i++) { total += wsum[i]; if (wsum[i] > wsum[best]) best = i; }
  if (total < 3.0f || wsum[best] <= 0) {
    s.accent = rgb565(225, 225, 225);   // greyscale cover
  } else {
    float r = rs[best] / wsum[best], g = gs[best] / wsum[best], b = bs[best] / wsum[best];
    float m = max(r, max(g, b));
    float k = 255.0f / (m < 1 ? 1 : m);
    r *= k; g *= k; b *= k;
    s.accent = rgb565(r + (255 - r) * 0.22f, g + (255 - g) * 0.22f, b + (255 - b) * 0.22f);
  }
  // 3) thumbnail: box-filter downscale
  for (int ty = 0; ty < THUMB; ty++) {
    int y0 = ty * ART / THUMB, y1 = (ty + 1) * ART / THUMB;
    for (int tx = 0; tx < THUMB; tx++) {
      int x0 = tx * ART / THUMB, x1 = (tx + 1) * ART / THUMB;
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          RGB c = unpack(a[y * ART + x]);
          r += c.r; g += c.g; b += c.b; n++;
        }
      s.thumb[ty * THUMB + tx] = rgb565(r / n, g / n, b / n);
    }
  }
  s.has = true;
}

// ============================================================================
//                                 NETWORK
// ============================================================================
String g_accessToken, g_refreshToken;
uint32_t g_tokenExpires = 0;

// Download a URL straight into a LittleFS file. HTTPClient::writeToStream handles
// Content-Length, chunked encoding and "read until close" correctly, and only needs
// a small buffer, so this works even while TLS uses most of the RAM.
static int httpGetToFile(const String& url, const char* path, int* bytes, const char* tag) {
  *bytes = 0;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(12000);
  http.setUserAgent(USER_AGENT);
  if (!http.begin(client, url)) return -1;
  int code = http.GET();
  if (code == 200) {
    File f = LittleFS.open(path, "w");
    if (!f) {
      LOG.printf("%s: can't open %s\n", tag, path);
      code = -10;
    } else {
      int n = http.writeToStream(&f);
      f.close();
      if (n < 0) { LOG.printf("%s: download error %d\n", tag, n); code = -11; }
      else *bytes = n;
    }
  } else if (code < 0) {
    char err[100] = "";
    client.lastError(err, sizeof(err));
    LOG.printf("%s: HTTP %d %s (heap %u, largest %u)\n", tag, code, err, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
  }
  http.end();   // TLS memory is freed here
  return code;
}

// Decode /art.jpg from flash into the shared art buffer. Caller holds the lock.
static bool decodeArtFile(ArtSet& dst) {
  uint16_t w = 0, h = 0;
  if (TJpgDec.getFsJpgSize(&w, &h, "/art.jpg", LittleFS) != JDR_OK || w == 0 || h == 0) return false;
  int scale = 1;
  const int scales[] = { 8, 4, 2 };
  for (int sc : scales) if (w / sc >= ART && h / sc >= ART) { scale = sc; break; }
  TJpgDec.setJpgScale(scale);
  // Decode into the screen canvas (the caller holds the lock, so nothing is
  // drawing), derive colours/thumbnail, then keep the pixels in flash: the
  // full-size art only lives in RAM while the album-art view draws it.
  uint16_t* px = canvas.getBuffer();
  memset(px, 0, ART * ART * 2);
  g_decodeTarget = px;
  int ox = ((int)(w / scale) - ART) / 2, oy = ((int)(h / scale) - ART) / 2;  // centre crop
  JRESULT r = TJpgDec.drawFsJpg(-ox, -oy, "/art.jpg", LittleFS);
  if (r != JDR_OK) { LOG.printf("art: jpeg decode failed (%d)\n", (int)r); return false; }
  deriveArt(dst, px);
  File f = LittleFS.open("/art.raw", "w");
  bool saved = f && f.write((const uint8_t*)px, ART * ART * 2) == (size_t)(ART * ART * 2);
  if (f) f.close();
  if (!saved) LOG.println("art: couldn't save to flash");
  dst.has = saved;
  return true;
}

// Download outside the lock (slow), then decode under the lock (~200 ms).
static void fetchArt(const String& url) {
  uint32_t t0 = millis();
  int bytes = 0;
  bool ok = false;
  if (url.length() == 0) LOG.println("art: no image url");
  else ok = httpGetToFile(url, "/art.jpg", &bytes, "art") == 200 && bytes > 0;
  uint32_t t1 = millis();
  lock();
  bool dec = ok && decodeArtFile(g_art[0]);
  if (!dec) deriveDefault(g_art[0]);
  g_artVersion++;
  unlock();
  LOG.printf("art: %s, %d bytes, download %ums, decode %ums\n", dec ? "OK" : "FAILED", bytes,
                (unsigned)(t1 - t0), (unsigned)(millis() - t1));
}

// POST to Spotify's token endpoint (PKCE: client_id in the body, no secret).
// Returns the HTTP code; on 200 `out` holds the JSON answer, otherwise `err`
// holds Spotify's error code (e.g. "invalid_grant") and description.
static int spotifyTokenRequest(const String& body, JsonDocument& out, String& err) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return -1;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  if (code < 0) {
    char e[100] = "";
    client.lastError(e, sizeof(e));
    err = String("connection failed: ") + e;
  }
  http.end();
  JsonDocument doc;
  if (resp.length() && !deserializeJson(doc, resp)) {
    if (code == 200) out = doc;
    else err = String(doc["error"] | "error") + ": " + String(doc["error_description"] | "");
  }
  return code;
}

static void setSpotify(SpState st, const String& msg) {
  lock();
  g_spState = st;
  g_spMsg = msg;
  unlock();
  if (msg.length()) LOG.printf("Spotify: %s\n", msg.c_str());
}

static void storeTokens(JsonDocument& doc) {
  g_accessToken = doc["access_token"] | "";
  uint32_t exp = doc["expires_in"] | 3600;
  g_tokenExpires = millis() + (exp > 300 ? exp - 120 : exp) * 1000UL;
  const char* rt = doc["refresh_token"];
  if (rt && *rt && g_refreshToken != rt) {   // PKCE refresh tokens rotate: keep the newest
    g_refreshToken = rt;
    Settings::setSpotifyRefreshToken(g_refreshToken);
  }
}

static bool refreshAccessToken(int* httpCode) {
  String cid;
  lock(); cid = g_clientId; unlock();
  JsonDocument doc;
  String err;
  int code = spotifyTokenRequest(String("grant_type=refresh_token&refresh_token=") + urlEncode(g_refreshToken) +
                                 "&client_id=" + urlEncode(cid), doc, err);
  *httpCode = code;
  if (code != 200 || !doc["access_token"].is<const char*>()) {
    LOG.printf("token refresh failed %d: %s (heap %u, largest %u)\n", code, err.c_str(),
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return false;
  }
  storeTokens(doc);
  LOG.println("Spotify access token refreshed");
  return true;
}

static bool ensureToken() {
  if (g_accessToken.length() && (int32_t)(millis() - g_tokenExpires) < 0) return true;
  int code = 0;
  if (refreshAccessToken(&code)) return true;
  if (code == 400 || code == 401) {   // the login was revoked or expired: ask the user to connect again
    g_refreshToken = "";
    g_accessToken = "";
    Settings::clearSpotifyLogin();
    setSpotify(SP_NEEDS_LOGIN, "Spotify login expired. Please connect again.");
    return false;
  }
  bool showing;
  lock(); showing = g_statusMode || !g_track.active; unlock();
  if (showing) {
    setStatus(code > 0 ? "Spotify login failed. Retrying..." : "Can't reach Spotify. Retrying...");
    lock(); g_statusMode = true; unlock();
  }
  return false;
}

// Turn the one-time code from the login redirect into tokens.
static void exchangeAuthCode() {
  String code, verifier, cid;
  lock();
  code = g_authCode;
  verifier = g_pkceVerifier;
  cid = g_clientId;
  g_authPending = false;
  g_authCode = "";
  unlock();
  LOG.println("Spotify: finishing login");
  JsonDocument doc;
  String err;
  int http = spotifyTokenRequest(String("grant_type=authorization_code&code=") + urlEncode(code) +
                                 "&redirect_uri=" + urlEncode(SPOTIFY_REDIRECT_URI) + "&client_id=" + urlEncode(cid) +
                                 "&code_verifier=" + urlEncode(verifier), doc, err);
  if (http != 200 || !doc["refresh_token"].is<const char*>()) {
    setSpotify(SP_ERROR, http > 0 ? "Spotify refused the login (" + err + "). Please try again."
                                  : "Couldn't reach Spotify (" + err + "). Please try again.");
    return;
  }
  g_refreshToken = "";
  storeTokens(doc);

  // who is this? (just for the web page)
  String user;
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient h;
    h.useHTTP10(true);
    h.setTimeout(8000);
    if (h.begin(client, "https://api.spotify.com/v1/me")) {
      h.addHeader("Authorization", String("Bearer ") + g_accessToken);
      if (h.GET() == 200) {
        JsonDocument f, me;
        f["display_name"] = true;
        f["id"] = true;
        if (!deserializeJson(me, h.getStream(), DeserializationOption::Filter(f)))
          user = me["display_name"] | (me["id"] | "");
      }
      h.end();
    }
  }
  Settings::setSpotifyUser(user);
  lock(); g_spUser = user; unlock();
  setSpotify(SP_CONNECTED, "");
  LOG.printf("Spotify: connected as %s\n", user.c_str());
  lock(); g_statusMode = true; strlcpy(g_status, "Talking to Spotify...", sizeof(g_status)); unlock();
  g_forcePoll = true;
}

// --- LRCLIB ---
static int lrclibGet(const String& url, JsonDocument& doc, JsonDocument& filter) {
  int bytes = 0;
  int code = httpGetToFile(url, "/lrc.json", &bytes, "lyrics");
  if (code == 200) {
    File f = LittleFS.open("/lrc.json", "r");
    DeserializationError err = f ? deserializeJson(doc, f, DeserializationOption::Filter(filter))
                                 : DeserializationError(DeserializationError::InvalidInput);
    if (f) f.close();
    if (err) { LOG.printf("lyrics: json %s (%d bytes)\n", err.c_str(), bytes); code = -2; }
  }
  return code;
}

static LyricsState fetchLyrics(const String& artist, const String& title, const String& album,
                               uint32_t durMs, std::vector<LyricLine>& out) {
  JsonDocument filter;
  filter["syncedLyrics"] = true;
  filter["instrumental"] = true;
  JsonDocument doc;
  bool instrumental = false;

  // 1) exact match
  String url = String("https://lrclib.net/api/get?artist_name=") + urlEncode(artist) +
               "&track_name=" + urlEncode(title) + "&album_name=" + urlEncode(album) +
               "&duration=" + String((durMs + 500) / 1000);
  if (lrclibGet(url, doc, filter) == 200) {
    const char* s = doc["syncedLyrics"];
    if (s && *s) { parseLRC(s, out); if (!out.empty()) return LYR_SYNCED; }
    instrumental = doc["instrumental"] | false;
  }
  if (instrumental) return LYR_INSTRUMENTAL;

  // 2) search, pick results whose duration matches, fetch them one by one
  JsonDocument sf;
  sf[0]["id"] = true;
  sf[0]["duration"] = true;
  JsonDocument results;
  url = String("https://lrclib.net/api/search?track_name=") + urlEncode(cleanTitle(title)) +
        "&artist_name=" + urlEncode(artist);
  if (lrclibGet(url, results, sf) != 200) return LYR_NOT_FOUND;
  int tries = 0;
  for (JsonObject r : results.as<JsonArray>()) {
    float d = r["duration"] | 0.0f;
    if (fabsf(d - durMs / 1000.0f) > 4.0f) continue;
    long id = r["id"] | 0L;
    if (!id) continue;
    JsonDocument d2;
    if (lrclibGet(String("https://lrclib.net/api/get/") + String(id), d2, filter) == 200) {
      const char* s = d2["syncedLyrics"];
      if (s && *s) { parseLRC(s, out); if (!out.empty()) return LYR_SYNCED; }
    }
    if (++tries >= 3) break;
  }
  return LYR_NOT_FOUND;
}

// --- Spotify polling ---
static void setNothingPlaying() {
  lock();
  if (g_track.active) g_lastActiveMs = millis();
  g_track.active = false;
  g_track.playing = false;
  g_statusMode = false;
  unlock();
}

static void pollSpotify(uint32_t& backoffUntil) {
  if (!ensureToken()) { backoffUntil = millis() + 15000; return; }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(8000);
  // market=from_token drops the long "available_markets" lists: ~4x smaller answer
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing?market=from_token")) return;
  const char* keys[] = { "Retry-After" };
  http.collectHeaders(keys, 1);
  http.addHeader("Authorization", String("Bearer ") + g_accessToken);
  uint32_t t0 = millis();
  int code = http.GET();
  uint32_t t1 = millis();

  if (code == 204) { http.end(); setNothingPlaying(); return; }
  if (code == 401) { http.end(); g_accessToken = ""; g_forcePoll = true; return; }
  if (code == 429) {
    int ra = http.header("Retry-After").toInt();
    http.end();
    backoffUntil = millis() + (uint32_t)max(ra, 5) * 1000UL;
    LOG.printf("rate limited, waiting %ds\n", max(ra, 5));
    return;
  }
  if (code != 200) {
    char err[100] = "";
    client.lastError(err, sizeof(err));
    http.end();
    LOG.printf("poll: HTTP %d %s (heap %u, largest %u)\n", code, err, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    backoffUntil = millis() + 5000;
    return;
  }

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["album"]["images"][0]["url"] = true;
  filter["item"]["album"]["images"][0]["width"] = true;
  // getString() reads the whole body correctly whether it is chunked or not
  String body = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    LOG.printf("spotify json: %s (%u bytes, heap %u)\n", err.c_str(), (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
    backoffUntil = millis() + 1000;
    return;
  }

  JsonObject item = doc["item"];
  if (item.isNull()) { setNothingPlaying(); return; }   // ad, podcast, etc.

  bool playing = doc["is_playing"] | false;
  uint32_t progress = (doc["progress_ms"] | 0UL) + (t1 - t0) / 2;
  String title = sanitize(item["name"] | "");
  String id = item["id"] | "";
  if (id.length() == 0) id = title;  // local files have no id
  uint32_t duration = item["duration_ms"] | 0UL;
  String primaryArtist, artists;
  int na = 0;
  for (JsonObject a : item["artists"].as<JsonArray>()) {
    const char* n = a["name"] | "";
    if (na == 0) primaryArtist = n;
    if (na < 3) { if (na) artists += ", "; artists += n; }
    na++;
  }
  artists = sanitize(artists.c_str());
  String album = item["album"]["name"] | "";

  // smallest image that is still >= 128 px wide (normally the 300 px one)
  String artUrl;
  int bestW = 1 << 30, largestW = 0;
  String largestUrl;
  for (JsonObject img : item["album"]["images"].as<JsonArray>()) {
    int w = img["width"] | 0;
    const char* u = img["url"] | "";
    if (w >= ART && w < bestW) { bestW = w; artUrl = u; }
    if (w > largestW) { largestW = w; largestUrl = u; }
  }
  if (artUrl.length() == 0) artUrl = largestUrl;

  bool changed;
  lock();
  changed = strcmp(g_track.id, id.c_str()) != 0;
  if (changed) {
    strlcpy(g_track.id, id.c_str(), sizeof(g_track.id));
    strlcpy(g_track.title, title.c_str(), sizeof(g_track.title));
    strlcpy(g_track.artist, artists.c_str(), sizeof(g_track.artist));
    g_track.durationMs = duration;
    g_track.progressMs = progress;
    g_track.stampMs = t1;
    g_lyrics.clear();
    g_lyrState = LYR_LOADING;
    g_lyrVersion++;
  } else {
    // Keep the local clock smooth: only correct it if it drifted noticeably.
    uint32_t local = positionOf(g_track, t1);
    int32_t diff = (int32_t)(progress - local);
    if (!(g_track.active && g_track.playing && playing && abs(diff) < 350)) {
      g_track.progressMs = progress;
      g_track.stampMs = t1;
    }
  }
  g_track.playing = playing;
  g_track.active = true;
  g_statusMode = false;
  g_lastActiveMs = millis();
  unlock();

  if (!changed) return;
  LOG.printf("Now playing: %s - %s  (detected %.1fs into the song)\n", artists.c_str(), title.c_str(),
                progress / 1000.0f);

  // album art -> back buffer, then swap
  fetchArt(artUrl);

  // lyrics
  std::vector<LyricLine> lines;
  LyricsState st = fetchLyrics(primaryArtist, title, album, duration, lines);
  LOG.printf("Lyrics: %s (%u lines)\n", st == LYR_SYNCED ? "synced" : "not found", (unsigned)lines.size());
  lock();
  if (strcmp(g_track.id, id.c_str()) == 0) {
    std::swap(g_lyrics, lines);
    g_lyrState = st;
    g_lyrVersion++;
  }
  unlock();
}

// ---------------------------------------------------------------- updates --
static void checkUpdate() {
  lock(); g_updState = UPD_CHECKING; unlock();
  int bytes = 0;
  int code = httpGetToFile(String("https://api.github.com/repos/") + GITHUB_REPO + "/releases/latest", "/rel.json",
                           &bytes, "update");
  String tag, url, msg;
  if (code == 200) {
    JsonDocument filter, doc;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    File f = LittleFS.open("/rel.json", "r");
    if (f && !deserializeJson(doc, f, DeserializationOption::Filter(filter))) {
      tag = doc["tag_name"] | "";
      for (JsonObject a : doc["assets"].as<JsonArray>())
        if (String(a["name"] | "") == "firmware.bin") url = a["browser_download_url"] | "";
    }
    if (f) f.close();
    if (tag.length() == 0) msg = "Couldn't read the latest release.";
    else if (url.length() == 0) msg = "Release " + tag + " has no firmware.bin (still building?).";
  } else if (code == 404) {
    msg = "No releases published yet.";
  } else {
    msg = "Couldn't check for updates (HTTP " + String(code) + ").";
  }
  lock();
  g_updLatest = tag;
  g_updUrl = url;
  g_updMsg = msg;
  if (msg.length()) g_updState = UPD_ERROR;
  else g_updState = (tag == FW_VERSION) ? UPD_LATEST : UPD_AVAILABLE;
  unlock();
  LOG.printf("Update check: installed %s, latest %s %s\n", FW_VERSION, tag.length() ? tag.c_str() : "?", msg.c_str());
}

static void installUpdate() {
  String url, tag;
  lock();
  url = g_updUrl;
  tag = g_updLatest;
  if (url.length()) { g_updState = UPD_INSTALLING; g_updProgress = 0; }
  unlock();
  if (url.length() == 0) return;
  LOG.printf("Installing update %s ...\n", tag.c_str());
  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int cur, int total) { g_updProgress = total > 0 ? (int)((int64_t)cur * 100 / total) : 0; });
  t_httpUpdate_return r = httpUpdate.update(client, url);
  if (r == HTTP_UPDATE_OK) {
    lock(); g_updState = UPD_DONE; g_updProgress = 100; unlock();
    LOG.printf("Update %s installed, restarting\n", tag.c_str());
    delay(2500);
    ESP.restart();
  }
  String err = httpUpdate.getLastErrorString();
  lock(); g_updState = UPD_ERROR; g_updMsg = "Update failed: " + err; unlock();
  LOG.printf("Update failed: %s\n", err.c_str());
}

// ------------------------------------------------------------------ Wi-Fi --
static void rebuildWifiMulti() {
  auto list = Settings::wifiList();
  delete g_wifiMulti;
  g_wifiMulti = new WiFiMulti();
  std::vector<String> names;
  for (auto& w : list) {
    g_wifiMulti->addAP(w.ssid.c_str(), w.pass.c_str());
    names.push_back(w.ssid);
  }
  lock(); g_savedSsids = names; unlock();
  LOG.printf("Saved Wi-Fi networks: %u\n", (unsigned)names.size());
}

static void startHotspot() {
  if (g_hotspot) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_NAME);
  webSetCaptivePortal(true);
  g_hotspotOffAt = 0;
  g_hotspot = true;
  logMem("hotspot on");
  LOG.printf("Setup hotspot \"%s\" on, open http://%s\n", SETUP_AP_NAME, WiFi.softAPIP().toString().c_str());
}

static void stopHotspot() {
  if (!g_hotspot) return;
  webSetCaptivePortal(false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_hotspot = false;
  g_setupDone = false;
  LOG.println("Setup hotspot off");
  logMem("hotspot off");
}

static void onWifiConnected() {
  static bool timeSet = false;
  strlcpy(g_ipStr, WiFi.localIP().toString().c_str(), sizeof(g_ipStr));
  LOG.printf("WiFi OK: %s, IP %s, signal %d dBm\n", WiFi.SSID().c_str(), g_ipStr, (int)WiFi.RSSI());
  if (!timeSet) { configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com"); timeSet = true; }
  // Keep the hotspot a little longer only if a phone is on it (to show the "you're connected" page).
  if (g_hotspot) g_hotspotOffAt = millis() + (WiFi.softAPgetStationNum() > 0 ? 180000 : 1);
  g_wifiUp = true;
  logMem("wifi up");
  setStatus("Talking to Spotify...");
  g_forcePoll = true;
}

// The web page asked to join a network: try it, and save it only if it works.
static void handleWifiTry() {
  String ssid, pass;
  lock();
  ssid = g_trySsid;
  pass = g_tryPass;
  g_tryPass = "";
  g_tryPending = false;
  g_tryState = TRY_TRYING;
  unlock();

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() != ssid) {
    // Already online elsewhere: switching would cut off the web page, so just save it.
    Settings::addWifi(ssid, pass);
    rebuildWifiMulti();
    lock(); g_tryState = TRY_OK; g_tryMsg = ""; unlock();
    LOG.printf("WiFi: saved %s (used when it's the strongest known network)\n", ssid.c_str());
    return;
  }
  LOG.printf("WiFi: trying %s\n", ssid.c_str());
  setStatus("Joining Wi-Fi...");
  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wl_status_t st = WL_IDLE_STATUS;
  for (int i = 0; i < 75; i++) {   // up to 15 s
    st = WiFi.status();
    if (st == WL_CONNECTED || st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) break;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (st == WL_CONNECTED) {
    Settings::addWifi(ssid, pass);
    rebuildWifiMulti();
    lock(); g_tryState = TRY_OK; g_tryMsg = ""; unlock();
    onWifiConnected();
    return;
  }
  String msg = st == WL_NO_SSID_AVAIL ? "Network not found. Is it 2.4 GHz and in range?"
                                      : "Couldn't connect. Is the password right?";
  WiFi.disconnect(false);
  lock(); g_tryState = TRY_FAIL; g_tryMsg = msg; unlock();
  LOG.printf("WiFi: %s failed (%d): %s\n", ssid.c_str(), (int)st, msg.c_str());
}

// --------------------------------------------------------------- the task --
static void netTask(void*) {
  uint32_t lastPoll = 0, backoffUntil = 0, lastWifiTry = 0, lastUpdCheck = 0;
  int wifiFails = 0;
  rebuildWifiMulti();
  for (;;) {
    uint32_t now = millis();

    // ---- Wi-Fi ----
    if (g_tryPending) { handleWifiTry(); continue; }
    if (g_wifiListChanged) { g_wifiListChanged = false; rebuildWifiMulti(); }
    if (WiFi.status() != WL_CONNECTED) {
      if (g_wifiUp) { g_wifiUp = false; LOG.println("WiFi lost"); lastWifiTry = 0; }
      size_t known;
      lock(); known = g_savedSsids.size(); unlock();
      if (known == 0) {                        // nothing saved yet: setup mode
        startHotspot();
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      // With the hotspot on, retry less often so the web page's own scans work.
      uint32_t gap = g_hotspot ? 30000 : 3000;
      if ((lastWifiTry == 0 || now - lastWifiTry > gap) && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        lastWifiTry = now;
        if (!g_hotspot) {
          setStatus(wifiFails ? "Looking for WiFi..." : "Connecting to WiFi...");
          lock(); g_statusMode = true; unlock();
        }
        if (g_wifiMulti->run(10000) == WL_CONNECTED) {
          wifiFails = 0;
          onWifiConnected();
        } else if (++wifiFails >= 4) {
          startHotspot();                      // ~1 minute without Wi-Fi: let the user fix it from a phone
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!g_wifiUp) { wifiFails = 0; onWifiConnected(); }
    if (g_hotspot && (g_setupDone || (g_hotspotOffAt && (int32_t)(now - g_hotspotOffAt) > 0) ||
                      (g_hotspotOffAt && WiFi.softAPgetStationNum() == 0)))
      stopHotspot();

    // ---- requests from the web page ----
    if (g_authPending) { exchangeAuthCode(); continue; }
    if (g_updInstallReq) { g_updInstallReq = false; installUpdate(); continue; }
    if (!g_hotspot && (g_updCheckReq || (lastUpdCheck == 0 && now > 60000) || now - lastUpdCheck > 12UL * 3600 * 1000)) {
      g_updCheckReq = false;
      lastUpdCheck = now ? now : 1;
      checkUpdate();
      continue;
    }

    // ---- Spotify ----
    SpState sp;
    lock(); sp = g_spState; unlock();
    if (sp != SP_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    if ((int32_t)(now - backoffUntil) < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    bool playing;
    lock(); playing = g_track.active && g_track.playing; unlock();
    uint32_t interval = playing ? POLL_INTERVAL_MS : POLL_IDLE_MS;
    if (!g_forcePoll && now - lastPoll < interval) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    g_forcePoll = false;
    lastPoll = now;
    pollSpotify(backoffUntil);
  }
}

// ============================================================================
//                       WEB PAGE  <->  DEVICE  (device.h)
// ============================================================================
static String randomString(size_t n) {
  static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  String s;
  for (size_t i = 0; i < n; i++) s += chars[esp_random() % 66];
  return s;
}

static String base64url(const uint8_t* d, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  String o;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    o += t[(v >> 18) & 63];
    o += t[(v >> 12) & 63];
    if (i + 1 < n) o += t[(v >> 6) & 63];
    if (i + 2 < n) o += t[v & 63];
  }
  return o;
}

void deviceStatus(JsonDocument& d) {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  d["version"] = FW_VERSION;
  d["uptime"] = now / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["host"] = String(DEVICE_HOSTNAME) + ".local";
  d["hotspot"] = (bool)g_hotspot;
  lock();
  JsonObject w = d["wifi"].to<JsonObject>();
  w["connected"] = up;
  if (up) {
    w["ssid"] = WiFi.SSID();
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
  JsonArray saved = w["saved"].to<JsonArray>();
  for (auto& n : g_savedSsids) saved.add(n);
  static const char* tryNames[] = { "idle", "trying", "ok", "fail" };
  w["tryState"] = tryNames[g_tryState];
  w["trySsid"] = g_trySsid;
  w["tryMsg"] = g_tryMsg;

  JsonObject sp = d["spotify"].to<JsonObject>();
  static const char* spNames[] = { "needs_client", "needs_login", "connecting", "connected", "error" };
  sp["state"] = spNames[g_spState];
  sp["clientId"] = g_clientId;
  sp["user"] = g_spUser;
  sp["msg"] = g_spMsg;
  sp["redirectUri"] = SPOTIFY_REDIRECT_URI;

  JsonObject np = d["now"].to<JsonObject>();
  np["active"] = g_track.active && g_spState == SP_CONNECTED;
  np["playing"] = g_track.playing;
  np["title"] = g_track.title;
  np["artist"] = g_track.artist;
  np["pos"] = positionOf(g_track, now);
  np["dur"] = g_track.durationMs;
  static const char* lyrNames[] = { "none", "loading", "synced", "not found", "instrumental" };
  np["lyrics"] = lyrNames[g_lyrState];
  np["art"] = g_artVersion;

  JsonObject u = d["update"].to<JsonObject>();
  static const char* updNames[] = { "idle", "checking", "latest", "available", "installing", "error", "done" };
  u["state"] = updNames[g_updState];
  u["latest"] = g_updLatest;
  u["progress"] = (int)g_updProgress;
  u["msg"] = g_updMsg;
  unlock();
}

void deviceTryWifi(const String& ssid, const String& pass) {
  lock();
  g_trySsid = ssid;
  g_tryPass = pass;
  g_tryState = TRY_TRYING;
  g_tryMsg = "";
  unlock();
  g_tryPending = true;
}

void deviceRemoveWifi(const String& ssid) {
  if (Settings::removeWifi(ssid)) LOG.printf("WiFi: removed %s\n", ssid.c_str());
  g_wifiListChanged = true;
}

void deviceSetupDone() { g_setupDone = true; }
bool deviceHotspotActive() { return g_hotspot; }

void deviceSetSpotifyClientId(const String& id) {
  Settings::setSpotifyClientId(id);
  lock();
  bool changed = g_clientId != id;
  g_clientId = id;
  if (changed || g_spState == SP_NEEDS_CLIENT) { g_spState = SP_NEEDS_LOGIN; g_spMsg = ""; }
  unlock();
  LOG.println("Spotify: Client ID saved");
}

String deviceSpotifyLoginUrl(const String& deviceBaseUrl) {
  String cid;
  lock(); cid = g_clientId; unlock();
  if (cid.length() == 0) return "";
  String verifier = randomString(64);
  uint8_t hash[32];
  SHA256Builder sha;
  sha.begin();
  sha.add((const uint8_t*)verifier.c_str(), verifier.length());
  sha.calculate();
  sha.getBytes(hash);
  String nonce = randomString(16);
  lock();
  g_pkceVerifier = verifier;
  g_pkceNonce = nonce;
  unlock();
  LOG.println("Spotify: login started from the web page");
  return String("https://accounts.spotify.com/authorize?response_type=code&client_id=") + urlEncode(cid) +
         "&redirect_uri=" + urlEncode(SPOTIFY_REDIRECT_URI) +
         "&scope=" + urlEncode("user-read-currently-playing user-read-playback-state") +
         "&code_challenge_method=S256&code_challenge=" + base64url(hash, 32) +
         "&state=" + urlEncode(nonce + "|" + deviceBaseUrl);
}

bool deviceSpotifyCallback(const String& code, const String& state, const String& error, String& msg) {
  String nonce;
  lock(); nonce = g_pkceNonce; unlock();
  if (error.length()) msg = error == "access_denied" ? "The Spotify login was cancelled." : "Spotify said: " + error;
  else if (nonce.length() == 0 || !state.startsWith(nonce + "|")) msg = "This login link has expired. Click Connect Spotify again.";
  else if (code.length() == 0) msg = "Spotify didn't send a login code. Please try again.";
  if (msg.length()) { setSpotify(SP_ERROR, msg); return false; }
  lock();
  g_authCode = code;
  g_pkceNonce = "";
  g_spState = SP_CONNECTING;
  g_spMsg = "";
  unlock();
  g_authPending = true;
  return true;
}

void deviceSpotifyLogout() {
  Settings::clearSpotifyLogin();
  lock();
  g_spUser = "";
  g_spState = g_clientId.length() ? SP_NEEDS_LOGIN : SP_NEEDS_CLIENT;
  g_spMsg = "";
  g_track.active = false;
  unlock();
  LOG.println("Spotify: disconnected");
}

void deviceCheckUpdate() { g_updCheckReq = true; }
void deviceInstallUpdate() { g_updInstallReq = true; }

void deviceRestart() {
  LOG.println("Restarting...");
  delay(200);
  ESP.restart();
}

void deviceReset(const String& what) {
  LOG.printf("Reset requested: %s\n", what.c_str());
  if (what == "wifi") Settings::clearWifi();
  else if (what == "spotify") Settings::clearSpotifyLogin();
  else Settings::factoryReset();
  deviceRestart();
}

// ============================================================================
//                                 DRAWING
// ============================================================================
static uint16_t* fb() { return canvas.getBuffer(); }

static void fillRectD(int x, int y, int w, int h, uint16_t c) {
  for (int yy = max(0, y); yy < min(H, y + h); yy++)
    for (int xx = max(0, x); xx < min(W, x + w); xx++) fb()[yy * W + xx] = c;
}
static void darkenRect(int x, int y, int w, int h, int keep) {
  for (int yy = max(0, y); yy < min(H, y + h); yy++)
    for (int xx = max(0, x); xx < min(W, x + w); xx++) {
      uint16_t& p = fb()[yy * W + xx];
      p = scale565(p, keep);
    }
}
static void tintRect(int x, int y, int w, int h, uint16_t c, int alpha) {
  for (int yy = max(0, y); yy < min(H, y + h); yy++)
    for (int xx = max(0, x); xx < min(W, x + w); xx++) {
      uint16_t& p = fb()[yy * W + xx];
      p = mix(p, c, alpha);
    }
}

// Blurred album-colour background: bilinear blend of the 4x4 colour grid,
// drifting slowly, with ordered dithering so dark gradients don't band.
static void drawBackground(const ArtSet& a, uint32_t now) {
  static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
  float ph = now * 0.00025f;
  int driftX = (int)(sinf(ph) * 60), driftY = (int)(cosf(ph * 0.77f) * 60);
  const int maxF = (GRID - 1) * 256 - 1;
  int rowR[GRID], rowG[GRID], rowB[GRID];
  uint16_t* buf = fb();
  for (int y = 0; y < H; y++) {
    int fy = y * (GRID - 1) * 256 / (H - 1) + driftY;
    fy = fy < 0 ? 0 : (fy > maxF ? maxF : fy);
    int iy = fy >> 8, ty = fy & 255;
    for (int c = 0; c < GRID; c++) {
      const RGB& p = a.grid[iy * GRID + c];
      const RGB& q = a.grid[(iy + 1) * GRID + c];
      rowR[c] = p.r * (256 - ty) + q.r * ty;
      rowG[c] = p.g * (256 - ty) + q.g * ty;
      rowB[c] = p.b * (256 - ty) + q.b * ty;
    }
    uint16_t* line = buf + y * W;
    for (int x = 0; x < W; x++) {
      int fx = x * (GRID - 1) * 256 / (W - 1) + driftX;
      fx = fx < 0 ? 0 : (fx > maxF ? maxF : fx);
      int ix = fx >> 8, tx = fx & 255;
      int d = bayer[y & 3][x & 3];
      int r = ((rowR[ix] * (256 - tx) + rowR[ix + 1] * tx) >> 16) + (d >> 1);
      int g = ((rowG[ix] * (256 - tx) + rowG[ix + 1] * tx) >> 16) + (d >> 2);
      int b = ((rowB[ix] * (256 - tx) + rowB[ix + 1] * tx) >> 16) + (d >> 1);
      line[x] = rgb565(r, g, b);
    }
  }
}

static void drawThumb(int x0, int y0, const ArtSet& a, bool paused) {
  darkenRect(x0 + 1, y0 + 2, THUMB, THUMB, 120);  // soft shadow
  const int R = 4;
  for (int y = 0; y < THUMB; y++)
    for (int x = 0; x < THUMB; x++) {
      int cx = x < R ? R - 1 - x : (x > THUMB - 1 - R ? x - (THUMB - R) : -1);
      int cy = y < R ? R - 1 - y : (y > THUMB - 1 - R ? y - (THUMB - R) : -1);
      if (cx >= 0 && cy >= 0 && cx * cx + cy * cy >= R * R) continue;  // rounded corners
      int xx = x0 + x, yy = y0 + y;
      if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      uint16_t c = a.thumb[y * THUMB + x];
      fb()[yy * W + xx] = paused ? scale565(c, 110) : c;
    }
  if (paused) {
    fillRectD(x0 + 12, y0 + 11, 4, 14, WHITE);
    fillRectD(x0 + 20, y0 + 11, 4, 14, WHITE);
  }
}

static void drawProgress(int x, int y, int w, float frac, uint16_t accent) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  int fw = (int)(frac * w + 0.5f);
  tintRect(x, y, w, 2, WHITE, 55);
  fillRectD(x, y, fw, 2, accent);
  canvas.fillCircle(x + fw, y + 1, 2, WHITE);
}

static void drawMarquee(int x, int baseline, int w, const char* s, const uint8_t* font, uint16_t col, uint32_t now) {
  u8f.setFont(font);
  u8f.setForegroundColor(col);
  int tw = u8f.getUTF8Width(s);
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  canvas.setClip(x, baseline - asc - 1, w, asc - desc + 3);
  if (tw <= w) {
    u8f.drawUTF8(x, baseline, s);
  } else {
    const int gap = 28, pause = 2000, msPerPx = 35;
    int cycle = tw + gap;
    uint32_t period = pause + (uint32_t)cycle * msPerPx;
    uint32_t m = now % period;
    int off = m < (uint32_t)pause ? 0 : (int)((m - pause) / msPerPx);
    u8f.drawUTF8(x - off, baseline, s);
    u8f.drawUTF8(x - off + cycle, baseline, s);
  }
  canvas.resetClip();
}

static void drawCentered(int baseline, const char* s, const uint8_t* font, uint16_t col) {
  u8f.setFont(font);
  u8f.setForegroundColor(col);
  int tw = u8f.getUTF8Width(s);
  u8f.drawUTF8((W - tw) / 2, baseline, s);
}

// ---------------- lyrics layout (owned by the render loop) ----------------
struct LineLayout { int16_t y, h; };
std::vector<LineLayout> g_layout;
uint32_t g_layoutVer = 0xFFFFFFFF;
int g_rowH = 14, g_lyrAsc = 10, g_spaceW = 3;
float g_scrollY = 0;
bool g_snapScroll = true;
bool g_preferArt = false;

static void ensureLayout() {
  if (g_layoutVer == g_lyrVersion) return;
  g_layoutVer = g_lyrVersion;
  u8f.setFont(FONT_LYRIC);
  g_lyrAsc = u8f.getFontAscent();
  g_rowH = g_lyrAsc - u8f.getFontDescent() + 2;
  g_spaceW = u8f.getUTF8Width("a a") - u8f.getUTF8Width("aa");
  g_layout.clear();
  g_layout.reserve(g_lyrics.size());
  Row rows[MAX_ROWS];
  int y = 0;
  for (auto& l : g_lyrics) {
    int n = l.text.length() ? wrapText(l.text.c_str(), LYR_W, rows, MAX_ROWS) : 0;
    if (n < 1) n = 1;
    int h = n * g_rowH;
    g_layout.push_back({ (int16_t)y, (int16_t)h });
    y += h + LINE_GAP;
  }
  g_snapScroll = true;
}

static int findLine(uint32_t pos) {
  int lo = 0, hi = (int)g_lyrics.size();
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (g_lyrics[mid].t <= pos) lo = mid + 1; else hi = mid;
  }
  return lo - 1;
}

static void formatTime(char* out, uint32_t ms);

static void drawLyricsView(const TrackInfo& t, const ArtSet& a, uint32_t pos, uint32_t now, float dt) {
  drawBackground(a, now);
  ensureLayout();

  int idx = findLine(pos + LYRICS_LEAD_MS);
  float target = idx >= 0 ? g_layout[idx].y + g_layout[idx].h / 2.0f : -(float)g_rowH;
  if (g_snapScroll) { g_scrollY = target; g_snapScroll = false; }
  float diff = target - g_scrollY;
  if (fabsf(diff) > 220) g_scrollY = target;              // big seek: jump
  else g_scrollY += diff * (1.0f - expf(-dt / 110.0f));   // smooth glide

  uint16_t baseDim = scale565(mix(WHITE, a.accent, 110), 165);
  canvas.setClip(0, LY_TOP, W, H - LY_TOP);
  u8f.setFont(FONT_LYRIC);
  u8f.setFontMode(1);
  Row rows[MAX_ROWS];
  for (size_t i = 0; i < g_lyrics.size(); i++) {
    const LineLayout& L = g_layout[i];
    int top = LY_CENTER + (int)lroundf(L.y - g_scrollY);  // top of this line's block
    if (top > H) break;
    if (top + L.h < LY_TOP) continue;
    float centre = top + L.h / 2.0f;
    float d = fabsf(centre - LY_CENTER);
    float hl = 1.0f - d / (g_rowH * 1.4f);
    if (hl < 0) hl = 0;
    float fade = (d - g_rowH) / (float)(H - LY_CENTER);
    fade = fade < 0 ? 0 : (fade > 1 ? 1 : fade);
    uint16_t dim = scale565(baseDim, 256 - (int)(fade * 190));
    uint16_t col = mix(dim, WHITE, (int)(hl * 255));

    const String& txt = g_lyrics[i].text;
    if (txt.length() == 0) {
      // instrumental gap: three dots that fill up until the next line
      float p = 0;
      if ((int)i == idx) {
        uint32_t t0 = g_lyrics[i].t, t1 = (i + 1 < g_lyrics.size()) ? g_lyrics[i + 1].t : t.durationMs;
        if (t1 > t0) p = (float)(pos + LYRICS_LEAD_MS - t0) / (float)(t1 - t0);
      }
      int cy = top + L.h / 2;
      for (int k = 0; k < 3; k++) {
        float lv = p * 3.0f - k;
        lv = lv < 0 ? 0 : (lv > 1 ? 1 : lv);
        uint16_t dc = (int)i == idx ? mix(dim, WHITE, (int)(lv * 255)) : dim;
        int r = ((int)i == idx && lv > 0.99f) ? 3 : 2;
        canvas.fillCircle(W / 2 - 10 + k * 10, cy, r, dc);
      }
      continue;
    }
    int n = wrapText(txt.c_str(), LYR_W, rows, MAX_ROWS);
    static char buf[256];

    if ((int)i != idx || !WORD_HIGHLIGHT) {
      u8f.setForegroundColor(col);
      for (int r = 0; r < n; r++) {
        int len = min((int)rows[r].len, 255);
        memcpy(buf, txt.c_str() + rows[r].start, len);
        buf[len] = 0;
        int tw = u8f.getUTF8Width(buf);
        u8f.drawUTF8((W - tw) / 2, top + r * g_rowH + g_lyrAsc + 1, buf);
      }
      continue;
    }

    // ---- current line: karaoke sweep, word by word ----
    // LRCLIB only times whole lines, so spread the words over the time the line
    // is probably sung (by length, capped by the gap to the next line).
    uint32_t t0 = g_lyrics[i].t;
    uint32_t tNext = (i + 1 < g_lyrics.size()) ? g_lyrics[i + 1].t : t.durationMs;
    uint32_t gap = tNext > t0 ? tNext - t0 : 1;
    uint32_t sing = max((uint32_t)900, (uint32_t)txt.length() * WORD_MS_PER_CHAR);
    if (sing > gap * 9 / 10) sing = gap * 9 / 10;
    if (sing < 1) sing = 1;
    uint32_t now2 = pos + LYRICS_LEAD_MS;
    float p = now2 > t0 ? (float)(now2 - t0) / (float)sing : 0.0f;
    if (p > 1) p = 1;

    int totalW = 0;  // weight = characters + 1 per word
    for (int r = 0; r < n; r++) {
      const char* s = txt.c_str() + rows[r].start;
      int len = rows[r].len, k = 0;
      while (k < len) {
        while (k < len && s[k] == ' ') k++;
        int e = k;
        while (e < len && s[e] != ' ') e++;
        if (e > k) totalW += (e - k) + 1;
        k = e;
      }
    }
    float cursor = p * totalW;
    // sung words: album accent colour (white if the cover is greyscale); unsung: dim grey
    RGB ac = unpack(a.accent);
    int acSat = max(ac.r, max(ac.g, ac.b)) - min(ac.r, min(ac.g, ac.b));
    uint16_t sungCol = acSat < 50 ? WHITE : a.accent;
    sungCol = mix(dim, sungCol, (int)(hl * 255));
    uint16_t todoCol = scale565(mix(dim, WHITE, (int)(hl * 255)), 105);

    int acc = 0;
    for (int r = 0; r < n; r++) {
      const char* s = txt.c_str() + rows[r].start;
      int len = min((int)rows[r].len, 255);
      memcpy(buf, s, len);
      buf[len] = 0;
      int x = (W - u8f.getUTF8Width(buf)) / 2;
      int baseline = top + r * g_rowH + g_lyrAsc + 1;
      int k = 0;
      while (k < len) {
        int sp = k;
        while (k < len && s[k] == ' ') k++;
        x += (k - sp) * g_spaceW;
        int e = k;
        while (e < len && s[e] != ' ') e++;
        if (e == k) break;
        int wl = e - k;
        memcpy(buf, s + k, wl);
        buf[wl] = 0;
        float f = (cursor - acc) / (0.35f * (wl + 1));   // snap on in the first third of the word
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        u8f.setForegroundColor(mix(todoCol, sungCol, (int)(f * 255)));
        u8f.drawUTF8(x, baseline, buf);
        x += u8f.getUTF8Width(buf);
        acc += wl + 1;
        k = e;
      }
    }
  }
  canvas.resetClip();

  // header
  drawThumb(5, 4, a, !t.playing);
  drawMarquee(47, 15, W - 47 - 4, t.title, FONT_TITLE, WHITE, now);
  drawMarquee(47, 27, W - 47 - 4, t.artist, FONT_SUB, mix(WHITE, a.accent, 120), now);
  drawProgress(47, 34, W - 47 - 6, t.durationMs ? (float)pos / t.durationMs : 0, a.accent);
}

static void formatTime(char* out, uint32_t ms) {
  uint32_t s = ms / 1000;
  sprintf(out, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void drawNoteIcon(int cx, int cy, uint16_t c) {
  canvas.fillCircle(cx - 6, cy + 10, 6, c);
  canvas.fillRect(cx - 1, cy - 14, 3, 25, c);
  canvas.fillTriangle(cx + 1, cy - 14, cx + 12, cy - 8, cx + 1, cy - 4, c);
}

static void drawArtView(const TrackInfo& t, const ArtSet& a, uint32_t pos, uint32_t now) {
  bool loaded = false;
  if (a.has) {
    File f = LittleFS.open("/art.raw", "r");
    loaded = f && f.read((uint8_t*)fb(), W * H * 2) == (size_t)(W * H * 2);
    if (f) f.close();
  }
  if (!loaded) { drawBackground(a, now); drawNoteIcon(64, 44, scale565(a.accent, 200)); }

  // smooth dark gradient at the bottom so the text is readable on any cover
  for (int y = 58; y < H; y++) {
    float f = (y - 58) / (float)(H - 58);
    f = f * f * (3 - 2 * f);              // smoothstep
    int keep = 256 - (int)(f * 205);
    for (int x = 0; x < W; x++) {
      uint16_t& p = fb()[y * W + x];
      p = scale565(p, keep);
    }
  }
  drawMarquee(6, 106, W - 12, t.title, FONT_TITLE, WHITE, now);
  drawMarquee(6, 117, W - 12, t.artist, FONT_SUB, mix(WHITE, a.accent, 120), now);
  drawProgress(6, 122, W - 12, t.durationMs ? (float)pos / t.durationMs : 0, a.accent);

  if (!t.playing) {  // pause badge
    canvas.fillCircle(W - 14, 13, 9, rgb565(20, 20, 20));
    fillRectD(W - 18, 8, 3, 10, WHITE);
    fillRectD(W - 13, 8, 3, 10, WHITE);
  }
  if (!g_preferArt && g_lyrState == LYR_LOADING) {  // tiny "fetching lyrics" spinner
    for (int k = 0; k < 3; k++) {
      int lit = (now / 200) % 3 == (uint32_t)k;
      canvas.fillCircle(8 + k * 6, 8, lit ? 2 : 1, lit ? WHITE : 0x8410);
    }
  }
}

static void drawIdle(const ArtSet& a, uint32_t now) {
  drawBackground(a, now);
  darkenRect(0, 0, W, H, 150);
  struct tm tmv;
  if (getLocalTime(&tmv, 0) && tmv.tm_year > 120) {
    char buf[8];
    int hr = tmv.tm_hour;
    if (!CLOCK_24H) { hr %= 12; if (hr == 0) hr = 12; }
    sprintf(buf, CLOCK_24H ? "%02d:%02d" : "%d:%02d", hr, tmv.tm_min);
    drawCentered(64, buf, FONT_CLOCK, WHITE);
    static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char d[32];
    snprintf(d, sizeof(d), "%s, %d %s", days[tmv.tm_wday], tmv.tm_mday, months[tmv.tm_mon]);
    drawCentered(80, d, FONT_SUB, mix(WHITE, a.accent, 100));
  } else {
    drawNoteIcon(64, 50, scale565(a.accent, 220));
  }
  float pulse = 0.5f + 0.5f * sinf(now * 0.004f);
  canvas.fillCircle(W / 2 - 34, 105, 2, mix(rgb565(40, 40, 40), a.accent, (int)(pulse * 255)));
  drawCentered(108, "Nothing playing", FONT_SUB, 0x9CF3);
}

static const ArtSet& defaultArt();
static void drawStatus(const char* msg, uint32_t now) {
  const ArtSet& def = defaultArt();
  drawBackground(def, now);
  // spinner
  for (int k = 0; k < 8; k++) {
    float ang = k * (PI / 4);
    int head = (now / 90) % 8;
    int age = (head - k + 8) % 8;
    uint16_t c = mix(WHITE, rgb565(60, 40, 90), age * 32);
    canvas.fillCircle(64 + (int)(cosf(ang) * 13), 48 + (int)(sinf(ang) * 13), age == 0 ? 3 : 2, c);
  }
  drawCentered(82, "Now Playing", FONT_LYRIC, def.accent);
  // wrap the message
  u8f.setFont(FONT_SUB);
  u8f.setForegroundColor(0xDEFB);
  Row rows[3];
  int n = wrapText(msg, 118, rows, 3);
  for (int r = 0; r < n; r++) {
    char buf[100];
    int len = min((int)rows[r].len, 99);
    memcpy(buf, msg + rows[r].start, len);
    buf[len] = 0;
    u8f.drawUTF8((W - u8f.getUTF8Width(buf)) / 2, 98 + r * 11, buf);
  }
}

static void pushFrame() {
#if TFT_SWAP_RB
  uint16_t* b = fb();
  for (int i = 0; i < W * H; i++) {
    uint16_t c = b[i];
    b[i] = ((c & 0x1F) << 11) | (c & 0x07E0) | (c >> 11);
  }
#endif
  tft.drawRGBBitmap(0, 0, fb(), W, H);
}

// ---------------------------------------------------------- QR code + setup screens --
static char s_qrText[96] = "";
static uint8_t s_qrSize = 0;
static uint8_t s_qr[41 * 41];

static void qrCapture(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  if (n > 41) n = 0;
  s_qrSize = n;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) s_qr[y * n + x] = esp_qrcode_get_module(q, x, y) ? 1 : 0;
}

// Draw a QR code centred at the top. Returns the y just below it.
static int drawQR(const char* text, int top, int scale) {
  if (strcmp(text, s_qrText) != 0) {
    strlcpy(s_qrText, text, sizeof(s_qrText));
    s_qrSize = 0;
    esp_qrcode_config_t cfg = {};
    cfg.display_func = qrCapture;
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    esp_qrcode_generate(&cfg, text);
  }
  if (!s_qrSize) return top;
  int px = s_qrSize * scale, quiet = 2 * scale;
  int x0 = (W - px) / 2, y0 = top + quiet;
  fillRectD(x0 - quiet, top, px + 2 * quiet, px + 2 * quiet, WHITE);
  for (int y = 0; y < s_qrSize; y++)
    for (int x = 0; x < s_qrSize; x++)
      if (s_qr[y * s_qrSize + x]) fillRectD(x0 + x * scale, y0 + y * scale, scale, scale, BLACK);
  return top + px + 2 * quiet;
}

static const ArtSet& defaultArt() {
  static ArtSet def;
  static bool init = false;
  if (!init) { deriveDefault(def); init = true; }
  return def;
}

// Hotspot is on and there's no Wi-Fi: scan the QR to join the setup hotspot.
static void drawSetupScreen(uint32_t now, bool knownNetworks) {
  drawBackground(defaultArt(), now);
  char wifiQr[80];
  snprintf(wifiQr, sizeof(wifiQr), "WIFI:T:nopass;S:%s;;", SETUP_AP_NAME);
  int y = drawQR(wifiQr, 4, 2);
  drawCentered(y + 11, knownNetworks ? "Wi-Fi not found" : "Scan to set up", FONT_TITLE, WHITE);
  drawCentered(y + 22, "or join Wi-Fi:", FONT_SUB, 0xBDF7);
  drawCentered(y + 33, SETUP_AP_NAME, FONT_SUB, defaultArt().accent);
  drawCentered(y + 44, "open 192.168.4.1", FONT_SUB, 0xBDF7);
}

// On Wi-Fi but Spotify isn't connected: show where the web page is.
static void drawConnectSpotify(uint32_t now, const String& msg) {
  drawBackground(defaultArt(), now);
  char url[48];
  snprintf(url, sizeof(url), "http://%s/", g_ipStr);
  int y = drawQR(url, 4, 2);
  drawCentered(y + 11, msg.length() ? "Reconnect Spotify" : "Connect Spotify", FONT_TITLE, WHITE);
  drawCentered(y + 22, "open in a browser:", FONT_SUB, 0xBDF7);
  char host[32];
  snprintf(host, sizeof(host), "%s.local", DEVICE_HOSTNAME);
  drawCentered(y + 33, host, FONT_SUB, defaultArt().accent);
  drawCentered(y + 44, g_ipStr, FONT_SUB, 0xBDF7);
}

static void drawUpdating(uint32_t now) {
  drawBackground(defaultArt(), now);
  drawCentered(48, "Updating", FONT_LYRIC, WHITE);
  int p = g_updProgress;
  int x = 14, w = W - 28;
  tintRect(x, 62, w, 6, WHITE, 50);
  fillRectD(x, 62, w * p / 100, 6, defaultArt().accent);
  char buf[24];
  snprintf(buf, sizeof(buf), g_updState == UPD_DONE ? "Done! Restarting" : "%d%%", p);
  drawCentered(84, buf, FONT_SUB, 0xDEFB);
  drawCentered(108, "Don't unplug me", FONT_SUB, 0x9CF3);
}

static void drawResetHold(uint32_t held) {
  darkenRect(0, 0, W, H, 60);
  char buf[32];
  int left = (int)((10000 - (int)min(held, (uint32_t)10000)) + 999) / 1000;
  drawCentered(50, "Factory reset", FONT_LYRIC, WHITE);
  snprintf(buf, sizeof(buf), left > 0 ? "keep holding: %d" : "resetting...", left);
  drawCentered(70, buf, FONT_SUB, 0xFBEF);
  drawCentered(92, "release to cancel", FONT_SUB, 0x9CF3);
}

static void renderFrame(uint32_t now, float dt) {
  bool active;
  lock();
  TrackInfo t = g_track;
  const ArtSet& a = g_art[g_artFront];
  active = t.active;
  SpState sp = g_spState;
  bool wifiUp = g_wifiUp;
  if (g_updState == UPD_INSTALLING || g_updState == UPD_DONE) {
    drawUpdating(now);
  } else if (g_hotspot && !wifiUp) {
    drawSetupScreen(now, !g_savedSsids.empty());
  } else if (wifiUp && (sp == SP_NEEDS_CLIENT || sp == SP_NEEDS_LOGIN || sp == SP_ERROR)) {
    drawConnectSpotify(now, g_spMsg);
  } else if (g_statusMode) {
    char msg[96];
    strlcpy(msg, g_status, sizeof(msg));
    drawStatus(msg, now);
  } else if (!t.active) {
    drawIdle(a, now);
  } else {
    uint32_t pos = positionOf(t, now);
    if (g_lyrState == LYR_SYNCED && !g_preferArt) drawLyricsView(t, a, pos, now, dt);
    else drawArtView(t, a, pos, now);
    // the song should be over: ask Spotify right away instead of waiting
    static uint32_t lastKick = 0;
    if (t.playing && t.durationMs && t.progressMs + (now - t.stampMs) > t.durationMs + 400 && now - lastKick > 2000) {
      lastKick = now;
      g_forcePoll = true;
    }
  }
  if (g_resetHoldMs > 1500) drawResetHold(g_resetHoldMs);
  pushFrame();   // inside the lock: the album-art decoder borrows the canvas
  unlock();

  // backlight
  static int bl = 0;
  bool busy = active || g_statusMode || g_hotspot || sp != SP_CONNECTED || g_updState == UPD_INSTALLING;
  int target = (busy || now - g_lastActiveMs < IDLE_DIM_AFTER_MS) ? BRIGHTNESS_ACTIVE : BRIGHTNESS_IDLE;
  if (bl != target) {
    bl += (target > bl) ? min(8, target - bl) : -min(4, bl - target);
#if TFT_BL >= 0
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL, bl);
#else
    ledcWrite(0, bl);
#endif
#endif
  }
}

// ============================================================================
//                              SETUP / LOOP
// ============================================================================
static void handleButton(uint32_t now) {
  static bool down = false;
  static uint32_t tDown = 0;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed && !down) { down = true; tDown = now; }
  else if (!pressed && down) {
    down = false;
    if (now - tDown < 1000) { g_preferArt = !g_preferArt; g_snapScroll = true; }   // short press
  }
  g_resetHoldMs = down ? now - tDown : 0;
  if (down && now - tDown >= 10000) {
    LOG.println("BOOT held 10 s: factory reset");
    renderFrame(now, FRAME_MS);
    Settings::factoryReset();
    delay(800);
    ESP.restart();
  }
}

void setup() {
  LOG.begin(115200);
  delay(100);
  LOG.printf("\nSpotify Now Playing display %s\n", FW_VERSION);
  mux = xSemaphoreCreateMutex();
  pinMode(BUTTON_PIN, INPUT_PULLUP);

#if TFT_BL >= 0
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
#endif
#endif

  tft.initR(TFT_INIT_TAB);
  tft.setRotation(TFT_ROTATION);
  tft.invertDisplay(TFT_INVERT);
  tft.fillScreen(BLACK);
  u8f.begin(canvas);
  u8f.setFontMode(1);
  u8f.setFontDirection(0);

  // allocate the two album-art buffers early, before WiFi fragments the heap
  deriveDefault(g_art[0]);
  if (!canvas.getBuffer()) {
    LOG.println("Not enough RAM for buffers!");
    while (true) delay(1000);
  }
  logMem("buffers");
  if (!LittleFS.begin(true)) LOG.println("LittleFS mount failed (album art disabled)");
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(jpgOut);

  // settings saved through the web page
  Settings::begin();
  g_clientId = Settings::spotifyClientId();
  g_refreshToken = Settings::spotifyRefreshToken();
  g_spUser = Settings::spotifyUser();
  g_spState = g_clientId.isEmpty() ? SP_NEEDS_CLIENT : (g_refreshToken.isEmpty() ? SP_NEEDS_LOGIN : SP_CONNECTED);
  LOG.printf("Firmware %s, Spotify: %s\n", FW_VERSION,
             g_spState == SP_CONNECTED ? "logged in" : (g_spState == SP_NEEDS_LOGIN ? "not logged in" : "no Client ID"));

  logMem("settings");
  WiFi.persistent(false);   // we keep the networks ourselves (settings.cpp)
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(DEVICE_HOSTNAME);
  strlcpy(g_status, "Connecting to WiFi...", sizeof(g_status));
  logMem("wifi init");
  webBegin();
  logMem("web server");
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t last = millis();
  uint32_t now = millis();
  float dt = (float)(now - last);
  if (dt > 200) dt = 200;
  last = now;
  handleButton(now);
  renderFrame(now, dt);
  uint32_t spent = millis() - now;
  if (spent < FRAME_MS) delay(FRAME_MS - spent);
}
