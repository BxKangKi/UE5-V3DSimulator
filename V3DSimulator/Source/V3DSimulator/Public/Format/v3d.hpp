// Copyright (c) 2026 BxKangKi. SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// V3D portable single-header file format. C++17, standard library only.
// All integer fields are explicitly little endian; floats are IEEE-754 binary32/64.
// Strings are length-prefixed UTF-8 without a terminator. No C++ structs are dumped.
// Geometry uses centimeters, left-handed Z-up (+X forward,+Y right,+Z up).
// Quaternion component order is XYZW; transforms are scale, then rotate, then translate.
// See Docs/V3D_FORMAT.md for the container and shader/material conventions.
#ifndef V3D_HPP_INCLUDED
#define V3D_HPP_INCLUDED

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace v3d {
static_assert(sizeof(float)==4 && sizeof(double)==8 &&
    std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
    "V3D requires IEEE-754 binary32/binary64");
using bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t max_name_bytes = 4096;
constexpr std::uint32_t max_string_bytes = 64u*1024u*1024u;
constexpr std::uint64_t max_member_bytes = 2147483647ull;
constexpr std::uint32_t max_texture_layers = 2048;
struct vec2 { float x=0,y=0; };
struct vec3 { float x=0,y=0,z=0; };
struct vec4 { float x=0,y=0,z=0,w=0; };
struct dvec3 { double x=0,y=0,z=0; };
struct quat { double x=0,y=0,z=0,w=1; };
struct transform { quat rotation; dvec3 translation; dvec3 scale{1,1,1}; };
struct bounds { dvec3 minimum,maximum; bool valid=false;
    dvec3 size() const { return valid ? dvec3{maximum.x-minimum.x,maximum.y-minimum.y,maximum.z-minimum.z}:dvec3{}; }
    dvec3 center() const { return valid ? dvec3{(minimum.x+maximum.x)*0.5,(minimum.y+maximum.y)*0.5,(minimum.z+maximum.z)*0.5}:dvec3{}; }
};
struct joint4 { std::uint16_t x=0,y=0,z=0,w=0; };
struct morph_target { std::string name; std::vector<vec3> positions,normals; };
struct primitive {
    std::vector<vec3> positions,normals;
    std::vector<vec4> tangents;
    std::vector<std::vector<vec2>> uvs;
    std::vector<std::uint32_t> indices;
    std::vector<std::vector<joint4>> joints;
    std::vector<std::vector<vec4>> weights;
    std::vector<vec4> colors;
    std::vector<morph_target> morph_targets;
    std::map<std::int32_t,std::string> bone_map;
    std::map<std::string,std::vector<float>> weight_maps;
    std::string material_name;
    std::int32_t material_id=-1,mode=4;
    bool has_material=false,high_precision_uvs=false,high_precision_weights=false,
         disable_shadows=false,has_indices=false;
};
struct mesh {
    std::int32_t mesh_index=-1;
    std::string name;
    std::vector<primitive> primitives;
    std::vector<transform> additional_transforms;
    bool has_normals=false,has_tangents=false,has_uv=false,has_vertex_colors=false;
};
struct bone { std::string name; std::int32_t parent_index=-1; transform local_transform; };
struct skin { std::int32_t skin_index=-1; std::vector<bone> bones; std::map<std::int32_t,std::string> joint_bone_map; };
struct scalar_parameter { std::string name; float value=0; };
struct vector_parameter { std::string name; vec4 value; };
struct texture_parameter { std::string name; std::int32_t texture_id=-1; std::string asset_path; };
struct material {
    std::int32_t material_id=-1;
    std::string name,base_material_path; // Engine hint; external readers need not load it.
    std::uint8_t material_type=0; // 0 opaque,1 translucent,2 two-sided,3 two-sided translucent,4 masked,5 two-sided masked.
    bool unlit=false;
    std::vector<scalar_parameter> scalars;
    std::vector<vector_parameter> vectors;
    std::vector<texture_parameter> textures;
};
// Values belong to V3D, never to an engine's EPixelFormat enum.
enum class texture_format : std::uint32_t {
    unknown=0, rgba8_unorm=1,bgra8_unorm=2,r8_unorm=3,rg8_unorm=4,
    rgba16_float=5,r16_float=6,r32_float=7,rgba32_float=8,r16_unorm=9,
    bc1_unorm=10,bc2_unorm=11,bc3_unorm=12,bc4_unorm=13,bc5_unorm=14,
    bc6h_ufloat=15,bc7_unorm=16,
    astc_4x4_unorm=20,astc_6x6_unorm=21,astc_8x8_unorm=22,
    astc_10x10_unorm=23,astc_12x12_unorm=24,
    etc2_rgb8_unorm=30,etc2_rgba8_unorm=31
};
enum class address_mode : std::uint8_t { wrap=0,clamp=1,mirror=2 };
enum class filter_mode : std::uint8_t { nearest=0,bilinear=1,trilinear=2,default_filter=3 };
struct mip { std::uint32_t width=0,height=0,layers=1; std::vector<std::uint8_t> bytes; };
struct texture {
    std::int32_t texture_id=-1;
    std::string name;
    std::uint32_t width=0,height=0,layers=1;
    texture_format pixel_format=texture_format::unknown;
    // Only set when reading legacy TEXR v1. New writers never emit engine enum IDs.
    std::int32_t legacy_unreal_pixel_format=-1;
    std::uint8_t address_x=0,address_y=0,filter=3,lod_group=0;
    bool srgb=false,is_array=false;
    // Each mip contains every layer, with each full layer preceding the next.
    // All layers share dimensions/format/mip count. Layer count never shrinks with mips.
    std::vector<mip> mips;
};
struct range {
    std::string name;
    std::uint64_t offset=0,stored_size=0,uncompressed_size=0;
    std::uint32_t crc=0;
    std::uint8_t codec=0; // 0 stored; 1 zlib. CRC is over uncompressed bytes.
};
struct manifest {
    std::map<std::int32_t,range> mesh_ranges,skin_ranges,material_ranges,texture_ranges;
    std::map<std::int32_t,std::string> mesh_names;
};

inline bool finite(const vec2& v) { return std::isfinite(v.x)&&std::isfinite(v.y); }
inline bool finite(const vec3& v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline bool finite(const vec4& v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::isfinite(v.w); }
inline bool finite(const dvec3& v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline bool finite(const transform& v) {
    const auto& q=v.rotation;
    return finite(v.translation)&&finite(v.scale)&&std::isfinite(q.x)&&std::isfinite(q.y)&&
        std::isfinite(q.z)&&std::isfinite(q.w)&&std::abs(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1.0)<1e-3;
}
inline bounds mesh_bounds(const mesh& value) {
    bounds b;
    for(const auto& p:value.primitives) for(const auto& v:p.positions) {
        if(!finite(v)) continue;
        if(!b.valid) { b.minimum=b.maximum={v.x,v.y,v.z}; b.valid=true; }
        else { b.minimum.x=std::min(b.minimum.x,double(v.x)); b.minimum.y=std::min(b.minimum.y,double(v.y)); b.minimum.z=std::min(b.minimum.z,double(v.z));
               b.maximum.x=std::max(b.maximum.x,double(v.x)); b.maximum.y=std::max(b.maximum.y,double(v.y)); b.maximum.z=std::max(b.maximum.z,double(v.z)); }
    }
    return b;
}
inline std::uint32_t crc32(const std::uint8_t* data,std::size_t size) {
    static const std::array<std::uint32_t,256> table=[](){
        std::array<std::uint32_t,256> t{};
        for(std::uint32_t i=0;i<256;++i){
            auto c=i;
            for(unsigned bit=0;bit<8;++bit)c=(c>>1)^((0u-(c&1u))&0xedb88320u);
            t[i]=c;
        }
        return t;
    }();
    std::uint32_t c=0xffffffffu;
    for(std::size_t i=0;i<size;++i)c=table[(c^data[i])&255u]^(c>>8);
    return c^0xffffffffu;
}
inline bool texture_mip_bytes(texture_format format,std::uint32_t w,std::uint32_t h,std::uint32_t layers,std::uint64_t& out) {
    std::uint32_t bx=1,by=1,b=0;
    switch(format) {
    case texture_format::rgba8_unorm: case texture_format::bgra8_unorm: case texture_format::r32_float:b=4;break;
    case texture_format::r8_unorm:b=1;break;
    case texture_format::rg8_unorm: case texture_format::r16_unorm: case texture_format::r16_float:b=2;break;
    case texture_format::rgba16_float:b=8;break;
    case texture_format::rgba32_float:b=16;break;
    case texture_format::bc1_unorm:case texture_format::bc4_unorm:case texture_format::etc2_rgb8_unorm:bx=by=4;b=8;break;
    case texture_format::bc2_unorm:case texture_format::bc3_unorm:case texture_format::bc5_unorm:case texture_format::bc6h_ufloat:case texture_format::bc7_unorm:case texture_format::etc2_rgba8_unorm:bx=by=4;b=16;break;
    case texture_format::astc_4x4_unorm:bx=by=4;b=16;break;
    case texture_format::astc_6x6_unorm:bx=by=6;b=16;break;
    case texture_format::astc_8x8_unorm:bx=by=8;b=16;break;
    case texture_format::astc_10x10_unorm:bx=by=10;b=16;break;
    case texture_format::astc_12x12_unorm:bx=by=12;b=16;break;
    default:out=0;return false;
    }
    if(!w||!h||!layers||w>65536||h>65536||layers>max_texture_layers) {out=0;return false;}
    out=((std::uint64_t(w)+bx-1)/bx)*((std::uint64_t(h)+by-1)/by)*b*layers;
    return out<=max_member_bytes;
}

namespace detail {
inline bool utf8(const std::string& s) {
    std::size_t i=0;
    while(i<s.size()) {
        const unsigned c=static_cast<unsigned char>(s[i++]);
        if(c<0x80) continue;
        unsigned n=0,cp=0,min=0;
        if(c>=0xc2&&c<=0xdf){n=1;cp=c&31;min=0x80;}
        else if(c>=0xe0&&c<=0xef){n=2;cp=c&15;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){n=3;cp=c&7;min=0x10000;}
        else return false;
        if(n>s.size()-i) return false;
        while(n--){const unsigned d=static_cast<unsigned char>(s[i++]);if((d&0xc0)!=0x80)return false;cp=(cp<<6)|(d&63);}
        if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)) return false;
    }
    return true;
}
class writer {
    bytes& data_; std::uint64_t limit_; bool ok_=true;
    bool need(std::size_t n) { if(!ok_||n>limit_||data_.size()>limit_-n){ok_=false;return false;}return true; }
public:
    explicit writer(bytes& data,std::uint64_t limit=max_member_bytes):data_(data),limit_(limit){data_.clear();}
    bool ok() const{return ok_;}
    void fail(){ok_=false;}
    void u8(std::uint8_t v){if(need(1))data_.push_back(v);}
    void u16(std::uint16_t v){u8(std::uint8_t(v));u8(std::uint8_t(v>>8));}
    void u32(std::uint32_t v){for(unsigned s=0;s<32;s+=8)u8(std::uint8_t(v>>s));}
    void i32(std::int32_t v){u32(static_cast<std::uint32_t>(v));}
    void u64(std::uint64_t v){for(unsigned s=0;s<64;s+=8)u8(std::uint8_t(v>>s));}
    void f32(float v){std::uint32_t b;std::memcpy(&b,&v,4);u32(b);}
    void f64(double v){std::uint64_t b;std::memcpy(&b,&v,8);u64(b);}
    void count(std::size_t n){if(n>std::numeric_limits<std::uint32_t>::max()){fail();return;}u32(static_cast<std::uint32_t>(n));}
    void raw(const std::uint8_t* p,std::size_t n){if(!need(n)||(!p&&n)){fail();return;}if(n)data_.insert(data_.end(),p,p+n);}
    void string(const std::string& s,std::uint32_t limit=max_string_bytes){if(s.size()>limit||!utf8(s)){fail();return;}count(s.size());raw(reinterpret_cast<const std::uint8_t*>(s.data()),s.size());}
    void vec2_value(const vec2& v){f32(v.x);f32(v.y);}
    void vec3_value(const vec3& v){f32(v.x);f32(v.y);f32(v.z);}
    void vec4_value(const vec4& v){f32(v.x);f32(v.y);f32(v.z);f32(v.w);}
    void vector(const dvec3& v){f64(v.x);f64(v.y);f64(v.z);}
    void transform_value(const transform& v){f64(v.rotation.x);f64(v.rotation.y);f64(v.rotation.z);f64(v.rotation.w);vector(v.translation);vector(v.scale);}
};
class reader {
    const std::uint8_t* data_;std::size_t size_,offset_=0;bool ok_;
    bool need(std::size_t n){if(!ok_||n>size_-offset_){ok_=false;return false;}return true;}
public:
    reader(const std::uint8_t* data,std::size_t size):data_(data),size_(size),ok_(data!=nullptr||size==0){}
    bool ok()const{return ok_;} bool end()const{return ok_&&offset_==size_;}
    void fail(){ok_=false;}
    std::size_t remaining()const{return ok_?size_-offset_:0;}
    std::size_t position()const{return offset_;}
    std::uint8_t u8(){if(!need(1))return 0;return data_[offset_++];}
    std::uint16_t u16(){const auto a=u8();const auto b=u8();return std::uint16_t(a|(std::uint16_t(b)<<8));}
    std::uint32_t u32(){std::uint32_t v=0;for(unsigned s=0;s<32;s+=8)v|=std::uint32_t(u8())<<s;return v;}
    std::int32_t i32(){const auto v=u32();std::int32_t out;std::memcpy(&out,&v,4);return out;}
    std::uint64_t u64(){std::uint64_t v=0;for(unsigned s=0;s<64;s+=8)v|=std::uint64_t(u8())<<s;return v;}
    float f32(){const auto b=u32();float v;std::memcpy(&v,&b,4);return v;}
    double f64(){const auto b=u64();double v;std::memcpy(&v,&b,8);return v;}
    bool boolean(){const auto b=u8();if(b>1)fail();return b!=0;}
    std::uint32_t count(std::uint32_t limit,std::size_t min_bytes=0){const auto n=u32();if(!ok_||n>limit||(min_bytes&&n>remaining()/min_bytes)){fail();return 0;}return n;}
    bool raw(std::uint8_t* p,std::size_t n){if((!p&&n)||!need(n)){fail();return false;}if(n)std::memcpy(p,data_+offset_,n);offset_+=n;return true;}
    bool skip(std::size_t n){if(!need(n))return false;offset_+=n;return true;}
    std::string string(std::uint32_t limit=max_string_bytes){const auto n=count(limit,1);if(!ok_||!n)return {};std::string s(reinterpret_cast<const char*>(data_+offset_),n);offset_+=n;if(!utf8(s))fail();return s;}
    vec2 vec2_value(){vec2 v;v.x=f32();v.y=f32();return v;}
    vec3 vec3_value(){vec3 v;v.x=f32();v.y=f32();v.z=f32();return v;}
    vec4 vec4_value(){vec4 v;v.x=f32();v.y=f32();v.z=f32();v.w=f32();return v;}
    dvec3 vector(){dvec3 v;v.x=f64();v.y=f64();v.z=f64();return v;}
    transform transform_value(){transform v;v.rotation.x=f64();v.rotation.y=f64();v.rotation.z=f64();v.rotation.w=f64();v.translation=vector();v.scale=vector();return v;}
};
inline void write_range(writer& w,const range& r){w.string(r.name,max_name_bytes);w.u64(r.offset);w.u64(r.stored_size);w.u64(r.uncompressed_size);w.u32(r.crc);w.u8(r.codec);w.u8(0);w.u16(0);}
inline range read_range(reader& r){range v;v.name=r.string(max_name_bytes);v.offset=r.u64();v.stored_size=r.u64();v.uncompressed_size=r.u64();v.crc=r.u32();v.codec=r.u8();const auto a=r.u8();const auto b=r.u16();if(a||b)r.fail();return v;}
inline bool failure(std::string& error,const char* message){error=message;return false;}
inline bool finish(writer& w,bytes& data,std::string& error){if(!w.ok()){data.clear();return failure(error,"V3D field or member size exceeded, or invalid UTF-8");}error.clear();return true;}
inline bool header(reader& r,std::uint32_t magic,std::uint32_t version=1){const auto m=r.u32();const auto v=r.u32();if(m!=magic||v!=version)r.fail();return r.ok();}
inline bool valid_name(const std::string& s){return !s.empty()&&s.size()<=max_name_bytes&&utf8(s)&&s.find('\0')==std::string::npos;}
template<class T,class Fn> inline void write_array(writer& w,const std::vector<T>& v,Fn fn){w.count(v.size());for(const auto& x:v)fn(x);}
template<class T,class Fn> inline bool read_array(reader& r,std::vector<T>& v,std::uint32_t limit,std::size_t width,Fn fn,bool skip=false){
    const auto n=r.count(limit,width);if(!r.ok())return false;
    if(skip)return r.skip(std::size_t(n)*width);
    v.reserve(n);for(std::uint32_t i=0;i<n&&r.ok();++i)v.push_back(fn());return r.ok();
}
template<class T> inline bool all_finite(const std::vector<T>& values){for(const auto& v:values)if(!finite(v))return false;return true;}
inline bool valid_range(const range& r){return valid_name(r.name)&&r.codec<=1&&r.stored_size&&r.uncompressed_size&&r.uncompressed_size<=max_member_bytes&&r.stored_size<=max_member_bytes&&(r.codec!=0||r.stored_size==r.uncompressed_size)&&r.offset<=std::numeric_limits<std::uint64_t>::max()-r.stored_size;}
} // namespace detail

// Member codecs follow; output is committed only on successful decode.
inline bool validate(const mesh& m,std::string& error) {
    if(m.mesh_index<0||m.name.size()>max_name_bytes||m.primitives.empty()||m.primitives.size()>65536||m.additional_transforms.size()>65536)
        return detail::failure(error,"Invalid mesh id, name or collection count");
    if(!detail::all_finite(m.additional_transforms))return detail::failure(error,"Invalid mesh transform");
    for(const auto& p:m.primitives) {
        const auto n=p.positions.size();
        if(!n||n>100000000||p.indices.size()>300000000||(p.has_indices&&p.indices.empty())||p.uvs.size()>16||p.joints.size()>8||p.weights.size()!=p.joints.size()||p.morph_targets.size()>4096||p.bone_map.size()>65536||p.weight_maps.size()>4096||p.mode<0||p.mode>6||p.material_id<-1||(p.has_material&&p.material_id<0))
            return detail::failure(error,"Invalid primitive counts, mode or material reference");
        if(!detail::all_finite(p.positions)||!detail::all_finite(p.normals)||!detail::all_finite(p.tangents)||!detail::all_finite(p.colors))return detail::failure(error,"Non-finite vertex data");
        if((!p.normals.empty()&&p.normals.size()!=n)||(!p.tangents.empty()&&p.tangents.size()!=n)||(!p.colors.empty()&&p.colors.size()!=n))return detail::failure(error,"Mismatched vertex stream lengths");
        for(auto i:p.indices)if(i>=n)return detail::failure(error,"Vertex index is outside the position stream");
        for(const auto& uv:p.uvs)if(uv.size()!=n||!detail::all_finite(uv))return detail::failure(error,"Invalid UV stream");
        for(std::size_t i=0;i<p.joints.size();++i)if(p.joints[i].size()!=n||p.weights[i].size()!=n||!detail::all_finite(p.weights[i]))return detail::failure(error,"Invalid skin vertex stream");
        for(const auto& mt:p.morph_targets)if((!mt.positions.empty()&&mt.positions.size()!=n)||(!mt.normals.empty()&&mt.normals.size()!=n)||!detail::all_finite(mt.positions)||!detail::all_finite(mt.normals))return detail::failure(error,"Invalid morph vertex stream");
        for(const auto& b:p.bone_map)if(b.first<0||!detail::valid_name(b.second))return detail::failure(error,"Invalid primitive bone map");
        for(const auto& wm:p.weight_maps) {if(!detail::valid_name(wm.first)||wm.second.size()!=n)return detail::failure(error,"Invalid weight map");for(auto f:wm.second)if(!std::isfinite(f))return detail::failure(error,"Non-finite weight map");}
    }
    error.clear();return true;
}
inline bool encode(const mesh& m,bytes& out,std::string& error) {
    out.clear();
    if(!validate(m,error))return false;
    detail::writer w(out);w.u32(0x4853454d);w.u32(1);w.i32(m.mesh_index);w.string(m.name,max_name_bytes);
    w.u8(m.has_normals);w.u8(m.has_tangents);w.u8(m.has_uv);w.u8(m.has_vertex_colors);
    detail::write_array(w,m.additional_transforms,[&](const transform& t){w.transform_value(t);});
    w.count(m.primitives.size());
    for(const auto& p:m.primitives) {
        detail::write_array(w,p.positions,[&](const vec3& v){w.vec3_value(v);});
        detail::write_array(w,p.normals,[&](const vec3& v){w.vec3_value(v);});
        detail::write_array(w,p.tangents,[&](const vec4& v){w.vec4_value(v);});
        w.count(p.uvs.size());for(const auto& uv:p.uvs)detail::write_array(w,uv,[&](const vec2& v){w.vec2_value(v);});
        detail::write_array(w,p.indices,[&](std::uint32_t i){w.u32(i);});
        w.count(p.joints.size());for(const auto& set:p.joints)detail::write_array(w,set,[&](const joint4& v){w.u16(v.x);w.u16(v.y);w.u16(v.z);w.u16(v.w);});
        w.count(p.weights.size());for(const auto& set:p.weights)detail::write_array(w,set,[&](const vec4& v){w.vec4_value(v);});
        detail::write_array(w,p.colors,[&](const vec4& v){w.vec4_value(v);});
        w.count(p.morph_targets.size());for(const auto& mt:p.morph_targets) {w.string(mt.name,max_name_bytes);detail::write_array(w,mt.positions,[&](const vec3& v){w.vec3_value(v);});detail::write_array(w,mt.normals,[&](const vec3& v){w.vec3_value(v);});}
        w.count(p.bone_map.size());for(const auto& b:p.bone_map){w.i32(b.first);w.string(b.second,max_name_bytes);}
        w.count(p.weight_maps.size());for(const auto& wm:p.weight_maps){w.string(wm.first,max_name_bytes);detail::write_array(w,wm.second,[&](float f){w.f32(f);});}
        w.string(p.material_name,max_name_bytes);w.i32(p.material_id);w.i32(p.mode);
        w.u8(p.has_material);w.u8(p.high_precision_uvs);w.u8(p.high_precision_weights);w.u8(p.disable_shadows);w.u8(p.has_indices);w.u8(0);w.u16(0);
    }
    return detail::finish(w,out,error);
}
inline bool decode(const std::uint8_t* data,std::size_t size,mesh& out,std::string& error,bool dependencies_only=false) {
    if(size>max_member_bytes)return detail::failure(error,"Mesh exceeds member limit");
    detail::reader r(data,size);mesh m;
    if(!detail::header(r,0x4853454d))return detail::failure(error,"Unsupported mesh magic/version");
    m.mesh_index=r.i32();m.name=r.string(max_name_bytes);
    m.has_normals=r.boolean();m.has_tangents=r.boolean();m.has_uv=r.boolean();m.has_vertex_colors=r.boolean();
    detail::read_array(r,m.additional_transforms,65536,80,[&](){return r.transform_value();},dependencies_only);
    const auto np=r.count(65536,4);m.primitives.reserve(np);
    for(std::uint32_t i=0;i<np&&r.ok();++i) {
        primitive p;
        detail::read_array(r,p.positions,100000000,12,[&](){return r.vec3_value();},dependencies_only);
        detail::read_array(r,p.normals,100000000,12,[&](){return r.vec3_value();},dependencies_only);
        detail::read_array(r,p.tangents,100000000,16,[&](){return r.vec4_value();},dependencies_only);
        const auto nuv=r.count(16,4);p.uvs.resize(nuv);for(auto& uv:p.uvs)detail::read_array(r,uv,100000000,8,[&](){return r.vec2_value();},dependencies_only);
        detail::read_array(r,p.indices,300000000,4,[&](){return r.u32();},dependencies_only);
        const auto nj=r.count(8,4);p.joints.resize(nj);for(auto& set:p.joints)detail::read_array(r,set,100000000,8,[&](){joint4 v;v.x=r.u16();v.y=r.u16();v.z=r.u16();v.w=r.u16();return v;},dependencies_only);
        const auto nw=r.count(8,4);p.weights.resize(nw);for(auto& set:p.weights)detail::read_array(r,set,100000000,16,[&](){return r.vec4_value();},dependencies_only);
        detail::read_array(r,p.colors,100000000,16,[&](){return r.vec4_value();},dependencies_only);
        const auto nmt=r.count(4096,12);p.morph_targets.reserve(nmt);
        for(std::uint32_t j=0;j<nmt&&r.ok();++j){morph_target mt;mt.name=r.string(max_name_bytes);detail::read_array(r,mt.positions,100000000,12,[&](){return r.vec3_value();},dependencies_only);detail::read_array(r,mt.normals,100000000,12,[&](){return r.vec3_value();},dependencies_only);p.morph_targets.push_back(std::move(mt));}
        const auto nb=r.count(65536,8);
        for(std::uint32_t j=0;j<nb&&r.ok();++j){const auto key=r.i32();auto name=r.string(max_name_bytes);if(key<0||!detail::valid_name(name)||!p.bone_map.emplace(key,std::move(name)).second)r.fail();}
        const auto nwm=r.count(4096,8);
        for(std::uint32_t j=0;j<nwm&&r.ok();++j){auto key=r.string(max_name_bytes);std::vector<float> values;detail::read_array(r,values,100000000,4,[&](){return r.f32();},dependencies_only);if(!detail::valid_name(key)||!p.weight_maps.emplace(std::move(key),std::move(values)).second)r.fail();}
        p.material_name=r.string(max_name_bytes);p.material_id=r.i32();p.mode=r.i32();
        p.has_material=r.boolean();p.high_precision_uvs=r.boolean();p.high_precision_weights=r.boolean();p.disable_shadows=r.boolean();p.has_indices=r.boolean();const auto a=r.u8();const auto b=r.u16();if(a||b)r.fail();
        if(p.material_id<-1||(p.has_material&&p.material_id<0)||p.mode<0||p.mode>6||nj!=nw)r.fail();
        m.primitives.push_back(std::move(p));
    }
    if(!r.end()||m.mesh_index<0||m.primitives.empty())return detail::failure(error,"Malformed or truncated mesh member");
    if(!dependencies_only&&!validate(m,error))return false;
    out=std::move(m);error.clear();return true;
}
inline bool validate(const skin& s,std::string& error) {
    if(s.skin_index<0||s.bones.empty()||s.bones.size()>65536||s.joint_bone_map.size()>65536)return detail::failure(error,"Invalid skin id/count");
    std::set<std::string> names;
    for(std::size_t i=0;i<s.bones.size();++i){const auto& b=s.bones[i];if(!detail::valid_name(b.name)||b.parent_index<-1||b.parent_index>=static_cast<std::int64_t>(i)||!finite(b.local_transform)||!names.insert(b.name).second)return detail::failure(error,"Invalid or duplicate bone/hierarchy/transform");}
    for(const auto& j:s.joint_bone_map)if(j.first<0||!names.count(j.second))return detail::failure(error,"Joint map references unknown bone");
    error.clear();return true;
}
inline bool encode(const skin& s,bytes& out,std::string& error) {
    out.clear();
    if(!validate(s,error))return false;
    detail::writer w(out);w.u32(0x4e494b53);w.u32(1);w.i32(s.skin_index);w.count(s.bones.size());
    for(const auto& b:s.bones){w.string(b.name,max_name_bytes);w.i32(b.parent_index);w.transform_value(b.local_transform);}
    w.count(s.joint_bone_map.size());for(const auto& j:s.joint_bone_map){w.i32(j.first);w.string(j.second,max_name_bytes);}return detail::finish(w,out,error);
}
inline bool decode(const std::uint8_t* data,std::size_t size,skin& out,std::string& error) {
    if(size>max_member_bytes)return detail::failure(error,"Skin exceeds member limit");
    detail::reader r(data,size);skin s;
    if(!detail::header(r,0x4e494b53))return detail::failure(error,"Unsupported skin magic/version");
    s.skin_index=r.i32();const auto nb=r.count(65536,88);s.bones.reserve(nb);
    for(std::uint32_t i=0;i<nb&&r.ok();++i){bone b;b.name=r.string(max_name_bytes);b.parent_index=r.i32();b.local_transform=r.transform_value();s.bones.push_back(std::move(b));}
    const auto nj=r.count(65536,8);for(std::uint32_t i=0;i<nj&&r.ok();++i){const auto k=r.i32();auto v=r.string(max_name_bytes);if(!s.joint_bone_map.emplace(k,std::move(v)).second)r.fail();}
    if(!r.end())return detail::failure(error,"Malformed or truncated skin member");
    if(!validate(s,error))return false;
    out=std::move(s);error.clear();return true;
}
inline bool validate(const material& m,std::string& error) {
    if(m.material_id<0||!detail::valid_name(m.name)||m.material_type>5||m.scalars.size()>65536||m.vectors.size()>65536||m.textures.size()>65536)return detail::failure(error,"Invalid material id/name/type/count");
    std::set<std::string> names;
    for(const auto& p:m.scalars)if(!detail::valid_name(p.name)||!std::isfinite(p.value)||!names.insert(p.name).second)return detail::failure(error,"Invalid or duplicate scalar parameter");
    names.clear();for(const auto& p:m.vectors)if(!detail::valid_name(p.name)||!finite(p.value)||!names.insert(p.name).second)return detail::failure(error,"Invalid or duplicate vector parameter");
    names.clear();for(const auto& p:m.textures)if(!detail::valid_name(p.name)||p.texture_id<-1||((p.texture_id>=0)==!p.asset_path.empty())||!names.insert(p.name).second)return detail::failure(error,"Invalid or duplicate texture parameter");
    error.clear();return true;
}
inline bool encode(const material& m,bytes& out,std::string& error) {
    out.clear();
    if(!validate(m,error))return false;
    detail::writer w(out);w.u32(0x4c54414d);w.u32(1);w.i32(m.material_id);w.string(m.name,max_name_bytes);w.string(m.base_material_path);w.u8(m.material_type);w.u8(m.unlit);w.u16(0);
    w.count(m.scalars.size());for(const auto& p:m.scalars){w.string(p.name,max_name_bytes);w.f32(p.value);}
    w.count(m.vectors.size());for(const auto& p:m.vectors){w.string(p.name,max_name_bytes);w.vec4_value(p.value);}
    w.count(m.textures.size());for(const auto& p:m.textures){w.string(p.name,max_name_bytes);w.i32(p.texture_id);w.string(p.asset_path);}return detail::finish(w,out,error);
}
inline bool decode(const std::uint8_t* data,std::size_t size,material& out,std::string& error) {
    if(size>max_member_bytes)return detail::failure(error,"Material exceeds member limit");
    detail::reader r(data,size);material m;
    if(!detail::header(r,0x4c54414d))return detail::failure(error,"Unsupported material magic/version");
    m.material_id=r.i32();m.name=r.string(max_name_bytes);m.base_material_path=r.string();m.material_type=r.u8();m.unlit=r.boolean();if(r.u16()!=0)r.fail();
    const auto ns=r.count(65536,8);m.scalars.reserve(ns);for(std::uint32_t i=0;i<ns&&r.ok();++i){scalar_parameter p;p.name=r.string(max_name_bytes);p.value=r.f32();m.scalars.push_back(std::move(p));}
    const auto nv=r.count(65536,20);m.vectors.reserve(nv);for(std::uint32_t i=0;i<nv&&r.ok();++i){vector_parameter p;p.name=r.string(max_name_bytes);p.value=r.vec4_value();m.vectors.push_back(std::move(p));}
    const auto nt=r.count(65536,12);m.textures.reserve(nt);for(std::uint32_t i=0;i<nt&&r.ok();++i){texture_parameter p;p.name=r.string(max_name_bytes);p.texture_id=r.i32();p.asset_path=r.string();m.textures.push_back(std::move(p));}
    if(!r.end())return detail::failure(error,"Malformed or truncated material member");
    if(!validate(m,error))return false;
    out=std::move(m);error.clear();return true;
}
inline bool validate(const texture& t,std::string& error,bool allow_legacy=false) {
    if(t.texture_id<0||!detail::valid_name(t.name)||!t.width||!t.height||t.width>65536||t.height>65536||!t.layers||t.layers>max_texture_layers||(!t.is_array&&t.layers!=1)||t.address_x>2||t.address_y>2||t.filter>3||t.mips.empty()||t.mips.size()>32)
        return detail::failure(error,"Invalid texture id/name/dimensions/sampler/mip count");
    const bool legacy=allow_legacy&&t.legacy_unreal_pixel_format>0&&t.pixel_format==texture_format::unknown;
    std::uint32_t w=t.width,h=t.height;
    for(const auto& m:t.mips){std::uint64_t expected=0;if(m.width!=w||m.height!=h||m.layers!=t.layers||m.bytes.empty()||m.bytes.size()>1024ull*1024ull*1024ull||(!legacy&&(!texture_mip_bytes(t.pixel_format,w,h,t.layers,expected)||m.bytes.size()!=expected)))return detail::failure(error,"Invalid texture format or mip byte layout");w=std::max(1u,w>>1);h=std::max(1u,h>>1);}
    error.clear();return true;
}
inline bool encode(const texture& t,bytes& out,std::string& error) {
    out.clear();
    if(!validate(t,error))return false;
    detail::writer w(out);w.u32(0x52584554);w.u32(2);w.i32(t.texture_id);w.string(t.name,max_name_bytes);w.u32(t.width);w.u32(t.height);w.u32(static_cast<std::uint32_t>(t.pixel_format));w.u32(t.layers);
    w.u8(t.address_x);w.u8(t.address_y);w.u8(t.filter);w.u8(t.lod_group);w.u8(t.srgb);w.u8(t.is_array);w.u16(0);w.count(t.mips.size());
    for(const auto& m:t.mips){w.u32(m.width);w.u32(m.height);w.u32(m.layers);w.count(m.bytes.size());w.raw(m.bytes.data(),m.bytes.size());}return detail::finish(w,out,error);
}
inline bool decode(const std::uint8_t* data,std::size_t size,texture& out,std::string& error,std::uint32_t max_resolution=0) {
    if(size>max_member_bytes)return detail::failure(error,"Texture exceeds member limit");
    detail::reader r(data,size);texture t;
    const auto magic=r.u32();const auto version=r.u32();if(magic!=0x52584554||(version!=1&&version!=2))return detail::failure(error,"Unsupported texture magic/version");
    t.texture_id=r.i32();t.name=r.string(max_name_bytes);t.width=r.u32();t.height=r.u32();const auto format=r.u32();
    if(version==1){if(format>std::uint32_t(std::numeric_limits<std::int32_t>::max()))r.fail();t.legacy_unreal_pixel_format=static_cast<std::int32_t>(format);}
    else {t.pixel_format=static_cast<texture_format>(format);t.layers=r.u32();}
    t.address_x=r.u8();t.address_y=r.u8();t.filter=r.u8();t.lod_group=r.u8();t.srgb=r.boolean();t.is_array=r.boolean();if(r.u16()!=0||(version==1&&t.is_array))r.fail();
    const auto nm=r.count(32,16);std::uint32_t w=t.width,h=t.height;
    if(!w||!h||w>65536||h>65536||!t.layers||t.layers>max_texture_layers||(!t.is_array&&t.layers!=1))r.fail();
    for(std::uint32_t i=0;i<nm&&r.ok();++i){mip m;m.width=r.u32();m.height=r.u32();m.layers=r.u32();const auto n=r.count(1024u*1024u*1024u,1);std::uint64_t expected=0;
        if(m.width!=w||m.height!=h||m.layers!=t.layers||!n||(version==2&&(!texture_mip_bytes(t.pixel_format,w,h,t.layers,expected)||n!=expected))){r.fail();break;}
        w=std::max(1u,w>>1);h=std::max(1u,h>>1);
        if(max_resolution&&(m.width>max_resolution||m.height>max_resolution)&&i+1<nm){r.skip(n);continue;}
        m.bytes.resize(n);r.raw(m.bytes.data(),n);t.mips.push_back(std::move(m));
    }
    if(!r.end()||t.mips.empty())return detail::failure(error,"Malformed or truncated texture member");
    t.width=t.mips.front().width;t.height=t.mips.front().height;
    if(!validate(t,error,version==1))return false;
    out=std::move(t);error.clear();return true;
}
inline bool validate(const manifest& m,std::string& error) {
    for(const auto* map:{&m.mesh_ranges,&m.skin_ranges,&m.material_ranges,&m.texture_ranges}){if(map->size()>1000000)return detail::failure(error,"Manifest range limit exceeded");for(const auto& p:*map)if(p.first<0||!detail::valid_range(p.second))return detail::failure(error,"Invalid manifest range");}
    if(m.mesh_names.size()>1000000)return detail::failure(error,"Manifest name limit exceeded");
    for(const auto& p:m.mesh_names)if(p.first<0||!detail::valid_name(p.second))return detail::failure(error,"Invalid manifest mesh name");
    error.clear();return true;
}
inline bool encode(const manifest& m,bytes& out,std::string& error) {
    out.clear();
    if(!validate(m,error))return false;
    detail::writer w(out);w.u32(0x464e414d);w.u32(1);
    for(const auto* map:{&m.mesh_ranges,&m.skin_ranges,&m.material_ranges,&m.texture_ranges}){w.count(map->size());for(const auto& p:*map){w.i32(p.first);detail::write_range(w,p.second);}}
    w.count(m.mesh_names.size());
    for(const auto& p:m.mesh_names){w.i32(p.first);w.string(p.second,max_name_bytes);}return detail::finish(w,out,error);
}
inline bool decode(const std::uint8_t* data,std::size_t size,manifest& out,std::string& error) {
    if(size>max_member_bytes)return detail::failure(error,"Manifest exceeds member limit");
    detail::reader r(data,size);manifest m;
    if(!detail::header(r,0x464e414d))return detail::failure(error,"Unsupported manifest magic/version");
    for(auto* map:{&m.mesh_ranges,&m.skin_ranges,&m.material_ranges,&m.texture_ranges}){const auto n=r.count(1000000,40);for(std::uint32_t i=0;i<n&&r.ok();++i){const auto key=r.i32();auto value=detail::read_range(r);if(!map->emplace(key,std::move(value)).second)r.fail();}}
    const auto nn=r.count(1000000,8);for(std::uint32_t i=0;i<nn&&r.ok();++i){const auto key=r.i32();auto value=r.string(max_name_bytes);if(!m.mesh_names.emplace(key,std::move(value)).second)r.fail();}
    if(!r.end())return detail::failure(error,"Malformed or truncated manifest member");
    if(!validate(m,error))return false;
    out=std::move(m);error.clear();return true;
}
// Convenience overloads preserve the pointer+length API for memory-mapped callers.
template<class T> inline bool decode(const bytes& data,T& out,std::string& error){return decode(data.data(),data.size(),out,error);}

// V3D_ARCHIVE_SECTION
// ---- Portable V3D archive, directory and scene metadata API -----------------
// All coordinates are left-handed, +X forward, +Y right, +Z up, centimetres.
// A transform is quaternion XYZW, translation XYZ, scale XYZ (10 IEEE f64s).
// This intentionally preserves the simulator's authored coordinate basis.
constexpr std::uint64_t archive_magic = UINT64_C(0x00000a0d1a443356);
constexpr std::uint64_t legacy_archive_magic = UINT64_C(0x0000444c524f5747);
constexpr std::uint32_t archive_header_bytes = 64;
constexpr std::uint64_t archive_revision = 6;
constexpr std::uint64_t archive_alignment = 4096;
constexpr std::uint64_t archive_max_bytes = UINT64_C(16) * 1024 * 1024 * 1024 * 1024;
constexpr std::uint64_t archive_max_directory_bytes = UINT64_C(256) * 1024 * 1024;
using guid = std::array<std::uint32_t, 4>;
struct archive_header {
    std::uint64_t directory_offset = 0, directory_size = 0, revision = archive_revision;
    std::uint32_t directory_crc = 0, model_count = 0, sound_count = 0;
    guid build_id{{0,0,0,0}};
    bool legacy_magic = false;
};
struct model_summary {
    std::int32_t source_node_count = 0, source_mesh_count = 0;
    std::int32_t source_material_count = 0, source_texture_count = 0;
    dvec3 center{}, size{};
};
struct gravity_field {
    bool enabled = false;
    std::uint8_t falloff = 0;
     // 0 constant, 1 linear.
    std::int32_t priority = 0;
    double radius_cm = 1000.0, strength_cm_per_second_squared = 980.0;
    dvec3 local_center{};
};
struct model_record {
    guid uuid{{0,0,0,0}};
    std::uint8_t asset_type = 0;
     // 0 model.
    std::uint8_t model_type = 1;
     // 1 static, 2 dynamic, 3 character.
    std::uint8_t entity_type = 0;
     // 0 none, 1 vehicle, 2 prop, 3 animal.
    std::uint8_t item_type = 0;
     // 0 none, 1 weapon, 2 tool, 3 misc.
    std::string name, display_name;
    gravity_field gravity;
    model_summary summary;
    range definition_range, metadata_range, manifest_range;
};
struct sound_record {
    guid uuid{{0,0,0,0}};
    std::string name, display_name;
    range definition_range, wave_range;
};
struct archive_directory {
    range config_range;
    std::vector<range> script_ranges;
     // 3 in v5/v6; empty in legacy v4.
    std::vector<model_record> models;
    std::vector<sound_record> sounds;
};
struct node_transform {
    std::int32_t node_index = -1, parent_index = -1, mesh_index = -1, skin_index = -1;
    std::string name;
    transform local_transform;
};
struct scene_instance {
    std::string mesh_name;
    // Accumulated transform in model/scene space; apply the model actor placement separately.
    transform world_transform;
    std::array<std::int32_t,3> coarse_chunk{{0,0,0}}, fine_chunk{{0,0,0}};
    bool always_loaded = true;
};
struct water_instance {
    // Accumulated transform in model/scene space; apply the model actor placement separately.
    transform world_transform;
    float stream_radius = 1.0f;
};
struct collider {
    std::uint8_t type = 0;
     // 0 none, 1 sphere, 2 capsule, 3 box.
    dvec3 center{}, size{};
};
struct light {
    dvec3 location{};
    float intensity = 0, source_radius = 0, soft_source_radius = 0;
    float attenuation_radius = 0, length = 0;
    std::uint8_t unit = 0;
     // 0 unitless, 1 candelas, 2 lumens, 3 EV, 4 nits.
};
struct mesh_settings {
    bool complex_collision = false, simple_collision = false, is_entity = false;
    std::vector<collider> colliders;
    std::vector<light> lights;
};
struct scene_mesh {
    std::array<std::int32_t,4> lods{{-1,-1,-1,-1}};
    dvec3 extent{}, size{};
    mesh_settings settings;
};
struct model_metadata {
    model_summary summary;
    std::vector<node_transform> nodes;
    std::map<std::string, scene_instance> instances;
    std::map<std::string, water_instance> waters;
    std::map<std::string, scene_mesh> meshes;
    std::map<std::string, mesh_settings> settings;
};
struct model_definition {
    std::map<std::string,std::string> bones;
    std::string json;
};
inline std::string guid_digits(const guid& value) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
     out.reserve(32);
    for (std::uint32_t word : value)
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(word >> shift) & 15]);
    return out;
}
inline const char* script_member_name(std::size_t index) {
    static const char* names[] = {"scripts/materials.json", "scripts/sounds.json", "scripts/commands.json"};
    return index < 3 ? names[index] : "";
}
namespace archive_detail {
inline bool fail(std::string& error, const char* text) { error = text;
 return false;
 }
inline bool finite_vector(const dvec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
 }
inline bool positive_size(const dvec3& v) { return finite_vector(v) && v.x >= 0 && v.y >= 0 && v.z >= 0;
 }
inline bool valid_guid(const guid& v) { return v[0] || v[1] || v[2] || v[3];
 }
inline bool valid_transform(const transform& t) {
    const quat& q = t.rotation;
    const double n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    return finite_vector(t.translation) && finite_vector(t.scale) && std::isfinite(n) && std::abs(n-1.0) <= 1e-4;
}
inline void write_guid(detail::writer& w, const guid& g) { for (auto v:g) w.u32(v);
 }
inline guid read_guid(detail::reader& r) { guid g{};
 for (auto& v:g) v=r.u32();
 return g;
 }
inline void write_summary(detail::writer& w, const model_summary& s) {
    w.i32(s.source_node_count);
     w.i32(s.source_mesh_count);
     w.i32(s.source_material_count);
     w.i32(s.source_texture_count);
    w.vector(s.center);
     w.vector(s.size);
}
inline model_summary read_summary(detail::reader& r) {
    model_summary s;
     s.source_node_count=r.i32();
     s.source_mesh_count=r.i32();
    s.source_material_count=r.i32();
     s.source_texture_count=r.i32();
     s.center=r.vector();
     s.size=r.vector();
     return s;
}
inline bool valid_summary(const model_summary& s) {
    return s.source_node_count >= 0 && s.source_node_count <= 2000000
        && s.source_mesh_count >= 0 && s.source_mesh_count <= 1000000
        && s.source_material_count >= 0 && s.source_material_count <= 1000000
        && s.source_texture_count >= 0 && s.source_texture_count <= 1000000
        && finite_vector(s.center) && positive_size(s.size);
}
inline bool safe_member_name(const std::string& s) {
    return !s.empty() && s.size() <= 4096 && s.front() != '/' && s.find('\\') == std::string::npos
        && s.find("../") == std::string::npos && s.find(':') == std::string::npos
        && s.find('\0') == std::string::npos;
}
inline bool suffix(const std::string& s, const char* end) {
    const std::size_t n = std::strlen(end);
     return s.size() >= n && s.compare(s.size()-n,n,end)==0;
}
inline bool valid_range(const range& r, std::uint64_t end, std::uint64_t limit) {
    return safe_member_name(r.name) && (suffix(r.name,".dat") || suffix(r.name,".json") || suffix(r.name,".wav"))
        && r.offset >= archive_header_bytes && r.offset % archive_alignment == 0
        && r.stored_size > 0 && r.stored_size <= limit && r.uncompressed_size > 0 && r.uncompressed_size <= limit
        && r.offset <= end && r.stored_size <= end-r.offset
        && ((r.codec==0 && r.stored_size==r.uncompressed_size) || (r.codec==1 && r.stored_size<r.uncompressed_size));
}
inline bool valid_settings(const mesh_settings& s) {
    if (s.colliders.size()>65536 || s.lights.size()>65536) return false;
    for (const auto& c:s.colliders) if(c.type>3 || !finite_vector(c.center) || !positive_size(c.size)) return false;
    for (const auto& l:s.lights)
        if (!finite_vector(l.location) || !std::isfinite(l.intensity) || !std::isfinite(l.source_radius)
            || !std::isfinite(l.soft_source_radius) || !std::isfinite(l.attenuation_radius)
            || !std::isfinite(l.length) || l.unit>4 || l.source_radius<0 || l.soft_source_radius<0
            || l.attenuation_radius<0 || l.length<0) return false;
    return true;
}
inline void write_settings(detail::writer& w, const mesh_settings& s) {
    w.u8(s.complex_collision);
     w.u8(s.simple_collision);
     w.u8(s.is_entity);
     w.u8(0);
    w.count(s.colliders.size());
    for (const auto& c:s.colliders) { w.u8(c.type);
    w.u8(0);
    w.u16(0);
    w.vector(c.center);
    w.vector(c.size);
     }
    w.count(s.lights.size());
    for (const auto& l:s.lights) {
        w.vector(l.location);
         w.f32(l.intensity);
        w.f32(l.source_radius);
        w.f32(l.soft_source_radius);
        w.f32(l.attenuation_radius);
        w.f32(l.length);
        w.u8(l.unit);
        w.u8(0);
        w.u16(0);
    }
}
inline bool read_settings(detail::reader& r, mesh_settings& s) {
    const auto a=r.u8(),b=r.u8(),c=r.u8();
     const auto reserved=r.u8();
    if(a>1||b>1||c>1||reserved!=0) return false;
    s.complex_collision=a!=0;
    s.simple_collision=b!=0;
    s.is_entity=c!=0;
    const auto nc=r.count(65536,52);
     if(!r.ok()) return false;
     s.colliders.reserve(nc);
    for(std::uint32_t i=0;i<nc;++i) {
        collider x;
         x.type=r.u8();
         const auto p=r.u8();
         const auto q=r.u16();
        x.center=r.vector();
        x.size=r.vector();
        if(p||q) return false;
        s.colliders.push_back(std::move(x));
    }
    const auto nl=r.count(65536,48);
     if(!r.ok()) return false;
     s.lights.reserve(nl);
    for(std::uint32_t i=0;i<nl;++i) {
        light x;
        x.location=r.vector();
        x.intensity=r.f32();
        x.source_radius=r.f32();
        x.soft_source_radius=r.f32();
        x.attenuation_radius=r.f32();
        x.length=r.f32();
        x.unit=r.u8();
        const auto p=r.u8();
        const auto q=r.u16();
        if(p||q)return false;
        s.lights.push_back(std::move(x));
    }
    return r.ok() && valid_settings(s);
}
} // namespace archive_detail
inline bool encode_header(const archive_header& h, bytes& out, std::string& error) {
    if ((h.legacy_magic && h.revision!=4 && h.revision!=5) || (!h.legacy_magic && h.revision!=archive_revision)
        || !archive_detail::valid_guid(h.build_id) || h.model_count>100000 || h.sound_count>100000
        || h.directory_offset<archive_header_bytes || !h.directory_size || h.directory_size>archive_max_directory_bytes
        || h.directory_offset>archive_max_bytes-h.directory_size)
        return archive_detail::fail(error,"Invalid V3D header fields");
    detail::writer w(out,archive_header_bytes);
    w.u64(h.legacy_magic?legacy_archive_magic:archive_magic);
    w.u32(archive_header_bytes);
    w.u32(h.sound_count);
    w.u64(h.directory_offset);
    w.u64(h.directory_size);
    w.u32(h.directory_crc);
    w.u32(h.model_count);
    archive_detail::write_guid(w,h.build_id);
    w.u64(h.revision);
    if(!w.ok())return archive_detail::fail(error,"V3D header exceeded 64 bytes");
    error.clear();
    return true;
}
inline bool decode_header(const std::uint8_t* data,std::size_t size,std::uint64_t file_size,archive_header& out,std::string& error) {
    if(size!=archive_header_bytes || file_size<archive_header_bytes || file_size>archive_max_bytes)
        return archive_detail::fail(error,"Invalid V3D header/file size");
    detail::reader r(data,size);
    archive_header h;
    const auto magic=r.u64();
    const auto header_size=r.u32();
    h.sound_count=r.u32();
    h.directory_offset=r.u64();
    h.directory_size=r.u64();
    h.directory_crc=r.u32();
    h.model_count=r.u32();
    h.build_id=archive_detail::read_guid(r);
    h.revision=r.u64();
    h.legacy_magic=magic==legacy_archive_magic;
    if(!r.end() || header_size!=archive_header_bytes
        || !((magic==archive_magic&&h.revision==6)||(magic==legacy_archive_magic&&(h.revision==4||h.revision==5)))
        || !archive_detail::valid_guid(h.build_id) || h.model_count>100000 || h.sound_count>100000
        || !h.directory_size || h.directory_size>archive_max_directory_bytes
        || h.directory_offset<archive_header_bytes || h.directory_offset>file_size
        || h.directory_size!=file_size-h.directory_offset)
        return archive_detail::fail(error,"Invalid V3D signature, revision, counts or directory bounds");
    out=h;
    error.clear();
    return true;
}
inline bool validate(const model_metadata& m,std::string& error) {
    using namespace archive_detail;
    if(!valid_summary(m.summary) || m.nodes.size()!=std::size_t(m.summary.source_node_count)
        || m.instances.size()>2000000 || m.waters.size()>2000000 || m.meshes.size()>1000000 || m.settings.size()>1000000
        || m.instances.size()+m.waters.size()>m.nodes.size() || m.meshes.size()>std::size_t(m.summary.source_mesh_count))
        return fail(error,"Invalid V3D scene counts or bounds");
    std::vector<std::int32_t> parents(m.nodes.size(),-2);
    for(const auto& n:m.nodes) {
        if(n.node_index<0 || std::size_t(n.node_index)>=m.nodes.size() || parents[n.node_index]!=-2
            || n.parent_index< -1 || n.parent_index>=m.summary.source_node_count || n.parent_index==n.node_index
            || n.mesh_index< -1 || n.mesh_index>=m.summary.source_mesh_count || n.skin_index< -1
            || n.name.size()>4096 || !valid_transform(n.local_transform)) return fail(error,"Invalid V3D hierarchy node");
        parents[n.node_index]=n.parent_index;
    }
    std::vector<std::uint8_t> states(m.nodes.size(),0);
    for(std::size_t start=0;start<m.nodes.size();++start) {
        std::int32_t i=static_cast<std::int32_t>(start);
        while(i!=-1&&states[i]==0){states[i]=1;
        i=parents[i];
        }
        if(i!=-1&&states[i]==1)return fail(error,"Cyclic V3D hierarchy");
        i=static_cast<std::int32_t>(start);
        while(i!=-1&&states[i]==1){states[i]=2;
        i=parents[i];
        }
    }
    for(const auto& kv:m.instances) {
        const auto& x=kv.second;
        if(kv.first.empty()||kv.first.size()>4096||m.meshes.find(x.mesh_name)==m.meshes.end()||!valid_transform(x.world_transform))
            return fail(error,"Invalid V3D scene instance");
        for(auto v:x.fine_chunk)if(v<0||v>=16)return fail(error,"Invalid V3D fine chunk");
    }
    for(const auto& kv:m.waters)if(kv.first.empty()||kv.first.size()>4096||!valid_transform(kv.second.world_transform)
        ||!std::isfinite(kv.second.stream_radius)||kv.second.stream_radius<=0)return fail(error,"Invalid V3D water instance");
    for(const auto& kv:m.meshes) {
        const auto& x=kv.second;
        bool has=false;
        if(kv.first.empty()||kv.first.size()>4096||!positive_size(x.extent)||!positive_size(x.size)||!valid_settings(x.settings))
            return fail(error,"Invalid V3D mesh bounds/settings");
        for(auto lod:x.lods){if(lod< -1||lod>=m.summary.source_mesh_count)return fail(error,"Invalid V3D LOD reference");
        has|=lod>=0;
        }
        if(!has)return fail(error,"V3D scene mesh has no LOD");
    }
    for(const auto& kv:m.settings)if(kv.first.empty()||kv.first.size()>4096||!valid_settings(kv.second))return fail(error,"Invalid V3D named settings");
    error.clear();
    return true;
}
inline bool encode(const model_metadata& m,bytes& out,std::string& error) {
    if(!validate(m,error))return false;
    detail::writer w(out,archive_max_directory_bytes);
    w.u32(0x31444d47u);
    archive_detail::write_summary(w,m.summary);
    w.count(m.nodes.size());
    for(const auto& n:m.nodes){w.i32(n.node_index);
    w.i32(n.parent_index);
    w.i32(n.mesh_index);
    w.i32(n.skin_index);
    w.string(n.name,4096);
    w.transform_value(n.local_transform);
    }
    w.count(m.instances.size());
    for(const auto& kv:m.instances){const auto& n=kv.second;
    w.string(kv.first,4096);
    w.string(n.mesh_name,4096);
    w.transform_value(n.world_transform);
    for(auto i:n.coarse_chunk)w.i32(i);
    for(auto i:n.fine_chunk)w.i32(i);
    w.u8(n.always_loaded);
    w.u8(0);
    w.u16(0);
    }
    w.count(m.waters.size());
    for(const auto& kv:m.waters){w.string(kv.first,4096);
    w.transform_value(kv.second.world_transform);
    w.f32(kv.second.stream_radius);
    }
    w.count(m.meshes.size());
    for(const auto& kv:m.meshes){w.string(kv.first,4096);
    for(auto i:kv.second.lods)w.i32(i);
    w.vector(kv.second.extent);
    w.vector(kv.second.size);
    archive_detail::write_settings(w,kv.second.settings);
    }
    w.count(m.settings.size());
    for(const auto& kv:m.settings){w.string(kv.first,4096);
    archive_detail::write_settings(w,kv.second);
    }
    if(!w.ok())return archive_detail::fail(error,"V3D metadata exceeded limits");
    error.clear();
    return true;
}
inline bool decode(const std::uint8_t* data,std::size_t size,model_metadata& out,std::string& error) {
    if(size>archive_max_directory_bytes)return archive_detail::fail(error,"V3D metadata exceeds limit");
    detail::reader r(data,size);
    model_metadata m;
    if(r.u32()!=0x31444d47u)return archive_detail::fail(error,"Invalid V3D metadata signature");
    m.summary=archive_detail::read_summary(r);
    auto count=r.count(2000000,100);
    if(!r.ok())return archive_detail::fail(error,"Truncated V3D node table");
    m.nodes.reserve(count);
    for(std::uint32_t i=0;i<count;++i){node_transform n;
    n.node_index=r.i32();
    n.parent_index=r.i32();
    n.mesh_index=r.i32();
    n.skin_index=r.i32();
    n.name=r.string(4096);
    n.local_transform=r.transform_value();
    m.nodes.push_back(std::move(n));
    }
    count=r.count(2000000,116);
    if(!r.ok())return archive_detail::fail(error,"Truncated V3D instance table");
    for(std::uint32_t i=0;i<count;++i){const auto key=r.string(4096);
    scene_instance n;
    n.mesh_name=r.string(4096);
    n.world_transform=r.transform_value();
    for(auto& v:n.coarse_chunk)v=r.i32();
    for(auto& v:n.fine_chunk)v=r.i32();
    const auto a=r.u8(),b=r.u8();
    const auto c=r.u16();
    n.always_loaded=a!=0;
    if(a>1||b||c||!m.instances.emplace(key,std::move(n)).second)return archive_detail::fail(error,"Invalid/duplicate V3D instance");
    }
    count=r.count(2000000,88);
    if(!r.ok())return archive_detail::fail(error,"Truncated V3D water table");
    for(std::uint32_t i=0;i<count;++i){const auto key=r.string(4096);
    water_instance n;
    n.world_transform=r.transform_value();
    n.stream_radius=r.f32();
    if(!m.waters.emplace(key,std::move(n)).second)return archive_detail::fail(error,"Duplicate V3D water instance");
    }
    count=r.count(1000000,80);
    if(!r.ok())return archive_detail::fail(error,"Truncated V3D scene mesh table");
    for(std::uint32_t i=0;i<count;++i){const auto key=r.string(4096);
    scene_mesh n;
    for(auto& v:n.lods)v=r.i32();
    n.extent=r.vector();
    n.size=r.vector();
    if(!archive_detail::read_settings(r,n.settings)||!m.meshes.emplace(key,std::move(n)).second)return archive_detail::fail(error,"Invalid/duplicate V3D scene mesh");
    }
    count=r.count(1000000,16);
    if(!r.ok())return archive_detail::fail(error,"Truncated V3D settings table");
    for(std::uint32_t i=0;i<count;++i){const auto key=r.string(4096);
    mesh_settings n;
    if(!archive_detail::read_settings(r,n)||!m.settings.emplace(key,std::move(n)).second)return archive_detail::fail(error,"Invalid/duplicate V3D settings");
    }
    if(!r.end())return archive_detail::fail(error,"V3D metadata is truncated or has trailing bytes");
    if(!validate(m,error))return false;
    out=std::move(m);
    return true;
}
inline bool encode(const model_definition& d,bytes& out,std::string& error) {
    if(d.bones.size()>65536||d.json.empty())return archive_detail::fail(error,"Invalid V3D definition");
    detail::writer w(out,archive_max_directory_bytes);
    w.u32(0x31464447u);
    w.count(d.bones.size());
    for(const auto& kv:d.bones){if(kv.first.empty()||kv.second.empty())return archive_detail::fail(error,"Empty V3D bone alias");
    w.string(kv.first,4096);
    w.string(kv.second,4096);
    }w.string(d.json);
    if(!w.ok())return archive_detail::fail(error,"V3D definition exceeded limits");
    error.clear();
    return true;
}
inline bool decode(const std::uint8_t* data,std::size_t size,model_definition& out,std::string& error) {
    if(size>archive_max_directory_bytes)return archive_detail::fail(error,"V3D definition exceeds limit");
    detail::reader r(data,size);
    model_definition d;
    if(r.u32()!=0x31464447u)return archive_detail::fail(error,"Invalid V3D definition signature");
    const auto n=r.count(65536,8);
    for(std::uint32_t i=0;i<n;++i){const auto k=r.string(4096);
    const auto v=r.string(4096);
    if(k.empty()||v.empty()||!d.bones.emplace(k,v).second)return archive_detail::fail(error,"Invalid/duplicate V3D bone alias");
    }
    d.json=r.string();
    if(!r.end()||d.json.empty())return archive_detail::fail(error,"Truncated/empty V3D definition");
    out=std::move(d);
    error.clear();
    return true;
}
inline bool encode_json(const std::string& json,bytes& out,std::string& error) {
    if(json.find_first_not_of(" \t\r\n")==std::string::npos)return archive_detail::fail(error,"Empty V3D JSON member");
    detail::writer w(out,archive_max_directory_bytes);
    w.u32(0x31434647u);
    w.string(json);
    if(!w.ok())return archive_detail::fail(error,"V3D JSON member exceeded limits");
    error.clear();
    return true;
}
inline bool decode_json(const std::uint8_t* data,std::size_t size,std::string& out,std::string& error) {
    if(size>archive_max_directory_bytes)return archive_detail::fail(error,"V3D JSON member exceeds limit");
    detail::reader r(data,size);
    if(r.u32()!=0x31434647u)return archive_detail::fail(error,"Invalid V3D JSON member signature");
    auto json=r.string();
    if(!r.end()||json.find_first_not_of(" \t\r\n")==std::string::npos)return archive_detail::fail(error,"Truncated/empty V3D JSON member");
    out=std::move(json);
    error.clear();
    return true;
}
namespace archive_detail {
inline bool valid_gravity(const gravity_field& g) {
    return g.falloff<=1 && std::isfinite(g.radius_cm) && g.radius_cm>=1 && g.radius_cm<=1e8
        && std::isfinite(g.strength_cm_per_second_squared) && g.strength_cm_per_second_squared>=0
        && g.strength_cm_per_second_squared<=1e6 && finite_vector(g.local_center)
        && std::abs(g.local_center.x)<=1e8 && std::abs(g.local_center.y)<=1e8 && std::abs(g.local_center.z)<=1e8;
}
inline bool valid_model(const model_record& m,std::uint64_t end) {
    const std::string prefix="models/"+guid_digits(m.uuid)+"/";
    const bool subtype=(m.model_type==2 && !(m.entity_type && m.item_type))
        || ((m.model_type==1 || m.model_type==3) && !m.entity_type && !m.item_type);
    return valid_guid(m.uuid)&&m.asset_type==0&&m.model_type>=1&&m.model_type<=3&&m.entity_type<=3&&m.item_type<=3&&subtype
        &&!m.name.empty()&&!m.display_name.empty()&&m.name.size()<=4096&&m.display_name.size()<=4096
        && valid_gravity(m.gravity)&&valid_summary(m.summary)
        &&m.definition_range.name==prefix+"definition.dat"&&m.metadata_range.name==prefix+"metadata.dat"&&m.manifest_range.name==prefix+"manifest.dat"
        &&valid_range(m.definition_range,end,archive_max_directory_bytes)&&valid_range(m.metadata_range,end,archive_max_directory_bytes)
        &&valid_range(m.manifest_range,end,archive_max_directory_bytes);
}
inline bool valid_sound(const sound_record& s,std::uint64_t end) {
    const std::string prefix="sounds/"+guid_digits(s.uuid)+"/";
    return valid_guid(s.uuid)&&!s.name.empty()&&!s.display_name.empty()&&s.name.size()<=4096&&s.display_name.size()<=4096
        &&s.definition_range.name==prefix+"definition.json"&&s.wave_range.name==prefix+"audio.wav"
        &&valid_range(s.definition_range,end,archive_max_directory_bytes)&&valid_range(s.wave_range,end,UINT64_C(512)*1024*1024);
}
inline void write_model(detail::writer& w,const model_record& m) {
    write_guid(w,m.uuid);
    w.u8(m.asset_type);
    w.u8(m.model_type);
    w.u8(m.entity_type);
    w.u8(m.item_type);
    w.string(m.name,4096);
    w.string(m.display_name,4096);
    w.u8(m.gravity.enabled);
    w.u8(m.gravity.falloff);
    w.i32(m.gravity.priority);
    w.f64(m.gravity.radius_cm);
    w.f64(m.gravity.strength_cm_per_second_squared);
    w.vector(m.gravity.local_center);
    write_summary(w,m.summary);
    detail::write_range(w,m.definition_range);
    detail::write_range(w,m.metadata_range);
    detail::write_range(w,m.manifest_range);
}
inline bool read_model(detail::reader& r,model_record& m) {
    m.uuid=read_guid(r);
    m.asset_type=r.u8();
    m.model_type=r.u8();
    m.entity_type=r.u8();
    m.item_type=r.u8();
    m.name=r.string(4096);
    m.display_name=r.string(4096);
    const auto enabled=r.u8();
    m.gravity.enabled=enabled!=0;
    m.gravity.falloff=r.u8();
    m.gravity.priority=r.i32();
    m.gravity.radius_cm=r.f64();
    m.gravity.strength_cm_per_second_squared=r.f64();
    m.gravity.local_center=r.vector();
    m.summary=read_summary(r);
    m.definition_range=detail::read_range(r);
    m.metadata_range=detail::read_range(r);
    m.manifest_range=detail::read_range(r);
    return r.ok()&&enabled<=1;
}
inline std::vector<range> root_ranges(const archive_directory& d) {
    std::vector<range> ranges;
    ranges.reserve(1+d.script_ranges.size()+3*d.models.size()+2*d.sounds.size());
    ranges.push_back(d.config_range);
    ranges.insert(ranges.end(),d.script_ranges.begin(),d.script_ranges.end());
    for(const auto& m:d.models){ranges.push_back(m.definition_range);
    ranges.push_back(m.metadata_range);
    ranges.push_back(m.manifest_range);
    }
    for(const auto& s:d.sounds){ranges.push_back(s.definition_range);
    ranges.push_back(s.wave_range);
    }return ranges;
}
inline bool disjoint(std::vector<range> ranges) {
    std::sort(ranges.begin(),ranges.end(),[](const range& a,const range& b){return a.offset<b.offset;});
    for(std::size_t i=1;i<ranges.size();++i)if(ranges[i-1].stored_size>ranges[i].offset-ranges[i-1].offset)return false;
    return true;
}
} // namespace archive_detail
inline bool validate_directory(const archive_directory& d,const archive_header& h,std::string& error) {
    using namespace archive_detail;
    if(h.revision<4||h.revision>6||d.models.size()!=h.model_count||d.sounds.size()!=h.sound_count
        ||d.models.size()>100000||d.sounds.size()>100000||d.config_range.name!="config.json"
        ||!valid_range(d.config_range,h.directory_offset,archive_max_directory_bytes)
        ||d.script_ranges.size()!=(h.revision>=5?3u:0u))return fail(error,"Invalid V3D directory counts/config/scripts");
    for(std::size_t i=0;i<d.script_ranges.size();++i)if(d.script_ranges[i].name!=script_member_name(i)
        ||!valid_range(d.script_ranges[i],h.directory_offset,UINT64_C(2)*1024*1024+8))return fail(error,"Invalid V3D script range");
    std::set<guid> ids;
    for(const auto& m:d.models)if(!valid_model(m,h.directory_offset)||!ids.insert(m.uuid).second)return fail(error,"Invalid/duplicate V3D model row");
    for(const auto& s:d.sounds)if(!valid_sound(s,h.directory_offset)||!ids.insert(s.uuid).second)return fail(error,"Invalid/duplicate V3D sound row");
    if(!disjoint(root_ranges(d)))return fail(error,"Overlapping V3D root member ranges");
    error.clear();
    return true;
}
inline bool encode_directory(const archive_directory& d,std::uint64_t revision,bytes& out,std::string& error) {
    archive_header h;
    h.directory_offset=archive_max_bytes;
    h.revision=revision;
    if(d.models.size()>100000||d.sounds.size()>100000)return archive_detail::fail(error,"V3D directory counts exceed limits");
    h.model_count=static_cast<std::uint32_t>(d.models.size());
    h.sound_count=static_cast<std::uint32_t>(d.sounds.size());
    if(!validate_directory(d,h,error))return false;
    detail::writer w(out,archive_max_directory_bytes);
    detail::write_range(w,d.config_range);
    for(const auto& r:d.script_ranges)detail::write_range(w,r);
    w.count(d.models.size());
    for(const auto& m:d.models)archive_detail::write_model(w,m);
    w.count(d.sounds.size());
    for(const auto& s:d.sounds){archive_detail::write_guid(w,s.uuid);
    w.u8(1);
    w.u8(0);
    w.u16(0);
    w.string(s.name,4096);
    w.string(s.display_name,4096);
    detail::write_range(w,s.definition_range);
    detail::write_range(w,s.wave_range);
    }
    if(!w.ok())return archive_detail::fail(error,"V3D directory exceeded limits");
    error.clear();
    return true;
}
inline bool decode_directory(const std::uint8_t* data,std::size_t size,const archive_header& h,archive_directory& out,std::string& error) {
    if(!data || size>archive_max_directory_bytes || size!=h.directory_size)return archive_detail::fail(error,"V3D directory data/size mismatch");
    if(crc32(data,size)!=h.directory_crc)return archive_detail::fail(error,"V3D directory CRC mismatch");
    detail::reader r(data,size);
    archive_directory d;
    d.config_range=detail::read_range(r);
    if(h.revision>=5)for(unsigned i=0;i<3;++i)d.script_ranges.push_back(detail::read_range(r));
    const auto count=r.count(100000,246);
    if(!r.ok()||count!=h.model_count)return archive_detail::fail(error,"V3D directory model count mismatch");
    d.models.reserve(count);
    for(std::uint32_t i=0;i<count;++i){model_record m;
    if(!archive_detail::read_model(r,m))return archive_detail::fail(error,"Truncated V3D model directory row");
    d.models.push_back(std::move(m));
    }
    const auto sounds=r.count(100000,100);
    if(!r.ok()||sounds!=h.sound_count)return archive_detail::fail(error,"V3D directory sound count mismatch");
    d.sounds.reserve(sounds);
    for(std::uint32_t i=0;i<sounds;++i){sound_record s;
    s.uuid=archive_detail::read_guid(r);
    const auto type=r.u8();
    const auto a=r.u8();
    const auto b=r.u16();
    s.name=r.string(4096);
    s.display_name=r.string(4096);
    s.definition_range=detail::read_range(r);
    s.wave_range=detail::read_range(r);
    if(type!=1||a||b)return archive_detail::fail(error,"Invalid V3D sound directory row");
    d.sounds.push_back(std::move(s));
    }
    if(!r.end())return archive_detail::fail(error,"V3D directory truncated or has trailing bytes");
    if(!validate_directory(d,h,error))return false;
    out=std::move(d);
    return true;
}
// The codec callback must decode exactly dst_size bytes and reject trailing input.
// Supply zlib's uncompress2/inflate when consuming compressed simulator archives.
// Stored members require no callback or external library.
using decompressor = std::function<bool(const std::uint8_t* src,std::size_t src_size,std::uint8_t* dst,std::size_t dst_size,std::string& error)>;
using read_at = std::function<bool(std::uint64_t offset,std::uint8_t* dst,std::size_t size)>;
struct archive_limits {
    std::uint64_t directory_bytes=archive_max_directory_bytes;
    std::uint64_t member_bytes=max_member_bytes;
};
class archive_reader {
    read_at read_;
    decompressor decompress_;
    archive_limits limits_;
    archive_header header_;
    archive_directory directory_;
    std::uint64_t file_size_=0;
public:
    const archive_header& header() const { return header_;
     }
    const archive_directory& directory() const { return directory_;
     }
    bool is_open() const { return bool(read_);
     }
    void close() { read_={};
    decompress_={};
    header_={};
    directory_={};
    file_size_=0;
     }
    bool open(read_at source,std::uint64_t file_size,std::string& error,decompressor decode_zlib={},archive_limits limits={}) {
        close();
        if(!source)return archive_detail::fail(error,"V3D requires a read_at callback");
        std::uint8_t raw[archive_header_bytes];
        archive_header h;
        if(!source(0,raw,sizeof(raw)))return archive_detail::fail(error,"Cannot read V3D header");
        if(!decode_header(raw,sizeof(raw),file_size,h,error))return false;
        if(h.directory_size>limits.directory_bytes || h.directory_size>std::numeric_limits<std::size_t>::max())return archive_detail::fail(error,"V3D directory exceeds configured memory limit");
        bytes data(static_cast<std::size_t>(h.directory_size));
        if(!source(h.directory_offset,data.data(),data.size()))return archive_detail::fail(error,"Cannot read V3D directory");
        archive_directory d;
        if(!decode_directory(data.data(),data.size(),h,d,error))return false;
        read_=std::move(source);
        decompress_=std::move(decode_zlib);
        limits_=limits;
        header_=h;
        directory_=std::move(d);
        file_size_=file_size;
        error.clear();
        return true;
    }
    bool open_file(const std::string& path,std::string& error,decompressor decode_zlib={},archive_limits limits={}) {
        close();
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if(!input)return archive_detail::fail(error,"Cannot open V3D file");
        const auto end=input.tellg();
        if(end<0)return archive_detail::fail(error,"Cannot determine V3D file size");
        const auto size=static_cast<std::uint64_t>(end);
        input.close();
        // Independent handles make concurrent reads safe;
        // Detect file replacement by size.
        read_at source=[path,size](std::uint64_t offset,std::uint8_t* dst,std::size_t count){
            if(offset>size||count>size-offset||offset>std::uint64_t(std::numeric_limits<std::streamoff>::max())||count>std::size_t(std::numeric_limits<std::streamsize>::max()))return false;
            std::ifstream f(path,std::ios::binary|std::ios::ate);
            if(!f||f.tellg()!=static_cast<std::streamoff>(size))return false;
            f.seekg(static_cast<std::streamoff>(offset));
            if(!f)return false;
            if(count)f.read(reinterpret_cast<char*>(dst),static_cast<std::streamsize>(count));
            return bool(f);
        };
        return open(std::move(source),size,error,std::move(decode_zlib),limits);
    }
    const model_record* find_model(const guid& id) const {for(const auto& m:directory_.models)if(m.uuid==id)return &m;
    return nullptr;
    }
    const sound_record* find_sound(const guid& id) const {for(const auto& s:directory_.sounds)if(s.uuid==id)return &s;
    return nullptr;
    }
    bool read_member(const range& member,bytes& out,std::string& error) const {
        out.clear();
        if(!read_)return archive_detail::fail(error,"V3D archive is not open");
        const auto limit=std::min(limits_.member_bytes,max_member_bytes);
        if(!archive_detail::valid_range(member,header_.directory_offset,limit)||member.uncompressed_size>std::numeric_limits<std::size_t>::max())return archive_detail::fail(error,"Invalid V3D member or configured memory limit exceeded");
        bytes stored(static_cast<std::size_t>(member.stored_size));
        if(!read_(member.offset,stored.data(),stored.size()))return archive_detail::fail(error,"Cannot read V3D member");
        if(member.codec==0)out=std::move(stored);
        else {
            if(!decompress_)return archive_detail::fail(error,"V3D zlib member requires a decompressor callback");
            out.resize(static_cast<std::size_t>(member.uncompressed_size));
            if(!decompress_(stored.data(),stored.size(),out.data(),out.size(),error)){out.clear();
            if(error.empty())error="V3D zlib decompression failed";
            return false;
            }
        }
        if(crc32(out.data(),out.size())!=member.crc){out.clear();
        return archive_detail::fail(error,"V3D member CRC mismatch");
        }error.clear();
        return true;
    }
    template<class T> bool read(const range& member,T& out,std::string& error) const {
        bytes data;
        if(!read_member(member,data,error))return false;
        return decode(data.data(),data.size(),out,error);
    }
    bool read_json(const range& member,std::string& out,std::string& error) const {
        bytes data;
        if(!read_member(member,data,error))return false;
        return decode_json(data.data(),data.size(),out,error);
    }
    bool read_metadata(const model_record& model,model_metadata& out,std::string& error) const {
        model_metadata m;
        if(!read(model.metadata_range,m,error))return false;
        const auto& a=m.summary;
        const auto& b=model.summary;
        if(a.source_node_count!=b.source_node_count||a.source_mesh_count!=b.source_mesh_count||a.source_material_count!=b.source_material_count||a.source_texture_count!=b.source_texture_count
            ||a.center.x!=b.center.x||a.center.y!=b.center.y||a.center.z!=b.center.z||a.size.x!=b.size.x||a.size.y!=b.size.y||a.size.z!=b.size.z)return archive_detail::fail(error,"V3D metadata summary disagrees with directory");
        out=std::move(m);
        return true;
    }
    bool read_manifest(const model_record& model,manifest& out,std::string& error) const {
        manifest m;
        if(!read(model.manifest_range,m,error))return false;
        if(m.mesh_ranges.size()!=std::size_t(model.summary.source_mesh_count)||m.material_ranges.size()!=std::size_t(model.summary.source_material_count)||m.texture_ranges.size()!=std::size_t(model.summary.source_texture_count)||m.mesh_names.size()!=m.mesh_ranges.size()||m.skin_ranges.size()>65536)return archive_detail::fail(error,"V3D manifest counts disagree with summary");
        auto ranges=archive_detail::root_ranges(directory_);
        std::set<std::string> names;
        const std::string prefix="models/"+guid_digits(model.uuid)+"/";
        const auto validate_manifest_ranges=[&](const std::map<std::int32_t,range>& values,bool contiguous){
            std::int32_t expected=0;
            for(const auto& kv:values){const auto& r=kv.second;
                if(kv.first<0||(contiguous&&kv.first!=expected++)||r.name.compare(0,prefix.size(),prefix)!=0
                    ||!archive_detail::suffix(r.name,".dat")||!archive_detail::valid_range(r,header_.directory_offset,max_member_bytes)||!names.insert(r.name).second)return false;
                ranges.push_back(r);
            }return true;
        };
        if(!validate_manifest_ranges(m.mesh_ranges,true)
            ||!validate_manifest_ranges(m.skin_ranges,false)
            ||!validate_manifest_ranges(m.material_ranges,true)
            ||!validate_manifest_ranges(m.texture_ranges,true)
            ||!archive_detail::disjoint(std::move(ranges)))return archive_detail::fail(error,"Invalid/overlapping V3D manifest ranges");
        for(const auto& kv:m.mesh_names)if(kv.second.empty()||m.mesh_ranges.find(kv.first)==m.mesh_ranges.end())return archive_detail::fail(error,"Invalid V3D manifest mesh name");
        out=std::move(m);
        return true;
    }
};
// Streaming writer for portable files. Add encoded members, build a manifest and
// directory from the returned ranges, then finish(). Writes stored members so the
// result is readable without any third-party decompressor. The caller should use
// a temporary path and rename only after finish() and its own verification succeed.
class archive_writer {
    std::ofstream file_;
    std::uint64_t offset_=0;
    bool finished_=false;
    std::map<std::string,range> written_;
    bool pad(std::string& error) {
        const std::uint64_t aligned=(offset_+archive_alignment-1)/archive_alignment*archive_alignment;
        if(aligned>archive_max_bytes)return archive_detail::fail(error,"V3D archive size limit exceeded");
        const std::size_t count=static_cast<std::size_t>(aligned-offset_);
        std::uint8_t zeros[archive_alignment]={};
        if(count)file_.write(reinterpret_cast<const char*>(zeros),static_cast<std::streamsize>(count));
        offset_=aligned;
        return bool(file_)||archive_detail::fail(error,"Cannot write V3D padding");
    }
public:
    bool open_file(const std::string& path,std::string& error) {
        file_.close();
        file_.clear();
        written_.clear();
        offset_=0;
        finished_=false;
        file_.open(path,std::ios::binary|std::ios::trunc);
        if(!file_)return archive_detail::fail(error,"Cannot create V3D file");
        const char zeros[archive_header_bytes]={};
        file_.write(zeros,sizeof(zeros));
        offset_=archive_header_bytes;
        if(!file_)return archive_detail::fail(error,"Cannot reserve V3D header");
        error.clear();
        return true;
    }
    bool add_member(const std::string& name,const bytes& data,range& out,std::string& error) {
        if(!file_||finished_||!archive_detail::safe_member_name(name)||data.empty()||data.size()>max_member_bytes
            ||written_.find(name)!=written_.end()||!(archive_detail::suffix(name,".dat")||archive_detail::suffix(name,".json")||archive_detail::suffix(name,".wav")))return archive_detail::fail(error,"Invalid/duplicate V3D member or inactive writer");
        if(!pad(error))return false;
        if(data.size()>archive_max_bytes-offset_)return archive_detail::fail(error,"V3D archive size limit exceeded");
        range member;
        member.name=name;
        member.offset=offset_;
        member.stored_size=member.uncompressed_size=data.size();
        member.crc=crc32(data.data(),data.size());
        member.codec=0;
        file_.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
        if(!file_)return archive_detail::fail(error,"Cannot write V3D member");
        offset_+=data.size();
        written_.emplace(name,member);
        out=std::move(member);
        error.clear();
        return true;
    }
    template<class T> bool add(const std::string& name,const T& value,range& out,std::string& error) {
        bytes data;
        if(!encode(value,data,error))return false;
        return add_member(name,data,out,error);
    }
    bool add_json(const std::string& name,const std::string& json,range& out,std::string& error) {
        bytes data;
        if(!encode_json(json,data,error))return false;
        return add_member(name,data,out,error);
    }
    bool finish(const archive_directory& directory,const guid& build_id,std::string& error) {
        if(!file_||finished_)return archive_detail::fail(error,"V3D writer is not active");
        if(!pad(error))return false;
        archive_header h;
        h.directory_offset=offset_;
        h.build_id=build_id;
        if(directory.models.size()>100000||directory.sounds.size()>100000)return archive_detail::fail(error,"V3D directory counts exceed limits");
        h.model_count=static_cast<std::uint32_t>(directory.models.size());
        h.sound_count=static_cast<std::uint32_t>(directory.sounds.size());
        if(!validate_directory(directory,h,error))return false;
        for(const auto& r:archive_detail::root_ranges(directory)) {
            const auto it=written_.find(r.name);
            if(it==written_.end())return archive_detail::fail(error,"V3D directory references unwritten member");
            const auto& v=it->second;
            if(r.offset!=v.offset||r.stored_size!=v.stored_size||r.uncompressed_size!=v.uncompressed_size||r.crc!=v.crc||r.codec!=v.codec)return archive_detail::fail(error,"V3D directory range differs from written member");
        }
        bytes data;
        if(!encode_directory(directory,archive_revision,data,error))return false;
        h.directory_size=data.size();
        h.directory_crc=crc32(data.data(),data.size());
        bytes header;
        if(!encode_header(h,header,error))return false;
        file_.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
        file_.seekp(0);
        file_.write(reinterpret_cast<const char*>(header.data()),static_cast<std::streamsize>(header.size()));
        file_.flush();
        if(!file_)return archive_detail::fail(error,"Cannot finalize V3D file");
        file_.close();
        if(file_.fail())return archive_detail::fail(error,"Cannot close V3D file");
        finished_=true;
        error.clear();
        return true;
    }
};

} // namespace v3d
#endif // V3D_HPP_INCLUDED
