#pragma once

#include <Arduino.h>

namespace deals {

constexpr char AP_SSID[] = "DEALS";
constexpr char AP_PASSWORD[] = "deals123";

constexpr uint8_t LIN_TX_PIN = 16;  // ESP32 TX -> LINTTL3 RX
constexpr uint8_t LIN_RX_PIN = 17;  // ESP32 RX <- LINTTL3 TX
constexpr uint8_t LIN_SLP_PIN = 5;  // HIGH = normal mode
constexpr uint8_t ACTIVITY_LED_PIN = 2;
constexpr uint32_t LIN_BAUDRATE = 19200;

}  // namespace deals
