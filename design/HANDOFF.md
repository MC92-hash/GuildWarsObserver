# gwobserver.com — Match Card Redesign / Handoff

**Reference implementation:** `Match Browser.dc.html` (opens directly in a browser).
**Target file in production:** `SourceFiles/draw_replay_browser.cpp` (the card gallery in the replay browser).
This document describes the redesigned browse-view match card so it can be ported into the
native ImGui renderer. All values below are the ones in the reference file as it now stands.

---

## 1. Theme / CI — Watchtower (theme index 1) — MANDATORY
The redesign is locked to the **Watchtower** palette already defined in `ApplyBrowserTheme(1)`.
**Do not introduce any color outside this set.** (Earlier green/blue winner styling was wrong and
has been removed.)

| Role | Hex | Existing token |
|---|---|---|
| Background | `#18181b` (zinc-950) | `kColorBg` |
| Panel / card | `#1c1c1f` | `s_themeColors.cardBg` |
| Card head / inset | `#161618` | — |
| Border (soft) | `#27272a` / `#3f3f46` | `kColorBorder` |
| Accent (amber) | `#f59e0b` | `kColorAccent` |
| Accent bright | `#fbbf24` | — |
| Text | `#e4e4e7` (zinc-200) | `kColorText` / `kCardGuildName` |
| Text dim | `#a1a1aa` (zinc-400) | `kCardMapName` |
| Text faint | `#71717a` (zinc-500) | `kCardVS` |
| Guild tag | amber @72% | `kCardGuildTag` |
| Build / strategy | amber | `kCardBuildName` |

Amber is the single accent — used only for: wordmark, guild tags, VS, the winner, build/strategy
pills, the clock icon, and active controls (density toggle, refresh, search focus).

---

## 2. Card layout — matchup grid
Card body is a 3-column grid **`left team | VS | right team`** (`1fr auto 1fr`).
- Left team left-aligned; right team right-aligned (tag then name) — mirrored matchup read.
- **Card head:** map thumbnail + map name + date on the left; duration (clock) + rank badge right.
- **Footer:** a right-aligned `Details ›` link (the earlier "{winner} won" text line was removed —
  the winner is already unambiguous from color + tint).

### Card head sizes (current)
- Map thumbnail placeholder: **135 × 86 px**, radius 6px, **3px** border `#3f3f46`, bg `#26262a`.
  This is a stand-in — production will drop the **real map screenshot** in here (see §7).
- Map name: **17px**, weight 600, `#d4d4d8`, single line + ellipsis.
- Date: **14px** mono, `#a1a1aa`.
- Duration: **16px** mono `#d4d4d8`, with an amber `#f59e0b` clock glyph.
- Rank badge: **12px** mono, `white-space:nowrap` (must never wrap to two lines).

---

## 3. No truncation (was the #1 bug)
- Team names use a **2-line clamp**, never single-line ellipsis:
  `-webkit-line-clamp:2; -webkit-box-orient:vertical; overflow:hidden; overflow-wrap:anywhere;`
- Guild tags and the comp row wrap instead of clipping — nothing is cut mid-word or mid-bracket.

---

## 3b. Type scale — EXACT px (single source of truth)
⚠ In the reference HTML, the team-name, comp-text, and icon sizes are written as template
variables (`{{ nameSize }}`, `{{ compFont }}`, `{{ iconPx }}`), NOT literal px — do not try to read
their value out of the markup. Use the numbers in this table.

| Element | Size (px) | Weight | Color | Notes |
|---|---|---|---|---|
| Header wordmark "MATCHES" | 15 | 700 | `#f59e0b` | letter-spacing 1.5 |
| Header count "15 / 1576" | 13 | 400 | `#71717a` | mono |
| Map name | 17 | 600 | `#d4d4d8` | 1 line + ellipsis |
| Date | 14 | 400 | `#a1a1aa` | mono |
| Duration | 16 | 400 | `#d4d4d8` | mono, amber clock |
| Rank badge | 12 | 600 | per tier | mono, nowrap |
| **Team name** | **16** | 700 win / 500 lose | `#fbbf24` win / `#a1a1aa` lose | now fixed at 16 |
| Guild tag `[..]` | 15.5 | 400 | amber @72% | mono |
| **Comp count** | **14** | 600 | `#d4d4d8` | mono, tweakable 11–20 |
| **Profession icon** | **20 × 20** | — | — | tweakable 14–30 |
| Strategy pill | 13.5 | 400 | `#f59e0b` | mono, dashed border |
| VS | 15 | 600 | `#f59e0b` | mono |
| Details link | 16 | 400 | `#a1a1aa` | — |
| Tier legend code | 10 | 600 | per tier | mono |

These are the current values as tuned in the reference. The team-name / comp / icon sizes started as
tweakable defaults (13.5 / 14 / 20) — team name has since been fixed at 16px; comp and icon remain
adjustable in the app (see §8). Implement the px values above as the shipping sizes.

---

## 3c. Fonts — two typefaces
The design uses two faces. The reference names them Chakra Petch (UI) and IBM Plex Mono (tabular);
**in the app these map to Roboto (UI) and Roboto Mono (tabular).**

- **UI / sans** — **Roboto** (app **font index 2**, Regular + Bold from the font table).
  Use for: team names, map name, header wordmark/labels, buttons, Details link.
- **Monospace / tabular** — **Roboto Mono** (add as a second `ImFont`; OFL-licensed, one .ttf).
  Use for the "mono" rows in the table above: comp counts, guild tags, date, duration, VS,
  rank badge, strategy pills, header count. Monospace matters most on the **numeric** ones
  (comp counts, duration, rank, totals) — tabular digits keep the columns aligned.

Why the current build looks off: every size/weight/color already matches, but the 7 mono elements
fall back to the sans (Roboto) because no monospace font is loaded — so digits don't align. Fix by
loading Roboto Mono and rendering those elements with it.

Fallback if you don't want a second font: render the mono elements in Roboto (index 2). Sizes/
weights/colors still match; you only lose digit alignment in the numeric columns.

---

## 4. Winner treatment (amber, in CI)
- Winner name: amber bright `#fbbf24`, weight 700. Loser: dim `#a1a1aa`, weight 500.
- Winner's half of the card gets a subtle amber vertical gradient:
  `linear-gradient(180deg, rgba(245,158,11,0.10) 0%, rgba(245,158,11,0.03) 100%)`.

---

## 5. Profession comp → real GW icons + count, ordered
- Parse the comp string (`"1D/2Me/3Mo/2P"`) into `{count, prof}` tokens.
- Render each token as **profession icon + count** (icon left, count right; count `#d4d4d8`, weight 600).
- **Icons are the game's own textures** — production already loads these via
  `GetProfessionIcon(profId)` from `Textures/professions/{id}.png` (also `Professions_Icons/`).
  The reference copied `1.png`–`10.png` into `prof/`.
- **Abbreviation → profession id** (`PROF_ID`):
  `W 1 · R 2 · Mo 3 · N 4 · Me 5 · E 6 · A 7 · Rt 8 · P 9 · D 10`
- **Display order** (`PROF_ORDER`) — melees first, then casters:
  `W, D, A, R, P` (Warrior, Dervish, Assassin, Ranger, Paragon) then
  `E, N, Me, Mo, Rt` (Elementalist, Necromancer, Mesmer, Monk, Ritualist).
  Sort every comp by this order before rendering.

---

## 6. Rank tiers — legend + ordered ramp (in CI)
Legend bar under the header. Ramp uses amber intensity for prestige, zinc for lower — **no
gold/silver/bronze/red**:
- `A AT` — filled amber `rgba(245,158,11,0.9)`, dark text `#18181b`
- `B AT` — transparent, amber outline `#f59e0b`, amber text
- `C AT` — faint amber tint `rgba(245,158,11,0.13)`, text `#e0a24e`
- `Scrim` — transparent, zinc outline `#3f3f46`, text `#71717a`

**Confirm tier order** — this assumes A > B > C. Flip the ramp if your ordering differs.

---

## 7. Header controls
- **Search** (new): free-text filter over team name, guild tag, and map.
- Sort control, grid density toggle (2/3/4 columns, live), refresh — as today.
- Strategy tag ("Spiritway", "Necro Balance") kept, styled as a dashed amber pill.

---

## 8. Tweakable parameters
The reference exposes three live typography controls (in the app these are host Tweaks;
in production make them settings / preference values):
- **Team name size** — 12–20px (default 13.5)
- **Comp text size** — 11–20px (default 14)
- **Profession icon size** — 14–30px (default 20)

---

## Data contract
```
map: string
date: string
duration: string        // "mm:ss"
rank: "A AT" | "B AT" | "C AT" | "Scrim"
teams: [teamA, teamB]   // exactly 2
team = {
  name: string
  tag: string           // guild tag, shown as [tag]
  comp: string          // "1D/2Me/3Mo/2P" (or per-player profession ids)
  won: boolean          // exactly one team per match is true
  strategy?: string     // optional label
}
```

## Open decisions
1. **Tier order** — confirm A > B > C.
2. **Map thumbnail** — placeholder at 135×86; wire the real map screenshot in (§2/§7).
3. Removed the empty 5-star rating row from the browse view — belongs on the match detail page.

---

## Follow-up fixes (post first port)

### ⚠ REVISED — the card is too TALL (empty band between header and teams)
Symptom (still present after the first attempt): each card reserves far more vertical height than
its content needs. The head (map/date/duration/rank) sits at the top, then there is a large **empty
dark band**, and the team names + comps sit in the lower-middle, with the `Details ›` link at the
bottom. Making the winner tint full-height did NOT fix this — it just painted the empty band.

**Root cause is card height, not the tint.** Somewhere the card/child is given a fixed or minimum
height (a constant, or sized to the tallest possible content, or the matchup region has its own tall
fixed height) that is bigger than what a 2-team matchup actually needs, and the matchup row is then
centered in that space.

**Fix:**
- Remove the fixed/min card height. The card height must be driven by its content:
  `head + matchup body + footer`, with no extra vertical space.
- Do NOT vertically center the matchup in a tall box. Stack top-to-bottom: head, then matchup
  (its natural height), then footer.
- The two team halves size to their content (name row + comp row); the winner's amber tint spans
  exactly that matchup region (which is now the correct, shorter height) — full-height of the
  matchup, not of an oversized card.
- Target: a card with a 5–6 icon comp should be roughly head + ~2 text lines + comp row + footer,
  with no empty band. Compare against `Match Browser.dc.html`, where the card is exactly this tall.

### Comp row overflows to the card edge
On some 3-wide cards the last profession icon/count sits flush against the card's right border.
Add right padding (≈12px) on the comp container / team half so the comp never touches the edge.

---

# Match Detail view — redesign

**Reference:** `Match Detail.dc.html`. Target: the match-details panel in the replay browser
(the `showSkills` / player-table code around `draw_replay_browser.cpp:3553–3835`).
Same Watchtower CI and same rules as the card view above.

## Layout
Three columns: **[meta sidebar 220px] · [team A panel] · [team B panel]**.
- **Sidebar:** map screenshot (150px tall), then date, map, mode, duration (amber clock),
  strategy (`◈` purple), a rating stub, a notes box, and the Lord Damage block.
- **Team panel:** team name + `[tag]` + `WON` chip (winner panel gets the amber top gradient),
  a column-header row, then one row per player.
- Top bar: `MATCH DETAILS` (amber) left; `▶ REPLAY MATCH` (amber outline button) + close `✕` right.

## Player row — shared column grid (both teams identical)
`grid-template-columns: 40px  minmax(90px,1fr)  24px  auto  14px  repeat(5, 46px)`
1. **Prof icons** — primary 22px + secondary 15px (real `GetProfessionIcon`).
2. **Player name** — 13px, ellipsis, no wrap.
3. **Copy build** — `⧉` icon button, tooltip "Copy build template" (calls `EncodeSkillTemplate`).
4. **Build** — the 8 skill icons via `GetSkillIcon(skillId)` from `used_skills`, 28px tiles, 2px gap.
   (In the reference these are placeholder tiles; use the real skill icons in-app.)
5. **14px spacer** — separates build from stats (fixes the "bar tight against stats" issue).
6. **5 stat columns** — 46px each, centered mono.

## Stat columns — LABELED + tooltips (this was the missing piece)
Each column has a small text label + the team total above the per-player numbers, and a `title`
tooltip. Suggested columns → map to `MatchIndex` player fields:
`K` Kills · `D` Deaths · `DMG` `total_damage` · `HEAL` `total_healing_dealt` · `RCV` `total_damage_received`.
- Big numbers formatted `22.8k`; zero values dimmed `#5b5b61`; K/D emphasized when > 0.
- Confirm which 5 (or 6) stats you actually want surfaced here.

## Notes
- The header stat icons in the current build are unlabeled — replace with the label+total scheme above.
- Both team panels must use the **same** grid template so columns line up across the match.
