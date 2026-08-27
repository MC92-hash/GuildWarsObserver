"""Factual StoC/objective timeline for Scout; no winner interpretation."""
from __future__ import annotations
import gzip
from pathlib import Path

JUMBO = {0:"base_under_attack",1:"guild_lord_under_attack",3:"captured_shrine",5:"captured_tower",6:"party_defeated",9:"morale_boost",11:"neutralized_shrine",16:"victory",17:"flawless_victory"}

def _int(v):
    try: return int(v)
    except (TypeError, ValueError): return 0

def _time(header):
    try:
        body=header[1:header.index("]")]; minute,rest=body.split(":",1)
        sec,millis=rest.split(".",1) if "." in rest else (rest,"0")
        return _int(minute)*60+_int(sec)+_int(millis)/1000
    except (ValueError, TypeError): return None

def _read(path):
    opener=gzip.open if path.suffix==".gz" else open
    with opener(path,"rt",encoding="utf-8-sig",errors="replace") as h: return h.readlines()

def _stream(path, source, decode):
    events=[]; audit={"lines":0,"records":0,"malformed_header":0,"unknown_record":0,"short_record":0}
    for number,raw in enumerate(_read(path),1):
        audit["lines"]+=1; line=raw.strip(); close=line.find("]")
        if not line.startswith("[") or close<0 or _time(line) is None:
            audit["malformed_header"]+=1; continue
        fields=[x.strip() for x in line[close+1:].strip().split(";")]
        event=decode(_time(line),fields,source,number)
        if event is None:
            audit["unknown_record"]+=1; continue
        audit["records"]+=1; events.append(event)
    return events,audit

def _decode_jumbo(t,f,s,n):
    if len(f)<3 or f[0] != "GAME_SMSG_JUMBO_MESSAGE": return None
    kind=JUMBO.get(_int(f[1]));
    if kind is None: return {"t":t,"kind":"jumbo.unknown","source":s,"line":n,"type_id":_int(f[1]),"raw_fields":f[2:]}
    value=f[2].split(" ",1)[0]
    return {"t":t,"kind":"jumbo."+kind,"source":s,"line":n,"party_value":_int(value),"party_label":f[2]}

def _decode_generic(prefix, arities=()):
    def decode(t,f,s,n):
        if not f or not f[0].startswith(prefix): return None
        return {"t":t,"kind":f[0].lower().replace("_","."),"source":s,"line":n,"fields":f[1:]}
    return decode

# The flag stream is numeric-tagged, NOT FLAG_-prefixed: "[ts] <type>;<f1>;<f2>...".
# Layouts are the snprintf calls at EventHooks.cpp:989-1085. Decoding it with the
# generic FLAG_ prefix rejected every line -- 53,525 read and 0 published in the
# August shard -- so the field names below are the record, not a guess.
# Two dead fields, deliberately still emitted so a reader sees them as dead:
# `team_code` on pickup/drop is only ever assigned inside the STATE packet, which
# the server never sends, so it is always 0 -- a flag's team comes from its item
# record's `extra_id`. Type 2 itself has zero occurrences in 266,675 records.
FLAG_RECORDS={0:("flag.pickup",("item_id","agent_id","team_code")),
              1:("flag.drop",("agent_id","team_code")),
              2:("flag.state",("team_code","item_id","state")),
              3:("flag.item",("item_id","model_id","extra_id","type_id")),
              4:("flag.stand",("stand_agent_id","field","value")),
              5:("flag.spawn",("agent_id","unknown","object_id")),
              6:("flag.announce",("action","template_id","team"))}
# A return and a stick share the announce record and carry a team but NO agent,
# so both stay at team level here; attributing one to a player is not possible
# from this stream.
FLAG_ANNOUNCE={0:"flag.return",1:"flag.stick"}

def _decode_flag(t,f,s,n):
    if not f or not f[0].isdigit(): return None
    spec=FLAG_RECORDS.get(_int(f[0]))
    if spec is None: return None
    kind,names=spec
    if len(f)-1<len(names): return None
    event={"t":t,"kind":kind,"source":s,"line":n}
    event.update({name:_int(value) for name,value in zip(names,f[1:])})
    if kind=="flag.announce": event["kind"]=FLAG_ANNOUNCE.get(event["action"],kind)
    return event

def build_timeline(infos: dict, match_dir: Path) -> dict:
    streams={}; total_audit={}
    names=("jumbo_messages","flag_events","objective_events","door_events",
           "manipulate_map_object_events","lifecycle_events")
    decoders={"jumbo_messages":_decode_jumbo,
              "flag_events":_decode_flag,
              "objective_events":_decode_generic("OBJECTIVE"),
              "door_events":_decode_generic("DOOR_"),
              "manipulate_map_object_events":_decode_generic("MAP_OBJECT"),
              "lifecycle_events":_decode_generic("AGENT_")}
    for name in names:
        base=match_dir/"StoC"; path=next((p for p in (base/(name+".txt.gz"),base/(name+".txt")) if p.is_file()),None)
        if path is None: continue
        events,audit=_stream(path,name,decoders[name]); streams[name]={"events":sorted(events,key=lambda e:(e["t"],e["line"])),"audit":audit}
    if not streams: return {}
    return {"schema":1,"streams":streams}
