# Cape Asset Mapping Table

Maps raw game CapeDesign values (from GWCA / infos.json) to gw-memorial banner designer asset indices.

Reverse-engineered manually using the CapeDebugWindow toolbox module.

## Emblem Mapping

Game `cape_emblem` (0-171) to gw-memorial `symbol-{index}.png` asset index.

| Game | Memorial | Game | Memorial | Game | Memorial | Game | Memorial |
|------|----------|------|----------|------|----------|------|----------|
| 0 | 53 | 43 | 95 | 86 | 14 | 129 | 9 |
| 1 | 54 | 44 | 96 | 87 | 15 | 130 | 8 |
| 2 | 55 | 45 | 97 | 88 | 16 | 131 | 7 |
| 3 | 56 | 46 | 172 | 89 | 17 | 132 | 6 |
| 4 | 57 | 47 | 99 | 90 | 18 | 133 | 5 |
| 5 | 58 | 48 | 3 | 91 | 19 | 134 | 4 |
| 6 | 59 | 49 | 101 | 92 | 20 | 135 | 98 |
| 7 | 60 | 50 | 102 | 93 | 21 | 136 | 2 |
| 8 | 61 | 51 | 103 | 94 | 22 | 137 | 1 |
| 9 | 62 | 52 | 104 | 95 | 23 | 138 | 81 |
| 10 | 63 | 53 | 105 | 96 | 24 | 139 | 171 |
| 11 | 64 | 54 | 106 | 97 | 25 | 140 | 170 |
| 12 | 137 | 55 | 107 | 98 | 26 | 141 | 65 |
| 13 | 66 | 56 | 108 | 99 | 27 | 142 | 157 |
| 14 | 67 | 57 | 109 | 100 | 28 | 143 | 156 |
| 15 | 68 | 58 | 110 | 101 | 173 | 144 | 155 |
| 16 | 69 | 59 | 111 | 102 | 169 | 145 | 154 |
| 17 | 70 | 60 | 112 | 103 | 168 | 146 | 153 |
| 18 | 71 | 61 | 113 | 104 | 167 | 147 | 151 |
| 19 | 72 | 62 | 114 | 105 | 166 | 148 | 148 |
| 20 | 73 | 63 | 115 | 106 | 165 | 149 | 146 |
| 21 | 74 | 64 | 116 | 107 | 164 | 150 | 145 |
| 22 | 75 | 65 | 117 | 108 | 163 | 151 | 30 |
| 23 | 76 | 66 | 118 | 109 | 162 | 152 | 100 |
| 24 | 77 | 67 | 119 | 110 | 161 | 153 | 32 |
| 25 | 78 | 68 | 120 | 111 | 160 | 154 | 33 |
| 26 | 79 | 69 | 121 | 112 | 159 | 155 | 34 |
| 27 | 80 | 70 | 122 | 113 | 152 | 156 | 35 |
| 28 | 31 | 71 | 123 | 114 | 150 | 157 | 36 |
| 29 | 82 | 72 | 124 | 115 | 149 | 158 | 37 |
| 30 | 83 | 73 | 125 | 116 | 147 | 159 | 38 |
| 31 | 84 | 74 | 126 | 117 | 144 | 160 | 39 |
| 32 | 158 | 75 | 127 | 118 | 143 | 161 | 40 |
| 33 | 86 | 76 | 128 | 119 | 142 | 162 | 41 |
| 34 | 87 | 77 | 129 | 120 | 141 | 163 | 42 |
| 35 | 0 | 78 | 130 | 121 | 140 | 164 | 43 |
| 36 | 88 | 79 | 131 | 122 | 139 | 165 | 44 |
| 37 | 89 | 80 | 132 | 123 | 138 | 166 | 45 |
| 38 | 90 | 81 | 133 | 124 | 85 | 167 | 46 |
| 39 | 91 | 82 | 134 | 125 | 136 | 168 | 47 |
| 40 | 92 | 83 | 135 | 126 | 29 | 169 | 48 |
| 41 | 93 | 84 | 12 | 127 | 11 | 170 | 49 |
| 42 | 94 | 85 | 13 | 128 | 10 | 171 | 50 |

## Shape Mapping

Game `cape_shape` (1-9) to gw-memorial `base-{index}.png`. Manually reverse-engineered lookup table (no simple formula).

| Game | Asset File |
|------|-----------|
| 1 | base-6.png |
| 2 | base-1.png |
| 3 | base-8.png |
| 4 | base-9.png |
| 5 | base-7.png |
| 6 | base-2.png |
| 7 | base-3.png |
| 8 | base-4.png |
| 9 | base-5.png |

Lookup: `kShapeLookup[] = { 0, 6, 1, 8, 9, 7, 2, 3, 4, 5 }` (index 0 unused)

## Detail Mapping

Game `cape_detail` (0-31) to gw-memorial `details-{index}.png`. Value 0 means no detail (plain).

| Game | Asset File |
|------|-----------|
| 0 | (none) |
| 1 | details-0.png |
| 2 | details-1.png |
| ... | ... |
| 31 | details-30.png |

Formula: `asset_index = game_detail - 1` (skip if game_detail == 0)

## Trim Mapping

Game `cape_trim` to trim type name and asset file prefix.

| Game | Trim Name | Asset Prefix | Notes |
|------|-----------|-------------|-------|
| 0 | None | (none) | No trim |
| 1 | Silver | trim-silver | |
| 2 | Gold | trim-gold | |
| 3 | Bronze | trim-bronze | |
| 4 | Red | trim-bronze | + hue filter |
| 5 | Blue | trim-bronze | + hue filter |
| 6 | Green | trim-bronze | + hue filter |
| 7 | Purple | trim-bronze | + hue filter |
| 8 | Orange | trim-bronze | + hue filter |
| 9 | Obsidian | trim-bronze | + hue/grayscale filter |
| 10 | ? | trim-bronze | + hue filter |
| 11 | Pink | trim-bronze | + hue filter |

Colored trims (4+) use the bronze trim texture with CSS-style hue rotation, brightness, contrast, and grayscale filters applied.

## Color Encoding

`cape_bg_color`, `cape_detail_color`, `cape_emblem_color` are encoded as:
- `hue = value / 16` (0-15)
- `shade = value % 16` (0-15)

16 hues: Red, Orange, Brown, Yellow, DkGreen, Green, Olive/Gold, Teal, Blue, Navy, Cyan, Purple, Violet, Maroon, Pink, Gray.
