import csv, collections, sys, io

SRC = r"c:\Users\melch\Downloads\Weapons mod.txt"
OUT = r"D:\Guild Wars OBS\GW Observer 1.2.7 Fork\SourceFiles\EquipmentComponents.h"

FIELDS = ['index', 'name', 'description', 'mod_id_hex', 'arg1', 'arg2', 'known_name']

# A component whose description has several effects came out double-encoded: each physical line
# of the record was separately quoted, so one logical record is spread over consecutive
# single-field rows. Rejoin them until the result parses into a full record.
raw = []
with open(SRC, newline='', encoding='utf-8-sig') as f:
    for row in csv.reader(f):
        raw.append(row)

records = []
pending = None
for row in raw[1:]:
    if len(row) >= len(FIELDS):
        records.append(row)
        continue
    if not row or not row[0]:
        continue
    pending = row[0] if pending is None else pending + '\n' + row[0]
    parsed = next(csv.reader(io.StringIO(pending)), None)
    if parsed and len(parsed) >= len(FIELDS):
        records.append(parsed)
        pending = None

rows = []
for row in records:
    # Extra commas can only come from an unquoted description; the mod columns are always the
    # last four, so anchor to the end of the record rather than counting from the front.
    head = row[:2]
    tail = row[-4:]
    desc = ','.join(row[2:-4])
    rows.append(dict(zip(FIELDS, head + [desc] + tail)))

print("records:", len(rows), "from", len(raw) - 1, "physical rows; unterminated:", pending is not None)

# index -> (name, description, component_id)
comps = {}
for r in rows:
    idx = r.get('index')
    if idx is None:
        continue
    if r.get('mod_id_hex') != '0x2408':
        continue
    try:
        a1 = int(r['arg1']); a2 = int(r['arg2'])
    except (TypeError, ValueError):
        continue
    cid = (a1 << 8) | a2
    comps[idx] = (r.get('name') or '', r.get('description') or '', cid)

print("components with identity word:", len(comps))

# Which marker class does each component use? (decides inscription vs prefix/suffix vs insignia)
markers = collections.defaultdict(set)
for r in rows:
    idx = r.get('index')
    if idx in comps:
        markers[idx].add(r.get('mod_id_hex'))

# cid -> list of (name, desc, marker_class)
by_cid = collections.defaultdict(list)
for idx, (name, desc, cid) in comps.items():
    m = markers[idx]
    if '0x2532' in m or '0xA532' in m:
        cls = 'Inscription'
    elif '0xA530' in m:
        cls = 'Insignia'
    else:
        cls = 'Upgrade'
    by_cid[cid].append((name, desc, cls))

table = []
ambiguous = 0
for cid, entries in sorted(by_cid.items()):
    names = {e[0] for e in entries}
    descs = {e[1] for e in entries}
    if len(names) > 1 or len(descs) > 1:
        # Shared by a family (e.g. every Mesmer minor rune). Recording the first would
        # display the wrong attribute, so mark it and let word decoding handle it.
        ambiguous += 1
        continue
    name, desc, cls = entries[0]
    table.append((cid, name, desc, cls))

print("unique usable components:", len(table), "ambiguous skipped:", ambiguous)


def cesc(s):
    out = []
    for ch in s:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            pass
        else:
            out.append(ch)
    return ''.join(out)


lines = []
w = lines.append
w('#pragma once')
w('')
w('// Upgrade components, generated from the game client\'s own PvP upgrade table')
w('// (Item Inspector -> "Game\'s PvP upgrade table" -> Copy CSV). Do not edit by hand;')
w('// regenerate from a fresh dump instead.')
w('//')
w('// Keyed by the component identity word 0x2408: id == (arg1 << 8) | arg2. That word')
w('// identifies which upgrade produced the effects that follow it, which is the only way to')
w('// get the magnitudes right - the effect words themselves are not enough. A Vampiric Axe')
w('// Haft and a Vampiric Hammer Haft both store Lifesteal as 0x25280100, but the axe drains 3')
w('// and the hammer 5; only the identity word (167 vs 168) separates them.')
w('//')
w('// Ids shared by a whole family (every Mesmer minor rune is 175) are omitted rather than')
w('// guessed at - those fall back to decoding the effect words directly.')
w('')
w('#include <cstdint>')
w('#include <string_view>')
w('')
w('namespace Equipment')
w('{')
w('    enum class ComponentClass : uint8_t')
w('    {')
w('        Upgrade,     // weapon prefix/suffix or rune (0x2530 marker)')
w('        Inscription, // 0x2532 / 0xA532 marker')
w('        Insignia,    // 0xA530 marker')
w('    };')
w('')
w('    struct ComponentInfo')
w('    {')
w('        uint16_t         id;')
w('        std::string_view name;        // "Zealous Sword Hilt", "Inscription: \\"I have the power!\\""')
w('        std::string_view description; // game text, \'\\n\' separated when it has several effects')
w('        ComponentClass   cls;')
w('    };')
w('')
w('    inline constexpr ComponentInfo kComponents[] = {')
for cid, name, desc, cls in table:
    w('        {%5d, "%s", "%s", ComponentClass::%s},' % (cid, cesc(name), cesc(desc), cls))
w('    };')
w('')
w('    inline const ComponentInfo* FindComponent(uint16_t id)')
w('    {')
w('        for (const auto& c : kComponents)')
w('        {')
w('            if (c.id == id) return &c;')
w('        }')
w('        return nullptr;')
w('    }')
w('}')
w('')

with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines))

print("wrote", OUT)
