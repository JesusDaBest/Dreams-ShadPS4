// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <variant>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct IndirectDrawParameters {
    u32 base_vertex_location{};
    u32 start_instance_location{};
};

struct ProgramCacheKey {
    u64 pgm_hash{};
    Shader::Stage stage{};
    s8 indirect_base_vertex_sgpr{-1};
    s8 indirect_start_instance_sgpr{-1};
    bool is_indirect_vertex{};

    ProgramCacheKey() = default;
    explicit ProgramCacheKey(u64 pgm_hash_) : pgm_hash{pgm_hash_} {}
    ProgramCacheKey(u64 pgm_hash_, Shader::Stage stage_, Shader::LogicalStage l_stage,
                    const Shader::RuntimeInfo& runtime_info);

    [[nodiscard]] u64 CacheHash() const noexcept;

    bool operator==(const ProgramCacheKey&) const noexcept = default;
};

struct ProgramCacheKeyHash {
    size_t operator()(const ProgramCacheKey& key) const noexcept {
        return static_cast<size_t>(key.CacheHash());
    }
};

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ProgramCacheKey cache_key{};
    ModuleList modules{};

    Program() = default;
    Program(Shader::Stage stage, Shader::LogicalStage l_stage, Shader::ShaderParams params,
            ProgramCacheKey cache_key_)
        : info{stage, l_stage, params}, cache_key{cache_key_} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline(
        const IndirectDrawParameters* indirect_draw = nullptr);

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(Shader::Stage stage, Shader::LogicalStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding,
                      const IndirectDrawParameters* indirect_draw = nullptr);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::Stage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

    /// Returns the exact effective SPIR-V words backing the currently selected logical stage.
    /// The span remains valid until that shader module is replaced or the cache is destroyed.
    std::span<const u32> GetCurrentShaderSpirv(Shader::LogicalStage stage) const;

    /// Returns the driver's opaque cache blob for the live Vulkan pipeline cache.
    /// An empty vector means the driver rejected the query.
    std::vector<u8> GetVulkanPipelineCacheData() const;

private:
    static std::string GetShaderName(Shader::Stage stage, const ProgramCacheKey& program_key,
                                     std::optional<size_t> perm = {});
    bool RefreshGraphicsKey(const IndirectDrawParameters* indirect_draw);
    bool RefreshGraphicsStages(const IndirectDrawParameters* indirect_draw);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage, size_t perm_idx,
                    std::string_view ext);
    void DumpShader(std::span<const u32> code, const ProgramCacheKey& program_key,
                    Shader::Stage stage, size_t perm_idx, std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(const ProgramCacheKey& program_key,
                                                   Shader::Stage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code,
                                   const std::span<const u32>& code_data,
                                   const ProgramCacheKey& program_key, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(
        Shader::Stage stage, Shader::LogicalStage l_stage,
        const IndirectDrawParameters* indirect_draw = nullptr);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<ProgramCacheKey, std::unique_ptr<Program>, ProgramCacheKeyHash> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    tsl::robin_map<vk::ShaderModule, std::vector<u32>> effective_spirv;
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
