#include "lin_monitor.h"

namespace {

bool validProtectedId(uint8_t pid) {
  const uint8_t id = pid & 0x3F;
  const uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
  const uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 1;
  return ((pid >> 6) & 1) == p0 && ((pid >> 7) & 1) == p1;
}

uint8_t linChecksum(const uint8_t* bytes, uint8_t length, uint16_t sum = 0) {
  for (uint8_t index = 0; index < length; ++index) {
    sum += bytes[index];
    if (sum > 0xFF) sum -= 0xFF;
  }
  return static_cast<uint8_t>(~sum);
}

}  // namespace

void LinMonitor::begin(HardwareSerial& serial, uint32_t baudrate, int8_t rxPin,
                       int8_t txPin) {
  serial_ = &serial;
  baudrate_ = baudrate;
  serial_->begin(baudrate, SERIAL_8N1, rxPin, txPin);
}

void LinMonitor::setChecksumMode(uint8_t mode) { checksumMode_ = mode; }

bool LinMonitor::finishFrame(LinObservation& observation) {
  state_ = State::WaitForBreak;
  if (length_ < 3) {
    length_ = 0;
    return false;
  }

  observation = {};
  observation.timestampMs = millis();
  observation.protectedId = buffer_[0];
  observation.dataLength = length_ - 2;
  memcpy(observation.data, &buffer_[1], observation.dataLength);
  observation.checksum = buffer_[length_ - 1];
  observation.pidValid = validProtectedId(observation.protectedId);

  const uint8_t classic = linChecksum(observation.data, observation.dataLength);
  const uint8_t frameId = observation.protectedId & 0x3F;
  const uint8_t enhanced = linChecksum(observation.data, observation.dataLength,
                                       observation.protectedId);
  const bool diagnosticFrame = frameId == 0x3C || frameId == 0x3D;
  observation.enhancedChecksum =
      !diagnosticFrame && enhanced == observation.checksum;
  const bool classicMatch = classic == observation.checksum;
  const bool enhancedMatch = !diagnosticFrame && enhanced == observation.checksum;
  observation.checksumValid = checksumMode_ == 1 ? classicMatch
      : checksumMode_ == 2 ? enhancedMatch : (classicMatch || enhancedMatch);
  observation.valid = observation.pidValid && observation.checksumValid;
  length_ = 0;
  return true;
}

bool LinMonitor::poll(LinObservation& observation) {
  if (serial_ == nullptr) return false;

  // Three character times reliably separate complete LIN responses.
  const uint32_t frameGapUs = (30000000UL + baudrate_ - 1) / baudrate_;
  if (state_ == State::CollectFrame && length_ > 0 &&
      static_cast<uint32_t>(micros() - lastByteUs_) >= frameGapUs) {
    return finishFrame(observation);
  }

  while (serial_->available() > 0) {
    const uint8_t value = static_cast<uint8_t>(serial_->read());
    lastByteUs_ = micros();

    if (state_ == State::WaitForBreak) {
      // ESP32 UART exposes a LIN break as a zero byte. Requiring sync next
      // prevents zero payload bytes from starting a frame.
      if (value == 0x00) state_ = State::WaitForSync;
      continue;
    }

    if (state_ == State::WaitForSync) {
      if (value == 0x55) {
        state_ = State::CollectFrame;
        length_ = 0;
      } else if (value != 0x00) {
        state_ = State::WaitForBreak;
      }
      continue;
    }

    if (length_ < sizeof(buffer_)) {
      buffer_[length_++] = value;
    } else {
      state_ = State::WaitForBreak;
      length_ = 0;
    }
  }

  return false;
}
