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

static std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> v;
  for (; s[0] && s[1]; s += 2) v.push_back((uint8_t) (hexval(s[0]) << 4 | hexval(s[1])));
  return v;
}

// iTM-style power-on command for group 0x07 from source 0x30, without checksum.
static const char *POWER_ON = "A5300714144003000000000000000080000000000000000000";

static void test_guards() {
  WriteQueue q;
  auto f = hex(POWER_ON);
  CHECK(q.enqueue(f.data(), f.size(), 0) == WriteResult::DISABLED);  // default off in the queue itself
  q.set_enabled(true);
  CHECK(q.enqueue(f.data(), f.size(), 0) == WriteResult::OK);
  CHECK(q.size() == 1);

  auto itm = f;
  itm[1] = ITM_ADDRESS;
  CHECK(q.enqueue(itm.data(), itm.size(), 0) == WriteResult::ITM_SOURCE);

  auto shortf = f;
  shortf.pop_back();  // length byte says 0x14 payload bytes, one missing
  CHECK(q.enqueue(shortf.data(), shortf.size(), 0) == WriteResult::BAD_LENGTH);
  CHECK(q.enqueue(f.data(), 3, 0) == WriteResult::BAD_LENGTH);

  CHECK(q.enqueue_hex("A530ZZ", 0) == WriteResult::BAD_HEX);
  CHECK(q.enqueue_hex("A530071", 0) == WriteResult::BAD_HEX);  // odd length
  CHECK(q.enqueue_hex(std::string(400, 'A'), 0) == WriteResult::BAD_LENGTH);

  while (q.size() < WriteQueue::MAX_QUEUE) CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::QUEUE_FULL);

  q.set_enabled(false);  // switching writes off drops everything queued
  CHECK(q.size() == 0);
  std::string out;
  CHECK(!q.next(10000, out));
}

static void test_checksum_and_spacing() {
  WriteQueue q;
  q.set_enabled(true);
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  std::string out;
  CHECK(q.next(1000, out));
  // the queued frame carries a checksum that makes it a valid frame
  CHECK(out.size() == 2 * 26);
  auto b = hex(out.c_str());
  CHECK(frame_valid(b.data(), b.size()));
  CHECK(out.rfind(POWER_ON, 0) == 0);
  CHECK(!q.next(1000 + WriteQueue::SPACING_MS, out));  // no echo yet
  CHECK(q.on_bus_frame(b.data(), b.size()));
  CHECK(!q.next(1000 + WriteQueue::SPACING_MS - 1, out));  // echo seen, but too soon
  CHECK(q.next(1000 + WriteQueue::SPACING_MS, out));
  CHECK(q.size() == 0);
}

static void test_retry() {
  WriteQueue q;
  q.set_enabled(true);
  std::string out;
  uint8_t n = 0;

  // collision right after sending -> same frame again, then success
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  uint32_t t = 1000;
  CHECK(q.next(t, out));
  std::string first = out;
  CHECK(q.on_refused(t + 50, &n) && n == 1);
  CHECK(!q.on_refused(t + 60));  // a second line for the same send does not queue it twice
  CHECK(q.size() == 1);
  t += WriteQueue::SPACING_MS;
  CHECK(q.next(t, out) && out == first);
  CHECK(q.size() == 0);

  // retries run out after MAX_RETRIES
  for (int i = 2; i <= WriteQueue::MAX_RETRIES; i++) {
    CHECK(q.on_refused(t + 10, &n) && n == i);
    t += WriteQueue::SPACING_MS;
    CHECK(q.next(t, out) && out == first);
  }
  CHECK(q.lost() == 0);
  CHECK(!q.on_refused(t + 10));
  CHECK(q.lost() == 1);  // refused on the last retry: given up
  CHECK(q.size() == 0);

  // a refusal long after the send must not replay the old frame
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  t += 10 * WriteQueue::SPACING_MS;
  CHECK(q.next(t, out));
  CHECK(!q.on_refused(t + WriteQueue::RETRY_WINDOW_MS));
  CHECK(!q.on_refused(t + 3600000));
  CHECK(q.size() == 0);

  // the same command sent again later gets its full retry budget
  auto echo = hex(out.c_str());
  CHECK(q.on_bus_frame(echo.data(), echo.size()));  // the previous one made it
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  t += WriteQueue::SPACING_MS;
  CHECK(q.next(t, out));
  CHECK(q.on_refused(t + 10, &n) && n == 1);

  // nothing sent yet: a refusal line is ignored
  WriteQueue fresh;
  fresh.set_enabled(true);
  CHECK(!fresh.on_refused(5));
}

struct Capture : DecoderSink {
  int calls = 0, mode = -1, fan = -1, airflow = -1, status = -1, power = -1, master = -1, defrost = -1;
  float room = NAN, step = NAN;
  void on_unit_value(uint8_t a, UnitField f, float v) override {
    calls++;
    if (a == 0x08 && f == UnitField::ROOM) room = v;
    if (f == UnitField::DEMAND_STEP) step = v;
  }
  void on_unit_role(uint8_t, bool m) override { calls++, master = m; }
  void on_defrost(bool d) override { calls++, defrost = d; }
  void on_unit_power(uint8_t, bool on) override { calls++, power = on; }
  void on_unit_mode(uint8_t, uint8_t m) override { calls++, mode = m; }
  void on_unit_fan(uint8_t, uint8_t s) override { calls++, fan = s; }
  void on_unit_airflow(uint8_t, uint8_t d) override { calls++, airflow = d; }
  void on_unit_status(uint8_t, uint8_t s) override { calls++, status = s; }
  void on_system_value(SystemField, float) override { calls++; }
  void on_compressor(uint8_t) override { calls++; }
  void on_clock(uint8_t, uint8_t) override { calls++; }
};

static void test_decoder_known_values() {
  uint8_t map[256];
  for (auto &m : map) m = 0xFF;
  // F5 status of unit 1-08 (captures/after-exp3.tsv), checked by hand:
  // mode 0x61 heat, status 0x02, room 0x1227/256 = 18.15 C, fan nibble 3 = medium
  auto f = hex("F508FF1C1182610100000002000012273700008068008E");
  CHECK(frame_valid(f.data(), f.size()));
  Capture c;
  decode_frame(f.data(), f.size(), map, c);
  CHECK(c.mode == 0x61);
  CHECK(c.status == 0x02);
  CHECK(c.fan == 3);
  CHECK(c.airflow == 7);  // byte 16 = 0x37: medium, swing
  CHECK(std::fabs(c.room - 18.15234375f) < 1e-6);

  // bad checksum: ignored
  auto bad = f;
  bad.back() ^= 0x01;
  CHECK(!frame_valid(bad.data(), bad.size()));
  Capture cb;
  decode_frame(bad.data(), bad.size(), map, cb);
  CHECK(cb.calls == 0);

  // truncated frame: ignored
  Capture ct;
  decode_frame(f.data(), f.size() - 1, map, ct);
  CHECK(ct.calls == 0);

  // Real frames from a bus log (docs/protocol.md, "Overnight log check"):
  // E9 page 6A with modules 1+2 in sub-state 07 (06:55:20) and back to normal
  Capture d1, d0;
  auto df = hex("E900FF10286A6D67C0180101070107020020202007D014C80000000000000000000000000000000000000000008D");
  decode_frame(df.data(), df.size(), map, d1);
  CHECK(d1.defrost == 1);
  auto dn = hex("E900FF10286A6D67C0180101000100010020202007D014C80000000000000000000000000000000000000000009C");
  decode_frame(dn.data(), dn.size(), map, d0);
  CHECK(d0.defrost == 0);
  // hydrobox F5 22B with heat demand: status 0x23 (on, outdoor running, demand), step 0x0F
  Capture h;
  auto hb = hex("F510FF1C10820101000F0023100017BE000000807634");
  decode_frame(hb.data(), hb.size(), map, h);
  CHECK(h.power == 1);
  CHECK(h.status == 0x23);
  CHECK(h.step == 15.0f);
  // F5 31B byte 18: group master 1-07 (0x12) and member 1-05 (0x10)
  Capture m, s5;
  auto fm = hex("F507FF1C198000610100000000160000000012000004020000800000000035");
  decode_frame(fm.data(), fm.size(), map, m);
  CHECK(m.master == 1);
  auto fs = hex("F505FF1C198000610100000000160000000010000004120000800001000028");
  decode_frame(fs.data(), fs.size(), map, s5);
  CHECK(s5.master == 0);

  // P1P2Monitor line parsing
  uint8_t buf[MAX_FRAME];
  float gap = 0;
  CHECK(parse_raw_line("R T  3.745: FD0AFF080550B0000100E9", buf, sizeof(buf), &gap) == 11);
  CHECK(std::fabs(gap - 3.745f) < 1e-4);
  CHECK(buf[0] == 0xFD && buf[10] == 0xE9);
  CHECK(parse_raw_line("* no frame here", buf, sizeof(buf), nullptr) == 0);

  // signed temperatures
  uint8_t neg[2] = {0x80 | 0x02, 0x80};  // sign-magnitude -2.5
  CHECK(std::fabs(sm88(neg, 0) + 2.5f) < 1e-6);
  uint8_t twos[2] = {0xFD, 0x80};  // two's complement -2.5
  CHECK(std::fabs(t88s(twos, 0) + 2.5f) < 1e-6);
}

static void test_budget() {
  const uint32_t R = WriteBudget::DEFAULT_REFILL_MS;
  WriteBudget b;  // defaults: 30 at once, one back every 75 min (7000/year)
  CHECK(b.available(0x07, 0) == WriteBudget::DEFAULT_BURST);
  for (int i = 0; i < WriteBudget::DEFAULT_BURST; i++) CHECK(b.take(0x07, 1000));
  CHECK(!b.take(0x07, 1000));          // used up
  CHECK(b.take(0x04, 1000));           // other groups have their own budget
  CHECK(!b.take(0x07, 1000 + R - 1));  // not refilled yet
  CHECK(b.take(0x07, 1000 + R));       // one token back after 75 min
  CHECK(!b.take(0x07, 1000 + R));
  CHECK(b.available(0x07, 1000 + 3 * R) == 2);
  CHECK(b.available(0x07, 1000 + 1000 * R) == WriteBudget::DEFAULT_BURST);  // capped
  // a full bucket does not save up: taking one right after it is full leaves 29
  CHECK(b.take(0x07, 1000 + 1000 * R + 1));
  CHECK(b.available(0x07, 1000 + 1000 * R + 2) == WriteBudget::DEFAULT_BURST - 1);

  // the long-term average stays at one per refill interval: 7000/year
  WriteBudget y;
  uint32_t taken = 0;
  for (uint64_t t = 0; t < 365ull * 24 * 3600 * 1000; t += 60000)  // an automation every minute
    if (y.take(0x09, (uint32_t) t)) taken++;
  CHECK(taken >= 7000 && taken <= 7000 + WriteBudget::DEFAULT_BURST + 1);

  // restore after a restart: saved tokens, refill restarts at the restore time
  WriteBudget r;
  r.restore(0x0C, 2, 5000);
  CHECK(r.available(0x0C, 5000) == 2);
  r.restore(0x0F, 999, 5000);  // never above the burst
  CHECK(r.available(0x0F, 5000) == WriteBudget::DEFAULT_BURST);
  CHECK(r.size() == 2 && r.dst_at(0) == 0x0C && r.tokens_at(0) == 2);

  // burst 0 disables the budget
  WriteBudget off;
  off.configure(0, R);
  for (int i = 0; i < 100; i++) CHECK(off.take(0x07, 0));
}

static void test_queue_uses_budget() {
  WriteQueue q;
  q.set_enabled(true);
  q.budget().configure(2, 1000);
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::BUDGET);
  CHECK(q.size() == 2);
  // a non-setting frame (not A5 40/41) does not use the budget
  CHECK(q.enqueue_hex("E93007140140", 0) == WriteResult::OK);
  // a refused frame (iTM source) does not take a token
  q.budget().configure(1, 1000);
  CHECK(q.enqueue_hex("A50A0414064116D0000000", 5000) == WriteResult::ITM_SOURCE);
  CHECK(q.budget().available(0x04, 5000) == 1);
  // retries re-send without taking another token
  std::string out;
  CHECK(q.next(0, out));
  CHECK(q.on_refused(10));
  CHECK(q.budget().available(0x07, 10) == 0);
  CHECK(is_setting_command(hex(POWER_ON).data(), hex(POWER_ON).size()));
  auto fan = hex("A5300714064116D0000000");
  CHECK(is_setting_command(fan.data(), fan.size()));
}

// Airflow direction (a capture of a central-controller test on group 1-01): the
// iTM's commands for group 1-01 and the status the units reported afterwards.
static void test_airflow() {
  uint8_t map[256];
  for (auto &m : map) m = 0xFF;
  // F5 23B of 1-01 before (byte 16 = 0x37), after position 0 (0x30) and position 4 (0x34)
  const char *status[] = {"F501FF1C118261010000000700001531370000806D007E",
                          "F501FF1C11826101000000070000154F30000080700064",
                          "F501FF1C11826101000000070000157B34000080720032"};
  const int expect[] = {7, 0, 4};
  for (int i = 0; i < 3; i++) {
    auto f = hex(status[i]);
    CHECK(frame_valid(f.data(), f.size()));
    Capture c;
    decode_frame(f.data(), f.size(), map, c);
    CHECK(c.airflow == expect[i]);
    CHECK(c.fan == 3);  // fan medium all along
  }

  // builder: the same bytes as the iTM sent (with its source 0x0A), plus checksum
  const struct { int fan, airflow; const char *frame; } cmds[] = {
      {-1, 0, "A50A0114064116080000007C"},
      {-1, 4, "A50A01140641160C00000078"},
      {-1, 7, "A50A01140641160F00000075"},
  };
  for (auto &c : cmds) {
    uint8_t f[FAN_AIRFLOW_LEN + 1];
    CHECK(build_fan_airflow(f, 0x0A, 0x01, c.fan, c.airflow));
    f[FAN_AIRFLOW_LEN] = frame_checksum(f, FAN_AIRFLOW_LEN);
    auto want = hex(c.frame);
    CHECK(want.size() == FAN_AIRFLOW_LEN + 1 && memcmp(f, want.data(), want.size()) == 0);
    CHECK(is_setting_command(f, FAN_AIRFLOW_LEN));
  }
  // fan only keeps the earlier frame (41 16 B0 = medium), both in one frame
  uint8_t f[FAN_AIRFLOW_LEN];
  CHECK(build_fan_airflow(f, 0x30, 0x07, 3, -1) && f[7] == 0xB0);
  CHECK(build_fan_airflow(f, 0x30, 0x07, 5, 7) && f[7] == 0xDF);
  // values the units do not know are refused, and so is an empty command
  CHECK(!build_fan_airflow(f, 0x30, 0x07, -1, 5));
  CHECK(!build_fan_airflow(f, 0x30, 0x07, -1, 6));
  CHECK(!build_fan_airflow(f, 0x30, 0x07, 2, -1));
  CHECK(!build_fan_airflow(f, 0x30, 0x07, -1, -1));
  // the queue still refuses the iTM's own source address for these frames
  WriteQueue q;
  q.set_enabled(true);
  CHECK(q.enqueue_hex("A50A011406411608000000", 0) == WriteResult::ITM_SOURCE);
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
  for (auto &c : cases) {
    uint8_t f[FAN_AIRFLOW_LEN];
    build_outlet_pair(f, 0x30, 0x04, c.pair, c.fixed);
    auto want = hex(c.frame);
    CHECK(want.size() == FAN_AIRFLOW_LEN && memcmp(f, want.data(), want.size()) == 0);
    CHECK(is_setting_command(f, FAN_AIRFLOW_LEN));  // counts against the write budget
  }
  // fan and airflow stay unchanged in these frames (byte 7 = 0)
  uint8_t f[FAN_AIRFLOW_LEN];
  build_outlet_pair(f, 0x30, 0x03, OutletPair::OUTLETS_2_4, false);
  CHECK(f[2] == 0x03 && f[7] == 0x00);
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
  for (auto &c : cases) {
    uint8_t f[HYDROBOX_OPERATION_LEN + 1];
    size_t n = build_operation(f, 0x0A, c.dst, c.hydrobox, c.pp, c.mm, c.xx, c.sp);
    CHECK(n == HYDROBOX_OPERATION_LEN);
    f[n] = frame_checksum(f, n);
    auto want = hex(c.frame);
    CHECK(want.size() == n + 1 && memcmp(f, want.data(), want.size()) == 0);
    CHECK(is_setting_command(f, n));
  }
  // indoor unit: the 26-byte frame the climate entity has always sent
  uint8_t f[HYDROBOX_OPERATION_LEN];
  size_t n = build_operation(f, 0x30, 0x07, false, 0x03, 0, 0, 0);
  auto want = hex(POWER_ON);
  CHECK(n == OPERATION_LEN && want.size() == n && memcmp(f, want.data(), n) == 0);
  n = build_operation(f, 0x30, 0x07, false, 0x00, 0x00, 0x00, 22);
  CHECK(f[11] == 22 && f[12] == 0x80 && f[15] == 0x80);
  // the queue takes the long frame from our own source and adds the checksum
  WriteQueue q;
  q.set_enabled(true);
  n = build_operation(f, 0x30, 0x10, true, 0x02, 0, 0, 0);
  CHECK(q.enqueue(f, n, 0) == WriteResult::OK);
  std::string out;
  CHECK(q.next(0, out) && out.size() == 2 * (n + 1) && out.compare(0, 14, "A53010141D4002") == 0);
  auto sent = hex(out.c_str());
  CHECK(frame_valid(sent.data(), sent.size()));
}

static void test_pending_setting() {
  const uint32_t P = PendingSetting::PENDING_MS;
  PendingSetting sp;
  CHECK(sp.changes(22, 0, true, 0));      // no bus status yet: send
  CHECK(!sp.changes(22, 22, false, 0));   // same as the bus: skip
  CHECK(sp.changes(23, 22, false, 0));    // 22 -> 23: send
  sp.commanded(23, 1000);
  // quick undo while the bus still says 22: 23 is current, so 22 is sent
  CHECK(sp.changes(22, 22, false, 2000));
  CHECK(!sp.changes(23, 22, false, 2000));  // repeating 23 is skipped
  sp.commanded(22, 2000);
  CHECK(!sp.changes(22, 23, false, 3000));  // bus lags at 23, 22 already sent
  // after the pending window the bus value counts again
  CHECK(!sp.changes(22, 22, false, 2000 + P));
  CHECK(sp.changes(22, 21, false, 2000 + P));  // changed elsewhere (iTM): send
  CHECK(sp.current(21, 2000 + P) == 21);
  CHECK(sp.current(21, 2500) == 22);

  // confirmed by the bus: the bus counts again, so an iTM change is not masked
  PendingSetting c;
  c.commanded(23, 0);
  c.observe(22);                          // stale status: still pending
  CHECK(!c.changes(23, 22, false, 1000));
  c.observe(23);                          // bus confirms 23
  CHECK(c.changes(23, 22, false, 2000));  // iTM set 22 meanwhile: 23 is sent again
  CHECK(!c.changes(22, 22, false, 2000));
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
  WriteQueue q;
  q.set_enabled(true);
  std::string out, f;
  uint8_t n = 0;
  CHECK(q.enqueue_hex(HB_OFF, 0) == WriteResult::OK);
  CHECK(q.enqueue_hex(POWER_ON, 0) == WriteResult::OK);
  uint32_t t = 1000;
  CHECK(q.next(t, out));
  const std::string hb = out;
  auto full = hex(hb.c_str());
  CHECK(full.size() == 35 && frame_valid(full.data(), full.size()));

  // the next frame waits for the echo, also past the spacing
  CHECK(!q.next(t + WriteQueue::SPACING_MS, out));
  CHECK(q.check_echo(t + WriteQueue::ECHO_TIMEOUT_MS - 1, f) == EchoResult::NONE);

  // other frames on the bus do not count: another unit's status, the iTM's
  // command for the same group, and our frame cut off after 30 bytes (as seen
  // at 18:46:41)
  auto status = hex("F510FF1C10820101000000031000192C000000805F0A");
  CHECK(!q.on_bus_frame(status.data(), status.size()));
  auto itm = full;
  itm[1] = ITM_ADDRESS;
  CHECK(!q.on_bus_frame(itm.data(), itm.size()));
  CHECK(!q.on_bus_frame(full.data(), 30));
  CHECK(q.in_flight());

  // no intact echo in time: the same frame again, ahead of the queue
  CHECK(q.check_echo(t + WriteQueue::ECHO_TIMEOUT_MS, f, &n) == EchoResult::RETRY && n == 1 && f == hb);
  CHECK(q.check_echo(t + WriteQueue::ECHO_TIMEOUT_MS + 1, f) == EchoResult::NONE);  // reported once
  t += WriteQueue::ECHO_TIMEOUT_MS;
  CHECK(q.next(t, out) && out == hb);

  // its echo arrives: done, and the next frame goes out
  CHECK(q.on_bus_frame(full.data(), full.size()));
  CHECK(!q.in_flight());
  CHECK(!q.on_bus_frame(full.data(), full.size()));  // a second copy is just bus traffic
  CHECK(q.check_echo(t + 10 * WriteQueue::ECHO_TIMEOUT_MS, f) == EchoResult::NONE);
  t += WriteQueue::SPACING_MS;
  CHECK(q.next(t, out) && out.rfind(POWER_ON, 0) == 0);

  // never an echo: MAX_RETRIES more sends, then lost, and the queue moves on
  CHECK(q.enqueue_hex(HB_OFF, 0) == WriteResult::OK);
  for (int i = 1; i <= WriteQueue::MAX_RETRIES; i++) {
    t += WriteQueue::ECHO_TIMEOUT_MS;
    CHECK(q.check_echo(t, f, &n) == EchoResult::RETRY && n == i);
    CHECK(q.next(t, out) && out == f);
  }
  t += WriteQueue::ECHO_TIMEOUT_MS;
  CHECK(q.check_echo(t, f) == EchoResult::LOST && f == out);
  CHECK(q.lost() == 1);
  CHECK(q.next(t, out) && out == hb);  // the hydrobox frame queued behind it
  CHECK(q.size() == 0);

  // switching writes off forgets the frame in flight
  q.set_enabled(false);
  CHECK(!q.in_flight());
  CHECK(q.check_echo(t + 10 * WriteQueue::ECHO_TIMEOUT_MS, f) == EchoResult::NONE);
}

static void test_group_action() {
  const float n = NAN;
  CHECK(group_action(-1, 0x61, 1, false, 20, 21) == Action::UNKNOWN);
  CHECK(group_action(0, 0x61, 1, true, 20, 21) == Action::OFF);
  CHECK(group_action(1, 0x61, 1, true, 20, 21) == Action::HEATING);
  CHECK(group_action(1, 0x61, 0, true, 20, 21) == Action::IDLE);
  CHECK(group_action(1, 0x61, -1, true, 20, 21) == Action::UNKNOWN);
  CHECK(group_action(1, 0x62, 1, true, 25, 24) == Action::COOLING);
  CHECK(group_action(1, 0x60, -1, true, n, n) == Action::FAN);
  CHECK(group_action(1, 0x60, 0, false, n, n) == Action::IDLE);
  CHECK(group_action(1, 0x67, 1, true, n, n) == Action::DRYING);
  CHECK(group_action(1, 0x63, 1, true, 20, 21) == Action::HEATING);
  CHECK(group_action(1, 0x63, 1, true, 23, 21) == Action::COOLING);
  CHECK(group_action(1, 0x63, 1, true, n, 21) == Action::UNKNOWN);
  CHECK(group_action(1, 0x00, 1, true, 20, 21) == Action::UNKNOWN);
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
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all unit tests passed\n");
  return 0;
}
