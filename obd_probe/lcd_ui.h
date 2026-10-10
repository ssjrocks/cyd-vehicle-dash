#pragma once
// Local touchscreen UI for the Waveshare ESP32-S3-Touch-LCD-4.3 (800x480 RGB panel, GT911 touch, CH422G IO expander).
// Replacement dash: top tab bar with Dash (speedo + tacho), Faults, All data, Bridge (WiFi / TCP bridge / dongle / settings).
//
// Threads: lvTask runs LVGL (holds lvMux while drawing; UI callbacks run inside it and must not block).
// worker does all dongle I/O (takes opMux per command, like the web handlers) and updates widgets under lvMux.
// Lock order: opMux -> connMux, and lvMux is never held while waiting for opMux/connMux.
// Panel pins/timings are from the ESPHome config that already drives this exact board (waveshare.yaml).

#include <Wire.h>
#include <lvgl.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_heap_caps.h>
#include <vector>
#include <algorithm>

namespace lcdui {

static const int W = 800, H = 480;
static const int I2C_SDA = 8, I2C_SCL = 9, TOUCH_INT = 4;
static const uint8_t CH422G_MODE = 0x24, CH422G_OUT = 0x38;  // EXIO1 touch reset, EXIO2 backlight, EXIO3 LCD reset
enum Tab : uint8_t { TAB_DASH, TAB_FAULTS, TAB_DATA, TAB_BRIDGE };

static esp_lcd_panel_handle_t panel = nullptr;
static SemaphoreHandle_t lvMux;
static QueueHandle_t jobs;
static uint8_t gtAddr = 0;
static bool rotated = true;
static volatile uint8_t activeTab = TAB_DASH;

// Sleep: with no dongle link and no touch for a minute the backlight goes off and drawing stops. Touch, or the
// dongle connecting (auto-reconnect keeps listening for it), wakes it. The chip itself stays awake: deep sleep
// would switch off the radio that listens for the dongle.
static const uint32_t SLEEP_AFTER_MS = 60000;
static volatile bool asleep = false;
static volatile uint32_t lastActivity = 0;
static bool swallowTouch = false;  // the touch that wakes the screen must not also press a button
static uint8_t ioOut = 0x2E;       // CH422G outputs: bit1 touch reset (high = run), bit2 backlight, bit3 LCD reset

void ioWrite(uint8_t v) {
  ioOut = v;
  Wire.beginTransmission(CH422G_OUT);
  Wire.write(v);
  Wire.endTransmission();
}

void backlight(bool on) { ioWrite(on ? (ioOut | 0x04) : (ioOut & ~0x04)); }

// ---------------------------------------------------------------- hardware

void ioExpanderInit() {
  Wire.beginTransmission(CH422G_MODE);
  Wire.write(0x01);  // IO0-7 as outputs
  Wire.endTransmission();
  Wire.beginTransmission(CH422G_OUT);
  Wire.write(0x2C);  // backlight + LCD on, touch held in reset
  Wire.endTransmission();
  delay(100);
  pinMode(TOUCH_INT, OUTPUT);
  digitalWrite(TOUCH_INT, LOW);  // INT low while reset releases -> GT911 at 0x5D
  delay(100);
  ioWrite(0x2E);  // release touch reset
  delay(200);
  pinMode(TOUCH_INT, INPUT);
}

bool gtRead(uint16_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)gtAddr, (size_t)n, true) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void gtWrite(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  Wire.endTransmission();
}

void touchInit() {
  for (uint8_t a : {0x5D, 0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      gtAddr = a;
      break;
    }
  }
  addLog(gtAddr ? 'i' : 'e', gtAddr ? "Touch controller GT911 at 0x" + String(gtAddr, HEX) : String("Touch controller not found on I2C"));
}

// Raw "is a finger down" check used while asleep (LVGL isn't running then)
bool touchDown() {
  uint8_t st;
  if (!gtAddr || !gtRead(0x814E, &st, 1) || !(st & 0x80)) return false;
  gtWrite(0x814E, 0);
  return (st & 0x0F) > 0;
}

void touchRead(lv_indev_drv_t *, lv_indev_data_t *data) {
  static int16_t lastX = 0, lastY = 0;
  data->state = LV_INDEV_STATE_REL;
  uint8_t st;
  if (gtAddr && gtRead(0x814E, &st, 1) && (st & 0x80)) {
    if ((st & 0x0F) > 0) {
      uint8_t p[4];
      if (gtRead(0x8150, p, 4)) {
        int16_t x = p[0] | (p[1] << 8), y = p[2] | (p[3] << 8);
        if (rotated) {
          x = W - 1 - x;
          y = H - 1 - y;
        }
        lastX = constrain(x, 0, W - 1);
        lastY = constrain(y, 0, H - 1);
        data->state = LV_INDEV_STATE_PR;
      }
    }
    gtWrite(0x814E, 0);
  }
  if (data->state == LV_INDEV_STATE_PR) lastActivity = millis();
  if (swallowTouch) {
    if (data->state == LV_INDEV_STATE_REL) swallowTouch = false;
    data->state = LV_INDEV_STATE_REL;
  }
  data->point.x = lastX;
  data->point.y = lastY;
}

bool panelInit() {
  esp_lcd_rgb_panel_config_t c = {};
  c.clk_src = LCD_CLK_SRC_DEFAULT;
  c.timings.pclk_hz = 14000000;
  c.timings.h_res = W;
  c.timings.v_res = H;
  c.timings.hsync_pulse_width = 10;
  c.timings.hsync_back_porch = 10;
  c.timings.hsync_front_porch = 20;
  c.timings.vsync_pulse_width = 10;
  c.timings.vsync_back_porch = 10;
  c.timings.vsync_front_porch = 10;
  c.data_width = 16;
  c.bits_per_pixel = 16;
  c.num_fbs = 1;
  c.bounce_buffer_size_px = W * 10;  // keeps the panel fed from SRAM while WiFi/flash load PSRAM
  c.hsync_gpio_num = 46;
  c.vsync_gpio_num = 3;
  c.de_gpio_num = 5;
  c.pclk_gpio_num = 7;
  c.disp_gpio_num = -1;
  const int pins[16] = {14, 38, 18, 17, 10,       // B3..B7 -> D0..D4
                        39, 0, 45, 48, 47, 21,    // G2..G7 -> D5..D10
                        1, 2, 42, 41, 40};        // R3..R7 -> D11..D15
  for (int i = 0; i < 16; i++) c.data_gpio_nums[i] = pins[i];
  c.flags.fb_in_psram = 1;
  if (esp_lcd_new_rgb_panel(&c, &panel) != ESP_OK) return false;
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  esp_lcd_panel_mirror(panel, rotated, rotated);
  return true;
}

void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
  esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);  // copies into the frame buffer
  lv_disp_flush_ready(d);
}

// ---------------------------------------------------------------- OBD response parsing (same rules as the web UI)

static String protoNum;  // ATDPN without the leading 'A'

std::vector<std::vector<uint8_t>> parseMsgs(const String &raw) {
  std::vector<std::vector<uint8_t>> out;
  int multiIdx = -1, multiLen = 0;
  int start = 0;
  while (start <= (int)raw.length()) {
    int e = start;
    while (e < (int)raw.length() && raw[e] != '\r' && raw[e] != '\n') e++;
    String l = raw.substring(start, e);
    start = e + 1;
    l.replace(">", "");
    l.trim();
    if (!l.length() || l.startsWith("SEARCHING") || l.startsWith("BUS INIT") || l.startsWith("STOPPED")) continue;
    if (l.length() == 3 && isxdigit(l[0]) && isxdigit(l[1]) && isxdigit(l[2])) {  // CAN multi-frame byte count
      multiLen = strtol(l.c_str(), nullptr, 16);
      out.push_back({});
      multiIdx = out.size() - 1;
      continue;
    }
    bool frame = l.length() > 2 && l[1] == ':' && multiIdx >= 0;
    if (frame) l = l.substring(2);
    std::vector<uint8_t> b;
    String hx;
    for (char ch : l)
      if (ch != ' ') hx += ch;
    bool ok = hx.length() >= 2 && hx.length() % 2 == 0;
    for (char ch : hx) ok = ok && isxdigit(ch);
    if (!ok) continue;
    for (size_t i = 0; i < hx.length(); i += 2) b.push_back(strtol(hx.substring(i, i + 2).c_str(), nullptr, 16));
    if (frame) {
      auto &m = out[multiIdx];
      m.insert(m.end(), b.begin(), b.end());
      if ((int)m.size() > multiLen) m.resize(multiLen);
    } else out.push_back(b);
  }
  return out;
}

bool pidData(const String &raw, uint8_t mode, uint8_t pid, std::vector<uint8_t> &d) {
  for (auto &m : parseMsgs(raw))
    if (m.size() >= 2 && m[0] == mode + 0x40 && m[1] == pid) {
      d.assign(m.begin() + 2, m.end());
      return true;
    }
  return false;
}

String errText(const String &raw) {
  String r = raw;
  r.replace("\r", " ");
  r.replace("\n", " ");
  r.replace(">", "");
  r.replace("SEARCHING...", "");
  r.trim();
  return r.length() ? r : String("no reply");
}

String dtcCode(uint8_t a, uint8_t b) {
  char s[6];
  snprintf(s, sizeof s, "%c%X%X%X%X", "PCBU"[a >> 6], (a >> 4) & 3, a & 15, b >> 4, b & 15);
  return s;
}

const char *dtcHint(const String &c) {
  struct D {
    const char *code, *text;
  };
  static const D T[] = {
    {"P0420", "Catalyst efficiency below threshold (bank 1)"}, {"P0430", "Catalyst efficiency below threshold (bank 2)"},
    {"P0700", "Transmission control system - fault stored in gearbox module"},
    {"P0171", "System too lean (bank 1)"}, {"P0172", "System too rich (bank 1)"},
    {"P0300", "Random/multiple misfire"}, {"P0301", "Cylinder 1 misfire"}, {"P0302", "Cylinder 2 misfire"},
    {"P0303", "Cylinder 3 misfire"}, {"P0304", "Cylinder 4 misfire"}, {"P0130", "O2 sensor circuit (B1S1)"},
    {"P0133", "O2 sensor slow response (B1S1)"}, {"P0136", "O2 sensor circuit (B1S2)"},
    {"P0101", "MAF range/performance"}, {"P0102", "MAF circuit low"}, {"P0113", "Intake air temp sensor high"},
    {"P0117", "Coolant temp sensor low"}, {"P0118", "Coolant temp sensor high"}, {"P0128", "Thermostat (coolant below regulating temp)"},
    {"P0335", "Crankshaft position sensor circuit"}, {"P0340", "Camshaft position sensor circuit"},
    {"P0401", "EGR flow insufficient"}, {"P0440", "EVAP system malfunction"}, {"P0442", "EVAP small leak"},
    {"P0455", "EVAP large leak"}, {"P0500", "Vehicle speed sensor"}, {"P0505", "Idle air control system"},
    {"P0562", "System voltage low"}, {"P0601", "ECU memory checksum error"},
  };
  for (auto &d : T)
    if (c == d.code) return d.text;
  return c[0] == 'P' && c[1] == '0' ? "generic powertrain code" : c[0] == 'P' ? "manufacturer-specific powertrain code" : "body/chassis/network code";
}

String pidName(uint8_t p) {
  switch (p) {
    case 0x01: return "Monitor status"; case 0x03: return "Fuel system"; case 0x04: return "Engine load"; case 0x05: return "Coolant temp";
    case 0x06: return "Short fuel trim B1"; case 0x07: return "Long fuel trim B1"; case 0x08: return "Short fuel trim B2"; case 0x09: return "Long fuel trim B2";
    case 0x0A: return "Fuel pressure"; case 0x0B: return "Intake MAP"; case 0x0C: return "Engine RPM"; case 0x0D: return "Vehicle speed";
    case 0x0E: return "Timing advance"; case 0x0F: return "Intake air temp"; case 0x10: return "MAF air flow"; case 0x11: return "Throttle position";
    case 0x13: return "O2 sensors present"; case 0x1C: return "OBD standard"; case 0x1F: return "Run time since start"; case 0x21: return "Distance with MIL on";
    case 0x2F: return "Fuel level"; case 0x31: return "Distance since clear"; case 0x33: return "Barometric pressure"; case 0x42: return "ECU voltage";
    case 0x46: return "Ambient air temp"; case 0x5C: return "Oil temp"; case 0x5E: return "Fuel rate";
  }
  if (p >= 0x14 && p <= 0x1B) return "O2 sensor B" + String(1 + ((p - 0x14) >> 2)) + "S" + String(1 + ((p - 0x14) & 3));
  if (p >= 0x3C && p <= 0x3F) return "Catalyst temp " + String(p - 0x3B);
  char b[12];
  snprintf(b, sizeof b, "PID %02X", p);
  return b;
}

String decodePid(uint8_t p, const std::vector<uint8_t> &d) {
  auto A = d.size() > 0 ? d[0] : 0, B = d.size() > 1 ? d[1] : 0;
  auto pct = [](float v) { return String(v, 1) + " %"; };
  auto trim = [&](uint8_t v) { return String((v - 128) * 100.0f / 128, 1) + " %"; };
  switch (p) {
    case 0x01: return String("Check-engine ") + (A & 0x80 ? "ON" : "off") + ", " + String(A & 0x7F) + " code(s)";
    case 0x03: return A == 1 ? "Open loop (cold)" : A == 2 ? "Closed loop" : A == 4 ? "Open loop (load)" : A == 8 ? "Open loop (fault)" : A == 16 ? "Closed loop (fault)" : String(A);
    case 0x04: case 0x11: case 0x2F: return pct(A * 100.0f / 255);
    case 0x05: case 0x0F: case 0x46: case 0x5C: return String((int)A - 40) + " \xC2\xB0" "C";
    case 0x06: case 0x07: case 0x08: case 0x09: return trim(A);
    case 0x0A: return String(A * 3) + " kPa";
    case 0x0B: case 0x33: return String(A) + " kPa";
    case 0x0C: return String((A * 256 + B) / 4) + " rpm";
    case 0x0D: return String(A) + " km/h";
    case 0x0E: return String(A / 2.0f - 64, 1) + " \xC2\xB0";
    case 0x10: return String((A * 256 + B) / 100.0f, 2) + " g/s";
    case 0x13: return "0x" + String(A, HEX);
    case 0x1C: return A == 6 ? "EOBD" : A == 1 ? "OBD-II" : A == 4 ? "OBD-I" : String(A);
    case 0x1F: return String(A * 256 + B) + " s";
    case 0x21: case 0x31: return String(A * 256 + B) + " km";
    case 0x42: return String((A * 256 + B) / 1000.0f, 2) + " V";
    case 0x5E: return String((A * 256 + B) / 20.0f, 2) + " L/h";
  }
  if (p >= 0x14 && p <= 0x1B) return String(A / 200.0f, 3) + " V" + (B != 0xFF ? ", trim " + trim(B) : String(""));
  if (p >= 0x3C && p <= 0x3F) return String((A * 256 + B) / 10.0f - 40, 0) + " \xC2\xB0" "C";
  String s;
  for (uint8_t x : d) {
    char h[4];
    snprintf(h, sizeof h, "%02X ", x);
    s += h;
  }
  return s;
}

// ---------------------------------------------------------------- widgets / shared state

static lv_obj_t *statusLbl, *tabs;
static lv_obj_t *faultLbl, *dataTable, *dataMsg;
static lv_obj_t *connList, *connInfo, *bridgeInfo, *wifiDrop, *bridgeSw;

struct Gauge {
  lv_obj_t *meter;
  lv_meter_indicator_t *needle, *arc;
  lv_obj_t *digits, *unit, *sub;
  int32_t shown;
};
static Gauge speedo, tacho, coolant, intake;
static lv_obj_t *milLbl, *noDataLbl;  // noDataLbl doubles as the battery readout when data is flowing
static float batteryV = 0;

static std::vector<uint8_t> supported;
static volatile bool needInit = true;  // run adapter init + PID discovery after (re)connect
static bool fastSuffix = false;        // ELM "expect 1 reply" suffix works -> no wait for the reply timeout
static volatile uint32_t lastGood = 0;
static volatile bool workerBusy = false;

struct ScanHit {
  String addr, name;
  int rssi;
  uint8_t atype;
  bool likely;
};
static std::vector<ScanHit> hits;

enum JobType : uint8_t { J_SCAN, J_CONNECT, J_RECONNECT, J_DISCONNECT, J_FORGET, J_DTC, J_CLEAR };
struct Job {
  JobType t;
  int arg;
};

void post(JobType t, int arg = 0) {
  Job j{t, arg};
  xQueueSend(jobs, &j, 0);
}

struct LvLock : RLock {
  LvLock() : RLock(lvMux, UINT32_MAX) {}
};

void setText(lv_obj_t *o, const String &s) {
  LvLock l;
  lv_label_set_text(o, s.c_str());
}

lv_obj_t *mkBtn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud = nullptr) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

lv_obj_t *mkText(lv_obj_t *parent, const lv_font_t *f = &lv_font_montserrat_16) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, lv_pct(100));
  lv_obj_set_style_text_font(l, f, 0);
  lv_label_set_text(l, "");
  return l;
}

lv_obj_t *mkRow(lv_obj_t *parent) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 10, 0);
  lv_obj_set_style_pad_row(r, 8, 0);
  return r;
}

lv_obj_t *mkHeading(lv_obj_t *parent, const char *t) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, lv_palette_main(LV_PALETTE_BLUE), 0);
  return l;
}

void tabLayout(lv_obj_t *t) {
  lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(t, 10, 0);
  lv_obj_set_style_pad_row(t, 8, 0);
}

// ---------------------------------------------------------------- gauges

void gaugeAnim(void *var, int32_t v) {
  Gauge *g = (Gauge *)var;
  g->shown = v;
  lv_meter_set_indicator_value(g->meter, g->needle, v);
  lv_meter_set_indicator_end_value(g->meter, g->arc, v);
}

// Called with lvMux held. value in scale units (km/h, or rpm/100).
void gaugeSet(Gauge &g, int32_t v) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &g);
  lv_anim_set_exec_cb(&a, gaugeAnim);
  lv_anim_set_values(&a, g.shown, v);
  lv_anim_set_time(&a, 300);  // roughly the K-line update interval, so the needle moves smoothly
  lv_anim_start(&a);
}

// Tacho scale runs 0..70 (x100 rpm); print the major labels as 0..7
void tachoLabels(lv_event_t *e) {
  lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
  if (d->class_p == &lv_meter_class && d->type == LV_METER_DRAW_PART_TICK && d->text) lv_snprintf(d->text, d->text_length, "%d", (int)(d->value / 10));
}

void mkGauge(Gauge &g, lv_obj_t *parent, int max, int ticks, int majorEvery, int redFrom, const char *unit, bool tach) {
  g.meter = lv_meter_create(parent);
  lv_obj_set_size(g.meter, 340, 340);
  lv_obj_set_style_pad_all(g.meter, 14, 0);
  lv_obj_set_style_text_font(g.meter, &lv_font_montserrat_20, 0);
  lv_meter_scale_t *sc = lv_meter_add_scale(g.meter);
  lv_meter_set_scale_ticks(g.meter, sc, ticks, 2, 12, lv_palette_main(LV_PALETTE_GREY));
  lv_meter_set_scale_major_ticks(g.meter, sc, majorEvery, 4, 20, lv_color_white(), 14);
  lv_meter_set_scale_range(g.meter, sc, 0, max, 270, 135);
  if (redFrom >= 0) {
    lv_meter_indicator_t *red = lv_meter_add_arc(g.meter, sc, 8, lv_palette_main(LV_PALETTE_RED), 0);
    lv_meter_set_indicator_start_value(g.meter, red, redFrom);
    lv_meter_set_indicator_end_value(g.meter, red, max);
  }
  g.arc = lv_meter_add_arc(g.meter, sc, 8, lv_palette_main(LV_PALETTE_CYAN), -12);
  lv_meter_set_indicator_start_value(g.meter, g.arc, 0);
  lv_meter_set_indicator_end_value(g.meter, g.arc, 0);
  g.needle = lv_meter_add_needle_line(g.meter, sc, 5, lv_palette_main(LV_PALETTE_ORANGE), -24);
  if (tach) lv_obj_add_event_cb(g.meter, tachoLabels, LV_EVENT_DRAW_PART_BEGIN, NULL);
  g.digits = lv_label_create(g.meter);
  lv_obj_set_style_text_font(g.digits, &lv_font_montserrat_48, 0);
  lv_label_set_text(g.digits, "--");
  lv_obj_align(g.digits, LV_ALIGN_CENTER, 0, 52);
  g.unit = lv_label_create(g.meter);
  lv_label_set_text(g.unit, unit);
  lv_obj_set_style_text_color(g.unit, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(g.unit, LV_ALIGN_CENTER, 0, 92);
  g.sub = lv_label_create(g.meter);
  lv_obj_set_style_text_font(g.sub, &lv_font_montserrat_20, 0);
  lv_label_set_text(g.sub, "");
  lv_obj_align(g.sub, LV_ALIGN_BOTTOM_MID, 0, -4);
  g.shown = 0;
}

// Small temperature gauge for the gap between the two main dials: a plain blue-to-red arc with a needle and no
// text inside (too small to read at this size). The value and caption are separate labels placed outside.
void mkMini(Gauge &g, lv_obj_t *parent, int min, int max, const char *caption) {
  g.meter = lv_meter_create(parent);
  lv_obj_set_size(g.meter, 90, 90);
  lv_obj_set_style_pad_all(g.meter, 4, 0);
  lv_obj_clear_flag(g.meter, LV_OBJ_FLAG_SCROLLABLE);
  lv_meter_scale_t *sc = lv_meter_add_scale(g.meter);
  lv_meter_set_scale_ticks(g.meter, sc, 0, 0, 0, lv_color_black());  // no ticks, no numbers
  lv_meter_set_scale_range(g.meter, sc, min, max, 240, 150);
  const int seg = 12;
  for (int i = 0; i < seg; i++) {  // colour bands blending from blue (cold) to red (hot)
    lv_color_t c = lv_color_mix(lv_palette_main(LV_PALETTE_RED), lv_palette_main(LV_PALETTE_BLUE), (uint8_t)(255 * i / (seg - 1)));
    lv_meter_indicator_t *a = lv_meter_add_arc(g.meter, sc, 10, c, 0);
    lv_meter_set_indicator_start_value(g.meter, a, min + (max - min) * i / seg);
    lv_meter_set_indicator_end_value(g.meter, a, min + (max - min) * (i + 1) / seg);
  }
  g.arc = lv_meter_add_arc(g.meter, sc, 0, lv_color_black(), 0);  // unused (zero width)
  g.needle = lv_meter_add_needle_line(g.meter, sc, 4, lv_color_white(), -14);
  lv_meter_set_indicator_value(g.meter, g.needle, min);
  g.shown = min;
  g.digits = lv_label_create(parent);
  lv_obj_set_style_text_font(g.digits, &lv_font_montserrat_20, 0);
  lv_obj_set_width(g.digits, 120);  // fixed width + centred text: stays centred as the value changes
  lv_obj_set_style_text_align(g.digits, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g.digits, "--");
  g.unit = lv_label_create(parent);
  lv_obj_set_width(g.unit, 120);
  lv_obj_set_style_text_align(g.unit, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g.unit, caption);
  lv_obj_set_style_text_font(g.unit, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(g.unit, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
  g.sub = nullptr;
}

void setTemp(Gauge &g, int t, int lo, int hi, int coldTo, int hotFrom) {  // caller holds lvMux
  gaugeSet(g, constrain(t, lo, hi));
  lv_label_set_text_fmt(g.digits, "%d \xC2\xB0" "C", t);
  lv_obj_set_style_text_color(g.digits, t >= hotFrom ? lv_palette_main(LV_PALETTE_RED) : t < coldTo ? lv_palette_lighten(LV_PALETTE_LIGHT_BLUE, 1) : lv_color_white(), 0);
}

void buildDataTable() {  // caller holds lvMux
  std::vector<uint8_t> rows;
  for (uint8_t p : supported)
    if (!(p == 0x02 || p == 0x41 || p % 0x20 == 0)) rows.push_back(p);
  // LVGL 8.4 crashes on a row count of 0, so an empty table keeps one placeholder row
  lv_table_set_row_cnt(dataTable, rows.size() ? rows.size() : 1);
  if (rows.empty()) {
    lv_table_set_cell_value(dataTable, 0, 0, "");
    lv_table_set_cell_value(dataTable, 0, 1, "waiting for the dongle...");
    lv_table_set_cell_value(dataTable, 0, 2, "");
  }
  int r = 0;
  for (uint8_t p : rows) {
    char id[6];
    snprintf(id, sizeof id, "01%02X", p);
    lv_table_set_cell_value(dataTable, r, 0, id);
    lv_table_set_cell_value(dataTable, r, 1, pidName(p).c_str());
    lv_table_set_cell_value(dataTable, r, 2, "-");
    r++;
  }
  lv_label_set_text(dataMsg, r ? "Updates continuously while this screen is open (about 4 readings a second on this car)." : "No supported PIDs yet - waiting for the dongle.");
}

// ---------------------------------------------------------------- dongle I/O (worker task)

// Runs one command with the dongle lock; false if the dongle is busy elsewhere or disconnected.
bool cmd(const String &c, String &raw, uint32_t timeout = 5000) {
  if (!linkUp) return false;
  RLock ol(opMux, 8000);
  if (!ol.ok) return false;
  bool to;
  uint32_t el;
  raw = runCmd(c, timeout, to, el);
  return true;
}

String need() { return linkUp ? "Dongle busy (web page, TCP bridge or reconnect) - try again" : "Dongle not connected - see the Bridge screen"; }

// Mode 01 read. Uses the ELM "1 reply expected" suffix when the adapter supports it (saves the reply timeout).
bool readPid(uint8_t pid, std::vector<uint8_t> &d, uint32_t timeout = 2000) {
  char c[8];
  snprintf(c, sizeof c, fastSuffix ? "01%02X1" : "01%02X", pid);
  String raw;
  if (!cmd(c, raw, timeout)) return false;
  if (pidData(raw, 1, pid, d)) {
    lastGood = millis();
    return true;
  }
  return false;
}

void initAdapter() {
  String raw;
  for (const char *c : {"ATE0", "ATS1", "ATH0", "ATSH C133F1"}) cmd(c, raw, 2000);
  supported.clear();
  for (int base = 0; base <= 0xE0; base += 0x20) {
    char c[5];
    snprintf(c, sizeof c, "01%02X", base);
    if (!cmd(c, raw, base ? 5000 : 20000)) return;  // retried on the next worker pass
    std::vector<uint8_t> d;
    if (!pidData(raw, 1, base, d) || d.size() < 4) {
      if (!base) return;  // no ECU answer yet (ignition off?) - try again later
      break;
    }
    bool more = false;
    for (int i = 0; i < 32; i++)
      if ((d[i >> 3] >> (7 - (i & 7))) & 1) {
        int p = base + i + 1;
        if (p == base + 0x20) more = true;
        else supported.push_back(p);
      }
    if (!more) break;
  }
  if (cmd("ATDPN", raw, 2000)) {
    protoNum = errText(raw);
    if (protoNum.startsWith("A")) protoNum = protoNum.substring(1);
  }
  std::vector<uint8_t> d;
  fastSuffix = true;
  if (!readPid(0x0C, d)) fastSuffix = false;
  addLog('i', "Dash: " + String(supported.size()) + " PIDs supported, protocol " + protoNum + (fastSuffix ? ", fast mode" : ""));
  {
    LvLock l;
    buildDataTable();
  }
  needInit = false;
}

void pollDash() {
  static uint8_t n = 0;
  std::vector<uint8_t> d;
  if (readPid(0x0D, d) && d.size() >= 1) {
    LvLock l;
    gaugeSet(speedo, d[0]);
    lv_label_set_text_fmt(speedo.digits, "%d", d[0]);
  }
  if (readPid(0x0C, d) && d.size() >= 2) {
    int rpm = (d[0] * 256 + d[1]) / 4;
    LvLock l;
    gaugeSet(tacho, rpm / 100);
    lv_label_set_text_fmt(tacho.digits, "%d", rpm);
  }
  switch (n++ % 9) {  // slower values, one every few cycles
    case 2:
      if (readPid(0x05, d) && d.size() >= 1) {
        LvLock l;
        setTemp(coolant, d[0] - 40, 40, 130, 60, 110);
      }
      break;
    case 4:
      if (readPid(0x0F, d) && d.size() >= 1) {
        LvLock l;
        setTemp(intake, d[0] - 40, -20, 80, 0, 60);
      }
      break;
    case 6: {
      String raw;
      if (cmd("ATRV", raw, 2000)) {
        float v = errText(raw).toFloat();
        if (v > 5) batteryV = v;  // shown by statusTick in the bottom-centre slot
      }
      break;
    }
    case 8:
      if (readPid(0x01, d) && d.size() >= 1) {
        LvLock l;
        if (d[0] & 0x80) lv_obj_clear_flag(milLbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(milLbl, LV_OBJ_FLAG_HIDDEN);
      }
      break;
  }
}

void pollData() {
  static size_t idx = 0;
  std::vector<uint8_t> rows;
  for (uint8_t p : supported)
    if (!(p == 0x02 || p == 0x41 || p % 0x20 == 0)) rows.push_back(p);
  if (rows.empty()) {
    vTaskDelay(pdMS_TO_TICKS(300));
    return;
  }
  idx %= rows.size();
  uint8_t p = rows[idx];
  std::vector<uint8_t> d;
  String v = readPid(p, d, 2500) ? decodePid(p, d) : String("-");
  LvLock l;
  if (idx < lv_table_get_row_cnt(dataTable)) lv_table_set_cell_value(dataTable, idx, 2, v.c_str());
  idx++;
}

void jobDtc() {
  String r, raw;
  if (!linkUp) {
    setText(faultLbl, need());
    return;
  }
  setText(faultLbl, "Reading...");
  bool can = protoNum.length() && strchr("6789AB", protoNum[0]);
  std::vector<uint8_t> d;
  if (readPid(0x01, d, 6000) && d.size())
    r += String("Check-engine light: ") + (d[0] & 0x80 ? "ON" : "off") + "  (" + String(d[0] & 0x7F) + " standard code(s))\n\n";
  for (auto pr : {std::make_pair("03", "Stored"), std::make_pair("07", "Pending")}) {
    if (!cmd(pr.first, raw, 6000)) {
      r += String(pr.second) + ": " + need() + "\n";
      continue;
    }
    uint8_t mode = strtol(pr.first, nullptr, 16) + 0x40;
    String list;
    bool any = false;
    for (auto &m : parseMsgs(raw)) {
      if (m.empty() || m[0] != mode) continue;
      any = true;
      for (size_t i = can ? 2 : 1; i + 1 < m.size(); i += 2)
        if (m[i] || m[i + 1]) {
          String c = dtcCode(m[i], m[i + 1]);
          list += "  " + c + "  " + dtcHint(c) + "\n";
        }
    }
    r += String(pr.second) + " codes:\n" + (list.length() ? list : any ? String("  none\n") : "  " + errText(raw) + "\n");
  }
  // K-line (KWP2000) ECUs: read codes by status. This is the read that shows codes other modules (e.g. the
  // automatic gearbox) have reported to the engine ECU, which standard mode 03 leaves out.
  if (!can && cmd("1800FF00", raw, 6000)) {
    String list;
    for (auto &m : parseMsgs(raw)) {
      if (m.size() < 2 || m[0] != 0x58) continue;
      for (size_t i = 2; i + 2 < m.size(); i += 3) {  // 58 <count> then <hi lo status> per code
        String c = dtcCode(m[i], m[i + 1]);
        uint8_t st = m[i + 2];
        list += "  " + c + "  " + dtcHint(c) + "\n      " + (st & 0x80 ? "lamp ON, " : "") + ((st & 0x60) == 0x60 ? "present now" : (st & 0x20) ? "stored" : "intermittent") + "\n";
      }
    }
    r += String("\nEngine ECU fault memory (incl. other modules):\n") + (list.length() ? list : String("  none\n"));
  }
  setText(faultLbl, r);
}

void jobClear() {
  String raw;
  if (!cmd("04", raw, 8000)) {
    setText(faultLbl, need());
    return;
  }
  setText(faultLbl, "Clear sent: " + errText(raw) + "\n(44 = cleared). Engine must be OFF with ignition ON.\nTap Read codes to confirm.");
}

void jobScan() {
  setText(connInfo, "Scanning for BLE devices (6 s)...");
  std::vector<ScanHit> found;
  {
    RLock ol(opMux, 8000);
    if (!ol.ok) {
      setText(connInfo, "Dongle busy - try again");
      return;
    }
    bleInit();
    BLEScan *sc = BLEDevice::getScan();
    sc->setActiveScan(true);
    sc->setInterval(100);
    sc->setWindow(99);
    BLEScanResults *res = sc->start(6, false);
    for (int i = 0; i < res->getCount(); i++) {
      BLEAdvertisedDevice d = res->getDevice(i);
      ScanHit h{d.getAddress().toString(), d.haveName() ? d.getName() : String(""), d.getRSSI(), (uint8_t)d.getAddressType(), false};
      String n = h.name;
      n.toLowerCase();
      h.likely = n.indexOf("obd") >= 0 || n.indexOf("elm") >= 0 || n.indexOf("vlink") >= 0 || n.indexOf("vgate") >= 0 || n.indexOf("icar") >= 0;
      for (int k = 0; k < d.getServiceUUIDCount(); k++) {
        String u = shortUuid(d.getServiceUUID(k).toString());
        if (u == "fff0" || u == "ffe0" || u == "18f0") h.likely = true;
      }
      if (h.name.length() || h.likely) found.push_back(h);
    }
    sc->clearResults();
  }
  std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.likely != b.likely ? a.likely : a.rssi > b.rssi; });
  LvLock l;
  hits = found;
  lv_obj_clean(connList);
  for (size_t i = 0; i < hits.size(); i++) {
    String t = (hits[i].likely ? String(LV_SYMBOL_OK " ") : String("")) + hits[i].name + "   " + hits[i].addr + "   " + String(hits[i].rssi) + " dBm";
    lv_obj_t *b = lv_list_add_btn(connList, LV_SYMBOL_BLUETOOTH, t.c_str());
    lv_obj_add_event_cb(b, [](lv_event_t *e) { post(J_CONNECT, (int)(intptr_t)lv_event_get_user_data(e)); }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  lv_label_set_text(connInfo, hits.empty() ? "No named BLE devices found. Dongle powered and in range?" : "Tap a device to connect (" LV_SYMBOL_OK " = looks like an OBD adapter). Can take up to 30 s.");
}

void jobConnect(int i) {
  ScanHit h;
  {
    LvLock l;
    if (i < 0 || i >= (int)hits.size()) return;
    h = hits[i];
  }
  setText(connInfo, "Connecting to " + h.name + " (" + h.addr + ")... up to 30 s");
  bool ok;
  {
    RLock ol(opMux, 8000);
    if (!ol.ok) {
      setText(connInfo, "Dongle busy - try again");
      return;
    }
    ok = connectBle(h.addr, h.atype, "", "");
    if (ok) {
      RLock cl(connMux, UINT32_MAX);
      devName = h.name;
      saveDevice("ble", h.addr, h.atype, h.name, "", "");
    }
  }
  String msg;
  {
    RLock cl(connMux, UINT32_MAX);
    msg = ok ? "Connected via " + layoutLabel + ". Saved - reconnects automatically." : lastConnectError;
  }
  needInit = true;
  setText(connInfo, msg);
}

void worker(void *) {
  bool wasUp = false;
  for (;;) {
    if (linkUp != wasUp) {
      wasUp = linkUp;
      if (linkUp) needInit = true;
    }
    Job j;
    bool idle = !linkUp || bgConnecting || (activeTab != TAB_DASH && activeTab != TAB_DATA);
    if (xQueueReceive(jobs, &j, idle ? pdMS_TO_TICKS(400) : 0) == pdTRUE) {
      workerBusy = true;
      switch (j.t) {
        case J_SCAN: jobScan(); break;
        case J_CONNECT: jobConnect(j.arg); break;
        case J_RECONNECT:
          if (!linkUp && !bgConnecting && autoTaskHandle) triggerReconnect();
          setText(connInfo, linkUp ? "Already connected" : "Reconnecting to the saved dongle in the background...");
          break;
        case J_DISCONNECT: {
          RLock ol(opMux, 8000);
          disconnectDongle();
          addLog('i', "Disconnected from touchscreen");
          setText(connInfo, "Disconnected");
          break;
        }
        case J_FORGET:
          prefs.remove("type");
          prefs.remove("addr");
          setText(connInfo, "Saved dongle forgotten");
          break;
        case J_DTC: jobDtc(); break;
        case J_CLEAR: jobClear(); break;
      }
      workerBusy = false;
      continue;
    }
    if (idle) continue;
    if (needInit) {
      initAdapter();
      if (needInit) vTaskDelay(pdMS_TO_TICKS(3000));  // no ECU answer yet: ignition probably off
      continue;
    }
    if (activeTab == TAB_DASH) pollDash();
    else pollData();
  }
}

// ---------------------------------------------------------------- periodic status (runs in the LVGL task)

void refreshBridge() {
  String y;
  if (wifiMode == 0) y = "WiFi is OFF. Web page and TCP bridge are unavailable until it is switched back on.\n";
  else {
    if (WiFi.status() == WL_CONNECTED) y += "Home WiFi: " + WiFi.SSID() + "   IP " + WiFi.localIP().toString() + "   " + String(WiFi.RSSI()) + " dBm\n";
    else if (wifiMode == 1) y += "Home WiFi: not connected (" + String(WIFI_SSID) + ")\n";
    if (WiFi.getMode() & WIFI_AP) y += "Hotspot: " + String(AP_SSID) + "   password " + AP_PASS + "   IP " + WiFi.softAPIP().toString() + "\n";
    String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    y += "Web page: http://" + ip + "\nTCP bridge (Torque, python-OBD...): " + ip + " port 35000 - " +
         (bridgeEnabled ? (bridgeClient && bridgeClient.connected() ? "client " + bridgeClient.remoteIP().toString() + " connected" : String("waiting")) : String("OFF")) + "\n";
  }
  y += "Firmware " FW_VERSION ", up " + String(millis() / 60000) + " min, heap " + String(ESP.getFreeHeap() / 1024) + " KB";
  lv_label_set_text(bridgeInfo, y.c_str());
}

void statusTick(lv_timer_t *) {
  activeTab = lv_tabview_get_tab_act(tabs);
  String s = LV_SYMBOL_WIFI " ";
  if (wifiMode == 0) s += "off";
  else if (WiFi.status() == WL_CONNECTED) s += WiFi.localIP().toString();
  else if (WiFi.getMode() & WIFI_AP) s += "hotspot " + WiFi.softAPIP().toString();
  else s += "searching";
  s += "     " LV_SYMBOL_BLUETOOTH " ";
  RLock cl(connMux, 0);
  if (linkUp) s += "#4ade80 " + String(cl.ok ? devName : String("dongle")) + "#";
  else if (bgConnecting) s += "#fbbf24 reconnecting...#";
  else s += "#f87171 no dongle#";
  if (bridgeClient && bridgeClient.connected()) s += "     TCP " + bridgeClient.remoteIP().toString();
  if (workerBusy) s += "     " LV_SYMBOL_REFRESH;
  lv_label_set_text(statusLbl, s.c_str());

  bool stale = !linkUp || millis() - lastGood > 3000;
  if (stale) {
    lv_label_set_text(noDataLbl, !linkUp ? "NO DONGLE" : needInit ? "WAITING FOR ECU - IGNITION ON?" : "NO DATA");
    lv_obj_set_style_text_color(noDataLbl, lv_palette_main(LV_PALETTE_RED), 0);
  } else if (batteryV > 0) {
    lv_label_set_text_fmt(noDataLbl, LV_SYMBOL_CHARGE " %.1f V", batteryV);
    lv_obj_set_style_text_color(noDataLbl, batteryV < 11.8f ? lv_palette_main(LV_PALETTE_RED) : lv_color_white(), 0);
  } else lv_label_set_text(noDataLbl, "");
  lv_color_t dc = stale ? lv_palette_main(LV_PALETTE_GREY) : lv_color_white();
  lv_obj_set_style_text_color(speedo.digits, dc, 0);
  lv_obj_set_style_text_color(tacho.digits, dc, 0);

  if (activeTab == TAB_BRIDGE) refreshBridge();
}

void sleepNow() {  // caller holds lvMux
  backlight(false);
  asleep = true;
  addLog('i', "Screen off: no dongle and idle for " + String(SLEEP_AFTER_MS / 1000) + " s (touch to wake)");
}

void wakeNow(const char *why) {  // caller holds lvMux
  asleep = false;
  lastActivity = millis();
  swallowTouch = true;
  lv_obj_invalidate(lv_scr_act());
  lv_timer_handler();  // redraw before the light comes on
  backlight(true);
  addLog('i', String("Screen on: ") + why);
}

void lvTask(void *) {
  lastActivity = millis();
  for (;;) {
    if (asleep) {
      bool byTouch = touchDown();
      if (byTouch || linkUp) {
        RLock l(lvMux, UINT32_MAX);
        wakeNow(byTouch ? "touch" : "dongle connected");
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    {
      RLock l(lvMux, UINT32_MAX);
      lv_timer_handler();
      if (linkUp || workerBusy) lastActivity = millis();
      else if (millis() - lastActivity > SLEEP_AFTER_MS) sleepNow();
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

// ---------------------------------------------------------------- build UI

void confirm(const char *title, const char *text, const char *yes, void (*onYes)()) {
  static const char *btns[3];
  static void (*cb)();
  btns[0] = yes;
  btns[1] = "Cancel";
  btns[2] = "";
  cb = onYes;
  lv_obj_t *m = lv_msgbox_create(NULL, title, text, btns, false);
  lv_obj_set_width(m, 520);
  lv_obj_center(m);
  lv_obj_add_event_cb(m, [](lv_event_t *e) {
    lv_obj_t *mb = lv_event_get_current_target(e);
    bool ok = lv_msgbox_get_active_btn(mb) == 0;
    lv_msgbox_close(mb);
    if (ok && cb) cb();
  }, LV_EVENT_VALUE_CHANGED, NULL);
}

void buildUi() {
  lv_obj_t *scr = lv_scr_act();
  tabs = lv_tabview_create(scr, LV_DIR_TOP, 52);
  lv_obj_set_size(tabs, W, H - 28);
  lv_obj_set_pos(tabs, 0, 28);
  lv_obj_t *tDash = lv_tabview_add_tab(tabs, LV_SYMBOL_GPS " Dash");
  lv_obj_t *tFault = lv_tabview_add_tab(tabs, LV_SYMBOL_WARNING " Faults");
  lv_obj_t *tData = lv_tabview_add_tab(tabs, LV_SYMBOL_LIST " All data");
  lv_obj_t *tBridge = lv_tabview_add_tab(tabs, LV_SYMBOL_WIFI " Bridge");
  lv_obj_set_style_text_font(lv_tabview_get_tab_btns(tabs), &lv_font_montserrat_20, 0);
  lv_obj_clear_flag(lv_tabview_get_content(tabs), LV_OBJ_FLAG_SCROLLABLE);  // tap tabs; no accidental swipes

  statusLbl = lv_label_create(scr);
  lv_label_set_recolor(statusLbl, true);
  lv_obj_set_width(statusLbl, W - 16);
  lv_obj_set_pos(statusLbl, 8, 5);
  lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_DOT);

  // Dash: speedo left, tacho right
  lv_obj_set_style_pad_all(tDash, 0, 0);
  lv_obj_clear_flag(tDash, LV_OBJ_FLAG_SCROLLABLE);
  mkGauge(speedo, tDash, 220, 45, 4, -1, "km/h", false);
  lv_obj_align(speedo.meter, LV_ALIGN_LEFT_MID, 16, 0);
  mkGauge(tacho, tDash, 70, 36, 5, 62, "rpm", true);  // Z18XE limiter ~6,400 rpm: red from 6,200
  lv_obj_align(tacho.meter, LV_ALIGN_RIGHT_MID, -16, 0);
  // Small gauges in the gaps above and below where the dials curve apart
  // Centre column, top to bottom: caption, coolant gauge, value | check-engine | value, intake gauge, caption | battery
  mkMini(coolant, tDash, 40, 130, "COOLANT");
  lv_obj_align(coolant.unit, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_align(coolant.meter, LV_ALIGN_TOP_MID, 0, 28);
  lv_obj_align_to(coolant.digits, coolant.meter, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  mkMini(intake, tDash, -20, 80, "INTAKE AIR");
  lv_obj_align(intake.meter, LV_ALIGN_TOP_MID, 0, 254);
  lv_obj_align_to(intake.unit, intake.meter, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  lv_obj_align_to(intake.digits, intake.meter, LV_ALIGN_OUT_TOP_MID, 0, -2);
  milLbl = lv_label_create(tDash);
  lv_label_set_text(milLbl, LV_SYMBOL_WARNING "\nCHECK\nENGINE");
  lv_obj_set_style_text_align(milLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(milLbl, lv_palette_main(LV_PALETTE_AMBER), 0);
  lv_obj_set_style_text_font(milLbl, &lv_font_montserrat_14, 0);
  lv_obj_align(milLbl, LV_ALIGN_TOP_MID, 0, 160);
  lv_obj_add_flag(milLbl, LV_OBJ_FLAG_HIDDEN);
  noDataLbl = lv_label_create(tDash);
  lv_obj_set_style_text_color(noDataLbl, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_set_style_text_font(noDataLbl, &lv_font_montserrat_20, 0);
  lv_label_set_text(noDataLbl, "NO DONGLE");
  lv_obj_align(noDataLbl, LV_ALIGN_BOTTOM_MID, 0, -2);

  // Faults
  tabLayout(tFault);
  lv_obj_t *r = mkRow(tFault);
  mkBtn(r, LV_SYMBOL_REFRESH " Read codes", [](lv_event_t *) { post(J_DTC); });
  lv_obj_t *cb = mkBtn(r, LV_SYMBOL_TRASH " Clear codes", [](lv_event_t *) {
    confirm("Clear fault codes?",
            "Turns off the check-engine light and erases stored codes and freeze-frame data. Readiness monitors reset, "
            "so the car may fail an emissions check until it has been driven. The fault comes back if it is still present. "
            "Engine off, ignition on.",
            "Clear", [] { post(J_CLEAR); });
  });
  lv_obj_set_style_bg_color(cb, lv_palette_main(LV_PALETTE_RED), 0);
  faultLbl = mkText(tFault, &lv_font_montserrat_20);
  lv_label_set_text(faultLbl, "Tap Read codes (ignition on).");

  // All data
  tabLayout(tData);
  dataMsg = mkText(tData);
  dataTable = lv_table_create(tData);
  lv_table_set_col_cnt(dataTable, 3);
  lv_table_set_col_width(dataTable, 0, 90);
  lv_table_set_col_width(dataTable, 1, 330);
  lv_table_set_col_width(dataTable, 2, 330);
  lv_obj_set_width(dataTable, lv_pct(100));
  buildDataTable();

  // Bridge: WiFi, TCP bridge, dongle, display
  tabLayout(tBridge);
  mkHeading(tBridge, "WiFi and data bridge");
  r = mkRow(tBridge);
  lv_obj_t *wl = lv_label_create(r);
  lv_label_set_text(wl, "WiFi:");
  wifiDrop = lv_dropdown_create(r);
  lv_dropdown_set_options(wifiDrop, "Off\nHome WiFi (hotspot if not found)\nHotspot only");
  lv_dropdown_set_selected(wifiDrop, wifiMode);
  lv_obj_set_width(wifiDrop, 340);
  lv_obj_add_event_cb(wifiDrop, [](lv_event_t *e) {
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel == wifiMode) return;
    prefs.putUChar("wifimode", sel);
    confirm("Restart to apply?", "The WiFi setting is saved and takes effect after a restart (about 5 s; the dash comes straight back).",
            "Restart", [] { rebootPending = true; });
  }, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_t *bl = lv_label_create(r);
  lv_label_set_text(bl, "   TCP bridge:");
  bridgeSw = lv_switch_create(r);
  if (bridgeEnabled) lv_obj_add_state(bridgeSw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(bridgeSw, [](lv_event_t *e) {
    bridgeEnabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    prefs.putBool("bridge", bridgeEnabled);
    addLog('i', String("TCP bridge switched ") + (bridgeEnabled ? "on" : "off") + " from touchscreen");
  }, LV_EVENT_VALUE_CHANGED, NULL);
  bridgeInfo = mkText(tBridge);

  mkHeading(tBridge, "OBD dongle");
  r = mkRow(tBridge);
  mkBtn(r, LV_SYMBOL_BLUETOOTH " Scan", [](lv_event_t *) { post(J_SCAN); });
  mkBtn(r, LV_SYMBOL_REFRESH " Reconnect", [](lv_event_t *) { post(J_RECONNECT); });
  mkBtn(r, LV_SYMBOL_CLOSE " Disconnect", [](lv_event_t *) { post(J_DISCONNECT); });
  mkBtn(r, "Forget", [](lv_event_t *) { post(J_FORGET); });
  connInfo = mkText(tBridge);
  lv_label_set_text(connInfo, "The saved dongle reconnects automatically. Scan only to pair a different one (ignition on, no phone connected to it).");
  connList = lv_list_create(tBridge);
  lv_obj_set_size(connList, lv_pct(100), 180);

  mkHeading(tBridge, "Display");
  r = mkRow(tBridge);
  lv_obj_t *sw = lv_switch_create(r);
  if (rotated) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, [](lv_event_t *e) {
    rotated = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    prefs.putBool("rot", rotated);
    esp_lcd_panel_mirror(panel, rotated, rotated);
    lv_obj_invalidate(lv_scr_act());
  }, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_t *swl = lv_label_create(r);
  lv_label_set_text(swl, "Rotate 180\xC2\xB0");
  mkBtn(r, LV_SYMBOL_POWER " Restart", [](lv_event_t *) { rebootPending = true; });

  lv_timer_create(statusTick, 500, NULL);
}

// ---------------------------------------------------------------- entry point

void begin() {
  lvMux = xSemaphoreCreateRecursiveMutex();
  jobs = xQueueCreate(8, sizeof(Job));
  rotated = prefs.getBool("rot", true);  // matches the ESPHome setup on this board (display rotated 180)
  wifiMode = prefs.getUChar("wifimode", 1);
  bridgeEnabled = prefs.getBool("bridge", true);
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  ioExpanderInit();
  touchInit();
  if (!panelInit()) {
    addLog('e', "LCD panel init failed");
    return;
  }
  lv_init();
  static lv_disp_draw_buf_t db;
  const size_t px = W * 48;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&db, b1, b2, px);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = W;
  dd.ver_res = H;
  dd.flush_cb = flush;
  dd.draw_buf = &db;
  lv_disp_t *disp = lv_disp_drv_register(&dd);
  lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL), true, &lv_font_montserrat_16);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = touchRead;
  lv_indev_drv_register(&id);
  buildUi();
  xTaskCreatePinnedToCore(lvTask, "lvgl", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(worker, "lcdwork", 8192, NULL, 1, NULL, 1);
  addLog('i', "Dash UI started");
}

}  // namespace lcdui
