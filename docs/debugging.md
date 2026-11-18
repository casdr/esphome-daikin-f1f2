# Debugging and new systems

This component was worked out on one Daikin VRV system. If yours is different, a bus log
is what's needed to support it, and you don't need MQTT for one: the bridge can write
every frame to its ESPHome log. I'm happy to help add support for other models; send a
log and a survey in a [New system report](https://github.com/casdr/esphome-daikin-f1f2/issues/new?template=new_system.yml).

## Making a bus log

The *Frame logging* switch comes with `packages/bridge.yaml`. It's off after every restart,
so it can't fill your logs by accident. While it's on, every bus frame becomes one log line
under the tag `daikin_diii.frame`, about 2-3 lines a second:

```
[22:55:39.451][I][daikin_diii.frame:309]: R T  8.289: FD0AFF14065082000100000A
```

`R` lines are bus frames (the gap since the previous frame, then the frame as hex), `X` lines
frames this bridge wrote, `E` lines frames with errors. The logger level must be `INFO` or
lower, which ESPHome's default `DEBUG` is.

1. Turn on **Frame logging**, in Home Assistant or on the bridge's web page.
2. Collect the log in one of these ways:
   - **ESPHome command line**: `esphome logs your-bridge.yaml > bus.log`, and stop it with
     Ctrl+C when you're done.
   - **ESPHome Builder**: the device's log window shows the same lines. Save them to a file.
   - **Home Assistant**: in the bridge's ESPHome options, turn on *Subscribe to logs from the
     device*, and set the ESPHome integration to log at INFO (*Enable debug logging* on
     its page also works). Afterwards download the log from Settings > System > Logs and
     turn both off again.
3. Let it run for 30-60 minutes, ideally while something happens:
   - switch a group on or off with its own remote;
   - change the setpoint on one remote: groups that always have the same settings can't
     be told apart otherwise;
   - write down the time of anything you do or see, such as a fault code.
4. Turn **Frame logging** off.

Then turn the log into a TSV, the format the other tools and [captures/](../captures) use:

```bash
python3 tools/esphome_log_to_tsv.py --date 2026-11-18 bus.log -o bus.tsv
```

`--date` is the date of the first line. Without it the converter takes the date from the
file's modification time, which is wrong for a log you copied or saved later. Lines from
Home Assistant's log carry their own date. Times are read in your computer's time zone,
unless the line has a UTC offset (Home Assistant's own log file has none by default): run
the converter where the log was made, or set `TZ`, for example `TZ=Europe/Amsterdam`. Both tools need only Python 3.10 or newer, no packages.

## The survey

```bash
python3 tools/survey.py bus.tsv > survey.md
```

`survey.py` prints a Markdown report:

- **Units**: every address that sent a status frame, as the controllers number them
  (`1-12` is address `0x0C`), with its role (group master, member, hydrobox), its current
  settings, and two suggestions:
  - **group**: the master whose settings the member always matched. *1-09 or 1-15* means
    those groups had the same settings for the whole log; a setpoint change on one remote
    tells them apart.
  - **B1 index**: the `B1` index whose return-air temperature matches the unit's own, with
    the share of readings that matched. The index is needed for the remote and pipe
    temperatures.
- **Controllers**: the source addresses of setting commands. `0x0A` is a Daikin iTM.
- **Frame types**: every frame type in the log, and whether the component knows it, with an
  example of each unknown one.
- **Packages**: a list for [example.yaml](../example.yaml). Check the groups and B1 indexes
  against the labels on your units or the addresses in your controller before you use it.

## Sharing it

Open a [New system report](https://github.com/casdr/esphome-daikin-f1f2/issues/new?template=new_system.yml),
paste the survey and attach `bus.tsv` (zip it if GitHub refuses the size). The TSV has only
the bus frames and their times; the converter drops every other log line, so nothing from
your Wi-Fi or Home Assistant ends up in it. The survey shows your unit addresses and settings.

## Unknown frame notifications

The bridge knows the frame types listed in [protocol.md](protocol.md). The first time after a
restart that it sees a type that isn't on that list, it logs a warning:

```
[W][daikin_diii]: Unknown frame type F5_25 (first one since boot): F5011C...
```

With **Unknown frame notifications** on, it also creates a notification in Home Assistant
with the frame and a link to report it. For that, Home Assistant must allow the device to
perform actions: Settings > Devices & services > ESPHome > your bridge > *Configure* >
*Allow the device to perform Home Assistant actions*.

A type is reported the first time it's seen and then at most once an hour while it keeps
coming, so a notification missed while Home Assistant was restarting, or while the switch was
off, comes through later. Each type has one notification, which a new report replaces. The
bridge keeps track of 32 unknown types per restart. Known only means the type is described in
protocol.md; several known types aren't decoded yet. When you report an unknown one, say
what the system was doing at that moment.

## Without the packages

If you write the config yourself, add the switch and, if you like, the trigger:

```yaml
daikin_diii:
  # ...
  on_unknown_frame:
    - logger.log:
        format: "New frame type %s: %s"
        args: [frame_type.c_str(), frame.c_str()]

switch:
  - platform: daikin_diii
    frame_logging: {name: Frame logging}
```

`frame` is the frame as hex, checksum included, and `frame_type` the type: command and
length (`F5_25`), and for `E9` also the page and the class byte or sub-page (`E9_41_E0_22`). `packages/bridge.yaml` has the full
notification.
