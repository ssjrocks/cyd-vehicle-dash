// OBD-Probe: ESP32 bridge between a Bluetooth ELM327-style OBD2 dongle and the LAN.
//
//  - Web UI + JSON API on port 80 (scan, connect, GATT dump, raw commands, log)
//  - Raw TCP bridge on port 35000 (same as WiFi ELM327 dongles: Torque, python-OBD, telnet)
//  - BLE dongles on every chip; Classic SPP dongles on chips that support it (original ESP32)
//  - Remembers the last dongle and reconnects automatically
//  - Firmware updates without USB: upload box in the web UI, or
//      curl http://obd-probe.local/api/prepare-update && curl -F fw=@obd_probe.ino.bin http://obd-probe.local/update
//    or ArduinoOTA (network port "obd-probe" in the Arduino IDE / arduino-cli upload -p obd-probe.local)

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include "sdkconfig.h"
#include <esp_system.h>
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF && defined(__XTENSA__)
#include <esp_core_dump.h>
#define HAVE_CRASH_SUMMARY 1
#else
#define HAVE_CRASH_SUMMARY 0
#endif
#if defined(CONFIG_BT_CLASSIC_ENABLED) && defined(CONFIG_BT_SPP_ENABLED)
#define HAVE_SPP 1
#include <BluetoothSerial.h>
#else
#define HAVE_SPP 0
#endif
#include "ui.h"
// Waveshare ESP32-S3-Touch-LCD-4.3: local touchscreen UI (lcd_ui.h). The only S3 board in use here.
#if CONFIG_IDF_TARGET_ESP32S3
#define HAS_LCD 1
#else
#define HAS_LCD 0
#endif

#define FW_VERSION "1.13.0"

// WiFi credentials and hotspot password live in secrets.h (git-ignored); copy secrets.example.h to start.
#include "secrets.h"
static const char *HOSTNAME = "obd-probe";
static const uint16_t BRIDGE_PORT = 35000;
// On-board LED, lit for as long as the firmware runs. The Lonely Binary LiPo board has no published pinout,
// so drive both common layouts: generic DevKit (GPIO2, active HIGH) and LOLIN32-Lite style (GPIO22, active LOW).
// Neither pin is used for anything else here.
struct LedPin {
  int pin;
  int onLevel;
};
#if HAVE_SPP
static const LedPin STATUS_LEDS[] = {{2, HIGH}, {22, LOW}};
#else
static const LedPin STATUS_LEDS[] = {{-1, 0}};
#endif

WebServer server(80);
WiFiServer bridgeServer(BRIDGE_PORT);
WiFiClient bridgeClient;
Preferences prefs;

// ---------------------------------------------------------------- freeze diagnostics
// The loop records what it is doing in RTC memory (survives a software reset). A watchdog task reboots the
// board if the loop stops for longer than any legitimate blocking call, and the next boot reports where it stuck.
static const uint32_t FREEZE_MAGIC = 0xF7EE2E01;
static const uint32_t FREEZE_LIMIT_MS = 150000;  // longest legit block: manual SPP connect ~90 s
RTC_NOINIT_ATTR static uint32_t rtcMagic;
RTC_NOINIT_ATTR static uint32_t rtcStuckMs;
RTC_NOINIT_ATTR static char rtcStage[64];
static volatile uint32_t loopBeat = 0;
static char stageNow[64] = "boot";
static uint32_t maxLoopMs = 0;
static char maxLoopStage[64] = "";
static String resetReasonStr, lastFreeze;

// Each call closes the previous stage, so the slowest stage is charged to the step that actually took the time.
static uint32_t stageT0 = 0;
// Takes a plain C string and never allocates: under memory pressure a String concatenation can fail and
// hand back a null buffer (that crashed 1.8.0 in strncpy).
void setStage(const char *st) {
  if (!st) st = "?";
  uint32_t now = millis();
  if (stageT0 && now - stageT0 > maxLoopMs) {
    maxLoopMs = now - stageT0;
    memcpy(maxLoopStage, stageNow, sizeof maxLoopStage);
  }
  stageT0 = now;
  strncpy(stageNow, st, sizeof stageNow - 1);
  stageNow[sizeof stageNow - 1] = 0;
  memcpy(rtcStage, stageNow, sizeof rtcStage);
}

void freezeWatchdog(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t stuck = millis() - loopBeat;
    if (loopBeat && stuck > FREEZE_LIMIT_MS) {
      rtcStuckMs = stuck;
      rtcMagic = FREEZE_MAGIC;
      Serial.printf("[e] main loop frozen %u ms in '%s' - rebooting\n", stuck, rtcStage);
      delay(100);
      esp_restart();
    }
  }
}

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on or reset button";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "CRASH (exception/panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply voltage dipped)";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default: return "other";
  }
}

// Summary of the last crash, read from the core dump flash partition (survives reboots until erased).
// Decode with: xtensa-esp32-elf-addr2line -pfiaC -e obd_probe-<ver>.elf <pc> <bt...>
String crashJson() {
#if HAVE_CRASH_SUMMARY
  if (esp_core_dump_image_check() != ESP_OK) return "{\"present\":false}";
  esp_core_dump_summary_t *cs = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
  if (!cs) return "{\"present\":true,\"error\":\"no memory\"}";
  String o;
  if (esp_core_dump_get_summary(cs) == ESP_OK) {
    char b[16];
    o = "{\"present\":true,\"task\":\"" + String(cs->exc_task) + "\"";
    snprintf(b, sizeof b, "0x%08x", cs->exc_pc);
    o += ",\"pc\":\"" + String(b) + "\",\"cause\":" + String(cs->ex_info.exc_cause);
    snprintf(b, sizeof b, "0x%08x", cs->ex_info.exc_vaddr);
    o += ",\"vaddr\":\"" + String(b) + "\",\"corrupted\":" + String(cs->exc_bt_info.corrupted ? "true" : "false") + ",\"bt\":[";
    for (uint32_t i = 0; i < cs->exc_bt_info.depth && i < 16; i++) {
      snprintf(b, sizeof b, "0x%08x", cs->exc_bt_info.bt[i]);
      o += String(i ? "," : "") + "\"" + b + "\"";
    }
    o += "],\"elfSha\":\"" + String((const char *)cs->app_elf_sha256).substring(0, 16) + "\"}";
  } else o = "{\"present\":true,\"error\":\"summary unreadable\"}";
  free(cs);
  return o;
#else
  return "{\"present\":false,\"unsupported\":true}";
#endif
}

// ---------------------------------------------------------------- log ring

struct LogEntry {
  uint32_t seq;
  uint32_t ms;
  char dir;  // 't' = to dongle, 'r' = from dongle, 'i' = info, 'e' = error
  String text;
};
static const int LOG_N = 40;  // bounded: with Classic BT up only ~20 KB of heap is left
static LogEntry logBuf[LOG_N];
static uint32_t logSeq = 0;
static SemaphoreHandle_t logMux;

// opMux: exclusive use of the dongle (commands, scans, connects). Web, touchscreen, TCP bridge and the
//        auto-reconnect task each take it, so their commands never interleave on the wire.
// connMux: guards the dongle description strings (devAddr, devName, layoutLabel, lastConnectError, gattJson),
//        which only connect/disconnect write. Readers try it briefly and fall back to placeholders.
static SemaphoreHandle_t opMux, connMux;
struct RLock {
  SemaphoreHandle_t m;
  bool ok;
  RLock(SemaphoreHandle_t mux, uint32_t ms) : m(mux) {
    ok = xSemaphoreTakeRecursive(m, ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(ms)) == pdTRUE;
  }
  ~RLock() {
    if (ok) xSemaphoreGiveRecursive(m);
  }
};

void addLog(char dir, const String &text) {
  xSemaphoreTake(logMux, portMAX_DELAY);
  uint32_t seq = ++logSeq;
  LogEntry &e = logBuf[seq % LOG_N];  // read side indexes by seq too
  e.seq = seq;
  e.ms = millis();
  e.dir = dir;
  e.text = text.length() > 120 ? text.substring(0, 120) + "..." : text;
  xSemaphoreGive(logMux);
  Serial.printf("[%c] %s\n", dir, text.c_str());
}

// ---------------------------------------------------------------- rx buffer (filled from BT task)

static String rxBuf;
static SemaphoreHandle_t rxMux;
static const size_t RX_MAX = 2048;  // longest ELM327 reply (multi-frame VIN) is a few hundred bytes

void rxAppend(const uint8_t *d, size_t n) {
  xSemaphoreTake(rxMux, portMAX_DELAY);
  rxBuf.concat((const char *)d, n);
  if (rxBuf.length() > RX_MAX) rxBuf.remove(0, rxBuf.length() - RX_MAX);
  xSemaphoreGive(rxMux);
}

String rxTake() {
  xSemaphoreTake(rxMux, portMAX_DELAY);
  String s = rxBuf;
  rxBuf = "";
  xSemaphoreGive(rxMux);
  return s;
}

bool rxHasPrompt() {
  xSemaphoreTake(rxMux, portMAX_DELAY);
  bool p = rxBuf.indexOf('>') >= 0;
  xSemaphoreGive(rxMux);
  return p;
}

// ---------------------------------------------------------------- json helpers

String jsonEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\r': o += "\\r"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20 || c >= 0x7f) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else o += (char)c;
    }
  }
  return o;
}

String q(const String &s) { return "\"" + jsonEsc(s) + "\""; }

String toHex(const String &s) {
  String o;
  char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    snprintf(b, sizeof b, "%02X", (uint8_t)s[i]);
    o += b;
  }
  return o;
}

// "0000fff0-0000-1000-8000-00805f9b34fb" -> "fff0"
String shortUuid(String u) {
  u.toLowerCase();
  if (u.length() == 36 && u.startsWith("0000") && u.endsWith("-0000-1000-8000-00805f9b34fb")) return u.substring(4, 8);
  return u;
}

// ---------------------------------------------------------------- dongle state

enum Transport { T_NONE, T_BLE, T_SPP };
// Set by /api/prepare-update: Bluetooth is shut down and must not be touched again until the reboot.
static volatile bool updatePrepared = false;
static uint32_t preparedAt = 0;
static Transport transport = T_NONE;
static volatile bool linkUp = false;
static String devAddr, devName;
static uint8_t devAddrType = 0;
static String lastConnectError;
static String gattJson = "[]";
static uint32_t connectedAt = 0;
// Auto-reconnect runs in its own task so a sleeping dongle never blocks the web server / OTA / bridge.
// While it runs it owns the dongle state (devAddr, lastConnectError, layoutLabel, gattJson): the HTTP side
// must not read or write those until bgConnecting drops.
static volatile bool bgConnecting = false;
static uint32_t nextAutoTry = 0;
static uint32_t autoBackoff = 20000;
static bool autoReconnect = true;
static uint32_t txBytes = 0, rxBytes = 0;
static uint32_t notifyCount = 0;

static BLEClient *bleClient = nullptr;
static BLERemoteCharacteristic *bleRx = nullptr, *bleTx = nullptr;
static bool bleTxResponse = false;
static uint16_t bleChunk = 20;
static bool bleInited = false;
// Classic and BLE together leave too little heap for WiFi on the ESP32, so only one stack runs per boot.
static String btMode = "ble";

#if HAVE_SPP
BluetoothSerial SerialBT;
// Every SerialBT call must check this: after SerialBT.end() the library's event groups are gone and any
// call (connected(), connect(), ...) asserts. That crashed 1.9.0 right after prepare-update.
static volatile bool sppStarted = false;
#endif

String printableChunk(const uint8_t *d, size_t n) {
  String s;
  s.concat((const char *)d, n);
  return s;
}

void bleNotify(BLERemoteCharacteristic *c, uint8_t *d, size_t n, bool isNotify) {
  notifyCount++;
  rxBytes += n;
  rxAppend(d, n);
  addLog('r', printableChunk(d, n));
}

class ClientCB : public BLEClientCallbacks {
  void onConnect(BLEClient *c) override {}
  void onDisconnect(BLEClient *c) override {
    if (transport == T_BLE) {
      linkUp = false;
      addLog('e', "BLE link dropped");
    }
  }
};

void bleInit() {
  if (bleInited) return;
#if HAVE_SPP
  btStartMode(BT_MODE_BLE);
#endif
  BLEDevice::init("obd-probe");
  bleInited = true;
}

void dongleWrite(const uint8_t *d, size_t n) {
  if (!linkUp) return;
  txBytes += n;
  if (transport == T_BLE && bleTx) {
    for (size_t off = 0; off < n; off += bleChunk) {
      size_t len = min((size_t)bleChunk, n - off);
      bleTx->writeValue((uint8_t *)d + off, len, bleTxResponse);
    }
  }
#if HAVE_SPP
  else if (transport == T_SPP && sppStarted) {
    SerialBT.write(d, n);
  }
#endif
}

void pumpSpp() {
#if HAVE_SPP
  if (transport != T_SPP || !sppStarted) return;
  if (!SerialBT.connected()) {
    if (linkUp) {
      linkUp = false;
      addLog('e', "SPP link dropped");
    }
    return;
  }
  uint8_t b[256];
  size_t n = 0;
  while (SerialBT.available() && n < sizeof b) b[n++] = SerialBT.read();
  if (n) {
    rxBytes += n;
    rxAppend(b, n);
    addLog('r', printableChunk(b, n));
  }
#endif
}

void disconnectDongle() {
  RLock cl(connMux, UINT32_MAX);
  if (transport == T_BLE && bleClient && bleClient->isConnected()) bleClient->disconnect();
#if HAVE_SPP
  if (transport == T_SPP && sppStarted) SerialBT.disconnect();
#endif
  linkUp = false;
  bleRx = bleTx = nullptr;
}

// Known BLE serial layouts used by ELM327 clones and branded adapters: service, notify(rx), write(tx)
struct KnownLayout {
  const char *svc, *rx, *tx, *label;
};
static const KnownLayout KNOWN[] = {
  {"fff0", "fff1", "fff2", "FFF0 (common Chinese ELM327 BLE)"},
  {"ffe0", "ffe1", "ffe1", "FFE0 (HM-10 style UART, IOS-Vlink)"},
  {"18f0", "2af0", "2af1", "18F0 (Vgate / vLinker)"},
  {"e7810a71-73ae-499d-8c15-faa9aef0c3f2", "bef8d6c9-9c21-4c9e-b632-bd58c1009f9f", "bef8d6c9-9c21-4c9e-b632-bd58c1009f9f", "OBDLink / STN BLE"},
  {"6e400001-b5a3-f393-e0a9-e50e24dcca9e", "6e400003-b5a3-f393-e0a9-e50e24dcca9e", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART"},
};
static String layoutLabel;

String propsStr(BLERemoteCharacteristic *c) {
  String p;
  if (c->canRead()) p += "R ";
  if (c->canWrite()) p += "W ";
  if (c->canWriteNoResponse()) p += "WNR ";
  if (c->canNotify()) p += "N ";
  if (c->canIndicate()) p += "I ";
  p.trim();
  return p;
}

bool connectBle(const String &addr, uint8_t atype, const String &rxHint, const String &txHint) {
  RLock cl(connMux, UINT32_MAX);
  if (updatePrepared) {
    lastConnectError = "firmware update in progress";
    return false;
  }
  bleInit();
  disconnectDongle();
  if (!bleClient) {
    bleClient = BLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCB());
  }
  addLog('i', "BLE connecting to " + addr + " (addr type " + String(atype) + ")");
  BLEAddress a(addr, atype);
  if (!bleClient->connect(a, atype, 10000)) {
    lastConnectError = "BLE connect failed (out of range, already connected to a phone, or wrong address type)";
    addLog('e', lastConnectError);
    return false;
  }
  transport = T_BLE;
  devAddr = addr;
  devAddrType = atype;
  if (bleClient->setMTU(185)) addLog('i', "MTU negotiated: " + String(bleClient->getMTU()));
  uint16_t mtu = bleClient->getMTU();
  bleChunk = (mtu > 23) ? min<uint16_t>(mtu - 3, 180) : 20;

  // Walk the whole GATT table so the UI can show exactly how this adapter is built.
  String g = "[";
  BLERemoteCharacteristic *firstN = nullptr, *firstW = nullptr;
  BLERemoteCharacteristic *knownRx = nullptr, *knownTx = nullptr;
  BLERemoteCharacteristic *hintRx = nullptr, *hintTx = nullptr;
  layoutLabel = "";
  auto *svcs = bleClient->getServices();
  bool firstSvc = true;
  for (auto &sp : *svcs) {
    BLERemoteService *s = sp.second;
    String su = shortUuid(s->getUUID().toString());
    if (!firstSvc) g += ",";
    firstSvc = false;
    g += "{\"uuid\":" + q(su) + ",\"chars\":[";
    auto *chars = s->getCharacteristics();
    bool firstC = true;
    for (auto &cp : *chars) {
      BLERemoteCharacteristic *c = cp.second;
      String cu = shortUuid(c->getUUID().toString());
      String val;
      bool std = (su == "1800" || su == "180a");
      if (std && c->canRead()) val = c->readValue();
      if (!firstC) g += ",";
      firstC = false;
      g += "{\"uuid\":" + q(cu) + ",\"handle\":" + String(c->getHandle()) + ",\"props\":" + q(propsStr(c));
      if (val.length()) g += ",\"value\":" + q(val) + ",\"hex\":" + q(toHex(val));
      g += "}";
      bool isStd = (su == "1800" || su == "1801" || su == "180a");
      if (!isStd) {
        if (!firstN && (c->canNotify() || c->canIndicate())) firstN = c;
        if (!firstW && (c->canWrite() || c->canWriteNoResponse())) firstW = c;
      }
      if (rxHint.length() && cu == rxHint) hintRx = c;
      if (txHint.length() && cu == txHint) hintTx = c;
      for (auto &k : KNOWN) {
        if (su == k.svc && cu == k.rx && (c->canNotify() || c->canIndicate())) { knownRx = c; layoutLabel = k.label; }
        if (su == k.svc && cu == k.tx && (c->canWrite() || c->canWriteNoResponse())) knownTx = c;
      }
    }
    g += "]}";
  }
  g += "]";
  gattJson = g;

  bleRx = hintRx ? hintRx : (knownRx ? knownRx : firstN);
  bleTx = hintTx ? hintTx : (knownTx ? knownTx : firstW);
  if (!layoutLabel.length()) layoutLabel = (hintRx || hintTx) ? "manual" : "guessed (first notify / first write)";
  if (!bleRx || !bleTx) {
    lastConnectError = "Connected, but no notify+write characteristic pair found - see GATT dump";
    addLog('e', lastConnectError);
    linkUp = false;
    return false;
  }
  bleTxResponse = !bleTx->canWriteNoResponse();
  bleRx->registerForNotify(bleNotify, bleRx->canNotify());
  addLog('i', "Using RX " + shortUuid(bleRx->getUUID().toString()) + " / TX " + shortUuid(bleTx->getUUID().toString()) + " [" + layoutLabel + "], write " + (bleTxResponse ? "with" : "without") + " response, chunk " + String(bleChunk));
  rxTake();
  linkUp = true;
  connectedAt = millis();
  lastConnectError = "";
  return true;
}

#if HAVE_SPP
// Bluetooth-task callbacks: every step of the SPP handshake goes into the web log, so a failed
// connect says *where* it failed (service lookup, pairing/PIN, or the RFCOMM channel itself).
static volatile int sppAuthResult = -1;  // -1 = no pairing happened, 0 = failed, 1 = ok
static volatile int sppLastChannels = -1;
static volatile int sppLastScn = 0;

void sppEvent(esp_spp_cb_event_t ev, esp_spp_cb_param_t *p) {
  switch (ev) {
    case ESP_SPP_DISCOVERY_COMP_EVT: {
      sppLastChannels = p->disc_comp.status == ESP_SPP_SUCCESS ? p->disc_comp.scn_num : 0;
      if (sppLastChannels > 0) sppLastScn = p->disc_comp.scn[0];
      String m = "SDP service lookup: status " + String(p->disc_comp.status) + ", " + String(p->disc_comp.scn_num) + " SPP channel(s)";
      for (int i = 0; i < p->disc_comp.scn_num; i++) {
        const char *n = p->disc_comp.service_name[i];
        m += String(i ? ", " : ": ") + "ch " + String(p->disc_comp.scn[i]) + (n && *n ? String(" \"") + n + "\"" : String(""));
      }
      addLog(p->disc_comp.status == ESP_SPP_SUCCESS ? 'i' : 'e', m);
      break;
    }
    case ESP_SPP_CL_INIT_EVT:
      addLog('i', "RFCOMM connect started: status " + String(p->cl_init.status) + (p->cl_init.sec_id ? ", security id " + String(p->cl_init.sec_id) : String("")));
      break;
    case ESP_SPP_OPEN_EVT:
      addLog(p->open.status == ESP_SPP_SUCCESS ? 'i' : 'e', "RFCOMM channel open: status " + String(p->open.status));
      break;
    case ESP_SPP_CLOSE_EVT:
      addLog('e', "RFCOMM closed: status " + String(p->close.status) + ", port status " + String(p->close.port_status) + (p->close.async ? " (closed by remote/link loss)" : " (closed locally)"));
      break;
    default: break;
  }
}

void sppAuth(bool ok) {
  sppAuthResult = ok ? 1 : 0;
  addLog(ok ? 'i' : 'e', ok ? "Pairing succeeded" : "Pairing FAILED (wrong PIN, or dongle refused - is a phone connected to it?)");
}

bool sppTry(const String &addr, int channel, esp_spp_sec_t sec, const char *label) {
  addLog('i', String("SPP attempt: ") + label);
  return SerialBT.connect(BTAddress(addr), channel, sec);
}

// quickChannel > 0: single authenticated attempt on that RFCOMM channel (auto-reconnect, ~10 s worst case).
// Otherwise the full discovery sequence with fallbacks (manual connect, up to ~90 s).
bool startSpp() {
  if (sppStarted) return true;
  if (updatePrepared) return false;
  SerialBT.register_callback(sppEvent);
  SerialBT.onAuthComplete(sppAuth);
  if (!SerialBT.begin("obd-probe", true, true)) {
    addLog('e', "Classic Bluetooth failed to start");
    return false;
  }
  sppStarted = true;
  return true;
}

bool connectSpp(const String &addr, const String &pin, int quickChannel = 0) {
  RLock cl(connMux, UINT32_MAX);
  disconnectDongle();
  if (!startSpp()) {
    lastConnectError = updatePrepared ? "firmware update in progress" : "Classic Bluetooth failed to start";
    return false;
  }
  transport = T_SPP;
  const esp_spp_sec_t authEnc = (esp_spp_sec_t)(ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT);
  bool ok = false;
  int chUsed = 0;
  if (quickChannel > 0) {
    String p = pin.length() ? pin : String("1234");
    SerialBT.setPin(p.c_str(), p.length());
    sppAuthResult = -1;
    sppLastChannels = -1;
    ok = sppTry(addr, quickChannel, authEnc, ("saved channel " + String(quickChannel) + ", PIN " + p).c_str());
    chUsed = quickChannel;
  } else {
    // Cheap ELM327 clones use legacy pairing; 1234 is usual, 0000 and 6789 turn up too.
    String pins[3] = {pin.length() ? pin : String("1234"), "0000", "6789"};
    int npins = (pins[0] == "1234") ? 3 : 1;
    for (int i = 0; i < npins && !ok; i++) {
      SerialBT.setPin(pins[i].c_str(), pins[i].length());
      sppAuthResult = -1;
      sppLastChannels = -1;
      sppLastScn = 0;
      addLog('i', "SPP connecting to " + addr + " with PIN " + pins[i]);
      ok = sppTry(addr, 0, authEnc, "service lookup (SDP), authenticated");
      if (ok) chUsed = sppLastScn;
      if (!ok && sppAuthResult == 0) continue;  // PIN rejected: next PIN
      if (!ok && (ok = sppTry(addr, 1, authEnc, "channel 1 directly, authenticated"))) chUsed = 1;
      if (!ok && sppAuthResult == 0) continue;
      if (!ok && (ok = sppTry(addr, 1, ESP_SPP_SEC_NONE, "channel 1 directly, no security"))) chUsed = 1;
      if (!ok && sppAuthResult != 0) break;  // not a PIN problem, other PINs won't help
      if (ok) prefs.putString("x1", pins[i]);
    }
  }
  if (!ok) {
    lastConnectError = sppAuthResult == 0 ? "SPP pairing rejected" :
                       sppAuthResult == -1 && sppLastChannels <= 0 ? "SPP: dongle never answered (out of range, busy with a phone, or asleep)" :
                       "SPP connect failed - see log for the step that failed";
    addLog('e', lastConnectError);
    return false;
  }
  if (chUsed > 0) prefs.putInt("ch", chUsed);
  devAddr = addr;
  gattJson = "[]";
  layoutLabel = "Classic SPP (RFCOMM)";
  rxTake();
  linkUp = true;
  connectedAt = millis();
  lastConnectError = "";
  addLog('i', "SPP connected");
  return true;
}
#endif

// Frees the Bluetooth stack's heap (~90 KB on the ESP32 in Classic mode) so an update never runs starved.
// Any update attempt ends in a reboot, which brings Bluetooth back.
static bool rebootPending = false;
void stopBluetoothForUpdate() {
  RLock ol(opMux, 20000);  // let a running command finish first
  disconnectDongle();
#if HAVE_SPP
  if (sppStarted) {
    sppStarted = false;  // first, so nothing calls into SerialBT while it is being torn down
    SerialBT.end();
  }
  transport = T_NONE;
#endif
  if (bleInited) {
    BLEDevice::deinit(false);
    bleInited = false;
    bleClient = nullptr;
  }
  addLog('i', "Bluetooth stopped for update, free heap " + String(ESP.getFreeHeap() / 1024) + " KB");
}

void saveDevice(const String &type, const String &addr, uint8_t atype, const String &name, const String &extra1, const String &extra2) {
  prefs.putString("btmode", type);
  prefs.putString("type", type);
  prefs.putString("addr", addr);
  prefs.putUChar("atype", atype);
  prefs.putString("name", name);
  prefs.putString("x1", extra1);
  prefs.putString("x2", extra2);
}

// Auto-reconnect first listens for the saved dongle (passive scan) and only connects when it is advertising:
// a blind connect to an absent or sleeping dongle ties up the radio for NimBLE's fixed 30 s timeout.
bool bleAdvertising(const String &addr, uint32_t secs) {
  bleInit();
  BLEScan *sc = BLEDevice::getScan();
  sc->setActiveScan(false);
  sc->setInterval(100);
  sc->setWindow(99);
  BLEScanResults *r = sc->start(secs, false);
  bool seen = false;
  for (int i = 0; i < r->getCount() && !seen; i++) seen = r->getDevice(i).getAddress().toString().equalsIgnoreCase(addr);
  sc->clearResults();
  return seen;
}

bool connectSaved() {
  String type = prefs.getString("type", "");
  String addr = prefs.getString("addr", "");
  if (!type.length() || !addr.length()) return false;
  devName = prefs.getString("name", "");
  if (HAVE_SPP && type != btMode) return false;
  if (type == "ble") {
    if (!bleAdvertising(addr, 3)) {
      RLock cl(connMux, UINT32_MAX);
      lastConnectError = "Dongle not advertising (ignition off, asleep or out of range)";
      return false;
    }
    addLog('i', "Saved dongle is advertising - connecting");
    return connectBle(addr, prefs.getUChar("atype", 0), prefs.getString("x1", ""), prefs.getString("x2", ""));
  }
#if HAVE_SPP
  if (type == "spp") return connectSpp(addr, prefs.getString("x1", "1234"), prefs.getInt("ch", 0));
#endif
  return false;
}

// Created once at boot (while memory is plentiful) and woken per attempt: creating a fresh task each time
// needed an 8 KB stack from a heap whose largest free block is ~10 KB in Classic mode.
static TaskHandle_t autoTaskHandle = nullptr;
// WiFi and Bluetooth share one radio: while Classic BT pages a dongle that isn't answering, WiFi gets almost
// no airtime and the web server goes quiet for ~20 s. So auto-reconnect waits while someone is using the page
// (the page has a "Reconnect now" button instead).
static const uint32_t WEB_QUIET_MS = 20000;
static volatile uint32_t lastWebActivity = 0;

bool autoPausedForWeb() { return lastWebActivity && millis() - lastWebActivity < WEB_QUIET_MS; }

void triggerReconnect() {
  bgConnecting = true;
  xTaskNotifyGive(autoTaskHandle);
}

void autoConnectTask(void *) {
  String lastOutcome;  // only log when the outcome changes, so a parked car doesn't flood the log
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool ok = false, tried = false;
    {
      RLock ol(opMux, 0);
      if (ol.ok) {
        tried = true;
        ok = connectSaved();
      }
    }
    autoBackoff = ok ? 20000 : min<uint32_t>(autoBackoff * 2, HAS_LCD ? 10000 : 300000);  // a dash must reconnect quickly (a miss costs a 3 s scan)
    nextAutoTry = millis() + autoBackoff;
    String outcome;
    if (tried && !ok) {
      RLock cl(connMux, UINT32_MAX);
      outcome = lastConnectError;
    }
    if (outcome.length() && outcome != lastOutcome) addLog('i', "Auto-reconnect: " + outcome + " - retrying every " + String(autoBackoff / 1000) + " s");
    if (tried) lastOutcome = outcome;
    __sync_synchronize();
    bgConnecting = false;
  }
}

void maybeAutoReconnect() {
  if (!autoReconnect || linkUp || bgConnecting || (int32_t)(millis() - nextAutoTry) < 0) return;
  if (!prefs.getString("addr", "").length()) return;
  if (bridgeClient && bridgeClient.connected()) return;
  if (!autoTaskHandle || updatePrepared || autoPausedForWeb()) return;
  triggerReconnect();
}

void sendJson(int code, const String &body);

bool rejectIfReconnecting() {
  if (!bgConnecting) return false;
  sendJson(409, "{\"error\":\"auto-reconnect in progress (max ~10 s) - try again shortly\",\"reconnecting\":true}");
  return true;
}

// ---------------------------------------------------------------- command engine

String runCmd(const String &cmd, uint32_t timeout, bool &timedOut, uint32_t &elapsed) {
  rxTake();
  String line = cmd + "\r";
  addLog('t', line);
  uint32_t t0 = millis();
  dongleWrite((const uint8_t *)line.c_str(), line.length());
  timedOut = true;
  while (millis() - t0 < timeout && linkUp) {
    pumpSpp();
    if (rxHasPrompt()) {
      timedOut = false;
      break;
    }
    delay(2);
  }
  delay(timedOut ? 0 : 5);
  pumpSpp();
  elapsed = millis() - t0;
  return rxTake();
}

// ---------------------------------------------------------------- HTTP API

void sendJson(int code, const String &body);

// Returns true (after replying) when the request needs the other radio stack: saves the mode and reboots into it.
bool needModeSwitch(const String &type) {
  if (!HAVE_SPP || type == btMode || (type != "ble" && type != "spp")) return false;
  addLog('i', "Switching radio to " + type + " mode - rebooting");
  prefs.putString("btonce", type);  // this boot only; afterwards the saved dongle's type wins again
  sendJson(503, "{\"switching\":true,\"mode\":" + q(type) + "}");
  delay(400);
  ESP.restart();
  return true;
}

// Distinct HTTP clients seen recently (to tell "phone never reaches us" apart from "we don't answer").
struct SeenClient {
  uint32_t ip;
  uint32_t lastMs;
  uint32_t hits;
};
static SeenClient seenClients[8];

void noteClient() {
  uint32_t ip = (uint32_t)server.client().remoteIP();
  int slot = 0;
  for (int i = 0; i < 8; i++) {
    if (seenClients[i].ip == ip) {
      seenClients[i].lastMs = millis();
      seenClients[i].hits++;
      return;
    }
    if (seenClients[i].lastMs < seenClients[slot].lastMs) slot = i;
  }
  seenClients[slot] = {ip, millis(), 1};
  addLog('i', "New HTTP client " + IPAddress(ip).toString() + " requested " + server.uri());
}

void sendJson(int code, const String &body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void apiStatus() {
  noteClient();
  String s = "{";
  s += "\"fw\":" + q(FW_VERSION);
  s += ",\"chip\":" + q(ESP.getChipModel());
  s += ",\"spp\":" + String(HAVE_SPP ? "true" : "false");
  s += ",\"btmode\":" + q(btMode);
  s += ",\"maxBlock\":" + String(ESP.getMaxAllocHeap());
  s += ",\"uptime\":" + String(millis() / 1000);
  s += ",\"heap\":" + String(ESP.getFreeHeap());
  s += ",\"wifi\":{\"ssid\":" + q(WiFi.SSID()) + ",\"ip\":" + q(WiFi.localIP().toString()) + ",\"rssi\":" + String(WiFi.RSSI()) + ",\"ap\":" + String((WiFi.getMode() & WIFI_AP) ? "true" : "false") + "}";
  s += ",\"transport\":" + q(transport == T_BLE ? "ble" : transport == T_SPP ? "spp" : "none");
  s += ",\"connected\":" + String(linkUp ? "true" : "false");
  s += ",\"reconnecting\":" + String(bgConnecting ? "true" : "false");
  s += ",\"autoPaused\":" + String(autoReconnect && !linkUp && autoPausedForWeb() ? "true" : "false");
  s += ",\"nextRetry\":" + String(linkUp || !autoReconnect ? 0 : max<int32_t>(0, (int32_t)(nextAutoTry - millis()) / 1000));
  // The background reconnect task owns these strings while it runs.
  RLock cl(connMux, 30);
  bool own = cl.ok;
  s += ",\"device\":{\"addr\":" + q(own ? devAddr : String("")) + ",\"name\":" + q(own ? devName : String("")) + ",\"atype\":" + String(devAddrType) + "}";
  s += ",\"layout\":" + q(own ? layoutLabel : String(""));
  if (own && transport == T_BLE && bleRx && bleTx) {
    s += ",\"rx\":" + q(shortUuid(bleRx->getUUID().toString())) + ",\"tx\":" + q(shortUuid(bleTx->getUUID().toString()));
    s += ",\"mtu\":" + String(bleClient ? bleClient->getMTU() : 0) + ",\"chunk\":" + String(bleChunk) + ",\"writeResp\":" + String(bleTxResponse ? "true" : "false");
  }
  s += ",\"connectedFor\":" + String(linkUp ? (millis() - connectedAt) / 1000 : 0);
  s += ",\"lastError\":" + q(own ? lastConnectError : String(""));
  s += ",\"bridge\":" + String(bridgeClient && bridgeClient.connected() ? q(bridgeClient.remoteIP().toString()) : String("null"));
  s += ",\"autoReconnect\":" + String(autoReconnect ? "true" : "false");
  s += ",\"saved\":{\"type\":" + q(prefs.getString("type", "")) + ",\"addr\":" + q(prefs.getString("addr", "")) + ",\"name\":" + q(prefs.getString("name", "")) + "}";
  s += ",\"bytes\":{\"tx\":" + String(txBytes) + ",\"rx\":" + String(rxBytes) + ",\"notifies\":" + String(notifyCount) + "}";
  s += ",\"resetReason\":" + q(resetReasonStr) + ",\"lastFreeze\":" + q(lastFreeze);
  s += ",\"minHeap\":" + String(ESP.getMinFreeHeap()) + ",\"maxLoopMs\":" + String(maxLoopMs) + ",\"maxLoopStage\":" + q(maxLoopStage);
  s += ",\"clients\":[";
  bool firstC = true;
  for (auto &c : seenClients) {
    if (!c.hits) continue;
    if (!firstC) s += ",";
    firstC = false;
    s += "{\"ip\":" + q(IPAddress(c.ip).toString()) + ",\"agoS\":" + String((millis() - c.lastMs) / 1000) + ",\"hits\":" + String(c.hits) + "}";
  }
  s += "]";
  s += ",\"logSeq\":" + String(logSeq);
  s += "}";
  sendJson(200, s);
}

void apiScan() {
  String type = server.hasArg("type") ? server.arg("type") : "ble";
  int secs = server.hasArg("secs") ? constrain(server.arg("secs").toInt(), 1, 30) : 6;
  if (rejectIfReconnecting()) return;
  if (needModeSwitch(type)) return;
  RLock ol(opMux, 15000);
  if (!ol.ok) {
    sendJson(409, "{\"error\":\"dongle busy\"}");
    return;
  }
  String o = "[";
  if (type == "ble") {
    if (updatePrepared) {
      sendJson(503, "{\"error\":\"update in progress\"}");
      return;
    }
    bleInit();
    addLog('i', "BLE scan " + String(secs) + "s");
    BLEScan *sc = BLEDevice::getScan();
    sc->setActiveScan(true);
    sc->setInterval(100);
    sc->setWindow(99);
    BLEScanResults *r = sc->start(secs, false);
    int n = r->getCount();
    for (int i = 0; i < n; i++) {
      BLEAdvertisedDevice d = r->getDevice(i);
      if (i) o += ",";
      o += "{\"addr\":" + q(d.getAddress().toString()) + ",\"atype\":" + String(d.getAddressType());
      o += ",\"name\":" + q(d.haveName() ? d.getName() : String("")) + ",\"rssi\":" + String(d.getRSSI());
      o += ",\"connectable\":" + String(d.isConnectable() ? "true" : "false");
      o += ",\"svcs\":[";
      for (int k = 0; k < d.getServiceUUIDCount(); k++) {
        if (k) o += ",";
        o += q(shortUuid(d.getServiceUUID(k).toString()));
      }
      o += "]";
      if (d.haveManufacturerData()) o += ",\"mfg\":" + q(toHex(d.getManufacturerData()));
      o += "}";
    }
    sc->clearResults();
    addLog('i', "BLE scan found " + String(n) + " devices");
  }
#if HAVE_SPP
  else if (type == "spp") {
    if (!startSpp()) {
      sendJson(503, "{\"error\":\"Classic Bluetooth not available (update in progress?)\"}");
      return;
    }
    addLog('i', "Classic BT inquiry " + String(secs) + "s");
    BTScanResults *r = SerialBT.discover(secs * 1000);
    int n = r ? r->getCount() : 0;
    for (int i = 0; i < n; i++) {
      BTAdvertisedDevice *d = r->getDevice(i);
      if (i) o += ",";
      o += "{\"addr\":" + q(d->getAddress().toString()) + ",\"name\":" + q(String(d->getName().c_str())) + ",\"rssi\":" + String(d->getRSSI()) + ",\"cod\":" + String(d->getCOD()) + "}";
    }
    addLog('i', "Classic inquiry found " + String(n) + " devices");
  }
#endif
  else {
    sendJson(400, "{\"error\":\"scan type not supported on this chip\"}");
    return;
  }
  o += "]";
  sendJson(200, o);
}

void apiConnect() {
  String type = server.hasArg("type") ? server.arg("type") : "ble";
  String addr = server.arg("addr");
  addr.toLowerCase();
  if (addr.length() != 17) {
    sendJson(400, "{\"error\":\"addr must be aa:bb:cc:dd:ee:ff\"}");
    return;
  }
  if (rejectIfReconnecting()) return;
  if (needModeSwitch(type)) return;
  RLock ol(opMux, 15000);
  if (!ol.ok) {
    sendJson(409, "{\"error\":\"dongle busy (touchscreen or bridge)\"}");
    return;
  }
  {
    RLock cl(connMux, UINT32_MAX);
    devName = server.arg("name");
  }
  bool ok = false;
  if (type == "ble") {
    uint8_t at = server.hasArg("atype") ? server.arg("atype").toInt() : 0;
    String rx = server.arg("rx"), tx = server.arg("tx");
    rx.toLowerCase();
    tx.toLowerCase();
    ok = connectBle(addr, at, rx, tx);
    if (ok) saveDevice("ble", addr, at, devName, rx, tx);
  }
#if HAVE_SPP
  else if (type == "spp") {
    String pin = server.hasArg("pin") ? server.arg("pin") : "1234";
    ok = connectSpp(addr, pin);
    if (ok) saveDevice("spp", addr, 0, devName, pin, "");
  }
#endif
  RLock cl(connMux, UINT32_MAX);
  String s = "{\"ok\":" + String(ok ? "true" : "false") + ",\"error\":" + q(lastConnectError) + ",\"gatt\":" + gattJson + "}";
  sendJson(ok ? 200 : 502, s);
}

void apiCmd() {
  if (rejectIfReconnecting()) return;
  if (!linkUp) {
    sendJson(409, "{\"error\":\"dongle not connected\"}");
    return;
  }
  if (bridgeClient && bridgeClient.connected()) {
    sendJson(409, "{\"error\":\"TCP bridge client is using the dongle\"}");
    return;
  }
  RLock ol(opMux, 15000);
  if (!ol.ok) {
    sendJson(409, "{\"error\":\"dongle busy (touchscreen is running commands)\"}");
    return;
  }
  String c = server.arg("c");
  uint32_t t = server.hasArg("t") ? server.arg("t").toInt() : 5000;
  bool to;
  uint32_t el;
  String r = runCmd(c, constrain(t, 100, 60000), to, el);
  sendJson(200, "{\"cmd\":" + q(c) + ",\"raw\":" + q(r) + ",\"ms\":" + String(el) + ",\"timeout\":" + String(to ? "true" : "false") + "}");
}

// Streamed in small chunks: with the Classic BT stack up the largest free heap block is only ~15 KB,
// so a full log built as one String would not fit. The mutex is held per chunk, never across network sends.
void apiLog() {
  uint32_t since = server.hasArg("since") ? server.arg("since").toInt() : 0;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  xSemaphoreTake(logMux, portMAX_DELAY);
  uint32_t s = logSeq > LOG_N ? logSeq - LOG_N + 1 : 1;
  xSemaphoreGive(logMux);
  if (since + 1 > s) s = since + 1;
  String o = "[";
  bool first = true;
  while (true) {
    xSemaphoreTake(logMux, portMAX_DELAY);
    uint32_t end = logSeq;
    while (s <= end && o.length() < 1200) {
      LogEntry &e = logBuf[s % LOG_N];
      if (e.seq == s) {
        if (!first) o += ",";
        first = false;
        o += "{\"s\":" + String(e.seq) + ",\"t\":" + String(e.ms) + ",\"d\":\"" + String(e.dir) + "\",\"x\":" + q(e.text) + "}";
      }
      s++;
    }
    xSemaphoreGive(logMux);
    if (s > end) break;
    server.sendContent(o);
    o = "";
  }
  o += "]";
  server.sendContent(o);
  server.sendContent("");
}

void setupHttp() {
  server.addMiddleware([](WebServer &srv, Middleware::Callback next) {
    char st[64];
    const char *u = srv.uri().c_str();
    snprintf(st, sizeof st, "http %s from %u.%u.%u.%u", u ? u : "?", srv.client().remoteIP()[0], srv.client().remoteIP()[1],
             srv.client().remoteIP()[2], srv.client().remoteIP()[3]);
    setStage(st);
    lastWebActivity = millis();
    return next();
  });
  server.on("/", HTTP_GET, [] {
    noteClient();
    server.send_P(200, "text/html", UI_HTML);
  });
  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/scan", HTTP_GET, apiScan);
  server.on("/api/connect", HTTP_GET, apiConnect);
  server.on("/api/disconnect", HTTP_GET, [] {
    if (rejectIfReconnecting()) return;
    RLock ol(opMux, 15000);
    disconnectDongle();
    addLog('i', "Disconnected by user");
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/reconnect", HTTP_GET, [] {
    if (linkUp) {
      sendJson(200, "{\"ok\":true,\"already\":true}");
      return;
    }
    if (rejectIfReconnecting()) return;
    if (!autoTaskHandle || updatePrepared || !prefs.getString("addr", "").length()) {
      sendJson(409, "{\"error\":\"no saved dongle (or update in progress)\"}");
      return;
    }
    sendJson(200, "{\"ok\":true,\"started\":true}");
    triggerReconnect();
  });
  server.on("/api/forget", HTTP_GET, [] {
    prefs.clear();
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/auto", HTTP_GET, [] {
    autoReconnect = server.arg("on") != "0";
    prefs.putBool("auto", autoReconnect);
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/gatt", HTTP_GET, [] {
    if (!rejectIfReconnecting()) sendJson(200, gattJson);
  });
  server.on("/api/cmd", HTTP_GET, apiCmd);
  server.on("/api/log", HTTP_GET, apiLog);
  // The upload body is parsed before any handler runs, so Bluetooth has to be stopped beforehand to leave
  // the web server enough memory. Reboots after 2 minutes if no update follows.
  server.on("/api/prepare-update", HTTP_GET, [] {
    if (rejectIfReconnecting()) return;
    stopBluetoothForUpdate();
    updatePrepared = true;
    preparedAt = millis();
    sendJson(200, "{\"ok\":true,\"heap\":" + String(ESP.getFreeHeap()) + ",\"maxBlock\":" + String(ESP.getMaxAllocHeap()) + "}");
  });
  server.on("/api/crash", HTTP_GET, [] {
#if HAVE_CRASH_SUMMARY
    if (server.arg("erase") == "1") {
      esp_core_dump_image_erase();
      sendJson(200, "{\"erased\":true}");
      return;
    }
#endif
    sendJson(200, crashJson());
  });
  server.on("/api/reboot", HTTP_GET, [] {
    sendJson(200, "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });
  server.on(
    "/update", HTTP_POST,
    [] {
      bool ok = !Update.hasError();
      server.send(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting\n" : String("FAILED: ") + Update.errorString() + " - rebooting into the old firmware\n");
      delay(500);
      ESP.restart();
    },
    [] {
      HTTPUpload &u = server.upload();
      if (u.status == UPLOAD_FILE_START) {
        stopBluetoothForUpdate();
        addLog('i', "HTTP OTA update started: " + u.filename);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) addLog('e', String("OTA begin failed: ") + Update.errorString());
      } else if (u.status == UPLOAD_FILE_WRITE) {
        Update.write(u.buf, u.currentSize);
      } else if (u.status == UPLOAD_FILE_END) {
        if (Update.end(true)) addLog('i', "OTA image written: " + String(u.totalSize) + " bytes");
        else addLog('e', String("OTA failed: ") + Update.errorString());
      } else if (u.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        addLog('e', "OTA upload aborted - rebooting into the old firmware");
        rebootPending = true;
      }
    });
  server.onNotFound([] { sendJson(404, "{\"error\":\"not found\"}"); });
  server.begin();
}

// ---------------------------------------------------------------- TCP bridge

// Settings (NVS): WiFi mode 0 = off, 1 = home WiFi with hotspot fallback, 2 = hotspot only (applied at boot);
// TCP bridge on/off (live).
static uint8_t wifiMode = 1;
static volatile bool bridgeEnabled = true;

void serviceBridge() {
  if (!bridgeEnabled) {
    if (bridgeClient && bridgeClient.connected()) {
      addLog('i', "TCP bridge switched off - closing client");
      bridgeClient.stop();
      bridgeClient = WiFiClient();
      xSemaphoreGiveRecursive(opMux);
    }
    if (bridgeServer.hasClient()) bridgeServer.accept().stop();
    return;
  }
  if (bridgeServer.hasClient()) {
    WiFiClient c = bridgeServer.accept();
    bool busy = bridgeClient && bridgeClient.connected();
    if (!busy && xSemaphoreTakeRecursive(opMux, 0) != pdTRUE) busy = true;  // released on disconnect
    if (busy) {
      c.println("BUSY");
      c.stop();
    } else {
      bridgeClient = c;
      bridgeClient.setNoDelay(true);
      rxTake();
      addLog('i', "TCP bridge client " + bridgeClient.remoteIP().toString() + " connected");
    }
  }
  if (!bridgeClient) return;
  if (!bridgeClient.connected()) {
    addLog('i', "TCP bridge client disconnected");
    bridgeClient.stop();
    bridgeClient = WiFiClient();
    xSemaphoreGiveRecursive(opMux);
    return;
  }
  uint8_t b[256];
  int n = 0;
  while (bridgeClient.available() && n < (int)sizeof b) b[n++] = bridgeClient.read();
  if (n) {
    if (linkUp) {
      addLog('t', printableChunk(b, n));
      dongleWrite(b, n);
    } else {
      bridgeClient.print("\r\nNO DONGLE\r\n>");
    }
  }
  pumpSpp();
  String r = rxTake();
  if (r.length()) bridgeClient.write((const uint8_t *)r.c_str(), r.length());
}

// ---------------------------------------------------------------- WiFi

static bool apUp = false;

static uint32_t wifiStartMs = 0;
static bool staLogged = false;

void startHotspot() {
  WiFi.mode(wifiMode == 2 ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  apUp = true;
  addLog('i', String("Hotspot ") + AP_SSID + " up at " + WiFi.softAPIP().toString());
}

// Non-blocking: the dash must not wait for WiFi. In home mode the hotspot starts if home WiFi isn't found in 20 s.
void wifiTick() {
  if (wifiMode != 1) return;
  if (WiFi.status() == WL_CONNECTED && !staLogged) {
    staLogged = true;
    addLog('i', "WiFi connected: " + WiFi.localIP().toString() + " RSSI " + String(WiFi.RSSI()) + (apUp ? " (hotspot stays up)" : ""));
  }
  if (!apUp && !staLogged && millis() - wifiStartMs > 20000) {
    addLog('i', "Home WiFi not found - starting hotspot as well");
    startHotspot();
  }
}

void setupWifi() {
  wifiMode = prefs.getUChar("wifimode", 1);
  bridgeEnabled = prefs.getBool("bridge", true);
  if (wifiMode == 0) {
    WiFi.mode(WIFI_OFF);
    addLog('i', "WiFi switched off in settings (turn it back on from the Bridge screen)");
    return;
  }
  WiFi.setHostname(HOSTNAME);
  if (wifiMode == 2) startHotspot();
  else {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiStartMs = millis();
  }
  // ArduinoOTA also starts mDNS under HOSTNAME
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([] {
    stopBluetoothForUpdate();
    addLog('i', "ArduinoOTA update started");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    addLog('e', "ArduinoOTA error " + String((int)e) + " - rebooting into the old firmware");
    rebootPending = true;
  });
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("elm327", "tcp", BRIDGE_PORT);
}

#if HAS_LCD
#include "lcd_ui.h"
#endif

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  for (const LedPin &l : STATUS_LEDS) {
    if (l.pin < 0) continue;
    pinMode(l.pin, OUTPUT);
    digitalWrite(l.pin, l.onLevel);
  }
  logMux = xSemaphoreCreateMutex();
  rxMux = xSemaphoreCreateMutex();
  opMux = xSemaphoreCreateRecursiveMutex();
  connMux = xSemaphoreCreateRecursiveMutex();
  xTaskCreate(autoConnectTask, "autoconn", 6144, NULL, 1, &autoTaskHandle);
  delay(200);
  addLog('i', String("OBD-Probe " FW_VERSION " on ") + ESP.getChipModel() + (HAVE_SPP ? " (BLE + Classic SPP)" : " (BLE only)"));
  esp_reset_reason_t rr = esp_reset_reason();
  resetReasonStr = resetReasonName(rr);
  if (rtcMagic == FREEZE_MAGIC && rr == ESP_RST_SW) {
    rtcStage[sizeof rtcStage - 1] = 0;
    lastFreeze = "main loop froze for " + String(rtcStuckMs / 1000) + " s while: " + String(rtcStage);
    resetReasonStr = "freeze watchdog";
    addLog('e', "Previous run: " + lastFreeze);
  }
  rtcMagic = 0;
  if (rr == ESP_RST_PANIC) addLog('e', "Crash dump: " + crashJson());
  addLog(rr == ESP_RST_PANIC || rr == ESP_RST_BROWNOUT || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ? 'e' : 'i', "Reset reason: " + resetReasonStr);
  prefs.begin("obdprobe", false);
  autoReconnect = prefs.getBool("auto", true);
#if HAVE_SPP
  // A scan/connect of the other type reboots into it once; otherwise follow the saved dongle (default Classic).
  String once = prefs.getString("btonce", "");
  if (once.length()) {
    btMode = once;
    prefs.remove("btonce");
  } else {
    String saved = prefs.getString("type", "");
    btMode = saved.length() ? saved : prefs.getString("btmode", "spp");
  }
  if (btMode != "ble") btMode = "spp";
  btMemRelease(btMode == "spp" ? BT_MODE_BLE : BT_MODE_CLASSIC_BT);
  addLog('i', "Radio mode: " + String(btMode == "spp" ? "Classic SPP" : "BLE") + " (switches automatically when you scan/connect the other type)");
#endif
#if HAS_LCD
  lcdui::begin();
#endif
  setupWifi();
  setupHttp();
  bridgeServer.begin();
  bridgeServer.setNoDelay(true);
  nextAutoTry = millis();  // first auto-reconnect starts from loop(), in the background
  loopBeat = millis();
  xTaskCreate(freezeWatchdog, "freezewd", 3072, NULL, 5, NULL);
}

void loop() {
  loopBeat = millis();
  if (rebootPending || (updatePrepared && millis() - preparedAt > 120000 && !Update.isRunning())) {
    if (!rebootPending) addLog('i', "No update arrived after prepare-update - rebooting to restore Bluetooth");
    delay(1000);
    ESP.restart();
  }
  setStage("ArduinoOTA.handle");
  ArduinoOTA.handle();
  setStage("http (idle)");
  server.handleClient();
  setStage("tcp bridge");
  serviceBridge();
  setStage("spp pump");
  pumpSpp();
  setStage("auto-reconnect check");
  maybeAutoReconnect();
  wifiTick();
  setStage("idle (delay)");
  delay(1);
}
