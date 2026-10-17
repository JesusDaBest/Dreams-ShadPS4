#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include RESOURCE_PASS_INCLUDE

namespace Common::Log { std::unordered_map<std::string_view,std::shared_ptr<spdlog::logger>> ALL_LOGGERS; }
namespace Common { std::string GetCurrentThreadName() { return "occlusion-sample-audit"; } }
void assert_fail_impl() { throw std::runtime_error("production assertion"); }
[[noreturn]] void unreachable_impl() { throw std::runtime_error("production unreachable"); }
std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    throw std::runtime_error("offline test requested emulator settings");
}
namespace Common::FS {
IOFile::IOFile(const std::filesystem::path&,FileAccessMode,FileType,FileShareFlag) {
    throw std::runtime_error("offline test requested profile file access");
}
IOFile::~IOFile() {}
const std::filesystem::path& GetUserPath(PathType) {
    throw std::runtime_error("offline test requested a profile path");
}
}

using namespace Shader;
static void check(bool value,const char* text) { if(!value)throw std::runtime_error(text); }
static float constant_float(IR::Value value) {
    value=value.Resolve();
    if(value.IsImmediate())return value.F32();
    auto* inst=value.InstRecursive();
    if(inst->GetOpcode()==IR::Opcode::FPClamp32)
        return std::clamp(constant_float(inst->Arg(0)),constant_float(inst->Arg(1)),constant_float(inst->Arg(2)));
    throw std::runtime_error("Unexpected constant mip expression");
}
static u32 constant_uint(IR::Value value) {
    value=value.Resolve();
    if(value.IsImmediate())return value.U32();
    auto* inst=value.InstRecursive();
    if(inst->GetOpcode()==IR::Opcode::ConvertU32F32)return u32(constant_float(inst->Arg(0)));
    throw std::runtime_error("Unexpected dimension mip expression");
}
static bool depends_on(IR::Value value,IR::Value needle,u32 depth=0) {
    value=value.Resolve();needle=needle.Resolve();
    if(value==needle)return true;
    if(value.IsImmediate() || depth>32)return false;
    auto* inst=value.InstRecursive();
    for(size_t i=0;i<inst->NumArgs();++i)
        if(inst->Arg(i).IsImmediate()==false && inst->Arg(i).TryInstRecursive() && depends_on(inst->Arg(i),needle,depth+1))return true;
    return false;
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"usage: test captured-culling-srt.bin");
        auto logger=std::make_shared<spdlog::logger>("audit",std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
        Common::Log::ALL_LOGGERS[Common::Log::Class::Debug]=logger;
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        check(bool(file),"Missing captured SRT");
        std::vector<u32> words(size_t(file.tellg())/4);
        file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),words.size()*4);
        u32 cases{};
        for (bool dreams : {false,true}) for (bool array_binding : {false,true}) for(bool declared_array : {false,true})
            for(bool normalized : {false,true}) for(u32 mip : {0u,1u,3u,8u}) {
                Info info{};info.pgm_hash=dreams ? DreamsCompat::TraversalShader : 0x12345678;info.flattened_ud_buf=words;
                auto image=info.ReadUdSharp<AmdGpu::Image>(84);
                image.type=u32(array_binding ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D);
                info.images.push_back({.sharp_idx=84,.is_sampled=true,.is_array=declared_array});
                std::memcpy(info.flattened_ud_buf.data()+84,&image,sizeof(image));
                info.samplers.push_back({.sharp_idx=138});
                Common::ObjectPool<IR::Inst> pool{1024};IR::Block block{pool};IR::IREmitter ir{block};
                const auto lod=ir.Imm32(float(mip));
                const auto body=declared_array ? ir.CompositeConstruct(ir.Imm32(80.f),ir.Imm32(24.f),ir.Imm32(0.f),lod)
                                              : ir.CompositeConstruct(ir.Imm32(80.f),ir.Imm32(24.f),lod,ir.Imm32(77.f));
                IR::TextureInstInfo flags{};flags.has_lod.Assign(true);flags.is_array.Assign(declared_array);flags.is_unnormalized.Assign(!normalized);
                const auto raw=ir.ImageSampleRaw(ir.Imm32(0u),ir.Imm32(0u),body,body,body,ir.Imm32(0.f),flags);
                ir.SetVectorReg(IR::VectorReg{0},IR::F32{ir.CompositeExtract(raw,0)});
                Profile profile{};
                Optimization::PatchImageSampleArgs(block,*raw.Inst(),info,profile,info.images[0],image);
                IR::BlockList blocks{&block};
                Optimization::ConstantPropagationPass(blocks);
                Optimization::IdentityRemovalPass(blocks);
                IR::Inst* sample{};IR::Inst* dimensions{};
                for(auto& inst:block) {
                    if(inst.GetOpcode()==IR::Opcode::ImageSampleExplicitLod)sample=&inst;
                    if(inst.GetOpcode()==IR::Opcode::ImageQueryDimensions)dimensions=&inst;
                }
                check(sample,"No lowered sample");
                const auto result_lod=sample->Arg(2).Resolve();
                const u32 expected=(!dreams && !array_binding && declared_array) ? 0 : mip;
                std::cout<<"dreams="<<dreams<<" array_binding="<<array_binding<<" declared_array="<<declared_array<<" normalized="<<normalized<<" supplied_mip="<<mip<<" actual_mip=";
                if(result_lod.IsImmediate())std::cout<<result_lod.F32();else std::cout<<"dynamic";
                std::cout<<'\n';
                check(result_lod.IsImmediate() && result_lod.F32()==float(expected),"LOD was read from the wrong address component");
                if(!normalized && !array_binding && declared_array) {
                    check(dimensions,"No dimensions for unnormalized coordinates");
                    auto query=dimensions->Arg(1).Resolve();
                    check(constant_uint(query)==expected,"Unnormalized coordinates use the wrong mip dimensions");
                }
                ++cases;
            }
        {
            Info info{};info.pgm_hash=DreamsCompat::TraversalShader;info.flattened_ud_buf=words;
            auto image=info.ReadUdSharp<AmdGpu::Image>(84);
            check(image.GetType()==AmdGpu::ImageType::Color2D,"Captured map is not plain 2D");
            info.images.push_back({.sharp_idx=84,.is_sampled=true,.is_array=true});
            info.samplers.push_back({.sharp_idx=138});
            Common::ObjectPool<IR::Inst> pool{1024};IR::Block block{pool};IR::IREmitter ir{block};
            const auto mip=ir.GetVectorReg<IR::F32>(IR::VectorReg{4});
            const auto body=ir.CompositeConstruct(ir.Imm32(80.f),ir.Imm32(24.f),ir.Imm32(0.f),mip);
            IR::TextureInstInfo flags{};flags.has_lod.Assign(true);flags.is_array.Assign(true);flags.is_unnormalized.Assign(true);
            const auto raw=ir.ImageSampleRaw(ir.Imm32(0u),ir.Imm32(0u),body,body,body,ir.Imm32(0.f),flags);
            ir.SetVectorReg(IR::VectorReg{0},IR::F32{ir.CompositeExtract(raw,0)});
            Profile profile{};Optimization::PatchImageSampleArgs(block,*raw.Inst(),info,profile,info.images[0],image);
            bool correct_sample{},correct_dimensions{};
            for(auto& inst:block) {
                if(inst.GetOpcode()==IR::Opcode::ImageSampleExplicitLod)correct_sample=inst.Arg(2).Resolve()==mip.Resolve();
                if(inst.GetOpcode()==IR::Opcode::ImageQueryDimensions)correct_dimensions=depends_on(inst.Arg(1),mip);
            }
            check(correct_sample && correct_dimensions,"Dynamic mip does not drive both sample and normalization");
            ++cases;
        }
        std::cout<<"PASS "<<cases<<" captured-descriptor lowering cases, including dynamic mip\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
