#include "logger.h"

LogTee LOG;

static const size_t RING = 6144;          // last ~6 KB of log text
static char s_ring[RING];
static uint32_t s_total = 0;              // bytes ever written
static bool s_lineStart = true;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void LogTee::begin(unsigned long baud) { Serial.begin(baud); }

void LogTee::putc(char c) {
  s_ring[s_total % RING] = c;
  s_total++;
}

size_t LogTee::write(uint8_t c) { return write(&c, 1); }

size_t LogTee::write(const uint8_t* buf, size_t len) {
  Serial.write(buf, len);
  char stamp[16];
  int sl = 0;
  portENTER_CRITICAL(&s_mux);
  for (size_t i = 0; i < len; i++) {
    char c = (char)buf[i];
    if (c == '\r') continue;
    if (s_lineStart) {   // timestamp at the start of every line
      if (!sl) sl = snprintf(stamp, sizeof(stamp), "[%7.1fs] ", millis() / 1000.0f);
      for (int k = 0; k < sl; k++) putc(stamp[k]);
      s_lineStart = false;
    }
    putc(c);
    if (c == '\n') s_lineStart = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return len;
}

uint32_t LogTee::readSince(uint32_t from, String& out) {
  uint32_t end;
  portENTER_CRITICAL(&s_mux);
  end = s_total;
  portEXIT_CRITICAL(&s_mux);
  uint32_t start = from;
  if (start > end) start = 0;                       // device restarted since the page last asked
  if (end - start > RING - 512) start = end - (RING - 512);  // stay clear of bytes being overwritten
  if (end - start > 4096) end = start + 4096;      // at most 4 KB per request; the page asks again
  out.reserve(out.length() + (end - start));
  for (uint32_t i = start; i < end; i++) out += s_ring[i % RING];
  return end;
}
