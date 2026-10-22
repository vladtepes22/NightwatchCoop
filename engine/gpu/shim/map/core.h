// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "texture_budget.h"

namespace Atlas2 {
using Json=nlohmann::json;
namespace fs=std::filesystem;
constexpr int Tile=256,Gutter=2,Dimension=Tile+Gutter*2;
constexpr size_t TileBytes=Dimension*Dimension*4;
inline std::vector<unsigned char> Read(const fs::path& p,size_t limit=64*1024*1024){
    auto size=fs::file_size(p);if(size>limit)throw std::runtime_error("asset exceeds bound");
    std::vector<unsigned char>b(size);std::ifstream f(p,std::ios::binary);f.read((char*)b.data(),b.size());
    if(!f)throw std::runtime_error("asset read failed");return b;
}
inline std::string Hash(const std::vector<unsigned char>& b){unsigned char h[32];SHA256(b.data(),b.size(),h);std::ostringstream s;for(auto v:h)s<<std::hex<<std::setw(2)<<std::setfill('0')<<int(v);return s.str();}
inline Json ReadJson(const fs::path& p,size_t limit=64*1024*1024){auto b=Read(p,limit);return Json::parse(b);}
inline void WriteJson(const fs::path& p,const Json& j){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f<<j.dump();if(!f)throw std::runtime_error("metadata write failed");}
inline void Write(const fs::path& p,const std::vector<unsigned char>& b){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f.write((const char*)b.data(),b.size());if(!f)throw std::runtime_error("asset write failed");}
inline void U32(std::vector<unsigned char>& b,uint32_t x){for(int i=0;i<4;++i)b.push_back((x>>(8*i))&255);}
inline uint32_t U32(const unsigned char* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline std::vector<unsigned char> Encode(const std::vector<unsigned char>& pixels){
    if(pixels.size()!=TileBytes)throw std::runtime_error("invalid tile bytes");
    std::vector<unsigned char>b={'A','2','T','1'};U32(b,Dimension);U32(b,Dimension);uLongf n=compressBound(pixels.size());b.resize(12+n);
    if(compress2(b.data()+12,&n,pixels.data(),pixels.size(),6)!=Z_OK)throw std::runtime_error("tile encode");b.resize(12+n);return b;
}
inline std::vector<unsigned char> Decode(const std::vector<unsigned char>& b,const std::string& expected){
    if(b.size()<12||b.size()>TileBytes+1024||Hash(b)!=expected||std::string((char*)b.data(),4)!="A2T1"||U32(b.data()+4)!=Dimension||U32(b.data()+8)!=Dimension)throw std::runtime_error("tile integrity mismatch");
    std::vector<unsigned char>p(TileBytes);uLongf n=p.size();uLong source=b.size()-12;
    if(uncompress2(p.data(),&n,b.data()+12,&source)!=Z_OK||n!=TileBytes||source!=b.size()-12)throw std::runtime_error("tile decode mismatch");return p;
}
struct Transform {
    double x0=0,z1=0,ppu=8;int width=0,height=0;
    std::array<double,2> Image(double x,double z)const{return {(x-x0)*ppu,(z1-z)*ppu};}
    std::array<double,2> World(double u,double v)const{return {x0+u/ppu,z1-v/ppu};}
    void Validate()const{if(!std::isfinite(x0)||!std::isfinite(z1)||!std::isfinite(ppu)||ppu<=0||width<=0||height<=0||width>32768||height>32768)throw std::runtime_error("invalid world transform");}
};
inline int Lod(double zoom,double ppu,int levels){if(!std::isfinite(zoom)||zoom<=0||levels<1)throw std::runtime_error("invalid zoom");return std::clamp(int(std::floor(std::log2(ppu/zoom))),0,levels-1);}
struct Surface {std::array<float,6> v;std::array<float,3> y;int layer=0;};
inline bool Height(const Surface& t,float x,float z,float& y){
    if(x<std::min({t.v[0],t.v[2],t.v[4]})||x>std::max({t.v[0],t.v[2],t.v[4]})||z<std::min({t.v[1],t.v[3],t.v[5]})||z>std::max({t.v[1],t.v[3],t.v[5]}))return false;
    auto cross=[&](int a,int b){return(t.v[b]-t.v[a])*(z-t.v[a+1])-(t.v[b+1]-t.v[a+1])*(x-t.v[a]);};
    float a=cross(0,2),b=cross(2,4),c=cross(4,0);
    if(!((a>=-.0001f&&b>=-.0001f&&c>=-.0001f)||(a<=.0001f&&b<=.0001f&&c<=.0001f)))return false;
    float d=(t.v[3]-t.v[5])*(t.v[0]-t.v[4])+(t.v[4]-t.v[2])*(t.v[1]-t.v[5]);if(std::abs(d)<1e-8f)return false;
    a=((t.v[3]-t.v[5])*(x-t.v[4])+(t.v[4]-t.v[2])*(z-t.v[5]))/d;b=((t.v[5]-t.v[1])*(x-t.v[4])+(t.v[0]-t.v[4])*(z-t.v[5]))/d;
    y=a*t.y[0]+b*t.y[1]+(1-a-b)*t.y[2];return std::isfinite(y);
}
struct Grid {
    double cell=8;std::vector<Surface> surfaces;std::map<std::pair<int,int>,std::vector<int>> cells;std::vector<float> anchors;
    size_t last_candidates=0,max_candidates=0;
    void Build(){cells.clear();for(size_t i=0;i<surfaces.size();++i){const auto& v=surfaces[i].v;
        int x0=int(std::floor(std::min({v[0],v[2],v[4]})/cell)),x1=int(std::floor(std::max({v[0],v[2],v[4]})/cell));
        int z0=int(std::floor(std::min({v[1],v[3],v[5]})/cell)),z1=int(std::floor(std::max({v[1],v[3],v[5]})/cell));
        if(int64_t(x1-x0+1)*(z1-z0+1)>100000)throw std::runtime_error("surface grid bounds");
        for(int x=x0;x<=x1;++x)for(int z=z0;z<=z1;++z)cells[{x,z}].push_back(int(i));
    }for(const auto& [k,v]:cells)max_candidates=std::max(max_candidates,v.size());if(max_candidates>8192)throw std::runtime_error("grid cell candidate bound");}
    int Select(float x,float y,float z,int previous=-1,float hysteresis=0,int manual=-1){
        last_candidates=0;if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||std::abs(x)>1e7||std::abs(z)>1e7)return -1;
        if(manual>=0&&manual<int(anchors.size()))return manual;
        float best=std::numeric_limits<float>::infinity(),prior=best;int result=-1;
        auto it=cells.find({int(std::floor(x/cell)),int(std::floor(z/cell))});
        if(it!=cells.end()){last_candidates=it->second.size();for(auto index:it->second){const auto& s=surfaces.at(index);float h;if(!Height(s,x,z,h))continue;float d=std::abs(y-h);if(d<best){best=d;result=s.layer;}if(s.layer==previous)prior=std::min(prior,d);}}
        if(result>=0){if(hysteresis>0&&previous>=0&&prior<=best+hysteresis)return previous;return result;}
        for(size_t i=0;i<anchors.size();++i){float d=std::abs(y-anchors[i]);if(d<best){best=d;result=int(i);}}return result;
    }
    Json ToJson()const{Json j={{"cell",cell},{"anchors",anchors},{"surfaces",Json::array()},{"cells",Json::array()}};for(auto& s:surfaces)j["surfaces"].push_back({s.layer,s.v,s.y});for(const auto& [k,v]:cells)j["cells"].push_back({k.first,k.second,v});return j;}
    void FromJson(const Json& j){cell=j.at("cell");anchors=j.at("anchors").get<std::vector<float>>();if(cell!=8||anchors.empty()||anchors.size()>128||j.at("surfaces").size()>500000||j.at("cells").size()>100000)throw std::runtime_error("grid bounds");
        for(float a:anchors)if(!std::isfinite(a))throw std::runtime_error("grid anchor");
        for(const auto& s:j.at("surfaces")){Surface v{s.at(1).get<std::array<float,6>>(),s.at(2).get<std::array<float,3>>(),s.at(0)};if(v.layer<0||v.layer>=int(anchors.size()))throw std::runtime_error("grid layer");for(float n:v.v)if(!std::isfinite(n))throw std::runtime_error("grid finite");for(float n:v.y)if(!std::isfinite(n))throw std::runtime_error("grid finite");surfaces.push_back(v);}
        for(const auto& c:j.at("cells")){auto v=c.at(2).get<std::vector<int>>();if(v.size()>8192)throw std::runtime_error("grid candidate count");for(int index:v)if(index<0||index>=int(surfaces.size()))throw std::runtime_error("grid index");max_candidates=std::max(max_candidates,v.size());cells[{c.at(0),c.at(1)}]=std::move(v);}
    }
};
// First matching authored reveal region, preserving V1's rule order.
inline bool Inside(const Json& r,float x,float y,float z){
    if(y<r.value("min_y",-1e9f)||y>r.value("max_y",1e9f))return false;
    if(r.contains("bounds")){const auto& b=r.at("bounds");if(x<b[0].get<float>()||z<b[1].get<float>()||x>b[2].get<float>()||z>b[3].get<float>())return false;}
    if(r.contains("centre")){const auto& p=r.at("centre");float dx=x-p[0].get<float>(),dz=z-p[2].get<float>();if(dx*dx+dz*dz>std::pow(r.value("radius",0.f),2))return false;}
    if(r.contains("triangles")){bool hit=false;for(const auto& t:r["triangles"]){auto cross=[&](int a,int b){return(t[b].get<float>()-t[a].get<float>())*(z-t[a+1].get<float>())-(t[b+1].get<float>()-t[a+1].get<float>())*(x-t[a].get<float>());};float a=cross(0,2),b=cross(2,4),c=cross(4,0);if((a>=0&&b>=0&&c>=0)||(a<=0&&b<=0&&c<=0)){hit=true;break;}}if(!hit)return false;}return true;
}
inline int Region(const Json& feature,float x,float y,float z){if(feature.value("always_visible",false))return -2;int i=0;for(const auto& r:feature.value("reveal_regions",Json::array())){if(Inside(r,x,y,z))return i;++i;}return -1;}
}
