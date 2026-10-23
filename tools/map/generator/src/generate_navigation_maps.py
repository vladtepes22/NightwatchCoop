"""Read-only navigation atlas generation from a user's Bloodborne 1.09 dump.

Geometry decoding follows the documented Havok 2014 compressed mesh format.
Uses Soulstruct's Binder/DCX readers; never writes to the supplied game folder.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import zlib
import re

import numpy as np
from PIL import Image, ImageDraw
from soulstruct.containers import Binder
from soulstruct.dcx import decompress

NAMES = {
    'm21_00':'Hunter\'s Dream', 'm21_01':'Abandoned Old Workshop',
    'm22_00':'Hemwick Charnel Lane', 'm23_00':'Old Yharnam',
    'm24_00':'Cathedral Ward', 'm24_01':'Central Yharnam',
    'm24_02':'Upper Cathedral Ward', 'm25_00':'Forsaken Castle Cainhurst',
    'm26_00':'Nightmare of Mensis', 'm27_00':'Forbidden Woods',
    'm28_00':'Yahar\'gul, Unseen Village',
    'm32_00':'Byrgenwerth / Lecture Building / Moonside Lake',
    'm33_00':'Nightmare Frontier', 'm34_00':'Hunter\'s Nightmare',
    'm35_00':'Research Hall', 'm36_00':'Fishing Hamlet',
}

def landmark_catalog(path):
    if not path:return []
    result=[]
    for line in path.read_text(encoding='utf-8').splitlines():
        cells=[v.strip() for v in line.split('|')]
        if len(cells)==6:
            try:result.append(dict(name=cells[1],pos=[float(v) for v in cells[2:5]]))
            except ValueError:pass
    return result

def msb(path):
    b=path.read_bytes()
    if b[:4]==b'DCX\0': b=decompress(b)[0]
    if b[:4]!=b'MSB ': raise ValueError('Not an MSB')
    def text(o):
        end=o
        while end+2<=len(b) and b[end:end+2]!=b'\0\0':end+=2
        return b[o:end].decode('utf-16le')
    lists={}; off=16; visited=set()
    while off:
        if off in visited:raise ValueError('Cyclic MSB')
        visited.add(off)
        _,count,no=struct.unpack_from('<iiq',b,off)
        offsets=struct.unpack_from(f'<{count}q',b,off+16)
        lists[text(no)]=offsets[:-1];off=offsets[-1]
    models=[text(o+struct.unpack_from('<q',b,o)[0]) for o in lists['MODEL_PARAM_ST']]
    parts=[]
    for o in lists['PARTS_PARAM_ST']:
        desc,name=struct.unpack_from('<qq',b,o)
        _,kind,_,model=struct.unpack_from('<4i',b,o+16)
        parts.append(dict(name=text(o+name),kind=kind,model=models[model] if model>=0 else '',
                          pos=struct.unpack_from('<3f',b,o+40),
                          rot=struct.unpack_from('<3f',b,o+52),scale=struct.unpack_from('<3f',b,o+64)))
    return parts

def collision_mesh(raw):
    if raw[:4]==b'DCX\0':raw=decompress(raw)[0]
    if raw[:8]!=bytes.fromhex('57e0e05710c0c010'):raise ValueError('Not a Havok packfile')
    start=struct.unpack_from('<h',raw,0x3e)[0]+0x40
    sections=[struct.unpack_from('<7I',raw,start+i*64+20) for i in range(3)]
    names=sections[0][0]; base,lo,gl,vi,ex,_,end=sections[2]
    fix={}
    for o in range(base+lo,base+gl-7,8):
        a,b=struct.unpack_from('<2I',raw,o)
        if a!=0xffffffff:fix[a]=b
    for o in range(base+gl,base+vi-11,12):
        a,sid,b=struct.unpack_from('<3I',raw,o)
        if a!=0xffffffff and sid==2:fix[a]=b
    trees=[];specs=[]
    for o in range(base+vi,base+ex-11,12):
        a,sid,n=struct.unpack_from('<3I',raw,o)
        if a==0xffffffff:continue
        name=raw[names+n:].split(b'\0',1)[0]
        specs.append((a,name))
        if name==b'hknpCompressedMeshShapeData':trees.append(a+16)
    if not trees:raise ValueError('Compressed collision tree missing')
    meshes=[];bodies=[]
    for a,name in specs:
        if name!=b'hknpPhysicsSystemData' or a+64 not in fix:continue
        count=struct.unpack_from('<I',raw,base+a+72)[0]
        for i in range(count):
            body=fix[a+64]+i*96;shape=fix.get(body)
            if shape is None:continue
            # Bloodborne's reuse-padding Havok 2014 layout stores the compressed
            # mesh data pointer at +0x58 (later layouts use +0x60).
            shape_data=fix.get(shape+88)
            if shape_data not in [t-16 for t in trees]:shape_data=fix.get(shape+96)
            if shape_data is None:continue
            position=np.array(struct.unpack_from('<4f',raw,base+body+48)[:3])
            q=np.array(struct.unpack_from('<4f',raw,base+body+64));q=q/max(float(np.linalg.norm(q)),1e-9)
            x,y,z,w=q
            rotation=np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                               [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                               [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
            bodies.append((shape_data+16,position,rotation))
    for tree in trees:
        tree_meshes=[]
        def array(offset,dtype,width=1):
            p=tree+offset; count=struct.unpack_from('<I',raw,base+p+8)[0]
            if not count:return np.empty((0,width),dtype=dtype) if width>1 else np.empty(0,dtype=dtype)
            if p not in fix or count>10_000_000:raise ValueError('Invalid collision array')
            a=np.frombuffer(raw,dtype=dtype,count=count*width,offset=base+fix[p])
            return a.reshape(-1,width) if width>1 else a
        domain=np.array(struct.unpack_from('<8f',raw,base+tree+16)).reshape(2,4)[:,:3]
        packed=array(112,'<u4');shared=array(128,'<u8');indices=array(96,'<u2');primitives=array(80,'u1',4)
        shared_xyz=np.column_stack([shared&0x1fffff,(shared>>21)&0x1fffff,shared>>42]).astype(float)
        shared_xyz=domain[0]+shared_xyz*(domain[1]-domain[0])/np.array([0x1fffff,0x1fffff,0x3fffff])
        count=struct.unpack_from('<I',raw,base+tree+72)[0]
        section_base=fix[tree+64]
        for i in range(count):
            o=base+section_base+i*96
            codec=np.array(struct.unpack_from('<6f',raw,o+48))
            first,sh,pr,_=struct.unpack_from('<4I',raw,o+72)
            npacked,nshared=struct.unpack_from('<2B',raw,o+88)
            v=packed[first:first+npacked]
            xyz=np.column_stack([v&0x7ff,(v>>11)&0x7ff,v>>22])*codec[3:]+codec[:3]
            if nshared:xyz=np.vstack([xyz,shared_xyz[indices[sh>>8:(sh>>8)+nshared]]])
            prim=primitives[pr>>8:(pr>>8)+(pr&255)]
            # Havok writes DE AD DE AD poison into padding slots in some sections.
            prim=prim[~np.all(prim==np.array([222,173,222,173]),axis=1)]
            if len(prim) and int(prim.max())>=len(xyz):raise ValueError(f'Collision vertex index out of bounds: section {i} vertices {len(xyz)} packed {npacked} shared {nshared} bad {prim[prim.max(axis=1)>=len(xyz)][:2].tolist()}')
            tris=[xyz[prim[:,:3]]]
            quad=prim[prim[:,2]!=prim[:,3]]
            if len(quad):tris.append(xyz[quad[:,[0,2,3]]])
            tree_meshes.append(np.concatenate(tris))
        geometry=np.concatenate(tree_meshes)
        transforms=[(p,r) for t,p,r in bodies if t==tree]
        if transforms:
            for p,r in transforms:meshes.append(geometry@r.T+p)
        else:meshes.append(geometry)
    return np.concatenate(meshes)

def transformed(tri,part):
    a=np.radians(part['rot']);cx,cy,cz=np.cos(a);sx,sy,sz=np.sin(a)
    rx=np.array([[1,0,0],[0,cx,-sx],[0,sx,cx]])
    ry=np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]])
    rz=np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]])
    return (tri*np.array(part['scale']))@(rz@ry@rx).T+np.array(part['pos'])

def generate(game,out,map_id,landmarks=()):
    root=game/'dvdroot_ps4'/'map';parts=msb(root/'mapstudio'/f'{map_id}.msb.dcx')
    folder=root/map_id; prefix='h'+map_id[1:]
    path=folder/f'{prefix}.hkxbhd'
    if not path.exists():return dict(map=map_id,error='Collision binder missing')
    entries={Path(e.name.replace('\\','/')).name.split('.')[0]:e for e in Binder.from_path(path).entries}
    low_path=folder/f'l{map_id[1:]}.hkxbhd'
    low_entries={Path(e.name.replace('\\','/')).name.split('.')[0]:e for e in Binder.from_path(low_path).entries} if low_path.exists() else {}
    patch=path.with_name(path.name+'.patch')
    if patch.exists():
        extra=Binder.from_path(patch,bdt_path=folder/f'{prefix}.hkxbdt.patch')
        entries.update({Path(e.name.replace('\\','/')).name.split('.')[0]:e for e in extra.entries})
    triangles=[];failures=[];cache={}
    for part in parts:
        if part['kind']!=5:continue
        model=part['model'];entry=entries.get(model)
        if entry is None:entry=entries.get(prefix+'_'+model[1:])
        if entry is None:
            entry=next((e for key,e in entries.items() if key.startswith(model+'_')),None)
        if entry is None:entry=low_entries.get('l'+map_id[1:]+'_'+model[1:])
        if entry is None:failures.append(model+': absent');continue
        try:
            if model not in cache:cache[model]=collision_mesh(entry.get_uncompressed_data())
            tri=transformed(cache[model],part)
            normal=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0])
            norm=np.linalg.norm(normal,axis=1)
            # Upward-facing floor/ramp surfaces, omitting walls and underside ceilings.
            keep=(normal[:,1] > 0.45*np.maximum(norm,1e-9))
            triangles.append(tri[keep])
        except Exception as ex:failures.append(model+': '+str(ex))
    if not triangles:raise ValueError(map_id+': no floor geometry; '+str(failures[:3]))
    tri=np.concatenate(triangles);verts=tri.reshape(-1,3)
    xmin,zmin=np.floor(verts[:,[0,2]].min(axis=0))-2
    xmax,zmax=np.ceil(verts[:,[0,2]].max(axis=0))+2
    cell=1.0;w=int(xmax-xmin);h=int(zmax-zmin)
    if w*h>8_000_000:raise ValueError('Map bounds too large')
    ymean=tri[:,:,1].mean(axis=1);bands=np.floor(ymean/6).astype(int)
    layers=[]
    for band in np.unique(bands):
        image=Image.new('1',(w,h));draw=ImageDraw.Draw(image)
        for t in tri[bands==band]:
            draw.polygon([(float(v[0]-xmin),float(v[2]-zmin)) for v in t],fill=1)
        mask=np.asarray(image,dtype=np.uint8)
        runs=[]
        active={}
        for z,row in enumerate(mask):
            delta=np.diff(np.pad(row.astype(np.int16),(1,1)))
            spans=list(zip(np.flatnonzero(delta==1),np.flatnonzero(delta==-1)))
            current={}
            for a,b in spans:
                key=(int(a),int(b));rect=active.pop(key,None)
                if rect is None:rect=[int(a)+xmin,z+zmin,int(b-a),1]
                else:rect[3]+=1
                current[key]=rect
            runs.extend(active.values());active=current
        runs.extend(active.values())
        layers.append(dict(height=int(band*6),rects=runs))
    lamps=[]
    for p in parts:
        if p['kind']==1 and p['model']=='o009900':
            nearest=min(landmarks,key=lambda l:sum((l['pos'][i]-p['pos'][i])**2 for i in range(3)),default=None)
            distance=sum((nearest['pos'][i]-p['pos'][i])**2 for i in range(3)) if nearest else float('inf')
            # o009900 is a return/warp reference, sometimes suspended above the
            # floor (Cainhurst's boss return point is over 11 m up). Project its
            # landmark elevation onto the highest collision surface below it.
            x,y,z=p['pos'];a2=tri[:,0][:,[0,2]];b2=tri[:,1][:,[0,2]];c2=tri[:,2][:,[0,2]]
            ab=b2-a2;ac=c2-a2;q=np.array([x,z])-a2
            determinant=ab[:,0]*ac[:,1]-ab[:,1]*ac[:,0]
            good=np.abs(determinant)>1e-8;den=np.where(good,determinant,1)
            u=(q[:,0]*ac[:,1]-q[:,1]*ac[:,0])/den;v=(ab[:,0]*q[:,1]-ab[:,1]*q[:,0])/den
            inside=good&(u>=-.01)&(v>=-.01)&(u+v<=1.01)
            height=tri[:,0,1]+u*(tri[:,1,1]-tri[:,0,1])+v*(tri[:,2,1]-tri[:,0,1])
            floor=height[inside&(height<=y+1)&(height>=y-30)]
            if not len(floor):
                # Warp references can sit just outside the physical lamp base.
                # Search only a two-metre neighbourhood, retaining source_pos.
                for dx,dz in [(-2,0),(2,0),(0,-2),(0,2),(-2,-2),(-2,2),(2,-2),(2,2)]:
                    q=np.array([x+dx,z+dz])-a2
                    u=(q[:,0]*ac[:,1]-q[:,1]*ac[:,0])/den;v=(ab[:,0]*q[:,1]-ab[:,1]*q[:,0])/den
                    inside=good&(u>=-.01)&(v>=-.01)&(u+v<=1.01)
                    height=tri[:,0,1]+u*(tri[:,1,1]-tri[:,0,1])+v*(tri[:,2,1]-tri[:,0,1])
                    candidate=height[inside&(height<=y+1)&(height>=y-30)]
                    if len(candidate):floor=candidate;break
            ground=float(floor.max()) if len(floor) else y
            lamps.append(dict(name=nearest['name'] if distance<100 else 'Lamp '+str(len(lamps)+1),
                              pos=[x,ground,z],source_pos=p['pos']))
    data=dict(version=1,id=map_id,name=NAMES.get(map_id[:6],map_id),bounds=[xmin,zmin,xmax,zmax],
              layers=layers,lamps=lamps,triangles=len(tri),failures=failures)
    out.mkdir(parents=True,exist_ok=True)
    (out/f'{map_id}.json').write_text(json.dumps(data,separators=(',',':')),encoding='utf-8')
    print(map_id,len(tri),'floor triangles',sum(len(l['rects']) for l in layers),'rects',len(failures),'failures',flush=True)
    return {k:data[k] for k in ('id','name','bounds','triangles','failures')}

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--game',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--map');ap.add_argument('--landmarks',type=Path);a=ap.parse_args()
    maps=[a.map] if a.map else [p.name.split('.')[0] for p in sorted((a.game/'dvdroot_ps4/map/mapstudio').glob('*.msb.dcx'))
                               if p.name[:6] in NAMES and p.name[7:9]=='00' and p.name[10:12]=='00']
    result=[generate(a.game,a.out,m,landmark_catalog(a.landmarks)) for m in maps]
    if not a.map:(a.out/'atlas.json').write_text(json.dumps(dict(version=1,maps=result),indent=2),encoding='utf-8')
