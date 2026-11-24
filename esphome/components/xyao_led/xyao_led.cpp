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

static const XyaoFont *find_font(const std::string &name) {
  for (int i = 0; i < XYAO_FONT_COUNT; i++)
    if (name == XYAO_FONTS[i].name)
      return &XYAO_FONTS[i];
  return &XYAO_FONTS[0];
}

static const XyaoIcon *find_icon(const std::string &name) {
  for (int i = 0; i < XYAO_ICON_COUNT; i++)
    if (name == XYAO_ICONS[i].name)
      return &XYAO_ICONS[i];
  return nullptr;
}

struct Rgb {
  uint8_t r, g, b;
  bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
};

// "[green]" / "[#00ff00]" at text[p]: returns the length of the tag (0 if none) and sets *out.
static size_t parse_colour_tag(const std::string &text, size_t p, Rgb *out) {
  if (text[p] != '[')
    return 0;
  size_t q = text.find(']', p);
  if (q == std::string::npos || q - p > 9)
    return 0;
  std::string name = text.substr(p + 1, q - p - 1);
  for (auto &ch : name)
    ch = tolower(ch);
  static const struct {
    const char *name;
    Rgb c;
  } COLOURS[] = {{"red", {255, 0, 0}},     {"green", {0, 255, 0}},     {"blue", {0, 0, 255}},
                 {"yellow", {255, 200, 0}}, {"white", {255, 255, 255}}, {"cyan", {0, 255, 255}},
                 {"magenta", {255, 0, 255}}, {"orange", {255, 100, 0}}};
  if (name.size() == 7 && name[0] == '#') {
    uint32_t v = strtoul(name.c_str() + 1, nullptr, 16);
    *out = {(uint8_t) (v >> 16), (uint8_t) ((v >> 8) & 0xFF), (uint8_t) (v & 0xFF)};
    return q - p + 1;
  }
  for (auto &c : COLOURS)
    if (name == c.name) {
      *out = c.c;
      return q - p + 1;
    }
  return 0;
}

// One rendered column: lit rows, plus the colour of the top (rows 0-7) and bottom (rows 8-15) halves.
struct Col {
  uint16_t bits;
  Rgb top, bot;
};

// Renders one line. ":name:" inserts an icon; ":name!:" blinks it (in the "off" phase it is drawn as
// "<name>_off" if that icon exists, otherwise blank). "[colour]" changes the colour from there on.
static Rgb rgb_of(uint32_t v) { return {(uint8_t) (v >> 16), (uint8_t) ((v >> 8) & 0xFF), (uint8_t) (v & 0xFF)}; }

// Renders one line. ":name:" inserts an icon; ":name!:" blinks it (in odd phases it is drawn as "<name>_off" if
// that icon exists, otherwise blank); if only "<name>_1", "<name>_2", ... exist the icon is animated (frame =
// phase). "[colour]" changes the colour from there on. Icons with fixed colours keep them.
static std::vector<Col> render_line(const std::string &text, const XyaoFont *fnt, bool allow_icons, int text_shift,
                                    int phase, bool *animated, Rgb *colour, bool *has_colour) {
  std::vector<Col> out;
  bool after_icon = false;  // icons end with a blank column: skip the next glyph's leading blank one
  for (size_t p = 0; p < text.size(); p++) {
    size_t tag = parse_colour_tag(text, p, colour);
    if (tag) {
      *has_colour = true;
      p += tag - 1;
      continue;
    }
    if (allow_icons && text[p] == ':') {
      size_t q = text.find(':', p + 1);
      if (q != std::string::npos && q - p > 1 && q - p <= 20) {
        std::string name = text.substr(p + 1, q - p - 1);
        bool blink = !name.empty() && name.back() == '!';
        if (blink)
          name.pop_back();
        const XyaoIcon *icon = find_icon(name);
        const XyaoIcon *draw = icon;
        if (icon == nullptr && (icon = find_icon(name + "_1")) != nullptr) {
          int n = 1;
          while (n < 16 && find_icon(name + "_" + std::to_string(n + 1)) != nullptr)
            n++;
          *animated = true;
          draw = find_icon(name + "_" + std::to_string(phase % n + 1));
        }
        if (icon != nullptr) {
          if (blink) {
            *animated = true;
            if (phase % 2)
              draw = find_icon(name + "_off");
          }
          Rgb top = icon->top ? rgb_of(icon->top) : *colour, bot = icon->bottom ? rgb_of(icon->bottom) : *colour;
          if (icon->top || icon->bottom)
            *has_colour = true;
          for (int k = 0; k < icon->width; k++)
            out.push_back({(uint16_t) (draw != nullptr && k < draw->width ? XYAO_ICON_COLS[draw->offset + k] : 0), top,
                           bot});
          after_icon = true;
          p = q;
          continue;
        }
      }
    }
    unsigned char ch = text[p];
    if (ch < 32 || ch > 126)
      ch = '?';
    int i = ch - 32;
    int k0 = (after_icon && ch != ' ' && fnt->widths[i] > 1 && fnt->columns[fnt->offsets[i]] == 0) ? 1 : 0;
    after_icon = false;
    for (int k = k0; k < fnt->widths[i]; k++)
      out.push_back({(uint16_t) (fnt->columns[fnt->offsets[i] + k] << text_shift), *colour, *colour});
  }
  return out;
}

// Renders one page: one line, or two lines ('|' / newline) in the 8-row "small" font.
static std::vector<Col> render_page(const std::string &text, const XyaoFont *base, Rgb colour, const char **used,
                                    int phase, bool *animated, bool *has_colour) {
  std::vector<std::string> lines(1);
  for (char ch : text) {
    if ((ch == '|' || ch == '\n') && lines.size() < 2)
      lines.emplace_back();
    else if (ch != '\r')
      lines.back() += ch;
  }
  bool two = lines.size() == 2;
  const XyaoFont *fnt = two ? find_font("small") : base;
  bool small = std::string(fnt->name) == "small";
  *used = fnt->name;
  std::vector<std::vector<Col>> rendered;
  size_t w = 0;
  for (size_t li = 0; li < lines.size(); li++) {
    // icons are 16 rows tall, so they only appear on single-line pages; small text is centred vertically
    // line 2 starts on row 9, leaving row 8 blank so line 1's descenders (g, y, p) never touch it
    int shift = two ? (li == 0 ? 0 : 9) : (small ? 4 : 0);
    auto cols = render_line(lines[li], fnt, !two, shift, phase, animated, &colour, has_colour);
    w = std::max(w, cols.size());
    rendered.push_back(cols);
  }
  std::vector<Col> page(w, Col{0, {0, 0, 0}, {0, 0, 0}});  // overlay the lines, each centred
  for (size_t li = 0; li < rendered.size(); li++) {
    auto &line = rendered[li];
    size_t pad = (w - line.size()) / 2;
    for (size_t k = 0; k < line.size(); k++) {
      Col &d = page[pad + k];
      d.bits |= line[k].bits;
      if (!two || li == 0)
        d.top = line[k].top;
      if (!two || li == 1)
        d.bot = line[k].bot;
    }
  }
  // trim blank columns at the edges (glyph spacing) so fit checks and centring use the visible width
  while (!page.empty() && page.back().bits == 0)
    page.pop_back();
  size_t lead = 0;
  while (lead < page.size() && page[lead].bits == 0)
    lead++;
  page.erase(page.begin(), page.begin() + lead);
  return page;
}

static std::vector<uint16_t> bits_of(const std::vector<Col> &cols) {
  std::vector<uint16_t> out;
  for (auto &c : cols)
    out.push_back(c.bits);
  return out;
}

// ---- minimal GIF89a encoder (8-colour palette, "uncompressed" LZW) ----
class GifWriter {
 public:
  GifWriter(int w, int h, const std::vector<Rgb> &palette) : w_(w), h_(h) {
    const char *sig = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    put16(w);
    put16(h);
    out.push_back(0xF2);  // global colour table, 3 bits/colour channel res, 8 entries
    out.push_back(0);
    out.push_back(0);
    for (int i = 0; i < 8; i++) {
      Rgb c = i < (int) palette.size() ? palette[i] : Rgb{0, 0, 0};
      out.push_back(c.r);
      out.push_back(c.g);
      out.push_back(c.b);
    }
    const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
                            0x03, 0x01, 0x00, 0x00, 0x00};
    out.insert(out.end(), loop, loop + sizeof(loop));
  }
  void frame(const std::vector<uint8_t> &px, int delay_cs) {
    const uint8_t gce[] = {0x21, 0xF9, 0x04, 0x04, (uint8_t) (delay_cs & 0xFF), (uint8_t) (delay_cs >> 8), 0x00, 0x00};
    out.insert(out.end(), gce, gce + sizeof(gce));
    out.push_back(0x2C);
    put16(0);
    put16(0);
    put16(w_);
    put16(h_);
    out.push_back(0x00);
    out.push_back(3);  // LZW minimum code size
    // 4-bit codes: clear(8), end(9); emit a clear before the table would grow past 15
    std::vector<uint8_t> data;
    uint32_t acc = 0;
    int nbits = 0;
    auto emit = [&](uint8_t code) {
      acc |= (uint32_t) code << nbits;
      nbits += 4;
      while (nbits >= 8) {
        data.push_back(acc & 0xFF);
        acc >>= 8;
        nbits -= 8;
      }
    };
    int since_clear = 0;
    emit(8);
    for (uint8_t p : px) {
      if (since_clear == 5) {
        emit(8);
        since_clear = 0;
      }
      emit(p & 7);
      since_clear++;
    }
    emit(9);
    if (nbits > 0)
      data.push_back(acc & 0xFF);
    for (size_t i = 0; i < data.size(); i += 255) {
      size_t n = std::min<size_t>(255, data.size() - i);
      out.push_back((uint8_t) n);
      out.insert(out.end(), data.begin() + i, data.begin() + i + n);
    }
    out.push_back(0x00);
  }
  std::vector<uint8_t> finish() {
    out.push_back(0x3B);
    return out;
  }
  std::vector<uint8_t> out;

 private:
  void put16(int v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
  }
  int w_, h_;
};

void XyaoLED::send_gif_(const std::vector<uint8_t> &gif, int frames) {
  const size_t MAX_0207 = 10000 - 11 - 13, MAX_0209 = 10000 - 11 - 5;
  this->write_raw_(this->make_frame_(0x0204, {0xFF, 0x00}));
  delay(150);
  std::vector<uint8_t> p = {(uint8_t) (gif.size() & 0xFF), (uint8_t) (gif.size() >> 8)};
  if (frames <= 1) {
    p.insert(p.end(), {0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00});
    p.insert(p.end(), gif.begin(), gif.end());
    this->write_raw_(this->make_frame_(0x0207, p));
  } else {
    // animations are always split across 0x0207 + at least one 0x0209
    size_t split = gif.size() > MAX_0207 ? MAX_0207 : std::max<size_t>(1, gif.size() / 2);
    p.insert(p.end(), {0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t) frames, 0x00, 0x03, 0x00});
    p.insert(p.end(), gif.begin(), gif.begin() + split);
    this->write_raw_(this->make_frame_(0x0207, p));
    for (size_t i = split; i < gif.size(); i += MAX_0209) {
      size_t n = std::min(MAX_0209, gif.size() - i);
      delay(150);
      std::vector<uint8_t> q = {(uint8_t) (n & 0xFF), (uint8_t) (n >> 8), 0x00, 0x02, 0x00};
      q.insert(q.end(), gif.begin() + i, gif.begin() + i + n);
      this->write_raw_(this->make_frame_(0x0209, q));
    }
  }
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
  const Rgb base_colour{r, g, b};
  const XyaoFont *base = find_font(font.empty() ? this->default_font_ : font);
  struct Page {
    std::string text;
    const XyaoFont *fnt;
    std::vector<Col> on;
    bool blink;  // animated: blinking or multi-frame icons
  };
  // '~' splits the message into pages the panel shows in turn
  std::vector<std::string> parts(1);
  for (char ch : text) {
    if (ch == '~')
      parts.emplace_back();
    else
      parts.back() += ch;
  }
  const char *used = base->name;
  std::vector<Page> pages;
  bool any_blink = false, any_colour = false;
  for (auto &part : parts) {
    Page pg{part, base, {}, false};
    pg.on = render_page(part, base, base_colour, &used, 0, &pg.blink, &any_colour);
    // auto-fit: if a page is wider than the panel, try narrower fonts before clipping / scrolling
    if ((int) pg.on.size() > W && mode != 2) {
      // take the first narrower font that fits; if none does, the narrowest result (least clipping)
      std::vector<Col> best = pg.on;
      const XyaoFont *best_fnt = base;
      const char *best_used = used;
      for (const char *alt : {"narrow", "tall", "slim", "small"}) {
        const char *u2 = used;
        bool bl = false, co = false;
        auto p2 = render_page(part, find_font(alt), base_colour, &u2, 0, &bl, &co);
        if (p2.size() < best.size()) {
          best = p2;
          best_fnt = find_font(alt);
          best_used = u2;
        }
        if ((int) p2.size() <= W)
          break;
      }
      // a single animated page can't scroll, and pages never scroll: use the narrowest rendering
      if ((int) best.size() <= W || pg.blink || parts.size() > 1) {
        pg.on = best;
        pg.fnt = best_fnt;
        used = best_used;
      }
    }
    if (pg.blink)
      any_blink = true;
    pages.push_back(pg);
  }
  bool flash = effect == "flash";
  bool multi = pages.size() > 1;
  bool scroll = !multi && !any_blink && (mode == 2 || (mode == 0 && (int) pages[0].on.size() > W));
  this->clear();  // otherwise each image is appended to the panel's looping playlist

  if (scroll) {
    // a wide strip the panel scrolls itself (one colour): 2px lead + text + one screen of blank trail
    std::vector<uint16_t> cols(2, 0);
    auto bits = bits_of(pages[0].on);
    cols.insert(cols.end(), bits.begin(), bits.end());
    int n = std::min(MAX_FRAMES, ((int) cols.size() + W + W - 1) / W);
    cols.resize((size_t) n * W, 0);
    std::vector<uint16_t> black(W, 0);
    this->send_mono_(black, 1, 0, 0, 0, false, 0x05);  // hides the previous screen scrolling out
    this->send_mono_(cols, n, r, g, b, true, speed > 0 ? std::min(speed, 255) : 0x3C);
  } else if (any_blink || any_colour) {
    // full-colour GIF: per-pixel colour and exact timing (centiseconds)
    std::vector<Rgb> palette = {{0, 0, 0}};
    auto idx = [&](const Rgb &c) -> uint8_t {
      for (size_t i = 0; i < palette.size(); i++)
        if (palette[i] == c)
          return i;
      if (palette.size() < 8) {
        palette.push_back(c);
        return palette.size() - 1;
      }
      return 1;
    };
    auto raster = [&](const std::vector<Col> &pg) {
      std::vector<uint8_t> px((size_t) W * PANEL_H, 0);
      int pad = (int) pg.size() < W ? (W - (int) pg.size()) / 2 : 0;
      for (size_t k = 0; k < pg.size() && pad + (int) k < W; k++)
        for (int y = 0; y < PANEL_H; y++)
          if ((pg[k].bits >> y) & 1)
            px[y * W + pad + k] = idx(y < 8 ? pg[k].top : pg[k].bot);
      return px;
    };
    // build the frame list first so the palette is complete before the header is written
    const int page_cs = speed > 0 ? speed * 10 : 250;  // page time; speed is in tenths of a second
    const int blink_cs = 25;                           // 2 blinks per second
    std::vector<std::pair<std::vector<uint8_t>, int>> frames;
    std::vector<uint8_t> blank((size_t) W * PANEL_H, 0);
    for (auto &pg : pages) {
      if (pg.blink) {
        // re-render the page for each time step; icons pick their blink phase / animation frame
        int steps = std::max(2, page_cs / blink_cs);
        steps += steps % 2;
        for (int k = 0; k < steps; k++) {
          bool d1 = false, d2 = false;
          const char *u3 = used;
          frames.push_back({raster(render_page(pg.text, pg.fnt, base_colour, &u3, k, &d1, &d2)), blink_cs});
        }
      } else if (flash) {
        frames.push_back({raster(pg.on), 50});
        frames.push_back({blank, 50});
      } else {
        frames.push_back({raster(pg.on), page_cs});
      }
    }
    if (frames.size() > 255)
      frames.resize(255);
    GifWriter gw(W, PANEL_H, palette);
    for (auto &f : frames)
      gw.frame(f.first, f.second);
    auto gif = gw.finish();
    if (gif.size() > 0xFFFF) {
      ESP_LOGW(TAG, "GIF too large (%u bytes)", (unsigned) gif.size());
      return;
    }
    this->send_gif_(gif, (int) frames.size());
    ESP_LOGD(TAG, "gif %u bytes, %u frames, %u colours", (unsigned) gif.size(), (unsigned) frames.size(),
             (unsigned) palette.size());
  } else {
    // one colour: 1-bit image, one frame per page (+ a blank frame after each when flashing)
    int per = flash ? 2 : 1;
    int n = std::min(MAX_FRAMES, (int) pages.size() * per);
    std::vector<uint16_t> cols((size_t) n * W, 0);
    for (size_t pi = 0; pi < pages.size() && (int) (pi * per) < n; pi++) {
      auto bits = bits_of(pages[pi].on);
      int pad = (int) bits.size() < W ? (W - (int) bits.size()) / 2 : 0;
      for (size_t k = 0; k < bits.size() && pad + (int) k < W; k++)
        cols[pi * per * W + pad + k] = bits[k];
    }
    int dur = speed > 0 ? std::min(speed, 255) : (flash && !multi ? 0x14 : 0x3C);
    this->send_mono_(cols, n, r, g, b, false, dur);
  }
  ESP_LOGI(TAG, "text '%s' (%s, %d page%s, %s%s%s)", text.c_str(), used, (int) pages.size(), multi ? "s" : "",
           scroll ? "scroll" : "static", any_blink ? ", blink" : "", any_colour ? ", colour" : "");
}

}  // namespace xyao_led
}  // namespace esphome
#endif
