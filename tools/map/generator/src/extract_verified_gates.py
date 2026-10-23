import pipeline_context as ctx
"""Read-only v1.2 openable-door catalog. Never writes game/save/geometry data."""
import hashlib,json,math,struct
from collections import defaultdict
from pathlib import Path
from probe_markers import ROOT,parts,instructions,Binder
from probe_gates import object_acts,resolved_instructions,door_description

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def objact_params():
 path=ROOT/'param/gameparam/gameparam.parambnd.dcx'
 entry=next(e for e in Binder.from_path(path).entries if e.name=='ObjActParam.param')
 b=entry.get_uncompressed_data();assert b[44:46]==b'\0\x04','Expected little-endian 64-bit row pointers'
 count=struct.unpack_from('<H',b,10)[0];rows={}
 for i in range(count):
  rid,_,offset,nameoff=struct.unpack_from('<iiqq',b,64+24*i);end=nameoff
  while b[end:end+2]!=b'\0\0':end+=2
  prompt,failure,flag,player,chr=struct.unpack_from('<iiiII',b,offset)
  rows[rid]=dict(name=b[nameoff:end].decode('utf-16le'),prompt_id=prompt,player_animation=player,object_animation=struct.unpack_from('<I',b,offset+28)[0],action_button_param=struct.unpack_from('<i',b,offset+40)[0])
 return rows,path

def signed(data,offset=0):return struct.unpack_from('<i',bytes.fromhex(data),offset)[0]
def excluded(desc):
 if any(s in desc for s in ('霧壁','霧扉','ダミー','門番','井戸','宝箱','宝死体','開かない')):return 'fog barrier, dummy, non-door or explicitly non-opening object'
 if desc.startswith('レバー') or desc.startswith('【企画OBJ_レバー'):return 'lever activator; not the door itself'
 return None

def main():
 params,parampath=objact_params();catalog={};report={};totals=defaultdict(int)
 for mapfile in sorted(ctx.BASE.glob('m*.json')):
  mid=mapfile.stem;msbpath=ROOT/'map/mapstudio'/f'{mid}.msb.dcx';eventpath=ROOT/'event'/f'{mid}.emevd.dcx'
  ps=parts(msbpath);acts=object_acts(msbpath)
  commonpath=ROOT/'event/common.emevd.dcx';local=instructions(eventpath)
  localids={i['event'] for i in local};shared=[i for i in instructions(commonpath) if i['event'] not in localids and i['event'] not in (0,50)]
  ins=resolved_instructions(local+shared);byinvoke=defaultdict(list);actbyentity=defaultdict(list)
  for i in ins:byinvoke[i['invocation']].append(i)
  for act in acts:actbyentity[act['entity']].append(act)
  markers=[];omitted=[];sources=dict(msb=str(msbpath.relative_to(ROOT)),msb_sha256=sha(msbpath),emevd=str(eventpath.relative_to(ROOT)),emevd_sha256=sha(eventpath),common_emevd=str(commonpath.relative_to(ROOT)),common_emevd_sha256=sha(commonpath),objact_param_binder=str(parampath.relative_to(ROOT)),objact_param_sha256=sha(parampath))
  for pi,p in enumerate(ps):
   if p['kind'] not in (1,9) or not door_description(p['desc']):continue
   reason=excluded(p['desc']);attached=[a for a in acts if a['part_index']==pi];direct=[];animated=[]
   for a in attached:
    row=params.get(a['param'])
    if row and door_description(row['name']) and not excluded(row['name']) and row['player_animation']>0:
     direct.append(dict(object_act=a,param_row=row,param_id=a['param']))
   for i in ins:
    if i['category']==2005 and i['index'] in (6,14) and len(bytes.fromhex(i['args']))>=8 and p['entity']>0 and signed(i['args'])==p['entity']:
     pid=signed(i['args'],4);row=params.get(pid)
     if row and door_description(row['name']) and not excluded(row['name']) and row['player_animation']>0:
      direct.append(dict(scripted_object_activation=dict(event_id=i['event'],instruction_line=i['line'],instruction=f'SetObjectActivation (2005,{i["index"]})',invocation=i['invocation']),param_row=row,param_id=pid))
    if (i['category'],i['index'])!=(2003,18) or len(bytes.fromhex(i['args']))<8 or p['entity']<0:continue
    if signed(i['args'])!=p['entity']:continue
    anim=signed(i['args'],4)
    # IDs differ between models. Opening must be corroborated by an opening
    # door-sound state or a successful activator in this exact event instance.
    activators=[];opening_notifications=[]
    for condition in byinvoke[i['invocation']]:
     if (condition['category'],condition['index'])==(2010,5) and len(bytes.fromhex(condition['args']))>=8 and signed(condition['args'])==p['entity'] and signed(condition['args'],4) in (1,3):
      opening_notifications.append(dict(instruction='NotifyDoorEventSoundDampening (2010,5)',event_id=condition['event'],instruction_line=condition['line'],door_state_enum=signed(condition['args'],4),meaning='DoorOpen' if signed(condition['args'],4)==1 else 'DoorOpening'))
     if (condition['category'],condition['index'])!=(5,2) or len(bytes.fromhex(condition['args']))<8:continue
     objactid=signed(condition['args'],4)
     for a in actbyentity[objactid]:
      if 0<=a['part_index']<len(ps):
       other=ps[a['part_index']];activators.append(dict(part=other['name'],description=other['desc'],entity_id=other['entity'],pos=other['pos'],object_act_id=objactid,param_id=a['param']))
    if opening_notifications or activators:
     animated.append(dict(instruction='ForceAnimation (2003,18)',event_id=i['event'],instruction_line=i['line'],animation_id=anim,invocation=i['invocation'],related_activators=activators,opening_notifications=opening_notifications))
   if reason or not (direct or animated):
    omitted.append(dict(part=p['name'],entity_id=p['entity'],description=p['desc'],reason=reason or 'no verified opening ObjectAct parameter or resolved opening animation',object_act_parameters=[a['param'] for a in attached]));continue
   marker=dict(kind='gate',name='Openable gate/door',pos=p['pos'],rotation=p['rot'],entity_id=p['entity'],part=p['name'],model=p['model'],description=p['desc'],availability='Static capability only; no open/closed/locked or quest-state claim',position_semantics='Actual authored door/gate object origin; lever coordinates never substituted',provenance=dict(**sources,explicit_door_description=p['desc'],opening_object_acts=direct,opening_animations=animated))
   assert all(math.isfinite(v) for v in marker['pos']);markers.append(marker)
  # Map bounds sanity only. Preserve original coordinates, never snap gates.
  data=json.loads(mapfile.read_text(encoding='utf-8'));xmin,zmin,xmax,zmax=data['bounds'];outside=[m['part'] for m in markers if not (xmin<=m['pos'][0]<=xmax and zmin<=m['pos'][2]<=zmax)]
  catalog[mid]=markers;report[mid]=dict(included=len(markers),excluded=len(omitted),outside_bounds=outside,exclusions=omitted)
  totals['included']+=len(markers);totals['excluded']+=len(omitted)
 ctx.GATES.write_text(json.dumps(catalog,ensure_ascii=False,indent=2),encoding='utf-8')
 result=dict(totals=dict(totals),areas=report,limitations=['Static capability; no current gate state','Only explicit authored door/gate descriptions with opening evidence','Ordinary maps in current atlas; Chalice dungeons excluded','Some cutscene-only or externally controlled doors may be omitted','Object origins and rotations; physical aperture width not decoded'])
 (ctx.REPORTS/'gate-extraction-report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
 print(json.dumps(dict(totals=dict(totals),areas={k:dict(included=v['included'],excluded=v['excluded'],outside_bounds=v['outside_bounds']) for k,v in report.items()}),indent=2))
