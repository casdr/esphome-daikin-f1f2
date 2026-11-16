# Hardware

## The bridge

The component runs on a [P1P2MQTT bridge](https://github.com/Arnold-n/P1P2MQTT) v1.2 or
v1.3:

- an ESP8266 (ESP-12F, 4 MB flash), which runs ESPHome;
- an ATmega328P with the P1P2Monitor firmware, F1F2 build, which does all bus I/O;
- a MAX22088 transceiver. The bridge is powered from the bus.

Connect it to the F1F2 terminals of an indoor unit, the outdoor unit or the central
controller. The bus has no polarity.

The ATmega reads every frame and prints it as a text line on its serial port. It sends a
frame when the ESP asks it to, and reports a collision or a refused write. Only the ATmega
touches the bus, so ESPHome crashing or restarting never puts anything on it.

## Pins

| ESP8266 | Use |
| --- | --- |
| GPIO1 / GPIO3 | UART0 to the ATmega, 250000 baud. ESPHome's logger must not use it (`logger: baud_rate: 0`). |
| GPIO5 | ATmega reset (`reset_pin`) |
| GPIO15 | ATmega serial enable (`serial_enable_pin`), needed on v1.2 and later. Also SCK while programming the ATmega. |
| GPIO2 | MOSI, only while programming the ATmega (`isp: mosi_pin`). Idles high, as booting needs. |
| GPIO0 | MISO, only while programming the ATmega (`isp: miso_pin`). Input. |

## ATmega firmware

Use the F1F2 build of P1P2Monitor: `P1P2Monitor-v0.9.58-Daikin-F1F2.ino.hex` from the
P1P2MQTT
[`Firmware_images/`](https://github.com/Arnold-n/P1P2MQTT/tree/main/Firmware_images)
folder. v0.9.56 works too; the only difference is EEPROM init on a blank chip.

To check what your ATmega runs, call the
`atmega_command` action with `V` (needs the write switch on) and look for the version line
in the ESPHome log:

```
ATmega: * P1P2Monitor v0.9.58 compiled Dec  8 2024 15:11:55 brand 1 modelnr 40 NEWP1P2LIB F1F2-series
```

To update the ATmega from ESPHome, see [atmega.md](atmega.md).

## First install

A new bridge runs the stock P1P2MQTT firmware on the ESP. To replace it with ESPHome:

1. Build the firmware: `esphome compile your-bridge.yaml`.
2. Open the bridge's web page (`http://<bridge-ip>/`), choose *Update Firmware ESP8266*
   and upload `.esphome/build/<name>/.pioenvs/<name>/firmware.bin`.
3. After the restart the bridge joins your Wi-Fi with the ESPHome config. If it can't, it
   opens the fallback access point from your config.

The stock firmware publishes Home Assistant discovery messages over MQTT. Before you
switch, send it the `D12` command (it removes its own retained discovery messages), or
remove the leftover MQTT entities in Home Assistant afterwards.

Later updates go over ESPHome OTA as usual.

## Going back to the stock firmware

Download `P1P2MQTT-bridge-*-Daikin-F1F2.ino.bin` from the P1P2MQTT `Firmware_images/`
folder and upload it as a firmware file, through ESPHome's web server OTA or with
`esphome upload your-bridge.yaml --file <image>.bin`. The ATmega keeps its firmware.

As a last resort, the ESP can be flashed with a USB-serial adapter (an ESP01 programmer
fits the bridge's header).
