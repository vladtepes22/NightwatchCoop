import pipeline_context as ctx
import json,struct
from pathlib import Path
from probe_markers import ROOT,raw,Binder
from soulstruct.base.text.fmg import FMG

def sections(b):
 def text(o):
  end=o
  while b[end:end+2]!=b'\0\0':end+=2
  return b[o:end].decode('utf-16le')
 lists={};off=16
 while off:
  _,count,no=struct.unpack_from('<iiq',b,off); offsets=struct.unpack_from(f'<{count}q',b,off+16)
  lists[text(no)]=offsets[:-1];off=offsets[-1]
 return lists,text

def regions(path):
 b=raw(path);lists,text=sections(b);result=[]
 for o in lists['POINT_PARAM_ST']:
  name=struct.unpack_from('<q',b,o)[0];shape=struct.unpack_from('<i',b,o+16)[0]
  pos=struct.unpack_from('<3f',b,o+20);rot=struct.unpack_from('<3f',b,o+32)
  sd,common=struct.unpack_from('<qq',b,o+64)
  sizes={0:0,1:1,2:1,3:2,4:2,5:3}
  size=struct.unpack_from('<'+str(sizes[shape])+'f',b,o+sd) if sizes[shape] else ()
  result.append(dict(authored_name=text(o+name),shape=shape,pos=list(pos),rotation=list(rot),size=list(size),entity_id=struct.unpack_from('<i',b,o+common)[0]))
 return result

def banners(path):
 b=raw(path);lists,text=sections(b);result=[]
 for o in lists['PARTS_PARAM_ST']:
  if struct.unpack_from('<i',b,o+20)[0]!=5:continue
  desc,name=struct.unpack_from('<qq',b,o);sub=struct.unpack_from('<q',b,o+184)[0]
  banner=struct.unpack_from('<h',b,o+sub+8)[0];modelindex=struct.unpack_from('<i',b,o+28)[0]
  models=[text(m+struct.unpack_from('<q',b,m)[0]) for m in lists['MODEL_PARAM_ST']]
  result.append(dict(part=text(o+name),description=text(o+desc),banner_id=banner,model=models[modelindex],pos=list(struct.unpack_from('<3f',b,o+40)),rot=list(struct.unpack_from('<3f',b,o+52)),scale=list(struct.unpack_from('<3f',b,o+64))))
 return result

def place_names():
 path=ROOT/'msg/engus/item.msgbnd.dcx';binder=Binder.from_path(path)
 entry=next(e for e in binder.entries if e.id==19)
 return FMG.from_bytes(entry.get_uncompressed_data()).entries,entry.name,path

