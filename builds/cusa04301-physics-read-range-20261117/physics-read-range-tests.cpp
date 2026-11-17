#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "e165_read_range.h"
using u32=std::uint32_t;using u64=std::uint64_t;using VAddr=u64;
namespace boost::container {template<class T,size_t N> using static_vector=std::vector<T>;}
namespace Common {struct ElfInfo {std::string serial="CUSA04301";static ElfInfo& Instance(){static ElfInfo e;return e;}const auto& GameSerial(){return serial;}};}
namespace Shader {enum class LogicalStage {Vertex,Fragment,Compute};enum class BufferType {Ordinary,Flatbuf};
struct Info {u64 pgm_hash{};bool uses_dma{},uses_ordered_count{};LogicalStage l_stage{};
std::vector<u32> user_data,flattened_ud_buf;struct Mask{u32 mask{};}ud_mask;
struct Resource{BufferType buffer_type{};bool is_written{};};std::vector<Resource> buffers;};}
namespace Vulkan {
struct DreamsDynamicReadRange {
    VAddr address;
    u32 size;
};
using DreamsDynamicReads = boost::container::static_vector<DreamsDynamicReadRange, 3>;

// Complete physical-address read sets of these shader versions, derived from their
// compiled ReadConst expressions. Keep the 32-bit offset wrap used by the shaders.
// A missing input or an address outside the guest GPU range retains the DMA fallback.
static std::optional<DreamsDynamicReads> ResolveDreamsDynamicReads(
    const Shader::Info& shader, std::span<const u32> code) {
    if (!shader.uses_dma || Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return {};
    }
    const auto& flat = shader.flattened_ud_buf;
    const auto ud = shader.user_data;
    // Runtime permutations can replace vertex user data with per-draw inputs;
    // full DMA mode can also turn formerly flattened reads into physical reads.
    // Accept only the complete, audited compiled modules. Others retain the
    // mapped-buffer path. No cached raw module pointer survives cache replacement.
    if (shader.pgm_hash == 0xe165304b) {
        // Complete physical read set of the two captured compute permutations.
        // Keep every other DMA shader/version on its existing route.
        if (shader.l_stage != Shader::LogicalStage::Compute ||
            shader.uses_ordered_count || shader.ud_mask.mask != 3 || shader.buffers.size() != 11 ||
            shader.buffers[8].buffer_type != Shader::BufferType::Flatbuf ||
            shader.buffers[8].is_written) {
            return {};
        }
        const bool module_matches =
            (code.size_bytes() == 90340 &&
             XXH3_64bits(code.data(), code.size_bytes()) == 0xb556d23dd3fe9845ULL) ||
            (code.size_bytes() == 90612 &&
             XXH3_64bits(code.data(), code.size_bytes()) == 0x8fc011cf8a62c29bULL);
        if (!module_matches) {
            return {};
        }
        const auto range = DreamsE165Reads::Bound(ud, flat);
        if (!range) {
            return {};
        }
        DreamsDynamicReads reads;
        if (range->size) {
            reads.push_back({range->address, range->size});
        }
        return reads;
    }
    u64 expected_hash{};
    size_t expected_bytes{};
    switch (shader.pgm_hash) {
    case 0x9e9c1c71: expected_hash = 0xcbe6aae3b2230cfeULL; expected_bytes = 13044; break;
    case 0xdcc325c2: expected_hash = 0x3982fdf0d9f4ee4fULL; expected_bytes = 116240; break;
    case 0xfffa32b6: expected_hash = 0xb04b52f8a8e23c2eULL; expected_bytes = 282672; break;
    default: return {};
    }
    if (code.size_bytes() != expected_bytes ||
        XXH3_64bits(code.data(), code.size_bytes()) != expected_hash) {
        return {};
    }
    DreamsDynamicReads ranges;
    const auto append = [&](u32 low, u32 high, u32 offset, u32 size) {
        constexpr u64 limit = u64{1} << 40;
        const u64 base = u64{low} | (u64{high} << 32);
        if (base == 0 || base >= limit || offset > limit - base ||
            size > limit - base - offset || (base & 3) != 0) {
            return false;
        }
        ranges.push_back({base + offset, size});
        return true;
    };
    switch (shader.pgm_hash) {
    case 0x9e9c1c71:
        if (shader.l_stage != Shader::LogicalStage::Vertex || ud.size() < 3 || flat.size() < 22 ||
            !append(flat[20], flat[21], ud[2] * u32{192} + u32{128}, 64)) {
            return {};
        }
        break;
    case 0xdcc325c2:
        if (shader.l_stage != Shader::LogicalStage::Fragment || ud.size() < 5 ||
            !append(ud[0], ud[1], ud[4] << 4, 16)) {
            return {};
        }
        break;
    case 0xfffa32b6:
        if (shader.l_stage != Shader::LogicalStage::Fragment || ud.size() < 3 || flat.size() < 169 ||
            !append(ud[0], ud[1], (ud[2] << 4) + u32{192}, 16) ||
            !append(flat[167], flat[168], ((ud[2] & 1) << 4) + u32{2304}, 12) ||
            !append(flat[16], flat[17], (ud[2] << 4) + u32{256}, 16)) {
            return {};
        }
        break;
    default:
        return {};
    }
    return ranges;
}

}
using namespace Vulkan;
static std::map<u64,std::vector<u32>> original_modules;
static void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static std::vector<u64> original_addresses(u64 shader, const std::vector<u32>& ud,
        const std::vector<u32>& flat) { switch(shader) {
case 0x9e9c1c71: return {(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(176u))) >> u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(176u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(176u))) >> u32(2u))) + u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(176u))) >> u32(2u))) + u32(3u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(128u))) >> u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(128u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(128u))) >> u32(2u))) + u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(128u))) >> u32(2u))) + u32(3u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(144u))) >> u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(144u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(144u))) >> u32(2u))) + u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(144u))) >> u32(2u))) + u32(3u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(160u))) >> u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(160u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(160u))) >> u32(2u))) + u32(2u))) << 2),(u64(flat[20u]) | (u64(flat[21u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) * u32(192u))) + u32(160u))) >> u32(2u))) + u32(3u))) << 2)};
case 0xdcc325c2: return {(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32(ud[4]) << u32(4u))) + u32(8u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(ud[4]) << u32(4u))) + u32(8u))) >> u32(2u))) + u32(1u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32(ud[4]) << u32(4u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32(ud[4]) << u32(4u))) >> u32(2u))) + u32(1u))) << 2)};
case 0xfffa32b6: return {(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(200u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(200u))) >> u32(2u))) + u32(1u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(192u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(192u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[167u]) | (u64(flat[168u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) & u32(1u))) << u32(4u))) + u32(2304u))) >> u32(2u))) << 2),(u64(flat[167u]) | (u64(flat[168u]) << 32)) + u32(u32((u32((u32((u32((u32((u32(ud[2]) & u32(1u))) << u32(4u))) + u32(2304u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[167u]) | (u64(flat[168u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) & u32(1u))) << u32(4u))) + u32(2312u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(200u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(200u))) >> u32(2u))) + u32(1u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(192u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(192u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[16u]) | (u64(flat[17u]) << 32)) + u32(u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(264u))) >> u32(2u))) << 2),(u64(flat[16u]) | (u64(flat[17u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(264u))) >> u32(2u))) + u32(1u))) << 2),(u64(flat[16u]) | (u64(flat[17u]) << 32)) + u32(u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(256u))) >> u32(2u))) << 2),(u64(flat[16u]) | (u64(flat[17u]) << 32)) + u32(u32((u32((u32((u32((u32(ud[2]) << u32(4u))) + u32(256u))) >> u32(2u))) + u32(1u))) << 2)};default: throw std::runtime_error("unexpected shader"); }}
static unsigned test_ranges() {
    std::mt19937 random(0x875a4591); unsigned cases=0;
    for(u64 hash:{0x9e9c1c71ull,0xdcc325c2ull,0xfffa32b6ull}) {
        const auto& code=original_modules.at(hash);
        Shader::Info shader;shader.pgm_hash=hash;shader.uses_dma=true;
        shader.l_stage=hash==0x9e9c1c71?Shader::LogicalStage::Vertex:Shader::LogicalStage::Fragment;
        shader.user_data.resize(16);shader.flattened_ud_buf.resize(256);
        const std::array<u32,10> edges={0,1,2,0x55555555,0x15555554,0x15555555,0xffffffef,0xfffffff0,0xfffffffe,0xffffffff};
        for(unsigned i=0;i<10010;i++) {
            auto& ud=shader.user_data;auto& flat=shader.flattened_ud_buf;
            for(auto& x:ud)x=random();for(auto& x:flat)x=random();
            ud[0]=random()&~u32(3);ud[1]=1+random()%200;
            for(unsigned at:{16,20,167}){flat[at]=random()&~u32(3);flat[at+1]=1+random()%200;}
            if(i<edges.size())ud[2]=ud[4]=edges[i];
            auto ranges=ResolveDreamsDynamicReads(shader,code);require(ranges.has_value(),"valid read rejected");
            const auto original=original_addresses(hash,ud,flat);
            for(auto addr:original)require(std::any_of(ranges->begin(),ranges->end(),[&](auto r){return r.address<=addr&&addr+4<=r.address+r.size;}),"original GPU read missing");
            for(auto r:*ranges)for(u64 a=r.address;a<r.address+r.size;a+=4)
                require(std::find(original.begin(),original.end(),a)!=original.end(),"extra unproven dword");
            ++cases;
        }
        // Invalid or unsupported inputs must retain the original mapped-DMA path.
        for(unsigned mode=0;mode<9;mode++) {
            auto bad=shader;
            if(mode==0)bad.uses_dma=false;
            if(mode==1)bad.pgm_hash^=1;
            if(mode==2)bad.l_stage=Shader::LogicalStage::Compute;
            if(mode==3)bad.user_data.resize(2);
            if(mode==4){bad.user_data[1]=0x100;bad.flattened_ud_buf[21]=0x100;}
            if(mode==5){bad.user_data[0]=1;bad.flattened_ud_buf[20]=1;}
            if(mode==6){bad.user_data[0]=bad.user_data[1]=bad.flattened_ud_buf[20]=bad.flattened_ud_buf[21]=0;}
            if(mode==7){Common::ElfInfo::Instance().serial="OTHER";}
            if(mode==8){bad.user_data[0]=0xfffffffcu;bad.user_data[1]=255;bad.user_data[2]=bad.user_data[4]=1;bad.flattened_ud_buf[20]=0xfffffffcu;bad.flattened_ud_buf[21]=255;}
            require(!ResolveDreamsDynamicReads(bad,code),"invalid range did not fall back");
            Common::ElfInfo::Instance().serial="CUSA04301";++cases;
        }
        if(hash!=0xdcc325c2){auto bad=shader;bad.flattened_ud_buf.resize(hash==0x9e9c1c71?21:168);require(!ResolveDreamsDynamicReads(bad,code),"short flat accepted");++cases;}
        require(!ResolveDreamsDynamicReads(shader,{}),"missing module accepted");++cases;
        auto changed_code=code;changed_code.back()^=1;
        require(!ResolveDreamsDynamicReads(shader,changed_code),"different module accepted");++cases;
    }
    std::cout<<"RANGE_CASES "<<cases<<'\n';return cases;
}

static std::array<u64,4> original_e165(const std::vector<u32>& ud,u32 i) {return {(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32(i) << u32(4u))) + u32(240u))) >> u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(i) << u32(4u))) + u32(240u))) >> u32(2u))) + u32(1u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(i) << u32(4u))) + u32(240u))) >> u32(2u))) + u32(2u))) << 2),(u64(ud[0]) | (u64(ud[1]) << 32)) + u32(u32((u32((u32((u32((u32(i) << u32(4u))) + u32(240u))) >> u32(2u))) + u32(3u))) << 2)};}
int main(int argc,char**argv){try{
 require(argc==6,"five real shader modules required");
 const std::array<u64,5> ids{0x9e9c1c71,0xdcc325c2,0xfffa32b6,0xe165304b,0xe165304b};
 std::vector<std::vector<u32>> codes;
 for(int x=1;x<argc;++x){std::ifstream f(argv[x],std::ios::binary|std::ios::ate);require(bool(f),"module open");
 auto n=size_t(f.tellg());require(n%4==0,"module alignment");std::vector<u32> code(n/4);f.seekg(0);f.read(reinterpret_cast<char*>(code.data()),n);
 require(bool(f),"module read");codes.push_back(code);original_modules[ids[x-1]]=std::move(code);}
 unsigned checks=test_ranges();
 Shader::Info shader;shader.pgm_hash=0xe165304b;shader.uses_dma=true;shader.l_stage=Shader::LogicalStage::Compute;
 shader.ud_mask.mask=3;shader.buffers.resize(11);shader.buffers[8].buffer_type=Shader::BufferType::Flatbuf;
 shader.user_data={0x100000,2};shader.flattened_ud_buf.resize(128);
 std::mt19937 random(0x5c1ae165);
 const std::array<u32,14> edges{0,1,2,3,63,64,65,65536,65537,0xffffff0,0xffffff1,0xffffff2,0x10000000,0xffffffff};
 for(unsigned permutation=3;permutation<5;++permutation){const auto& code=codes[permutation];
  for(unsigned c=0;c<12000;++c){auto count=c<edges.size()?edges[c]:random()%65537;
   shader.flattened_ud_buf[80]=count;auto result=ResolveDreamsDynamicReads(shader,code);
   bool accepted=count<=65536;require(result.has_value()==accepted,"count rejection mismatch");++checks;
   if(!accepted)continue;
   if(!count){require(result->empty(),"zero loop read");++checks;continue;}
   require(result->size()==1,"one table range required");auto r=(*result)[0];
   for(u32 i:std::array<u32,3>{0,count-1,u32(random()%count)}){
    for(auto address:original_e165(shader.user_data,i)){require(r.address<=address && address+4<=r.address+r.size,"real instruction outside range");++checks;}}
   // At both boundaries, the bound must include exactly the first/last record.
   require(original_e165(shader.user_data,0)[0]==r.address,"first read");
   require(original_e165(shader.user_data,count-1)[3]+4==r.address+r.size,"last read");checks+=2;
  }
  shader.flattened_ud_buf[80]=4;
  for(unsigned mode=0;mode<13;++mode){auto bad=shader;
   if(mode==0)bad.uses_dma=false;if(mode==1)bad.pgm_hash^=1;
   if(mode==2)bad.l_stage=Shader::LogicalStage::Fragment;if(mode==3)bad.uses_ordered_count=true;
   if(mode==4)bad.ud_mask.mask=6;if(mode==5)bad.buffers.resize(10);
   if(mode==6)bad.buffers[8].buffer_type=Shader::BufferType::Ordinary;
   if(mode==7)bad.buffers[8].is_written=true;if(mode==8)bad.user_data.resize(1);
   if(mode==9)bad.flattened_ud_buf.resize(80);if(mode==10)bad.user_data[0]|=1;
   if(mode==11)bad.user_data={0,0};if(mode==12)bad.user_data[1]=256;
   require(!ResolveDreamsDynamicReads(bad,code),"uncertain input accepted");++checks;}
  Common::ElfInfo::Instance().serial="OTHER";require(!ResolveDreamsDynamicReads(shader,code),"wrong game");++checks;
  Common::ElfInfo::Instance().serial="CUSA04301";
  auto changed=code;changed.back()^=1;require(!ResolveDreamsDynamicReads(shader,changed),"wrong compiled module");++checks;
  for(u32 c:{1,2,16,65536}){shader.flattened_ud_buf[80]=c;u64 size=u64(c)*16;
   u64 last=(u64{1}<<40)-240-size;shader.user_data={u32(last),u32(last>>32)};
   require(bool(ResolveDreamsDynamicReads(shader,code)),"last valid address");++checks;
   last+=4;shader.user_data={u32(last),u32(last>>32)};require(!ResolveDreamsDynamicReads(shader,code),"address overflow");++checks;}
  shader.user_data={0x100000,2};
 }
 std::cout<<"PASS "<<checks<<" address/guard checks; two real compute permutations and three unchanged graphics resolvers\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
