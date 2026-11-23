// Log output that goes to the USB serial port AND to a ring buffer that the
// web page reads for its live log view. Use LOG.printf / LOG.println.
#pragma once
#include <Arduino.h>

class LogTee : public Print {
 public:
  void begin(unsigned long baud);
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t len) override;
  using Print::write;

  // Copy log text written at or after byte position `from` into `out`.
  // Returns the position to ask for next time. If `from` is older than the
  // buffer holds, starts at the oldest byte still available.
  uint32_t readSince(uint32_t from, String& out);

 private:
  void putc(char c);
};

extern LogTee LOG;
