import pipeline_context as ctx
"""Merge verified static landmarks into geometry JSONs; game data is read only.

Run from repository root with the bundled Python. Imports no Bloodborne package.
"""
import argparse,hashlib,json,math,re,struct
from collections import defaultdict
from pathlib import Path
from probe_markers import parts,instructions,esd_commands,Binder,ROOT

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def spatial_dedup(markers):
 result=[]
 for m in markers:
  match=next((other for other in result if m['kind']==other['kind']=='npc' and m['name']==other['name'] and m['model']==other['model'] and math.hypot(m['pos'][0]-other['pos'][0],m['pos'][2]-other['pos'][2])<=2 and abs(m['pos'][1]-other['pos'][1])<=3),None)
  if match:
   match.setdefault('site_variants',[]).append({k:m[k] for k in ('part','entity_id','talk_id','description','pos','provenance')})
  else:result.append(m)
 return result
def proximity(m,data):
 x,y,z=m['pos'];xmin,zmin,xmax,zmax=data['bounds']
 distance=math.inf
 for layer in data['layers']:
  dy=max(layer['height']-y,0,y-(layer['height']+6))
  for rx,rz,w,h in layer['rects']:
   dx=max(rx-x,0,x-(rx+w));dz=max(rz-z,0,z-(rz+h))
   distance=min(distance,math.sqrt(dx*dx+dy*dy+dz*dz))
 return dict(inside_bounds=xmin<=x<=xmax and zmin<=z<=zmax,nearest_floor_band_distance=round(distance,3),floor_measurement='distance to quantized 6m height-band rectangle volumes')
def ladder_instances(ins):
 events=defaultdict(list)
 for i in ins:events[i['event']].append(i)
 found=[];seen=set();unresolved=[]
 def walk(eid,context=b'',depth=0):
  key=(eid,context)
  if key in seen or depth>12:return
  seen.add(key)
  for i in events[eid]:
   data=bytearray.fromhex(i['args']);valid=True
   for dest,source,size in i['replacements']:
    if source+size>len(context):valid=False;break
    data[dest:dest+size]=context[source:source+size]
   if i['category']==2009 and i['index']==0:
    if valid and len(data)==12:found.append((struct.unpack('<3i',data),i['event'],i['line']))
    else:unresolved.append((eid,i['line']))
   if valid and i['category']==2000 and i['index']==0 and len(data)>=8:
    slot,target=struct.unpack_from('<II',data)
    if target in events:walk(target,bytes(data[8:]),depth+1)
 walk(0);walk(50)
 return found,unresolved

def npc_role(p):
 d=p['desc']
 if any(s in d for s in ('ワープ','侵入','獣状態','変身後','宝死体','復活用祭壇','ジェスチャー')):return None
 if not any(s in d for s in ('会話','NPC_','デュラ','ヤマムラ')):return None
 if '時計塔の女医' in d:return 'Adeline site'
 for text,label in [('人形','Doll'),('拠点老人','Gehrman'),('デュラ','Djura'),('老婆','Elderly woman'),('尼僧','Adella'),('娼婦','Arianna'),('偏屈','Skeptical man'),('血族狩り','Alfred'),('復讐者','Eileen'),('盲人','Oedon Chapel dweller'),('生贄街の聖女','Amygdala transport resident'),('ニセ女医','Impostor doctor'),('女医','Doctor'),('重病人','Gilbert'),('少女の姉','Older sister'),('少女','Young girl'),('老夫婦','Resident couple'),('クモ','Patches'),('汎用住人','Resident'),('血族の女王','Vileblood Queen'),('感染者の乞食','Suspicious beggar'),('大学教授','Master Willem'),('ルドウイーク','Ludwig head'),('秘密を探','Simon'),('教会の殺し屋','Brador'),('ヤマムラ','Yamamura'),('時計塔の女医','Adeline'),('患者','Research Hall patient'),('時計塔の狩人','Maria'),('漁村の司祭','Fishing Hamlet priest')]:
  if text in d:return label+' site'
 return 'Conversation site'

def update(source,out):
 out.mkdir(parents=True,exist_ok=True); report={};totals=defaultdict(int)
 for path in sorted(source.glob('m*.json')):
  mid=path.stem; msbpath=ROOT/'map/mapstudio'/f'{mid}.msb.dcx';eventpath=ROOT/'event'/f'{mid}.emevd.dcx';talkpath=ROOT/'script/talk'/f'{mid}.talkesdbnd.dcx'
  ps=parts(msbpath);byentity=defaultdict(list)
  for p in ps:byentity[p['entity']].append(p)
  ls,unresolved=ladder_instances(instructions(eventpath));markers=[];skipped=[];dedup=set()
  for (start,stop,entity),event,line in ls:
   matches=[p for p in byentity[entity] if p['kind'] in (1,9)]
   if len(matches)!=1:skipped.append(dict(entity=entity,reason='ladder object missing or ambiguous'));continue
   if entity in dedup:continue
   dedup.add(entity);p=matches[0]
   markers.append(dict(kind='ladder',name='Ladder',pos=p['pos'],entity_id=entity,part=p['name'],model=p['model'],rotation=p['rot'],description=p['desc'],position_semantics='MSB object origin; endpoints unavailable',start_climbing_flag=start,stop_climbing_flag=stop,provenance=dict(source=f'event/{mid}.emevd.dcx',instruction='RegisterLadder (2009,0)',event_id=event,instruction_line=line,position_source=f'map/mapstudio/{mid}.msb.dcx')))
  talk={}
  for e in Binder.from_path(talkpath).entries:
   match=re.search(r't(\d+)\.esd$',e.name)
   if match:talk[int(match[1])]=(e.name,esd_commands(e.get_uncompressed_data()))
  for p in ps:
   role=npc_role(p);tid=p.get('talk',0)
   if not role or tid not in talk or (1,1) not in talk[tid][1]:continue
   # Require both explicit conversation metadata and a real TalkToPlayer command.
   markers.append(dict(kind='npc',name=role,pos=p['pos'],entity_id=p['entity'],part=p['name'],model=p['model'],talk_id=tid,description=p['desc'],presence='quest dependent; static site only',provenance=dict(source=f'map/mapstudio/{mid}.msb.dcx',evidence='explicit conversation/NPC description plus matching ESD TalkToPlayer (1,1)',talk_source=f'script/talk/{mid}.talkesdbnd.dcx',talk_entry=talk[tid][0])))
  npc_before=sum(m['kind']=='npc' for m in markers);markers=spatial_dedup(markers)
  data=json.loads(path.read_text(encoding='utf-8')); original_lamps=data.get('lamps');data['markers']=markers
  suspicious=[]
  for m in markers:
   m['spatial_qa']=proximity(m,data)
   if not m['spatial_qa']['inside_bounds'] or m['spatial_qa']['nearest_floor_band_distance']>5:
    suspicious.append(dict(kind=m['kind'],part=m['part'],pos=m['pos'],**m['spatial_qa']))
  data['marker_metadata']=dict(version=1,source='local Bloodborne 1.09 static map and event data',geometry_source_sha256=sha(path),msb_sha256=sha(msbpath),emevd_sha256=sha(eventpath),talk_binder_sha256=sha(talkpath),limitations=['Quest state and current NPC presence unavailable','Coordinates are authored static positions, not live actors','Ladder endpoints not decoded; markers show registered object origins','NPC variants and resident dialogue alternatives may share locations','Dialogue emitted by bosses, invaders, lamps and service messengers is excluded'])
  assert data.get('lamps')==original_lamps
  assert all(all(math.isfinite(x) for x in m['pos']) for m in markers)
  (out/path.name).write_text(json.dumps(data,separators=(',',':')),encoding='utf-8')
  counts={kind:sum(m['kind']==kind for m in markers) for kind in ('ladder','npc')};counts.update(npc_before_spatial_dedup=npc_before,max_floor_band_distance=max([m['spatial_qa']['nearest_floor_band_distance'] for m in markers],default=0),unresolved_ladder_instructions=unresolved,skipped_ladders=skipped,spatial_review=suspicious)
  report[mid]=counts
  for kind in ('ladder','npc'):totals[kind]+=counts[kind]
 atlas=source/'atlas.json';(out/'atlas.json').write_bytes(atlas.read_bytes())
 totals['npc_before_spatial_dedup']=sum(v['npc_before_spatial_dedup'] for v in report.values())
 result=dict(totals=dict(totals),maps=report)
 (ctx.REPORTS/'marker-report.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
 return result
