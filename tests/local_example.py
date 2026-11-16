#!/usr/bin/env python3
"""Write a copy of example.yaml that uses this checkout instead of GitHub.

example.yaml loads the component and the packages from the GitHub repository.
To test changes before they are pushed (and in CI), this writes the same config
with the component as a local source and every package as a local !include, and
turns on the commented-out hydrobox so its package is tested too.

  python3 tests/local_example.py build/ci
  esphome config build/ci/example.yaml
"""
from __future__ import annotations

import pathlib
import re
import shutil
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = pathlib.Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)

    s = (REPO / "example.yaml").read_text()
    s = s.replace("  - source: github://casdr/esphome-daikin-f1f2@main",
                  f"  - source: {{type: local, path: {REPO / 'components'}}}")
    s = s.replace("      # - path: packages/hydrobox.yaml\n      #   vars:",
                  "      - path: packages/hydrobox.yaml\n        vars:")
    head, sep, files = s.partition("packages:\n  daikin:\n")
    if not sep:
        sys.exit("example.yaml: no 'packages: daikin:' block found")
    items = re.findall(r"- path: (\S+)(?:\n\s+vars: (\{[^}]*\}))?", files)
    lines = ["packages:\n"]
    for i, (path, vars_) in enumerate(items):
        inc = f"file: {REPO / path}" + (f", vars: {vars_}" if vars_ else "")
        lines.append(f"  p{i}: !include {{{inc}}}\n")
    (out / "example.yaml").write_text(head + "".join(lines))
    secrets = out / "secrets.yaml"
    if not secrets.exists():
        shutil.copy(REPO / "secrets.yaml.example", secrets)
    print(f"{out / 'example.yaml'}: {len(items)} packages")


if __name__ == "__main__":
    main()
