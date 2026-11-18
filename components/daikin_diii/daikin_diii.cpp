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

void DaikinDIII::add_unit(uint8_t addr, uint8_t b1_index, uint8_t group, bool hydrobox) {
  Unit unit{};
  unit.addr = addr;
  unit.group = group;
  unit.hydrobox = hydrobox;
  // Without a B1 index no B1 frame will tell whether the group has a remote,
  // so the climate uses the return-air temperature right away.
  if (b1_index == 0xFF) unit.remote_known = true;
  this->units_.push_back(unit);
  if (b1_index != 0xFF) this->b1_to_diii_[b1_index] = addr;
}

Unit *DaikinDIII::find_unit_(uint8_t addr) {
  for (auto &unit : this->units_)
    if (unit.addr == addr) return &unit;
  return nullptr;
}

#ifdef USE_SENSOR
void DaikinDIII::set_unit_sensor(uint8_t addr, uint8_t field, sensor::Sensor *sensor) {
  Unit *unit = this->find_unit_(addr);
  if (unit != nullptr && field < (uint8_t) UnitField::COUNT) unit->sensors[field] = sensor;
}
#endif
#ifdef USE_SENSOR
void DaikinDIII::set_unit_diag_sensor(uint8_t addr, uint8_t kind, sensor::Sensor *sensor) {
  Unit *unit = this->find_unit_(addr);
  if (unit != nullptr && kind < (uint8_t) UnitDiag::COUNT) unit->diag[kind] = sensor;
}
#endif
#ifdef USE_BINARY_SENSOR
void DaikinDIII::set_unit_binary_sensor(uint8_t addr, uint8_t kind, binary_sensor::BinarySensor *sensor) {
  Unit *unit = this->find_unit_(addr);
  if (unit != nullptr && kind < (uint8_t) UnitBinary::COUNT) unit->binary[kind] = sensor;
}
#endif
#ifdef USE_TEXT_SENSOR
void DaikinDIII::set_unit_text_sensor(uint8_t addr, uint8_t kind, text_sensor::TextSensor *sensor) {
  Unit *unit = this->find_unit_(addr);
  if (unit != nullptr && kind < (uint8_t) UnitText::COUNT) unit->text[kind] = sensor;
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
  BudgetSave saved{};
  if (!this->budget_pref_.load(&saved) || saved.n > 16) return;
  uint32_t now = millis();
  for (uint8_t i = 0; i < saved.n; i++) this->writer_.budget().restore(saved.dst[i], saved.tokens[i], now);
  ESP_LOGI(TAG, "Write budget restored for %u group(s)", saved.n);
}

void DaikinDIII::save_budget_() {
  auto &budget = this->writer_.budget();
  BudgetSave saved{};
  for (size_t i = 0; i < budget.size() && saved.n < 16; i++, saved.n++) {
    saved.dst[saved.n] = budget.dst_at(i);
    saved.tokens[saved.n] = budget.tokens_at(i);
  }
  this->budget_pref_.save(&saved);
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

bool DaikinDIII::check_enqueue_(::diii::WriteResult result, bool setting) {
  if (result == ::diii::WriteResult::OK) {
    ESP_LOGD(TAG, "Queued bus write (queue %u)", (unsigned) this->writer_.size());
    if (setting && this->writer_.budget().enabled()) this->save_budget_();
    return true;
  }
  const char *why = ::diii::write_result_str(result);
  this->note_write_refused(why);
  ESP_LOGW(TAG, "Write refused: %s", why);
  return false;
}

bool DaikinDIII::write_frame_hex(const std::string &hex) {
  bool setting = hex.size() >= 12 && (hex.compare(0, 2, "A5") == 0 || hex.compare(0, 2, "a5") == 0);
  return this->check_enqueue_(this->writer_.enqueue_hex(hex, millis()), setting);
}

bool DaikinDIII::enqueue_frame(const uint8_t *frame, size_t length) {
  return this->check_enqueue_(this->writer_.enqueue(frame, length, millis()),
                              ::diii::is_setting_command(frame, length));
}

void DaikinDIII::process_queue_() {
  std::string frame;
  uint8_t retry = 0;
  switch (this->writer_.check_echo(millis(), frame, &retry)) {
    case ::diii::EchoResult::RETRY:
      ESP_LOGW(TAG, "No echo on the bus for %s, retry %u", frame.c_str(), retry);
      this->set_last_write_(std::string("no echo, retry ") + std::to_string(retry) + ": " + frame);
      break;
    case ::diii::EchoResult::LOST:
      ESP_LOGE(TAG, "Bus write lost (no echo after %u retries): %s", ::diii::WriteQueue::MAX_RETRIES, frame.c_str());
      this->set_last_write_(std::string("lost ") + frame);
      break;
    case ::diii::EchoResult::NONE:
      break;
  }
  if (!this->writer_.next(millis(), frame)) return;
  ESP_LOGI(TAG, "Bus write: %s", frame.c_str());
  this->write_str(MAGIC);
  this->write_str("W");
  this->write_str(frame.c_str());
  this->write_str("\n");
  this->publish_raw_('X', frame.c_str());
  this->set_last_write_(std::string("sent ") + frame);
}

bool DaikinDIII::command_op_(uint8_t group, uint8_t power_byte, uint8_t mode_byte, uint8_t mode_detail,
                             uint8_t setpoint) {
  // the hydrobox takes the same command in a longer frame (diii_writer.h)
  const Unit *unit = this->get_unit(group);
  const bool hydrobox = unit != nullptr && unit->hydrobox;
  if (hydrobox && setpoint != 0) {
    this->note_write_refused("hydrobox setpoint unknown");
    return false;
  }
  uint8_t frame[::diii::HYDROBOX_OPERATION_LEN];
  size_t length = ::diii::build_operation(frame, this->source_address_, group, hydrobox, power_byte, mode_byte,
                                          mode_detail, setpoint);
  return this->enqueue_frame(frame, length);
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
  uint8_t frame[::diii::FAN_AIRFLOW_LEN];
  if (!::diii::build_fan_airflow(frame, this->source_address_, group, speed, -1)) return false;
  return this->enqueue_frame(frame, sizeof(frame));
}

bool DaikinDIII::command_airflow(uint8_t group, uint8_t direction) {
  uint8_t frame[::diii::FAN_AIRFLOW_LEN];
  if (!::diii::build_fan_airflow(frame, this->source_address_, group, -1, direction)) {
    this->note_write_refused("unknown airflow direction");
    return false;
  }
  return this->enqueue_frame(frame, sizeof(frame));
}

bool DaikinDIII::command_outlet_pair(uint8_t unit, ::diii::OutletPair pair, bool fixed) {
  uint8_t frame[::diii::FAN_AIRFLOW_LEN];
  ::diii::build_outlet_pair(frame, this->source_address_, unit, pair, fixed);
  return this->enqueue_frame(frame, sizeof(frame));
}

void DaikinDIII::set_unit_airflow_select(uint8_t addr, DaikinAirflowSelect *airflow_select) {
  Unit *unit = this->find_unit_(addr);
  if (unit != nullptr) unit->airflow_select = airflow_select;
}

void DaikinDIII::set_unit_climate(uint8_t addr, DaikinClimate *climate) {
  Unit *unit = this->find_unit_(addr);
  if (unit != nullptr) unit->climate = climate;
}

const Unit *DaikinDIII::get_unit(uint8_t addr) const {
  for (auto &unit : this->units_)
    if (unit.addr == addr) return &unit;
  return nullptr;
}

void DaikinDIII::set_last_write_(const std::string &text) {
#ifdef USE_TEXT_SENSOR
  auto *sensor = this->system_text_[(int) SystemText::LAST_WRITE];
  if (sensor != nullptr) sensor->publish_state(text);
#endif
}

void DaikinDIII::set_raw_enabled(bool enabled) {
  if (enabled == this->raw_enabled_) return;
  if (!enabled) this->publish_raw_('E', "E NOTE: raw logging disabled by switch");
  this->raw_enabled_ = enabled;
  if (enabled) this->publish_raw_('E', "E NOTE: raw logging enabled by switch");
  ESP_LOGI(TAG, "Raw logging %s", enabled ? "enabled" : "disabled");
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
      uint8_t discarded;
      if (!this->read_byte(&discarded)) break;
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
    uint8_t received;
    if (!this->read_byte(&received)) break;
    if (received == '\r') continue;
    if (received != '\n') {
      if (this->line_len_ < sizeof(this->line_buf_) - 1) this->line_buf_[this->line_len_++] = (char) received;
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
      uint8_t frame[::diii::MAX_FRAME];
      size_t length = ::diii::parse_raw_line(line, frame, sizeof(frame), nullptr);
      if (length == 0) return;
      this->publish_raw_('R', line);
      if (length == 1) return;  // ACK (06) etc.
      if (::diii::frame_valid(frame, length)) {
        this->frames_ok_++;
        // our own frame read back from the bus: only now is it written
        if (frame[0] == 0xA5 && this->writer_.on_bus_frame(frame, length)) {
          std::string hex;
          char hex_byte[3];
          for (size_t i = 0; i < length; i++) {
            snprintf(hex_byte, sizeof(hex_byte), "%02X", frame[i]);
            hex += hex_byte;
          }
          ESP_LOGD(TAG, "Bus write confirmed: %s", hex.c_str());
          this->set_last_write_(std::string("on bus ") + hex);
        }
        if (::diii::is_setting_command(frame, length)) {  // from the iTM, a remote or us
          Unit *unit = this->find_unit_(frame[2]);
          if (unit != nullptr) unit->commands++;
        }
        ::diii::decode_frame(frame, length, this->b1_to_diii_, *this);
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
        const uint32_t lost_before = this->writer_.lost();
        if (this->writer_.on_refused(millis(), &retry)) {
          ESP_LOGW(TAG, "ATmega refused/collision, retry %u", retry);
        } else if (this->writer_.lost() != lost_before) {
          ESP_LOGE(TAG, "Bus write lost (refused after %u retries)", ::diii::WriteQueue::MAX_RETRIES);
        }
      }
      break;
    default:
      ESP_LOGV(TAG, "ATmega line: %s", line);
      break;
  }
}

void DaikinDIII::notify_climate_(Unit *unit) {
#ifdef USE_CLIMATE
  if (unit->climate != nullptr) unit->climate->update_from_unit(*unit);
#endif
}

void DaikinDIII::notify_airflow_(Unit *unit) {
#ifdef USE_SELECT
  if (unit->airflow_select != nullptr) unit->airflow_select->update_from_unit(*unit);
#endif
}

void DaikinDIII::on_unit_airflow(uint8_t addr, uint8_t direction) {
  Unit *unit = this->find_unit_(addr);
  if (unit == nullptr) return;
  unit->airflow = (int8_t) direction;
  // on every report (~1/min): a command the unit did not
  // apply leaves the bus value as it was, and the select must fall back to it
  // once PendingSetting's window has passed (update_from_unit only publishes
  // when the shown option differs, so this sends nothing)
  this->notify_airflow_(unit);
}

// ---- DecoderSink ----

void DaikinDIII::on_unit_value(uint8_t addr, UnitField field, float value) {
  Unit *unit = this->find_unit_(addr);
  if (unit == nullptr) return;
  if (field == UnitField::SETPOINT && unit->setpoint != value) {
    unit->setpoint = value;
    this->notify_climate_(unit);
  } else if (field == UnitField::ROOM && unit->room != value) {
    unit->room = value;
    this->notify_climate_(unit);
  } else if (field == UnitField::REMOTE) {
    // 0x1610 (22.0625) is reported by units without their own remote: ignore it
    float remote = (value == 22.0625f) ? NAN : value;
    bool first = !unit->remote_known;
    unit->remote_known = true;
    if (first || (!(remote == unit->remote) && !(std::isnan(remote) && std::isnan(unit->remote)))) {
      unit->remote = remote;
      this->notify_climate_(unit);
    }
  }
#ifdef USE_SENSOR
  auto *sensor = unit->sensors[(int) field];
  if (sensor != nullptr && (!sensor->has_state() || sensor->get_raw_state() != value)) sensor->publish_state(value);
#endif
}

void DaikinDIII::on_unit_power(uint8_t addr, bool on) {
  Unit *unit = this->find_unit_(addr);
  if (unit == nullptr) return;
  if (unit->power != (on ? 1 : 0)) {
    unit->power = on ? 1 : 0;
    this->notify_climate_(unit);
  }
#ifdef USE_BINARY_SENSOR
  auto *sensor = unit->binary[(int) UnitBinary::POWER];
  if (sensor != nullptr) sensor->publish_state(on);
#endif
}

static const char *mode_name(uint8_t mode) {
  switch (mode) {
    case 0x60: return "fan_only";
    case 0x61: return "heat";
    case 0x62: return "cool";
    case 0x63: return "auto";
    case 0x67: return "dry";
    default: return "unknown";
  }
}

static const char *fan_name(uint8_t speed) {
  switch (speed) {
    case 0: return "auto";
    case 1: return "low";
    case 3: return "medium";
    case 5: return "high";
    default: return "unknown";
  }
}

void DaikinDIII::on_unit_mode(uint8_t addr, uint8_t mode) {
  Unit *unit = this->find_unit_(addr);
  if (unit == nullptr) return;
  if (unit->mode != mode) {
    unit->mode = mode;
    this->notify_climate_(unit);
  }
#ifdef USE_TEXT_SENSOR
  auto *sensor = unit->text[(int) UnitText::MODE];
  if (sensor != nullptr) {
    const char *name = mode_name(mode);
    if (!sensor->has_state() || sensor->get_state() != name) sensor->publish_state(name);
  }
#endif
}

void DaikinDIII::on_unit_fan(uint8_t addr, uint8_t speed) {
  Unit *unit = this->find_unit_(addr);
  if (unit == nullptr) return;
  if (unit->fan != (int8_t) speed) {
    unit->fan = (int8_t) speed;
    this->notify_climate_(unit);
  }
#ifdef USE_TEXT_SENSOR
  auto *sensor = unit->text[(int) UnitText::FAN];
  if (sensor != nullptr) {
    const char *name = fan_name(speed);
    if (!sensor->has_state() || sensor->get_state() != name) sensor->publish_state(name);
  }
#endif
}

void DaikinDIII::on_unit_status(uint8_t addr, uint8_t bits) {
  Unit *unit = this->find_unit_(addr);
  if (unit == nullptr) return;
  // bit 0 = on. Use it until the first F5 31B frame (power) has been seen,
  // so power/climate are known within ~1 min after a reboot.
  if (unit->power == -1) this->on_unit_power(addr, (bits & 0x01) != 0);
  const bool action_bits_changed = unit->status < 0 || ((unit->status ^ bits) & 0x24) != 0;
  unit->status = bits;
#ifdef USE_BINARY_SENSOR
  auto *sensor = unit->binary[(int) UnitBinary::HEATING];
  if (sensor != nullptr) sensor->publish_state((bits & 0x20) != 0);  // 0x20 = demand (thermo on)
#endif
  // the group's climate shows the demand of all its units as hvac_action
  if (action_bits_changed) {
    Unit *master = this->find_unit_(unit->group);
    if (master != nullptr) this->notify_climate_(master);
  }
}

int DaikinDIII::group_demand(uint8_t group, bool *fan_running) const {
  int demand = -1;
  bool fan = false;
  for (const auto &unit : this->units_) {
    if (unit.group != group || unit.status < 0) continue;
    if (demand < 0) demand = 0;
    if (unit.status & 0x20) demand = 1;
    if (unit.status & 0x04) fan = true;
  }
  if (fan_running != nullptr) *fan_running = fan;
  return demand;
}

void DaikinDIII::publish_diagnostics_() {
#ifdef USE_SENSOR
  auto publish = [](sensor::Sensor *sensor, float value) {
    if (sensor != nullptr && (!sensor->has_state() || sensor->get_raw_state() != value)) sensor->publish_state(value);
  };
  publish(this->diag_sensors_[(int) DiagSensor::FRAMES_OK], (float) this->frames_ok_);
  publish(this->diag_sensors_[(int) DiagSensor::FRAMES_ERR], (float) this->frames_err_);
  publish(this->diag_sensors_[(int) DiagSensor::WRITE_QUEUE], (float) this->writer_.size());
  publish(this->diag_sensors_[(int) DiagSensor::WRITES_LOST], (float) this->writer_.lost());
  uint32_t now = millis();
  for (auto &unit : this->units_) {
    publish(unit.diag[(int) UnitDiag::COMMANDS], (float) unit.commands);
    if (unit.diag[(int) UnitDiag::BUDGET] != nullptr && this->writer_.budget().enabled())
      publish(unit.diag[(int) UnitDiag::BUDGET], (float) this->writer_.budget().available(unit.addr, now));
  }
#endif
#ifdef USE_BINARY_SENSOR
  auto *sensor = this->system_binary_[(int) SystemBinary::RAW_MQTT];
#ifdef DAIKIN_DIII_RAW_MQTT
  if (sensor != nullptr) sensor->publish_state(this->mqtt_configured_ && this->mqtt_.connected());
#else
  if (sensor != nullptr) sensor->publish_state(false);
#endif
#endif
}

void DaikinDIII::on_system_value(SystemField field, float value) {
#ifdef USE_SENSOR
  auto *sensor = this->system_sensors_[(int) field];
  if (sensor != nullptr && (!sensor->has_state() || sensor->get_raw_state() != value)) sensor->publish_state(value);
#endif
}

void DaikinDIII::on_compressor(uint8_t state) {
#ifdef USE_BINARY_SENSOR
  auto *sensor = this->system_binary_[(int) SystemBinary::COMPRESSOR];
  if (sensor != nullptr) sensor->publish_state(state == 2);
#endif
}

void DaikinDIII::on_clock(uint8_t hour, uint8_t minute) {
#ifdef USE_TEXT_SENSOR
  auto *sensor = this->system_text_[(int) SystemText::CLOCK];
  if (sensor != nullptr) {
    char clock_text[8];
    snprintf(clock_text, sizeof(clock_text), "%02u:%02u", hour, minute);
    sensor->publish_state(clock_text);
  }
#endif
}

void DaikinDIII::on_unit_role(uint8_t addr, bool master) {
  Unit *unit = this->find_unit_(addr);
  if (unit == nullptr || unit->role == (master ? 1 : 0)) return;
  unit->role = master ? 1 : 0;
  // The bus says which unit is the group master (it has the remote). A climate
  // is only correct on a master, so flag a unit table that disagrees.
  bool configured_master = unit->group == unit->addr;
  if (master != configured_master)
    ESP_LOGW(TAG, "Unit 0x%02X is a group %s on the bus, but configured as %s (group 0x%02X): check the unit table",
             addr, master ? "master" : "member", configured_master ? "master" : "member", unit->group);
}

void DaikinDIII::on_defrost(bool active) {
#ifdef USE_BINARY_SENSOR
  auto *sensor = this->system_binary_[(int) SystemBinary::DEFROST];
  if (sensor != nullptr) sensor->publish_state(active);
#endif
}

void DaikinDIII::dump_config() {
  ESP_LOGCONFIG(TAG, "Daikin DIII-NET (F1F2) via P1P2Monitor ATmega");
  ESP_LOGCONFIG(TAG, "  Units: %u", (unsigned) this->units_.size());
  for (auto &unit : this->units_) ESP_LOGCONFIG(TAG, "    0x%02X (group 0x%02X)", unit.addr, unit.group);
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

void DaikinDIII::set_isp_enabled(bool enabled) {
  if (enabled) {
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
    WiFiClient client = this->isp_server_->accept();
    if (client) {
      this->isp_client_ = client;
      this->isp_client_.setNoDelay(true);
      this->isp_last_rx_ms_ = now;
      ESP_LOGW(TAG, "ATmega programming: client %s connected", client.remoteIP().toString().c_str());
    }
  } else {
    WiFiClient extra = this->isp_server_->accept();  // one programmer at a time
    if (extra) extra.stop();
    uint8_t data[256];
    int pending = this->isp_client_.available();
    if (pending > 0) {
      int length = this->isp_client_.read(data, pending < (int) sizeof(data) ? (size_t) pending : sizeof(data));
      if (length > 0) {
        this->isp_last_rx_ms_ = now;
        std::string reply;
        this->isp_.feed(data, (size_t) length, reply);
        if (!reply.empty()) this->isp_client_.write((const uint8_t *) reply.data(), reply.size());
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
    uint8_t discarded;
    if (!this->read_byte(&discarded)) break;
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
