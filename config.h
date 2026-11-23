// ============================================================================
//  config.h  —  hardware and behaviour settings
//
//  No passwords or keys live here any more: Wi-Fi networks and your Spotify
//  login are set up through the device's web page and stored on the ESP32.
//  This file is safe to share.
// ============================================================================
#pragma once

// ---------- Web interface / setup -------------------------------------------
#define DEVICE_HOSTNAME   "lyrics"                  // web page at http://lyrics.local
#define SETUP_AP_NAME     "Lyrics-Display-Setup"    // hotspot shown when no Wi-Fi works
#define GITHUB_REPO       "alexmoen955/spotify-lyrics-display"   // updates come from its releases
// The Spotify login comes back through this page (docs/index.html on GitHub Pages).
// It must be added as a Redirect URI in your Spotify app.
#define SPOTIFY_REDIRECT_URI "https://alexmoen955.github.io/spotify-lyrics-display/"

// ---------- Clock shown when nothing is playing -----------------------------
// POSIX TZ string. Examples:
//   "UTC0"                                  UTC
//   "GMT0BST,M3.5.0/1,M10.5.0"              UK
//   "CET-1CEST,M3.5.0,M10.5.0/3"            Central Europe
//   "EST5EDT,M3.2.0,M11.1.0"                US Eastern
//   "PST8PDT,M3.2.0,M11.1.0"                US Pacific
#define TIMEZONE   "CET-1CEST,M3.5.0,M10.5.0/3"   // Belgium (Ghent)
#define CLOCK_24H  1

// ---------- Display wiring (ESP32 hardware SPI: SCK=18, MOSI=23) -----------
#define TFT_CS    5
#define TFT_DC    16    // a.k.a. A0 / RS
#define TFT_RST   17    // a.k.a. RES / RESET
#define TFT_BL    4     // a.k.a. LED / BL  (PWM brightness). -1 if wired to 3V3
#define BUTTON_PIN 0    // BOOT button: press = lyrics / album-art view, hold 10 s = factory reset

// ---------- Display tuning (only touch if the picture looks wrong) ---------
#define TFT_INIT_TAB  INITR_144GREENTAB  // try INITR_GREENTAB or INITR_BLACKTAB if edges show garbage
#define TFT_ROTATION  0                  // 0..3, rotate the picture
#define TFT_INVERT    0                  // 1 if colours look like a photo negative
#define TFT_SWAP_RB   0                  // 1 if red and blue are swapped (blue faces look red)

// ---------- Behaviour --------------------------------------------------------
#define LYRICS_LEAD_MS     250    // show each lyric line this early (feels more "in sync")
#define WORD_HIGHLIGHT     1      // 1 = light up the current line word by word (karaoke), 0 = whole line
#define WORD_MS_PER_CHAR   70     // sweep speed: higher = slower word-by-word highlight
#define POLL_INTERVAL_MS   2500   // how often to ask Spotify while music plays
#define POLL_IDLE_MS       6000   // ...and while nothing plays
#define IDLE_DIM_AFTER_MS  120000 // dim the backlight after 2 min of nothing playing
#define BRIGHTNESS_ACTIVE  255
#define BRIGHTNESS_IDLE    40
