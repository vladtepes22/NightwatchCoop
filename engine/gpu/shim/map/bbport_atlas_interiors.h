// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_atlas_style.h"
#include <numeric>

// Enclosed gaps in the verified floor union, not inferred building solids.
// Ring winding is immaterial. Open, crossing or self-touching topology fails
// closed (no decoration). Every mapped floor on the same layer is excluded; other levels are context.
namespace BbAtlasInteriors {
using BbAtlasStyle::Point;
using BbAtlasStyle::Triangle;
using BbAtlasStyle::Line;
using Ring=std::vector<Point>;
struct Bounds {float x0,z0,x1,z1;};
inline Bounds Box(const Ring& r){Bounds b{r[0].x,r[0].z,r[0].x,r[0].z};for(auto p:r){b.x0=std::min(b.x0,p.x);b.z0=std::min(b.z0,p.z);b.x1=std::max(b.x1,p.x);b.z1=std::max(b.z1,p.z);}return b;}
inline bool Overlap(Bounds a,Bounds b){return a.x0<=b.x1&&b.x0<=a.x1&&a.z0<=b.z1&&b.z0<=a.z1;}
inline double Turn(Point a,Point b,Point c){return (double(b.x)-a.x)*(double(c.z)-a.z)-(double(b.z)-a.z)*(double(c.x)-a.x);}
inline bool Same(Point a,Point b){return a.x==b.x&&a.z==b.z;}
inline double Area(const Ring& r){double v=0;for(size_t i=1;i+1<r.size();++i)v+=Turn(r[0],r[i],r[i+1]);return v*.5;}
inline bool On(Point a,Point b,Point p){return std::abs(Turn(a,b,p))<1.e-8&&p.x>=std::min(a.x,b.x)&&p.x<=std::max(a.x,b.x)&&p.z>=std::min(a.z,b.z)&&p.z<=std::max(a.z,b.z);}
inline bool Crosses(Point a,Point b,Point c,Point d){
    if(std::max(a.x,b.x)<std::min(c.x,d.x)||std::max(c.x,d.x)<std::min(a.x,b.x)||std::max(a.z,b.z)<std::min(c.z,d.z)||std::max(c.z,d.z)<std::min(a.z,b.z))return false;
    const double u=Turn(a,b,c),v=Turn(a,b,d),s=Turn(c,d,a),t=Turn(c,d,b);
    return (((u>0&&v<0)||(u<0&&v>0))&&((s>0&&t<0)||(s<0&&t>0)))||On(a,b,c)||On(a,b,d)||On(c,d,a)||On(c,d,b);
}
inline bool Inside(const Ring& r,Point p){bool in=false;for(size_t i=0,j=r.size()-1;i<r.size();j=i++){auto a=r[i],b=r[j];if((a.z>p.z)!=(b.z>p.z)&&double(p.x)<(double(b.x)-a.x)*(double(p.z)-a.z)/(double(b.z)-a.z)+a.x)in=!in;}return in;}
inline bool Prepare(Ring& r){
    if(r.size()<4||r.size()>4096||!Same(r.front(),r.back()))return false;
    r.pop_back();for(auto p:r)if(!std::isfinite(p.x)||!std::isfinite(p.z))return false;
    for(size_t i=0;i<r.size()&&r.size()>=3;){size_t prev=(i+r.size()-1)%r.size(),next=(i+1)%r.size();if(Same(r[i],r[next])||On(r[prev],r[next],r[i])){r.erase(r.begin()+i);if(i)--i;}else ++i;}
    if(r.size()<3||std::abs(Area(r))<1.e-5)return false;
    for(size_t i=0;i<r.size();++i)for(size_t j=i+1;j<r.size();++j){if(j==i+1||(i==0&&j+1==r.size()))continue;if(Crosses(r[i],r[(i+1)%r.size()],r[j],r[(j+1)%r.size()]))return false;}
    return true;
}
inline bool Triangulate(Ring r,std::vector<Triangle>& result){
    const size_t start=result.size();if(Area(r)<0)std::reverse(r.begin(),r.end());
    while(r.size()>3){bool found=false;for(size_t i=0;i<r.size();++i){const size_t prev=(i+r.size()-1)%r.size(),next=(i+1)%r.size();const auto a=r[prev],b=r[i],c=r[next];if(Turn(a,b,c)<=1.e-9)continue;bool occupied=false;
        for(size_t k=0;k<r.size();++k)if(k!=prev&&k!=i&&k!=next&&Turn(a,b,r[k])>=-1.e-9&&Turn(b,c,r[k])>=-1.e-9&&Turn(c,a,r[k])>=-1.e-9){occupied=true;break;}
        if(occupied)continue;result.push_back({{a,b,c}});r.erase(r.begin()+i);found=true;break;
    }if(!found){result.resize(start);return false;}}
    if(r.size()==3)result.push_back({{r[0],r[1],r[2]}});return true;
}
// Half-plane clipping in double arithmetic. A 2cm clearance makes float output
// conservative even at the large world coordinates in the shipped maps.
inline Ring Half(const Ring& polygon,Point a,Point b,bool inside,double clearance){
    Ring out;if(polygon.empty())return out;const double length=std::hypot(double(b.x)-a.x,double(b.z)-a.z);
    auto side=[&](Point p){const double v=Turn(a,b,p)+clearance*length;return inside?v:-v;};
    auto prev=polygon.back();double pv=side(prev);for(auto cur:polygon){double cv=side(cur);if((pv>=0)!=(cv>=0)){const double t=pv/(pv-cv);out.push_back({float(prev.x+(double(cur.x)-prev.x)*t),float(prev.z+(double(cur.z)-prev.z)*t)});}if(cv>=0)out.push_back(cur);prev=cur;pv=cv;}return out;
}
inline std::vector<Ring> Subtract(const Ring& polygon,Triangle floor){
    if(Turn(floor.p[0],floor.p[1],floor.p[2])<0)std::swap(floor.p[1],floor.p[2]);
    if(std::abs(Turn(floor.p[0],floor.p[1],floor.p[2]))<1.e-10)return {polygon};
    Ring remainder=polygon;std::vector<Ring> out;
    for(size_t i=0;i<3&&!remainder.empty();++i){auto fragment=Half(remainder,floor.p[i],floor.p[(i+1)%3],false,.02);if(fragment.size()>=3&&std::abs(Area(fragment))>1.e-7)out.push_back(std::move(fragment));remainder=Half(remainder,floor.p[i],floor.p[(i+1)%3],true,.02);}return out;
}
struct Cache {
    bool ready=false;
    size_t holes=0,rejected=0,budget_skips=0;
    std::vector<std::vector<Triangle>> layers;
    struct Hatch {bool ready=false;std::vector<Line> lines;};
    std::vector<std::array<Hatch,BbAtlasStyle::CutCache::lod_count>> hatches;
    static constexpr size_t max_fragments=4096,max_layer_triangles=60000;
    void Build(const std::vector<std::vector<Ring>>& outlines,std::vector<BbAtlasStyle::FloorIndex>& layer_floors){
        layers.clear();layers.resize(outlines.size());hatches.clear();hatches.resize(outlines.size());holes=rejected=budget_skips=0;
        for(size_t li=0;li<outlines.size();++li){auto& floors=layer_floors.at(li);auto rings=outlines[li];std::vector<bool> valid(rings.size());std::vector<Bounds> bounds;bounds.reserve(rings.size());
            for(size_t i=0;i<rings.size();++i){valid[i]=Prepare(rings[i]);bounds.push_back(rings[i].empty()?Bounds{}:Box(rings[i]));if(!valid[i])++rejected;}
            // Rounding to millimetres can create touching edges. Do not guess
            // the topology in those cases; invalidate both affected rings.
            for(size_t i=0;i<rings.size();++i)for(size_t j=i+1;j<rings.size();++j)if(Overlap(bounds[i],bounds[j])){
                if(!valid[i]||!valid[j])continue;bool crossing=false;
                for(size_t u=0;u<rings[i].size()&&!crossing;++u)for(size_t v=0;v<rings[j].size();++v)if(Crosses(rings[i][u],rings[i][(u+1)%rings[i].size()],rings[j][v],rings[j][(v+1)%rings[j].size()])){crossing=true;break;}
                if(crossing){valid[i]=valid[j]=false;rejected+=2;}
            }
            for(size_t i=0;i<rings.size();++i){if(!valid[i])continue;size_t depth=0;bool uncertain=false;
                for(size_t j=0;j<rings.size();++j)if(i!=j&&Overlap(bounds[i],bounds[j])){if(!valid[j]){uncertain=true;break;}if(Inside(rings[j],rings[i][0]))++depth;}
                if(uncertain||!(depth&1))continue;
                std::vector<Triangle> seeds;if(!Triangulate(rings[i],seeds)){++rejected;continue;}++holes;
                std::vector<Triangle> filled;bool exhausted=false;
                for(const auto& seed:seeds){std::vector<Ring> fragments={Ring(seed.p.begin(),seed.p.end())};const auto b=Box(fragments[0]);size_t operations=0;
                    floors.Query({b.x0-.03f,b.z0-.03f},{b.x1+.03f,b.z1+.03f},[&](const Triangle& floor){if(exhausted||fragments.empty())return;const auto fb=Box(Ring(floor.p.begin(),floor.p.end()));if(!Overlap({b.x0-.03f,b.z0-.03f,b.x1+.03f,b.z1+.03f},fb))return;std::vector<Ring> next;
                        for(const auto& polygon:fragments){if(++operations>100000){exhausted=true;return;}auto pieces=Subtract(polygon,floor);for(auto& p:pieces)next.push_back(std::move(p));if(next.size()>max_fragments){exhausted=true;return;}}fragments=std::move(next);
                    });if(exhausted)break;
                    for(const auto& polygon:fragments)for(size_t k=1;k+1<polygon.size();++k)if(std::abs(Turn(polygon[0],polygon[k],polygon[k+1]))>1.e-6)filled.push_back({{polygon[0],polygon[k],polygon[k+1]}});
                    if(filled.size()+layers[li].size()>max_layer_triangles){exhausted=true;break;}
                }
                if(exhausted){++budget_skips;continue;}layers[li].insert(layers[li].end(),filled.begin(),filled.end());
            }
        }ready=true;
    }
    const std::vector<Line>& Get(size_t layer,int level){
        auto& cached=hatches[layer][std::clamp(level,0,BbAtlasStyle::CutCache::lod_count-1)];if(cached.ready)return cached.lines;cached.ready=true;
        const float spacing=BbAtlasStyle::CutCache::Spacing(level)*1.5f;
        for(size_t i=0;i<layers[layer].size();++i){const auto& t=layers[layer][i];float low=t.p[0].x-t.p[0].z,high=low;for(auto p:t.p){low=std::min(low,p.x-p.z);high=std::max(high,p.x-p.z);}const auto b=Box(Ring(t.p.begin(),t.p.end()));
            for(int n=int(std::ceil(low/spacing));n<=int(std::floor(high/spacing));++n){const float phase=n*spacing;Point a{b.x0-1,b.x0-1-phase},end{b.x1+1,b.x1+1-phase};float lo,hi;if(BbAtlasStyle::Clip(t.p,a,end,lo,hi))cached.lines.push_back({BbAtlasStyle::Mix(a,end,lo),BbAtlasStyle::Mix(a,end,hi),BbAtlasStyle::Hash(uint32_t(n))});if(cached.lines.size()>150000){cached.lines.clear();return cached.lines;}}
        }return cached.lines;
    }
};
} // namespace BbAtlasInteriors
