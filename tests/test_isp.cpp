// Host tests for the ATmega programmer (avr_isp.h): an avrdude-style STK500v1
// session against a simulated ATmega328P that answers the ISP instructions
// from the datasheet. Exit code != 0 on failure.
//   make -C esphome/tests test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "avr_isp.h"

using namespace diii;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                  \
    }                                                              \
  } while (0)

// ATmega328P as seen over ISP: 16 K words of flash, 64-word pages, 1 KB EEPROM.
class FakeAtmega : public IspPort {
 public:
  static constexpr size_t FLASH_WORDS = 16384;
  static constexpr size_t PAGE_WORDS = 64;

  std::vector<uint16_t> flash = std::vector<uint16_t>(FLASH_WORDS, 0xFFFF);
  std::vector<uint8_t> eeprom = std::vector<uint8_t>(1024, 0xFF);
  uint16_t page_buf[PAGE_WORDS];
  bool pins_taken = false;
  bool reset_held = false;
  bool enabled = false;      // programming enable accepted
  int fail_enables = 0;      // refuse this many programming-enable attempts first
  int enable_attempts = 0;
  uint32_t delayed_ms = 0;
  int page_writes = 0;

  FakeAtmega() { std::memset(this->page_buf, 0xFF, sizeof(this->page_buf)); }

  void begin() override { this->pins_taken = true; }
  void end() override { this->pins_taken = false; }
  void set_reset(bool held) override {
    if (!held) this->enabled = false;  // leaving reset ends programming mode
    this->reset_held = held;
  }
  void delay_ms(uint32_t ms) override { this->delayed_ms += ms; }

  uint8_t transfer(uint8_t out) override {
    if (!this->pins_taken || !this->reset_held) return 0xFF;  // not listening
    this->in_[this->n_++] = out;
    uint8_t reply = 0;
    if (this->n_ == 3 && this->in_[0] == 0xAC && this->in_[1] == 0x53) {
      // Programming Enable: the 3rd byte echoes 0x53 once the chip is in sync
      this->enable_attempts++;
      if (this->enable_attempts > this->fail_enables) {
        this->enabled = true;
        reply = 0x53;
      }
    }
    if (this->n_ == 4) {
      reply = this->execute_();
      this->n_ = 0;
    }
    return reply;
  }

  uint16_t word(size_t addr) const { return this->flash[addr]; }

 private:
  uint8_t in_[4];
  int n_ = 0;

  uint8_t execute_() {
    const uint8_t a = this->in_[0], b = this->in_[1], c = this->in_[2], d = this->in_[3];
    if (!this->enabled) return 0xFF;
    const uint16_t addr = (uint16_t) ((b << 8) | c);
    switch (a) {
      case 0xAC:
        if (b == 0x80) std::fill(this->flash.begin(), this->flash.end(), 0xFFFF);  // chip erase
        return 0;
      case 0x30: {  // read signature byte c
        static const uint8_t sig[3] = {0x1E, 0x95, 0x0F};
        return c < 3 ? sig[c] : 0;
      }
      case 0x40:  // load page buffer, low byte (word index in c)
        this->page_buf[c % PAGE_WORDS] = (uint16_t) ((this->page_buf[c % PAGE_WORDS] & 0xFF00) | d);
        return 0;
      case 0x48:  // load page buffer, high byte
        this->page_buf[c % PAGE_WORDS] = (uint16_t) ((this->page_buf[c % PAGE_WORDS] & 0x00FF) | (d << 8));
        return 0;
      case 0x4C: {  // write page: the page containing word address `addr`
        size_t base = addr - addr % PAGE_WORDS;
        // flash can only be programmed from 1 to 0 (needs an erase first)
        for (size_t i = 0; i < PAGE_WORDS; i++) this->flash[base + i] &= this->page_buf[i];
        std::memset(this->page_buf, 0xFF, sizeof(this->page_buf));
        this->page_writes++;
        return 0;
      }
      case 0x20:
        return (uint8_t) (this->flash[addr] & 0xFF);
      case 0x28:
        return (uint8_t) (this->flash[addr] >> 8);
      case 0xC0:
        this->eeprom[addr % 1024] = d;
        return 0;
      case 0xA0:
        return this->eeprom[addr % 1024];
      case 0xF0:
        return 0;  // never busy
      default:
        return 0;
    }
  }
};

static std::string run(Stk500 &stk, const std::vector<uint8_t> &cmd, bool byte_by_byte = false) {
  std::string out;
  if (byte_by_byte) {
    for (uint8_t b : cmd) stk.feed(&b, 1, out);
  } else {
    stk.feed(cmd.data(), cmd.size(), out);
  }
  return out;
}

static std::string bytes(std::initializer_list<int> l) {
  std::string s;
  for (int b : l) s.push_back((char) b);
  return s;
}

static const std::string OK = bytes({0x14, 0x10});

// SET_DEVICE as avrdude sends it for an m328p: page size 128 at parameters 12-13
static std::vector<uint8_t> set_device() {
  std::vector<uint8_t> v = {0x42, 0x86, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x03, 0xFF, 0xFF,
                            0xFF, 0xFF, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x80, 0x00, 0x20};
  return v;
}

static std::vector<uint8_t> prog_page(const std::vector<uint8_t> &data, char memtype = 'F') {
  std::vector<uint8_t> v = {0x64, (uint8_t) (data.size() >> 8), (uint8_t) data.size(), (uint8_t) memtype};
  v.insert(v.end(), data.begin(), data.end());
  v.push_back(0x20);
  return v;
}

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = (uint8_t) (seed + i * 7);
  return v;
}

static void test_session(bool byte_by_byte) {
  FakeAtmega chip;
  Stk500 stk(&chip);

  CHECK(run(stk, {0x30, 0x20}, byte_by_byte) == OK);
  CHECK(run(stk, {0x31, 0x20}, byte_by_byte) == bytes({0x14, 'A', 'V', 'R', ' ', 'I', 'S', 'P', 0x10}));
  CHECK(run(stk, {0x41, 0x81, 0x20}, byte_by_byte) == bytes({0x14, 1, 0x10}));
  CHECK(run(stk, {0x41, 0x82, 0x20}, byte_by_byte) == bytes({0x14, 18, 0x10}));
  CHECK(run(stk, set_device(), byte_by_byte) == OK);
  CHECK(run(stk, {0x45, 0x05, 0x04, 0xD7, 0xC2, 0x00, 0x20}, byte_by_byte) == OK);

  CHECK(!chip.reset_held);
  CHECK(run(stk, {0x50, 0x20}, byte_by_byte) == OK);
  CHECK(stk.in_progmode());
  CHECK(stk.sync_ok());
  CHECK(chip.reset_held && chip.pins_taken);

  CHECK(run(stk, {0x75, 0x20}, byte_by_byte) == bytes({0x14, 0x1E, 0x95, 0x0F, 0x10}));
  // signature through UNIVERSAL, as avrdude also does
  CHECK(run(stk, {0x56, 0x30, 0x00, 0x01, 0x00, 0x20}, byte_by_byte) == bytes({0x14, 0x95, 0x10}));

  chip.flash[5] = 0x1234;  // old firmware
  CHECK(run(stk, {0x56, 0xAC, 0x80, 0x00, 0x00, 0x20}, byte_by_byte) == bytes({0x14, 0x00, 0x10}));
  CHECK(chip.word(5) == 0xFFFF);  // erased

  // page 0
  auto p0 = pattern(128, 1);
  CHECK(run(stk, {0x55, 0x00, 0x00, 0x20}, byte_by_byte) == OK);
  CHECK(run(stk, prog_page(p0), byte_by_byte) == OK);
  CHECK(chip.page_writes == 1);
  CHECK(chip.word(0) == (uint16_t) (p0[0] | (p0[1] << 8)));
  CHECK(chip.word(63) == (uint16_t) (p0[126] | (p0[127] << 8)));

  // a 256-byte block from word 0x40 covers pages 1 and 2; LOAD_ADDRESS is little endian
  auto p12 = pattern(256, 9);
  CHECK(run(stk, {0x55, 0x40, 0x00, 0x20}, byte_by_byte) == OK);
  CHECK(run(stk, prog_page(p12), byte_by_byte) == OK);
  CHECK(chip.page_writes == 3);
  CHECK(chip.word(0x40) == (uint16_t) (p12[0] | (p12[1] << 8)));
  CHECK(chip.word(0xBF) == (uint16_t) (p12[254] | (p12[255] << 8)));
  CHECK(chip.word(0xC0) == 0xFFFF);

  // a high page: word address 0x3FC0 (last page of the 32 KB flash)
  auto plast = pattern(128, 77);
  CHECK(run(stk, {0x55, 0xC0, 0x3F, 0x20}, byte_by_byte) == OK);
  CHECK(run(stk, prog_page(plast), byte_by_byte) == OK);
  CHECK(chip.word(0x3FC0) == (uint16_t) (plast[0] | (plast[1] << 8)));

  // read back (verify)
  CHECK(run(stk, {0x55, 0x40, 0x00, 0x20}, byte_by_byte) == OK);
  std::string back = run(stk, {0x74, 0x01, 0x00, 'F', 0x20}, byte_by_byte);
  CHECK(back.size() == 258);
  CHECK(back.front() == 0x14 && back.back() == 0x10);
  CHECK(back.substr(1, 256) == std::string(p12.begin(), p12.end()));

  // EEPROM write and read
  CHECK(run(stk, {0x55, 0x10, 0x00, 0x20}, byte_by_byte) == OK);
  CHECK(run(stk, prog_page({0xAB, 0xCD}, 'E'), byte_by_byte) == OK);
  CHECK(chip.eeprom[0x10] == 0xAB && chip.eeprom[0x11] == 0xCD);
  CHECK(run(stk, {0x55, 0x10, 0x00, 0x20}, byte_by_byte) == OK);
  CHECK(run(stk, {0x74, 0x00, 0x02, 'E', 0x20}, byte_by_byte) == bytes({0x14, 0xAB, 0xCD, 0x10}));

  CHECK(run(stk, {0x51, 0x20}, byte_by_byte) == OK);
  CHECK(!stk.in_progmode());
  CHECK(!chip.reset_held && !chip.pins_taken);
}

static void test_errors() {
  FakeAtmega chip;
  Stk500 stk(&chip);
  CHECK(run(stk, {0x30, 0x21}) == bytes({0x15}));         // wrong end-of-packet: no sync
  CHECK(run(stk, {0x99, 0x20}) == bytes({0x12}));         // unknown command
  CHECK(run(stk, prog_page(pattern(4, 0))) == bytes({0x14, 0x11}));  // not in programming mode
  CHECK(chip.page_writes == 0);
  std::string too_long = run(stk, {0x64, 0x02, 0x00, 'F', 0x20});  // 512 bytes: refused
  CHECK(!too_long.empty() && too_long[0] == 0x15);
  CHECK(chip.page_writes == 0);
  CHECK(run(stk, {0x30, 0x20}) == OK);                    // back in sync afterwards
  CHECK(run(stk, {0x20, 0x30, 0x20}) == OK);              // a stray end-of-packet is skipped

  // the chip refuses programming enable twice; the third attempt (after a reset pulse) works
  FakeAtmega slow;
  slow.fail_enables = 2;
  Stk500 s2(&slow);
  CHECK(run(s2, {0x50, 0x20}) == OK);
  CHECK(s2.sync_ok());
  CHECK(slow.enable_attempts == 3);

  // a chip that never answers: avrdude gets OK, then a zero signature and stops
  FakeAtmega dead;
  dead.fail_enables = 1000;
  Stk500 s3(&dead);
  CHECK(run(s3, {0x50, 0x20}) == OK);
  CHECK(!s3.sync_ok());
  CHECK(run(s3, {0x75, 0x20}) == bytes({0x14, 0xFF, 0xFF, 0xFF, 0x10}));

  // client disconnects mid-session: abort releases the ATmega
  FakeAtmega chip2;
  Stk500 s4(&chip2);
  CHECK(run(s4, {0x50, 0x20}) == OK);
  std::string partial = run(s4, {0x64, 0x00, 0x80, 'F', 0x01, 0x02});  // incomplete page
  CHECK(partial.empty());
  s4.abort();
  CHECK(!s4.in_progmode());
  CHECK(!chip2.reset_held && !chip2.pins_taken);
  CHECK(chip2.page_writes == 0);
  CHECK(run(s4, {0x30, 0x20}) == OK);  // the partial command was dropped
}

int main() {
  test_session(false);
  test_session(true);
  test_errors();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all ISP tests passed\n");
  return 0;
}
