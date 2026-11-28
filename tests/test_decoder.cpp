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
  void on_unit_value(uint8_t addr, UnitField field, float value) override {
    units[addr].v[(int) field] = value;
    units[addr].has[(int) field] = true;
  }
  void on_unit_power(uint8_t addr, bool on) override { units[addr].power = on; }
  void on_unit_mode(uint8_t addr, uint8_t mode) override { units[addr].mode = mode; }
  void on_unit_fan(uint8_t addr, uint8_t speed) override { units[addr].fan = speed; }
  void on_unit_airflow(uint8_t addr, uint8_t direction) override { units[addr].airflow = direction; }
  void on_unit_status(uint8_t addr, uint8_t bits) override { units[addr].status = bits; }
  void on_system_value(SystemField field, float value) override {
    sys[(int) field] = value;
    sys_has[(int) field] = true;
  }
  void on_compressor(uint8_t state) override {
    compressor = state;
    detector.compressor(state);
    if (defrost >= 0) defrost = detector.active();
  }
  void on_clock(uint8_t hour, uint8_t minute) override { hh = hour, mm = minute; }
  void on_unit_role(uint8_t addr, bool master) override { units[addr].master = master; }
  void on_outdoor_module(uint8_t state, uint8_t sub_state) override {
    detector.module(state, sub_state);
    defrost = detector.active();
  }
  void on_oil_return(bool active) override { oil_return = active; }
  DefrostDetector detector;
  int defrost = -1, oil_return = -1;
};

int main(int argc, char **argv) {
  uint8_t b1map[256];
  memset(b1map, 0xFF, sizeof(b1map));
  for (auto &pair : B1_MAP) b1map[pair[0]] = pair[1];
  Sink sink;
  long frames = 0, valid = 0;
  for (int i = 1; i < argc; i++) {
    FILE *file = fopen(argv[i], "r");
    if (!file) continue;
    char line[512];
    while (fgets(line, sizeof line, file)) {
      // TSV: time \t gap \t hex [\t # note]  -> rebuild "R T gap: hex"
      char *tab1 = strchr(line, '\t');
      if (!tab1) continue;
      char *tab2 = strchr(tab1 + 1, '\t');
      if (!tab2) continue;
      char *hex = tab2 + 1;
      if (*hex == '#' || *hex == '\t' || *hex == '\n') continue;
      char *end = hex;
      while (hexval(*end) >= 0) end++;
      *end = 0;
      *tab2 = 0;
      char raw_line[400];
      snprintf(raw_line, sizeof raw_line, "R T %s: %s", tab1 + 1, hex);
      uint8_t frame[MAX_FRAME];
      size_t length = parse_raw_line(raw_line, frame, sizeof frame, nullptr);
      if (length < 2) continue;
      frames++;
      if (frame_valid(frame, length)) valid++;
      decode_frame(frame, length, b1map, sink);
    }
    fclose(file);
  }
  const char *names[] = {"room", "intake", "remote", "pipe_a", "pipe_b", "setpoint", "demand_step"};
  for (auto &entry : sink.units) {
    printf("unit %02X", entry.first);
    for (int field = 0; field < (int) UnitField::COUNT; field++)
      if (entry.second.has[field]) printf(" %s=%.2f", names[field], entry.second.v[field]);
    if (entry.second.power >= 0) printf(" power=%d", entry.second.power);
    if (entry.second.mode >= 0) printf(" mode=%02X", entry.second.mode);
    if (entry.second.fan >= 0) printf(" fan=%d", entry.second.fan);
    if (entry.second.airflow >= 0) printf(" airflow=%d", entry.second.airflow);
    if (entry.second.status >= 0) printf(" status=%02X", entry.second.status);
    if (entry.second.master >= 0) printf(" master=%d", entry.second.master);
    printf("\n");
  }
  const char *system_names[] = {"outdoor", "t_evap", "t_cond", "discharge_1", "discharge_2", "current",
                                "comp_step_1", "comp_step_2", "fan_step_1", "fan_step_2", "comp_speed_1", "comp_speed_2",
                                "comp_speed_total"};
  printf("system");
  for (int field = 0; field < (int) SystemField::COUNT; field++)
    if (sink.sys_has[field]) printf(" %s=%.2f", system_names[field], sink.sys[field]);
  printf(" compressor=%d clock=%02d:%02d", sink.compressor, sink.hh, sink.mm);
  if (sink.defrost >= 0) printf(" defrost=%d oil_return=%d", sink.defrost, sink.oil_return);
  printf("\n");
  fprintf(stderr, "frames=%ld valid=%ld\n", frames, valid);
  return 0;
}
