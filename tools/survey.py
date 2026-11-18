#!/usr/bin/env python3
"""Summarise an F1F2 bus log: the units on the bus, their groups and B1 indexes, the
outdoor-unit pages and the frames this component doesn't know yet.

  survey.py bus.tsv [more.tsv ...] > survey.md

The output is Markdown, for a "New system report" issue, and ends with a packages list
for example.yaml. Groups and B1 indexes are suggestions worked out from the log: check
them against the labels on the units or the addresses in your controller.

Standard library only. Reads the TSV written by f1f2_log.py or esphome_log_to_tsv.py.
"""
from __future__ import annotations

import collections
import datetime as dt
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_unknown import sig  # noqa: E402  (catalogue of known frame signatures)

# ignore a member's status this soon after its master's changed: members report a
# command 1-5 s after their master
GROUP_SETTLE_S = 10.0
# a B1 return-air reading counts for an address when the F5 room temperature is this recent
B1_MATCH_AGE_S = 120.0


def t88(frame: bytes, offset: int) -> float:
    return ((frame[offset] << 8) | frame[offset + 1]) / 256.0


def frame_valid(frame: bytes) -> bool:
    return len(frame) >= 6 and len(frame) == 6 + frame[4] and sum(frame[1:]) & 0xFF == 0


def unit_name(address: int) -> str:
    """DIII address as the controllers show it: 0x0C = 1-12, 0x10 = 2-00."""
    return f"{1 + address // 16}-{address % 16:02d}"


MODES = {0x60: "fan", 0x61: "heat", 0x62: "cool", 0x63: "auto", 0x67: "dry"}


@dataclass
class Unit:
    address: int
    hydrobox: bool = False
    master: bool | None = None
    power: int | None = None
    mode: int | None = None
    setpoint: int | None = None
    room: float | None = None
    room_time: float = 0.0
    state: tuple | None = None  # (power, mode, setpoint) from the last F5 31 B frame
    state_time: float = 0.0  # when that state was first reported
    # master address -> [statuses that matched the master's, statuses compared]
    agreement: dict[int, list[int]] = field(default_factory=dict)


@dataclass
class Survey:
    frames: int = 0
    invalid: int = 0
    notes: collections.Counter = field(default_factory=collections.Counter)
    first: float | None = None
    last: float | None = None
    units: dict[int, Unit] = field(default_factory=dict)
    signatures: collections.Counter = field(default_factory=collections.Counter)
    unknown: dict[tuple, str] = field(default_factory=dict)
    command_sources: collections.Counter = field(default_factory=collections.Counter)
    # B1 index -> address -> [matches, comparisons]
    b1_votes: dict[int, dict[int, list[int]]] = field(default_factory=lambda: collections.defaultdict(dict))

    def unit(self, address: int) -> Unit:
        return self.units.setdefault(address, Unit(address))

    def feed(self, when: float, frame: bytes) -> None:
        self.frames += 1
        self.first = when if self.first is None else min(self.first, when)
        self.last = when if self.last is None else max(self.last, when)
        if len(frame) == 1:
            return  # ACK
        if not frame_valid(frame):
            self.invalid += 1
            return
        key, known = sig(frame)
        self.signatures[key] += 1
        if not known and key not in self.unknown:
            self.unknown[key] = frame.hex().upper()
        cmd, src = frame[0], frame[1]
        if cmd == 0xF5 and len(frame) == 23:
            unit = self.unit(src)
            unit.mode = frame[6]
            unit.room, unit.room_time = t88(frame, 14), when
        elif cmd == 0xF5 and len(frame) == 22:
            unit = self.unit(src)
            unit.hydrobox = True
            unit.power = frame[11] & 0x01
        elif cmd == 0xF5 and len(frame) == 31:
            unit = self.unit(src)
            unit.power = 1 if frame[6] else 0
            if frame[7] & 0xF0 == 0x60:
                unit.mode = frame[7]
            if 10 <= frame[13] <= 35:
                unit.setpoint = frame[13]
            unit.master = bool(frame[18] & 0x02)
            state = (frame[6], frame[7], frame[13])
            if state != unit.state:
                unit.state, unit.state_time = state, when
            if not unit.master:
                self._agree(unit, when)
        elif cmd == 0xB1 and len(frame) == 46:
            self._b1(src, t88(frame, 43), when)
        elif cmd == 0xA5 and len(frame) > 5 and frame[5] in (0x40, 0x41):
            self.command_sources[src] += 1

    def _agree(self, member: Unit, when: float) -> None:
        """Compare a member's status with every master's current one."""
        for master in self.units.values():
            if not master.master or master.state is None or when - master.state_time < GROUP_SETTLE_S:
                continue
            tally = member.agreement.setdefault(master.address, [0, 0])
            tally[1] += 1
            if master.state == member.state:
                tally[0] += 1

    def _b1(self, index: int, intake: float, when: float) -> None:
        votes = self.b1_votes[index]
        for unit in self.units.values():
            if unit.room is None or when - unit.room_time > B1_MATCH_AGE_S:
                continue
            tally = votes.setdefault(unit.address, [0, 0])
            tally[1] += 1
            if abs(unit.room - intake) < 0.05:
                tally[0] += 1

    # --- conclusions -------------------------------------------------------------------

    def b1_index_of(self) -> dict[int, tuple[int, float]]:
        """address -> (B1 index, share of matching readings), best match per index."""
        best: dict[int, tuple[int, float]] = {}
        for index, votes in self.b1_votes.items():
            scored = [(matches / total, address) for address, (matches, total) in votes.items() if total >= 3]
            if not scored:
                continue
            share, address = max(scored)
            if share >= 0.5 and (address not in best or share > best[address][1]):
                best[address] = (index, share)
        return best

    def group_of(self) -> dict[int, list[int]]:
        """member address -> the master(s) whose status it always matched.

        A member reports the same power, mode and setpoint as its master. More than one
        master means the log can't tell them apart (those groups always had the same
        settings); a log with a setting changed on one group's remote settles it.
        """
        groups: dict[int, list[int]] = {}
        for unit in self.units.values():
            if unit.master or unit.hydrobox:
                continue
            shares = {address: matched / total for address, (matched, total) in unit.agreement.items() if total >= 3}
            if not shares:
                continue
            best = max(shares.values())
            if best >= 0.9:
                groups[unit.address] = sorted(a for a, share in shares.items() if share >= best - 0.01)
        return groups

def read(paths: list[str], survey: Survey) -> None:
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                columns = line.rstrip("\n").split("\t")
                if len(columns) < 3:
                    continue
                try:
                    when = float(columns[0])
                except ValueError:
                    continue
                if not columns[2]:
                    note = columns[3] if len(columns) > 3 else ""
                    if note.startswith("# X"):
                        survey.notes["own writes (X)"] += 1
                    elif note.startswith("# E"):
                        survey.notes["error lines (E)"] += 1
                    continue
                try:
                    frame = bytes.fromhex(columns[2])
                except ValueError:
                    survey.invalid += 1
                    continue
                survey.feed(when, frame)


def report(survey: Survey) -> str:
    out: list[str] = []
    span = (survey.last - survey.first) / 60 if survey.first is not None else 0.0
    start = dt.datetime.fromtimestamp(survey.first).strftime("%Y-%m-%d %H:%M") if survey.first else "-"
    out.append("## Bus survey\n")
    out.append(f"Log from {start}, {span:.0f} minutes, {survey.frames} frames, {survey.invalid} with a bad checksum.")
    for name, count in sorted(survey.notes.items()):
        out.append(f"Also {count} {name}.")
    if span < 30:
        out.append("\nThis log is short: groups and B1 indexes need units switching on or off and "
                   "temperatures moving. 30-60 minutes around a start or a setting change works best.")

    groups, b1 = survey.group_of(), survey.b1_index_of()
    out.append("\n### Units\n")
    out.append("| unit | address | role | group (guess) | B1 index (guess) | power | mode | setpoint | room |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for address in sorted(survey.units):
        unit = survey.units[address]
        role = "hydrobox" if unit.hydrobox else {True: "master", False: "member", None: "?"}[unit.master]
        group = " or ".join(unit_name(a) for a in groups[address]) if address in groups else ("own" if unit.master else "?")
        index = f"0x{b1[address][0]:02X} ({b1[address][1]:.0%})" if address in b1 else "?"
        power = {1: "on", 0: "off", None: "?"}[unit.power]
        mode = MODES.get(unit.mode, f"0x{unit.mode:02X}") if unit.mode else "?"
        setpoint = f"{unit.setpoint} °C" if unit.setpoint else "?"
        room = f"{unit.room:.1f} °C" if unit.room is not None else "?"
        out.append(f"| {unit_name(address)} | 0x{address:02X} | {role} | {group} | {index} | {power} | {mode} | {setpoint} | {room} |")

    if survey.command_sources:
        out.append("\n### Controllers\n")
        out.append("Sources of setting commands (`A5` 40/41) on the bus; `0x0A` is a Daikin iTM:\n")
        for source, count in survey.command_sources.most_common():
            out.append(f"- `0x{source:02X}`: {count} commands")

    out.append("\n### Frame types\n")
    out.append("| frame | count | known |")
    out.append("|---|---|---|")
    for key, count in sorted(survey.signatures.items(), key=lambda item: str(item[0])):
        out.append(f"| {describe(key)} | {count} | {'no' if key in survey.unknown else 'yes'} |")
    if survey.unknown:
        out.append("\nOne example of each unknown frame:\n")
        out.append("```")
        for key, example in survey.unknown.items():
            out.append(f"{describe(key)}: {example}")
        out.append("```")

    out.append("\n### Packages for example.yaml\n")
    out.append("Check the groups and B1 indexes first. A group marked *not sure* had the same settings as "
               "another group for the whole log: change the setpoint on one remote while logging to tell them "
               "apart. Add `remote-sensors.yaml` for a group master with a B1 index, and set your own names "
               "and areas.\n")
    out.append("```yaml")
    out.append("      - path: packages/bridge.yaml")
    for address in sorted(survey.units):
        unit = survey.units[address]
        name = f"Daikin {unit_name(address)}"
        index = f', b1_index: "0x{b1[address][0]:02X}"' if address in b1 else ""
        if unit.hydrobox:
            out.append("      - path: packages/hydrobox.yaml")
            out.append(f'        vars: {{id: "{address:02x}", name: {name}, area: area_main}}')
        elif address in groups:
            out.append("      - path: packages/unit.yaml")
            others = ", ".join(f'"{a:02x}"' for a in groups[address][1:])
            ambiguous = f"  # not sure: could also be {others}" if others else ""
            out.append(f'        vars: {{id: "{address:02x}", group: "{groups[address][0]:02x}", name: {name}, '
                       f'area: area_main{index}}}{ambiguous}')
        else:
            out.append("      - path: packages/group-master.yaml")
            out.append(f'        vars: {{id: "{address:02x}", name: {name}, area: area_main{index}}}')
    out.append("```")
    return "\n".join(out) + "\n"


def describe(key: tuple) -> str:
    if key[0] == "E9":
        b3, length, page, sub = key[1]
        return f"E9 page {page:02X}" + (f"/{sub:02X}" if sub is not None else "") + f", {length} B, b3 {b3:02X}"
    return f"{key[0]} {key[1]} B"


def main(argv: list[str] | None = None) -> int:
    paths = (argv if argv is not None else sys.argv[1:])
    if not paths or paths[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if paths else 2
    survey = Survey()
    read(paths, survey)
    if survey.frames == 0:
        print("No frames found in " + ", ".join(paths), file=sys.stderr)
        return 1
    sys.stdout.write(report(survey))
    return 0


if __name__ == "__main__":
    sys.exit(main())
