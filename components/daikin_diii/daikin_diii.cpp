#include "daikin_diii.h"
#ifdef USE_CLIMATE
#include "daikin_climate.h"
#ifdef USE_SELECT
#include "airflow_select.h"
#endif
#endif

#include <cstring>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"


namespace esphome {
namespace daikin_diii {

static const char *const TAG = "daikin_diii";
static const char *const MAGIC = "1P2P";

void DaikinDIII::add_unit(uint8_t addr, uint8_t b1_index, uint8_t group) {
  Unit u{};
  u.addr = addr;
  u.group = group;
  // Without a B1 index no B1 frame will tell whether the group has a remote,
  // so the climate uses the return-air temperature right away.
  if (b1_index == 0xFF) u.remote_known = true;
  this->units_.push_back(u);
  if (b1_index != 0xFF) this->b1_to_diii_[b1_index] = addr;
}

Unit *DaikinDIII::find_unit_(uint8_t addr) {
  for (auto &u : this->units_)
    if (u.addr == addr) return &u;
  return nullptr;
}

#ifdef USE_SENSOR
void DaikinDIII::set_unit_sensor(uint8_t addr, uint8_t field, sensor::Sensor *s) {
  Unit *u = this->find_unit_(addr);
  if (u != nullptr && field < (uint8_t) UnitField::COUNT) u->sensors[field] = s;
}
#endif
#ifdef USE_SENSOR
void DaikinDIII::set_unit_diag_sensor(uint8_t addr, uint8_t kind, sensor::Sensor *s) {
  Unit *u = this->find_unit_(addr);
  if (u != nullptr && kind < (uint8_t) UnitDiag::COUNT) u->diag[kind] = s;
}
#endif
#ifdef USE_BINARY_SENSOR
void DaikinDIII::set_unit_binary_sensor(uint8_t addr, uint8_t kind, binary_sensor::BinarySensor *s) {
  Unit *u = this->find_unit_(addr);
  if (u != nullptr && kind < (uint8_t) UnitBinary::COUNT) u->binary[kind] = s;
}
#endif
#ifdef USE_TEXT_SENSOR
void DaikinDIII::set_unit_text_sensor(uint8_t addr, uint8_t kind, text_sensor::TextSensor *s) {
  Unit *u = this->find_unit_(addr);
  if (u != nullptr && kind < (uint8_t) UnitText::COUNT) u->text[kind] = s;
}
#endif

// Write budget tokens are kept in a preference so a restart does not refill
// them. On the ESP8266 a non-flash preference lives in RTC memory: it survives
// restarts and OTA, costs no flash wear, and is lost on power loss (then the
// budget starts full again).
struct BudgetSave {
  uint8_t n;
  uint8_t dst[16];
  uint16_t tokens[16];
};

void DaikinDIII::load_budget_() {
  this->budget_pref_ = global_preferences->make_preference<BudgetSave>(fnv1_hash("daikin_diii_budget"), false);
  BudgetSave s{};
  if (!this->budget_pref_.load(&s) || s.n > 16) return;
  uint32_t now = millis();
  for (uint8_t i = 0; i < s.n; i++) this->writer_.budget().restore(s.dst[i], s.tokens[i], now);
  ESP_LOGI(TAG, "Write budget restored for %u group(s)", s.n);
}

void DaikinDIII::save_budget_() {
  auto &b = this->writer_.budget();
  BudgetSave s{};
  for (size_t i = 0; i < b.size() && s.n < 16; i++, s.n++) {
    s.dst[s.n] = b.dst_at(i);
    s.tokens[s.n] = b.tokens_at(i);
  }
  this->budget_pref_.save(&s);
}

void DaikinDIII::setup() {
  this->load_budget_();
#ifdef DAIKIN_DIII_RAW_MQTT
  if (!this->mqtt_broker_.empty()) {
    IPAddress ip;
    if (ip.fromString(this->mqtt_broker_.c_str())) {
      this->mqtt_.setServer(ip, this->mqtt_port_);
    } else {
      this->mqtt_.setServer(this->mqtt_broker_.c_str(), this->mqtt_port_);
    }
    if (!this->mqtt_user_.empty()) this->mqtt_.setCredentials(this->mqtt_user_.c_str(), this->mqtt_password_.c_str());
    this->mqtt_.setClientId(this->mqtt_client_id_.c_str());
    this->mqtt_will_topic_ = this->mqtt_prefix_ + "L" + this->mqtt_suffix_;
    this->mqtt_.setWill(this->mqtt_will_topic_.c_str(), 1, true, "offline");
    this->mqtt_.setKeepAlive(30);
    this->mqtt_.onConnect([this](bool) {
      ESP_LOGI(TAG, "Raw MQTT connected to %s:%u", this->mqtt_broker_.c_str(), this->mqtt_port_);
      this->mqtt_.publish(this->mqtt_will_topic_.c_str(), 1, true, "online");
    });
    this->mqtt_.onDisconnect([](AsyncMqttClientDisconnectReason reason) {
      ESP_LOGW(TAG, "Raw MQTT disconnected (reason %d)", (int) reason);
    });
    this->mqtt_configured_ = true;
  }
#endif
  if (this->serial_enable_pin_ != nullptr) {
    this->serial_enable_pin_->setup();
    this->serial_enable_pin_->digital_write(true);  // allow ATmega serial I/O (bridge v1.2)
  }
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
    this->reset_pin_->pin_mode(gpio::FLAG_INPUT);  // released: ATmega runs
  }
#ifdef DAIKIN_DIII_ISP
  if (this->isp_mosi_pin_ != nullptr) {
    this->isp_mosi_pin_->setup();
    this->isp_mosi_pin_->digital_write(true);  // idle high (GPIO2: also keeps the blue LED off)
  }
  if (this->isp_miso_pin_ != nullptr) this->isp_miso_pin_->setup();
#endif
  delay(200);
  // Two dummy lines: P1P2Monitor ignores the first line after (re)start.
  this->write_str(MAGIC);
  this->write_str("* Dummy line 1.\n");
  this->write_str(MAGIC);
  this->write_str("* Dummy line 2.\n");
  this->ignore_next_line_ = true;
}

void DaikinDIII::reset_atmega() {
  if (this->reset_pin_ == nullptr) return;
  ESP_LOGW(TAG, "Hard resetting ATmega");
  this->reset_pin_->pin_mode(gpio::FLAG_OUTPUT);
  this->reset_pin_->digital_write(false);
  delay(1);
  this->reset_pin_->pin_mode(gpio::FLAG_INPUT);
  delay(200);
  this->ignore_next_line_ = true;
  this->write_str(MAGIC);
  this->write_str("* Dummy line 1.\n");
  this->write_str(MAGIC);
  this->write_str("* Dummy line 2.\n");
}

bool DaikinDIII::send_atmega_command(const std::string &cmd) {
  if (!this->writer_.enabled() || !::diii::atmega_command_allowed(cmd)) {
    this->note_write_refused(this->writer_.enabled() ? "ATmega command not allowed" : "writes disabled");
    ESP_LOGW(TAG, "ATmega command refused");
    return false;
  }
  ESP_LOGI(TAG, "ATmega command: %s", cmd.c_str());
  this->write_str(MAGIC);
  this->write_str(cmd.c_str());
  this->write_str("\n");
  return true;
}

bool DaikinDIII::check_enqueue_(::diii::WriteResult r, bool setting) {
  if (r == ::diii::WriteResult::OK) {
    ESP_LOGD(TAG, "Queued bus write (queue %u)", (unsigned) this->writer_.size());
    if (setting && this->writer_.budget().enabled()) this->save_budget_();
    return true;
  }
  const char *why = ::diii::write_result_str(r);
  this->note_write_refused(why);
  ESP_LOGW(TAG, "Write refused: %s", why);
  return false;
}

bool DaikinDIII::write_frame_hex(const std::string &hex) {
  bool setting = hex.size() >= 12 && (hex.compare(0, 2, "A5") == 0 || hex.compare(0, 2, "a5") == 0);
  return this->check_enqueue_(this->writer_.enqueue_hex(hex, millis()), setting);
}

bool DaikinDIII::enqueue_frame(const uint8_t *b, size_t n) {
  return this->check_enqueue_(this->writer_.enqueue(b, n, millis()), ::diii::is_setting_command(b, n));
}

void DaikinDIII::process_queue_() {
  std::string frame;
  if (!this->writer_.next(millis(), frame)) return;
  ESP_LOGI(TAG, "Bus write: %s", frame.c_str());
  this->write_str(MAGIC);
  this->write_str("W");
  this->write_str(frame.c_str());
  this->write_str("\n");
  this->publish_raw_('X', frame.c_str());
  this->set_last_write_(std::string("sent ") + frame);
}

bool DaikinDIII::command_op_(uint8_t group, uint8_t pp, uint8_t mm, uint8_t xx, uint8_t sp) {
  uint8_t f[25] = {0xA5, this->source_address_, group, 0x14, 0x14, 0x40, pp, mm, xx, 0x00, 0x00,
                   sp, (uint8_t) (sp ? 0x80 : 0x00), 0x00, 0x00, 0x80};
  return this->enqueue_frame(f, sizeof(f));
}

bool DaikinDIII::command_power(uint8_t group, bool on) { return this->command_op_(group, on ? 0x03 : 0x02, 0, 0, 0); }

bool DaikinDIII::command_mode(uint8_t group, uint8_t mode_low) {
  // payload[3] follows the mode: 00 fan, 01 heat, 02 cool (as sent by the iTM)
  return this->command_op_(group, 0x00, (uint8_t) (0x10 | mode_low), mode_low <= 2 ? mode_low : 0, 0);
}

bool DaikinDIII::command_setpoint(uint8_t group, uint8_t celsius) {
  if (celsius < 16) celsius = 16;
  if (celsius > 32) celsius = 32;
  return this->command_op_(group, 0x00, 0x00, 0x00, celsius);
}

bool DaikinDIII::command_fan(uint8_t group, uint8_t speed) {
  uint8_t f[::diii::FAN_AIRFLOW_LEN];
  if (!::diii::build_fan_airflow(f, this->source_address_, group, speed, -1)) return false;
  return this->enqueue_frame(f, sizeof(f));
}

bool DaikinDIII::command_airflow(uint8_t group, uint8_t direction) {
  uint8_t f[::diii::FAN_AIRFLOW_LEN];
  if (!::diii::build_fan_airflow(f, this->source_address_, group, -1, direction)) {
    this->note_write_refused("unknown airflow direction");
    return false;
  }
  return this->enqueue_frame(f, sizeof(f));
}

bool DaikinDIII::command_outlet_pair(uint8_t unit, ::diii::OutletPair pair, bool fixed) {
  uint8_t f[::diii::FAN_AIRFLOW_LEN];
  ::diii::build_outlet_pair(f, this->source_address_, unit, pair, fixed);
  return this->enqueue_frame(f, sizeof(f));
}

void DaikinDIII::set_unit_airflow_select(uint8_t addr, DaikinAirflowSelect *s) {
  Unit *u = this->find_unit_(addr);
  if (u != nullptr) u->airflow_select = s;
}

void DaikinDIII::set_unit_climate(uint8_t addr, DaikinClimate *c) {
  Unit *u = this->find_unit_(addr);
  if (u != nullptr) u->climate = c;
}

const Unit *DaikinDIII::get_unit(uint8_t addr) const {
  for (auto &u : this->units_)
    if (u.addr == addr) return &u;
  return nullptr;
}

void DaikinDIII::set_last_write_(const std::string &s) {
#ifdef USE_TEXT_SENSOR
  auto *t = this->system_text_[(int) SystemText::LAST_WRITE];
  if (t != nullptr) t->publish_state(s);
#endif
}

void DaikinDIII::set_raw_enabled(bool en) {
  if (en == this->raw_enabled_) return;
  if (!en) this->publish_raw_('E', "E NOTE: raw logging disabled by switch");
  this->raw_enabled_ = en;
  if (en) this->publish_raw_('E', "E NOTE: raw logging enabled by switch");
  ESP_LOGI(TAG, "Raw logging %s", en ? "enabled" : "disabled");
}

void DaikinDIII::publish_raw_(char kind, const char *payload) {
  if (!this->raw_enabled_) return;
#ifdef DAIKIN_DIII_RAW_MQTT
  if (this->mqtt_configured_) {
    if (this->mqtt_.connected()) {
      std::string topic = this->mqtt_prefix_ + kind + this->mqtt_suffix_;
      if (this->mqtt_.publish(topic.c_str(), 0, false, payload, strlen(payload)) != 0) {
        this->raw_published_++;
      } else {
        this->raw_dropped_++;
      }
    } else {
      this->raw_dropped_++;
    }
  }
#endif
}

#ifdef DAIKIN_DIII_RAW_MQTT
void DaikinDIII::mqtt_loop_() {
  if (!this->mqtt_configured_ || this->mqtt_.connected() || !network::is_connected()) return;
  uint32_t now = millis();
  if (this->mqtt_last_attempt_ != 0 && now - this->mqtt_last_attempt_ < 10000) return;
  this->mqtt_last_attempt_ = now;
  ESP_LOGD(TAG, "Connecting raw MQTT to %s:%u", this->mqtt_broker_.c_str(), this->mqtt_port_);
  this->mqtt_.connect();
}
#endif

void DaikinDIII::loop() {
#ifdef DAIKIN_DIII_RAW_MQTT
  this->mqtt_loop_();
#endif
#ifdef DAIKIN_DIII_ISP
  if (this->isp_server_ != nullptr) this->isp_loop_();
  if (this->isp_.in_progmode()) {
    // The ATmega is held in reset: no bus traffic, no writes. Drop whatever
    // the UART still holds; the link restarts after programming.
    while (this->available()) {
      uint8_t c;
      if (!this->read_byte(&c)) break;
    }
    return;
  }
#endif
  this->process_queue_();
  uint32_t now_ms = millis();
  if (now_ms - this->last_diag_ms_ > 30000) {
    this->last_diag_ms_ = now_ms;
    this->publish_diagnostics_();
  }
  if (now_ms - this->last_stats_ms_ > 300000) {
    this->last_stats_ms_ = now_ms;
    ESP_LOGI(TAG, "Stats: frames ok=%u err=%u, raw published=%u dropped=%u", this->frames_ok_, this->frames_err_,
             this->raw_published_, this->raw_dropped_);
  }
  // Bounded work per loop iteration to keep the ESP8266 responsive.
  int budget = 512;
  while (budget-- > 0 && this->available()) {
    uint8_t c;
    if (!this->read_byte(&c)) break;
    if (c == '\r') continue;
    if (c != '\n') {
      if (this->line_len_ < sizeof(this->line_buf_) - 1) this->line_buf_[this->line_len_++] = (char) c;
      continue;
    }
    this->line_buf_[this->line_len_] = 0;
    if (this->line_len_ > 0) {
      if (this->ignore_next_line_) {
        this->ignore_next_line_ = false;
      } else {
        this->handle_line_(this->line_buf_);
      }
    }
    this->line_len_ = 0;
  }
}

void DaikinDIII::handle_line_(const char *line) {
  switch (line[0]) {
    case 'R': {
      uint8_t buf[::diii::MAX_FRAME];
      size_t n = ::diii::parse_raw_line(line, buf, sizeof(buf), nullptr);
      if (n == 0) return;
      this->publish_raw_('R', line);
      if (n == 1) return;  // ACK (06) etc.
      if (::diii::frame_valid(buf, n)) {
        this->frames_ok_++;
        if (::diii::is_setting_command(buf, n)) {  // from the iTM, a remote or us
          Unit *u = this->find_unit_(buf[2]);
          if (u != nullptr) u->commands++;
        }
        ::diii::decode_frame(buf, n, this->b1_to_diii_, *this);
      } else {
        this->frames_err_++;
        ESP_LOGV(TAG, "Invalid frame: %s", line);
      }
      break;
    }
    case 'E':
      this->frames_err_++;
      this->publish_raw_('E', line);
      ESP_LOGD(TAG, "ATmega error frame: %s", line);
      break;
    case '*':
      ESP_LOGD(TAG, "ATmega: %s", line);
      if (strstr(line, "efus") != nullptr || strstr(line, "ollis") != nullptr) {
        this->publish_raw_('E', line);
        this->set_last_write_(line);
        uint8_t retry = 0;
        if (this->writer_.on_refused(millis(), &retry)) ESP_LOGW(TAG, "ATmega refused/collision, retry %u", retry);
      }
      break;
    default:
      ESP_LOGV(TAG, "ATmega line: %s", line);
      break;
  }
}

void DaikinDIII::notify_climate_(Unit *u) {
#ifdef USE_CLIMATE
  if (u->climate != nullptr) u->climate->update_from_unit(*u);
#endif
}

void DaikinDIII::notify_airflow_(Unit *u) {
#ifdef USE_SELECT
  if (u->airflow_select != nullptr) u->airflow_select->update_from_unit(*u);
#endif
}

void DaikinDIII::on_unit_airflow(uint8_t addr, uint8_t direction) {
  Unit *u = this->find_unit_(addr);
  if (u == nullptr) return;
  u->airflow = (int8_t) direction;
  // on every report (~1/min): a command the unit did not
  // apply leaves the bus value as it was, and the select must fall back to it
  // once PendingSetting's window has passed (update_from_unit only publishes
  // when the shown option differs, so this sends nothing)
  this->notify_airflow_(u);
}

// ---- DecoderSink ----

void DaikinDIII::on_unit_value(uint8_t addr, UnitField f, float v) {
  Unit *u = this->find_unit_(addr);
  if (u == nullptr) return;
  if (f == UnitField::SETPOINT && u->setpoint != v) {
    u->setpoint = v;
    this->notify_climate_(u);
  } else if (f == UnitField::ROOM && u->room != v) {
    u->room = v;
    this->notify_climate_(u);
  } else if (f == UnitField::REMOTE) {
    // 0x1610 (22.0625) is reported by units without their own remote: ignore it
    float r = (v == 22.0625f) ? NAN : v;
    bool first = !u->remote_known;
    u->remote_known = true;
    if (first || (!(r == u->remote) && !(std::isnan(r) && std::isnan(u->remote)))) {
      u->remote = r;
      this->notify_climate_(u);
    }
  }
#ifdef USE_SENSOR
  auto *s = u->sensors[(int) f];
  if (s != nullptr && (!s->has_state() || s->get_raw_state() != v)) s->publish_state(v);
#endif
}

void DaikinDIII::on_unit_power(uint8_t addr, bool on) {
  Unit *u = this->find_unit_(addr);
  if (u == nullptr) return;
  if (u->power != (on ? 1 : 0)) {
    u->power = on ? 1 : 0;
    this->notify_climate_(u);
  }
#ifdef USE_BINARY_SENSOR
  auto *s = u->binary[(int) UnitBinary::POWER];
  if (s != nullptr) s->publish_state(on);
#endif
}

static const char *mode_name(uint8_t m) {
  switch (m) {
    case 0x60: return "fan_only";
    case 0x61: return "heat";
    case 0x62: return "cool";
    case 0x63: return "auto";
    case 0x67: return "dry";
    default: return "unknown";
  }
}

static const char *fan_name(uint8_t s) {
  switch (s) {
    case 0: return "auto";
    case 1: return "low";
    case 3: return "medium";
    case 5: return "high";
    default: return "unknown";
  }
}

void DaikinDIII::on_unit_mode(uint8_t addr, uint8_t mode) {
  Unit *u = this->find_unit_(addr);
  if (u == nullptr) return;
  if (u->mode != mode) {
    u->mode = mode;
    this->notify_climate_(u);
  }
#ifdef USE_TEXT_SENSOR
  auto *s = u->text[(int) UnitText::MODE];
  if (s != nullptr) {
    const char *n = mode_name(mode);
    if (!s->has_state() || s->get_state() != n) s->publish_state(n);
  }
#endif
}

void DaikinDIII::on_unit_fan(uint8_t addr, uint8_t speed) {
  Unit *u = this->find_unit_(addr);
  if (u == nullptr) return;
  if (u->fan != (int8_t) speed) {
    u->fan = (int8_t) speed;
    this->notify_climate_(u);
  }
#ifdef USE_TEXT_SENSOR
  auto *s = u->text[(int) UnitText::FAN];
  if (s != nullptr) {
    const char *n = fan_name(speed);
    if (!s->has_state() || s->get_state() != n) s->publish_state(n);
  }
#endif
}

void DaikinDIII::on_unit_status(uint8_t addr, uint8_t bits) {
  Unit *u = this->find_unit_(addr);
  if (u == nullptr) return;
  // bit 0 = on. Use it until the first F5 31B frame (power) has been seen,
  // so power/climate are known within ~1 min after a reboot.
  if (u->power == -1) this->on_unit_power(addr, (bits & 0x01) != 0);
#ifdef USE_BINARY_SENSOR
  auto *s = u->binary[(int) UnitBinary::HEATING];
  if (s != nullptr) s->publish_state((bits & 0x20) != 0);  // (hyp) 0x20 = heating demand / thermo on
#endif
}

void DaikinDIII::publish_diagnostics_() {
#ifdef USE_SENSOR
  auto pub = [](sensor::Sensor *s, float v) {
    if (s != nullptr && (!s->has_state() || s->get_raw_state() != v)) s->publish_state(v);
  };
  pub(this->diag_sensors_[(int) DiagSensor::FRAMES_OK], (float) this->frames_ok_);
  pub(this->diag_sensors_[(int) DiagSensor::FRAMES_ERR], (float) this->frames_err_);
  pub(this->diag_sensors_[(int) DiagSensor::WRITE_QUEUE], (float) this->writer_.size());
  uint32_t now = millis();
  for (auto &u : this->units_) {
    pub(u.diag[(int) UnitDiag::COMMANDS], (float) u.commands);
    if (u.diag[(int) UnitDiag::BUDGET] != nullptr && this->writer_.budget().enabled())
      pub(u.diag[(int) UnitDiag::BUDGET], (float) this->writer_.budget().available(u.addr, now));
  }
#endif
#ifdef USE_BINARY_SENSOR
  auto *b = this->system_binary_[(int) SystemBinary::RAW_MQTT];
#ifdef DAIKIN_DIII_RAW_MQTT
  if (b != nullptr) b->publish_state(this->mqtt_configured_ && this->mqtt_.connected());
#else
  if (b != nullptr) b->publish_state(false);
#endif
#endif
}

void DaikinDIII::on_system_value(SystemField f, float v) {
#ifdef USE_SENSOR
  auto *s = this->system_sensors_[(int) f];
  if (s != nullptr && (!s->has_state() || s->get_raw_state() != v)) s->publish_state(v);
#endif
}

void DaikinDIII::on_compressor(uint8_t state) {
#ifdef USE_BINARY_SENSOR
  auto *s = this->system_binary_[(int) SystemBinary::COMPRESSOR];
  if (s != nullptr) s->publish_state(state == 2);
#endif
}

void DaikinDIII::on_clock(uint8_t hour, uint8_t minute) {
#ifdef USE_TEXT_SENSOR
  auto *s = this->system_text_[(int) SystemText::CLOCK];
  if (s != nullptr) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", hour, minute);
    s->publish_state(buf);
  }
#endif
}

void DaikinDIII::on_unit_role(uint8_t addr, bool master) {
  Unit *u = this->find_unit_(addr);
  if (u == nullptr || u->role == (master ? 1 : 0)) return;
  u->role = master ? 1 : 0;
  // The bus says which unit is the group master (it has the remote). A climate
  // is only correct on a master, so flag a unit table that disagrees.
  bool configured_master = u->group == u->addr;
  if (master != configured_master)
    ESP_LOGW(TAG, "Unit 0x%02X is a group %s on the bus, but configured as %s (group 0x%02X): check the unit table",
             addr, master ? "master" : "member", configured_master ? "master" : "member", u->group);
}

void DaikinDIII::on_defrost(bool active) {
#ifdef USE_BINARY_SENSOR
  auto *s = this->system_binary_[(int) SystemBinary::DEFROST];
  if (s != nullptr) s->publish_state(active);
#endif
}

void DaikinDIII::dump_config() {
  ESP_LOGCONFIG(TAG, "Daikin DIII-NET (F1F2) via P1P2Monitor ATmega");
  ESP_LOGCONFIG(TAG, "  Units: %u", (unsigned) this->units_.size());
  for (auto &u : this->units_) ESP_LOGCONFIG(TAG, "    0x%02X (group 0x%02X)", u.addr, u.group);
  if (!this->mqtt_broker_.empty())
    ESP_LOGCONFIG(TAG, "  Raw lines via MQTT %s:%u -> %s{R,E,X}%s", this->mqtt_broker_.c_str(), this->mqtt_port_,
                  this->mqtt_prefix_.c_str(), this->mqtt_suffix_.c_str());
  ESP_LOGCONFIG(TAG, "  Writes enabled: %s (source address 0x%02X)", YESNO(this->writer_.enabled()), this->source_address_);
  ESP_LOGCONFIG(TAG, "  Write budget per group: %u at once", this->writer_.budget().burst());
  LOG_PIN("  Reset pin: ", this->reset_pin_);
  LOG_PIN("  Serial enable pin: ", this->serial_enable_pin_);
#ifdef DAIKIN_DIII_ISP
  ESP_LOGCONFIG(TAG, "  ATmega programming: port %u, open for %u min at a time (SCK = serial enable pin)",
                this->isp_port_, (unsigned) (this->isp_window_ms_ / 60000));
  LOG_PIN("    MOSI pin: ", this->isp_mosi_pin_);
  LOG_PIN("    MISO pin: ", this->isp_miso_pin_);
#endif
}

#ifdef DAIKIN_DIII_ISP
// --- ATmega programming ------------------------------------------------------
// avrdude talks STK500v1 to us over TCP (avr_isp.h turns that into ISP
// instructions); the ATmega is programmed over bit-banged SPI on the bridge's
// pins: SCK = serial enable (GPIO15), MOSI = GPIO2, MISO = GPIO0, RESET = GPIO5.

void DaikinDIII::BridgeIsp::begin() {
  this->hub_->serial_enable_pin_->digital_write(false);  // SCK low before reset
  this->hub_->isp_mosi_pin_->pin_mode(gpio::FLAG_OUTPUT);
  this->hub_->isp_mosi_pin_->digital_write(false);
  this->hub_->isp_miso_pin_->pin_mode(gpio::FLAG_INPUT);
}

void DaikinDIII::BridgeIsp::end() {
  this->hub_->isp_mosi_pin_->digital_write(true);
  this->hub_->serial_enable_pin_->digital_write(true);  // back to: ATmega serial enabled
}

void DaikinDIII::BridgeIsp::set_reset(bool held) {
  if (held) {
    this->hub_->reset_pin_->pin_mode(gpio::FLAG_OUTPUT);
    this->hub_->reset_pin_->digital_write(false);
  } else {
    this->hub_->reset_pin_->pin_mode(gpio::FLAG_INPUT);  // released, pulled up on the board
  }
}

uint8_t DaikinDIII::BridgeIsp::transfer(uint8_t out) {
  // SPI mode 0, MSB first, ~200 kHz (the ATmega needs SCK phases > 2 CPU cycles)
  uint8_t in = 0;
  for (uint8_t i = 0; i < 8; i++) {
    this->hub_->isp_mosi_pin_->digital_write((out & 0x80) != 0);
    out <<= 1;
    delayMicroseconds(2);
    this->hub_->serial_enable_pin_->digital_write(true);
    delayMicroseconds(2);
    in = (uint8_t) ((in << 1) | (this->hub_->isp_miso_pin_->digital_read() ? 1 : 0));
    this->hub_->serial_enable_pin_->digital_write(false);
  }
  return in;
}

void DaikinDIII::BridgeIsp::delay_ms(uint32_t ms) { delay(ms); }

void DaikinDIII::set_isp_enabled(bool en) {
  if (en) {
    if (this->isp_server_ != nullptr) return;
    if (this->isp_mosi_pin_ == nullptr || this->isp_miso_pin_ == nullptr || this->serial_enable_pin_ == nullptr ||
        this->reset_pin_ == nullptr) {
      ESP_LOGE(TAG, "ATmega programming needs reset, serial enable, MOSI and MISO pins");
      if (this->isp_state_cb_) this->isp_state_cb_(false);
      return;
    }
    this->isp_server_ = new WiFiServer(this->isp_port_);
    this->isp_server_->begin();
    this->isp_opened_ms_ = millis();
    ESP_LOGW(TAG, "ATmega programming port %u open for %u min: avrdude -c avrisp -P net:<bridge>:%u -p m328p ...",
             this->isp_port_, (unsigned) (this->isp_window_ms_ / 60000), this->isp_port_);
    return;
  }
  if (this->isp_server_ == nullptr) return;
  this->isp_close_client_("programming port closed");
  this->isp_server_->stop();
  delete this->isp_server_;
  this->isp_server_ = nullptr;
  ESP_LOGI(TAG, "ATmega programming port closed");
}

void DaikinDIII::isp_loop_() {
  const uint32_t now = millis();
  if (!this->isp_client_.connected()) {
    if (this->isp_.in_progmode() || this->isp_was_programming_) this->isp_close_client_("client disconnected");
    WiFiClient c = this->isp_server_->accept();
    if (c) {
      this->isp_client_ = c;
      this->isp_client_.setNoDelay(true);
      this->isp_last_rx_ms_ = now;
      ESP_LOGW(TAG, "ATmega programming: client %s connected", c.remoteIP().toString().c_str());
    }
  } else {
    WiFiClient extra = this->isp_server_->accept();  // one programmer at a time
    if (extra) extra.stop();
    uint8_t buf[256];
    int avail = this->isp_client_.available();
    if (avail > 0) {
      int n = this->isp_client_.read(buf, avail < (int) sizeof(buf) ? (size_t) avail : sizeof(buf));
      if (n > 0) {
        this->isp_last_rx_ms_ = now;
        std::string out;
        this->isp_.feed(buf, (size_t) n, out);
        if (!out.empty()) this->isp_client_.write((const uint8_t *) out.data(), out.size());
      }
    }
    // avrdude never pauses this long; a crashed client must not keep the ATmega in reset
    if (this->isp_.in_progmode() && now - this->isp_last_rx_ms_ > 30000) this->isp_close_client_("client idle for 30 s");
  }

  const bool programming = this->isp_.in_progmode();
  if (programming && !this->isp_was_programming_) {
    ESP_LOGW(TAG, "ATmega in programming mode (held in reset), %s", this->isp_.sync_ok() ? "in sync" : "NOT in sync");
  } else if (!programming && this->isp_was_programming_) {
    this->isp_after_programming_();
  }
  this->isp_was_programming_ = programming;

  // Close the port after the window; a session still running gets 5 more minutes.
  const uint32_t open_for = now - this->isp_opened_ms_;
  if ((open_for > this->isp_window_ms_ && !programming) || open_for > this->isp_window_ms_ + 300000) {
    ESP_LOGW(TAG, "ATmega programming window over");
    this->set_isp_enabled(false);
    if (this->isp_state_cb_) this->isp_state_cb_(false);
  }
}

void DaikinDIII::isp_close_client_(const char *why) {
  if (this->isp_.in_progmode()) ESP_LOGW(TAG, "ATmega programming aborted: %s", why);
  this->isp_.abort();  // leaves programming mode and releases reset
  if (this->isp_client_) this->isp_client_.stop();
  if (this->isp_was_programming_) {
    this->isp_was_programming_ = false;
    this->isp_after_programming_();
  }
}

// The ATmega has just left reset: restart the serial link as at boot.
void DaikinDIII::isp_after_programming_() {
  ESP_LOGW(TAG, "ATmega released after programming (%u flash pages written); restarting the serial link",
           (unsigned) this->isp_.pages_written());
  this->line_len_ = 0;
  delay(200);
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c)) break;
  }
  this->write_str(MAGIC);
  this->write_str("* Dummy line 1.\n");
  this->write_str(MAGIC);
  this->write_str("* Dummy line 2.\n");
  this->ignore_next_line_ = true;
}
#endif

}  // namespace daikin_diii
}  // namespace esphome
