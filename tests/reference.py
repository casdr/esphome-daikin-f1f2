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

FAN_NUM = {v: k for k, v in FAN.items()}
MODE_NUM = {v: k for k, v in MODES.items()}
FIELDS = [("room", "room"), ("intake", "intake"), ("remote", "remote"), ("pipe_a", "pipe_a"),
          ("pipe_b", "pipe_b"), ("setpoint", "setpoint"), ("demand_step", "demand_step")]


def main() -> None:
    st = State()
    for path in sys.argv[1:]:
        for line in open(path):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 3 and p[2] and not p[2].startswith("#"):
                try:
                    st.feed(bytes.fromhex(p[2]), float(p[0]))
                except ValueError:
                    pass
    for addr in sorted(st.units):
        u = st.units[addr]
        if not u:
            continue
        out = [f"unit {addr:02X}"]
        for key, name in FIELDS:
            if key in u:
                out.append(f"{name}={u[key]:.2f}")
        if "power" in u:
            out.append(f"power={1 if u['power'] == 'on' else 0}")
        if "mode" in u and u["mode"] in MODE_NUM:
            out.append(f"mode={MODE_NUM[u['mode']]:02X}")
        if "fan_status" in u and u["fan_status"] in FAN_NUM:
            out.append(f"fan={FAN_NUM[u['fan_status']]}")
        if "airflow" in u:
            out.append(f"airflow={u['airflow']}")
        if "status" in u:
            out.append(f"status={u['status']:02X}")
        if "master" in u:
            out.append(f"master={int(u['master'])}")
        print(" ".join(out))
    o = st.outdoor
    sysout = ["system"]
    for key, name in [("outdoor_air", "outdoor"), ("t_evap", "t_evap"), ("t_cond", "t_cond"),
                      ("m1_discharge", "discharge_1"), ("m2_discharge", "discharge_2"), ("capacity", "capacity"),
                      ("m1_comp_step", "comp_step_1"), ("m2_comp_step", "comp_step_2"), ("m1_fan_step", "fan_step_1"),
                      ("m2_fan_step", "fan_step_2"), ("m1_comp_freq", "comp_freq_1"), ("m2_comp_freq", "comp_freq_2")]:
        if key in o:
            sysout.append(f"{name}={o[key]:.2f}")
    comp = {"off": 0, "start": 1, "on": 2}.get(o.get("compressor"), -1)  # type: ignore[arg-type]
    clock = o.get("clock", "-1:-1")
    hh, mm = (int(x) for x in clock.split(":"))
    sysout.append(f"compressor={comp} clock={hh:02d}:{mm:02d}")
    if "defrost" in o:
        sysout.append(f"defrost={int(bool(o['defrost']))}")
    print(" ".join(sysout))


if __name__ == "__main__":
    main()
