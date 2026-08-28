"""Per-player Dervish avatar uptime, from the ``model_events`` StoC stream.

A Dervish avatar replaces the player's model, and ``ModelTracker.cpp`` hooks
``GAME_SMSG_UPDATE_AGENT_MODEL`` and decodes ``transmog_npc_id ^ 0x20000000``
into a two-sided state machine with millisecond stamps. So uptime here is
**measured, not modelled**: a transform on, a transform off, and the seconds
between them.

Validated against the skill stream on the recordings that carry both: the
transform count matches the avatar cast count (16 against 16, 20 against 23),
model 7 lines up with Avatar of Balthazar casts and model 10 with Lyssa, and
every transformed agent in the sample is a Dervish.

**The stream landed 2026-08-19 and exists in 44 of 2,366 archived matches.** It
cannot be backfilled -- the older recordings do not contain the packet -- so this
is forward-only in exactly the sense ``player_kills`` is: absent for the archive
rather than zero, and accruing on its own. Estimating it from cast times and a
nominal form duration was considered and rejected: the duration scales with
Mysticism and a form ends early on death, so it would be a model wearing a
measurement's name.
"""
from __future__ import annotations

import gzip
from collections import Counter
from pathlib import Path

from combat_analytics import _player_lookup as player_lookup
from combat_analytics import match_seconds, match_window

SCHEMA_VERSION = 1

# Model ids, from EXPORTS_CONVENTIONS.md:336-385. Model 0 is "not transformed".
AVATAR_MODELS: dict[int, str] = {
    7: "Avatar of Balthazar",
    8: "Avatar of Dwayna",
    9: "Avatar of Grenth",
    10: "Avatar of Lyssa",
    11: "Avatar of Melandru",
}

# MODEL_INIT is the baseline for an agent, MODEL_CHANGE is the packet-driven
# transform, MODEL_RESYNC is the poll noticing one the packet missed. All three
# carry ``agent_id;model_id`` in the same two positions; the tails differ and
# are not read here.
_MODEL_KINDS = ("MODEL_INIT", "MODEL_CHANGE", "MODEL_RESYNC")


def _seconds(header: str) -> float | None:
    try:
        minute, rest = header.split(":", 1)
        second, millis = rest.split(".", 1) if "." in rest else (rest, "0")
        return int(minute) * 60 + int(second) + int(millis) / 1000.0
    except (ValueError, TypeError):
        return None


def read_model_records(match_dir: Path) -> list[tuple[float, int, int]]:
    """(time, agent_id, model_id), or [] when the stream is absent."""
    stoc = match_dir / "StoC"
    path = next((p for p in (stoc / "model_events.txt.gz", stoc / "model_events.txt")
                 if p.is_file()), None)
    if path is None:
        return []
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    records: list[tuple[float, int, int]] = []
    with opener(path, "rt", encoding="utf-8-sig", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            close = line.find("]")
            if not line.startswith("[") or close < 0:
                continue
            when = _seconds(line[1:close])
            if when is None:
                continue
            fields = line[close + 1:].strip().split(";")
            if fields[0] not in _MODEL_KINDS or len(fields) < 3:
                continue
            try:
                records.append((when, int(fields[1]), int(fields[2])))
            except ValueError:
                continue
    return records


def build_avatar_ledger(infos: dict, match_dir: Path) -> dict:
    """Per-player form uptime, or {} when the stream is absent.

    Records what happened and nothing more. Whether a player *counts* as an
    avatar runner is a cohort question and is decided by the consumer, not here.
    """
    records = read_model_records(match_dir)
    if not records:
        return {}
    players = player_lookup(infos)
    if not players:
        return {}

    # The denominator is the MATCH, and the match is a WINDOW inside instance
    # time, not [0, duration]: a GvG starts about a minute after the instance is
    # created. Clipping to the wrong window is not a rounding error -- it read
    # 75.0% for a form worn 92.7% of one match, because every stretch after
    # 332 s was thrown away while the match actually ran to 392 s. Clipping to
    # the right one also makes a share above 100% impossible.
    start, end = match_window(infos)
    if end <= start:
        start, end = 0.0, max(when for when, _agent, _model in records)
    horizon = end - start

    def clip(value: float) -> float:
        return min(max(value, start), end)

    rows = {
        agent_id: {
            "form_seconds": 0,
            "form_transforms": 0,
            "form_id": 0,
            "match_seconds": round(horizon),
        }
        for agent_id in players
    }
    state: dict[int, tuple[float, int]] = {}
    forms: dict[int, Counter] = {}
    untracked = 0
    unknown_models = 0

    for when, agent_id, model_id in sorted(records, key=lambda r: r[0]):
        # Only an avatar counts as avatar uptime. Every one of the 646
        # transforms in the archive today is a Dervish avatar, so this changes
        # nothing now and keeps a future costume or transmog out of the metric.
        if model_id and model_id not in AVATAR_MODELS:
            unknown_models += 1
            model_id = 0
        previous = state.get(agent_id)
        if previous is not None and previous[1] != 0 and agent_id in rows:
            rows[agent_id]["form_seconds"] += max(
                0, round(clip(when) - clip(previous[0])))
        if model_id and (previous is None or previous[1] != model_id):
            if agent_id in rows:
                rows[agent_id]["form_transforms"] += 1
                forms.setdefault(agent_id, Counter())[model_id] += 1
            else:
                untracked += 1
        state[agent_id] = (when, model_id)

    # A form still up when the recording stops ran to the end of the match.
    for agent_id, (when, model_id) in state.items():
        if model_id and agent_id in rows:
            rows[agent_id]["form_seconds"] += max(0, round(end - clip(when)))

    for agent_id, counted in forms.items():
        rows[agent_id]["form_id"] = counted.most_common(1)[0][0]

    # A share cannot exceed its whole. `form_seconds` is a sum of per-interval
    # roundings against one rounded denominator, so it can drift a second or two
    # above it on a short match with many transforms -- and the consumer's own
    # guard would then withhold the metric entirely rather than show 101%.
    for row in rows.values():
        row["form_seconds"] = min(row["form_seconds"], row["match_seconds"])

    output: dict[str, list[dict[str, int]]] = {}
    for agent_id, (party_id, player_number) in players.items():
        output.setdefault(party_id, []).append(
            {"player_number": player_number, **rows[agent_id]}
        )
    for party_rows in output.values():
        party_rows.sort(key=lambda row: row["player_number"])

    return {
        "schema": SCHEMA_VERSION,
        "players": output,
        "attribution": {
            "form_records": len(records),
            "form_agents_transformed": len(forms),
            "form_transforms_untracked_agent": untracked,
            "form_models_not_an_avatar": unknown_models,
            "form_match_seconds": round(horizon),
        },
    }
