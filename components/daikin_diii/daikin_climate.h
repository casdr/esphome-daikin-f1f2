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
  void set_group(uint8_t g) { this->group_ = g; }

  climate::ClimateTraits traits() override {
    climate::ClimateTraits t;
    t.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
    t.add_supported_mode(climate::CLIMATE_MODE_OFF);
    t.add_supported_mode(climate::CLIMATE_MODE_HEAT);
    t.add_supported_mode(climate::CLIMATE_MODE_COOL);
    t.add_supported_mode(climate::CLIMATE_MODE_FAN_ONLY);
    // Auto and dry can be set by the iTM or a wall remote and are shown as
    // such, but commanding them from HA is not supported (codes untested).
    t.add_supported_mode(climate::CLIMATE_MODE_HEAT_COOL);
    t.add_supported_mode(climate::CLIMATE_MODE_DRY);
    t.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
    t.add_supported_fan_mode(climate::CLIMATE_FAN_LOW);
    t.add_supported_fan_mode(climate::CLIMATE_FAN_MEDIUM);
    t.add_supported_fan_mode(climate::CLIMATE_FAN_HIGH);
    t.set_visual_min_temperature(16);
    t.set_visual_max_temperature(32);
    t.set_visual_target_temperature_step(1);
    t.set_visual_current_temperature_step(0.1);
    return t;
  }

  void control(const climate::ClimateCall &call) override {
    if (!this->parent_->is_write_enabled()) {
      ESP_LOGW("daikin_diii.climate", "Group 0x%02X: command ignored, bus writes are disabled", this->group_);
      this->parent_->note_write_refused("writes disabled");
      this->publish_state();  // snap the UI back to the real state
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
    const Unit *u = this->parent_->get_unit(this->group_);
    const uint32_t now = millis();
    const int bus_power = u != nullptr ? u->power : -1;
    const int bus_mode = u != nullptr ? u->mode : 0;
    const int bus_sp = (u != nullptr && !std::isnan(u->setpoint)) ? (int) lroundf(u->setpoint) : -1;
    const int bus_fan = u != nullptr ? u->fan : -1;
    bool refused = false;

    if (call.get_mode().has_value()) {
      auto m = *call.get_mode();
      bool ok = true;
      if (m == climate::CLIMATE_MODE_OFF) {
        if (this->power_.changes(0, bus_power, bus_power < 0, now)) {
          ok = this->parent_->command_power(this->group_, false);
          if (ok) this->power_.commanded(0, now);
        }
      } else {
        uint8_t low = m == climate::CLIMATE_MODE_HEAT ? 1 : m == climate::CLIMATE_MODE_COOL ? 2 : 0;
        if (this->mode_.changes(0x60 | low, bus_mode, bus_mode == 0, now)) {
          ok = this->parent_->command_mode(this->group_, low);
          if (ok) this->mode_.commanded(0x60 | low, now);
        }
        if (ok && this->power_.changes(1, bus_power, bus_power < 0, now)) {
          ok = this->parent_->command_power(this->group_, true);
          if (ok) this->power_.commanded(1, now);
        }
      }
      if (ok) this->mode = m;
      refused |= !ok;
    }
    if (call.get_target_temperature().has_value()) {
      long c = lroundf(*call.get_target_temperature());
      c = c < 16 ? 16 : c > 32 ? 32 : c;
      bool ok = true;
      if (this->setpoint_.changes((int) c, bus_sp, bus_sp < 0, now)) {
        ok = this->parent_->command_setpoint(this->group_, (uint8_t) c);
        if (ok) this->setpoint_.commanded((int) c, now);
      }
      if (ok) this->target_temperature = c;
      refused |= !ok;
    }
    if (call.get_fan_mode().has_value()) {
      auto f = *call.get_fan_mode();
      uint8_t s = f == climate::CLIMATE_FAN_LOW ? 1 : f == climate::CLIMATE_FAN_MEDIUM ? 3 : f == climate::CLIMATE_FAN_HIGH ? 5 : 0;
      bool ok = true;
      if (this->fan_.changes(s, bus_fan, bus_fan < 0, now)) {
        ok = this->parent_->command_fan(this->group_, s);
        if (ok) this->fan_.commanded(s, now);
      }
      if (ok) this->fan_mode = f;
      refused |= !ok;
    }
    if (refused) ESP_LOGW("daikin_diii.climate", "Group 0x%02X: command refused, see Last bus command", this->group_);
    this->publish_state();  // optimistic for what was sent; corrected by the next bus status
  }

  void update_from_unit(const Unit &u) {
    // the bus status confirms commands sent earlier (see PendingSetting)
    this->power_.observe(u.power);
    this->mode_.observe(u.mode);
    this->setpoint_.observe(std::isnan(u.setpoint) ? -1 : (int) lroundf(u.setpoint));
    this->fan_.observe(u.fan);
    // after a restart: publish nothing until power and fan are known, so HA
    // does not show a made-up state (fan "on", no setpoint) for a minute
    if (u.power < 0 || u.fan < 0) return;
    climate::ClimateMode m = climate::CLIMATE_MODE_OFF;
    if (u.power == 1) {
      switch (u.mode) {
        case 0x61: m = climate::CLIMATE_MODE_HEAT; break;
        case 0x62: m = climate::CLIMATE_MODE_COOL; break;
        case 0x60: m = climate::CLIMATE_MODE_FAN_ONLY; break;
        case 0x63: m = climate::CLIMATE_MODE_HEAT_COOL; break;
        case 0x67: m = climate::CLIMATE_MODE_DRY; break;
        case 0x00: m = this->mode; break;  // power known, mode not seen yet
        default:
          // unknown mode code: keep the last known mode
          ESP_LOGW("daikin_diii.climate", "Group 0x%02X: unknown mode 0x%02X", this->group_, u.mode);
          m = this->mode == climate::CLIMATE_MODE_OFF ? climate::CLIMATE_MODE_HEAT : this->mode;
          break;
      }
    }
    climate::ClimateFanMode f = climate::CLIMATE_FAN_AUTO;
    switch (u.fan) {
      case 1: f = climate::CLIMATE_FAN_LOW; break;
      case 3: f = climate::CLIMATE_FAN_MEDIUM; break;
      case 5: f = climate::CLIMATE_FAN_HIGH; break;
      default: break;
    }
    // current temperature: the remote controller (middle of the room) when the
    // group has one, otherwise the unit's return-air sensor. Until a B1 frame
    // has said which (up to a minute after a restart) it stays unknown: the
    // return air is ~2 degrees warmer and would show up as a spike.
    float cur = !std::isnan(u.remote) ? u.remote : (u.remote_known ? u.room : NAN);
    bool changed = this->mode != m || (u.fan >= 0 && (!this->fan_mode.has_value() || *this->fan_mode != f)) ||
                   (!std::isnan(u.setpoint) && this->target_temperature != u.setpoint) ||
                   (!std::isnan(cur) && this->current_temperature != cur);
    if (!changed) return;
    this->mode = m;
    if (u.fan >= 0) this->fan_mode = f;
    if (!std::isnan(u.setpoint)) this->target_temperature = u.setpoint;
    if (!std::isnan(cur)) this->current_temperature = cur;
    this->publish_state();
  }

 protected:
  uint8_t group_{0};
  // last commanded values (see PendingSetting)
  ::diii::PendingSetting power_, mode_, setpoint_, fan_;
};

}  // namespace daikin_diii
}  // namespace esphome
