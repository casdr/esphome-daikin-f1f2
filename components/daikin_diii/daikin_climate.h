#pragma once

#include "esphome/components/climate/climate.h"
#include "daikin_diii.h"

namespace esphome {
namespace daikin_diii {

// Climate entity for one remote-control group (addressed via its master unit).
// State comes from the bus (F5 status frames of the master); commands are A5
// frames queued on the hub and only sent when bus writes are enabled.
class DaikinClimate : public climate::Climate, public Parented<DaikinDIII> {
 public:
  void set_group(uint8_t group) { this->group_ = group; }

  climate::ClimateTraits traits() override {
    climate::ClimateTraits climate_traits;
    climate_traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
    if (this->is_hydrobox_()) {
      // power and heat/cool only: the water setpoint's encoding is unknown
      climate_traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
      climate_traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
      climate_traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
      climate_traits.set_visual_current_temperature_step(0.1);
      return climate_traits;
    }
    climate_traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
    climate_traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
    climate_traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
    climate_traits.add_supported_mode(climate::CLIMATE_MODE_FAN_ONLY);
    // Auto and dry can be set by the iTM or a wall remote and are shown as
    // such, but commanding them from HA is not supported (codes untested).
    climate_traits.add_supported_mode(climate::CLIMATE_MODE_HEAT_COOL);
    climate_traits.add_supported_mode(climate::CLIMATE_MODE_DRY);
    climate_traits.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
    climate_traits.add_supported_fan_mode(climate::CLIMATE_FAN_LOW);
    climate_traits.add_supported_fan_mode(climate::CLIMATE_FAN_MEDIUM);
    climate_traits.add_supported_fan_mode(climate::CLIMATE_FAN_HIGH);
    climate_traits.set_visual_min_temperature(16);
    climate_traits.set_visual_max_temperature(32);
    climate_traits.set_visual_target_temperature_step(1);
    climate_traits.set_visual_current_temperature_step(0.1);
    // hvac_action from the units' demand bit (group_action in diii_decoder.h)
    climate_traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION);
    return climate_traits;
  }

  void control(const climate::ClimateCall &call) override {
    if (!this->parent_->is_write_enabled()) {
      ESP_LOGW("daikin_diii.climate", "Group 0x%02X: command ignored, bus writes are disabled", this->group_);
      this->parent_->note_write_refused("writes disabled");
      this->publish_state();  // snap the UI back to the real state
      return;
    }
    if (this->is_hydrobox_()) {
      this->control_hydrobox_(call);
      return;
    }
    if (call.get_mode().has_value() &&
        (*call.get_mode() == climate::CLIMATE_MODE_HEAT_COOL || *call.get_mode() == climate::CLIMATE_MODE_DRY)) {
      ESP_LOGW("daikin_diii.climate", "Group 0x%02X: auto/dry cannot be set from HA", this->group_);
      this->parent_->note_write_refused("auto/dry not supported");
      this->publish_state();
      return;
    }
    // Only send what really changes: every setting command counts towards the
    // indoor units' write budget (Daikin: 7000 per unit per year). "Current"
    // is what this bridge commanded in the last minute, else the bus status
    // (PendingSetting in diii_writer.h); unknown state is sent anyway.
    const Unit *unit = this->parent_->get_unit(this->group_);
    const uint32_t now = millis();
    const int bus_power = unit != nullptr ? unit->power : -1;
    const int bus_mode = unit != nullptr ? unit->mode : 0;
    const int bus_setpoint = (unit != nullptr && !std::isnan(unit->setpoint)) ? (int) lroundf(unit->setpoint) : -1;
    const int bus_fan = unit != nullptr ? unit->fan : -1;
    bool refused = false;

    if (call.get_mode().has_value()) {
      auto new_mode = *call.get_mode();
      bool ok = true;
      if (new_mode == climate::CLIMATE_MODE_OFF) {
        if (this->power_.changes(0, bus_power, bus_power < 0, now)) {
          ok = this->parent_->command_power(this->group_, false);
          if (ok) this->power_.commanded(0, now);
        }
      } else {
        uint8_t mode_low = new_mode == climate::CLIMATE_MODE_HEAT ? 1 : new_mode == climate::CLIMATE_MODE_COOL ? 2 : 0;
        if (this->mode_.changes(0x60 | mode_low, bus_mode, bus_mode == 0, now)) {
          ok = this->parent_->command_mode(this->group_, mode_low);
          if (ok) this->mode_.commanded(0x60 | mode_low, now);
        }
        if (ok && this->power_.changes(1, bus_power, bus_power < 0, now)) {
          ok = this->parent_->command_power(this->group_, true);
          if (ok) this->power_.commanded(1, now);
        }
      }
      if (ok) this->mode = new_mode;
      refused |= !ok;
    }
    if (call.get_target_temperature().has_value()) {
      long celsius = lroundf(*call.get_target_temperature());
      celsius = celsius < 16 ? 16 : celsius > 32 ? 32 : celsius;
      bool ok = true;
      if (this->setpoint_.changes((int) celsius, bus_setpoint, bus_setpoint < 0, now)) {
        ok = this->parent_->command_setpoint(this->group_, (uint8_t) celsius);
        if (ok) this->setpoint_.commanded((int) celsius, now);
      }
      if (ok) this->target_temperature = celsius;
      refused |= !ok;
    }
    if (call.get_fan_mode().has_value()) {
      auto new_fan_mode = *call.get_fan_mode();
      uint8_t speed = new_fan_mode == climate::CLIMATE_FAN_LOW      ? 1
                      : new_fan_mode == climate::CLIMATE_FAN_MEDIUM ? 3
                      : new_fan_mode == climate::CLIMATE_FAN_HIGH   ? 5
                                                                    : 0;
      bool ok = true;
      if (this->fan_.changes(speed, bus_fan, bus_fan < 0, now)) {
        ok = this->parent_->command_fan(this->group_, speed);
        if (ok) this->fan_.commanded(speed, now);
      }
      if (ok) this->fan_mode = new_fan_mode;
      refused |= !ok;
    }
    if (refused) ESP_LOGW("daikin_diii.climate", "Group 0x%02X: command refused, see Last bus command", this->group_);
    this->publish_state();  // optimistic for what was sent; corrected by the next bus status
  }

  void update_from_unit(const Unit &unit) {
    // the bus status confirms commands sent earlier (see PendingSetting)
    this->power_.observe(unit.power);
    this->mode_.observe(unit.mode);
    this->setpoint_.observe(std::isnan(unit.setpoint) ? -1 : (int) lroundf(unit.setpoint));
    this->fan_.observe(unit.fan);
    if (unit.hydrobox) {
      this->update_hydrobox_(unit);
      return;
    }
    // after a restart: publish nothing until power and fan are known, so HA
    // does not show a made-up state (fan "on", no setpoint) for a minute
    if (unit.power < 0 || unit.fan < 0) return;
    climate::ClimateMode new_mode = climate::CLIMATE_MODE_OFF;
    if (unit.power == 1) {
      switch (unit.mode) {
        case 0x61: new_mode = climate::CLIMATE_MODE_HEAT; break;
        case 0x62: new_mode = climate::CLIMATE_MODE_COOL; break;
        case 0x60: new_mode = climate::CLIMATE_MODE_FAN_ONLY; break;
        case 0x63: new_mode = climate::CLIMATE_MODE_HEAT_COOL; break;
        case 0x67: new_mode = climate::CLIMATE_MODE_DRY; break;
        case 0x00: new_mode = this->mode; break;  // power known, mode not seen yet
        default:
          // unknown mode code: keep the last known mode
          ESP_LOGW("daikin_diii.climate", "Group 0x%02X: unknown mode 0x%02X", this->group_, unit.mode);
          new_mode = this->mode == climate::CLIMATE_MODE_OFF ? climate::CLIMATE_MODE_HEAT : this->mode;
          break;
      }
    }
    climate::ClimateFanMode new_fan_mode = climate::CLIMATE_FAN_AUTO;
    switch (unit.fan) {
      case 1: new_fan_mode = climate::CLIMATE_FAN_LOW; break;
      case 3: new_fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
      case 5: new_fan_mode = climate::CLIMATE_FAN_HIGH; break;
      default: break;
    }
    // current temperature: the remote controller (middle of the room) when the
    // group has one, otherwise the unit's return-air sensor. Until a B1 frame
    // has said which (up to a minute after a restart) it stays unknown: the
    // return air is ~2 degrees warmer and would show up as a spike.
    float current = !std::isnan(unit.remote) ? unit.remote : (unit.remote_known ? unit.room : NAN);
    climate::ClimateAction new_action = this->action_(unit, current);
    bool changed = this->mode != new_mode ||
                   (unit.fan >= 0 && (!this->fan_mode.has_value() || *this->fan_mode != new_fan_mode)) ||
                   (!std::isnan(unit.setpoint) && this->target_temperature != unit.setpoint) ||
                   (!std::isnan(current) && this->current_temperature != current) || this->action != new_action;
    if (!changed) return;
    this->mode = new_mode;
    this->action = new_action;
    if (unit.fan >= 0) this->fan_mode = new_fan_mode;
    if (!std::isnan(unit.setpoint)) this->target_temperature = unit.setpoint;
    if (!std::isnan(current)) this->current_temperature = current;
    this->publish_state();
  }

 protected:
  climate::ClimateAction action_(const Unit &unit, float current) const {
    bool fan_running = false;
    const int demand = this->parent_->group_demand(this->group_, &fan_running);
    float target = std::isnan(unit.setpoint) ? this->target_temperature : unit.setpoint;
    switch (::diii::group_action(unit.power, unit.mode, demand, fan_running, current, target)) {
      case ::diii::Action::OFF: return climate::CLIMATE_ACTION_OFF;
      case ::diii::Action::HEATING: return climate::CLIMATE_ACTION_HEATING;
      case ::diii::Action::COOLING: return climate::CLIMATE_ACTION_COOLING;
      case ::diii::Action::FAN: return climate::CLIMATE_ACTION_FAN;
      case ::diii::Action::DRYING: return climate::CLIMATE_ACTION_DRYING;
      // unknown shows as idle: ESPHome has no "unknown" action, and idle is
      // what the unit does until its first status frame (within ~1 min)
      default: return climate::CLIMATE_ACTION_IDLE;
    }
  }

  bool is_hydrobox_() const {
    const Unit *unit = this->parent_->get_unit(this->group_);
    return unit != nullptr && unit->hydrobox;
  }

  // Hydrobox: on/off and heat/cool with the same 40 command as indoor units
  // (a 35-byte frame, built by the hub). Mode codes as for indoor units: heat
  // (11 01) is what the iTM sends; cool (12 02) is not confirmed on the hydrobox.
  void control_hydrobox_(const climate::ClimateCall &call) {
    if (!call.get_mode().has_value()) {
      this->publish_state();
      return;
    }
    const Unit *unit = this->parent_->get_unit(this->group_);
    const uint32_t now = millis();
    const int bus_power = unit != nullptr ? unit->power : -1;
    auto new_mode = *call.get_mode();
    bool ok = true;
    if (new_mode == climate::CLIMATE_MODE_OFF) {
      if (this->power_.changes(0, bus_power, bus_power < 0, now)) {
        ok = this->parent_->command_power(this->group_, false);
        if (ok) this->power_.commanded(0, now);
      }
    } else if (new_mode == climate::CLIMATE_MODE_HEAT || new_mode == climate::CLIMATE_MODE_COOL) {
      // The hydrobox reports no mode on the bus that we can read, so the mode
      // is sent when it differs from what this entity last showed.
      uint8_t mode_low = new_mode == climate::CLIMATE_MODE_HEAT ? 1 : 2;
      if (this->mode != new_mode || bus_power != 1) {
        ok = this->parent_->command_mode(this->group_, mode_low);
        if (ok) this->hydrobox_mode_ = new_mode;
      }
      if (ok && this->power_.changes(1, bus_power, bus_power < 0, now)) {
        ok = this->parent_->command_power(this->group_, true);
        if (ok) this->power_.commanded(1, now);
      }
    } else {
      this->parent_->note_write_refused("hydrobox: off, heat or cool only");
      ok = false;
    }
    if (ok) {
      this->mode = new_mode;
    } else {
      ESP_LOGW("daikin_diii.climate", "Hydrobox 0x%02X: command refused, see Last bus command", this->group_);
    }
    this->publish_state();
  }

  void update_hydrobox_(const Unit &unit) {
    if (unit.power < 0) return;
    // The hydrobox reports power once a minute (F5 22B), so until the bus
    // confirms a command the commanded power counts (PendingSetting).
    const int power = this->power_.current(unit.power, millis());
    climate::ClimateMode new_mode = power == 1 ? this->hydrobox_mode_ : climate::CLIMATE_MODE_OFF;
    bool changed = this->mode != new_mode || (!std::isnan(unit.room) && this->current_temperature != unit.room);
    if (!changed) return;
    this->mode = new_mode;
    if (!std::isnan(unit.room)) this->current_temperature = unit.room;
    this->publish_state();
  }

  uint8_t group_{0};
  // last heat/cool mode commanded to a hydrobox; shown while it is on (heat
  // until this bridge commands cool)
  climate::ClimateMode hydrobox_mode_{climate::CLIMATE_MODE_HEAT};
  // last commanded values (see PendingSetting)
  ::diii::PendingSetting power_, mode_, setpoint_, fan_;
};

}  // namespace daikin_diii
}  // namespace esphome
