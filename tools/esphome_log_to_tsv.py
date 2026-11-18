#!/usr/bin/env python3
"""Turn an ESPHome log with Frame logging on into a bus log (TSV).

With the bridge's *Frame logging* switch on, every bus frame is logged under the tag
daikin_diii.frame, in the same form as the stock P1P2MQTT raw output:

    [22:41:03.123][I][daikin_diii.frame:310]: R T  3.745: E900FF1028...
    [22:41:05.001][I][daikin_diii.frame:310]: X A5300114144003...

This keeps only those lines and writes the TSV that the other tools and captures/ use:

    <unix time>  <gap before the frame, s>  <frame as hex>          (R lines)
    <unix time>                             # X <frame> / # E <line> (own writes, errors)

It reads the output of `esphome logs`, a log downloaded from the ESPHome Builder, and
Home Assistant's log when the ESPHome integration forwards the device's logs. Lines
without a date get the date from --date, or else from the file's modification time (the
day before when the first line is later in the day than that); the date moves on by a
day when the time of day goes back. Times without a UTC offset are in this computer's
time zone.

  esphome_log_to_tsv.py bus.log > bus.tsv
  esphome_log_to_tsv.py --date 2026-11-18 bus.log > bus.tsv

Standard library only.
"""
from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
from pathlib import Path
from typing import Iterable, Iterator, TextIO

FRAME_TAG = "daikin_diii.frame"
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
# full timestamp (Home Assistant's log) or time of day ([HH:MM:SS] / [HH:MM:SS.mmm])
DATETIME_RE = re.compile(
    r"(\d{4}-\d{2}-\d{2})[ T](\d{2}):(\d{2}):(\d{2})(?:[.,](\d+))?(Z|[+-]\d{2}:?\d{2})?"
)
TIME_RE = re.compile(r"\[(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?\]")
# the message after the tag: "[daikin_diii.frame:310]: R T  3.745: FD0A..." or
# "[daikin_diii.frame]: ..." (no line number in some log formats)
MESSAGE_RE = re.compile(r"\[" + re.escape(FRAME_TAG) + r"(?::\d+)?\]:\s?(.*)$")
RAW_RE = re.compile(r"^R\s+T\s*([\d.]+):\s*([0-9A-Fa-f]+)\s*$")
HEX_RE = re.compile(r"^[0-9A-Fa-f]+$")


def _fraction(digits: str | None) -> float:
    return float("0." + digits) if digits else 0.0


def _offset(text: str | None) -> dt.tzinfo | None:
    if not text:
        return None
    if text == "Z":
        return dt.timezone.utc
    sign = -1 if text[0] == "-" else 1
    digits = text[1:].replace(":", "")
    return dt.timezone(sign * dt.timedelta(hours=int(digits[:2]), minutes=int(digits[2:])))


class Clock:
    """Turns the time of day on each line into a unix time."""

    def __init__(self, start: dt.date, tz: dt.tzinfo | None = None) -> None:
        self.date = start
        self.tz = tz
        self.last_seconds: float | None = None

    def stamp(self, line: str) -> float | None:
        full = DATETIME_RE.search(line)
        if full:
            date = dt.date.fromisoformat(full.group(1))
            hours, minutes, seconds = (int(full.group(i)) for i in (2, 3, 4))
            self.date = date
            self.last_seconds = hours * 3600 + minutes * 60 + seconds + _fraction(full.group(5))
            offset = _offset(full.group(6))
            if offset is not None:  # Home Assistant writes its UTC offset: use it
                moment = dt.datetime.combine(date, dt.time(hours, minutes, seconds), tzinfo=offset)
                return moment.timestamp() + _fraction(full.group(5))
            return self._unix(hours, minutes, seconds, _fraction(full.group(5)))
        tod = TIME_RE.search(line)
        if not tod:
            return None
        hours, minutes, seconds = (int(tod.group(i)) for i in (1, 2, 3))
        of_day = hours * 3600 + minutes * 60 + seconds + _fraction(tod.group(4))
        # a log that runs past midnight: the time of day goes back by hours, not seconds
        if self.last_seconds is not None and of_day < self.last_seconds - 3600:
            self.date += dt.timedelta(days=1)
        self.last_seconds = of_day
        return self._unix(hours, minutes, seconds, _fraction(tod.group(4)))

    def _unix(self, hours: int, minutes: int, seconds: int, fraction: float) -> float:
        moment = dt.datetime.combine(self.date, dt.time(hours, minutes, seconds), tzinfo=self.tz)
        return moment.timestamp() + fraction


def guess_start_date(path: Path) -> dt.date:
    """The date of the first line of a log, from the file's modification time.

    The file was last written at the end of the log. When the first line's time of day is
    later than that, the log ran past midnight and started the day before. A log longer
    than a day needs --date.
    """
    modified = dt.datetime.fromtimestamp(path.stat().st_mtime)
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = ANSI_RE.sub("", line)
            if DATETIME_RE.search(line):
                return modified.date()  # the lines carry their own date
            tod = TIME_RE.search(line)
            if tod and MESSAGE_RE.search(line):
                first = dt.time(int(tod.group(1)), int(tod.group(2)), int(tod.group(3)))
                return modified.date() - dt.timedelta(days=1) if first > modified.time() else modified.date()
    return modified.date()


def convert(lines: Iterable[str], clock: Clock) -> Iterator[str]:
    """Yield TSV rows for the frame lines in an ESPHome log."""
    for raw_line in lines:
        line = ANSI_RE.sub("", raw_line.rstrip("\r\n"))
        message = MESSAGE_RE.search(line)
        if not message:
            continue
        unix_time = clock.stamp(line)
        if unix_time is None:
            continue
        text = message.group(1).strip()
        frame = RAW_RE.match(text)
        if frame:
            yield f"{unix_time:.3f}\t{float(frame.group(1)):.3f}\t{frame.group(2).upper()}"
        elif text.startswith("X ") and HEX_RE.match(text[2:].strip()):
            yield f"{unix_time:.3f}\t\t\t# X {text[2:].strip().upper()}"
        elif text.startswith("E"):
            yield f"{unix_time:.3f}\t\t\t# {text}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log", nargs="?", help="ESPHome log file (default: stdin)")
    parser.add_argument("--date", help="date of the first line, YYYY-MM-DD (default: the file's date, or today)")
    parser.add_argument("-o", "--output", help="TSV file to write (default: stdout)")
    args = parser.parse_args(argv)

    if args.date:
        try:
            start = dt.date.fromisoformat(args.date)
        except ValueError:
            parser.error(f"--date must be YYYY-MM-DD, not {args.date!r}")
    elif args.log:
        start = guess_start_date(Path(args.log))
    else:
        start = dt.date.today()

    clock = Clock(start)
    source: TextIO = open(args.log, encoding="utf-8", errors="replace") if args.log else sys.stdin
    target: TextIO = open(args.output, "w", encoding="utf-8") if args.output else sys.stdout
    count = 0
    try:
        for row in convert(source, clock):
            target.write(row + "\n")
            count += 1
    finally:
        if args.log:
            source.close()
        if args.output:
            target.close()
    print(f"{count} lines written", file=sys.stderr)
    if count == 0:
        print("No frame lines found: was Frame logging on, and the log level INFO or lower?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
