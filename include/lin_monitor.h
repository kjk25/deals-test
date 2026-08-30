#pragma once

#include <Arduino.h>

struct LinObservation {
  uint32_t timestampMs = 0;
  uint8_t protectedId = 0;
  uint8_t data[8] = {};
  uint8_t dataLength = 0;
  uint8_t checksum = 0;
  bool pidValid = false;
  bool checksumValid = false;
  bool enhancedChecksum = false;
  bool valid = false;
};

class LinMonitor {
 public:
  void begin(HardwareSerial& serial, uint32_t baudrate, int8_t rxPin, int8_t txPin);
  void setChecksumMode(uint8_t mode);
  bool poll(LinObservation& observation);

 private:
  enum class State : uint8_t { WaitForBreak, WaitForSync, CollectFrame };

  bool finishFrame(LinObservation& observation);

  HardwareSerial* serial_ = nullptr;
  uint8_t buffer_[10] = {};
  uint8_t length_ = 0;
  uint32_t baudrate_ = 0;
  uint32_t lastByteUs_ = 0;
  State state_ = State::WaitForBreak;
  uint8_t checksumMode_ = 0;  // 0 = automatisch, 1 = classic, 2 = enhanced
};
