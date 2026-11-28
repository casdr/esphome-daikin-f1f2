#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "esphome/components/uart/uart.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "diii_decoder.h"
#include "diii_writer.h"

#ifdef DAIKIN_DIII_RAW_MQTT
#include <AsyncMqttClient.h>
#endif

#ifdef DAIKIN_DIII_ISP
#include <ESP8266WiFi.h>
#include "avr_isp.h"
#endif

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace daikin_diii {

using ::diii::DecoderSink;
using ::diii::SystemField;
using ::diii::UnitField;

enum class UnitBinary : uint8_t { POWER = 0, HEATING = 1, COUNT };
enum class UnitText : uint8_t { MODE = 0, FAN = 1, COUNT };
enum class SystemBinary : uint8_t { COMPRESSOR = 0, RAW_MQTT = 1, DEFROST = 2, OIL_RETURN = 3, COUNT };
enum class DiagSensor : uint8_t { FRAMES_OK = 0, FRAMES_ERR = 1, WRITE_QUEUE = 2, WRITES_LOST = 3, COUNT };
// per group master: setting commands seen on the bus (any source), write budget left
enum class UnitDiag : uint8_t { COMMANDS = 0, BUDGET = 1, COUNT };
enum class SystemText : uint8_t { CLOCK = 0, LAST_WRITE = 1, COUNT };

class DaikinClimate;
class DaikinAirflowSelect;

struct Unit {
  uint8_t addr;
  uint8_t group;
  bool hydrobox{false};  // takes 35-byte operation commands (diii_writer.h)
  // last state seen on the bus
  int8_t power{-1};       // F5 31B byte 6
  uint8_t mode{0};        // 0x60 fan, 0x61 heat, 0x62 cool, ...
  int8_t fan{-1};         // 0 auto, 1 low, 3 medium, 5 high
  int8_t airflow{-1};     // F5 23B @16 low nibble: 0-4 position, 7 swing
  float setpoint{NAN};
  float room{NAN};
  float remote{NAN};   // remote-controller sensor (middle of the room), NAN if no remote
  bool remote_known{false};  // a B1 frame has told whether the group has a remote
  DaikinClimate *climate{nullptr};
  DaikinAirflowSelect *airflow_select{nullptr};
  uint32_t commands{0};  // A5 40/41 frames to this address seen on the bus since boot
  int8_t role{-1};       // F5 31B @18: 1 group master, 0 member, -1 not seen yet
  int16_t status{-1};    // status bits (F5 31B @19 / 23B @11), -1 not seen yet
#ifdef USE_SENSOR
  sensor::Sensor *sensors[(int) UnitField::COUNT] = {};
  sensor::Sensor *diag[(int) UnitDiag::COUNT] = {};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *binary[(int) UnitBinary::COUNT] = {};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text[(int) UnitText::COUNT] = {};
#endif
};

class DaikinDIII : public Component, public uart::UARTDevice, public DecoderSink {
 public:
  DaikinDIII() { memset(this->b1_to_diii_, 0xFF, sizeof(this->b1_to_diii_)); }
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_reset_pin(InternalGPIOPin *pin) { this->reset_pin_ = pin; }
  void set_serial_enable_pin(GPIOPin *pin) { this->serial_enable_pin_ = pin; }
  // Raw lines (R/E from the ATmega, X = own writes) go straight to MQTT (own
  // lightweight client, so the entities are not duplicated on MQTT as
  // ESPHome's mqtt component would do).
  void set_raw_mqtt(const std::string &broker, uint16_t port, const std::string &user, const std::string &password,
                    const std::string &client_id, const std::string &topic_prefix, const std::string &topic_suffix) {
    this->mqtt_broker_ = broker;
    this->mqtt_port_ = port;
    this->mqtt_user_ = user;
    this->mqtt_password_ = password;
    this->mqtt_client_id_ = client_id;
    this->mqtt_prefix_ = topic_prefix;
    this->mqtt_suffix_ = topic_suffix;
  }
  void add_unit(uint8_t addr, uint8_t b1_index, uint8_t group, bool hydrobox = false);
  void set_source_address(uint8_t address) { this->source_address_ = address; }
  // Write budget per group (see diii_writer.h); burst 0 disables it.
  void set_write_budget(uint16_t burst, uint32_t refill_ms) { this->writer_.budget().configure(burst, refill_ms); }
  void set_unit_climate(uint8_t addr, DaikinClimate *climate);
  void set_unit_airflow_select(uint8_t addr, DaikinAirflowSelect *airflow_select);
  const Unit *get_unit(uint8_t addr) const;
  // Demand (status bit 0x20) of any unit in the group: 1, 0, or -1 while no
  // unit of the group has sent its status yet. fan_running: any unit's 0x04.
  int group_demand(uint8_t group, bool *fan_running) const;

#ifdef USE_SENSOR
  void set_unit_sensor(uint8_t addr, uint8_t field, sensor::Sensor *sensor);
  void set_unit_diag_sensor(uint8_t addr, uint8_t kind, sensor::Sensor *sensor);
  void set_system_sensor(uint8_t field, sensor::Sensor *sensor) { this->system_sensors_[field] = sensor; }
  void set_diag_sensor(uint8_t kind, sensor::Sensor *sensor) { this->diag_sensors_[kind] = sensor; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_unit_binary_sensor(uint8_t addr, uint8_t kind, binary_sensor::BinarySensor *sensor);
  void set_system_binary_sensor(uint8_t kind, binary_sensor::BinarySensor *sensor) {
    this->system_binary_[kind] = sensor;
  }
#endif
#ifdef USE_TEXT_SENSOR
  void set_unit_text_sensor(uint8_t addr, uint8_t kind, text_sensor::TextSensor *sensor);
  void set_system_text_sensor(uint8_t kind, text_sensor::TextSensor *sensor) { this->system_text_[kind] = sensor; }
#endif

  // Bus writes (research / control). Refused unless writes are enabled.
  void set_write_enabled(bool enabled) { this->writer_.set_enabled(enabled); }
  // Raw frame stream (MQTT) on/off. A marker line is published at each change.
  void set_raw_enabled(bool enabled);
  // Frame logging: every raw line (R/E/X, as on MQTT) also goes to the ESPHome log
  // under the tag daikin_diii.frame, for a bus log without MQTT (docs/debugging.md).
  void set_frame_log_enabled(bool enabled);
  // Called for a frame type that isn't in the catalogue (is_known_frame in
  // diii_decoder.h): the first time it's seen, then at most once an hour while it keeps
  // coming, so a notification missed while HA was away comes through later. Gets the frame
  // as hex and its type (e.g. "F5_25", "E9_41_E0_22"). on_unknown_frame in YAML.
  void add_on_unknown_frame_callback(std::function<void(const std::string &, const std::string &)> &&callback) {
    this->unknown_frame_callback_.add(std::move(callback));
  }
  bool is_write_enabled() const { return this->writer_.enabled(); }
  // frame without checksum, e.g. "A5300714144003..."; checksum is appended and
  // the frame is queued (min. 1.5 s spacing, retried when the ATmega refuses it
  // or reports a collision). Guards and queue: diii_writer.h.
  bool write_frame_hex(const std::string &hex);
  bool enqueue_frame(const uint8_t *frame, size_t length);
  // Group commands (to the group master address), see docs/protocol.md.
  // A unit added as hydrobox gets the long frame; a setpoint for it is refused.
  bool command_power(uint8_t group, bool on);
  bool command_mode(uint8_t group, uint8_t mode_low);  // 0 fan, 1 heat, 2 cool
  bool command_setpoint(uint8_t group, uint8_t celsius);
  bool command_fan(uint8_t group, uint8_t speed);      // 0 auto, 1 low, 3 medium, 5 high
  bool command_airflow(uint8_t group, uint8_t direction);  // 0-4 position (0 horizontal), 7 swing
  // per-outlet flap setting of one unit (master or member), by pair of opposite outlets
  bool command_outlet_pair(uint8_t unit, ::diii::OutletPair pair, bool fixed);
  void note_write_refused(const char *why) { this->set_last_write_(std::string("refused: ") + why); }
  // Research: pass a P1P2Monitor command (without magic string), e.g. "V" or
  // "T50". Needs bus writes enabled; only allowlisted commands, see
  // atmega_command_allowed() in diii_writer.h.
  bool send_atmega_command(const std::string &cmd);
  void reset_atmega();

#ifdef DAIKIN_DIII_ISP
  // ATmega programming (avrdude -c avrisp -P net:<bridge>:<port>). SCK is the
  // serial-enable pin (GPIO15 on bridge v1.2/v1.3), RESET the reset pin.
  void set_isp(GPIOPin *mosi, GPIOPin *miso, uint16_t port, uint32_t window_ms) {
    this->isp_mosi_pin_ = mosi;
    this->isp_miso_pin_ = miso;
    this->isp_port_ = port;
    this->isp_window_ms_ = window_ms;
  }
  // Opens/closes the programming port. It closes by itself after the window.
  void set_isp_enabled(bool enabled);
  bool is_isp_enabled() const { return this->isp_server_ != nullptr; }
  // called with false when the port closes by itself (window over)
  void set_isp_state_callback(std::function<void(bool)> &&callback) { this->isp_state_cb_ = std::move(callback); }
#endif

  // DecoderSink
  void on_unit_value(uint8_t addr, UnitField field, float value) override;
  void on_unit_power(uint8_t addr, bool on) override;
  void on_unit_mode(uint8_t addr, uint8_t mode) override;
  void on_unit_fan(uint8_t addr, uint8_t speed) override;
  void on_unit_status(uint8_t addr, uint8_t bits) override;
  void on_system_value(SystemField field, float value) override;
  void on_compressor(uint8_t state) override;
  void on_clock(uint8_t hour, uint8_t minute) override;
  void on_unit_role(uint8_t addr, bool master) override;
  void on_outdoor_module(uint8_t state, uint8_t sub_state) override;
  void on_oil_return(bool active) override;
  void on_unit_airflow(uint8_t addr, uint8_t direction) override;

 protected:
  Unit *find_unit_(uint8_t addr);
  void handle_line_(const char *line);
  void publish_raw_(char kind, const char *payload);
  void set_last_write_(const std::string &text);
  bool command_op_(uint8_t group, uint8_t power_byte, uint8_t mode_byte, uint8_t mode_detail, uint8_t setpoint);
  bool check_enqueue_(::diii::WriteResult result, bool setting);
  void load_budget_();
  void save_budget_();
  ESPPreferenceObject budget_pref_;
  void process_queue_();
  void notify_climate_(Unit *unit);
  void notify_airflow_(Unit *unit);

  uint8_t source_address_{0x30};
  ::diii::WriteQueue writer_;
  InternalGPIOPin *reset_pin_{nullptr};
  GPIOPin *serial_enable_pin_{nullptr};
  std::string mqtt_broker_;
  uint16_t mqtt_port_{1883};
  std::string mqtt_user_, mqtt_password_, mqtt_client_id_, mqtt_prefix_, mqtt_suffix_;
  std::string mqtt_will_topic_;
  uint32_t mqtt_last_attempt_{0};
  uint32_t raw_published_{0};
  uint32_t raw_dropped_{0};
  uint32_t last_stats_ms_{0};
  // the VRV clock, run on between its broadcasts (BusClock in diii_decoder.h)
  ::diii::BusClock clock_;
  ::diii::DefrostDetector defrost_;
  void publish_defrost_();
  int clock_published_{-1};  // minute of day last published, -1 none
  void publish_clock_(uint32_t now_ms);
#ifdef DAIKIN_DIII_RAW_MQTT
  AsyncMqttClient mqtt_;
  bool mqtt_configured_{false};
  void mqtt_loop_();
#endif
  std::vector<Unit> units_;
  uint8_t b1_to_diii_[256];

#ifdef DAIKIN_DIII_ISP
  // Bit-banged ISP on the bridge's pins, for the STK500 handler.
  class BridgeIsp : public ::diii::IspPort {
   public:
    explicit BridgeIsp(DaikinDIII *hub) : hub_(hub) {}
    void begin() override;
    void end() override;
    void set_reset(bool held) override;
    uint8_t transfer(uint8_t out) override;
    void delay_ms(uint32_t ms) override;

   protected:
    DaikinDIII *hub_;
  };
  void isp_loop_();
  void isp_close_client_(const char *why);
  void isp_after_programming_();
  GPIOPin *isp_mosi_pin_{nullptr};
  GPIOPin *isp_miso_pin_{nullptr};
  uint16_t isp_port_{328};
  uint32_t isp_window_ms_{600000};
  uint32_t isp_opened_ms_{0};
  uint32_t isp_last_rx_ms_{0};
  bool isp_was_programming_{false};
  WiFiServer *isp_server_{nullptr};
  WiFiClient isp_client_;
  BridgeIsp isp_bus_{this};
  ::diii::Stk500 isp_{&isp_bus_};
  std::function<void(bool)> isp_state_cb_;
#endif

  char line_buf_[300];
  size_t line_len_{0};
  bool ignore_next_line_{true};
  bool raw_enabled_{true};
  bool frame_log_enabled_{false};
  // frame types reported as unknown (frame_signature) and when, at most MAX_UNKNOWN_REPORTED
  static const size_t MAX_UNKNOWN_REPORTED = 32;
  static const uint32_t UNKNOWN_REPEAT_MS = 3600UL * 1000;
  struct UnknownReported {
    uint32_t key;
    uint32_t last_ms;
  };
  std::vector<UnknownReported> unknown_reported_;
  CallbackManager<void(const std::string &, const std::string &)> unknown_frame_callback_;
  void check_known_(const uint8_t *frame, size_t length);
  uint32_t frames_ok_{0};
  uint32_t frames_err_{0};

#ifdef USE_SENSOR
  sensor::Sensor *system_sensors_[(int) SystemField::COUNT] = {};
  sensor::Sensor *diag_sensors_[(int) DiagSensor::COUNT] = {};
#endif
  uint32_t last_diag_ms_{0};
  void publish_diagnostics_();
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *system_binary_[(int) SystemBinary::COUNT] = {};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *system_text_[(int) SystemText::COUNT] = {};
#endif
};

// on_unknown_frame: runs with `frame` (hex) and `frame_type` for a frame type unknown to
// the component, see add_on_unknown_frame_callback.
class UnknownFrameTrigger : public Trigger<std::string, std::string> {
 public:
  explicit UnknownFrameTrigger(DaikinDIII *parent) {
    parent->add_on_unknown_frame_callback(
        [this](const std::string &frame, const std::string &frame_type) { this->trigger(frame, frame_type); });
  }
};

}  // namespace daikin_diii
}  // namespace esphome
