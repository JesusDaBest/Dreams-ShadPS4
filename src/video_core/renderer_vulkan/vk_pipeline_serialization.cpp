// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/dreams_compat.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// Liverpool DS_ORDERED_COUNT supplies its ordered-counter base in M0's high half in bytes, and
// OFFSET0 is byte encoded too. Version 17 incorrectly treated the M0 base as a dword index,
// aliasing Dreams compaction counters into unrelated guest GDS words.
static constexpr u32 ShaderBinaryVersion = 18u;
static constexpr u32 ShaderMetaVersion = 6u;
static constexpr u32 PreviousShaderMetaVersion = 5u;
static constexpr u32 PipelineKeyVersion = 3u;
static constexpr size_t PreviousRuntimeInfoSize = 248u;
static_assert(sizeof(Shader::RuntimeInfo) == 256u);
} // namespace Serialization

namespace Vulkan {
namespace {

std::string ShaderBinaryCacheKey(const Shader::Info& info,
                                 const ProgramCacheKey& program_key, size_t perm_idx) {
    const u64 pgm_hash = info.pgm_hash;
    // Vertex ID lowering changed independently of every compute/fragment shader. Keep that
    // invalidation stage-local so a vertex fix cannot force Dreams' expensive compute shaders to
    // rebuild, while also preventing binaries made by the incomplete lowering from being reused.
    if (info.l_stage == Shader::LogicalStage::Vertex) {
        if (program_key.is_indirect_vertex) {
            return fmt::format("{:#018x}_{}_drawparams4_indirect_{}_{}_{}", pgm_hash, perm_idx,
                               static_cast<u32>(program_key.stage),
                               static_cast<s32>(program_key.indirect_base_vertex_sgpr),
                               static_cast<s32>(program_key.indirect_start_instance_sgpr));
        }
        return fmt::format("{:#018x}_{}_drawparams4", pgm_hash, perm_idx);
    }
    if (pgm_hash == Shader::DreamsCompat::TraversalShader) {
        if (Shader::DreamsCompat::CaptureB535Membership()) {
            return fmt::format(
                "{:#018x}_{}_doc{}_membership_capture{}_schema{}{}", pgm_hash, perm_idx,
                Shader::DreamsCompat::TraversalCacheRevision,
                Shader::DreamsCompat::ForceB535FirstGateFailure()
                    ? "_force_first_gate_failure"
                    : "",
                Shader::DreamsCompat::B535MembershipCapture::Schema,
                Shader::DreamsCompat::CaptureB535InstructionTrace()
                    ? fmt::format("_microtrace{}",
                                  Shader::DreamsCompat::B535InstructionTraceCacheRevision)
                    : std::string{});
        }
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::TraversalCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::QueueProducerShader &&
        Shader::DreamsCompat::CaptureOrderedChain()) {
        return fmt::format("{:#018x}_{}_ordered_chain_capture{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::QueueProducerCapture::Schema);
    }
    if (pgm_hash == Shader::DreamsCompat::QueueProducerShaderAlt) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::QueueProducerAltCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::B1SeedWriterShader) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::B1SeedWriterCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::SceneCompactShader &&
        Shader::DreamsCompat::UseSceneCompactExactOrderedCountReplay()) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::SceneCompactCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::SculptVolumeWriterShader) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::SculptVolumeWriterCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::SculptSurfaceCompactShader) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::SculptSurfaceCompactCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::SculptSurfaceFinalizeShader) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::SculptSurfaceFinalizeCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::IndirectArgsShader) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::IndirectArgsCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::VisibilityCandidateCompactShader) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::VisibilityCandidateCompactCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::GatherInputCompactShader) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::GatherInputCompactCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::GatherVoxelsShader) {
        return fmt::format("{:#018x}_{}_lds{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::GatherVoxelsCacheRevision);
    }
    if (pgm_hash == Shader::DreamsCompat::VisibilityListCompactShader ||
        pgm_hash == Shader::DreamsCompat::VisibilityListCompactShaderAlt) {
        return fmt::format("{:#018x}_{}_doc{}", pgm_hash, perm_idx,
                           Shader::DreamsCompat::VisibilityListCompactCacheRevision);
    }
    return fmt::format("{:#018x}_{}", pgm_hash, perm_idx);
}

} // Anonymous namespace

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened() ||
        Shader::DreamsCompat::IsCaptureInstrumentedShader(info.pgm_hash)) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, const Shader::Info& info,
                          const ProgramCacheKey& program_key, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened() ||
        Shader::DreamsCompat::IsCaptureInstrumentedShader(info.pgm_hash)) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       ShaderBinaryCacheKey(info, program_key, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion &&
        meta_version != Serialization::PreviousShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    // Meta v5 stored RuntimeInfo before the indirect-draw specialization fields were appended.
    // Read precisely that stable prefix so existing compute/fragment caches remain usable.
    const size_t runtime_info_size =
        meta_version == Serialization::PreviousShaderMetaVersion
            ? Serialization::PreviousRuntimeInfoSize
            : sizeof(Shader::RuntimeInfo);
    spec.Deserialize(ar, runtime_info_size);
    info.Deserialize(ar);

    // Capture variants inject diagnostic writes into the translated shader. A normal cached
    // binary has the same guest program hash but none of those writes, so it must not satisfy a
    // capture-enabled load.
    if (Shader::DreamsCompat::IsCaptureInstrumentedShader(info.pgm_hash)) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *infos[0], modules[0], sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    // A stale pipeline can fail after loading only some stages. Never let those partial stage
    // arrays or fetch metadata leak into the next pipeline considered by cache warmup.
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader.reset();

    graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    if (Shader::DreamsCompat::CaptureVs370Interface() ||
        Shader::DreamsCompat::CaptureCe3ReadConst() ||
        Shader::DreamsCompat::CaptureCe3FleckTrace() ||
        Shader::DreamsCompat::CaptureCe3CoverageTrace()) {
        // Reject target-containing pipelines before LoadPipelineStage mutates the program cache,
        // stage arrays, or fetch-shader state. All unrelated cached graphics pipelines still warm
        // normally; the instrumented VS and its pipelines compile on demand.
        for (const auto hash : graphics_key.stage_hashes) {
            if (!hash) {
                continue;
            }
            std::vector<u8> meta_blob;
            Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                               fmt::format("{:#018x}", hash), meta_blob);
            if (meta_blob.empty()) {
                return false;
            }

            Serialization::Archive meta_ar{std::move(meta_blob)};
            Shader::Info info{};
            std::optional<Shader::Gcn::FetchShaderData> fetch_shader_data;
            Shader::StageSpecialization spec{};
            spec.info = &info;
            size_t perm_idx{};
            if (!LoadShaderMeta(meta_ar, info, fetch_shader_data, spec, perm_idx)) {
                return false;
            }
            const bool target_vs370 =
                Shader::DreamsCompat::CaptureVs370Interface() &&
                info.pgm_hash == Shader::DreamsCompat::Vs370InterfaceCaptureShader &&
                info.stage == Shader::Stage::Vertex &&
                info.l_stage == Shader::LogicalStage::Vertex;
            const bool target_ce3 =
                (Shader::DreamsCompat::CaptureCe3ReadConst() ||
                 Shader::DreamsCompat::CaptureCe3FleckTrace() ||
                 Shader::DreamsCompat::CaptureCe3CoverageTrace()) &&
                info.pgm_hash == Shader::DreamsCompat::Ce3ReadConstCaptureShader &&
                info.stage == Shader::Stage::Fragment &&
                info.l_stage == Shader::LogicalStage::Fragment;
            if (target_vs370 || target_ce3) {
                return false;
            }
        }
    }

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    ASSERT(is_new);

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
        runtime_infos, fetch_shader, modules, sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, fetch_shader, spec, perm_idx)) {
        return false;
    }

    const ProgramCacheKey program_key{program->info.pgm_hash, program->info.stage,
                                      program->info.l_stage, spec.runtime_info};
    program->cache_key = program_key;

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       ShaderBinaryCacheKey(program->info, program_key, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    auto [it_pgm, new_program] = program_cache.try_emplace(program_key);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    spec.info = &it_pgm.value()->info;
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);
    effective_spirv.insert_or_assign(module, spv);

    infos[stage] = &it_pgm.value()->info;
    modules[stage] = module;

    return true;
}

void PipelineCache::WarmUp() {
    if (Shader::DreamsCompat::CaptureB1WriterProvenance()) {
        LOG_INFO(Render, "Pipeline cache disabled while Dreams B1 writer provenance is active");
        return;
    }
    if (Shader::DreamsCompat::CaptureImageGather3D()) {
        LOG_INFO(Render, "Pipeline cache disabled while 3D ImageGather capture is active");
        return;
    }
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render,
                    "Pipeline cache profile has unexpected size ({} != {}). Ignoring the cache",
                    profile_data.size(), sizeof(Shader::Profile));
        Storage::DataBase::Instance().Close();
        return;
    }

    Shader::Profile cached_profile{};
    std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    if (cached_profile != profile) {
        LOG_WARNING(Render,
                    "Pipeline cache isn't compatible with current system. Ignoring the cache");
        Storage::DataBase::Instance().Close();
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            }
        });

    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    info.Write(runtime_ud_sharps);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);
    info.Read(runtime_ud_sharps);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar, size_t runtime_info_size) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    ASSERT(runtime_info_size <= sizeof(runtime_info));
    std::memset(&runtime_info, 0, sizeof(runtime_info));
    spec.Read(&runtime_info, runtime_info_size);
    if (runtime_info_size < sizeof(runtime_info)) {
        runtime_info.is_indirect_draw = false;
        runtime_info.indirect_base_vertex_sgpr = -1;
        runtime_info.indirect_start_instance_sgpr = -1;
    }

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
