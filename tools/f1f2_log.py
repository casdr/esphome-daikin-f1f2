#!/usr/bin/env python3
"""Log raw F1F2 packets from a P1P2MQTT bridge to a TSV file.

P1P2MQTT publishes raw bus traffic on P1P2/R/<client>, e.g.
    "R                     T  3.745: FD0AFF140650820008000003"
where T is the gap since the previous packet (s) and the hex is the packet
(the bridge has already stripped CRC/parity).

Output columns: unix_time  gap_s  hex  [marker]
Type a line on stdin while logging to insert a marker (e.g. "setpoint 23"),
which makes it easy to correlate packets with actions on a remote control.

Broker and credentials come from MQTT_HOST (default localhost), MQTT_PORT (1883),
MQTT_USER and MQTT_PASSWORD; the topic suffix from MQTT_TOPIC_SUFFIX
(default /P1P2MQTT/bridge0). Command-line options override them.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import threading
import time
from typing import Any

import paho.mqtt.client as mqtt

RAW_RE = re.compile(r"T\s*([\d.]+):\s*([0-9A-Fa-f]+)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("MQTT_HOST", "localhost"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("MQTT_PORT", "1883")))
    ap.add_argument("--topic", default="P1P2/R" + os.environ.get("MQTT_TOPIC_SUFFIX", "/P1P2MQTT/bridge0"))
    ap.add_argument("--out", default=None, help="output file (default: captures/f1f2-<timestamp>.tsv)")
    ap.add_argument("--duration", type=float, default=0, help="stop after N seconds (0 = until Ctrl-C)")
    ap.add_argument("--quiet", action="store_true", help="don't echo packets to stdout")
    args = ap.parse_args()

    out = args.out or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "captures", time.strftime("f1f2-%Y%m%d-%H%M%S.tsv")
    )
    os.makedirs(os.path.dirname(out), exist_ok=True)
    fh = open(out, "a", buffering=1)
    lock = threading.Lock()
    stats = {"n": 0, "bad": 0}

    def write(line: str) -> None:
        with lock:
            fh.write(line + "\n")

    def on_connect(
        c: mqtt.Client, u: Any, flags: mqtt.ConnectFlags, rc: mqtt.ReasonCode, props: mqtt.Properties | None = None
    ) -> None:
        print(f"connected ({rc}), subscribing {args.topic} -> {out}", file=sys.stderr)
        c.subscribe(args.topic)

    def on_message(c: mqtt.Client, u: Any, m: mqtt.MQTTMessage) -> None:
        now = time.time()
        txt = m.payload.decode(errors="replace")
        mt = RAW_RE.search(txt)
        if not mt:
            stats["bad"] += 1
            write(f"{now:.3f}\t\t\t# unparsed: {txt.strip()}")
            return
        stats["n"] += 1
        gap, hx = mt.group(1), mt.group(2).upper()
        write(f"{now:.3f}\t{gap}\t{hx}")
        if not args.quiet:
            print(f"{time.strftime('%H:%M:%S')} {gap:>7} {hx}")

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"f1f2-log-{os.getpid()}")
    if os.environ.get("MQTT_USER"):
        c.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASSWORD"))
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(args.host, args.port, 30)
    c.loop_start()

    def read_markers() -> None:
        for line in sys.stdin:
            line = line.strip()
            if line:
                write(f"{time.time():.3f}\t\t\t# MARK {line}")
                print(f"--- marker: {line}", file=sys.stderr)

    if sys.stdin.isatty():
        threading.Thread(target=read_markers, daemon=True).start()

    try:
        t0 = time.time()
        while not args.duration or time.time() - t0 < args.duration:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        c.loop_stop()
        fh.close()
        print(f"\n{stats['n']} packets, {stats['bad']} unparsed -> {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
