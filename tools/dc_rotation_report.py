#!/usr/bin/env python3
"""How the bots' class rotations played out in `.dc test` runs (the rotation census, record schema 14+).

    dc_rotation_report.py                      every census run in the vanilla realm's records
    dc_rotation_report.py tr-20261007-201500   runs whose id starts with this (a run, or a batch)
    dc_rotation_report.py --last 10            the last 10 census runs
    dc_rotation_report.py --class rogue        one class
    dc_rotation_report.py --per-run            one block per party member instead of per class/spec

Per class and spec, summed over the runs: each spell's casts and its share of the casts, how many casts the
server refused and the most common reason (a spell that keeps failing for the same reason is a rotation
reaching for something it can't use: wrong stance or form, out of range, no reagent), and the class
abilities the bots knew but never cast.

The records are dc_testruns.jsonl in the worldserver's working directory: env/dist/realms/<realm>/ on
the progressive realms (--realm), or any directory with --data-dir / $DC_DATA_DIR.
"""

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

RUNS_FILE = "dc_testruns.jsonl"
HERE = Path(__file__).resolve().parent


def die(msg):
    print(f"dc_rotation_report: {msg}", file=sys.stderr)
    sys.exit(2)


def find_runs_file(args):
    if args.data_dir or os.environ.get("DC_DATA_DIR"):
        path = Path(args.data_dir or os.environ["DC_DATA_DIR"]).expanduser() / RUNS_FILE
        if not path.exists():
            die(f"no {path}")
        return path
    for start in [HERE, Path.cwd().resolve()]:
        for cand in [start, *start.parents]:
            for hit in [cand / "env" / "dist" / "realms" / args.realm, cand / "env" / "dist" / "bin", cand]:
                if (hit / RUNS_FILE).exists():
                    return hit / RUNS_FILE
    die(f"could not find {RUNS_FILE}; pass --data-dir (the worldserver's working directory)")


def cast_result_names():
    """SpellCastResult codes to names, read from the core's SharedDefines.h."""
    for cand in [HERE, *HERE.parents]:
        header = cand / "src" / "server" / "shared" / "SharedDefines.h"
        if header.exists():
            text = header.read_text(errors="replace")
            body = text[text.index("enum SpellCastResult"):]
            body = body[:body.index("};")]
            return {int(v, 0): n.replace("SPELL_FAILED_", "").lower()
                    for n, v in re.findall(r"(SPELL_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", body)}
    return {}


def load_runs(path, prefix, last):
    runs = []
    with open(path, errors="replace") as f:
        for line in f:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get("schema", 0) < 14 or not any(c.get("rotation") for c in rec.get("comp", [])):
                continue
            if prefix and not rec.get("runId", "").startswith(prefix):
                continue
            runs.append(rec)
    return runs[-last:] if last else runs


def print_block(title, runs, casts, failed, reasons, unused, members, names):
    total = sum(casts.values())
    print(f"\n== {title}  ({members} member{'s' if members != 1 else ''} over {runs} run{'s' if runs != 1 else ''}, "
          f"{total} casts)")
    if not casts and not failed:
        print("   no spells cast or tried")
    rows = sorted(set(casts) | set(failed), key=lambda s: (-casts[s], -failed[s], s))
    for spell in rows:
        share = f"{100.0 * casts[spell] / total:5.1f}%" if total else "    -"
        line = f"   {casts[spell]:6d} {share}  {spell}"
        if failed[spell]:
            reason, count = reasons[spell].most_common(1)[0]
            line += f"   (failed {failed[spell]}x, mostly {names.get(reason, reason)} {count}x)"
        print(line)
    never = sorted(s for s, n in unused.items() if n == members)
    sometimes = sorted(s for s, n in unused.items() if n < members)
    if never:
        print(f"   known, never cast: {', '.join(never)}")
    if sometimes:
        print(f"   known, not cast by every member: {', '.join(sometimes)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix", nargs="?", help="run id (or id prefix) to include")
    ap.add_argument("--last", type=int, default=0, help="only the last N census runs")
    ap.add_argument("--class", dest="cls", help="only this class (warrior, rogue, ...)")
    ap.add_argument("--per-run", action="store_true", help="one block per party member")
    ap.add_argument("--realm", default="vanilla", help="progressive realm whose records to read (default vanilla)")
    ap.add_argument("--data-dir", help="directory holding dc_testruns.jsonl")
    args = ap.parse_args()

    path = find_runs_file(args)
    runs = load_runs(path, args.prefix, args.last)
    if not runs:
        die(f"no runs with a rotation census in {path}")
    names = cast_result_names()
    print(f"{len(runs)} run(s) from {path}")

    groups = defaultdict(lambda: {"runs": set(), "members": 0, "casts": Counter(), "failed": Counter(),
                                  "reasons": defaultdict(Counter), "unused": Counter()})
    for rec in runs:
        for member in rec.get("comp", []):
            if args.cls and member.get("class") != args.cls.lower():
                continue
            if args.per_run:
                key = (f"{rec['runId']} {member.get('name')} - {member.get('class')} {member.get('spec')} "
                       f"({member.get('role')}, level {member.get('level')}) in {rec.get('dungeonName')}")
            else:
                key = f"{member.get('class')} / {member.get('spec')} ({member.get('role')})"
            g = groups[key]
            g["runs"].add(rec["runId"])
            g["members"] += 1
            for spell in member.get("rotation", []):
                g["casts"][spell["name"]] += spell["casts"]
                g["failed"][spell["name"]] += spell.get("failed", 0)
                if spell.get("failed"):
                    g["reasons"][spell["name"]][spell["topFail"]] += spell["topFailCount"]
            for spell in member.get("unusedSpells", []):
                g["unused"][spell] += 1

    for key in sorted(groups):
        g = groups[key]
        print_block(key, len(g["runs"]), g["casts"], g["failed"], g["reasons"], g["unused"], g["members"], names)


if __name__ == "__main__":
    main()
