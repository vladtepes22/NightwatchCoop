import pipeline_context as ctx
"""Convert embedded DefineFont3 outlines to a local map-only TrueType subset.

Reads the user's font.gfx; writes only isolated map assets. Adobe's SWF spec
defines the glyph outlines at 20*1024 units. No system fonts are installed.
"""
from pathlib import Path
import struct,sys,json,hashlib,zlib
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

class Bits:
    def __init__(self,b):self.b=b;self.p=0
    def u(self,n):
        v=0
        for _ in range(n):
            if self.p>=len(self.b)*8:raise ValueError('Truncated glyph')
            v=(v<<1)|((self.b[self.p//8]>>(7-self.p%8))&1);self.p+=1
        return v
    def s(self,n):
        v=self.u(n);return v-(1<<n) if n and v&(1<<(n-1)) else v

def glyph(b):
    bits=Bits(b);fills=bits.u(4);lines=bits.u(4);pen=TTGlyphPen(None)
    x=y=0;started=False
    def pt(x,y):return round(x/10),round(-y/10)
    while True:
        if not bits.u(1):
            flags=bits.u(5)
            if not flags:break
            if flags&1:
                if started:pen.closePath()
                n=bits.u(5);x=bits.s(n);y=bits.s(n);pen.moveTo(pt(x,y));started=True
            if flags&2:bits.u(fills)
            if flags&4:bits.u(fills)
            if flags&8:bits.u(lines)
            if flags&16:raise ValueError('Unexpected font shape style table')
        else:
            straight=bits.u(1);n=bits.u(4)+2
            if not started:pen.moveTo(pt(x,y));started=True
            if straight:
                if bits.u(1):x+=bits.s(n);y+=bits.s(n)
                elif bits.u(1):y+=bits.s(n)
                else:x+=bits.s(n)
                pen.lineTo(pt(x,y))
            else:
                cx=x+bits.s(n);cy=y+bits.s(n);x=cx+bits.s(n);y=cy+bits.s(n)
                pen.qCurveTo(pt(cx,cy),pt(x,y))
    if started:pen.closePath()
    return pen.glyph()

def font_tag(path):
    b=path.read_bytes()
    if b[:3] in (b'CWS',b'CFX'):b=b[:8]+zlib.decompress(b[8:])
    assert b[:3] in (b'GFX',b'FWS')
    n=b[8]>>3;o=8+(5+4*n+7)//8+4
    while o+2<=len(b):
        h=struct.unpack_from('<H',b,o)[0];o+=2;tag,length=h>>6,h&63
        if length==63:length=struct.unpack_from('<I',b,o)[0];o+=4
        data=b[o:o+length];o+=length
        if tag==75:return data
    raise ValueError('DefineFont3 absent')

def extract(source,out):
    b=font_tag(source);flags=b[2];n=b[4];name=b[5:5+n].rstrip(b'\0').decode('utf-8')
    count=struct.unpack_from('<H',b,5+n)[0];base=7+n;wide=bool(flags&8);size=4 if wide else 2;code='I' if wide else 'H'
    offsets=struct.unpack_from('<'+code*(count+1),b,base)
    codebase=base+offsets[-1];codes=struct.unpack_from('<'+('H' if flags&4 else 'B')*count,b,codebase)
    metricsbase=codebase+count*(2 if flags&4 else 1)
    assert flags&128
    asc,desc,leading=struct.unpack_from('<3h',b,metricsbase)
    advances=struct.unpack_from('<'+'h'*count,b,metricsbase+6)
    selected=[i for i,c in enumerate(codes) if 32<=c<=0x24f or 0x400<=c<=0x52f or 0x2000<=c<=0x206f]
    order=['.notdef'];cmap={};glyfs={'.notdef':TTGlyphPen(None).glyph()};hmetrics={'.notdef':(1024,0)}
    for i in selected:
        cp=codes[i];gn='uni%04X'%cp
        if gn in glyfs:continue
        g=glyph(b[base+offsets[i]:base+offsets[i+1]])
        order.append(gn);cmap[cp]=gn;glyfs[gn]=g
        hmetrics[gn]=(max(1,round(advances[i]/10)),0)
    fb=FontBuilder(2048,isTTF=True);fb.setupGlyphOrder(order);fb.setupCharacterMap(cmap);fb.setupGlyf(glyfs)
    fb.setupHorizontalMetrics(hmetrics)
    fb.setupHorizontalHeader(ascent=round(asc/10),descent=-abs(round(desc/10)),lineGap=max(0,round(leading/10)))
    fb.setupNameTable(dict(familyName=name,styleName='Embedded',uniqueFontIdentifier='Local Bloodborne map subset',fullName=name+' (local map subset)',psName='BloodborneMapNativeSubset'))
    fb.setupOS2(sTypoAscender=round(asc/10),sTypoDescender=-abs(round(desc/10)),usWinAscent=round(asc/10),usWinDescent=abs(round(desc/10)))
    fb.setupPost();fb.setupMaxp();fb.font['head'].created=fb.font['head'].modified=3406620153;out.parent.mkdir(parents=True,exist_ok=True);fb.save(out)
    assert all(ord(c) in cmap for c in 'Central YharnamGreat BridgeNear Tomb of Oedon')
    result=dict(font=name,glyphs=len(cmap),source=source.relative_to(ctx.GAME).as_posix(),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),output='art/native-font.ttf',ascent=asc,descent=desc,leading=leading)
    out.with_suffix('.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,ensure_ascii=False))
