import pipeline_context as ctx
import sys,struct,json
from pathlib import Path
from soulstruct.dcx import decompress
from soulstruct.containers import Binder
ROOT=ctx.GAME_DATA
def raw(p):
 b=p.read_bytes();return decompress(b)[0] if b[:4]==b'DCX\0' else b
def parts(p):
 b=raw(p)
 def text(o):
  end=o
  while b[end:end+2]!=b'\0\0':end+=2
  return b[o:end].decode('utf-16le')
 lists={};off=16
 while off:
  _,count,no=struct.unpack_from('<iiq',b,off); offsets=struct.unpack_from(f'<{count}q',b,off+16)
  lists[text(no)]=offsets[:-1];off=offsets[-1]
 models=[text(o+struct.unpack_from('<q',b,o)[0]) for o in lists['MODEL_PARAM_ST']]
 result=[]
 for o in lists['PARTS_PARAM_ST']:
  desc,name=struct.unpack_from('<qq',b,o); kind,model=struct.unpack_from('<i',b,o+20)[0],struct.unpack_from('<i',b,o+28)[0]
  common,sub=struct.unpack_from('<qq',b,o+176)
  d=dict(name=text(o+name),desc=text(o+desc),kind=kind,model=models[model] if model>=0 else '',pos=list(struct.unpack_from('<3f',b,o+40)),rot=list(struct.unpack_from('<3f',b,o+52)),entity=struct.unpack_from('<i',b,o+common)[0])
  if kind in (2,10):d.update(zip(['ai','character','talk','player'],struct.unpack_from('<4i',b,o+sub+8)))
  result.append(d)
 return result
def instructions(p):
 b=raw(p); h=struct.unpack_from('<16Q',b,16)
 ec,eo,ic,io=h[:4]; argbase=h[13]
 result=[]
 for j in range(ec):
  eid,count,local,nrepl,repl,rest=struct.unpack_from('<5QI',b,eo+48*j)
  replacements=[struct.unpack_from('<3Qii',b,h[9]+repl+32*k) for k in range(nrepl)]
  for line in range(count):
   cat,idx,size,localarg,layer=struct.unpack_from('<IIQqq',b,io+local+32*line)
   result.append(dict(event=eid,line=line,category=cat,index=idx,args=b[argbase+localarg:argbase+localarg+size].hex(),replacements=[list(v[1:4]) for v in replacements if v[0]==line]))
 return result
def esd_commands(b):
 # External header is 108 bytes; internal offsets use that as their base.
 assert b[:4] in (b'fsSL',b'fSSL')
 width=8 if b[:4]==b'fsSL' else 4; code='q' if width==8 else 'i'; data=b[108:]
 machines,count=struct.unpack_from('<qq' if width==8 else '<ii',data,24 if width==8 else 20)
 found=set();visited=set()
 def commands(off,count):
  for j in range(count):found.add(struct.unpack_from('<ii',data,off+j*(8+2*width)))
 def condition(off):
  if off in visited:return
  visited.add(off);v=struct.unpack_from('<'+code*7,data,off)
  commands(v[1],v[2])
  for k in range(v[4]):condition(struct.unpack_from('<'+code,data,v[3]+k*width)[0])
 for i in range(count):
  _,off,n,_=struct.unpack_from('<'+code*4,data,machines+i*width*4)
  for j in range(n):
   v=struct.unpack_from('<'+code*9,data,off+j*width*9)
   for k in range(v[2]):condition(struct.unpack_from('<'+code,data,v[1]+k*width)[0])
   for k in (3,5,7):commands(v[k],v[k+1])
 return sorted(found)
