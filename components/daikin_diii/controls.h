#pragma once

#include "daikin_diii.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

namespace esphome {
namespace daikin_diii {

#ifdef USE_SWITCH
// Both switches are Components so that setup() applies the restore mode
// (stored state, or the default); a bare Switch would always start off.
class RestoringSwitch : public switch_::Switch, public Component {
 public:
  void setup() override {
    auto initial = this->get_initial_state_with_restore_mode();
    if (initial.has_value()) this->write_state(*initial);
  }
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
};

// Master switch for all bus writes (off until turned on, then keeps its state across restarts).
class WriteEnableSwitch : public RestoringSwitch, public Parented<DaikinDIII> {
 protected:
  void write_state(bool state) override {
    this->parent_->set_write_enabled(state);
    this->publish_state(state);
  }
};

#ifdef DAIKIN_DIII_ISP
// Opens the ATmega programming port for a limited time. Always off after a
// restart (no restore), and turns itself off when the window is over.
class AtmegaProgrammingSwitch : public switch_::Switch, public Component, public Parented<DaikinDIII> {
 public:
  void setup() override {
    this->parent_->set_isp_state_callback([this](bool on) { this->publish_state(on); });
    this->publish_state(false);
  }

 protected:
  void write_state(bool state) override {
    this->parent_->set_isp_enabled(state);
    this->publish_state(this->parent_->is_isp_enabled());
  }
};
#endif

// Raw frame logging (MQTT stream) on/off.
class RawLoggingSwitch : public RestoringSwitch, public Parented<DaikinDIII> {
 protected:
  void write_state(bool state) override {
    this->parent_->set_raw_enabled(state);
    this->publish_state(state);
  }
};

// Frame logging to the ESPHome log (no MQTT needed). Always off after a restart.
class FrameLoggingSwitch : public RestoringSwitch, public Parented<DaikinDIII> {
 protected:
  void write_state(bool state) override {
    this->parent_->set_frame_log_enabled(state);
    this->publish_state(state);
  }
};
#endif

#ifdef USE_BUTTON
class ResetAtmegaButton : public button::Button, public Parented<DaikinDIII> {
 protected:
  void press_action() override { this->parent_->reset_atmega(); }
};
#endif

}  // namespace daikin_diii
}  // namespace esphome
