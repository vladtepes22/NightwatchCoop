"""Local Atlas extraction; game data is read only and never distributed."""
import argparse,hashlib,json,math,os,platform,shutil,struct,sys,time,traceback,uuid
from pathlib import Path
import pipeline_context as ctx
import game_identity
VERSION='1.0.0'
def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        while block:=f.read(4*1024*1024):h.update(block)
    return h.hexdigest()
def emit(stage,**kw):print(json.dumps(dict(stage=stage,**kw),ensure_ascii=True),flush=True)
def emit_progress(phase,completed,total,start,end,message):
    """Stable, monotonic work progress; percentages are units, not time estimates."""
    if not (0 <= completed <= total and 0 <= start <= end <= 100):
        raise ValueError('Invalid Atlas generation progress')
    # Diagnostic emit() records keep their historic stage identifiers. This
    # separate record is the stable, human-readable progress protocol.
    print(json.dumps(dict(progress=start+(end-start)*completed//total,
                          stage=message,phase=phase,completed=completed,total=total),
                     ensure_ascii=True),flush=True)
def jsonwrite(path,value):path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
def contained_target(output,relative):
    path=(output/relative).resolve()
    if output not in path.parents:raise ValueError('Generated output path escapes the selected Atlas root: '+relative)
    return path
def install_file(source,destination):
    temporary=destination.with_name(destination.name+'.atlas-generation-tmp')
    destination.parent.mkdir(parents=True,exist_ok=True)
    try:shutil.copyfile(source,temporary);os.replace(temporary,destination)
    finally:
        if temporary.exists():temporary.unlink()
def commit_generated(output,stage,sources,manifest):
    """Fixed owned-path transaction with rollback, never paths from old metadata."""
    records={relative:sha(source) for relative,source in sources.items()}
    manifest=dict(manifest,files=records);ready=stage/'ready-generated-assets.json';jsonwrite(ready,manifest)
    updates=dict(sources);updates['maps/generated-assets.json']=ready
    backups={};changed=[]
    for relative in updates:
        target=contained_target(output,relative)
        if target.exists():
            backup=stage/'previous-generated'/relative;backup.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(target,backup);backups[relative]=backup
    try:
        for relative,source in updates.items():
            changed.append(relative);install_file(source,contained_target(output,relative))
        assert all(sha(contained_target(output,path))==digest for path,digest in records.items())
    except BaseException:
        for relative in reversed(changed):
            target=contained_target(output,relative)
            if relative in backups:install_file(backups[relative],target)
            elif target.exists():target.unlink()
        raise
    return records
def read_sfo(path):
    b=path.read_bytes();magic,version,keybase,values,count=struct.unpack_from('<5I',b);assert magic==0x46535000
    out={}
    for i in range(count):
        keyoff,fmt,length,maximum,offset=struct.unpack_from('<HHIII',b,20+i*16)
        key=b[keybase+keyoff:].split(b'\0',1)[0].decode('utf-8');value=b[values+offset:values+offset+length]
        out[key]=value.rstrip(b'\0').decode('utf-8') if fmt in (0x204,0x004) else value.hex()
    return out
def portable(value):
    # Internal binder entry strings often contain the game's build-machine path.
    # Runtime evidence needs the entry basename plus its enclosing binder path.
    if isinstance(value,list):return [portable(v) for v in value]
    if isinstance(value,dict):return {k:portable(v) for k,v in value.items()}
    if isinstance(value,str):
        text=value.replace('\\','/')
        if len(text)>2 and text[1]==':' and text[2]=='/':return text.rsplit('/',1)[-1]
        return text
    return value
def invariants(maps,catalog):
    atlas=json.loads((maps/'atlas.json').read_text(encoding='utf-8'));assert len(atlas['maps'])==16,'Expected the 16 supported ordinary maps'
    counts={'maps':16,'layers':0,'floor_triangles':0,'wall_segments':0,'lamps':0,'ladders':0,'npcs':0,'gates':0,'catalog_regions':0}
    for item in atlas['maps']:
        mid=item['id'];data=json.loads((maps/(mid+'.json')).read_text(encoding='utf-8'));assert data['id']==mid and data['version']==2
        assert data['bounds'][0]<data['bounds'][2] and data['bounds'][1]<data['bounds'][3]
        counts['lamps']+=len(data['lamps']);assert data['layers']
        for l in data['layers']:
            assert l['span']==3 and len(l['triangles'])==len(l['heights']);counts['layers']+=1;counts['floor_triangles']+=len(l['triangles']);counts['wall_segments']+=len(l['walls'])
            for tri,h in zip(l['triangles'],l['heights']):assert len(tri)==6 and len(h)==3 and all(math.isfinite(x) for x in tri+h)
            for wall in l['walls']:assert len(wall)==4 and all(math.isfinite(x) for x in wall)
        for marker in data['markers']:
            assert len(marker['pos'])==3 and all(math.isfinite(x) for x in marker['pos'])
            kind=marker['kind'];assert kind in ('ladder','npc','gate');counts[{'ladder':'ladders','npc':'npcs','gate':'gates'}[kind]]+=1
            if kind=='gate':assert marker['provenance']['opening_object_acts'] or marker['provenance']['opening_animations'],'Gate lacks verified opening evidence'
        assert mid in catalog;counts['catalog_regions']+=len(catalog[mid]['regions'])
    return counts
def main():
    # Launcher pipes and non-English Windows consoles must not inherit an ANSI
    # encoding for source names, diagnostics or paths containing Unicode.
    for stream in (sys.stdout,sys.stderr):
        if hasattr(stream,'reconfigure'):stream.reconfigure(encoding='utf-8',errors='backslashreplace')
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--game-dir',type=Path,required=True);parser.add_argument('--output-dir',type=Path,required=True);parser.add_argument('--staging-dir',type=Path);parser.add_argument('--replace',action='store_true',help='Regenerate fixed owned assets with staged backup and rollback');parser.add_argument('--version',action='version',version=VERSION);args=parser.parse_args()
    game=args.game_dir.resolve();output=args.output_dir.resolve()
    if not (game/'dvdroot_ps4').is_dir() and (game/'Image0/dvdroot_ps4').is_dir():game=game/'Image0'
    if not (game/'dvdroot_ps4').is_dir():raise ValueError('Select the game root containing dvdroot_ps4 and eboot.bin (or its parent containing Image0).')
    if output==game or game in output.parents or output in game.parents:raise ValueError('Atlas output must be separate from the protected game folder.')
    stage=args.staging_dir.resolve() if args.staging_dir else output/'.generation-staging'/str(uuid.uuid4())
    if stage==game or game in stage.parents or stage in game.parents:raise ValueError('Staging must be separate from game files.')
    if stage.exists() and any(stage.iterdir()):raise ValueError('Staging directory must be empty; choose a new directory.')
    if (output/'maps/generated-assets.json').exists() and not args.replace:raise ValueError('Generated assets already exist. Use --replace to repair/regenerate with a staged backup.')
    game_identity.require_supported(game)
    protected={p.relative_to(game).as_posix():sha(p) for p in [game/'eboot.bin',game/'sce_sys/param.sfo']}
    sfo=read_sfo(game/'sce_sys/param.sfo');emit('preflight',generator_version=VERSION,game_version=sfo.get('APP_VER'),title_id=sfo.get('TITLE_ID'),game_eboot_sha256=protected['eboot.bin'])
    emit_progress('collision',0,16,0,30,'Reading collision maps (0/16)')
    output.mkdir(parents=True,exist_ok=True);ctx.configure(game,stage)
    # The general-purpose library normally creates editor config/log files and
    # scans other games for proprietary tools. Readers here use in-memory paths.
    import soulstruct_local_config
    sys.modules['soulstruct.config']=soulstruct_local_config
    # Explicit imports make the frozen build's dependency discovery reliable.
    import generate_navigation_maps as base
    import probe_markers,update_verified_markers as markers
    import generate_floorplans_v12 as vector
    import probe_gates,extract_verified_gates as gates
    import probe_location_names,extract_subarea_names as names
    import generate_map_v13 as continuous
    import extract_native_map_font as native_font
    import extract_atlas_features as features
    import parallel_maps
    mids=[p.name.split('.')[0] for p in sorted((game/'dvdroot_ps4/map/mapstudio').glob('*.msb.dcx')) if p.name[:6] in base.NAMES and p.name[7:9]=='00' and p.name[10:12]=='00']
    if len(mids)!=16:raise ValueError(f'Game source contains {len(mids)} supported maps, expected 16. Check the complete game+DLC extraction.')
    start=time.monotonic()
    emit('base-collision',maps=mids,workers=parallel_maps.worker_count())
    result=parallel_maps.run(base.generate,[(game,ctx.BASE,mid) for mid in mids],game,stage,
        lambda n:emit_progress('collision',n,16,0,30,f'Reading collision maps ({n}/16)'))
    if any('id' not in r for r in result):raise ValueError('Missing collision binders in selected game source')
    atlas=dict(version=1,maps=result);jsonwrite(ctx.BASE/'atlas.json',atlas)
    emit('verified-ladders-npcs');markers.update(ctx.BASE,ctx.MARKERS)
    emit_progress('markers',1,1,30,33,'Locating ladders and NPCs')
    emit('vector-floorplans',maps=mids)
    geometry=parallel_maps.floorplans(game,stage,ctx.MARKERS,ctx.VECTOR,mids,{r['id']:r['triangles'] for r in result},
        lambda n,total:emit_progress('floorplans',n,total,33,76,f'Building floor plans ({n}/{total})'))
    jsonwrite(ctx.VECTOR/'atlas.json',atlas);jsonwrite(ctx.REPORTS/'geometry-report.json',geometry)
    emit('verified-gates');gates.main();gate_catalog=json.loads(ctx.GATES.read_text(encoding='utf-8'))
    emit_progress('gates',1,1,76,80,'Locating gates')
    # The old pipeline produced only a separate gate catalog. Merge explicitly,
    # retaining authored positions and every existing verified ladder/NPC.
    for mid in mids:
        path=ctx.VECTOR/(mid+'.json');data=json.loads(path.read_text(encoding='utf-8'));prior=data['markers'];data['markers']=prior+gate_catalog[mid]
        assert data['markers'][:len(prior)]==prior
        data['gate_metadata']={'source':'verified ObjectAct and resolved opening-event evidence','state_tracking':False,'count':len(gate_catalog[mid])};jsonwrite(path,data)
    emit('localized-names');names.main();catalog=json.loads((ctx.NAMES/'catalog.json').read_text(encoding='utf-8'));links=json.loads((ctx.NAMES/'collision-links.json').read_text(encoding='utf-8'))
    emit_progress('names',1,1,80,83,'Reading area names')
    generated=stage/'final-maps'
    emit('continuous-heights',maps=mids)
    reports=[]
    for mid,(report,entry) in zip(mids,parallel_maps.run(parallel_maps.heights,[(mid,ctx.VECTOR,generated,catalog,links) for mid in mids],game,stage,
            lambda n:emit_progress('heights',n,16,83,95,f'Adding elevation ({n}/16)'))):
        reports.append(report);catalog[mid]=entry
    jsonwrite(generated/'atlas.json',atlas);jsonwrite(generated/'catalog.json',catalog);jsonwrite(ctx.REPORTS/'continuous-report.json',reports)
    parallel_maps.run(parallel_maps.portable_rewrite,[(path,) for path in sorted(generated.glob('*.json'))],game,stage)
    emit('native-font');font=stage/'native-font.ttf';native_font.extract(game/'dvdroot_ps4/menu/enggb/font.gfx',font)
    emit_progress('font',1,1,95,97,'Preparing map labels')
    features.extract(game,generated)
    catalog=json.loads((generated/'catalog.json').read_text(encoding='utf-8'));counts=invariants(generated,catalog)
    # Protected fingerprints are rechecked after all extraction, before success.
    for relative,digest in protected.items():assert sha(game/relative)==digest,'Protected game input changed during generation'
    emit_progress('verify',1,1,97,99,'Checking generated maps')
    sources={'maps/'+source.name:source for source in sorted(generated.glob('*.json'))};sources['art/native-font.ttf']=font
    manifest={'schema':1,'version':1,'generator_version':VERSION,'game_image_sha256':game_identity.GAME_IMAGE_SHA256,'game_eboot_sha256':protected['eboot.bin'],'game_version':sfo.get('APP_VER'),'title_id':sfo.get('TITLE_ID'),'protected_inputs':protected,'counts':counts,'geometry_semantics':'16 ordinary collision-derived maps; static markers, no live gate/quest state','local_only':True}
    commit_generated(output,stage,sources,manifest)
    emit_progress('complete',1,1,99,100,'Atlas maps ready')
    emit('complete',manifest='maps/generated-assets.json',seconds=round(time.monotonic()-start,2),counts=counts)
    return 0
if __name__=='__main__':
    try:sys.exit(main())
    except Exception as error:emit('error',message=str(error));traceback.print_exc(file=sys.stderr);sys.exit(1)
