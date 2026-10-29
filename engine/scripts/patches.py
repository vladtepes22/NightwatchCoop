# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import hashlib
from party_env import party_enabled, party_switch_on
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

EBOOT_BASE=0x400000
SKIP_NETWORK_CHOICE='Skip Online/Offline Choice'


SKIP_NETWORK_CHOICE_ONLINE='Party: Skip Online/Offline Choice (Online)'


def skip_network_choice(env=os.environ):
    return env.get('BB_SKIP_NETWORK_CHOICE','1')!='0'


def network_choice_patch(env=os.environ, available=None):
    """The title-dialog patch for BB_SKIP_NETWORK_CHOICE: '0' none (the game asks), 'online' the
    PLAY ONLINE variant (party mode; none, with a warning, when the XML lacks it), else PLAY OFFLINE."""
    # Party mode (BB_PARTY set) boots ONLINE through the party NP layer unless told otherwise.
    value=env.get('BB_SKIP_NETWORK_CHOICE','online' if party_mode(env) else '1').strip().lower()
    if value=='0':
        return None
    if value=='online':
        if available is not None and SKIP_NETWORK_CHOICE_ONLINE not in available:
            print(f'Patches: BB_SKIP_NETWORK_CHOICE=online but "{SKIP_NETWORK_CHOICE_ONLINE}" is not in the '
                  'patch file; the title asks PLAY ONLINE / PLAY OFFLINE',file=sys.stderr)
            return None
        return SKIP_NETWORK_CHOICE_ONLINE
    return SKIP_NETWORK_CHOICE


# Party co-op (BB_PARTY set): the seamless patches (docs/party/seamless_rules.md), unless
# BB_PARTY_SEAMLESS=0. The community "Disable HTTP Requests" patch is never applied in party mode
# (the party's FROM API runs over the game's HTTP calls).
PARTY_SEAMLESS=['Party: Bells anywhere','Party: Bells after boss defeated',
                'Party: Keep session on map reload','Party: SOS sign timeout 30s']
PARTY_NO_INSIGHT='Party: Bells without Insight'
# Join timing (docs/party/join_timing.md): the multiplayer banners' hold and the bell "Searching..."
# banners (BB_PARTY_FAST_MESSAGES=0 turns them off); the game's own join / summon pacing
# (BB_PARTY_FAST_JOIN=0 turns it off). Both on by default in party mode, independent of
# BB_PARTY_SEAMLESS, and left out of the party version check (Party="cosmetic").
PARTY_FAST_MESSAGES=['Party: Fast multiplayer messages','Party: Hide bell searching messages']
PARTY_FAST_JOIN=['Party: Fast join','Party: Fast rejoin']
# With fast join unless BB_PARTY_FAST_SIGN=0: the first sign 0.5 s after a bell (docs/party/join_timing.md).
PARTY_FAST_SIGN='Party: Fast bell sign'
DISABLE_HTTP='Disable HTTP Requests'


def party_mode(env=os.environ):
    """BB_PARTY turns party mode on (party_env.py: unset, empty, 0, off, false, no = off)."""
    return party_enabled(env)


def party_patches(env=os.environ):
    if not party_mode(env):
        return []
    names=[]
    if env.get('BB_PARTY_SEAMLESS','1').strip()!='0':
        names+=PARTY_SEAMLESS
        if env.get('BB_PARTY_BELL_NO_INSIGHT','1').strip()!='0':
            names.append(PARTY_NO_INSIGHT)
    if party_switch_on(env,'BB_PARTY_FAST_JOIN'):
        names+=PARTY_FAST_JOIN
    if party_switch_on(env,'BB_PARTY_FAST_JOIN') and party_switch_on(env,'BB_PARTY_FAST_SIGN'):
        names.append(PARTY_FAST_SIGN)
    if party_switch_on(env,'BB_PARTY_FAST_MESSAGES'):
        names+=PARTY_FAST_MESSAGES
    return names


# Party version check (gpu/shim/party/party_runtime.cpp, PartyLink HELLO): the host admits a guest
# only with the same eboot.bin, the same gameplay patches and the same gameplay mods. Patches that
# change only what one machine draws or plays back (graphics, frame rate, resolution, LOD, sound
# workarounds, camera and input feel, language) are left out of that check, so friends can play
# with different graphics settings. Every other patch counts: the party patches, the skip-online
# dialog, rally / physics / debug / cheat patches and any patch not listed here (external ones
# too, unless their XML marks them Party="cosmetic").
COSMETIC_PATCHES={
    # Frame rate (the 30 FPS rule's speed fixes go with them).
    '30 FPS++','60 FPS++','90 FPS++','Uncap FPS++','Sprint Fix (High FPS)',
    # Resolution / render size / memory for it.
    'Optimal 1080p','Increased Graphics Heap Sizes',
    # Effects and level of detail.
    'Disable AA','Disable Chromatic Aberration','Disable DoF','Disable Dynamic Light Shadows (perf increase)',
    'Disable Motion Blur (perf increase)','Disable SSAO','Enable Screen Space Reflections (READ NOTE)',
    'Model LOD -2 (Highest)','Model LOD 1 (Lower)','Model LOD 2 (Lowest)','Performance Patch (perf increase)',
    # Platform workarounds (CPU / sound).
    'Intel Black Tonemap Fix','Intel 12th Gen+ SFX workaround','FMOD Crash Fix',
    # Presentation, camera and input on this machine only.
    '50% Text scale','Skip Intro','Unlock Game Region','Bookmark and Capture outputs',
    'Increased camera distance','Disable Camera Auto Rotation via Movement','Sensitive Analog Input (easier to run)',
    # Party join timing and banners (BB_PARTY_FAST_JOIN / BB_PARTY_FAST_MESSAGES): local pacing only.
    'Party: Fast join','Party: Fast rejoin','Party: Fast bell sign','Party: Fast multiplayer messages','Party: Hide bell searching messages',
}
COSMETIC_PREFIXES=('Resolution Patch ',)
COSMETIC_MARKS=('Light Grid',)
PARTY_PATCH_FILE='party_patch_hash.txt'


def patch_is_cosmetic(name, meta=None):
    """True when patch `name` changes no gameplay (left out of the party version check)."""
    if meta is not None and meta.get('Party','').strip().lower() in ('cosmetic','gameplay'):
        return meta.get('Party').strip().lower()=='cosmetic'
    return (name in COSMETIC_PATCHES or name.startswith(COSMETIC_PREFIXES)
            or any(mark in name for mark in COSMETIC_MARKS))


def party_patch_set(items):
    """(hash hex, names) of the gameplay patches. items: [(name, meta or None, writes)]; cosmetic
    ones are dropped; the hash covers names and bytes, independent of order."""
    gameplay=sorted((name,writes) for name,meta,writes in items if not patch_is_cosmetic(name,meta))
    digest=hashlib.sha256(b'bbparty-patches-1')
    for name,writes in gameplay:
        digest.update(b'\0patch\0'+name.encode('utf-8')+b'\0')
        for offset,data in writes:
            digest.update(struct.pack('<QQ',offset,len(data))+data)
    return digest.hexdigest(),[name for name,_ in gameplay]


def write_party_patch_file(out, digest, names, cosmetic=()):
    lines=['# Party version check (scripts/patches.py): the gameplay patches of patches.bin. Players',
           '# with another hash are refused by the party host; cosmetic patches do not count.',
           f'hash {digest}']
    lines+=[f'patch {n}' for n in names]
    lines+=[f'# cosmetic {n}' for n in cosmetic]
    (Path(out)/PARTY_PATCH_FILE).write_text('\n'.join(lines)+'\n',encoding='utf-8')


# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
# Above 60 FPS the sprint fix always goes with the frame rate patch: without it sprinting drops to
# half speed (the game's wall detector measured distance per frame, a 30 FPS rule).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++','Sprint Fix (High FPS)'],
             'uncap':['Uncap FPS++','Sprint Fix (High FPS)']}
SCALED_FPS={'120':120,'144':144}
SCALED_BASE='90 FPS++'


def scaled_rate_values(fps):
    """Patch address -> bytes: the 90 FPS++ values that depend on the frame rate, for `fps`."""
    f=lambda *v: b''.join(struct.pack('<f',x) for x in v)
    frame=bytes.fromhex('41c7442418')+f(1/fps)
    cap=struct.pack('<I',{90:10000,120:8000,144:6500}[fps])
    return {
        0x0F383CA: f(1/fps),
        0x1CBDB42: bytes.fromhex('c7857cffffff')+f(0.0125*90/fps)+bytes.fromhex('909090909090'),
        0x1929676: struct.pack('<i',fps//6),
        0x192C405: struct.pack('<i',fps//3),
        0x2434840: frame, 0x243485A: frame, 0x243487E: frame, 0x24348B3: frame,
        0x25629F0: b'\xbf'+struct.pack('<I',990000//fps),
        0x2C012CC: b'\x81\xff'+cap+b'\x7e\x05\xbf'+cap+bytes.fromhex('e98aeb3b00'),
        0x4CF6420: f(*[3/fps]*4),
        0x4D27FD4: f(-3/fps), 0x4D27FE0: f(3/fps),
        0x4D29170: f(1/fps),
    }


def rate_scaled(writes, fps):
    """The 90 FPS++ writes with their frame-rate values for `fps`."""
    values=scaled_rate_values(fps)
    out,seen=[],set()
    for offset,data in writes:
        value=values.get(offset+EBOOT_BASE)
        if value is not None:
            if len(value)!=len(data): raise ValueError(f'{SCALED_BASE}: {offset+EBOOT_BASE:#x} changed size')
            data=value
            seen.add(offset+EBOOT_BASE)
        out.append((offset,data))
    missing=set(values)-seen
    if missing: raise ValueError(f'{SCALED_BASE}: no write at {", ".join(hex(a) for a in sorted(missing))}')
    return out



EFFECT_DATA_RATE=(0x26FB39F,0x4CAD7FC)
CAMERA_DEFAULTS={0x183a4c3:0x19c,0x183a4cd:0x1a0,0x183a4d7:0x1a4,0x183a4e1:0x1a8,0x183a4f5:0x1b0,
                 0x183a4ff:0x1b4,0x183a509:0x1b8,0x183a527:0x1c4,0x183a531:0x1c8,0x183a53b:0x1cc,
                 0x183a649:0x238,0x183a653:0x23c,0x183a65d:0x240,0x183a8c1:0x19c,0x183a8cb:0x1a0,
                 0x183a8d5:0x1a4,0x183a8df:0x1a8,0x183a8f3:0x1b0,0x183a8fd:0x1b4,0x183a907:0x1b8,
                 0x183a925:0x1c4,0x183a92f:0x1c8,0x183a939:0x1cc,0x183aa47:0x238,0x183aa51:0x23c,
                 0x183aa5b:0x240}
CAMERA_PARAMETER_BLOCKS=((0x183FF29,0x183FF70),(0x1840799,0x18407EB))
CAMERA_FIXED_STORE=bytes.fromhex('41c785')
CAMERA_PITCH_ANGLE,CAMERA_PITCH_KEPT=0x238,0x1a8
CAMERA_GAME_EASES={0x4D25E68:0.1,0x4D25E94:1/30}
CAMERA_LOCK_BLEND=(0x183A7DE,bytes.fromhex('cdcc4c3d'))
STATUS_STEP_FIRST=0x1CBC4FB
STATUS_STEP_SECOND=(0x1CBC511,bytes.fromhex('37b70603'),bytes.fromhex('f3e3a903'))


def per_frame(weight, frames, field=None):
    """A weight tuned for one frame, for a frame `frames` times as long: the share moved, 1-(1-w)^n,
    or for the pitch return the share kept, w^n. The pitch angle and values outside (0, 1) stay."""
    if field==CAMERA_PITCH_ANGLE or not 0<weight<1: return weight
    if field==CAMERA_PITCH_KEPT: return weight**frames
    return 1-(1-weight)**frames


def follow_camera_write(at, data, fps):
    """A list's camera write with its weights converted to a frame of 1/fps: the 60 list's, the
    game's own from 30."""
    if at in CAMERA_DEFAULTS:
        return struct.pack('<f',per_frame(struct.unpack('<f',data)[0],60/fps,CAMERA_DEFAULTS[at]))
    if len(data)==11 and data[:3]==CAMERA_FIXED_STORE and any(lo<=at<hi for lo,hi in CAMERA_PARAMETER_BLOCKS):
        field,weight=struct.unpack('<If',data[3:])
        return data[:7]+struct.pack('<f',per_frame(weight,60/fps,field))
    if at in CAMERA_GAME_EASES:
        return struct.pack('<f',per_frame(CAMERA_GAME_EASES[at],30/fps))
    return data


def corrected_frame_rate(writes, fps, elf=None):
    """The frame-rate preset's writes with the corrections above, for `fps` (None: uncapped, whose
    rate is unknown here: only the effects' rate and the status effects are put right). `elf`, when
    given, must have the game's bytes where a correction adds a write."""
    camera=bool(fps) and os.environ.get('BB_CAMERA_FIX','0')=='1'
    out=[]
    for offset,data in writes:
        at=offset+EBOOT_BASE
        if at in EFFECT_DATA_RATE: continue
        if camera: data=follow_camera_write(at,data,fps)
        out.append((offset,data))
    unchanged=lambda at,was: elf is None or original_bytes(elf,at-EBOOT_BASE,len(was))==was
    at,was,now=STATUS_STEP_SECOND
    if any(offset+EBOOT_BASE==STATUS_STEP_FIRST for offset,_ in out) and unchanged(at,was):
        out.append((at-EBOOT_BASE,now))
    at,was=CAMERA_LOCK_BLEND
    if camera and unchanged(at,was):
        out.append((at-EBOOT_BASE,struct.pack('<f',per_frame(struct.unpack('<f',was)[0],30/fps))))
    return out


OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
PRESET_CUSTOM=len(PRESET_SCALES)


def preset_scale(settings):
    preset=int(settings.get('preset','0') or 0)
    if preset==PRESET_CUSTOM:
        try:
            return min(3.0,max(1.0,float(settings.get('render_scale','1.3') or 1.3)))
        except ValueError:
            return 1.3
    return PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
}
INTEL_TONEMAP='Intel Black Tonemap Fix'
SFX_WORKAROUND='Intel 12th Gen+ SFX workaround'
HEAP_PATCH='Increased Graphics Heap Sizes'
RENDER_POOL_SITES=(0x2195c09,0x2195c46)
RENDER_POOL_CODE=bytes.fromhex('ba00000400')
RENDER_POOL_SIZE=0x100000
GX_REGION_SITE=0x1d9933d
GX_REGION_CODE=bytes.fromhex('c78388000000')+struct.pack('<I',0x8000000)
GX_REGION_GAME,GX_REGION_DEFAULT,GX_REGION_MAX=160,288,416
GX_HEAP_SITE=0x4736dd8
GX_HEAP_PS4=0x8ef00000


def intel_cpu(cpuinfo='/proc/cpuinfo'):
    try:
        with open(cpuinfo) as f:
            return any(line.startswith('vendor_id') and 'GenuineIntel' in line for line in f)
    except OSError:
        return False


def intel_tonemap_fix(env=os.environ, cpuinfo='/proc/cpuinfo'):
    forced=env.get('BB_INTEL_TONEMAP_FIX')
    return forced=='1' if forced in ('0','1') else intel_cpu(cpuinfo)

# model_lod: -2 highest, 0 the game's, 1 lower. 2 ("Model LOD 2 (Lowest)") applies the Lower patch:
# with the lowest level the game draws nothing for a model that has no third level of detail, and
# some map floors have none (the Lecture Building's floor showed the background through it).
# -2 ("Model LOD -2 (Highest)") is an opt-in: its bias reaches one of the game's three model draw
# paths only, and many objects past their nearest level of detail are then not drawn at all
# (Central Yharnam's corpses popped in close and vanished further away). A -2 saved before
# settings_rev 2 applies nothing: the game turns it into 0 at its first start.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 1 (Lower)'}
# Every model LOD patch stays the engine's to set, so a patch file cannot turn -2 or 2 on.
MODEL_LOD_PATCHES=('Model LOD -2 (Highest)','Model LOD 1 (Lower)','Model LOD 2 (Lowest)')


def validate_patch_requirements(names, game):
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if 'Restore Debug Menu (READ NOTES)' in names:
        font = game / 'dvdroot_ps4/font'
        missing = [name for name in ('DbgFont14h.ccm', 'DbgFont14h.tpf')
                   if not (font / name).is_file() or (font / name).stat().st_size == 0]
        if missing:
            unpacked = (font / 'DbgFont14h-tpf-dcx').is_dir() or (font / 'DbgFont14h-tpf').is_dir()
            raise ValueError('Debug menu needs the font files '
                             f'{", ".join(missing)} in dvdroot_ps4/font, in the game folder or in an '
                             'enabled mod (Mods page).'
                             + (' The installed mod has the font unpacked (a DbgFont14h-tpf-dcx folder): '
                                'repack it with WitchyBND, or use the packed files' if unpacked else '')
                             + '. The fonts: https://www.nexusmods.com/bloodborne/mods/253; or turn the '
                             'debug menu off (debug_menu in bbport.ini)')


DEFAULT_OFF=('effect_chromatic_aberration','effect_dof','effect_motion_blur','effect_game_aa')
def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings:
            if key in DEFAULT_OFF and off: names.append(off)
            continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    value=settings.get('model_lod','0')
    rev=settings.get('settings_rev','0')
    lod=MODEL_LOD.get(value)
    if lod and (value!='-2' or (rev.isdigit() and int(rev)>=2)): names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text().splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    scale=preset_scale(settings)
    if scale==1.0: return None
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; "auto" is the display's own
    (BB_DISPLAY_SIZE, from run_windows.py); 1920x1080 by default."""
    value=settings.get('output_res','').lower()
    if value=='auto': value=os.environ.get('BB_DISPLAY_SIZE','')
    try:
        w,h=(int(v) for v in value.split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or settings.get('upscaler')=='taa': return None
    scale=1.0
    if settings.get('upscaler','fsr3')!='off':
        scale=preset_scale(settings)
    scale=max(1.0,min(scale,out[0]/640,out[1]/360))
    render=tuple(max(4,round(v/scale/4)*4) for v in out)
    if render==OUTPUT_SIZE: render=(1916,1076)
    return render,out


def ui_size(settings, out):
    """The game's UI movie viewport for an output of `out`: 1920x1080, or with Widescreen on
    (bbport.ini widescreen, on by default) a screen of another shape, that shape around it -
    2520x1080 at 21:9, 3840x1080 at 32:9, 1920x1200 at 16:10. Scaleform shows its 1920x1080
    stage whole and centred in it (the HUD stays 16:9 in the middle), and what the
    game places past the stage - enemy gauges, co-op names, the lock-on marker at the screen's
    sides (engine/widescreen.cpp) - is drawn there instead of being cut off at the 16:9 box."""
    w,h=out
    if settings.get('widescreen','1')=='0' or w*OUTPUT_SIZE[1]==h*OUTPUT_SIZE[0]:
        return OUTPUT_SIZE
    if w*OUTPUT_SIZE[1]>h*OUTPUT_SIZE[0]:
        return (min(8192,round(OUTPUT_SIZE[1]*w/h/2)*2),OUTPUT_SIZE[1])
    return (OUTPUT_SIZE[0],min(8192,round(OUTPUT_SIZE[0]*h/w/2)*2))


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def originals_match(xml, name, app_version, elf):
    """False (with a message) when a line of patch `name` carries Original="hex" (the 1.09 bytes
    the patch replaces) and the eboot holds something else there: the patch is then left out."""
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name')!=name or meta.get('AppVer')!=app_version: continue
        for line in meta.iter('Line'):
            original=line.get('Original')
            if not original: continue
            want=bytes.fromhex(original.replace(' ',''))
            have=original_bytes(elf,int(line.get('Address'),0)-EBOOT_BASE,len(want))
            if len(want)!=len(encode(line)) or have!=want:
                print(f'Patches: "{name}" left out: {line.get("Address")} holds '
                      f'{have.hex() if have else "nothing"}, not {want.hex()} (another eboot?)',file=sys.stderr)
                return False
    return True



def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


def site_table_path(xml):
    return Path(xml).with_name('Bloodborne-1.09-sites.json')


def original_bytes(elf, offset, length):
    """The executable's bytes at image offset `offset` (zero past a segment's file data)."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    for i in range(phnum):
        kind,_,file_offset,vaddr,_,filesz,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1 and vaddr<=offset and offset+length<=vaddr+memsz:
            start=offset-vaddr
            data=elf[file_offset+start:file_offset+min(filesz,start+length)] if start<filesz else b''
            return bytes(data)+bytes(length-len(data))
    return None


def site_crcs(elf, writes):
    return [zlib.crc32(original_bytes(elf,offset,len(data)) or b'') for offset,data in writes]


def differing_sites(elf, writes, expected):
    """Image offsets of the writes whose original bytes aren't the standard 1.09's."""
    if len(expected)!=len(writes): return [offset for offset,_ in writes]
    return [offset for (offset,_),now,crc in zip(writes,site_crcs(elf,writes),expected) if now!=crc]


def checked_patches(xml, names, app_version, segments, elf, group=()):
    """`names` without the patches whose sites differ from the standard 1.09; one of `group` (the
    FPS preset's patches, which go together) differing leaves out all of it."""
    try:
        table=json.loads(site_table_path(xml).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return names
    bad={}
    for name in names:
        if name in table:
            diff=differing_sites(elf,compile_patches(xml,[name],app_version,segments),table[name])
            if diff: bad[name]=diff
    if any(name in bad for name in group):
        for name in group:
            if name in names: bad.setdefault(name,[])
    for name,diff in bad.items():
        where=(f'at {len(diff)} of its {len(table[name])} spots (first: image offset {diff[0]:#x})' if diff
               else 'with the FPS patch above')
        print(f'Patches: "{name}" left out: this game executable is not the standard 1.09 {where}', flush=True)
    return [name for name in names if name not in bad]


def render_pool_writes(elf):
    """The writes that enlarge the render pools, when the executable has the standard code there."""
    if os.environ.get('BB_RENDER_POOLS')=='0':
        print('Patches: render pools at the game\'s 256 KiB (BB_RENDER_POOLS=0)',flush=True)
        return []
    if any(original_bytes(elf,site,len(RENDER_POOL_CODE))!=RENDER_POOL_CODE for site in RENDER_POOL_SITES):
        print('Patches: render pools left at the game\'s size: this executable differs there',flush=True)
        return []
    return [(site+1,struct.pack('<I',RENDER_POOL_SIZE)) for site in RENDER_POOL_SITES]


def gx_region_writes(elf, heaps_written, env=os.environ):
    """The writes that enlarge the GX range allocator (and, unless another write already sizes the
    graphics heaps, GFX_GraphicsPrivate by as much), when the executable has the standard bytes."""
    value=env.get('BB_GX_REGION_MB','').strip()
    mib=int(value) if value.isdigit() else GX_REGION_DEFAULT
    if mib<=GX_REGION_GAME:
        print(f'Patches: the GX range allocator at the game\'s {GX_REGION_GAME} MiB (BB_GX_REGION_MB)',flush=True)
        return []
    mib=min(mib,GX_REGION_MAX)
    if original_bytes(elf,GX_REGION_SITE,len(GX_REGION_CODE))!=GX_REGION_CODE or \
       (not heaps_written and original_bytes(elf,GX_HEAP_SITE,8)!=struct.pack('<Q',GX_HEAP_PS4)):
        print('Patches: the GX range allocator left at the game\'s size: this executable differs there',flush=True)
        return []
    out=[(GX_REGION_SITE+6,struct.pack('<I',(mib-GX_REGION_GAME+128)<<20))]
    if not heaps_written:
        out.append((GX_HEAP_SITE,struct.pack('<Q',GX_HEAP_PS4+((mib-GX_REGION_GAME)<<20))))
    print(f'Patches: the GX range allocator {mib} MiB (the game\'s {GX_REGION_GAME})'+
          ('' if heaps_written else f', the graphics heap {(GX_HEAP_PS4>>20)+mib-GX_REGION_GAME} MiB'),flush=True)
    return out



EDITION_CALL=(0x1fb6d20,bytes.fromhex('e8bb210000'),bytes.fromhex('b801000000'))


def old_hunters_writes(elf):
    """The edition write when the DLC switch is on and the executable has 1.09's call there."""
    if os.environ.get('BB_DLC')!='1':
        return []
    offset,call,edition_one=EDITION_CALL
    if original_bytes(elf,offset,len(call))!=call:
        print('Patches: The Old Hunters edition left alone: this executable differs there',flush=True)
        return []
    return [(offset,edition_one)]


def frame_time_writes(elf, segments, settings, names, env=os.environ):
    """The port's frame-time code (start/patches.cpp FrameTimeCode does the same): steady cloth time
    (cloth_time.py), cloth as on PS4 (cloth.py, only with it; bbport.ini steady_cloth), plant sway
    by the frame's time (foliage_time.py), the air drag of 30 FPS (air_drag.py). On by default;
    BB_CLOTH_TIME / BB_STEADY_CLOTH / BB_FOLIAGE_TIME / BB_AIR_DRAG = 0 leave one out."""
    sys.path.insert(0,str(Path(__file__).resolve().parent))
    import frame_time_code, quit_menu
    try:
        quit_menu.check_segments(segments)
    except ValueError as error:
        print(f'Patches: {error}; steady cloth, plant sway and jump drag left out')
        return []
    writes,cloth_time=[],False
    for name,module,switch,setting,needs,_ in frame_time_code.GROUPS:
        if env.get(switch)=='0' or (setting and settings.get(setting,'1')=='0'):
            print(f'Patches: {name} off ({setting or switch}=0)')
            continue
        if needs and not cloth_time:
            print(f'Patches: {name} left out: it steps with Steady cloth time, which is off')
            continue
        try:
            module.check_original(lambda start,end: original_bytes(elf,start,end-start))
        except ValueError as error:
            print(f'Patches: {error}; {name} left out')
            continue
        writes+=module.code_writes()
        names.append(name)
        cloth_time=cloth_time or switch=='BB_CLOTH_TIME'
    return writes


def write_site_table(xml, app_version, elf_path):
    """The site table from a verified 1.09 eboot.elf (every eboot patch of `app_version`)."""
    from engine_assets import EBOOT_SHA, sha
    elf=Path(elf_path).read_bytes()
    if sha(elf) not in EBOOT_SHA: raise ValueError(f'{elf_path} is not the verified 1.09 executable')
    segments=eboot_segments(elf)
    table={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        name=meta.get('Name')
        if meta.get('AppVer')!=app_version or meta.get('AppElf','eboot.bin')!='eboot.bin' or name in table: continue
        try:
            table[name]=site_crcs(elf,compile_patches(xml,[name],app_version,segments))
        except ValueError:
            continue
    site_table_path(xml).write_text(json.dumps(table,indent=0,sort_keys=True)+'\n',encoding='utf-8')
    return table


BLOODBORNE_IDS={'CUSA00207','CUSA00208','CUSA00299','CUSA00900','CUSA01363','CUSA03014',
                'CUSA03023','CUSA03173','CUSA03179'}


def same_app_version(value, app_version):
    """'1.09' is '01.09' (patch files write either)."""
    try:
        return value is not None and [int(p) for p in value.strip().split('.')]==[int(p) for p in app_version.split('.')]
    except ValueError:
        return False


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve() or path.name.lower()==Path(exclude).name.lower(): continue
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&BLOODBORNE_IDS: continue
        for meta in root.iter('Metadata'):
            if same_app_version(meta.get('AppVer'),app_version) and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def managed_builtin(name):
    return (name in {n for names in FPS_PRESETS.values() for n in names} or name in (SCALED_BASE, '30 FPS++',
            '60FPS (no deltatime)', INTEL_TONEMAP, SFX_WORKAROUND) or
            name in {n for pair in EFFECTS.values() for n in pair if n} or name in MODEL_LOD_PATCHES or
            name.startswith('Resolution Patch'))


def builtin_selection(xml, config):
    """The built-in patches switched on in the launcher's Mods page: patches.json keys
    "Bloodborne.xml/<name>" in "enabled" (built-in ones start off, whatever the file says)."""
    try:
        enabled=json.loads(Path(config).read_text()).get('enabled',[]) if config and Path(config).is_file() else []
    except (OSError, ValueError):
        return []
    prefix=Path(xml).name+'/'
    known={meta.get('Name') for meta in ET.parse(xml).getroot().iter('Metadata')}
    return [k[len(prefix):] for k in enabled if k.startswith(prefix) and k[len(prefix):] in known
            and not managed_builtin(k[len(prefix):])]


def placeable(xml, name, app_version, segments):
    """Whether a built-in patch switched on in the Mods page fits this eboot. One that doesn't
    (Performance Patch writes past its end: GitHub issues 28, 29) is left out, as an external
    patch would be, instead of stopping the launch."""
    try:
        compile_patches(xml,[name],app_version,segments)
        return True
    except ValueError as error:
        print(f'Patches: skipped {Path(xml).name}/{name}: {str(error).removeprefix(name+": ")}',file=sys.stderr)
        return False


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


REGION_FUNCTION=(0x1fb9190,0x1fb91e3)


def breaks_online(writes):
    """Whether online is on and these writes change the game's region, unless the player chose
    to keep such a patch (the launcher asks at Play: BB_REGION_PATCH_ONLINE=keep)."""
    start,end=REGION_FUNCTION
    return (os.environ.get('BB_ONLINE')=='1' and os.environ.get('BB_REGION_PATCH_ONLINE')!='keep' and
            any(offset<end and start<offset+len(data) for offset,data in writes))


def overlaps(writes, footprint):
    return any(offset+i in footprint for offset,data in writes for i in range(len(data)))


def resolution_footprint(xml, app_version, segments):
    """The bytes the game's resolution and UI layout live at: every community "Resolution Patch"
    writes them (RESOLUTION_TEMPLATE's sites)."""
    return {offset+i for offset,data in compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
            for i in range(len(data))}


def skip_for_online(key):
    print(f'Patches: skipped {key} while online is on: it moves the game to a region with no Bloodborne '
          'server (Play online would fail); it applies again with online off',file=sys.stderr)


def compile_external(selected, segments, resolution=None, applied=None):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole.
    `resolution`: (footprint, output size) while the output resolution sets the game's resolution
    and UI itself: a patch that writes them too is left out, unless the player kept it (the
    launcher asks at Play: BB_RESOLUTION_PATCH=keep). Applied after ours, it won: the picture
    kept the patch's shape with black bars, and the widescreen hooks found its bytes and refused."""
    writes=[]
    for key,_,meta in selected:
        if party_mode() and meta.get('Name') == DISABLE_HTTP:
            print(f'Patches: {key} disabled in party mode',file=sys.stderr)
            continue
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        if breaks_online(ours):
            skip_for_online(key)
            continue
        if resolution and os.environ.get('BB_RESOLUTION_PATCH')!='keep' and overlaps(ours,resolution[0]):
            w,h=resolution[1]
            print(f'Patches: skipped {key}: it sets the game\'s resolution and UI layout, which the output '
                  f'resolution ({w}x{h}) already does; it applies again with a 1920x1080 output',file=sys.stderr)
            continue
        writes+=ours
        if applied is not None: applied.append((key,meta,ours))
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS)+sorted(SCALED_FPS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--output-res',default='',help='output resolution WxH (the upscaler\'s; the UI stays 1920x1080)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    p.add_argument('--write-site-table',type=Path,metavar='EBOOT_ELF',
                   help='write the standard 1.09 site table next to the XML from a verified eboot.elf')
    a=p.parse_args()
    if a.write_site_table:
        print(f'{len(write_site_table(a.xml,a.app_version,a.write_site_table))} patches in {site_table_path(a.xml)}')
        return
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
        return
    if a.print_preset_size:
        settings=read_settings(a.settings)
        if 'BB_UPSCALER' in os.environ:
            settings['upscaler']='fsr3' if os.environ['BB_UPSCALER']=='fsr3' else 'off'
        if 'BB_UPSCALE_PRESET' in os.environ:
            settings['preset']=os.environ['BB_UPSCALE_PRESET']
        size=render_size(settings)
        if size: print(f'{size[0]}x{size[1]}')
        return
    preset=FPS_PRESETS[a.fps] if a.fps in FPS_PRESETS else [SCALED_BASE,'Sprint Fix (High FPS)']
    if os.environ.get('LANTERN_FRAMERATE_FROM_ENGINE')=='1':
        print(f"Patches: {a.fps} FPS from the engine's native frame rate (no frame-rate patches)")
        preset=[]
    names=preset+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    selected=[n for n in builtin_selection(a.xml,a.patches_config) if n not in names]
    names+=selected
    if intel_tonemap_fix() and INTEL_TONEMAP not in names:
        names.append(INTEL_TONEMAP)
    if party_mode():
        names=[n for n in names if n not in (DISABLE_HTTP,SKIP_NETWORK_CHOICE)]
        names += [n for n in party_patches() + [SKIP_NETWORK_CHOICE_ONLINE] if n not in names]
    if os.environ.get('BB_SFX_WORKAROUND')=='1' and SFX_WORKAROUND not in names:
        names.append(SFX_WORKAROUND)
    validate_patch_requirements(names,a.game_dir)
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    names=[n for n in names if n not in selected or placeable(a.xml,n,a.app_version,segments)]
    names=checked_patches(a.xml,names,a.app_version,segments,elf,preset)
    for name in [n for n in names if breaks_online(compile_patches(a.xml,[n],a.app_version,segments))]:
        skip_for_online(name)
        names.remove(name)
    names=[n for n in names if originals_match(a.xml,n,a.app_version,elf)]
    writes=compile_patches(a.xml,names,a.app_version,segments)
    metas={m.get('Name'):m for m in ET.parse(a.xml).getroot().iter('Metadata') if m.get('AppVer')==a.app_version}
    party_items=[(n,metas.get(n),compile_patches(a.xml,[n],a.app_version,segments)) for n in names]
    if a.fps in SCALED_FPS and SCALED_BASE in names:
        writes=rate_scaled(writes,SCALED_FPS[a.fps])
        print(f'Patches: {a.fps} FPS: {SCALED_BASE} with its frame-rate values for {a.fps}')
    if preset and preset[0] in names:
        writes=corrected_frame_rate(writes,{'60':60,'90':90,**SCALED_FPS}.get(a.fps),elf)
    writes+=old_hunters_writes(elf)
    pools=render_pool_writes(elf)
    if pools:
        writes+=pools
        print(f'Patches: render pools {RENDER_POOL_SIZE>>10} KiB each (the game\'s 256)')
    writes+=frame_time_writes(elf,segments,read_settings(a.settings),names)
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    out=output_size(read_settings(a.settings))
    if a.output_res:
        out=tuple(int(v) for v in a.output_res.lower().split('x'))
    ui=ui_size(read_settings(a.settings),out) if size else OUTPUT_SIZE
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1] and HEAP_PATCH not in names:
            heap=HEAP_PATCH
            writes+=compile_patches(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if a.patches_dir:
        resolution=(resolution_footprint(a.xml,a.app_version,segments),out) if out!=OUTPUT_SIZE else None
        applied=[]
        writes+=compile_external(external_selection(external_patches(a.patches_dir,a.app_version,a.xml),
                                                    a.patches_config),segments,resolution,applied)
        party_items += [(meta.get('Name')+' (external)',meta,ours) for _,meta,ours in applied]
    # Whether a patch enlarges the game's heaps (the built-in one, or a resolution patch above
    # 1080p from a patches/ file): run_windows.py then gives the game the larger memory pool.
    heap_bytes={offset+i for offset,data in compile_patches(a.xml,[HEAP_PATCH],a.app_version,segments)
                for i in range(len(data))}
    heaps=any(offset+i in heap_bytes for offset,data in writes for i in range(len(data)))
    writes+=gx_region_writes(elf,any(offset<GX_HEAP_SITE+16 and offset+len(data)>GX_HEAP_SITE
                                     for offset,data in writes))
    (a.out/'patches-info.json').write_text(json.dumps({'heap_patch':heaps,'ui_size':f'{ui[0]}x{ui[1]}'})+'\n',encoding='utf-8')
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    digest,gameplay=party_patch_set(party_items)
    write_party_patch_file(a.out,digest,gameplay,sorted(n for n,m,_ in party_items if patch_is_cosmetic(n,m)))
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()
