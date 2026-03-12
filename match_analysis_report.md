# Match Recording Precision Analysis

**Date**: March 11, 2026  
**Reference match (working)**: `SF_OG_ROOK` — recorded August 23, 2025  
**Broken match (choppy)**: `Match_20260216_225913` — recorded February 16, 2026

---

## Step 1 — Folder Inventory

Both matches share identical structure: `infos.json`, `Agents/*.txt.gz`, `StoC/*.txt.gz`.  
Same StoC file types present in both.

**Differences:**

- The broken match has an extra `StoC/unknown_events.txt.gz` (13 KB) not present in the reference.
- The broken match's `infos.json` includes per-player combat stats (`total_damage`, `attacks_started`, `crits_dealt`, `deaths`, `kills`, etc.) which the reference does not have — this confirms a **newer capture software version**.

---

## Step 2 — Timestamp Precision

### Agent Position Data

| Metric              | REF (SF_OG_ROOK) | BRK (Match_20260216) | Ratio            |
|---------------------|-------------------|----------------------|------------------|
| Lines per player    | ~6,300            | ~490                 | **13x fewer**    |
| Min delta           | 111 ms            | 167 ms               | ~same ballpark   |
| Avg delta           | 290–410 ms        | 650–1,086 ms         | **2–3x slower**  |
| Effective rate      | 2.4–3.4 Hz        | 0.6–1.5 Hz           | **3–5x slower**  |
| Duration            | 1,607s (~27 min)  | 906s (~15 min)       | different match  |
| Timestamp format    | `[MM:SS.mmm]`     | `[MM:SS.mmm]`        | **same**         |

Agent files still have millisecond timestamps, but far fewer snapshots are recorded.

### StoC Event Files — THE SMOKING GUN

| File                    | REF format      | BRK format   |
|-------------------------|-----------------|--------------|
| skill_events            | `[00:00.590]`   | `[00:01]`    |
| combat_events           | `[00:00.590]`   | `[00:01]`    |
| agent_events            | `[00:00.590]`   | `[00:01]`    |
| basic_attack_events     | `[00:00.590]`   | `[00:01]`    |
| attack_skill_events     | `[00:00.590]`   | `[00:01]`    |

**All StoC files in the broken match use whole-second timestamps with NO decimal component.**

### Raw Evidence

Reference skill events:

```
[00:00.590] SKILL_ACTIVATED;3205;29;0
[00:06.622] SKILL_ACTIVATED;1774;87;0
[00:06.678] INSTANT_SKILL_USED;349;62;62
[00:06.800] INSTANT_SKILL_USED;1037;58;58
```

Broken skill events:

```
[00:01] SKILL_ACTIVATED;3205;48;0
[00:04] INSTANT_SKILL_USED;1043;86;86
[00:06] INSTANT_SKILL_USED;1544;85;85
[00:08] SKILL_ACTIVATED;1521;87;0
```

---

## Step 3 — Coordinate Data Structure

Both use identical schema: `[timestamp] X;Y;Z;rotation;...45 more fields`.

| Metric                 | REF                    | BRK                    |
|------------------------|------------------------|------------------------|
| Coordinate type        | Float (3 decimals)     | Float (3 decimals)     |
| Has fractional coords  | Yes                    | Yes                    |
| Field count per line   | ~48 semicolon-separated| ~48 semicolon-separated|

Coordinates are **not quantized** — both have full float precision. The schema is identical.

---

## Step 4 — Skill Event Structure

Both have the same event types (`SKILL_ACTIVATED`, `SKILL_FINISHED`, `INSTANT_SKILL_USED`) and same field structure (`event_type;skill_id;agent_id;target_id`).

No structural change in event data. The **only** difference is the timestamp format: `[MM:SS.mmm]` vs `[MM:SS]`.

---

## Step 5 — File Size and Density

| Metric                 | REF       | BRK       | Ratio              |
|------------------------|-----------|-----------|--------------------|
| Total size             | 3.41 MB   | 0.55 MB   | **6.2x smaller**   |
| Agents dir             | 2.64 MB   | 0.21 MB   | **12.6x smaller**  |
| StoC dir               | 0.76 MB   | 0.30 MB   | **2.5x smaller**   |
| Agent snapshots/sec    | 3.9       | 0.6       | **6.5x fewer**     |
| Skill events/sec       | 5.2       | N/A       | —                  |
| Agent events/sec       | 33.6      | N/A       | —                  |

(StoC events/sec cannot be computed for the broken match because timestamps lack millisecond precision.)

---

## Step 6 — Metadata and Version Detection

The broken match `infos.json` has **additional fields** per player not present in the reference:

```
total_damage, attacks_started, attacks_finished, attacks_stopped,
skills_activated, skills_finished, skills_stopped,
attack_skills_activated, attack_skills_finished, attack_skills_stopped,
interrupted_count, interrupted_skills_count,
cancelled_attacks_count, cancelled_skills_count,
crits_dealt, crits_received, deaths, kills
```

This confirms a **different/newer version of the capture tool**.

No explicit `version`, `capture_rate`, or `sample_rate` field exists in either file.

---

## Step 7 — Conclusion

### 1. Timestamp precision

- **Reference**: millisecond precision (`[MM:SS.mmm]`) in all files
- **Broken**: millisecond in Agent files, **whole-second only** (`[MM:SS]`) in all StoC event files

### 2. Position update rate

- **Reference**: ~3–4 Hz (250–350 ms average delta)
- **Broken**: ~0.6–1.5 Hz (650–1100 ms average delta)

### 3. Schema changes between versions

1. StoC timestamp format changed from `[MM:SS.mmm]` to `[MM:SS]`
2. `infos.json` gained per-player combat stat fields
3. New `unknown_events.txt.gz` file added
4. Agent snapshot data format is identical but captured far less frequently

### 4. Root cause

**The capture tool was updated after December 2025.** The new version:

- **(b) Changed the StoC timestamp format** — stripping milliseconds from all event timestamps, rounding to whole seconds
- **(a) Reduced the agent snapshot capture rate** — from ~3–4 Hz down to ~1 Hz

This is **not** compression or quantization of coordinates (those are still float). It is a **capture-side** change in how timestamps are written and how often snapshots are sampled.

### 5. Fix needed

The fix must happen in the **capture tool** (not the observer/replay program):

1. **Restore millisecond precision in StoC event timestamps** — change the timestamp formatter from `[MM:SS]` back to `[MM:SS.mmm]`
2. **Restore the agent snapshot polling rate** — increase from ~1 Hz back to ~3–4 Hz minimum
