#pragma once
// Daikin F1F2 / DIII-NET frame decoder. Pure C++ (no ESPHome dependency) so it
// can be unit-tested on the host. Field layout: see docs/protocol.md.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace diii {

static const size_t MAX_FRAME = 96;

// Byte offsets, counted from the start of the frame (cmd = byte 0), and frame
// lengths including the checksum. One namespace per frame type; docs/protocol.md
// lists the same names in the "code" column of each frame's table.
namespace header {
static const size_t CMD = 0;
static const size_t SRC = 1;
static const size_t DST = 2;
static const size_t CLASS = 3;           // "b3": 08/10/14/1C (hyp: source/class)
static const size_t PAYLOAD_LENGTH = 4;  // total frame length = 6 + this
static const size_t PAYLOAD = 5;         // payload[0]: sub-command or page id
static const size_t OVERHEAD = 6;        // header (5) + checksum (1)
}  // namespace header

// F5 23 B: periodic status of an indoor unit
namespace f5_status {
static const size_t LENGTH = 23;
static const size_t MODE = 6;
static const size_t STATUS = 11;       // status bits
static const size_t ROOM = 14;         // 8.8, return air
static const size_t FAN_AIRFLOW = 16;  // high nibble fan speed, low nibble airflow direction
}  // namespace f5_status

// F5 31 B: indoor unit status on change
namespace f5_change {
static const size_t LENGTH = 31;
static const size_t POWER = 6;
static const size_t MODE = 7;
static const size_t SETPOINT = 13;  // whole degrees
static const size_t ROLE = 18;      // 0x12 group master, 0x10 member
static const size_t STATUS = 19;    // status bits
}  // namespace f5_change

// F5 22 B: periodic status of the hydrobox
namespace f5_hydrobox {
static const size_t LENGTH = 22;
static const size_t DEMAND_STEP = 9;
static const size_t STATUS = 11;       // status bits; bit 0 is power (no 31 B frame)
static const size_t TEMPERATURE = 14;  // 8.8
}  // namespace f5_hydrobox

// B1 46 B: per-unit data to the outdoor unit, src = B1 index
namespace b1_unit {
static const size_t LENGTH = 46;
static const size_t PIPE_A = 13;    // 8.8 (hyp: liquid pipe)
static const size_t PIPE_B = 15;    // 8.8 (hyp: gas pipe)
static const size_t SETPOINT = 17;  // whole degrees; byte 18 holds flags
static const size_t REMOTE = 30;    // 8.8, remote-controller sensor
static const size_t INTAKE = 43;    // 8.8, return air
}  // namespace b1_unit

// E9: outdoor/system data, page id at header::PAYLOAD
namespace e9_page_65 {
static const size_t LENGTH = 46;
// ERQ outdoor units send a 43 B page 65. Its first bytes look the same (idle frames
// only, P1P2MQTT discussion #293), so it is decoded with the same offsets (hyp).
static const size_t LENGTH_SHORT = 43;
static const size_t COMPRESSOR = 11;  // 0 off, 1 starting/stopping, 2 running
static const size_t T_EVAP = 15;      // 8.8 sign-magnitude
static const size_t T_COND = 17;      // 8.8 sign-magnitude
static const size_t COMP_SPEED_TOTAL = 25;  // 16-bit big-endian: 3 x the sum of the E0/02+03 COMP_SPEED bytes
static const size_t CURRENT = 35;           // 16-bit big-endian, 0.1 A, 00 02 when idle (hyp: compressor current)
}  // namespace e9_page_65

namespace e9_page_67 {
static const size_t LENGTH = 44;
static const size_t OUTDOOR = 21;  // 8.8 sign-magnitude (like Te)
}  // namespace e9_page_67

// three outdoor modules as (state, sub-state) pairs at 11..16
namespace e9_page_6a {
static const size_t LENGTH = 46;
static const size_t STATE_1 = 11;  // module 1: 01 not heating/cooling, 02 normal operation
static const size_t SUB_STATE_1 = 12;
static const size_t SUB_STATE_2 = 14;
static const size_t SUB_STATE_3 = 16;
}  // namespace e9_page_6a

// E0 41 B: a sub-page per outdoor module, 02/03 status and 12/13 temperatures
namespace e9_page_e0 {
static const size_t LENGTH = 41;
static const size_t SUB_PAGE = 11;
static const size_t COMP_SPEED = 32;  // sub-page 02/03, 0 off, 20-130 running (hyp: compressor speed)
static const size_t DISCHARGE = 16;   // sub-page 12/13, whole degrees, sign-magnitude (hyp)
static const size_t COMP_STEP = 37;   // sub-page 12/13 (hyp)
static const size_t FAN_STEP = 39;    // sub-page 12/13 (hyp: outdoor fan step)
}  // namespace e9_page_e0

namespace e9_page_e6 {
static const size_t LENGTH = 36;
static const size_t HOUR = 11;
static const size_t MINUTE = 12;
}  // namespace e9_page_e6

// Defrost, from E9 pages 65 and 6A (38 cycles, 2026-11-23..27): while the compressor
// runs, module 1 goes from 02 (normal) to 01 with sub-state 00 for about 7 minutes,
// then sub-state 05, and back to 02 after 10-16 minutes in all. A compressor start
// also passes 01 -> 02 and oil return (07) and residual operation (09) also show 01,
// so defrost only starts from 02 while the compressor runs, and ends at 02, 07, 09 or
// when the compressor stops.
class DefrostDetector {
 public:
  void compressor(uint8_t state) {
    this->compressor_ = state;
    if (state == 0) this->active_ = false;
  }
  void module(uint8_t state, uint8_t sub_state) {
    if (!this->active_ && state == 0x01 && (sub_state == 0x00 || sub_state == 0x05) && this->last_state_ == 0x02 &&
        this->compressor_ == 0x02) {
      this->active_ = true;
    } else if (this->active_ && (state == 0x02 || sub_state == 0x07 || sub_state == 0x09)) {
      this->active_ = false;
    }
    this->last_state_ = state;
  }
  bool active() const { return this->active_; }

 private:
  uint8_t compressor_ = 0;
  uint8_t last_state_ = 0;
  bool active_ = false;
};

// The VRV clock (E9 page E6) comes every 10 minutes, at about hh:m5:04 by that clock.
// BusClock runs it on between broadcasts, so the time can be shown every minute. It
// stops after MAX_RUN_ON_MS without a broadcast: then the bus clock counts as lost,
// and a clock that stops changing is the sign of that.
class BusClock {
 public:
  static const uint32_t MAX_RUN_ON_MS = 15UL * 60 * 1000;  // one missed broadcast and some margin
  static const uint32_t SECONDS_AT_BROADCAST = 4;          // the broadcast comes 4 s into the minute

  void set(uint8_t hour, uint8_t minute, uint32_t now_ms) {
    this->minute_of_day_ = (uint16_t) (hour * 60 + minute);
    this->set_ms_ = now_ms;
    this->valid_ = true;
  }
  // Minutes since midnight now, or -1 before the first broadcast or once it's too old.
  int minute_of_day(uint32_t now_ms) const {
    if (!this->valid_) return -1;
    const uint32_t elapsed_ms = now_ms - this->set_ms_;  // wraps correctly with millis()
    if (elapsed_ms > MAX_RUN_ON_MS) return -1;
    const uint32_t seconds = SECONDS_AT_BROADCAST + elapsed_ms / 1000;
    return (int) ((this->minute_of_day_ + seconds / 60) % (24 * 60));
  }

 protected:
  uint16_t minute_of_day_{0};
  uint32_t set_ms_{0};
  bool valid_{false};
};

// A5 operation command (payload[0] 40), as built in diii_writer.h
namespace a5_operation {
static const size_t POWER = 6;
static const size_t MODE = 7;
static const size_t MODE_DETAIL = 8;
static const size_t SETPOINT = 11;
static const size_t SETPOINT_VALID = 12;
static const size_t SOURCE_FLAG = 15;  // 80 in every command from the iTM and from us
}  // namespace a5_operation

enum class UnitField : uint8_t {
  ROOM = 0,      // F5 23B @14 (and 22B hydrobox @14) - return air, = iTM temperature
  INTAKE,        // B1 46B @43
  REMOTE,        // B1 46B @30 - remote-controller sensor
  PIPE_A,        // B1 46B @13 (hyp: liquid pipe)
  PIPE_B,        // B1 46B @15 (hyp: gas pipe)
  SETPOINT,      // F5 31B @13 (whole degrees) and B1 46B @17
  DEMAND_STEP,   // F5 22B @9, hydrobox: 0 without heat demand, 1-15 with (hyp: requested capacity step)
  COUNT
};

enum class SystemField : uint8_t {
  OUTDOOR = 0,   // E9 page 67 @21 (8.8 signed)
  T_EVAP,        // E9 page 65 @15 (8.8 sign-magnitude, hyp)
  T_COND,        // E9 page 65 @17 (8.8, hyp)
  DISCHARGE_1,   // E9 page E0/12 @16 (whole degrees, hyp)
  DISCHARGE_2,   // E9 page E0/13 @16
  CURRENT,       // E9 page 65 @35-36 / 10 (hyp: compressor current, A)
  COMP_STEP_1,   // E9 page E0/12 @37 (hyp: compressor step, 0 = off)
  COMP_STEP_2,   // E9 page E0/13 @37
  FAN_STEP_1,    // E9 page E0/12 @39 (hyp: outdoor fan step)
  FAN_STEP_2,    // E9 page E0/13 @39
  COMP_SPEED_1,      // E9 page E0/02 @32 (hyp: compressor speed, unit unknown)
  COMP_SPEED_2,      // E9 page E0/03 @32
  COMP_SPEED_TOTAL,  // E9 page 65 @25-26 (16-bit, = 3 x (COMP_SPEED_1 + COMP_SPEED_2))
  COUNT
};

// Receives decoded values. Called for every frame that carries the value
// (no change detection here).
class DecoderSink {
 public:
  virtual ~DecoderSink() = default;
  virtual void on_unit_value(uint8_t addr, UnitField field, float value) = 0;
  virtual void on_unit_power(uint8_t addr, bool on) = 0;
  virtual void on_unit_mode(uint8_t addr, uint8_t mode) = 0;  // 0x60 fan, 0x61 heat, 0x62 cool, ...
  virtual void on_unit_fan(uint8_t addr, uint8_t speed) = 0;  // 0 auto, 1 low, 3 medium, 5 high (hyp, from F5 23B @16)
  // status bits, F5 31B @19 / 23B @11 (hyp: 0x01 on, 0x02 outdoor running, 0x04 fan, 0x20 heating demand)
  virtual void on_unit_status(uint8_t addr, uint8_t bits) = 0;
  virtual void on_system_value(SystemField field, float value) = 0;
  virtual void on_compressor(uint8_t state) = 0;  // 0 off, 1 start, 2 running
  virtual void on_clock(uint8_t hour, uint8_t minute) = 0;
  // F5 31B @18 bit 0x02: the unit is a group master (0x12) or a member (0x10)
  virtual void on_unit_role(uint8_t /*addr*/, bool /*master*/) {}
  // E9 page 6A: state and sub-state of outdoor module 1 (feed a DefrostDetector), and
  // oil return: a module in sub-state 07, every 8.4-8.7 compressor hours (2026-11-23..27)
  virtual void on_outdoor_module(uint8_t /*state*/, uint8_t /*sub_state*/) {}
  virtual void on_oil_return(bool /*active*/) {}
  // F5 23B @16 low nibble: airflow direction, 0-4 fixed position (0 horizontal,
  // 4 down), 7 swing (confirmed with the iTM, 2026-11-16)
  virtual void on_unit_airflow(uint8_t /*addr*/, uint8_t /*direction*/) {}
};

inline float t88(const uint8_t *frame, size_t offset) {
  return (float) ((uint16_t) (frame[offset] << 8 | frame[offset + 1])) / 256.0f;
}
// whole degrees, sign-magnitude: top bit = negative (0x88 = -8)
inline float sm8(uint8_t raw) { return (raw & 0x80) ? -(float) (raw & 0x7F) : (float) raw; }
inline float sm88(const uint8_t *frame, size_t offset) {
  uint16_t raw = (uint16_t) (frame[offset] << 8 | frame[offset + 1]);
  float magnitude = (float) (raw & 0x7FFF) / 256.0f;
  return (raw & 0x8000) ? -magnitude : magnitude;
}

// Frame length = 6 + payload length, checksum: sum(bytes[1..]) == 0 mod 256.
inline bool frame_valid(const uint8_t *frame, size_t length) {
  if (length < header::OVERHEAD || length != header::OVERHEAD + frame[header::PAYLOAD_LENGTH]) return false;
  uint8_t sum = 0;
  for (size_t i = 1; i < length; i++) sum += frame[i];
  return sum == 0;
}

// Checksum byte to append to a frame (without checksum).
inline uint8_t frame_checksum(const uint8_t *frame, size_t length_without_checksum) {
  uint8_t sum = 0;
  for (size_t i = 1; i < length_without_checksum; i++) sum += frame[i];
  return (uint8_t) (0x100 - sum);
}

inline int hexval(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

// Parse a P1P2Monitor line "R T  3.745: FD0AFF..." (also accepts the
// MQTT form with an inserted timestamp). Returns number of bytes, 0 on error.
inline size_t parse_raw_line(const char *line, uint8_t *out, size_t capacity, float *gap) {
  const char *gap_marker = strchr(line, 'T');
  const char *colon = strchr(line, ':');
  if (!colon) return 0;
  if (gap) *gap = (gap_marker && gap_marker < colon) ? (float) atof(gap_marker + 1) : -1.0f;
  const char *cursor = colon + 1;
  while (*cursor == ' ') cursor++;
  size_t length = 0;
  while (cursor[0] && cursor[1] && length < capacity) {
    int high = hexval(cursor[0]), low = hexval(cursor[1]);
    if (high < 0 || low < 0) break;
    out[length++] = (uint8_t) (high << 4 | low);
    cursor += 2;
  }
  return length;
}

// b1_to_diii: 256-entry map from B1 index to DIII address (0xFF = unknown).
// Frame types seen and described in docs/protocol.md, the same list as KNOWN and
// KNOWN_E9 in tools/find_unknown.py (tests/test_tools.py checks they match).
struct KnownFrame {
  uint8_t cmd;
  uint8_t length;
};
static const KnownFrame KNOWN_FRAMES[] = {
    {0xF5, 22}, {0xF5, 23}, {0xF5, 31}, {0xF5, 37}, {0xB1, 46}, {0xB1, 34}, {0xB1, 28}, {0xB1, 33},
    {0xA9, 24}, {0xA9, 16}, {0xE5, 12}, {0xFD, 11}, {0xFD, 12}, {0xA5, 12}, {0xA5, 26}, {0xA5, 35},
    {0xF5, 8},  {0xF5, 10}, {0xF5, 27}, {0xF5, 33}, {0xAD, 26}, {0xA9, 46}, {0xBD, 12}, {0xBD, 26},
    {0xA4, 26}, {0x06, 1},
};
// E9 pages: class byte (b3), length, page, and the sub-page (byte 11) of the 41 B
// page E0 (0xFF = any)
struct KnownE9 {
  uint8_t b3;
  uint8_t length;
  uint8_t page;
  uint8_t sub;
};
static const KnownE9 KNOWN_E9[] = {
    {0x08, 8, 0x2F, 0xFF},  {0x0C, 8, 0x2F, 0xFF},  {0x10, 8, 0x2F, 0xFF},  {0x08, 17, 0x68, 0xFF},
    {0x0C, 45, 0x6E, 0xFF}, {0x10, 44, 0x67, 0xFF}, {0x10, 46, 0x65, 0xFF}, {0x10, 46, 0x6A, 0xFF},
    {0x10, 43, 0x65, 0xFF},  // ERQ
    {0x1C, 36, 0xE6, 0xFF}, {0x1C, 46, 0xE0, 0xFF}, {0x10, 32, 0x6D, 0xFF}, {0x1C, 41, 0xE0, 0x02},
    {0x1C, 41, 0xE0, 0x03}, {0x1C, 41, 0xE0, 0x12}, {0x1C, 41, 0xE0, 0x13},
};

// One number per frame type, for remembering which unknown types were reported:
// cmd, length, and for E9 the page and the sub-page (41 B page E0) or class byte.
inline uint32_t frame_signature(const uint8_t *frame, size_t length) {
  uint32_t key = (uint32_t) frame[header::CMD] << 24 | (uint32_t) (length & 0xFF) << 16;
  if (frame[header::CMD] == 0xE9 && length > header::PAYLOAD) {
    const uint8_t page = frame[header::PAYLOAD];
    const bool sub_page = page == 0xE0 && length == e9_page_e0::LENGTH;
    key |= (uint32_t) page << 8 | (sub_page ? frame[e9_page_e0::SUB_PAGE] : frame[header::CLASS]);
  }
  return key;
}

// Whether the frame type is in the catalogue above. Call it on valid frames only.
inline bool is_known_frame(const uint8_t *frame, size_t length) {
  const uint8_t cmd = frame[header::CMD];
  if (cmd == 0xE9) {
    if (length <= header::PAYLOAD) return false;
    const uint8_t page = frame[header::PAYLOAD];
    const uint8_t sub = (page == 0xE0 && length == e9_page_e0::LENGTH) ? frame[e9_page_e0::SUB_PAGE] : 0xFF;
    for (const auto &known : KNOWN_E9)
      if (known.b3 == frame[header::CLASS] && known.length == length && known.page == page && known.sub == sub)
        return true;
    return false;
  }
  for (const auto &known : KNOWN_FRAMES)
    if (known.cmd == cmd && known.length == length) return true;
  return false;
}

inline void decode_frame(const uint8_t *frame, size_t length, const uint8_t *b1_to_diii, DecoderSink &sink) {
  if (!frame_valid(frame, length)) return;
  const uint8_t cmd = frame[header::CMD], src = frame[header::SRC];
  if (cmd == 0xF5) {
    if (length == f5_status::LENGTH) {
      sink.on_unit_mode(src, frame[f5_status::MODE]);
      sink.on_unit_value(src, UnitField::ROOM, t88(frame, f5_status::ROOM));
      sink.on_unit_fan(src, frame[f5_status::FAN_AIRFLOW] >> 4);
      sink.on_unit_airflow(src, frame[f5_status::FAN_AIRFLOW] & 0x0F);
      sink.on_unit_status(src, frame[f5_status::STATUS]);
    } else if (length == f5_hydrobox::LENGTH) {
      sink.on_unit_value(src, UnitField::ROOM, t88(frame, f5_hydrobox::TEMPERATURE));
      sink.on_unit_power(src, (frame[f5_hydrobox::STATUS] & 0x01) != 0);  // no 31B frame: power is status bit 0
      sink.on_unit_status(src, frame[f5_hydrobox::STATUS]);
      sink.on_unit_value(src, UnitField::DEMAND_STEP, (float) frame[f5_hydrobox::DEMAND_STEP]);
    } else if (length == f5_change::LENGTH) {
      sink.on_unit_power(src, frame[f5_change::POWER] != 0);
      if ((frame[f5_change::MODE] & 0xF0) == 0x60) sink.on_unit_mode(src, frame[f5_change::MODE]);
      const uint8_t setpoint = frame[f5_change::SETPOINT];
      if (setpoint >= 10 && setpoint <= 35) sink.on_unit_value(src, UnitField::SETPOINT, (float) setpoint);
      sink.on_unit_status(src, frame[f5_change::STATUS]);
      sink.on_unit_role(src, (frame[f5_change::ROLE] & 0x02) != 0);
    }
  } else if (cmd == 0xB1 && length == b1_unit::LENGTH) {
    uint8_t addr = b1_to_diii[src];
    if (addr == 0xFF) return;
    sink.on_unit_value(addr, UnitField::PIPE_A, t88(frame, b1_unit::PIPE_A));
    sink.on_unit_value(addr, UnitField::PIPE_B, t88(frame, b1_unit::PIPE_B));
    // whole degrees: byte 18 is not a fraction (flags)
    sink.on_unit_value(addr, UnitField::SETPOINT, (float) frame[b1_unit::SETPOINT]);
    sink.on_unit_value(addr, UnitField::REMOTE, t88(frame, b1_unit::REMOTE));
    sink.on_unit_value(addr, UnitField::INTAKE, t88(frame, b1_unit::INTAKE));
  } else if (cmd == 0xE9) {
    const uint8_t page = frame[header::PAYLOAD];
    const bool class_10 = frame[header::CLASS] == 0x10;
    if ((length == e9_page_65::LENGTH || length == e9_page_65::LENGTH_SHORT) && class_10 && page == 0x65) {
      sink.on_compressor(frame[e9_page_65::COMPRESSOR]);
      sink.on_system_value(SystemField::T_EVAP, sm88(frame, e9_page_65::T_EVAP));
      sink.on_system_value(SystemField::T_COND, sm88(frame, e9_page_65::T_COND));
      sink.on_system_value(SystemField::CURRENT, (float) (frame[e9_page_65::CURRENT] << 8 | frame[e9_page_65::CURRENT + 1]) / 10.0f);
      sink.on_system_value(SystemField::COMP_SPEED_TOTAL,
                           (float) (frame[e9_page_65::COMP_SPEED_TOTAL] << 8 | frame[e9_page_65::COMP_SPEED_TOTAL + 1]));
    } else if (length == e9_page_6a::LENGTH && class_10 && page == 0x6A) {
      sink.on_outdoor_module(frame[e9_page_6a::STATE_1], frame[e9_page_6a::SUB_STATE_1]);
      sink.on_oil_return(frame[e9_page_6a::SUB_STATE_1] == 0x07 || frame[e9_page_6a::SUB_STATE_2] == 0x07 ||
                         frame[e9_page_6a::SUB_STATE_3] == 0x07);
    } else if (length == e9_page_67::LENGTH && class_10 && page == 0x67) {
      sink.on_system_value(SystemField::OUTDOOR, sm88(frame, e9_page_67::OUTDOOR));
    } else if (length == e9_page_e0::LENGTH && page == 0xE0) {
      const uint8_t sub_page = frame[e9_page_e0::SUB_PAGE];
      if (sub_page == 0x12 || sub_page == 0x13) {
        bool module1 = sub_page == 0x12;
        sink.on_system_value(module1 ? SystemField::DISCHARGE_1 : SystemField::DISCHARGE_2,
                             sm8(frame[e9_page_e0::DISCHARGE]));
        sink.on_system_value(module1 ? SystemField::COMP_STEP_1 : SystemField::COMP_STEP_2,
                             (float) frame[e9_page_e0::COMP_STEP]);
        sink.on_system_value(module1 ? SystemField::FAN_STEP_1 : SystemField::FAN_STEP_2,
                             (float) frame[e9_page_e0::FAN_STEP]);
      } else if (sub_page == 0x02 || sub_page == 0x03) {
        sink.on_system_value(sub_page == 0x02 ? SystemField::COMP_SPEED_1 : SystemField::COMP_SPEED_2,
                             (float) frame[e9_page_e0::COMP_SPEED]);
      }
    } else if (length == e9_page_e6::LENGTH && page == 0xE6) {
      const uint8_t hour = frame[e9_page_e6::HOUR], minute = frame[e9_page_e6::MINUTE];
      if (hour < 24 && minute < 60) sink.on_clock(hour, minute);
    }
  }
}

// What a group is doing now (HA's hvac_action), from its mode and the status
// bits of its units (F5 byte 19 / 11): 0x20 is the unit's demand (thermo on),
// 0x04 its fan running. A group has demand when any of its units has.
// demand < 0 means no status frame seen yet. Demand in cool mode is taken as
// cooling by analogy; the bit is only confirmed for heating so far.
enum class Action : uint8_t { UNKNOWN, OFF, IDLE, HEATING, COOLING, FAN, DRYING };

inline Action group_action(int power, uint8_t mode, int demand, bool fan_running, float current, float target) {
  if (power < 0) return Action::UNKNOWN;
  if (power == 0) return Action::OFF;
  if (mode == 0x60) return fan_running ? Action::FAN : Action::IDLE;
  if (demand < 0) return Action::UNKNOWN;
  if (demand == 0) return Action::IDLE;
  switch (mode) {
    case 0x61: return Action::HEATING;
    case 0x62: return Action::COOLING;
    case 0x67: return Action::DRYING;
    case 0x63:
      // auto: the bus does not say which way it works; guess from the room
      if (std::isnan(current) || std::isnan(target)) return Action::UNKNOWN;
      return current < target ? Action::HEATING : Action::COOLING;
    default: return Action::UNKNOWN;
  }
}

}  // namespace diii
