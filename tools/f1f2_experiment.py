#!/usr/bin/env python3
"""Run change-and-restore experiments on Daikin indoor units while logging F1F2.

Actions:
  raw:<hex>                                    write an F1F2 frame via the F1F2 bridge (hex without
                                               checksum; the checksum is appended and the frame is
                                               sent as W<hex> to the ATmega)
  mqtt:<payload>                               publish to P1P2/W/<p1p2 suffix>, a second P1P2MQTT
                                               bridge on a unit's P1/P2 bus (needs --p1p2-topic-suffix)
  ha:<domain>.<service>:<entity_id>[:<json>]   Home Assistant service call (REST API)

Timeline: --pre s logging, each --step followed by --gap s, then all --restore
actions (ALWAYS run once any step went out, also on Ctrl-C/errors), --post s.
F1F2 raw packets, decoded P1P2 state changes and MARK lines go into one TSV.

Environment:
  MQTT_HOST (default localhost), MQTT_PORT (1883), MQTT_USER, MQTT_PASSWORD
  MQTT_TOPIC_SUFFIX     F1F2 bridge, default /P1P2MQTT/bridge0
  P1P2_TOPIC_SUFFIX     optional second bridge on a P1/P2 bus
  HA_URL, HA_TOKEN      for ha: actions, e.g. http://homeassistant.local:8123 and a
                        long-lived access token
  HA_STATUS_TEMPLATE    optional Jinja template rendered after each ha: action and
                        written as a MARK line; {entity_id} is replaced by the entity

Example:
  f1f2_experiment.py --name fan-1-04 \\
    --step 'ha:climate.set_fan_mode:climate.x:{"fan_mode":"high"}' \\
    --restore 'ha:climate.set_fan_mode:climate.x:{"fan_mode":"medium"}'
"""
from __future__ import annotations

import argparse
import json
import os
import re
import time
from typing import Any

import paho.mqtt.client as mqtt
import requests

RAW_RE = re.compile(r"T\s*([\d.]+):\s*([0-9A-Fa-f]+)")


class HomeAssistant:
    """Minimal Home Assistant REST API client (HA_URL, HA_TOKEN)."""

    def __init__(self) -> None:
        self.url = os.environ["HA_URL"].rstrip("/")
        self.session = requests.Session()
        self.session.headers["Authorization"] = f"Bearer {os.environ['HA_TOKEN']}"

    def service(self, domain: str, service: str, entity_id: str, **data: Any) -> Any:
        r = self.session.post(f"{self.url}/api/services/{domain}/{service}",
                              json={"entity_id": entity_id, **data}, timeout=30)
        r.raise_for_status()
        return r.json()

    def template(self, template: str) -> str:
        r = self.session.post(f"{self.url}/api/template", json={"template": template}, timeout=30)
        r.raise_for_status()
        return r.text


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True)
    ap.add_argument("--step", action="append", default=[], help="omit to only watch for --pre seconds")
    ap.add_argument("--restore", action="append", default=[])
    ap.add_argument("--pre", type=float, default=30)
    ap.add_argument("--gap", type=float, default=60)
    ap.add_argument("--post", type=float, default=60)
    ap.add_argument("--host", default=os.environ.get("MQTT_HOST", "localhost"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("MQTT_PORT", "1883")))
    ap.add_argument("--topic-suffix", default=os.environ.get("MQTT_TOPIC_SUFFIX", "/P1P2MQTT/bridge0"),
                    help="topic suffix of the F1F2 bridge")
    ap.add_argument("--p1p2-topic-suffix", default=os.environ.get("P1P2_TOPIC_SUFFIX"),
                    help="topic suffix of an optional second bridge on a P1/P2 bus")
    ap.add_argument("--out-dir", default="captures")
    ap.add_argument("--ha-template", default=os.environ.get("HA_STATUS_TEMPLATE"),
                    help="template rendered after each ha: action ({entity_id} = the entity)")
    args = ap.parse_args()
    f1f2 = args.topic_suffix
    p1p2 = args.p1p2_topic_suffix

    out = os.path.join(args.out_dir, time.strftime(f"exp-{args.name}-%Y%m%d-%H%M%S.tsv"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    fh = open(out, "w", buffering=1)
    last_state: dict[str, str] = {}
    ha: HomeAssistant | None = None

    def w(line: str) -> None:
        fh.write(line + "\n")

    def on_connect(
        c: mqtt.Client, u: Any, f: mqtt.ConnectFlags, rc: mqtt.ReasonCode, p: mqtt.Properties | None = None
    ) -> None:
        topics = [(f"P1P2/R{f1f2}", 0), (f"P1P2/S{f1f2}", 0)]
        if p1p2:
            topics.append((f"P1P2/P{p1p2}/#", 0))
        c.subscribe(topics)

    def on_message(c: mqtt.Client, u: Any, m: mqtt.MQTTMessage) -> None:
        now = time.time()
        txt = m.payload.decode(errors="replace")
        if m.topic.startswith("P1P2/R/"):
            mt = RAW_RE.search(txt)
            if mt:
                w(f"{now:.3f}\t{mt.group(1)}\t{mt.group(2).upper()}")
            return
        if m.topic.startswith("P1P2/S/"):
            if "Uptime" not in txt:
                w(f"{now:.3f}\t\t\t# S F1F2 bridge: {txt.strip()}")
            return
        key = m.topic.split(f"{p1p2}/", 1)[1]
        if key.startswith(("U/", "A/9", "S/9")) or key.startswith("T/1/Temperature_Inside"):
            return
        if last_state.get(key) != txt:
            if key in last_state:
                w(f"{now:.3f}\t\t\t# P1P2 {key}: {last_state[key]} -> {txt}")
            last_state[key] = txt

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"f1f2-exp-{os.getpid()}")
    if os.environ.get("MQTT_USER"):
        c.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASSWORD"))
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(args.host, args.port, 30)
    c.loop_start()

    def mark(s: str) -> None:
        w(f"{time.time():.3f}\t\t\t# MARK {s}")
        print(time.strftime("%H:%M:%S"), s, flush=True)

    def run(action: str) -> None:
        nonlocal ha
        kind, rest = action.split(":", 1)
        if kind == "mqtt":
            if not rest:
                raise ValueError("empty mqtt payload")
            if not p1p2:
                raise ValueError("mqtt: actions need --p1p2-topic-suffix")
            c.publish(f"P1P2/W{p1p2}", rest, qos=1).wait_for_publish(10)
        elif kind == "raw":
            b = bytes.fromhex(rest)
            if len(b) != 5 + b[4]:
                raise ValueError(f"length field mismatch: {len(b)} bytes, len field {b[4]}")
            cs = (-sum(b[1:])) & 0xFF
            frame = (b + bytes([cs])).hex().upper()
            mark(f"RAW W{frame}")
            c.publish(f"P1P2/W{f1f2}", "W" + frame, qos=1).wait_for_publish(10)
        elif kind == "ha":
            parts = rest.split(":", 2)
            domain, service = parts[0].split(".", 1)
            data = json.loads(parts[2]) if len(parts) > 2 else {}
            ha = ha or HomeAssistant()
            res = ha.service(domain, service, parts[1], **data)
            print("   ->", res, flush=True)
            if args.ha_template:
                e = parts[1]
                mark(f"HA {e}: {ha.template(args.ha_template.replace('{entity_id}', e))}")
        else:
            raise ValueError(action)

    sent = False
    try:
        mark(f"start pre={args.pre}")
        time.sleep(args.pre)
        for st in args.step:
            sent = True
            mark(f"CMD {st}")
            run(st)
            time.sleep(args.gap)
    finally:
        if sent:
            for r in args.restore:
                for attempt in range(3):
                    try:
                        mark(f"RESTORE {r}")
                        run(r)
                        break
                    except Exception as e:  # noqa: BLE001 - restore must be retried
                        mark(f"RESTORE FAILED ({e}), retrying")
                        time.sleep(5)
    time.sleep(args.post)
    mark("end")
    c.loop_stop()
    fh.close()
    print("->", out)


if __name__ == "__main__":
    main()
