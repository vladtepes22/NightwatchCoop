import pipeline_context as ctx
import json,struct
from collections import defaultdict
from pathlib import Path
from probe_markers import parts,instructions,raw,ROOT

def object_acts(path):
 b=raw(path)
 def text(o):
  end=o
  while b[end:end+2]!=b'\0\0':end+=2
  return b[o:end].decode('utf-16le')
 lists={};off=16
 while off:
  _,count,no=struct.unpack_from('<iiq',b,off); offsets=struct.unpack_from(f'<{count}q',b,off+16)
  lists[text(no)]=offsets[:-1];off=offsets[-1]
 result=[]
 for o in lists['EVENT_PARAM_ST']:
  name,index,kind=struct.unpack_from('<qii',b,o)
  if kind!=7:continue
  common,sub=struct.unpack_from('<qq',b,o+24)
  entity,part,param,state,flag=struct.unpack_from('<iiiB3xi',b,o+sub)
  result.append(dict(name=text(o+name),entity=entity,part_index=part,param=param,default_state=state,flag=flag))
 return result

def resolved_instructions(ins):
 events=defaultdict(list)
 for i in ins:events[i['event']].append(i)
 result=[];seen=set()
 def walk(eid,context=b'',depth=0):
  if (eid,context) in seen or depth>15:return
  seen.add((eid,context))
  for i in events[eid]:
   data=bytearray.fromhex(i['args']);valid=True
   for dest,source,size in i['replacements']:
    if source+size>len(context):valid=False;break
    data[dest:dest+size]=context[source:source+size]
   if valid:
    result.append(dict(i,args=data.hex(),invocation=f'{eid}:{context.hex()}'))
    if i['category']==2000 and i['index']==0 and len(data)>=8:
     slot,target=struct.unpack_from('<II',data)
     if target in events:walk(target,bytes(data[8:]),depth+1)
 walk(0);walk(50)
 return result

def door_description(desc):return any(s in desc for s in ('扉','門','ドア','ゲート','鉄柵','格子','戸'))

