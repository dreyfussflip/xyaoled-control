#include "xyao_led.h"
#ifdef USE_ESP32
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "xyao_font.h"
#include <algorithm>

namespace esphome {
namespace xyao_led {

static const char *const TAG = "xyao_led";
static const uint16_t SERVICE_AE00 = 0xAE00;
static const uint16_t CHAR_AE01 = 0xAE01;
static const uint16_t CHAR_AE02 = 0xAE02;
static const int PANEL_H = 16;
static const int MAX_FRAMES = 77;  // one 0x0207 frame is capped at 10000 bytes

void XyaoLED::dump_config() {
  ESP_LOGCONFIG(TAG, "XYAO LED panel:");
  ESP_LOGCONFIG(TAG, "  Width: %d", this->width_);
}

void XyaoLED::enable_notify_(esp_gatt_if_t gattc_if) {
  esp_err_t err = esp_ble_gattc_register_for_notify(gattc_if, this->parent()->get_remote_bda(), this->ae02_handle_);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "register_for_notify failed: %s", esp_err_to_name(err));
  auto *cccd = this->parent()->get_config_descriptor(this->ae02_handle_);
  if (cccd == nullptr) {
    ESP_LOGW(TAG, "AE02 CCCD not found; sending init without notify");
    this->init_pending_ = true;
    this->init_at_ = millis() + 300;
    return;
  }
  uint16_t en = 0x0001;
  esp_ble_gattc_write_char_descr(gattc_if, this->parent()->get_conn_id(), cccd->handle, sizeof(en), (uint8_t *) &en,
                                 ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

void XyaoLED::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                  esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *c1 = this->parent()->get_characteristic(SERVICE_AE00, CHAR_AE01);
      auto *c2 = this->parent()->get_characteristic(SERVICE_AE00, CHAR_AE02);
      if (c1 == nullptr) {
        ESP_LOGE(TAG, "AE01 not found - is this an XYAO panel?");
        break;
      }
      this->ae01_handle_ = c1->handle;
      if (c2 != nullptr) {
        this->ae02_handle_ = c2->handle;
        this->enable_notify_(gattc_if);
      } else {
        this->init_pending_ = true;
        this->init_at_ = millis() + 300;
      }
      this->node_state = espbt::ClientState::ESTABLISHED;
      ESP_LOGI(TAG, "connected (AE01=0x%04X AE02=0x%04X)", this->ae01_handle_, this->ae02_handle_);
      break;
    }
    case ESP_GATTC_CFG_MTU_EVT:
      this->mtu_ = param->cfg_mtu.mtu;
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      // CCCD written - the panel is listening, send the handshake shortly
      this->init_pending_ = true;
      this->init_at_ = millis() + 300;
      break;
    case ESP_GATTC_NOTIFY_EVT: {
      auto &n = param->notify;
      if (n.handle != this->ae02_handle_ || n.value_len < 12)
        break;
      const uint8_t *v = n.value;
      // Status frame: 88 FF 00 [03|05] .. .. W(u16) H(u16) 04 POWER ...
      // (00 03 seen on 32x16 firmware with power 01/00, 00 05 on 64x16 firmware with power 01/02)
      if (v[0] == 0x88 && v[1] == 0xFF && v[2] == 0x00 && (v[3] == 0x03 || v[3] == 0x05)) {
        this->reported_width_ = v[6] | (v[7] << 8);
        this->reported_height_ = v[8] | (v[9] << 8);
        bool on = v[11] == 0x01;
        if (!this->have_state_ || on != this->power_state_)
          ESP_LOGI(TAG, "status: %dx%d power=%s", this->reported_width_, this->reported_height_, on ? "ON" : "OFF");
        if (!this->have_state_ && this->reported_width_ != 0 && this->reported_width_ != this->width_)
          ESP_LOGW(TAG, "panel reports width %d but width is configured as %d", this->reported_width_, this->width_);
        this->power_state_ = on;
        this->have_state_ = true;
      }
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT:
      this->ae01_handle_ = 0;
      this->ae02_handle_ = 0;
      this->have_state_ = false;
      this->init_done_ = false;
      this->init_pending_ = false;
      this->seq_ = 1;
      break;
    default:
      break;
  }
}

void XyaoLED::loop() {
  if (this->init_pending_ && (int32_t) (millis() - this->init_at_) >= 0 && this->ae01_handle_ != 0) {
    this->init_pending_ = false;
    ESP_LOGD(TAG, "sending init handshake");
    this->send_frames_hex(this->init_hex_);
    this->init_done_ = true;
    this->seq_ = 2;
  }
}

void XyaoLED::write_raw_(const std::vector<uint8_t> &data) {
  if (this->node_state != espbt::ClientState::ESTABLISHED || this->ae01_handle_ == 0) {
    ESP_LOGW(TAG, "not connected; dropping %u bytes", (unsigned) data.size());
    return;
  }
  size_t chunk = std::min<size_t>(this->mtu_ > 3 ? this->mtu_ - 3 : 20, 512);
  for (size_t i = 0; i < data.size(); i += chunk) {
    size_t n = std::min(chunk, data.size() - i);
    esp_err_t err = esp_ble_gattc_write_char((esp_gatt_if_t) this->parent()->get_gattc_if(),
                                             this->parent()->get_conn_id(), this->ae01_handle_, n,
                                             (uint8_t *) data.data() + i, ESP_GATT_WRITE_TYPE_NO_RSP,
                                             ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "write failed at %u: %s", (unsigned) i, esp_err_to_name(err));
    if (i + n < data.size())
      delay(20);  // let the BLE stack drain; the panel reassembles by the frame's length field
  }
}

void XyaoLED::send_frames_hex(const std::string &hex) {
  std::string cur;
  auto flush = [&]() {
    std::vector<uint8_t> frame;
    for (size_t i = 0; i + 1 < cur.size(); i += 2)
      frame.push_back((uint8_t) strtol(cur.substr(i, 2).c_str(), nullptr, 16));
    if (!frame.empty()) {
      this->write_raw_(frame);
      delay(150);
    }
    cur.clear();
  };
  for (char ch : hex) {
    if (ch == '|')
      flush();
    else if (isxdigit((unsigned char) ch))
      cur += ch;
  }
  flush();
}

std::vector<uint8_t> XyaoLED::make_frame_(uint16_t type, const std::vector<uint8_t> &params) {
  // 99 AA 00 2E FF 88 | len(u16 LE, whole frame) | type(u16 LE) | seq | params
  std::vector<uint8_t> f = {0x99, 0xAA, 0x00, 0x2E, 0xFF, 0x88, 0, 0, (uint8_t) (type & 0xFF), (uint8_t) (type >> 8),
                            this->seq_};
  f.insert(f.end(), params.begin(), params.end());
  f[6] = f.size() & 0xFF;
  f[7] = (f.size() >> 8) & 0xFF;
  return f;
}

void XyaoLED::set_power(bool on) {
  this->write_raw_(this->make_frame_(0x0011, {(uint8_t) (on ? 0x01 : 0x02), 0x01, 0x01}));
  this->seq_++;
}

void XyaoLED::set_brightness(int level) {
  level = std::max(0, std::min(100, level));
  this->write_raw_(this->make_frame_(0x0012, {0x01, (uint8_t) level}));
  this->seq_++;
}

void XyaoLED::clear() {
  this->write_raw_(this->make_frame_(0x0005, {0x01, 0x01, 0x00, 0x00, 0x00, 0x00}));
  delay(300);  // clear sends no completion notify
}

void XyaoLED::send_mono_(const std::vector<uint16_t> &cols, int n, uint8_t r, uint8_t g, uint8_t b, bool scroll,
                         int dur) {
  const int W = this->width_;
  const int bpr = W / 8;
  const int fb = PANEL_H * bpr;
  // 1-bit image payload: header(16) + RGB + mid(6) + n bitmaps (16 rows x W/8 bytes, LSB = leftmost pixel)
  std::vector<uint8_t> p = {0, 0, 0x00, 0x02, 0x00, 0x02, 0x01, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00};
  int sub = 12 + n * fb;
  p[0] = sub & 0xFF;
  p[1] = (sub >> 8) & 0xFF;
  p[9] = (uint8_t) n;
  p[11] = scroll ? 0x00 : 0x03;
  p.insert(p.end(), {r, g, b, 0x00, 0x00, 0xFF, (uint8_t) (scroll ? 1 : 0), (uint8_t) dur, 0x00});
  size_t base = p.size();
  p.resize(base + (size_t) n * fb, 0);
  for (int f = 0; f < n; f++)
    for (int row = 0; row < PANEL_H; row++)
      for (int c = 0; c < W; c++) {
        size_t col = (size_t) f * W + c;
        if (col < cols.size() && ((cols[col] >> row) & 1))
          p[base + f * fb + row * bpr + c / 8] |= 1 << (c % 8);
      }
  this->write_raw_(this->make_frame_(0x0204, {0xFF, 0x00}));
  delay(150);
  this->write_raw_(this->make_frame_(0x0207, p));
  delay(150);
  this->seq_++;
}

void XyaoLED::display_text(const std::string &text, uint8_t r, uint8_t g, uint8_t b, int mode, int speed,
                           const std::string &font, const std::string &effect) {
  if (!this->is_ready()) {
    ESP_LOGW(TAG, "display_text: panel not connected");
    return;
  }
  const int W = this->width_;
  const std::string &fname = font.empty() ? this->default_font_ : font;
  const XyaoFont *fnt = &XYAO_FONTS[0];
  for (int i = 0; i < XYAO_FONT_COUNT; i++)
    if (fname == XYAO_FONTS[i].name)
      fnt = &XYAO_FONTS[i];
  std::vector<uint16_t> txt;
  for (unsigned char ch : text) {
    if (ch < 32 || ch > 126)
      ch = '?';
    int i = ch - 32;
    for (int k = 0; k < fnt->widths[i]; k++)
      txt.push_back(fnt->columns[fnt->offsets[i] + k]);
  }
  bool flash = effect == "flash";
  bool scroll = mode == 2 || (mode == 0 && (int) txt.size() > W);
  int dur = speed > 0 ? std::min(speed, 255) : (flash && !scroll ? 0x14 : 0x3C);
  this->clear();  // otherwise each image is appended to the panel's looping playlist
  if (!scroll) {
    std::vector<uint16_t> cols(W, 0);
    int pad = (int) txt.size() < W ? (W - (int) txt.size()) / 2 : 0;
    for (size_t k = 0; k < txt.size() && pad + (int) k < W; k++)
      cols[pad + k] = txt[k];
    if (flash)
      cols.resize((size_t) W * 2, 0);  // frame 2 is blank: the panel alternates text / blank
    this->send_mono_(cols, flash ? 2 : 1, r, g, b, false, dur);
  } else {
    // a wide strip the panel scrolls itself: 2px lead + text + one screen of blank trail
    std::vector<uint16_t> cols(2, 0);
    cols.insert(cols.end(), txt.begin(), txt.end());
    int n = std::min(MAX_FRAMES, ((int) cols.size() + W + W - 1) / W);
    cols.resize((size_t) n * W, 0);
    std::vector<uint16_t> black(W, 0);
    this->send_mono_(black, 1, 0, 0, 0, false, 0x05);  // hides the previous screen scrolling out
    this->send_mono_(cols, n, r, g, b, true, dur);
  }
  ESP_LOGI(TAG, "text '%s' (%s, %d px, %s%s)", text.c_str(), fnt->name, (int) txt.size(), scroll ? "scroll" : "static",
           flash && !scroll ? ", flash" : "");
}

}  // namespace xyao_led
}  // namespace esphome
#endif
