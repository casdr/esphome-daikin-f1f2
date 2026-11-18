// Host unit tests: bus write guards, queue/retry timing (diii_writer.h) and a
// few hand-checked decoder values (diii_decoder.h). Exit code != 0 on failure.
//   make -C esphome/tests test
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "diii_decoder.h"
#include "diii_writer.h"

using namespace diii;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                  \
    }                                                              \
  } while (0)

static std::vector<uint8_t> hex(const char *text) {
  std::vector<uint8_t> bytes;
  for (; text[0] && text[1]; text += 2) bytes.push_back((uint8_t) (hexval(text[0]) << 4 | hexval(text[1])));
  return bytes;
}

// iTM-style power-on command for group 0x07 from source 0x30, without checksum.
static const char *POWER_ON = "A5300714144003000000000000000080000000000000000000";

static void test_guards() {
  WriteQueue queue;
  auto frame = hex(POWER_ON);
  CHECK(queue.enqueue(frame.data(), frame.size(), 0) == WriteResult::DISABLED);  // default off in the queue itself
  queue.set_enabled(true);
  CHECK(queue.enqueue(frame.data(), frame.size(), 0) == WriteResult::OK);
  CHECK(queue.size() == 1);

  auto itm = frame;
  itm[1] = ITM_ADDRESS;
  CHECK(queue.enqueue(itm.data(), itm.size(), 0) == WriteResult::ITM_SOURCE);

  auto short_frame = frame;
  short_frame.pop_back();  // length byte says 0x14 payload bytes, one missing
  CHECK(queue.enqueue(short_frame.data(), short_frame.size(), 0) == WriteResult::BAD_LENGTH);
  CHECK(queue.enqueue(frame.data(), 3, 0) == WriteResult::BAD_LENGTH);

  CHECK(queue.enqueue_hex("A530ZZ", 0) == WriteResult::BAD_HEX);
  CHECK(queue.enqueue_hex("A530071", 0) == WriteResult::BAD_HEX);  // odd length
  CHECK(queue.enqueue_hex(std::string(400, 'A'), 0) == WriteResult::BAD_LENGTH);

  while (queue.size() < WriteQueue::MAX_QUEUE) CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::QUEUE_FULL);

  queue.set_enabled(false);  // switching writes off drops everything queued
  CHECK(queue.size() == 0);
  std::string out;
  CHECK(!queue.next(10000, out));
}

static void test_checksum_and_spacing() {
  WriteQueue queue;
  queue.set_enabled(true);
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  std::string out;
  CHECK(queue.next(1000, out));
  // the queued frame carries a checksum that makes it a valid frame
  CHECK(out.size() == 2 * 26);
  auto frame = hex(out.c_str());
  CHECK(frame_valid(frame.data(), frame.size()));
  CHECK(out.rfind(POWER_ON, 0) == 0);
  CHECK(!queue.next(1000 + WriteQueue::SPACING_MS, out));  // no echo yet
  CHECK(queue.on_bus_frame(frame.data(), frame.size()));
  CHECK(!queue.next(1000 + WriteQueue::SPACING_MS - 1, out));  // echo seen, but too soon
  CHECK(queue.next(1000 + WriteQueue::SPACING_MS, out));
  CHECK(queue.size() == 0);
}

static void test_retry() {
  WriteQueue queue;
  queue.set_enabled(true);
  std::string out;
  uint8_t retry_no = 0;

  // collision right after sending -> same frame again, then success
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  uint32_t now = 1000;
  CHECK(queue.next(now, out));
  std::string first = out;
  CHECK(queue.on_refused(now + 50, &retry_no) && retry_no == 1);
  CHECK(!queue.on_refused(now + 60));  // a second line for the same send does not queue it twice
  CHECK(queue.size() == 1);
  now += WriteQueue::SPACING_MS;
  CHECK(queue.next(now, out) && out == first);
  CHECK(queue.size() == 0);

  // retries run out after MAX_RETRIES
  for (int i = 2; i <= WriteQueue::MAX_RETRIES; i++) {
    CHECK(queue.on_refused(now + 10, &retry_no) && retry_no == i);
    now += WriteQueue::SPACING_MS;
    CHECK(queue.next(now, out) && out == first);
  }
  CHECK(queue.lost() == 0);
  CHECK(!queue.on_refused(now + 10));
  CHECK(queue.lost() == 1);  // refused on the last retry: given up
  CHECK(queue.size() == 0);

  // a refusal long after the send must not replay the old frame
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  now += 10 * WriteQueue::SPACING_MS;
  CHECK(queue.next(now, out));
  CHECK(!queue.on_refused(now + WriteQueue::RETRY_WINDOW_MS));
  CHECK(!queue.on_refused(now + 3600000));
  CHECK(queue.size() == 0);

  // the same command sent again later gets its full retry budget
  auto echo = hex(out.c_str());
  CHECK(queue.on_bus_frame(echo.data(), echo.size()));  // the previous one made it
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  now += WriteQueue::SPACING_MS;
  CHECK(queue.next(now, out));
  CHECK(queue.on_refused(now + 10, &retry_no) && retry_no == 1);

  // nothing sent yet: a refusal line is ignored
  WriteQueue fresh;
  fresh.set_enabled(true);
  CHECK(!fresh.on_refused(5));
}

struct Capture : DecoderSink {
  int calls = 0, mode = -1, fan = -1, airflow = -1, status = -1, power = -1, master = -1, defrost = -1;
  float room = NAN, step = NAN;
  void on_unit_value(uint8_t addr, UnitField field, float value) override {
    calls++;
    if (addr == 0x08 && field == UnitField::ROOM) room = value;
    if (field == UnitField::DEMAND_STEP) step = value;
  }
  void on_unit_role(uint8_t, bool is_master) override { calls++, master = is_master; }
  void on_defrost(bool active) override { calls++, defrost = active; }
  void on_unit_power(uint8_t, bool on) override { calls++, power = on; }
  void on_unit_mode(uint8_t, uint8_t unit_mode) override { calls++, mode = unit_mode; }
  void on_unit_fan(uint8_t, uint8_t speed) override { calls++, fan = speed; }
  void on_unit_airflow(uint8_t, uint8_t direction) override { calls++, airflow = direction; }
  void on_unit_status(uint8_t, uint8_t bits) override { calls++, status = bits; }
  void on_system_value(SystemField, float) override { calls++; }
  void on_compressor(uint8_t) override { calls++; }
  void on_clock(uint8_t, uint8_t) override { calls++; }
};

static void test_decoder_known_values() {
  uint8_t b1_to_diii[256];
  for (auto &entry : b1_to_diii) entry = 0xFF;
  // F5 status of unit 1-08 (captures/after-exp3.tsv), checked by hand:
  // mode 0x61 heat, status 0x02, room 0x1227/256 = 18.15 C, fan nibble 3 = medium
  auto frame = hex("F508FF1C1182610100000002000012273700008068008E");
  CHECK(frame_valid(frame.data(), frame.size()));
  Capture capture;
  decode_frame(frame.data(), frame.size(), b1_to_diii, capture);
  CHECK(capture.mode == 0x61);
  CHECK(capture.status == 0x02);
  CHECK(capture.fan == 3);
  CHECK(capture.airflow == 7);  // byte 16 = 0x37: medium, swing
  CHECK(std::fabs(capture.room - 18.15234375f) < 1e-6);

  // bad checksum: ignored
  auto bad = frame;
  bad.back() ^= 0x01;
  CHECK(!frame_valid(bad.data(), bad.size()));
  Capture capture_bad;
  decode_frame(bad.data(), bad.size(), b1_to_diii, capture_bad);
  CHECK(capture_bad.calls == 0);

  // truncated frame: ignored
  Capture capture_truncated;
  decode_frame(frame.data(), frame.size() - 1, b1_to_diii, capture_truncated);
  CHECK(capture_truncated.calls == 0);

  // Real frames from a bus log (docs/protocol.md, "Overnight log check"):
  // E9 page 6A with modules 1+2 in sub-state 07 (06:55:20) and back to normal
  Capture capture_defrost, capture_normal;
  auto defrost = hex("E900FF10286A6D67C0180101070107020020202007D014C80000000000000000000000000000000000000000008D");
  decode_frame(defrost.data(), defrost.size(), b1_to_diii, capture_defrost);
  CHECK(capture_defrost.defrost == 1);
  auto normal = hex("E900FF10286A6D67C0180101000100010020202007D014C80000000000000000000000000000000000000000009C");
  decode_frame(normal.data(), normal.size(), b1_to_diii, capture_normal);
  CHECK(capture_normal.defrost == 0);
  // hydrobox F5 22B with heat demand: status 0x23 (on, outdoor running, demand), step 0x0F
  Capture capture_hydrobox;
  auto hydrobox = hex("F510FF1C10820101000F0023100017BE000000807634");
  decode_frame(hydrobox.data(), hydrobox.size(), b1_to_diii, capture_hydrobox);
  CHECK(capture_hydrobox.power == 1);
  CHECK(capture_hydrobox.status == 0x23);
  CHECK(capture_hydrobox.step == 15.0f);
  // F5 31B byte 18: group master 1-07 (0x12) and member 1-05 (0x10)
  Capture capture_master, capture_member;
  auto master_status = hex("F507FF1C198000610100000000160000000012000004020000800000000035");
  decode_frame(master_status.data(), master_status.size(), b1_to_diii, capture_master);
  CHECK(capture_master.master == 1);
  auto member_status = hex("F505FF1C198000610100000000160000000010000004120000800001000028");
  decode_frame(member_status.data(), member_status.size(), b1_to_diii, capture_member);
  CHECK(capture_member.master == 0);

  // P1P2Monitor line parsing
  uint8_t parsed[MAX_FRAME];
  float gap = 0;
  CHECK(parse_raw_line("R T  3.745: FD0AFF080550B0000100E9", parsed, sizeof(parsed), &gap) == 11);
  CHECK(std::fabs(gap - 3.745f) < 1e-4);
  CHECK(parsed[0] == 0xFD && parsed[10] == 0xE9);
  CHECK(parse_raw_line("* no frame here", parsed, sizeof(parsed), nullptr) == 0);

  // signed temperatures
  uint8_t neg[2] = {0x80 | 0x02, 0x80};  // sign-magnitude -2.5
  CHECK(std::fabs(sm88(neg, 0) + 2.5f) < 1e-6);
  uint8_t twos[2] = {0xFD, 0x80};  // two's complement -2.5
  CHECK(std::fabs(t88s(twos, 0) + 2.5f) < 1e-6);
}

static void test_budget() {
  const uint32_t refill_ms = WriteBudget::DEFAULT_REFILL_MS;
  WriteBudget budget;  // defaults: 30 at once, one back every 75 min (7000/year)
  CHECK(budget.available(0x07, 0) == WriteBudget::DEFAULT_BURST);
  for (int i = 0; i < WriteBudget::DEFAULT_BURST; i++) CHECK(budget.take(0x07, 1000));
  CHECK(!budget.take(0x07, 1000));                  // used up
  CHECK(budget.take(0x04, 1000));                   // other groups have their own budget
  CHECK(!budget.take(0x07, 1000 + refill_ms - 1));  // not refilled yet
  CHECK(budget.take(0x07, 1000 + refill_ms));       // one token back after 75 min
  CHECK(!budget.take(0x07, 1000 + refill_ms));
  CHECK(budget.available(0x07, 1000 + 3 * refill_ms) == 2);
  CHECK(budget.available(0x07, 1000 + 1000 * refill_ms) == WriteBudget::DEFAULT_BURST);  // capped
  // a full bucket does not save up: taking one right after it is full leaves 29
  CHECK(budget.take(0x07, 1000 + 1000 * refill_ms + 1));
  CHECK(budget.available(0x07, 1000 + 1000 * refill_ms + 2) == WriteBudget::DEFAULT_BURST - 1);

  // the long-term average stays at one per refill interval: 7000/year
  WriteBudget year_budget;
  uint32_t taken = 0;
  for (uint64_t time_ms = 0; time_ms < 365ull * 24 * 3600 * 1000; time_ms += 60000)  // an automation every minute
    if (year_budget.take(0x09, (uint32_t) time_ms)) taken++;
  CHECK(taken >= 7000 && taken <= 7000 + WriteBudget::DEFAULT_BURST + 1);

  // restore after a restart: saved tokens, refill restarts at the restore time
  WriteBudget restored;
  restored.restore(0x0C, 2, 5000);
  CHECK(restored.available(0x0C, 5000) == 2);
  restored.restore(0x0F, 999, 5000);  // never above the burst
  CHECK(restored.available(0x0F, 5000) == WriteBudget::DEFAULT_BURST);
  CHECK(restored.size() == 2 && restored.dst_at(0) == 0x0C && restored.tokens_at(0) == 2);

  // burst 0 disables the budget
  WriteBudget off;
  off.configure(0, refill_ms);
  for (int i = 0; i < 100; i++) CHECK(off.take(0x07, 0));
}

static void test_queue_uses_budget() {
  WriteQueue queue;
  queue.set_enabled(true);
  queue.budget().configure(2, 1000);
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::BUDGET);
  CHECK(queue.size() == 2);
  // a non-setting frame (not A5 40/41) does not use the budget
  CHECK(queue.enqueue_hex("E93007140140", 0) == WriteResult::OK);
  // a refused frame (iTM source) does not take a token
  queue.budget().configure(1, 1000);
  CHECK(queue.enqueue_hex("A50A0414064116D0000000", 5000) == WriteResult::ITM_SOURCE);
  CHECK(queue.budget().available(0x04, 5000) == 1);
  // retries re-send without taking another token
  std::string out;
  CHECK(queue.next(0, out));
  CHECK(queue.on_refused(10));
  CHECK(queue.budget().available(0x07, 10) == 0);
  CHECK(is_setting_command(hex(POWER_ON).data(), hex(POWER_ON).size()));
  auto fan = hex("A5300714064116D0000000");
  CHECK(is_setting_command(fan.data(), fan.size()));
}

// Airflow direction (a capture of a central-controller test on group 1-01): the
// iTM's commands for group 1-01 and the status the units reported afterwards.
static void test_airflow() {
  uint8_t b1_to_diii[256];
  for (auto &entry : b1_to_diii) entry = 0xFF;
  // F5 23B of 1-01 before (byte 16 = 0x37), after position 0 (0x30) and position 4 (0x34)
  const char *status[] = {"F501FF1C118261010000000700001531370000806D007E",
                          "F501FF1C11826101000000070000154F30000080700064",
                          "F501FF1C11826101000000070000157B34000080720032"};
  const int expect[] = {7, 0, 4};
  for (int i = 0; i < 3; i++) {
    auto frame = hex(status[i]);
    CHECK(frame_valid(frame.data(), frame.size()));
    Capture capture;
    decode_frame(frame.data(), frame.size(), b1_to_diii, capture);
    CHECK(capture.airflow == expect[i]);
    CHECK(capture.fan == 3);  // fan medium all along
  }

  // builder: the same bytes as the iTM sent (with its source 0x0A), plus checksum
  const struct { int fan, airflow; const char *frame; } commands[] = {
      {-1, 0, "A50A0114064116080000007C"},
      {-1, 4, "A50A01140641160C00000078"},
      {-1, 7, "A50A01140641160F00000075"},
  };
  for (auto &command : commands) {
    uint8_t frame[FAN_AIRFLOW_LEN + 1];
    CHECK(build_fan_airflow(frame, 0x0A, 0x01, command.fan, command.airflow));
    frame[FAN_AIRFLOW_LEN] = frame_checksum(frame, FAN_AIRFLOW_LEN);
    auto want = hex(command.frame);
    CHECK(want.size() == FAN_AIRFLOW_LEN + 1 && memcmp(frame, want.data(), want.size()) == 0);
    CHECK(is_setting_command(frame, FAN_AIRFLOW_LEN));
  }
  // fan only keeps the earlier frame (41 16 B0 = medium), both in one frame
  uint8_t frame[FAN_AIRFLOW_LEN];
  CHECK(build_fan_airflow(frame, 0x30, 0x07, 3, -1) && frame[7] == 0xB0);
  CHECK(build_fan_airflow(frame, 0x30, 0x07, 5, 7) && frame[7] == 0xDF);
  // values the units do not know are refused, and so is an empty command
  CHECK(!build_fan_airflow(frame, 0x30, 0x07, -1, 5));
  CHECK(!build_fan_airflow(frame, 0x30, 0x07, -1, 6));
  CHECK(!build_fan_airflow(frame, 0x30, 0x07, 2, -1));
  CHECK(!build_fan_airflow(frame, 0x30, 0x07, -1, -1));
  // the queue still refuses the iTM's own source address for these frames
  WriteQueue queue;
  queue.set_enabled(true);
  CHECK(queue.enqueue_hex("A50A011406411608000000", 0) == WriteResult::ITM_SOURCE);
}

// Outlet pairs: the frames that changed unit 0 of group 1-04 on 2026-11-16
// (captures/exp-raw-1-04-flap-*.tsv), checked against P1P2 packet 0x15. Fixed
// needs b9 low F: FF A0 00 was sent by the first build and ignored by the unit.
static void test_outlet_pair() {
  const struct { OutletPair pair; bool fixed; const char *frame; } cases[] = {
      {OutletPair::OUTLETS_1_3, false, "A530041406411600F4A000"},  // outlets 1+3 -> normal
      {OutletPair::OUTLETS_2_4, false, "A530041406411600F4F000"},  // outlets 2+4 -> normal
      {OutletPair::OUTLETS_1_3, true, "A530041406411600FFAF00"},   // outlets 1+3 -> fixed 0
      {OutletPair::OUTLETS_2_4, true, "A530041406411600FFFF00"},   // outlets 2+4 -> fixed 0
  };
  for (auto &test_case : cases) {
    uint8_t frame[FAN_AIRFLOW_LEN];
    build_outlet_pair(frame, 0x30, 0x04, test_case.pair, test_case.fixed);
    auto want = hex(test_case.frame);
    CHECK(want.size() == FAN_AIRFLOW_LEN && memcmp(frame, want.data(), want.size()) == 0);
    CHECK(is_setting_command(frame, FAN_AIRFLOW_LEN));  // counts against the write budget
  }
  // fan and airflow stay unchanged in these frames (byte 7 = 0)
  uint8_t frame[FAN_AIRFLOW_LEN];
  build_outlet_pair(frame, 0x30, 0x03, OutletPair::OUTLETS_2_4, false);
  CHECK(frame[2] == 0x03 && frame[7] == 0x00);
}

// Operation command 40: the builder makes the iTM's own frames (source 0x0A,
// with checksum) for an indoor unit and for the hydrobox (35 bytes, seen
// 2026-11-16: heat + on at 05:08:28, off at 21:07:02).
static void test_operation() {
  const struct { uint8_t dst; bool hydrobox; uint8_t pp, mm, xx, sp; const char *frame; } cases[] = {
      {0x10, true, 0x00, 0x11, 0x01, 0, "A50A10141D4000110100000000000080000000000000000000000000000000000000E3"},
      {0x10, true, 0x03, 0x00, 0x00, 0, "A50A10141D4003000000000000000080000000000000000000000000000000000000F2"},
      {0x10, true, 0x02, 0x00, 0x00, 0, "A50A10141D4002000000000000000080000000000000000000000000000000000000F3"},
  };
  for (auto &test_case : cases) {
    uint8_t frame[HYDROBOX_OPERATION_LEN + 1];
    size_t length = build_operation(frame, 0x0A, test_case.dst, test_case.hydrobox, test_case.pp, test_case.mm,
                                    test_case.xx, test_case.sp);
    CHECK(length == HYDROBOX_OPERATION_LEN);
    frame[length] = frame_checksum(frame, length);
    auto want = hex(test_case.frame);
    CHECK(want.size() == length + 1 && memcmp(frame, want.data(), want.size()) == 0);
    CHECK(is_setting_command(frame, length));
  }
  // indoor unit: the 26-byte frame the climate entity has always sent
  uint8_t frame[HYDROBOX_OPERATION_LEN];
  size_t length = build_operation(frame, 0x30, 0x07, false, 0x03, 0, 0, 0);
  auto want = hex(POWER_ON);
  CHECK(length == OPERATION_LEN && want.size() == length && memcmp(frame, want.data(), length) == 0);
  length = build_operation(frame, 0x30, 0x07, false, 0x00, 0x00, 0x00, 22);
  CHECK(frame[11] == 22 && frame[12] == 0x80 && frame[15] == 0x80);
  // the queue takes the long frame from our own source and adds the checksum
  WriteQueue queue;
  queue.set_enabled(true);
  length = build_operation(frame, 0x30, 0x10, true, 0x02, 0, 0, 0);
  CHECK(queue.enqueue(frame, length, 0) == WriteResult::OK);
  std::string out;
  CHECK(queue.next(0, out) && out.size() == 2 * (length + 1) && out.compare(0, 14, "A53010141D4002") == 0);
  auto sent = hex(out.c_str());
  CHECK(frame_valid(sent.data(), sent.size()));
}

static void test_pending_setting() {
  const uint32_t pending_ms = PendingSetting::PENDING_MS;
  PendingSetting setpoint;
  CHECK(setpoint.changes(22, 0, true, 0));     // no bus status yet: send
  CHECK(!setpoint.changes(22, 22, false, 0));  // same as the bus: skip
  CHECK(setpoint.changes(23, 22, false, 0));   // 22 -> 23: send
  setpoint.commanded(23, 1000);
  // quick undo while the bus still says 22: 23 is current, so 22 is sent
  CHECK(setpoint.changes(22, 22, false, 2000));
  CHECK(!setpoint.changes(23, 22, false, 2000));  // repeating 23 is skipped
  setpoint.commanded(22, 2000);
  CHECK(!setpoint.changes(22, 23, false, 3000));  // bus lags at 23, 22 already sent
  // after the pending window the bus value counts again
  CHECK(!setpoint.changes(22, 22, false, 2000 + pending_ms));
  CHECK(setpoint.changes(22, 21, false, 2000 + pending_ms));  // changed elsewhere (iTM): send
  CHECK(setpoint.current(21, 2000 + pending_ms) == 21);
  CHECK(setpoint.current(21, 2500) == 22);

  // confirmed by the bus: the bus counts again, so an iTM change is not masked
  PendingSetting confirmed;
  confirmed.commanded(23, 0);
  confirmed.observe(22);                          // stale status: still pending
  CHECK(!confirmed.changes(23, 22, false, 1000));
  confirmed.observe(23);                          // bus confirms 23
  CHECK(confirmed.changes(23, 22, false, 2000));  // iTM set 22 meanwhile: 23 is sent again
  CHECK(!confirmed.changes(22, 22, false, 2000));
}

static void test_atmega_commands() {
  CHECK(atmega_command_allowed("V"));
  CHECK(atmega_command_allowed("T50"));
  CHECK(atmega_command_allowed("o1F"));
  CHECK(!atmega_command_allowed(""));
  CHECK(!atmega_command_allowed("W0A0714144003"));  // raw frame write: must use WriteQueue
  CHECK(!atmega_command_allowed("w300714"));
  CHECK(!atmega_command_allowed("L1"));  // controller mode
  CHECK(!atmega_command_allowed("V\nW0A0714"));  // chained command
  CHECK(!atmega_command_allowed("T5\r"));
  CHECK(!atmega_command_allowed("T 50"));
  CHECK(!atmega_command_allowed("T12345678901234567"));  // too long
}

// A frame counts as written only once its echo is read back from the bus
// (2026-11-17: refused, dropped and damaged hydrobox frames were reported as
// sent, and the hydrobox stayed on).
static void test_echo() {
  const char *HB_OFF = "A53010141D4002000000000000000080000000000000000000000000000000000000";  // 34 B, no checksum
  WriteQueue queue;
  queue.set_enabled(true);
  std::string out, frame;
  uint8_t retry_no = 0;
  CHECK(queue.enqueue_hex(HB_OFF, 0) == WriteResult::OK);
  CHECK(queue.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  uint32_t now = 1000;
  CHECK(queue.next(now, out));
  const std::string hydrobox_frame = out;
  auto full = hex(hydrobox_frame.c_str());
  CHECK(full.size() == 35 && frame_valid(full.data(), full.size()));

  // the next frame waits for the echo, also past the spacing
  CHECK(!queue.next(now + WriteQueue::SPACING_MS, out));
  CHECK(queue.check_echo(now + WriteQueue::ECHO_TIMEOUT_MS - 1, frame) == EchoResult::NONE);

  // other frames on the bus do not count: another unit's status, the iTM's
  // command for the same group, and our frame cut off after 30 bytes (as seen
  // at 18:46:41)
  auto status = hex("F510FF1C10820101000000031000192C000000805F0A");
  CHECK(!queue.on_bus_frame(status.data(), status.size()));
  auto itm = full;
  itm[1] = ITM_ADDRESS;
  CHECK(!queue.on_bus_frame(itm.data(), itm.size()));
  CHECK(!queue.on_bus_frame(full.data(), 30));
  CHECK(queue.in_flight());

  // no intact echo in time: the same frame again, ahead of the queue
  CHECK(queue.check_echo(now + WriteQueue::ECHO_TIMEOUT_MS, frame, &retry_no) == EchoResult::RETRY && retry_no == 1 &&
        frame == hydrobox_frame);
  CHECK(queue.check_echo(now + WriteQueue::ECHO_TIMEOUT_MS + 1, frame) == EchoResult::NONE);  // reported once
  now += WriteQueue::ECHO_TIMEOUT_MS;
  CHECK(queue.next(now, out) && out == hydrobox_frame);

  // its echo arrives: done, and the next frame goes out
  CHECK(queue.on_bus_frame(full.data(), full.size()));
  CHECK(!queue.in_flight());
  CHECK(!queue.on_bus_frame(full.data(), full.size()));  // a second copy is just bus traffic
  CHECK(queue.check_echo(now + 10 * WriteQueue::ECHO_TIMEOUT_MS, frame) == EchoResult::NONE);
  now += WriteQueue::SPACING_MS;
  CHECK(queue.next(now, out) && out.rfind(POWER_ON, 0) == 0);

  // never an echo: MAX_RETRIES more sends, then lost, and the queue moves on
  CHECK(queue.enqueue_hex(HB_OFF, 0) == WriteResult::OK);
  for (int i = 1; i <= WriteQueue::MAX_RETRIES; i++) {
    now += WriteQueue::ECHO_TIMEOUT_MS;
    CHECK(queue.check_echo(now, frame, &retry_no) == EchoResult::RETRY && retry_no == i);
    CHECK(queue.next(now, out) && out == frame);
  }
  now += WriteQueue::ECHO_TIMEOUT_MS;
  CHECK(queue.check_echo(now, frame) == EchoResult::LOST && frame == out);
  CHECK(queue.lost() == 1);
  CHECK(queue.next(now, out) && out == hydrobox_frame);  // the hydrobox frame queued behind it
  CHECK(queue.size() == 0);

  // switching writes off forgets the frame in flight
  queue.set_enabled(false);
  CHECK(!queue.in_flight());
  CHECK(queue.check_echo(now + 10 * WriteQueue::ECHO_TIMEOUT_MS, frame) == EchoResult::NONE);
}

static void test_group_action() {
  const float unknown = NAN;
  CHECK(group_action(-1, 0x61, 1, false, 20, 21) == Action::UNKNOWN);
  CHECK(group_action(0, 0x61, 1, true, 20, 21) == Action::OFF);
  CHECK(group_action(1, 0x61, 1, true, 20, 21) == Action::HEATING);
  CHECK(group_action(1, 0x61, 0, true, 20, 21) == Action::IDLE);
  CHECK(group_action(1, 0x61, -1, true, 20, 21) == Action::UNKNOWN);
  CHECK(group_action(1, 0x62, 1, true, 25, 24) == Action::COOLING);
  CHECK(group_action(1, 0x60, -1, true, unknown, unknown) == Action::FAN);
  CHECK(group_action(1, 0x60, 0, false, unknown, unknown) == Action::IDLE);
  CHECK(group_action(1, 0x67, 1, true, unknown, unknown) == Action::DRYING);
  CHECK(group_action(1, 0x63, 1, true, 20, 21) == Action::HEATING);
  CHECK(group_action(1, 0x63, 1, true, 23, 21) == Action::COOLING);
  CHECK(group_action(1, 0x63, 1, true, unknown, 21) == Action::UNKNOWN);
  CHECK(group_action(1, 0x00, 1, true, 20, 21) == Action::UNKNOWN);
}

static void test_known_frames() {
  // real frames from the test system: F5 23 B status and E9 page 65 are known
  auto status = hex("F500FF1C1182610100000000000013" "1A370000800200" "0A");
  CHECK(is_known_frame(status.data(), status.size()));
  auto page65 = hex("E900FF1028656D67C0180100001000094F095D00000D00000000000816080014000000000226000000001000006A");
  CHECK(page65.size() == 46);
  CHECK(is_known_frame(page65.data(), page65.size()));
  // an F5 frame of a length never seen, and an E9 page that isn't in the catalogue
  std::vector<uint8_t> odd(24, 0x00);
  odd[0] = 0xF5;
  CHECK(!is_known_frame(odd.data(), odd.size()));
  auto page99 = page65;
  page99[5] = 0x99;
  CHECK(!is_known_frame(page99.data(), page99.size()));
  // page E0 41 B is known per sub-page: 12 yes, 22 no
  std::vector<uint8_t> e0(41, 0x00);
  e0[0] = 0xE9;
  e0[3] = 0x1C;
  e0[5] = 0xE0;
  e0[11] = 0x12;
  CHECK(is_known_frame(e0.data(), e0.size()));
  e0[11] = 0x22;
  CHECK(!is_known_frame(e0.data(), e0.size()));
  // the signature separates frame types and ignores the content
  auto other_status = status;
  other_status[14] = 0x15;
  CHECK(frame_signature(status.data(), status.size()) == frame_signature(other_status.data(), other_status.size()));
  CHECK(frame_signature(page65.data(), page65.size()) != frame_signature(page99.data(), page99.size()));
  CHECK(frame_signature(odd.data(), odd.size()) != frame_signature(status.data(), status.size()));
}

int main() {
  test_guards();
  test_checksum_and_spacing();
  test_retry();
  test_echo();
  test_atmega_commands();
  test_budget();
  test_queue_uses_budget();
  test_pending_setting();
  test_decoder_known_values();
  test_airflow();
  test_outlet_pair();
  test_operation();
  test_group_action();
  test_known_frames();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all unit tests passed\n");
  return 0;
}
