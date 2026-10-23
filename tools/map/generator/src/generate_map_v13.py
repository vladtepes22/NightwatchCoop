import pipeline_context as ctx
"""Add real continuous vertex heights and named collision footprints, read-only."""
from pathlib import Path
import sys,json,math,time
from generate_floorplans_v12 import all_geometry,floor_union,components,SPAN
from generate_navigation_maps import msb,collision_mesh,transformed,Binder
import numpy as np
from scipy.spatial import cKDTree
import shapely as sh

def named_meshes(game,mid,links):
    folder=game/'dvdroot_ps4/map'/mid;prefix='h'+mid[1:]
    def entries(path,**kwargs):
        return {Path(e.name.replace('\\','/')).name.split('.')[0]:e for e in Binder.from_path(path,**kwargs).entries} if path.exists() else {}
    path=folder/(prefix+'.hkxbhd');high=entries(path);low=entries(folder/('l'+mid[1:]+'.hkxbhd'))
    high.update(entries(path.with_name(path.name+'.patch'),bdt_path=folder/(prefix+'.hkxbdt.patch')))
    cache={};result={};evidence={};fail=[]
    for link in links:
        model=link['model'];name=link['name'];entry=high.get(model) or high.get(prefix+'_'+model[1:])
        if entry is None:entry=next((e for k,e in high.items() if k.startswith(model+'_')),None)
        if entry is None:entry=low.get('l'+mid[1:]+'_'+model[1:])
        try:
            if entry is None:raise ValueError('absent collision')
            if model not in cache:cache[model]=collision_mesh(entry.get_uncompressed_data())
            t=transformed(cache[model],link);n=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]);length=np.linalg.norm(n,axis=1)
            t=t[n[:,1]>.45*length]
            if len(t):result.setdefault(name,[]).append(t);evidence.setdefault(name,[]).append(link['evidence'])
        except Exception as ex:fail.append(link['part']+': '+str(ex))
    return {k:np.concatenate(v) for k,v in result.items()},evidence,fail

def sample_heights(floors,triangles):
    """Interpolate original 3D triangles at each vector floor-plan vertex."""
    coords=np.array(triangles,dtype=float).reshape(-1,2)
    if not len(coords):return []
    center=floors[:,:,[0,2]].mean(axis=1);tree=cKDTree(center)
    k=min(24,len(floors));dist,ids=tree.query(coords,k=k)
    if k==1:ids=ids[:,None];dist=dist[:,None]
    hits=floors[ids];a=hits[:,:,0,:][:,:,[0,2]];b=hits[:,:,1,:][:,:,[0,2]];c=hits[:,:,2,:][:,:,[0,2]]
    ab=b-a;ac=c-a;q=coords[:,None,:]-a
    den=ab[:,:,0]*ac[:,:,1]-ab[:,:,1]*ac[:,:,0];good=np.abs(den)>1e-8
    d=np.where(good,den,1);u=(q[:,:,0]*ac[:,:,1]-q[:,:,1]*ac[:,:,0])/d;v=(ab[:,:,0]*q[:,:,1]-ab[:,:,1]*q[:,:,0])/d
    inside=good&(u>=-.02)&(v>=-.02)&(u+v<=1.02)
    yy=hits[:,:,0,1]+u*(hits[:,:,1,1]-hits[:,:,0,1])+v*(hits[:,:,2,1]-hits[:,:,0,1])
    choices=np.argmax(inside,axis=1);has=inside.any(axis=1);y=yy[np.arange(len(coords)),choices]
    # A simplified contour can move <=8cm outside a triangle. In those rare
    # cases use the nearest original vertex within the same floor band.
    vertices=floors.reshape(-1,3);vtree=cKDTree(vertices[:,[0,2]])
    _,nearest=vtree.query(coords[~has]);y[~has]=vertices[nearest,1]
    assert np.isfinite(y).all()
    return np.round(y,3).reshape(-1,3).tolist()

def generate(mid,source,out,catalog,links):
    start=time.monotonic();data=json.loads((source/(mid+'.json')).read_text(encoding='utf-8'));tri,fail=all_geometry(ctx.GAME,mid)
    n=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]);length=np.linalg.norm(n,axis=1)
    floors=tri[n[:,1]>.45*length];bands=np.floor(floors[:,:,1].mean(axis=1)/SPAN).astype(int)
    for layer in data['layers']:
        fs=floors[bands==round(layer['height']/SPAN)]
        assert len(fs)
        layer['heights']=sample_heights(fs,layer['triangles'])
    entry=catalog[mid]
    # Replace placeholder lamp names only with joins confirmed against native
    # ReturnPointParam/EventText. Keep the same projected ground positions.
    real=[]
    for landmark in entry['landmarks']:
        nearest=min(data['lamps'],key=lambda l:math.hypot(l['pos'][0]-landmark['pos'][0],l['pos'][2]-landmark['pos'][2]),default=None)
        if nearest and math.hypot(nearest['pos'][0]-landmark['pos'][0],nearest['pos'][2]-landmark['pos'][2])<1:
            nearest=dict(nearest);nearest['name']=landmark['name'];real.append(nearest)
    data['lamps']=real
    meshes,proof,omissions=named_meshes(ctx.GAME,mid,links.get(mid,[]))
    regions=[]
    for name,mesh in meshes.items():
        mbands=np.floor(mesh[:,:,1].mean(axis=1)/SPAN).astype(int)
        for band in np.unique(mbands):
            part=mesh[mbands==band];floor=floor_union(part);tess=sh.constrained_delaunay_triangles(floor)
            ts=[[round(v,3) for xy in list(p.exterior.coords)[:3] for v in xy] for p in components(tess,'Polygon')]
            if ts:regions.append(dict(name=name,shape='footprint',min_y=round(float(part[:,:,1].min()),3),max_y=round(float(part[:,:,1].max()),3),triangles=ts,main_location=mid.startswith('m32_'),evidence={'kind':'localized named collision floor surface','source_links':proof[name],'height_band':int(band)}))
    entry['regions']=regions
    entry['provenance']['region_status']='Verified localized collision floor footprints; no invented volumes'
    entry['provenance']['omitted_collision_links']=omissions
    data['style_metadata']={'continuous_heights':'sampled original collision triangle barycentric interpolation; nearest original vertex fallback for contour simplification','original_floor_geometry_preserved':True}
    out.mkdir(parents=True,exist_ok=True);(out/(mid+'.json')).write_text(json.dumps(data,separators=(',',':')))
    result=dict(map=mid,regions=len(regions),named_surfaces=len(meshes),omissions=omissions,seconds=round(time.monotonic()-start,1));print(json.dumps(result),flush=True);return result

