#pragma once

#include "esphome/components/select/select.h"
#include "esphome/core/log.h"
#include "daikin_diii.h"

namespace esphome {
namespace daikin_diii {

// Airflow direction (flap position) of one remote-control group, addressed via
// its master unit. The options are in the order of select.py: index 0 = swing,
// index 1..5 = position 0 (horizontal) .. 4 (down). State comes from the
// master's F5 23B frame (every ~60 s); a change is one A5 41 command, sent
// only when bus writes are on and the value really changes (write budget).
class DaikinAirflowSelect : public select::Select, public Parented<DaikinDIII> {
 public:
  void set_group(uint8_t group) { this->group_ = group; }

  // bus code (0-4 position, 7 swing) <-> option index
  static int index_for_code(int code) { return code == 7 ? 0 : (code >= 0 && code <= 4) ? code + 1 : -1; }
  static int code_for_index(size_t index) { return index == 0 ? 7 : (index <= 5 ? (int) index - 1 : -1); }

  void update_from_unit(const Unit &unit) {
    this->pending_.observe(unit.airflow);
    // a command not yet confirmed (status only every ~60 s) counts as current,
    // so the UI does not jump back to the old value in between
    int index = index_for_code(this->pending_.current(unit.airflow, millis()));
    if (index < 0) {
      ESP_LOGW("daikin_diii.airflow", "Group 0x%02X: unknown airflow code %d", this->group_, unit.airflow);
      return;
    }
    auto current = this->active_index();
    if (!this->has_state() || !current.has_value() || *current != (size_t) index) this->publish_state((size_t) index);
  }

 protected:
  void control(size_t index) override {
    const int code = code_for_index(index);
    if (code < 0) return;
    if (!this->parent_->is_write_enabled()) {
      ESP_LOGW("daikin_diii.airflow", "Group 0x%02X: command ignored, bus writes are disabled", this->group_);
      this->parent_->note_write_refused("writes disabled");
      this->republish_();  // snap the UI back to the real state
      return;
    }
    const Unit *unit = this->parent_->get_unit(this->group_);
    const int bus_code = unit != nullptr ? unit->airflow : -1;
    const uint32_t now = millis();
    // every setting command counts towards the indoor units' write budget
    if (this->pending_.changes(code, bus_code, bus_code < 0, now)) {
      if (!this->parent_->command_airflow(this->group_, (uint8_t) code)) {
        ESP_LOGW("daikin_diii.airflow", "Group 0x%02X: command refused, see Last bus command", this->group_);
        this->republish_();
        return;
      }
      this->pending_.commanded(code, now);
    }
    this->publish_state(index);  // optimistic; the next F5 23B frame confirms it
  }

  void republish_() {
    const Unit *unit = this->parent_->get_unit(this->group_);
    int index = unit != nullptr ? index_for_code(unit->airflow) : -1;
    if (index >= 0) this->publish_state((size_t) index);
  }

  uint8_t group_{0};
  ::diii::PendingSetting pending_;
};

// Per-outlet flap setting of one unit, for a pair of opposite outlets (1+3 or
// 2+4): index 0 = Normal, 1 = Fixed (0). The units do not report this setting
// on F1F2, so the state is the last value sent from here, kept in flash and
// restored after a reboot without sending anything. Choosing the value already
// shown sends nothing (write budget); every other change is one A5 41 command
// through the bus-write switch and the write budget.
class DaikinOutletPairSelect : public select::Select, public Component, public Parented<DaikinDIII> {
 public:
  void set_unit(uint8_t unit) { this->unit_ = unit; }
  void set_pair(uint8_t pair) {
    this->pair_ = pair ? ::diii::OutletPair::OUTLETS_2_4 : ::diii::OutletPair::OUTLETS_1_3;
  }

  void setup() override {
    this->pref_ = this->make_entity_preference<uint8_t>();
    uint8_t saved_index;
    if (this->pref_.load(&saved_index) && saved_index < 2) this->publish_state((size_t) saved_index);
  }
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(size_t index) override {
    if (index > 1) return;
    auto current = this->active_index();
    if (this->has_state() && current.has_value() && *current == index) return;  // nothing changes
    if (!this->parent_->is_write_enabled()) {
      ESP_LOGW("daikin_diii.outlets", "Unit 0x%02X: command ignored, bus writes are disabled", this->unit_);
      this->parent_->note_write_refused("writes disabled");
      this->republish_();
      return;
    }
    if (!this->parent_->command_outlet_pair(this->unit_, this->pair_, index == 1)) {
      ESP_LOGW("daikin_diii.outlets", "Unit 0x%02X: command refused, see Last bus command", this->unit_);
      this->republish_();
      return;
    }
    uint8_t saved_index = (uint8_t) index;
    this->pref_.save(&saved_index);
    this->publish_state(index);
  }

  void republish_() {
    auto current = this->active_index();
    if (this->has_state() && current.has_value()) this->publish_state(*current);
  }

  uint8_t unit_{0};
  ::diii::OutletPair pair_{::diii::OutletPair::OUTLETS_1_3};
  ESPPreferenceObject pref_;
};

}  // namespace daikin_diii
}  // namespace esphome
