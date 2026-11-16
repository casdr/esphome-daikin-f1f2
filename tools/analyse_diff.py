#!/usr/bin/env python3
"""Show F1F2 frames whose content changes during an experiment capture.

For each (cmd, addr, dst, len) frame key, print every time its payload changes
(ignoring the checksum), with the time relative to the CMD marker and the
changed byte offsets. MARK and P1P2 lines are interleaved for context.

usage: analyse_diff.py exp-capture.tsv
"""
from __future__ import annotations

import sys

path = sys.argv[1]
t_cmd: float | None = None
events: list[tuple[float, str]] = []
last: dict[tuple[int, int, int, int], bytes] = {}
for line in open(path):
    p = line.rstrip("\n").split("\t")
    t = float(p[0])
    if p[-1].startswith("#"):
        if "MARK CMD" in p[-1]:
            t_cmd = t
        events.append((t, p[-1]))
        continue
    hx = p[2]
    if len(hx) < 12:
        continue
    b = bytes.fromhex(hx)
    key = (b[0], b[1], b[2], len(b))
    body = b[:-1]
    if key in last and last[key] != body:
        diffs = [i for i, (x, y) in enumerate(zip(last[key], body)) if x != y]
        desc = " ".join(f"[{i}]{last[key][i]:02X}->{body[i]:02X}" for i in diffs)
        events.append((t, f"   {b[0]:02X} {b[1]:02X} {b[2]:02X} len{len(b):<3d} {desc}"))
    last[key] = body
t0 = t_cmd or events[0][0]
for t, s in events:
    print(f"{t - t0:+8.1f}s {s}")
