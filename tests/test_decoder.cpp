// Host test: feed capture TSV files through the ESPHome decoder core and print
// the final state in a canonical format (compare with tests/reference.py).
//   c++ -std=c++17 -I../components/daikin_diii test_decoder.cpp -o test_decoder
//   ./test_decoder ../captures/*.tsv
#include <cstdio>
#include <map>
#include <string>

#include "diii_decoder.h"

using namespace diii;

static const uint8_t B1_MAP[][2] = {{0x01, 0x0C}, {0x02, 0x08}, {0x03, 0x0B}, {0x04, 0x09}, {0x05, 0x00},
                                    {0x06, 0x02}, {0x07, 0x04}, {0x08, 0x01}, {0x09, 0x03}, {0x0A, 0x07},
                                    {0x0B, 0x06}, {0x0C, 0x05}, {0x0D, 0x0E}, {0x0E, 0x11}, {0x0F, 0x0D},
                                    {0x10, 0x0F}};

struct Unit {
  float v[(int) UnitField::COUNT];
  bool has[(int) UnitField::COUNT] = {};
  int power = -1, mode = -1, fan = -1, airflow = -1, status = -1, master = -1;
};

struct Sink : DecoderSink {
  std::map<int, Unit> units;
  float sys[(int) SystemField::COUNT];
  bool sys_has[(int) SystemField::COUNT] = {};
  int compressor = -1, hh = -1, mm = -1;
  void on_unit_value(uint8_t a, UnitField f, float v) override {
    units[a].v[(int) f] = v;
    units[a].has[(int) f] = true;
  }
  void on_unit_power(uint8_t a, bool on) override { units[a].power = on; }
  void on_unit_mode(uint8_t a, uint8_t m) override { units[a].mode = m; }
  void on_unit_fan(uint8_t a, uint8_t s) override { units[a].fan = s; }
  void on_unit_airflow(uint8_t a, uint8_t d) override { units[a].airflow = d; }
  void on_unit_status(uint8_t a, uint8_t s) override { units[a].status = s; }
  void on_system_value(SystemField f, float v) override {
    sys[(int) f] = v;
    sys_has[(int) f] = true;
  }
  void on_compressor(uint8_t s) override { compressor = s; }
  void on_clock(uint8_t h, uint8_t m) override { hh = h, mm = m; }
  void on_unit_role(uint8_t a, bool master) override { units[a].master = master; }
  void on_defrost(bool active) override { defrost = active; }
  int defrost = -1;
};

int main(int argc, char **argv) {
  uint8_t b1map[256];
  memset(b1map, 0xFF, sizeof(b1map));
  for (auto &p : B1_MAP) b1map[p[0]] = p[1];
  Sink sink;
  long frames = 0, valid = 0;
  for (int i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "r");
    if (!f) continue;
    char line[512];
    while (fgets(line, sizeof line, f)) {
      // TSV: time \t gap \t hex [\t # note]  -> rebuild "R T gap: hex"
      char *t1 = strchr(line, '\t');
      if (!t1) continue;
      char *t2 = strchr(t1 + 1, '\t');
      if (!t2) continue;
      char *hex = t2 + 1;
      if (*hex == '#' || *hex == '\t' || *hex == '\n') continue;
      char *end = hex;
      while (hexval(*end) >= 0) end++;
      *end = 0;
      *t2 = 0;
      char rl[400];
      snprintf(rl, sizeof rl, "R T %s: %s", t1 + 1, hex);
      uint8_t buf[MAX_FRAME];
      size_t n = parse_raw_line(rl, buf, sizeof buf, nullptr);
      if (n < 2) continue;
      frames++;
      if (frame_valid(buf, n)) valid++;
      decode_frame(buf, n, b1map, sink);
    }
    fclose(f);
  }
  const char *names[] = {"room", "intake", "remote", "pipe_a", "pipe_b", "setpoint", "demand_step"};
  for (auto &kv : sink.units) {
    printf("unit %02X", kv.first);
    for (int f = 0; f < (int) UnitField::COUNT; f++)
      if (kv.second.has[f]) printf(" %s=%.2f", names[f], kv.second.v[f]);
    if (kv.second.power >= 0) printf(" power=%d", kv.second.power);
    if (kv.second.mode >= 0) printf(" mode=%02X", kv.second.mode);
    if (kv.second.fan >= 0) printf(" fan=%d", kv.second.fan);
    if (kv.second.airflow >= 0) printf(" airflow=%d", kv.second.airflow);
    if (kv.second.status >= 0) printf(" status=%02X", kv.second.status);
    if (kv.second.master >= 0) printf(" master=%d", kv.second.master);
    printf("\n");
  }
  const char *sn[] = {"outdoor", "t_evap", "t_cond", "discharge_1", "discharge_2", "capacity", "comp_step_1",
                      "comp_step_2", "fan_step_1", "fan_step_2", "comp_freq_1", "comp_freq_2"};
  printf("system");
  for (int f = 0; f < (int) SystemField::COUNT; f++)
    if (sink.sys_has[f]) printf(" %s=%.2f", sn[f], sink.sys[f]);
  printf(" compressor=%d clock=%02d:%02d", sink.compressor, sink.hh, sink.mm);
  if (sink.defrost >= 0) printf(" defrost=%d", sink.defrost);
  printf("\n");
  fprintf(stderr, "frames=%ld valid=%ld\n", frames, valid);
  return 0;
}
