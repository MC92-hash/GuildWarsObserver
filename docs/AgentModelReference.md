# GW Observer — Agent Model Reference

> Complete mapping of 3D models used in the GW Observer replay renderer.
>
> **Source file:** `SourceFiles/ReplayMapData.h`

---

## Table of Contents

- [Player Models](#player-models)
  - [Warrior (ID 1)](#warrior-id-1)
  - [Ranger (ID 2)](#ranger-id-2)
  - [Monk (ID 3)](#monk-id-3)
  - [Necromancer (ID 4)](#necromancer-id-4)
  - [Mesmer (ID 5)](#mesmer-id-5)
  - [Elementalist (ID 6)](#elementalist-id-6)
  - [Assassin (ID 7)](#assassin-id-7)
  - [Ritualist (ID 8)](#ritualist-id-8)
  - [Paragon (ID 9)](#paragon-id-9)
  - [Dervish (ID 10)](#dervish-id-10)
- [NPC Models](#npc-models)
- [Spirit Models](#spirit-models)
- [Variant Selection](#variant-selection)

---

## Player Models

Models are assigned per **primary profession** and **gender**.
When multiple variants exist, they are **randomly shuffled** per match to visually distinguish players of the same profession/gender, avoiding duplicate models until all variants are exhausted (see [Variant Selection](#variant-selection)).

---

### Warrior (ID 1)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1FC11` | 11.879872 | 75.734184 | Stefan | Fighter Henchman |
| 1 | `0x2D2A4` | 11.879872 | 75.734184 | Lukas | Guardian Henchman |
| 2 | `0x1C828` | 11.879872 | 75.734184 | Duke Barradin | |
| 3 | `0x2D341` | 11.879872 | 75.734184 | Seaguard Eli | |
| 4 | `0x1FBCD` | 11.879872 | 75.734184 | Prince Rurik | |
| 5 | `0x22B45` | 11.879872 | 75.734184 | Captain Miken | |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1FBC4` | 36.000000 | 72.000000 | Devona | Fighter Henchman |
| 1 | `0x3BD9E` | 15.350650 | 73.640617 | Timera | Brawler Henchman |
| 2 | `0x26C53` | 15.350650 | 73.640617 | Zaishen Fighter | |
| 3 | `0x22B54` | 15.350650 | 73.640617 | Adepte | |

---

### Ranger (ID 2)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1FBBA` | 14.406947 | 75.844055 | Aidan | Archer Henchman |
| 1 | `0x26C56` | 14.406947 | 75.844055 | Zaishen Archer | |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1FC35` | 15.350650 | 73.640617 | Reyna | Archer Henchman |
| 1 | `0x1C801` | 15.350650 | 73.640617 | Lulu Xan | Magebane Henchman |
| 2 | `0x2D2E2` | 15.350650 | 73.640617 | Aurora | |

---

### Monk (ID 3)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x26C4D` | 14.406947 | 75.844055 | Zaishen Healer | |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1C7EE` | 15.350650 | 73.640617 | Lina | Protector Henchman |
| 1 | `0x1FC32` | 15.350650 | 73.640617 | Alesia | Healer Henchman |
| 2 | `0x2D22C` | 15.350650 | 73.640617 | Sister Tai | Healer Henchman |
| 3 | `0x2D126` | 36.000000 | 72.000000 | Danika | |
| 4 | `0x29997` | 36.000000 | 72.000000 | Blahks | |

---

### Necromancer (ID 4)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x3BBC6` | 14.406947 | 75.844055 | Olias | Hero base armor |
| 1 | `0x2D3D1` | 11.879872 | 75.734184 | Ghavin | |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1FB82` | 36.000000 | 72.000000 | Eve | Cultist Henchman |
| 1 | `0x2D225` | 15.350650 | 73.640617 | Su | Grave Henchman |

---

### Mesmer (ID 5)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1C7CD` | 11.879872 | 75.734184 | Dunham | Enchanter Henchman |
| 1 | `0x2D21E` | 14.406947 | 75.844055 | Lo Sha | Illusion Henchman |
| 2 | `0x1C7CC` | 11.879872 | 75.734184 | Tannaros | Punishing Henchman |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x4C460` | 15.350650 | 73.640617 | Gwen | Hero base armor |
| 1 | `0x3BD99` | 15.350650 | 73.640617 | *(unnamed)* | |

---

### Elementalist (ID 6)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1FC2F` | 14.406947 | 75.844055 | Orion | Mage Henchman |
| 1 | `0x2D236` | 14.406947 | 75.844055 | Headmaster Vhang | Shock Henchman |
| 2 | `0x2D155` | 11.879872 | 75.734184 | Argo | |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x1FBBF` | 36.000000 | 72.000000 | Cynn | Mage Henchman |
| 1 | `0x26C50` | 15.350650 | 73.640617 | Zaishen Mage | |
| 2 | `0x1C835` | 36.000000 | 72.000000 | Luzy Fiera | Fire Henchman |
| 3 | `0x560D8` | 36.000000 | 72.000000 | Suzu | |

---

### Assassin (ID 7)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x2D217` | 14.406947 | 75.844055 | Panaku | Cutthroat Henchman |
| 1 | `0x528FC` | 36.000000 | 72.000000 | Kah Xan | Assassin Henchman |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x3BC80` | 15.350650 | 73.640617 | Zenmai | Hero base armor |
| 1 | `0x2D15C` | 15.350650 | 73.640617 | Nika | |
| 2 | `0x2D37F` | 36.000000 | 72.000000 | Fuu Rin | |

---

### Ritualist (ID 8)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x2D2F3` | 14.406947 | 75.844055 | Professor Gai | Spirit Henchman |
| 1 | `0x2D2A9` | 14.406947 | 75.844055 | Aeson | Spirit Henchman |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x4C476` | 15.350650 | 73.640617 | Xandra | Hero base armor |
| 1 | `0x2D1A3` | 36.000000 | 72.000000 | Narcissia | Spirit Henchman |
| 2 | `0x2D136` | 36.000000 | 72.000000 | Nuno | |

---

### Paragon (ID 9)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x3BD8E` | 14.406947 | 75.844055 | Sogolon | Motivation Henchman |
| 1 | `0x3BCF7` | 14.406947 | 75.844055 | General Morgahn | |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x4C449` | 15.350650 | 73.640617 | Hayda | Hero base armor |
| 1 | `0x3BCD0` | 15.350650 | 73.640617 | Kormir | |

---

### Dervish (ID 10)

#### Male

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x4C454` | 14.406947 | 75.844055 | Kahmu | Hero base armor |
| 1 | `0x560E2` | 14.406947 | 75.844055 | Alsacien | |

#### Female

| # | File ID | Width | Height | Name | Notes |
|---|---------|-------|--------|------|-------|
| 0 | `0x3BD6A` | 15.350650 | 73.640617 | Melonni | Hero base armor |

---

## NPC Models

Used for GvG match NPCs. Selected by `modelId` from the game server.

| Model ID | File ID | Height | Scale | Name |
|----------|---------|--------|-------|------|
| 170 | `0x2D161` | 98.454437 | 1.3x | Guild Lord |
| 172 | `0x2D236` | 75.844055 | 1.0x | Bodyguard |
| 173 | `0x26C4A` | 75.734184 | 1.0x | Footman |
| 174 | `0x26C4A` | 75.734184 | 1.0x | Knight |
| 175 | `0x2D18A` | 72.000000 | 1.0x | Archer |
| 176 | `0x2D18A` | 72.000000 | 1.0x | Archer |

---

## Spirit Models

Spirits are classified into three categories based on their `modelId`.

| Category | File ID | Height | Scale | Notes |
|----------|---------|--------|-------|-------|
| Nature Ritual | `0x22A34` | 73.917145 | 0.8x | Fertile Season, Frozen Soil, etc. |
| Offensive Binding Ritual | `0x2D408` | 95.705956 | 1.0x | Bloodsong, Anguish, Pain, etc. |
| Defensive Binding Ritual | `0x2D44E` | 84.404671 | 1.0x | Shelter, Union, Recuperation, etc. |

---

## Variant Selection

When multiple players share the same profession and gender, the renderer assigns different model variants **randomly** to distinguish them visually. The selection algorithm:

1. Group all players by `(profession, gender)` pair
2. For each group, create a shuffled permutation of available variant indices
3. Assign variants from the shuffled list — no duplicates until all variants are exhausted
4. If more players than variants, start a new shuffled round

**Example** with 5 Male Warriors (6 variants available):

- Player A → Duke Barradin (`0x1C828`) *(random)*
- Player B → Seaguard Eli (`0x2D341`) *(random)*
- Player C → Stefan (`0x1FC11`) *(random)*
- Player D → Captain Miken (`0x22B45`) *(random)*
- Player E → Prince Rurik (`0x1FBCD`) *(random)*

Each match produces a different assignment. The randomization is seeded per match load.

---

## Model Count Summary

| Profession | Male | Female | Total |
|------------|------|--------|-------|
| Warrior | 6 | 4 | 10 |
| Ranger | 2 | 3 | 5 |
| Monk | 1 | 5 | 6 |
| Necromancer | 2 | 2 | 4 |
| Mesmer | 3 | 2 | 5 |
| Elementalist | 3 | 4 | 7 |
| Assassin | 2 | 3 | 5 |
| Ritualist | 2 | 3 | 5 |
| Paragon | 2 | 2 | 4 |
| Dervish | 2 | 1 | 3 |
| **Total** | **25** | **29** | **54** |

NPCs: 6 entries (4 unique models) · Spirits: 3 categories (3 unique models)
