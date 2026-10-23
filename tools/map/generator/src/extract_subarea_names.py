import pipeline_context as ctx
"""Extract trustworthy localized names. No invented district boundaries."""
import hashlib,json,math,struct
from collections import defaultdict
from pathlib import Path
from probe_markers import ROOT,Binder,parts
from probe_location_names import regions,banners,place_names
from soulstruct.base.text.fmg import FMG

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def localized_lamp_names():
 path=ROOT/'msg/engus/menu.msgbnd.dcx';result=defaultdict(list)
 for entry in Binder.from_path(path).entries:
  if not entry.name.endswith('.fmg'):continue
  for rid,value in FMG.from_bytes(entry.get_uncompressed_data()).entries.items():
   if value and '\n' not in value.strip():result[value.strip()].append(dict(binder=str(path.relative_to(ROOT)),entry=entry.name,entry_id=entry.id,text_id=rid))
 return result,path

def return_points():
 path=ROOT/'param/gameparam/gameparam.parambnd.dcx'
 entry=next(e for e in Binder.from_path(path).entries if e.name=='ReturnPointParam.param')
 b=entry.get_uncompressed_data();assert b[44:46]==b'\0\x04';rows={}
 for i in range(struct.unpack_from('<H',b,10)[0]):
  rid,_,off,nameoff=struct.unpack_from('<iiqq',b,64+i*24)
  area,block,entity=struct.unpack_from('<hhi',b,off);chair=struct.unpack_from('<b',b,off+13)[0]
  rows[rid]=dict(area=area,block=block,return_point_entity=entity,warp_chair=chair)
 return rows,path

def subtitle(catalog,mapid,pos):
 """Reference runtime policy: explicit region first, then conservative Near.

 This catalog has no certified localized region volumes. Future volumes must
 have independently verified shape semantics before being included.
 """
 entry=catalog.get(mapid);pos=list(pos)
 if not entry or len(pos)!=3 or not all(math.isfinite(v) for v in pos):return ''
 eligible=[]
 for landmark in entry['landmarks']:
  x,y,z=landmark['pos'];distance=math.hypot(pos[0]-x,pos[2]-z);vertical=abs(pos[1]-y)
  if distance<=landmark['max_horizontal_distance'] and vertical<=landmark['max_vertical_distance']:
   eligible.append((math.hypot(distance,vertical),landmark['name']))
 return 'Near '+min(eligible)[1] if eligible else ''

def main():
 localized,menupath=localized_lamp_names();official,placeentry,placepath=place_names();catalog={};audit={};total=0;rejected=[];collision_links={};rps,parampath=return_points()
 event_entry=next(e for e in Binder.from_path(menupath).entries if e.id==30);eventtexts=FMG.from_bytes(event_entry.get_uncompressed_data()).entries
 for mapfile in sorted(ctx.VECTOR.glob('m*.json')):
  mid=mapfile.stem;data=json.loads(mapfile.read_text(encoding='utf-8'));msbpath=ROOT/'map/mapstudio'/f'{mid}.msb.dcx';rs=regions(msbpath);bs=banners(msbpath);landmarks=[]
  collision_links[mid]=[dict(name=official[abs(b['banner_id'])].strip(),place_name_id=abs(b['banner_id']),part=b['part'],model=b['model'],pos=b['pos'],rot=b['rot'],scale=b['scale'],evidence=dict(msb=str(msbpath.relative_to(ROOT)),msb_sha256=sha(msbpath),part_subtype=5,part_subtype_data_pointer_header_offset='0xB8',place_name_banner_id_subtype_offset='0x08',place_name_banner_raw=b['banner_id'],place_name_fmg_source=str(placepath.relative_to(ROOT)),place_name_fmg_entry=placeentry,place_name_fmg_binder_sha256=sha(placepath),place_name_fmg_entry_id=19,meaning='Exact localized collision banner association; actual mesh required for position membership')) for b in bs if b['banner_id']!=-1 and official.get(abs(b['banner_id']),'').strip()]
  warp_parts=[p for p in parts(msbpath) if p['kind']==1 and p['model']=='o009900']
  for lamp in data.get('lamps',[]):
   name=lamp['name'].strip();sourcepos=lamp.get('source_pos',lamp['pos']);matched=[p for p in warp_parts if sum((p['pos'][i]-sourcepos[i])**2 for i in range(3))<0.001];name_join=None
   if len(matched)==1:
    wp=matched[0];rid=wp['entity']+1000;row=rps.get(rid);slot=wp['entity']%1000-950;textid=70000000+wp['entity']//1000-1+slot
    if row and row['warp_chair']>=10 and 0<=slot<=9 and eventtexts.get(textid):
     name=eventtexts[textid].strip();name_join=dict(lamp_reference_part=wp['name'],lamp_reference_entity=wp['entity'],return_point_param_id=rid,return_point_param=row,return_point_param_source=str(parampath.relative_to(ROOT)),return_point_param_sha256=sha(parampath),event_text_id=textid,event_text_entry=event_entry.name,event_text_entry_id=30,label_scheme='70000000 + lamp_reference_entity//1000 - 1 + (lamp_reference_entity%1000-950)',source_catalog_label=lamp['name'])
   evidence=localized.get(name,[])
   if not evidence:rejected.append(dict(map_id=mid,name=name,reason='no exact localized English menu-text match'));continue
   landmark=dict(name=name,pos=lamp['pos'],max_horizontal_distance=60.0,max_vertical_distance=6.0,subtitle_prefix='Near ',kind='lamp_landmark',evidence=dict(position_source='local-generation/vector/'+mapfile.name,position_source_sha256=sha(mapfile),original_authored_position=lamp.get('source_pos',lamp['pos']),text_matches=evidence,english_menu_binder_sha256=sha(menupath),native_label_join=name_join,meaning='Localized lamp location; proximity is not official district membership'))
   landmarks.append(landmark)
  catalog[mid]=dict(regions=[],landmarks=landmarks,provenance=dict(msb_source=str(msbpath.relative_to(ROOT)),msb_sha256=sha(msbpath),place_name_binder=str(placepath.relative_to(ROOT)),place_name_entry=placeentry,place_name_binder_sha256=sha(placepath),region_status='No certified localized district volumes; explicit Near landmark fallback only'))
  bannerids=sorted(set(abs(v['banner_id']) for v in bs if v['banner_id']!=-1));officialbanners={str(i):official.get(i,'').strip() for i in bannerids}
  audit[mid]=dict(authored_region_count=len(rs),authored_volume_count=sum(r['shape'] in (2,3,5) for r in rs),localized_region_volume_count=0,verified_landmarks=len(landmarks),official_collision_banner_names=officialbanners,collision_names_are_region_volumes=False)
  total+=len(landmarks)
 out=ctx.NAMES;out.mkdir(exist_ok=True)
 (out/'catalog.json').write_text(json.dumps(catalog,ensure_ascii=False,indent=2),encoding='utf-8')
 (out/'collision-links.json').write_text(json.dumps(collision_links,ensure_ascii=False,indent=2),encoding='utf-8')
 report=dict(totals=dict(areas=len(catalog),localized_region_volumes=0,verified_landmarks=total,rejected_lamp_labels=len(rejected),named_collision_links=sum(len(v) for v in collision_links.values())),maps=audit,rejected=rejected,runtime_policy=dict(horizontal_distance_m=60,vertical_distance_m=6,comparison='inclusive',choose='nearest Euclidean distance after both cutoffs',subtitle='Near <localized name>',no_match='omit subtitle'),limitations=['Current-map filtering required; no cross-map nearest search','Lamp proximity does not establish church/bridge/district membership','Boundaries, connectivity, wall visibility and quest state unavailable','No invented regions from AI/sound/wind boxes or collision bounding boxes'])
 (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
 print(json.dumps(dict(totals=report['totals'],rejected=rejected),indent=2))
