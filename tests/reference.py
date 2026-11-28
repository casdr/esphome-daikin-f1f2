#!/usr/bin/env python3
"""Reference output for test_decoder.cpp, using tools/f1f2_decode.py.

Prints the final state of the same capture files in the same canonical format.
  python3 reference.py ../captures/*.tsv
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from f1f2_decode import FAN, MODES, State  # noqa: E402

FAN_NUM = {name: code for code, name in FAN.items()}
MODE_NUM = {name: code for code, name in MODES.items()}
FIELDS = [("room", "room"), ("intake", "intake"), ("remote", "remote"), ("pipe_a", "pipe_a"),
          ("pipe_b", "pipe_b"), ("setpoint", "setpoint"), ("demand_step", "demand_step")]


def main() -> None:
    state = State()
    for path in sys.argv[1:]:
        for line in open(path):
            columns = line.rstrip("\n").split("\t")
            if len(columns) >= 3 and columns[2] and not columns[2].startswith("#"):
                try:
                    state.feed(bytes.fromhex(columns[2]), float(columns[0]))
                except ValueError:
                    pass
    for addr in sorted(state.units):
        unit = state.units[addr]
        if not unit:
            continue
        out = [f"unit {addr:02X}"]
        for key, name in FIELDS:
            if key in unit:
                out.append(f"{name}={unit[key]:.2f}")
        if "power" in unit:
            out.append(f"power={1 if unit['power'] == 'on' else 0}")
        if "mode" in unit and unit["mode"] in MODE_NUM:
            out.append(f"mode={MODE_NUM[unit['mode']]:02X}")
        if "fan_status" in unit and unit["fan_status"] in FAN_NUM:
            out.append(f"fan={FAN_NUM[unit['fan_status']]}")
        if "airflow" in unit:
            out.append(f"airflow={unit['airflow']}")
        if "status" in unit:
            out.append(f"status={unit['status']:02X}")
        if "master" in unit:
            out.append(f"master={int(unit['master'])}")
        print(" ".join(out))
    outdoor = state.outdoor
    system_out = ["system"]
    for key, name in [("outdoor_air", "outdoor"), ("t_evap", "t_evap"), ("t_cond", "t_cond"),
                      ("m1_discharge", "discharge_1"), ("m2_discharge", "discharge_2"), ("current", "current"),
                      ("m1_comp_step", "comp_step_1"), ("m2_comp_step", "comp_step_2"), ("m1_fan_step", "fan_step_1"),
                      ("m2_fan_step", "fan_step_2"), ("m1_comp_speed", "comp_speed_1"), ("m2_comp_speed", "comp_speed_2"),
                      ("comp_speed_total", "comp_speed_total")]:
        if key in outdoor:
            system_out.append(f"{name}={outdoor[key]:.2f}")
    compressor = {"off": 0, "start": 1, "on": 2}.get(outdoor.get("compressor"), -1)  # type: ignore[arg-type]
    clock = outdoor.get("clock", "-1:-1")
    hour, minute = (int(part) for part in clock.split(":"))
    system_out.append(f"compressor={compressor} clock={hour:02d}:{minute:02d}")
    if "defrost" in outdoor:
        system_out.append(f"defrost={int(bool(outdoor['defrost']))} oil_return={int(bool(outdoor['oil_return']))}")
    print(" ".join(system_out))


if __name__ == "__main__":
    main()
