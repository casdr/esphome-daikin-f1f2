#!/usr/bin/env python3
"""Decode Daikin F1F2 (DIII-NET) frames into a per-unit status table.

  f1f2_decode.py capture.tsv             decode the last state from a capture
  f1f2_decode.py --live [--seconds 90]   listen on MQTT and decode

The field definitions follow docs/protocol.md. Fields marked (hyp) are hypotheses.
Live mode reads MQTT_HOST (default localhost), MQTT_PORT (1883), MQTT_USER,
MQTT_PASSWORD and MQTT_TOPIC_SUFFIX (default /P1P2MQTT/bridge0) from the environment.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time
from typing import Any

# DIII index -> label, for the test system (17 indoor units and a hydrobox)
UNITS = {
    0x00: "1-00", 0x01: "1-01", 0x02: "1-02",
    0x03: "1-03", 0x04: "1-04", 0x05: "1-05",
    0x06: "1-06", 0x07: "1-07", 0x08: "1-08",
    0x09: "1-09", 0x0B: "1-11", 0x0C: "1-12",
    0x0D: "1-13", 0x0E: "1-14", 0x0F: "1-15",
    0x10: "2-00 hydrobox", 0x11: "2-01",
}
# B1 index -> DIII index, derived by correlating B1 @43 with F5 room temperature.
# 0x02/0x05 (1-08/1-00) are only weakly separated, see docs/protocol.md.
B1_TO_DIII = {
    0x01: 0x0C, 0x02: 0x08, 0x03: 0x0B, 0x04: 0x09, 0x05: 0x00, 0x06: 0x02,
    0x07: 0x04, 0x08: 0x01, 0x09: 0x03, 0x0A: 0x07, 0x0B: 0x06, 0x0C: 0x05,
    0x0D: 0x0E, 0x0E: 0x11, 0x0F: 0x0D, 0x10: 0x0F,
}
# remote-control groups: master DIII index -> member DIII indexes (experiment 5)
GROUPS = {
    0x01: [0x00, 0x01, 0x02], 0x04: [0x03, 0x04, 0x05, 0x06], 0x07: [0x07],
    0x09: [0x08, 0x09, 0x0B], 0x0C: [0x0C], 0x0F: [0x0D, 0x0E, 0x0F], 0x11: [0x11], 0x10: [0x10],
}
MODES = {0x60: "fan", 0x61: "heat", 0x62: "cool", 0x63: "auto", 0x67: "dry"}
FAN = {0: "auto", 1: "low", 3: "medium", 5: "high"}
RAW_RE = re.compile(r"T\s*([\d.]+):\s*([0-9A-Fa-f]+)")


def t88(b: bytes, o: int, signed: bool = False) -> float:
    return int.from_bytes(b[o:o + 2], "big", signed=signed) / 256


def sm8(b: bytes, o: int) -> float:
    """8.8 sign-magnitude (bit 15 = negative)."""
    v = int.from_bytes(b[o:o + 2], "big")
    return -((v & 0x7FFF) / 256) if v & 0x8000 else v / 256


def checksum_ok(b: bytes) -> bool:
    return len(b) < 2 or sum(b[1:]) & 0xFF == 0


class State:
    def __init__(self) -> None:
        self.units: dict[int, dict[str, Any]] = {a: {} for a in UNITS}
        self.outdoor: dict[str, Any] = {}
        self.commands: list[tuple[float | None, str]] = []

    def feed(self, b: bytes, t: float | None = None) -> None:
        if len(b) < 6 or not checksum_ok(b) or len(b) != 6 + b[4]:
            return
        cmd, src, dst = b[0], b[1], b[2]
        if cmd == 0xF5 and src in self.units:
            u = self.units[src]
            if len(b) == 23:
                u["mode"] = MODES.get(b[6], f"0x{b[6]:02X}")
                u["room"] = t88(b, 14)
                u["fan_status"] = FAN.get(b[16] >> 4, f"0x{b[16]:02X}")  # (hyp)
                u["airflow"] = b[16] & 0x0F  # 0-4 position (0 horizontal, 4 down), 7 swing
                u["status"] = b[11]  # (hyp) status bits
            elif len(b) == 22:  # hydrobox
                u["room"] = t88(b, 14)
                u["power"] = "on" if b[11] & 0x01 else "off"  # no 31 B frame: status bit 0
                u["status"] = b[11]
                u["demand_step"] = float(b[9])  # (hyp) requested capacity step, 0 = no demand
            elif len(b) == 31:
                u["power"] = "on" if b[6] else "off"
                u["mode"] = MODES.get(b[7], u.get("mode"))
                u["setpoint"] = float(b[13])
                u["status"] = b[19]  # (hyp) status bits, 0x20 = heating demand
                u["master"] = bool(b[18] & 0x02)  # group master 0x12, member 0x10
        elif cmd == 0xB1 and len(b) == 46 and src in B1_TO_DIII:
            u = self.units[B1_TO_DIII[src]]
            u["setpoint"] = float(b[17])  # byte 18 holds flags
            u["pipe_a"] = t88(b, 13)    # (hyp) liquid/gas pipe
            u["pipe_b"] = t88(b, 15)    # (hyp)
            u["remote"] = t88(b, 30)    # remote-controller sensor (1-04 matches P1P2)
            u["intake"] = t88(b, 43)    # return air, equals F5 room temperature
            u["t11"] = t88(b, 11)       # unknown temperature (hyp)
        elif cmd == 0xE9 and len(b) in (46, 43) and b[3] == 0x10 and b[5] == 0x65:  # 43 B: ERQ (hyp)
            self.outdoor["compressor"] = {0: "off", 1: "start", 2: "on"}.get(b[11], f"0x{b[11]:02X}")
            self.outdoor["t_evap"] = sm8(b, 15)  # (hyp) Te, low-pressure saturation
            self.outdoor["t_cond"] = sm8(b, 17)  # Tc, sign-magnitude; ~ambient idle, ~42 heating
            self.outdoor["current"] = b[36] / 10  # (hyp) compressor current, A
            self.outdoor["comp_speed_total"] = float(b[25] << 8 | b[26])  # = 3 x (module 1 + module 2 speed)
        elif cmd == 0xE9 and len(b) == 46 and b[3] == 0x10 and b[5] == 0x6A:
            self.outdoor["defrost"] = 0x07 in (b[12], b[14], b[16])  # (hyp) defrost or oil return
        elif cmd == 0xE9 and len(b) == 44 and b[3] == 0x10:
            self.outdoor["outdoor_air"] = sm8(b, 21)  # sign-magnitude, like Te
        elif cmd == 0xE9 and len(b) == 41 and b[5] == 0xE0 and b[11] in (0x12, 0x13):
            m = b[11] - 0x11
            self.outdoor[f"m{m}_discharge"] = float(-(b[16] & 0x7F) if b[16] & 0x80 else b[16])  # (hyp) sign-magnitude
            self.outdoor[f"m{m}_comp_step"] = float(b[37])  # (hyp)
            self.outdoor[f"m{m}_fan_step"] = float(b[39])  # (hyp)
        elif cmd == 0xE9 and len(b) == 41 and b[5] == 0xE0 and b[11] in (0x02, 0x03):
            self.outdoor[f"m{b[11] - 1}_comp_speed"] = float(b[32])  # (hyp) compressor speed
        elif cmd == 0xE9 and len(b) == 36 and b[5] == 0xE6:
            self.outdoor["clock"] = f"{b[11]:02d}:{b[12]:02d}"
        elif cmd == 0xA5:
            self.commands.append((t, b.hex().upper()))

    def table(self) -> str:
        cols = ["power", "mode", "fan_status", "setpoint", "room", "intake", "remote", "pipe_a", "pipe_b", "t11"]
        lines = [f"{'unit':20s} " + " ".join(f"{c:>9s}" for c in cols)]
        for a, name in UNITS.items():
            u = self.units[a]
            vals = []
            for c in cols:
                v = u.get(c, "")
                vals.append(f"{v:9.2f}" if isinstance(v, float) else f"{str(v):>9s}")
            lines.append(f"{name:20s} " + " ".join(vals))
        lines.append("outdoor: " + ", ".join(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}" for k, v in self.outdoor.items()))
        return "\n".join(lines)


def from_file(path: str, st: State) -> None:
    for line in open(path):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 3 and p[2] and not p[2].startswith("#"):
            st.feed(bytes.fromhex(p[2]), float(p[0]))


def live(seconds: float, st: State) -> None:
    import paho.mqtt.client as mqtt

    def on_message(c: mqtt.Client, u: Any, m: mqtt.MQTTMessage) -> None:
        mt = RAW_RE.search(m.payload.decode(errors="replace"))
        if mt:
            st.feed(bytes.fromhex(mt.group(2)), time.time())

    topic = "P1P2/R" + os.environ.get("MQTT_TOPIC_SUFFIX", "/P1P2MQTT/bridge0")
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"f1f2-decode-{os.getpid()}")
    if os.environ.get("MQTT_USER"):
        c.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASSWORD"))
    c.on_connect = lambda c, u, f, rc, p=None: c.subscribe(topic)
    c.on_message = on_message
    c.connect(os.environ.get("MQTT_HOST", "localhost"), int(os.environ.get("MQTT_PORT", "1883")), 30)
    c.loop_start()
    time.sleep(seconds)
    c.loop_stop()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", nargs="?")
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--seconds", type=float, default=90)
    args = ap.parse_args()
    st = State()
    if args.live:
        live(args.seconds, st)
    elif args.capture:
        from_file(args.capture, st)
    else:
        ap.error("give a capture file or --live")
    print(st.table())


if __name__ == "__main__":
    sys.exit(main())  # type: ignore[func-returns-value]
