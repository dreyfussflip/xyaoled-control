#pragma once
#ifdef USE_ESP32
#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include <esp_gattc_api.h>
#include <string>
#include <vector>

namespace esphome {
namespace xyao_led {

namespace espbt = esphome::esp32_ble_tracker;

// ESPHome BLE client for XYAO-LED ("XyaoLED_xxxx") flexible matrix panels.
// Protocol: service AE00, write AE01 (write-without-response), notify AE02 (see PROTOCOL.md).
// Every session must start with an init/handshake frame or the panel ignores all commands;
// this component sends it automatically on every (re)connect.
class XyaoLED : public Component, public ble_client::BLEClientNode {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_width(int width) { this->width_ = width; }
  void set_init_hex(const std::string &hex) { this->init_hex_ = hex; }

  // Actions
  void set_power(bool on);
  void set_brightness(int level);  // 0..100
  void clear();                    // clears screen and the on-device playlist
  // mode: 0 = auto (scroll if wider than the panel), 1 = static, 2 = scroll. speed: 0 = default.
  // font: mono, sans, bold, italic, narrow, large ("" = default). effect: "none" or "flash" (static text).
  void display_text(const std::string &text, uint8_t r, uint8_t g, uint8_t b, int mode = 0, int speed = 0,
                    const std::string &font = "", const std::string &effect = "");
  void set_default_font(const std::string &font) { this->default_font_ = font; }
  void send_frames_hex(const std::string &hex);  // raw command frames, '|' separated

  bool is_ready() const { return this->ae01_handle_ != 0 && this->init_done_; }
  bool is_on() const { return this->power_state_; }
  bool has_state() const { return this->have_state_; }
  int panel_width() const { return this->width_; }

 protected:
  std::vector<uint8_t> make_frame_(uint16_t type, const std::vector<uint8_t> &params);
  void write_raw_(const std::vector<uint8_t> &data);
  void send_mono_(const std::vector<uint16_t> &cols, int frames, uint8_t r, uint8_t g, uint8_t b, bool scroll,
                  int dur);
  void enable_notify_(esp_gatt_if_t gattc_if);
  void send_gif_(const std::vector<uint8_t> &gif, int frames);

  uint16_t ae01_handle_{0};
  uint16_t ae02_handle_{0};
  uint16_t mtu_{23};
  uint8_t seq_{1};
  int width_{32};
  int reported_width_{0};
  int reported_height_{0};
  bool power_state_{false};
  bool have_state_{false};
  bool init_pending_{false};
  bool init_done_{false};
  uint32_t init_at_{0};
  std::string init_hex_;
  std::string default_font_{"mono"};
};

}  // namespace xyao_led
}  // namespace esphome
#endif
