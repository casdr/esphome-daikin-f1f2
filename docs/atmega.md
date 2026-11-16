# Updating the ATmega

The ATmega on the bridge runs P1P2Monitor. The stock P1P2MQTT firmware can program it over
the network, so normally you'd need the stock firmware back on the ESP to update it. This
component can do the same: the ESP programs the ATmega over ISP and acts as an STK500v1
programmer on a TCP port, so the usual `avrdude -c avrisp` works.

## Setup

```yaml
daikin_diii:
  reset_pin: GPIO5          # RESET
  serial_enable_pin: GPIO15 # SCK while programming
  isp:
    mosi_pin: GPIO2
    miso_pin: GPIO0
    # port: 328
    # window: 10min

switch:
  - platform: daikin_diii
    atmega_programming: {name: ATmega programming}
```

The ISP lines are bit-banged on the pins the bridge wires to the ATmega's programming pins,
the same ones the stock firmware uses.

## Flashing

1. Get the image from the P1P2MQTT
   [`Firmware_images/`](https://github.com/Arnold-n/P1P2MQTT/tree/main/Firmware_images)
   folder: `P1P2Monitor-v0.9.58-Daikin-F1F2.ino.hex` for the F1F2 bus.
2. Turn on *ATmega programming*. The port listens for 10 minutes (`window`), then the switch
   turns itself off. It's always off after a restart. Bus writes don't need to be on.
3. Optional, read-only: check the connection and what's on the chip now. This compares the
   flash with an image without writing anything:
   ```bash
   avrdude -c avrisp -P net:<bridge-ip>:328 -p m328p -U flash:v:P1P2Monitor-v0.9.56-Daikin-F1F2.ino.hex:i
   ```
4. Write (avrdude erases, writes and verifies):
   ```bash
   avrdude -c avrisp -P net:<bridge-ip>:328 -p m328p -U flash:w:P1P2Monitor-v0.9.58-Daikin-F1F2.ino.hex:i
   ```
5. Check the version with the `atmega_command` action `V` (the reply is in the ESPHome log)
   and that *Bus frames OK* counts up again.
6. Turn *ATmega programming* off, or let it turn itself off.

On a P1P2MQTT bridge v1.3 writing the 17 KB image takes about 13 s and verifying another 13 s.

## What happens meanwhile

- While avrdude is connected and the ATmega is in programming mode, it's held in reset: no
  bus frames, no writes, and a gap in the raw stream. The rest of the bus is not affected;
  the bridge only listens and sends.
- When avrdude is done, the ATmega is released, the ESP drops whatever was left in the
  serial buffer and restarts the serial link as at boot.
- If avrdude disconnects mid-way or sends nothing for 30 s, the session is aborted and the
  ATmega released. A half-written chip means no bus data until a successful run; the ESP
  keeps running, so just run avrdude again.
- One client at a time. A session that's still running when the window ends gets 5 more
  minutes.

## Notes

- Only write flash. Leave the fuses alone, and leave the EEPROM alone too: P1P2Monitor keeps
  its settings there.
- The port has no password. That's why it only listens while the switch is on, for a
  limited time.
- The STK500v1 handling is in [`components/daikin_diii/avr_isp.h`](../components/daikin_diii/avr_isp.h),
  written from Atmel's application note AVR061 (STK500 protocol) and the ATmega328P
  datasheet (serial programming). [`tests/test_isp.cpp`](../tests/test_isp.cpp) runs a full
  avrdude-style session against a simulated ATmega328P.
