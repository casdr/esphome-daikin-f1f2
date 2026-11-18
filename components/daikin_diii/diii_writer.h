#pragma once
// Bus write guards and queue for DIII-NET, plain C++ (no ESPHome dependencies)
// so it can be host-tested (tests/test_units.cpp). The hub owns one WriteQueue
// and feeds it the clock; see daikin_diii.cpp.

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "diii_decoder.h"

namespace diii {

enum class WriteResult : uint8_t { OK, DISABLED, BAD_HEX, BAD_LENGTH, ITM_SOURCE, QUEUE_FULL, BUDGET };

inline const char *write_result_str(WriteResult result) {
  switch (result) {
    case WriteResult::OK: return "ok";
    case WriteResult::DISABLED: return "writes disabled";
    case WriteResult::BAD_HEX: return "bad hex";
    case WriteResult::BAD_LENGTH: return "length field mismatch";
    case WriteResult::ITM_SOURCE: return "source 0x0A is the iTM";
    case WriteResult::QUEUE_FULL: return "queue full";
    case WriteResult::BUDGET: return "write budget used up";
  }
  return "?";
}

// The iTM's own source address. Writing with it makes the iTM raise error MC
// ("address duplicated"), so it is always refused.
static const uint8_t ITM_ADDRESS = 0x0A;

// P1P2Monitor serial commands that the research action atmega_command may pass
// on: V (version), T/O (write timing) and G/H (CRC settings), each with an
// optional hex/decimal argument. Everything else is refused, in particular W
// (raw frame write: frames must go through WriteQueue and its guards), L
// (controller mode) and newlines, which would chain more commands.
inline bool atmega_command_allowed(const std::string &cmd) {
  if (cmd.empty() || cmd.size() > 16) return false;
  switch (cmd[0]) {
    case 'V': case 'v': case 'T': case 't': case 'O': case 'o':
    case 'G': case 'g': case 'H': case 'h':
      break;
    default:
      return false;
  }
  for (size_t i = 1; i < cmd.size(); i++)
    if (hexval(cmd[i]) < 0) return false;
  return true;
}

// A setting command: A5 <src> <dst> .. .. 40 (operation: power, mode,
// setpoint) or 41 (fan). The indoor unit stores these settings in non-volatile
// memory, which wears out (see docs/protocol.md, "Write budget").
inline bool is_setting_command(const uint8_t *frame, size_t length) {
  return length >= header::OVERHEAD && frame[header::CMD] == 0xA5 &&
         (frame[header::PAYLOAD] == 0x40 || frame[header::PAYLOAD] == 0x41);
}

// Operation command 40 (power, mode, setpoint), without checksum:
// A5 <src> <dst> 14 <len> | 40 PP MM XX 00 00 SP SF 00 00 80 00 ...
// PP (power_byte) 03 on / 02 off / 00 unchanged, MM (mode_byte) 0x10|mode (0 fan,
// 1 heat, 2 cool) or 00, XX (mode_detail) follows the mode (00 fan, 01 heat,
// 02 cool; what else it selects is not known), SP (setpoint) in whole degrees
// with SF 0x80, or both 00 (docs/protocol.md).
// Indoor units take a 20-byte payload. The hydrobox takes the same first 11
// payload bytes in a 29-byte payload padded with zeros, as the iTM sends it:
//   A5 0A 10 14 1D 40 03 00 .. (on), 40 02 00 .. (off), 40 00 11 01 00 .. (heat)
// Its water setpoint is somewhere in the padding, encoding unknown.
static const size_t OPERATION_LEN = 25;
static const size_t HYDROBOX_OPERATION_LEN = 34;
inline size_t build_operation(uint8_t *frame, uint8_t src, uint8_t dst, bool hydrobox, uint8_t power_byte,
                              uint8_t mode_byte, uint8_t mode_detail, uint8_t setpoint) {
  const size_t length = hydrobox ? HYDROBOX_OPERATION_LEN : OPERATION_LEN;
  for (size_t i = 0; i < length; i++) frame[i] = 0;
  frame[header::CMD] = 0xA5;
  frame[header::SRC] = src;
  frame[header::DST] = dst;
  frame[header::CLASS] = 0x14;
  frame[header::PAYLOAD_LENGTH] = (uint8_t) (length - header::PAYLOAD);  // without checksum here
  frame[header::PAYLOAD] = 0x40;
  frame[a5_operation::POWER] = power_byte;
  frame[a5_operation::MODE] = mode_byte;
  frame[a5_operation::MODE_DETAIL] = mode_detail;
  frame[a5_operation::SETPOINT] = setpoint;
  frame[a5_operation::SETPOINT_VALID] = setpoint ? 0x80 : 0x00;
  frame[a5_operation::SOURCE_FLAG] = 0x80;
  return length;
}

// Group command 41 (fan speed and/or airflow direction), without checksum:
// A5 <src> <group> 14 06 | 41 16 XX 00 00 00. XX high nibble 0x80|speed<<4
// (0 auto, 1 low, 3 medium, 5 high), low nibble 0x08|direction (0-4 position,
// 7 swing); a nibble of 0 leaves that setting unchanged (docs/protocol.md).
// fan / airflow < 0 = unchanged. Returns false for values the units do not know.
static const size_t FAN_AIRFLOW_LEN = 11;
inline bool build_fan_airflow(uint8_t *frame, uint8_t src, uint8_t group, int fan, int airflow) {
  if (fan >= 0 && fan != 0 && fan != 1 && fan != 3 && fan != 5) return false;
  if (airflow >= 0 && airflow > 4 && airflow != 7) return false;
  if (fan < 0 && airflow < 0) return false;
  uint8_t fan_airflow = 0;
  if (fan >= 0) fan_airflow |= (uint8_t) (0x80 | (fan << 4));
  if (airflow >= 0) fan_airflow |= (uint8_t) (0x08 | airflow);
  const uint8_t bytes[FAN_AIRFLOW_LEN] = {0xA5, src, group, 0x14, 0x06, 0x41, 0x16, fan_airflow, 0x00, 0x00, 0x00};
  for (size_t i = 0; i < FAN_AIRFLOW_LEN; i++) frame[i] = bytes[i];
  return true;
}

// Per-outlet flap setting of a unit, by pair of opposite outlets (docs/protocol.md,
// "Writing it from F1F2"): the trailing bytes of the same 41 command,
// 41 16 00 b8 b9 00. Only the four frames below were seen to work (2026-11-16):
//   normal:  b8 F4, b9 A0 (outlets 1+3) / F0 (outlets 2+4)
//   fixed 0: b8 FF, b9 AF (outlets 1+3) / FF (outlets 2+4); with b9 low 0 instead
//            of F the unit ignores it (FF A0, FF 40, FF 80, FF 50 did nothing)
// Blocking a single outlet is not possible this way.
enum class OutletPair : uint8_t { OUTLETS_1_3 = 0, OUTLETS_2_4 = 1 };
inline void build_outlet_pair(uint8_t *frame, uint8_t src, uint8_t dst, OutletPair pair, bool fixed) {
  const uint8_t bytes[FAN_AIRFLOW_LEN] = {0xA5, src, dst, 0x14, 0x06, 0x41, 0x16, 0x00,
                                          (uint8_t) (fixed ? 0xFF : 0xF4),
                                          (uint8_t) ((pair == OutletPair::OUTLETS_1_3 ? 0xA0 : 0xF0) | (fixed ? 0x0F : 0x00)),
                                          0x00};
  for (size_t i = 0; i < FAN_AIRFLOW_LEN; i++) frame[i] = bytes[i];
}

// Token bucket per destination (group master), to keep automatic control
// within Daikin's limit of 7000 control commands per indoor unit per year
// (Modbus Interface DIII design guide). `burst` commands are available at
// once; one token comes back every `refill_ms`. The clock is millis(); the
// wrap after 49 days can only make the refill slower.
class WriteBudget {
 public:
  static const uint16_t DEFAULT_BURST = 30;
  static const uint32_t DEFAULT_REFILL_MS = 4505143;  // 365 d / 7000 = 75 min

  // burst 0 disables the budget (every command allowed).
  void configure(uint16_t burst, uint32_t refill_ms) {
    this->burst_ = burst;
    this->refill_ms_ = refill_ms == 0 ? 1 : refill_ms;
    for (auto &entry : this->entries_)
      if (entry.tokens > burst) entry.tokens = burst;
  }
  bool enabled() const { return this->burst_ != 0; }
  uint16_t burst() const { return this->burst_; }

  uint16_t available(uint8_t dst, uint32_t now_ms) {
    if (!this->enabled()) return 0;
    Entry *entry = this->entry_(dst, now_ms);
    if (entry == nullptr) return 0;
    this->refill_(*entry, now_ms);
    return entry->tokens;
  }
  bool take(uint8_t dst, uint32_t now_ms) {
    if (!this->enabled()) return true;
    Entry *entry = this->entry_(dst, now_ms);
    if (entry == nullptr) return false;
    this->refill_(*entry, now_ms);
    if (entry->tokens == 0) return false;
    entry->tokens--;
    return true;
  }
  // Restore a saved token count (after a restart). The refill restarts at
  // now_ms, so time spent rebooting is not credited.
  void restore(uint8_t dst, uint16_t tokens, uint32_t now_ms) {
    Entry *entry = this->entry_(dst, now_ms);
    if (entry == nullptr) return;
    entry->tokens = tokens > this->burst_ ? this->burst_ : tokens;
    entry->last_ms = now_ms;
  }
  // For saving: destination and tokens of every tracked group.
  size_t size() const { return this->entries_.size(); }
  uint8_t dst_at(size_t i) const { return this->entries_[i].dst; }
  uint16_t tokens_at(size_t i) const { return this->entries_[i].tokens; }

  static const size_t MAX_ENTRIES = 32;

 protected:
  struct Entry {
    uint8_t dst;
    uint16_t tokens;
    uint32_t last_ms;
  };
  Entry *entry_(uint8_t dst, uint32_t now_ms) {
    for (auto &entry : this->entries_)
      if (entry.dst == dst) return &entry;
    if (this->entries_.size() >= MAX_ENTRIES) return nullptr;
    this->entries_.push_back(Entry{dst, this->burst_, now_ms});
    return &this->entries_.back();
  }
  void refill_(Entry &entry, uint32_t now_ms) {
    uint32_t intervals = (now_ms - entry.last_ms) / this->refill_ms_;
    if (intervals > 0) {
      uint32_t refilled = entry.tokens + intervals;
      entry.tokens = refilled > this->burst_ ? this->burst_ : (uint16_t) refilled;
      entry.last_ms += intervals * this->refill_ms_;
    }
    if (entry.tokens >= this->burst_) entry.last_ms = now_ms;  // a full bucket does not save up
  }
  uint16_t burst_{DEFAULT_BURST};
  uint32_t refill_ms_{DEFAULT_REFILL_MS};
  std::vector<Entry> entries_;
};

// The last value this bridge commanded for one setting, used to decide
// whether a new command changes anything. The F5 status frames lag behind a
// command by seconds to a minute, so until the bus reports the commanded
// value (at most PENDING_MS) that value counts as the current one. Without
// this, a quick undo (22 -> 23 -> 22) would compare with the stale bus value
// 22 and be skipped. Once confirmed, the bus is the truth again, so a later
// change on the iTM is not masked.
class PendingSetting {
 public:
  static const uint32_t PENDING_MS = 60000;

  void commanded(int value, uint32_t now_ms) {
    this->value_ = value;
    this->ms_ = now_ms;
    this->set_ = true;
  }
  // The bus reported a value: the command is confirmed when it matches.
  void observe(int bus_value) {
    if (this->set_ && bus_value == this->value_) this->set_ = false;
  }
  // Current value: the commanded one while pending, else the bus value.
  int current(int bus_value, uint32_t now_ms) const {
    return (this->set_ && now_ms - this->ms_ < PENDING_MS) ? this->value_ : bus_value;
  }
  // Whether sending `target` would change the setting.
  bool changes(int target, int bus_value, bool bus_unknown, uint32_t now_ms) const {
    bool pending = this->set_ && now_ms - this->ms_ < PENDING_MS;
    if (!pending && bus_unknown) return true;
    return this->current(bus_value, now_ms) != target;
  }

 protected:
  int value_{0};
  uint32_t ms_{0};
  bool set_{false};
};

// What became of the frame in flight, see WriteQueue::check_echo().
enum class EchoResult : uint8_t { NONE, RETRY, LOST };

// Frames go out one at a time. A frame counts as written only once the ATmega
// reads it back from the bus intact (its echo, an R line with the same bytes):
// "sent" only means the ATmega took it, and on a busy bus it can still refuse
// it (when the previous frame is still waiting for a gap), drop it, or put it
// on the bus damaged. Until the echo arrives (or ECHO_TIMEOUT_MS passes) the
// next frame waits; a frame without an intact echo is sent again, at most
// MAX_RETRIES times, and then counts as lost.
class WriteQueue {
 public:
  static const size_t MAX_QUEUE = 16;
  static const uint32_t SPACING_MS = 1500;  // minimum time between two sends
  static const uint8_t MAX_RETRIES = 3;
  // A refusal/collision line only counts for the frame sent within this window.
  static const uint32_t RETRY_WINDOW_MS = 1500;
  // Longest wait for the echo. On a real bus the ATmega put frames out
  // within 2.5 s of taking them, 7 s once on a busy bus (2026-11-17).
  static const uint32_t ECHO_TIMEOUT_MS = 10000;

  void set_enabled(bool enable) {
    this->enabled_ = enable;
    if (!enable) {
      this->queue_.clear();
      this->in_flight_ = false;
    }
  }
  bool enabled() const { return this->enabled_; }
  size_t size() const { return this->queue_.size(); }

  WriteBudget &budget() { return this->budget_; }

  // Frame without checksum; the checksum byte is appended. On OK the frame is
  // queued as uppercase hex (with checksum). A setting command also takes a
  // token from its destination's write budget.
  WriteResult enqueue(const uint8_t *frame, size_t length, uint32_t now_ms) {
    if (!this->enabled_) return WriteResult::DISABLED;
    // frame without checksum: header + payload
    if (length < header::PAYLOAD || length >= MAX_FRAME || length != header::PAYLOAD + frame[header::PAYLOAD_LENGTH])
      return WriteResult::BAD_LENGTH;
    if (frame[header::SRC] == ITM_ADDRESS) return WriteResult::ITM_SOURCE;
    if (this->queue_.size() >= MAX_QUEUE) return WriteResult::QUEUE_FULL;
    if (is_setting_command(frame, length) && !this->budget_.take(frame[header::DST], now_ms))
      return WriteResult::BUDGET;
    char hex[2 * MAX_FRAME + 2];
    for (size_t i = 0; i < length; i++) snprintf(hex + 2 * i, 3, "%02X", frame[i]);
    snprintf(hex + 2 * length, 3, "%02X", frame_checksum(frame, length));
    this->queue_.push_back(Item{hex, 0});
    return WriteResult::OK;
  }

  WriteResult enqueue_hex(const std::string &hex, uint32_t now_ms) {
    if (hex.size() % 2 != 0) return WriteResult::BAD_HEX;
    uint8_t frame[MAX_FRAME];
    size_t length = 0;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
      int high = hexval(hex[i]), low = hexval(hex[i + 1]);
      if (high < 0 || low < 0) return WriteResult::BAD_HEX;
      if (length >= sizeof(frame)) return WriteResult::BAD_LENGTH;
      frame[length++] = (uint8_t) (high << 4 | low);
    }
    return this->enqueue(frame, length, now_ms);
  }

  // The frame in flight has had no intact echo for ECHO_TIMEOUT_MS: queue it
  // again (RETRY, with its retry number) or give up on it (LOST). `frame` is
  // the frame concerned. Call before next().
  EchoResult check_echo(uint32_t now_ms, std::string &frame, uint8_t *retry_no = nullptr) {
    if (!this->in_flight_ || now_ms - this->last_send_ms_ < ECHO_TIMEOUT_MS) return EchoResult::NONE;
    this->in_flight_ = false;
    frame = this->last_.hex;
    if (this->last_.retries >= MAX_RETRIES) {
      this->lost_++;
      return EchoResult::LOST;
    }
    Item again{this->last_.hex, (uint8_t) (this->last_.retries + 1)};
    if (retry_no != nullptr) *retry_no = again.retries;
    this->queue_.push_front(again);
    return EchoResult::RETRY;
  }

  // A valid frame was read from the bus. Returns true when it is the echo of
  // the frame in flight, which is then done.
  bool on_bus_frame(const uint8_t *frame, size_t length) {
    if (!this->in_flight_ || this->last_.hex.size() != 2 * length) return false;
    for (size_t i = 0; i < length; i++) {
      int high = hexval(this->last_.hex[2 * i]), low = hexval(this->last_.hex[2 * i + 1]);
      if (high < 0 || low < 0 || (uint8_t) (high << 4 | low) != frame[i]) return false;
    }
    this->in_flight_ = false;
    return true;
  }

  // Frames given up on after MAX_RETRIES sends without an intact echo.
  uint32_t lost() const { return this->lost_; }
  bool in_flight() const { return this->in_flight_; }

  // Next frame to send at time now_ms: none while the previous one waits for
  // its echo, and not within SPACING_MS of the previous send. Returns false
  // when nothing is due.
  bool next(uint32_t now_ms, std::string &frame) {
    if (this->queue_.empty() || !this->enabled_) return false;
    if (this->in_flight_) return false;
    if (this->sent_any_ && now_ms - this->last_send_ms_ < SPACING_MS) return false;
    this->last_ = this->queue_.front();
    this->queue_.pop_front();
    this->last_send_ms_ = now_ms;
    this->sent_any_ = true;
    this->in_flight_ = true;
    frame = this->last_.hex;
    return true;
  }

  // The ATmega reported a refusal or collision. Re-queue the last frame at the
  // front, but only if it was sent within RETRY_WINDOW_MS and has retries
  // left (otherwise it counts as lost). A frame is retried at most once per
  // refusal line. A refusal outside the window is left to check_echo().
  bool on_refused(uint32_t now_ms, uint8_t *retry_no = nullptr) {
    if (!this->in_flight_ || now_ms - this->last_send_ms_ >= RETRY_WINDOW_MS) return false;
    this->in_flight_ = false;
    if (this->last_.retries >= MAX_RETRIES) {
      this->lost_++;
      return false;
    }
    Item again{this->last_.hex, (uint8_t) (this->last_.retries + 1)};
    if (retry_no != nullptr) *retry_no = again.retries;
    this->queue_.push_front(again);
    return true;
  }

 protected:
  struct Item {
    std::string hex;
    uint8_t retries;
  };
  std::deque<Item> queue_;
  WriteBudget budget_;
  Item last_{};
  bool enabled_{false};
  bool in_flight_{false};
  bool sent_any_{false};
  uint32_t last_send_ms_{0};
  uint32_t lost_{0};
};

}  // namespace diii
