#pragma once

// In-system programming of the bridge's ATmega328P from the ESP8266, as an
// STK500v1 programmer ("avrisp" in avrdude) on a TCP port. Plain C++ (no
// ESPHome/Arduino), so the host tests can run it against a simulated chip.
//
// Written from the public specifications: Atmel AVR061 "STK500 Communication
// Protocol" (the v1 command set avrdude's avrisp programmer uses) and the
// ATmega328P datasheet, "Serial Downloading" (the 4-byte ISP instructions).
//
// The host side (avrdude) does the hex parsing, chip signature check, erase
// and verify. This side only translates STK500 commands to ISP instructions.

#include <cstddef>
#include <cstdint>
#include <string>

namespace diii {

// The wires: SCK, MOSI, MISO and RESET of the ATmega. On the P1P2MQTT bridge
// v1.2/v1.3 these are bit-banged GPIOs (see daikin_diii.cpp).
class IspPort {
 public:
  virtual ~IspPort() = default;
  // take the pins: SCK low, MOSI as output, MISO as input
  virtual void begin() = 0;
  // give the pins back (the caller restores serial-enable, reset etc.)
  virtual void end() = 0;
  // drive the ATmega's RESET line: true = held in reset (line low)
  virtual void set_reset(bool held) = 0;
  // one SPI mode 0 byte, MSB first; returns the byte read on MISO
  virtual uint8_t transfer(uint8_t out) = 0;
  virtual void delay_ms(uint32_t ms) = 0;
};

class Stk500 {
 public:
  // STK500v1 protocol constants (AVR061)
  static constexpr uint8_t STK_OK = 0x10;
  static constexpr uint8_t STK_FAILED = 0x11;
  static constexpr uint8_t STK_UNKNOWN = 0x12;
  static constexpr uint8_t STK_INSYNC = 0x14;
  static constexpr uint8_t STK_NOSYNC = 0x15;
  static constexpr uint8_t CRC_EOP = 0x20;

  static constexpr uint8_t CMD_GET_SYNC = 0x30;
  static constexpr uint8_t CMD_GET_SIGN_ON = 0x31;
  static constexpr uint8_t CMD_SET_PARAMETER = 0x40;
  static constexpr uint8_t CMD_GET_PARAMETER = 0x41;
  static constexpr uint8_t CMD_SET_DEVICE = 0x42;
  static constexpr uint8_t CMD_SET_DEVICE_EXT = 0x45;
  static constexpr uint8_t CMD_ENTER_PROGMODE = 0x50;
  static constexpr uint8_t CMD_LEAVE_PROGMODE = 0x51;
  static constexpr uint8_t CMD_CHIP_ERASE = 0x52;
  static constexpr uint8_t CMD_LOAD_ADDRESS = 0x55;
  static constexpr uint8_t CMD_UNIVERSAL = 0x56;
  static constexpr uint8_t CMD_PROG_FLASH = 0x60;
  static constexpr uint8_t CMD_PROG_DATA = 0x61;
  static constexpr uint8_t CMD_PROG_PAGE = 0x64;
  static constexpr uint8_t CMD_READ_PAGE = 0x74;
  static constexpr uint8_t CMD_READ_SIGN = 0x75;

  // largest page avrdude sends in one PROG_PAGE (it uses the flash page size,
  // 128 bytes on the ATmega328P; 256 leaves room)
  static constexpr size_t MAX_BLOCK = 256;

  explicit Stk500(IspPort *port) : port_(port) {}

  // Feed bytes received from the TCP client. Replies are appended to `out`.
  // Bytes of an incomplete command are kept until the rest arrives.
  void feed(const uint8_t *data, size_t length, std::string &out) {
    for (size_t i = 0; i < length; i++) {
      // A lone CRC_EOP where a command should start is the tail of a command
      // that was dropped (NOSYNC). Skip it, or the parser would stay one byte
      // off while avrdude resyncs with GET_SYNC + CRC_EOP pairs.
      if (this->len_ == 0 && data[i] == CRC_EOP) continue;
      if (this->len_ < sizeof(this->buf_)) {
        this->buf_[this->len_++] = data[i];
      } else {
        // longer than any valid command: drop it and resync
        this->len_ = 0;
        out.push_back((char) STK_NOSYNC);
        continue;
      }
      size_t need = this->needed_();
      if (need != 0 && this->len_ >= need) {
        this->execute_(out);
        this->len_ = 0;
      }
    }
  }

  // The client went away (or the session is cancelled): leave programming
  // mode and release the ATmega.
  void abort() {
    if (this->progmode_) this->leave_progmode_();
    this->len_ = 0;
  }

  bool in_progmode() const { return this->progmode_; }
  // last result of the programming-enable handshake (for logging)
  bool sync_ok() const { return this->sync_ok_; }
  uint32_t pages_written() const { return this->pages_written_; }

 protected:
  // Total length of the command at the start of buf_, including CRC_EOP.
  // 0 = not known yet (need more bytes).
  size_t needed_() const {
    switch (this->buf_[0]) {
      case CMD_GET_SYNC:
      case CMD_GET_SIGN_ON:
      case CMD_ENTER_PROGMODE:
      case CMD_LEAVE_PROGMODE:
      case CMD_CHIP_ERASE:
      case CMD_READ_SIGN:
        return 2;
      case CMD_GET_PARAMETER:
        return 3;
      case CMD_SET_PARAMETER:
        return 4;
      case CMD_LOAD_ADDRESS:
      case CMD_PROG_FLASH:
        return 4;
      case CMD_PROG_DATA:
        return 3;
      case CMD_UNIVERSAL:
        return 6;
      case CMD_SET_DEVICE:
        return 22;  // 20 parameter bytes
      case CMD_SET_DEVICE_EXT:
        return 7;  // 5 parameter bytes (avrdude for programmer software >= 1.10)
      case CMD_READ_PAGE:
        return 5;
      case CMD_PROG_PAGE: {
        if (this->len_ < 3) return 0;
        size_t block_length = ((size_t) this->buf_[1] << 8) | this->buf_[2];
        if (block_length > MAX_BLOCK) return 2;  // refused in execute_
        return 1 + 2 + 1 + block_length + 1;     // cmd, length, memtype, data, EOP
      }
      default:
        return 2;  // unknown command: answered after one more byte
    }
  }

  void execute_(std::string &out) {
    const uint8_t cmd = this->buf_[0];
    const size_t length = this->needed_();
    if (this->buf_[length - 1] != CRC_EOP) {
      out.push_back((char) STK_NOSYNC);
      return;
    }
    switch (cmd) {
      case CMD_GET_SYNC:
        this->ok_(out);
        break;
      case CMD_GET_SIGN_ON:
        out.push_back((char) STK_INSYNC);
        out.append("AVR ISP");
        out.push_back((char) STK_OK);
        break;
      case CMD_GET_PARAMETER:
        out.push_back((char) STK_INSYNC);
        out.push_back((char) this->parameter_(this->buf_[1]));
        out.push_back((char) STK_OK);
        break;
      case CMD_SET_PARAMETER:
      case CMD_SET_DEVICE_EXT:
      case CMD_PROG_FLASH:
      case CMD_PROG_DATA:
        this->ok_(out);
        break;
      case CMD_SET_DEVICE: {
        // parameter bytes 12-13: flash page size in bytes, big endian
        // (buf_[0] is the command, so parameter k is buf_[1 + k])
        uint16_t page_size = (uint16_t) ((this->buf_[13] << 8) | this->buf_[14]);
        this->page_bytes_ = (page_size >= 2 && page_size <= MAX_BLOCK) ? page_size : 128;
        this->ok_(out);
        break;
      }
      case CMD_ENTER_PROGMODE:
        this->enter_progmode_();
        // like other avrisp programmers this answers OK either way; avrdude
        // then reads the signature and stops on a mismatch (0x000000/0xFFFFFF)
        this->ok_(out);
        break;
      case CMD_LEAVE_PROGMODE:
        this->leave_progmode_();
        this->ok_(out);
        break;
      case CMD_CHIP_ERASE:
        // avrdude erases with UNIVERSAL (AC 80 00 00); this is only a fallback
        if (this->progmode_) {
          this->isp_(0xAC, 0x80, 0x00, 0x00);
          this->port_->delay_ms(ERASE_DELAY_MS);
        }
        this->ok_(out);
        break;
      case CMD_LOAD_ADDRESS:
        this->address_ = (uint16_t) (this->buf_[1] | (this->buf_[2] << 8));  // little endian
        this->ok_(out);
        break;
      case CMD_UNIVERSAL: {
        uint8_t reply = this->progmode_ ? this->isp_(this->buf_[1], this->buf_[2], this->buf_[3], this->buf_[4]) : 0;
        if (this->progmode_ && this->buf_[1] == 0xAC && this->buf_[2] == 0x80) this->port_->delay_ms(ERASE_DELAY_MS);
        out.push_back((char) STK_INSYNC);
        out.push_back((char) reply);
        out.push_back((char) STK_OK);
        break;
      }
      case CMD_READ_SIGN:
        out.push_back((char) STK_INSYNC);
        for (uint8_t i = 0; i < 3; i++)
          out.push_back((char) (this->progmode_ ? this->isp_(0x30, 0x00, i, 0x00) : 0x00));
        out.push_back((char) STK_OK);
        break;
      case CMD_PROG_PAGE:
        this->prog_page_(out);
        break;
      case CMD_READ_PAGE:
        this->read_page_(out);
        break;
      default:
        out.push_back((char) STK_UNKNOWN);
        break;
    }
  }

  static void ok_(std::string &out) {
    out.push_back((char) STK_INSYNC);
    out.push_back((char) STK_OK);
  }

  // values avrdude reads (hardware / software version of an AVR ISP)
  static uint8_t parameter_(uint8_t parameter) {
    switch (parameter) {
      case 0x80:  // hardware version
        return 2;
      case 0x81:  // software major
        return 1;
      case 0x82:  // software minor
        return 18;
      case 0x93:  // SCK duration: report a slow clock, matches the bit-banged SPI
        return 1;
      case 0x98:  // top card detect
        return 3;
      default:
        return 0;
    }
  }

  uint8_t isp_(uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4) {
    this->port_->transfer(byte1);
    this->port_->transfer(byte2);
    this->port_->transfer(byte3);
    return this->port_->transfer(byte4);
  }

  // Datasheet "Serial Programming Algorithm": RESET and SCK low, wait at
  // least 20 ms, send Programming Enable; the 3rd byte echoes 0x53 when the
  // chip is in sync. If not, give RESET a positive pulse and try again.
  void enter_progmode_() {
    if (this->progmode_) return;
    this->port_->begin();
    this->port_->set_reset(true);
    this->port_->delay_ms(20);
    this->sync_ok_ = false;
    for (int attempt = 0; attempt < ENABLE_ATTEMPTS && !this->sync_ok_; attempt++) {
      if (attempt > 0) {
        this->port_->set_reset(false);
        this->port_->delay_ms(1);
        this->port_->set_reset(true);
        this->port_->delay_ms(20);
      }
      this->port_->transfer(0xAC);
      this->port_->transfer(0x53);
      this->sync_ok_ = this->port_->transfer(0x00) == 0x53;
      this->port_->transfer(0x00);
    }
    this->progmode_ = true;
    this->pages_written_ = 0;
  }

  void leave_progmode_() {
    this->progmode_ = false;
    this->port_->end();
    this->port_->set_reset(false);
  }

  // Load Program Memory Page (0x40 low byte, 0x48 high byte) per word, then
  // Write Program Memory Page (0x4C) for every page the block touches.
  // address_ is a word address for flash and a byte address for EEPROM.
  void prog_page_(std::string &out) {
    const size_t block_length = ((size_t) this->buf_[1] << 8) | this->buf_[2];
    const uint8_t memtype = this->buf_[3];
    const uint8_t *data = this->buf_ + 4;
    if (block_length > MAX_BLOCK || !this->progmode_) {
      out.push_back((char) STK_INSYNC);
      out.push_back((char) STK_FAILED);
      return;
    }
    if (memtype == 'F') {
      const uint16_t page_words = (uint16_t) (this->page_bytes_ / 2);
      uint16_t addr = this->address_;
      uint16_t page = (uint16_t) (addr - addr % page_words);
      for (size_t i = 0; i < block_length; i += 2) {
        uint16_t this_page = (uint16_t) (addr - addr % page_words);
        if (this_page != page) {
          this->commit_page_(page);
          page = this_page;
        }
        uint8_t low = data[i];
        uint8_t high = (i + 1 < block_length) ? data[i + 1] : 0xFF;
        this->isp_(0x40, 0x00, (uint8_t) (addr & 0xFF), low);
        this->isp_(0x48, 0x00, (uint8_t) (addr & 0xFF), high);
        addr++;
      }
      this->commit_page_(page);
      this->address_ = addr;
      this->ok_(out);
    } else if (memtype == 'E') {
      // EEPROM, one byte at a time (0xC0); not needed for a firmware update
      uint16_t addr = this->address_;
      for (size_t i = 0; i < block_length; i++, addr++) {
        this->isp_(0xC0, (uint8_t) (addr >> 8), (uint8_t) (addr & 0xFF), data[i]);
        this->wait_ready_(EEPROM_DELAY_MS);
      }
      this->address_ = addr;
      this->ok_(out);
    } else {
      out.push_back((char) STK_INSYNC);
      out.push_back((char) STK_FAILED);
    }
  }

  void commit_page_(uint16_t word_addr) {
    this->isp_(0x4C, (uint8_t) (word_addr >> 8), (uint8_t) (word_addr & 0xFF), 0x00);
    this->wait_ready_(FLASH_DELAY_MS);
    this->pages_written_++;
  }

  // Poll RDY/BSY (0xF0, bit 0 = busy) until done, at most `max_ms`.
  void wait_ready_(uint32_t max_ms) {
    for (uint32_t waited_ms = 0; waited_ms < max_ms; waited_ms++) {
      if ((this->isp_(0xF0, 0x00, 0x00, 0x00) & 0x01) == 0) return;
      this->port_->delay_ms(1);
    }
  }

  void read_page_(std::string &out) {
    const size_t block_length = ((size_t) this->buf_[1] << 8) | this->buf_[2];
    const uint8_t memtype = this->buf_[3];
    if (block_length > MAX_BLOCK || (memtype != 'F' && memtype != 'E')) {
      out.push_back((char) STK_INSYNC);
      out.push_back((char) STK_FAILED);
      return;
    }
    out.push_back((char) STK_INSYNC);
    uint16_t addr = this->address_;
    if (memtype == 'F') {
      for (size_t i = 0; i < block_length; i += 2, addr++) {
        out.push_back((char) (this->progmode_ ? this->isp_(0x20, (uint8_t) (addr >> 8), (uint8_t) addr, 0) : 0xFF));
        if (i + 1 < block_length)
          out.push_back((char) (this->progmode_ ? this->isp_(0x28, (uint8_t) (addr >> 8), (uint8_t) addr, 0) : 0xFF));
      }
    } else {
      for (size_t i = 0; i < block_length; i++, addr++)
        out.push_back((char) (this->progmode_ ? this->isp_(0xA0, (uint8_t) (addr >> 8), (uint8_t) addr, 0) : 0xFF));
    }
    this->address_ = addr;
    out.push_back((char) STK_OK);
  }

  static constexpr int ENABLE_ATTEMPTS = 5;
  static constexpr uint32_t FLASH_DELAY_MS = 10;   // datasheet t_WD_FLASH 4.5 ms
  static constexpr uint32_t EEPROM_DELAY_MS = 10;  // t_WD_EEPROM 3.6 ms
  static constexpr uint32_t ERASE_DELAY_MS = 20;   // t_WD_ERASE 9 ms

  IspPort *port_;
  uint8_t buf_[1 + 2 + 1 + MAX_BLOCK + 1];
  size_t len_{0};
  bool progmode_{false};
  bool sync_ok_{false};
  uint16_t address_{0};
  uint16_t page_bytes_{128};
  uint32_t pages_written_{0};
};

}  // namespace diii
