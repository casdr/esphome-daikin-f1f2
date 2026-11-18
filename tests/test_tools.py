"""Tests for the debug tools: tools/esphome_log_to_tsv.py and tools/survey.py, and the
known-frame catalogue shared by the firmware and tools/find_unknown.py.

  python3 -m unittest discover -s tests -p 'test_*.py'
"""
from __future__ import annotations

import datetime as dt
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import esphome_log_to_tsv as log_to_tsv  # noqa: E402
import find_unknown  # noqa: E402
import survey  # noqa: E402

# `esphome logs` output as captured from a bridge, with the colour codes the CLI adds
CLI_LOG = """\
INFO ESPHome 2026.10.2
INFO Successfully connected to daikin-f1f2 @ 192.168.1.20 in 0.016s
[22:55:39.300][I][daikin_diii:418]: Frame logging enabled
\x1b[0;32m[22:55:39.451][I][daikin_diii.frame:309]: R T  8.289: FD0AFF14065082000100000A\x1b[0m
[22:55:39.635][I][daikin_diii.frame:309]: R T  0.067: F508FF1C11826101000000000000158A37000080020090
[22:55:40.010][S][sensor]: 'Uptime' >> 193 s
[22:55:41.200][I][daikin_diii.frame:309]: X A5300114144003000000000000000080000000000000000000
[22:55:41.900][I][daikin_diii.frame:309]: E NOTE: raw logging disabled by switch
[23:59:59.900][I][daikin_diii.frame:309]: R T  0.002: 06
[00:00:00.100][I][daikin_diii.frame:309]: R T  0.200: 06
"""


def checksum(frame: bytes) -> bytes:
    return frame + bytes([(-sum(frame[1:])) & 0xFF])


def f5_status(address: int, room: float, mode: int = 0x61) -> bytes:
    body = bytearray(22)
    body[0], body[1], body[2], body[3], body[4] = 0xF5, address, 0xFF, 0x1C, 0x11
    body[6] = mode
    raw = round(room * 256)
    body[14], body[15] = raw >> 8, raw & 0xFF
    return checksum(bytes(body))


def f5_change(address: int, power: int, setpoint: int, master: bool) -> bytes:
    body = bytearray(30)
    body[0], body[1], body[2], body[3], body[4] = 0xF5, address, 0xFF, 0x1C, 0x19
    body[6], body[7], body[13] = power, 0x61, setpoint
    body[18] = 0x12 if master else 0x10
    return checksum(bytes(body))


def b1_frame(index: int, intake: float) -> bytes:
    body = bytearray(45)
    body[0], body[1], body[2], body[3], body[4] = 0xB1, index, 0x00, 0x08, 0x28
    raw = round(intake * 256)
    body[43], body[44] = raw >> 8, raw & 0xFF
    return checksum(bytes(body))


class LogToTsvTest(unittest.TestCase):
    def rows(self, text: str, start: str = "2026-11-18") -> list[list[str]]:
        clock = log_to_tsv.Clock(dt.date.fromisoformat(start), dt.timezone.utc)
        return [row.split("\t") for row in log_to_tsv.convert(text.splitlines(True), clock)]

    def test_cli_log(self) -> None:
        rows = self.rows(CLI_LOG)
        self.assertEqual(len(rows), 6)  # 4 frames, one own write, one note; other lines dropped
        first = rows[0]
        self.assertEqual(first[2], "FD0AFF14065082000100000A")
        self.assertEqual(first[1], "8.289")
        expected = dt.datetime(2026, 11, 18, 22, 55, 39, 451000, tzinfo=dt.timezone.utc).timestamp()
        self.assertAlmostEqual(float(first[0]), expected, places=3)
        self.assertEqual(rows[2][1:], ["", "", "# X A5300114144003000000000000000080000000000000000000"])
        self.assertEqual(rows[3][3], "# E NOTE: raw logging disabled by switch")

    def test_midnight(self) -> None:
        rows = self.rows(CLI_LOG)
        before, after = float(rows[4][0]), float(rows[5][0])
        self.assertAlmostEqual(after - before, 0.2, places=3)

    def test_full_timestamps(self) -> None:
        # a line that carries its own date wins over the start date
        line = ("2026-11-20 08:00:01.250 INFO (MainThread) [homeassistant.components.esphome] "
                "daikin-f1f2: [I][daikin_diii.frame:309]: R T  0.067: F508FF1C\n")
        rows = self.rows(line, start="2026-11-18")
        expected = dt.datetime(2026, 11, 20, 8, 0, 1, 250000, tzinfo=dt.timezone.utc).timestamp()
        self.assertAlmostEqual(float(rows[0][0]), expected, places=3)
        self.assertEqual(rows[0][2], "F508FF1C")

    def test_home_assistant_log(self) -> None:
        # Home Assistant's log (ESPHome integration with "subscribe to logs" on) has a UTC offset
        line = ("\x1b[32m2026-11-18 22:58:28.764+01:00 INFO (MainThread) "
                "[homeassistant.components.esphome.manager] Daikin F1F2: "
                "[I][daikin_diii.frame:309]: R T  0.026: E900FF0C276E\x1b[0m\n")
        rows = self.rows(line, start="2026-01-01")
        expected = dt.datetime(2026, 11, 18, 21, 58, 28, 764000, tzinfo=dt.timezone.utc).timestamp()
        self.assertAlmostEqual(float(rows[0][0]), expected, places=3)
        self.assertEqual(rows[0][1:], ["0.026", "E900FF0C276E"])

    def test_home_assistant_log_without_offset(self) -> None:
        # Home Assistant's own log file has no offset by default: the clock's time zone applies
        line = ("2026-11-18 22:58:28.764 INFO (MainThread) [homeassistant.components.esphome.manager] "
                "Daikin F1F2: [I][daikin_diii.frame:309]: R T  0.026: E900FF0C\n")
        rows = self.rows(line)  # the test clock runs in UTC
        expected = dt.datetime(2026, 11, 18, 22, 58, 28, 764000, tzinfo=dt.timezone.utc).timestamp()
        self.assertAlmostEqual(float(rows[0][0]), expected, places=3)

    def test_start_date_from_file(self) -> None:
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "bus.log"
            path.write_text("[23:59:58.000][I][daikin_diii.frame:309]: R T  0.002: 06\n"
                            "[00:00:02.000][I][daikin_diii.frame:309]: R T  0.002: 06\n")
            # written at 00:00:03 on 19 Nov: the log started on the 18th
            written = dt.datetime(2026, 11, 19, 0, 0, 3).timestamp()
            os.utime(path, (written, written))
            self.assertEqual(log_to_tsv.guess_start_date(path), dt.date(2026, 11, 18))
            # a log within one day keeps the file's date
            path.write_text("[08:00:00.000][I][daikin_diii.frame:309]: R T  0.002: 06\n")
            os.utime(path, (written + 9 * 3600, written + 9 * 3600))
            self.assertEqual(log_to_tsv.guess_start_date(path), dt.date(2026, 11, 19))

    def test_no_frames(self) -> None:
        self.assertEqual(self.rows("[22:55:40.010][S][sensor]: 'Uptime' >> 193 s\n"), [])


class SurveyTest(unittest.TestCase):
    def build(self) -> survey.Survey:
        result = survey.Survey()
        when = 1_800_000_000.0
        # master 0x01 with member 0x00, master 0x04 on its own; different setpoints
        for step in range(30):
            when += 20
            result.feed(when, f5_change(0x01, 1, 21, True))
            result.feed(when + 1, f5_change(0x04, 1, 23 if step % 2 else 22, True))
            result.feed(when + 12, f5_change(0x00, 1, 21, False))
            result.feed(when + 13, f5_status(0x00, 20.0 + step / 10))
            result.feed(when + 13, f5_status(0x01, 21.5 - step / 20))
            result.feed(when + 13, f5_status(0x04, 19.0 + step / 15))
            result.feed(when + 14, b1_frame(0x05, 20.0 + step / 10))
            result.feed(when + 14, b1_frame(0x08, 21.5 - step / 20))
            result.feed(when + 14, b1_frame(0x07, 19.0 + step / 15))
        return result

    def test_groups_and_b1(self) -> None:
        result = self.build()
        self.assertEqual(result.group_of(), {0x00: [0x01]})
        b1 = {address: index for address, (index, _) in result.b1_index_of().items()}
        self.assertEqual(b1, {0x00: 0x05, 0x01: 0x08, 0x04: 0x07})

    def test_report(self) -> None:
        text = survey.report(self.build())
        self.assertIn("| 1-00 | 0x00 | member | 1-01 | 0x05", text)
        self.assertIn('vars: {id: "00", group: "01", name: Daikin 1-00, area: area_main, b1_index: "0x05"}', text)
        self.assertIn('vars: {id: "04", name: Daikin 1-04, area: area_main, b1_index: "0x07"}', text)

    def test_ambiguous_group(self) -> None:
        # two masters that always have the same settings can't be told apart
        result = survey.Survey()
        when = 1_800_000_000.0
        for _ in range(10):
            when += 20
            result.feed(when, f5_change(0x01, 1, 21, True))
            result.feed(when, f5_change(0x04, 1, 21, True))
            result.feed(when + 12, f5_change(0x00, 1, 21, False))
        self.assertEqual(result.group_of(), {0x00: [0x01, 0x04]})
        self.assertIn("not sure: could also be", survey.report(result))

    def test_member_without_group(self) -> None:
        # a member whose master never shows up must not be suggested as a group master
        result = survey.Survey()
        result.feed(1_800_000_000.0, f5_change(0x00, 1, 21, False))
        result.feed(1_800_000_001.0, f5_status(0x03, 20.0))  # status only, role unknown
        text = survey.report(result)
        self.assertNotIn('vars: {id: "00", name:', text)
        self.assertIn("1-00: a member, group unknown", text)
        self.assertIn("1-03: role unknown", text)
        self.assertNotIn("packages/group-master.yaml", text)

    def test_unknown_frame(self) -> None:
        result = survey.Survey()
        result.feed(1_800_000_000.0, checksum(bytes([0xF5, 0x01, 0xFF, 0x1C, 0x13]) + bytes(19)))
        self.assertEqual(len(result.unknown), 1)
        self.assertIn("| F5 25 B | 1 | no |", survey.report(result))


class CatalogueTest(unittest.TestCase):
    """The firmware's KNOWN_FRAMES/KNOWN_E9 must list the same frame types as find_unknown.py."""

    def test_same_as_find_unknown(self) -> None:
        header = (ROOT / "components" / "daikin_diii" / "diii_decoder.h").read_text()
        frames_block = header.split("KNOWN_FRAMES[] = {")[1].split("};")[0]
        e9_block = header.split("KNOWN_E9[] = {")[1].split("};")[0]
        frames = {("%02X" % int(cmd, 16), int(length))
                  for cmd, length in re.findall(r"\{(0x[0-9A-Fa-f]+),\s*(\d+)\}", frames_block)}
        e9 = set()
        for b3, length, page, sub in re.findall(
                r"\{(0x[0-9A-Fa-f]+),\s*(\d+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\}", e9_block):
            e9.add((int(b3, 16), int(length), int(page, 16), None if sub.upper() == "0XFF" else int(sub, 16)))
        self.assertEqual(frames, find_unknown.KNOWN)
        self.assertEqual(e9, find_unknown.KNOWN_E9)


if __name__ == "__main__":
    unittest.main()
