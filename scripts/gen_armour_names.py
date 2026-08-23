import re, collections
SRC = r'D:\Guild Wars Tools\gwtoolbox-replay-plugin-feat-model-tracker\gwtoolbox-replay-plugin-feat-model-tracker\GWToolboxdll\Windows\ArmoryWindow_Constants.h'
OUT = r'd:\Guild Wars OBS\GW Observer 1.2.7 Fork\SourceFiles\ArmourNames.h'

pat = re.compile(r'\{"([^"]+)",\s*(0x[0-9A-Fa-f]+),\s*Profession::(\w+),\s*ItemType::(\w+),'
                 r'\s*Campaign::(\w+),\s*((?:0x)?[0-9A-Fa-f]+)')
PROF = {'Warrior':1,'Ranger':2,'Monk':3,'Necromancer':4,'Mesmer':5,'Elementalist':6,
        'Assassin':7,'Ritualist':8,'Paragon':9,'Dervish':10,'None':0}
SLOT = {'Chestpiece':2,'Leggings':3,'Headpiece':4,'Boots':5,'Gloves':6,
        'Costume':2,'Costume_Headpiece':4}

rows, seen = [], set()
for m in pat.finditer(open(SRC, encoding='utf-8', errors='replace').read()):
    name, mid, prof, itype, camp, setidx = m.groups()
    mid = int(mid, 16)
    if mid in seen or itype not in SLOT: continue
    seen.add(mid)
    rows.append((mid, PROF.get(prof, 0), SLOT[itype], int(setidx, 16 if setidx.startswith('0x') else 10),
                 name.replace('"', '\\"'), camp))
rows.sort()

body = '\n'.join('    { %6d, %2d, %d, %3d, "%s", "%s" },' % r for r in rows)
open(OUT, 'w', encoding='utf-8', newline='\n').write(f'''#pragma once

// Armour piece names, keyed by the model file id the recording already carries.
//
// The server sends another player's armour as a skin and a dye and nothing else -- no name, and an
// empty mod list -- so the panel had nothing to call the pieces it was drawing. This table supplies
// the missing half.
//
// Ported from GWToolbox's GWToolboxdll/Windows/ArmoryWindow_Constants.h, which is where the
// community's mapping of model ids to armour names lives. Credit and licence belong to the
// GWToolbox authors; this is their data, reshaped into a lookup and trimmed to the five worn
// slots. Regenerated with scripts, not hand-edited -- go back to the toolbox table to change it.
//
// Two independent checks that the key is right: every armour piece in the sampled matches resolves
// to a set belonging to that player's own primary profession, and the table's trailing set index
// matches the `dyeTint` byte the recording sends alongside the skin.

#include <cstdint>

namespace ArmourNames
{{
    struct Piece
    {{
        uint32_t    modelFileId;  // ItemDef::modelFileId
        uint8_t     profession;   // 1..10, matches AgentReplayData::primaryProf
        uint8_t     slot;         // EQUIP_SET slot: 2 chest, 3 legs, 4 head, 5 feet, 6 hands
        uint8_t     setIndex;     // matches ItemDef::dyeTint
        const char* name;
        const char* campaign;
    }};

    inline constexpr Piece kPieces[] = {{
{body}
    }};

    inline const Piece* Find(uint32_t modelFileId)
    {{
        // Sorted by id, so a binary search: the table is over a thousand entries and the panel
        // asks five times per player per frame.
        size_t lo = 0, hi = std::size(kPieces);
        while (lo < hi)
        {{
            const size_t mid = (lo + hi) / 2;
            if (kPieces[mid].modelFileId < modelFileId) lo = mid + 1;
            else                                        hi = mid;
        }}
        return (lo < std::size(kPieces) && kPieces[lo].modelFileId == modelFileId) ? &kPieces[lo] : nullptr;
    }}
}}
''')
print('wrote %d pieces' % len(rows))
