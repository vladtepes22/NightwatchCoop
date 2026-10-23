import pipeline_context as ctx
"""Read-only vector floor plans and wall sections from authored collision meshes.

Only offline map artifacts are written. Openable gates are merged separately,
requiring authored interaction evidence rather than guesses from appearance.
"""
from pathlib import Path
import sys, json, argparse, hashlib, time
import numpy as np
import shapely as sh
from shapely.geometry import Polygon, LineString
from generate_navigation_maps import msb, collision_mesh, transformed, Binder

SPAN = 3.0
def components(geometry, kind):
    if geometry.geom_type == kind:
        yield geometry
    elif hasattr(geometry, 'geoms'):
        for g in geometry.geoms: yield from components(g, kind)

def all_geometry(game, mid):
    root=game/'dvdroot_ps4/map'; folder=root/mid; prefix='h'+mid[1:]
    path=folder/(prefix+'.hkxbhd')
    def entries(path, **kwargs):
        return {Path(e.name.replace('\\','/')).name.split('.')[0]:e for e in Binder.from_path(path, **kwargs).entries} if path.exists() else {}
    high=entries(path); low=entries(folder/('l'+mid[1:]+'.hkxbhd'))
    high.update(entries(path.with_name(path.name+'.patch'), bdt_path=folder/(prefix+'.hkxbdt.patch')))
    meshes=[]; cache={}; failures=[]
    for p in msb(root/'mapstudio'/(mid+'.msb.dcx')):
        if p['kind']!=5: continue
        model=p['model']; entry=high.get(model) or high.get(prefix+'_'+model[1:])
        if entry is None: entry=next((v for k,v in high.items() if k.startswith(model+'_')),None)
        if entry is None: entry=low.get('l'+mid[1:]+'_'+model[1:])
        try:
            if entry is None: raise ValueError('collision absent')
            if model not in cache: cache[model]=collision_mesh(entry.get_uncompressed_data())
            meshes.append(transformed(cache[model],p))
        except Exception as e: failures.append(model+': '+str(e))
    if not meshes: raise ValueError('No decoded geometry for '+mid)
    return np.concatenate(meshes), failures

def floor_union(tris):
    projected=tris[:,:,[0,2]]
    polygons=sh.polygons(projected)
    polygons=polygons[sh.area(polygons)>.025]
    # Chunked exact polygon union avoids any square-pixel approximation.
    chunks=[sh.union_all(polygons[i:i+5000],grid_size=.025) for i in range(0,len(polygons),5000)]
    union=sh.union_all(chunks,grid_size=.025)
    union=sh.make_valid(union)
    polygons=[p for p in components(union,'Polygon') if p.area>.06]
    return sh.simplify(sh.union_all(polygons),.08,preserve_topology=True)

def wall_section(tris, height):
    """Intersect steep collision faces with a horizontal chest-height plane."""
    if not len(tris):return []
    pts=[]; valid=[]
    for a,b in ((0,1),(1,2),(2,0)):
        dy=tris[:,b,1]-tris[:,a,1]
        ok=(np.abs(dy)>1e-7)&(height>=np.minimum(tris[:,a,1],tris[:,b,1]))&(height<=np.maximum(tris[:,a,1],tris[:,b,1]))
        t=(height-tris[:,a,1])/np.where(ok,dy,1)
        p=tris[:,a]+t[:,None]*(tris[:,b]-tris[:,a])
        pts.append(p[:,[0,2]]);valid.append(ok)
    lines=[]
    for a,b in ((0,1),(0,2),(1,2)):
        ok=valid[a]&valid[b]
        pairs=np.stack([pts[a][ok],pts[b][ok]],axis=1)
        if len(pairs):
            pairs=pairs[np.linalg.norm(pairs[:,1]-pairs[:,0],axis=1)>.12]
            lines.extend(sh.linestrings(pairs))
    return lines

def vector_layer(floors, walls, base):
    floor=floor_union(floors)
    anchor=float(np.median(floors[:,:,1].mean(axis=1)))
    sections=wall_section(walls,anchor+1.2)
    if sections:
        walls2d=sh.intersection(sh.union_all(sections,grid_size=.025),floor.buffer(.75))
        walls2d=sh.simplify(sh.line_merge(walls2d),.04,preserve_topology=True)
    else: walls2d=LineString()
    triangles=[];outlines=[];segments=[]
    for p in components(floor,'Polygon'):
        for ring in (p.exterior,*p.interiors):
            outlines.append([[round(x,3),round(z,3)] for x,z in ring.coords])
    tess=sh.constrained_delaunay_triangles(floor)
    for p in components(tess,'Polygon'):
        triangles.append([round(v,3) for xy in list(p.exterior.coords)[:3] for v in xy])
    for line in components(walls2d,'LineString'):
        xy=list(line.coords)
        segments.extend([[round(v,3) for p in (a,b) for v in p] for a,b in zip(xy,xy[1:]) if np.linalg.norm(np.array(b)-a)>.12])
    return dict(height=base,span=SPAN,anchor=round(anchor,3),triangles=triangles,outlines=outlines,walls=segments)

def prepare(game, mid):
    """A map's collision split into floors, walls and the floors' height bands."""
    tri,failures=all_geometry(game,mid)
    normals=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]);length=np.linalg.norm(normals,axis=1)
    good=np.isfinite(tri).all(axis=(1,2))&(length>1e-8)
    floors=tri[good&(normals[:,1]>.45*length)]
    walls=tri[good&(np.abs(normals[:,1])<.30*length)&(np.ptp(tri[:,:,1],axis=1)>.65)]
    bands=np.floor(floors[:,:,1].mean(axis=1)/SPAN).astype(int)
    return tri,failures,floors,walls,bands

def band_layers(floors, walls, bands, selected):
    """(band, layer) for each selected band that has floor triangles."""
    layers=[]
    for band in selected:
        layer=vector_layer(floors[bands==band],walls,float(band*SPAN))
        if layer['triangles']:layers.append((int(band),layer))
    return layers

def generate(game, source, out, mid):
    start=time.monotonic();tri,failures,floors,walls,bands=prepare(game,mid)
    layers=[layer for _,layer in band_layers(floors,walls,bands,np.unique(bands))]
    return write(source,out,mid,len(tri),failures,layers,start)

def write(source, out, mid, collision_triangles, failures, layers, start):
    data=json.loads((source/(mid+'.json')).read_text(encoding='utf-8'))
    data.update(version=2,layers=layers,failures=failures)
    data['floorplan_metadata']=dict(format='vector collision floor plan',height_band=SPAN,wall_semantics='Steep collision surfaces intersected 1.2m above representative floor height; not live gate state',source='local high-resolution collision meshes',simplification_m=.08,collision_triangles=collision_triangles,omitted_collisions=failures)
    out.mkdir(parents=True,exist_ok=True)
    (out/(mid+'.json')).write_text(json.dumps(data,separators=(',',':')),encoding='utf-8')
    result=dict(map=mid,layers=len(layers),triangles=sum(len(l['triangles']) for l in layers),walls=sum(len(l['walls']) for l in layers),omissions=failures,seconds=round(time.monotonic()-start,1))
    print(json.dumps(result),flush=True)
    return result

