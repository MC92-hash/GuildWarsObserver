"""Compare historical bar IDs with original cast events; read archives only.

Usage: python scripts/audit_historical_skill_ids.py <directory-of-tar.gz-files>
One missing bar ID and one extra cast ID provide an unambiguous set comparison.
Ambiguous comparisons are reported, never assigned an identity.
"""
import collections
import gzip
import json
import pathlib
import sys
import tarfile


def audit(directory):
    repo = pathlib.Path(__file__).resolve().parent.parent
    data = json.loads((repo / 'Data/skilldata.json').read_text(encoding='utf-8'))['skilldata']
    reverse = {v['split_id']: int(k) for k, v in data.items() if v.get('pvp_split')}
    names = json.loads((repo / 'Data/skilldesc-en.json').read_text(encoding='utf-8'))['skilldesc']
    evidence = []
    for archive in sorted(pathlib.Path(directory).glob('*.tar.gz')):
        with tarfile.open(archive) as tar:
            members = tar.getnames()
            info_name = next((n for n in members if pathlib.PurePosixPath(n).name == 'infos.json'), None)
            if not info_name:
                continue
            info = json.load(tar.extractfile(info_name))
            casts = collections.defaultdict(collections.Counter)
            for member in members:
                if pathlib.PurePosixPath(member).name not in ('skill_events.txt.gz', 'attack_skill_events.txt.gz'):
                    continue
                lines = gzip.decompress(tar.extractfile(member).read()).decode('utf-8-sig').splitlines()
                for line in lines:
                    if '] ' not in line:
                        continue
                    fields = line.split('] ', 1)[1].split(';')
                    if len(fields) < 3 or fields[0] not in ('SKILL_ACTIVATED', 'ATTACK_SKILL_ACTIVATED', 'INSTANT_SKILL_USED'):
                        continue
                    skill, agent = map(int, fields[1:3])
                    casts[agent][reverse.get(skill, skill)] += 1
            for party in info.get('parties', {}).values():
                for player in party.get('PLAYER', []):
                    bar = set(player.get('used_skills', []))
                    high = sorted(s for s in bar if s > 3431)
                    if not high:
                        continue
                    observed = casts[player['id']]
                    missing = sorted(set(observed) - {reverse.get(s, s) for s in bar if s <= 3431})
                    evidence.append(dict(match=archive.name[:-7], agent=player['id'],
                        player=player['encoded_name'], bar=sorted(bar), unresolved=high,
                        candidates={str(s): observed[s] for s in missing}))
    counts = collections.defaultdict(collections.Counter)
    ambiguous = []
    for row in evidence:
        if len(row['unresolved']) == len(row['candidates']) == 1:
            counts[row['unresolved'][0]][int(next(iter(row['candidates'])))] += 1
        else:
            ambiguous.append(row)
    for old, candidates in sorted(counts.items()):
        print(old, [(s, names.get(str(s), {}).get('name'), n) for s, n in candidates.items()])
    print('Comparisons:', len(evidence), 'ambiguous:', len(ambiguous))
    for row in ambiguous:
        print('AMBIGUOUS', row['match'], row['unresolved'], row['candidates'])
    return evidence


if __name__ == '__main__':
    evidence = audit(sys.argv[1])
    if len(sys.argv) > 2:
        pathlib.Path(sys.argv[2]).write_text(json.dumps(evidence, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
