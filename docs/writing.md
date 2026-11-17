# Controlling units

Writing to the bus makes the bridge a second central controller. This page explains the
guards the component puts around that, and what to expect next to an iTM or wired remotes.

## The write switch

Nothing is written while *Bus writes enabled* is off. With it off, a climate, airflow or
outlet change is refused, logged in *Last bus command* (`refused: writes disabled`) and the
entity springs back to the real state.

In [packages/bridge.yaml](../packages/bridge.yaml) the switch starts off (`RESTORE_DEFAULT_OFF`) and then
keeps its state across restarts. Leave it off until the unit table is right: a wrong group
master sends commands to the wrong units.

## Source address

The bridge writes from address `0x30` (`source_address`). Pick another free address if `0x30`
is taken on your bus; addresses that appear as a source in your bus log are taken.

Never use `0x0A`. It is the iTM's own address, and two senders with it make the iTM raise
error *MC (address duplicated)*. The component refuses it, both in the config and for every
frame.

## Write budget

Every setting command (on/off, mode, setpoint, fan, airflow, outlets) is stored in the
indoor unit's non-volatile memory, which wears out. Daikin limits control commands to about
7000 per indoor unit per year (about 19 a day) for its own gateways; see
[protocol.md](protocol.md#write-budget) for the sources. The component keeps well within
that:

- It only sends what changes. A repeated setpoint or fan speed is skipped. Until the bus
  confirms a command (at most about a minute), the commanded value counts as current, so a
  quick undo is still sent.
- Each group has a budget for commands from this bridge: 30 at once, one more every
  75 minutes (= 7000 a year). Above that, a command is refused
  (`refused: write budget used up`) and the entity springs back. The refill keeps the
  yearly average; the 30 lets normal manual use through without hitting the limit.
- The budget survives restarts and OTA updates (it's kept in RTC memory, so it doesn't wear
  the ESP's flash either). After a power loss it starts full.
- `setting_commands` counts every setting command on the bus for a group, also from the
  iTM and wired remotes. A daily [utility meter](https://www.home-assistant.io/integrations/utility_meter/)
  on it shows how many commands a group gets per day.

Change the budget with `write_budget: {burst: …, per_year: …}`, or turn it off with
`burst: 0`. Keep automations frugal: one that changes a setpoint every 10 minutes uses a
year's budget in two to three months.

## Queue and retries

Writes go out one at a time. A frame only counts as written once the ATmega reads it back
from the bus intact: its echo, an `R` line with exactly the bytes that were sent. Handing a
frame to the ATmega is not enough. On a busy bus the ATmega can refuse it (`Refusing to write
packet while previous packet wasn't finished`, when the frame before it is still waiting for a
gap), take it and never put it on the bus, or put it on the bus damaged. On one installation 7
of 67 writes went that way in two days, among them two hydrobox offs, and the hydrobox stayed
on.

- The next frame waits until the previous one has its echo, and goes out at least 1.5 s after
  it. The echo usually comes within 2.5 s of the send.
- No intact echo within 10 s: the frame is sent again. A refusal or collision reported within
  1.5 s of the send also sends it again straight away.
- After 3 resends it is given up on: *Last bus command* shows `lost <frame>`, the log has an
  error and *Bus writes lost* goes up. A confirmed frame shows as `on bus <frame>`.
- *Bus write queue* shows how many are waiting; turning writes off empties it.

A lost setting usually shows in the entity too: the climate entity falls back to what the unit
reports once the command's pending time (1 minute) has passed. Watch *Bus writes lost* in your
monitoring.

## Next to an iTM or wired remotes

- Every controller sees the others' changes: the bus status follows the last command,
  wherever it came from.
- The iTM marks a unit that another controller switches off as *setback*. Switching it
  on and off once from the iTM clears that.
- The iTM's own schedules keep running. If both the iTM and Home Assistant schedule the same
  units, they will fight; move the schedule to one of them.

## Things to know when testing

- Switching a unit on in heat starts the outdoor compressor after 1.5 to 2 minutes, and it
  keeps running for up to about 15 minutes after the last demand. Use fan only for quick
  tests.
- Restore what you change. The bus has no undo, and the write budget counts every command.
