#pragma once
// Daikin F1F2 / DIII-NET frame decoder. Pure C++ (no ESPHome dependency) so it
// can be unit-tested on the host. Field layout: see docs/protocol.md.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace diii {

static const size_t MAX_FRAME = 96;

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
  CAPACITY,      // E9 page 65 @36 (0-255, hyp: capacity / load)
  COMP_STEP_1,   // E9 page E0/12 @37 (hyp: compressor step, 0 = off)
  COMP_STEP_2,   // E9 page E0/13 @37
  FAN_STEP_1,    // E9 page E0/12 @39 (hyp: outdoor fan step)
  FAN_STEP_2,    // E9 page E0/13 @39
  COMP_FREQ_1,   // E9 page E0/02 @22 (hyp: compressor frequency, Hz)
  COMP_FREQ_2,   // E9 page E0/03 @22
  COUNT
};

// Receives decoded values. Called for every frame that carries the value
// (no change detection here).
class DecoderSink {
 public:
  virtual ~DecoderSink() = default;
  virtual void on_unit_value(uint8_t addr, UnitField f, float v) = 0;
  virtual void on_unit_power(uint8_t addr, bool on) = 0;
  virtual void on_unit_mode(uint8_t addr, uint8_t mode) = 0;  // 0x60 fan, 0x61 heat, 0x62 cool, ...
  virtual void on_unit_fan(uint8_t addr, uint8_t speed) = 0;  // 0 auto, 1 low, 3 medium, 5 high (hyp, from F5 23B @16)
  // status bits, F5 31B @19 / 23B @11 (hyp: 0x01 on, 0x02 outdoor running, 0x04 fan, 0x20 heating demand)
  virtual void on_unit_status(uint8_t addr, uint8_t bits) = 0;
  virtual void on_system_value(SystemField f, float v) = 0;
  virtual void on_compressor(uint8_t state) = 0;  // 0 off, 1 start, 2 running
  virtual void on_clock(uint8_t hour, uint8_t minute) = 0;
  // F5 31B @18 bit 0x02: the unit is a group master (0x12) or a member (0x10)
  virtual void on_unit_role(uint8_t /*addr*/, bool /*master*/) {}
  // E9 page 6A: an outdoor module in sub-state 07 (hyp: defrost or oil return)
  virtual void on_defrost(bool /*active*/) {}
  // F5 23B @16 low nibble: airflow direction, 0-4 fixed position (0 horizontal,
  // 4 down), 7 swing (confirmed with the iTM, 2026-11-16)
  virtual void on_unit_airflow(uint8_t /*addr*/, uint8_t /*direction*/) {}
};

inline float t88(const uint8_t *b, size_t o) { return (float) ((uint16_t) (b[o] << 8 | b[o + 1])) / 256.0f; }
inline float t88s(const uint8_t *b, size_t o) { return (float) ((int16_t) (b[o] << 8 | b[o + 1])) / 256.0f; }
inline float sm88(const uint8_t *b, size_t o) {
  uint16_t v = (uint16_t) (b[o] << 8 | b[o + 1]);
  float f = (float) (v & 0x7FFF) / 256.0f;
  return (v & 0x8000) ? -f : f;
}

// Frame length = 6 + payload length, checksum: sum(bytes[1..]) == 0 mod 256.
inline bool frame_valid(const uint8_t *b, size_t len) {
  if (len < 6 || len != (size_t) 6 + b[4]) return false;
  uint8_t s = 0;
  for (size_t i = 1; i < len; i++) s += b[i];
  return s == 0;
}

// Checksum byte to append to a frame (without checksum).
inline uint8_t frame_checksum(const uint8_t *b, size_t len_without_cs) {
  uint8_t s = 0;
  for (size_t i = 1; i < len_without_cs; i++) s += b[i];
  return (uint8_t) (0x100 - s);
}

inline int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Parse a P1P2Monitor line "R T  3.745: FD0AFF..." (also accepts the
// MQTT form with an inserted timestamp). Returns number of bytes, 0 on error.
inline size_t parse_raw_line(const char *line, uint8_t *out, size_t cap, float *gap) {
  const char *t = strchr(line, 'T');
  const char *colon = strchr(line, ':');
  if (!colon) return 0;
  if (gap) *gap = (t && t < colon) ? (float) atof(t + 1) : -1.0f;
  const char *p = colon + 1;
  while (*p == ' ') p++;
  size_t n = 0;
  while (p[0] && p[1] && n < cap) {
    int hi = hexval(p[0]), lo = hexval(p[1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = (uint8_t) (hi << 4 | lo);
    p += 2;
  }
  return n;
}

// b1_to_diii: 256-entry map from B1 index to DIII address (0xFF = unknown).
inline void decode_frame(const uint8_t *b, size_t len, const uint8_t *b1_to_diii, DecoderSink &sink) {
  if (!frame_valid(b, len)) return;
  const uint8_t cmd = b[0], src = b[1];
  if (cmd == 0xF5) {
    if (len == 23) {
      sink.on_unit_mode(src, b[6]);
      sink.on_unit_value(src, UnitField::ROOM, t88(b, 14));
      sink.on_unit_fan(src, b[16] >> 4);
      sink.on_unit_airflow(src, b[16] & 0x0F);
      sink.on_unit_status(src, b[11]);
    } else if (len == 22) {  // hydrobox
      sink.on_unit_value(src, UnitField::ROOM, t88(b, 14));
      sink.on_unit_power(src, (b[11] & 0x01) != 0);  // no 31B frame: power is status bit 0
      sink.on_unit_status(src, b[11]);
      sink.on_unit_value(src, UnitField::DEMAND_STEP, (float) b[9]);
    } else if (len == 31) {
      sink.on_unit_power(src, b[6] != 0);
      if ((b[7] & 0xF0) == 0x60) sink.on_unit_mode(src, b[7]);
      if (b[13] >= 10 && b[13] <= 35) sink.on_unit_value(src, UnitField::SETPOINT, (float) b[13]);
      sink.on_unit_status(src, b[19]);
      sink.on_unit_role(src, (b[18] & 0x02) != 0);
    }
  } else if (cmd == 0xB1 && len == 46) {
    uint8_t addr = b1_to_diii[src];
    if (addr == 0xFF) return;
    sink.on_unit_value(addr, UnitField::PIPE_A, t88(b, 13));
    sink.on_unit_value(addr, UnitField::PIPE_B, t88(b, 15));
    sink.on_unit_value(addr, UnitField::SETPOINT, (float) b[17]);  // byte 18 is not a fraction (flags)
    sink.on_unit_value(addr, UnitField::REMOTE, t88(b, 30));
    sink.on_unit_value(addr, UnitField::INTAKE, t88(b, 43));
  } else if (cmd == 0xE9) {
    const uint8_t page = b[5];
    if (len == 46 && b[3] == 0x10 && page == 0x65) {
      sink.on_compressor(b[11]);
      sink.on_system_value(SystemField::T_EVAP, sm88(b, 15));
      sink.on_system_value(SystemField::T_COND, t88(b, 17));
      sink.on_system_value(SystemField::CAPACITY, (float) b[36]);
    } else if (len == 46 && b[3] == 0x10 && page == 0x6A) {
      // three modules as (state, sub-state) at @11..16
      sink.on_defrost(b[12] == 0x07 || b[14] == 0x07 || b[16] == 0x07);
    } else if (len == 44 && b[3] == 0x10 && page == 0x67) {
      sink.on_system_value(SystemField::OUTDOOR, t88s(b, 21));
    } else if (len == 41 && page == 0xE0 && (b[11] == 0x12 || b[11] == 0x13)) {
      bool m1 = b[11] == 0x12;
      sink.on_system_value(m1 ? SystemField::DISCHARGE_1 : SystemField::DISCHARGE_2, (float) b[16]);
      sink.on_system_value(m1 ? SystemField::COMP_STEP_1 : SystemField::COMP_STEP_2, (float) b[37]);
      sink.on_system_value(m1 ? SystemField::FAN_STEP_1 : SystemField::FAN_STEP_2, (float) b[39]);
    } else if (len == 41 && page == 0xE0 && (b[11] == 0x02 || b[11] == 0x03)) {
      sink.on_system_value(b[11] == 0x02 ? SystemField::COMP_FREQ_1 : SystemField::COMP_FREQ_2, (float) b[22]);
    } else if (len == 36 && page == 0xE6) {
      if (b[11] < 24 && b[12] < 60) sink.on_clock(b[11], b[12]);
    }
  }
}

}  // namespace diii
