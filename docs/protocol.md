# Daikin F1F2 (DIII-NET) protocol

## Introduction

F1F2 is the DIII-NET bus of Daikin VRV systems. It connects:

- the outdoor unit, address `0x00`. Status bursts arrive with very regular
  timing, so it is probably the bus master *(hyp)*;
- the indoor units and other indoor equipment such as a hydrobox, one
  DIII address each;
- central controllers such as the Daikin intelligent Touch Manager (iTM),
  which uses address `0x0A`.

Everything here was worked out from bus logs of one test system:

- outdoor unit REYQ10T7Y1B (from the nameplate), a 10 HP VRV IV *heat recovery*
  unit. Each branch (BS box) can heat or cool on its own;
- 16 indoor units in 7 remote-control groups, and a hydrobox;
- an iTM, used to send commands while logging, and whose own display/web interface
  gave the values to check the bus against;
- a second P1P2MQTT bridge on the P1/P2 bus of one unit (group 1-04), for checks
  against the values P1P2MQTT already decodes.

Other systems may differ. A meaning marked *(hyp)* is a hypothesis that fits the
data but is not confirmed. Everything still open is collected under
[Open questions](#open-questions).

## Physical layer and the P1P2MQTT bridge

Frames were captured and written with a
[P1P2MQTT](https://github.com/Arnold-n/P1P2MQTT) bridge. Its ATmega runs the F1F2
build of P1P2Monitor and does all bus I/O. It prints one text line per event on its
serial port:

| line | meaning |
|------|---------|
| `R T  3.745: FD0AFF140650820008000003` | A received frame. `T` is the gap since the previous frame in seconds, the hex is the frame. CRC/parity are already stripped. |
| `E …` | A frame received with an error. |
| `* …` | A status message, for example `* Scheduling write: …` after a write, a collision, or `Refusing to write packet while previous packet wasn't finished`. |

The stock ESP firmware publishes these lines on MQTT: frames on `P1P2/R/<topic suffix>`,
status on `P1P2/S/<topic suffix>`.

Commands to the ATmega start with `1P2P`. A write is `W<hex>`, the full frame
including its checksum. With the stock ESP firmware you publish `W<hex>` on
`P1P2/W/<topic suffix>` and the ESP passes it on unchanged as `1P2PW…`.

- The F1F2 build sends the frame as given, directly after bus silence. It adds no
  CRC and enforces no write budget.
- The ATmega has a write buffer of one frame. A second frame sent before the first
  is out is refused. Space writes at least 1 s apart and wait for the ACK.
- Your own writes come back as `R` frames, like any other traffic.

## Frame format

```
+-----+-----+-----+-----+-----+--------------+----+
| cmd | src | dst | b3  | len | payload[len] | cs |
+-----+-----+-----+-----+-----+--------------+----+
  0     1      2     3     4     5 ..           5+len
```

- `cmd`: frame type (`F5`, `B1`, `E9`, `A5`, …), see [Frame reference](#frame-reference).
- `src` = sender, `dst` = destination, `FF` = broadcast.
- `b3` takes the values `08`/`10`/`14`/`1C`. Possibly a source/class field *(hyp)*. It
  belongs to the frame type: logs of two other systems (an RXQ20 in [P1P2MQTT issue #78](https://github.com/Arnold-n/P1P2MQTT/issues/78) and an ERQ in
  [discussion #293](https://github.com/Arnold-n/P1P2MQTT/discussions/293)) have the same `b3` per frame type as this one (`E9` page `65` `10`, page `E0`
  `1C`, `B1` `08`, `A9` `10`, `F5` `1C`, ...). Whether it names the sending device is open.
- `len` = payload length. Total frame length = `6 + len` (holds for every frame
  type seen).
- The checksum byte makes `sum(bytes[1:]) & 0xFF == 0`, so
  `cs = -sum(bytes[1:-1]) & 0xFF`. The cmd byte isn't part of the sum.
- Many frame types carry a sub-command or page id in payload[0] (frame byte 5).
- `6D 67 C0 18` appears in many frames (`A9`, `B1`, `E9`, `AD`, `F5` page `90`).
  It identifies the installation: the RXQ20 and ERQ logs above carry `AF 42 D0 48` and
  `FF 97 C3 17` at the same place in their `A9`, `B1` and `E9` frames, constant over the
  whole log. What it is derived from (a serial number?) is unknown.

An ACK is a single byte `06`, sent about 2 ms after a directed frame (`A5`, `A9`,
`B1`, …). About 28% of all frames are ACKs.

Byte offsets in this document count from the start of the frame (cmd = byte 0),
unless they say "payload[n]".

The component names the offsets it reads or writes in
[`diii_decoder.h`](../components/daikin_diii/diii_decoder.h), one namespace per frame
type, for example `f5_status::MODE` for byte 6 of the `F5` 23 B frame. The *code* column
of each frame table below gives the name; a byte without one isn't used by the code.
The header bytes are `header::CMD` (0), `header::SRC` (1), `header::DST` (2),
`header::CLASS` (3, `b3`), `header::PAYLOAD_LENGTH` (4) and `header::PAYLOAD` (5, payload[0]).
All names are frame offsets, also for the `A5` command whose table counts payload
bytes (payload[n] = frame byte 5 + n).

## Data types

| type | encoding | example |
|------|----------|---------|
| temperature, word | 8.8 fixed point, big endian, °C = value / 256 | `16 00` = 22.0, `0x1179` = 17.47 |
| temperature, byte | whole °C | `0x16` = 22 |
| negative values (`E9` frames) | sign-magnitude: top bit = negative | `0x88` = −8 °C, `0x8B46` = −11.27 °C |

Indoor temperatures are unsigned.

## Addressing

### DIII addresses

A unit's address on the bus is its DIII-NET address as a linear index:
`(group-1)*16 + unit`. So `1-00` → `0x00`, `1-15` → `0x0F`, `2-00` → `0x10`.

The iTM uses `0x0A`. On the test system no unit has DIII address `1-10`, so that
address is free for it, and `0x0A` never appears on the bus as a unit.

### Groups and group masters

Indoor units are grouped behind a shared remote control. Only the group master
takes commands; a command to the master switches every unit of the group, and each
member then broadcasts its own status. The members cannot be addressed on their own.
In the iTM the members have no power state or fan mode of their own and offer only
off/heat. The units with the changeover option in the iTM are exactly the group
masters.

A unit says whether it is a master in its `F5` 31 B status: byte 18 `0x12` (master)
or `0x10` (member), together with byte 22 (`02` / `12`) and byte 27 (`0` / `1`).
To find the groups on a new system, switch each master on in turn and watch which
`F5` sources report power on.

Groups on the test system:

| master (A5 dst) | members (F5 src) |
|-----------------|------------------|
| 0x01 (1-01) | 0x00, 0x01, 0x02 |
| 0x04 (1-04) | 0x03, 0x04, 0x05, 0x06 |
| 0x07 (1-07) | 0x07 |
| 0x09 (1-09) | 0x08, 0x09, 0x0B |
| 0x0C (1-12) | 0x0C |
| 0x0F (1-15) | 0x0D, 0x0E, 0x0F |
| 0x11 (2-01) | 0x11 |
| 0x10 (2-00) | hydrobox, its own unit |

### B1 index ↔ DIII address

`A9`, `B1` and `AD` frames do not use the DIII address. They use a 1-based index
`0x01..0x11` (17 entries, `0x0A` included), probably the outdoor unit's internal
numbering *(hyp)*. The mapping has to be found per system:

1. Correlate the return-air temperature in `B1` 46 B (bytes 43–44) with the room
   temperature in each unit's `F5` 23 B frame (bytes 14–15) over a long log.
2. Confirm by switching a group on: the `B1` 34 B frames of its members change
   byte 12 from `00` to `05`. For group 1-04 that was `B1` 07/09/0B/0C.
3. The hydrobox sends no 46 B `B1` frame. Its index shows up as the only source of
   the 28/33 B frames, whose byte 23 changes when the hydrobox starts.

The mapping on the test system:

| B1 | DIII | | B1 | DIII |
|----|------|-|----|------|
| 01 | 1-12 | | 09 | 1-03 |
| 02 | 1-08 *(weak, maybe 1-00)* | | 0A | 1-07 |
| 03 | 1-11 | | 0B | 1-06 |
| 04 | 1-09 | | 0C | 1-05 |
| 05 | 1-00 *(weak, maybe 1-08)* | | 0D | 1-14 |
| 06 | 1-02 | | 0E | 2-01 |
| 07 | 1-04 | | 0F | 1-13 |
| 08 | 1-01 | | 10 | 1-15 |
| 11 | 2-00 hydrobox (28/33 B frames) | | | |

The pair 02/05 could only be weakly separated by temperature. The `AD` frames from
group 1-01's units come from 05/06/07, which does not match this table (1-01 = `B1`
08), so either `AD` numbers differently or the 02/05 pair is wrong.

### Source addresses for a third-party controller

Units accept commands from any source address. A frame from an unused address
gets the same ACK and the same status change as one from the iTM.

- **Never send with source `0x0A`** (the iTM's address). The iTM then logs the error
  `DIII-NET "Adres Geduplic.(1)"` (duplicate address), code MC. It was cleared in the iTM's service mode, after which the iTM
  restarted (see [`A4` and `BD`](#a4-and-bd-itm-start-up)).
- Pick an address no unit or controller uses; check a log of the bus first. The
  ESPHome component uses `0x30`, so it has its own identity and does not pose as
  the iTM. Commands from `0x30` gave no error.

## Bus traffic and timing

| traffic | sender → receiver | when |
|---------|-------------------|------|
| `F5` 23 B status | each unit → `FF` | about every 60 s per unit |
| `F5` 31 B status | each unit → `FF` | within 1–5 s of a change (group members within 1–3 s of the command), plus a full burst of all units every 3–4 min and sometimes right after a command |
| `F5` 10/8 B, page `91` | each unit → `FF` | hourly at hh:10:40, after the iTM's `E5 … 50 91` |
| `B1` | unit index → `00` | in bursts; sensor values change about every 50 s |
| `A9` 24 B | `00` → each unit index | periodic, per unit |
| `A9` 46 B, page `6A` | `00` → each unit index | in bursts, trigger unknown |
| `A9` 16 B | `00` → unit index | on power-off |
| `E9` | `00` → `FF` | per page, every 20 s to 10 min, see [`E9`](#e9-outdoorsystem-data) |
| `AD` | unit index → `00` | on events (thermo on/off) |
| `FD` | iTM → `FF` | the most frequent frame; often just before an `F5` burst *(hyp: the iTM requests status)* |
| `E5` | iTM → `FF` | every 5 min |
| `A5` | controller → group master | on a command |

Burst details:

- `F5` bursts: the 15 units of DIII group 1, 26 ms apart, in bit-reversed
  address order (00, 08, 04, 0C, 02, 06, 0E, 01, 09, 05, 0D, 03, 0B, 07, 0F). Then
  `11` and `10` separately. The timing points to one sender (the outdoor unit as bus
  master?) *(hyp)*.
- `B1` bursts: every frame gets a `06` ACK after 2 ms. The order comes in pairs:
  11, 01, 11, 02, 09, 0A, 05, 06, 0D, 0E, 03, 04, 0B, 0C, 07, 08, …
- `A9 00 → nn` and `A5` frames get an ACK as well.

Response times seen:

- The iTM puts a command on the bus 0.5–2.5 s after it is given. Sometimes it sends
  the same frame twice (retry?).
- The iTM picks up a change made by another controller in about 34 s (its poll cycle).
- The iTM's clock can drift: a schedule set for 06:30 ran at 06:23:28. The
  outdoor unit's clock is in `E9` page `E6`.

## Frame reference

### `F5`: unit status

Broadcast by each unit (`F5 aa FF 1C len`, `aa` = DIII address).

| length | `len` | sent by | content |
|--------|-------|---------|---------|
| 23 B | `11` | indoor units | periodic status |
| 31 B | `19` | indoor units | status on change; its two variants match the iTM's setback/power state for the unit |
| 22 B | `10` | hydrobox | periodic status |
| 37 B | `1F` | hydrobox | second status frame, meaning unknown |
| 10 B / 8 B | `04` / `02` | indoor units / hydrobox | page `91`, hourly |
| 27 B / 33 B | `15` / `1B` | indoor units / hydrobox | page `90`, at iTM start-up |

#### `F5` 23 B: indoor status

```
F5 00 FF 1C 11 | 82 61 01 00 00 00 00 00 00 13 1A 37 00 00 80 02 00 | 0A
               |  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 | cs
```

| byte | meaning | confidence / evidence | code |
|------|---------|-----------------------|------|
| 5 | `0x82` | *(hyp: on/off + flags)* | |
| 6 | operating mode, see [mode codes](#mode-codes) | confirmed with the iTM | `f5_status::MODE` |
| 7 | `01` heat, `02` cool | seen in heat and cool | |
| 9 | `05` while heating, `00` otherwise (also `00` in cool) | | |
| 11 | [status bits](#status-bits) | confirmed | `f5_status::STATUS` |
| 14–15 | room (return-air) temperature, 8.8 | within ±0.2 °C of the iTM for all units (the iTM rounds down to 0.1) | `f5_status::ROOM` |
| 16 | high nibble = fan speed, low nibble = airflow direction, see [fan and airflow codes](#fan-and-airflow-codes) | confirmed with iTM commands | `f5_status::FAN_AIRFLOW` |
| 19 | `80` normally; `81` for a few minutes on 2-00 and 2-01 only: during the low-capacity step of `E9` page `6A` sub-state `07` (16 and 18/11), and on 17/11 also 14 times without it | *(hyp: a step on the second line, 2-xx)* | |
| 20 | `02` while the unit is off; `0x30–0xA5` while it runs, changing about once a minute. Not a counter | *(hyp: expansion valve opening or capacity request)* | |

- The room temperature changes about every 30 s.
- The airflow direction is only in this frame. After a command it shows up in
  the unit's next 23 B frame, so up to ~60 s later.
- Example values on the test system (byte 16 low nibble): swing on 1-00..02,
  1-08/09/11 and 1-13..15; position 4 on 1-03..06; position 0 on 1-07, 1-12 and 2-01.
  Units that always report `0` might have no adjustable flaps (ducted) *(open)*.

#### `F5` 31 B: indoor status on change

Example, unit on in cool, setpoint 22:

```
F5 aa FF 1C 19 | 80 01 62 02 00 00 00 00 16 00 00 00 00 12 05 00 04 02 04 00 80 … | cs
```

| byte | meaning | confidence | code |
|------|---------|------------|------|
| 6 | power: `00` off, `01` on | confirmed | `f5_change::POWER` |
| 7 | operating mode, see [mode codes](#mode-codes) | confirmed | `f5_change::MODE` |
| 8 | `00` fan, `01` heat, `02` cool (same as `A5` payload[3]) | *(hyp for fan)* | |
| 13 | setpoint, whole °C | confirmed | `f5_change::SETPOINT` |
| 17 | `00` → `02` on power-on | | |
| 18 | `0x12` group master, `0x10` member. Constant per role, so a role flag | confirmed | `f5_change::ROLE` |
| 19 | [status bits](#status-bits) | confirmed | `f5_change::STATUS` |
| 22 | `02` master, `12` member | | |
| 23 | `00` → `04` on power-on | | |
| 27 | `0` master, `1` member | | |

#### `F5` 22 B: hydrobox status

The hydrobox sends no 31 B frame, so its power state comes from status bit `0x01`.

| byte | meaning | confidence | code |
|------|---------|------------|------|
| 9 | `00` without heat demand; counts `01` → `0F` and back while it has demand, about one step per minute | *(hyp: requested capacity step)* | `f5_hydrobox::DEMAND_STEP` |
| 11 | [status bits](#status-bits) | confirmed during a start-up: `01` → `21` → `23` → `03` → `01` | `f5_hydrobox::STATUS` |
| 14–15 | hydrobox temperature (water?), 8.8 | 23.66 against 23.7 on the iTM | `f5_hydrobox::TEMPERATURE` |
| 19 | `80` normally, `81` as on the indoor units 2-xx (byte 19 of the 23 B frame) | | |

The 37 B hydrobox frame changes between off and heating. Byte 6 is power (`00`/`01`),
within 2 s of a command. Byte 7 has always been `01` *(hyp: the mode, 1 = heat)*.
Byte 13 has always been `16` (22) *(hyp: a setpoint in whole °C, at the same offset as in
the indoor units' 31 B frame)*. The decoder doesn't read this frame yet.

#### `F5` page `91` (hourly)

`F5 aa FF 1C 04 | 91 BD 00 03` (10 B) from every indoor unit, `F5 10 FF 1C 02 | 91 00`
(8 B) from the hydrobox. It comes as a burst of all units in the usual bit-reversed
order, right after the iTM's `E5 … 50 91`. The content is the same for all indoor
units and did not change over hours *(hyp: unit type/capacity class or static
configuration)*.

#### `F5` page `90` (iTM start-up)

`F5 aa FF 1C 15 | 90 2F 01 20 10 20 10 02 69 02 1C/24 42/40 0D 52 55 16 01 6D 67 C0 18`
(27 B) from every indoor unit, and `F5 10 FF 1C 1B | 90 20 01 …` (33 B) from the
hydrobox. Contains the system ID `6D67C018` *(hyp: unit model/configuration info,
requested by the iTM at start-up)*.

#### Status bits

`F5` 31 B byte 19, `F5` 23 B byte 11 and hydrobox `F5` 22 B byte 11:

| bit | meaning | confidence |
|-----|---------|------------|
| `0x01` | unit on | confirmed |
| `0x02` | outdoor unit / compressor running. System-wide: it is set on units that are off too | confirmed |
| `0x04` | indoor fan running | confirmed |
| `0x20` | heat demand (thermo on) | confirmed |

Common values:

| value | situation |
|-------|-----------|
| `00` | off |
| `02` | off, outdoor unit running |
| `01` | on, no demand, outdoor unit idle |
| `05` | on, cool, no compressor (also fan only) |
| `03` / `07` | on, fan only |
| `21` | on, heat, just started |
| `23` | on, heat, outdoor running |
| `25` / `27` | on, heat, fan running |

Bit `0x02` matches P1P2 `System_Status` bit 1 (130 = `0x82`; 128 without it). It
stays set on every unit while the compressor runs on after the last demand, so it
can stay set long after a unit was switched off. It is not a leftover setting.

Heat demand cycles by itself: a heating unit drops and picks up `0x20` every
4–5 minutes (thermo on/off).

### `B1`: per-unit data to the outdoor unit

`B1 nn 00 08 len`, `nn` = [B1 index](#b1-index--diii-address), lengths 34, 46, 28
and 33 B. Every frame is ACKed. It contains several temperatures
(for example `1694 1354 12A4`) and the system ID.

The 46-byte frames from indoor units:

| byte | meaning | confidence / evidence | code |
|------|---------|-----------------------|------|
| 11–12 | unknown temperature, 21–24 °C | *(open)* | |
| 13–14 | pipe temperature A, 8.8 | *(hyp: liquid)*; 19 → 27–41 °C while heating | `b1_unit::PIPE_A` |
| 15–16 | pipe temperature B, 8.8 | *(hyp: gas)*; 19 → ~42 °C while heating, ≈ Tc | `b1_unit::PIPE_B` |
| 17–18 | setpoint: byte 17 whole °C, byte 18 flags (`16 00` = 22) | follows every setpoint command | `b1_unit::SETPOINT` |
| 30–31 | remote-controller sensor, 8.8 | for 1-04 equal to P1P2 `Temperature_Room`; units without a remote show a fixed `0x1610` (22.06) | `b1_unit::REMOTE` |
| 43–44 | return air, 8.8 | equal to the `F5` room temperature | `b1_unit::INTAKE` |

The 34-byte frames from indoor units:

- byte 12: `00` → `05` when the unit is switched on.
- The mode shows up as `…0A A0 01 00 00 58 03…` in cool and `…0A 50 01/05 00 00 48 03…`
  in heat (seen on 1-07, `B1` 0A) *(hyp: A0/50 = cool/heat, 58/48 a mode bitfield)*.
- byte 21 varies while the unit runs *(open)*.

28 B and 33 B come from the hydrobox only (index `0x11` on the test system).
33 B byte 23 changes when the hydrobox starts.

### `E9`: outdoor/system data

Broadcast by the outdoor unit: `E9 00 FF b3 len | page 6D 67 C0 18 unit … | cs`.

- payload[0] = page id. payload[1..4] = `6D 67 C0 18`, the system ID.
- payload[5] = `01` (outdoor system 1), or `20` on page `6E`.
- On page `E0` (41 B), byte 11 is a sub-page (`e9_page_e0::SUB_PAGE`): `02`/`03` = status of module 1/2,
  `12`/`13` = temperatures of module 1/2. A REYQ10T is, as far as known, a single
  casing, so "module 1/2" are probably not two outdoor units *(open: compressors or
  circuits?)*. Other systems report only module 1: the RXQ20 in issue #78 sends `E0`
  sub-pages `02` and `12` only, and the ERQ in discussion #293 one sub-page of each
  kind. So module 2 is something this outdoor unit has and those don't; the number of
  compressors per model would tell whether it is a compressor.

Field meanings come from correlating each byte with the compressor state and with
Tc and outside air over logs that include about 35 min of compressor running
(`tools/analyse_e9.py`). "r" is that correlation.

| page | len | b3 | interval | byte | meaning | evidence / confidence | code |
|------|-----|----|----------|------|---------|-----------------------|------|
| `65` | 46 | 10 | 20 s | 11 | compressor: `00` off, `01` starting/stopping, `02` running | coincides with heat demand | `e9_page_65::COMPRESSOR` |
| `65` | | | | 14 | `04` while running | | |
| `65` | | | | 15–16 (8.8, sign-mag) | Te, evaporating temperature (low pressure) | ≈ outdoor when idle, drops to −11 °C while heating | `e9_page_65::T_EVAP` |
| `65` | | | | 17–18 (8.8) | Tc, condensing temperature (high pressure) | ≈ outdoor when idle, up to 42 °C; = indoor gas pipes | `e9_page_65::T_COND` |
| `65` | | | | 25–26 (16-bit) | compressor speed, total of both modules: 0 idle, 64–756 running; = 3 × (`E0`/02 byte 32 + `E0`/03 byte 32), r = 0.98 | unit unknown, see below | `e9_page_65::COMP_SPEED_TOTAL` |
| `65` | | | | 27 | outside air, whole °C | r = 0.91 | |
| `65` | | | | 36 | `02` idle, 0x47–0xFE running | *(hyp: compressor current in 0.1 A, see below)* | `e9_page_65::CURRENT` |
| `65` | | | | 37 | `26` idle / `29` running | | |
| `67` | 44 | 10 | 20 s | 21–22 (8.8) | outside air | r = 1.00; also correct while the compressor runs; matched a local weather report and a car thermometer within 1 °C | `e9_page_67::OUTDOOR` |
| `67` | | | | 41–42 (8.8) | Tc (copy of `65` byte 17) | r = 1.00 | |
| `E0`/12,13 | 41 | 1C | 60 s | 15 | outside air, whole °C (per module) | r = 0.95 | |
| | | | | 16 | discharge temperature | *(hyp)*; 13 °C idle → 48 °C running, r(Tc) = 0.96 | `e9_page_e0::DISCHARGE` |
| | | | | 18 | 27–42 °C, rises slowly, stays high afterwards | *(hyp: inverter fin / compressor body)* | |
| | | | | 19–21, 25 | heat-exchanger temperatures, drop below ambient in heating | *(hyp)* | |
| | | | | 23–24, 27, 29, 30 | can go negative (sign-mag, e.g. `88` = −8) | *(hyp: suction/Te per module)* | |
| | | | | 28 | Tc, whole °C | r = 1.00 | |
| | | | | 37 | 0 idle, 6–9 running | *(hyp: compressor step)* | `e9_page_e0::COMP_STEP` |
| | | | | 39 | 0 idle, 3–7 running | *(hyp: outdoor fan step)* | `e9_page_e0::FAN_STEP` |
| `E0`/02,03 | 41 | 1C | 60 s | 14–16 | `00 00 00` idle → `01 1F 03` running | | |
| | | | | 20–21 | flags `08 60`/`88 70` idle, `80 F0` running | | |
| | | | | 22, 24 | 0 → mostly 2–15, at most 0x67/0x7E running | correlates poorly with the compressor speed (r = 0.26) | |
| | | | | 26 | `02` while running | | |
| | | | | 32 | compressor speed of the module: 0 off, 20–130 running, continuous | *(hyp)*; ×3 summed over both modules = page `65` bytes 25–26 | `e9_page_e0::COMP_SPEED` |
| | | | | 38 | 0 → 0x4F/0x50/0x59 running | *(hyp: fan rpm step)* | |
| `E0` | 46 | 1C | 60 s | 14–15 | `40 00` idle / `52 02` running | | |
| | | | | 17–18 (8.8) | outside air | r = 1.00 | |
| | | | | 25 | outside air, whole °C | | |
| | | | | 30, 36, 38, 42 | 0/small idle, large running | *(hyp: run counters / capacity)* | |
| `6A` | 46 | 10 | on change | 11–16 | 3× (state, sub-state): `01 00` idle, `02 00` running, `01 09` right after stop, `01 07` during a 9-minute special operation, see below | *(hyp: per module; 09 = residual/oil return, 07 = oil return or another recovery operation)* | `e9_page_6a::SUB_STATE_1`, `e9_page_6a::SUB_STATE_2`, `e9_page_6a::SUB_STATE_3` |
| | | | | 17–19 | `20 10 10` / `20 20 20` | | |
| | | | | 20–23 | `07 D0 14 C8` constant (= 2000, 5320) | | |
| `6E` | 45 | 0C | 30 s | 11 | `00`/`0C` | | |
| | | | | 12–13 | `14 30` / `94 10` | | |
| | | | | 26–36 | 6× `00 80` / `80 00` | *(hyp: flags for 6 zones/groups)* | |
| `68` | 17 | 08 | 5–10 min | 10–15 | `01 01 11 11 11 00` constant | *(hyp: configuration)* | |
| `E6` | 36 | 1C | at hh:m5:04 | 11–12 | clock: hours, minutes | `13 23` = 19:35, `14 05` = 20:05 | `e9_page_e6::HOUR`, `e9_page_e6::MINUTE` |
| | | | | 13–30 | `10 20 00 00 00 12 3A 71 21 00 60 08 58 C0 07 C0 B4 04` constant | | |
| `2F` | 8 | 08/0C/10 | 2 min | 6 | `01` constant. Comes as 3 frames with b3 08, 0C and 10 | *(hyp: heartbeat per device)* | |
| `6D` | 32 | 10 | daily | 10–11 | `01 81`, bytes 12–30 all `00` | see below | |

The compressor speed and current fields come from a finding on an ERQ outdoor unit
([P1P2MQTT discussion #293](https://github.com/Arnold-n/P1P2MQTT/discussions/293)): on its 43-byte page `65`,
byte 26 is the compressor frequency in Hz, matching the steps in the ERQ service manual, and
byte 36 / 10 is the current of one phase. On the test system (two modules, 46-byte page)
byte 26 is the low byte of the 16-bit value in bytes 25–26, which wraps above 255. Over three days
of logs it equals 3 × the sum of the per-module `E0` byte 32 (r = 0.98, 2,340 frame pairs);
when one module is off, the total is 3 × the other. Byte 32 runs from 20 to 130 without
steps, so the unit (Hz, rps or a percentage) and the factor 3 are open. Byte 36 is `02`
idle on both systems, as 0.2 A would be, but on the test system it follows the speed only
loosely (r = 0.55): 11.5 A with both modules at their highest speed, and up to 25.4 A at other times.

Page `6D` comes once a day, always the same frame:
`E9 00 FF 10 1A | 6D 6D 67 C0 18 01 81 00 … 00 | 3C`. After the system ID come `01`
(outdoor system 1, as in the other pages), `81` and 19 zero bytes. On the test system it
came at 11:40:54, 11:41:03 and 11:41:13 on three days in a row, about 10 s later each
day *(hyp: a 24-hour timer in the outdoor unit, counted from its own start, that runs
slightly slow)*. The zero payload suggests an event or status page with nothing to
report *(hyp)*.

Sub-state `07` on modules 1 and 2 (`01 07 01 07 02 00`) is a recurring operation of
about 9 minutes, seen 6 times in three days at 7–9 °C outside, sometimes 8 h 15 min
apart. It was first taken for a defrost; a closer trace of two of them (2026-11-18)
makes that unlikely:

- For about 4 minutes page `65` byte 36 (capacity) climbs from about 125 to 245 with
  Te at +1…+6 °C.
- Then capacity drops to near 0 within 20 s, Te dips to −3…−5 °C and Tc rises to about
  44 °C for 2–3 minutes. During this step `F5` byte 19 was `81` on 2-00 and 2-01 only
  (that flag also comes without sub-state `07`).
- The rest runs at capacity 110–135 with Te around 0…+2 °C, followed by sub-state `09`
  on all three modules for a minute or two.
- The indoor gas pipes stay at 35–51 °C throughout, so the indoor units keep heating:
  no reverse cycle shows on the indoor side, and with Te above 0 °C most of the time
  frost on the outdoor coil is unlikely.
- After the morning ones on 16/11 and 18/11 the gas pipe of 2-01 (on 16/11 also 1-04)
  went cold (10–25 °C) for several minutes while the others stayed hot; not after the
  afternoon one on 18/11, when the compressor stopped. The others weren't traced.

*(hyp: an oil-return or recovery operation; Daikin VRV systems run oil return at
intervals.)* The component's binary sensor is called *Defrost or oil return* for this
reason.

### `A5`: setting command

`A5 <src> <group master> 14 len | payload | cs`, ACKed with `06`. Lengths 12 B
(fan/airflow, payload[0] `41`), 26 B (operation, payload[0] `40`) and 35 B
(operation to the hydrobox, 29 B payload). See [Controlling units](#controlling-units).

### `A4` and `BD`: iTM start-up

Only seen when the iTM starts up. The iTM then sends, per unit:

- members: `BD 0A aa 14 14 | 40 00 …` (26 B);
- masters: `A5 0A aa 14 14 | 40 00 … 80`, an empty operation command.

Then per master:

- `BD 0A aa 14 06 | 50 91 00 FF 00 00` (12 B);
- `A4 0A aa 14 14 | 40 08 … 80` (26 B);
- `A5 … | 40 00 … 00 00 02`, with payload[13] = `02` and payload[10] = `00`.

### `A9`: outdoor unit to each unit

`A9 00 nn …`, `nn` = [B1 index](#b1-index--diii-address), ACKed.

| length | content |
|--------|---------|
| 24 B (`A9 00 nn 10 12`) | nearly identical for every unit; byte 18 = `1B`/`09`/`1A` |
| 16 B | sent on power-off; byte 13 `07` → `03`. Seen for nn = 08, 09, 0B, 0C when group 1-04 was switched off |
| 46 B (`A9 00 nn 10 28`) | page `6A`, the per-module outdoor state of `E9` page `6A`, sent to each unit in bursts. Not on every change of outdoor state: on 2026-11-18 page `6A` changed 15 times and there was one burst, right after all groups were switched off. The trigger is unknown |

46 B layout: `A9 00 dd 10 28 | 6A 6D 67 C0 18 01 02 00 02 00 02 00 SS 07 D0 14 C8 …`.
SS (payload[13]) steps `10` → `11` → `12` → `20`: `20` = standby/stopped for that
unit (most units), `1x` = an active state step for the units concerned *(hyp)*.

### `AD`: unit to the outdoor unit

`AD nn 00 08 14 | 6C 6D 67 C0 18 06 03 00 XX 00 00 YY YY ZZ …` (26 B), from a unit
(`nn` = 01..10, B1/A9 numbering) to the outdoor unit, page `6C`, on events.

- XX = `82` at rest (all units at the same moment, or a group after it stops).
- On demand (group switched on): XX = `02`/`03`, ZZ (byte 19) = `0E`.
- On stop: XX = `02` and bytes 16–17 = `03 E8` (= 1000).

*(hyp: thermo/capacity request from the unit to the outdoor unit; bit `0x80` = idle;
`03E8` = 1000 a target of some kind)*.

### `FD`: iTM broadcast

`FD 0A FF b3 len`, 11 or 12 B, the most frequent frame on the bus.

- 12 B with b3 `14`: byte 8 cycles through `01, 02, 04, 08, 40`, one step per
  ~11 s *(hyp: poll phase)*.
- 12 B with b3 `08`: byte 8 = `06, 16, …, 56`.
- 11 B: byte 8 = `00`/`01`.

### `E5`: iTM poll

`E5 0A FF 14 06 | 50 80 00 FF 00 00` every 5 min from the iTM *(hyp: iTM
heartbeat/status)*. Once an hour, at hh:10:40, payload[1] is `91` (normally `80`), and
every unit answers with its `F5` page `91` frame. So `E5` is probably a poll request
with the page number in payload[1] *(hyp)*.

## Controlling units

### Command overview

Commands go to the group master and apply to the whole group.

```
A5 <src> <grp> 14 06 | 41 16 FS b8 b9 b10 | cs      fan, airflow, outlet pairs
      FS high nibble: 0x80 | speed<<4  (0 auto, 1 low, 3 medium, 5 high), 0 = no change
      FS low nibble:  0x08 | direction (0..4 position, 7 swing), 0 = no change
      b8 b9 b10:      outlet pairs, 00 00 00 = no change (see "Outlet pairs")

A5 <src> <grp> 14 14 | 40 PP MM XX 00 00 SS VV 00 00 80 00×9 | cs      operation
      PP: 00 = no change, 02 = off, 03 = on
      MM: 00 = no change, 0x10 | mode
      XX: comes with MM: 00 fan / 01 heat / 02 cool / 02 auto / 06 setpoint only
      SS: setpoint in whole °C, 00 = no change
      VV: 80 when SS is valid
```

Each command is answered with an `06` ACK within 2 ms.

### `40` operation command

| payload idx | meaning | confidence | code |
|-------------|---------|------------|------|
| 0 | `40` = operation command | confirmed | |
| 1 | power: `00` no change, `02` off, `03` on (bit 1 = valid, bit 0 = on) | confirmed | `a5_operation::POWER` |
| 2 | mode: `00` no change, `0x10 \| mode`, see [mode codes](#mode-codes) | confirmed for fan, heat, cool | `a5_operation::MODE` |
| 3 | sent with the mode: `00` fan, `01` heat, `02` cool and auto, `06` setpoint only | *(hyp: which setpoint applies, heating or cooling; P1P2 also has separate `Setpoint_Heating`/`Setpoint_Cooling`)* | `a5_operation::MODE_DETAIL` |
| 6 | setpoint, whole °C (`00` = no change) | confirmed | `a5_operation::SETPOINT` |
| 7 | `80` when byte 6 is set | a valid flag: it is `80` for 20, 22 and 24 alike. Half degrees are untested. | `a5_operation::SETPOINT_VALID` |
| 10 | `80` in every command from the iTM and from us | *(hyp: "source = central")*; `00` in one command during iTM start-up | `a5_operation::SOURCE_FLAG` |
| 13 | `00`; `02` in one command during iTM start-up | | |

Examples:

| action | payload |
|--------|---------|
| on | `40 03 00 00 00 00 00 00 00 80 00…` |
| off | `40 02 00 00 00 00 00 00 00 80 00…` |
| mode fan only | `40 00 10 00 00 00 00 00 00 80 00…` |
| mode heat | `40 00 11 01 00 00 00 00 00 80 00…` |
| mode cool | `40 00 12 02 00 00 00 00 00 80 00…` |
| setpoint 24 | `40 00 00 00 00 00 18 80 00 00 80 00 …` |
| iTM schedule: auto, 23 °C | `40 00 13 02 00 00 17 80 …` |
| iTM schedule: setpoint 21 only | `40 00 16 06 00 00 15 80 …` |

- The iTM sends a mode change and power-on as two commands (mode first, then
  `40 03`). Its schedule sends three per group master: mode + setpoint, fan
  (`41 16 B0`), and on (`40 03`).
- The hydrobox gets the same `40` command with a longer payload:
  `A5 0A 10 14 1D | 40 …` (35 B), for example `40 00 11 01 …` (heat), `40 03` (on) and
  `40 02` (off). The first 11 payload bytes are laid out as above; the other 9 are `00`.
  Sent from a third-party source address (`0x30`), heat, on and off are ACKed and the
  hydrobox follows within 2 s. Cool (`40 00 12 02`) has not been tried on it.
- An iTM schedule event with mode and setpoint ticked sent the hydrobox the setpoint
  bytes too: `40 00 11 01 00 00 16 80 …` (heat, 22 °C), ACKed. The hydrobox's 37 B
  status showed `16` at byte 13 before and after, so it's not known whether it took the
  value or what it means for a hydrobox. The leaving-water setpoint has not been seen in
  any command yet.

### `41` fan, airflow and outlet command

Payload `41 16 FS b8 b9 b10`. Byte 1 is always `16` *(hyp: sub-index/flags)*.
FS sets fan speed and airflow direction; either half can be left at `0` for no
change, so one frame can set both. Fan and airflow codes are below.

Examples: fan high `41 16 D0 00 00 00`, fan medium `41 16 B0 00 00 00`, airflow
position 0 `41 16 08 00 00 00`, position 4 `41 16 0C`, swing `41 16 0F`.

A fan change while the unit is off is accepted, but shows no visible status change.

### Mode codes

| mode | `A5` byte 2 | `F5` status | P1P2 |
|------|-------------|-------------|------|
| fan | `10` | `60` | 96 |
| heat | `11` | `61` | 97 |
| cool | `12` | `62` | 98 |
| auto | `13` (iTM mode *Auto*; units then report the mode they run in, e.g. `61`) | `63` *(hyp)* | 99 |
| dry | `17` *(hyp)* | `67` *(hyp)* | 103 |
| setpoint only | `16` (iTM action *Setpoint*; the unit keeps its mode) | | |

Auto and dry were not sent in our own tests.

### Fan and airflow codes

| fan | speed | `41` FS high nibble | `F5` 23 B byte 16 high nibble | P1P2 |
|-----|-------|---------------------|-------------------------------|------|
| auto | 0 | `8` | `0` | |
| low | 1 | `9` | `1` | `0x11` |
| medium | 3 | `B` | `3` | `0x31` |
| high | 5 | `D` | `5` | `0x51` |

| airflow | `41` FS low nibble | `F5` 23 B byte 16 low nibble |
|---------|--------------------|------------------------------|
| position 0 (horizontal) | `8` | `0` |
| positions 1–3 | `9`–`B` | `1`–`3` |
| position 4 (down) | `C` | `4` |
| swing | `F` | `7` |

The iTM lists swing, then positions 0..4.

### What the units send back

| change | where it shows | how fast |
|--------|----------------|----------|
| power, mode, setpoint | `F5` 31 B of every unit in the group | 1–5 s |
| power, mode, status bits | `F5` 23 B | next periodic frame (up to ~60 s) |
| fan speed, airflow direction | `F5` 23 B byte 16 only | next periodic frame (up to ~60 s) |
| setpoint | `B1` 46 B bytes 17–18 | next `B1` burst |
| power-on | `B1` 34 B byte 12 `00` → `05` of each member | next `B1` burst |
| power-off | `A9` 16 B frames | at once |
| outlet pairs | not on F1F2; visible on the unit's P1/P2 bus only | ~5 s |

### Outlet pairs

The three trailing bytes of the `41` command (frame bytes 8–10, `00 00 00` in every
iTM command) set the per-outlet flap setting of cassette units, the same setting
the wired remote writes as "individual flap". The unit applies it within ~5 s and
the remote adopts the value. It is only visible on the P1/P2 bus
([packet `0x15`](#p1p2-packet-0x15-individual-flap-setting)).

Model that fits all 14 test commands:

```
A5 <src> <group> 14 06 | 41 16 00 b8 b9 b10
  b8 high nibble: F = valid (0 or A: the command is ignored)
  b8 low nibble:  value. F -> P1P2 0 (hyp: a fixed flap position), 4 -> P1P2 F
                  (no setting). A and E: no visible effect
  b9 high nibble: which outlets. A -> outlets 1+3, F -> outlets 2+4.
                  4, 8, 5: ignored (no single outlet found)
  b9 low nibble:  with value F (fixed 0) it must be F: FF AF / FF FF work, FF A0 is
                  ignored. With value 4 (normal) 0 works (F4 A0, F4 F0)
  b10:            no effect seen

Working frames:
  outlets 1+3: normal F4 A0 00, fixed FF AF 00
  outlets 2+4: normal F4 F0 00, fixed FF FF 00
```

Tested combinations (P1P2 state of unit 0 as mask / b7 b8, or as the value per outlet
1 2 3 4):

| bytes 8–10 | before | after |
|------------|--------|-------|
| `00 00 00` | `04 / FA FF` | unchanged |
| `FF AF 00` | `04 / FA FF` | `0E / 0A 0F` (outlets 1 and 3 → `0`) |
| `F4 FA FF` | `0E / 0A 0F` | `0A / 0F 0F` (outlet 2 → `F`) |
| `FE AF FF` | `0A / 0F 0F` | unchanged |
| `FF FF 44` | `0E / 0A 0F` | `0F / 00 00` (all four → `0`) |
| `A0 00 00` | 0 0 0 0 | unchanged |
| `0A 00 00` | 0 0 0 0 | unchanged |
| `F4 A0 00` | 0 0 0 0 | F 0 F 0 |
| `F4 A0 FF` | F 0 F 0 | unchanged |
| `F4 F0 00` | F 0 F 0 | F F F F |
| `FF 40 00` | F F F F | unchanged |
| `FF 80 00` | F F F F | unchanged |
| `FF 50 00` | F F F F | unchanged |
| `FA A0 00` | F F F F | unchanged |
| `FF AF 00` (outlets 1+3 fixed) | `04 / FA FF` | `FE / 0A 0F`; `F4 A0 00` gave `04 / FA FF` again |

Limits:

- F1F2 sets opposite outlets in pairs, to normal or to value `0`. Blocking a
  single outlet (P1P2 `A`), which the remote can do, was not reached; the value for
  it (if any) is unknown.
- Value `0` cannot be cleared from the remote directly (it shows as "no setting",
  and selecting that again sends nothing). `F4 A0 00` + `F4 F0 00` clear it.
- There is no read-back on F1F2, so a controller can only write it.
- Sent to a member unit, the command is ACKed but nothing visible happens. That test
  used the ignored `FF A0 00`, and P1P2 only shows unit 0 of a group, so whether
  members apply it is unknown.
- No error frames on either bus during the tests, and the airflow byte did not change.

### Write budget

Every setting change reaches the indoor unit's non-volatile memory (EEPROM),
which has a limited number of write cycles. What Daikin says:

- Modbus Interface DIII design guide (EKMBDXA7V1, 4P357732-1, 2014-04),
  §1.2 *Limitations*: "The number of control commands per indoor unit is
  limited to 7000 per year." An automatic control program must stay below it.
  This is the interface for exactly this bus (DIII-NET / F1F2).
- iTM BACnet Server Gateway design guide (DCM014A51), part 4 §3.1
  *Indoor Unit EEPROM*: every change is written to EEPROM. A data item takes up to
  1,000,000 writes, depending on the model. Keep changes to 70,000–80,000 per
  year; the rule of thumb is below 200 per day per data item. The same value sent
  again is not counted. Counted items: setpoint, start/stop, mode, airflow
  direction, fan speed. Past the limit the unit keeps working, but it no longer
  remembers its settings after a power failure.
- The BACnet interface guide (DMS502B71) also names 7000 per year for
  automatic control (seen in search results only).

What this means on the bus: each `A5 … 40` (operation) or `A5 … 41` (fan)
command to a group master changes stored settings in every unit of that
group. The iTM sends one frame per change (no repeats seen in our captures),
and a controller's own writes come back as `R` frames, so counting `A5 40/41`
frames per destination counts all setting commands from every source.

The component follows the strictest figure, 7000 a year. It skips unchanged
settings and gives each group a token bucket for its own commands: 30 at once, one
back every 365 d / 7000 = 75 min, kept in RTC memory. A per-group *Setting
commands* counter shows the commands from every source.

### Running next to an iTM

- The iTM follows changes made by another controller, within its ~34 s poll cycle.
- The iTM keeps its own state per unit. After another controller switched a unit
  on and off, the iTM showed it as `"SB Laag"` (setback low), and that did not clear
  by itself. Switching the unit on and off once from the iTM cleared it. The iTM may
  later apply its setback control (minimum temperature) to such a unit. A controller
  next to an iTM must either leave power switching to the iTM or accept this.
- The iTM enforces its own setpoint against the P1/P2 remote line: a setpoint written
  on P1/P2 was back at the old value ~6 s later, and no F1F2 frame was sent. So
  writing over P1/P2 is not useful while an iTM controls the units; send `A5`
  commands on F1F2 instead.
- The iTM's schedules keep running and send the same `A5` commands.
- A schedule *stop* event sends more than off when mode, fan or setpoint are ticked in
  it: per group master the mode with setpoint (`40 00 11 01 00 00 16 80`), the fan
  (`41 16 B0`) and then off (`40 02`). That overwrites the setpoint another controller
  set. Untick them to get a plain `40 02`.
- The iTM runs its schedule on its own clock, which can drift (its time settings have
  no sync option): one
  ran 6.5 minutes fast. The outdoor unit's clock (`E9` page `E6`) was just as fast
  *(hyp: the iTM sets it)*, so it shows the drift.

### What can go wrong

- Source `0x0A` causes iTM error MC (duplicate address). See
  [source addresses](#source-addresses-for-a-third-party-controller).
- Back-to-back writes are refused by the ATmega's one-frame buffer, and a write
  can collide with other traffic. Both are reported on a `*` line; send again.
- In heat the compressor starts 1.5–2 min after a unit is
  switched on. After the last demand it keeps running 13–17 min with indoor units
  off *(hyp: minimum run time / anti-short-cycle / oil return)*, or 3–5 min after
  hydrobox-only demand, with state `01` (stopping) for ~2 min before `00`. So a short
  test in heat costs up to 15+ min of compressor time. In cool, 60 s was not enough
  to start the compressor. A hydrobox switched on cycles by itself (demand ~20–30 min,
  pause ~10–15 min).

## P1/P2 packet `0x15`: individual flap setting

The per-outlet flap setting ("individual flap" on cassette remotes) travels on the
P1/P2 bus between a wired remote and its group, in packet type `0x15`. P1P2Monitor
shows it in raw output mode `J3`. The remote sends `00 00 15` to the unit about
every 5 s, and the unit echoes it as `40 00 15`.

```
00 00 15 10 20 | b5 F0 b7 b8 FF 00 F0 … | cs
b7 high nibble = outlet 1, b7 low = outlet 2, b8 high = outlet 3, b8 low = outlet 4
      F = no setting (normal), A = blocked (closed)
      0 = seen on outlet 1 (hyp: a fixed position; 0-4 like the airflow direction)
b5 low nibble  = outlets with a setting other than F: 0x08 outlet 1, 0x04 outlet 2,
                 0x02 outlet 3, 0x01 outlet 4
b5 high nibble = one-shot "changed" flags in the same order (0x80 outlet 1 … 0x10
                 outlet 4), set in one packet and cleared in the next
unit echo: 40 00 15 11 20 | F0|mask F2 b7 b8 FF 00 …
```

Examples for unit 0:

| state | b5 (changed → settled) | b7 b8 |
|-------|------------------------|-------|
| outlet 2 blocked | `04` | `FA FF` |
| outlet 1 set to `0` (with outlet 2 blocked) | `8C` → `0C` | `0A FF` |
| outlet 1 blocked | `88` → `08` | `AF FF` |
| all released | `80` → `00` | `FF FF` |
| outlet 3 blocked | `22` → `02` | `FF AF` |
| outlet 4 blocked | `11` → `01` | `FF FA` |

- Only one outlet per unit can be blocked; the remote releases the other first.
- Bytes 10, 14 and 18 (`F0`) and the zero bytes after them are probably the same
  block for units 1–3 of the group *(hyp)*. On the test system they stay
  `00 F0 00 00`: P1P2 only shows unit 0.
- A change on the remote does not appear on F1F2: no command, and no status byte
  changes. F1F2 can write the setting (see [Outlet pairs](#outlet-pairs)) but not read it.
- Stock P1P2MQTT (F-series model 10) cannot write it: it only writes packet `0x38`
  from its own auxiliary-controller slot (`F0`).

## Open questions

- Malfunction/error codes: not yet found on the bus.
- Auto and dry: the `F5` codes `63`/`67` and the `A5` code `17` for dry are not
  confirmed. Cooling with the compressor actually running is not examined.
- Other iTM functions (remote-controller prohibition, setback, filter reset) and
  their frames.
- `b3` in the header: fixed per frame type on three systems; whether it names the sender.
- What the system ID is derived from.
- `F5` 23 B byte 5 (`0x82`), byte 20 (expansion valve or capacity request?), and
  whether units that report airflow `0` have fixed flaps.
- `F5` 31 B byte 8 for fan, bytes 17 and 23.
- The hydrobox: the rest of its 37 B frame, whether byte 9 is a capacity step, and
  how the leaving-water setpoint is commanded.
- `B1` 46 B bytes 11–12 (a temperature, 21–24 °C), which pipe is A and B, `B1` 34 B
  byte 21 and the mode bytes.
- The B1 pair 02/05 (1-08/1-00), and why `AD` sources do not match the B1 mapping.
- `A9` 24 B, `A9` 46 B SS, `AD` XX/YY/ZZ.
- `FD` and `E5` in general.
- Most `E9` fields marked *(hyp)* above, and what "module 1/2" are on a single-casing
  outdoor unit (the RXQ20 and ERQ report module 1 only).
- `E9` page `6D`, daily: what it reports.
- `E9` page `6A` sub-state `07` (oil return?) and `F5` byte 19 `81` on the 2-xx units.
- What triggers the `A9` 46 B bursts.
- `A5` payload[3] (which setpoint applies?), payload[10] and payload[13].
- `A4` and `BD` frames.
- Outlet pairs: the value for blocking a single outlet, whether member units apply
  the setting, and what P1P2 value `0` means.
- Whether half-degree setpoints exist on the bus.
