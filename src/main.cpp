#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include "config.h"
#include "lin_monitor.h"

namespace {

HardwareSerial linSerial(2);
LinMonitor linMonitor;
WebServer server(80);
WebSocketsServer webSocket(81);
Preferences preferences;
uint32_t frameCount = 0;
uint32_t validFrameCount = 0;
uint32_t invalidFrameCount = 0;
LinObservation latest;
constexpr size_t FRAME_HISTORY_SIZE = 64;
LinObservation frameHistory[FRAME_HISTORY_SIZE];
uint32_t frameSequence[FRAME_HISTORY_SIZE] = {};
size_t frameHistoryHead = 0;
size_t frameHistoryCount = 0;
constexpr size_t CAPTURE_QUEUE_SIZE = 128;
LinObservation captureQueue[CAPTURE_QUEUE_SIZE];
size_t captureQueueRead = 0;
size_t captureQueueWrite = 0;
size_t captureQueueCount = 0;
size_t captureQueueHighWater = 0;
uint32_t captureQueueDrops = 0;
uint8_t webSocketClients = 0;
uint32_t lastFrameMs = 0;
uint32_t lastRateMs = 0;
uint32_t lastRateFrameCount = 0;
uint32_t frameRate = 0;
uint32_t lastStatusBroadcastMs = 0;
uint32_t currentBaudrate = deals::LIN_BAUDRATE;
uint8_t checksumMode = 0;  // 0 = auto, 1 = classic, 2 = enhanced
bool listenOnly = true;
bool filterEnabled = false;
bool filterInclude = false;
bool filteredIds[64] = {};
File ldfUploadFile;
constexpr size_t LDF_MAX_SIZE = 256 * 1024;
size_t ldfUploadSize = 0;
bool ldfUploadFailed = false;
String ldfUploadPath;
uint32_t filteredFrameCount = 0;
bool activityLedPulse = false;
uint32_t activityLedUntilMs = 0;
String stationSsid;
String stationPassword;

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character == '"' || character == '\\') escaped += '\\';
    if (character == '\n') escaped += "\\n";
    else if (character == '\r') escaped += "\\r";
    else if (character == '\t') escaped += "\\t";
    else escaped += character;
  }
  return escaped;
}

void connectStation() {
  WiFi.disconnect(false, false);
  if (stationSsid.isEmpty()) return;
  WiFi.begin(stationSsid.c_str(), stationPassword.c_str());
  Serial.printf("Connecting Wi-Fi station to %s\n", stationSsid.c_str());
}

void pulseActivityLed(uint32_t durationMs = 20) {
  digitalWrite(deals::ACTIVITY_LED_PIN, HIGH);
  activityLedPulse = true;
  activityLedUntilMs = millis() + durationMs;
}

void serviceActivityLed() {
  if (activityLedPulse && static_cast<int32_t>(millis() - activityLedUntilMs) >= 0) {
    digitalWrite(deals::ACTIVITY_LED_PIN, LOW);
    activityLedPulse = false;
  }
}

void applyLinTransmitMode() {
  if (!listenOnly) return;
  // TJA1020: TXD HIGH is recessive. Disconnect the ESP32 UART output and
  // hold TXD HIGH so the transceiver cannot drive the LIN bus dominant.
  // Keep UART RX running; stopping the UART also stops passive monitoring.
  pinMode(deals::LIN_TX_PIN, INPUT_PULLUP);
}

String hexByte(uint8_t value) {
  char text[3];
  snprintf(text, sizeof(text), "%02X", value);
  return String(text);
}

uint8_t protectedId(uint8_t id) {
  id &= 0x3F;
  const uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
  const uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 1;
  return id | (p0 << 6) | (p1 << 7);
}

uint8_t checksum(const uint8_t* data, uint8_t length, uint16_t sum = 0) {
  for (uint8_t index = 0; index < length; ++index) {
    sum += data[index];
    if (sum > 0xFF) sum -= 0xFF;
  }
  return static_cast<uint8_t>(~sum);
}

bool parseLinData(String text, uint8_t* data, uint8_t& length) {
  text.trim();
  text.toUpperCase();
  text.replace(",", " ");
  text.replace(";", " ");
  text.replace("0X", "");
  length = 0;
  if (text.length() == 0) return true;

  if (text.indexOf(' ') < 0) {
    if ((text.length() % 2) != 0 || text.length() > 16) return false;
    for (int index = 0; index < text.length(); index += 2) {
      char token[3] = {text[index], text[index + 1], 0};
      char* end = nullptr;
      const long value = strtol(token, &end, 16);
      if (end == token || *end != 0 || value < 0 || value > 0xFF) return false;
      data[length++] = static_cast<uint8_t>(value);
    }
    return true;
  }

  int start = 0;
  while (start < text.length()) {
    while (start < text.length() && text[start] == ' ') ++start;
    if (start >= text.length()) break;
    int end = text.indexOf(' ', start);
    if (end < 0) end = text.length();
    if (length >= 8) return false;
    const String token = text.substring(start, end);
    char* parsedEnd = nullptr;
    const long value = strtol(token.c_str(), &parsedEnd, 16);
    if (parsedEnd == token.c_str() || *parsedEnd != 0 || value < 0 || value > 0xFF) return false;
    data[length++] = static_cast<uint8_t>(value);
    start = end + 1;
  }
  return true;
}

void sendLinFrame(uint8_t id, const uint8_t* data, uint8_t length, bool enhancedChecksum) {
  const uint8_t pid = protectedId(id);
  const uint8_t sum = checksum(data, length, enhancedChecksum ? pid : 0);
  uint8_t payload[11] = {0x55, pid};
  memcpy(&payload[2], data, length);
  payload[length + 2] = sum;

  linSerial.flush();
  linSerial.end();
  pinMode(deals::LIN_TX_PIN, OUTPUT);
  digitalWrite(deals::LIN_TX_PIN, LOW);
  delayMicroseconds((14000000UL + currentBaudrate - 1) / currentBaudrate);
  digitalWrite(deals::LIN_TX_PIN, HIGH);
  delayMicroseconds((2000000UL + currentBaudrate - 1) / currentBaudrate);
  linSerial.begin(currentBaudrate, SERIAL_8N1, deals::LIN_RX_PIN, deals::LIN_TX_PIN);
  delayMicroseconds((1000000UL + currentBaudrate - 1) / currentBaudrate);
  pulseActivityLed();
  linSerial.write(payload, length + 3);
  linSerial.flush();
}

String filterIdsText() {
  String result;
  for (uint8_t id = 0; id < 64; ++id) {
    if (!filteredIds[id]) continue;
    if (result.length()) result += ' ';
    result += hexByte(id);
  }
  return result;
}

void rememberFrame(const LinObservation& observation) {
  frameHistory[frameHistoryHead] = observation;
  frameSequence[frameHistoryHead] = frameCount;
  frameHistoryHead = (frameHistoryHead + 1) % FRAME_HISTORY_SIZE;
  if (frameHistoryCount < FRAME_HISTORY_SIZE) ++frameHistoryCount;
}

bool enqueueFrame(const LinObservation& observation) {
  if (captureQueueCount >= CAPTURE_QUEUE_SIZE) {
    ++captureQueueDrops;
    return false;
  }
  captureQueue[captureQueueWrite] = observation;
  captureQueueWrite = (captureQueueWrite + 1) % CAPTURE_QUEUE_SIZE;
  ++captureQueueCount;
  if (captureQueueCount > captureQueueHighWater) captureQueueHighWater = captureQueueCount;
  return true;
}

String frameJson(const LinObservation& frame, uint32_t sequence) {
  String json = "{\"sequence\":" + String(sequence);
  json += ",\"time\":" + String(frame.timestampMs);
  json += ",\"pid\":" + String(frame.protectedId);
  json += ",\"id\":" + String(frame.protectedId & 0x3F);
  json += ",\"length\":" + String(frame.dataLength);
  json += ",\"data\":\"";
  for (uint8_t index = 0; index < frame.dataLength; ++index) {
    if (index > 0) json += ' ';
    json += hexByte(frame.data[index]);
  }
  json += "\",\"checksum\":\"" + hexByte(frame.checksum) + "\"";
  json += ",\"checksumType\":\"";
  json += frame.enhancedChecksum ? "enhanced" : "classic";
  json += "\",\"pidValid\":";
  json += frame.pidValid ? "true" : "false";
  json += ",\"checksumValid\":";
  json += frame.checksumValid ? "true" : "false";
  json += ",\"valid\":";
  json += frame.valid ? "true" : "false";
  json += '}';
  return json;
}

String statusJson() {
  String json = "{\"project\":\"DEALS\",\"version\":\"";
  json += DEALS_VERSION;
  json += "\",\"baudrate\":" + String(currentBaudrate);
  json += ",\"checksumMode\":";
  json += String((char)34);
  json += checksumMode == 1 ? "classic" : checksumMode == 2 ? "enhanced" : "auto";
  json += String((char)34);
  json += ",\"listenOnly\":" + String(listenOnly ? "true" : "false");
  json += ",\"frames\":" + String(frameCount);
  json += ",\"validFrames\":" + String(validFrameCount);
  json += ",\"invalidFrames\":" + String(invalidFrameCount);
  json += ",\"lastPid\":" + String(latest.protectedId);
  json += ",\"lastLength\":" + String(latest.dataLength);
  json += ",\"lastValid\":" + String(latest.valid ? "true" : "false");
  json += ",\"queue\":" + String(captureQueueCount);
  json += ",\"queueCapacity\":" + String(CAPTURE_QUEUE_SIZE);
  json += ",\"queueHighWater\":" + String(captureQueueHighWater);
  json += ",\"queueDrops\":" + String(captureQueueDrops);
  json += ",\"queueUtilization\":" + String((captureQueueCount * 100U) / CAPTURE_QUEUE_SIZE);
  json += ",\"uartBytes\":" + String(linSerial.available());
  json += ",\"wsClients\":" + String(webSocketClients);
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"frameRate\":" + String(frameRate);
  json += ",\"filteredFrames\":" + String(filteredFrameCount);
  json += ",\"filterEnabled\":" + String(filterEnabled ? "true" : "false");
  json += ",\"filterMode\":\"" + String(filterInclude ? "include" : "exclude") + "\"";
  json += ",\"filterIds\":\"" + filterIdsText() + "\"";
  json += ",\"lastFrameAgoMs\":" + String(lastFrameMs ? millis() - lastFrameMs : 0);
  json += ",\"apSsid\":\"" + String(deals::AP_SSID) + "\"";
  json += ",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"staConfigured\":" + String(stationSsid.isEmpty() ? "false" : "true");
  json += ",\"staConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"staSsid\":\"" + jsonEscape(stationSsid) + "\"";
  json += ",\"staIp\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\"";
  json += ",\"parser\":\"validated-passive\"}";
  return json;
}

String framesJson() {
  String json = "{\"frames\":[";
  json.reserve(256 + frameHistoryCount * 150);
  const size_t oldest =
      (frameHistoryHead + FRAME_HISTORY_SIZE - frameHistoryCount) %
      FRAME_HISTORY_SIZE;

  for (size_t offset = 0; offset < frameHistoryCount; ++offset) {
    const size_t index = (oldest + offset) % FRAME_HISTORY_SIZE;
    const LinObservation& frame = frameHistory[index];
    if (offset > 0) json += ',';
    json += "{\"sequence\":" + String(frameSequence[index]);
    json += ",\"time\":" + String(frame.timestampMs);
    json += ",\"pid\":" + String(frame.protectedId);
    json += ",\"id\":" + String(frame.protectedId & 0x3F);
    json += ",\"length\":" + String(frame.dataLength);
    json += ",\"data\":\"";
    for (uint8_t byteIndex = 0; byteIndex < frame.dataLength; ++byteIndex) {
      if (byteIndex > 0) json += ' ';
      json += hexByte(frame.data[byteIndex]);
    }
    json += "\",\"checksum\":\"" + hexByte(frame.checksum) + "\"";
    json += ",\"checksumType\":\"";
    json += frame.enhancedChecksum ? "enhanced" : "classic";
    json += "\",\"pidValid\":";
    json += frame.pidValid ? "true" : "false";
    json += ",\"checksumValid\":";
    json += frame.checksumValid ? "true" : "false";
    json += ",\"valid\":";
    json += frame.valid ? "true" : "false";
    json += '}';
  }
  json += "]}";
  return json;
}

void setupWebServer() {
  server.on("/", HTTP_GET, [] {
    File file = LittleFS.open("/index.html", "r");
    if (!file) {
      server.send(500, "text/plain", "index.html unavailable");
      return;
    }
    server.streamFile(file, "text/html");
    file.close();
  });
  server.on("/api/status", HTTP_GET, [] {
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/wifi", HTTP_GET, [] {
    String json = "{\"ssid\":\"" + jsonEscape(stationSsid) + "\"";
    json += ",\"passwordStored\":" + String(stationPassword.isEmpty() ? "false" : "true");
    json += ",\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += ",\"ip\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });
  server.on("/api/wifi", HTTP_POST, [] {
    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"error\":\"missing ssid\"}");
      return;
    }
    String nextSsid = server.arg("ssid");
    nextSsid.trim();
    if (nextSsid.length() > 32) {
      server.send(400, "application/json", "{\"error\":\"ssid too long\"}");
      return;
    }
    String nextPassword = nextSsid == stationSsid ? stationPassword : "";
    if (server.hasArg("password") && !server.arg("password").isEmpty()) {
      nextPassword = server.arg("password");
    }
    if (nextPassword.length() > 64) {
      server.send(400, "application/json", "{\"error\":\"password too long\"}");
      return;
    }
    if (nextSsid.isEmpty()) nextPassword = "";
    stationSsid = nextSsid;
    stationPassword = nextPassword;
    preferences.putString("staSsid", stationSsid);
    preferences.putString("staPass", stationPassword);
    connectStation();
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/frames", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", framesJson());
  });
  server.on("/api/frames/clear", HTTP_POST, [] {
    frameHistoryHead = 0;
    frameHistoryCount = 0;
    server.send(204);
  });
  server.on("/api/lin", HTTP_POST, [] {
    const uint32_t baud = server.hasArg("baudrate") ? server.arg("baudrate").toInt() : currentBaudrate;
    if (baud != 9600 && baud != 10400 && baud != 19200 && baud != 20000) {
      server.send(400, "application/json", "{\"error\":\"invalid baudrate\"}");
      return;
    }
    uint8_t nextChecksumMode = checksumMode;
    if (server.hasArg("checksum")) {
      const String mode = server.arg("checksum");
      if (mode != "auto" && mode != "classic" && mode != "enhanced") {
        server.send(400, "application/json", "{\"error\":\"invalid checksum mode\"}");
        return;
      }
      nextChecksumMode = mode == "classic" ? 1 : mode == "enhanced" ? 2 : 0;
    }
    const bool nextListenOnly = server.hasArg("mode") ? server.arg("mode") != "normal" : listenOnly;
    currentBaudrate = baud;
    checksumMode = nextChecksumMode;
    listenOnly = nextListenOnly;
    preferences.putUInt("baud", currentBaudrate);
    preferences.putUChar("checksum", checksumMode);
    preferences.putBool("listen", listenOnly);
    linSerial.end();
    linMonitor.begin(linSerial, currentBaudrate, deals::LIN_RX_PIN,
                     listenOnly ? -1 : deals::LIN_TX_PIN);
    linMonitor.setChecksumMode(checksumMode);
    applyLinTransmitMode();
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/send", HTTP_POST, [] {
    if (listenOnly) {
      server.send(403, "application/json", "{\"error\":\"LIN transmit disabled\"}");
      return;
    }
    String idText = server.arg("id");
    idText.trim();
    idText.toUpperCase();
    idText.replace("0X", "");
    if (idText.endsWith("H")) idText.remove(idText.length() - 1);
    char* idEnd = nullptr;
    const long id = strtol(idText.c_str(), &idEnd, 16);
    uint8_t data[8] = {};
    uint8_t length = 0;
    if (idEnd == idText.c_str() || *idEnd != 0 || id < 0 || id > 0x3F ||
        !parseLinData(server.arg("data"), data, length) || length > 8) {
      server.send(400, "application/json", "{\"error\":\"invalid LIN frame\"}");
      return;
    }
    const String mode = server.arg("checksum");
    const bool diagnostic = id == 0x3C || id == 0x3D;
    const bool enhanced = mode == "enhanced" || (mode != "classic" && !diagnostic);
    sendLinFrame(static_cast<uint8_t>(id), data, length, enhanced);
    server.send(200, "application/json",
                String("{\"sent\":true,\"id\":") + String(id) +
                    ",\"pid\":" + String(protectedId(static_cast<uint8_t>(id))) +
                    ",\"length\":" + String(length) +
                    ",\"checksumType\":\"" + (enhanced ? "enhanced" : "classic") + "\"}");
  });
  server.on("/api/filter", HTTP_POST, [] {
    memset(filteredIds, 0, sizeof(filteredIds));
    filterInclude = server.arg("mode") == "include";
    String ids = server.arg("ids");
    ids.replace(',', ' '); ids.replace(';', ' '); ids.toUpperCase();
    filterEnabled = false;
    int start = 0;
    while (start < ids.length()) {
      while (start < ids.length() && ids[start] == ' ') ++start;
      int end = ids.indexOf(' ', start); if (end < 0) end = ids.length();
      String token = ids.substring(start, end); token.replace("0X", "");
      if (token.length()) { const int id = strtol(token.c_str(), nullptr, 16); if (id >= 0 && id < 64) { filteredIds[id] = true; filterEnabled = true; } }
      start = end + 1;
    }
    uint64_t mask = 0;
    for (uint8_t id = 0; id < 64; ++id) if (filteredIds[id]) mask |= (uint64_t{1} << id);
    preferences.putULong64("filterMask", mask);
    preferences.putBool("filterOn", filterEnabled);
    preferences.putBool("filterInc", filterInclude);
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/diagnostics/reset", HTTP_POST, [] {
    captureQueueHighWater = captureQueueCount;
    captureQueueDrops = 0;
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/ldf/file", HTTP_GET, [] {
    String name = server.arg("name"); name.replace("/", ""); name.replace("\\", "");
    if (!name.endsWith(".ldf")) { server.send(400); return; }
    File file = LittleFS.open("/ldf/" + name, "r");
    if (!file) { server.send(404); return; }
    server.streamFile(file, "text/plain"); file.close();
  });
  server.on("/api/ldf", HTTP_GET, [] {
    String json = "{\"maxSize\":" + String(LDF_MAX_SIZE) + ",\"files\":["; bool first = true;
    File dir = LittleFS.open("/ldf"); File file = dir.openNextFile();
    while (file) { if (!file.isDirectory()) { if (!first) json += ','; first = false; String name = file.name(); const int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1); json += "{\"name\":\"" + name + "\",\"size\":" + String(file.size()) + "}"; } file = dir.openNextFile(); }
    json += "]}"; server.send(200, "application/json", json);
  });
  server.on("/api/ldf", HTTP_DELETE, [] {
    String name = server.arg("name"); name.replace("/", ""); name.replace("\\", "");
    if (!name.endsWith(".ldf") || !LittleFS.remove("/ldf/" + name)) { server.send(404); return; }
    server.send(204);
  });
  server.on("/api/ldf/upload", HTTP_POST, [] { server.send(ldfUploadFailed ? 413 : 204); }, [] {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) { ldfUploadSize = 0; ldfUploadFailed = false; String name = upload.filename; name.replace("/", ""); name.replace("\\", ""); ldfUploadPath = "/ldf/" + name; if (name.endsWith(".ldf")) ldfUploadFile = LittleFS.open(ldfUploadPath, "w"); else ldfUploadFailed = true; }
    else if (upload.status == UPLOAD_FILE_WRITE && ldfUploadFile) { ldfUploadSize += upload.currentSize; if (ldfUploadSize <= LDF_MAX_SIZE) ldfUploadFile.write(upload.buf, upload.currentSize); else { ldfUploadFailed = true; ldfUploadFile.close(); LittleFS.remove(ldfUploadPath); } }
    else if (upload.status == UPLOAD_FILE_END && ldfUploadFile) ldfUploadFile.close();
    else if (upload.status == UPLOAD_FILE_ABORTED && ldfUploadFile) ldfUploadFile.close();
  });
  server.serveStatic("/", LittleFS, "/");
  server.begin();
  webSocket.begin();
  webSocket.onEvent([](uint8_t, WStype_t type, uint8_t*, size_t) {
    if (type == WStype_CONNECTED && webSocketClients < 255) ++webSocketClients;
    if (type == WStype_DISCONNECTED && webSocketClients > 0) --webSocketClients;
  });
}

void broadcastStatus() {
  String message = "{\"type\":\"status\",\"value\":" + statusJson() + '}';
  webSocket.broadcastTXT(message);
}

void drainCaptureQueue() {
  uint8_t drained = 0;
  while (captureQueueCount > 0 && drained < 32) {
    LinObservation observation = captureQueue[captureQueueRead];
    captureQueueRead = (captureQueueRead + 1) % CAPTURE_QUEUE_SIZE;
    --captureQueueCount;
    ++drained;

    latest = observation;
    ++frameCount;
    lastFrameMs = millis();
    if (observation.valid) ++validFrameCount; else ++invalidFrameCount;
    rememberFrame(observation);
    if (webSocketClients > 0) {
      String message = "{\"type\":\"frame\",\"value\":" +
                       frameJson(observation, frameCount) + '}';
      webSocket.broadcastTXT(message);
    }
  }
}

}  // namespace

void setup() {
  pinMode(deals::ACTIVITY_LED_PIN, OUTPUT);
  digitalWrite(deals::ACTIVITY_LED_PIN, LOW);
  pinMode(deals::LIN_SLP_PIN, OUTPUT);
  digitalWrite(deals::LIN_SLP_PIN, HIGH);
  Serial.begin(115200);

  LittleFS.begin(true);
  if (!LittleFS.exists("/ldf")) LittleFS.mkdir("/ldf");
  preferences.begin("deals", false);
  currentBaudrate = preferences.getUInt("baud", deals::LIN_BAUDRATE);
  checksumMode = preferences.getUChar("checksum", 0);
  listenOnly = preferences.getBool("listen", true);
  if (currentBaudrate != 9600 && currentBaudrate != 10400 && currentBaudrate != 19200 && currentBaudrate != 20000) currentBaudrate = deals::LIN_BAUDRATE;
  filterEnabled = preferences.getBool("filterOn", false);
  filterInclude = preferences.getBool("filterInc", false);
  stationSsid = preferences.getString("staSsid", "");
  stationPassword = preferences.getString("staPass", "");
  const uint64_t savedFilterMask = preferences.getULong64("filterMask", 0);
  for (uint8_t id = 0; id < 64; ++id) filteredIds[id] = (savedFilterMask & (uint64_t{1} << id)) != 0;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(deals::AP_SSID, deals::AP_PASSWORD);
  connectStation();

  linMonitor.begin(linSerial, currentBaudrate, deals::LIN_RX_PIN,
                   listenOnly ? -1 : deals::LIN_TX_PIN);
  applyLinTransmitMode();
  linMonitor.setChecksumMode(checksumMode);
  setupWebServer();

  Serial.printf("DEALS %s ready at http://%s\n", DEALS_VERSION,
                WiFi.softAPIP().toString().c_str());
  if (!stationSsid.isEmpty()) Serial.printf("Station Wi-Fi configured: %s\n", stationSsid.c_str());
}

void loop() {
  server.handleClient();
  webSocket.loop();
  serviceActivityLed();

  LinObservation observation;
  if (linMonitor.poll(observation)) {
    const uint8_t id = observation.protectedId & 0x3F;
    const bool accepted = !filterEnabled || (filterInclude ? filteredIds[id] : !filteredIds[id]);
    if (accepted) enqueueFrame(observation); else ++filteredFrameCount;
    pulseActivityLed();
  }
  drainCaptureQueue();

  const uint32_t now = millis();
  if (now - lastRateMs >= 1000) {
    frameRate = frameCount - lastRateFrameCount;
    lastRateFrameCount = frameCount;
    lastRateMs = now;
  }
  if (now - lastStatusBroadcastMs >= 1000) {
    lastStatusBroadcastMs = now;
    broadcastStatus();
  }
}
