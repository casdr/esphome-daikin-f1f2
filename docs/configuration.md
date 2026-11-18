# Configuration

[example.yaml](../example.yaml) is a complete device config, built from the
[packages](../packages). This page explains the `daikin_diii` options, the entities, and
how to find your system's layout.

## Packages

The packages write the hub and entity config below for you: one include per indoor unit.

| Package | vars | What it adds |
| --- | --- | --- |
| `bridge.yaml` | | ESP8266, UART, the `daikin_diii` hub with the bridge's pins and `isp`, the system sensors, the switches, the ATmega restart button and the `write_frame` / `atmega_command` actions. |
| `group-master.yaml` | `id`, `name`, `area`, `b1_index` (optional) | A device, the unit (as its own group master), climate, airflow select, temperature, setpoint, fan, mode, power, heat demand, setting commands and write budget. |
| `unit.yaml` | `id`, `group`, `name`, `area`, `b1_index` (optional) | A device, the unit as a member of `group`, temperature, mode, power and heat demand. |
| `remote-sensors.yaml` | `id` | Remote controller and pipe temperatures. The unit needs its `b1_index`. |
| `outlets.yaml` | `id` | The outlet pair selects of a 4-way cassette. |
| `hydrobox.yaml` | `id`, `name`, `area` | A device, the hydrobox (`hydrobox: true`), climate (on/off, heat/cool), temperature, heat demand step, power, heat demand, setting commands and write budget. |

- `id` and `group` are addresses as two hex digits (`"01"` = `1-01`, `"10"` = `2-00`).
  Quote them, or YAML reads `01` as a number.
- `area` is the id of an area you define under `esphome: areas:`.
- `b1_index` is written as `"0x09"`.

Add or change anything on top in your own config: ESPHome merges the packages with it.
For example, `raw_mqtt` goes under your own `daikin_diii:` key.

## The hub

```yaml
uart:
  id: atmega
  tx_pin: GPIO1
  rx_pin: GPIO3
  baud_rate: 250000
  rx_buffer_size: 1024

daikin_diii:
  id: daikin_bus
  uart_id: atmega
  reset_pin: GPIO5
  serial_enable_pin: GPIO15
  units:
    - {address: 0x00, group: 0x01, b1_index: 0x05}
    - {address: 0x01, group: 0x01, b1_index: 0x08}
```

| Option | Default | Description |
| --- | --- | --- |
| `uart_id` | | The UART to the ATmega. Must be 250000 baud with TX and RX. |
| `reset_pin` | | ESP pin on the ATmega's reset line (GPIO5). Needed for the restart button and for `isp`. |
| `serial_enable_pin` | | ESP pin that enables the ATmega's serial output on bridge v1.2 and later (GPIO15). |
| `units` | required | The indoor units, see below. |
| `source_address` | `0x30` | Bus address this bridge writes from. `0x0A` is refused: it's the iTM's address. |
| `write_budget` | `{burst: 30, per_year: 7000}` | Commands per group this bridge may send: `burst` at once, refilled at `per_year` per year. `burst: 0` turns the budget off. See [writing.md](writing.md). |
| `isp` | | Optional ATmega programming, see [atmega.md](atmega.md). Keys: `mosi_pin`, `miso_pin`, `port` (328), `window` (10 min). |
| `raw_mqtt` | | Optional raw frame stream, see below. |
| `on_unknown_frame` | | Automation, run with `frame` (hex) for the first frame of each type the component doesn't know, once per boot. `bridge.yaml` uses it for a Home Assistant notification, see [debugging.md](debugging.md#unknown-frame-notifications). |

### Units

| Key | Description |
| --- | --- |
| `address` | The unit's DIII-NET address. `1-00` is `0x00`, `1-15` is `0x0F`, `2-00` is `0x10`. |
| `group` | Address of the group master: the unit with the wired remote, which commands go to. A unit on its own is its own master. Default: its own address. |
| `b1_index` | Index of the unit in the `B1` frames, which carry the remote, pipe and intake temperatures. Optional; without it those sensors stay empty and the climate shows the return-air temperature. |
| `hydrobox` | `true` for a hydrobox: it takes a longer operation command (35 bytes). A setpoint command to it is refused. Default `false`. |

Every unit in a group follows its master: one command switches the whole group. Configure
the climate, airflow select, setpoint and fan on the master. Units that aren't in the table
are ignored.

## Finding your layout

You need a bus log for this. The easiest way is *Frame logging*, which needs no MQTT: see
[debugging.md](debugging.md). `tools/survey.py` then lists the addresses, roles, likely
groups and B1 indexes, and prints a packages list. You can also log with `raw_mqtt` and
[`tools/f1f2_log.py`](../tools/f1f2_log.py), or use a log from the stock P1P2MQTT firmware
(the same format). The sections below explain what the survey looks at.

### Addresses

Every indoor unit sends an `F5` status frame about once a minute, with its
own address as the source (byte 1). This lists them with a frame count:

```bash
awk -F'\t' '$3 ~ /^F5/ {print substr($3, 3, 2)}' my-log.tsv | sort | uniq -c
```

The labels on the units or the addresses in an iTM or wired remote show which is which.
`tools/f1f2_decode.py` has the test system's tables (`UNITS`, `B1_TO_DIII`, `GROUPS`) at the
top; change them to yours before you use it.

### Groups

The `F5` 31-byte frame says whether a unit is a group master (byte 18 bit
`0x02`). With the units configured, the component logs a warning for every unit whose role
on the bus differs from your table:

```
Unit 0x02 is a group master on the bus, but configured as member (group 0x01): check the unit table
```

Units that switch on and off together are in the same group.

### B1 indexes

`B1` frames don't use the DIII address. Each carries the unit's return-air
temperature at bytes 43-44, which is the same value as that unit's `F5` room temperature
(bytes 14-15). Match the two over a log in which the temperatures move, for example while
units start heating; a unit switching on is the clearest. [protocol.md](protocol.md#b1-index--diii-address)
shows the mapping found on the test system as an example. Yours will differ.

## Entities

All entity platforms use `platform: daikin_diii`. With `unit:` the keys belong to that
indoor unit; without it they're system-wide.

### Per unit (`unit: 0x..`)

| Platform | Key | Notes |
| --- | --- | --- |
| `climate` | | On a group master or a hydrobox. `unit` is required. On a hydrobox it offers off, heat and cool, without a target temperature. On a group master it also reports what the group is doing (`hvac_action`): heating or cooling while any unit of the group has demand (status bit `0x20`), idle without, fan in fan only, off when the group is off. Demand in cool mode is shown as cooling by analogy; the bit is only confirmed for heating. In auto the bus doesn't say which way it works, so room against setpoint decides. |
| `sensor` | `room_temperature` | Return air at the unit (`F5`). |
| `sensor` | `setpoint` | |
| `sensor` | `remote_temperature` | The wired remote's sensor. Needs `b1_index`. Units without a remote report a fixed 22.06 °C, which is ignored for the climate. |
| `sensor` | `pipe_a_temperature`, `pipe_b_temperature` | Liquid and gas pipe *(hypothesis: which is which)*. Need `b1_index`. |
| `sensor` | `intake_temperature` | Return air from the `B1` frame. Needs `b1_index`. |
| `sensor` | `setting_commands` | Setting commands to this address seen on the bus since boot, from any controller. |
| `sensor` | `write_budget` | Commands this bridge may still send to the group now. |
| `sensor` | `demand_step` | Hydrobox: 0 without heat demand, 1-15 with *(hypothesis)*. |
| `binary_sensor` | `power`, `heating_active` | `heating_active` is the unit's heat demand (status bit `0x20`). |
| `text_sensor` | `mode`, `fan` | |
| `select` | | Airflow direction, on a group master. |
| `select` | `type: outlet_pair`, `pair: "1+3"` or `"2+4"` | Per-outlet flap setting of a cassette, on any unit. Write-only. |

### System

| Platform | Keys |
| --- | --- |
| `sensor` | `outdoor_temperature`, `evaporating_temperature`, `condensing_temperature`, `discharge_temperature_1`, `discharge_temperature_2` |
| `sensor` (raw, diagnostic) | `capacity`, `compressor_step_1`, `compressor_step_2`, `fan_step_1`, `fan_step_2`, `compressor_value_1`, `compressor_value_2` |
| `sensor` (diagnostic) | `frames_ok`, `frames_error`, `write_queue`, `writes_lost` |
| `binary_sensor` | `compressor_running`, `defrost`, `raw_mqtt_connected` |
| `text_sensor` | `clock`, `last_write` |
| `switch` | `write_enable`, `raw_logging`, `frame_logging`, `atmega_programming` |
| `button` | `reset_atmega` |

Modules 1 and 2 are the two blocks of outdoor data the system reports; on the test system's
single outdoor unit they may be two compressors or circuits.

## Raw MQTT

```yaml
daikin_diii:
  raw_mqtt:
    broker: 192.168.1.10
    username: !secret mqtt_username
    password: !secret mqtt_password
```

| Key | Default |
| --- | --- |
| `broker`, `port` | required, 1883 |
| `username`, `password` | empty |
| `client_id` | `daikin-f1f2-raw` |
| `topic_prefix`, `topic_suffix` | `P1P2/`, `/P1P2MQTT/bridge0` |

Topics: `P1P2/R/...` every bus frame, `P1P2/E/...` frames with errors and ATmega errors,
`P1P2/X/...` frames this bridge wrote. The payload is the stock P1P2MQTT line
(`R T 0.453: E900FF...`), so tools made for the stock firmware keep working. The stream has
its own small MQTT client: the entities are not duplicated on MQTT.

The `raw_logging` switch turns the stream off and on and writes a note line at each change.

## Actions

`packages/bridge.yaml` adds these to `api: actions:` for protocol research.
Both need *Bus writes enabled*.

- `write_frame` (`hex`): send a raw frame, hex without the checksum. It goes through the
  same guards as every other write (source address, length, queue).
- `atmega_command` (`cmd`): pass a command to P1P2Monitor. Only `V` (version), `T`, `O`,
  `G` and `H` (timing and CRC settings) are allowed; frames must use `write_frame`.
