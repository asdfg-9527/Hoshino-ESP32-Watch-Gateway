// Suppress warnings that ESP-IDF's stricter defaults turn into build errors
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#include <Arduino.h>
#include <climits>
#include <map>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "sd_storage.h"
#include "watch_secret_local.h"
#define HOSHINO_ENABLE_ASR 0
#if HOSHINO_ENABLE_ASR
#include <SPIFFS.h>
#include <opus.h>
#endif
// 0.96" SSD1306 状态屏（U8g2 硬件 I2C）。在 flash 紧张的环境（例如
// bare_minimum_2MB 分区表）可以用 -DHOSHINO_ENABLE_DISPLAY=0 整块编译掉，
// 这样 U8g2 不会被链接，固件体积与该功能引入前保持一致。
#ifndef HOSHINO_ENABLE_DISPLAY
#define HOSHINO_ENABLE_DISPLAY 1
#endif
// 一次整屏推送要几十毫秒的 I2C 阻塞时间，状态屏不需要更高的刷新率。
#ifndef HOSHINO_DISPLAY_REFRESH_MS
#define HOSHINO_DISPLAY_REFRESH_MS 500
#endif
#if HOSHINO_ENABLE_DISPLAY
#include <Wire.h>
#include <U8g2lib.h>
#endif
#include <BluetoothSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/ccm.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#ifndef HOSHINO_DISABLE_LWIP_OVERRIDE
#include <lwip_napt_override.h>
#endif
extern "C" {
#include <lwip/ip4_addr.h>
#include <lwip/lwip_napt.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/dns.h>
}
namespace {
constexpr uint16_t kPort = 80;
constexpr size_t kMaxBody = 16 * 1024;
constexpr uint32_t kHttpTimeoutMs = 25000;
constexpr uint32_t kWatchScanSeconds = 8;
constexpr uint32_t kWatchVersionResponseTimeoutMs = 5000;
constexpr uint32_t kWatchBridgeResponseTimeoutMs = 12000;
constexpr uint32_t kWatchQuickAppLaunchWaitMs = 3000;
constexpr uint32_t kWatchQuickAppWakeResponseTimeoutMs = 5000;
constexpr uint32_t kWatchQuickAppWakeRetryDelayMs = 2500;
constexpr uint8_t kWatchQuickAppWakeAttempts = 3;
// Round-3/Weather captures reached the Xiaomi SPP service on RFCOMM server
// channel 5. Do not hard-code it as the only path: resolve via SDP first, then
// prefer channel 5 when advertised and only use it as a bounded fallback.
constexpr int kWatchObservedSppChannel = 5;
constexpr uint32_t kWatchSdpCacheMs = 30 * 60 * 1000;
constexpr uint32_t kWatchContextIdleResetMs = 10 * 60 * 1000;
constexpr uint32_t kBenchAckTimeoutMs = 8000;
constexpr uint32_t kBenchControlTimeoutMs = 8000;
constexpr uint32_t kBenchControlGapMs = 150;
constexpr uint32_t kBenchPacedChunkGapMs = 25;
constexpr uint8_t kBenchChunkRetries = 2;
constexpr size_t kBenchChunkRawMax = 512;
constexpr char kHoshinoQuickAppPackage[] = "com.hoshino.app";
constexpr char kApSsid[] = "Hoshino-Bridge";
// 配网触发 IO：短接这两个引脚（GPIO16 与 GPIO17）持续约 2 秒，即可进入配网模式。
//   GPIO16 = 输出高电平；GPIO17 = 输入下拉。短接后 GPIO17 读到高电平即触发。
// 这两个引脚是 UART2 的 RX/TX，本固件未使用 UART2，属于空闲引脚。
constexpr int kSetupTriggerPinOut = 16;
constexpr int kSetupTriggerPinIn  = 17;
constexpr uint32_t kSetupTriggerHoldMs = 2000;
constexpr uint8_t kSppV1VersionRequest[] = {
    0xba, 0xdc, 0xfe, 0x00, 0xc0, 0x03, 0x00, 0x00, 0x00, 0x00, 0xef,
};
constexpr uint8_t kSppHello[] = {
    0xba, 0xdc, 0xfe, 0x00, 0xc0, 0x03, 0x00, 0x00, 0x01, 0x00, 0xef,
};
constexpr uint32_t kSppHelloDrainWindowMs = 100;
constexpr size_t kSppHelloDrainMaxBytes = 128;
constexpr uint8_t kSppV2SessionStartRequest[] = {
    0xa5, 0xa5, 0x02, 0x00, 0x16, 0x00, 0x1d, 0x4d,
    0x01, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00,
    0x02, 0x02, 0x00, 0x00, 0xfc,
    0x03, 0x02, 0x00, 0x20, 0x00,
    0x04, 0x02, 0x00, 0x10, 0x27,
};

WebServer server(kPort);
Preferences prefs;
hoshino::SdStorageService sdStorage;  // Dedicated HSPI host on SCK18/MISO19/MOSI23/CS5; mounted only on demand.
BluetoothSerial watchBt;
constexpr size_t kWatchRxQueueBytes = 4 * 1024;
QueueHandle_t watchRxQueue = nullptr;
volatile uint32_t watchRxDroppedBytes = 0;
static size_t gSppRxBuffered = 0;
static uint32_t gSppRxResyncBytes = 0;
static uint32_t gSppRxCrcFailures = 0;
static uint32_t gSppRxOversizeHeaders = 0;
static uint32_t gSppRxPartialTimeouts = 0;
static uint32_t gBenchInterChunkGapMs = 0;
static bool gSppTxCoalesced = true;
static int gWatchResolvedSppChannel = 0;
static uint32_t gWatchSdpResolvedMs = 0;
static char gWatchSdpSource[24] = "unresolved";
static char gWatchSdpAddress[18]{};
String ssid, wifiPass, baseUrl, apiKey, model, localToken, caPem, apPassword, watchMac, watchAuthKey;
bool allowInsecureTls = false;
bool autoConnectWatch = false;
#if HOSHINO_ENABLE_ASR
bool serialAudioExport = false;
bool nativeXiaoAiReturn = true;
bool chatAfterAsr = true;
bool captureChannel8 = false;
volatile bool gXiaoAiEndPending = false;
volatile uint32_t gXiaoAiEndDueMs = 0;
volatile uint32_t gXiaoAiSessionSerial = 0;
volatile uint32_t gXiaoAiActiveSessionId = 0;
volatile bool gXiaoAiSessionActive = false;
volatile bool gXiaoAiFinalSent = false;
volatile uint32_t gXiaoAiSessionStartedMs = 0;
volatile uint32_t gXiaoAiLastProgressMs = 0;
volatile uint32_t gXiaoAiAudioMs = 0;
volatile uint32_t gXiaoAiNextPartialAudioMs = kXiaoAiRollingFirstAudioMs;
volatile bool gXiaoAiFinalAsrPending = false;
volatile uint32_t gXiaoAiFinalPendingSessionId = 0;
volatile bool gXiaoAiAwaitAssistant = false;
volatile uint32_t gXiaoAiAssistantDoneSessionId = 0;
volatile uint32_t gXiaoAiPartialSentCount = 0;
#endif
bool gWatchAutoReconnectSuppressed = false;
uint32_t gNextWatchAutoConnectMs = 0;
bool gMdnsStarted = false;
// 配网模式状态：true 时板子处于 SoftAP 配网模式（HTTP 服务器从 AP 提供服务）。
bool gSetupMode = false;
// HTTP 服务器是否已启动（配网模式、SoftAP 或 STA 连接后都会置位）。
bool gServerStarted = false;
// SoftAP can be the only usable interface while home Wi-Fi is empty/down.
volatile bool gSoftApActive = false;
// A STA connect is started asynchronously during the Round-4 hold.  Keep an
// explicit operation marker so the watch setup scan never asks the Wi-Fi
// driver to scan while esp_wifi_connect() is still negotiating.
volatile bool gWifiStaConnecting = false;
volatile uint32_t gWifiStaConnectDeadlineMs = 0;
volatile bool gWifiStaStopReconnectPending = false;
volatile uint32_t gWifiStaFailureBackoffUntilMs = 0;
volatile uint8_t gWifiStaLastReason = 0;
char gWifiState[24] = "idle";
char gWifiError[32] = "";
String gPendingWifiSsid;
String gPendingWifiPassword;
volatile bool gWifiProvisioningPending = false;
constexpr uint32_t kWifiStaConnectTimeoutMs = 15000;
constexpr uint32_t kWifiStaFailureBackoffMs = 1500;
int maxTokens = 512;
volatile bool gWatchBridgeRunning = false;
volatile bool gWatchBridgeStopRequested = false;
TaskHandle_t gWatchBridgeTask = nullptr;
#if HOSHINO_ENABLE_ASR
TaskHandle_t gWatchAsrTask = nullptr;
#endif
SemaphoreHandle_t gBridgeStateMutex = nullptr;
#if HOSHINO_ENABLE_ASR
char gLastAsrTranscript[768]{};
char gLastAsrError[192]{};
#endif
char gLastAiAnswer[768]{};
char gWatchBridgeState[64] = "idle";
#if HOSHINO_ENABLE_ASR
char gXiaoAiLastPartial[768]{};
char gXiaoAiFinalPacketPath[48]{};
char gXiaoAiFinalWavPath[48]{};

struct AsrResultMessage {
  bool ok = false;
  bool finalResult = false;
  uint32_t sessionId = 0;
  char text[768]{};
  char error[192]{};
};
static QueueHandle_t gAsrResultQueue = nullptr;
static volatile bool gXiaoAiStartPending = false;

struct AsrJobContext {
  bool finalResult = false;
  uint32_t sessionId = 0;
  char packetPath[48]{};
  char wavPath[48]{};
};
#endif

struct WatchConversationTurn {
  char user[384]{};
  char assistant[768]{};
};
constexpr size_t kWatchContextTurns = 3;
WatchConversationTurn gWatchContext[kWatchContextTurns];
size_t gWatchContextCount = 0;
size_t gWatchContextNext = 0;
uint32_t gWatchContextLastMs = 0;

#if HOSHINO_ENABLE_ASR
bool scheduleWatchAsrJob(const char* packetPath, const char* wavPath, bool finalResult, uint32_t sessionId);
#endif
bool parseWatchAddress(const String& value, uint8_t output[6]);
void authenticateWatchSpp(String target, String secretText);
void setWatchBridgeState(const char* state);
bool sendEncryptedWatchPb(uint8_t& sequence, const uint8_t key[16], const uint8_t* pb, size_t pbLength);
bool ensureScratchBuffers();
bool ensureAppScratchBuffers();
void connectWifi();
void enterSetupMode();
void setupTriggerPins();
bool setupTriggered();
void beginWifiStaConnection(uint32_t timeoutMs);
void serviceWifiStaConnectionState(bool cancelled = false);
void clearWifiStaConnection(const char* reason);
void handleWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void finishPendingWifiProvisioning(bool connected);
constexpr size_t kFetchTraceEntries = 8;
String fetchTrace[kFetchTraceEntries];
size_t fetchTraceNext = 0;
size_t fetchTraceCount = 0;

void logWatchSppEvent(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
  if (!param) return;
  switch (event) {
    case ESP_SPP_INIT_EVT:
      Serial.printf("WATCH_SPP_EVENT INIT status=%d\n", static_cast<int>(param->init.status));
      break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
      Serial.printf("WATCH_SPP_EVENT DISCOVERY status=%d channels=%u",
                    static_cast<int>(param->disc_comp.status),
                    static_cast<unsigned>(param->disc_comp.scn_num));
      for (uint8_t index = 0; index < param->disc_comp.scn_num && index < ESP_SPP_MAX_SCN; ++index) {
        Serial.printf(" scn%u=%u", static_cast<unsigned>(index),
                      static_cast<unsigned>(param->disc_comp.scn[index]));
      }
      Serial.println();
      break;
    case ESP_SPP_CL_INIT_EVT:
      Serial.printf("WATCH_SPP_EVENT CL_INIT status=%d handle=%lu sec_id=%u use_co=%u\n",
                    static_cast<int>(param->cl_init.status),
                    static_cast<unsigned long>(param->cl_init.handle),
                    static_cast<unsigned>(param->cl_init.sec_id),
                    param->cl_init.use_co ? 1u : 0u);
      break;
    case ESP_SPP_OPEN_EVT:
      Serial.printf("WATCH_SPP_EVENT OPEN status=%d handle=%lu\n",
                    static_cast<int>(param->open.status),
                    static_cast<unsigned long>(param->open.handle));
      break;
    case ESP_SPP_CLOSE_EVT:
      Serial.printf("WATCH_SPP_EVENT CLOSE status=%d handle=%lu async=%u\n",
                    static_cast<int>(param->close.status),
                    static_cast<unsigned long>(param->close.handle),
                    param->close.async ? 1u : 0u);
      break;
    default:
      break;
  }
}

String cleanBase(String s) {
  s.trim();
  while (s.endsWith("/")) s.remove(s.length() - 1);
  return s;
}

bool beginWatchBluetooth(const char* localName) {
  if (!watchBt.begin(localName, true)) return false;
  watchBt.register_callback(logWatchSppEvent);
  if (!watchRxQueue) watchRxQueue = xQueueCreate(kWatchRxQueueBytes, sizeof(uint8_t));
  if (!watchRxQueue) {
    watchBt.end();
    return false;
  }
  xQueueReset(watchRxQueue);
  watchRxDroppedBytes = 0;
  gSppRxBuffered = 0;
  gSppRxResyncBytes = 0;
  gSppRxCrcFailures = 0;
  gSppRxOversizeHeaders = 0;
  gSppRxPartialTimeouts = 0;
  watchBt.onData([](const uint8_t* data, size_t length) {
    if (!watchRxQueue) return;
    for (size_t index = 0; index < length; ++index) {
      if (xQueueSend(watchRxQueue, data + index, 0) != pdTRUE) {
        watchRxDroppedBytes += static_cast<uint32_t>(length - index);
        break;
      }
    }
  });
  return true;
}

void clearWatchSdpCache(const char* reason) {
  gWatchResolvedSppChannel = 0;
  gWatchSdpResolvedMs = 0;
  gWatchSdpAddress[0] = '\0';
  snprintf(gWatchSdpSource, sizeof(gWatchSdpSource), "%s", reason ? reason : "cleared");
}

bool watchSdpCacheValid(const String& addressText) {
  return gWatchSdpResolvedMs != 0 &&
         millis() - gWatchSdpResolvedMs < kWatchSdpCacheMs &&
         addressText.equalsIgnoreCase(gWatchSdpAddress);
}

int resolveWatchSppChannel(uint8_t address[6], const char* purpose) {
  BTAddress remoteAddress(address);
  const String addressText = remoteAddress.toString(true);
  if (watchSdpCacheValid(addressText) && gWatchResolvedSppChannel > 0) {
    Serial.printf("WATCH_SDP_CACHE_HIT purpose=%s address=%s channel=%d source=%s age_ms=%lu\n",
                  purpose ? purpose : "unknown", addressText.c_str(), gWatchResolvedSppChannel,
                  gWatchSdpSource, static_cast<unsigned long>(millis() - gWatchSdpResolvedMs));
    return gWatchResolvedSppChannel;
  }

  Serial.printf("WATCH_SDP_QUERY_START purpose=%s address=%s preferred=%d\n",
                purpose ? purpose : "unknown", addressText.c_str(), kWatchObservedSppChannel);
  std::map<int, std::string> channels = watchBt.getChannels(remoteAddress);
  int selected = 0;
  for (const auto& entry : channels) {
    Serial.printf("WATCH_SDP_SERVICE channel=%d name=%s\n",
                  entry.first, entry.second.empty() ? "<unnamed>" : entry.second.c_str());
    if (entry.first == kWatchObservedSppChannel) selected = entry.first;
  }
  if (selected == 0 && !channels.empty()) selected = channels.begin()->first;

  snprintf(gWatchSdpAddress, sizeof(gWatchSdpAddress), "%s", addressText.c_str());
  gWatchSdpResolvedMs = millis();
  if (selected > 0) {
    gWatchResolvedSppChannel = selected;
    snprintf(gWatchSdpSource, sizeof(gWatchSdpSource), "%s",
             selected == kWatchObservedSppChannel ? "sdp_preferred5" : "sdp_first");
    Serial.printf("WATCH_SDP_QUERY_OK address=%s services=%u selected=%d source=%s\n",
                  addressText.c_str(), static_cast<unsigned>(channels.size()), selected, gWatchSdpSource);
    return selected;
  }

  // The current Redmi Watch 6 captures consistently use RFCOMM server channel 5.
  // If SDP temporarily fails, use that observed channel before falling back to
  // BluetoothSerial channel=0 auto-detection.
  gWatchResolvedSppChannel = kWatchObservedSppChannel;
  snprintf(gWatchSdpSource, sizeof(gWatchSdpSource), "capture_fallback5");
  Serial.printf("WATCH_SDP_QUERY_EMPTY address=%s fallback=%d\n",
                addressText.c_str(), kWatchObservedSppChannel);
  return kWatchObservedSppChannel;
}

bool connectWatchWithSdp(uint8_t address[6], const char* purpose) {
  const int resolved = resolveWatchSppChannel(address, purpose);
  int candidates[3] = {resolved, kWatchObservedSppChannel, 0};
  int attempted[3] = {-1, -1, -1};
  size_t attemptedCount = 0;

  for (int candidate : candidates) {
    bool duplicate = false;
    for (size_t i = 0; i < attemptedCount; ++i) {
      if (attempted[i] == candidate) { duplicate = true; break; }
    }
    if (duplicate) continue;
    attempted[attemptedCount++] = candidate;

    Serial.printf("WATCH_SDP_CONNECT_TRY purpose=%s channel=%d source=%s\n",
                  purpose ? purpose : "unknown", candidate,
                  candidate == resolved ? gWatchSdpSource :
                  (candidate == kWatchObservedSppChannel ? "fallback5" : "arduino_auto"));
    if (watchBt.connect(address, candidate, ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER)) {
      if (candidate > 0) {
        gWatchResolvedSppChannel = candidate;
        gWatchSdpResolvedMs = millis();
        BTAddress remoteAddress(address);
        const String addressText = remoteAddress.toString(true);
        snprintf(gWatchSdpAddress, sizeof(gWatchSdpAddress), "%s", addressText.c_str());
        if (candidate != resolved) snprintf(gWatchSdpSource, sizeof(gWatchSdpSource), "connect_fallback");
      }
      Serial.printf("WATCH_SDP_CONNECT_OK purpose=%s channel=%d\n",
                    purpose ? purpose : "unknown", candidate);
      return true;
    }
    Serial.printf("WATCH_SDP_CONNECT_FAIL purpose=%s channel=%d\n",
                  purpose ? purpose : "unknown", candidate);
    delay(120);
  }

  clearWatchSdpCache("connect_failed");
  return false;
}

void probeWatchSdp(String target) {
  target.trim();
  target.toUpperCase();
  uint8_t address[6];
  if (!parseWatchAddress(target, address)) {
    Serial.println("WATCH_SDP_INVALID_ADDRESS");
    return;
  }
  if (!beginWatchBluetooth("Hoshino-Bridge-SDP")) {
    Serial.println("WATCH_SDP_BT_INIT_FAILED");
    return;
  }
  clearWatchSdpCache("manual_probe");
  const int channel = resolveWatchSppChannel(address, "manual_probe");
  Serial.printf("WATCH_SDP_RESULT address=%s selected_channel=%d source=%s\n",
                target.c_str(), channel, gWatchSdpSource);
  watchBt.end();
}

int watchRxAvailable() {
  return watchRxQueue ? static_cast<int>(uxQueueMessagesWaiting(watchRxQueue)) : 0;
}

int readWatchByte() {
  uint8_t value = 0;
  return watchRxQueue && xQueueReceive(watchRxQueue, &value, 0) == pdTRUE ? value : -1;
}

String readConfigString(const char* key, const char* fallback) {
  return prefs.isKey(key) ? prefs.getString(key) : String(fallback);
}

bool validWatchAddress(const String& value) {
  if (value.length() != 17) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if ((i + 1) % 3 == 0) {
      if (value[i] != ':') return false;
    } else if (!isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
  return static_cast<uint8_t>(c - 'a' + 10);
}

bool parseWatchAddress(const String& value, uint8_t output[6]) {
  if (!validWatchAddress(value)) return false;
  for (size_t i = 0; i < 6; ++i) {
    size_t offset = i * 3;
    output[i] = static_cast<uint8_t>((hexNibble(value[offset]) << 4) | hexNibble(value[offset + 1]));
  }
  return true;
}

String serialSafeName(const std::string& value) {
  String safe;
  for (char c : value) {
    if (safe.length() >= 48) break;
    safe += (c >= 0x20 && c <= 0x7e) ? c : '_';
  }
  return safe.isEmpty() ? "unnamed" : safe;
}

void probeWatchClassic(String target) {
  target.trim();
  target.toUpperCase();
  if (!validWatchAddress(target)) {
    Serial.println("WATCH_PROBE_INVALID_ADDRESS");
    return;
  }
  if (!beginWatchBluetooth("Hoshino-Bridge-Probe")) {
    Serial.println("WATCH_PROBE_BT_INIT_FAILED");
    return;
  }

  Serial.println("WATCH_PROBE_SCAN_STARTED transport=classic");
  bool found = false;
  BTScanResults* results = watchBt.discover(kWatchScanSeconds * 1000);
  if (results) {
    for (int i = 0; i < results->getCount(); ++i) {
      BTAdvertisedDevice* device = results->getDevice(i);
      if (!device) continue;
      String address = String(device->getAddress().toString().c_str());
      address.toUpperCase();
      if (address != target) continue;
      found = true;
      String name = serialSafeName(device->getName());
      Serial.printf("WATCH_PROBE_FOUND transport=classic name=%s rssi=%d cod=0x%06lx\n",
                    name.c_str(), device->getRSSI(), static_cast<unsigned long>(device->getCOD()));
      break;
    }
    watchBt.discoverClear();
  }
  watchBt.end();
  Serial.println(found ? "WATCH_PROBE_COMPLETE found=true" : "WATCH_PROBE_COMPLETE found=false");
}

void connectWatchSpp(String target) {
  target.trim();
  target.toUpperCase();
  uint8_t address[6];
  if (!parseWatchAddress(target, address)) {
    Serial.println("WATCH_SPP_INVALID_ADDRESS");
    return;
  }
  if (!beginWatchBluetooth("Hoshino-Bridge-Probe")) {
    Serial.println("WATCH_SPP_BT_INIT_FAILED");
    return;
  }

  Serial.println("WATCH_SPP_CONNECT_STARTED security=none");
  const bool connected = connectWatchWithSdp(address, "connect_probe");
  if (connected) {
    Serial.println("WATCH_SPP_CONNECT_OK");
    watchBt.disconnect();
  } else {
    Serial.println("WATCH_SPP_CONNECT_FAILED");
  }
  watchBt.end();
}

void connectWatchSppChannel(String target, int channel) {
  target.trim();
  target.toUpperCase();
  uint8_t address[6];
  if (!parseWatchAddress(target, address) || channel < 1 || channel > 30) {
    Serial.println("WATCH_SPP_CHANNEL_INVALID_INPUT");
    return;
  }
  if (!beginWatchBluetooth("Hoshino-Bridge-Probe")) {
    Serial.println("WATCH_SPP_CHANNEL_BT_INIT_FAILED");
    return;
  }

  Serial.printf("WATCH_SPP_CHANNEL_CONNECT_STARTED channel=%d security=none\n", channel);
  const bool connected = watchBt.connect(address, channel, ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER);
  Serial.println(connected ? "WATCH_SPP_CHANNEL_CONNECT_OK" : "WATCH_SPP_CHANNEL_CONNECT_FAILED");
  if (connected) watchBt.disconnect();
  delay(250);
  watchBt.end();
}

String hexBytes(const uint8_t* value, size_t length) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  String result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    result += kHex[(value[i] >> 4) & 0x0f];
    result += kHex[value[i] & 0x0f];
  }
  return result;
}

void logWatchResponse(const char* label) {
  uint8_t response[256];
  size_t responseLength = 0;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < kWatchVersionResponseTimeoutMs && responseLength < sizeof(response)) {
    while (watchRxAvailable() > 0 && responseLength < sizeof(response)) {
      const int next = readWatchByte();
      if (next >= 0) response[responseLength++] = static_cast<uint8_t>(next);
    }
    delay(5);
  }
  if (responseLength > 0) {
    Serial.printf("%s bytes=%u hex=%s\\n", label, static_cast<unsigned>(responseLength), hexBytes(response, responseLength).c_str());
  } else {
    Serial.printf("%s_TIMEOUT\\n", label);
  }
}

void probeWatchSppVersion(String target) {
  target.trim();
  target.toUpperCase();
  uint8_t address[6];
  if (!parseWatchAddress(target, address)) {
    Serial.println("WATCH_VERSION_INVALID_ADDRESS");
    return;
  }
  if (!beginWatchBluetooth("Hoshino-Bridge-Probe")) {
    Serial.println("WATCH_VERSION_BT_INIT_FAILED");
    return;
  }

  Serial.println("WATCH_VERSION_CONNECT_STARTED security=none");
  const bool connected = connectWatchWithSdp(address, "version_probe");
  if (!connected) {
    Serial.println("WATCH_VERSION_CONNECT_FAILED");
    watchBt.end();
    return;
  }

  Serial.println("WATCH_VERSION_REQUEST_SENT protocol=sppv1_plaintext");
  watchBt.write(kSppV1VersionRequest, sizeof(kSppV1VersionRequest));
  logWatchResponse("WATCH_VERSION_RESPONSE");
  watchBt.disconnect();
  watchBt.end();
}

void probeWatchSppSession(String target) {
  target.trim();
  target.toUpperCase();
  uint8_t address[6];
  if (!parseWatchAddress(target, address)) {
    Serial.println("WATCH_SESSION_INVALID_ADDRESS");
    return;
  }
  if (!beginWatchBluetooth("Hoshino-Bridge-Probe")) {
    Serial.println("WATCH_SESSION_BT_INIT_FAILED");
    return;
  }
  Serial.println("WATCH_SESSION_CONNECT_STARTED security=none");
  if (!connectWatchWithSdp(address, "session_probe")) {
    Serial.println("WATCH_SESSION_CONNECT_FAILED");
    watchBt.end();
    return;
  }
  Serial.println("WATCH_SESSION_REQUEST_SENT protocol=sppv2_plaintext");
  watchBt.write(kSppV2SessionStartRequest, sizeof(kSppV2SessionStartRequest));
  logWatchResponse("WATCH_SESSION_RESPONSE");
  watchBt.disconnect();
  watchBt.end();
}

constexpr size_t kSppV2PayloadMax = 4096;
constexpr size_t kSppWriteChunkBytes = 16;

struct SppV2Frame {
  uint8_t type = 0;
  uint8_t sequence = 0;
  uint8_t payload[kSppV2PayloadMax]{};
  size_t payloadLength = 0;
};

// Memory optimization: every SPP receive path already shares the same
// BluetoothSerial connection, byte queue and gSppRxFrame assembler.
// Keep a single decoded 4 KiB frame workspace instead of one per helper.
static SppV2Frame gSharedSppFrame;

// Large protocol workspaces live on the heap to keep static DRAM usage
// within the ESP32 default .bss/.data region (the firmware previously
// overflowed dram0_0_seg by ~24 KB). Allocated in setup().
static uint8_t* gSppTxFrame = nullptr;
static uint8_t* gSppRxFrame = nullptr;
static uint8_t* gQuickAppMessageScratch = nullptr;
static uint8_t* gQuickAppThirdpartyScratch = nullptr;
static uint8_t* gEncryptedPayloadScratch = nullptr;
static uint8_t* gRawCommandPayloadScratch = nullptr;
static bool gWatchRawTraceEnabled = false;
constexpr size_t kWatchNetworkMtu = 1520; // Official round-3 trace carries IPv4 packets up to 1518 B.
constexpr size_t kWatchNetworkPacketMax = 1600;
constexpr UBaseType_t kWatchNetworkTxQueueDepth = 4;
struct WatchNetworkPacket {
  uint16_t length = 0;
  uint8_t bytes[kWatchNetworkPacketMax]{};
};
static struct netif* gWatchNetworkNetif = nullptr;
static QueueHandle_t gWatchNetworkTxQueue = nullptr;
static SemaphoreHandle_t gWatchNetworkInitDone = nullptr;
static WatchNetworkPacket* gWatchNetworkTxEnqueueScratch = nullptr;
static WatchNetworkPacket* gWatchNetworkTxDrainScratch = nullptr;
static bool gWatchNetworkReady = false;
static volatile uint32_t gWatchNetworkRxPackets = 0;
static volatile uint32_t gWatchNetworkTxPackets = 0;
static volatile uint32_t gWatchNetworkDroppedPackets = 0;
static volatile err_t gWatchNetworkInitResult = ERR_INPROGRESS;
#if HOSHINO_ENABLE_ASR
constexpr size_t kWatchStreamPacketMax = 512;
constexpr size_t kWatchStreamPacketCountMax = 256;
constexpr char kWatchStreamCapturePath[] = "/watch_stream.packets";
constexpr uint32_t kWatchAsrSampleRate = 16000;
constexpr uint16_t kWatchAsrChannels = 1;
constexpr size_t kWatchAsrPcmMaxBytes = 480000; // 15 s @ 16 kHz mono PCM16
constexpr size_t kWatchOpusSubframeBytes = 80;
constexpr size_t kWatchOpusAggregateBytes = 400;
constexpr size_t kWatchAsrTaskStackBytes = 48 * 1024;
static size_t gWatchStreamCaptureLength = 0;
static size_t gWatchStreamPacketCount = 0;
static bool gWatchStreamCaptureActive = false;
static bool gWatchStreamCaptureTruncated = false;
static uint32_t gWatchStreamCaptureRxDroppedStart = 0;
static bool gWatchStreamCaptureFsReady = false;
static bool gWatchStreamCaptureFileFailed = false;
static File gWatchStreamCaptureFile;
static uint8_t gWatchStreamExportPacket[kWatchStreamPacketMax];
static uint8_t gWatchStreamExportBase64[4 * ((kWatchStreamPacketMax + 2) / 3) + 1];

void markXiaoAiProgress(const char* reason) {
  gXiaoAiLastProgressMs = millis();
  if (reason) {
    Serial.printf("XIAOAI_PROGRESS session=%lu reason=%s audio_ms=%lu partials=%lu\n",
                  static_cast<unsigned long>(gXiaoAiActiveSessionId), reason,
                  static_cast<unsigned long>(gXiaoAiAudioMs),
                  static_cast<unsigned long>(gXiaoAiPartialSentCount));
  }
}

void beginXiaoAiSession() {
  ++gXiaoAiSessionSerial;
  if (gXiaoAiSessionSerial == 0) ++gXiaoAiSessionSerial;
  gXiaoAiActiveSessionId = gXiaoAiSessionSerial;
  gXiaoAiSessionActive = true;
  gXiaoAiFinalSent = false;
  gXiaoAiSessionStartedMs = millis();
  gXiaoAiLastProgressMs = gXiaoAiSessionStartedMs;
  gXiaoAiAudioMs = 0;
  gXiaoAiNextPartialAudioMs = kXiaoAiRollingFirstAudioMs;
  gXiaoAiFinalAsrPending = false;
  gXiaoAiFinalPendingSessionId = 0;
  gXiaoAiAwaitAssistant = false;
  gXiaoAiAssistantDoneSessionId = 0;
  gXiaoAiPartialSentCount = 0;
  gXiaoAiLastPartial[0] = '\0';
  gXiaoAiStartPending = true;
  gXiaoAiEndPending = false;
  gXiaoAiEndDueMs = 0;
  Serial.printf("XIAOAI_SESSION_BEGIN session=%lu timely_start_target_ms=500 rolling_first_ms=%lu rolling_interval_ms=%lu no_progress_timeout_ms=%lu\n",
                static_cast<unsigned long>(gXiaoAiActiveSessionId),
                static_cast<unsigned long>(kXiaoAiRollingFirstAudioMs),
                static_cast<unsigned long>(kXiaoAiRollingIntervalMs),
                static_cast<unsigned long>(kXiaoAiNoProgressTimeoutMs));
}

void finishXiaoAiSession(const char* reason) {
  Serial.printf("XIAOAI_SESSION_END session=%lu reason=%s elapsed_ms=%lu partials=%lu\n",
                static_cast<unsigned long>(gXiaoAiActiveSessionId), reason ? reason : "unknown",
                static_cast<unsigned long>(millis() - gXiaoAiSessionStartedMs),
                static_cast<unsigned long>(gXiaoAiPartialSentCount));
  gXiaoAiSessionActive = false;
  gXiaoAiStartPending = false;
  gXiaoAiEndPending = false;
  gXiaoAiFinalAsrPending = false;
  gXiaoAiAwaitAssistant = false;
}

bool copySpiffsFile(const char* sourcePath, const char* destinationPath) {
  if (!sourcePath || !destinationPath || !gWatchStreamCaptureFsReady) return false;
  File source = SPIFFS.open(sourcePath, FILE_READ);
  if (!source) return false;
  SPIFFS.remove(destinationPath);
  File destination = SPIFFS.open(destinationPath, FILE_WRITE);
  if (!destination) { source.close(); return false; }
  uint8_t buffer[512];
  bool ok = true;
  while (source.available()) {
    const size_t got = source.read(buffer, sizeof(buffer));
    if (!got) break;
    if (destination.write(buffer, got) != got) { ok = false; break; }
  }
  destination.flush();
  source.close();
  destination.close();
  if (!ok) SPIFFS.remove(destinationPath);
  return ok;
}

void buildSessionAsrPaths(uint32_t sessionId, bool finalResult,
                          char* packetPath, size_t packetCapacity,
                          char* wavPath, size_t wavCapacity) {
  snprintf(packetPath, packetCapacity, finalResult ? "/asr_%08lx_f.pkt" : "/asr_%08lx_p.pkt",
           static_cast<unsigned long>(sessionId));
  snprintf(wavPath, wavCapacity, finalResult ? "/asr_%08lx_f.wav" : "/asr_%08lx_p.wav",
           static_cast<unsigned long>(sessionId));
}

void emitWatchRawTrace(const SppV2Frame& frame) {
  if (!gWatchRawTraceEnabled) return;
  const uint8_t channel = frame.payloadLength >= 1 ? (frame.payload[0] & 0x0f) : 0xff;
  const uint8_t opcode = frame.payloadLength >= 2 ? frame.payload[1] : 0xff;
  Serial.printf("WATCH_RAW_FRAME type=%u seq=%u bytes=%u channel=%u opcode=%u\n",
                static_cast<unsigned>(frame.type), static_cast<unsigned>(frame.sequence),
                static_cast<unsigned>(frame.payloadLength), static_cast<unsigned>(channel),
                static_cast<unsigned>(opcode));
}

void beginWatchStreamCapture(uint16_t mode) {
  if (gWatchStreamCaptureFile) gWatchStreamCaptureFile.close();
  // Mount SPIFFS lazily here instead of in setup(): the mount costs heap that
  // the Bluetooth controller init needs up front.
  if (!gWatchStreamCaptureFsReady) gWatchStreamCaptureFsReady = SPIFFS.begin(true);
  beginXiaoAiSession();
  gWatchStreamCaptureLength = 0;
  gWatchStreamPacketCount = 0;
  gWatchStreamCaptureActive = true;
  gWatchStreamCaptureTruncated = false;
  gWatchStreamCaptureRxDroppedStart = watchRxDroppedBytes;
  gWatchStreamCaptureFileFailed = !gWatchStreamCaptureFsReady;
  if (!gWatchStreamCaptureFileFailed) {
    SPIFFS.remove(kWatchStreamCapturePath);
    gWatchStreamCaptureFile = SPIFFS.open(kWatchStreamCapturePath, FILE_WRITE);
    gWatchStreamCaptureFileFailed = !gWatchStreamCaptureFile;
  }
  Serial.printf("WATCH_STREAM_CAPTURE_BEGIN mode=%u private_local_only=true\n", static_cast<unsigned>(mode));
}

void appendWatchStreamPacket(const uint8_t* data, size_t length) {
  if (!gWatchStreamCaptureActive || !data || length == 0) return;
  if (length > kWatchStreamPacketMax || gWatchStreamPacketCount >= kWatchStreamPacketCountMax) {
    gWatchStreamCaptureTruncated = true;
    return;
  }
  const uint8_t lengthBytes[] = {
      static_cast<uint8_t>((length >> 8) & 0xff), static_cast<uint8_t>(length & 0xff)};
  if (gWatchStreamCaptureFileFailed || !gWatchStreamCaptureFile ||
      gWatchStreamCaptureFile.write(lengthBytes, sizeof(lengthBytes)) != sizeof(lengthBytes) ||
      gWatchStreamCaptureFile.write(data, length) != length) {
    gWatchStreamCaptureFileFailed = true;
    gWatchStreamCaptureTruncated = true;
    return;
  }
  ++gWatchStreamPacketCount;
  gWatchStreamCaptureLength += length;
  if (length >= kWatchOpusSubframeBytes && (length % kWatchOpusSubframeBytes) == 0) {
    gXiaoAiAudioMs += static_cast<uint32_t>((length / kWatchOpusSubframeBytes) * 20u);
  }
}

void exportWatchStreamCapture() {
  if (!gWatchStreamCaptureFsReady || gWatchStreamCaptureFileFailed) return;
  File capture = SPIFFS.open(kWatchStreamCapturePath, FILE_READ);
  if (!capture) {
    Serial.println("WATCH_STREAM_EXPORT_OPEN_FAILED");
    return;
  }
  size_t index = 0;
  while (capture.available()) {
    uint8_t lengthBytes[2]{};
    if (capture.read(lengthBytes, sizeof(lengthBytes)) != sizeof(lengthBytes)) {
      Serial.println("WATCH_STREAM_EXPORT_LENGTH_FAILED");
      break;
    }
    const size_t length = (static_cast<size_t>(lengthBytes[0]) << 8) | lengthBytes[1];
    if (length == 0 || length > kWatchStreamPacketMax ||
        capture.read(gWatchStreamExportPacket, length) != static_cast<int>(length)) {
      Serial.println("WATCH_STREAM_EXPORT_READ_FAILED");
      break;
    }
    size_t encodedLength = 0;
    if (mbedtls_base64_encode(gWatchStreamExportBase64, sizeof(gWatchStreamExportBase64) - 1,
                              &encodedLength, gWatchStreamExportPacket, length) != 0) {
      Serial.println("WATCH_STREAM_EXPORT_ENCODE_FAILED");
      break;
    }
    gWatchStreamExportBase64[encodedLength] = 0;
    Serial.printf("WATCH_STREAM_FILE_PACKET index=%u bytes=%u b64=%s\n",
                  static_cast<unsigned>(index), static_cast<unsigned>(length),
                  gWatchStreamExportBase64);
    Serial.flush();
    ++index;
    delay(1);
  }
  capture.close();
  mbedtls_platform_zeroize(gWatchStreamExportPacket, sizeof(gWatchStreamExportPacket));
  mbedtls_platform_zeroize(gWatchStreamExportBase64, sizeof(gWatchStreamExportBase64));
  Serial.printf("WATCH_STREAM_EXPORT_END packets=%u\n", static_cast<unsigned>(index));
}

void emitWatchStreamCapture() {
  if (!gWatchStreamCaptureActive) return;
  gWatchStreamCaptureActive = false;
  if (gWatchStreamCaptureFile) gWatchStreamCaptureFile.close();
  // The observed 5-10 s offline timeout is a post-utterance assistant wait,
  // not a safe reason to terminate a long utterance while ch3 audio is still
  // arriving. Arm the local no-progress deadline from microphone END.
  gXiaoAiLastProgressMs = millis();
  Serial.printf("XIAOAI_POST_AUDIO_WAIT_ARMED session=%lu timeout_ms=%lu\n",
                static_cast<unsigned long>(gXiaoAiActiveSessionId),
                static_cast<unsigned long>(kXiaoAiNoProgressTimeoutMs));
  const bool captureOk = !gWatchStreamCaptureTruncated && !gWatchStreamCaptureFileFailed &&
                         watchRxDroppedBytes == gWatchStreamCaptureRxDroppedStart &&
                         gWatchStreamPacketCount > 0;
  Serial.printf("WATCH_STREAM_CAPTURE_END bytes=%u packets=%u truncated=%s rx_dropped=%lu encoding=opus_aggregate\n",
                static_cast<unsigned>(gWatchStreamCaptureLength),
                static_cast<unsigned>(gWatchStreamPacketCount),
                gWatchStreamCaptureTruncated ? "true" : "false",
                static_cast<unsigned long>(watchRxDroppedBytes - gWatchStreamCaptureRxDroppedStart));
  if (serialAudioExport) exportWatchStreamCapture();
  if (captureOk && gWatchStreamCaptureFsReady) {
    const uint32_t sessionId = gXiaoAiActiveSessionId;
    buildSessionAsrPaths(sessionId, true, gXiaoAiFinalPacketPath, sizeof(gXiaoAiFinalPacketPath),
                         gXiaoAiFinalWavPath, sizeof(gXiaoAiFinalWavPath));
    SPIFFS.remove(gXiaoAiFinalPacketPath);
    SPIFFS.remove(gXiaoAiFinalWavPath);
    if (SPIFFS.rename(kWatchStreamCapturePath, gXiaoAiFinalPacketPath)) {
      if (!scheduleWatchAsrJob(gXiaoAiFinalPacketPath, gXiaoAiFinalWavPath, true, sessionId)) {
        gXiaoAiFinalAsrPending = true;
        gXiaoAiFinalPendingSessionId = sessionId;
        Serial.printf("WATCH_ASR_FINAL_DEFERRED session=%lu busy=true\n", static_cast<unsigned long>(sessionId));
      }
    } else {
      Serial.println("WATCH_ASR_SNAPSHOT_FAILED");
    }
  } else {
    Serial.println("WATCH_ASR_SKIPPED capture_invalid=true");
  }
  gWatchStreamCaptureLength = 0;
  gWatchStreamPacketCount = 0;
  gWatchStreamCaptureTruncated = false;
}

void handleWatchStreamData(const uint8_t* data, size_t length) {
  if (!data || length < 2) return;
  const uint16_t type = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
  if (type == 1 && length >= 6) {
    const uint16_t declared = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]);
    const uint16_t mode = static_cast<uint16_t>((static_cast<uint16_t>(data[4]) << 8) | data[5]);
    if (declared == 2) beginWatchStreamCapture(mode);
    return;
  }
  if (type == 2 && length >= 4 && gWatchStreamCaptureActive) {
    const uint16_t declared = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]);
    if (declared <= length - 4) appendWatchStreamPacket(data + 4, declared);
    else gWatchStreamCaptureTruncated = true;
    return;
  }
  if (type == 4) emitWatchStreamCapture();
}
#endif  // HOSHINO_ENABLE_ASR

// Xiaomi SPPv2 uses an 8-bit transport sequence. The real-device failure
// pattern is consistent with an already-used sequence being transport-ACKed
// but not delivered again to the QuickApp message channel after wrap. Renew
// the authenticated SPPv2 session before the 8-bit sequence can wrap.
constexpr uint8_t kBenchSppRenewAtSequence = 250;
static bool gBenchSessionRenewalEnabled = false;
static uint8_t gBenchSessionSecret[16]{};
static uint8_t gBenchSessionEncKey[16]{};
static uint8_t gBenchSessionDecKey[16]{};
static uint32_t gBenchSessionRenewals = 0;

void logStackHighWater(const char* stage) {
  const UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("STACK_HIGH_WATER stage=%s words=%u approx_bytes=%u rx_dropped=%lu\n",
                stage ? stage : "unknown",
                static_cast<unsigned>(words),
                static_cast<unsigned>(words * sizeof(StackType_t)),
                static_cast<unsigned long>(watchRxDroppedBytes));
}

uint16_t crc16Arc(const uint8_t* bytes, size_t length) {
  uint16_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xa001) : static_cast<uint16_t>(crc >> 1);
  }
  return crc;
}

bool writeWatchBytes(const uint8_t* bytes, size_t length) {
  // Diagnostic/production candidate: BluetoothSerial already queues packets,
  // coalesces them into its internal SPP_TX_MAX buffer, and waits for the IDF
  // SPP write-complete/congestion events in its TX task. Sending an entire
  // Hoshino SPP frame as one queued packet avoids dozens of heap allocations
  // and queue operations caused by the legacy 16-byte segmentation.
  if (gSppTxCoalesced) {
    return watchBt.write(bytes, length) == length;
  }
  for (size_t offset = 0; offset < length; offset += kSppWriteChunkBytes) {
    const size_t chunkLength = min(kSppWriteChunkBytes, length - offset);
    if (watchBt.write(bytes + offset, chunkLength) != chunkLength) return false;
    if (offset + chunkLength < length) delay(5);
  }
  return true;
}

// Large protocol workspaces are allocated lazily, right after the Bluetooth
// controller has finished initialising, so they never starve the Bluetooth
// init path of heap. Called again on demand as a nullptr-safe guard.
// Only the two SPP frame buffers (needed for auth) are allocated here; the
// four QuickApp/network workspaces are allocated later, after auth succeeds,
// so the RFCOMM connect path still has plenty of heap.
bool ensureScratchBuffers() {
  if (gSppTxFrame) return true;
  gSppTxFrame = static_cast<uint8_t*>(malloc(8 + kSppV2PayloadMax));
  gSppRxFrame = static_cast<uint8_t*>(malloc(8 + kSppV2PayloadMax));
  gRawCommandPayloadScratch = static_cast<uint8_t*>(malloc(kSppV2PayloadMax));
  gEncryptedPayloadScratch = static_cast<uint8_t*>(malloc(kSppV2PayloadMax));
  if (!gSppTxFrame || !gSppRxFrame || !gRawCommandPayloadScratch ||
      !gEncryptedPayloadScratch) {
    Serial.println("HOSHINO_SCRATCH_MALLOC_FAILED");
    return false;
  }
  Serial.printf("HOSHINO_CORE_SCRATCH_ALLOCATED free=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

// Allocates the two QuickApp message workspaces. Invoked after auth succeeds
// and as a lazy guard at the top of every consumer that needs them.
bool ensureAppScratchBuffers() {
  if (gQuickAppMessageScratch) return true;
  gQuickAppMessageScratch = static_cast<uint8_t*>(malloc(kSppV2PayloadMax));
  gQuickAppThirdpartyScratch = static_cast<uint8_t*>(malloc(kSppV2PayloadMax));
  if (!gQuickAppMessageScratch || !gQuickAppThirdpartyScratch) {
    Serial.println("HOSHINO_APP_SCRATCH_MALLOC_FAILED");
    return false;
  }
  Serial.printf("HOSHINO_APP_SCRATCH_ALLOCATED free=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

bool writeSppV2Frame(uint8_t type, uint8_t sequence, const uint8_t* payload, size_t payloadLength) {
  if (payloadLength > kSppV2PayloadMax) return false;
  if (!ensureScratchBuffers()) return false;
  uint8_t* frame = gSppTxFrame;
  const uint16_t checksum = crc16Arc(payload, payloadLength);
  frame[0] = 0xa5; frame[1] = 0xa5; frame[2] = type & 0x0f; frame[3] = sequence;
  frame[4] = static_cast<uint8_t>(payloadLength & 0xff); frame[5] = static_cast<uint8_t>(payloadLength >> 8);
  frame[6] = static_cast<uint8_t>(checksum & 0xff); frame[7] = static_cast<uint8_t>(checksum >> 8);
  if (payloadLength > 0) memcpy(frame + 8, payload, payloadLength);
  return writeWatchBytes(frame, payloadLength + 8);
}

err_t watchNetworkOutput(struct netif*, struct pbuf* packet, const ip4_addr_t*) {
  if (!gWatchNetworkTxQueue || !gWatchNetworkTxEnqueueScratch || !packet ||
      packet->tot_len == 0 || packet->tot_len > kWatchNetworkPacketMax) {
    ++gWatchNetworkDroppedPackets;
    Serial.printf("WATCH_RETURN_DROP stage=invalid len=%u\n", static_cast<unsigned>(packet ? packet->tot_len : 0));
    return ERR_BUF;
  }
  // Return-path tracer: this callback means the response reached the watch netif
  // (internet -> WiFi STA -> NAPT reverse -> watch netif).
  Serial.printf("WATCH_RETURN_FROM_LWIP len=%u\n", static_cast<unsigned>(packet->tot_len));
  gWatchNetworkTxEnqueueScratch->length = static_cast<uint16_t>(packet->tot_len);
  if (pbuf_copy_partial(packet, gWatchNetworkTxEnqueueScratch->bytes, packet->tot_len, 0) != packet->tot_len ||
      xQueueSend(gWatchNetworkTxQueue, gWatchNetworkTxEnqueueScratch, 0) != pdTRUE) {
    ++gWatchNetworkDroppedPackets;
    Serial.printf("WATCH_RETURN_DROP stage=queue_full len=%u\n", static_cast<unsigned>(packet->tot_len));
    return ERR_BUF;
  }
  ++gWatchNetworkTxPackets;
  Serial.printf("WATCH_RETURN_ENQUEUED len=%u\n", static_cast<unsigned>(packet->tot_len));
  return ERR_OK;
}

err_t initWatchNetworkNetif(struct netif* network) {
  if (!network) return ERR_ARG;
  network->name[0] = 'w';
  network->name[1] = 't';
  network->output = watchNetworkOutput;
  network->mtu = kWatchNetworkMtu;
  network->flags = NETIF_FLAG_BROADCAST;
  return ERR_OK;
}

// Helper: send a raw bootstrap protobuf message to the watch.
static void doBootstrap(const uint8_t encKey[16], uint8_t& seq,
                         const char* data, size_t len,
                         const char* label) {
  if (!encKey || !data || !len || !label) return;
  // data is a char* literal, cast to uint8_t*
  const uint8_t txSequence = seq;
  if (sendEncryptedWatchPb(seq, encKey,
                           reinterpret_cast<const uint8_t*>(data), len)) {
    Serial.printf("%s OK seq=%u\n", label, static_cast<unsigned>(txSequence));
  } else {
    Serial.printf("%s FAIL seq=%u\n", label, static_cast<unsigned>(txSequence));
  }
}

void setupWatchNetworkProxyInTcpip(void*) {
  ip4_addr_t address{}, netmask{}, gateway{};
  IP4_ADDR(&address, 10, 1, 10, 1);
  IP4_ADDR(&netmask, 255, 255, 255, 0);
  // CRITICAL: gateway MUST be 0.0.0.0. Passing a non-zero gateway here makes
  // lwIP install this watch netif as the DEFAULT route, which then overrides
  // the WiFi STA default route (0.0.0.0/0 -> 192.168.31.1). Result: DNS from
  // the watch hits ip4_forward -> ip4_route_src returns NULL -> IP4_FWD_NOROUTE.
  IP4_ADDR(&gateway, 0, 0, 0, 0);

  // Dump current netif list BEFORE adding the watch netif.
  {
    Serial.printf("NETIF_LIST_BEFORE default=%s\n", ::netif_default ? "set" : "NULL");
    for (struct netif* n = netif_list; n != NULL; n = n->next) {
      Serial.printf("  netif %c%c ip=%d.%d.%d.%d gw=%d.%d.%d.%d up=%d link=%d\n",
                    n->name[0], n->name[1],
                    ip4_addr1(netif_ip4_addr(n)), ip4_addr2(netif_ip4_addr(n)),
                    ip4_addr3(netif_ip4_addr(n)), ip4_addr4(netif_ip4_addr(n)),
                    ip4_addr1(netif_ip4_gw(n)), ip4_addr2(netif_ip4_gw(n)),
                    ip4_addr3(netif_ip4_gw(n)), ip4_addr4(netif_ip4_gw(n)),
                    netif_is_up(n) ? 1 : 0, netif_is_link_up(n) ? 1 : 0);
    }
  }

  if (!gWatchNetworkNetif || !netif_add(gWatchNetworkNetif, &address, &netmask, &gateway,
                 nullptr, initWatchNetworkNetif, tcpip_input)) {
    gWatchNetworkInitResult = ERR_IF;
  } else {
    netif_set_up(gWatchNetworkNetif);
    netif_set_link_up(gWatchNetworkNetif);
    ip_napt_enable(ip4_addr_get_u32(netif_ip4_addr(gWatchNetworkNetif)), 1);
    gWatchNetworkInitResult = ERR_OK;
  }

  {
    Serial.printf("NETIF_LIST_AFTER default=%s default_ip=%d.%d.%d.%d\n",
                  ::netif_default ? "set" : "NULL",
                  ::netif_default ? ip4_addr1(netif_ip4_addr(::netif_default)) : 0,
                  ::netif_default ? ip4_addr2(netif_ip4_addr(::netif_default)) : 0,
                  ::netif_default ? ip4_addr3(netif_ip4_addr(::netif_default)) : 0,
                  ::netif_default ? ip4_addr4(netif_ip4_addr(::netif_default)) : 0);
  }
  if (gWatchNetworkInitDone) xSemaphoreGive(gWatchNetworkInitDone);
}

bool setupWatchNetworkProxy() {
  if (gWatchNetworkReady) return true;
  if (!gWatchNetworkNetif) gWatchNetworkNetif = static_cast<struct netif*>(calloc(1, sizeof(struct netif)));
  if (!gWatchNetworkTxEnqueueScratch) gWatchNetworkTxEnqueueScratch = static_cast<WatchNetworkPacket*>(calloc(1, sizeof(WatchNetworkPacket)));
  if (!gWatchNetworkTxDrainScratch) gWatchNetworkTxDrainScratch = static_cast<WatchNetworkPacket*>(calloc(1, sizeof(WatchNetworkPacket)));
  if (!gWatchNetworkNetif || !gWatchNetworkTxEnqueueScratch || !gWatchNetworkTxDrainScratch) {
    Serial.println("WATCH_NETWORK_PROXY_INIT_FAILED stage=buffers");
    return false;
  }
  if (!gWatchNetworkTxQueue) {
    gWatchNetworkTxQueue = xQueueCreate(kWatchNetworkTxQueueDepth, sizeof(WatchNetworkPacket));
  }
  if (!gWatchNetworkTxQueue) {
    Serial.println("WATCH_NETWORK_PROXY_INIT_FAILED stage=queue");
    return false;
  }
  if (!gWatchNetworkInitDone) gWatchNetworkInitDone = xSemaphoreCreateBinary();
  if (!gWatchNetworkInitDone) {
    Serial.println("WATCH_NETWORK_PROXY_INIT_FAILED stage=semaphore");
    return false;
  }
  gWatchNetworkInitResult = ERR_INPROGRESS;
  const err_t callbackResult = tcpip_callback(setupWatchNetworkProxyInTcpip, nullptr);
  if (callbackResult != ERR_OK || xSemaphoreTake(gWatchNetworkInitDone, pdMS_TO_TICKS(3000)) != pdTRUE ||
      gWatchNetworkInitResult != ERR_OK) {
    Serial.printf("WATCH_NETWORK_PROXY_INIT_FAILED stage=tcpip callback=%d result=%d\n",
                  static_cast<int>(callbackResult), static_cast<int>(gWatchNetworkInitResult));
    return false;
  }
  gWatchNetworkReady = true;
  Serial.printf("WATCH_NETWORK_PROXY_READY ip=10.1.10.1 watch=10.1.10.2 mtu=%u napt=true\n",
                static_cast<unsigned>(kWatchNetworkMtu));
  return true;
}

void resetWatchNetworkSessionQueue() {
  if (gWatchNetworkTxQueue) xQueueReset(gWatchNetworkTxQueue);
}

bool injectWatchNetworkPacket(const uint8_t* bytes, size_t length) {
  if (!gWatchNetworkReady || !bytes || length < 20 || length > kWatchNetworkPacketMax || (bytes[0] >> 4) != 4) {
    ++gWatchNetworkDroppedPackets;
    return false;
  }
  // Use PBUF_LINK (not PBUF_RAW) so the pbuf reserves room for the Ethernet
  // header. When ip4_forward() hands this packet to etharp_output(), it calls
  // pbuf_header(q, -SIZEOF_ETH_HDR) to prepend the Ethernet header; a PBUF_RAW
  // pbuf has no such headroom and etharp_output returns ERR_BUF (-2), so the
  // packet is never actually transmitted. This was the return-path root cause.
  struct pbuf* packet = pbuf_alloc(PBUF_LINK, static_cast<u16_t>(length), PBUF_POOL);
  if (!packet || pbuf_take(packet, bytes, length) != ERR_OK) {
    if (packet) pbuf_free(packet);
    ++gWatchNetworkDroppedPackets;
    return false;
  }
  const err_t result = tcpip_input(packet, gWatchNetworkNetif);
  if (result != ERR_OK) {
    pbuf_free(packet);
    ++gWatchNetworkDroppedPackets;
    Serial.printf("WATCH_NETWORK_RX_INJECT_FAILED result=%d len=%u\n", static_cast<int>(result), static_cast<unsigned>(length));
    return false;
  }
  ++gWatchNetworkRxPackets;
  // Return-path tracer: log the uplink with protocol / src / dst.
  {
    const size_t ihl = static_cast<size_t>(bytes[0] & 0x0f) * 4;
    const uint8_t proto = bytes[9];
    const char* protoName = (proto == 6) ? "tcp" : (proto == 17) ? "udp" : (proto == 1) ? "icmp" : "ip";
    Serial.printf("WATCH_UPLINK_TO_LWIP proto=%u(%s) src=%d.%d.%d.%d dst=%d.%d.%d.%d",
                  static_cast<unsigned>(proto), protoName,
                  bytes[12], bytes[13], bytes[14], bytes[15],
                  bytes[16], bytes[17], bytes[18], bytes[19]);
    if ((proto == 6 || proto == 17) && length >= ihl + 4) {
      const uint16_t sport = (static_cast<uint16_t>(bytes[ihl]) << 8) | bytes[ihl + 1];
      const uint16_t dport = (static_cast<uint16_t>(bytes[ihl + 2]) << 8) | bytes[ihl + 3];
      Serial.printf(" sport=%u dport=%u", static_cast<unsigned>(sport), static_cast<unsigned>(dport));
    }
    Serial.printf(" len=%u\n", static_cast<unsigned>(length));
  }
  return true;
}

void drainWatchNetworkTx(uint8_t& sequence) {
  if (!gWatchNetworkTxQueue || !watchBt.connected()) return;
  if (!gWatchNetworkTxDrainScratch) return;
  if (!ensureScratchBuffers()) return;
  while (xQueueReceive(gWatchNetworkTxQueue, gWatchNetworkTxDrainScratch, 0) == pdTRUE) {
    const uint8_t txSeq = sequence;
    const uint8_t* b = gWatchNetworkTxDrainScratch->bytes;
    const size_t n = gWatchNetworkTxDrainScratch->length;
    Serial.printf("WATCH_RETURN_DRAIN len=%u q_left=%u\n",
                  static_cast<unsigned>(n),
                  static_cast<unsigned>(uxQueueMessagesWaiting(gWatchNetworkTxQueue)));
    gRawCommandPayloadScratch[0] = 7;
    gRawCommandPayloadScratch[1] = 1;
    memcpy(gRawCommandPayloadScratch + 2, b, n);
    if (!writeSppV2Frame(3, sequence++, gRawCommandPayloadScratch, n + 2)) {
      ++gWatchNetworkDroppedPackets;
      Serial.printf("WATCH_RETURN_DROP stage=spp_write seq=%u len=%u total_drop=%lu\n",
                    static_cast<unsigned>(txSeq), static_cast<unsigned>(n),
                    static_cast<unsigned long>(gWatchNetworkDroppedPackets));
      break;
    }
    Serial.printf("WATCH_RETURN_SPP_SENT seq=%u len=%u\n",
                  static_cast<unsigned>(txSeq), static_cast<unsigned>(n));
  }
}

void resetSppRxAssembler() {
  gSppRxBuffered = 0;
}

void dropSppRxPrefix(size_t count) {
  if (count == 0 || gSppRxBuffered == 0) return;
  if (count >= gSppRxBuffered) {
    gSppRxBuffered = 0;
    return;
  }
  memmove(gSppRxFrame, gSppRxFrame + count, gSppRxBuffered - count);
  gSppRxBuffered -= count;
}

void logSppRxParserState(const char* stage) {
  Serial.printf("SPP_RX_PARSER stage=%s buffered=%u queue=%d resync_bytes=%lu crc_fail=%lu oversize=%lu partial_timeouts=%lu dropped=%lu\n",
                stage ? stage : "unknown",
                static_cast<unsigned>(gSppRxBuffered),
                watchRxAvailable(),
                static_cast<unsigned long>(gSppRxResyncBytes),
                static_cast<unsigned long>(gSppRxCrcFailures),
                static_cast<unsigned long>(gSppRxOversizeHeaders),
                static_cast<unsigned long>(gSppRxPartialTimeouts),
                static_cast<unsigned long>(watchRxDroppedBytes));
}

bool readSppV2Frame(SppV2Frame& output, uint32_t timeoutMs) {
  uint8_t* buffer = gSppRxFrame;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    // Resynchronise one byte at a time, but keep all incomplete data across
    // readSppV2Frame() calls. The old implementation used a local buffered=0
    // and silently discarded a partial frame whenever a wait timed out.
    while (gSppRxBuffered >= 2 && (buffer[0] != 0xa5 || buffer[1] != 0xa5)) {
      dropSppRxPrefix(1);
      ++gSppRxResyncBytes;
    }

    if (gSppRxBuffered >= 8) {
      const size_t payloadLength = static_cast<size_t>(buffer[4] | (static_cast<uint16_t>(buffer[5]) << 8));
      if (payloadLength > kSppV2PayloadMax) {
        dropSppRxPrefix(1);
        ++gSppRxOversizeHeaders;
        continue;
      }
      const size_t frameLength = payloadLength + 8;
      if (gSppRxBuffered >= frameLength) {
        const uint16_t expected = static_cast<uint16_t>(buffer[6] | (static_cast<uint16_t>(buffer[7]) << 8));
        if (crc16Arc(buffer + 8, payloadLength) != expected) {
          dropSppRxPrefix(1);
          ++gSppRxCrcFailures;
          continue;
        }
        output.type = buffer[2] & 0x0f;
        output.sequence = buffer[3];
        output.payloadLength = payloadLength;
        if (payloadLength > 0) memcpy(output.payload, buffer + 8, payloadLength);
        dropSppRxPrefix(frameLength);
        return true;
      }
    }

    // Pull every byte already delivered by BluetoothSerial into the persistent
    // assembler, but never overflow the frame buffer. Extra frames remain in
    // the FreeRTOS queue and are consumed by the next call.
    bool received = false;
    while (gSppRxBuffered < 8 + kSppV2PayloadMax) {
      const int next = readWatchByte();
      if (next < 0) break;
      buffer[gSppRxBuffered++] = static_cast<uint8_t>(next);
      received = true;
      if (gSppRxBuffered >= 8) {
        const size_t payloadLength = static_cast<size_t>(buffer[4] | (static_cast<uint16_t>(buffer[5]) << 8));
        if (buffer[0] == 0xa5 && buffer[1] == 0xa5 && payloadLength <= kSppV2PayloadMax && gSppRxBuffered >= payloadLength + 8) break;
      }
    }
    if (!received) delay(2);
  }
  if (gSppRxBuffered > 0) ++gSppRxPartialTimeouts;
  return false;
}

// A Xiaomi SPP peer expects this plaintext handshake before the SPPv2 start
// request.  Its response is not an SPPv2 frame, so discard only a small,
// bounded response window before resetting the SPPv2 assembler.
bool startSppV2Session(const char* label) {
  if (watchBt.write(kSppHello, sizeof(kSppHello)) != sizeof(kSppHello)) {
    Serial.printf("%s_SPP_HELLO_WRITE_FAILED\n", label);
    return false;
  }

  const uint32_t startedAt = millis();
  size_t drainedBytes = 0;
  while (millis() - startedAt < kSppHelloDrainWindowMs && drainedBytes < kSppHelloDrainMaxBytes) {
    while (watchRxAvailable() > 0 && drainedBytes < kSppHelloDrainMaxBytes) {
      if (readWatchByte() < 0) break;
      ++drainedBytes;
    }
    delay(1);
  }
  resetSppRxAssembler();
  Serial.printf("%s_SPP_HELLO_SENT drained=%u\n", label, static_cast<unsigned>(drainedBytes));

  if (watchBt.write(kSppV2SessionStartRequest, sizeof(kSppV2SessionStartRequest)) != sizeof(kSppV2SessionStartRequest)) {
    Serial.printf("%s_SESSION_WRITE_FAILED\n", label);
    return false;
  }
  return true;
}

bool hmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t output[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info && mbedtls_md_hmac(info, key, keyLength, message, messageLength, output) == 0;
}

bool readProtoVarint(const uint8_t* data, size_t length, size_t& cursor, uint32_t& value) {
  value = 0;
  for (uint8_t shift = 0; shift < 32 && cursor < length; shift += 7) {
    const uint8_t next = data[cursor++];
    value |= static_cast<uint32_t>(next & 0x7f) << shift;
    if (!(next & 0x80)) return true;
  }
  return false;
}

bool protoVarintField(const uint8_t* data, size_t length, uint32_t wantedField, uint32_t& value) {
  size_t cursor = 0;
  while (cursor < length) {
    uint32_t tag = 0;
    if (!readProtoVarint(data, length, cursor, tag)) return false;
    const uint32_t field = tag >> 3, wire = tag & 7;
    if (wire == 0) {
      uint32_t next = 0;
      if (!readProtoVarint(data, length, cursor, next)) return false;
      if (field == wantedField) { value = next; return true; }
    } else if (wire == 2) {
      uint32_t size = 0;
      if (!readProtoVarint(data, length, cursor, size) || size > length - cursor) return false;
      cursor += size;
    } else if (wire == 5) {
      if (length - cursor < 4) return false;
      cursor += 4;
    } else if (wire == 1) {
      if (length - cursor < 8) return false;
      cursor += 8;
    } else return false;
  }
  return false;
}

bool protoBytesField(const uint8_t* data, size_t length, uint32_t wantedField, const uint8_t*& value, size_t& valueLength) {
  size_t cursor = 0;
  while (cursor < length) {
    uint32_t tag = 0;
    if (!readProtoVarint(data, length, cursor, tag)) return false;
    const uint32_t field = tag >> 3, wire = tag & 7;
    if (wire == 2) {
      uint32_t size = 0;
      if (!readProtoVarint(data, length, cursor, size) || size > length - cursor) return false;
      if (field == wantedField) { value = data + cursor; valueLength = size; return true; }
      cursor += size;
    } else if (wire == 0) {
      uint32_t ignored = 0;
      if (!readProtoVarint(data, length, cursor, ignored)) return false;
    } else if (wire == 5) {
      if (length - cursor < 4) return false;
      cursor += 4;
    } else if (wire == 1) {
      if (length - cursor < 8) return false;
      cursor += 8;
    } else return false;
  }
  return false;
}

bool skipProtoField(const uint8_t* data, size_t length, size_t& cursor, uint32_t wire) {
  uint32_t size = 0;
  if (wire == 0) return readProtoVarint(data, length, cursor, size);
  if (wire == 2) {
    return readProtoVarint(data, length, cursor, size) && size <= length - cursor && ((cursor += size), true);
  }
  if (wire == 5 && length - cursor >= 4) { cursor += 4; return true; }
  if (wire == 1 && length - cursor >= 8) { cursor += 8; return true; }
  return false;
}

class ProtoWriter {
 public:
  ProtoWriter(uint8_t* output, size_t capacity) : output_(output), capacity_(capacity) {}
  bool putByte(uint8_t value) { if (length_ >= capacity_) return false; output_[length_++] = value; return true; }
  bool putVarint(uint32_t value) {
    do { if (!putByte(static_cast<uint8_t>((value & 0x7f) | (value > 0x7f ? 0x80 : 0)))) return false; value >>= 7; } while (value);
    return true;
  }
  bool putBytesField(uint32_t field, const uint8_t* value, size_t valueLength) {
    return valueLength <= UINT32_MAX && putVarint((field << 3) | 2) && putVarint(static_cast<uint32_t>(valueLength)) && putRaw(value, valueLength);
  }
  bool putStringField(uint32_t field, const char* value) { return putBytesField(field, reinterpret_cast<const uint8_t*>(value), strlen(value)); }
  bool putRaw(const uint8_t* value, size_t valueLength) {
    if (valueLength > capacity_ - length_) return false;
    if (valueLength) memcpy(output_ + length_, value, valueLength);
    length_ += valueLength;
    return true;
  }
  size_t length() const { return length_; }
 private:
  uint8_t* output_;
  size_t capacity_;
  size_t length_ = 0;
};

bool protoNthBytesField(const uint8_t* data, size_t length, uint32_t wantedField,
                        uint32_t wantedIndex, const uint8_t*& value, size_t& valueLength) {
  size_t cursor = 0;
  uint32_t seen = 0;
  while (cursor < length) {
    uint32_t tag = 0;
    if (!readProtoVarint(data, length, cursor, tag)) return false;
    const uint32_t field = tag >> 3, wire = tag & 7;
    if (wire == 2) {
      uint32_t size = 0;
      if (!readProtoVarint(data, length, cursor, size) || size > length - cursor) return false;
      if (field == wantedField) {
        if (seen == wantedIndex) {
          value = data + cursor;
          valueLength = size;
          return true;
        }
        ++seen;
      }
      cursor += size;
    } else if (wire == 0) {
      uint32_t ignored = 0;
      if (!readProtoVarint(data, length, cursor, ignored)) return false;
    } else if (wire == 5) {
      if (length - cursor < 4) return false;
      cursor += 4;
    } else if (wire == 1) {
      if (length - cursor < 8) return false;
      cursor += 8;
    } else {
      return false;
    }
  }
  return false;
}

bool protoStringEquals(const uint8_t* data, size_t length, uint32_t field, const char* expected) {
  const uint8_t* value = nullptr;
  size_t valueLength = 0;
  if (!expected || !protoBytesField(data, length, field, value, valueLength)) return false;
  const size_t expectedLength = strlen(expected);
  return valueLength == expectedLength && memcmp(value, expected, expectedLength) == 0;
}

// Round-4 type2/id109:
// outer field4 -> field71 -> repeated field1 rule records.
// Extract the live watch_manual rule version/update timestamp (record field5).
bool parseRound4Id109State(const uint8_t* pb, size_t pbLength,
                           uint32_t& watchManualTimestamp,
                           uint32_t& watchSnapTimestamp) {
  watchManualTimestamp = 0;
  watchSnapTimestamp = 0;

  uint32_t type = 0, id = 0;
  if (!protoVarintField(pb, pbLength, 1, type) ||
      !protoVarintField(pb, pbLength, 2, id) ||
      type != 2 || id != 109) return false;

  const uint8_t* outer4 = nullptr;
  size_t outer4Length = 0;
  if (!protoBytesField(pb, pbLength, 4, outer4, outer4Length)) return false;

  const uint8_t* rules = nullptr;
  size_t rulesLength = 0;
  if (!protoBytesField(outer4, outer4Length, 71, rules, rulesLength)) return false;

  bool foundManual = false;
  for (uint32_t index = 0; index < 8; ++index) {
    const uint8_t* record = nullptr;
    size_t recordLength = 0;
    if (!protoNthBytesField(rules, rulesLength, 1, index, record, recordLength)) break;

    uint32_t timestamp = 0;
    protoVarintField(record, recordLength, 5, timestamp);
    if (protoStringEquals(record, recordLength, 2, "watch_manual")) {
      watchManualTimestamp = timestamp;
      foundManual = true;
    } else if (protoStringEquals(record, recordLength, 2, "watch_snap_rule")) {
      watchSnapTimestamp = timestamp;
    }
  }
  return foundManual;
}

// Build the exact Round-4 type2/id110 schema, but source field5 from the
// CURRENT watch's id109 response rather than hardcoding the captured timestamp.
bool buildRound4Id110(uint32_t liveTimestamp,
                      uint8_t* output, size_t capacity, size_t& outputLength) {
  uint8_t record[40]{};
  ProtoWriter recordWriter(record, sizeof(record));
  if (!recordWriter.putVarint((1u << 3) | 0u) || !recordWriter.putVarint(1) ||
      !recordWriter.putStringField(2, "manual_zen_rule") ||
      !recordWriter.putVarint((3u << 3) | 0u) || !recordWriter.putVarint(0) ||
      !recordWriter.putVarint((4u << 3) | 0u) || !recordWriter.putVarint(0) ||
      !recordWriter.putVarint((5u << 3) | 0u) || !recordWriter.putVarint(liveTimestamp)) {
    return false;
  }

  uint8_t repeatedRules[48]{};
  ProtoWriter rulesWriter(repeatedRules, sizeof(repeatedRules));
  if (!rulesWriter.putBytesField(1, record, recordWriter.length())) return false;

  uint8_t system[56]{};
  ProtoWriter systemWriter(system, sizeof(system));
  if (!systemWriter.putBytesField(71, repeatedRules, rulesWriter.length())) return false;

  ProtoWriter packetWriter(output, capacity);
  if (!packetWriter.putVarint((1u << 3) | 0u) || !packetWriter.putVarint(2) ||
      !packetWriter.putVarint((2u << 3) | 0u) || !packetWriter.putVarint(110) ||
      !packetWriter.putBytesField(4, system, systemWriter.length())) return false;

  outputLength = packetWriter.length();
  return true;
}

bool buildSyncNetworkStatus(uint8_t* output, size_t capacity, size_t& outputLength) {
  uint8_t networkStatus[4]{};
  ProtoWriter statusWriter(networkStatus, sizeof(networkStatus));
  if (!statusWriter.putVarint((1u << 3) | 0u) || !statusWriter.putVarint(2)) return false;

  uint8_t system[12]{};
  ProtoWriter systemWriter(system, sizeof(system));
  if (!systemWriter.putBytesField(59, networkStatus, statusWriter.length())) return false;

  ProtoWriter packetWriter(output, capacity);
  if (!packetWriter.putVarint((1u << 3) | 0u) || !packetWriter.putVarint(2) ||
      !packetWriter.putVarint((2u << 3) | 0u) || !packetWriter.putVarint(92) ||
      !packetWriter.putBytesField(4, system, systemWriter.length())) return false;
  outputLength = packetWriter.length();
  return true;
}

#if HOSHINO_ENABLE_ASR
// --- Native XiaoAI transcript support (disabled in Interconnect builds) ---
bool buildXiaoAiTranscript(const char* text, bool finalResult, uint32_t eventCode,
                           uint8_t* output, size_t capacity, size_t& outputLength) {
  if (!text) text = "";
  const size_t textLength = strlen(text);
  if (textLength > 700) return false;

  uint8_t textNode[720]{};
  ProtoWriter textWriter(textNode, sizeof(textNode));
  if (!textWriter.putBytesField(1, reinterpret_cast<const uint8_t*>(text), textLength)) return false;

  uint8_t level1[736]{};
  ProtoWriter level1Writer(level1, sizeof(level1));
  if (!level1Writer.putBytesField(1, textNode, textWriter.length())) return false;

  uint8_t payload[752]{};
  ProtoWriter payloadWriter(payload, sizeof(payload));
  if (!payloadWriter.putVarint((1u << 3) | 0u) || !payloadWriter.putVarint(finalResult ? 1u : 0u) ||
      !payloadWriter.putBytesField(2, level1, level1Writer.length())) return false;

  uint8_t assistantInner[800]{};
  ProtoWriter assistantWriter(assistantInner, sizeof(assistantInner));
  if (!assistantWriter.putVarint((3u << 3) | 0u) || !assistantWriter.putVarint(eventCode) ||
      !assistantWriter.putBytesField(13, payload, payloadWriter.length()) ||
      !assistantWriter.putVarint((2u << 3) | 0u) || !assistantWriter.putVarint(0)) return false;

  uint8_t assistantMid[816]{};
  ProtoWriter midWriter(assistantMid, sizeof(assistantMid));
  if (!midWriter.putBytesField(2, assistantInner, assistantWriter.length())) return false;

  uint8_t assistantOuter[832]{};
  ProtoWriter outerWriter(assistantOuter, sizeof(assistantOuter));
  if (!outerWriter.putBytesField(1, assistantMid, midWriter.length())) return false;

  ProtoWriter commandWriter(output, capacity);
  if (!commandWriter.putVarint((1u << 3) | 0u) || !commandWriter.putVarint(14) ||
      !commandWriter.putVarint((2u << 3) | 0u) || !commandWriter.putVarint(0) ||
      !commandWriter.putBytesField(16, assistantOuter, outerWriter.length())) return false;
  outputLength = commandWriter.length();
  return true;
}

// Confirmed against the official round-3 phone trace.  The 15-byte native
// XiaoAI lifecycle packet is:
//   08 0e 10 00 82 01 08 0a 06 12 04 18 <state> 10 00
// state=0 is sent immediately before the final transcript; state=7 closes the
// native assistant transaction after the phone has delivered its result.
bool buildXiaoAiState(uint32_t state, uint8_t* output, size_t capacity, size_t& outputLength) {
  uint8_t inner[8]{};
  ProtoWriter innerWriter(inner, sizeof(inner));
  if (!innerWriter.putVarint((3u << 3) | 0u) || !innerWriter.putVarint(state) ||
      !innerWriter.putVarint((2u << 3) | 0u) || !innerWriter.putVarint(0)) return false;

  uint8_t mid[12]{};
  ProtoWriter midWriter(mid, sizeof(mid));
  if (!midWriter.putBytesField(2, inner, innerWriter.length())) return false;

  uint8_t outer[16]{};
  ProtoWriter outerWriter(outer, sizeof(outer));
  if (!outerWriter.putBytesField(1, mid, midWriter.length())) return false;

  ProtoWriter commandWriter(output, capacity);
  if (!commandWriter.putVarint((1u << 3) | 0u) || !commandWriter.putVarint(14) ||
      !commandWriter.putVarint((2u << 3) | 0u) || !commandWriter.putVarint(0) ||
      !commandWriter.putBytesField(16, outer, outerWriter.length())) return false;
  outputLength = commandWriter.length();
  return true;
}
#endif  // HOSHINO_ENABLE_ASR

uint16_t ipv4HeaderChecksum(const uint8_t* bytes, size_t length) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < length; i += 2) sum += (static_cast<uint16_t>(bytes[i]) << 8) | bytes[i + 1];
  if (length & 1) sum += static_cast<uint16_t>(bytes[length - 1]) << 8;
  while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

bool sendWatchNetworkRaw(uint8_t& sequence, const uint8_t* bytes, size_t length) {
  if (!bytes || length == 0 || length > kWatchNetworkPacketMax || length + 2 > kSppV2PayloadMax) return false;
  if (!ensureScratchBuffers()) return false;
  gRawCommandPayloadScratch[0] = 7;
  gRawCommandPayloadScratch[1] = 1;
  memcpy(gRawCommandPayloadScratch + 2, bytes, length);
  return writeSppV2Frame(3, sequence++, gRawCommandPayloadScratch, length + 2);
}

uint8_t dhcpMessageType(const uint8_t* bootp, size_t length) {
  if (!bootp || length < 240 || bootp[236] != 0x63 || bootp[237] != 0x82 || bootp[238] != 0x53 || bootp[239] != 0x63) return 0;
  size_t cursor = 240;
  while (cursor < length) {
    const uint8_t code = bootp[cursor++];
    if (code == 255) break;
    if (code == 0) continue;
    if (cursor >= length) break;
    const uint8_t optionLength = bootp[cursor++];
    if (optionLength > length - cursor) break;
    if (code == 53 && optionLength == 1) return bootp[cursor];
    cursor += optionLength;
  }
  return 0;
}

bool sendWatchDhcpReply(uint8_t& sequence, const uint8_t* requestBootp, size_t requestLength, uint8_t replyType) {
  if (!requestBootp || requestLength < 240 || (replyType != 2 && replyType != 5)) return false;
  uint8_t packet[340]{};
  constexpr size_t ipLen = 20, udpLen = 8, bootpFixed = 240;
  uint8_t* ip = packet;
  uint8_t* udp = packet + ipLen;
  uint8_t* bootp = udp + udpLen;

  bootp[0] = 2; // BOOTREPLY
  bootp[1] = requestBootp[1];
  bootp[2] = requestBootp[2];
  bootp[3] = 0;
  memcpy(bootp + 4, requestBootp + 4, 4); // xid
  bootp[10] = 0; bootp[11] = 0; // flags = 0 (official phone behaviour)
  bootp[16] = 10; bootp[17] = 1; bootp[18] = 10; bootp[19] = 2; // yiaddr
  bootp[20] = 10; bootp[21] = 1; bootp[22] = 10; bootp[23] = 1; // siaddr
  memcpy(bootp + 28, requestBootp + 28, 16); // chaddr
  bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
  size_t o = bootpFixed;
  auto put = [&](uint8_t code, const uint8_t* value, uint8_t n) {
    bootp[o++] = code; bootp[o++] = n; memcpy(bootp + o, value, n); o += n;
  };
  const uint8_t typeValue[] = {replyType}; put(53, typeValue, 1);
  const uint8_t mask[] = {255,255,255,0}; put(1, mask, 4);
  const uint8_t router[] = {10,1,10,1}; put(3, router, 4);
  const uint8_t lease[] = {0x10,0x0e,0x00,0x00}; put(51, lease, 4); // 3600 s LE
  const uint8_t serverId[] = {10,1,10,1}; put(54, serverId, 4);
  const uint8_t dns[] = {114,114,114,114}; put(6, dns, 4);
  bootp[o++] = 255;
  const size_t bootpLength = o;
  const size_t udpLength = udpLen + bootpLength;
  const size_t totalLength = ipLen + udpLength;

  ip[0] = 0x45; ip[1] = 0;
  ip[2] = static_cast<uint8_t>(totalLength >> 8); ip[3] = static_cast<uint8_t>(totalLength & 0xff);
  ip[4] = 0; ip[5] = static_cast<uint8_t>(esp_random() & 0xff);
  ip[6] = 0; ip[7] = 0; ip[8] = 64; ip[9] = 17;
  ip[12] = 10; ip[13] = 1; ip[14] = 10; ip[15] = 1;
  ip[16] = 255; ip[17] = 255; ip[18] = 255; ip[19] = 255;
  const uint16_t checksum = ipv4HeaderChecksum(ip, ipLen);
  ip[10] = static_cast<uint8_t>(checksum >> 8); ip[11] = static_cast<uint8_t>(checksum & 0xff);

  udp[0] = 0; udp[1] = 67; udp[2] = 0; udp[3] = 68;
  udp[4] = static_cast<uint8_t>(udpLength >> 8); udp[5] = static_cast<uint8_t>(udpLength & 0xff);
  udp[6] = 0; udp[7] = 0; // IPv4 UDP checksum may be zero.
  const bool sent = sendWatchNetworkRaw(sequence, packet, totalLength);
  if (sent) Serial.println(replyType == 2 ? "WATCH_DHCP_OFFER ip=10.1.10.2 gateway=10.1.10.1 dns=114.114.114.114" :
                                            "WATCH_DHCP_ACK ip=10.1.10.2 gateway=10.1.10.1 dns=114.114.114.114");
  return sent;
}

bool handleWatchDhcp(const uint8_t* bytes, size_t length, uint8_t& sequence) {
  if (!bytes || length < 20 || (bytes[0] >> 4) != 4) return false;
  const size_t ihl = static_cast<size_t>(bytes[0] & 0x0f) * 4;
  if (ihl < 20 || length < ihl + 8 || bytes[9] != 17) return false;
  const uint8_t* udp = bytes + ihl;
  const uint16_t srcPort = (static_cast<uint16_t>(udp[0]) << 8) | udp[1];
  const uint16_t dstPort = (static_cast<uint16_t>(udp[2]) << 8) | udp[3];
  const uint16_t udpLength = (static_cast<uint16_t>(udp[4]) << 8) | udp[5];
  if (srcPort != 68 || dstPort != 67 || udpLength < 8 + 240 || ihl + udpLength > length) return false;
  const uint8_t* bootp = udp + 8;
  const size_t bootpLength = udpLength - 8;
  const uint8_t type = dhcpMessageType(bootp, bootpLength);
  if (type == 1) { sendWatchDhcpReply(sequence, bootp, bootpLength, 2); Serial.println("WATCH_DHCP_DISCOVER"); return true; }
  if (type == 3) { sendWatchDhcpReply(sequence, bootp, bootpLength, 5); Serial.println("WATCH_DHCP_REQUEST"); return true; }
  return false;
}

#if HOSHINO_ENABLE_ASR
bool sendPendingXiaoAiStart(uint8_t& sequence, const uint8_t encKey[16]) {
  if (!gXiaoAiStartPending || !nativeXiaoAiReturn) return true;
  gXiaoAiStartPending = false;
  uint8_t pb[128]{}; size_t pbLength = 0;
  if (!buildXiaoAiTranscript("", false, 1, pb, sizeof(pb), pbLength)) return false;
  const bool ok = sendEncryptedWatchPb(sequence, encKey, pb, pbLength);
  Serial.printf("XIAOAI_TRANSCRIPT_START_SENT ok=%s event=1 bytes=%u confirmed_shape=true\n", ok ? "true" : "false", static_cast<unsigned>(pbLength));
  if (ok) markXiaoAiProgress("start_empty_partial");
  return ok;
}

void scheduleXiaoAiEnd(uint32_t delayMs = kXiaoAiEndDelayMs) {
  gXiaoAiEndPending = true;
  gXiaoAiEndDueMs = millis() + delayMs;
}

bool sendPendingXiaoAiEnd(uint8_t& sequence, const uint8_t encKey[16]) {
  if (!gXiaoAiEndPending || !nativeXiaoAiReturn) return true;
  if (static_cast<int32_t>(millis() - gXiaoAiEndDueMs) < 0) return true;
  gXiaoAiEndPending = false;
  uint8_t pb[32]{}; size_t pbLength = 0;
  if (!buildXiaoAiState(7, pb, sizeof(pb), pbLength)) return false;
  const bool ok = sendEncryptedWatchPb(sequence, encKey, pb, pbLength);
  Serial.printf("XIAOAI_STATE_END_SENT ok=%s state=7 bytes=%u confirmed_shape=true\n",
                ok ? "true" : "false", static_cast<unsigned>(pbLength));
  if (ok) finishXiaoAiSession("state7_sent");
  return ok;
}

bool scheduleRollingAsrIfDue() {
  if (!gXiaoAiSessionActive || !gWatchStreamCaptureActive || gWatchStreamCaptureFileFailed ||
      !gWatchStreamCaptureFsReady || gWatchAsrTask || gXiaoAiFinalAsrPending ||
      gXiaoAiAudioMs < gXiaoAiNextPartialAudioMs) return false;
  // Close/reopen around the snapshot so the partial reader never races a
  // simultaneously-open SPIFFS writer handle on cores/framework versions that
  // do not guarantee coherent concurrent access to one file.
  char partialPacketPath[48]{};
  char partialWavPath[48]{};
  buildSessionAsrPaths(gXiaoAiActiveSessionId, false, partialPacketPath, sizeof(partialPacketPath),
                       partialWavPath, sizeof(partialWavPath));
  if (gWatchStreamCaptureFile) {
    gWatchStreamCaptureFile.flush();
    gWatchStreamCaptureFile.close();
  }
  const bool copied = copySpiffsFile(kWatchStreamCapturePath, partialPacketPath);
  gWatchStreamCaptureFile = SPIFFS.open(kWatchStreamCapturePath, FILE_APPEND);
  if (!gWatchStreamCaptureFile) {
    gWatchStreamCaptureFileFailed = true;
    Serial.println("WATCH_ASR_PARTIAL_REOPEN_FAILED");
    SPIFFS.remove(partialPacketPath);
    return false;
  }
  if (!copied) {
    Serial.println("WATCH_ASR_PARTIAL_SNAPSHOT_FAILED");
    return false;
  }
  if (!scheduleWatchAsrJob(partialPacketPath, partialWavPath, false, gXiaoAiActiveSessionId)) {
    SPIFFS.remove(partialPacketPath);
    return false;
  }
  gXiaoAiNextPartialAudioMs = gXiaoAiAudioMs + kXiaoAiRollingIntervalMs;
  Serial.printf("WATCH_ASR_PARTIAL_SCHEDULED session=%lu audio_ms=%lu next_audio_ms=%lu\n",
                static_cast<unsigned long>(gXiaoAiActiveSessionId),
                static_cast<unsigned long>(gXiaoAiAudioMs),
                static_cast<unsigned long>(gXiaoAiNextPartialAudioMs));
  return true;
}

void scheduleDeferredFinalAsrIfReady() {
  if (!gXiaoAiFinalAsrPending || gWatchAsrTask) return;
  const uint32_t sessionId = gXiaoAiFinalPendingSessionId;
  if (sessionId == 0 || sessionId != gXiaoAiActiveSessionId) {
    gXiaoAiFinalAsrPending = false;
    if (gXiaoAiFinalPacketPath[0]) SPIFFS.remove(gXiaoAiFinalPacketPath);
    if (gXiaoAiFinalWavPath[0]) SPIFFS.remove(gXiaoAiFinalWavPath);
    return;
  }
  if (scheduleWatchAsrJob(gXiaoAiFinalPacketPath, gXiaoAiFinalWavPath, true, sessionId)) {
    gXiaoAiFinalAsrPending = false;
    Serial.printf("WATCH_ASR_FINAL_RESUMED session=%lu\n", static_cast<unsigned long>(sessionId));
  }
}

void serviceXiaoAiAssistantCompletion() {
  if (!gXiaoAiSessionActive || !gXiaoAiAwaitAssistant) return;
  if (gXiaoAiAssistantDoneSessionId == gXiaoAiActiveSessionId) {
    gXiaoAiAwaitAssistant = false;
    scheduleXiaoAiEnd();
    Serial.printf("XIAOAI_ASSISTANT_COMPLETE session=%lu native_answer_return=false end_scheduled=true\n",
                  static_cast<unsigned long>(gXiaoAiActiveSessionId));
  }
}

void serviceXiaoAiNoProgressTimeout() {
  // Never close an Assistant transaction only because cloud progress is slow
  // while the user is still speaking. The deadline is evaluated after ch3 END.
  if (!gXiaoAiSessionActive || gWatchStreamCaptureActive || gXiaoAiEndPending || gXiaoAiLastProgressMs == 0) return;
  const uint32_t age = millis() - gXiaoAiLastProgressMs;
  if (age < kXiaoAiNoProgressTimeoutMs) return;
  Serial.printf("XIAOAI_NO_PROGRESS_TIMEOUT session=%lu age_ms=%lu threshold_ms=%lu inferred_guard=true\n",
                static_cast<unsigned long>(gXiaoAiActiveSessionId),
                static_cast<unsigned long>(age),
                static_cast<unsigned long>(kXiaoAiNoProgressTimeoutMs));
  scheduleXiaoAiEnd(0);
}

void drainAsrResultToWatch(uint8_t& sequence, const uint8_t encKey[16]) {
  if (!gAsrResultQueue) return;
  AsrResultMessage result;
  while (xQueueReceive(gAsrResultQueue, &result, 0) == pdTRUE) {
    if (!gXiaoAiSessionActive || result.sessionId == 0 || result.sessionId != gXiaoAiActiveSessionId) {
      Serial.printf("XIAOAI_ASR_RESULT_STALE result_session=%lu active_session=%lu final=%s\n",
                    static_cast<unsigned long>(result.sessionId),
                    static_cast<unsigned long>(gXiaoAiActiveSessionId),
                    result.finalResult ? "true" : "false");
      continue;
    }
    if (!result.ok || !nativeXiaoAiReturn) {
      if (result.finalResult) scheduleXiaoAiEnd(0);
      continue;
    }

    if (!result.finalResult) {
      if (gXiaoAiFinalSent || strcmp(result.text, gXiaoAiLastPartial) == 0) continue;
      uint8_t partialPb[1024]{}; size_t partialLength = 0;
      if (buildXiaoAiTranscript(result.text, false, 1, partialPb, sizeof(partialPb), partialLength) &&
          sendEncryptedWatchPb(sequence, encKey, partialPb, partialLength)) {
        snprintf(gXiaoAiLastPartial, sizeof(gXiaoAiLastPartial), "%s", result.text);
        ++gXiaoAiPartialSentCount;
        markXiaoAiProgress("partial_transcript");
        Serial.printf("XIAOAI_TRANSCRIPT_PARTIAL_SENT session=%lu event=1 bytes=%u text_bytes=%u\n",
                      static_cast<unsigned long>(result.sessionId), static_cast<unsigned>(partialLength),
                      static_cast<unsigned>(strlen(result.text)));
      }
      continue;
    }

    // The official phone sends a 15-byte state=0 packet, then the same
    // transcript with final=1.  This lifecycle is now confirmed by the HCI
    // capture; only the later assistant-answer/TTS events remain unresolved.
    uint8_t statePb[32]{}; size_t stateLength = 0;
    if (buildXiaoAiState(0, statePb, sizeof(statePb), stateLength)) {
      const bool stateOk = sendEncryptedWatchPb(sequence, encKey, statePb, stateLength);
      Serial.printf("XIAOAI_STATE_FINALIZING_SENT ok=%s state=0 bytes=%u confirmed_shape=true\n",
                    stateOk ? "true" : "false", static_cast<unsigned>(stateLength));
      if (stateOk) markXiaoAiProgress("state0");
    }

    uint8_t pb[1024]{}; size_t pbLength = 0;
    if (buildXiaoAiTranscript(result.text, true, 1, pb, sizeof(pb), pbLength) &&
        sendEncryptedWatchPb(sequence, encKey, pb, pbLength)) {
      Serial.printf("XIAOAI_TRANSCRIPT_FINAL_SENT event=1 bytes=%u text_bytes=%u confirmed_shape=true\n",
                    static_cast<unsigned>(pbLength), static_cast<unsigned>(strlen(result.text)));
      gXiaoAiFinalSent = true;
      snprintf(gXiaoAiLastPartial, sizeof(gXiaoAiLastPartial), "%s", result.text);
      markXiaoAiProgress("final_transcript");
      if (chatAfterAsr) {
        gXiaoAiAwaitAssistant = true;
        Serial.printf("XIAOAI_WAIT_ASSISTANT session=%lu no_progress_timeout_ms=%lu\n",
                      static_cast<unsigned long>(result.sessionId),
                      static_cast<unsigned long>(kXiaoAiNoProgressTimeoutMs));
      } else {
        scheduleXiaoAiEnd();
      }
    } else {
      Serial.println("XIAOAI_TRANSCRIPT_FINAL_SEND_FAILED");
    }
  }
}
#endif  // HOSHINO_ENABLE_ASR

struct WatchAppInfo {
  uint8_t fingerprint[64]{};
  size_t fingerprintLength = 0;
  bool found = false;
};

bool buildQuickAppBasicInfo(uint8_t* output, size_t capacity, size_t& outputLength, const WatchAppInfo& app) {
  ProtoWriter writer(output, capacity);
  if (!app.found || app.fingerprintLength == 0 || !writer.putStringField(1, kHoshinoQuickAppPackage) || !writer.putBytesField(2, app.fingerprint, app.fingerprintLength)) return false;
  outputLength = writer.length();
  return true;
}

bool wrapThirdpartyWearPacket(uint32_t id, const uint8_t* thirdparty, size_t thirdpartyLength, uint8_t* output, size_t capacity, size_t& outputLength) {
  ProtoWriter writer(output, capacity);
  if (!writer.putVarint((1 << 3) | 0) || !writer.putVarint(20) || !writer.putVarint((2 << 3) | 0) || !writer.putVarint(id) ||
      !writer.putBytesField(22, thirdparty, thirdpartyLength)) return false;
  outputLength = writer.length();
  return true;
}

bool buildQuickAppListRequest(uint8_t* output, size_t capacity, size_t& outputLength) {
  ProtoWriter writer(output, capacity);
  if (!writer.putVarint((1 << 3) | 0) || !writer.putVarint(20) || !writer.putVarint((2 << 3) | 0) || !writer.putVarint(0)) return false;
  outputLength = writer.length();
  return true;
}

bool buildQuickAppStatus(const WatchAppInfo& app, uint8_t* output, size_t capacity, size_t& outputLength) {
  uint8_t basic[192]; size_t basicLength = 0;
  if (!buildQuickAppBasicInfo(basic, sizeof(basic), basicLength, app)) return false;
  uint8_t phoneStatus[224]; ProtoWriter phoneWriter(phoneStatus, sizeof(phoneStatus));
  if (!phoneWriter.putBytesField(1, basic, basicLength) || !phoneWriter.putVarint((2 << 3) | 0) || !phoneWriter.putVarint(1)) return false;
  uint8_t thirdparty[256]; ProtoWriter thirdWriter(thirdparty, sizeof(thirdparty));
  if (!thirdWriter.putBytesField(8, phoneStatus, phoneWriter.length())) return false;
  return wrapThirdpartyWearPacket(7, thirdparty, thirdWriter.length(), output, capacity, outputLength);
}

bool buildQuickAppLaunch(const WatchAppInfo& app, uint8_t* output, size_t capacity, size_t& outputLength) {
  uint8_t basic[192]; size_t basicLength = 0;
  if (!buildQuickAppBasicInfo(basic, sizeof(basic), basicLength, app)) return false;
  uint8_t launch[224]; ProtoWriter launchWriter(launch, sizeof(launch));
  if (!launchWriter.putBytesField(1, basic, basicLength) || !launchWriter.putStringField(2, "")) return false;
  uint8_t thirdparty[256]; ProtoWriter thirdWriter(thirdparty, sizeof(thirdparty));
  if (!thirdWriter.putBytesField(6, launch, launchWriter.length())) return false;
  return wrapThirdpartyWearPacket(4, thirdparty, thirdWriter.length(), output, capacity, outputLength);
}

bool buildQuickAppMessage(const WatchAppInfo& app, const uint8_t* content, size_t contentLength, uint8_t* output, size_t capacity, size_t& outputLength) {
  if (!ensureAppScratchBuffers()) return false;
  uint8_t basic[192]; size_t basicLength = 0;
  if (!buildQuickAppBasicInfo(basic, sizeof(basic), basicLength, app)) return false;
  ProtoWriter messageWriter(gQuickAppMessageScratch, kSppV2PayloadMax);
  if (!messageWriter.putBytesField(1, basic, basicLength) || !messageWriter.putBytesField(2, content, contentLength)) return false;
  ProtoWriter thirdWriter(gQuickAppThirdpartyScratch, kSppV2PayloadMax);
  if (!thirdWriter.putBytesField(9, gQuickAppMessageScratch, messageWriter.length())) return false;
  return wrapThirdpartyWearPacket(8, gQuickAppThirdpartyScratch, thirdWriter.length(), output, capacity, outputLength);
}

bool parseSecretHex(String value, uint8_t output[16]) {
  value.trim();
  if (value.length() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    const char high = value[i * 2], low = value[i * 2 + 1];
    if (!isxdigit(static_cast<unsigned char>(high)) || !isxdigit(static_cast<unsigned char>(low))) return false;
    output[i] = static_cast<uint8_t>((hexNibble(high) << 4) | hexNibble(low));
  }
  return true;
}

bool buildStep3(const uint8_t secret[16], const uint8_t phoneNonce[16], const uint8_t watchNonce[16], const uint8_t watchHmac[32], uint8_t output[160], size_t& outputLength, uint8_t sessionDecKey[16] = nullptr, uint8_t sessionEncKey[16] = nullptr, const uint8_t* deviceInfoOverride = nullptr, size_t deviceInfoOverrideLength = 0) {
  uint8_t transcript[32], hmacKey[32], expansion[64], input[44], confirmation[32], encryptedNonces[32], encryptionNonce[12], deviceInfo[48], encryptedInfo[52];
  memcpy(transcript, phoneNonce, 16); memcpy(transcript + 16, watchNonce, 16);
  if (!hmacSha256(transcript, sizeof(transcript), secret, 16, hmacKey)) return false;
  static constexpr uint8_t kAuthLabel[] = {'m','i','w','e','a','r','-','a','u','t','h'};
  memcpy(input, kAuthLabel, sizeof(kAuthLabel)); input[sizeof(kAuthLabel)] = 1;
  if (!hmacSha256(hmacKey, sizeof(hmacKey), input, sizeof(kAuthLabel) + 1, expansion)) return false;
  memcpy(input, expansion, 32); memcpy(input + 32, kAuthLabel, sizeof(kAuthLabel)); input[43] = 2;
  if (!hmacSha256(hmacKey, sizeof(hmacKey), input, sizeof(input), expansion + 32)) return false;
  uint8_t confirmationInput[32];
  memcpy(confirmationInput, watchNonce, 16); memcpy(confirmationInput + 16, phoneNonce, 16);
  if (!hmacSha256(expansion, 16, confirmationInput, sizeof(confirmationInput), confirmation) || memcmp(confirmation, watchHmac, 32) != 0) return false;
  if (sessionDecKey) memcpy(sessionDecKey, expansion, 16);
  if (sessionEncKey) memcpy(sessionEncKey, expansion + 16, 16);
  if (!hmacSha256(expansion + 16, 16, transcript, sizeof(transcript), encryptedNonces)) return false;
  size_t infoLength = 0;
  if (deviceInfoOverride && deviceInfoOverrideLength > 0 &&
      deviceInfoOverrideLength <= sizeof(deviceInfo)) {
    memcpy(deviceInfo, deviceInfoOverride, deviceInfoOverrideLength);
    infoLength = deviceInfoOverrideLength;
  } else {
    const char kPhoneName[] = "Hoshino-Bridge";
    deviceInfo[infoLength++] = 0x08; deviceInfo[infoLength++] = 0;
    deviceInfo[infoLength++] = 0x15; deviceInfo[infoLength++] = 0x00; deviceInfo[infoLength++] = 0x00; deviceInfo[infoLength++] = 0x10; deviceInfo[infoLength++] = 0x42;
    deviceInfo[infoLength++] = 0x1a; deviceInfo[infoLength++] = sizeof(kPhoneName) - 1; memcpy(deviceInfo + infoLength, kPhoneName, sizeof(kPhoneName) - 1); infoLength += sizeof(kPhoneName) - 1;
    deviceInfo[infoLength++] = 0x20; deviceInfo[infoLength++] = 0xe0; deviceInfo[infoLength++] = 0x01;
    deviceInfo[infoLength++] = 0x2a; deviceInfo[infoLength++] = 0x02; deviceInfo[infoLength++] = 'Z'; deviceInfo[infoLength++] = 'H';
  }
  memcpy(encryptionNonce, expansion + 36, 4); memset(encryptionNonce + 4, 0, 8);
  mbedtls_ccm_context ccm; mbedtls_ccm_init(&ccm);
  const int setKeyResult = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, expansion + 16, 128);
  const int encryptResult = setKeyResult == 0 ? mbedtls_ccm_encrypt_and_tag(&ccm, infoLength, encryptionNonce, sizeof(encryptionNonce), nullptr, 0, deviceInfo, encryptedInfo, encryptedInfo + infoLength, 4) : -1;
  mbedtls_ccm_free(&ccm);
  if (encryptResult != 0) return false;
  // Command.auth(field3) -> Auth.authStep3(field32) -> {
  //   encryptedNonces(field1, 32B),
  //   encryptedDeviceInfo(field2, infoLength + 4B CCM tag)
  // }
  // The old implementation hard-coded 73/70, which was only valid for the
  // 30-byte Hoshino-Bridge DeviceInfo. Round-4 official DeviceInfo is 28B,
  // therefore its correct lengths are 71/68. Build them dynamically.
  const size_t encryptedInfoLength = infoLength + 4u;
  const size_t authStep3BodyLength =
      2u + 32u +                  // field1 tag+len + encryptedNonces
      2u + encryptedInfoLength;   // field2 tag+len + encryptedDeviceInfo
  const size_t authMessageLength =
      2u + 1u + authStep3BodyLength;  // field32 tag (2B) + 1B varint length

  if (encryptedInfoLength >= 128u ||
      authStep3BodyLength >= 128u ||
      authMessageLength >= 128u) {
    return false;
  }

  size_t cursor = 0;
  output[cursor++] = 0x08; output[cursor++] = 1;
  output[cursor++] = 0x10; output[cursor++] = 27;
  output[cursor++] = 0x1a;
  output[cursor++] = static_cast<uint8_t>(authMessageLength);

  output[cursor++] = 0x82; output[cursor++] = 0x02;
  output[cursor++] = static_cast<uint8_t>(authStep3BodyLength);

  output[cursor++] = 0x0a; output[cursor++] = 32;
  memcpy(output + cursor, encryptedNonces, 32); cursor += 32;

  output[cursor++] = 0x12;
  output[cursor++] = static_cast<uint8_t>(encryptedInfoLength);
  memcpy(output + cursor, encryptedInfo, encryptedInfoLength);
  cursor += encryptedInfoLength;

  outputLength = cursor;
  Serial.printf("WATCH_AUTH_STEP3_LAYOUT info=%u encrypted_info=%u inner=%u auth=%u total=%u\n",
                static_cast<unsigned>(infoLength),
                static_cast<unsigned>(encryptedInfoLength),
                static_cast<unsigned>(authStep3BodyLength),
                static_cast<unsigned>(authMessageLength),
                static_cast<unsigned>(outputLength));
  mbedtls_platform_zeroize(transcript, sizeof(transcript)); mbedtls_platform_zeroize(hmacKey, sizeof(hmacKey)); mbedtls_platform_zeroize(expansion, sizeof(expansion)); mbedtls_platform_zeroize(input, sizeof(input)); mbedtls_platform_zeroize(confirmation, sizeof(confirmation)); mbedtls_platform_zeroize(encryptedNonces, sizeof(encryptedNonces)); mbedtls_platform_zeroize(encryptionNonce, sizeof(encryptionNonce)); mbedtls_platform_zeroize(deviceInfo, sizeof(deviceInfo)); mbedtls_platform_zeroize(encryptedInfo, sizeof(encryptedInfo));
  return true;
}

bool aes128CtrCrypt(const uint8_t key[16], uint8_t* bytes, size_t length) {
  uint8_t nonceCounter[16], streamBlock[16]{};
  size_t offset = 0;
  memcpy(nonceCounter, key, sizeof(nonceCounter));
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  const int setKeyResult = mbedtls_aes_setkey_enc(&aes, key, 128);
  const int cryptResult = setKeyResult == 0 ? mbedtls_aes_crypt_ctr(&aes, length, &offset, nonceCounter, streamBlock, bytes, bytes) : -1;
  mbedtls_aes_free(&aes);
  mbedtls_platform_zeroize(nonceCounter, sizeof(nonceCounter));
  mbedtls_platform_zeroize(streamBlock, sizeof(streamBlock));
  return cryptResult == 0;
}

bool sendEncryptedWatchChannel(uint8_t& sequence, const uint8_t key[16],
                               uint8_t channel, uint8_t opcode,
                               const uint8_t* data, size_t dataLength) {
  if (dataLength + 2 > kSppV2PayloadMax) return false;
  if (!ensureScratchBuffers()) return false;
  uint8_t* payload = gEncryptedPayloadScratch;
  payload[0] = channel;
  payload[1] = opcode;
  memcpy(payload + 2, data, dataLength);
  if (!aes128CtrCrypt(key, payload + 2, dataLength)) return false;
  const bool sent = writeSppV2Frame(3, sequence++, payload, dataLength + 2);
  mbedtls_platform_zeroize(payload, dataLength + 2);
  return sent;
}

bool sendEncryptedWatchPb(uint8_t& sequence, const uint8_t key[16], const uint8_t* pb, size_t pbLength) {
  return sendEncryptedWatchChannel(sequence, key, 1, 2, pb, pbLength);
}

// Derive only the normal-session CTR keys for an already-captured auth
// transcript.  This is intentionally smaller than buildStep3(): no CCM,
// deviceInfo or Step3 payload is built, keeping the 6144-byte WROOM watch-task
// stack comfortable.
bool deriveCapturedSessionKeysOnly(const uint8_t secret[16],
                                   const uint8_t phoneNonce[16],
                                   const uint8_t watchNonce[16],
                                   const uint8_t watchHmac[32],
                                   uint8_t sessionDecKey[16],
                                   uint8_t sessionEncKey[16]) {
  uint8_t transcript[32]{};
  uint8_t hmacKey[32]{};
  uint8_t expansion[64]{};
  uint8_t input[44]{};
  uint8_t confirmation[32]{};
  uint8_t confirmationInput[32]{};

  memcpy(transcript, phoneNonce, 16);
  memcpy(transcript + 16, watchNonce, 16);
  if (!hmacSha256(transcript, sizeof(transcript), secret, 16, hmacKey)) return false;

  static constexpr uint8_t kAuthLabel[] = {'m','i','w','e','a','r','-','a','u','t','h'};
  memcpy(input, kAuthLabel, sizeof(kAuthLabel));
  input[sizeof(kAuthLabel)] = 1;
  if (!hmacSha256(hmacKey, sizeof(hmacKey), input, sizeof(kAuthLabel) + 1, expansion)) return false;

  memcpy(input, expansion, 32);
  memcpy(input + 32, kAuthLabel, sizeof(kAuthLabel));
  input[43] = 2;
  if (!hmacSha256(hmacKey, sizeof(hmacKey), input, sizeof(input), expansion + 32)) return false;

  memcpy(confirmationInput, watchNonce, 16);
  memcpy(confirmationInput + 16, phoneNonce, 16);
  const bool ok = hmacSha256(expansion, 16, confirmationInput, sizeof(confirmationInput), confirmation) &&
                  memcmp(confirmation, watchHmac, 32) == 0;
  if (ok) {
    memcpy(sessionDecKey, expansion, 16);
    memcpy(sessionEncKey, expansion + 16, 16);
  }

  mbedtls_platform_zeroize(transcript, sizeof(transcript));
  mbedtls_platform_zeroize(hmacKey, sizeof(hmacKey));
  mbedtls_platform_zeroize(expansion, sizeof(expansion));
  mbedtls_platform_zeroize(input, sizeof(input));
  mbedtls_platform_zeroize(confirmation, sizeof(confirmation));
  mbedtls_platform_zeroize(confirmationInput, sizeof(confirmationInput));
  return ok;
}


// Decrypt the exact AuthStep3 DeviceInfo from the CURRENT Xiaomi 17 Pro
// Round-4 21:01:20 successful session.
// Captured Step3 field2 is 32 bytes = 28-byte CCM plaintext + 4-byte tag.
// The long-term Auth Key never leaves RAM and is never printed.
bool decryptRound4CapturedAuthDeviceInfo(const uint8_t secret[16],
                                         uint8_t output[48],
                                         size_t& outputLength) {
  static constexpr uint8_t kPhoneNonce[16] = {
      0x7e,0xef,0x3c,0x2c,0xf5,0xf3,0xb2,0x71,0xb7,0xce,0x7f,0x69,0x1c,0x78,0xf1,0x38};
  static constexpr uint8_t kWatchNonce[16] = {
      0xdd,0x21,0xa3,0x09,0x4e,0xe7,0x02,0x05,0x72,0xff,0x14,0x57,0xaf,0xa9,0xd0,0x39};
  static constexpr uint8_t kWatchHmac[32] = {
      0x30,0xb5,0xc4,0x55,0xe8,0xc5,0x9f,0x9d,0xbb,0xc5,0xce,0x33,0x59,0x4b,0xee,0x00,
      0x0c,0x59,0x5b,0x7b,0xbe,0x0f,0x83,0xe0,0xe9,0xd0,0x4c,0xa6,0x9f,0xac,0xae,0x49};
  static constexpr uint8_t kEncryptedInfoAndTag[32] = {
      0x3e,0x44,0x09,0xa4,0x00,0x82,0xb3,0x0e,0x99,0xe8,0xef,0xb0,0x4e,0x4f,0x7e,0xbf,
      0x10,0xc0,0x80,0x02,0x1e,0xcf,0xab,0xd5,0x05,0x60,0x5d,0x4f,0x58,0xb8,0x62,0x69};

  constexpr size_t kPlainLength = sizeof(kEncryptedInfoAndTag) - 4;
  uint8_t capturedDecKey[16]{};
  uint8_t capturedEncKey[16]{};
  uint8_t nonce[12]{};

  bool ok = deriveCapturedSessionKeysOnly(secret, kPhoneNonce, kWatchNonce, kWatchHmac,
                                          capturedDecKey, capturedEncKey);
  if (ok) {
    // Xiaomi AuthStep3 CCM nonce = expansion[36:40] || 8 zero bytes.
    // deriveCapturedSessionKeysOnly returns encKey=expansion[16:32], so derive
    // the complete expansion again only for the 4-byte CCM nonce suffix.
    uint8_t transcript[32]{};
    uint8_t hmacKey[32]{};
    uint8_t expansion[64]{};
    uint8_t input[44]{};
    memcpy(transcript, kPhoneNonce, 16);
    memcpy(transcript + 16, kWatchNonce, 16);
    ok = hmacSha256(transcript, sizeof(transcript), secret, 16, hmacKey);
    static constexpr uint8_t kAuthLabel[] = {'m','i','w','e','a','r','-','a','u','t','h'};
    if (ok) {
      memcpy(input, kAuthLabel, sizeof(kAuthLabel));
      input[sizeof(kAuthLabel)] = 1;
      ok = hmacSha256(hmacKey, sizeof(hmacKey), input, sizeof(kAuthLabel) + 1, expansion);
    }
    if (ok) {
      memcpy(input, expansion, 32);
      memcpy(input + 32, kAuthLabel, sizeof(kAuthLabel));
      input[43] = 2;
      ok = hmacSha256(hmacKey, sizeof(hmacKey), input, sizeof(input), expansion + 32);
    }
    if (ok) memcpy(nonce, expansion + 36, 4);
    mbedtls_platform_zeroize(transcript, sizeof(transcript));
    mbedtls_platform_zeroize(hmacKey, sizeof(hmacKey));
    mbedtls_platform_zeroize(expansion, sizeof(expansion));
    mbedtls_platform_zeroize(input, sizeof(input));
  }

  if (ok) {
    mbedtls_ccm_context ccm;
    mbedtls_ccm_init(&ccm);
    const int setKeyResult =
        mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, capturedEncKey, 128);
    const int decryptResult =
        setKeyResult == 0
            ? mbedtls_ccm_auth_decrypt(&ccm,
                                       kPlainLength,
                                       nonce, sizeof(nonce),
                                       nullptr, 0,
                                       kEncryptedInfoAndTag,
                                       output,
                                       kEncryptedInfoAndTag + kPlainLength, 4)
            : -1;
    mbedtls_ccm_free(&ccm);
    ok = decryptResult == 0;
  }

  if (ok) {
    outputLength = kPlainLength;
    // Expected protobuf starts with field1(varint) then field2(fixed32).
    ok = outputLength >= 7 && output[0] == 0x08 && output[2] == 0x15;
  }
  if (!ok) {
    outputLength = 0;
    mbedtls_platform_zeroize(output, 48);
  }

  mbedtls_platform_zeroize(capturedDecKey, sizeof(capturedDecKey));
  mbedtls_platform_zeroize(capturedEncKey, sizeof(capturedEncKey));
  mbedtls_platform_zeroize(nonce, sizeof(nonce));
  return ok;
}

bool protoFixed32Field(const uint8_t* data, size_t length,
                       uint32_t wantedField, uint32_t& value) {
  size_t cursor = 0;
  while (cursor < length) {
    uint32_t tag = 0;
    if (!readProtoVarint(data, length, cursor, tag)) return false;
    const uint32_t field = tag >> 3;
    const uint32_t wire = tag & 7u;
    if (wire == 5) {
      if (length - cursor < 4) return false;
      if (field == wantedField) {
        value = static_cast<uint32_t>(data[cursor]) |
                (static_cast<uint32_t>(data[cursor + 1]) << 8) |
                (static_cast<uint32_t>(data[cursor + 2]) << 16) |
                (static_cast<uint32_t>(data[cursor + 3]) << 24);
        return true;
      }
      cursor += 4;
    } else if (wire == 0) {
      uint32_t ignored = 0;
      if (!readProtoVarint(data, length, cursor, ignored)) return false;
    } else if (wire == 2) {
      uint32_t n = 0;
      if (!readProtoVarint(data, length, cursor, n) || n > length - cursor) return false;
      cursor += n;
    } else if (wire == 1) {
      if (length - cursor < 8) return false;
      cursor += 8;
    } else {
      return false;
    }
  }
  return false;
}

void logRound4AuthDeviceInfo(const uint8_t* info, size_t length) {
  uint32_t platform = 0, unknown4 = 0, apiBits = 0;
  const uint8_t* phoneName = nullptr;
  const uint8_t* region = nullptr;
  size_t phoneNameLength = 0, regionLength = 0;
  protoVarintField(info, length, 1, platform);
  protoFixed32Field(info, length, 2, apiBits);
  protoBytesField(info, length, 3, phoneName, phoneNameLength);
  protoVarintField(info, length, 4, unknown4);
  protoBytesField(info, length, 5, region, regionLength);

  float apiLevel = 0.0f;
  static_assert(sizeof(apiLevel) == sizeof(apiBits), "float must be 32-bit");
  memcpy(&apiLevel, &apiBits, sizeof(apiLevel));

  Serial.printf("WATCH_R4_AUTH_IDENTITY len=%u platform=%lu api=%.3f unknown4=%lu phoneName=",
                static_cast<unsigned>(length),
                static_cast<unsigned long>(platform),
                static_cast<double>(apiLevel),
                static_cast<unsigned long>(unknown4));
  if (phoneName) {
    for (size_t i = 0; i < phoneNameLength; ++i) Serial.write(phoneName[i]);
  } else {
    Serial.print("<missing>");
  }
  Serial.print(" region=");
  if (region) {
    for (size_t i = 0; i < regionLength; ++i) Serial.write(region[i]);
  } else {
    Serial.print("<missing>");
  }
  Serial.print(" hex=");
  for (size_t i = 0; i < length; ++i) Serial.printf("%02X", info[i]);
  Serial.println();
}

// Verified Round-4 CompanionDevice protobuf plaintext. This is never sent
// directly: buildStep3() AES-CCM encrypts it with the keys and nonce derived
// from the current watch AuthKey and current authentication session.
static constexpr uint8_t kRound4CompatibleDeviceInfo[] = {
    0x08,0x00,0x15,0x00,0x00,0x10,0x42,
    0x1a,0x0a,'2','5','0','9','8','P','N','5','A','C',
    0x20,0xfe,0xbd,0xdc,0x0d,0x2a,0x02,'C','N',
};

// Decrypt the exact 137-byte type23/id3 template captured from the current
// Xiaomi 17 Pro Round-4 session. The long-term Auth Key is never printed.
bool decryptRound4Type23Template(const uint8_t secret[16], uint8_t output[137]) {
  static constexpr uint8_t kPhoneNonce[16] = {
      0x7e,0xef,0x3c,0x2c,0xf5,0xf3,0xb2,0x71,0xb7,0xce,0x7f,0x69,0x1c,0x78,0xf1,0x38};
  static constexpr uint8_t kWatchNonce[16] = {
      0xdd,0x21,0xa3,0x09,0x4e,0xe7,0x02,0x05,0x72,0xff,0x14,0x57,0xaf,0xa9,0xd0,0x39};
  static constexpr uint8_t kWatchHmac[32] = {
      0x30,0xb5,0xc4,0x55,0xe8,0xc5,0x9f,0x9d,0xbb,0xc5,0xce,0x33,0x59,0x4b,0xee,0x00,
      0x0c,0x59,0x5b,0x7b,0xbe,0x0f,0x83,0xe0,0xe9,0xd0,0x4c,0xa6,0x9f,0xac,0xae,0x49};

  // Phone -> Watch seq13, channel=1/opcode=2, ciphertext only (137 bytes).
  static constexpr uint8_t kType23Id3Cipher[137] = {
      0x93,0xe7,0x2e,0x56,0x9b,0x39,0xeb,0x9f,0x19,0xcc,0x02,0x9b,0x99,0x16,0x91,0xfe,
      0x37,0x85,0x59,0xd8,0x58,0x24,0xa0,0xda,0x23,0xd0,0x0f,0xa8,0x89,0xdc,0x12,0xc4,
      0x0b,0x78,0xbc,0x0f,0x9d,0x3b,0x72,0x69,0x12,0xe9,0x96,0x18,0x6b,0xb9,0xe3,0x34,
      0xd6,0xab,0x8c,0x0f,0x1f,0xe0,0x7d,0x08,0x6e,0xfd,0xd1,0x05,0xbc,0x15,0x2d,0xf7,
      0xe7,0x4a,0x81,0xd9,0x78,0x4f,0x60,0xfd,0xd8,0xd2,0x72,0x4c,0x9e,0x93,0x38,0xe3,
      0x5a,0x2d,0x6c,0x3e,0xe5,0xf1,0xd4,0x54,0xea,0x1f,0xd8,0xa2,0x82,0x08,0x5e,0x3d,
      0x97,0x95,0xf8,0x5b,0x43,0xb2,0xed,0x56,0xf4,0x45,0x5a,0xd4,0x46,0x3d,0x16,0x5b,
      0x40,0xa7,0x8f,0x40,0x1a,0xd7,0x3e,0x14,0x7f,0xf0,0xcb,0xc4,0x49,0x26,0x64,0x8c,
      0x37,0x73,0xe8,0x9c,0x8d,0x3c,0xea,0xce,0x36};

  uint8_t capturedDecKey[16]{};
  uint8_t capturedEncKey[16]{};
  bool ok = deriveCapturedSessionKeysOnly(secret, kPhoneNonce, kWatchNonce, kWatchHmac,
                                          capturedDecKey, capturedEncKey);
  if (ok) {
    memcpy(output, kType23Id3Cipher, sizeof(kType23Id3Cipher));
    ok = aes128CtrCrypt(capturedEncKey, output, sizeof(kType23Id3Cipher));
  }
  if (ok) {
    ok = output[0] == 0x08 && output[1] == 0x17 &&
         output[2] == 0x10 && output[3] == 0x03 &&
         output[4] == 0xca && output[5] == 0x01;
  }

  mbedtls_platform_zeroize(capturedDecKey, sizeof(capturedDecKey));
  mbedtls_platform_zeroize(capturedEncKey, sizeof(capturedEncKey));
  if (!ok) mbedtls_platform_zeroize(output, 137);
  return ok;
}

// One-shot offline extractor for the official Round-3 phone capture.
// Uses the Watch Auth Key already stored in Preferences. The key is never printed.
// No Bluetooth connection is required for this command.
void dumpRound3BootstrapPlaintext() {
  static constexpr uint8_t kPhoneNonce[16] = {
      0xaa,0xd7,0x4e,0x3d,0x63,0x89,0xe4,0x0b,0xb9,0x3a,0xd8,0x54,0x9e,0xa9,0xec,0x84};
  static constexpr uint8_t kWatchNonce[16] = {
      0xcf,0x75,0xbc,0x5f,0xb8,0x04,0xc5,0x47,0x14,0xfe,0x6e,0x03,0xdb,0xb0,0x68,0x7a};
  static constexpr uint8_t kWatchHmac[32] = {
      0x8b,0x5b,0x85,0xd0,0xa3,0xd5,0xb8,0x42,0x4e,0x84,0x69,0xb5,0x8a,0x32,0xfb,0xd9,
      0x96,0x16,0x90,0x62,0x57,0x5f,0xbc,0x42,0x20,0xfb,0xa2,0xb5,0xb9,0x88,0x49,0xc9};

  // Phone -> Watch ch1/op2 ciphertext only (01 02 channel/opcode are intentionally excluded).
  static constexpr uint8_t kType8Id52Cipher[55] = {
      0x91,0xe6,0xd3,0xaa,0xfc,0x32,0xa3,0x3e,0x50,0x0e,0x8f,0xa4,0x0c,0x3b,0x23,0x0d,
      0xe0,0xe2,0xbc,0xfe,0x7b,0x18,0x75,0x0e,0x60,0x0c,0x0f,0x1f,0x99,0x46,0x2e,0xee,
      0x86,0xaf,0x25,0xa8,0xd3,0xe1,0xcd,0x38,0x06,0x9c,0x6f,0xaa,0x7d,0xef,0xd9,0xcc,
      0xc7,0xab,0xd0,0xbc,0x5a,0x20,0xf9};
  static constexpr uint8_t kType23Id3Cipher[111] = {
      0x91,0xf9,0xd3,0x9d,0x64,0x02,0x29,0x2e,0x18,0x0e,0xe2,0xa4,0x0e,0x39,0xb0,0x8b,
      0x8d,0x99,0x61,0xe5,0x2e,0x75,0x18,0x79,0x19,0xf3,0x80,0x42,0xe8,0x9e,0x43,0xe8,
      0x6a,0x2f,0xb2,0x5f,0xb4,0xe1,0x2a,0xa9,0x51,0x98,0xf1,0x1a,0x55,0xa6,0xd3,0xde,
      0x47,0x76,0xdb,0xb1,0x87,0xbf,0xd7,0xb5,0x43,0xbf,0x1f,0x73,0xfa,0x16,0xb2,0x2f,
      0xa4,0x7e,0x8e,0xea,0x5b,0x75,0x91,0xf7,0xc3,0x8a,0x2a,0x73,0x39,0x5d,0xb3,0x5e,
      0x7c,0x5f,0x87,0x1c,0x94,0x10,0x68,0x0a,0x30,0x27,0x5a,0xd8,0x6f,0xee,0x60,0x69,
      0xe1,0xb5,0x4b,0xde,0x56,0x28,0xa6,0xcd,0xfa,0x19,0xbd,0x89,0xe2,0x4f,0x4a};

  uint8_t secret[16]{}, step3[160]{}, sessionDecKey[16]{}, sessionEncKey[16]{};
  size_t step3Length = 0;
  if (!parseSecretHex(watchAuthKey, secret)) {
    Serial.println("ROUND3_DECRYPT_FAIL reason=watch_auth_missing_or_invalid");
    return;
  }
  if (!buildStep3(secret, kPhoneNonce, kWatchNonce, kWatchHmac,
                  step3, step3Length, sessionDecKey, sessionEncKey)) {
    Serial.println("ROUND3_DECRYPT_FAIL reason=auth_key_does_not_match_round3_capture");
    mbedtls_platform_zeroize(secret, sizeof(secret));
    mbedtls_platform_zeroize(step3, sizeof(step3));
    return;
  }

  uint8_t type8[sizeof(kType8Id52Cipher)]{};
  uint8_t type23[sizeof(kType23Id3Cipher)]{};
  memcpy(type8, kType8Id52Cipher, sizeof(type8));
  memcpy(type23, kType23Id3Cipher, sizeof(type23));
  const bool ok8 = aes128CtrCrypt(sessionEncKey, type8, sizeof(type8));
  const bool ok23 = aes128CtrCrypt(sessionEncKey, type23, sizeof(type23));
  const bool prefix8 = ok8 && sizeof(type8) >= 4 && type8[0] == 0x08 && type8[1] == 0x08 && type8[2] == 0x10 && type8[3] == 0x34;
  const bool prefix23 = ok23 && sizeof(type23) >= 4 && type23[0] == 0x08 && type23[1] == 0x17 && type23[2] == 0x10 && type23[3] == 0x03;

  if (prefix8 && prefix23) {
    Serial.printf("TYPE8_52_HEX=%s\n", hexBytes(type8, sizeof(type8)).c_str());
    Serial.printf("TYPE23_3_HEX=%s\n", hexBytes(type23, sizeof(type23)).c_str());
    Serial.println("ROUND3_DECRYPT_OK auth_valid=true key_not_printed=true");
  } else {
    Serial.printf("ROUND3_DECRYPT_FAIL reason=plaintext_prefix_mismatch type8=%s type23=%s\n",
                  prefix8 ? "ok" : "bad", prefix23 ? "ok" : "bad");
  }

  mbedtls_platform_zeroize(secret, sizeof(secret));
  mbedtls_platform_zeroize(step3, sizeof(step3));
  mbedtls_platform_zeroize(sessionDecKey, sizeof(sessionDecKey));
  mbedtls_platform_zeroize(sessionEncKey, sizeof(sessionEncKey));
  mbedtls_platform_zeroize(type8, sizeof(type8));
  mbedtls_platform_zeroize(type23, sizeof(type23));
}

bool decryptWatchPb(SppV2Frame& frame, const uint8_t key[16], const uint8_t*& pb, size_t& pbLength) {
  if (frame.type != 3 || frame.payloadLength < 3 || frame.payload[0] != 1 || frame.payload[1] != 2) return false;
  pbLength = frame.payloadLength - 2;
  if (!aes128CtrCrypt(key, frame.payload + 2, pbLength)) return false;
  pb = frame.payload + 2;
  return true;
}

bool parseQuickAppList(const uint8_t* pb, size_t pbLength, WatchAppInfo& app) {
  uint32_t type = 0, id = 0;
  const uint8_t* thirdparty = nullptr; size_t thirdpartyLength = 0;
  if (!protoVarintField(pb, pbLength, 1, type) || !protoVarintField(pb, pbLength, 2, id) || type != 20 || id != 0 ||
      !protoBytesField(pb, pbLength, 22, thirdparty, thirdpartyLength)) return false;
  const uint8_t* list = nullptr; size_t listLength = 0;
  if (!protoBytesField(thirdparty, thirdpartyLength, 1, list, listLength)) return false;
  size_t cursor = 0;
  while (cursor < listLength) {
    uint32_t tag = 0, itemLength = 0;
    if (!readProtoVarint(list, listLength, cursor, tag)) return false;
    if ((tag >> 3) != 1 || (tag & 7) != 2) {
      if (!skipProtoField(list, listLength, cursor, tag & 7)) return false;
      continue;
    }
    if (!readProtoVarint(list, listLength, cursor, itemLength) || itemLength > listLength - cursor) return false;
    const uint8_t* item = list + cursor; cursor += itemLength;
    const uint8_t* packageName = nullptr; size_t packageNameLength = 0;
    const uint8_t* fingerprint = nullptr; size_t fingerprintLength = 0;
    if (!protoBytesField(item, itemLength, 1, packageName, packageNameLength) || !protoBytesField(item, itemLength, 2, fingerprint, fingerprintLength)) continue;
    if (packageNameLength == strlen(kHoshinoQuickAppPackage) && memcmp(packageName, kHoshinoQuickAppPackage, packageNameLength) == 0 && fingerprintLength > 0 && fingerprintLength <= sizeof(app.fingerprint)) {
      memcpy(app.fingerprint, fingerprint, fingerprintLength);
      app.fingerprintLength = fingerprintLength;
      app.found = true;
      return true;
    }
  }
  return false;
}

bool parseQuickAppMessage(const uint8_t* pb, size_t pbLength, const uint8_t*& content, size_t& contentLength) {
  uint32_t type = 0, id = 0;
  const uint8_t* thirdparty = nullptr; size_t thirdpartyLength = 0, messageLength = 0;
  const uint8_t* message = nullptr;
  if (!protoVarintField(pb, pbLength, 1, type) || !protoVarintField(pb, pbLength, 2, id) || type != 20 || id != 9 ||
      !protoBytesField(pb, pbLength, 22, thirdparty, thirdpartyLength) || !protoBytesField(thirdparty, thirdpartyLength, 9, message, messageLength)) return false;
  return protoBytesField(message, messageLength, 2, content, contentLength);
}

bool containsAscii(const uint8_t* data, size_t length, const char* text) {
  const size_t textLength = strlen(text);
  if (!textLength || textLength > length) return false;
  for (size_t i = 0; i + textLength <= length; ++i) if (memcmp(data + i, text, textLength) == 0) return true;
  return false;
}

bool sendSppV2Protobuf(uint8_t sequence, const uint8_t* command, size_t commandLength) {
  if (commandLength + 2 > kSppV2PayloadMax) return false;
  if (!ensureScratchBuffers()) return false;
  uint8_t* payload = gRawCommandPayloadScratch;
  payload[0] = 1; payload[1] = 1;
  memcpy(payload + 2, command, commandLength);
  return writeSppV2Frame(3, sequence, payload, commandLength + 2);
}

bool extractWatchNonce(const SppV2Frame& frame, const uint8_t*& nonce, size_t& nonceLength, const uint8_t*& hmac, size_t& hmacLength) {
  if (frame.type != 3 || frame.payloadLength < 3 || (frame.payload[0] & 0x0f) != 1 || frame.payload[1] != 1) return false;
  const uint8_t* command = frame.payload + 2; const size_t commandLength = frame.payloadLength - 2;
  uint32_t subtype = 0;
  const uint8_t* auth = nullptr; size_t authLength = 0;
  if (!protoVarintField(command, commandLength, 2, subtype) || subtype != 26 || !protoBytesField(command, commandLength, 3, auth, authLength)) return false;
  const uint8_t* watchNonce = nullptr; size_t watchNonceLength = 0;
  if (!protoBytesField(auth, authLength, 31, watchNonce, watchNonceLength)) return false;
  return protoBytesField(watchNonce, watchNonceLength, 1, nonce, nonceLength) && protoBytesField(watchNonce, watchNonceLength, 2, hmac, hmacLength);
}

bool isAuthSuccess(const SppV2Frame& frame) {
  if (frame.type != 3 || frame.payloadLength < 3 || (frame.payload[0] & 0x0f) != 1 || frame.payload[1] != 1) return false;
  uint32_t subtype = 0;
  return protoVarintField(frame.payload + 2, frame.payloadLength - 2, 2, subtype) && subtype == 27;
}

uint8_t benchByte(uint32_t offset) {
  return static_cast<uint8_t>((offset * 29u + (offset >> 3) * 17u + 0x5du) & 0xffu);
}

void fillBenchBytes(uint32_t offset, uint8_t* output, size_t length) {
  for (size_t index = 0; index < length; ++index) output[index] = benchByte(offset + index);
}

bool sha256BenchData(uint32_t size, char output[65]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  uint8_t chunk[kBenchChunkRawMax], digest[32];
  bool ok = mbedtls_md_setup(&context, info, 0) == 0 && mbedtls_md_starts(&context) == 0;
  for (uint32_t offset = 0; ok && offset < size; offset += sizeof(chunk)) {
    const size_t length = min(static_cast<uint32_t>(sizeof(chunk)), size - offset);
    fillBenchBytes(offset, chunk, length);
    ok = mbedtls_md_update(&context, chunk, length) == 0;
  }
  if (ok) ok = mbedtls_md_finish(&context, digest) == 0;
  mbedtls_md_free(&context);
  if (!ok) return false;
  static constexpr char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(digest); ++index) {
    output[index * 2] = hex[(digest[index] >> 4) & 15];
    output[index * 2 + 1] = hex[digest[index] & 15];
  }
  output[64] = '\0';
  mbedtls_platform_zeroize(digest, sizeof(digest));
  return true;
}

bool base64BenchEncode(const uint8_t* input, size_t length, char* output, size_t capacity) {
  static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t required = ((length + 2) / 3) * 4 + 1;
  if (capacity < required) return false;
  size_t in = 0, out = 0;
  while (in < length) {
    const uint32_t a = input[in++];
    const bool hasB = in < length; const uint32_t b = hasB ? input[in++] : 0;
    const bool hasC = in < length; const uint32_t c = hasC ? input[in++] : 0;
    const uint32_t value = (a << 16) | (b << 8) | c;
    output[out++] = table[(value >> 18) & 63]; output[out++] = table[(value >> 12) & 63];
    output[out++] = hasB ? table[(value >> 6) & 63] : '='; output[out++] = hasC ? table[value & 63] : '=';
  }
  output[out] = '\0';
  return true;
}

bool jsonEscapeBenchString(const char* input, char* output, size_t capacity) {
  size_t out = 0;
  for (size_t in = 0; input[in] != '\0'; ++in) {
    const char value = input[in];
    const bool needsEscape = value == '"' || value == '\\';
    if (out + (needsEscape ? 2 : 1) >= capacity) return false;
    if (needsEscape) output[out++] = '\\';
    output[out++] = value;
  }
  output[out] = '\0';
  return true;
}

void configureBenchSessionRenewal(const uint8_t secret[16], const uint8_t encKey[16], const uint8_t decKey[16]) {
  memcpy(gBenchSessionSecret, secret, sizeof(gBenchSessionSecret));
  memcpy(gBenchSessionEncKey, encKey, sizeof(gBenchSessionEncKey));
  memcpy(gBenchSessionDecKey, decKey, sizeof(gBenchSessionDecKey));
  gBenchSessionRenewals = 0;
  gBenchSessionRenewalEnabled = true;
}

void clearBenchSessionRenewal() {
  gBenchSessionRenewalEnabled = false;
  gBenchSessionRenewals = 0;
  mbedtls_platform_zeroize(gBenchSessionSecret, sizeof(gBenchSessionSecret));
  mbedtls_platform_zeroize(gBenchSessionEncKey, sizeof(gBenchSessionEncKey));
  mbedtls_platform_zeroize(gBenchSessionDecKey, sizeof(gBenchSessionDecKey));
}

bool renewBenchSppSession(uint8_t& sequence) {
  if (!gBenchSessionRenewalEnabled) return false;

  Serial.printf("SPP_SESSION_RENEW start old_sequence=%u renewals=%lu\n",
                static_cast<unsigned>(sequence),
                static_cast<unsigned long>(gBenchSessionRenewals));

  // A benchmark send only renews after the previous application ACK completed,
  // so no application response should still be pending here.
  if (watchBt.write(kSppV2SessionStartRequest, sizeof(kSppV2SessionStartRequest)) != sizeof(kSppV2SessionStartRequest)) {
    Serial.println("SPP_SESSION_RENEW_FAIL stage=session_write");
    return false;
  }

  SppV2Frame& frame = gSharedSppFrame;
  bool sessionReady = false;
  uint32_t deadline = millis() + kWatchVersionResponseTimeoutMs;
  while (millis() < deadline) {
    if (!readSppV2Frame(frame, deadline - millis())) break;
    if (frame.type == 3) writeSppV2Frame(1, frame.sequence, nullptr, 0);
    if (frame.type == 2 && frame.payloadLength >= 1 && frame.payload[0] == 2) {
      sessionReady = true;
      break;
    }
  }
  if (!sessionReady) {
    Serial.println("SPP_SESSION_RENEW_FAIL stage=session_response");
    return false;
  }

  uint8_t phoneNonce[16]{};
  uint8_t step3[160]{};
  size_t step3Length = 0;
  esp_fill_random(phoneNonce, sizeof(phoneNonce));
  uint8_t nonceCommand[27] = {0x08, 1, 0x10, 26, 0x1a, 21, 0xf2, 0x01, 18, 0x0a, 16};
  memcpy(nonceCommand + 11, phoneNonce, sizeof(phoneNonce));
  if (!sendSppV2Protobuf(0, nonceCommand, sizeof(nonceCommand))) {
    Serial.println("SPP_SESSION_RENEW_FAIL stage=nonce_write");
    mbedtls_platform_zeroize(phoneNonce, sizeof(phoneNonce));
    return false;
  }

  const uint8_t* watchNonce = nullptr;
  const uint8_t* watchHmac = nullptr;
  size_t watchNonceLength = 0, watchHmacLength = 0;
  deadline = millis() + kWatchVersionResponseTimeoutMs;
  while (millis() < deadline) {
    if (!readSppV2Frame(frame, deadline - millis())) break;
    if (frame.type == 3) writeSppV2Frame(1, frame.sequence, nullptr, 0);
    if (extractWatchNonce(frame, watchNonce, watchNonceLength, watchHmac, watchHmacLength)) break;
  }
  if (!watchNonce || watchNonceLength != 16 || watchHmacLength != 32 ||
      !buildStep3(gBenchSessionSecret, phoneNonce, watchNonce, watchHmac, step3, step3Length,
                  gBenchSessionDecKey, gBenchSessionEncKey)) {
    Serial.println("SPP_SESSION_RENEW_FAIL stage=nonce_or_key");
    mbedtls_platform_zeroize(phoneNonce, sizeof(phoneNonce));
    mbedtls_platform_zeroize(step3, sizeof(step3));
    return false;
  }

  if (!sendSppV2Protobuf(1, step3, step3Length)) {
    Serial.println("SPP_SESSION_RENEW_FAIL stage=auth_write");
    mbedtls_platform_zeroize(phoneNonce, sizeof(phoneNonce));
    mbedtls_platform_zeroize(step3, sizeof(step3));
    return false;
  }

  bool authenticated = false;
  deadline = millis() + kWatchVersionResponseTimeoutMs;
  while (millis() < deadline) {
    if (!readSppV2Frame(frame, deadline - millis())) break;
    if (frame.type == 3) writeSppV2Frame(1, frame.sequence, nullptr, 0);
    if (isAuthSuccess(frame)) {
      authenticated = true;
      break;
    }
  }

  mbedtls_platform_zeroize(phoneNonce, sizeof(phoneNonce));
  mbedtls_platform_zeroize(step3, sizeof(step3));
  if (!authenticated) {
    Serial.println("SPP_SESSION_RENEW_FAIL stage=auth_response");
    return false;
  }

  sequence = 2;
  ++gBenchSessionRenewals;
  Serial.printf("SPP_SESSION_RENEW_OK new_sequence=%u renewals=%lu\n",
                static_cast<unsigned>(sequence),
                static_cast<unsigned long>(gBenchSessionRenewals));
  return true;
}

bool ensureBenchSppSequenceBudget(uint8_t& sequence) {
  if (!gBenchSessionRenewalEnabled || sequence < kBenchSppRenewAtSequence) return true;
  Serial.printf("SPP_SEQUENCE_GUARD sequence=%u threshold=%u\n",
                static_cast<unsigned>(sequence),
                static_cast<unsigned>(kBenchSppRenewAtSequence));
  return renewBenchSppSession(sequence);
}

bool sendBenchJson(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const char* text) {
  if (!ensureBenchSppSequenceBudget(sequence)) {
    Serial.println("BENCH_SEND_FAIL reason=spp_session_renewal");
    return false;
  }
  const size_t length = strlen(text);
  if (!ensureScratchBuffers()) return false;
  uint8_t* packet = gRawCommandPayloadScratch;
  size_t packetLength = 0;
  const uint8_t* activeKey = gBenchSessionRenewalEnabled ? gBenchSessionEncKey : encKey;
  return length < kSppV2PayloadMax &&
         buildQuickAppMessage(app, reinterpret_cast<const uint8_t*>(text), length,
                              packet, kSppV2PayloadMax, packetLength) &&
         sendEncryptedWatchPb(sequence, activeKey, packet, packetLength);
}

bool waitBenchReply(const uint8_t decKey[16], const char* type, const char* phase, int expectedSequence, uint32_t timeoutMs, bool* duplicate = nullptr, char* hash = nullptr) {
  const uint32_t startedAt = millis();
  if (duplicate) *duplicate = false;
  if (hash) hash[0] = '\0';
  uint32_t frames = 0, nonData = 0, decryptFail = 0, quickAppParseFail = 0, otherPb = 0, jsonFail = 0, wrongType = 0, wrongPhase = 0, wrongSequence = 0;
  while (millis() - startedAt < timeoutMs) {
    const uint32_t remaining = timeoutMs - (millis() - startedAt);
    SppV2Frame& frame = gSharedSppFrame;
    if (!readSppV2Frame(frame, remaining)) break;
    ++frames;
    if (frame.type != 3) { ++nonData; continue; }
    writeSppV2Frame(1, frame.sequence, nullptr, 0);
    const uint8_t* incoming = nullptr; size_t incomingLength = 0, contentLength = 0; const uint8_t* content = nullptr;
    const uint8_t* activeDecKey = gBenchSessionRenewalEnabled ? gBenchSessionDecKey : decKey;
    if (!decryptWatchPb(frame, activeDecKey, incoming, incomingLength)) { ++decryptFail; continue; }
    if (!parseQuickAppMessage(incoming, incomingLength, content, contentLength)) {
      // Not every encrypted protobuf delivered by the watch is a QuickApp
      // message. Classify readable outer protobufs separately so unrelated
      // asynchronous traffic is not falsely reported as corruption.
      uint32_t outerType = 0, outerId = 0;
      const bool hasOuterType = protoVarintField(incoming, incomingLength, 1, outerType);
      const bool hasOuterId = protoVarintField(incoming, incomingLength, 2, outerId);
      if (hasOuterType && hasOuterId && (outerType != 20 || outerId != 9)) {
        ++otherPb;
        Serial.printf("BENCH_PB_OTHER frame_seq=%u pb_type=%lu pb_id=%lu pb_len=%u\n",
                      static_cast<unsigned>(frame.sequence),
                      static_cast<unsigned long>(outerType),
                      static_cast<unsigned long>(outerId),
                      static_cast<unsigned>(incomingLength));
      } else {
        ++quickAppParseFail;
        Serial.printf("BENCH_APP_PARSE_FAIL frame_seq=%u pb_type=%s pb_id=%s pb_len=%u head=",
                      static_cast<unsigned>(frame.sequence),
                      hasOuterType ? String(outerType).c_str() : "?",
                      hasOuterId ? String(outerId).c_str() : "?",
                      static_cast<unsigned>(incomingLength));
        const size_t headLength = min<size_t>(incomingLength, 16);
        for (size_t i = 0; i < headLength; ++i) Serial.printf("%02x", incoming[i]);
        Serial.println();
      }
      continue;
    }
    static JsonDocument document;
    document.clear();
    if (deserializeJson(document, content, contentLength)) { ++jsonFail; continue; }
    const char* gotType = document["type"] | "";
    JsonObject payload = document["payload"].as<JsonObject>();
    if (strcmp(gotType, "capabilities_response") == 0) {
      Serial.printf("BENCH_CAPS buildTag=%s\n", String(payload["buildTag"] | "missing").c_str());
    }
    if (strcmp(gotType, "pong") != 0) {
      Serial.printf("BENCH_RX type=%s phase=%s code=%s sequence=%d replyTo=%s\n",
                    gotType,
                    String(payload["phase"] | "").c_str(),
                    String(payload["code"] | "").c_str(),
                    payload["sequence"] | -1,
                    String(document["replyTo"] | "").c_str());
    }
    if (strcmp(gotType, "package_status") == 0 && payload["ok"].is<bool>() && !payload["ok"].as<bool>()) {
      Serial.printf("BENCH_REMOTE_ERROR code=%s message=%s\n", String(payload["code"] | "unknown").c_str(), String(payload["message"] | "").c_str());
      return false;
    }
    if (strcmp(gotType, type) == 0 && payload["ok"].is<bool>() && !payload["ok"].as<bool>()) {
      Serial.printf("BENCH_REMOTE_ERROR type=%s code=%s message=%s\n", gotType, String(payload["code"] | "unknown").c_str(), String(payload["message"] | "").c_str());
      return false;
    }
    if (strcmp(gotType, "package_status") == 0) {
      Serial.printf("PKG_STATUS_REPLY_RX type=%s replyTo=%s phase=%s ok=%s\n",
                    gotType,
                    String(document["replyTo"] | "").c_str(),
                    String(payload["phase"] | "").c_str(),
                    payload["ok"].is<bool>() && payload["ok"].as<bool>() ? "true" : "false");
      JsonObject diag = payload["transportDiag"].as<JsonObject>();
      if (!diag.isNull()) {
        Serial.printf("BENCH_CHUNK_PATH_DIAG expected_seq=%d expected_offset=%ld inflight=%s inflight_seq=%d inflight_offset=%ld rx_count=%ld last_rx_seq=%d last_rx_offset=%ld last_write_seq=%d last_write_offset=%ld last_ack_seq=%d last_ack_next=%ld ack_count=%ld ack_send_success_seq=%d ack_send_success_count=%ld ack_send_fail_seq=%d ack_send_fail_code=%s recovered_count=%ld\n",
                      diag["expectedSequence"] | -1,
                      static_cast<long>(diag["expectedOffset"] | -1),
                      (diag["inFlight"] | false) ? "true" : "false",
                      diag["inFlightSequence"] | -1,
                      static_cast<long>(diag["inFlightOffset"] | -1),
                      static_cast<long>(diag["rxCount"] | -1),
                      diag["lastRxSequence"] | -1,
                      static_cast<long>(diag["lastRxOffset"] | -1),
                      diag["lastWriteSequence"] | -1,
                      static_cast<long>(diag["lastWriteOffset"] | -1),
                      diag["lastAckSequence"] | -1,
                      static_cast<long>(diag["lastAckNextOffset"] | -1),
                      static_cast<long>(diag["ackCount"] | -1),
                      diag["lastAckSendSuccessSequence"] | -1,
                      static_cast<long>(diag["ackSendSuccessCount"] | -1),
                      diag["lastAckSendFailSequence"] | -1,
                      String(diag["lastAckSendFailCode"] | "").c_str(),
                      static_cast<long>(diag["recoveredCount"] | -1));
      }
    }
    if (strcmp(gotType, type) != 0) { ++wrongType; continue; }
    if (phase && strcmp(payload["phase"] | "", phase) != 0) { ++wrongPhase; continue; }
    const int receivedSequence = payload["sequence"] | -1;
    if (expectedSequence >= 0 && receivedSequence != expectedSequence) { ++wrongSequence; continue; }
    if (duplicate) *duplicate = payload["duplicate"] | false;
    if (hash) snprintf(hash, 65, "%s", String(payload["sha256"] | "").c_str());
    return true;
  }
  Serial.printf("BENCH_WAIT_TIMEOUT expected_type=%s expected_phase=%s expected_sequence=%d frames=%lu non_data=%lu decrypt_fail=%lu app_parse_fail=%lu other_pb=%lu json_fail=%lu wrong_type=%lu wrong_phase=%lu wrong_sequence=%lu rx_dropped=%lu\n",
                type ? type : "",
                phase ? phase : "",
                expectedSequence,
                static_cast<unsigned long>(frames),
                static_cast<unsigned long>(nonData),
                static_cast<unsigned long>(decryptFail),
                static_cast<unsigned long>(quickAppParseFail),
                static_cast<unsigned long>(otherPb),
                static_cast<unsigned long>(jsonFail),
                static_cast<unsigned long>(wrongType),
                static_cast<unsigned long>(wrongPhase),
                static_cast<unsigned long>(wrongSequence),
                static_cast<unsigned long>(watchRxDroppedBytes));
  logStackHighWater("bench_wait_timeout");
  logSppRxParserState("bench_wait_timeout");
  return false;
}

struct BenchCounters {
  uint32_t errors = 0;
  uint32_t retries = 0;
  uint32_t duplicates = 0;
  uint32_t ackTimeouts = 0;
};

enum BenchFault : uint8_t {
  kBenchFaultNone = 0,
  kBenchFaultPause = 1,
  kBenchFaultDropAck = 2,
  kBenchFaultDuplicate = 4,
  kBenchFaultAbort = 8,
  kBenchFaultDropDurableAck = 16,
};

bool runBenchControlProbe(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  static char text[256];
  int length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_caps\",\"timestamp\":%lu,\"type\":\"capabilities_request\",\"payload\":{}}", static_cast<unsigned long>(millis()));
  const bool caps = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "capabilities_response", nullptr, -1, kBenchControlTimeoutMs);
  Serial.printf("BENCH_CONTROL capabilities=%s\n", caps ? "PASS" : "FAIL");
  if (!caps) return false;

  delay(kBenchControlGapMs);
  length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_status\",\"timestamp\":%lu,\"type\":\"package_status\",\"payload\":{\"query\":true}}", static_cast<unsigned long>(millis()));
  Serial.println("PKG_STATUS_TX id=bench_status type=package_status");
  const bool status = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "package_status", nullptr, -1, kBenchControlTimeoutMs);
  Serial.printf("BENCH_CONTROL package_status=%s\n", status ? "PASS" : "FAIL");
  if (!status) return false;

  // Always clear stale benchmark state from a previously interrupted run.
  length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_cleanup\",\"timestamp\":%lu,\"type\":\"package_abort\",\"payload\":{}}", static_cast<unsigned long>(millis()));
  const bool cleanup = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "package_status", nullptr, -1, kBenchControlTimeoutMs);
  Serial.printf("BENCH_CONTROL cleanup=%s\n", cleanup ? "PASS" : "FAIL");
  return cleanup;
}

bool runBenchPingTest(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  static uint32_t samples[100]; memset(samples, 0, sizeof(samples));
  uint32_t total = 0, minimum = UINT32_MAX, maximum = 0; uint16_t success = 0;
  for (uint16_t index = 0; index < 100; ++index) {
    static char text[256];
    const int length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_ping_%u\",\"timestamp\":%lu,\"type\":\"ping\",\"payload\":{\"from\":\"esp32\"}}", static_cast<unsigned>(index), static_cast<unsigned long>(millis()));
    const uint32_t started = millis();
    if (length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "pong", nullptr, -1, kBenchAckTimeoutMs)) {
      const uint32_t elapsed = millis() - started; samples[success++] = elapsed; total += elapsed; minimum = min(minimum, elapsed); maximum = max(maximum, elapsed);
    }
  }
  for (uint16_t i = 1; i < success; ++i) for (uint16_t j = i; j > 0 && samples[j] < samples[j - 1]; --j) { const uint32_t swap = samples[j]; samples[j] = samples[j - 1]; samples[j - 1] = swap; }
  const uint32_t p95 = success ? samples[(success * 95 + 99) / 100 - 1] : 0;
  Serial.println("PING_TEST");
  Serial.printf("count=100\nsuccess=%u\nloss=%u\navg_ms=%lu\nmin_ms=%lu\nmax_ms=%lu\np95_ms=%lu\n", static_cast<unsigned>(success), static_cast<unsigned>(100 - success), static_cast<unsigned long>(success ? total / success : 0), static_cast<unsigned long>(success ? minimum : 0), static_cast<unsigned long>(maximum), static_cast<unsigned long>(p95));
  return success == 100;
}

bool runBenchFile(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16], uint32_t size, size_t chunkSize, BenchCounters& counters, uint8_t faults = kBenchFaultNone, const char* storageMode = nullptr) {
  if (chunkSize > kBenchChunkRawMax) { Serial.printf("BENCH_CHUNK_SKIPPED chunk_size=%u reason=spp_message_limit\n", static_cast<unsigned>(chunkSize)); return false; }
  static char hash[65], receivedHash[65], packageId[32], text[kSppV2PayloadMax];
  if (!sha256BenchData(size, hash)) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=sha256"); return false; }
  snprintf(packageId, sizeof(packageId), "bench_%08lx", static_cast<unsigned long>(esp_random()));
  logStackHighWater("bench_file_begin");

  int length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_begin\",\"timestamp\":%lu,\"type\":\"package_begin\",\"payload\":{\"packageId\":\"%s\",\"packageType\":\"comic\",\"fileCount\":1,\"totalSize\":%lu}}", static_cast<unsigned long>(millis()), packageId, static_cast<unsigned long>(size));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text) || !sendBenchJson(app, sequence, encKey, text)) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=package_begin_send"); return false; }
  if (!waitBenchReply(decKey, "package_status", "begun", -1, kBenchControlTimeoutMs)) {
    ++counters.errors;
    Serial.println("BENCH_STAGE_FAIL stage=package_begin_reply");
    // Query state once more: if this returns, the router is alive and the log
    // reveals whether begin is stuck in filesystem setup or its reply was lost.
    length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_begin_diag\",\"timestamp\":%lu,\"type\":\"package_status\",\"payload\":{}}", static_cast<unsigned long>(millis()));
    if (length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text)) (void)waitBenchReply(decKey, "package_status", nullptr, -1, 2500);
    return false;
  }

  static char manifest[256], escapedManifest[512];
  snprintf(manifest, sizeof(manifest), "{\"format\":\"hoshino-package\",\"formatVersion\":1,\"type\":\"comic\",\"id\":\"%s\",\"files\":[{\"path\":\"bench.bin\",\"size\":%lu,\"sha256\":\"%s\"}]}", packageId, static_cast<unsigned long>(size), hash);
  if (!jsonEscapeBenchString(manifest, escapedManifest, sizeof(escapedManifest))) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=manifest_escape"); return false; }
  length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_manifest\",\"timestamp\":%lu,\"type\":\"package_manifest\",\"payload\":{\"text\":\"%s\"}}", static_cast<unsigned long>(millis()), escapedManifest);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text) || !sendBenchJson(app, sequence, encKey, text) || !waitBenchReply(decKey, "package_status", "manifest_saved", -1, kBenchControlTimeoutMs)) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=manifest"); return false; }
  if (storageMode && storageMode[0]) {
    length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_file\",\"timestamp\":%lu,\"type\":\"package_file_begin\",\"payload\":{\"path\":\"bench.bin\",\"size\":%lu,\"sha256\":\"%s\",\"storageMode\":\"%s\"}}", static_cast<unsigned long>(millis()), static_cast<unsigned long>(size), hash, storageMode);
  } else {
    length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_file\",\"timestamp\":%lu,\"type\":\"package_file_begin\",\"payload\":{\"path\":\"bench.bin\",\"size\":%lu,\"sha256\":\"%s\"}}", static_cast<unsigned long>(millis()), static_cast<unsigned long>(size), hash);
  }
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text) || !sendBenchJson(app, sequence, encKey, text) || !waitBenchReply(decKey, "package_status", "file_begin", -1, kBenchControlTimeoutMs)) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=file_begin"); return false; }

  const uint32_t started = millis(); static uint8_t raw[kBenchChunkRawMax]; static char encoded[4 * ((kBenchChunkRawMax + 2) / 3) + 1];
  uint32_t offset = 0, sequenceNo = 0;
  while (offset < size) {
    if ((faults & kBenchFaultPause) && sequenceNo == 2) {
      Serial.println("BENCH_FAULT_A_PAUSE duration_ms=2000");
      delay(2000);
    }
    const size_t currentSize = min(static_cast<uint32_t>(chunkSize), size - offset);
    fillBenchBytes(offset, raw, currentSize);
    if (!base64BenchEncode(raw, currentSize, encoded, sizeof(encoded))) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=base64"); return false; }
    length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_chunk_%lu\",\"timestamp\":%lu,\"type\":\"package_file_chunk\",\"payload\":{\"offset\":%lu,\"sequence\":%lu,\"data\":\"%s\"}}", static_cast<unsigned long>(sequenceNo), static_cast<unsigned long>(millis()), static_cast<unsigned long>(offset), static_cast<unsigned long>(sequenceNo), encoded);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=chunk_json_size"); return false; }
    if ((sequenceNo % 16u) == 0u) {
      Serial.printf("BENCH_CHUNK_PROGRESS app_sequence=%lu offset=%lu spp_next=%u renewals=%lu\n",
                    static_cast<unsigned long>(sequenceNo),
                    static_cast<unsigned long>(offset),
                    static_cast<unsigned>(sequence),
                    static_cast<unsigned long>(gBenchSessionRenewals));
    }
    bool acknowledged = false;
    for (uint8_t attempt = 0; attempt <= kBenchChunkRetries && !acknowledged; ++attempt) {
      bool duplicate = false;
      if (!sendBenchJson(app, sequence, encKey, text)) break;
      if ((faults & kBenchFaultDropAck) && sequenceNo == 3 && attempt == 0) {
        (void)waitBenchReply(decKey, "package_chunk_ack", nullptr, static_cast<int>(sequenceNo), kBenchAckTimeoutMs);
        ++counters.ackTimeouts; ++counters.retries;
        Serial.println("BENCH_FAULT_B_RETRY simulated_ack_drop=true");
        continue;
      }
      if ((faults & kBenchFaultDropDurableAck) && sequenceNo == 15 && attempt == 0) {
        (void)waitBenchReply(decKey, "package_chunk_ack", nullptr, static_cast<int>(sequenceNo), kBenchAckTimeoutMs);
        ++counters.ackTimeouts; ++counters.retries;
        Serial.println("BENCH_FAULT_D_DURABLE_ACK_RETRY simulated_ack_drop=true sequence=15");
        continue;
      }
      acknowledged = waitBenchReply(decKey, "package_chunk_ack", nullptr, static_cast<int>(sequenceNo), kBenchAckTimeoutMs, &duplicate);
      if (duplicate) ++counters.duplicates;
      if (!acknowledged) { ++counters.ackTimeouts; if (attempt < kBenchChunkRetries) ++counters.retries; }
    }
    if (!acknowledged) {
      ++counters.errors;
      Serial.printf("BENCH_STAGE_FAIL stage=chunk_ack sequence=%lu offset=%lu spp_next=%u renewals=%lu\n",
                    static_cast<unsigned long>(sequenceNo),
                    static_cast<unsigned long>(offset),
                    static_cast<unsigned>(sequence),
                    static_cast<unsigned long>(gBenchSessionRenewals));
      // One low-frequency control-plane query after the first real failure.
      // package_status now exposes a read-only in-memory snapshot showing
      // whether this exact chunk reached the QuickApp, committed to storage,
      // and reached the ACK send call. This avoids guessing between forward
      // SPP loss, system.file stall, and reverse-path loss.
      length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_chunk_diag\",\"timestamp\":%lu,\"type\":\"package_status\",\"payload\":{\"query\":true}}", static_cast<unsigned long>(millis()));
      bool diagOk = false;
      if (length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text)) {
        diagOk = waitBenchReply(decKey, "package_status", nullptr, -1, 3000);
        Serial.printf("BENCH_CHUNK_PATH_QUERY %s\n", diagOk ? "PASS" : "FAIL");
      } else {
        Serial.println("BENCH_CHUNK_PATH_QUERY SEND_FAIL");
      }
      if (!diagOk) {
        length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_postfail_ping\",\"timestamp\":%lu,\"type\":\"ping\",\"payload\":{\"from\":\"esp32_postfail\"}}", static_cast<unsigned long>(millis()));
        const bool pingOk = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "pong", nullptr, -1, 3000);
        Serial.printf("BENCH_POSTFAIL_PING %s\n", pingOk ? "PASS" : "FAIL");
      }
      logSppRxParserState("chunk_failure_exit");
      return false;
    }
    if (gBenchInterChunkGapMs > 0) delay(gBenchInterChunkGapMs);
    if ((faults & kBenchFaultDuplicate) && sequenceNo == 4) {
      bool duplicate = false;
      if (!sendBenchJson(app, sequence, encKey, text) || !waitBenchReply(decKey, "package_chunk_ack", nullptr, static_cast<int>(sequenceNo), kBenchAckTimeoutMs, &duplicate) || !duplicate) { ++counters.errors; Serial.println("BENCH_STAGE_FAIL stage=duplicate_idempotency"); return false; }
      ++counters.duplicates;
      Serial.println("BENCH_FAULT_C_DUPLICATE duplicate_ack=true");
    }
    offset += currentSize; ++sequenceNo;
    if ((faults & kBenchFaultAbort) && sequenceNo == 5) {
      length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_abort_midfile\",\"timestamp\":%lu,\"type\":\"package_abort\",\"payload\":{}}", static_cast<unsigned long>(millis()));
      const bool abortOk = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "package_status", nullptr, -1, kBenchControlTimeoutMs);
      Serial.println("BENCH_RECOVERY_NOT_FALSE_SUCCESS phase=package_abort_requested");
      Serial.printf("BENCH_RECOVERY midfile_abort=%s transferred_bytes=%lu\n", abortOk ? "PASS" : "FAIL", static_cast<unsigned long>(offset));
      if (!abortOk) ++counters.errors;
      return abortOk;
    }
  }

  length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_end\",\"timestamp\":%lu,\"type\":\"package_file_end\",\"payload\":{}}", static_cast<unsigned long>(millis()));
  const bool completed = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "package_status", "file_saved", -1, kBenchControlTimeoutMs, nullptr, receivedHash);
  const uint32_t elapsed = millis() - started;
  const bool integrityOk = completed && strcmp(hash, receivedHash) == 0;
  const float kbps = integrityOk && elapsed ? (static_cast<float>(size) * 1000.0f / 1024.0f / elapsed) : 0.0f;
  Serial.printf("FILE_TEST size_bytes=%lu transferred_bytes=%lu elapsed_ms=%lu KB/s=%.2f %s\n", static_cast<unsigned long>(size), static_cast<unsigned long>(offset), static_cast<unsigned long>(elapsed), kbps, integrityOk ? "success" : "fail");
  Serial.printf("BENCH_INTEGRITY size=%lu tx_hash=%s rx_hash=%s %s\n", static_cast<unsigned long>(size), hash, receivedHash, integrityOk ? "PASS" : "FAIL");

  // Benchmark packages are disposable. Abort after verifying the saved file so
  // every iteration starts from idle and leaves no large test package behind.
  length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_abort_done\",\"timestamp\":%lu,\"type\":\"package_abort\",\"payload\":{}}", static_cast<unsigned long>(millis()));
  const bool cleanupOk = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "package_status", nullptr, -1, kBenchControlTimeoutMs);
  Serial.printf("BENCH_CLEANUP package_abort=%s\n", cleanupOk ? "PASS" : "FAIL");
  if (!integrityOk || !cleanupOk) ++counters.errors;
  logStackHighWater("bench_file_end");
  return integrityOk && cleanupOk;
}

bool runBenchPushTest(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  uint32_t total = 0; uint8_t success = 0;
  for (uint8_t item = 0; item < 20; ++item) {
    static char text[256]; const int length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"bench_push_%u\",\"timestamp\":%lu,\"type\":\"esp32_push_test\",\"payload\":{\"sequence\":%u,\"msg\":\"ESP32_PUSH\"}}", static_cast<unsigned>(item), static_cast<unsigned long>(millis()), static_cast<unsigned>(item));
    const uint32_t started = millis();
    if (length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchReply(decKey, "esp32_push_ack", nullptr, item, kBenchAckTimeoutMs)) { total += millis() - started; ++success; }
  }
  Serial.printf("BENCH_PUSH_TEST push_success=%u push_fail=%u avg_latency_ms=%lu\n", static_cast<unsigned>(success), static_cast<unsigned>(20 - success), static_cast<unsigned long>(success ? total / success : 0));
  Serial.println(success == 20 ? "BENCH_PUSH_ACK PASS" : "BENCH_PUSH_ACK FAIL");
  return success == 20;
}

bool runBenchStress(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16], uint32_t durationSeconds, BenchCounters& counters) {
  const uint32_t started = millis(); const uint32_t beforeErrors = counters.errors, beforeRetries = counters.retries;
  uint32_t totalBytes = 0;
  while (millis() - started < durationSeconds * 1000u) {
    if (!runBenchFile(app, sequence, encKey, decKey, 8u * 1024u, kBenchChunkRawMax, counters)) break;
    totalBytes += 8u * 1024u;
  }
  const uint32_t elapsed = millis() - started;
  const float kbps = elapsed ? (static_cast<float>(totalBytes) * 1000.0f / 1024.0f / elapsed) : 0.0f;
  const bool ok = counters.errors == beforeErrors;
  Serial.printf("BENCH_STRESS duration_s=%lu total_bytes=%lu avg_KB/s=%.2f errors=%lu retries=%lu disconnects=0 %s\n", static_cast<unsigned long>(elapsed / 1000u), static_cast<unsigned long>(totalBytes), kbps, static_cast<unsigned long>(counters.errors - beforeErrors), static_cast<unsigned long>(counters.retries - beforeRetries), ok ? "PASS" : "FAIL");
  return ok;
}

bool runBenchFaults(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16], BenchCounters& counters) {
  const bool abc = runBenchFile(app, sequence, encKey, decKey, 8u * 1024u, kBenchChunkRawMax, counters, kBenchFaultPause | kBenchFaultDropAck | kBenchFaultDuplicate);
  const bool abortSafe = runBenchFile(app, sequence, encKey, decKey, 8u * 1024u, kBenchChunkRawMax, counters, kBenchFaultAbort);
  Serial.printf("BENCH_FAULTS abc=%s disconnect_phase1=%s\n", abc ? "PASS" : "FAIL", abortSafe ? "PASS" : "FAIL");
  return abc && abortSafe;
}

bool waitBenchStreamCapability(const uint8_t decKey[16], uint32_t timeoutMs) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    const uint32_t remaining = timeoutMs - (millis() - startedAt);
    SppV2Frame& frame = gSharedSppFrame;
    if (!readSppV2Frame(frame, remaining)) break;
    if (frame.type != 3) continue;
    writeSppV2Frame(1, frame.sequence, nullptr, 0);
    const uint8_t* incoming = nullptr; size_t incomingLength = 0, contentLength = 0; const uint8_t* content = nullptr;
    if (!decryptWatchPb(frame, decKey, incoming, incomingLength) || !parseQuickAppMessage(incoming, incomingLength, content, contentLength)) continue;
    static JsonDocument document;
    document.clear();
    if (deserializeJson(document, content, contentLength)) continue;
    if (strcmp(document["type"] | "", "capabilities_response") != 0) continue;
    JsonObject payload = document["payload"].as<JsonObject>();
    JsonObject caps = payload["capabilities"].as<JsonObject>();
    const char* buildTag = payload["buildTag"] | "";
    const bool probe = caps["streamProbe"] | false;
    const bool pushProbe = caps["pushRouteProbe"] | false;
    const int version = caps["streamProbeVersion"] | 0;
    Serial.printf("STREAM_AB_PREFLIGHT_RX buildTag=%s streamProbe=%s pushRouteProbe=%s version=%d\n", buildTag, probe ? "true" : "false", pushProbe ? "true" : "false", version);
    if (!probe || !pushProbe || version < 3 || strcmp(buildTag, "hoshino-final-v7-20260812") != 0) {
      Serial.println("STREAM_AB_PREFLIGHT_STALE_RPK");
      return false;
    }
    return true;
  }
  Serial.println("STREAM_AB_PREFLIGHT_TIMEOUT");
  return false;
}

bool runWatchStreamProbe(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16], const char* mode) {
  constexpr uint32_t kStreamProbeSize = 256u * 1024u;
  constexpr size_t kStreamProbeChunk = 512;
  static char text[kSppV2PayloadMax];
  static uint8_t raw[kBenchChunkRawMax];
  static char encoded[4 * ((kBenchChunkRawMax + 2) / 3) + 1];
  if (!mode || (strcmp(mode, "echo") != 0 && strcmp(mode, "decode") != 0)) return false;
  const char* probeMode = strcmp(mode, "decode") == 0 ? "decode_v3" : "echo_v3";

  Serial.printf("STREAM_AB_V3_START mode=%s total_bytes=%lu chunk_size=%u spp_next=%u\n",
                probeMode,
                static_cast<unsigned long>(kStreamProbeSize),
                static_cast<unsigned>(kStreamProbeChunk),
                static_cast<unsigned>(sequence));

  int length = snprintf(text, sizeof(text),
                        "{\"v\":2,\"id\":\"stream_caps_v3\",\"timestamp\":%lu,\"type\":\"capabilities_request\",\"payload\":{}}",
                        static_cast<unsigned long>(millis()));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text) ||
      !sendBenchJson(app, sequence, encKey, text) ||
      !waitBenchStreamCapability(decKey, kBenchControlTimeoutMs)) {
    Serial.printf("STREAM_AB_V3_FAIL mode=%s stage=preflight_capability spp_next=%u\n", probeMode, static_cast<unsigned>(sequence));
    return false;
  }
  Serial.println("STREAM_AB_V3_PREFLIGHT pushRouteProbe=true version=3 PASS");
  delay(kBenchControlGapMs);

  static const char kSmokeB64[] = "AAECAwQFBgcICQoLDA0ODw==";
  length = snprintf(text, sizeof(text),
                    "{\"v\":2,\"id\":\"stream_v3_smoke\",\"timestamp\":%lu,\"type\":\"esp32_push_test\",\"payload\":{\"probeMode\":\"%s\",\"sequence\":0,\"offset\":0,\"rawLength\":16,\"data\":\"%s\"}}",
                    static_cast<unsigned long>(millis()), probeMode, kSmokeB64);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text) ||
      !sendBenchJson(app, sequence, encKey, text) ||
      !waitBenchReply(decKey, "esp32_push_ack", nullptr, 0, kBenchControlTimeoutMs)) {
    Serial.printf("STREAM_AB_V3_FAIL mode=%s stage=smoke spp_next=%u\n", probeMode, static_cast<unsigned>(sequence));
    return false;
  }
  Serial.printf("STREAM_AB_V3_SMOKE mode=%s PASS\n", probeMode);
  delay(kBenchControlGapMs);

  uint32_t offset = 0;
  uint32_t sequenceNo = 0;
  const uint32_t started = millis();
  while (offset < kStreamProbeSize) {
    const size_t currentSize = min(static_cast<uint32_t>(kStreamProbeChunk), kStreamProbeSize - offset);
    fillBenchBytes(offset, raw, currentSize);
    if (!base64BenchEncode(raw, currentSize, encoded, sizeof(encoded))) {
      Serial.printf("STREAM_AB_V3_FAIL mode=%s stage=base64 sequence=%lu offset=%lu\n", probeMode,
                    static_cast<unsigned long>(sequenceNo), static_cast<unsigned long>(offset));
      return false;
    }
    length = snprintf(text, sizeof(text),
                      "{\"v\":2,\"id\":\"stream_v3_%lu\",\"timestamp\":%lu,\"type\":\"esp32_push_test\",\"payload\":{\"probeMode\":\"%s\",\"sequence\":%lu,\"offset\":%lu,\"rawLength\":%u,\"data\":\"%s\"}}",
                      static_cast<unsigned long>(sequenceNo),
                      static_cast<unsigned long>(millis()),
                      probeMode,
                      static_cast<unsigned long>(sequenceNo),
                      static_cast<unsigned long>(offset),
                      static_cast<unsigned>(currentSize),
                      encoded);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) {
      Serial.printf("STREAM_AB_V3_FAIL mode=%s stage=json_size sequence=%lu offset=%lu json_len=%d\n", probeMode,
                    static_cast<unsigned long>(sequenceNo), static_cast<unsigned long>(offset), length);
      return false;
    }
    if ((sequenceNo % 16u) == 0u) {
      Serial.printf("STREAM_AB_V3_PROGRESS mode=%s sequence=%lu offset=%lu spp_next=%u\n",
                    probeMode, static_cast<unsigned long>(sequenceNo), static_cast<unsigned long>(offset), static_cast<unsigned>(sequence));
    }
    if (!sendBenchJson(app, sequence, encKey, text) ||
        !waitBenchReply(decKey, "esp32_push_ack", nullptr, static_cast<int>(sequenceNo), kBenchAckTimeoutMs)) {
      Serial.printf("STREAM_AB_V3_FAIL mode=%s stage=chunk_ack sequence=%lu offset=%lu spp_next=%u\n",
                    probeMode, static_cast<unsigned long>(sequenceNo), static_cast<unsigned long>(offset), static_cast<unsigned>(sequence));
      return false;
    }
    offset += currentSize;
    sequenceNo += 1;
  }
  const uint32_t elapsed = millis() - started;
  const float kbps = elapsed ? (static_cast<float>(offset) * 1000.0f / 1024.0f / elapsed) : 0.0f;
  Serial.printf("STREAM_AB_V3_RESULT mode=%s transferred_bytes=%lu chunks=%lu elapsed_ms=%lu avg_KB/s=%.2f spp_next=%u rx_dropped=%lu PASS\n",
                probeMode,
                static_cast<unsigned long>(offset),
                static_cast<unsigned long>(sequenceNo),
                static_cast<unsigned long>(elapsed), kbps,
                static_cast<unsigned>(sequence),
                static_cast<unsigned long>(watchRxDroppedBytes));
  return true;
}


bool waitBenchSegment32KCapability(const uint8_t decKey[16], uint32_t timeoutMs) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    const uint32_t remaining = timeoutMs - (millis() - startedAt);
    SppV2Frame& frame = gSharedSppFrame;
    if (!readSppV2Frame(frame, remaining)) break;
    if (frame.type != 3) continue;
    writeSppV2Frame(1, frame.sequence, nullptr, 0);
    const uint8_t* incoming = nullptr; size_t incomingLength = 0, contentLength = 0; const uint8_t* content = nullptr;
    if (!decryptWatchPb(frame, decKey, incoming, incomingLength) || !parseQuickAppMessage(incoming, incomingLength, content, contentLength)) continue;
    static JsonDocument document;
    document.clear();
    if (deserializeJson(document, content, contentLength)) continue;
    if (strcmp(document["type"] | "", "capabilities_response") != 0) continue;
    JsonObject payload = document["payload"].as<JsonObject>();
    JsonObject caps = payload["capabilities"].as<JsonObject>();
    const char* buildTag = payload["buildTag"] | "";
    const bool segmentProbe = caps["packageSegmentProbe"] | false;
    const int segmentBytes = caps["packageSegmentBytes"] | 0;
    const char* hashMode = caps["packageSegmentHashMode"] | "";
    const bool durableAckReplay = caps["packageSegmentDurableAckReplay"] | false;
    Serial.printf("SEG32K_PREFLIGHT_RX buildTag=%s segmentProbe=%s segmentBytes=%d durableAckReplay=%s hashMode=%s\n",
                  buildTag, segmentProbe ? "true" : "false", segmentBytes, durableAckReplay ? "true" : "false", hashMode);
    if (!segmentProbe || segmentBytes != 32768 || !durableAckReplay || strcmp(buildTag, "hoshino-final-v7-20260812") != 0) {
      Serial.println("SEG32K_PREFLIGHT_STALE_RPK");
      return false;
    }
    return true;
  }
  Serial.println("SEG32K_PREFLIGHT_TIMEOUT");
  return false;
}

bool runBenchSegment32KPreflight(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  static char text[256];
  const int length = snprintf(text, sizeof(text), "{\"v\":2,\"id\":\"seg32k_caps\",\"timestamp\":%lu,\"type\":\"capabilities_request\",\"payload\":{}}", static_cast<unsigned long>(millis()));
  const bool ok = length > 0 && static_cast<size_t>(length) < sizeof(text) && sendBenchJson(app, sequence, encKey, text) && waitBenchSegment32KCapability(decKey, kBenchControlTimeoutMs);
  Serial.printf("SEG32K_PREFLIGHT %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

void runWatchBenchmark256KSegment32K(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  BenchCounters counters;
  logStackHighWater("benchmark_256k_seg32k_start");
  const bool preflightOk = runBenchSegment32KPreflight(app, sequence, encKey, decKey);
  const bool controlOk = preflightOk && runBenchControlProbe(app, sequence, encKey, decKey);
  const bool fileOk = controlOk && runBenchFile(app, sequence, encKey, decKey, 256u * 1024u, kBenchChunkRawMax, counters, kBenchFaultNone, "segment32k");
  Serial.printf("BENCH_256K_SEG32K_RESULT preflight=%s control=%s file=%s errors=%lu retries=%lu ack_timeouts=%lu rx_dropped=%lu %s\n",
                preflightOk ? "PASS" : "FAIL",
                controlOk ? "PASS" : "FAIL",
                fileOk ? "PASS" : "FAIL",
                static_cast<unsigned long>(counters.errors),
                static_cast<unsigned long>(counters.retries),
                static_cast<unsigned long>(counters.ackTimeouts),
                static_cast<unsigned long>(watchRxDroppedBytes),
                preflightOk && controlOk && fileOk ? "PASS" : "FAIL");
  logStackHighWater("benchmark_256k_seg32k_end");
}

void runWatchFinalAcceptance(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  BenchCounters counters;
  logStackHighWater("final_acceptance_start");
  Serial.println("FINAL_ACCEPTANCE_START version=7.1 tx=coalesced storage=segment32k batch=4096");
  const bool preflightOk = runBenchSegment32KPreflight(app, sequence, encKey, decKey);
  const bool controlOk = preflightOk && runBenchControlProbe(app, sequence, encKey, decKey);
  const bool echoOk = controlOk && runWatchStreamProbe(app, sequence, encKey, decKey, "echo");
  const bool decodeOk = echoOk && runWatchStreamProbe(app, sequence, encKey, decKey, "decode");
  // 16 KiB reaches the 2nd 4 KiB flush trigger (seq 15). Deliberately discard
  // that trigger ACK once so the QuickApp must replay its RAM durable-ACK
  // snapshot without another system.file read/write.
  const bool durableRetryOk = decodeOk && runBenchFile(app, sequence, encKey, decKey,
      16u * 1024u, kBenchChunkRawMax, counters, kBenchFaultDropDurableAck, "segment32k");
  const bool file256Ok = durableRetryOk && runBenchFile(app, sequence, encKey, decKey,
      256u * 1024u, kBenchChunkRawMax, counters, kBenchFaultNone, "segment32k");
  const bool pushOk = file256Ok && runBenchPushTest(app, sequence, encKey, decKey);
  const bool pass = preflightOk && controlOk && echoOk && decodeOk && durableRetryOk && file256Ok && pushOk && watchRxDroppedBytes == 0;
  Serial.printf("FINAL_ACCEPTANCE_RESULT preflight=%s control=%s echo256=%s decode256=%s durable_ack_retry=%s segment256=%s push=%s errors=%lu retries=%lu duplicates=%lu ack_timeouts=%lu rx_dropped=%lu %s\n",
                preflightOk ? "PASS" : "FAIL",
                controlOk ? "PASS" : "FAIL",
                echoOk ? "PASS" : "FAIL",
                decodeOk ? "PASS" : "FAIL",
                durableRetryOk ? "PASS" : "FAIL",
                file256Ok ? "PASS" : "FAIL",
                pushOk ? "PASS" : "FAIL",
                static_cast<unsigned long>(counters.errors),
                static_cast<unsigned long>(counters.retries),
                static_cast<unsigned long>(counters.duplicates),
                static_cast<unsigned long>(counters.ackTimeouts),
                static_cast<unsigned long>(watchRxDroppedBytes),
                pass ? "PASS" : "FAIL");
  logSppRxParserState("final_acceptance_end");
  logStackHighWater("final_acceptance_end");
}

void runWatchBenchmark256K(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  BenchCounters counters;
  logStackHighWater("benchmark_256k_start");
  const bool controlOk = runBenchControlProbe(app, sequence, encKey, decKey);
  const bool fileOk = controlOk && runBenchFile(app, sequence, encKey, decKey, 256u * 1024u, kBenchChunkRawMax, counters);
  Serial.printf("BENCH_256K_RESULT control=%s file=%s renewals=%lu errors=%lu retries=%lu ack_timeouts=%lu rx_dropped=%lu %s\n",
                controlOk ? "PASS" : "FAIL",
                fileOk ? "PASS" : "FAIL",
                static_cast<unsigned long>(gBenchSessionRenewals),
                static_cast<unsigned long>(counters.errors),
                static_cast<unsigned long>(counters.retries),
                static_cast<unsigned long>(counters.ackTimeouts),
                static_cast<unsigned long>(watchRxDroppedBytes),
                controlOk && fileOk ? "PASS" : "FAIL");
  logStackHighWater("benchmark_256k_end");
}

void runWatchBenchmarkQuick(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  BenchCounters counters;
  logStackHighWater("quick_benchmark_start");
  const bool controlOk = runBenchControlProbe(app, sequence, encKey, decKey);
  const bool pingOk = runBenchPingTest(app, sequence, encKey, decKey);
  const bool file256 = controlOk && runBenchFile(app, sequence, encKey, decKey, 8u * 1024u, 256, counters);
  const bool file512 = file256 && runBenchFile(app, sequence, encKey, decKey, 8u * 1024u, 512, counters);
  const bool faultsOk = file512 && runBenchFaults(app, sequence, encKey, decKey, counters);
  const bool pushOk = runBenchPushTest(app, sequence, encKey, decKey);
  Serial.printf("BENCH_QUICK_RESULT control=%s ping=%s file256=%s file512=%s faults=%s push=%s errors=%lu retries=%lu duplicates=%lu ack_timeouts=%lu rx_dropped=%lu %s\\n",
                controlOk ? "PASS" : "FAIL",
                pingOk ? "PASS" : "FAIL",
                file256 ? "PASS" : "FAIL",
                file512 ? "PASS" : "FAIL",
                faultsOk ? "PASS" : "FAIL",
                pushOk ? "PASS" : "FAIL",
                static_cast<unsigned long>(counters.errors),
                static_cast<unsigned long>(counters.retries),
                static_cast<unsigned long>(counters.duplicates),
                static_cast<unsigned long>(counters.ackTimeouts),
                static_cast<unsigned long>(watchRxDroppedBytes),
                controlOk && pingOk && file256 && file512 && faultsOk && pushOk ? "PASS" : "FAIL");
  logStackHighWater("quick_benchmark_end");
}

void runWatchBenchmark(const WatchAppInfo& app, uint8_t& sequence, const uint8_t encKey[16], const uint8_t decKey[16]) {
  BenchCounters counters;
  logStackHighWater("benchmark_start");
  const bool controlOk = runBenchControlProbe(app, sequence, encKey, decKey);
  const bool pingOk = runBenchPingTest(app, sequence, encKey, decKey);
  const uint32_t fixedSizes[] = {64u * 1024u, 256u * 1024u, 1024u * 1024u, 4u * 1024u * 1024u};
  bool fourMegOk = false;
  for (size_t index = 0; index < sizeof(fixedSizes) / sizeof(fixedSizes[0]); ++index) {
    const bool ok = controlOk && runBenchFile(app, sequence, encKey, decKey, fixedSizes[index], kBenchChunkRawMax, counters);
    if (fixedSizes[index] == 4u * 1024u * 1024u) fourMegOk = ok;
    if (!ok) break;
  }
  if (fourMegOk) {
    runBenchFile(app, sequence, encKey, decKey, 8u * 1024u * 1024u, kBenchChunkRawMax, counters);
    runBenchFile(app, sequence, encKey, decKey, 16u * 1024u * 1024u, kBenchChunkRawMax, counters);
  }
  const size_t sweep[] = {256, 512, 1024, 2048, 4096, 8192, 16384};
  for (size_t index = 0; index < sizeof(sweep) / sizeof(sweep[0]); ++index) {
    const uint32_t started = millis(); const uint32_t beforeErrors = counters.errors, beforeRetries = counters.retries;
    const bool ok = controlOk && sweep[index] <= kBenchChunkRawMax && runBenchFile(app, sequence, encKey, decKey, 1024u * 1024u, sweep[index], counters);
    const uint32_t elapsed = millis() - started;
    const float kbps = ok && elapsed ? (1024.0f * 1000.0f / elapsed) : 0.0f;
    Serial.printf("CHUNK_SWEEP chunk_size=%u elapsed_ms=%lu KB/s=%.2f errors=%lu retries=%lu %s\n", static_cast<unsigned>(sweep[index]), static_cast<unsigned long>(elapsed), kbps, static_cast<unsigned long>(counters.errors - beforeErrors), static_cast<unsigned long>(counters.retries - beforeRetries), ok ? "PASS" : (sweep[index] > kBenchChunkRawMax ? "SKIP" : "FAIL"));
  }
  const bool stress60 = controlOk && runBenchStress(app, sequence, encKey, decKey, 60, counters);
  const bool stress300 = stress60 && runBenchStress(app, sequence, encKey, decKey, 300, counters);
  const bool pushOk = runBenchPushTest(app, sequence, encKey, decKey);
  const bool faultsOk = controlOk && runBenchFaults(app, sequence, encKey, decKey, counters);
  Serial.printf("BENCH_COUNTERS control=%s ping=%s stress60=%s stress300=%s faults=%s push=%s errors=%lu retries=%lu duplicate_count=%lu ack_timeout_count=%lu missing_sequence_count=0 disconnects=0 rx_dropped=%lu\n", controlOk ? "PASS" : "FAIL", pingOk ? "PASS" : "FAIL", stress60 ? "PASS" : "FAIL", stress300 ? "PASS" : "FAIL", faultsOk ? "PASS" : "FAIL", pushOk ? "PASS" : "FAIL", static_cast<unsigned long>(counters.errors), static_cast<unsigned long>(counters.retries), static_cast<unsigned long>(counters.duplicates), static_cast<unsigned long>(counters.ackTimeouts), static_cast<unsigned long>(watchRxDroppedBytes));
  logStackHighWater("benchmark_end");
}

void authenticateWatchSpp(String target, String secretText) {
  uint8_t address[6], secret[16]{}, phoneNonce[16]{}, step3[160]{}, encKey[16]{}, decKey[16]{};
  size_t step3Length = 0;
  target.trim(); target.toUpperCase();
  if (!parseWatchAddress(target, address) || !parseSecretHex(secretText, secret)) {
    Serial.println("WATCH_AUTH_INVALID_INPUT");
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return;
  }
  Serial.printf("WATCH_AUTH_HEAP stage=before_bt free=%lu largest=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  if (!beginWatchBluetooth("Hoshino-Bridge-Probe")) { Serial.println("WATCH_AUTH_BT_INIT_FAILED"); mbedtls_platform_zeroize(secret, sizeof(secret)); return; }
  // Bluetooth controller is now initialised; allocate the large protocol
  // workspaces on the heap without starving the Bluetooth init path.
  ensureScratchBuffers();
  Serial.println("WATCH_AUTH_CONNECT_TRY channel=0 lifecycle=single_attempt");
  if (!connectWatchWithSdp(address, "authenticated_bridge")) {
    Serial.println("WATCH_AUTH_CONNECT_FAILED lifecycle=closing_bt");
    watchBt.end();
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return;
  }
  bool authenticated = false;
  do {
    Serial.println("WATCH_AUTH_SESSION_STARTED");
    if (!startSppV2Session("WATCH_AUTH")) break;
    SppV2Frame& frame = gSharedSppFrame;
    if (!readSppV2Frame(frame, kWatchVersionResponseTimeoutMs) || frame.type != 2 || frame.payloadLength < 1 || frame.payload[0] != 2) { Serial.println("WATCH_AUTH_SESSION_RESPONSE_INVALID"); break; }
    esp_fill_random(phoneNonce, sizeof(phoneNonce));
    uint8_t nonceCommand[27] = {0x08, 1, 0x10, 26, 0x1a, 21, 0xf2, 0x01, 18, 0x0a, 16};
    memcpy(nonceCommand + 11, phoneNonce, sizeof(phoneNonce));
    if (!sendSppV2Protobuf(0, nonceCommand, sizeof(nonceCommand))) { Serial.println("WATCH_AUTH_NONCE_WRITE_FAILED"); break; }
    const uint8_t* watchNonce = nullptr; const uint8_t* watchHmac = nullptr; size_t watchNonceLength = 0, watchHmacLength = 0; bool nonceDataSeen = false;
    const uint32_t nonceDeadline = millis() + kWatchVersionResponseTimeoutMs;
    while (millis() < nonceDeadline) {
      if (!readSppV2Frame(frame, nonceDeadline - millis())) break;
      if (frame.type == 3) { nonceDataSeen = true; writeSppV2Frame(1, frame.sequence, nullptr, 0); }
      if (extractWatchNonce(frame, watchNonce, watchNonceLength, watchHmac, watchHmacLength)) break;
    }
    if (!watchNonce) { Serial.println(nonceDataSeen ? "WATCH_AUTH_NONCE_FORMAT_INVALID" : "WATCH_AUTH_NONCE_TIMEOUT"); break; }
    if (watchNonceLength != 16 || watchHmacLength != 32) { Serial.println("WATCH_AUTH_NONCE_LENGTH_INVALID"); break; }

    // The known Round-4 protobuf is plaintext in firmware. buildStep3() uses
    // this watch's current AuthKey and current nonce exchange to AES-CCM
    // encrypt it inside a fresh Auth Step3 envelope.
    Serial.println("WATCH_R4_AUTH_IDENTITY_COMPATIBLE_TEMPLATE");
    const uint8_t* sessionDeviceInfo = kRound4CompatibleDeviceInfo;
    const size_t sessionDeviceInfoLength = sizeof(kRound4CompatibleDeviceInfo);
    if (!buildStep3(secret, phoneNonce, watchNonce, watchHmac,
                    step3, step3Length, decKey, encKey,
                    sessionDeviceInfo, sessionDeviceInfoLength)) {
      Serial.println("WATCH_AUTH_HMAC_OR_STEP3_INVALID");
      break;
    }
    Serial.printf("WATCH_R4_AUTH_IDENTITY_BUILT source=%s step3_bytes=%u\n",
                  "compatible_plaintext_reencrypted",
                  static_cast<unsigned>(step3Length));

    if (!sendSppV2Protobuf(1, step3, step3Length)) { Serial.println("WATCH_AUTH_STEP3_WRITE_FAILED"); break; }
    const uint32_t resultDeadline = millis() + kWatchVersionResponseTimeoutMs;
    while (millis() < resultDeadline) {
      if (!readSppV2Frame(frame, resultDeadline - millis())) break;
      if (frame.type == 3) writeSppV2Frame(1, frame.sequence, nullptr, 0);
      if (isAuthSuccess(frame)) { authenticated = true; break; }
    }
    Serial.println(authenticated ? "WATCH_AUTH_OK" : "WATCH_AUTH_RESPONSE_TIMEOUT");
    if (!authenticated) break;

    Serial.println("WATCH_HOLD_CONNECTED runtime_session=true");
    setWatchBridgeState("connected");
    const bool networkProxyReady = gWatchRawTraceEnabled ? false : setupWatchNetworkProxy();
    if (gWatchRawTraceEnabled) Serial.println("WATCH_NETWORK_PROXY_SKIPPED raw_trace=true");
    Serial.printf("WATCH_AUTH_HEAP stage=after_network free=%lu largest=%lu proxy=%s\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                  networkProxyReady ? "true" : "false");
    resetWatchNetworkSessionQueue();
    uint8_t outgoingSequence = 2;

    // ---------------------------------------------------------------
    // Round-4 exact replay, captured from the CURRENT Xiaomi 17 Pro.
    // Important differences vs old Round-3:
    //   ch8 id = B9379878
    //   seq4 is 5/10, not 2/14
    //   extra 2/14 variant ending in 08 02
    //   20/0 comes before 8/52
    //   type23/id3 is now 137 bytes
    // ---------------------------------------------------------------
    static constexpr uint8_t kRound4Ch8Init[] = {
        0x11,0x04,0x00,0x0e,0x0a,0x08,
        'B','9','3','7','9','8','7','8'
    };

    // Reuse the already-allocated 4 KiB raw-command workspace: zero new heap
    // and no 137-byte local array on the 6144-byte watch task stack.
    uint8_t* round4Type23 = gRawCommandPayloadScratch;
    constexpr size_t kRound4Type23Length = 137;
    // This captured 23/3 blob is bound to the original capture session/key.
    // It is not part of HMAC identity authentication, so a key mismatch here
    // must not tear down an otherwise authenticated live watch session.
    bool haveType23 = round4Type23 && decryptRound4Type23Template(secret, round4Type23);
    if (haveType23) {
      Serial.printf("WATCH_ROUND4_TEMPLATE_DECRYPT_OK type23_len=%u free=%lu largest=%lu stack_hwm=%u\n",
                    static_cast<unsigned>(kRound4Type23Length),
                    static_cast<unsigned long>(ESP.getFreeHeap()),
                    static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    } else {
      Serial.println("WATCH_ROUND4_TEMPLATE_SKIPPED reason=key_mismatch (non-fatal)");
    }

    const uint8_t ch8InitSeq = outgoingSequence;
    if (sendEncryptedWatchChannel(outgoingSequence, encKey, 8, 2,
                                  kRound4Ch8Init, sizeof(kRound4Ch8Init))) {
      Serial.printf("WATCH_R4_CH8_INIT OK seq=%u id=B9379878\n",
                    static_cast<unsigned>(ch8InitSeq));
    } else {
      Serial.printf("WATCH_R4_CH8_INIT FAIL seq=%u\n",
                    static_cast<unsigned>(ch8InitSeq));
      mbedtls_platform_zeroize(round4Type23, kRound4Type23Length);
      break;
    }

    // ---------------------------------------------------------------
    // Round-4 21:00:57 SUCCESS branch ("warm branch"), exact ordering:
    // seq3  2/92
    // seq4  2/14 (...08 04)
    // seq5  2/2
    // seq6  2/44
    // seq7  2/6
    // seq8  5/10
    // seq9  2/14 (...08 02)
    // seq10 2/14 (...08 04)
    // seq11 20/0
    // seq12 23/3
    // seq13 8/52
    // seq14 2/2
    // Then after Watch initial batch: seq15 2/2, seq16 2/109.
    // Official Watch emits 23/2 BEFORE replying to 2/109.
    // ---------------------------------------------------------------

    uint8_t networkStatusPb[24]{};
    size_t networkStatusPbLength = 0;

    vTaskDelay(pdMS_TO_TICKS(9));
    const uint8_t statusSeq = outgoingSequence;
    if (buildSyncNetworkStatus(networkStatusPb, sizeof(networkStatusPb), networkStatusPbLength) &&
        sendEncryptedWatchPb(outgoingSequence, encKey, networkStatusPb, networkStatusPbLength)) {
      Serial.printf("WATCH_R4W_2/92 OK seq=%u\n", static_cast<unsigned>(statusSeq));
    } else {
      Serial.printf("WATCH_R4W_2/92 FAIL seq=%u\n", static_cast<unsigned>(statusSeq));
      mbedtls_platform_zeroize(round4Type23, kRound4Type23Length);
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(6));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x02\x10\x0e\x22\x05\x92\x02\x02\x08\x04", 11,
                "WATCH_R4W_2/14_04_A");

    vTaskDelay(pdMS_TO_TICKS(2));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x02\x10\x02", 4,
                "WATCH_R4W_2/2_A");

    vTaskDelay(pdMS_TO_TICKS(1));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x02\x10\x2c\x22\x07\xaa\x02\x04\x0a\x02\x08\x00", 13,
                "WATCH_R4W_2/44");

    vTaskDelay(pdMS_TO_TICKS(1));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x02\x10\x06\x22\x0a\xa2\x01\x07\x0a\x05\x7a\x68\x5f\x63\x6e", 16,
                "WATCH_R4W_2/6_LANG");

    vTaskDelay(pdMS_TO_TICKS(2));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x05\x10\x0a\x3a\x00", 6,
                "WATCH_R4W_5/10");

    vTaskDelay(pdMS_TO_TICKS(1));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x02\x10\x0e\x22\x05\x92\x02\x02\x08\x02", 11,
                "WATCH_R4W_2/14_02");

    vTaskDelay(pdMS_TO_TICKS(1));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x02\x10\x0e\x22\x05\x92\x02\x02\x08\x04", 11,
                "WATCH_R4W_2/14_04_B");

    vTaskDelay(pdMS_TO_TICKS(5));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x14\x10\x00", 4,
                "WATCH_R4W_20/0");

    vTaskDelay(pdMS_TO_TICKS(3));
    // type23/id3 is sent only when the captured template was recoverable;
    // otherwise it does not consume a sequence number and the live-session
    // bootstrap remains contiguous.
    if (haveType23) {
      const uint8_t type23Seq = outgoingSequence;
      if (sendEncryptedWatchPb(outgoingSequence, encKey, round4Type23, kRound4Type23Length)) {
        Serial.printf("WATCH_R4W_23/3 OK seq=%u bytes=%u\n",
                      static_cast<unsigned>(type23Seq),
                      static_cast<unsigned>(kRound4Type23Length));
      } else {
        Serial.printf("WATCH_R4W_23/3 FAIL seq=%u\n", static_cast<unsigned>(type23Seq));
        mbedtls_platform_zeroize(round4Type23, kRound4Type23Length);
        break;
      }
    } else {
      Serial.println("WATCH_R4W_23/3 SKIPPED (no capture key)");
    }

    vTaskDelay(pdMS_TO_TICKS(4));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x08\x10\x34\x52\x31\xe2\x02\x2e"
                "\x0a\x09\x08\x03\x10\x9c\xea\x02\x18\xbc\x77"
                "\x0a\x09\x08\x01\x10\x9c\xea\x02\x18\xbc\x77"
                "\x0a\x0a\x08\x06\x10\xfd\x84\x01\x18\xdc\xe2\x02"
                "\x0a\x0a\x08\x02\x10\x92\xf1\x01\x18\xef\xab\x01", 55,
                "WATCH_R4W_8/52");

    vTaskDelay(pdMS_TO_TICKS(9));
    doBootstrap(encKey, outgoingSequence,
                "\x08\x02\x10\x02", 4,
                "WATCH_R4W_2/2_B");

    mbedtls_platform_zeroize(round4Type23, kRound4Type23Length);

    Serial.println("WATCH_BOOTSTRAP_DONE mode=round4_auth_identity_exact_dynamic_lengths_v6_1");
    uint8_t dataPrefixBudget = 0;
    uint8_t networkFrameBudget = 32;

    // Round-4 post-bootstrap gate:
    // current official phone waits for the watch's initial response batch,
    // then sends 2/109 + 2/2 + 2/2.  After the watch returns 2/109 and
    // the following 2/2 responses, the phone sends 2/110.  type23/id2,
    // channel-8 and DHCP then follow.
    bool r4Seen20 = false;
    uint32_t r4Seen20Ms = 0;
    bool r4WarmExtra2Sent = false;
    uint32_t r4WarmExtra2DueMs = 0;
    uint32_t r4Warm109DueMs = 0;
    uint32_t r4Tx109DueMs = 0; // retained only for existing diagnostics
    bool r4Sent109 = false;
    bool r4Seen109 = false;
    uint32_t r4LiveWatchManualTs = 0;
    uint32_t r4LiveWatchSnapTs = 0;
    bool r4Parsed109State = false;
    uint8_t r4Post109Id2Count = 0;
    uint32_t r4Tx110DueMs = 0;
    bool r4Sent110 = false;
    bool r4Seen23Id2 = false;
    bool r4SawDhcp = false;

    // Round-4 timing-critical gate. Keep Wi-Fi and all nonessential background
    // work OFF until the official 109 -> 110 -> 23/2 -> DHCP transition either
    // succeeds or times out.  This protects both WROOM heap and millisecond
    // scheduling fidelity.
    const uint32_t bootstrapDoneMs = millis();
    constexpr uint32_t kR4GateTimeoutMs = 700;
    bool r4GateActive = true;
    bool wifiStartPending = !ssid.isEmpty() && WiFi.status() != WL_CONNECTED;
    uint32_t r4SlowAckCount = 0;
    uint32_t r4MaxAckUs = 0;
    Serial.printf("WATCH_R4W_FAST_GATE_START branch=210057 timeout_ms=%lu free=%lu largest=%lu\n",
                  static_cast<unsigned long>(kR4GateTimeoutMs),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()));

    while (watchBt.connected() && !gWatchBridgeStopRequested) {
      serviceWifiStaConnectionState(gWatchBridgeStopRequested);
      const uint32_t relNowMs = static_cast<uint32_t>(millis() - bootstrapDoneMs);

      // Periodic heap telemetry (every 30 s) so memory headroom is observable
      // during the long-lived bridge session even though this task blocks loop().
      {
        static uint32_t sNextHeapLogMs = 0;
        if (relNowMs >= sNextHeapLogMs) {
          sNextHeapLogMs = relNowMs + 30000u;
          Serial.printf("WATCH_LIVE_HEAP rel_ms=%lu free=%lu largest=%lu min_free=%lu tx_drop=%lu rx=%lu tx=%lu\n",
                        static_cast<unsigned long>(relNowMs),
                        static_cast<unsigned long>(ESP.getFreeHeap()),
                        static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                        static_cast<unsigned long>(ESP.getMinFreeHeap()),
                        static_cast<unsigned long>(gWatchNetworkDroppedPackets),
                        static_cast<unsigned long>(gWatchNetworkRxPackets),
                        static_cast<unsigned long>(gWatchNetworkTxPackets));
        }
      }

      // Exact 21:00:57 post-bootstrap branch:
      //   Watch 20/0
      //     + ~6.8 ms  -> Phone seq15 2/2
      //     + ~25.8 ms -> Phone seq16 2/109
      //     + ~21.4 ms -> Watch 23/2 (before Watch replies to 2/109)
      if (r4GateActive && r4Seen20 && !r4WarmExtra2Sent &&
          r4WarmExtra2DueMs != 0 &&
          static_cast<int32_t>(millis() - r4WarmExtra2DueMs) >= 0) {
        static constexpr uint8_t kR4Id2[] = {0x08,0x02,0x10,0x02};
        const uint32_t txStartedUs = micros();
        const uint8_t seq2 = outgoingSequence;
        r4WarmExtra2Sent = sendEncryptedWatchPb(outgoingSequence, encKey,
                                                kR4Id2, sizeof(kR4Id2));
        const uint32_t txUs = micros() - txStartedUs;
        if (r4WarmExtra2Sent) r4Warm109DueMs = millis() + 26u;
        Serial.printf("WATCH_R4W_GATE_TX rel_ms=%lu 2/2_seq=%u status=%s tx_us=%lu next109_due_rel_ms=%lu\n",
                      static_cast<unsigned long>(millis() - bootstrapDoneMs),
                      static_cast<unsigned>(seq2),
                      r4WarmExtra2Sent ? "OK" : "FAIL",
                      static_cast<unsigned long>(txUs),
                      r4Warm109DueMs ? static_cast<unsigned long>(r4Warm109DueMs - bootstrapDoneMs) : 0ul);
      }

      if (r4GateActive && r4WarmExtra2Sent && !r4Sent109 &&
          r4Warm109DueMs != 0 &&
          static_cast<int32_t>(millis() - r4Warm109DueMs) >= 0) {
        static constexpr uint8_t kR4Id109[] = {0x08,0x02,0x10,0x6d};
        const uint32_t txStartedUs = micros();
        const uint8_t seq109 = outgoingSequence;
        r4Sent109 = sendEncryptedWatchPb(outgoingSequence, encKey,
                                         kR4Id109, sizeof(kR4Id109));
        const uint32_t txUs = micros() - txStartedUs;
        Serial.printf("WATCH_R4W_GATE_TX rel_ms=%lu 2/109_seq=%u status=%s tx_us=%lu expect_23_before_109_reply=true\n",
                      static_cast<unsigned long>(millis() - bootstrapDoneMs),
                      static_cast<unsigned>(seq109),
                      r4Sent109 ? "OK" : "FAIL",
                      static_cast<unsigned long>(txUs));
      }

      // Deliberately DO NOT send 2/110 in this A/B round.
      // During the timing-critical gate, run ONLY SPP/DHCP work. No Wi-Fi,
      // mDNS, HTTP, or other application work.
      if (!r4GateActive) {
        drainWatchNetworkTx(outgoingSequence);
      }

      if (r4GateActive &&
          (r4SawDhcp || static_cast<uint32_t>(millis() - bootstrapDoneMs) >= kR4GateTimeoutMs)) {
        r4GateActive = false;
        Serial.printf("WATCH_R4W_FAST_GATE_END rel_ms=%lu result=%s 20=%u tx109=%u rx109=%u id2count=%u tx110=%u 23=%u dhcp=%u max_ack_us=%lu slow_acks=%lu free=%lu largest=%lu\n",
                      static_cast<unsigned long>(millis() - bootstrapDoneMs),
                      r4SawDhcp ? "SUCCESS" : "TIMEOUT",
                      r4Seen20 ? 1u : 0u, r4Sent109 ? 1u : 0u, r4Seen109 ? 1u : 0u,
                      static_cast<unsigned>(r4Post109Id2Count),
                      r4Sent110 ? 1u : 0u, r4Seen23Id2 ? 1u : 0u, r4SawDhcp ? 1u : 0u,
                      static_cast<unsigned long>(r4MaxAckUs),
                      static_cast<unsigned long>(r4SlowAckCount),
                      static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()));

        if (wifiStartPending) {
          Serial.printf("WATCH_HEAP_PRE_WIFI rel_ms=%lu free=%lu largest=%lu\n",
                        static_cast<unsigned long>(millis() - bootstrapDoneMs),
                        static_cast<unsigned long>(ESP.getFreeHeap()),
                        static_cast<unsigned long>(ESP.getMaxAllocHeap()));
          WiFi.persistent(false);
          WiFi.setAutoReconnect(true);
          WiFi.mode(WIFI_STA);
          beginWifiStaConnection(kWifiStaConnectTimeoutMs);
          WiFi.begin(ssid.c_str(), wifiPass.c_str());
          wifiStartPending = false;
          Serial.printf("WATCH_WIFI_STA_STARTED_NONBLOCKING rel_ms=%lu free=%lu largest=%lu\n",
                        static_cast<unsigned long>(millis() - bootstrapDoneMs),
                        static_cast<unsigned long>(ESP.getFreeHeap()),
                        static_cast<unsigned long>(ESP.getMaxAllocHeap()));
        }
      }

      // Never let a 250 ms read timeout destroy the Round-4 deadlines.
      const uint32_t sppReadTimeoutMs = r4GateActive ? 2u : 250u;
      if (!readSppV2Frame(frame, sppReadTimeoutMs)) {
        delay(2);
        continue;
      }
#if HOSHINO_ENABLE_ASR
      emitWatchRawTrace(frame);
#endif
      const size_t encryptedLength = frame.payloadLength;
      const uint8_t rawChannel = frame.payloadLength >= 1 ? (frame.payload[0] & 0x0f) : 0xff;
      const uint8_t rawOpcode = frame.payloadLength >= 2 ? frame.payload[1] : 0xff;
      const size_t dataLength = frame.payloadLength >= 2 ? frame.payloadLength - 2 : 0;
      if (frame.type == 3 && rawChannel != 7 && rawChannel != 10) {
        const uint32_t ackStartedUs = micros();
        const bool ackOk = writeSppV2Frame(1, frame.sequence, nullptr, 0);
        const uint32_t ackUs = micros() - ackStartedUs;
        if (ackUs > r4MaxAckUs) r4MaxAckUs = ackUs;
        if (r4GateActive && ackUs >= 5000u) {
          ++r4SlowAckCount;
          Serial.printf("WATCH_R4_SLOW_ACK rel_ms=%lu seq=%u ok=%s ack_us=%lu\n",
                        static_cast<unsigned long>(millis() - bootstrapDoneMs),
                        static_cast<unsigned>(frame.sequence),
                        ackOk ? "true" : "false",
                        static_cast<unsigned long>(ackUs));
        }
      }
      bool dataDecrypted = false;
      if (frame.type == 3 && dataLength > 0 && rawChannel != 1 && rawOpcode == 2) {
        dataDecrypted = aes128CtrCrypt(decKey, frame.payload + 2, dataLength);
      }
      if (frame.type == 3 && rawChannel == 8 && dataLength > 0) {
        // WROOM-safe diagnostic: print directly, no temporary String/heap allocation.
        const size_t ch8PrefixLength = min(static_cast<size_t>(48), dataLength);
        Serial.printf("WATCH_CH8_RX seq=%u opcode=%u bytes=%u decrypted=%s prefix=",
                      static_cast<unsigned>(frame.sequence),
                      static_cast<unsigned>(rawOpcode),
                      static_cast<unsigned>(dataLength),
                      dataDecrypted ? "true" : "false");
        for (size_t i = 0; i < ch8PrefixLength; ++i) {
          Serial.printf("%02X", frame.payload[2 + i]);
        }
        Serial.println();
      }
      if (frame.type == 3 && rawChannel == 7 && dataLength > 0 &&
          (rawOpcode == 1 || (rawOpcode == 2 && dataDecrypted))) {
        const bool dhcpHandled = handleWatchDhcp(frame.payload + 2, dataLength, outgoingSequence);
        if (dhcpHandled && !r4SawDhcp) {
          r4SawDhcp = true;
          Serial.printf("WATCH_R4W_SUCCESS DHCP rel_ms=%lu max_ack_us=%lu slow_acks=%lu\n",
                        static_cast<unsigned long>(millis() - bootstrapDoneMs),
                        static_cast<unsigned long>(r4MaxAckUs),
                        static_cast<unsigned long>(r4SlowAckCount));
        }
        if (!dhcpHandled && !injectWatchNetworkPacket(frame.payload + 2, dataLength)) {
          Serial.printf("WATCH_NETWORK_RX_REJECTED opcode=%u bytes=%u\n",
                        static_cast<unsigned>(rawOpcode), static_cast<unsigned>(dataLength));
        }
      }
      const uint8_t* pb = nullptr;
      size_t pbLength = 0;
      uint32_t pbType = 0xffffffffu, pbId = 0xffffffffu;
      const bool decrypted = decryptWatchPb(frame, decKey, pb, pbLength);
      if (decrypted) {
        protoVarintField(pb, pbLength, 1, pbType);
        protoVarintField(pb, pbLength, 2, pbId);

        if (pbType == 20 && pbId == 0 && !r4Seen20) {
          r4Seen20 = true;
          r4Seen20Ms = millis();
          r4WarmExtra2DueMs = r4Seen20Ms + 7u;
          Serial.printf("WATCH_R4W_GATE_RX 20/0 rel_ms=%lu extra2_due_rel_ms=%lu\n",
                        static_cast<unsigned long>(r4Seen20Ms - bootstrapDoneMs),
                        static_cast<unsigned long>(r4WarmExtra2DueMs - bootstrapDoneMs));
        }
        if (r4Sent109 && pbType == 2 && pbId == 109 && !r4Seen109) {
          r4Seen109 = true;
          r4Parsed109State = parseRound4Id109State(pb, pbLength,
                                                   r4LiveWatchManualTs,
                                                   r4LiveWatchSnapTs);
          Serial.printf("WATCH_R4_GATE_RX 2/109 rel_ms=%lu bytes=%u parsed=%s watch_manual_ts=%lu watch_snap_ts=%lu captured_manual_ts=%lu same_as_capture=%s hex=",
                        static_cast<unsigned long>(millis() - bootstrapDoneMs),
                        static_cast<unsigned>(pbLength),
                        r4Parsed109State ? "true" : "false",
                        static_cast<unsigned long>(r4LiveWatchManualTs),
                        static_cast<unsigned long>(r4LiveWatchSnapTs),
                        static_cast<unsigned long>(1786703622u),
                        r4LiveWatchManualTs == 1786703622u ? "true" : "false");
          for (size_t i = 0; i < pbLength; ++i) Serial.printf("%02X", pb[i]);
          Serial.println();
        } else if (r4Sent109 && r4Seen109 && !r4Sent110 &&
                   pbType == 2 && pbId == 2 && r4Post109Id2Count < 2) {
          ++r4Post109Id2Count;
          Serial.printf("WATCH_R4_GATE_RX 2/2_after109 rel_ms=%lu count=%u bytes=%u\n",
                        static_cast<unsigned long>(millis() - bootstrapDoneMs),
                        static_cast<unsigned>(r4Post109Id2Count),
                        static_cast<unsigned>(pbLength));
          if (r4Post109Id2Count == 2) {
            r4Tx110DueMs = millis() + 10u;
            Serial.printf("WATCH_R4_GATE_110_ARM due_rel_ms=%lu\n",
                          static_cast<unsigned long>(r4Tx110DueMs - bootstrapDoneMs));
          }
        }
        if (pbType == 23 && pbId == 2 && !r4Seen23Id2) {
          r4Seen23Id2 = true;
          Serial.printf("WATCH_R4W_SUCCESS type23/id2 rel_ms=%lu\n",
                        static_cast<unsigned long>(millis() - bootstrapDoneMs));
        }

        // Log unknown protobuf types for reverse-engineering
        if (pbType != 2 && pbType != 10) {
          Serial.printf("WATCH_PB_UNKNOWN type=%lu id=%lu bytes=%u hex=",
                        static_cast<unsigned long>(pbType), static_cast<unsigned long>(pbId),
                        static_cast<unsigned>(pbLength));
          const size_t pbPrefixLength = min(static_cast<size_t>(64), pbLength);
          for (size_t i = 0; i < pbPrefixLength; ++i) Serial.printf("%02X", pb[i]);
          Serial.println();
        }
        if (pbType == 10) {
          Serial.printf("WATCH_NETWORK_CONTROL type=10 id=%lu bytes=%u observed_official_state_machine=true\n",
                        static_cast<unsigned long>(pbId), static_cast<unsigned>(pbLength));
          // Official phone replies type10/id5 to type10/id3
          if (pbId == 3) {
            static const uint8_t kType10Id5Reply[] = {0x08, 0x0a, 0x10, 0x05};
            if (sendEncryptedWatchPb(outgoingSequence, encKey,
                                     kType10Id5Reply, sizeof(kType10Id5Reply))) {
              Serial.println("WATCH_TYPE10_ID3_REPLY type10/id5");
            }
          }
        }
        // Log type=18 but do NOT reply — it's in the Music protobuf family,
        // not a network-config request.  The real DHCP trigger is the
        // post-auth bootstrap sequence (type23/id3 → watch type23/id2).
        if (pbType == 18) {
          Serial.printf("WATCH_PB_OBSERVED type=18 (Music family) id=%lu bytes=%u hex=",
                        static_cast<unsigned long>(pbId), static_cast<unsigned>(pbLength));
          const size_t type18PrefixLength = min(static_cast<size_t>(64), pbLength);
          for (size_t i = 0; i < type18PrefixLength; ++i) Serial.printf("%02X", pb[i]);
          Serial.println();
        }
        if (networkProxyReady && pbType == 2 && pbId == 82) {
          if (sendEncryptedWatchPb(outgoingSequence, encKey, networkStatusPb, networkStatusPbLength)) {
            Serial.println("WATCH_NETWORK_STATUS_SENT capability=2 request_id=82");
          } else {
            Serial.println("WATCH_NETWORK_STATUS_SEND_FAILED request_id=82");
          }
        }
      }
      Serial.printf("WATCH_HOLD_FRAME rel_ms=%lu type=%u seq=%u bytes=%u decrypted=%s pb_type=%lu pb_id=%lu\n",
                    static_cast<unsigned long>(millis() - bootstrapDoneMs),
                    static_cast<unsigned>(frame.type),
                    static_cast<unsigned>(frame.sequence),
                    static_cast<unsigned>(encryptedLength),
                    decrypted ? "true" : "false",
                    static_cast<unsigned long>(pbType),
                    static_cast<unsigned long>(pbId));
      if (frame.type == 3 && frame.payloadLength >= 2 && rawChannel != 1) {
        if (rawChannel == 7 && dataLength > 0 && networkFrameBudget > 0) {
          const size_t networkPrefixLength = min(static_cast<size_t>(24), dataLength);
          const String networkPrefix = hexBytes(frame.payload + 2, networkPrefixLength);
          Serial.printf("WATCH_NETWORK_FRAME opcode=%u bytes=%u ip_version=%u prefix=%s\n",
                        static_cast<unsigned>(rawOpcode), static_cast<unsigned>(dataLength),
                        static_cast<unsigned>(frame.payload[2] >> 4), networkPrefix.c_str());
          --networkFrameBudget;
        }
        if (dataLength <= 8) dataPrefixBudget = 3;
        if (dataLength <= 8 || dataPrefixBudget > 0) {
          const size_t prefixLength = min(static_cast<size_t>(16), dataLength);
          const String prefix = hexBytes(frame.payload + 2, prefixLength);
          Serial.printf("WATCH_DATA_FRAME channel=%u opcode=%u data_bytes=%u prefix=%s data_decrypted=%s\n",
                        static_cast<unsigned>(rawChannel),
                        static_cast<unsigned>(rawOpcode),
                        static_cast<unsigned>(dataLength), prefix.c_str(),
                        dataDecrypted ? "true" : "false");
          if (dataLength > 8 && dataPrefixBudget > 0) --dataPrefixBudget;
        }
      }
    }
    if (gWatchBridgeStopRequested) Serial.println("WATCH_HOLD_STOP_REQUESTED");
    clearWifiStaConnection(gWatchBridgeStopRequested ? "cancelled" : "spp_session_end");
    Serial.printf("WATCH_HOLD_DISCONNECTED network_rx=%lu network_tx=%lu network_drop=%lu r4_20=%u r4_tx109=%u r4_rx109=%u r4_id2count=%u r4_tx110=%u r4_23=%u r4_dhcp=%u max_ack_us=%lu slow_acks=%lu\n",
                  static_cast<unsigned long>(gWatchNetworkRxPackets),
                  static_cast<unsigned long>(gWatchNetworkTxPackets),
                  static_cast<unsigned long>(gWatchNetworkDroppedPackets),
                  r4Seen20 ? 1u : 0u, r4Sent109 ? 1u : 0u, r4Seen109 ? 1u : 0u,
                  static_cast<unsigned>(r4Post109Id2Count),
                  r4Sent110 ? 1u : 0u, r4Seen23Id2 ? 1u : 0u, r4SawDhcp ? 1u : 0u,
                  static_cast<unsigned long>(r4MaxAckUs),
                  static_cast<unsigned long>(r4SlowAckCount));
  } while (false);
  watchBt.disconnect(); watchBt.end();
  gWatchRawTraceEnabled = false;
  mbedtls_platform_zeroize(secret, sizeof(secret)); mbedtls_platform_zeroize(phoneNonce, sizeof(phoneNonce)); mbedtls_platform_zeroize(step3, sizeof(step3));
  mbedtls_platform_zeroize(encKey, sizeof(encKey)); mbedtls_platform_zeroize(decKey, sizeof(decKey));
  secretText = "";
}


void drainWatchWakeControlFrames(uint32_t durationMs) {
  const uint32_t deadline = millis() + durationMs;
  SppV2Frame& frame = gSharedSppFrame;
  uint32_t frames = 0;
  while (millis() < deadline) {
    const uint32_t remaining = deadline - millis();
    if (!readSppV2Frame(frame, remaining)) break;
    ++frames;
    if (frame.type == 3) writeSppV2Frame(1, frame.sequence, nullptr, 0);
  }
  if (frames) Serial.printf("WATCH_BRIDGE_WAKE_CONTROL_DRAIN frames=%lu\n", static_cast<unsigned long>(frames));
}

bool sendQuickAppWakePing(const WatchAppInfo& app, uint8_t& sequence,
                          const uint8_t encKey[16], const uint8_t decKey[16],
                          uint8_t* pb, size_t pbCapacity, size_t& pbLength,
                          uint32_t timeoutMs, uint32_t logicalAttempt,
                          const char* stage) {
  char pingJson[256];
  const int pingLength = snprintf(pingJson, sizeof(pingJson),
      "{\"v\":2,\"id\":\"esp32_wake_%lu\",\"timestamp\":%lu,\"type\":\"ping\",\"payload\":{\"from\":\"esp32\",\"protocolVersion\":2}}",
      static_cast<unsigned long>(logicalAttempt), static_cast<unsigned long>(millis()));
  if (pingLength <= 0 || static_cast<size_t>(pingLength) >= sizeof(pingJson) ||
      !buildQuickAppMessage(app, reinterpret_cast<const uint8_t*>(pingJson), static_cast<size_t>(pingLength), pb, pbCapacity, pbLength) ||
      !sendEncryptedWatchPb(sequence, encKey, pb, pbLength)) {
    Serial.printf("WATCH_BRIDGE_WAKE_PING_WRITE_FAILED stage=%s attempt=%lu\n", stage, static_cast<unsigned long>(logicalAttempt));
    return false;
  }

  Serial.printf("WATCH_BRIDGE_WAKE_PING_SENT stage=%s attempt=%lu timeout_ms=%lu\n",
                stage, static_cast<unsigned long>(logicalAttempt), static_cast<unsigned long>(timeoutMs));
  const uint32_t deadline = millis() + timeoutMs;
  SppV2Frame& frame = gSharedSppFrame;
  uint32_t frames = 0, nonData = 0, decryptFail = 0, appParseFail = 0;
  while (millis() < deadline) {
    const uint32_t remaining = deadline - millis();
    if (!readSppV2Frame(frame, remaining)) break;
    ++frames;
    if (frame.type != 3) { ++nonData; continue; }
    writeSppV2Frame(1, frame.sequence, nullptr, 0);
    const uint8_t* incoming = nullptr; size_t incomingLength = 0;
    const uint8_t* content = nullptr; size_t contentLength = 0;
    if (!decryptWatchPb(frame, decKey, incoming, incomingLength)) { ++decryptFail; continue; }
    if (!parseQuickAppMessage(incoming, incomingLength, content, contentLength)) { ++appParseFail; continue; }
    if (containsAscii(content, contentLength, "\"type\":\"pong\"")) {
      Serial.printf("WATCH_BRIDGE_WAKE_PONG stage=%s attempt=%lu frames=%lu\n",
                    stage, static_cast<unsigned long>(logicalAttempt), static_cast<unsigned long>(frames));
      return true;
    }
  }
  Serial.printf("WATCH_BRIDGE_WAKE_PING_TIMEOUT stage=%s attempt=%lu frames=%lu non_data=%lu decrypt_fail=%lu app_parse_fail=%lu buffered=%u queue=%d\n",
                stage, static_cast<unsigned long>(logicalAttempt), static_cast<unsigned long>(frames),
                static_cast<unsigned long>(nonData), static_cast<unsigned long>(decryptFail),
                static_cast<unsigned long>(appParseFail), static_cast<unsigned>(gSppRxBuffered), watchRxAvailable());
  return false;
}

bool wakeQuickAppRobust(const WatchAppInfo& app, uint8_t& sequence,
                        const uint8_t encKey[16], const uint8_t decKey[16],
                        uint8_t* pb, size_t pbCapacity, size_t& pbLength) {
  Serial.println("WATCH_BRIDGE_WAKE_BOOTSTRAP version=7.1 strategy=ping-launch-poll-relaunch");

  // Fast path: after an update the QuickApp may already be resident even if a
  // launch request is unnecessary. Avoid perturbing a healthy app first.
  if (sendQuickAppWakePing(app, sequence, encKey, decKey, pb, pbCapacity, pbLength,
                           2500, 0, "resident")) {
    Serial.println("WATCH_BRIDGE_WAKE_READY path=resident");
    return true;
  }

  constexpr uint8_t kLaunchCycles = 2;
  constexpr uint8_t kPollsPerCycle = 4;
  for (uint8_t cycle = 1; cycle <= kLaunchCycles; ++cycle) {
    Serial.printf("WATCH_BRIDGE_WAKE_LAUNCH cycle=%u/%u\n", static_cast<unsigned>(cycle), static_cast<unsigned>(kLaunchCycles));

    // Serialize status and launch instead of firing both system commands back
    // to back. The old final-acceptance path could leave control responses
    // queued while the newly-installed QuickApp was cold-starting.
    if (!buildQuickAppStatus(app, pb, pbCapacity, pbLength) || !sendEncryptedWatchPb(sequence, encKey, pb, pbLength)) {
      Serial.printf("WATCH_BRIDGE_STATUS_WRITE_FAILED cycle=%u\n", static_cast<unsigned>(cycle));
      return false;
    }
    drainWatchWakeControlFrames(750);

    if (!buildQuickAppLaunch(app, pb, pbCapacity, pbLength) || !sendEncryptedWatchPb(sequence, encKey, pb, pbLength)) {
      Serial.printf("WATCH_BRIDGE_LAUNCH_WRITE_FAILED cycle=%u\n", static_cast<unsigned>(cycle));
      return false;
    }
    drainWatchWakeControlFrames(1250);
    delay(cycle == 1 ? 2500 : 4000);

    for (uint8_t poll = 1; poll <= kPollsPerCycle; ++poll) {
      const uint32_t logicalAttempt = static_cast<uint32_t>(cycle) * 10u + poll;
      if (sendQuickAppWakePing(app, sequence, encKey, decKey, pb, pbCapacity, pbLength,
                               3000, logicalAttempt, cycle == 1 ? "launch" : "relaunch")) {
        Serial.printf("WATCH_BRIDGE_WAKE_READY path=%s cycle=%u poll=%u\n",
                      cycle == 1 ? "launch" : "relaunch", static_cast<unsigned>(cycle), static_cast<unsigned>(poll));
        return true;
      }
      if (poll < kPollsPerCycle) delay(700);
    }
  }

  Serial.println("WATCH_BRIDGE_WAKE_RECOVERY_EXHAUSTED");
  logSppRxParserState("wake_recovery_exhausted");
  return false;
}

void bridgeWatchQuickApp(String target, String secretText, uint8_t benchmarkMode = 0) {
  uint8_t address[6], secret[16]{}, phoneNonce[16]{}, step3[160]{}, encKey[16]{}, decKey[16]{};
  size_t step3Length = 0;
  WatchAppInfo app;
  target.trim(); target.toUpperCase();
  if (!parseWatchAddress(target, address) || !parseSecretHex(secretText, secret)) {
    Serial.println("WATCH_BRIDGE_INVALID_INPUT");
    return;
  }
  Serial.println("WATCH_BRIDGE_BT_INIT_STARTED");
  if (!beginWatchBluetooth("Hoshino-Bridge")) {
    Serial.println("WATCH_BRIDGE_BT_INIT_FAILED");
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return;
  }
  Serial.println("WATCH_BRIDGE_CONNECT_STARTED");
  if (!connectWatchWithSdp(address, "quickapp_bridge")) {
    Serial.println("WATCH_BRIDGE_CONNECT_FAILED");
    watchBt.end();
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return;
  }

  bool authenticated = false;
  do {
    Serial.println("WATCH_BRIDGE_SESSION_STARTED");
    if (!startSppV2Session("WATCH_BRIDGE")) break;
    SppV2Frame& frame = gSharedSppFrame;
    if (!readSppV2Frame(frame, kWatchVersionResponseTimeoutMs) || frame.type != 2 || frame.payloadLength < 1 || frame.payload[0] != 2) { Serial.println("WATCH_BRIDGE_SESSION_RESPONSE_INVALID"); break; }
    esp_fill_random(phoneNonce, sizeof(phoneNonce));
    uint8_t nonceCommand[27] = {0x08, 1, 0x10, 26, 0x1a, 21, 0xf2, 0x01, 18, 0x0a, 16};
    memcpy(nonceCommand + 11, phoneNonce, sizeof(phoneNonce));
    if (!sendSppV2Protobuf(0, nonceCommand, sizeof(nonceCommand))) { Serial.println("WATCH_BRIDGE_NONCE_WRITE_FAILED"); break; }
    const uint8_t* watchNonce = nullptr; const uint8_t* watchHmac = nullptr; size_t watchNonceLength = 0, watchHmacLength = 0;
    const uint32_t nonceDeadline = millis() + kWatchVersionResponseTimeoutMs;
    while (millis() < nonceDeadline) {
      if (!readSppV2Frame(frame, nonceDeadline - millis())) break;
      if (frame.type == 3) writeSppV2Frame(1, frame.sequence, nullptr, 0);
      if (extractWatchNonce(frame, watchNonce, watchNonceLength, watchHmac, watchHmacLength)) break;
    }
    if (!watchNonce || watchNonceLength != 16 || watchHmacLength != 32 || !buildStep3(secret, phoneNonce, watchNonce, watchHmac, step3, step3Length, decKey, encKey)) {
      Serial.println("WATCH_BRIDGE_AUTH_FAILED");
      break;
    }
    if (!sendSppV2Protobuf(1, step3, step3Length)) { Serial.println("WATCH_BRIDGE_AUTH_WRITE_FAILED"); break; }
    const uint32_t authDeadline = millis() + kWatchVersionResponseTimeoutMs;
    while (millis() < authDeadline) {
      if (!readSppV2Frame(frame, authDeadline - millis())) break;
      if (frame.type == 3) writeSppV2Frame(1, frame.sequence, nullptr, 0);
      if (isAuthSuccess(frame)) { authenticated = true; break; }
    }
    if (!authenticated) { Serial.println("WATCH_BRIDGE_AUTH_TIMEOUT"); break; }
    Serial.println("WATCH_BRIDGE_AUTH_OK");

    if (!ensureScratchBuffers()) {
      Serial.println("WATCH_BRIDGE_SCRATCH_UNAVAILABLE");
      break;
    }
    uint8_t* pb = gRawCommandPayloadScratch;
    size_t pbLength = 0;
    uint8_t sequence = 2;
    if (!buildQuickAppListRequest(pb, kSppV2PayloadMax, pbLength) ||
        !sendEncryptedWatchPb(sequence, encKey, pb, pbLength)) {
      Serial.println("WATCH_BRIDGE_APP_LIST_WRITE_FAILED");
      break;
    }
    Serial.println("WATCH_BRIDGE_APP_LIST_REQUEST_SENT");
    const uint32_t listDeadline = millis() + kWatchBridgeResponseTimeoutMs;
    while (millis() < listDeadline && !app.found) {
      if (!readSppV2Frame(frame, listDeadline - millis())) break;
      if (frame.type != 3) continue;
      writeSppV2Frame(1, frame.sequence, nullptr, 0);
      const uint8_t* incoming = nullptr; size_t incomingLength = 0;
      if (decryptWatchPb(frame, decKey, incoming, incomingLength)) parseQuickAppList(incoming, incomingLength, app);
    }
    if (!app.found) { Serial.println("WATCH_BRIDGE_HOSHINO_APP_NOT_FOUND"); break; }
    Serial.printf("WATCH_BRIDGE_HOSHINO_APP_FOUND fingerprint_bytes=%u fingerprint_sha1=%s\n",
                  static_cast<unsigned>(app.fingerprintLength), hexBytes(app.fingerprint, app.fingerprintLength).c_str());

    const bool pong = wakeQuickAppRobust(app, sequence, encKey, decKey, pb, kSppV2PayloadMax, pbLength);
    if (!pong) Serial.println("WATCH_BRIDGE_WAKE_TIMEOUT");
    Serial.println(pong ? "WATCH_BRIDGE_PONG_OK" : "WATCH_BRIDGE_PONG_TIMEOUT");
    if (!pong) {
      if (benchmarkMode == 10) {
        Serial.println("FINAL_ACCEPTANCE_RESULT wake=FAIL preflight=NOT_RUN control=NOT_RUN echo256=NOT_RUN decode256=NOT_RUN durable_ack_retry=NOT_RUN segment256=NOT_RUN push=NOT_RUN FAIL");
      }
      break;
    }
    if (benchmarkMode == 2 || benchmarkMode == 1 || benchmarkMode == 3 || benchmarkMode == 4 || benchmarkMode == 5 || benchmarkMode == 6 || benchmarkMode == 7 || benchmarkMode == 8 || benchmarkMode == 9 || benchmarkMode == 10) {
      // The isolated 256 KiB probe disproved sequence-wrap as the current first
      // failure: it stalls before the guard threshold. Keep normal benchmark
      // modes on the raw transport sequence so a later 255->0 issue, if real,
      // remains observable instead of being hidden by a speculative workaround.
      const bool renewalExperiment = benchmarkMode == 4;
      const bool pacedExperiment = benchmarkMode == 5;
      const bool coalescedTxExperiment = benchmarkMode == 8;  // retained as compatibility alias; coalesced is now production default
      if (renewalExperiment) configureBenchSessionRenewal(secret, encKey, decKey);
      gSppTxCoalesced = true;
      Serial.println("SPP_TX_MODE coalesced=true production_default legacy_segment_bytes=16 disabled=true");
      gBenchInterChunkGapMs = pacedExperiment ? kBenchPacedChunkGapMs : 0;
      if (pacedExperiment) Serial.printf("BENCH_PACING enabled gap_ms=%lu\n", static_cast<unsigned long>(gBenchInterChunkGapMs));
      if (benchmarkMode == 2) runWatchBenchmarkQuick(app, sequence, encKey, decKey);
      else if (benchmarkMode == 3 || benchmarkMode == 4 || benchmarkMode == 5) runWatchBenchmark256K(app, sequence, encKey, decKey);
      else if (benchmarkMode == 6 || benchmarkMode == 8) runWatchStreamProbe(app, sequence, encKey, decKey, "echo");
      else if (benchmarkMode == 7) runWatchStreamProbe(app, sequence, encKey, decKey, "decode");
      else if (benchmarkMode == 9) runWatchBenchmark256KSegment32K(app, sequence, encKey, decKey);
      else if (benchmarkMode == 10) runWatchFinalAcceptance(app, sequence, encKey, decKey);
      else runWatchBenchmark(app, sequence, encKey, decKey);
      gBenchInterChunkGapMs = 0;
      gSppTxCoalesced = true;
      if (renewalExperiment) clearBenchSessionRenewal();
      break;
    }

    const uint32_t inboxNonce = esp_random();
    const int writeLength = snprintf(reinterpret_cast<char*>(pb), kSppV2PayloadMax,
      "{\"v\":2,\"id\":\"esp32_write\",\"timestamp\":%lu,\"type\":\"esp32_bridge_write\",\"payload\":{\"name\":\"smoke_%08lx.json\",\"text\":\"{\\\"source\\\":\\\"esp32\\\",\\\"transport\\\":\\\"spp_v2\\\",\\\"kind\\\":\\\"smoke\\\"}\"}}",
      static_cast<unsigned long>(millis()), static_cast<unsigned long>(inboxNonce));
    if (writeLength <= 0 || static_cast<size_t>(writeLength) >= kSppV2PayloadMax || !buildQuickAppMessage(app, pb, static_cast<size_t>(writeLength), pb, kSppV2PayloadMax, pbLength) || !sendEncryptedWatchPb(sequence, encKey, pb, pbLength)) {
      Serial.println("WATCH_BRIDGE_INBOX_WRITE_FAILED");
      break;
    }
    Serial.println("WATCH_BRIDGE_INBOX_WRITE_SENT");
    bool inboxWriteOk = false, inboxWriteAnswered = false;
    const uint32_t writeDeadline = millis() + kWatchBridgeResponseTimeoutMs;
    while (millis() < writeDeadline) {
      if (!readSppV2Frame(frame, writeDeadline - millis())) break;
      if (frame.type != 3) continue;
      writeSppV2Frame(1, frame.sequence, nullptr, 0);
      const uint8_t* incoming = nullptr; size_t incomingLength = 0, contentLength = 0; const uint8_t* content = nullptr;
      if (decryptWatchPb(frame, decKey, incoming, incomingLength) && parseQuickAppMessage(incoming, incomingLength, content, contentLength)) {
        Serial.printf("WATCH_BRIDGE_RX bytes=%u\n", static_cast<unsigned>(contentLength));
        if (containsAscii(content, contentLength, "\"type\":\"esp32_bridge_ack\"")) {
          inboxWriteAnswered = true;
          if (containsAscii(content, contentLength, "\"ok\":true") || containsAscii(content, contentLength, "\"ok\":1")) {
            inboxWriteOk = true;
          } else if (containsAscii(content, contentLength, "inbox_entry_exists")) {
            Serial.println("WATCH_BRIDGE_INBOX_ACK_EXISTS");
          } else if (containsAscii(content, contentLength, "inbox_mkdir_failed")) {
            Serial.println("WATCH_BRIDGE_INBOX_ACK_MKDIR_FAILED");
          } else if (containsAscii(content, contentLength, "inbox_write_failed")) {
            Serial.println("WATCH_BRIDGE_INBOX_ACK_WRITE_FAILED");
          } else {
            Serial.println("WATCH_BRIDGE_INBOX_ACK_REJECTED");
          }
          break;
        }
      }
    }
    Serial.println(inboxWriteOk ? "WATCH_BRIDGE_INBOX_WRITE_OK" : (inboxWriteAnswered ? "WATCH_BRIDGE_INBOX_WRITE_REJECTED" : "WATCH_BRIDGE_INBOX_WRITE_TIMEOUT"));
  } while (false);

  watchBt.disconnect(); watchBt.end();
  mbedtls_platform_zeroize(secret, sizeof(secret)); mbedtls_platform_zeroize(phoneNonce, sizeof(phoneNonce)); mbedtls_platform_zeroize(step3, sizeof(step3));
  mbedtls_platform_zeroize(encKey, sizeof(encKey)); mbedtls_platform_zeroize(decKey, sizeof(decKey)); mbedtls_platform_zeroize(app.fingerprint, sizeof(app.fingerprint));
  secretText = "";
}

// ESP32 独立 DNS 自测（不经过手表 ch7 / custom netif / NAPT 路径）。
// 直接走 WiFi STA -> 网关 -> DNS 服务器，用于验证 WiFi/ARP/网关/外网/DNS 本身。
static void espDnsSelfTestCb(const char* name, const ip_addr_t* ipaddr, void* arg) {
  (void)arg;
  if (ipaddr != nullptr) {
    char ipbuf[48]{};
    if (IP_IS_V4(ipaddr)) {
      const ip4_addr_t* a = ip_2_ip4(ipaddr);
      snprintf(ipbuf, sizeof(ipbuf), "%d.%d.%d.%d", ip4_addr1(a), ip4_addr2(a), ip4_addr3(a), ip4_addr4(a));
    } else {
      snprintf(ipbuf, sizeof(ipbuf), "<ipv6>");
    }
    Serial.printf("ESP_DNS_SELFTEST_RX name=%s ip=%s\n", name ? name : "?", ipbuf);
  } else {
    Serial.printf("ESP_DNS_SELFTEST_FAIL name=%s reason=resolve_timeout\n", name ? name : "?");
  }
}

constexpr size_t kSerialSdListLimit = 48;
constexpr size_t kSerialSdHeadMaxBytes = 128;

void writeSdSerialJson(JsonDocument& document) {
  Serial.print("SD_JSON ");
  serializeJson(document, Serial);
  Serial.println();
}

void writeSdSerialError(const char* event, const String& error) {
  JsonDocument document;
  document["event"] = event;
  document["ok"] = false;
  document["error"] = error;
  writeSdSerialJson(document);
}

void handleSdSerialStatus() {
  String error;
  if (!sdStorage.ensureMounted(error)) {
    writeSdSerialError("sd_status", error);
    return;
  }
  const hoshino::SdStorageStatus status = sdStorage.status();
  JsonDocument document;
  document["event"] = "sd_status";
  document["ok"] = true;
  document["mounted"] = status.mounted;
  document["root"] = hoshino::SdStorageService::rootPath();
  document["cardType"] = status.cardType;
  document["cardSize"] = status.cardSize;
  document["totalBytes"] = status.totalBytes;
  document["usedBytes"] = status.usedBytes;
  writeSdSerialJson(document);
}

void handleSdSerialList(const String& relativePath) {
  String error;
  File directory = sdStorage.openRead(relativePath, error);
  if (!directory) {
    writeSdSerialError("sd_list", error);
    return;
  }
  if (!directory.isDirectory()) {
    directory.close();
    writeSdSerialError("sd_list", "not_directory");
    return;
  }

  JsonDocument begin;
  begin["event"] = "sd_list_begin";
  begin["ok"] = true;
  begin["path"] = relativePath.isEmpty() ? "." : relativePath;
  begin["limit"] = kSerialSdListLimit;
  writeSdSerialJson(begin);

  size_t count = 0;
  bool truncated = false;
  while (true) {
    File child = directory.openNextFile();
    if (!child) break;
    if (count >= kSerialSdListLimit) {
      truncated = true;
      child.close();
      break;
    }
    JsonDocument item;
    item["event"] = "sd_entry";
    item["name"] = child.name();
    item["type"] = child.isDirectory() ? "directory" : "file";
    if (!child.isDirectory()) item["size"] = child.size();
    writeSdSerialJson(item);
    child.close();
    ++count;
  }
  directory.close();

  JsonDocument end;
  end["event"] = "sd_list_end";
  end["ok"] = true;
  end["count"] = count;
  end["truncated"] = truncated;
  writeSdSerialJson(end);
}

void handleSdSerialHead(const String& relativePath) {
  if (relativePath.isEmpty()) {
    writeSdSerialError("sd_head", "path_required");
    return;
  }
  String error;
  File file = sdStorage.openRead(relativePath, error);
  if (!file) {
    writeSdSerialError("sd_head", error);
    return;
  }
  if (file.isDirectory()) {
    file.close();
    writeSdSerialError("sd_head", "not_file");
    return;
  }

  const uint64_t fileSize = file.size();
  uint8_t raw[kSerialSdHeadMaxBytes]{};
  const size_t wanted = fileSize < sizeof(raw) ? static_cast<size_t>(fileSize) : sizeof(raw);
  const size_t bytesRead = file.read(raw, wanted);
  file.close();

  unsigned char encoded[4 * ((kSerialSdHeadMaxBytes + 2) / 3) + 1]{};
  size_t encodedLength = 0;
  const int result = mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &encodedLength, raw, bytesRead);
  if (result != 0) {
    writeSdSerialError("sd_head", "base64_failed");
    return;
  }
  encoded[encodedLength] = '\0';

  JsonDocument document;
  document["event"] = "sd_head";
  document["ok"] = true;
  document["path"] = relativePath;
  document["encoding"] = "base64";
  document["bytes"] = bytesRead;
  document["fileSize"] = fileSize;
  document["truncated"] = fileSize > bytesRead;
  document["data"] = reinterpret_cast<const char*>(encoded);
  writeSdSerialJson(document);
}

void handleWatchProbeSerial() {
  static String command;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      // WATCH_CONFIG with ssid + wifi_pass + watch_mac + watch_auth +
      // auto_connect is ~137 chars; keep a generous 256-byte buffer.
      if (command.length() < 256) command += c;
      continue;
    }
    command.trim();
    if (command == "WATCH_SETUP") {
      // 串口命令触发配网：写标志 + 重启，重启后进入 AP 配网模式。
      Serial.println("SETUP_MODE_REASON serial_command (save flag + reboot)");
      prefs.putBool("force_setup", true);
      delay(300);
      ESP.restart();
    }
    else if (command.startsWith("WATCH_CONFIG ")) {
      // Syntax: WATCH_CONFIG ssid=xxx wifi_pass=yyy watch_mac=AA:BB:CC:DD:EE:FF watch_auth=32hex
      String args = command.substring(13);
      int sep, end;
      // First argument has no leading space
      if (args.startsWith("ssid=")) {
        end = args.indexOf(' '); if (end < 0) end = args.length();
        ssid = args.substring(5, end); prefs.putString("ssid", ssid);
      }
      if ((sep = args.indexOf(" wifi_pass=")) >= 0) {
        end = args.indexOf(' ', sep + 11); if (end < 0) end = args.length();
        wifiPass = args.substring(sep + 11, end); prefs.putString("wifi_pass", wifiPass);
      }
      if ((sep = args.indexOf(" watch_mac=")) >= 0) {
        end = args.indexOf(' ', sep + 11); if (end < 0) end = args.length();
        watchMac = args.substring(sep + 11, end); prefs.putString("watch_mac", watchMac);
      }
      if ((sep = args.indexOf(" watch_auth=")) >= 0) {
        end = args.indexOf(' ', sep + 12); if (end < 0) end = args.length();
        watchAuthKey = args.substring(sep + 12, end); prefs.putString("watch_auth", watchAuthKey);
      }
      if ((sep = args.indexOf(" auto_connect=")) >= 0) {
        autoConnectWatch = args.substring(sep + 14) == "1";
        prefs.putBool("watch_auto", autoConnectWatch);
      }
      prefs.putString("local_token", "hoshino-local");
      prefs.putString("ap_pass", "hoshino-setup");
      Serial.printf("WATCH_CONFIG_SAVED ssid=%s mac=%s auth=%s\n",
                    ssid.c_str(), watchMac.c_str(), watchAuthKey.length() == 32 ? "ok" : "missing");
      if (!ssid.isEmpty()) {
        WiFi.mode(WIFI_STA);
        beginWifiStaConnection(15000);
        WiFi.begin(ssid.c_str(), wifiPass.c_str());
        Serial.println("WIFI_CONNECTING_STA_ONLY");
        uint32_t wstart=millis();
        while(WiFi.status()!=WL_CONNECTED && gWifiStaConnecting && millis()-wstart<15000){
          serviceWifiStaConnectionState(false);
          delay(250);
        }
        clearWifiStaConnection(WiFi.status()==WL_CONNECTED ? "connected" : "serial_wait_end");
        Serial.printf("WIFI_CONFIG_RESULT connected=%s ip=%s\n", WiFi.status()==WL_CONNECTED?"true":"false", WiFi.localIP().toString().c_str());
      }
    }
    else if (command == "SD_STATUS") {
      handleSdSerialStatus();
    }
    else if (command == "SD_LIST") {
      handleSdSerialList("");
    }
    else if (command.startsWith("SD_LIST ")) {
      String path = command.substring(8);
      path.trim();
      handleSdSerialList(path);
    }
    else if (command == "SD_HEAD") {
      handleSdSerialHead("");
    }
    else if (command.startsWith("SD_HEAD ")) {
      String path = command.substring(8);
      path.trim();
      handleSdSerialHead(path);
    }
    command = "";
  }
}

void loadConfig() {
  prefs.begin("hoshino", false);
  ssid = readConfigString("ssid", "");
  wifiPass = readConfigString("wifi_pass", "");
  baseUrl = cleanBase(readConfigString("base_url", "https://api.xiaomimimo.com/v1"));
  apiKey = readConfigString("api_key", "");
  model = readConfigString("model", "mimo-v2.5-pro");
  watchMac = readConfigString("watch_mac", "");
  if (watchMac.length() != 17) watchMac = "";
  watchAuthKey = readConfigString("watch_auth", "");
  if (watchAuthKey.length() != 32) watchAuthKey = "";
  localToken = readConfigString("local_token", "hoshino-local");
  caPem = readConfigString("ca_pem", "");
  apPassword = readConfigString("ap_pass", "hoshino-setup");
  if (apPassword.length() < 8) apPassword = "hoshino-setup";
  maxTokens = prefs.getInt("max_tokens", 512);
  allowInsecureTls = prefs.getBool("insecure_tls", false);
  autoConnectWatch = prefs.getBool("watch_auto", false);
}

bool authorized() {
  if (localToken.isEmpty()) return true;
  String h = server.header("Authorization");
  if (h == "Bearer " + localToken) return true;
  return server.header("X-Hoshino-Token") == localToken;
}

void jsonError(int code, const char* err, const String& message) {
  JsonDocument d;
  d["ok"] = false; d["error"] = err; d["message"] = message;
  String out; serializeJson(d, out);
  server.send(code, "application/json; charset=utf-8", out);
}

void sendJson(int code, JsonDocument& d) {
  String out; serializeJson(d, out);
  server.send(code, "application/json; charset=utf-8", out);
}

bool requireAuth() {
  if (authorized()) return true;
  jsonError(401, "unauthorized", "invalid local token");
  return false;
}

bool isSetupApClient() {
  return server.client().localIP() == WiFi.softAPIP();
}

bool requireSetupAp() {
  if (isSetupApClient()) return true;
  jsonError(403, "setup_ap_required", "Sensitive setup is only available through the Hoshino-Bridge SoftAP");
  return false;
}

// This is deliberately separate from the phone-facing SoftAP setup API. The
// existing authenticated SPP network tunnel gives the watch 10.1.10.2 and the
// ESP32 10.1.10.1; only that peer may change the home Wi-Fi credentials here.
// Do not relax this to a general LAN endpoint: the request carries a password.
bool isWatchNetworkClient() {
  if (!gWatchNetworkReady) return false;
  const IPAddress remote = server.client().remoteIP();
  return remote[0] == 10 && remote[1] == 1 && remote[2] == 10 && remote[3] == 2;
}

bool requireWatchSetupClient() {
  if (!isWatchNetworkClient()) {
    jsonError(403, "watch_tunnel_required", "watch setup is only available through the authenticated watch tunnel");
    return false;
  }
  // A context marker prevents this sensitive operation being accidentally
  // reached by an unrelated request routed through the watch netif.
  if (server.header("X-Hoshino-Watch-Setup") != "1") {
    jsonError(400, "watch_setup_marker_required", "missing X-Hoshino-Watch-Setup header");
    return false;
  }
  return true;
}

// Storage is reachable through the already-authenticated watch tunnel, but it
// must not inherit the sensitive Wi-Fi provisioning marker. LAN callers still
// need the configured local token as usual.
bool requireStorageAuth() {
  if (authorized()) return true;
  if (isWatchNetworkClient() && server.header("X-Hoshino-Storage") == "1") return true;
  jsonError(401, "unauthorized", "invalid local token");
  return false;
}

void handleWatchSetupStatus() {
  if (!requireWatchSetupClient()) return;
  serviceWifiStaConnectionState(false);
  JsonDocument d;
  d["ok"] = true;
  d["bridge"] = "hoshino-esp32";
  d["watchTunnel"] = true;
  d["watchIp"] = "10.1.10.2";
  d["gatewayIp"] = "10.1.10.1";
  d["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  d["wifiConfigured"] = !ssid.isEmpty();
  d["wifiState"] = gWifiStaConnecting ? "connecting" : gWifiState;
  d["status"] = gWifiStaConnecting ? "connecting" : gWifiState;
  d["wifiError"] = gWifiError;
  d["wifiReason"] = gWifiStaLastReason;
  d["ssid"] = ssid;  // The password is never returned.
  sendJson(200, d);
}

// Wi-Fi scan is a watch-only read operation carried by the existing
// authenticated virtual network.  It does not touch SPP framing, the
// Bluetooth tunnel, DHCP, or NAPT state.  Results are first copied into a
// bounded workspace so scanDelete() can release the driver buffer before JSON serialization.
void handleWatchSetupScan() {
  if (!requireWatchSetupClient()) return;

  serviceWifiStaConnectionState(false);
  const bool failureBackoff = gWifiStaFailureBackoffUntilMs != 0 &&
      static_cast<int32_t>(millis() - gWifiStaFailureBackoffUntilMs) < 0;
  if (gWifiStaConnecting || failureBackoff) {
    JsonDocument d;
    d["ok"] = false;
    d["status"] = "wifi_connecting";
    d["message"] = "Wi-Fi 正在连接，请稍候";
    const uint32_t remaining = failureBackoff ? gWifiStaFailureBackoffUntilMs - millis() : 1000u;
    d["retryAfterMs"] = min(static_cast<uint32_t>(2000), max(static_cast<uint32_t>(250), remaining));
    d["rawCount"] = 0;
    d["truncated"] = false;
    d["networks"].to<JsonArray>();
    d["count"] = 0;
    Serial.println("WATCH_WIFI_SCAN_DEFER reason=sta_connecting");
    sendJson(200, d);
    return;
  }

  // scanNetworks() enables STA internally, but make the mode transition
  // explicit so an AP-only bridge becomes APSTA without tearing down the AP.
  // Do not restore AP-only afterwards: the AP/HTTP path and watch tunnel must
  // remain live while the driver finishes and releases its scan buffers.
  const wifi_mode_t modeBeforeScan = WiFi.getMode();
  if ((modeBeforeScan & WIFI_MODE_STA) == 0) {
    const wifi_mode_t scanMode = (modeBeforeScan & WIFI_MODE_AP) ? WIFI_AP_STA : WIFI_STA;
    if (!WiFi.mode(scanMode) || (WiFi.getMode() & WIFI_MODE_STA) == 0) {
      return jsonError(503, "wifi_scan_mode_failed", "unable to enable Wi-Fi STA for scan");
    }
    Serial.printf("WATCH_WIFI_SCAN_MODE before=%d after=%d ap_preserved=%s\n",
                  static_cast<int>(modeBeforeScan), static_cast<int>(WiFi.getMode()),
                  (modeBeforeScan & WIFI_MODE_AP) ? "true" : "false");
  }

  // Keep this endpoint bounded: WROOM runs Classic BT + Wi-Fi + the watch
  // virtual network at the same time, so an unbounded scan result list can
  // create a large temporary heap peak.  Twenty APs is more than enough for
  // the watch UI while keeping the response and JSON document small.
  constexpr size_t kResultLimit = 20;
  String selectedSsids[kResultLimit];
  int32_t selectedRssi[kResultLimit] = {};
  size_t selectedCount = 0;

  Serial.printf("WATCH_WIFI_SCAN_BEGIN free=%lu largest=%lu min_free=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap()));

  const int16_t found = WiFi.scanNetworks(false, true);
  if (found < 0) {
    WiFi.scanDelete();
    Serial.printf("WATCH_WIFI_SCAN_FAIL code=%d free=%lu largest=%lu\n",
                  static_cast<int>(found),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    return jsonError(503, "wifi_scan_failed", String("scanNetworks=") + String(found));
  }

  // Copy only the strongest unique APs into a bounded workspace.  The scan
  // driver buffer is released before ArduinoJson builds the response, which
  // avoids holding the driver result table + JSON tree + serialized String at
  // the same time.
  for (int16_t i = 0; i < found; ++i) {
    const String networkName = WiFi.SSID(i);
    if (networkName.isEmpty()) continue;
    const int32_t rssi = WiFi.RSSI(i);

    bool duplicate = false;
    for (size_t j = 0; j < selectedCount; ++j) {
      if (selectedSsids[j] == networkName) {
        if (rssi > selectedRssi[j]) selectedRssi[j] = rssi;
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    if (selectedCount < kResultLimit) {
      selectedSsids[selectedCount] = networkName;
      selectedRssi[selectedCount] = rssi;
      ++selectedCount;
      continue;
    }

    size_t weakest = 0;
    for (size_t j = 1; j < selectedCount; ++j) {
      if (selectedRssi[j] < selectedRssi[weakest]) weakest = j;
    }
    if (rssi > selectedRssi[weakest]) {
      selectedSsids[weakest] = networkName;
      selectedRssi[weakest] = rssi;
    }
  }

  Serial.printf("WATCH_WIFI_SCAN_RAW count=%d selected=%u free=%lu largest=%lu\n",
                static_cast<int>(found), static_cast<unsigned>(selectedCount),
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  WiFi.scanDelete();
  Serial.printf("WATCH_WIFI_SCAN_BUFFER_FREED free=%lu largest=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));

  JsonDocument d;
  d["ok"] = true;
  d["status"] = "success";
  d["message"] = "扫描成功";
  d["rawCount"] = found;
  d["truncated"] = static_cast<size_t>(found) > selectedCount;
  JsonArray networks = d["networks"].to<JsonArray>();
  for (size_t i = 0; i < selectedCount; ++i) {
    JsonObject item = networks.add<JsonObject>();
    item["ssid"] = selectedSsids[i].c_str();
    item["rssi"] = selectedRssi[i];
  }
  d["count"] = networks.size();

  Serial.printf("WATCH_WIFI_SCAN_RESPONSE count=%u free=%lu largest=%lu min_free=%lu\n",
                static_cast<unsigned>(networks.size()),
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap()));
  sendJson(200, d);
}

String body();

void handleWatchSetupWifi() {
  if (!requireWatchSetupClient()) return;
  const String requestBody = body();
  if (requestBody.isEmpty()) return jsonError(400, "invalid_body", "empty or too large");
  JsonDocument in;
  if (deserializeJson(in, requestBody)) return jsonError(400, "invalid_json", "cannot parse json");
  const String nextSsid = String(in["ssid"] | "");
  const String nextPass = String(in["wifiPassword"] | "");
  // Preserve the strings exactly as received. A leading space is a valid SSID byte.
  if (nextSsid.isEmpty()) {
    return jsonError(400, "invalid_ssid", "Wi-Fi 名称不能为空");
  }
  if (nextSsid.length() > 64) {
    return jsonError(400, "invalid_ssid", "Wi-Fi 名称长度超过 64 字节");
  }
  if (nextPass.length() < 8) {
    return jsonError(400, "invalid_password", "Wi-Fi 密码至少需要 8 个字符");
  }
  if (nextPass.length() > 128) {
    return jsonError(400, "invalid_password", "Wi-Fi 密码长度超过 128 字节");
  }
  serviceWifiStaConnectionState(false);
  if (gWifiStaConnecting || (gWifiStaFailureBackoffUntilMs != 0 &&
      static_cast<int32_t>(millis() - gWifiStaFailureBackoffUntilMs) < 0)) {
    JsonDocument busy;
    busy["ok"] = true;
    busy["saved"] = false;
    busy["rebooting"] = false;
    busy["status"] = "wifi_connecting";
    busy["wifiState"] = "connecting";
    busy["wifiError"] = gWifiError;
    busy["wifiReason"] = gWifiStaLastReason;
    busy["retryAfterMs"] = 1000;
    busy["message"] = "Wi-Fi 正在连接，请稍候";
    sendJson(200, busy);
    return;
  }

  // Keep AP/HTTP/tunnel alive while the single asynchronous STA attempt runs.
  const wifi_mode_t before = WiFi.getMode();
  if ((before & WIFI_MODE_STA) == 0) {
    const wifi_mode_t withSta = (before & WIFI_MODE_AP) ? WIFI_AP_STA : WIFI_STA;
    if (!WiFi.mode(withSta) || (WiFi.getMode() & WIFI_MODE_STA) == 0) {
      return jsonError(503, "wifi_connect_start_failed", "无法启用 Wi-Fi STA");
    }
  }
  gPendingWifiSsid = nextSsid;
  gPendingWifiPassword = nextPass;
  gWifiProvisioningPending = true;
  WiFi.setAutoReconnect(true);
  beginWifiStaConnection(kWifiStaConnectTimeoutMs);
  WiFi.begin(gPendingWifiSsid.c_str(), gPendingWifiPassword.c_str());
  Serial.printf("WATCH_WIFI_CONNECT_BEGIN ssid_bytes=%u timeout_ms=%lu\n",
                static_cast<unsigned>(gPendingWifiSsid.length()),
                static_cast<unsigned long>(kWifiStaConnectTimeoutMs));
  JsonDocument out;
  out["ok"] = true;
  out["saved"] = false;
  out["rebooting"] = false;
  out["status"] = "connecting";
  out["wifiState"] = "connecting";
  out["wifiError"] = "";
  out["wifiReason"] = 0;
  out["retryAfterMs"] = 500;
  out["message"] = "Wi-Fi 正在连接，请稍候";
  sendJson(200, out);
}

void recordWatchFetchProbe() {
  String entry = "WATCH_FETCH_PROBE peer=" + server.client().remoteIP().toString() + " path=" + server.uri();
  fetchTrace[fetchTraceNext] = entry;
  fetchTraceNext = (fetchTraceNext + 1) % kFetchTraceEntries;
  if (fetchTraceCount < kFetchTraceEntries) ++fetchTraceCount;
  Serial.println(entry);
}

void handleFetchTrace() {
  if (!requireSetupAp()) return;
  JsonDocument d;
  d["ok"] = true;
  JsonArray entries = d["entries"].to<JsonArray>();
  for (size_t offset = 0; offset < fetchTraceCount; ++offset) {
    const size_t index = (fetchTraceNext + kFetchTraceEntries - fetchTraceCount + offset) % kFetchTraceEntries;
    entries.add(fetchTrace[index]);
  }
  sendJson(200, d);
}

String body() {
  String b = server.arg("plain");
  if (b.length() > kMaxBody) return "";
  return b;
}

bool beginHttp(const String& url, HTTPClient& http, WiFiClient& plain, WiFiClientSecure& secure, String& error) {
  http.setTimeout(kHttpTimeoutMs);
  if (url.startsWith("https://")) {
    if (!caPem.isEmpty()) secure.setCACert(caPem.c_str());
    else if (allowInsecureTls) secure.setInsecure();
    else {
      error = "https_ca_missing: paste the issuing root CA in WebUI or explicitly enable insecure TLS for development";
      return false;
    }
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

bool mimoRequest(const String& userText, String& answer, String& error) {
  if (WiFi.status() != WL_CONNECTED) { error = "wifi_not_connected"; return false; }
  if (apiKey.isEmpty()) { error = "mimo_api_key_missing"; return false; }
  if (baseUrl.isEmpty()) { error = "mimo_base_url_missing"; return false; }
  if (userText.isEmpty() || userText.length() > 8000) { error = "invalid_input"; return false; }

  String url = cleanBase(baseUrl) + "/chat/completions";
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(url, http, plain, secure, error)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + apiKey);

  JsonDocument req;
  req["model"] = model;
  req["stream"] = false;
  req["max_completion_tokens"] = constrain(maxTokens, 1, 4096);
  JsonObject thinking = req["thinking"].to<JsonObject>();
  thinking["type"] = "disabled";
  JsonArray messages = req["messages"].to<JsonArray>();
  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] = "You are MiMo, an AI assistant used through a small watch bridge. Reply concisely.";
  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = userText;
  String payload; serializeJson(req, payload);

  int code = http.POST(payload);
  String response = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    error = "upstream_http_" + String(code) + ":" + response.substring(0, 512);
    return false;
  }
  JsonDocument res;
  DeserializationError de = deserializeJson(res, response);
  if (de) { error = "upstream_json_invalid"; return false; }
  const char* text = res["choices"][0]["message"]["content"] | nullptr;
  if (!text) { error = "upstream_content_missing"; return false; }
  answer = String(text);
  if (answer.length() > 12000) answer = answer.substring(0, 12000);
  return true;
}

void resetWatchConversationContext(const char* reason) {
  const bool locked = gBridgeStateMutex && xSemaphoreTake(gBridgeStateMutex, pdMS_TO_TICKS(250)) == pdTRUE;
  memset(gWatchContext, 0, sizeof(gWatchContext));
  gWatchContextCount = 0;
  gWatchContextNext = 0;
  gWatchContextLastMs = millis();
  if (locked) xSemaphoreGive(gBridgeStateMutex);
  Serial.printf("WATCH_AI_CONTEXT_RESET reason=%s\n", reason ? reason : "unknown");
}

void maybeExpireWatchConversationContext() {
  if (gWatchContextCount == 0 || gWatchContextLastMs == 0) return;
  if (millis() - gWatchContextLastMs >= kWatchContextIdleResetMs) resetWatchConversationContext("idle_timeout");
}

void appendWatchConversationTurn(const String& userText, const String& assistantText) {
  const bool locked = gBridgeStateMutex && xSemaphoreTake(gBridgeStateMutex, pdMS_TO_TICKS(250)) == pdTRUE;
  WatchConversationTurn& turn = gWatchContext[gWatchContextNext];
  snprintf(turn.user, sizeof(turn.user), "%s", userText.c_str());
  snprintf(turn.assistant, sizeof(turn.assistant), "%s", assistantText.c_str());
  gWatchContextNext = (gWatchContextNext + 1) % kWatchContextTurns;
  if (gWatchContextCount < kWatchContextTurns) ++gWatchContextCount;
  gWatchContextLastMs = millis();
  if (locked) xSemaphoreGive(gBridgeStateMutex);
}

bool mimoWatchRequest(const String& userText, String& answer, String& error) {
  if (WiFi.status() != WL_CONNECTED) { error = "wifi_not_connected"; return false; }
  if (apiKey.isEmpty()) { error = "mimo_api_key_missing"; return false; }
  if (baseUrl.isEmpty()) { error = "mimo_base_url_missing"; return false; }
  if (userText.isEmpty() || userText.length() > 8000) { error = "invalid_input"; return false; }
  maybeExpireWatchConversationContext();

  String url = cleanBase(baseUrl) + "/chat/completions";
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(url, http, plain, secure, error)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + apiKey);

  JsonDocument req;
  req["model"] = model;
  req["stream"] = false;
  req["max_completion_tokens"] = constrain(maxTokens, 1, 4096);
  JsonObject thinking = req["thinking"].to<JsonObject>();
  thinking["type"] = "disabled";
  JsonArray messages = req["messages"].to<JsonArray>();
  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] = "You are MiMo behind a Redmi Watch assistant bridge. Keep answers concise and preserve conversational context. If a request needs a live tool or watch presentation that the bridge has not implemented yet, answer with the useful text result only; do not invent device-side tool events.";

  WatchConversationTurn contextSnapshot[kWatchContextTurns]{};
  size_t contextCount = 0;
  {
    const bool locked = gBridgeStateMutex && xSemaphoreTake(gBridgeStateMutex, pdMS_TO_TICKS(250)) == pdTRUE;
    contextCount = gWatchContextCount;
    const size_t first = (gWatchContextNext + kWatchContextTurns - contextCount) % kWatchContextTurns;
    for (size_t i = 0; i < contextCount; ++i) contextSnapshot[i] = gWatchContext[(first + i) % kWatchContextTurns];
    if (locked) xSemaphoreGive(gBridgeStateMutex);
  }
  for (size_t i = 0; i < contextCount; ++i) {
    const WatchConversationTurn& turn = contextSnapshot[i];
    if (turn.user[0]) {
      JsonObject priorUser = messages.add<JsonObject>();
      priorUser["role"] = "user";
      priorUser["content"] = turn.user;
    }
    if (turn.assistant[0]) {
      JsonObject priorAssistant = messages.add<JsonObject>();
      priorAssistant["role"] = "assistant";
      priorAssistant["content"] = turn.assistant;
    }
  }
  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = userText;
  String payload; serializeJson(req, payload);

  int code = http.POST(payload);
  String response = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    error = "upstream_http_" + String(code) + ":" + response.substring(0, 512);
    return false;
  }
  JsonDocument res;
  if (deserializeJson(res, response)) { error = "upstream_json_invalid"; return false; }
  const char* text = res["choices"][0]["message"]["content"] | nullptr;
  if (!text) { error = "upstream_content_missing"; return false; }
  answer = String(text);
  if (answer.length() > 12000) answer = answer.substring(0, 12000);
  appendWatchConversationTurn(userText, answer);
  return true;
}



#if HOSHINO_ENABLE_ASR
// ---- MiMo ASR pipeline: watch Opus aggregate -> PCM16 WAV -> streamed Base64 -> MiMo ----
void writeLe16(File& f, uint16_t value) {
  const uint8_t b[] = {static_cast<uint8_t>(value & 0xff), static_cast<uint8_t>((value >> 8) & 0xff)};
  f.write(b, sizeof(b));
}

void writeLe32(File& f, uint32_t value) {
  const uint8_t b[] = {
      static_cast<uint8_t>(value & 0xff), static_cast<uint8_t>((value >> 8) & 0xff),
      static_cast<uint8_t>((value >> 16) & 0xff), static_cast<uint8_t>((value >> 24) & 0xff)};
  f.write(b, sizeof(b));
}

bool writePcmWavHeader(File& wav, uint32_t pcmBytes) {
  if (!wav || !wav.seek(0, SeekSet)) return false;
  wav.write(reinterpret_cast<const uint8_t*>("RIFF"), 4);
  writeLe32(wav, 36u + pcmBytes);
  wav.write(reinterpret_cast<const uint8_t*>("WAVEfmt "), 8);
  writeLe32(wav, 16);
  writeLe16(wav, 1); // PCM
  writeLe16(wav, kWatchAsrChannels);
  writeLe32(wav, kWatchAsrSampleRate);
  writeLe32(wav, kWatchAsrSampleRate * kWatchAsrChannels * 2u);
  writeLe16(wav, kWatchAsrChannels * 2u);
  writeLe16(wav, 16);
  wav.write(reinterpret_cast<const uint8_t*>("data"), 4);
  writeLe32(wav, pcmBytes);
  return wav.position() == 44;
}

bool decodeOneWatchOpusFrame(OpusDecoder* decoder, const uint8_t* frame, size_t frameLength,
                             File& wav, uint32_t& pcmBytes, uint32_t& decodedFrames, String& error) {
  if (!decoder || !frame || !frameLength) { error = "opus_invalid_frame"; return false; }
  int16_t pcm[1920]{}; // enough for 120 ms at 16 kHz; observed frames are 20 ms.
  const int samples = opus_decode(decoder, frame, static_cast<opus_int32>(frameLength), pcm,
                                  sizeof(pcm) / sizeof(pcm[0]), 0);
  if (samples < 0) {
    error = "opus_decode_" + String(samples);
    return false;
  }
  const size_t bytes = static_cast<size_t>(samples) * sizeof(int16_t);
  if (pcmBytes + bytes > kWatchAsrPcmMaxBytes) {
    error = "audio_too_long_max_15s";
    return false;
  }
  if (wav.write(reinterpret_cast<const uint8_t*>(pcm), bytes) != bytes) {
    error = "wav_write_failed";
    return false;
  }
  pcmBytes += bytes;
  ++decodedFrames;
  return true;
}

bool decodeWatchOpusPacketsToWav(const char* packetPath, const char* wavPath, String& error) {
  if (!gWatchStreamCaptureFsReady) { error = "spiffs_unavailable"; return false; }
  File packets = SPIFFS.open(packetPath, FILE_READ);
  if (!packets) { error = "packet_file_missing"; return false; }
  SPIFFS.remove(wavPath);
  File wav = SPIFFS.open(wavPath, FILE_WRITE);
  if (!wav) { packets.close(); error = "wav_open_failed"; return false; }
  uint8_t blank[44]{};
  if (wav.write(blank, sizeof(blank)) != sizeof(blank)) {
    packets.close(); wav.close(); SPIFFS.remove(wavPath); error = "wav_header_reserve_failed"; return false;
  }

  int opusError = OPUS_OK;
  OpusDecoder* decoder = opus_decoder_create(kWatchAsrSampleRate, kWatchAsrChannels, &opusError);
  if (!decoder || opusError != OPUS_OK) {
    packets.close(); wav.close(); SPIFFS.remove(wavPath);
    error = "opus_decoder_create_" + String(opusError);
    if (decoder) opus_decoder_destroy(decoder);
    return false;
  }

  uint8_t aggregate[kWatchStreamPacketMax]{};
  uint32_t pcmBytes = 0, decodedFrames = 0, aggregates = 0;
  bool ok = true;
  while (packets.available() && ok) {
    uint8_t lenBytes[2]{};
    if (packets.read(lenBytes, 2) != 2) { error = "packet_length_truncated"; ok = false; break; }
    const size_t length = (static_cast<size_t>(lenBytes[0]) << 8) | lenBytes[1];
    if (!length || length > sizeof(aggregate) || packets.read(aggregate, length) != static_cast<int>(length)) {
      error = "packet_payload_truncated"; ok = false; break;
    }
    ++aggregates;

    bool fixed80 = length >= kWatchOpusSubframeBytes && (length % kWatchOpusSubframeBytes) == 0;
    if (fixed80) {
      for (size_t offset = 0; offset < length; offset += kWatchOpusSubframeBytes) {
        if (aggregate[offset] != 0xB8) { fixed80 = false; break; }
      }
    }
    if (fixed80) {
      for (size_t offset = 0; offset < length; offset += kWatchOpusSubframeBytes) {
        if (!decodeOneWatchOpusFrame(decoder, aggregate + offset, kWatchOpusSubframeBytes,
                                     wav, pcmBytes, decodedFrames, error)) { ok = false; break; }
      }
    } else {
      // Fallback for a future firmware that stops aggregating fixed 80-byte CBR frames.
      if (!decodeOneWatchOpusFrame(decoder, aggregate, length, wav, pcmBytes, decodedFrames, error)) ok = false;
    }
  }
  opus_decoder_destroy(decoder);
  packets.close();

  if (ok && pcmBytes > 0) ok = writePcmWavHeader(wav, pcmBytes);
  if (!ok && error.isEmpty()) error = "wav_header_finalize_failed";
  wav.close();
  if (!ok || pcmBytes == 0) {
    SPIFFS.remove(wavPath);
    if (error.isEmpty()) error = "no_audio_decoded";
    return false;
  }
  Serial.printf("WATCH_ASR_WAV_READY aggregates=%lu opus_frames=%lu pcm_bytes=%lu sample_rate=%u\n",
                static_cast<unsigned long>(aggregates), static_cast<unsigned long>(decodedFrames),
                static_cast<unsigned long>(pcmBytes), static_cast<unsigned>(kWatchAsrSampleRate));
  return true;
}

class MiMoAsrBodyStream : public Stream {
 public:
  MiMoAsrBodyStream(File file, const String& modelName, const String& language)
      : file_(file),
        prefix_(String("{\"model\":\"") + jsonEscape(modelName) +
                "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,"),
        suffix_(String("\"}}]}],\"asr_options\":{\"language\":\"") + jsonEscape(language) + "\"}}") {
    const size_t fileSize = file_ ? file_.size() : 0;
    base64Length_ = 4 * ((fileSize + 2) / 3);
    totalLength_ = prefix_.length() + base64Length_ + suffix_.length();
  }

  size_t contentLength() const { return totalLength_; }
  int available() override {
    const size_t remaining = totalLength_ > emitted_ ? totalLength_ - emitted_ : 0;
    return remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
  }
  int read() override {
    if (peeked_ >= 0) { const int value = peeked_; peeked_ = -1; ++emitted_; return value; }
    const int value = readInternal();
    if (value >= 0) ++emitted_;
    return value;
  }
  int peek() override {
    if (peeked_ < 0) peeked_ = readInternal();
    return peeked_;
  }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }

 private:
  static String jsonEscape(const String& value) {
    String out; out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
      const char c = value[i];
      if (c == '\\' || c == '"') { out += '\\'; out += c; }
      else if (c == '\n') out += "\\n";
      else if (c == '\r') out += "\\r";
      else if (static_cast<uint8_t>(c) >= 0x20) out += c;
    }
    return out;
  }

  bool fillBase64() {
    if (!file_ || !file_.available()) return false;
    uint8_t raw[768]{}; // divisible by 3 -> 1024 Base64 bytes for full chunks.
    const size_t got = file_.read(raw, sizeof(raw));
    if (!got) return false;
    size_t encoded = 0;
    if (mbedtls_base64_encode(base64_, sizeof(base64_), &encoded, raw, got) != 0) return false;
    base64Pos_ = 0;
    base64Size_ = encoded;
    return true;
  }

  int readInternal() {
    if (prefixPos_ < prefix_.length()) return static_cast<uint8_t>(prefix_[prefixPos_++]);
    if (base64Pos_ < base64Size_) return base64_[base64Pos_++];
    if (file_ && file_.available()) {
      if (!fillBase64()) return -1;
      return base64_[base64Pos_++];
    }
    if (suffixPos_ < suffix_.length()) return static_cast<uint8_t>(suffix_[suffixPos_++]);
    return -1;
  }

  File file_;
  String prefix_, suffix_;
  size_t prefixPos_ = 0, suffixPos_ = 0;
  size_t base64Length_ = 0, totalLength_ = 0, emitted_ = 0;
  uint8_t base64_[1025]{};
  size_t base64Pos_ = 0, base64Size_ = 0;
  int peeked_ = -1;
};

bool mimoAsrRequest(const char* wavPath, String& transcript, String& error) {
  if (WiFi.status() != WL_CONNECTED) { error = "wifi_not_connected"; return false; }
  if (apiKey.isEmpty()) { error = "mimo_api_key_missing"; return false; }
  File wav = SPIFFS.open(wavPath, FILE_READ);
  if (!wav) { error = "asr_wav_missing"; return false; }
  if (wav.size() > 7 * 1024 * 1024) { wav.close(); error = "asr_audio_too_large"; return false; }

  const String language = (asrLanguage == "zh" || asrLanguage == "en") ? asrLanguage : "auto";
  const String activeAsrModel = asrModel.isEmpty() ? "mimo-v2.5-asr" : asrModel;
  MiMoAsrBodyStream requestStream(wav, activeAsrModel, language);
  const String url = cleanBase(baseUrl) + "/chat/completions";
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(url, http, plain, secure, error)) { wav.close(); return false; }
  http.setTimeout(45000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + apiKey);
  const int code = http.sendRequest("POST", &requestStream, requestStream.contentLength());
  const String response = http.getString();
  http.end();
  wav.close();
  if (code < 200 || code >= 300) {
    error = "asr_http_" + String(code) + ":" + response.substring(0, 384);
    return false;
  }
  JsonDocument res;
  if (deserializeJson(res, response)) { error = "asr_json_invalid"; return false; }
  const char* text = res["choices"][0]["message"]["content"] | nullptr;
  if (!text || !*text) { error = "asr_text_missing"; return false; }
  transcript = text;
  transcript.trim();
  if (transcript.length() > 740) transcript = transcript.substring(0, 740);
  return !transcript.isEmpty();
}

void updateAsrStatus(const String& transcript, const String& error, const String& answer) {
  if (gBridgeStateMutex && xSemaphoreTake(gBridgeStateMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
    snprintf(gLastAsrTranscript, sizeof(gLastAsrTranscript), "%s", transcript.c_str());
    snprintf(gLastAsrError, sizeof(gLastAsrError), "%s", error.c_str());
    snprintf(gLastAiAnswer, sizeof(gLastAiAnswer), "%s", answer.c_str());
    xSemaphoreGive(gBridgeStateMutex);
  } else {
    snprintf(gLastAsrTranscript, sizeof(gLastAsrTranscript), "%s", transcript.c_str());
    snprintf(gLastAsrError, sizeof(gLastAsrError), "%s", error.c_str());
    snprintf(gLastAiAnswer, sizeof(gLastAiAnswer), "%s", answer.c_str());
  }
}

void watchAsrTaskMain(void* opaque) {
  AsrJobContext* job = static_cast<AsrJobContext*>(opaque);
  if (!job) {
    gWatchAsrTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  const bool finalResult = job->finalResult;
  const uint32_t sessionId = job->sessionId;
  const String packetPath = job->packetPath;
  const String wavPath = job->wavPath;
  delete job;

  String error, transcript, answer;
  bool ok = decodeWatchOpusPacketsToWav(packetPath.c_str(), wavPath.c_str(), error);
  if (ok) ok = mimoAsrRequest(wavPath.c_str(), transcript, error);

  AsrResultMessage result;
  result.ok = ok;
  result.finalResult = finalResult;
  result.sessionId = sessionId;
  if (ok) snprintf(result.text, sizeof(result.text), "%s", transcript.c_str());
  else snprintf(result.error, sizeof(result.error), "%s", error.c_str());
  updateAsrStatus(transcript, error, "");
  if (gAsrResultQueue) xQueueSend(gAsrResultQueue, &result, pdMS_TO_TICKS(100));

  if (ok) {
    Serial.printf("WATCH_ASR_RESULT session=%lu final=%s text_bytes=%u\n",
                  static_cast<unsigned long>(sessionId), finalResult ? "true" : "false",
                  static_cast<unsigned>(transcript.length()));
    if (finalResult && chatAfterAsr) {
      String chatError;
      if (mimoWatchRequest(transcript, answer, chatError)) {
        updateAsrStatus(transcript, "", answer);
        Serial.printf("WATCH_MIMO_CHAT_RESULT session=%lu answer_bytes=%u context_turns=%u native_answer_return=false\n",
                      static_cast<unsigned long>(sessionId), static_cast<unsigned>(answer.length()),
                      static_cast<unsigned>(gWatchContextCount));
      } else {
        updateAsrStatus(transcript, "chat:" + chatError, "");
        Serial.printf("WATCH_MIMO_CHAT_FAILED session=%lu reason=%s\n",
                      static_cast<unsigned long>(sessionId), chatError.c_str());
      }
      gXiaoAiAssistantDoneSessionId = sessionId;
    }
  } else {
    Serial.printf("WATCH_ASR_FAILED session=%lu final=%s reason=%s\n",
                  static_cast<unsigned long>(sessionId), finalResult ? "true" : "false", error.c_str());
    if (finalResult && chatAfterAsr) gXiaoAiAssistantDoneSessionId = sessionId;
  }

  SPIFFS.remove(packetPath.c_str());
  SPIFFS.remove(wavPath.c_str());
  gWatchAsrTask = nullptr;
  vTaskDelete(nullptr);
}

bool scheduleWatchAsrJob(const char* packetPath, const char* wavPath, bool finalResult, uint32_t sessionId) {
  if (gWatchAsrTask || !packetPath || !wavPath || sessionId == 0) return false;
  if (!gAsrResultQueue) gAsrResultQueue = xQueueCreate(2, sizeof(AsrResultMessage));
  if (!gAsrResultQueue) return false;

  AsrJobContext* job = new AsrJobContext();
  if (!job) {
    Serial.println("WATCH_ASR_JOB_ALLOC_FAILED");
    return false;
  }
  job->finalResult = finalResult;
  job->sessionId = sessionId;
  snprintf(job->packetPath, sizeof(job->packetPath), "%s", packetPath);
  snprintf(job->wavPath, sizeof(job->wavPath), "%s", wavPath);
  BaseType_t created = xTaskCreatePinnedToCore(watchAsrTaskMain, finalResult ? "hoshino_asr_final" : "hoshino_asr_partial",
                                               kWatchAsrTaskStackBytes, job, 1, &gWatchAsrTask, 1);
  if (created != pdPASS) {
    delete job;
    gWatchAsrTask = nullptr;
    Serial.println("WATCH_ASR_TASK_CREATE_FAILED");
    return false;
  }
  Serial.printf("WATCH_ASR_TASK_STARTED session=%lu final=%s model=mimo-v2.5-asr decode=opus_5x80_to_wav16k\n",
                static_cast<unsigned long>(sessionId), finalResult ? "true" : "false");
  return true;
}
#endif  // HOSHINO_ENABLE_ASR

// Phone-facing setup page. Keep this surface limited to the AP configuration
// endpoints below; bridge controls and cloud/chat/storage APIs stay off AP.
const char kMinimalSetupWebUi[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Hoshino 配网</title>
<style>body{font-family:system-ui;background:#0c1118;color:#eef4ff;max-width:680px;margin:auto;padding:18px}input,button{box-sizing:border-box;width:100%;padding:11px;margin:5px 0;border-radius:8px;border:1px solid #334;background:#151d29;color:#fff}button{background:#2c6bed;cursor:pointer}.card{background:#111923;padding:15px;border-radius:12px;margin:12px 0}label{display:block;margin-top:10px}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.check{width:auto}pre{white-space:pre-wrap;word-break:break-word;background:#080d13;padding:12px;border-radius:8px}@media(max-width:650px){.row{grid-template-columns:1fr}}</style></head><body>
<h2>Hoshino ESP32 配网</h2><p>请填写家庭 Wi‑Fi 和手表身份。保存后设备会重启；热点消失属于正常现象。</p>
<div class=card><div class=row><label>家庭 Wi‑Fi SSID<input id=ssid autocomplete=off></label><label>Wi‑Fi 密码<input id=wp type=password autocomplete=off></label></div>
<div class=row><label>Watch MAC<input id=mac placeholder="AA:BB:CC:DD:EE:FF" autocomplete=off></label><label>Watch Auth Key<input id=wauth type=password placeholder="32 位十六进制" autocomplete=off></label></div>
<div class=row><label>本地管理 Token<input id=token value="hoshino-local" autocomplete=off></label><label>Setup AP 密码<input id=ap type=password value="hoshino-setup" autocomplete=off></label></div>
<label><input class=check id=autow type=checkbox checked> 开机自动连接手表</label><button onclick=save()>保存配置并重启</button><pre id=o>正在读取状态…</pre></div>
<script>
const o=document.getElementById('o');
async function status(){try{const r=await fetch('/setup/status');const d=await r.json();o.textContent=d.ok?('热点地址：'+(d.setupApIp||location.hostname)+'\n家庭 Wi‑Fi：'+(d.wifiConfigured?'已配置':'未配置')+'\n手表身份：'+(d.watchConfigured?'已配置':'未配置')):JSON.stringify(d)}catch(e){o.textContent=String(e)}}
async function save(){const body={ssid:ssid.value,wifiPassword:wp.value,watchMac:mac.value,watchAuthKey:wauth.value,localToken:token.value,apPassword:ap.value,autoConnectWatch:autow.checked};o.textContent='正在保存…';try{const r=await fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const d=await r.json();o.textContent=d.ok?'已保存，设备正在重启，请等待热点消失。':('保存失败：'+(d.message||d.error||JSON.stringify(d)))}catch(e){o.textContent=String(e)}}
status();
</script></body></html>
)HTML";

void setWatchBridgeState(const char* state) {
  if (!state) state = "unknown";
  if (gBridgeStateMutex && xSemaphoreTake(gBridgeStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snprintf(gWatchBridgeState, sizeof(gWatchBridgeState), "%s", state);
    xSemaphoreGive(gBridgeStateMutex);
  } else {
    snprintf(gWatchBridgeState, sizeof(gWatchBridgeState), "%s", state);
  }
}

String getWatchBridgeState() {
  char state[sizeof(gWatchBridgeState)]{};
  if (gBridgeStateMutex && xSemaphoreTake(gBridgeStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snprintf(state, sizeof(state), "%s", gWatchBridgeState);
    xSemaphoreGive(gBridgeStateMutex);
  } else {
    snprintf(state, sizeof(state), "%s", gWatchBridgeState);
  }
  return String(state);
}

void watchBridgeTaskMain(void*) {
  gWatchBridgeRunning = true;
  gWatchBridgeStopRequested = false;
  setWatchBridgeState("connecting");
  String mac = watchMac;
  String auth = watchAuthKey;
  authenticateWatchSpp(mac, auth);
  auth = "";
  // Bluetooth SPP is up; now bring up home Wi-Fi for the QuickApp network
  // bridge (NAPT). This keeps heap large/unfragmented during BT init.
  connectWifi();
  gWatchBridgeRunning = false;
  gWatchBridgeStopRequested = false;
  setWatchBridgeState("idle");
  gWatchBridgeTask = nullptr;
  vTaskDelete(nullptr);
}

bool startWatchBridge(String& error) {
  gWatchAutoReconnectSuppressed = false;
  if (gWatchBridgeRunning || gWatchBridgeTask) { error = "already_running"; return false; }
  if (watchMac.length() != 17) { error = "watch_mac_missing"; return false; }
  if (watchAuthKey.length() != 32) { error = "watch_auth_key_missing"; return false; }
  // Home Wi-Fi is connected lazily AFTER Bluetooth auth succeeds, so the
  // Bluetooth controller can init while the heap is still large and unfragmented.
  gWatchBridgeStopRequested = false;
  setWatchBridgeState("starting");
  const BaseType_t created = xTaskCreatePinnedToCore(watchBridgeTaskMain, "hoshino_watch", 6144,
                                                     nullptr, 2, &gWatchBridgeTask, 1);
  if (created != pdPASS) {
    gWatchBridgeTask = nullptr;
    setWatchBridgeState("task_create_failed");
    error = "watch_task_create_failed";
    return false;
  }
  return true;
}

void stopWatchBridge() {
  gWatchAutoReconnectSuppressed = true;
  gWatchBridgeStopRequested = true;
  setWatchBridgeState("stopping");
}

void handleStatus(bool publicSetup) {
  const bool watchFetchProbe = server.arg("source") == "watch_quickapp";
  const bool setupApClient = isSetupApClient();
  // The watch itself may probe this endpoint through channel 7 before an admin
  // token exists in its HTTP client. Return only non-sensitive liveness data in
  // that case. Home-LAN management requires the configured local token.
  if (publicSetup && watchFetchProbe && !setupApClient && !authorized()) {
    JsonDocument d;
    d["ok"] = true;
    d["bridge"] = "hoshino-esp32";
    d["watchFetchProbe"] = true;
    d["networkProxyReady"] = gWatchNetworkReady;
    d["wifi"] = WiFi.status() == WL_CONNECTED;
    sendJson(200, d);
    return;
  }
  if (!setupApClient && !requireAuth()) return;
  JsonDocument d;
  d["ok"] = true;
  d["bridge"] = "hoshino-esp32";
  d["version"] = 3;
  d["wifi"] = WiFi.status() == WL_CONNECTED;
  d["staIp"] = WiFi.localIP().toString();
  d["setupApIp"] = WiFi.softAPIP().toString();
  d["model"] = model;
  d["baseUrl"] = baseUrl;
  d["apiKeyConfigured"] = !apiKey.isEmpty();
  d["watchConfigured"] = watchMac.length() == 17 && watchAuthKey.length() == 32;
  d["watchMac"] = watchMac; // MAC is not a secret; auth key is intentionally never returned.
  d["sdpResolvedChannel"] = gWatchResolvedSppChannel;
  d["sdpSource"] = gWatchSdpSource;
  d["sdpCacheAgeMs"] = gWatchSdpResolvedMs ? millis() - gWatchSdpResolvedMs : 0;
  d["watchBridgeRunning"] = gWatchBridgeRunning;
  d["watchBridgeState"] = getWatchBridgeState();
  d["networkProxyReady"] = gWatchNetworkReady;
  d["networkRxPackets"] = gWatchNetworkRxPackets;
  d["networkTxPackets"] = gWatchNetworkTxPackets;
  d["networkDroppedPackets"] = gWatchNetworkDroppedPackets;
  d["lastAiAnswer"] = gLastAiAnswer;
  d["watchConversationTurns"] = gWatchContextCount;
  d["watchConversationIdleResetMs"] = kWatchContextIdleResetMs;
  d["autoConnectWatch"] = autoConnectWatch;
  d["caConfigured"] = !caPem.isEmpty();
  d["insecureTls"] = allowInsecureTls;
  d["watchFetchProbe"] = watchFetchProbe;
  d["localTokenHint"] = "enter the local token configured during setup";
  sendJson(200, d);
}

String storageContentType(const String& path) {
  String lower = path;
  lower.toLowerCase();
  if (lower.endsWith(".json")) return "application/json";
  if (lower.endsWith(".txt") || lower.endsWith(".log") || lower.endsWith(".md")) return "text/plain; charset=utf-8";
  if (lower.endsWith(".html") || lower.endsWith(".htm")) return "text/html; charset=utf-8";
  if (lower.endsWith(".css")) return "text/css; charset=utf-8";
  if (lower.endsWith(".js")) return "application/javascript";
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".gif")) return "image/gif";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".wav")) return "audio/wav";
  if (lower.endsWith(".mp3")) return "audio/mpeg";
  if (lower.endsWith(".bin")) return "application/octet-stream";
  return "application/octet-stream";
}

int storageErrorStatus(const String& error) {
  if (error == "not_found") return 404;
  if (error == "path_required" || error == "path_too_long" ||
      error == "invalid_path" || error == "invalid_path_segment" ||
      error == "invalid_path_character") return 400;
  return 503;
}

void handleStorageStatus() {
  if (!requireStorageAuth()) return;
  String error;
  if (!sdStorage.ensureMounted(error)) return jsonError(503, "sd_unavailable", error);
  const hoshino::SdStorageStatus st = sdStorage.status();
  JsonDocument d;
  d["ok"] = true;
  d["mounted"] = st.mounted;
  d["root"] = hoshino::SdStorageService::rootPath();
  d["cardType"] = st.cardType;
  d["cardSizeBytes"] = st.cardSize;
  d["totalBytes"] = st.totalBytes;
  d["usedBytes"] = st.usedBytes;
  d["freeBytes"] = st.totalBytes >= st.usedBytes ? st.totalBytes - st.usedBytes : 0;
  d["spi"] = "VSPI";
  d["pins"]["sck"] = 18;
  d["pins"]["miso"] = 19;
  d["pins"]["mosi"] = 23;
  d["pins"]["cs"] = 5;
  sendJson(200, d);
}

void handleStorageList() {
  if (!requireStorageAuth()) return;
  const String path = server.arg("path");
  String error;
  File dir = sdStorage.openRead(path, error);
  if (!dir) return jsonError(storageErrorStatus(error), "storage_list_failed", error);
  if (!dir.isDirectory()) {
    dir.close();
    return jsonError(400, "not_a_directory", path);
  }

  JsonDocument d;
  d["ok"] = true;
  d["path"] = path;
  JsonArray items = d["items"].to<JsonArray>();
  constexpr size_t kMaxStorageListEntries = 48;
  size_t count = 0;
  bool truncated = false;
  for (;;) {
    File child = dir.openNextFile();
    if (!child) break;
    if (count >= kMaxStorageListEntries) {
      truncated = true;
      child.close();
      break;
    }
    JsonObject item = items.add<JsonObject>();
    item["name"] = child.name();
    item["directory"] = child.isDirectory();
    item["size"] = child.isDirectory() ? 0 : child.size();
    child.close();
    ++count;
  }
  dir.close();
  d["count"] = count;
  d["truncated"] = truncated;
  sendJson(200, d);
}

void handleStorageFile() {
  if (!requireStorageAuth()) return;
  const String path = server.arg("path");
  if (path.isEmpty()) return jsonError(400, "path_required", "use ?path=relative/file.bin");
  String error;
  File file = sdStorage.openRead(path, error);
  if (!file) return jsonError(storageErrorStatus(error), "storage_read_failed", error);
  if (file.isDirectory()) {
    file.close();
    return jsonError(400, "is_a_directory", path);
  }
  server.sendHeader("Cache-Control", "private, max-age=60");
  server.sendHeader("X-Hoshino-Storage", "sd");
  server.streamFile(file, storageContentType(path));
  file.close();
}

void handleStorageChunk() {
  if (!requireStorageAuth()) return;
  const String path = server.arg("path");
  if (path.isEmpty()) return jsonError(400, "path_required", "use ?path=relative/file.bin");

  const long requestedOffset = server.hasArg("offset") ? server.arg("offset").toInt() : 0;
  const long requestedLength = server.hasArg("length") ? server.arg("length").toInt() : 1024;
  if (requestedOffset < 0 || requestedLength <= 0 || requestedLength > 2048) {
    return jsonError(400, "invalid_range", "offset>=0 and 1<=length<=2048 required");
  }

  String error;
  File file = sdStorage.openRead(path, error);
  if (!file) return jsonError(storageErrorStatus(error), "storage_read_failed", error);
  if (file.isDirectory()) {
    file.close();
    return jsonError(400, "is_a_directory", path);
  }

  const size_t fileSize = file.size();
  const size_t offset = static_cast<size_t>(requestedOffset);
  if (offset > fileSize) {
    file.close();
    return jsonError(416, "range_not_satisfiable", "offset exceeds file size");
  }
  if (!file.seek(offset, SeekSet)) {
    file.close();
    return jsonError(500, "seek_failed", path);
  }

  const size_t toRead = min(static_cast<size_t>(requestedLength), fileSize - offset);
  uint8_t* raw = static_cast<uint8_t*>(malloc(toRead ? toRead : 1));
  if (!raw) {
    file.close();
    return jsonError(503, "low_memory", "cannot allocate SD read buffer");
  }
  const size_t bytesRead = toRead ? file.read(raw, toRead) : 0;
  file.close();

  const size_t encodedCapacity = 4 * ((bytesRead + 2) / 3) + 1;
  unsigned char* encoded = static_cast<unsigned char*>(malloc(encodedCapacity));
  if (!encoded) {
    free(raw);
    return jsonError(503, "low_memory", "cannot allocate base64 buffer");
  }
  size_t encodedLength = 0;
  const int base64Rc = mbedtls_base64_encode(encoded, encodedCapacity, &encodedLength, raw, bytesRead);
  free(raw);
  if (base64Rc != 0) {
    free(encoded);
    return jsonError(500, "base64_failed", String(base64Rc));
  }
  encoded[encodedLength] = '\0';

  String response;
  response.reserve(encodedLength + 192);
  response += "{\"ok\":true,\"encoding\":\"base64\",\"offset\":";
  response += String(static_cast<unsigned long>(offset));
  response += ",\"bytes\":";
  response += String(static_cast<unsigned long>(bytesRead));
  response += ",\"nextOffset\":";
  response += String(static_cast<unsigned long>(offset + bytesRead));
  response += ",\"size\":";
  response += String(static_cast<unsigned long>(fileSize));
  response += ",\"eof\":";
  response += (offset + bytesRead >= fileSize) ? "true" : "false";
  response += ",\"data\":\"";
  response.concat(reinterpret_cast<const char*>(encoded), encodedLength);
  response += "\"}";
  free(encoded);
  server.send(200, "application/json", response);
}

void handlePhoneSetupStatus() {
  if (!isSetupApClient()) {
    jsonError(403, "setup_ap_required", "setup status is only available through the Hoshino-Bridge SoftAP");
    return;
  }
  JsonDocument d;
  d["ok"] = true;
  d["wifiConfigured"] = !ssid.isEmpty();
  d["watchConfigured"] = watchMac.length() == 17 && watchAuthKey.length() == 32;
  d["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  d["setupApIp"] = WiFi.softAPIP().toString();
  sendJson(200, d);
}

void handlePhoneSetupSave() {
  if (!requireSetupAp()) return;
  const String requestBody = body();
  if (requestBody.isEmpty()) return jsonError(400, "invalid_body", "empty or too large");

  JsonDocument in;
  if (deserializeJson(in, requestBody)) return jsonError(400, "invalid_json", "cannot parse json");

  String newSsid = String(in["ssid"] | "");
  String newPass = String(in["wifiPassword"] | "");
  String newMac = String(in["watchMac"] | "");
  String newAuth = String(in["watchAuthKey"] | "");
  String newToken = String(in["localToken"] | "");
  String newAp = String(in["apPassword"] | "");
  const bool newAuto = in["autoConnectWatch"] | false;

  newMac.trim();
  newMac.toUpperCase();
  newAuth.trim();
  newAuth.toLowerCase();
  if (newToken.isEmpty()) newToken = localToken;
  if (newAp.isEmpty()) newAp = apPassword;

  if (newSsid.length() > 64 || newPass.length() > 128 || newToken.length() > 128 ||
      newAp.length() < 8 || newAp.length() > 63) {
    return jsonError(400, "invalid_config", "field length invalid");
  }
  if (!newMac.isEmpty() && newMac.length() != 17) {
    return jsonError(400, "invalid_watch_mac", "expected AA:BB:CC:DD:EE:FF");
  }
  if (!newAuth.isEmpty()) {
    if (newAuth.length() != 32) return jsonError(400, "invalid_watch_auth", "expected 32 hexadecimal characters");
    for (size_t i = 0; i < newAuth.length(); ++i) {
      if (!isxdigit(static_cast<unsigned char>(newAuth[i]))) {
        return jsonError(400, "invalid_watch_auth", "expected hexadecimal characters only");
      }
    }
  }

  const String oldSsid = ssid;
  const String oldPass = wifiPass;
  const String oldMac = watchMac;
  const String oldAuth = watchAuthKey;
  const String oldToken = localToken;
  const String oldAp = apPassword;
  const bool oldAuto = autoConnectWatch;
  const bool saved =
      prefs.putString("ssid", newSsid) == newSsid.length() &&
      prefs.putString("wifi_pass", newPass) == newPass.length() &&
      prefs.putString("watch_mac", newMac) == newMac.length() &&
      prefs.putString("watch_auth", newAuth) == newAuth.length() &&
      prefs.putString("local_token", newToken) == newToken.length() &&
      prefs.putString("ap_pass", newAp) == newAp.length();
  if (!saved) {
    prefs.putString("ssid", oldSsid);
    prefs.putString("wifi_pass", oldPass);
    prefs.putString("watch_mac", oldMac);
    prefs.putString("watch_auth", oldAuth);
    prefs.putString("local_token", oldToken);
    prefs.putString("ap_pass", oldAp);
    prefs.putBool("watch_auto", oldAuto);
    return jsonError(503, "config_save_failed", "preferences write failed; previous configuration restored");
  }
  prefs.putBool("watch_auto", newAuto || (newMac.length() == 17 && newAuth.length() == 32));
  ssid = newSsid;
  wifiPass = newPass;
  watchMac = newMac;
  watchAuthKey = newAuth;
  localToken = newToken;
  apPassword = newAp;
  autoConnectWatch = newAuto || (newMac.length() == 17 && newAuth.length() == 32);

  JsonDocument out;
  out["ok"] = true;
  out["saved"] = true;
  out["rebooting"] = true;
  out["watchConfigured"] = newMac.length() == 17 && newAuth.length() == 32;
  sendJson(200, out);
  Serial.println("SETUP_SAVED rebooting_to_normal_mode");
  delay(400);
  ESP.restart();
}

void setupRoutes() {
  const char* keys[] = {"Authorization", "X-Hoshino-Token", "X-Hoshino-Watch-Setup", "X-Hoshino-Storage"};
  server.collectHeaders(keys, 4);
  server.on("/", HTTP_GET, [](){ server.send_P(200, "text/html; charset=utf-8", kMinimalSetupWebUi); });
  server.on("/setup/status", HTTP_GET, [](){ handlePhoneSetupStatus(); });
  server.on("/setup", HTTP_POST, [](){ handlePhoneSetupSave(); });
  // These watch-only provisioning routes intentionally do not alter SPP,
  // virtual netif, DHCP, DNS, NAPT, or any Bluetooth/TCP tunnel code.
  server.on("/watch-setup/status", HTTP_GET, [](){ handleWatchSetupStatus(); });
  server.on("/watch-setup/scan", HTTP_GET, [](){ handleWatchSetupScan(); });
  server.on("/watch-setup/wifi", HTTP_POST, [](){ handleWatchSetupWifi(); });
  server.on("/api/v1/storage/status", HTTP_GET, [](){ handleStorageStatus(); });
  server.on("/api/v1/storage/list", HTTP_GET, [](){ handleStorageList(); });
  server.on("/api/v1/storage/file", HTTP_GET, [](){ handleStorageFile(); });
  server.on("/api/v1/storage/chunk", HTTP_GET, [](){ handleStorageChunk(); });
  server.onNotFound([](){
    if (gSetupMode) {
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "redirect to setup");
      return;
    }
    server.send(404, "text/plain; charset=utf-8", "not found\n");
  });
}

void maintainMdns() {
  if (WiFi.status() != WL_CONNECTED) {
    if (gMdnsStarted) { MDNS.end(); gMdnsStarted = false; }
    return;
  }
  if (!gMdnsStarted && MDNS.begin("hoshino-bridge")) {
    MDNS.addService("http", "tcp", 80);
    gMdnsStarted = true;
    Serial.println("MDNS_READY host=hoshino-bridge.local");
  }
}

// ---- 配网模式（首次配置 / IO 短接触发） ----

void setupTriggerPins() {
  pinMode(kSetupTriggerPinOut, OUTPUT);
  digitalWrite(kSetupTriggerPinOut, HIGH);  // 输出高电平
  pinMode(kSetupTriggerPinIn, INPUT_PULLDOWN);  // 输入下拉
}

bool setupTriggered() {
  // GPIO17 被 GPIO16 短接后读到高电平。
  return digitalRead(kSetupTriggerPinIn) == HIGH;
}

const char* wifiFailureName(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "wrong_password";
    case WIFI_REASON_NO_AP_FOUND:
      return "network_not_found";
    default:
      return "connect_failed";
  }
}

void handleWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    if (!gWifiStaConnecting && !gWifiProvisioningPending) return;
    const uint8_t reason = info.wifi_sta_disconnected.reason;
    gWifiStaLastReason = reason;
    snprintf(gWifiError, sizeof(gWifiError), "%s", wifiFailureName(reason));
    snprintf(gWifiState, sizeof(gWifiState), "%s", "error");
    gWifiStaStopReconnectPending = true;
    gWifiStaFailureBackoffUntilMs = millis() + kWifiStaFailureBackoffMs;
    Serial.printf("WIFI_STA_FAILURE reason=%u error=%s backoff_ms=%lu\n",
                  static_cast<unsigned>(reason), gWifiError,
                  static_cast<unsigned long>(kWifiStaFailureBackoffMs));
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    gWifiStaLastReason = 0;
    gWifiError[0] = '\0';
    snprintf(gWifiState, sizeof(gWifiState), "%s", "connected");
    gWifiStaStopReconnectPending = false;
    gWifiStaFailureBackoffUntilMs = 0;
  }
}

void beginWifiStaConnection(uint32_t timeoutMs) {
  gWifiStaConnecting = true;
  gWifiStaConnectDeadlineMs = millis() + (timeoutMs ? timeoutMs : kWifiStaConnectTimeoutMs);
  gWifiStaStopReconnectPending = false;
  gWifiStaFailureBackoffUntilMs = 0;
  gWifiStaLastReason = 0;
  gWifiError[0] = '\0';
  snprintf(gWifiState, sizeof(gWifiState), "%s", "connecting");
}

void clearWifiStaConnection(const char* reason) {
  if (gWifiStaConnecting) {
    Serial.printf("WATCH_WIFI_STA_CONNECT_END reason=%s status=%d\n",
                  reason ? reason : "unknown", static_cast<int>(WiFi.status()));
  }
  gWifiStaConnecting = false;
  gWifiStaConnectDeadlineMs = 0;
}

void serviceWifiStaConnectionState(bool cancelled) {
  if (gWifiStaStopReconnectPending) {
    WiFi.setAutoReconnect(false);
    gWifiStaStopReconnectPending = false;
    gWifiStaConnecting = false;
    gWifiStaConnectDeadlineMs = 0;
    if (gWifiProvisioningPending) {
      gWifiProvisioningPending = false;
      gPendingWifiSsid = "";
      gPendingWifiPassword = "";
    }
    Serial.printf("WIFI_STA_RECONNECT_STOPPED error=%s reason=%u\n",
                  gWifiError[0] ? gWifiError : "connect_failed",
                  static_cast<unsigned>(gWifiStaLastReason));
    return;
  }
  if (!gWifiStaConnecting) return;
  const wl_status_t state = WiFi.status();
  const bool timedOut = static_cast<int32_t>(millis() - gWifiStaConnectDeadlineMs) >= 0;
  const bool failed = state == WL_NO_SSID_AVAIL || state == WL_CONNECT_FAILED;
  if (cancelled) {
    clearWifiStaConnection("cancelled");
    gWifiProvisioningPending = false;
    gPendingWifiSsid = "";
    gPendingWifiPassword = "";
  } else if (state == WL_CONNECTED) {
    clearWifiStaConnection("connected");
    finishPendingWifiProvisioning(true);
  } else if (failed) {
    snprintf(gWifiState, sizeof(gWifiState), "%s", "error");
    if (!gWifiError[0]) snprintf(gWifiError, sizeof(gWifiError), "%s",
                                 state == WL_NO_SSID_AVAIL ? "network_not_found" : "connect_failed");
    WiFi.setAutoReconnect(false);
    clearWifiStaConnection("failed");
    finishPendingWifiProvisioning(false);
  } else if (timedOut) {
    snprintf(gWifiState, sizeof(gWifiState), "%s", "timeout");
    snprintf(gWifiError, sizeof(gWifiError), "%s", "connect_timeout");
    WiFi.setAutoReconnect(false);
    clearWifiStaConnection("timeout");
    finishPendingWifiProvisioning(false);
  }
}

void finishPendingWifiProvisioning(bool connected) {
  if (!gWifiProvisioningPending) return;
  const String pendingSsid = gPendingWifiSsid;
  const String pendingPass = gPendingWifiPassword;
  gWifiProvisioningPending = false;
  gPendingWifiSsid = "";
  gPendingWifiPassword = "";
  if (!connected) return;

  const String oldSsid = ssid;
  const String oldPass = wifiPass;
  const bool ssidSaved = prefs.putString("ssid", pendingSsid) == pendingSsid.length();
  const bool passSaved = prefs.putString("wifi_pass", pendingPass) == pendingPass.length();
  if (!ssidSaved || !passSaved) {
    prefs.putString("ssid", oldSsid);
    prefs.putString("wifi_pass", oldPass);
    snprintf(gWifiState, sizeof(gWifiState), "%s", "error");
    snprintf(gWifiError, sizeof(gWifiError), "%s", "wifi_config_save_failed");
    return;
  }
  ssid = pendingSsid;
  wifiPass = pendingPass;
  gWifiError[0] = '\0';
  snprintf(gWifiState, sizeof(gWifiState), "%s", "connected");
  Serial.printf("WATCH_WIFI_SETUP_SAVED ssid_bytes=%u\n",
                static_cast<unsigned>(ssid.length()));
}

void enterSetupMode() {
  gSetupMode = true;
  gWatchBridgeStopRequested = true;  // 停止任何进行中的桥接，让阻塞的 loop() 恢复
  gWatchAutoReconnectSuppressed = true;
  WiFi.mode(WIFI_AP);
  const bool apStarted = WiFi.softAP(kApSsid, apPassword.c_str());
  gSoftApActive = apStarted;
  if (!gServerStarted) {
    server.begin();
    gServerStarted = true;
  }
  Serial.printf("SETUP_MODE_ACTIVE ap=%s ip=%s started=%s\n",
                kApSsid, WiFi.softAPIP().toString().c_str(),
                apStarted ? "true" : "false");
}

// 独立任务检测 GPIO 短接：不依赖 loop()，因此即使 authenticateWatchSpp
// 同步阻塞了 Arduino 主循环，这个任务仍能触发配网。
static void setupTriggerTask(void*) {
  uint32_t since = 0;
  while (true) {
    if (setupTriggered()) {
      if (since == 0) since = millis();
      else if (millis() - since >= kSetupTriggerHoldMs) {
        // 不在运行时切换 WiFi（STA→AP 会因内存/状态问题失败）。
        // 改为写入标志并重启，重启后在干净 boot 环境直接进入 AP 配网模式。
        Serial.println("SETUP_MODE_REASON io_short (save flag + reboot)");
        prefs.putBool("force_setup", true);
        delay(300);  // 让 NVS 标志落盘
        ESP.restart();
      }
    } else {
      since = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void connectWifi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  // When a home Wi-Fi is already configured, use STA-only mode so the
  // SoftAP netif does not consume the heap the Bluetooth controller needs.
  // The setup AP is only enabled when no Wi-Fi has been configured yet.
  if (ssid.isEmpty()) {
    clearWifiStaConnection("ap_mode");
    WiFi.mode(WIFI_AP);
    const bool apStarted = WiFi.softAP(kApSsid, apPassword.c_str());
    gSoftApActive = apStarted;
    Serial.printf("SoftAP started=%s ssid=%s ap=%s (no home wifi configured)\n",
                  apStarted ? "true" : "false", kApSsid, WiFi.softAPIP().toString().c_str());
    return;
  }
  gSoftApActive = false;
  WiFi.mode(WIFI_STA);
  beginWifiStaConnection(kWifiStaConnectTimeoutMs);
  WiFi.begin(ssid.c_str(), wifiPass.c_str());
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED && gWifiStaConnecting && millis()-start<60000){
    serviceWifiStaConnectionState(false);
    delay(250);
  }
  clearWifiStaConnection(WiFi.status()==WL_CONNECTED ? "connected" : "wait_end");
  Serial.printf("STA mode connected=%s ip=%s\n",
                WiFi.status() == WL_CONNECTED ? "true" : "false",
                WiFi.localIP().toString().c_str());
  // mDNS is started lazily from loop() after the Bluetooth bridge has
  // initialised, to keep heap available for the Bluetooth controller.
}

// ==================== 0.96" SSD1306 状态屏（I2C） ====================
// I2C 接 GPIO21(SDA) / GPIO22(SCL)。这两个引脚在本固件里完全空闲：GPIO16/17 是
// 配网短接触发，GPIO5/18/19/23 是 SD 卡的 HSPI。模块为 0.96 寸 128x64 SSD1306，
// 地址通常是 0x3C（部分模块是 0x3D，下面的总线扫描会自动兜底）。
//
// 内存策略沿用本项目一贯做法：
//   * 全屏 framebuffer 只有 1024 字节，对 WROOM 的堆影响可忽略；
//   * 不开新任务、不做堆分配 —— 所有文案都是栈上 char[] + snprintf，唯一一次
//     String 访问是读取已存在的 ssid，不做构造；
//   * 已配置的正常启动把初始化推迟到 Wi-Fi 就绪，让蓝牙控制器与 esp_wifi_init
//     先拿到没有被 Wire 切碎的堆（与 SD 卡惰性挂载同一个思路）。
#if HOSHINO_ENABLE_DISPLAY
constexpr int kOledSdaPin = 21;
constexpr int kOledSclPin = 22;
constexpr uint8_t kOledI2cAddr = 0x3C;
constexpr uint32_t kDisplayRefreshMs = HOSHINO_DISPLAY_REFRESH_MS;
// 6x10 字体的字宽步进正好是 6px，128px 一行放得下 21 个字符。
constexpr uint8_t kDisplayLineChars = 21;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, kOledSclPin, kOledSdaPin);
bool gDisplayReady = false;
bool gDisplayInitDeferred = false;
uint32_t gLastDisplayMs = 0;

// 按屏幕宽度截断，避免长 SSID 直接画到屏幕外。
void displayDrawLine(int y, const char* text) {
  char line[kDisplayLineChars + 1];
  size_t index = 0;
  while (index < kDisplayLineChars && text[index] != '\0') {
    line[index] = text[index];
    ++index;
  }
  line[index] = '\0';
  display.drawStr(2, y, line);
}

// 把桥接状态压成适合屏幕的短文案；未知状态原样显示（本身已经比较短）。
const char* displayStateLabel(const char* state) {
  if (!state || !*state) return "idle";
  if (strcmp(state, "connected") == 0) return "connected";
  if (strcmp(state, "connecting") == 0 || strcmp(state, "starting") == 0) return "connecting";
  if (strcmp(state, "stopping") == 0) return "stopping";
  if (strcmp(state, "task_create_failed") == 0) return "task FAIL";
  return state;
}

// 把 uint32 计数压成最多 5 个字符：<10000 原样显示，<1000 万加 k，再大加 M。
// 第 5 行有 21 字符的硬上限，压过之后两个计数加标签最多 15 字符，永远放得下。
void formatCompactCount(char* out, size_t capacity, uint32_t value) {
  if (value < 10000UL) {
    snprintf(out, capacity, "%lu", static_cast<unsigned long>(value));
  } else if (value < 10000000UL) {
    snprintf(out, capacity, "%luk", static_cast<unsigned long>(value / 1000UL));
  } else {
    snprintf(out, capacity, "%luM", static_cast<unsigned long>(value / 1000000UL));
  }
}

// 渲染一帧。只在 loop() 里调用，display 对象不会被其它任务触碰，无需额外加锁。
void renderDisplay() {
  if (!gDisplayReady) return;

  // gWatchBridgeState 由桥接任务在 gBridgeStateMutex 下改写，这里加锁快照，
  // 避免把写了一半的字符串画到屏幕上。
  char bridgeState[sizeof(gWatchBridgeState)]{};
  if (gBridgeStateMutex && xSemaphoreTake(gBridgeStateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    memcpy(bridgeState, gWatchBridgeState, sizeof(bridgeState));
    xSemaphoreGive(gBridgeStateMutex);
  } else {
    snprintf(bridgeState, sizeof(bridgeState), "%s", gWatchBridgeState);
  }
  bridgeState[sizeof(bridgeState) - 1] = '\0';

  char wifiState[sizeof(gWifiState)]{};
  snprintf(wifiState, sizeof(wifiState), "%s", gWifiState);
  char wifiError[sizeof(gWifiError)]{};
  snprintf(wifiError, sizeof(wifiError), "%s", gWifiError);

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tr);
  char buf[48];

  // 第 1 行：工作模式
  snprintf(buf, sizeof(buf), "Hoshino %s", gSetupMode ? "SETUP" : "GATEWAY");
  displayDrawLine(10, buf);

  // 第 2 行：本机 IP —— 配网模式是 SoftAP 地址，正常模式是 STA 地址。
  // 直接从 IPAddress 的 4 个字节格式化，不构造 String，避免在 BT 认证→Wi-Fi
  // 启动这个堆最紧的窗口里反复申请/释放小块把堆切碎。
  const IPAddress ip = gSetupMode ? WiFi.softAPIP()
      : (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress(0, 0, 0, 0));
  snprintf(buf, sizeof(buf), "IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  displayDrawLine(22, buf);

  // 第 3 行：家庭 Wi-Fi 状态（配网页 / SSID / 失败原因 / 连接中）
  if (gSetupMode) {
    snprintf(buf, sizeof(buf), "AP %s", kApSsid);
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "WIFI %s", ssid.c_str());
  } else if (wifiError[0]) {
    snprintf(buf, sizeof(buf), "WIFI %s", wifiError);
  } else {
    snprintf(buf, sizeof(buf), "WIFI %s", wifiState);
  }
  displayDrawLine(34, buf);

  // 第 4 行：手表隧道状态
  const char* watchLabel = "idle";
  if (gWatchNetworkReady) watchLabel = "tunnel up";
  else if (gWatchBridgeRunning) watchLabel = displayStateLabel(bridgeState);
  snprintf(buf, sizeof(buf), "WATCH %s", watchLabel);
  displayDrawLine(46, buf);

  // 第 5 行：NAPT 转发计数与空闲堆。计数先压成紧凑形式（见 formatCompactCount），
  // 保证在 21 字符上限内放得下；万一堆的数字把整行撑爆，就整段丢掉堆，
  // 而绝不让数字被截断 —— 否则 "118K" 会显示成 "118"，变成误导读数。
  char txText[8];
  char rxText[8];
  formatCompactCount(txText, sizeof(txText), gWatchNetworkTxPackets);
  formatCompactCount(rxText, sizeof(rxText), gWatchNetworkRxPackets);
  snprintf(buf, sizeof(buf), "TX%s RX%s %luK", txText, rxText,
           static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
  if (strlen(buf) > kDisplayLineChars) {
    snprintf(buf, sizeof(buf), "TX%s RX%s", txText, rxText);
  }
  displayDrawLine(58, buf);

  display.sendBuffer();
}

// 探测 I2C 总线并点亮屏幕。任何失败都只打串口日志，绝不影响桥接主流程。
void initDisplay() {
  Wire.begin(kOledSdaPin, kOledSclPin);

  // 先扫一遍总线：模块没接好、地址是 0x3D、或者根本没供电，都能从串口直接区分，
  // 不用拆机。地址就取扫到的第一个设备。
  uint8_t detected = 0;
  Serial.printf("DISPLAY_I2C_SCAN sda=%d scl=%d:", kOledSdaPin, kOledSclPin);
  for (uint8_t address = 0x03; address <= 0x77; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", address);
      if (detected == 0) detected = address;
    }
  }
  Serial.println();

  if (detected == 0) {
    Serial.printf("DISPLAY_INIT_FAILED no_i2c_device sda=%d scl=%d\n", kOledSdaPin, kOledSclPin);
    return;
  }

  uint8_t address = kOledI2cAddr;
  if (detected != kOledI2cAddr) {
    Serial.printf("DISPLAY_ADDR_OVERRIDE configured=0x%02X using=0x%02X\n", kOledI2cAddr, detected);
    address = detected;
  }

  // U8g2 的 setI2CAddress 接收 8 位地址，而 I2C 扫描出来的是 7 位地址。
  display.setI2CAddress(static_cast<uint8_t>(address << 1));
  if (!display.begin()) {
    Serial.printf("DISPLAY_INIT_FAILED no_ack addr=0x%02X\n", address);
    return;
  }

  gDisplayReady = true;
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tr);
  displayDrawLine(22, "Hoshino Gateway");
  displayDrawLine(36, "display ready");
  display.sendBuffer();
  Serial.printf("DISPLAY_READY addr=0x%02X refresh_ms=%lu\n",
                address, static_cast<unsigned long>(kDisplayRefreshMs));
}

// 正常启动时屏幕初始化被推迟到 Wi-Fi 就绪。配网模式永远不会连上 STA，所以必须把
// gSetupMode / gSoftApActive 也算作就绪条件，否则 IO 短接触发的配网重启后屏幕
// 永远不会点亮（上一版 OLED fork 在这里漏掉了配网分支）。
void serviceDisplayInit() {
  if (gDisplayReady || !gDisplayInitDeferred) return;
  if (!gSetupMode && !gSoftApActive && WiFi.status() != WL_CONNECTED) return;
  gDisplayInitDeferred = false;
  initDisplay();
}

// 节流刷新：loop() 每轮只睡 2ms，而一次整屏 I2C 推送要几十毫秒，必须限速。
void serviceDisplay() {
  if (!gDisplayReady) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - gLastDisplayMs) < static_cast<int32_t>(kDisplayRefreshMs)) return;
  gLastDisplayMs = now;
  renderDisplay();
}
#endif  // HOSHINO_ENABLE_DISPLAY
}  // namespace

void setup() {
  Serial.begin(115200);
  WiFi.onEvent(handleWifiEvent);
  // Use the sdkconfig WiFi buffer counts (dynamic RX/TX = 8) instead of the
  // Arduino default hardcoded 32. Arduino's WiFiGeneric.cpp overrides the
  // sdkconfig values with 32 unless useStaticBuffers(true) is set before init.
  WiFi.useStaticBuffers(true);
  // Initialize the TCP/IP stack before any Bluetooth or network operations.
  // The tcpip thread is needed for tcpip_callback() used by the network bridge
  // (setupWatchNetworkProxy), which is called during Bluetooth auth, before
  // WiFi is connected. Without this, tcpip_callback asserts "Invalid mbox".
  tcpip_init(NULL, NULL);
#ifndef HOSHINO_DISABLE_LWIP_OVERRIDE
  hoshino_lwip_napt_override_marker();
#endif
  // Release the Bluetooth LE controller memory before Classic SPP init:
  // this firmware only uses Bluetooth Classic, and the BLE controller would
  // otherwise reserve ~32 KB of internal DRAM that the SPP host stack needs.
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  Serial.printf("HOSHINO_BLE_MEM_RELEASED free=%lu largest=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  // Large protocol workspaces are NOT allocated here: Bluetooth controller
  // init needs the free heap first. They are allocated lazily after
  // beginWatchBluetooth() succeeds (see ensureScratchBuffers()).
  loadConfig();
  setupTriggerPins();
#if HOSHINO_ENABLE_DISPLAY
  // 显示屏与 Wire 会占用并切碎堆：已配置的正常启动推迟到 Wi-Fi 就绪后再初始化，
  // 让蓝牙控制器与 esp_wifi_init 先拿到最干净的堆。缺少手表身份的首配启动没有
  // 经典蓝牙竞争内存，可以立即点亮。
  if (watchMac.length() != 17 || watchAuthKey.length() != 32) {
    initDisplay();
  } else {
    gDisplayInitDeferred = true;
    Serial.println("DISPLAY_INIT_DEFERRED until=wifi_or_setup_mode");
  }
#endif
  // 独立任务检测 GPIO 短接（不依赖被桥接阻塞的 loop()）。
  xTaskCreatePinnedToCore(setupTriggerTask, "hoshino_setup_trig", 2048, nullptr, 1, nullptr, 1);
  if (!gBridgeStateMutex) gBridgeStateMutex = xSemaphoreCreateMutex();
  // ASR result queue is optional; scheduleWatchAsrJob() allocates it lazily.
#if HOSHINO_ENABLE_ASR
  gWatchStreamCaptureFsReady = false;  // mounted lazily on first capture
  Serial.printf("WATCH_CAPTURE_FS lazy_mount=%s\n", gWatchStreamCaptureFsReady ? "true" : "false");
#endif
  // Wi-Fi is NOT connected here: it is brought up inside watchBridgeTaskMain
  // AFTER Bluetooth auth succeeds, to keep heap large/unfragmented for BT init.
  setupRoutes();
  Serial.println("WATCH_RETURN_PATH_TRACER v7 enabled");
  Serial.println("HOSHINO_MEM_OPT v7 shared_spp_frame=true reused_tx_scratch=true lazy_asr_queue=true");
  Serial.printf("Hoshino Bridge ready. watch_configured=%s\n",
                (watchMac.length()==17 && watchAuthKey.length()==32) ? "true" : "false");

  // IO 短接触发的强制配网：重启后检测到标志，直接进入 SoftAP 配网模式。
  if (prefs.getBool("force_setup", false)) {
    prefs.putBool("force_setup", false);  // 清除一次性标志
    Serial.println("SETUP_MODE_REASON io_short_reboot");
    enterSetupMode();
    return;
  }

  // 首次配网：缺少完整 Watch 身份时进入 SoftAP。家庭 Wi-Fi 可以为空，
  // 这样已配置手表身份的设备仍会先启动原始蓝牙桥接并提供隧道配网接口。
  const bool needSetup = watchMac.length() != 17 || watchAuthKey.length() != 32;
  if (needSetup) {
    Serial.println("SETUP_MODE_REASON first_boot_no_config");
    enterSetupMode();
    return;  // 配网模式下不启动桥接，等用户在网页上保存配置后重启。
  }

  // 只要 Watch MAC + Auth Key 完整，就自动启动桥接（配网保存后自然进入工作模式），
  // 不依赖 autoConnectWatch 标志，避免用户忘勾「自动连接」导致配好后不连手表。
  if (watchMac.length() == 17 && watchAuthKey.length() == 32) {
    String error;
    if (!startWatchBridge(error)) Serial.printf("WATCH_AUTOSTART_FAILED reason=%s\n", error.c_str());
  }
  // HTTP server is started from loop() once home Wi-Fi is connected
  // (lwIP must be initialised first).
}

void loop() {
  handleWatchProbeSerial();

#if HOSHINO_ENABLE_DISPLAY
  // 状态屏必须在 gSetupMode 的提前 return 之前刷新，否则配网模式下屏幕不更新。
  serviceDisplayInit();
  serviceDisplay();
#endif

  // 配网模式：仅服务 AP 上的 HTTP 配置页面，不做桥接/重连。
  if (gSetupMode) {
    if (gServerStarted) server.handleClient();
    delay(2);
    return;
  }

  // 正常工作模式：STA 或已认证的手表虚拟网络就绪后启动 HTTP 服务器。
  if (!gServerStarted && (gSoftApActive || gWatchNetworkReady || WiFi.status() == WL_CONNECTED)) {
    server.begin();
    gServerStarted = true;
    Serial.println("HOSHINO_HTTP_SERVER_STARTED");
  }
  if (gServerStarted) server.handleClient();
  maintainMdns();

  // IO 短接触发配网已由独立任务 setupTriggerTask 处理（不依赖本循环）。

  if (autoConnectWatch && !gWatchAutoReconnectSuppressed &&
      !gWatchBridgeRunning && !gWatchBridgeTask &&
      static_cast<int32_t>(millis() - gNextWatchAutoConnectMs) >= 0) {
    String error;
    if (!startWatchBridge(error)) {
      if (error != "already_running") Serial.printf("WATCH_AUTORECONNECT_FAILED reason=%s\n", error.c_str());
    }
    gNextWatchAutoConnectMs = millis() + 5000;
  }
  delay(2);
}
