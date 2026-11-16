#!/usr/bin/env python3
"""Field analysis of E9 (outdoor/system) frames across all captures.

For every E9 page variant and every varying byte / 16-bit word: value range,
mean while the compressor is idle vs running, and correlation with t_cond
(page 65 @17) and outdoor air (page 67 @21).
"""
from __future__ import annotations

import bisect
import collections
import statistics
import sys
from collections.abc import Callable, Sequence


def load(paths: list[str]) -> list[tuple[float, bytes]]:
    seen, rows = set(), []
    for f in paths:
        for line in open(f):
            p = line.rstrip("\n").split("\t")
            if len(p) < 3 or not p[2].startswith("E9"):
                continue
            try:
                t = float(p[0])
            except ValueError:
                continue
            if (t, p[2]) in seen:
                continue
            seen.add((t, p[2]))
            rows.append((t, bytes.fromhex(p[2])))
    rows.sort()
    return rows


def series(
    rows: list[tuple[float, bytes]], pred: Callable[[bytes], bool], fn: Callable[[bytes], float]
) -> tuple[list[float], list[float]]:
    s = [(t, fn(b)) for t, b in rows if pred(b)]
    return [x[0] for x in s], [x[1] for x in s]


def at(ts: list[float], vs: list[float], t: float) -> float | None:
    i = bisect.bisect_right(ts, t) - 1
    return vs[i] if i >= 0 else None


def corr(a: Sequence[float], b: Sequence[float]) -> float:
    if len(a) < 5 or statistics.pstdev(a) == 0 or statistics.pstdev(b) == 0:
        return 0.0
    ma, mb = statistics.mean(a), statistics.mean(b)
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    return cov / (len(a) * statistics.pstdev(a) * statistics.pstdev(b))


def main() -> None:
    rows = load(sys.argv[1:])
    p65 = lambda b: b[5] == 0x65 and len(b) == 46  # noqa: E731
    p67 = lambda b: b[5] == 0x67 and len(b) == 44  # noqa: E731
    ct, cv = series(rows, p65, lambda b: b[11])
    tt, tv = series(rows, p65, lambda b: int.from_bytes(b[17:19], "big") / 256)
    ot, ov = series(rows, p67, lambda b: int.from_bytes(b[21:23], "big") / 256)

    var: collections.defaultdict[tuple[int, int, int, int | None], list[tuple[float, bytes]]] = collections.defaultdict(list)
    for t, b in rows:
        key = (b[3], len(b), b[5], b[11] if b[5] in (0xE0, 0xE6) and len(b) == 41 else None)
        var[key].append((t, b))

    for key, v in sorted(var.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2], kv[0][3] or 0)):
        b3, n, page, sub = key
        print(f"\n=== E9 b3={b3:02X} len={n} page={page:02X}" + (f" sub={sub:02X}" if sub is not None else "") + f"  n={len(v)}")
        print("    ", v[-1][1].hex(" ").upper())
        for i in range(11, n - 1):
            for width in (1, 2):
                if i + width > n - 1:
                    continue
                vals = [int.from_bytes(b[i:i + width], "big") for _, b in v]
                if len(set(vals)) < 2:
                    continue
                if width == 1 and len(set(vals)) > 12:
                    continue  # probably low byte of a word
                idle = [x for (t, _), x in zip(v, vals) if at(ct, cv, t) == 0]
                run = [x for (t, _), x in zip(v, vals) if at(ct, cv, t) == 2]
                tc = [(x, at(tt, tv, t)) for (t, _), x in zip(v, vals) if at(tt, tv, t) is not None]
                oc = [(x, at(ot, ov, t)) for (t, _), x in zip(v, vals) if at(ot, ov, t) is not None]
                r_t = corr([a for a, _ in tc], [c for _, c in tc])  # type: ignore[misc]  # None filtered above
                r_o = corr([a for a, _ in oc], [c for _, c in oc])  # type: ignore[misc]  # None filtered above
                f = (lambda x: f"{x / 256:.2f}") if width == 2 else (lambda x: f"{round(x):02X}")
                desc = f"@{i}{'w' if width == 2 else 'b'} range {f(min(vals))}..{f(max(vals))}"
                if idle and run:
                    desc += f"  idle~{f(statistics.mean(idle))} run~{f(statistics.mean(run))}"
                if width == 1:
                    desc += f"  vals={sorted(set(f(x) for x in vals))}"
                desc += f"  r(tcond)={r_t:+.2f} r(out)={r_o:+.2f}"
                print("    ", desc)


if __name__ == "__main__":
    main()
