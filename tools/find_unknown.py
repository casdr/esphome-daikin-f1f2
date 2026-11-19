#!/usr/bin/env python3
"""Find frame signatures that are not in the known catalogue (docs/protocol.md),
and new values in known frames, across capture/log TSV files.

  find_unknown.py captures/*.tsv raw-log.tsv
"""
from __future__ import annotations

import collections
import sys
import time
from typing import Any

KNOWN = {
    ("F5", 22), ("F5", 23), ("F5", 31), ("F5", 37),
    ("B1", 46), ("B1", 34), ("B1", 28), ("B1", 33),
    ("A9", 24), ("A9", 16),
    ("E5", 12),
    ("FD", 11), ("FD", 12),
    ("A5", 12), ("A5", 26), ("A5", 35),  # operation/fan commands; 35 B = hydrobox
    ("F5", 8), ("F5", 10), ("F5", 27), ("F5", 33),  # page 91 (hourly), page 90 (iTM start-up)
    ("AD", 26), ("A9", 46), ("BD", 12), ("BD", 26), ("A4", 26),
    ("06", 1),
}
KNOWN_E9: set[tuple[int, int, int, int | None]] = {  # (b3, len, page, sub)
    (0x08, 8, 0x2F, None), (0x0C, 8, 0x2F, None), (0x10, 8, 0x2F, None), (0x08, 17, 0x68, None),
    (0x0C, 45, 0x6E, None), (0x10, 44, 0x67, None), (0x10, 46, 0x65, None), (0x10, 46, 0x6A, None),
    (0x10, 43, 0x65, None),  # ERQ
    (0x1C, 36, 0xE6, None), (0x1C, 46, 0xE0, None),
    (0x10, 32, 0x6D, None),  # seen once, 2026-11-16 11:40:54, meaning unknown
    (0x1C, 41, 0xE0, 0x02), (0x1C, 41, 0xE0, 0x03), (0x1C, 41, 0xE0, 0x12), (0x1C, 41, 0xE0, 0x13),
}


def sig(b: bytes) -> tuple[tuple[object, ...], bool]:
    if b[0] == 0xE9 and len(b) >= 6:
        sub = b[11] if (b[5] == 0xE0 and len(b) == 41) else None
        k: tuple[object, ...] = (b[3], len(b), b[5], sub)
        return ("E9", k), k in KNOWN_E9
    k = ("%02X" % b[0], len(b))
    return k, k in KNOWN


def ts(t: float) -> str:
    return time.strftime("%H:%M:%S", time.localtime(t))


def main() -> None:
    seen: set[tuple[float, str]] = set()
    rows: list[tuple[float, bytes]] = []
    for f in sys.argv[1:]:
        for line in open(f):
            p = line.rstrip("\n").split("\t")
            if len(p) < 3 or not p[2] or p[2].startswith("#"):
                continue
            try:
                t, b = float(p[0]), bytes.fromhex(p[2])
            except ValueError:
                continue
            if (round(t, 1), p[2]) in seen:
                continue
            seen.add((round(t, 1), p[2]))
            rows.append((t, b))
    rows.sort()
    print(f"{len(rows)} frames, {ts(rows[0][0])} .. {ts(rows[-1][0])}")

    stats: collections.OrderedDict[tuple[object, ...], dict[str, Any]] = collections.OrderedDict()
    for t, b in rows:
        k, known = sig(b)
        s = stats.setdefault(k, {"n": 0, "first": t, "last": t, "known": known, "ex": set(), "src": set(), "dst": set()})
        s["n"] += 1
        s["last"] = t
        if len(s["ex"]) < 4:
            s["ex"].add(b.hex().upper())
        if len(b) >= 3:
            s["src"].add(b[1])
            s["dst"].add(b[2])

    print("\n== UNKNOWN signatures")
    for k, s in stats.items():
        if s["known"]:
            continue
        print(f"{str(k):34s} n={s['n']:4d} first {ts(s['first'])} last {ts(s['last'])} "
              f"src={sorted('%02X' % x for x in s['src'])} dst={sorted('%02X' % x for x in s['dst'])}")
        for e in sorted(s["ex"]):
            print("      ", e)

    # new src/dst combos and payload[0] variants in known signatures
    print("\n== Known signatures: payload variants (first byte) / src-dst pairs")
    var: collections.defaultdict[tuple[object, ...], collections.Counter[int]] = collections.defaultdict(
        lambda: collections.Counter())
    pairs: collections.defaultdict[tuple[object, ...], set[tuple[int, int]]] = collections.defaultdict(set)
    for t, b in rows:
        k, known = sig(b)
        if not known or len(b) < 6:
            continue
        var[k][b[5]] += 1
        pairs[k].add((b[1], b[2]))
    for k in var:
        if k[0] in ("FD", "E5", "A5", "A9", "B1"):
            print(f"{str(k):34s} payload[0]: {dict((('%02X' % a), n) for a, n in var[k].items())}  "
                  f"pairs: {sorted('%02X>%02X' % p for p in pairs[k])[:12]}{' ...' if len(pairs[k]) > 12 else ''}")


if __name__ == "__main__":
    main()
