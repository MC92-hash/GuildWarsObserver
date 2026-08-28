"""Observed Guild Lord pressure and pit roster analytics."""
from __future__ import annotations
import gzip
from collections import defaultdict
from pathlib import Path
from typing import Iterable

SCHEMA = 1
MODEL_NAMES = {170: "Guild Lord", 172: "Bodyguard", 173: "Footman",
               174: "Knight", 175: "Archer", 176: "Archer",
               168: "Flame Sentinel", 2280: "Bone Horror"}

def _int(value, default=0):
    try: return int(value)
    except (TypeError, ValueError): return default

def parse_lord_events(lines: Iterable[str]) -> list[dict]:
    out = []
    for raw in lines:
        line = raw.strip()
        if not line.startswith("[") or "]" not in line: continue
        stamp, body = line.split("]", 1)
        try:
            minute, sec = stamp[1:].split(":", 1)
            if "." in sec: second, millis = sec.split(".", 1)
            else: second, millis = sec, "0"
            time = int(minute) * 60 + int(second) + int(millis) / 1000
        except ValueError: continue
        fields = [x.strip() for x in body.strip().split(";")]
        if len(fields) < 9 or fields[0] != "LORD_DAMAGE": continue
        out.append({"time": time, "caster_id": _int(fields[1]),
                    "target_id": _int(fields[2]), "value": fields[3],
                    "damage_type": _int(fields[4]), "attacking_team": _int(fields[5]),
                    "damage": _int(fields[6]), "damage_before": _int(fields[7]),
                    "damage_after": _int(fields[8])})
    return out

def _read(path: Path) -> list[dict]:
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8-sig", errors="replace") as handle:
        return parse_lord_events(handle)

def build_lord_pressure(infos: dict, match_dir: Path) -> dict:
    lord_path = next((p for p in (match_dir / "StoC" / "lord_events.txt.gz",
                                  match_dir / "StoC" / "lord_events.txt") if p.is_file()), None)
    parties = infos.get("parties") if isinstance(infos, dict) else None
    if not isinstance(parties, dict): return {}
    roster, audit = {}, defaultdict(int)
    for party_id, party in parties.items():
        others = party.get("OTHER", []) if isinstance(party, dict) else []
        rows = []
        for npc in others:
            if not isinstance(npc, dict): continue
            model = _int(npc.get("model_id"))
            name = MODEL_NAMES.get(model)
            if name is None:
                audit["unclassified_other"] += 1; continue
            row = {"agent_id": _int(npc.get("id")), "model_id": model, "name": name}
            if "deaths" in npc: row["deaths"] = _int(npc.get("deaths"))
            rows.append(row)
        if rows: roster[str(party_id)] = rows
    if lord_path is None: return {"schema": SCHEMA, "roster": roster, "audit": dict(audit)} if roster else {}
    events = _read(lord_path)
    ledgers = defaultdict(lambda: {"damage_packets_sum": 0, "damage_counter_final": 0,
                                   "largest_packet": 0, "lead_attackers": defaultdict(int),
                                   "events": []})
    for event in events:
        team = str(event["attacking_team"])
        ledger = ledgers[team]
        ledger["damage_packets_sum"] += max(0, event["damage"])
        ledger["damage_counter_final"] = max(ledger["damage_counter_final"], event["damage_after"])
        ledger["largest_packet"] = max(ledger["largest_packet"], max(0, event["damage"]))
        ledger["lead_attackers"][str(event["caster_id"])] += max(0, event["damage"])
        ledger["events"].append(event)
    out = {}
    for team, ledger in ledgers.items():
        events = ledger.pop("events"); by_attacker = ledger.pop("lead_attackers")
        lead = max(by_attacker, key=lambda key: (by_attacker[key], -int(key))) if by_attacker else None
        values = [event["damage"] for event in events]
        ledger.update({"lead_attacker_id": _int(lead) if lead is not None else None,
                       "first_pressure": values[0] if values else 0,
                       "final_pressure": values[-1] if values else 0,
                       "event_count": len(events)})
        out[team] = ledger
    for ledger in out.values():
        if ledger["damage_packets_sum"] != ledger["damage_counter_final"]:
            audit["counter_packet_disagreement"] += 1
    return {"schema": SCHEMA, "teams": out, "roster": roster, "audit": dict(audit)}
