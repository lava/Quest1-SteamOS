#!/usr/bin/env python3
"""Apply Android audio_route mixer settings with amixer.

Usage: mixer-paths.py [-c CARD] [-n] MIXER_PATHS_XML [PATH ...]

Applies the initial settings (the <ctl> elements directly under <mixer>), then each named <path>
in order, with nested <path name=".."/> references expanded, as Android's libaudioroute does.
Controls with an "id" (one element of a multi-value control) are skipped: on the Quest they are
voice-call settings only. -n prints the amixer commands instead of running them.
Installed as /usr/local/lib/quest1/mixer-paths.py, used by quest1-audio.
"""

import argparse
import subprocess
import sys
import xml.etree.ElementTree as ET


def ctl_value(value: str) -> str:
    """'0 0' -> '0,0' for multi-value integer controls; enum names stay as they are."""
    parts = value.split()
    if len(parts) > 1 and all(p.lstrip("-").isdigit() for p in parts):
        return ",".join(parts)
    return value


def expand(paths: dict[str, ET.Element], name: str, depth: int = 0) -> list[tuple[str, str]]:
    if depth > 8:
        raise ValueError(f"path nesting too deep at {name!r}")
    if name not in paths:
        raise KeyError(f"no path {name!r}")
    out: list[tuple[str, str]] = []
    for el in paths[name]:
        if el.tag == "path":
            out += expand(paths, el.attrib["name"], depth + 1)
        elif el.tag == "ctl" and "id" not in el.attrib:
            out.append((el.attrib["name"], el.attrib["value"]))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-c", "--card", default="0")
    ap.add_argument("-n", "--dry-run", action="store_true")
    ap.add_argument("xml")
    ap.add_argument("paths", nargs="*")
    args = ap.parse_args()

    root = ET.parse(args.xml).getroot()
    paths = {p.attrib["name"]: p for p in root.findall("path")}
    ctls = [(c.attrib["name"], c.attrib["value"]) for c in root.findall("ctl") if "id" not in c.attrib]
    for name in args.paths:
        ctls += expand(paths, name)

    failed = 0
    for name, value in ctls:
        cmd = ["amixer", "-q", "-c", args.card, "cset", f"name={name}", ctl_value(value)]
        if args.dry_run:
            print(" ".join(cmd))
        elif subprocess.run(cmd, stderr=subprocess.DEVNULL).returncode != 0:
            failed += 1
    print(f"mixer-paths: {len(ctls) - failed} controls set, {failed} not present", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
