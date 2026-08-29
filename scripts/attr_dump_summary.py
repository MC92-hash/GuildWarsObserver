#!/usr/bin/env python3
"""Summarise GW Observer attribute-solver dumps across many matches.

The app writes ``%TEMP%\\gwo_attributes_<match>.txt`` on load when ``GWO_ATTR_DEBUG=1`` is set in
the environment (see ``ReplayWindow::WriteAttributeDebugDump`` and
``AttributeModel::WriteDebugDump``). One dump is readable by eye; the yield of a change is only
visible across dozens of them. This script reduces a folder of dumps to two tables:

* per (match, player, attribute): the reported range, the best rank, the confidence, whether the
  range is budget-only, how many observations back it, which genres spoke, how many were flagged
  suspicious or are floors, and how many contradictions the player has;
* per genre: how many (player, attribute) readings it contributed to, over all matches.

Usage::

    python scripts/attr_dump_summary.py                       # every dump in %TEMP%
    python scripts/attr_dump_summary.py path/to/dumps/*.txt   # explicit files
    python scripts/attr_dump_summary.py --csv out.csv         # also write the per-attribute table

Developer tool, not part of the build or the publish pipeline.
"""

import argparse
import collections
import csv
import glob
import os
import re
import sys
import tempfile

HEADER_RE   = re.compile(r"^(?P<name>.+?)\s+\(agent (?P<agent>\d+), prof (?P<p>\d+)/(?P<s>\d+)\)$")
ATTR_RE     = re.compile(r"^\s{4}(?P<attr>[^:]+): (?P<range>\S+)\s+best=(?P<best>\d+)"
                         r"(?P<budget>\s+\(budget only\))?\s+confidence=(?P<conf>[\d.]+)")
# Evidence lines carry the attribute they speak for in their own "-> Attribute ranks" tail, which
# is what they are keyed by: dumps written before the per-attribute layout list them under the
# player rather than under the attribute, and both shapes then read the same way.
EVIDENCE_RE = re.compile(r"^\s{4,}\[(?P<genre>[^\]]+)\] (?P<text>.*?) -> (?P<floor>at least )?"
                         r"(?P<attr>.+?) (?P<ranks>\S+)\s+w=(?P<w>[\d.]+)\s*$")
COUNT_RE    = re.compile(r"\(x(?P<n>\d+)\)")
POINTS_RE   = re.compile(r"^\s{2}points spent: (?P<lo>\d+)-(?P<hi>\d+)\s+feasible builds: (?P<n>\d+)")

PROF = {1: "W", 2: "R", 3: "Mo", 4: "N", 5: "Me", 6: "E", 7: "A", 8: "Rt", 9: "P", 10: "D"}


def match_name(path):
    base = os.path.basename(path)
    if base.startswith("gwo_attributes_"):
        base = base[len("gwo_attributes_"):]
    return base[:-4] if base.endswith(".txt") else base


def parse_dump(path):
    """Yield one dict per (player, attribute) block of a dump."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()

    player = None
    attr = None
    rows = []
    by_key = {}   # (agent, attribute name) -> row, so evidence can precede or follow its header
    contradictions = collections.Counter()
    in_contradictions = False

    def row_for(attr_name):
        key = (player["agent"], attr_name)
        if key not in by_key:
            by_key[key] = {
                "match": match_name(path),
                "player": player["name"],
                "agent": player["agent"],
                "prof": player["prof"],
                "attribute": attr_name,
                "range": "?",
                "best": -1,
                "budget_only": False,
                "confidence": 0.0,
                "observations": 0,
                "suspicious": 0,
                "floors": 0,
                "genres": collections.Counter(),
                "points": player["points"],
                "feasible": player["feasible"],
                "_agent": player["agent"],
            }
            rows.append(by_key[key])
        return by_key[key]

    for line in lines:
        if line.startswith("====="):
            player, attr, in_contradictions = None, None, False
            continue
        m = HEADER_RE.match(line)
        if m and player is None and not line.startswith(" "):
            player = {
                "name": m.group("name").strip(),
                "agent": int(m.group("agent")),
                "prof": f"{PROF.get(int(m.group('p')), '?')}/{PROF.get(int(m.group('s')), '?')}",
                "points": "", "feasible": "",
            }
            continue
        if player is None:
            continue

        m = POINTS_RE.match(line)
        if m:
            player["points"] = f"{m.group('lo')}-{m.group('hi')}"
            player["feasible"] = m.group("n")
            continue

        if line.startswith("  contradictions"):
            in_contradictions = True
            attr = None
            if line.strip().endswith("none"):
                in_contradictions = False
            continue
        if in_contradictions:
            if line.startswith("    "):
                contradictions[player["agent"]] += 1
            continue

        m = EVIDENCE_RE.match(line)
        if m:
            row = row_for(m.group("attr").strip())
            text = m.group("text")
            n = COUNT_RE.search(text)
            count = int(n.group("n")) if n else 1
            row["observations"] += count
            row["genres"][m.group("genre")] += count
            if text.startswith("suspicious:"):
                row["suspicious"] += count
            if m.group("floor"):
                row["floors"] += count
            continue

        m = ATTR_RE.match(line)
        if m:
            attr = row_for(m.group("attr").strip())
            attr["range"] = m.group("range")
            attr["best"] = int(m.group("best"))
            attr["budget_only"] = bool(m.group("budget"))
            attr["confidence"] = float(m.group("conf"))
            continue

    for r in rows:
        r["contradictions"] = contradictions.get(r["_agent"], 0)
        del r["_agent"]
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dumps", nargs="*", help="dump files or globs (default: %%TEMP%%\\gwo_attributes_*.txt)")
    ap.add_argument("--csv", help="write the per-attribute table to this CSV file")
    ap.add_argument("--budget", action="store_true", help="include budget-only attributes in the listing")
    args = ap.parse_args()

    patterns = args.dumps or [os.path.join(tempfile.gettempdir(), "gwo_attributes_*.txt")]
    files = sorted({f for p in patterns for f in glob.glob(p)})
    if not files:
        print("no dumps found", file=sys.stderr)
        return 1

    rows = []
    for f in files:
        rows.extend(parse_dump(f))

    # Per-genre yield: how many (match, player, attribute) readings each genre took part in, and
    # how many observations it contributed.
    genre_readings = collections.Counter()
    genre_obs = collections.Counter()
    for r in rows:
        for g, n in r["genres"].items():
            genre_readings[g] += 1
            genre_obs[g] += n

    measured = [r for r in rows if not r["budget_only"]]
    print(f"dumps: {len(files)}   players: {len({(r['match'], r['agent']) for r in rows})}   "
          f"attribute readings: {len(measured)} measured, {len(rows) - len(measured)} budget-only")
    print()
    print(f"{'genre':<26}{'readings':>10}{'observations':>14}")
    for g, n in genre_readings.most_common():
        print(f"{g:<26}{n:>10}{genre_obs[g]:>14}")
    print()

    listing = rows if args.budget else measured
    print(f"{'match':<44}{'player':<24}{'prof':<7}{'attribute':<22}{'range':<8}{'best':>4}"
          f"{'conf':>6}{'obs':>6}{'susp':>5}{'floor':>6}{'contr':>6}  genres")
    for r in listing:
        genres = ", ".join(f"{g}:{n}" for g, n in r["genres"].most_common())
        print(f"{r['match'][:43]:<44}{r['player'][:23]:<24}{r['prof']:<7}{r['attribute'][:21]:<22}"
              f"{r['range']:<8}{r['best']:>4}{r['confidence']:>6.2f}{r['observations']:>6}"
              f"{r['suspicious']:>5}{r['floors']:>6}{r['contradictions']:>6}  {genres}")

    if args.csv:
        fields = ["match", "player", "agent", "prof", "attribute", "range", "best", "budget_only",
                  "confidence", "observations", "suspicious", "floors", "contradictions",
                  "points", "feasible", "genres"]
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=fields)
            w.writeheader()
            for r in rows:
                out = dict(r)
                out["genres"] = ";".join(f"{g}:{n}" for g, n in r["genres"].most_common())
                w.writerow({k: out[k] for k in fields})
        print(f"\nwrote {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
