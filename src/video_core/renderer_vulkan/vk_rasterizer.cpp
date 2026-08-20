// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "common/debug.h"
#include "common/elf_info.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/dreams_compat.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

// Raw guest buffers use UINT32_MAX to disable the PS4 bounds check. Mirroring the remaining
// multi-gigabyte guest VMA into one Vulkan buffer exceeds AMD's maxBufferSize and thrashes unified
// memory. Keep a practical window; robust buffer access safely handles addresses outside it.
static constexpr u64 MaxUnboundedGuestBufferWindow = 64_MB;
static constexpr u64 DreamsTraversalShader = Shader::DreamsCompat::TraversalShader;
static constexpr VAddr DreamsTraversalCompletionOffset =
    Shader::DreamsCompat::TraversalCompletionIndex * sizeof(u32);
static bool OrderDreamsIndirectTraversal() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_ORDER_INDIRECT");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled;
}

static u32 DreamsTraversalBatchSize() {
    static const u32 batch_size = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_ORDERED_COUNT_BATCH");
        if (value == nullptr || value[0] == '\0') {
            return 1U;
        }
        char* end{};
        const u64 parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<u32>(std::clamp<u64>(parsed, 1, 64)) : 1U;
    }();
    return batch_size;
}

static void DispatchDreamsTraversalOrdered(vk::CommandBuffer cmdbuf, u32 dim_x, u32 dim_y,
                                           u32 dim_z) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
    };
    const vk::DependencyInfo dependency{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };

    bool first = true;
    const u32 batch_size = DreamsTraversalBatchSize();
    for (u32 z = 0; z < dim_z; ++z) {
        for (u32 y = 0; y < dim_y; ++y) {
            for (u32 x = 0; x < dim_x; x += batch_size) {
                if (!first) {
                    cmdbuf.pipelineBarrier2(dependency);
                }
                cmdbuf.dispatchBase(x, y, z, std::min(batch_size, dim_x - x), 1, 1);
                first = false;
            }
        }
    }
}

struct DreamsBufferWriter {
    enum class Kind : u32 {
        Shader,
        Copy,
        Fill,
    };

    u64 sequence;
    u64 dispatch_sequence;
    u64 shader_hash;
    Shader::LogicalStage stage;
    Kind kind;
    u32 binding_index;
    bool declared_write;
    VAddr base;
    u64 size;
    u32 stride;
    VAddr source;
};

static std::deque<DreamsBufferWriter> g_dreams_buffer_writers;
static u64 g_dreams_buffer_writer_sequence{};

struct DreamsTrackedBufferRange {
    VAddr base{};
    u64 size{};

    bool operator==(const DreamsTrackedBufferRange&) const = default;
};

static std::array<DreamsTrackedBufferRange, 2> g_dreams_visibility_input_ranges{};
static std::vector<DreamsBufferWriter> g_dreams_visibility_input_writers;

struct DreamsCpuWriteWatch {
    std::atomic<u64> base{};
    std::atomic<u64> size{};
    std::atomic<u32> kind{};
    std::atomic<u64> events{};
    std::atomic<u64> bytes{};
    std::atomic<u64> last_addr{};
    std::atomic<u64> last_size{};
};

static constexpr u32 DreamsCpuWriteWatchCount = 16;
static std::array<DreamsCpuWriteWatch, DreamsCpuWriteWatchCount> g_dreams_cpu_write_watches{};
static std::atomic<bool> g_dreams_cpu_write_watches_active{};

static void RegisterDreamsCpuWriteWatch(VAddr base, u64 size, u32 kind) {
    if (base == 0 || size == 0) {
        return;
    }
    for (auto& watch : g_dreams_cpu_write_watches) {
        if (watch.base.load(std::memory_order_relaxed) == base) {
            watch.kind.store(kind, std::memory_order_relaxed);
            watch.size.store(size, std::memory_order_release);
            g_dreams_cpu_write_watches_active.store(true, std::memory_order_release);
            return;
        }
    }
    for (auto& watch : g_dreams_cpu_write_watches) {
        u64 expected{};
        if (!watch.base.compare_exchange_strong(expected, base, std::memory_order_relaxed)) {
            continue;
        }
        watch.kind.store(kind, std::memory_order_relaxed);
        watch.size.store(size, std::memory_order_release);
        g_dreams_cpu_write_watches_active.store(true, std::memory_order_release);
        return;
    }
}

static void RecordDreamsCpuWrite(VAddr addr, u64 size) {
    if (!g_dreams_cpu_write_watches_active.load(std::memory_order_acquire)) {
        return;
    }
    const u64 end = addr + size;
    for (auto& watch : g_dreams_cpu_write_watches) {
        const u64 watch_size = watch.size.load(std::memory_order_acquire);
        const u64 watch_base = watch.base.load(std::memory_order_relaxed);
        if (watch_base == 0 || watch_size == 0 || addr >= watch_base + watch_size ||
            watch_base >= end) {
            continue;
        }
        watch.events.fetch_add(1, std::memory_order_relaxed);
        watch.bytes.fetch_add(size, std::memory_order_relaxed);
        watch.last_addr.store(addr, std::memory_order_relaxed);
        watch.last_size.store(size, std::memory_order_relaxed);
    }
}

static void LogDreamsCpuWriteWatches() {
    static std::array<u64, DreamsCpuWriteWatchCount> last_events{};
    for (u32 index = 0; index < g_dreams_cpu_write_watches.size(); ++index) {
        const auto& watch = g_dreams_cpu_write_watches[index];
        const u64 events = watch.events.load(std::memory_order_relaxed);
        if (events == last_events[index]) {
            continue;
        }
        last_events[index] = events;
        LOG_WARNING(Render_Vulkan,
                    "Dreams CPU write watch index={} kind={} range={:#x}+{:#x} events={} "
                    "bytes={:#x} last={:#x}+{:#x}",
                    index, watch.kind.load(std::memory_order_relaxed),
                    watch.base.load(std::memory_order_relaxed),
                    watch.size.load(std::memory_order_relaxed), events,
                    watch.bytes.load(std::memory_order_relaxed),
                    watch.last_addr.load(std::memory_order_relaxed),
                    watch.last_size.load(std::memory_order_relaxed));
    }
}

static std::array<DreamsTrackedBufferRange, 2> g_dreams_sprite_batch_output_ranges{};
static u64 g_dreams_sprite_batch_producer_sequence{};
static u32 g_dreams_sprite_batch_consumer_trace_count{};
static std::array<DreamsTrackedBufferRange, 3> g_dreams_sprite_geometry_output_ranges{};
static u64 g_dreams_sprite_geometry_producer_sequence{};
static u32 g_dreams_sprite_geometry_consumer_trace_count{};

struct DreamsImageWriter {
    enum class Kind : u32 {
        Storage,
        ColorTarget,
        DepthTarget,
    };

    u64 sequence;
    u64 dispatch_sequence;
    u64 shader_hash;
    Shader::LogicalStage stage;
    Kind kind;
    u32 binding_index;
    u32 image_id;
    VAddr base;
    u64 size;
};

static std::deque<DreamsImageWriter> g_dreams_image_writers;
static u64 g_dreams_image_writer_sequence{};

struct RecentComputeDispatch {
    u64 hash{};
    u32 dim_x{};
    u32 dim_y{};
    u32 dim_z{};
};

static std::array<RecentComputeDispatch, 32> g_recent_compute_dispatches{};
static u32 g_recent_compute_dispatch_index{};
static u64 g_compute_dispatch_sequence{};

struct DreamsComputeWindowEntry {
    u64 hash{};
    bool indirect{};
    u64 count{};
    u64 first_sequence{};
    u64 last_sequence{};
    std::array<u32, 3> first_dims{};
    std::array<u32, 3> last_dims{};
    u32 buffers{};
    u32 images{};
    u32 flat_size{};
    std::array<u32, 64> flat_or{};
    std::array<u32, 64> flat_max{};
};

static bool g_dreams_compute_window_active{};
static u32 g_dreams_compute_window_id{};
static u32 g_dreams_compute_window_poll_count{};
static std::vector<DreamsComputeWindowEntry> g_dreams_compute_window_entries;
static std::unordered_map<u64, u32> g_dreams_compute_window_entry_indices;

static u32 g_dreams_replay_progress_gds_index = std::numeric_limits<u32>::max();
static u32 g_dreams_replay_progress_cycle{};
static u32 g_dreams_replay_progress_cycle_observation_count{};
static bool g_dreams_replay_progress_target_found{};

static bool TraceDreamsDiagnostics() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_DIAGNOSTICS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool TraceDreamsOrderedCounters() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_ORDERED_COUNTERS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool TraceDreamsBufferDependencies() {
    const char* value = std::getenv("SHADPS4_DREAMS_DEP_TRACE");
    return value != nullptr && std::string_view{value} == "1" &&
           Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool TraceDreamsProducerSources() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_PRODUCER_SOURCE_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool TraceDreamsSceneStream() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_SCENE_STREAM_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool TraceDreamsVisibilityCounts() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_VISIBILITY_COUNT_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool g_dreams_visibility_count_trace_active{};

static bool ConsumeDreamsVisibilityCountTraceRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return false;
    }
    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_VISIBILITY_COUNT_TRACE_TRIGGER_FILE");
    if (trigger_file == nullptr || trigger_file[0] == '\0') {
        return false;
    }
    std::error_code error;
    return std::filesystem::remove(trigger_file, error);
}

static bool TraceDreamsGraphicsPipelines() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_GRAPHICS_PIPELINE_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool RefreshDreamsProducerCount() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_PRODUCER_COUNT_READBACK");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool ConsumeDreamsProducerInputTraceRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return false;
    }
    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_PRODUCER_INPUT_TRACE_TRIGGER_FILE");
    if (trigger_file == nullptr || trigger_file[0] == '\0') {
        return false;
    }
    std::error_code error;
    return std::filesystem::remove(trigger_file, error);
}

static u32 g_dreams_sprite_consumer_trace_remaining{};
static u32 g_dreams_sprite_consumer_trace_ordinal{};

static bool ConsumeDreamsSpriteConsumerTraceRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return false;
    }
    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_SPRITE_TRACE_TRIGGER_FILE");
    if (trigger_file != nullptr && trigger_file[0] != '\0') {
        std::error_code error;
        if (std::filesystem::remove(trigger_file, error)) {
            g_dreams_sprite_consumer_trace_remaining = 8;
        }
    }
    if (g_dreams_sprite_consumer_trace_remaining == 0) {
        return false;
    }
    --g_dreams_sprite_consumer_trace_remaining;
    return true;
}

static u32 g_dreams_sprite_geometry_trace_remaining{};

static bool ConsumeDreamsSpriteGeometryTraceRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return false;
    }
    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_GEOMETRY_TRACE_TRIGGER_FILE");
    if (trigger_file != nullptr && trigger_file[0] != '\0') {
        std::error_code error;
        if (std::filesystem::remove(trigger_file, error)) {
            g_dreams_sprite_geometry_trace_remaining = 12;
        }
    }
    if (g_dreams_sprite_geometry_trace_remaining == 0) {
        return false;
    }
    --g_dreams_sprite_geometry_trace_remaining;
    return true;
}

static constexpr u64 DreamsFleckFragmentShader = 0xdcc325c2;
static constexpr u64 DreamsSpriteGeometryVertexShader = 0xd25db925;
static constexpr u64 DreamsSpriteGeometryVertexShaderAlt = 0x3706083c;

static bool TraceDreamsFleckRendering() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_FLECK_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool ForceDreamsStorageDependencies() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_FORCE_STORAGE_BARRIERS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static std::optional<std::filesystem::path> ConsumeDreamsFleckDumpRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return std::nullopt;
    }
    const char* trigger_file = std::getenv("SHADPS4_DREAMS_FLECK_DUMP_TRIGGER_FILE");
    const char* output_dir = std::getenv("SHADPS4_DREAMS_FLECK_DUMP_DIR");
    if (trigger_file == nullptr || trigger_file[0] == '\0' || output_dir == nullptr ||
        output_dir[0] == '\0') {
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::remove(trigger_file, error)) {
        return std::nullopt;
    }
    const std::filesystem::path output{output_dir};
    std::filesystem::create_directories(output, error);
    if (error) {
        LOG_ERROR(Render_Vulkan, "Failed to create Dreams fleck dump directory {}: {}",
                  output.string(), error.message());
        return std::nullopt;
    }
    return output;
}

static std::optional<std::filesystem::path> ConsumeDreamsGeometryInputDumpRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return std::nullopt;
    }
    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_GEOMETRY_INPUT_DUMP_TRIGGER_FILE");
    const char* output_dir = std::getenv("SHADPS4_DREAMS_GEOMETRY_INPUT_DUMP_DIR");
    if (trigger_file == nullptr || trigger_file[0] == '\0' || output_dir == nullptr ||
        output_dir[0] == '\0') {
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::remove(trigger_file, error)) {
        return std::nullopt;
    }
    const std::filesystem::path output{output_dir};
    std::filesystem::create_directories(output, error);
    if (error) {
        LOG_ERROR(Render_Vulkan, "Failed to create Dreams geometry input dump directory {}: {}",
                  output.string(), error.message());
        return std::nullopt;
    }
    return output;
}

static std::optional<std::filesystem::path> ConsumeDreamsFleckTargetDumpRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return std::nullopt;
    }
    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_FLECK_TARGET_DUMP_TRIGGER_FILE");
    const char* output_dir = std::getenv("SHADPS4_DREAMS_FLECK_TARGET_DUMP_DIR");
    if (trigger_file == nullptr || trigger_file[0] == '\0' || output_dir == nullptr ||
        output_dir[0] == '\0') {
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::remove(trigger_file, error)) {
        return std::nullopt;
    }
    const std::filesystem::path output{output_dir};
    std::filesystem::create_directories(output, error);
    if (error) {
        LOG_ERROR(Render_Vulkan, "Failed to create Dreams fleck target dump directory {}: {}",
                  output.string(), error.message());
        return std::nullopt;
    }
    return output;
}

static std::optional<std::filesystem::path> ConsumeDreamsVisibilityTargetDumpRequest() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return std::nullopt;
    }
    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_VISIBILITY_TARGET_DUMP_TRIGGER_FILE");
    const char* output_dir = std::getenv("SHADPS4_DREAMS_VISIBILITY_TARGET_DUMP_DIR");
    if (trigger_file == nullptr || trigger_file[0] == '\0' || output_dir == nullptr ||
        output_dir[0] == '\0') {
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::remove(trigger_file, error)) {
        return std::nullopt;
    }
    const std::filesystem::path output{output_dir};
    std::filesystem::create_directories(output, error);
    if (error) {
        LOG_ERROR(Render_Vulkan,
                  "Failed to create Dreams visibility target dump directory {}: {}",
                  output.string(), error.message());
        return std::nullopt;
    }
    return output;
}

static bool IsDreamsFleckPipeline(const GraphicsPipeline* pipeline) {
    if (!TraceDreamsFleckRendering()) {
        return false;
    }
    return std::ranges::any_of(pipeline->GetStages(), [](const Shader::Info* shader) {
        return shader != nullptr && shader->l_stage == Shader::LogicalStage::Fragment &&
               shader->pgm_hash == DreamsFleckFragmentShader;
    });
}

static bool IsDreamsVisibilityPipeline(const GraphicsPipeline* pipeline) {
    if (!TraceDreamsFleckRendering()) {
        return false;
    }
    return std::ranges::any_of(pipeline->GetStages(), [](const Shader::Info* shader) {
        return shader != nullptr && shader->l_stage == Shader::LogicalStage::Fragment &&
               (shader->pgm_hash == 0xce3b8413 || shader->pgm_hash == 0x2b6d3647);
    });
}

static bool IsDreamsSpriteGeometryPipeline(const GraphicsPipeline* pipeline) {
    if (!TraceDreamsProducerSources()) {
        return false;
    }
    return std::ranges::any_of(pipeline->GetStages(), [](const Shader::Info* shader) {
        return shader != nullptr && shader->l_stage == Shader::LogicalStage::Vertex &&
               (shader->pgm_hash == DreamsSpriteGeometryVertexShader ||
                shader->pgm_hash == DreamsSpriteGeometryVertexShaderAlt);
    });
}

static bool IsDreamsMainSpriteGeometryPipeline(const GraphicsPipeline* pipeline) {
    if (!TraceDreamsProducerSources()) {
        return false;
    }
    return std::ranges::any_of(pipeline->GetStages(), [](const Shader::Info* shader) {
        return shader != nullptr && shader->l_stage == Shader::LogicalStage::Vertex &&
               shader->pgm_hash == DreamsSpriteGeometryVertexShader;
    });
}

static void LogDreamsRenderPipeline(std::string_view role, u32 ordinal, bool indirect, bool indexed,
                                    const GraphicsPipeline* pipeline, const AmdGpu::Regs& regs,
                                    const RenderState& state) {
    std::array<u64, Shader::MaxStageTypes> hashes{};
    for (const auto* shader : pipeline->GetStages()) {
        if (shader != nullptr) {
            hashes[static_cast<u32>(shader->l_stage)] = shader->pgm_hash;
        }
    }
    LOG_WARNING(
        Render_Vulkan,
        "Dreams {} draw #{} indirect={} indexed={} stages fs={:#x} tcs={:#x} tes={:#x} "
        "vs={:#x} gs={:#x} primitive={} framebuffer={}x{} layers={} mrt_mask={:#x} "
        "screen_scissor={},{}-{},{} generic_scissor={},{}-{},{} "
        "viewport0=offset({},{},{}) scale({},{},{}) control={},{},{},{},{},{} "
        "clip_disabled={} viewport_scissor0={},{}-{},{}",
        role, ordinal, indirect, indexed,
        hashes[static_cast<u32>(Shader::LogicalStage::Fragment)],
        hashes[static_cast<u32>(Shader::LogicalStage::TessellationControl)],
        hashes[static_cast<u32>(Shader::LogicalStage::TessellationEval)],
        hashes[static_cast<u32>(Shader::LogicalStage::Vertex)],
        hashes[static_cast<u32>(Shader::LogicalStage::Geometry)],
        static_cast<u32>(regs.primitive_type), state.width, state.height, state.num_layers,
        pipeline->GetGraphicsKey().mrt_mask, regs.screen_scissor.top_left_x,
        regs.screen_scissor.top_left_y, regs.screen_scissor.bottom_right_x,
        regs.screen_scissor.bottom_right_y, regs.generic_scissor.top_left_x,
        regs.generic_scissor.top_left_y, regs.generic_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_y, regs.viewports[0].xoffset,
        regs.viewports[0].yoffset, regs.viewports[0].zoffset, regs.viewports[0].xscale,
        regs.viewports[0].yscale, regs.viewports[0].zscale, regs.viewport_control.xoffset_enable,
        regs.viewport_control.xscale_enable, regs.viewport_control.yoffset_enable,
        regs.viewport_control.yscale_enable, regs.viewport_control.zoffset_enable,
        regs.viewport_control.zscale_enable, regs.IsClipDisabled(),
        regs.viewport_scissors[0].top_left_x, regs.viewport_scissors[0].top_left_y,
        regs.viewport_scissors[0].bottom_right_x, regs.viewport_scissors[0].bottom_right_y);

    for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
        if ((pipeline->GetGraphicsKey().mrt_mask & (1U << cb)) == 0) {
            continue;
        }
        const auto& color = regs.color_buffers[cb];
        LOG_WARNING(Render_Vulkan,
                    "Dreams {} target #{} mrt={} guest={:#x} pitch={} height={} bytes={:#x} "
                    "format={}/{} slices={}",
                    role, ordinal, cb, color.Address(), color.Pitch(), color.Height(),
                    color.GetColorSliceSize(), AmdGpu::NameOf(color.GetDataFmt()),
                    AmdGpu::NameOf(color.GetNumberFmt()), color.NumSlices());
    }
}

struct DreamsGraphicsPipelineTraceKey {
    std::array<u64, Shader::MaxStageTypes> hashes{};
    u32 primitive{};
    u32 mrt_mask{};
    bool indirect{};
    bool indexed{};

    bool operator==(const DreamsGraphicsPipelineTraceKey&) const = default;
};

struct DreamsSceneGraphicsBindingTraceKey {
    u64 generation{};
    u64 shader_hash{};
    Shader::LogicalStage stage{};
    u32 binding_index{};
    VAddr base{};
    u64 size{};

    bool operator==(const DreamsSceneGraphicsBindingTraceKey&) const = default;
};

static u32 g_dreams_scene_graphics_probe_budget{};
static u64 g_dreams_scene_graphics_probe_generation{};
static u32 g_dreams_scene_graphics_relevant_draw_count{};
static std::vector<DreamsGraphicsPipelineTraceKey> g_dreams_scene_graphics_pipelines;
static std::vector<DreamsSceneGraphicsBindingTraceKey> g_dreams_scene_graphics_bindings;
static std::array<DreamsTrackedBufferRange, 64> g_dreams_scene_generated_ranges{};
static u32 g_dreams_scene_generated_range_count{};

static DreamsGraphicsPipelineTraceKey GetDreamsGraphicsPipelineTraceKey(
    bool indirect, bool indexed, const GraphicsPipeline* pipeline, const AmdGpu::Regs& regs) {
    DreamsGraphicsPipelineTraceKey key{
        .primitive = static_cast<u32>(regs.primitive_type),
        .mrt_mask = pipeline->GetGraphicsKey().mrt_mask,
        .indirect = indirect,
        .indexed = indexed,
    };
    for (const auto* shader : pipeline->GetStages()) {
        if (shader != nullptr) {
            key.hashes[static_cast<u32>(shader->l_stage)] = shader->pgm_hash;
        }
    }
    return key;
}

static void TraceDreamsGraphicsPipeline(bool indirect, bool indexed,
                                        const GraphicsPipeline* pipeline,
                                        const AmdGpu::Regs& regs, u32 direct_count,
                                        u32 direct_instances, VAddr args_address, u32 stride,
                                        u32 max_count, VAddr count_address) {
    if (!TraceDreamsGraphicsPipelines()) {
        return;
    }

    const auto key = GetDreamsGraphicsPipelineTraceKey(indirect, indexed, pipeline, regs);

    static std::vector<DreamsGraphicsPipelineTraceKey> traced_pipelines;
    if (traced_pipelines.size() >= 2048 ||
        std::ranges::find(traced_pipelines, key) != traced_pipelines.end()) {
        return;
    }
    traced_pipelines.push_back(key);

    const auto recent_compute_hash = [](u32 back) {
        const u32 recent_count =
            std::min<u32>(g_recent_compute_dispatch_index, g_recent_compute_dispatches.size());
        if (back >= recent_count) {
            return u64{};
        }
        const u32 index =
            (g_recent_compute_dispatch_index - 1 - back) % g_recent_compute_dispatches.size();
        return g_recent_compute_dispatches[index].hash;
    };
    const auto& target = regs.color_buffers[0];
    LOG_WARNING(
        Render_Vulkan,
        "Dreams graphics pipeline #{} indirect={} indexed={} fs={:#x} tcs={:#x} tes={:#x} "
        "vs={:#x} gs={:#x} primitive={} mrt={:#x} target0={:#x} target0_pitch={} "
        "target0_height={} target0_format={}/{} direct_count={} direct_instances={} "
        "args={:#x} stride={} max_count={} count={:#x} recent_compute={:#x},{:#x},{:#x},"
        "{:#x},{:#x},{:#x},{:#x},{:#x}",
        traced_pipelines.size(), indirect, indexed,
        key.hashes[static_cast<u32>(Shader::LogicalStage::Fragment)],
        key.hashes[static_cast<u32>(Shader::LogicalStage::TessellationControl)],
        key.hashes[static_cast<u32>(Shader::LogicalStage::TessellationEval)],
        key.hashes[static_cast<u32>(Shader::LogicalStage::Vertex)],
        key.hashes[static_cast<u32>(Shader::LogicalStage::Geometry)], key.primitive, key.mrt_mask,
        target.Address(), target.Pitch(), target.Height(), AmdGpu::NameOf(target.GetDataFmt()),
        AmdGpu::NameOf(target.GetNumberFmt()), direct_count, direct_instances, args_address, stride,
        max_count, count_address, recent_compute_hash(0), recent_compute_hash(1),
        recent_compute_hash(2), recent_compute_hash(3), recent_compute_hash(4),
        recent_compute_hash(5), recent_compute_hash(6), recent_compute_hash(7));
}

static void TraceDreamsSceneGraphicsDraw(bool indirect, bool indexed,
                                         const GraphicsPipeline* pipeline,
                                         const AmdGpu::Regs& regs, const RenderState& state,
                                         u32 direct_count, u32 direct_instances,
                                         VAddr args_address, u32 stride, u32 max_count,
                                         VAddr count_address) {
    if (g_dreams_scene_graphics_probe_budget == 0) {
        return;
    }
    --g_dreams_scene_graphics_probe_budget;

    const auto key = GetDreamsGraphicsPipelineTraceKey(indirect, indexed, pipeline, regs);
    const u64 fragment_hash = key.hashes[static_cast<u32>(Shader::LogicalStage::Fragment)];
    const bool relevant = fragment_hash == DreamsFleckFragmentShader ||
                          fragment_hash == 0xce3b8413 || fragment_hash == 0x2b6d3647;
    const bool unique = std::ranges::find(g_dreams_scene_graphics_pipelines, key) ==
                        g_dreams_scene_graphics_pipelines.end();
    if (!unique && !relevant) {
        return;
    }
    if (unique && g_dreams_scene_graphics_pipelines.size() < 256) {
        g_dreams_scene_graphics_pipelines.push_back(key);
    }
    if (relevant && g_dreams_scene_graphics_relevant_draw_count++ >= 256) {
        return;
    }

    const u32 ordinal = relevant ? g_dreams_scene_graphics_relevant_draw_count
                                 : static_cast<u32>(g_dreams_scene_graphics_pipelines.size());
    LogDreamsRenderPipeline(relevant ? "scene-relevant" : "scene-unique", ordinal, indirect,
                            indexed, pipeline, regs, state);
    LOG_WARNING(Render_Vulkan,
                "Dreams scene graphics probe generation={} relevant={} remaining={} "
                "direct_count={} instances={} args={:#x} stride={} max_count={} count={:#x}",
                g_dreams_scene_graphics_probe_generation, relevant,
                g_dreams_scene_graphics_probe_budget, direct_count, direct_instances,
                args_address, stride, max_count, count_address);
}

static bool TrackDreamsBufferWriters() {
    return TraceDreamsBufferDependencies() || TraceDreamsProducerSources() ||
           TraceDreamsSceneStream() || TraceDreamsFleckRendering() ||
           TraceDreamsVisibilityCounts();
}

static bool TrackDreamsImageWriters() {
    return TraceDreamsProducerSources() || TraceDreamsFleckRendering();
}

static bool OverlapsDreamsRange(VAddr lhs_base, u64 lhs_size, VAddr rhs_base, u64 rhs_size) {
    return lhs_base != 0 && rhs_base != 0 && lhs_size != 0 && rhs_size != 0 &&
           lhs_base < rhs_base + rhs_size && rhs_base < lhs_base + lhs_size;
}

static bool RememberDreamsSceneGeneratedRange(VAddr base, u64 size) {
    if (base == 0 || size == 0) {
        return false;
    }
    const DreamsTrackedBufferRange range{.base = base, .size = size};
    const auto begin = g_dreams_scene_generated_ranges.begin();
    const auto end = begin + g_dreams_scene_generated_range_count;
    if (std::ranges::find(begin, end, range) != end ||
        g_dreams_scene_generated_range_count == g_dreams_scene_generated_ranges.size()) {
        return false;
    }
    g_dreams_scene_generated_ranges[g_dreams_scene_generated_range_count++] = range;
    return true;
}

static void RememberDreamsVisibilityInputWriter(const DreamsBufferWriter& writer) {
    if (!writer.declared_write) {
        return;
    }
    const bool overlaps_input = std::ranges::any_of(g_dreams_visibility_input_ranges, [&](auto range) {
        return OverlapsDreamsRange(writer.base, writer.size, range.base, range.size);
    });
    if (!overlaps_input) {
        return;
    }

    const auto same_writer = [&](const DreamsBufferWriter& candidate) {
        return candidate.shader_hash == writer.shader_hash && candidate.stage == writer.stage &&
               candidate.kind == writer.kind && candidate.binding_index == writer.binding_index &&
               candidate.base == writer.base && candidate.size == writer.size &&
               candidate.stride == writer.stride && candidate.source == writer.source;
    };
    if (auto existing = std::ranges::find_if(g_dreams_visibility_input_writers, same_writer);
        existing != g_dreams_visibility_input_writers.end()) {
        *existing = writer;
        return;
    }
    g_dreams_visibility_input_writers.push_back(writer);
}

static void SetDreamsVisibilityInputRanges(DreamsTrackedBufferRange records,
                                           DreamsTrackedBufferRange aux) {
    const std::array ranges{records, aux};
    if (ranges == g_dreams_visibility_input_ranges) {
        return;
    }
    g_dreams_visibility_input_ranges = ranges;
    g_dreams_visibility_input_writers.clear();
    for (const auto& writer : g_dreams_buffer_writers) {
        RememberDreamsVisibilityInputWriter(writer);
    }
}

static void RecordDreamsBufferWriter(DreamsBufferWriter writer) {
    g_dreams_buffer_writers.push_back(writer);
    RememberDreamsVisibilityInputWriter(writer);
    constexpr size_t MaxDreamsBufferWriterHistory = 65536;
    if (g_dreams_buffer_writers.size() > MaxDreamsBufferWriterHistory) {
        g_dreams_buffer_writers.pop_front();
    }
}

static void RecordDreamsImageWriter(DreamsImageWriter writer) {
    g_dreams_image_writers.push_back(writer);
    constexpr size_t MaxDreamsImageWriterHistory = 65536;
    if (g_dreams_image_writers.size() > MaxDreamsImageWriterHistory) {
        g_dreams_image_writers.pop_front();
    }
}

static bool TraceDreamsCsgReplay() {
    const char* value = std::getenv("SHADPS4_DREAMS_CSG_TRACE");
    return value != nullptr && std::string_view{value} == "1" &&
           Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool TraceDreamsReplayResult() {
    const char* value = std::getenv("SHADPS4_DREAMS_CPU_ROOT_TRACE");
    return value != nullptr && std::string_view{value} == "1" &&
           Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

struct ComputeSkipMode {
    std::vector<u64> hashes;
    bool all{};
};

static const ComputeSkipMode& GetComputeSkipMode() {
    static const ComputeSkipMode mode = [] {
        const char* value = std::getenv("SHADPS4_SKIP_COMPUTE_SHADER");
        if (value == nullptr || *value == '\0') {
            return ComputeSkipMode{};
        }
        if (std::strcmp(value, "all") == 0) {
            return ComputeSkipMode{.all = true};
        }
        ComputeSkipMode parsed_mode{};
        while (*value != '\0') {
            char* end{};
            const u64 parsed = std::strtoull(value, &end, 0);
            if (end == value) {
                return ComputeSkipMode{};
            }
            parsed_mode.hashes.push_back(parsed);
            if (*end == '\0') {
                break;
            }
            if (*end != ',' && *end != ';') {
                return ComputeSkipMode{};
            }
            value = end + 1;
        }
        return parsed_mode;
    }();
    return mode;
}

static bool ShouldSkipComputeShader(const Shader::Info& info) {
    const auto& mode = GetComputeSkipMode();
    if (!mode.all && std::ranges::find(mode.hashes, info.pgm_hash) == mode.hashes.end()) {
        return false;
    }
    static bool reported = false;
    if (!reported) {
        LOG_WARNING(Render_Vulkan, "Skipping selected compute shaders for diagnostics");
        reported = true;
    }
    return true;
}

static bool ConsumeDreamsComputeWindowTrigger(const char* variable) {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return false;
    }
    const char* path = std::getenv(variable);
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::error_code error;
    return std::filesystem::remove(path, error);
}

static void FinishDreamsComputeWindow() {
    if (!g_dreams_compute_window_active) {
        return;
    }
    g_dreams_compute_window_active = false;
    std::ranges::sort(g_dreams_compute_window_entries, std::greater{},
                      &DreamsComputeWindowEntry::count);
    u64 total{};
    for (const auto& entry : g_dreams_compute_window_entries) {
        total += entry.count;
    }
    LOG_WARNING(Render_Vulkan, "Dreams compute window end id={} total={} shaders={}",
                g_dreams_compute_window_id, total, g_dreams_compute_window_entries.size());
    for (u32 ordinal = 0; ordinal < g_dreams_compute_window_entries.size(); ++ordinal) {
        const auto& entry = g_dreams_compute_window_entries[ordinal];
        std::string nonzero_flat;
        for (u32 index = 0; index < entry.flat_size && index < entry.flat_or.size(); ++index) {
            if (entry.flat_or[index] == 0) {
                continue;
            }
            fmt::format_to(std::back_inserter(nonzero_flat), "{}{}:{:#x}/{:#x}",
                           nonzero_flat.empty() ? "" : ",", index, entry.flat_or[index],
                           entry.flat_max[index]);
        }
        LOG_WARNING(
            Render_Vulkan,
            "Dreams compute window id={} rank={} hash={:#x} indirect={} count={} sequence={}-{} "
            "first_dims={}x{}x{} last_dims={}x{}x{} buffers={} images={} flat_size={} "
            "flat_or_max={}",
            g_dreams_compute_window_id, ordinal, entry.hash, entry.indirect, entry.count,
            entry.first_sequence, entry.last_sequence, entry.first_dims[0], entry.first_dims[1],
            entry.first_dims[2], entry.last_dims[0], entry.last_dims[1], entry.last_dims[2],
            entry.buffers, entry.images, entry.flat_size, nonzero_flat);
    }
    g_dreams_compute_window_entries.clear();
    g_dreams_compute_window_entry_indices.clear();
}

static void TraceDreamsComputeWindow(const Shader::Info& info, bool indirect, u32 dim_x, u32 dim_y,
                                     u32 dim_z) {
    if (++g_dreams_compute_window_poll_count % 128 == 0) {
        if (ConsumeDreamsComputeWindowTrigger("SHADPS4_DREAMS_COMPUTE_TRACE_START")) {
            g_dreams_compute_window_entries.clear();
            g_dreams_compute_window_entry_indices.clear();
            g_dreams_compute_window_active = true;
            ++g_dreams_compute_window_id;
            LOG_WARNING(Render_Vulkan, "Dreams compute window start id={} sequence={}",
                        g_dreams_compute_window_id, g_compute_dispatch_sequence);
        }
        if (ConsumeDreamsComputeWindowTrigger("SHADPS4_DREAMS_COMPUTE_TRACE_STOP")) {
            FinishDreamsComputeWindow();
        }
    }
    if (!g_dreams_compute_window_active) {
        return;
    }

    const u64 key = info.pgm_hash ^ (indirect ? 1ULL << 63 : 0);
    const auto [it, inserted] = g_dreams_compute_window_entry_indices.try_emplace(
        key, static_cast<u32>(g_dreams_compute_window_entries.size()));
    if (inserted) {
        g_dreams_compute_window_entries.push_back({
            .hash = info.pgm_hash,
            .indirect = indirect,
            .first_sequence = g_compute_dispatch_sequence,
            .first_dims = {dim_x, dim_y, dim_z},
            .buffers = static_cast<u32>(info.buffers.size()),
            .images = static_cast<u32>(info.images.size()),
            .flat_size = static_cast<u32>(info.flattened_ud_buf.size()),
        });
    }
    auto& entry = g_dreams_compute_window_entries[it->second];
    ++entry.count;
    entry.last_sequence = g_compute_dispatch_sequence;
    entry.last_dims = {dim_x, dim_y, dim_z};
    const u32 flat_count = std::min<u32>(info.flattened_ud_buf.size(), entry.flat_or.size());
    for (u32 index = 0; index < flat_count; ++index) {
        entry.flat_or[index] |= info.flattened_ud_buf[index];
        entry.flat_max[index] = std::max(entry.flat_max[index], info.flattened_ud_buf[index]);
    }
}

static void TraceComputeShader(const Shader::Info& info, bool indirect, u32 dim_x, u32 dim_y,
                               u32 dim_z) {
    g_recent_compute_dispatches[g_recent_compute_dispatch_index++ %
                                g_recent_compute_dispatches.size()] = {
        .hash = info.pgm_hash,
        .dim_x = dim_x,
        .dim_y = dim_y,
        .dim_z = dim_z,
    };
    ++g_compute_dispatch_sequence;
    TraceDreamsComputeWindow(info, indirect, dim_x, dim_y, dim_z);

    static const bool enabled = std::getenv("SHADPS4_TRACE_COMPUTE") != nullptr;
    static u32 count{};
    if (enabled && count++ < 4096) {
        LOG_WARNING(Render_Vulkan, "Compute dispatch hash={:#x} dims={}x{}x{}", info.pgm_hash,
                    dim_x, dim_y, dim_z);
    }
}

static bool ShouldProfileCompute() {
    static const bool enabled = std::getenv("SHADPS4_PROFILE_COMPUTE") != nullptr;
    static u32 count{};
    return enabled && count++ < 512;
}

static u32 ConsumeDreamsGpuProfileEvent() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return 0;
    }
    static u32 remaining{};
    static u32 ordinal{};
    const char* trigger_file = std::getenv("SHADPS4_DREAMS_GPU_PROFILE_TRIGGER_FILE");
    if (trigger_file != nullptr && trigger_file[0] != '\0') {
        std::error_code error;
        if (std::filesystem::remove(trigger_file, error)) {
            remaining = 512;
            ordinal = 0;
            LOG_WARNING(Render_Vulkan, "Dreams one-shot GPU profile started");
        }
    }
    if (remaining == 0) {
        return 0;
    }
    --remaining;
    return ++ordinal;
}

static bool ShouldProfileDreamsTraversal(const Shader::Info& info) {
    static const bool enabled =
        std::getenv("SHADPS4_PROFILE_DREAMS_TRAVERSAL") != nullptr;
    static u32 count{};
    return enabled && info.pgm_hash == DreamsTraversalShader && count++ < 128;
}

static bool TraceDreamsTraversalProgress() {
    static const bool enabled =
        std::getenv("SHADPS4_TRACE_DREAMS_TRAVERSAL_PROGRESS") != nullptr;
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    const u32 gpu_profile_ordinal = ConsumeDreamsGpuProfileEvent();
    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
    }
    const auto gpu_profile_start = std::chrono::steady_clock::now();

    TraceDreamsGraphicsPipeline(false, is_indexed, pipeline, regs, regs.num_indices,
                                regs.num_instances.NumInstances(), 0, 0, 0, 0);

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    TraceDreamsSceneGraphicsDraw(false, is_indexed, pipeline, regs, state, regs.num_indices,
                                 regs.num_instances.NumInstances(), 0, 0, 0, 0);

    static u32 dreams_fleck_direct_trace_count{};
    if (IsDreamsFleckPipeline(pipeline) && dreams_fleck_direct_trace_count < 32) {
        const u32 ordinal = ++dreams_fleck_direct_trace_count;
        LogDreamsRenderPipeline("decoder", ordinal, false, is_indexed, pipeline, regs, state);
        LOG_WARNING(Render_Vulkan,
                    "Dreams decoder direct args #{} vertices_or_indices={} instances={} "
                    "index_offset={}",
                    ordinal, regs.num_indices, regs.num_instances.NumInstances(), index_offset);
    }
    static u32 dreams_visibility_direct_trace_count{};
    if (IsDreamsVisibilityPipeline(pipeline) && dreams_visibility_direct_trace_count < 64) {
        const u32 ordinal = ++dreams_visibility_direct_trace_count;
        LogDreamsRenderPipeline("visibility", ordinal, false, is_indexed, pipeline, regs, state);
        LOG_WARNING(Render_Vulkan,
                    "Dreams visibility direct args #{} vertices_or_indices={} instances={} "
                    "index_offset={}",
                    ordinal, regs.num_indices, regs.num_instances.NumInstances(), index_offset);
    }
    static u32 dreams_sprite_geometry_direct_trace_count{};
    if (IsDreamsSpriteGeometryPipeline(pipeline) &&
        dreams_sprite_geometry_direct_trace_count < 64) {
        const u32 ordinal = ++dreams_sprite_geometry_direct_trace_count;
        LogDreamsRenderPipeline("sprite-geometry", ordinal, false, is_indexed, pipeline, regs,
                                state);
        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite geometry direct args #{} vertices_or_indices={} instances={} "
                    "index_offset={}",
                    ordinal, regs.num_indices, regs.num_instances.NumInstances(), index_offset);
    }

    buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset, buffer_barriers);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    DebugState.IncDrawCall();

    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
        u64 vertex_hash{};
        u64 fragment_hash{};
        for (const auto* shader : pipeline->GetStages()) {
            if (shader == nullptr) {
                continue;
            }
            if (shader->l_stage == Shader::LogicalStage::Vertex) {
                vertex_hash = shader->pgm_hash;
            } else if (shader->l_stage == Shader::LogicalStage::Fragment) {
                fragment_hash = shader->pgm_hash;
            }
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - gpu_profile_start)
                                 .count();
        LOG_WARNING(Render_Vulkan,
                    "Dreams GPU profile #{} kind=draw indexed={} vs={:#x} fs={:#x} "
                    "vertices={} instances={} elapsed={:.3f}ms",
                    gpu_profile_ordinal, is_indexed, vertex_hash, fragment_hash, regs.num_indices,
                    regs.num_instances.NumInstances(), elapsed);
    }

    if (IsDreamsFleckPipeline(pipeline)) {
        if (const auto output_dir = ConsumeDreamsFleckTargetDumpRequest()) {
            for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
                if ((pipeline->GetGraphicsKey().mrt_mask & (1U << cb)) == 0) {
                    continue;
                }
                const auto image_id = cb_descs[cb].first;
                const auto& image = texture_cache.GetImage(image_id);
                const auto file =
                    *output_dir /
                    fmt::format("mrt{}-{:#x}-{}x{}-pitch{}-bits{}.bin", cb,
                                image.info.guest_address, image.info.size.width,
                                image.info.size.height, image.info.pitch, image.info.num_bits);
                texture_cache.DumpImageLinear(image_id, file);
                LOG_WARNING(Render_Vulkan,
                            "Dreams fleck target dumped mrt={} id={} guest={:#x} "
                            "extent={}x{} pitch={} bits={} file={}",
                            cb, image_id.index, image.info.guest_address,
                            image.info.size.width, image.info.size.height, image.info.pitch,
                            image.info.num_bits, file.string());
            }
        }
    }

    ResetBindings();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    const u32 gpu_profile_ordinal = ConsumeDreamsGpuProfileEvent();
    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
    }
    const auto gpu_profile_start = std::chrono::steady_clock::now();

    TraceDreamsGraphicsPipeline(true, is_indexed, pipeline, regs, 0, 0, arg_address + offset,
                                stride, max_count, count_address);

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    const auto dreams_graphics_key =
        GetDreamsGraphicsPipelineTraceKey(true, is_indexed, pipeline, regs);
    const u64 dreams_fragment_hash =
        dreams_graphics_key.hashes[static_cast<u32>(Shader::LogicalStage::Fragment)];
    static std::optional<std::filesystem::path> dreams_visibility_dump_dir;
    static u32 dreams_visibility_dump_draw_count{};
    if (dreams_fragment_hash == 0x2b6d3647) {
        if (auto output_dir = ConsumeDreamsVisibilityTargetDumpRequest()) {
            dreams_visibility_dump_dir = std::move(output_dir);
            dreams_visibility_dump_draw_count = 0;
        }
    }

    TraceDreamsSceneGraphicsDraw(true, is_indexed, pipeline, regs, state, 0, 0,
                                 arg_address + offset, stride, max_count, count_address);

    static u32 dreams_sprite_geometry_draw_trace_count{};
    const VAddr sprite_commands_address = arg_address + offset;
    const u64 sprite_commands_size = static_cast<u64>(stride) * max_count;
    const bool dreams_main_sprite_geometry = IsDreamsMainSpriteGeometryPipeline(pipeline);
    const bool dreams_sprite_geometry = IsDreamsSpriteGeometryPipeline(pipeline);
    const bool trace_dreams_sprite_geometry_draw =
        TraceDreamsProducerSources() && dreams_sprite_geometry &&
        ConsumeDreamsSpriteGeometryTraceRequest();
    if (trace_dreams_sprite_geometry_draw) {
        const u32 ordinal = ++dreams_sprite_geometry_draw_trace_count;
        LogDreamsRenderPipeline("sprite-geometry", ordinal, true, is_indexed, pipeline,
                                liverpool->regs, state);
        scheduler.Finish();
        u32 draw_count = max_count;
        if (count_address != 0 && memory->IsValidMapping(count_address, sizeof(draw_count))) {
            buffer_cache.ReadMemory(count_address, sizeof(draw_count));
            std::memcpy(&draw_count, std::bit_cast<const void*>(count_address),
                        sizeof(draw_count));
            draw_count = std::min(draw_count, max_count);
        }
        const u32 traced_commands = std::min(draw_count, 16U);
        const u64 traced_size = static_cast<u64>(traced_commands) * stride;
        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite geometry indirect #{} producer_dispatch={} args={:#x} "
                    "stride={} max_count={} count={:#x} draw_count={} traced={}",
                    ordinal, g_dreams_sprite_geometry_producer_sequence,
                    sprite_commands_address, stride, max_count, count_address, draw_count,
                    traced_commands);
        if (dreams_main_sprite_geometry) {
            const u64 commands_end = sprite_commands_address + sprite_commands_size;
            u32 writer_matches{};
            for (auto writer = g_dreams_buffer_writers.rbegin();
                 writer != g_dreams_buffer_writers.rend() && writer_matches < 64; ++writer) {
                const bool overlaps_commands =
                    writer->declared_write && writer->base < commands_end &&
                    sprite_commands_address < writer->base + writer->size;
                const bool overlaps_count =
                    count_address != 0 && writer->declared_write &&
                    OverlapsDreamsRange(writer->base, writer->size, count_address, sizeof(u32));
                if (!overlaps_commands && !overlaps_count) {
                    continue;
                }
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams sprite geometry args writer #{} match={} seq={} dispatch={} kind={} "
                    "shader={:#x} stage={} binding={} range={:#x}+{:#x} stride={} "
                    "commands={} count={}",
                    ordinal, writer_matches, writer->sequence, writer->dispatch_sequence,
                    static_cast<u32>(writer->kind), writer->shader_hash,
                    static_cast<u32>(writer->stage), writer->binding_index, writer->base,
                    writer->size, writer->stride, overlaps_commands, overlaps_count);
                ++writer_matches;
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams sprite geometry args writer summary #{} matches={} history={}",
                        ordinal, writer_matches, g_dreams_buffer_writers.size());
            if (ordinal <= 4) {
                const u32 recent_count =
                    std::min<u32>(g_recent_compute_dispatch_index, g_recent_compute_dispatches.size());
                for (u32 recent = 0; recent < recent_count; ++recent) {
                    const u32 index = (g_recent_compute_dispatch_index - 1 - recent) %
                                      g_recent_compute_dispatches.size();
                    const auto& dispatch = g_recent_compute_dispatches[index];
                    LOG_WARNING(Render_Vulkan,
                                "Dreams sprite geometry recent compute #{} back={} shader={:#x} "
                                "dims={}x{}x{}",
                                ordinal, recent, dispatch.hash, dispatch.dim_x, dispatch.dim_y,
                                dispatch.dim_z);
                }
            }
        }
        if (traced_size != 0 &&
            memory->IsValidMapping(sprite_commands_address, traced_size)) {
            buffer_cache.ReadMemory(sprite_commands_address, traced_size);
            for (u32 command_index = 0; command_index < traced_commands; ++command_index) {
                const auto* command_address = std::bit_cast<const u8*>(sprite_commands_address) +
                                              static_cast<u64>(command_index) * stride;
                if (is_indexed) {
                    VkDrawIndexedIndirectCommand command{};
                    std::memcpy(&command, command_address, sizeof(command));
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams sprite geometry command #{}[{}] indices={} instances={} "
                        "first_index={} vertex_offset={} first_instance={}",
                        ordinal, command_index, command.indexCount, command.instanceCount,
                        command.firstIndex, command.vertexOffset, command.firstInstance);
                } else {
                    VkDrawIndirectCommand command{};
                    std::memcpy(&command, command_address, sizeof(command));
                    LOG_WARNING(Render_Vulkan,
                                "Dreams sprite geometry command #{}[{}] vertices={} instances={} "
                                "first_vertex={} first_instance={}",
                                ordinal, command_index, command.vertexCount,
                                command.instanceCount, command.firstVertex,
                                command.firstInstance);
                }
            }
        }
        if (dreams_main_sprite_geometry && draw_count != 0) {
            const u64 scan_size = static_cast<u64>(draw_count) * stride;
            if (memory->IsValidMapping(sprite_commands_address, scan_size)) {
                buffer_cache.ReadMemory(sprite_commands_address, scan_size);
                u32 nonzero_commands{};
                u32 nonzero_instances{};
                u32 reported{};
                for (u32 command_index = 0; command_index < draw_count; ++command_index) {
                    const auto* command_address = std::bit_cast<const u8*>(sprite_commands_address) +
                                                  static_cast<u64>(command_index) * stride;
                    VkDrawIndexedIndirectCommand command{};
                    std::memcpy(&command, command_address, sizeof(command));
                    const bool nonzero = command.indexCount != 0 || command.instanceCount != 0 ||
                                         command.firstIndex != 0 || command.vertexOffset != 0 ||
                                         command.firstInstance != 0;
                    nonzero_commands += nonzero;
                    nonzero_instances += command.instanceCount != 0;
                    if (nonzero && reported < 16) {
                        LOG_WARNING(
                            Render_Vulkan,
                            "Dreams sprite geometry nonzero command #{}[{}] indices={} "
                            "instances={} first_index={} vertex_offset={} first_instance={}",
                            ordinal, command_index, command.indexCount, command.instanceCount,
                            command.firstIndex, command.vertexOffset, command.firstInstance);
                        ++reported;
                    }
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams sprite geometry command summary #{} scanned={} nonzero={} "
                            "with_instances={}",
                            ordinal, draw_count, nonzero_commands, nonzero_instances);
            }
        }
    }

    static u32 dreams_fleck_indirect_trace_count{};
    const bool trace_dreams_decoder_indirect = IsDreamsFleckPipeline(pipeline);
    const bool trace_dreams_visibility_indirect = IsDreamsVisibilityPipeline(pipeline);
    if ((trace_dreams_decoder_indirect || trace_dreams_visibility_indirect) &&
        dreams_fleck_indirect_trace_count < 64) {
        const u32 ordinal = ++dreams_fleck_indirect_trace_count;
        const std::string_view role =
            trace_dreams_visibility_indirect ? "visibility" : "decoder";
        LogDreamsRenderPipeline(role, ordinal, true, is_indexed, pipeline, liverpool->regs, state);

        scheduler.Finish();
        u32 draw_count = max_count;
        if (count_address != 0 && memory->IsValidMapping(count_address, sizeof(draw_count))) {
            buffer_cache.ReadMemory(count_address, sizeof(draw_count));
            std::memcpy(&draw_count, std::bit_cast<const void*>(count_address), sizeof(draw_count));
            draw_count = std::min(draw_count, max_count);
        }
        constexpr u32 MaxTracedCommands = 8;
        const u32 traced_commands = std::min(draw_count, MaxTracedCommands);
        const VAddr commands_address = arg_address + offset;
        const u64 commands_size = static_cast<u64>(traced_commands) * stride;
        LOG_WARNING(Render_Vulkan,
                    "Dreams {} indirect args #{} address={:#x} stride={} max_count={} "
                    "count_address={:#x} draw_count={} traced={}",
                    role, ordinal, commands_address, stride, max_count, count_address, draw_count,
                    traced_commands);
        if (trace_dreams_visibility_indirect) {
            const u64 commands_end = commands_address + std::max<u64>(commands_size, stride);
            u32 writer_matches{};
            for (auto writer = g_dreams_buffer_writers.rbegin();
                 writer != g_dreams_buffer_writers.rend() && writer_matches < 32; ++writer) {
                const u64 writer_end = writer->base + writer->size;
                if (!writer->declared_write || writer->base >= commands_end ||
                    commands_address >= writer_end) {
                    continue;
                }
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams visibility args writer #{} match={} seq={} dispatch={} kind={} "
                    "shader={:#x} stage={} binding={} range={:#x}+{:#x} stride={} source={:#x}",
                    ordinal, writer_matches, writer->sequence, writer->dispatch_sequence,
                    static_cast<u32>(writer->kind), writer->shader_hash,
                    static_cast<u32>(writer->stage), writer->binding_index, writer->base,
                    writer->size, writer->stride, writer->source);
                ++writer_matches;
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams visibility args writer summary #{} matches={} history={}",
                        ordinal, writer_matches, g_dreams_buffer_writers.size());
            if (ordinal <= 6) {
                const u32 recent_count = std::min<u32>(g_recent_compute_dispatch_index,
                                                       g_recent_compute_dispatches.size());
                for (u32 recent = 0; recent < recent_count; ++recent) {
                    const u32 index = (g_recent_compute_dispatch_index - 1 - recent) %
                                      g_recent_compute_dispatches.size();
                    const auto& dispatch = g_recent_compute_dispatches[index];
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams visibility recent compute #{} back={} shader={:#x} dims={}x{}x{}",
                        ordinal, recent, dispatch.hash, dispatch.dim_x, dispatch.dim_y,
                        dispatch.dim_z);
                }
            }
        }
        if (commands_size != 0 && memory->IsValidMapping(commands_address, commands_size)) {
            buffer_cache.ReadMemory(commands_address, commands_size);
            for (u32 command_index = 0; command_index < traced_commands; ++command_index) {
                const auto* command_address = std::bit_cast<const u8*>(commands_address) +
                                              static_cast<u64>(command_index) * stride;
                if (is_indexed) {
                    VkDrawIndexedIndirectCommand command{};
                    std::memcpy(&command, command_address, sizeof(command));
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams {} indirect command #{}[{}] indices={} instances={} "
                        "first_index={} vertex_offset={} first_instance={}",
                        role, ordinal, command_index, command.indexCount, command.instanceCount,
                        command.firstIndex, command.vertexOffset, command.firstInstance);
                } else {
                    VkDrawIndirectCommand command{};
                    std::memcpy(&command, command_address, sizeof(command));
                    LOG_WARNING(Render_Vulkan,
                                "Dreams {} indirect command #{}[{}] vertices={} instances={} "
                                "first_vertex={} first_instance={}",
                                role, ordinal, command_index, command.vertexCount,
                                command.instanceCount, command.firstVertex, command.firstInstance);
                }
            }
        }
    }

    buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0, buffer_barriers);
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }
    if (count_buffer) {
        if (auto barrier = count_buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                                    vk::PipelineStageFlagBits2::eDrawIndirect)) {
            buffer_barriers.emplace_back(*barrier);
        }
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }

    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
        u64 vertex_hash{};
        u64 fragment_hash{};
        for (const auto* shader : pipeline->GetStages()) {
            if (shader == nullptr) {
                continue;
            }
            if (shader->l_stage == Shader::LogicalStage::Vertex) {
                vertex_hash = shader->pgm_hash;
            } else if (shader->l_stage == Shader::LogicalStage::Fragment) {
                fragment_hash = shader->pgm_hash;
            }
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - gpu_profile_start)
                                 .count();
        LOG_WARNING(Render_Vulkan,
                    "Dreams GPU profile #{} kind=draw-indirect indexed={} vs={:#x} fs={:#x} "
                    "max_count={} stride={} elapsed={:.3f}ms",
                    gpu_profile_ordinal, is_indexed, vertex_hash, fragment_hash, max_count, stride,
                    elapsed);
    }

    const bool dreams_visibility_draw =
        dreams_fragment_hash == 0x2b6d3647 || dreams_fragment_hash == 0xce3b8413;
    if (dreams_visibility_dump_dir && dreams_visibility_draw &&
        ++dreams_visibility_dump_draw_count == 3) {
        const auto dump_target = [&](std::string_view role, VideoCore::ImageId image_id) {
            if (!image_id) {
                return;
            }
            const auto& image = texture_cache.GetImage(image_id);
            const auto file =
                *dreams_visibility_dump_dir /
                fmt::format("{}-{:#x}-{}x{}-pitch{}-bits{}.bin", role,
                            image.info.guest_address, image.info.size.width,
                            image.info.size.height, image.info.pitch, image.info.num_bits);
            texture_cache.DumpImageLinear(image_id, file);
            LOG_WARNING(Render_Vulkan,
                        "Dreams visibility target dumped role={} id={} guest={:#x} "
                        "extent={}x{} pitch={} bits={} file={}",
                        role, image_id.index, image.info.guest_address, image.info.size.width,
                        image.info.size.height, image.info.pitch, image.info.num_bits,
                        file.string());
        };
        dump_target("color", cb_descs[0].first);
        dump_target("depth", db_desc.first);
        dreams_visibility_dump_dir.reset();
    }

    ResetBindings();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (RefreshDreamsProducerCount() && cs.pgm_hash == 0x2bfebd3c &&
        cs.user_data.size() >= 2) {
        const VAddr srt_base =
            (u64{cs.user_data[0]} | (u64{cs.user_data[1]} << 32)) & 0xFFFFFFFFFFFFULL;
        constexpr u64 CountOffset = 0x64;
        const VAddr count_address = srt_base + CountOffset;
        if (srt_base != 0 && memory->IsValidMapping(count_address, sizeof(u32))) {
            const u32 count_before =
                cs.flattened_ud_buf.size() > 41 ? cs.flattened_ud_buf[41] : 0;
            const bool gpu_modified =
                buffer_cache.IsRegionGpuModified(count_address, sizeof(u32));
            buffer_cache.ReadMemory(count_address, sizeof(u32));
            auto& mutable_cs = const_cast<Shader::Info&>(cs);
            mutable_cs.RefreshFlatBuf();
            const u32 count_after =
                mutable_cs.flattened_ud_buf.size() > 41 ? mutable_cs.flattened_ud_buf[41] : 0;
            static u32 readback_trace_count{};
            if (readback_trace_count++ < 32 || count_before != count_after) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams producer count readback address={:#x} gpu_modified={} "
                            "before={} after={}",
                            count_address, gpu_modified, count_before, count_after);
            }
        }
    }
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }
    TraceComputeShader(cs, false, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    if (ShouldSkipComputeShader(cs)) {
        return;
    }
    const u32 gpu_profile_ordinal = ConsumeDreamsGpuProfileEvent();
    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
    }
    const auto gpu_profile_start = std::chrono::steady_clock::now();

    if (TraceDreamsSceneStream() && cs.pgm_hash == 0xa35538cb) {
        static u32 scene_args_setup_trace_count{};
        const u32 scene_args_setup_ordinal = ++scene_args_setup_trace_count;
        if (scene_args_setup_ordinal <= 8 || scene_args_setup_ordinal % 300 == 0) {
            scheduler.Finish();
            const VAddr srt_base = cs.user_data.size() >= 2
                                       ? (u64{cs.user_data[0]} | (u64{cs.user_data[1]} << 32)) &
                                             0xFFFFFFFFFFFFULL
                                       : 0;
            std::array<u32, 13> raw_srt{};
            if (srt_base != 0 && memory->IsValidMapping(srt_base, sizeof(raw_srt))) {
                buffer_cache.ReadMemory(srt_base, sizeof(raw_srt));
                std::memcpy(raw_srt.data(), std::bit_cast<const void*>(srt_base),
                            sizeof(raw_srt));
            }
            const u32 flat_count =
                cs.flattened_ud_buf.size() > 28 ? cs.flattened_ud_buf[28] : 0;
            LOG_WARNING(Render_Vulkan,
                        "Dreams scene args setup #{} sequence={} srt={:#x} raw_count={} "
                        "flat_count={} flat_size={} buffers={}",
                        scene_args_setup_ordinal, g_compute_dispatch_sequence, srt_base,
                        raw_srt[12], flat_count, cs.flattened_ud_buf.size(), cs.buffers.size());
            for (u32 i = 0; i < cs.buffers.size(); ++i) {
                const auto& desc = cs.buffers[i];
                const auto sharp = desc.GetSharp(cs);
                LOG_WARNING(Render_Vulkan,
                            "Dreams scene args setup #{} buffer={} type={} special={} write={} "
                            "formatted={} base={:#x}+{:#x} stride={} records={}",
                            scene_args_setup_ordinal, i, static_cast<u32>(desc.buffer_type),
                            desc.IsSpecial(), desc.is_written, desc.is_formatted,
                            sharp.base_address, sharp.GetSize(), sharp.stride, sharp.num_records);
            }

            if (srt_base != 0) {
                const VAddr count_address = srt_base + 12 * sizeof(u32);
                const VAddr count_end = count_address + sizeof(u32);
                u32 writer_matches{};
                for (auto writer = g_dreams_buffer_writers.rbegin();
                     writer != g_dreams_buffer_writers.rend() && writer_matches < 32; ++writer) {
                    const u64 writer_end = writer->base + writer->size;
                    if (!writer->declared_write || writer->base >= count_end ||
                        count_address >= writer_end) {
                        continue;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams scene count writer #{} writer_seq={} dispatch={} kind={} "
                                "shader={:#x} stage={} binding={} range={:#x}+{:#x} stride={} "
                                "source={:#x}",
                                scene_args_setup_ordinal, writer->sequence,
                                writer->dispatch_sequence, static_cast<u32>(writer->kind),
                                writer->shader_hash, static_cast<u32>(writer->stage),
                                writer->binding_index, writer->base, writer->size, writer->stride,
                                writer->source);
                    ++writer_matches;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams scene count writer summary #{} address={:#x} matches={} "
                            "history={}",
                            scene_args_setup_ordinal, count_address, writer_matches,
                            g_dreams_buffer_writers.size());
            }
        }
    }

    if (TraceDreamsDiagnostics() && cs.pgm_hash == 0xd049fb84) {
        static u32 target_dispatch_count{};
        if (target_dispatch_count++ < 256) {
            const auto user_data = [&](u32 index) {
                return index < cs.user_data.size() ? cs.user_data[index] : 0;
            };
            const VAddr constants_address =
                (u64{user_data(2)} | (u64{user_data(3)} << 32)) & 0xFFFFFFFFFFFFULL;
            constexpr u64 ProbeSize = 12 * sizeof(u32);
            const bool mapped =
                constants_address != 0 && memory->IsValidMapping(constants_address, ProbeSize);
            const bool cpu_modified =
                mapped && buffer_cache.IsRegionCpuModified(constants_address, ProbeSize);
            const bool gpu_modified =
                mapped && buffer_cache.IsRegionGpuModified(constants_address, ProbeSize);
            std::array<u32, 12> constants{};
            if (mapped) {
                std::memcpy(constants.data(), std::bit_cast<const void*>(constants_address),
                            ProbeSize);
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams target dispatch={} sequence={} dims={}x{}x{} threads={} "
                        "ud0-7={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
                        "constants={:#x} mapped={} cpu_modified={} gpu_modified={} "
                        "c8-11={},{},{},{}",
                        target_dispatch_count, g_compute_dispatch_sequence, cs_program.dim_x,
                        cs_program.dim_y, cs_program.dim_z, cs_program.num_thread_x.full,
                        user_data(0), user_data(1), user_data(2), user_data(3), user_data(4),
                        user_data(5), user_data(6), user_data(7), constants_address, mapped,
                        cpu_modified, gpu_modified, constants[8], constants[9], constants[10],
                        constants[11]);
        }
    }

    const bool is_dreams_csg_replay_setup = cs.pgm_hash == 0x2c11357d;
    static u32 dreams_csg_replay_setup_trace_count{};
    const bool trace_dreams_csg_replay_setup =
        TraceDreamsCsgReplay() && is_dreams_csg_replay_setup &&
        dreams_csg_replay_setup_trace_count++ < 12;
    std::array<u32, 3> dreams_csg_args_pre{};
    std::array<u32, 11> dreams_csg_state_pre{};
    VAddr dreams_csg_args_address{};
    VAddr dreams_csg_state_address{};
    const auto read_dreams_csg_direct_state = [&](auto& args, auto& state) {
        if (cs.buffers.size() < 2) {
            return;
        }
        const auto args_sharp = cs.buffers[0].GetSharp(cs);
        dreams_csg_args_address = args_sharp.base_address;
        constexpr u64 StateOffset = 576 * sizeof(u32);
        if (args_sharp.base_address != 0 && args_sharp.GetSize() >= sizeof(args)) {
            buffer_cache.ReadMemory(args_sharp.base_address, sizeof(args));
            if (memory->IsValidMapping(args_sharp.base_address, sizeof(args))) {
                std::memcpy(args.data(), std::bit_cast<const void*>(args_sharp.base_address),
                            sizeof(args));
            }
        }
        if (cs.buffers[1].IsSpecial()) {
            const auto* gds = buffer_cache.GetGdsBuffer();
            dreams_csg_state_address = StateOffset;
            std::memcpy(state.data(), gds->mapped_data.data() + StateOffset, sizeof(state));
            return;
        }
        const auto state_sharp = cs.buffers[1].GetSharp(cs);
        dreams_csg_state_address = state_sharp.base_address;
        if (state_sharp.base_address != 0 &&
            state_sharp.GetSize() >= StateOffset + sizeof(state)) {
            const VAddr sample_address = state_sharp.base_address + StateOffset;
            buffer_cache.ReadMemory(sample_address, sizeof(state));
            if (memory->IsValidMapping(sample_address, sizeof(state))) {
                std::memcpy(state.data(), std::bit_cast<const void*>(sample_address),
                            sizeof(state));
            }
        }
    };
    if (trace_dreams_csg_replay_setup) {
        scheduler.Finish();
        read_dreams_csg_direct_state(dreams_csg_args_pre, dreams_csg_state_pre);
        LOG_WARNING(Render_Vulkan,
                    "Dreams CSG setup pre args={:#x} dims={}x{}x{} state={:#x} "
                    "d576-586={},{},{},{},{},{},{},{},{},{},{}",
                    dreams_csg_args_address, dreams_csg_args_pre[0], dreams_csg_args_pre[1],
                    dreams_csg_args_pre[2], dreams_csg_state_address, dreams_csg_state_pre[0],
                    dreams_csg_state_pre[1], dreams_csg_state_pre[2], dreams_csg_state_pre[3],
                    dreams_csg_state_pre[4], dreams_csg_state_pre[5], dreams_csg_state_pre[6],
                    dreams_csg_state_pre[7], dreams_csg_state_pre[8], dreams_csg_state_pre[9],
                    dreams_csg_state_pre[10]);
    }

    if (TraceDreamsReplayResult() && cs.pgm_hash == 0xd4d6c3e3 && !cs.buffers.empty() &&
        !cs.buffers[0].IsSpecial()) {
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        const u32 copy_count = std::min(flat(18), flat(20));
        const u32 gds_base_byte = flat(21);
        const auto sharp = cs.buffers[0].GetSharp(cs);
        const bool is_target = sharp.stride == sizeof(u32) && sharp.num_records == 45 &&
                               sharp.GetSize() == 45 * sizeof(u32) && copy_count == 45;
        if (is_target && (gds_base_byte & 3) == 0) {
            g_dreams_replay_progress_target_found = true;
            g_dreams_replay_progress_gds_index = gds_base_byte / sizeof(u32) + 7;
            ++g_dreams_replay_progress_cycle;
            g_dreams_replay_progress_cycle_observation_count = 0;
            if (g_dreams_replay_progress_cycle <= 8) {
                scheduler.Finish();
                std::array<u32, 12> input{};
                if (sharp.base_address != 0 && sharp.GetSize() >= sizeof(input) &&
                    memory->IsValidMapping(sharp.base_address, sizeof(input))) {
                    buffer_cache.ReadMemory(sharp.base_address, sizeof(input));
                    std::memcpy(input.data(), std::bit_cast<const void*>(sharp.base_address),
                                sizeof(input));
                }
                u32 gds_progress{};
                const auto* gds = buffer_cache.GetGdsBuffer();
                if (g_dreams_replay_progress_gds_index < gds->mapped_data.size() / sizeof(u32)) {
                    std::memcpy(&gds_progress,
                                gds->mapped_data.data() +
                                    g_dreams_replay_progress_gds_index * sizeof(u32),
                                sizeof(u32));
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams replay target upload cycle={} sequence={} flat18={} flat20={} "
                            "gds_byte={:#x} progress_index={} gds_progress={} input={:#x} "
                            "in0-11={},{},{},{},{},{},{},{},{},{},{},{}",
                            g_dreams_replay_progress_cycle, g_compute_dispatch_sequence, flat(18),
                            flat(20), gds_base_byte, g_dreams_replay_progress_gds_index,
                            gds_progress, sharp.base_address, input[0], input[1], input[2], input[3],
                            input[4], input[5], input[6], input[7], input[8], input[9], input[10],
                            input[11]);
            }
        }
    }

    struct DreamsCsgBuilderTrace {
        bool enabled{};
        u32 ordinal{};
        u64 dispatch_ordinal{};
        VAddr srt_base{};
        std::array<u32, 2> gds_before{};
    } dreams_csg_builder_trace{};
    static u32 dreams_csg_builder_trace_count{};
    static u64 dreams_csg_builder_dispatch_count{};
    static u32 dreams_csg_builder_last_input_count = std::numeric_limits<u32>::max();
    const bool is_dreams_csg_builder = cs.pgm_hash == 0x62a348b1;
    const u64 dreams_csg_builder_dispatch_ordinal =
        is_dreams_csg_builder ? ++dreams_csg_builder_dispatch_count : 0;
    const u32 dreams_csg_builder_input_count =
        is_dreams_csg_builder && cs.flattened_ud_buf.size() > 36 ? cs.flattened_ud_buf[36] : 0;
    const bool dreams_csg_builder_count_changed =
        is_dreams_csg_builder &&
        dreams_csg_builder_input_count != dreams_csg_builder_last_input_count;
    if (is_dreams_csg_builder) {
        dreams_csg_builder_last_input_count = dreams_csg_builder_input_count;
    }
    const bool trace_dreams_csg_builder =
        TraceDreamsCsgReplay() && is_dreams_csg_builder &&
        (dreams_csg_builder_dispatch_ordinal <= 4 || dreams_csg_builder_count_changed ||
         dreams_csg_builder_dispatch_ordinal % 600 == 0) &&
        dreams_csg_builder_trace_count < 20;
    if (trace_dreams_csg_builder) {
        dreams_csg_builder_trace.enabled = true;
        dreams_csg_builder_trace.ordinal = ++dreams_csg_builder_trace_count;
        dreams_csg_builder_trace.dispatch_ordinal = dreams_csg_builder_dispatch_ordinal;
        scheduler.Finish();

        const auto user_data = [&](u32 index) {
            return index < cs.user_data.size() ? cs.user_data[index] : 0;
        };
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        dreams_csg_builder_trace.srt_base =
            (u64{user_data(0)} | (u64{user_data(1)} << 32)) & 0xFFFFFFFFFFFFULL;
        std::array<u32, 28> raw_srt{};
        if (dreams_csg_builder_trace.srt_base != 0 &&
            memory->IsValidMapping(dreams_csg_builder_trace.srt_base, sizeof(raw_srt))) {
            buffer_cache.ReadMemory(dreams_csg_builder_trace.srt_base, sizeof(raw_srt));
            std::memcpy(raw_srt.data(),
                        std::bit_cast<const void*>(dreams_csg_builder_trace.srt_base),
                        sizeof(raw_srt));
        }
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&dreams_csg_builder_trace.gds_before[0],
                    gds->mapped_data.data() + 14 * sizeof(u32), sizeof(u32));
        std::memcpy(&dreams_csg_builder_trace.gds_before[1],
                    gds->mapped_data.data() + 18 * sizeof(u32), sizeof(u32));
        LOG_WARNING(
            Render_Vulkan,
            "Dreams CSG builder pre #{} dispatch={} sequence={} dims={}x{}x{} threads={} "
            "srt={:#x} "
            "user_data={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} flat_size={} "
            "raw4-15={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
            "raw24-27={:#x},{:#x},{:#x},{:#x} "
            "flat16-25={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
            "flat36-37={},{} gds14={} gds18={}",
            dreams_csg_builder_trace.ordinal, dreams_csg_builder_trace.dispatch_ordinal,
            g_compute_dispatch_sequence, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
            cs_program.num_thread_x.full,
            dreams_csg_builder_trace.srt_base, user_data(0), user_data(1), user_data(2),
            user_data(3), user_data(4), user_data(5), user_data(6), user_data(7),
            cs.flattened_ud_buf.size(), raw_srt[4], raw_srt[5], raw_srt[6], raw_srt[7],
            raw_srt[8], raw_srt[9], raw_srt[10], raw_srt[11], raw_srt[12], raw_srt[13],
            raw_srt[14], raw_srt[15], raw_srt[24], raw_srt[25], raw_srt[26], raw_srt[27],
            flat(16), flat(17), flat(18), flat(19), flat(20), flat(21), flat(22), flat(23),
            flat(24), flat(25), flat(36), flat(37),
            dreams_csg_builder_trace.gds_before[0], dreams_csg_builder_trace.gds_before[1]);

        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            const auto sharp = desc.GetSharp(cs);
            if (desc.IsSpecial()) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams CSG builder buffer #{} index={} special=true type={} write={}",
                            dreams_csg_builder_trace.ordinal, index,
                            static_cast<u32>(desc.buffer_type), desc.is_written);
                continue;
            }
            const u64 scan_limit = index >= 2 ? 8_MB : 1_MB;
            const u64 scan_size = std::min<u64>(sharp.GetSize(), scan_limit) & ~u64{3};
            const bool mapped = sharp.base_address != 0 && scan_size != 0 &&
                                memory->IsValidMapping(sharp.base_address, scan_size);
            if (mapped) {
                buffer_cache.ReadMemory(sharp.base_address, scan_size);
            }
            u32 nonzero{};
            u32 first_nonzero = std::numeric_limits<u32>::max();
            u32 first_value{};
            std::array<u32, 12> head{};
            if (mapped) {
                const auto* words = std::bit_cast<const u32*>(sharp.base_address);
                std::memcpy(head.data(), words, std::min<u64>(sizeof(head), scan_size));
                for (u32 word = 0; word < scan_size / sizeof(u32); ++word) {
                    if (words[word] == 0) {
                        continue;
                    }
                    ++nonzero;
                    if (first_nonzero == std::numeric_limits<u32>::max()) {
                        first_nonzero = word;
                        first_value = words[word];
                    }
                }
            }
            LOG_WARNING(
                Render_Vulkan,
                "Dreams CSG builder buffer #{} index={} write={} base={:#x}+{:#x} stride={} "
                "records={} mapped={} scan={:#x} nonzero={} first={} value={:#x} "
                "head={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                dreams_csg_builder_trace.ordinal, index, desc.is_written, sharp.base_address,
                sharp.GetSize(), sharp.stride, sharp.num_records, mapped, scan_size, nonzero,
                first_nonzero, first_value, head[0], head[1], head[2], head[3], head[4], head[5],
                head[6], head[7], head[8], head[9], head[10], head[11]);
            if (index >= 2 && TraceDreamsBufferDependencies()) {
                u32 matches{};
                for (auto writer = g_dreams_buffer_writers.rbegin();
                     writer != g_dreams_buffer_writers.rend() && matches < 32; ++writer) {
                    if (!writer->declared_write ||
                        !OverlapsDreamsRange(sharp.base_address, sharp.GetSize(), writer->base,
                                            writer->size)) {
                        continue;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams CSG builder input #{} index={} writer={} sequence={} "
                                "dispatch={} shader={:#x} kind={} binding={} base={:#x}+{:#x} "
                                "stride={} source={:#x}",
                                dreams_csg_builder_trace.ordinal, index, matches, writer->sequence,
                                writer->dispatch_sequence, writer->shader_hash,
                                static_cast<u32>(writer->kind), writer->binding_index, writer->base,
                                writer->size, writer->stride, writer->source);
                    ++matches;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams CSG builder input #{} index={} writer_matches={} history={}",
                            dreams_csg_builder_trace.ordinal, index, matches,
                            g_dreams_buffer_writers.size());
            }
        }
    }

    const bool is_dreams_producer =
        Common::ElfInfo::Instance().GameSerial() == "CUSA04301" && cs.pgm_hash == 0x2bfebd3c;
    const u32 dreams_producer_record_count =
        is_dreams_producer && cs.flattened_ud_buf.size() > 41 ? cs.flattened_ud_buf[41] : 0;
    if (is_dreams_producer && ConsumeDreamsProducerInputTraceRequest()) {
        scheduler.Finish();
        const VAddr srt_base = cs.user_data.size() >= 2
                                   ? (u64{cs.user_data[0]} | (u64{cs.user_data[1]} << 32)) &
                                         0xFFFFFFFFFFFFULL
                                   : 0;
        std::array<u32, 32> raw_srt{};
        if (srt_base != 0 && memory->IsValidMapping(srt_base, sizeof(raw_srt))) {
            buffer_cache.ReadMemory(srt_base, sizeof(raw_srt));
            std::memcpy(raw_srt.data(), std::bit_cast<const void*>(srt_base), sizeof(raw_srt));
        }
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        LOG_WARNING(Render_Vulkan,
                    "Dreams producer input snapshot generation={} sequence={} srt={:#x} "
                    "records={} raw20-31={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},"
                    "{:#x},{:#x},{:#x},{:#x},{:#x} flat36-45={:#x},{:#x},{:#x},{:#x},"
                    "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    g_dreams_scene_graphics_probe_generation, g_compute_dispatch_sequence, srt_base,
                    dreams_producer_record_count, raw_srt[20], raw_srt[21], raw_srt[22],
                    raw_srt[23], raw_srt[24], raw_srt[25], raw_srt[26], raw_srt[27], raw_srt[28],
                    raw_srt[29], raw_srt[30], raw_srt[31], flat(36), flat(37), flat(38), flat(39),
                    flat(40), flat(41), flat(42), flat(43), flat(44), flat(45));

        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            const auto sharp = desc.GetSharp(cs);
            LOG_WARNING(Render_Vulkan,
                        "Dreams producer input resource buffer={} special={} write={} type={} "
                        "base={:#x}+{:#x} stride={} records={}",
                        index, desc.IsSpecial(), desc.is_written,
                        static_cast<u32>(desc.buffer_type), sharp.base_address, sharp.GetSize(),
                        sharp.stride, sharp.num_records);
            if ((index < 2 || index > 4) || desc.IsSpecial() || sharp.base_address == 0 ||
                sharp.stride == 0) {
                continue;
            }
            const u64 scan_size = std::min<u64>(sharp.GetSize(), 16_MB) & ~u64{3};
            if (scan_size == 0 || !memory->IsValidMapping(sharp.base_address, scan_size)) {
                continue;
            }
            buffer_cache.ReadMemory(sharp.base_address, scan_size);
            const auto* words = std::bit_cast<const u32*>(sharp.base_address);
            const u32 dwords = static_cast<u32>(scan_size / sizeof(u32));
            const u32 stride_dwords = std::max<u32>(sharp.stride / sizeof(u32), 1);
            const u32 records = std::min<u32>(sharp.num_records, dwords / stride_dwords);
            u64 nonzero_dwords{};
            u32 nonzero_records{};
            u32 first_dword = std::numeric_limits<u32>::max();
            u32 first_value{};
            u32 samples{};
            for (u32 word = 0; word < dwords; ++word) {
                if (words[word] == 0) {
                    continue;
                }
                ++nonzero_dwords;
                if (first_dword == std::numeric_limits<u32>::max()) {
                    first_dword = word;
                    first_value = words[word];
                }
            }
            for (u32 record = 0; record < records; ++record) {
                const u32 start = record * stride_dwords;
                bool nonzero{};
                for (u32 word = 0; word < stride_dwords && start + word < dwords; ++word) {
                    nonzero |= words[start + word] != 0;
                }
                if (!nonzero) {
                    continue;
                }
                ++nonzero_records;
                if (samples++ < 4) {
                    std::array<u32, 12> sample{};
                    for (u32 word = 0; word < sample.size() && word < stride_dwords; ++word) {
                        sample[word] = words[start + word];
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams producer input sample buffer={} record={} values={:#x},"
                                "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},"
                                "{:#x},{:#x}",
                                index, record, sample[0], sample[1], sample[2], sample[3],
                                sample[4], sample[5], sample[6], sample[7], sample[8], sample[9],
                                sample[10], sample[11]);
                }
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams producer input scan buffer={} bytes={:#x} dwords={} nonzero_dw={} "
                        "first_dw={} first_value={:#x} records={} nonzero_records={}",
                        index, scan_size, dwords, nonzero_dwords, first_dword, first_value, records,
                        nonzero_records);
        }
        LOG_WARNING(Render_Vulkan, "Dreams producer input snapshot complete");
    }
    if (is_dreams_producer && TraceDreamsProducerSources() && cs.buffers.size() > 4) {
        const VAddr srt_base = cs.user_data.size() >= 2
                                   ? (u64{cs.user_data[0]} | (u64{cs.user_data[1]} << 32)) &
                                         0xFFFFFFFFFFFFULL
                                   : 0;
        constexpr u64 WatchPageSize = 64_KB;
        RegisterDreamsCpuWriteWatch((srt_base + 0x64) & ~(WatchPageSize - 1), WatchPageSize, 1);
        const auto records = cs.buffers[3].GetSharp(cs);
        const auto aux = cs.buffers[4].GetSharp(cs);
        RegisterDreamsCpuWriteWatch(records.base_address, records.GetSize(), 2);
        RegisterDreamsCpuWriteWatch(aux.base_address, aux.GetSize(), 3);
        LogDreamsCpuWriteWatches();
    }
    if (is_dreams_producer && TraceDreamsProducerSources() && cs.buffers.size() > 4) {
        static u32 source_trace_count{};
        const u32 source_trace_ordinal = ++source_trace_count;
        if (source_trace_ordinal <= 8 || source_trace_ordinal % 600 == 0) {
            const VAddr srt_base = cs.user_data.size() >= 2
                                       ? (u64{cs.user_data[0]} | (u64{cs.user_data[1]} << 32)) &
                                             0xFFFFFFFFFFFFULL
                                       : 0;
            std::array<u32, 30> raw_srt{};
            const bool srt_mapped =
                srt_base != 0 && memory->IsValidMapping(srt_base, sizeof(raw_srt));
            if (srt_mapped) {
                std::memcpy(raw_srt.data(), std::bit_cast<const void*>(srt_base), sizeof(raw_srt));
            }
            const auto flat_value = [&](u32 index) {
                return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
            };
            LOG_WARNING(Render_Vulkan,
                        "Dreams source state dispatch={} srt={:#x} mapped={} records={} "
                        "raw23-29={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
                        "flat39-45={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                        g_compute_dispatch_sequence, srt_base, srt_mapped,
                        dreams_producer_record_count, raw_srt[23], raw_srt[24], raw_srt[25],
                        raw_srt[26], raw_srt[27], raw_srt[28], raw_srt[29], flat_value(39),
                        flat_value(40), flat_value(41), flat_value(42), flat_value(43),
                        flat_value(44), flat_value(45));
            if (source_trace_ordinal == 1) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams pre-producer binding history entries={}",
                            g_dreams_buffer_writers.size());
                for (const auto& binding : g_dreams_buffer_writers) {
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams pre-producer binding seq={} dispatch={} shader={:#x} stage={} "
                        "kind={} binding={} write={} base={:#x}+{:#x} stride={} source={:#x}",
                        binding.sequence, binding.dispatch_sequence, binding.shader_hash,
                        static_cast<u32>(binding.stage), static_cast<u32>(binding.kind),
                        binding.binding_index, binding.declared_write, binding.base, binding.size,
                        binding.stride, binding.source);
                }
            }
            const auto trace_target = [&](std::string_view name, VAddr target_base,
                                          u64 target_size) {
                if (target_base == 0 || target_size == 0) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams source target={} base={:#x} size={:#x} unavailable",
                                name, target_base, target_size);
                    return;
                }

                const u64 target_end = target_base + target_size;
                const bool mapped = memory->IsValidMapping(target_base, target_size);
                const bool cpu_modified =
                    mapped && buffer_cache.IsRegionCpuModified(target_base, target_size);
                const bool gpu_modified =
                    mapped && buffer_cache.IsRegionGpuModified(target_base, target_size);
                u32 matches{};
                for (auto writer = g_dreams_buffer_writers.rbegin();
                     writer != g_dreams_buffer_writers.rend() && matches < 32; ++writer) {
                    const u64 writer_end = writer->base + writer->size;
                    if (!writer->declared_write || writer->base >= target_end ||
                        target_base >= writer_end) {
                        continue;
                    }
                    const u64 overlap_base = std::max<u64>(target_base, writer->base);
                    const u64 overlap_end = std::min<u64>(target_end, writer_end);
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams source target={} base={:#x}+{:#x} writer_seq={} dispatch={} "
                        "kind={} shader={:#x} stage={} binding={} write={} range={:#x}+{:#x} "
                        "stride={} "
                        "source={:#x} overlap={:#x}+{:#x}",
                        name, target_base, target_size, writer->sequence,
                        writer->dispatch_sequence, static_cast<u32>(writer->kind),
                        writer->shader_hash, static_cast<u32>(writer->stage), writer->binding_index,
                        writer->declared_write, writer->base, writer->size, writer->stride,
                        writer->source, overlap_base, overlap_end - overlap_base);
                    ++matches;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams source summary target={} base={:#x}+{:#x} mapped={} "
                            "cpu_modified={} gpu_modified={} matches={} history={}",
                            name, target_base, target_size, mapped, cpu_modified, gpu_modified,
                            matches, g_dreams_buffer_writers.size());
            };

            trace_target("count", srt_base + 0x64, sizeof(u32));
            for (u32 index : {3U, 4U}) {
                const auto target = cs.buffers[index].GetSharp(cs);
                trace_target(index == 3 ? "records" : "aux", target.base_address,
                             target.GetSize());
            }
        }
    }
    static u32 dreams_producer_sync_count{};
    const bool trace_dreams_producer_sync =
        is_dreams_producer && TraceDreamsOrderedCounters() && dreams_producer_sync_count++ < 16;
    if (trace_dreams_producer_sync) {
        scheduler.Finish();
        std::array<u32, 2> queue_state{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(queue_state.data(),
                    gds->mapped_data.data() +
                        Shader::DreamsCompat::QueueProducerCounterIndex * sizeof(u32),
                    sizeof(queue_state));
        u32 compact_count{};
        std::memcpy(&compact_count, gds->mapped_data.data(), sizeof(compact_count));
        const auto flat_value = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        LOG_WARNING(Render_Vulkan,
                    "Dreams producer synchronized pre sequence={} gds0={} ordered508={} "
                    "ordered509={} "
                    "f24-30={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    g_compute_dispatch_sequence, compact_count, queue_state[0], queue_state[1],
                    flat_value(24), flat_value(25), flat_value(26), flat_value(27), flat_value(28),
                    flat_value(29), flat_value(30));
    }
    if (is_dreams_producer &&
        (TraceDreamsDiagnostics() || TraceDreamsOrderedCounters())) {
        static u64 dreams_producer_dispatch_count{};
        static u32 last_dreams_producer_record_count = std::numeric_limits<u32>::max();
        const u64 dispatch_count = ++dreams_producer_dispatch_count;
        const u64 srt_base = cs.user_data.size() >= 2
                                 ? u64{cs.user_data[0]} | (u64{cs.user_data[1]} << 32)
                                 : 0;
        if (dispatch_count <= 16 || dreams_producer_record_count != last_dreams_producer_record_count ||
            dispatch_count % 120 == 0) {
            const auto flat_value = [&](u32 index) {
                return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
            };
            LOG_WARNING(Render_Vulkan,
                        "Dreams producer frame={} records={} srt={:#x} count_addr={:#x} "
                        "c24-29={:#x},{:#x},{:#x},{:#x},"
                        "{:#x},{:#x}",
                        dispatch_count, dreams_producer_record_count, srt_base, srt_base + 0x64,
                        flat_value(40), flat_value(41), flat_value(42), flat_value(43),
                        flat_value(44), flat_value(45));
        }
        last_dreams_producer_record_count = dreams_producer_record_count;
    }

    struct DreamsTransferBufferSnapshot {
        u32 index{};
        VAddr base{};
        u64 size{};
        u32 stride{};
        u32 records{};
        bool declared_write{};
        std::vector<u32> words{};
    };
    struct DreamsTransferTrace {
        bool enabled{};
        u64 shader_hash{};
        std::vector<DreamsTransferBufferSnapshot> buffers{};
    } dreams_transfer_trace{};
    const auto capture_dreams_transfer_buffer = [&](u32 index, u64 max_bytes) {
        if (index >= cs.buffers.size() || cs.buffers[index].IsSpecial()) {
            return;
        }
        const auto& desc = cs.buffers[index];
        const auto sharp = desc.GetSharp(cs);
        const u64 read_size = std::min<u64>(sharp.GetSize(), max_bytes) & ~u64{3};
        if (sharp.base_address == 0 || read_size == 0 ||
            !memory->IsValidMapping(sharp.base_address, read_size)) {
            return;
        }
        buffer_cache.ReadMemory(sharp.base_address, read_size);
        auto& snapshot = dreams_transfer_trace.buffers.emplace_back();
        snapshot.index = index;
        snapshot.base = sharp.base_address;
        snapshot.size = sharp.GetSize();
        snapshot.stride = sharp.stride;
        snapshot.records = sharp.num_records;
        snapshot.declared_write = desc.is_written;
        snapshot.words.resize(read_size / sizeof(u32));
        std::memcpy(snapshot.words.data(), std::bit_cast<const void*>(sharp.base_address),
                    read_size);
    };

    static u32 dreams_scene_target_probe_budget{};
    static std::vector<u32> dreams_scene_gds_before{};
    static bool captured_dreams_scene_gds{};
    if (TraceDreamsOrderedCounters() &&
        cs.pgm_hash == Shader::DreamsCompat::SceneCompactShader) {
        scheduler.Finish();
        dreams_transfer_trace.enabled = true;
        dreams_transfer_trace.shader_hash = cs.pgm_hash;
        capture_dreams_transfer_buffer(0, 8_MB);
        dreams_scene_target_probe_budget = 512;
        g_dreams_scene_graphics_probe_budget = 32768;
        ++g_dreams_scene_graphics_probe_generation;
        g_dreams_scene_graphics_relevant_draw_count = 0;
        g_dreams_scene_graphics_pipelines.clear();
        g_dreams_scene_graphics_bindings.clear();
        g_dreams_scene_generated_ranges.fill({});
        g_dreams_scene_generated_range_count = 0;
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene graphics probe armed generation={} budget={}",
                    g_dreams_scene_graphics_probe_generation,
                    g_dreams_scene_graphics_probe_budget);
        const auto* gds = buffer_cache.GetGdsBuffer();
        dreams_scene_gds_before.resize(gds->mapped_data.size() / sizeof(u32));
        std::memcpy(dreams_scene_gds_before.data(), gds->mapped_data.data(),
                    dreams_scene_gds_before.size() * sizeof(u32));
        captured_dreams_scene_gds = true;
    }
    const bool trace_dreams_scene_target =
        TraceDreamsOrderedCounters() && cs.pgm_hash == 0xd049fb84 &&
        dreams_scene_target_probe_budget != 0;
    if (dreams_scene_target_probe_budget != 0 &&
        cs.pgm_hash != Shader::DreamsCompat::SceneCompactShader) {
        --dreams_scene_target_probe_budget;
    }
    if (trace_dreams_scene_target) {
        scheduler.Finish();
        dreams_transfer_trace.enabled = true;
        dreams_transfer_trace.shader_hash = cs.pgm_hash;
        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            if (cs.buffers[index].is_written) {
                capture_dreams_transfer_buffer(index, 8_MB);
            }
        }
        dreams_scene_target_probe_budget = 0;
    }

    static bool dumped_dreams_producer_pre = false;
    if (TraceDreamsDiagnostics() && !dumped_dreams_producer_pre &&
        is_dreams_producer && cs.buffers.size() > 4) {
        dumped_dreams_producer_pre = true;

        constexpr std::array<u32, 12> FlatIndices{18, 22, 36, 37, 38, 39,
                                                   40, 41, 42, 43, 44, 45};
        std::array<u32, FlatIndices.size()> flat_values{};
        for (u32 i = 0; i < FlatIndices.size(); ++i) {
            if (FlatIndices[i] < cs.flattened_ud_buf.size()) {
                flat_values[i] = cs.flattened_ud_buf[FlatIndices[i]];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams producer pre flat_size={} f18={:#x} f22={:#x} "
                    "f36-45={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    cs.flattened_ud_buf.size(), flat_values[0], flat_values[1], flat_values[2],
                    flat_values[3], flat_values[4], flat_values[5], flat_values[6], flat_values[7],
                    flat_values[8], flat_values[9], flat_values[10], flat_values[11]);

        const auto user_data_value = [&](u32 index) {
            return index < cs.user_data.size() ? cs.user_data[index] : 0;
        };
        LOG_WARNING(Render_Vulkan,
                    "Dreams producer user_data size={} values={:#x},{:#x},{:#x},{:#x},"
                    "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},"
                    "{:#x},{:#x} walker={:#x} walker_size={}",
                    cs.user_data.size(), user_data_value(0), user_data_value(1),
                    user_data_value(2), user_data_value(3), user_data_value(4),
                    user_data_value(5), user_data_value(6), user_data_value(7),
                    user_data_value(8), user_data_value(9), user_data_value(10),
                    user_data_value(11), user_data_value(12), user_data_value(13),
                    user_data_value(14), user_data_value(15),
                    reinterpret_cast<uintptr_t>(cs.srt_info.walker_func),
                    cs.srt_info.walker_func_size);

        constexpr std::array<u32, 8> GdsIndices{0, 1, 2, 4, 6, 320, 321,
                                                DreamsTraversalCompletionOffset / sizeof(u32)};
        std::array<u32, GdsIndices.size()> gds_pre{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        for (u32 i = 0; i < GdsIndices.size(); ++i) {
            std::memcpy(&gds_pre[i], gds->mapped_data.data() + GdsIndices[i] * sizeof(u32),
                        sizeof(u32));
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams producer pre dims={}x{}x{} threads={} GDS0={} GDS1={} GDS2={} "
                    "GDS4={} GDS6={} GDS320={} GDS321={} completion={}",
                    cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                    cs_program.num_thread_x.full, gds_pre[0], gds_pre[1], gds_pre[2], gds_pre[3],
                    gds_pre[4], gds_pre[5], gds_pre[6], gds_pre[7]);

        struct CpuBufferScan {
            bool mapped{};
            bool cpu_modified{};
            bool gpu_modified{};
            bool scanned{};
            u64 nonzero_dwords{};
            u32 first_dword{std::numeric_limits<u32>::max()};
            u32 first_value{};
            u32 first_record{std::numeric_limits<u32>::max()};
        };
        const auto scan_cpu_buffer = [&](u32 index) {
            CpuBufferScan scan{};
            const auto buffer = cs.buffers[index].GetSharp(cs);
            if (cs.buffers[index].IsSpecial() || buffer.base_address == 0) {
                return scan;
            }
            const u64 size = static_cast<u64>(buffer.GetSize()) & ~u64{3};
            if (size == 0) {
                return scan;
            }
            scan.mapped = memory->IsValidMapping(buffer.base_address, size);
            scan.cpu_modified = buffer_cache.IsRegionCpuModified(buffer.base_address, size);
            scan.gpu_modified = buffer_cache.IsRegionGpuModified(buffer.base_address, size);
            if (!scan.mapped) {
                return scan;
            }
            if (scan.gpu_modified) {
                // Diagnostics need the authoritative GPU contents, not the stale guest mirror.
                buffer_cache.ReadMemory(buffer.base_address, size);
                scan.cpu_modified = buffer_cache.IsRegionCpuModified(buffer.base_address, size);
                scan.gpu_modified = buffer_cache.IsRegionGpuModified(buffer.base_address, size);
            }
            if (scan.gpu_modified) {
                return scan;
            }
            scan.scanned = true;
            const auto* words = std::bit_cast<const u32*>(buffer.base_address);
            const u32 num_dwords = static_cast<u32>(size / sizeof(u32));
            const u32 stride_dwords = std::max<u32>(buffer.stride / sizeof(u32), 1);
            for (u32 i = 0; i < num_dwords; ++i) {
                if (words[i] == 0) {
                    continue;
                }
                ++scan.nonzero_dwords;
                if (scan.first_dword == std::numeric_limits<u32>::max()) {
                    scan.first_dword = i;
                    scan.first_value = words[i];
                    scan.first_record = i / stride_dwords;
                }
            }
            return scan;
        };
        for (u32 index = 1; index <= 4; ++index) {
            const auto buffer = cs.buffers[index].GetSharp(cs);
            const auto scan = scan_cpu_buffer(index);
            LOG_WARNING(Render_Vulkan,
                        "Dreams producer pre buffer={} base={:#x} size={:#x} stride={} "
                        "mapped={} cpu_modified={} gpu_modified={} scanned={} nonzero_dw={} "
                        "first_dw={} value={:#x} first_record={}",
                        index, buffer.base_address, buffer.GetSize(), buffer.stride, scan.mapped,
                        scan.cpu_modified, scan.gpu_modified, scan.scanned, scan.nonzero_dwords,
                        scan.first_dword, scan.first_value, scan.first_record);
        }

        if (TraceDreamsBufferDependencies()) {
            for (u32 index : {3U, 4U}) {
                const auto target = cs.buffers[index].GetSharp(cs);
                const u64 target_end = target.base_address + target.GetSize();
                u32 matches{};
                for (auto writer = g_dreams_buffer_writers.rbegin();
                     writer != g_dreams_buffer_writers.rend() && matches < 64; ++writer) {
                    const u64 writer_end = writer->base + writer->size;
                    if (writer->base >= target_end || target.base_address >= writer_end) {
                        continue;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams dependency input={} target={:#x}+{:#x} writer_seq={} "
                                "shader={:#x} stage={} range={:#x}+{:#x} stride={}",
                                index, target.base_address, target.GetSize(), writer->sequence,
                                writer->shader_hash, static_cast<u32>(writer->stage), writer->base,
                                writer->size, writer->stride);
                    ++matches;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams dependency input={} target={:#x}+{:#x} matches={} history={}",
                            index, target.base_address, target.GetSize(), matches,
                            g_dreams_buffer_writers.size());
            }
        }
    }

    struct DreamsSceneStreamDispatchTrace {
        bool enabled{};
        u32 ordinal{};
        u32 counter_before{};
        VAddr output_base{};
        u64 output_size{};
        u32 output_stride{};
    } scene_stream_trace{};
    static u32 dreams_scene_stream_trace_count{};
    if (TraceDreamsSceneStream() && cs.pgm_hash == 0x34267ace &&
        dreams_scene_stream_trace_count < 8 && cs.buffers.size() > 5) {
        scene_stream_trace.enabled = true;
        scene_stream_trace.ordinal = ++dreams_scene_stream_trace_count;
        const auto output = cs.buffers[0].GetSharp(cs);
        scene_stream_trace.output_base = output.base_address;
        scene_stream_trace.output_size = output.GetSize();
        scene_stream_trace.output_stride = output.stride;

        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&scene_stream_trace.counter_before,
                    gds->mapped_data.data() + 2 * sizeof(u32), sizeof(u32));
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene stream pre #{} sequence={} dims={}x{}x{} threads={} "
                    "counter={} output={:#x}+{:#x} stride={} buffers={} images={}",
                    scene_stream_trace.ordinal, g_compute_dispatch_sequence, cs_program.dim_x,
                    cs_program.dim_y, cs_program.dim_z, cs_program.num_thread_x.full,
                    scene_stream_trace.counter_before, scene_stream_trace.output_base,
                        scene_stream_trace.output_size, scene_stream_trace.output_stride,
                        cs.buffers.size(), cs.images.size());
    }

    struct DreamsVisibilityCountDirectTrace {
        bool enabled{};
        u32 ordinal{};
        VAddr output_base{};
        u64 output_size{};
        std::array<u32, 25> output_before{};
        std::array<u32, 21> gds_before{};
    } visibility_count_trace{};
    static u32 dreams_seed_count_trace_ordinal{};
    static u32 dreams_args_init_trace_ordinal{};
    static u32 dreams_primary_finalize_trace_ordinal{};
    static u32 dreams_secondary_seed_trace_ordinal{};
    static u32 dreams_secondary_finalize_trace_ordinal{};
    u32* visibility_count_ordinal{};
    if (TraceDreamsVisibilityCounts()) {
        if (cs.pgm_hash == Shader::DreamsCompat::QueueProducerShader &&
            ConsumeDreamsVisibilityCountTraceRequest()) {
            g_dreams_visibility_count_trace_active = true;
            LOG_WARNING(Render_Vulkan, "Dreams visibility count one-shot trace armed");
        }
        switch (cs.pgm_hash) {
        case Shader::DreamsCompat::QueueProducerShader:
            visibility_count_ordinal = &dreams_seed_count_trace_ordinal;
            break;
        case 0x86b79c2a:
            visibility_count_ordinal = &dreams_args_init_trace_ordinal;
            break;
        case 0x19c58242:
            visibility_count_ordinal = &dreams_primary_finalize_trace_ordinal;
            break;
        case 0x19b50176:
            visibility_count_ordinal = &dreams_secondary_seed_trace_ordinal;
            break;
        case 0x76197e85:
            visibility_count_ordinal = &dreams_secondary_finalize_trace_ordinal;
            break;
        default:
            break;
        }
    }
    if (visibility_count_ordinal != nullptr) {
        const u32 ordinal = ++*visibility_count_ordinal;
        if (g_dreams_visibility_count_trace_active || ordinal <= 8 || ordinal % 120 == 0) {
            visibility_count_trace.enabled = true;
            visibility_count_trace.ordinal = ordinal;
            scheduler.Finish();
            const auto* gds = buffer_cache.GetGdsBuffer();
            std::memcpy(visibility_count_trace.gds_before.data(),
                        gds->mapped_data.data() + 320 * sizeof(u32),
                        sizeof(visibility_count_trace.gds_before));
            if (!cs.buffers.empty() && !cs.buffers[0].IsSpecial()) {
                const auto output = cs.buffers[0].GetSharp(cs);
                visibility_count_trace.output_base = output.base_address;
                visibility_count_trace.output_size = output.GetSize();
                if (output.base_address != 0 &&
                    output.GetSize() >= sizeof(visibility_count_trace.output_before)) {
                    buffer_cache.ReadMemory(output.base_address,
                                            sizeof(visibility_count_trace.output_before));
                    if (memory->IsValidMapping(output.base_address,
                                               sizeof(visibility_count_trace.output_before))) {
                        std::memcpy(visibility_count_trace.output_before.data(),
                                    std::bit_cast<const void*>(output.base_address),
                                    sizeof(visibility_count_trace.output_before));
                    }
                }
            }
            const auto flat = [&](u32 index) {
                return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
            };
            LOG_WARNING(
                Render_Vulkan,
                "Dreams visibility count direct pre hash={:#x} ordinal={} sequence={} "
                "dims={}x{}x{} flat25={} flat41={} output={:#x}+{:#x} "
                "out0-8={},{},{},{},{},{},{},{},{} gds320-330={},{},{},{},{},{},{},{},{},{},{}",
                cs.pgm_hash, ordinal, g_compute_dispatch_sequence, cs_program.dim_x,
                cs_program.dim_y, cs_program.dim_z, flat(25), flat(41),
                visibility_count_trace.output_base, visibility_count_trace.output_size,
                visibility_count_trace.output_before[0], visibility_count_trace.output_before[1],
                visibility_count_trace.output_before[2], visibility_count_trace.output_before[3],
                visibility_count_trace.output_before[4], visibility_count_trace.output_before[5],
                visibility_count_trace.output_before[6], visibility_count_trace.output_before[7],
                visibility_count_trace.output_before[8], visibility_count_trace.gds_before[0],
                visibility_count_trace.gds_before[1], visibility_count_trace.gds_before[2],
                visibility_count_trace.gds_before[3], visibility_count_trace.gds_before[4],
                visibility_count_trace.gds_before[5], visibility_count_trace.gds_before[6],
                visibility_count_trace.gds_before[7], visibility_count_trace.gds_before[8],
                visibility_count_trace.gds_before[9], visibility_count_trace.gds_before[10]);

            if (cs.pgm_hash == Shader::DreamsCompat::QueueProducerShader &&
                (g_dreams_visibility_count_trace_active || ordinal <= 8 || ordinal % 600 == 0) &&
                cs.buffers.size() > 4) {
                const auto records = cs.buffers[3].GetSharp(cs);
                const auto aux = cs.buffers[4].GetSharp(cs);
                SetDreamsVisibilityInputRanges(
                    {.base = records.base_address, .size = records.GetSize()},
                    {.base = aux.base_address, .size = aux.GetSize()});
                for (const u32 buffer_index : {3U, 4U}) {
                    if (cs.buffers[buffer_index].IsSpecial()) {
                        continue;
                    }
                    const auto input = cs.buffers[buffer_index].GetSharp(cs);
                    const u64 read_size = std::min<u64>(input.GetSize(), 8_MB);
                    if (input.base_address == 0 || read_size < sizeof(u32) ||
                        !memory->IsValidMapping(input.base_address, read_size)) {
                        continue;
                    }

                    const bool gpu_modified_before =
                        buffer_cache.IsRegionGpuModified(input.base_address, read_size);
                    buffer_cache.ReadMemory(input.base_address, read_size);
                    const auto* words = std::bit_cast<const u32*>(input.base_address);
                    const u32 word_count = static_cast<u32>(read_size / sizeof(u32));
                    u32 nonzero_words{};
                    u32 first_nonzero = std::numeric_limits<u32>::max();
                    u32 first_value{};
                    u64 hash = 1469598103934665603ULL;
                    for (u32 word = 0; word < word_count; ++word) {
                        hash ^= words[word];
                        hash *= 1099511628211ULL;
                        if (words[word] == 0) {
                            continue;
                        }
                        ++nonzero_words;
                        if (first_nonzero == std::numeric_limits<u32>::max()) {
                            first_nonzero = word;
                            first_value = words[word];
                        }
                    }
                    const u32 stride_words = std::max<u32>(input.stride / sizeof(u32), 1);
                    const u32 first_record = first_nonzero == std::numeric_limits<u32>::max()
                                                 ? std::numeric_limits<u32>::max()
                                                 : first_nonzero / stride_words;
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams visibility seed input hash={:#x} ordinal={} buffer={} "
                        "base={:#x}+{:#x} stride={} read={:#x} gpu_before={} nonzero={} "
                        "first={} record={} value={:#x} hash={:#x} "
                        "head={:#x},{:#x},{:#x},{:#x},"
                        "{:#x},{:#x},{:#x},{:#x}",
                        cs.pgm_hash, ordinal, buffer_index, input.base_address, input.GetSize(),
                        input.stride, read_size, gpu_modified_before, nonzero_words,
                        first_nonzero, first_record, first_value, hash, words[0], words[1], words[2],
                        words[3], words[4], words[5], words[6], words[7]);

                    u32 writer_matches{};
                    u32 logged_writers{};
                    for (auto writer = g_dreams_visibility_input_writers.rbegin();
                         writer != g_dreams_visibility_input_writers.rend(); ++writer) {
                        if (!OverlapsDreamsRange(input.base_address, input.GetSize(), writer->base,
                                                writer->size)) {
                            continue;
                        }
                        ++writer_matches;
                        if (logged_writers >= 32) {
                            continue;
                        }
                        LOG_WARNING(
                            Render_Vulkan,
                            "Dreams visibility input writer ordinal={} buffer={} seq={} dispatch={} "
                            "kind={} shader={:#x} stage={} binding={} range={:#x}+{:#x} "
                            "stride={} source={:#x}",
                            ordinal, buffer_index, writer->sequence, writer->dispatch_sequence,
                            static_cast<u32>(writer->kind), writer->shader_hash,
                            static_cast<u32>(writer->stage), writer->binding_index, writer->base,
                            writer->size, writer->stride, writer->source);
                        ++logged_writers;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams visibility input writer summary ordinal={} buffer={} "
                                "matches={} logged={} tracked={}",
                                ordinal, buffer_index, writer_matches, logged_writers,
                                g_dreams_visibility_input_writers.size());
                }
            }
        }
    }

    static u32 dreams_sprite_batch_trace_count{};
    VAddr dreams_sprite_staging_base{};
    if (TraceDreamsProducerSources()) {
        for (const auto& desc : cs.buffers) {
            if (!desc.IsSpecial()) {
                const auto sharp = desc.GetSharp(cs);
                if (sharp.stride == 0x150) {
                    dreams_sprite_staging_base = sharp.base_address;
                    break;
                }
            }
        }
    }
    const bool is_dreams_sprite_batch = dreams_sprite_staging_base != 0;
    const bool is_dreams_target_sprite_batch =
        is_dreams_sprite_batch &&
        memory->IsValidMapping(dreams_sprite_staging_base, 0x124) &&
        *std::bit_cast<const u32*>(dreams_sprite_staging_base + sizeof(u32)) == 0xfffefcbdu &&
        *std::bit_cast<const u32*>(dreams_sprite_staging_base + 0x11c) == 0x7929;
    const u32 dreams_sprite_batch_ordinal =
        is_dreams_target_sprite_batch ? ++dreams_sprite_batch_trace_count : 0;
    const bool trace_dreams_sprite_batch =
        is_dreams_target_sprite_batch && dreams_sprite_batch_ordinal <= 4;
    if (is_dreams_target_sprite_batch && cs.buffers.size() > 3) {
        const auto commands = cs.buffers[0].GetSharp(cs);
        const auto auxiliary = cs.buffers[3].GetSharp(cs);
        g_dreams_sprite_batch_output_ranges = {{
            {.base = commands.base_address, .size = commands.GetSize()},
            {.base = auxiliary.base_address, .size = auxiliary.GetSize()},
        }};
        g_dreams_sprite_batch_producer_sequence = g_compute_dispatch_sequence;
    }
    struct DreamsSpriteConsumerTrace {
        struct BufferSnapshot {
            VAddr base{};
            std::vector<u32> words;
        };

        bool enabled{};
        u32 ordinal{};
        std::vector<u32> gds_before;
        std::vector<BufferSnapshot> buffers_before;
    } sprite_consumer_trace{};
    const bool is_dreams_sprite_consumer =
        Shader::DreamsCompat::IsSpriteCullShader(cs.pgm_hash);
    if (TraceDreamsProducerSources() && is_dreams_sprite_consumer &&
        ConsumeDreamsSpriteConsumerTraceRequest()) {
        sprite_consumer_trace.enabled = true;
        sprite_consumer_trace.ordinal = ++g_dreams_sprite_consumer_trace_ordinal;
        scheduler.Finish();

        const auto* gds = buffer_cache.GetGdsBuffer();
        sprite_consumer_trace.gds_before.resize(gds->mapped_data.size() / sizeof(u32));
        std::memcpy(sprite_consumer_trace.gds_before.data(), gds->mapped_data.data(),
                    sprite_consumer_trace.gds_before.size() * sizeof(u32));
        sprite_consumer_trace.buffers_before.resize(cs.buffers.size());

        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite consumer pre #{} sequence={} shader={:#x} dims={}x{}x{} "
                    "threads={} buffers={} flat_size={}",
                    sprite_consumer_trace.ordinal, g_compute_dispatch_sequence, cs.pgm_hash,
                    cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                    cs_program.num_thread_x.full, cs.buffers.size(), cs.flattened_ud_buf.size());
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite consumer constants #{} f31-32={:#x},{:#x} "
                    "f45-53={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    sprite_consumer_trace.ordinal, flat(31), flat(32), flat(45), flat(46),
                    flat(47), flat(48), flat(49), flat(50), flat(51), flat(52), flat(53));
        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            const auto sharp = desc.GetSharp(cs);
            LOG_WARNING(Render_Vulkan,
                        "Dreams sprite consumer buffer #{} index={} special={} write={} "
                        "formatted={} type={} base={:#x}+{:#x} stride={} records={}",
                        sprite_consumer_trace.ordinal, index, desc.IsSpecial(), desc.is_written,
                        desc.is_formatted, static_cast<u32>(desc.buffer_type), sharp.base_address,
                        sharp.GetSize(), sharp.stride, sharp.num_records);
            if (desc.IsSpecial() || !desc.is_written || sharp.base_address == 0) {
                continue;
            }
            const u64 read_size = std::min<u64>(sharp.GetSize(), 2_MB) & ~u64{3};
            if (read_size == 0 || !memory->IsValidMapping(sharp.base_address, read_size)) {
                continue;
            }
            buffer_cache.ReadMemory(sharp.base_address, read_size);
            auto& snapshot = sprite_consumer_trace.buffers_before[index];
            snapshot.base = sharp.base_address;
            snapshot.words.resize(read_size / sizeof(u32));
            std::memcpy(snapshot.words.data(), std::bit_cast<const void*>(sharp.base_address),
                        read_size);
        }
    }
    if (trace_dreams_sprite_batch) {
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite batch pre #{} sequence={} shader={:#x} dims={}x{}x{} "
                    "threads={} buffers={} flat_size={} f0-7={:#x},{:#x},{:#x},{:#x},"
                    "{:#x},{:#x},{:#x},{:#x}",
                    dreams_sprite_batch_ordinal, g_compute_dispatch_sequence, cs.pgm_hash,
                    cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                    cs_program.num_thread_x.full, cs.buffers.size(), cs.flattened_ud_buf.size(),
                    flat(0), flat(1), flat(2), flat(3), flat(4), flat(5), flat(6), flat(7));
        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            const auto sharp = desc.GetSharp(cs);
            const bool mapped = !desc.IsSpecial() && sharp.base_address != 0 &&
                                memory->IsValidMapping(sharp.base_address, sharp.GetSize());
            LOG_WARNING(Render_Vulkan,
                        "Dreams sprite batch buffer #{} index={} special={} write={} formatted={} "
                        "type={} base={:#x}+{:#x} stride={} records={} cpu_modified={} "
                        "gpu_modified={}",
                        dreams_sprite_batch_ordinal, index, desc.IsSpecial(), desc.is_written,
                        desc.is_formatted, static_cast<u32>(desc.buffer_type), sharp.base_address,
                        sharp.GetSize(), sharp.stride, sharp.num_records,
                        mapped && buffer_cache.IsRegionCpuModified(sharp.base_address,
                                                                   sharp.GetSize()),
                        mapped && buffer_cache.IsRegionGpuModified(sharp.base_address,
                                                                   sharp.GetSize()));
        }
    }

    const bool trace_dreams_replay_progress =
        TraceDreamsReplayResult() && g_dreams_replay_progress_target_found &&
        g_dreams_replay_progress_gds_index != std::numeric_limits<u32>::max() &&
        g_dreams_replay_progress_cycle <= 8 &&
        g_dreams_replay_progress_cycle_observation_count < 128 &&
        std::ranges::any_of(cs.buffers, [](const auto& desc) {
            return desc.buffer_type == Shader::BufferType::GdsBuffer;
        });
    u32 dreams_replay_progress_before{};
    if (trace_dreams_replay_progress) {
        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&dreams_replay_progress_before,
                    gds->mapped_data.data() +
                        g_dreams_replay_progress_gds_index * sizeof(u32),
                    sizeof(u32));
    }

    if (!BindResources(pipeline)) {
        return;
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    const bool profile = ShouldProfileCompute();
    const auto profile_start = std::chrono::steady_clock::now();
    if (cs.pgm_hash == DreamsTraversalShader) {
        DispatchDreamsTraversalOrdered(cmdbuf, cs_program.dim_x, cs_program.dim_y,
                                       cs_program.dim_z);
    } else {
        cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    }
    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - gpu_profile_start)
                                 .count();
        LOG_WARNING(Render_Vulkan,
                    "Dreams GPU profile #{} kind=dispatch hash={:#x} dims={}x{}x{} "
                    "elapsed={:.3f}ms",
                    gpu_profile_ordinal, cs.pgm_hash, cs_program.dim_x, cs_program.dim_y,
                    cs_program.dim_z, elapsed);
    }
    // Dreams uses this shader as an explicit GDS-to-CPU result transfer. The result can share a
    // tracked page with later CPU writes, which removes precise-readback protection before the CPU
    // consumes it. Download the shader's actual output at the transfer point instead.
    if (Common::ElfInfo::Instance().GameSerial() == "CUSA04301" && cs.pgm_hash == 0xcca80e03 &&
        !cs.buffers.empty() && !cs.buffers[0].IsSpecial()) {
        const auto result = cs.buffers[0].GetSharp(cs);
        if (result.base_address != 0 && result.GetSize() != 0) {
            buffer_cache.ReadMemory(result.base_address, result.GetSize());
        }
    }
    if (TraceDreamsReplayResult() && cs.pgm_hash == 0xcca80e03 &&
        g_dreams_replay_progress_target_found &&
        g_dreams_replay_progress_gds_index != std::numeric_limits<u32>::max() &&
        g_dreams_replay_progress_cycle <= 8 && !cs.buffers.empty() &&
        !cs.buffers[0].IsSpecial()) {
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        const auto result = cs.buffers[0].GetSharp(cs);
        const u32 copy_count = std::min(flat(18), flat(20));
        if (result.stride == sizeof(u32) && result.num_records == 45 &&
            result.GetSize() == 45 * sizeof(u32) && copy_count == 45) {
            scheduler.Finish();
            std::array<u32, 12> output{};
            if (result.base_address != 0 && memory->IsValidMapping(result.base_address,
                                                                   sizeof(output))) {
                buffer_cache.ReadMemory(result.base_address, sizeof(output));
                std::memcpy(output.data(), std::bit_cast<const void*>(result.base_address),
                            sizeof(output));
            }
            u32 gds_progress{};
            const auto* gds = buffer_cache.GetGdsBuffer();
            const u64 progress_offset =
                static_cast<u64>(g_dreams_replay_progress_gds_index) * sizeof(u32);
            if (progress_offset + sizeof(u32) <= gds->mapped_data.size()) {
                std::memcpy(&gds_progress, gds->mapped_data.data() + progress_offset,
                            sizeof(u32));
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams replay target readback cycle={} sequence={} flat18={} flat20={} "
                        "gds_byte={:#x} progress_index={} gds_progress={} output={:#x} "
                        "out0-11={},{},{},{},{},{},{},{},{},{},{},{}",
                        g_dreams_replay_progress_cycle, g_compute_dispatch_sequence, flat(18),
                        flat(20), flat(21), g_dreams_replay_progress_gds_index, gds_progress,
                        result.base_address, output[0], output[1], output[2], output[3], output[4],
                        output[5], output[6], output[7], output[8], output[9], output[10],
                        output[11]);
        }
    }
    if (trace_dreams_replay_progress) {
        scheduler.Finish();
        u32 after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&after,
                    gds->mapped_data.data() +
                        g_dreams_replay_progress_gds_index * sizeof(u32),
                    sizeof(u32));
        const u32 observation = g_dreams_replay_progress_cycle_observation_count++;
        if (dreams_replay_progress_before != after || cs.pgm_hash == 0xd4d6c3e3 ||
            cs.pgm_hash == 0xd049fb84 || cs.pgm_hash == 0xcca80e03) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams replay progress direct cycle={} #{} sequence={} shader={:#x} "
                        "dims={}x{}x{} index={} value={}->{}",
                        g_dreams_replay_progress_cycle, observation, g_compute_dispatch_sequence,
                        cs.pgm_hash, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                        g_dreams_replay_progress_gds_index, dreams_replay_progress_before, after);
        }
    }
    if (sprite_consumer_trace.enabled) {
        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        const auto* gds_after = std::bit_cast<const u32*>(gds->mapped_data.data());
        u32 gds_changed{};
        u32 gds_reported{};
        for (u32 index = 0; index < sprite_consumer_trace.gds_before.size(); ++index) {
            if (sprite_consumer_trace.gds_before[index] == gds_after[index]) {
                continue;
            }
            ++gds_changed;
            if (gds_reported < 64) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams sprite consumer GDS #{} index={} before={:#x} after={:#x}",
                            sprite_consumer_trace.ordinal, index,
                            sprite_consumer_trace.gds_before[index], gds_after[index]);
                ++gds_reported;
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite consumer GDS summary #{} shader={:#x} changed={} reported={}",
                    sprite_consumer_trace.ordinal, cs.pgm_hash, gds_changed, gds_reported);

        for (u32 index = 0; index < sprite_consumer_trace.buffers_before.size(); ++index) {
            const auto& snapshot = sprite_consumer_trace.buffers_before[index];
            if (snapshot.words.empty()) {
                continue;
            }
            const u64 read_size = snapshot.words.size() * sizeof(u32);
            const bool gpu_modified_before =
                buffer_cache.IsRegionGpuModified(snapshot.base, read_size);
            buffer_cache.ReadMemory(snapshot.base, read_size);
            const auto* words = std::bit_cast<const u32*>(snapshot.base);
            u32 changed{};
            u32 first_changed = std::numeric_limits<u32>::max();
            u32 before_value{};
            u32 after_value{};
            u32 nonzero_words{};
            for (u32 word = 0; word < snapshot.words.size(); ++word) {
                nonzero_words += words[word] != 0;
                if (snapshot.words[word] == words[word]) {
                    continue;
                }
                ++changed;
                if (first_changed == std::numeric_limits<u32>::max()) {
                    first_changed = word;
                    before_value = snapshot.words[word];
                    after_value = words[word];
                }
            }
            LOG_WARNING(
                Render_Vulkan,
                "Dreams sprite consumer output #{} shader={:#x} buffer={} base={:#x}+{:#x} "
                "gpu_before={} changed={} first={} before={:#x} after={:#x} nonzero={}",
                sprite_consumer_trace.ordinal, cs.pgm_hash, index, snapshot.base, read_size,
                gpu_modified_before, changed, first_changed, before_value, after_value,
                nonzero_words);
        }
    }
    if (trace_dreams_sprite_batch) {
        scheduler.Finish();
        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            if (desc.IsSpecial()) {
                continue;
            }
            const auto sharp = desc.GetSharp(cs);
            const u64 read_size = std::min<u64>(sharp.GetSize(), 1_MB) & ~u64{3};
            if (sharp.base_address == 0 || read_size == 0 ||
                !memory->IsValidMapping(sharp.base_address, read_size)) {
                continue;
            }
            const bool gpu_modified_before =
                buffer_cache.IsRegionGpuModified(sharp.base_address, read_size);
            buffer_cache.ReadMemory(sharp.base_address, read_size);
            const auto* words = std::bit_cast<const u32*>(sharp.base_address);
            const u32 word_count = static_cast<u32>(read_size / sizeof(u32));
            u32 nonzero_words{};
            u32 first_nonzero = std::numeric_limits<u32>::max();
            u32 first_value{};
            for (u32 word = 0; word < word_count; ++word) {
                if (words[word] == 0) {
                    continue;
                }
                ++nonzero_words;
                if (first_nonzero == std::numeric_limits<u32>::max()) {
                    first_nonzero = word;
                    first_value = words[word];
                }
            }
            if (index == 0) {
                u32 reported{};
                for (u32 word = 0; word < word_count && reported < 64; ++word) {
                    if (words[word] == 0) {
                        continue;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams sprite batch nonzero #{} buffer={} dword={} byte={:#x} "
                                "value={:#x}",
                                dreams_sprite_batch_ordinal, index, word,
                                static_cast<u64>(word) * sizeof(u32), words[word]);
                    ++reported;
                }
            }
            LOG_WARNING(
                Render_Vulkan,
                "Dreams sprite batch post #{} index={} write={} base={:#x}+{:#x} stride={} "
                "read={:#x} gpu_before={} nonzero={} first={} value={:#x} "
                "head={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                dreams_sprite_batch_ordinal, index, desc.is_written, sharp.base_address,
                sharp.GetSize(), sharp.stride, read_size, gpu_modified_before, nonzero_words,
                first_nonzero, first_value, words[0], words[1], words[2], words[3], words[4],
                words[5], words[6], words[7]);
        }
    }
    if (visibility_count_trace.enabled) {
        scheduler.Finish();
        std::array<u32, 21> gds_after{};
        std::array<u32, 25> output_after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(gds_after.data(), gds->mapped_data.data() + 320 * sizeof(u32),
                    sizeof(gds_after));
        if (visibility_count_trace.output_base != 0 &&
            visibility_count_trace.output_size >= sizeof(output_after)) {
            buffer_cache.ReadMemory(visibility_count_trace.output_base, sizeof(output_after));
            if (memory->IsValidMapping(visibility_count_trace.output_base,
                                       sizeof(output_after))) {
                std::memcpy(output_after.data(),
                            std::bit_cast<const void*>(visibility_count_trace.output_base),
                            sizeof(output_after));
            }
        }
        LOG_WARNING(
            Render_Vulkan,
            "Dreams visibility count direct post hash={:#x} ordinal={} "
            "out0-8={},{},{},{},{},{},{},{},{} out14,16,21,24={},{},{},{} "
            "gds320-330={},{},{},{},{},{},{},{},{},{},{}",
            cs.pgm_hash, visibility_count_trace.ordinal, output_after[0], output_after[1],
            output_after[2], output_after[3], output_after[4], output_after[5], output_after[6],
            output_after[7], output_after[8], output_after[14], output_after[16],
            output_after[21], output_after[24], gds_after[0], gds_after[1], gds_after[2],
            gds_after[3], gds_after[4], gds_after[5], gds_after[6], gds_after[7],
            gds_after[8], gds_after[9], gds_after[10]);
        if (cs.pgm_hash == 0x76197e85 && g_dreams_visibility_count_trace_active) {
            g_dreams_visibility_count_trace_active = false;
            LOG_WARNING(Render_Vulkan, "Dreams visibility count one-shot trace complete");
        }
    }
    if (scene_stream_trace.enabled) {
        scheduler.Finish();
        u32 counter_after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&counter_after, gds->mapped_data.data() + 2 * sizeof(u32), sizeof(u32));
        const u32 produced = counter_after >= scene_stream_trace.counter_before
                                 ? counter_after - scene_stream_trace.counter_before
                                 : 0;
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene stream post #{} counter={} produced={} reset={} output={:#x}+"
                    "{:#x}",
                    scene_stream_trace.ordinal, counter_after, produced,
                    counter_after < scene_stream_trace.counter_before,
                    scene_stream_trace.output_base, scene_stream_trace.output_size);

        constexpr u64 RecordSize = 4 * sizeof(u32);
        const u64 record_offset = u64{scene_stream_trace.counter_before} * RecordSize;
        const u32 records_to_read = std::min(produced, 8U);
        const u64 bytes_to_read = u64{records_to_read} * RecordSize;
        if (records_to_read != 0 && scene_stream_trace.output_base != 0 &&
            record_offset <= scene_stream_trace.output_size &&
            bytes_to_read <= scene_stream_trace.output_size - record_offset) {
            const VAddr read_address = scene_stream_trace.output_base + record_offset;
            buffer_cache.ReadMemory(read_address, bytes_to_read);
            if (memory->IsValidMapping(read_address, bytes_to_read)) {
                const auto* records = std::bit_cast<const u32*>(read_address);
                for (u32 record = 0; record < records_to_read; ++record) {
                    const u32* values = records + record * 4;
                    LOG_WARNING(Render_Vulkan,
                                "Dreams scene stream record #{} index={} values={:#x},{:#x},"
                                "{:#x},{:#x}",
                                scene_stream_trace.ordinal,
                                scene_stream_trace.counter_before + record, values[0], values[1],
                                values[2], values[3]);
                }
            }
        }
    }
    if (dreams_transfer_trace.enabled) {
        scheduler.Finish();
        for (const auto& snapshot : dreams_transfer_trace.buffers) {
            const u64 read_size = snapshot.words.size() * sizeof(u32);
            buffer_cache.ReadMemory(snapshot.base, read_size);
            const auto* after = std::bit_cast<const u32*>(snapshot.base);
            u32 changed{};
            u32 nonzero_before{};
            u32 nonzero_after{};
            u32 first_changed = std::numeric_limits<u32>::max();
            u32 first_before{};
            u32 first_after{};
            u32 first_nonzero_after = std::numeric_limits<u32>::max();
            for (u32 word = 0; word < snapshot.words.size(); ++word) {
                nonzero_before += snapshot.words[word] != 0;
                nonzero_after += after[word] != 0;
                if (after[word] != 0 &&
                    first_nonzero_after == std::numeric_limits<u32>::max()) {
                    first_nonzero_after = word;
                }
                if (snapshot.words[word] == after[word]) {
                    continue;
                }
                ++changed;
                if (first_changed == std::numeric_limits<u32>::max()) {
                    first_changed = word;
                    first_before = snapshot.words[word];
                    first_after = after[word];
                }
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams transfer post shader={:#x} buffer={} write={} base={:#x}+{:#x} "
                        "stride={} records={} read={:#x} changed={} first_changed={} "
                        "values={:#x}->{:#x} nonzero={}->{} first_nonzero_after={}",
                        dreams_transfer_trace.shader_hash, snapshot.index,
                        snapshot.declared_write, snapshot.base, snapshot.size, snapshot.stride,
                        snapshot.records, read_size, changed, first_changed, first_before,
                        first_after, nonzero_before, nonzero_after, first_nonzero_after);
            if (dreams_transfer_trace.shader_hash == 0xd049fb84 && changed != 0 &&
                RememberDreamsSceneGeneratedRange(snapshot.base, read_size)) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams scene generated range generation={} slot={} buffer={} "
                            "base={:#x}+{:#x} changed={}",
                            g_dreams_scene_graphics_probe_generation,
                            g_dreams_scene_generated_range_count - 1, snapshot.index,
                            snapshot.base, read_size, changed);
            }
        }
    }
    const bool trace_dreams_counter_dispatch =
        TraceDreamsOrderedCounters() &&
        (is_dreams_producer || cs.pgm_hash == Shader::DreamsCompat::SceneCompactShader);
    if (trace_dreams_counter_dispatch) {
        static std::array<u32, 3> previous_queue_state{};
        scheduler.Finish();
        std::array<u32, 3> queue_state{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&queue_state[0], gds->mapped_data.data(), sizeof(u32));
        std::memcpy(&queue_state[1],
                    gds->mapped_data.data() +
                        Shader::DreamsCompat::QueueProducerCounterIndex * sizeof(u32),
                    2 * sizeof(u32));
        if (queue_state != previous_queue_state) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams GDS queue change sequence={} shader={:#x} dims={}x{}x{} "
                        "gds0={} ordered508={} ordered509={} previous={},{},{}",
                        g_compute_dispatch_sequence, cs.pgm_hash, cs_program.dim_x,
                        cs_program.dim_y, cs_program.dim_z, queue_state[0], queue_state[1],
                        queue_state[2], previous_queue_state[0], previous_queue_state[1],
                        previous_queue_state[2]);
            previous_queue_state = queue_state;
        }
        if (cs.pgm_hash == Shader::DreamsCompat::SceneCompactShader) {
            u32 changed{};
            u32 reported{};
            for (u32 i = 0; i < dreams_scene_gds_before.size(); ++i) {
                u32 current{};
                std::memcpy(&current, gds->mapped_data.data() + i * sizeof(u32), sizeof(u32));
                if (!captured_dreams_scene_gds || current == dreams_scene_gds_before[i]) {
                    continue;
                }
                ++changed;
                if (reported++ < 256) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams scene GDS change index={:#x} before={} after={}", i,
                                dreams_scene_gds_before[i], current);
                }
            }
            LOG_WARNING(Render_Vulkan, "Dreams scene GDS changed={} reported={}", changed,
                        std::min(reported, 256U));
        }
    }
    if (TraceDreamsDiagnostics()) {
        static bool trace_target_lifetime{};
        static u32 target_lifetime_observations{};
        if (cs.pgm_hash == 0xd049fb84) {
            trace_target_lifetime = true;
        }
        if (trace_target_lifetime && target_lifetime_observations++ < 800) {
            scheduler.Finish();
            std::array<u32, 15> counters{};
            const auto* gds = buffer_cache.GetGdsBuffer();
            std::memcpy(counters.data(), gds->mapped_data.data() + 768 * sizeof(u32),
                        sizeof(counters));
            LOG_WARNING(Render_Vulkan,
                        "Dreams target lifetime observation={} sequence={} shader={:#x} "
                        "dims={}x{}x{} GDS768-782="
                        "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
                        target_lifetime_observations, g_compute_dispatch_sequence, cs.pgm_hash,
                        cs_program.dim_x, cs_program.dim_y, cs_program.dim_z, counters[0],
                        counters[1], counters[2], counters[3], counters[4], counters[5],
                        counters[6], counters[7], counters[8], counters[9], counters[10],
                        counters[11], counters[12], counters[13], counters[14]);
        }
    }
    static bool dumped_dreams_initial_queue = false;
    if ((TraceDreamsDiagnostics() || TraceDreamsOrderedCounters()) &&
        !dumped_dreams_initial_queue && is_dreams_producer &&
        dreams_producer_record_count != 0 && cs.buffers.size() > 5) {
        dumped_dreams_initial_queue = true;
        scheduler.Finish();
        const auto read_buffer = [&](u32 index, auto& values) {
            const auto buffer = cs.buffers[index].GetSharp(cs);
            if (!cs.buffers[index].IsSpecial() && buffer.base_address != 0 &&
                buffer.GetSize() >= sizeof(values)) {
                buffer_cache.ReadMemory(buffer.base_address, sizeof(values));
                if (memory->IsValidMapping(buffer.base_address, sizeof(values))) {
                    std::memcpy(values.data(), std::bit_cast<const void*>(buffer.base_address),
                                sizeof(values));
                }
            }
        };
        std::array<u32, 16> output{};
        std::array<u32, 16> transforms{};
        std::array<u32, 16> input{};
        read_buffer(1, output);
        read_buffer(2, transforms);
        read_buffer(3, input);
        u32 gds_total{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&gds_total,
                    gds->mapped_data.data() +
                        Shader::DreamsCompat::QueueProducerCounterIndex * sizeof(u32),
                    sizeof(gds_total));
        LOG_WARNING(Render_Vulkan,
                    "Dreams initial producer groups={} threads={} total={} output={:#x}/{:#x} "
                    "{:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} "
                    "{:#x}/{:#x} {:#x}/{:#x}",
                    cs_program.dim_x, cs_program.num_thread_x.full, gds_total, output[0], output[1],
                    output[2], output[3], output[4], output[5], output[6], output[7], output[8],
                    output[9], output[10], output[11], output[12], output[13], output[14],
                    output[15]);
        LOG_WARNING(Render_Vulkan,
                    "Dreams initial transforms={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
                    "input={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    transforms[0], transforms[1], transforms[2], transforms[3], transforms[4],
                    transforms[5], transforms[6], transforms[7], input[0], input[1], input[2],
                    input[3], input[4], input[5], input[6], input[7]);

        struct BufferScan {
            u64 nonzero_dwords{};
            u32 first_dword{std::numeric_limits<u32>::max()};
            u32 first_value{};
            u32 nonzero_records{};
            u32 first_record{std::numeric_limits<u32>::max()};
            std::array<u32, 8> first_record_values{};
        };
        const auto scan_buffer = [&](u32 index, u64 byte_limit) {
            BufferScan scan{};
            const auto buffer = cs.buffers[index].GetSharp(cs);
            if (cs.buffers[index].IsSpecial() || buffer.base_address == 0) {
                return scan;
            }
            const u64 size = std::min<u64>(buffer.GetSize(), byte_limit) & ~u64{3};
            if (size == 0 || !memory->IsValidMapping(buffer.base_address, size)) {
                return scan;
            }
            buffer_cache.ReadMemory(buffer.base_address, size);
            const auto* words = std::bit_cast<const u32*>(buffer.base_address);
            const u32 num_dwords = static_cast<u32>(size / sizeof(u32));
            const u32 stride_dwords = std::max<u32>(buffer.stride / sizeof(u32), 1);
            for (u32 i = 0; i < num_dwords; ++i) {
                if (words[i] != 0) {
                    ++scan.nonzero_dwords;
                    if (scan.first_dword == std::numeric_limits<u32>::max()) {
                        scan.first_dword = i;
                        scan.first_value = words[i];
                    }
                }
            }
            for (u32 record = 0; record * stride_dwords < num_dwords; ++record) {
                const u32 start = record * stride_dwords;
                const u32 end = std::min<u32>(start + stride_dwords, num_dwords);
                bool nonzero = false;
                for (u32 i = start; i < end; ++i) {
                    nonzero |= words[i] != 0;
                }
                if (!nonzero) {
                    continue;
                }
                ++scan.nonzero_records;
                if (scan.first_record == std::numeric_limits<u32>::max()) {
                    scan.first_record = record;
                    for (u32 i = 0; i < scan.first_record_values.size() && start + i < end; ++i) {
                        scan.first_record_values[i] = words[start + i];
                    }
                }
            }
            return scan;
        };
        const auto queue_scan = scan_buffer(1, 64_KB);
        const auto transform_scan = scan_buffer(2, std::numeric_limits<u64>::max());
        const auto input_scan = scan_buffer(3, std::numeric_limits<u64>::max());
        const auto material_scan = scan_buffer(4, std::numeric_limits<u64>::max());
        const auto log_scan = [](const char* name, const BufferScan& scan) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams producer {} scan nonzero_dw={} first_dw={} value={:#x} "
                        "nonzero_records={} first_record={} sample={:#x},{:#x},{:#x},{:#x},"
                        "{:#x},{:#x},{:#x},{:#x}",
                        name, scan.nonzero_dwords, scan.first_dword, scan.first_value,
                        scan.nonzero_records, scan.first_record, scan.first_record_values[0],
                        scan.first_record_values[1], scan.first_record_values[2],
                        scan.first_record_values[3], scan.first_record_values[4],
                        scan.first_record_values[5], scan.first_record_values[6],
                        scan.first_record_values[7]);
        };
        log_scan("queue", queue_scan);
        log_scan("transforms", transform_scan);
        log_scan("input", input_scan);
        log_scan("material", material_scan);

        std::array<u32, 32> srt_values{};
        VAddr srt_address{};
        if (cs.user_data.size() >= 2) {
            srt_address =
                (u64{cs.user_data[0]} | (u64{cs.user_data[1]} << 32)) & 0xFFFFFFFFFFFFULL;
        }
        if (srt_address != 0 && memory->IsValidMapping(srt_address, sizeof(srt_values))) {
            buffer_cache.ReadMemory(srt_address, sizeof(srt_values));
            std::memcpy(srt_values.data(), std::bit_cast<const void*>(srt_address),
                        sizeof(srt_values));
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams producer SRT={:#x} c2={} c6={} c20-29={:#x},{:#x},{:#x},{:#x},"
                    "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    srt_address, srt_values[2], srt_values[6], srt_values[20], srt_values[21],
                    srt_values[22], srt_values[23], srt_values[24], srt_values[25], srt_values[26],
                    srt_values[27], srt_values[28], srt_values[29]);
    }
    if (profile) {
        scheduler.Finish();
    }
    if (trace_dreams_csg_replay_setup) {
        scheduler.Finish();
        std::array<u32, 3> args{};
        std::array<u32, 11> state{};
        read_dreams_csg_direct_state(args, state);
        LOG_WARNING(Render_Vulkan,
                    "Dreams CSG setup post args={:#x} dims={}x{}x{} state={:#x} "
                    "d576-586={},{},{},{},{},{},{},{},{},{},{}",
                    dreams_csg_args_address, args[0], args[1], args[2], dreams_csg_state_address,
                    state[0], state[1], state[2], state[3], state[4], state[5], state[6], state[7],
                    state[8], state[9], state[10]);
    }
    if (dreams_csg_builder_trace.enabled) {
        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::array<u32, 2> gds_after{};
        std::memcpy(&gds_after[0], gds->mapped_data.data() + 14 * sizeof(u32), sizeof(u32));
        std::memcpy(&gds_after[1], gds->mapped_data.data() + 18 * sizeof(u32), sizeof(u32));
        LOG_WARNING(Render_Vulkan,
                    "Dreams CSG builder post #{} gds14={}->{} gds18={}->{}",
                    dreams_csg_builder_trace.ordinal, dreams_csg_builder_trace.gds_before[0],
                    gds_after[0], dreams_csg_builder_trace.gds_before[1], gds_after[1]);
        for (u32 index = 0; index < std::min<u32>(2, cs.buffers.size()); ++index) {
            const auto& desc = cs.buffers[index];
            if (desc.IsSpecial()) {
                continue;
            }
            const auto sharp = desc.GetSharp(cs);
            const u64 read_size = std::min<u64>(sharp.GetSize(), 1_MB) & ~u64{3};
            if (sharp.base_address == 0 || read_size == 0 ||
                !memory->IsValidMapping(sharp.base_address, read_size)) {
                continue;
            }
            buffer_cache.ReadMemory(sharp.base_address, read_size);
            const auto* words = std::bit_cast<const u32*>(sharp.base_address);
            u32 nonzero{};
            u32 first_nonzero = std::numeric_limits<u32>::max();
            u32 first_value{};
            for (u32 word = 0; word < read_size / sizeof(u32); ++word) {
                if (words[word] == 0) {
                    continue;
                }
                ++nonzero;
                if (first_nonzero == std::numeric_limits<u32>::max()) {
                    first_nonzero = word;
                    first_value = words[word];
                }
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams CSG builder output #{} index={} base={:#x}+{:#x} stride={} "
                        "nonzero={} first={} value={:#x}",
                        dreams_csg_builder_trace.ordinal, index, sharp.base_address,
                        sharp.GetSize(), sharp.stride, nonzero, first_nonzero, first_value);
        }
    }
    if (profile) {
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - profile_start)
                                 .count();
        LOG_WARNING(Render_Vulkan, "Compute profile hash={:#x} dims={}x{}x{} elapsed={:.3f}ms",
                    cs.pgm_hash, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z, elapsed);
    }
    DebugState.IncDispatch();

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    TraceComputeShader(cs, true, 0, 0, 0);
    if (ShouldSkipComputeShader(cs)) {
        return;
    }
    const u32 gpu_profile_ordinal = ConsumeDreamsGpuProfileEvent();
    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
    }
    const auto gpu_profile_start = std::chrono::steady_clock::now();

    if (!BindResources(pipeline)) {
        return;
    }

    const bool profile_dreams = ShouldProfileDreamsTraversal(cs);
    const bool profile = ShouldProfileCompute() || profile_dreams;
    const VAddr args_address = address + offset;
    struct DreamsVisibilityCountIndirectTrace {
        bool enabled{};
        u32 ordinal{};
        VAddr args_address{};
        std::array<u32, 3> dims{};
        std::array<u32, 21> gds_before{};
    } visibility_count_trace{};
    static u32 dreams_traversal_count_trace_ordinal{};
    static u32 dreams_secondary_count_trace_ordinal{};
    u32* visibility_count_ordinal{};
    if (TraceDreamsVisibilityCounts()) {
        if (cs.pgm_hash == Shader::DreamsCompat::TraversalShader) {
            visibility_count_ordinal = &dreams_traversal_count_trace_ordinal;
        } else if (cs.pgm_hash == Shader::DreamsCompat::QueueProducerShaderAlt) {
            visibility_count_ordinal = &dreams_secondary_count_trace_ordinal;
        }
    }
    if (visibility_count_ordinal != nullptr) {
        const u32 ordinal = ++*visibility_count_ordinal;
        if (g_dreams_visibility_count_trace_active || ordinal <= 8 || ordinal % 120 == 0) {
            visibility_count_trace.enabled = true;
            visibility_count_trace.ordinal = ordinal;
            visibility_count_trace.args_address = args_address;
            scheduler.Finish();
            buffer_cache.ReadMemory(args_address, sizeof(visibility_count_trace.dims));
            if (memory->IsValidMapping(args_address, sizeof(visibility_count_trace.dims))) {
                std::memcpy(visibility_count_trace.dims.data(),
                            std::bit_cast<const void*>(args_address),
                            sizeof(visibility_count_trace.dims));
            }
            if (g_recent_compute_dispatch_index != 0) {
                auto& recent = g_recent_compute_dispatches
                    [(g_recent_compute_dispatch_index - 1) % g_recent_compute_dispatches.size()];
                recent.dim_x = visibility_count_trace.dims[0];
                recent.dim_y = visibility_count_trace.dims[1];
                recent.dim_z = visibility_count_trace.dims[2];
            }
            const auto* gds = buffer_cache.GetGdsBuffer();
            std::memcpy(visibility_count_trace.gds_before.data(),
                        gds->mapped_data.data() + 320 * sizeof(u32),
                        sizeof(visibility_count_trace.gds_before));
            const auto flat = [&](u32 index) {
                return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
            };
            LOG_WARNING(
                Render_Vulkan,
                "Dreams visibility count indirect pre hash={:#x} ordinal={} sequence={} "
                "args={:#x} dims={}x{}x{} flat18={} flat22={} flat_size={} "
                "gds320-330={},{},{},{},{},{},{},{},{},{},{}",
                cs.pgm_hash, ordinal, g_compute_dispatch_sequence, args_address,
                visibility_count_trace.dims[0], visibility_count_trace.dims[1],
                visibility_count_trace.dims[2], flat(18), flat(22),
                cs.flattened_ud_buf.size(), visibility_count_trace.gds_before[0],
                visibility_count_trace.gds_before[1], visibility_count_trace.gds_before[2],
                visibility_count_trace.gds_before[3], visibility_count_trace.gds_before[4],
                visibility_count_trace.gds_before[5], visibility_count_trace.gds_before[6],
                visibility_count_trace.gds_before[7], visibility_count_trace.gds_before[8],
                visibility_count_trace.gds_before[9], visibility_count_trace.gds_before[10]);

            const u64 args_end = args_address + sizeof(visibility_count_trace.dims);
            u32 writer_matches{};
            for (auto writer = g_dreams_buffer_writers.rbegin();
                 writer != g_dreams_buffer_writers.rend() && writer_matches < 16; ++writer) {
                const u64 writer_end = writer->base + writer->size;
                if (!writer->declared_write || writer->base >= args_end ||
                    args_address >= writer_end) {
                    continue;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams visibility count args writer hash={:#x} ordinal={} "
                            "writer_seq={} dispatch={} kind={} shader={:#x} binding={} "
                            "range={:#x}+{:#x} stride={} source={:#x}",
                            cs.pgm_hash, ordinal, writer->sequence, writer->dispatch_sequence,
                            static_cast<u32>(writer->kind), writer->shader_hash,
                            writer->binding_index, writer->base, writer->size, writer->stride,
                            writer->source);
                ++writer_matches;
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams visibility count args summary hash={:#x} ordinal={} "
                        "writers={} history={}",
                        cs.pgm_hash, ordinal, writer_matches, g_dreams_buffer_writers.size());

            if (g_dreams_visibility_count_trace_active || ordinal <= 2) {
                for (u32 index = 0; index < cs.buffers.size(); ++index) {
                    const auto& desc = cs.buffers[index];
                    const auto sharp = desc.GetSharp(cs);
                    LOG_WARNING(Render_Vulkan,
                                "Dreams visibility count resource hash={:#x} buffer={} "
                                "special={} write={} base={:#x}+{:#x} stride={} records={}",
                                cs.pgm_hash, index, desc.IsSpecial(), desc.is_written,
                                sharp.base_address, sharp.GetSize(), sharp.stride,
                                sharp.num_records);
                }
            }
        }
    }
    struct DreamsSceneStreamIndirectTrace {
        bool enabled{};
        u32 ordinal{};
        u32 counter_before{};
        VAddr output_base{};
        u64 output_size{};
        u32 output_stride{};
        std::array<u32, 3> dims{};
    } scene_stream_trace{};
    static u32 dreams_scene_stream_indirect_trace_count{};
    if (TraceDreamsSceneStream() && cs.pgm_hash == 0x34267ace &&
        dreams_scene_stream_indirect_trace_count < 8 && cs.buffers.size() > 5) {
        scene_stream_trace.enabled = true;
        scene_stream_trace.ordinal = ++dreams_scene_stream_indirect_trace_count;
        const auto output = cs.buffers[0].GetSharp(cs);
        scene_stream_trace.output_base = output.base_address;
        scene_stream_trace.output_size = output.GetSize();
        scene_stream_trace.output_stride = output.stride;

        scheduler.Finish();
        buffer_cache.ReadMemory(args_address, sizeof(scene_stream_trace.dims));
        if (memory->IsValidMapping(args_address, sizeof(scene_stream_trace.dims))) {
            std::memcpy(scene_stream_trace.dims.data(), std::bit_cast<const void*>(args_address),
                        sizeof(scene_stream_trace.dims));
        }
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&scene_stream_trace.counter_before,
                    gds->mapped_data.data() + 2 * sizeof(u32), sizeof(u32));
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene stream indirect pre #{} sequence={} args={:#x} dims={}x{}x{} "
                    "threads={} counter={} output={:#x}+{:#x} stride={} buffers={} images={}",
                    scene_stream_trace.ordinal, g_compute_dispatch_sequence, args_address,
                    scene_stream_trace.dims[0], scene_stream_trace.dims[1],
                    scene_stream_trace.dims[2], cs_program.num_thread_x.full,
                    scene_stream_trace.counter_before, scene_stream_trace.output_base,
                    scene_stream_trace.output_size, scene_stream_trace.output_stride,
                    cs.buffers.size(), cs.images.size());

        const u64 args_end = args_address + sizeof(scene_stream_trace.dims);
        u32 writer_matches{};
        for (auto writer = g_dreams_buffer_writers.rbegin();
             writer != g_dreams_buffer_writers.rend() && writer_matches < 32; ++writer) {
            const u64 writer_end = writer->base + writer->size;
            if (!writer->declared_write || writer->base >= args_end || args_address >= writer_end) {
                continue;
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams scene args writer #{} writer_seq={} dispatch={} kind={} "
                        "shader={:#x} stage={} binding={} range={:#x}+{:#x} stride={} "
                        "source={:#x}",
                        scene_stream_trace.ordinal, writer->sequence, writer->dispatch_sequence,
                        static_cast<u32>(writer->kind), writer->shader_hash,
                        static_cast<u32>(writer->stage), writer->binding_index, writer->base,
                        writer->size, writer->stride, writer->source);
            ++writer_matches;
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene args writer summary #{} matches={} history={}",
                    scene_stream_trace.ordinal, writer_matches, g_dreams_buffer_writers.size());

        const u32 recent_count = std::min<u32>(g_recent_compute_dispatch_index,
                                               g_recent_compute_dispatches.size());
        for (u32 recent = 0; recent < recent_count; ++recent) {
            const u32 index =
                (g_recent_compute_dispatch_index - 1 - recent) % g_recent_compute_dispatches.size();
            const auto& dispatch = g_recent_compute_dispatches[index];
            LOG_WARNING(Render_Vulkan,
                        "Dreams scene recent compute #{} back={} shader={:#x} dims={}x{}x{}",
                        scene_stream_trace.ordinal, recent, dispatch.hash, dispatch.dim_x,
                        dispatch.dim_y, dispatch.dim_z);
        }
    }
    struct DreamsSceneArgsBuilderTrace {
        bool enabled{};
        u32 ordinal{};
        VAddr output_base{};
        u64 output_size{};
        std::array<u32, 3> dims{};
        std::array<u32, 24> output_before{};
        std::array<u32, 4> gds_before{};
    } args_builder_trace{};
    static u32 dreams_scene_args_builder_trace_count{};
    if (TraceDreamsSceneStream() && cs.pgm_hash == 0x4bc902cf &&
        dreams_scene_args_builder_trace_count < 20 && cs.buffers.size() > 8) {
        args_builder_trace.enabled = true;
        args_builder_trace.ordinal = ++dreams_scene_args_builder_trace_count;
        const auto output = cs.buffers[0].GetSharp(cs);
        args_builder_trace.output_base = output.base_address;
        args_builder_trace.output_size = output.GetSize();

        scheduler.Finish();
        buffer_cache.ReadMemory(args_address, sizeof(args_builder_trace.dims));
        if (memory->IsValidMapping(args_address, sizeof(args_builder_trace.dims))) {
            std::memcpy(args_builder_trace.dims.data(), std::bit_cast<const void*>(args_address),
                        sizeof(args_builder_trace.dims));
        }
        if (args_builder_trace.output_base != 0 &&
            args_builder_trace.output_size >= sizeof(args_builder_trace.output_before)) {
            buffer_cache.ReadMemory(args_builder_trace.output_base,
                                    sizeof(args_builder_trace.output_before));
            if (memory->IsValidMapping(args_builder_trace.output_base,
                                       sizeof(args_builder_trace.output_before))) {
                std::memcpy(args_builder_trace.output_before.data(),
                            std::bit_cast<const void*>(args_builder_trace.output_base),
                            sizeof(args_builder_trace.output_before));
            }
        }
        const auto* gds = buffer_cache.GetGdsBuffer();
        constexpr std::array<u32, 4> GdsIndices{258, 259, 515, 516};
        for (u32 i = 0; i < GdsIndices.size(); ++i) {
            std::memcpy(&args_builder_trace.gds_before[i],
                        gds->mapped_data.data() + GdsIndices[i] * sizeof(u32), sizeof(u32));
        }
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        LOG_WARNING(
            Render_Vulkan,
            "Dreams scene args builder pre #{} sequence={} args={:#x} dims={}x{}x{} "
            "output={:#x}+{:#x} out0-2={},{},{} out8-10={},{},{} out16-18={},{},{} "
            "gds258,259,515,516={},{},{},{} flat2,30,43={},{},{} gds_special={}",
            args_builder_trace.ordinal, g_compute_dispatch_sequence, args_address,
            args_builder_trace.dims[0], args_builder_trace.dims[1], args_builder_trace.dims[2],
            args_builder_trace.output_base, args_builder_trace.output_size,
            args_builder_trace.output_before[0], args_builder_trace.output_before[1],
            args_builder_trace.output_before[2], args_builder_trace.output_before[8],
            args_builder_trace.output_before[9], args_builder_trace.output_before[10],
            args_builder_trace.output_before[16], args_builder_trace.output_before[17],
            args_builder_trace.output_before[18], args_builder_trace.gds_before[0],
            args_builder_trace.gds_before[1], args_builder_trace.gds_before[2],
            args_builder_trace.gds_before[3], flat(2), flat(30), flat(43),
            cs.buffers[8].IsSpecial());
    }
    const bool is_dreams_csg_replay = cs.pgm_hash == 0x8b19605c;
    static u32 dreams_csg_replay_trace_count{};
    const bool trace_dreams_csg_replay = TraceDreamsCsgReplay() && is_dreams_csg_replay &&
                                         dreams_csg_replay_trace_count++ < 4;
    VAddr dreams_csg_state_address{};
    const auto log_dreams_csg_buffers = [&](std::string_view phase) {
        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            const auto sharp = desc.GetSharp(cs);
            if (desc.IsSpecial()) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams CSG buffer {} phase={} special=true type={} write={}", index,
                            phase, static_cast<u32>(desc.buffer_type), desc.is_written);
                continue;
            }

            constexpr u64 MaxScanSize = 1_MB;
            const u64 scan_size = std::min<u64>(sharp.GetSize(), MaxScanSize) & ~u64{3};
            const bool mapped = sharp.base_address != 0 && scan_size != 0 &&
                                memory->IsValidMapping(sharp.base_address, scan_size);
            if (mapped) {
                buffer_cache.ReadMemory(sharp.base_address, scan_size);
            }

            u64 nonzero{};
            u32 first_nonzero = std::numeric_limits<u32>::max();
            u32 first_value{};
            std::array<u32, 8> head{};
            if (mapped) {
                const auto* words = std::bit_cast<const u32*>(sharp.base_address);
                std::memcpy(head.data(), words, sizeof(head));
                const u32 num_dwords = static_cast<u32>(scan_size / sizeof(u32));
                for (u32 word = 0; word < num_dwords; ++word) {
                    if (words[word] == 0) {
                        continue;
                    }
                    ++nonzero;
                    if (first_nonzero == std::numeric_limits<u32>::max()) {
                        first_nonzero = word;
                        first_value = words[word];
                    }
                }
            }
            LOG_WARNING(
                Render_Vulkan,
                "Dreams CSG buffer {} phase={} base={:#x} size={:#x} stride={} records={} "
                "write={} formatted={} mapped={} cpu_modified={} gpu_modified={} scan={:#x} "
                "nonzero={} first_dw={} first_value={:#x} head={:#x},{:#x},{:#x},{:#x},"
                "{:#x},{:#x},{:#x},{:#x}",
                index, phase, sharp.base_address, sharp.GetSize(), sharp.stride, sharp.num_records,
                desc.is_written, desc.is_formatted, mapped,
                mapped && buffer_cache.IsRegionCpuModified(sharp.base_address, scan_size),
                mapped && buffer_cache.IsRegionGpuModified(sharp.base_address, scan_size), scan_size,
                nonzero, first_nonzero, first_value, head[0], head[1], head[2], head[3], head[4],
                head[5], head[6], head[7]);
            if (phase == "pre" && TraceDreamsBufferDependencies()) {
                u32 matches{};
                for (auto writer = g_dreams_buffer_writers.rbegin();
                     writer != g_dreams_buffer_writers.rend() && matches < 16; ++writer) {
                    if (writer->shader_hash == cs.pgm_hash ||
                        !OverlapsDreamsRange(sharp.base_address, sharp.GetSize(), writer->base,
                                            writer->size)) {
                        continue;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams CSG input {} writer #{} sequence={} dispatch={} "
                                "shader={:#x} stage={} kind={} binding={} declared={} "
                                "base={:#x}+{:#x} stride={} source={:#x}",
                                index, matches, writer->sequence, writer->dispatch_sequence,
                                writer->shader_hash, static_cast<u32>(writer->stage),
                                static_cast<u32>(writer->kind), writer->binding_index,
                                writer->declared_write, writer->base, writer->size, writer->stride,
                                writer->source);
                    ++matches;
                }
                LOG_WARNING(Render_Vulkan, "Dreams CSG input {} writer matches={}", index,
                            matches);
            }
        }
    };
    const auto read_dreams_csg_indirect_state = [&](auto& dims, auto& state) {
        buffer_cache.ReadMemory(args_address, sizeof(dims));
        if (memory->IsValidMapping(args_address, sizeof(dims))) {
            std::memcpy(dims.data(), std::bit_cast<const void*>(args_address), sizeof(dims));
        }
        if (cs.buffers.size() <= 6) {
            return;
        }
        constexpr u64 StateOffset = 576 * sizeof(u32);
        if (cs.buffers[6].IsSpecial()) {
            const auto* gds = buffer_cache.GetGdsBuffer();
            dreams_csg_state_address = StateOffset;
            std::memcpy(state.data(), gds->mapped_data.data() + StateOffset, sizeof(state));
            return;
        }
        const auto state_sharp = cs.buffers[6].GetSharp(cs);
        dreams_csg_state_address = state_sharp.base_address;
        if (state_sharp.base_address != 0 &&
            state_sharp.GetSize() >= StateOffset + sizeof(state)) {
            const VAddr sample_address = state_sharp.base_address + StateOffset;
            buffer_cache.ReadMemory(sample_address, sizeof(state));
            if (memory->IsValidMapping(sample_address, sizeof(state))) {
                std::memcpy(state.data(), std::bit_cast<const void*>(sample_address),
                            sizeof(state));
            }
        }
    };
    if (trace_dreams_csg_replay) {
        scheduler.Finish();
        log_dreams_csg_buffers("pre");
        std::array<u32, 3> dims{};
        std::array<u32, 11> state{};
        read_dreams_csg_indirect_state(dims, state);
        LOG_WARNING(Render_Vulkan,
                    "Dreams CSG replay pre args={:#x} dims={}x{}x{} state={:#x} "
                    "d576-586={},{},{},{},{},{},{},{},{},{},{}",
                    args_address, dims[0], dims[1], dims[2], dreams_csg_state_address, state[0],
                    state[1], state[2], state[3], state[4], state[5], state[6], state[7], state[8],
                    state[9], state[10]);
    }
    const bool ordered_dreams_traversal = cs.pgm_hash == DreamsTraversalShader;
    const bool trace_dreams_progress =
        ordered_dreams_traversal && TraceDreamsTraversalProgress();
    std::array<u32, 3> ordered_dims{};
    std::array<u32, 18> progress_pre_gds{};
    bool have_ordered_dims = false;
    const auto read_dreams_gds = [&] {
        constexpr std::array<u32, 18> indices{
            2,    4,    6,    320,  321,  1282, 1286, 1283, 1284,
            322,  326,  323,  324,  16370, 16374, 16371, 16372,
            DreamsTraversalCompletionOffset / sizeof(u32)};
        std::array<u32, indices.size()> values{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        for (u32 i = 0; i < indices.size(); ++i) {
            std::memcpy(&values[i], gds->mapped_data.data() + indices[i] * sizeof(u32),
                        sizeof(u32));
        }
        return values;
    };
    const auto read_dreams_ordered_scratch = [&] {
        std::array<u32, Shader::DreamsCompat::OrderedCounterCount * 5> values{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        for (u32 counter = 0; counter < Shader::DreamsCompat::OrderedCounterCount; ++counter) {
            const u32 base = Shader::DreamsCompat::OrderedScratchBaseDword +
                             counter * Shader::DreamsCompat::OrderedCounterStrideDwords;
            const std::array<u32, 5> indices{
                base, base + 1, base + Shader::DreamsCompat::OrderedEntryDwords,
                base + Shader::DreamsCompat::OrderedEntryDwords + 1,
                Shader::DreamsCompat::OrderedBaseSlotsDword + counter};
            for (u32 i = 0; i < indices.size(); ++i) {
                std::memcpy(&values[counter * indices.size() + i],
                            gds->mapped_data.data() + indices[i] * sizeof(u32), sizeof(u32));
            }
        }
        return values;
    };

    // Exact indirect ordering requires a GPU readback and thousands of tiny submissions. Keep it
    // available for diagnosis without imposing that cost on normal gameplay.
    const bool inspect_ordered_dims =
        ordered_dreams_traversal && OrderDreamsIndirectTraversal();
    if (inspect_ordered_dims) {
        scheduler.Finish();
    }
    if (ordered_dreams_traversal && inspect_ordered_dims) {
        buffer_cache.ReadMemory(args_address, sizeof(ordered_dims));
        if (memory->IsValidMapping(args_address, sizeof(ordered_dims))) {
            std::memcpy(ordered_dims.data(), std::bit_cast<const void*>(args_address),
                        sizeof(ordered_dims));
            have_ordered_dims = true;
        }
        if (trace_dreams_progress) {
            progress_pre_gds = read_dreams_gds();
        }
    }
    if (profile_dreams) {
        const auto gds = read_dreams_gds();
        LOG_WARNING(Render_Vulkan,
                    "Dreams traversal pre args={:#x} old={},{},{} base={} total={} "
                    "dword_base={},{},{},{} byte_base={},{},{},{} sequences={},{},{},{} "
                    "completion={}",
                    args_address, gds[0], gds[1], gds[2], gds[3], gds[4], gds[5], gds[6],
                    gds[7], gds[8], gds[9], gds[10], gds[11], gds[12], gds[13], gds[14],
                    gds[15], gds[16], gds[17]);

        static bool dumped_traversal_inputs = false;
        if (!dumped_traversal_inputs && gds[4] != 0) {
            dumped_traversal_inputs = true;
            for (u32 i = 0; i < cs.buffers.size(); ++i) {
                const auto& desc = cs.buffers[i];
                const auto vsharp = desc.GetSharp(cs);
                LOG_WARNING(Render_Vulkan,
                            "Dreams traversal buffer={} sharp={} type={} write={} formatted={} "
                            "base={:#x} size={:#x} stride={} records={} used={:#x}",
                            i, desc.sharp_idx, static_cast<u32>(desc.buffer_type), desc.is_written,
                            desc.is_formatted, vsharp.base_address, vsharp.GetSize(), vsharp.stride,
                            vsharp.num_records, static_cast<u32>(desc.used_types));
            }

            const auto read_dwords = [&](u32 buffer_index, u64 dword_offset,
                                         auto& values) -> bool {
                if (buffer_index >= cs.buffers.size()) {
                    return false;
                }
                const auto& desc = cs.buffers[buffer_index];
                if (desc.IsSpecial()) {
                    return false;
                }
                const auto vsharp = desc.GetSharp(cs);
                const u64 byte_offset = dword_offset * sizeof(u32);
                if (vsharp.base_address == 0 || byte_offset + sizeof(values) > vsharp.GetSize()) {
                    return false;
                }
                const VAddr sample_address = vsharp.base_address + byte_offset;
                buffer_cache.ReadMemory(sample_address, sizeof(values));
                if (!memory->IsValidMapping(sample_address, sizeof(values))) {
                    return false;
                }
                std::memcpy(values.data(), std::bit_cast<const void*>(sample_address),
                            sizeof(values));
                return true;
            };

            std::array<u32, 16> queue{};
            if (read_dwords(6, static_cast<u64>(gds[3]) * 2, queue)) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams traversal queue base={} records={:#x}/{:#x} {:#x}/{:#x} "
                            "{:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} "
                            "{:#x}/{:#x}",
                            gds[3], queue[0], queue[1], queue[2], queue[3], queue[4], queue[5],
                            queue[6], queue[7], queue[8], queue[9], queue[10], queue[11], queue[12],
                            queue[13], queue[14], queue[15]);

                const u32 node_index = queue[0] & 0x00ffffff;
                const u32 object_index = queue[1] & 0x0000ffff;
                std::array<u32, 4> node{};
                std::array<u32, 12> transform{};
                if (read_dwords(10, node_index, node)) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams traversal first node index={} data={:#x},{:#x},{:#x},{:#x}",
                                node_index, node[0], node[1], node[2], node[3]);
                }
                if (read_dwords(8, static_cast<u64>(object_index) * 108 + 24, transform)) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams traversal first object index={} transform={:#x},{:#x},{:#x},"
                                "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                                object_index, transform[0], transform[1], transform[2], transform[3],
                                transform[4], transform[5], transform[6], transform[7], transform[8],
                                transform[9], transform[10], transform[11]);
                }

                const auto queue_sharp = cs.buffers[6].GetSharp(cs);
                const u64 queue_offset = static_cast<u64>(gds[3]) * queue_sharp.stride;
                const u64 queue_bytes = std::min<u64>(
                    static_cast<u64>(gds[4]) * queue_sharp.stride,
                    queue_sharp.GetSize() > queue_offset ? queue_sharp.GetSize() - queue_offset
                                                         : 0);
                if (queue_bytes != 0 &&
                    memory->IsValidMapping(queue_sharp.base_address + queue_offset, queue_bytes)) {
                    buffer_cache.ReadMemory(queue_sharp.base_address + queue_offset, queue_bytes);
                    const auto* words = std::bit_cast<const u32*>(queue_sharp.base_address +
                                                                  queue_offset);
                    const u64 word_count = queue_bytes / sizeof(u32);
                    u64 nonzero{};
                    u64 first = std::numeric_limits<u64>::max();
                    for (u64 i = 0; i < word_count; ++i) {
                        if (words[i] != 0) {
                            ++nonzero;
                            first = std::min(first, i);
                        }
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams traversal queue scan bytes={:#x} nonzero={} first_dw={}",
                                queue_bytes, nonzero, first);
                }
            }
        }
    }

    struct DreamsSpriteIndirectConsumerTrace {
        struct BufferSnapshot {
            VAddr base{};
            std::vector<u32> words;
        };

        bool enabled{};
        u32 ordinal{};
        std::array<u32, 3> dims{};
        std::vector<u32> gds_before;
        std::vector<BufferSnapshot> buffers_before;
    } sprite_consumer_trace{};
    const bool is_dreams_sprite_consumer =
        Shader::DreamsCompat::IsSpriteCullShader(cs.pgm_hash);
    if (TraceDreamsProducerSources() && is_dreams_sprite_consumer &&
        ConsumeDreamsSpriteConsumerTraceRequest()) {
        sprite_consumer_trace.enabled = true;
        sprite_consumer_trace.ordinal = ++g_dreams_sprite_consumer_trace_ordinal;
        scheduler.Finish();
        buffer_cache.ReadMemory(args_address, sizeof(sprite_consumer_trace.dims));
        if (memory->IsValidMapping(args_address, sizeof(sprite_consumer_trace.dims))) {
            std::memcpy(sprite_consumer_trace.dims.data(),
                        std::bit_cast<const void*>(args_address),
                        sizeof(sprite_consumer_trace.dims));
        }
        if (sprite_consumer_trace.dims[0] != 0 && cs.buffers.size() > 3) {
            const auto commands = cs.buffers[0].GetSharp(cs);
            const auto counts = cs.buffers[1].GetSharp(cs);
            const auto records = cs.buffers[3].GetSharp(cs);
            g_dreams_sprite_geometry_output_ranges = {{
                {.base = commands.base_address, .size = commands.GetSize()},
                {.base = counts.base_address, .size = counts.GetSize()},
                {.base = records.base_address, .size = records.GetSize()},
            }};
            g_dreams_sprite_geometry_producer_sequence = g_compute_dispatch_sequence;
        }

        const auto* gds = buffer_cache.GetGdsBuffer();
        sprite_consumer_trace.gds_before.resize(gds->mapped_data.size() / sizeof(u32));
        std::memcpy(sprite_consumer_trace.gds_before.data(), gds->mapped_data.data(),
                    sprite_consumer_trace.gds_before.size() * sizeof(u32));
        sprite_consumer_trace.buffers_before.resize(cs.buffers.size());

        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite indirect consumer pre #{} sequence={} shader={:#x} "
                    "args={:#x} dims={}x{}x{} threads={} buffers={} flat_size={}",
                    sprite_consumer_trace.ordinal, g_compute_dispatch_sequence, cs.pgm_hash,
                    args_address, sprite_consumer_trace.dims[0], sprite_consumer_trace.dims[1],
                    sprite_consumer_trace.dims[2], cs_program.num_thread_x.full,
                    cs.buffers.size(), cs.flattened_ud_buf.size());
        const auto flat = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        LOG_WARNING(Render_Vulkan,
                    "Dreams sprite indirect consumer constants #{} f31-32={:#x},{:#x} "
                    "f45-53={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    sprite_consumer_trace.ordinal, flat(31), flat(32), flat(45), flat(46),
                    flat(47), flat(48), flat(49), flat(50), flat(51), flat(52), flat(53));
        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            const auto sharp = desc.GetSharp(cs);
            LOG_WARNING(
                Render_Vulkan,
                "Dreams sprite indirect consumer buffer #{} index={} special={} write={} "
                "formatted={} type={} base={:#x}+{:#x} stride={} records={}",
                sprite_consumer_trace.ordinal, index, desc.IsSpecial(), desc.is_written,
                desc.is_formatted, static_cast<u32>(desc.buffer_type), sharp.base_address,
                sharp.GetSize(), sharp.stride, sharp.num_records);
            if (desc.IsSpecial() || !desc.is_written || sharp.base_address == 0) {
                continue;
            }
            const u64 read_size = std::min<u64>(sharp.GetSize(), 2_MB) & ~u64{3};
            if (read_size == 0 || !memory->IsValidMapping(sharp.base_address, read_size)) {
                continue;
            }
            auto& snapshot = sprite_consumer_trace.buffers_before[index];
            snapshot.base = sharp.base_address;
            snapshot.words.resize(read_size / sizeof(u32));
        }
    }

    const bool trace_dreams_replay_progress =
        TraceDreamsReplayResult() && g_dreams_replay_progress_target_found &&
        g_dreams_replay_progress_gds_index != std::numeric_limits<u32>::max() &&
        g_dreams_replay_progress_cycle <= 8 &&
        g_dreams_replay_progress_cycle_observation_count < 128 &&
        std::ranges::any_of(cs.buffers, [](const auto& desc) {
            return desc.buffer_type == Shader::BufferType::GdsBuffer;
        });
    u32 dreams_replay_progress_before{};
    if (trace_dreams_replay_progress) {
        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        const u64 progress_offset =
            static_cast<u64>(g_dreams_replay_progress_gds_index) * sizeof(u32);
        if (progress_offset + sizeof(u32) <= gds->mapped_data.size()) {
            std::memcpy(&dreams_replay_progress_before,
                        gds->mapped_data.data() + progress_offset, sizeof(u32));
        }
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(args_address, size, false);
    if (profile_dreams) {
        buffer_cache.ReadMemory(args_address, sizeof(std::array<u32, 3>));
        if (memory->IsValidMapping(args_address, sizeof(std::array<u32, 3>))) {
            std::array<u32, 3> dims{};
            std::memcpy(dims.data(), std::bit_cast<const void*>(args_address), sizeof(dims));
            LOG_WARNING(Render_Vulkan, "Indirect args hash={:#x} address={:#x} dims={}x{}x{}",
                        cs.pgm_hash, args_address, dims[0], dims[1], dims[2]);
        }
    }

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    const auto profile_start = std::chrono::steady_clock::now();
    if (have_ordered_dims) {
        DispatchDreamsTraversalOrdered(cmdbuf, ordered_dims[0], ordered_dims[1], ordered_dims[2]);
    } else {
        cmdbuf.dispatchIndirect(buffer->Handle(), base);
    }
    if (gpu_profile_ordinal != 0) {
        scheduler.Finish();
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - gpu_profile_start)
                                 .count();
        LOG_WARNING(Render_Vulkan,
                    "Dreams GPU profile #{} kind=dispatch-indirect hash={:#x} "
                    "elapsed={:.3f}ms",
                    gpu_profile_ordinal, cs.pgm_hash, elapsed);
    }
    if (trace_dreams_replay_progress) {
        scheduler.Finish();
        u32 after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        const u64 progress_offset =
            static_cast<u64>(g_dreams_replay_progress_gds_index) * sizeof(u32);
        if (progress_offset + sizeof(u32) <= gds->mapped_data.size()) {
            std::memcpy(&after, gds->mapped_data.data() + progress_offset, sizeof(u32));
        }
        const u32 observation = g_dreams_replay_progress_cycle_observation_count++;
        if (dreams_replay_progress_before != after) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams replay progress indirect cycle={} #{} sequence={} shader={:#x} "
                        "args={:#x} index={} value={}->{}",
                        g_dreams_replay_progress_cycle, observation, g_compute_dispatch_sequence,
                        cs.pgm_hash, args_address, g_dreams_replay_progress_gds_index,
                        dreams_replay_progress_before, after);
        }
    }
    if (sprite_consumer_trace.enabled) {
        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        const auto* gds_after = std::bit_cast<const u32*>(gds->mapped_data.data());
        u32 gds_changed{};
        u32 gds_reported{};
        for (u32 index = 0; index < sprite_consumer_trace.gds_before.size(); ++index) {
            if (sprite_consumer_trace.gds_before[index] == gds_after[index]) {
                continue;
            }
            ++gds_changed;
            if (gds_reported < 64) {
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams sprite indirect consumer GDS #{} index={} before={:#x} after={:#x}",
                    sprite_consumer_trace.ordinal, index,
                    sprite_consumer_trace.gds_before[index], gds_after[index]);
                ++gds_reported;
            }
        }
        LOG_WARNING(
            Render_Vulkan,
            "Dreams sprite indirect consumer GDS summary #{} shader={:#x} changed={} reported={}",
            sprite_consumer_trace.ordinal, cs.pgm_hash, gds_changed, gds_reported);

        for (u32 index = 0; index < sprite_consumer_trace.buffers_before.size(); ++index) {
            const auto& snapshot = sprite_consumer_trace.buffers_before[index];
            if (snapshot.words.empty()) {
                continue;
            }
            const u64 read_size = snapshot.words.size() * sizeof(u32);
            const bool gpu_modified_before =
                buffer_cache.IsRegionGpuModified(snapshot.base, read_size);
            buffer_cache.ReadMemory(snapshot.base, read_size);
            const auto* words = std::bit_cast<const u32*>(snapshot.base);
            u32 changed{};
            u32 first_changed = std::numeric_limits<u32>::max();
            u32 before_value{};
            u32 after_value{};
            u32 nonzero_words{};
            for (u32 word = 0; word < snapshot.words.size(); ++word) {
                nonzero_words += words[word] != 0;
                if (snapshot.words[word] == words[word]) {
                    continue;
                }
                ++changed;
                if (first_changed == std::numeric_limits<u32>::max()) {
                    first_changed = word;
                    before_value = snapshot.words[word];
                    after_value = words[word];
                }
            }
            u32 reported{};
            for (u32 word = 0; word < snapshot.words.size() && reported < 64; ++word) {
                if (words[word] == 0) {
                    continue;
                }
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams sprite indirect consumer nonzero #{} shader={:#x} buffer={} "
                    "dword={} byte={:#x} value={:#x}",
                    sprite_consumer_trace.ordinal, cs.pgm_hash, index, word,
                    static_cast<u64>(word) * sizeof(u32), words[word]);
                ++reported;
            }
            LOG_WARNING(
                Render_Vulkan,
                "Dreams sprite indirect consumer output #{} shader={:#x} buffer={} "
                "base={:#x}+{:#x} gpu_before={} changed={} first={} before={:#x} "
                "after={:#x} nonzero={}",
                sprite_consumer_trace.ordinal, cs.pgm_hash, index, snapshot.base, read_size,
                gpu_modified_before, changed, first_changed, before_value, after_value,
                nonzero_words);
        }
    }
    if (visibility_count_trace.enabled) {
        scheduler.Finish();
        std::array<u32, 21> gds_after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(gds_after.data(), gds->mapped_data.data() + 320 * sizeof(u32),
                    sizeof(gds_after));
        LOG_WARNING(
            Render_Vulkan,
            "Dreams visibility count indirect post hash={:#x} ordinal={} "
            "gds320-330={},{},{},{},{},{},{},{},{},{},{} "
            "gds331-340={},{},{},{},{},{},{},{},{},{}",
            cs.pgm_hash, visibility_count_trace.ordinal, gds_after[0], gds_after[1],
            gds_after[2], gds_after[3], gds_after[4], gds_after[5], gds_after[6],
            gds_after[7], gds_after[8], gds_after[9], gds_after[10], gds_after[11],
            gds_after[12], gds_after[13], gds_after[14], gds_after[15], gds_after[16],
            gds_after[17], gds_after[18], gds_after[19], gds_after[20]);
    }
    if (args_builder_trace.enabled) {
        scheduler.Finish();
        std::array<u32, 24> output_after{};
        std::array<u32, 4> gds_after{};
        if (args_builder_trace.output_base != 0 &&
            args_builder_trace.output_size >= sizeof(output_after)) {
            buffer_cache.ReadMemory(args_builder_trace.output_base, sizeof(output_after));
            if (memory->IsValidMapping(args_builder_trace.output_base, sizeof(output_after))) {
                std::memcpy(output_after.data(),
                            std::bit_cast<const void*>(args_builder_trace.output_base),
                            sizeof(output_after));
            }
        }
        const auto* gds = buffer_cache.GetGdsBuffer();
        constexpr std::array<u32, 4> GdsIndices{258, 259, 515, 516};
        for (u32 i = 0; i < GdsIndices.size(); ++i) {
            std::memcpy(&gds_after[i], gds->mapped_data.data() + GdsIndices[i] * sizeof(u32),
                        sizeof(u32));
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene args builder post #{} out0-2={},{},{} out8-10={},{},{} "
                    "out16-18={},{},{} gds258,259,515,516={},{},{},{}",
                    args_builder_trace.ordinal, output_after[0], output_after[1], output_after[2],
                    output_after[8], output_after[9], output_after[10], output_after[16],
                    output_after[17], output_after[18], gds_after[0], gds_after[1], gds_after[2],
                    gds_after[3]);
    }
    if (scene_stream_trace.enabled) {
        scheduler.Finish();
        u32 counter_after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(&counter_after, gds->mapped_data.data() + 2 * sizeof(u32), sizeof(u32));
        const u32 produced = counter_after >= scene_stream_trace.counter_before
                                 ? counter_after - scene_stream_trace.counter_before
                                 : 0;
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene stream indirect post #{} counter={} produced={} reset={} "
                    "output={:#x}+{:#x}",
                    scene_stream_trace.ordinal, counter_after, produced,
                    counter_after < scene_stream_trace.counter_before,
                    scene_stream_trace.output_base, scene_stream_trace.output_size);

        constexpr u64 RecordSize = 4 * sizeof(u32);
        const u64 record_offset = u64{scene_stream_trace.counter_before} * RecordSize;
        const u32 records_to_read = std::min(produced, 8U);
        const u64 bytes_to_read = u64{records_to_read} * RecordSize;
        if (records_to_read != 0 && scene_stream_trace.output_base != 0 &&
            record_offset <= scene_stream_trace.output_size &&
            bytes_to_read <= scene_stream_trace.output_size - record_offset) {
            const VAddr read_address = scene_stream_trace.output_base + record_offset;
            buffer_cache.ReadMemory(read_address, bytes_to_read);
            if (memory->IsValidMapping(read_address, bytes_to_read)) {
                const auto* records = std::bit_cast<const u32*>(read_address);
                for (u32 record = 0; record < records_to_read; ++record) {
                    const u32* values = records + record * 4;
                    LOG_WARNING(Render_Vulkan,
                                "Dreams scene stream indirect record #{} index={} "
                                "values={:#x},{:#x},{:#x},{:#x}",
                                scene_stream_trace.ordinal,
                                scene_stream_trace.counter_before + record, values[0], values[1],
                                values[2], values[3]);
                }
            }
        }
    }
    if (profile) {
        scheduler.Finish();
    }
    if (trace_dreams_csg_replay) {
        scheduler.Finish();
        log_dreams_csg_buffers("post");
        std::array<u32, 3> dims{};
        std::array<u32, 11> state{};
        read_dreams_csg_indirect_state(dims, state);
        LOG_WARNING(Render_Vulkan,
                    "Dreams CSG replay post args={:#x} dims={}x{}x{} state={:#x} "
                    "d576-586={},{},{},{},{},{},{},{},{},{},{}",
                    args_address, dims[0], dims[1], dims[2], dreams_csg_state_address, state[0],
                    state[1], state[2], state[3], state[4], state[5], state[6], state[7], state[8],
                    state[9], state[10]);
    }
    if (trace_dreams_progress && have_ordered_dims && ordered_dims[0] > 1) {
        static u64 traversal_index{};
        const auto post_gds = read_dreams_gds();
        LOG_WARNING(Render_Vulkan,
                    "Dreams traversal progress #{} groups={} input_base={} input_total={} "
                    "output_base={} output_total={} completion={}",
                    ++traversal_index, ordered_dims[0], progress_pre_gds[3], progress_pre_gds[4],
                    post_gds[3], post_gds[4], post_gds[17]);
    }
    if (profile) {
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - profile_start)
                                 .count();
        LOG_WARNING(Render_Vulkan, "Compute profile hash={:#x} dims=indirect elapsed={:.3f}ms",
                    cs.pgm_hash, elapsed);
        if (profile_dreams) {
            buffer_cache.ReadMemory(args_address, sizeof(std::array<u32, 3>));
            std::array<u32, 3> dims{};
            if (memory->IsValidMapping(args_address, sizeof(dims))) {
                std::memcpy(dims.data(), std::bit_cast<const void*>(args_address), sizeof(dims));
            }
            const auto gds = read_dreams_gds();
            const auto ordered = read_dreams_ordered_scratch();
            LOG_WARNING(Render_Vulkan,
                        "Dreams traversal post args={}x{}x{} old={},{},{} base={} total={} "
                        "dword_base={},{},{},{} byte_base={},{},{},{} sequences={},{},{},{} "
                        "completion={}",
                        dims[0], dims[1], dims[2], gds[0], gds[1], gds[2], gds[3], gds[4],
                        gds[5], gds[6], gds[7], gds[8], gds[9], gds[10], gds[11], gds[12],
                        gds[13], gds[14], gds[15], gds[16], gds[17]);
            LOG_WARNING(Render_Vulkan,
                        "Dreams ordered scratch c0={:#x},{:#x}/{:#x},{:#x} base={:#x} "
                        "c1={:#x},{:#x}/{:#x},{:#x} base={:#x} "
                        "c2={:#x},{:#x}/{:#x},{:#x} base={:#x} "
                        "c3={:#x},{:#x}/{:#x},{:#x} base={:#x}",
                        ordered[0], ordered[1], ordered[2], ordered[3], ordered[4], ordered[5],
                        ordered[6], ordered[7], ordered[8], ordered[9], ordered[10], ordered[11],
                        ordered[12], ordered[13], ordered[14], ordered[15], ordered[16], ordered[17],
                        ordered[18], ordered[19]);
            std::array<u64, Shader::DreamsCompat::OrderedCounterCount> ordered_sums{};
            std::array<u32, Shader::DreamsCompat::OrderedCounterCount> nonzero_groups{};
            std::array<u32, Shader::DreamsCompat::OrderedCounterCount> missing_groups{};
            std::array<u32, Shader::DreamsCompat::OrderedCounterCount> first_nonzero{};
            first_nonzero.fill(std::numeric_limits<u32>::max());
            const auto* gds_buffer = buffer_cache.GetGdsBuffer();
            const u32 group_count = std::min<u32>(
                dims[0], Shader::DreamsCompat::MaxTraversalWorkgroups);
            for (u32 counter = 0; counter < Shader::DreamsCompat::OrderedCounterCount; ++counter) {
                const u32 base = Shader::DreamsCompat::OrderedScratchBaseDword +
                                 counter * Shader::DreamsCompat::OrderedCounterStrideDwords;
                for (u32 group = 0; group < group_count; ++group) {
                    u32 encoded{};
                    const u32 index = base + group * Shader::DreamsCompat::OrderedEntryDwords;
                    std::memcpy(&encoded,
                                gds_buffer->mapped_data.data() + index * sizeof(u32), sizeof(u32));
                    if (encoded == 0) {
                        ++missing_groups[counter];
                        continue;
                    }
                    const u32 count = encoded - 1;
                    ordered_sums[counter] += count;
                    if (count != 0) {
                        ++nonzero_groups[counter];
                        first_nonzero[counter] = std::min(first_nonzero[counter], group);
                    }
                }
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams ordered totals sum={},{},{},{} nonzero_groups={},{},{},{} "
                        "missing={},{},{},{} first={},{},{},{}",
                        ordered_sums[0], ordered_sums[1], ordered_sums[2], ordered_sums[3],
                        nonzero_groups[0], nonzero_groups[1], nonzero_groups[2], nonzero_groups[3],
                        missing_groups[0], missing_groups[1], missing_groups[2], missing_groups[3],
                        first_nonzero[0], first_nonzero[1], first_nonzero[2], first_nonzero[3]);
        }
    }
    DebugState.IncDispatch();

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        stage->PushUd(binding, push_data);
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        // We only use fault buffer for DMA right now.
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (auto& range : mapped_ranges) {
            buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
        }
        fault_process_pending = true;
    }

    return true;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    buffer_bindings.clear();

    const u32 dreams_scene_input_range_count = g_dreams_scene_generated_range_count;
    bool dreams_scene_compute_consumer = false;
    if (g_dreams_scene_graphics_probe_budget != 0 &&
        stage.l_stage == Shader::LogicalStage::Compute) {
        for (const auto& desc : stage.buffers) {
            if (desc.IsSpecial()) {
                continue;
            }
            const auto sharp = desc.GetSharp(stage);
            if (sharp.num_records == std::numeric_limits<u32>::max()) {
                continue;
            }
            for (u32 range_index = 0; range_index < dreams_scene_input_range_count;
                 ++range_index) {
                const auto& generated = g_dreams_scene_generated_ranges[range_index];
                if (OverlapsDreamsRange(sharp.base_address, sharp.GetSize(), generated.base,
                                        generated.size)) {
                    dreams_scene_compute_consumer = true;
                    break;
                }
            }
            if (dreams_scene_compute_consumer) {
                break;
            }
        }
    }

    u32 dreams_stage_bit = 0;
    switch (stage.pgm_hash) {
    case 0x86b79c2a:
        dreams_stage_bit = 1U << 0;
        break;
    case 0x2bfebd3c:
        dreams_stage_bit = 1U << 1;
        break;
    case 0x692f0f7f:
        dreams_stage_bit = 1U << 2;
        break;
    default:
        break;
    }
    static u32 dumped_dreams_stage_metadata{};
    const bool dump_dreams_stage = TraceDreamsDiagnostics() && dreams_stage_bit != 0 &&
                                   (dumped_dreams_stage_metadata & dreams_stage_bit) == 0;
    dumped_dreams_stage_metadata |= dump_dreams_stage ? dreams_stage_bit : 0;

    for (u32 buffer_index = 0; buffer_index < stage.buffers.size(); ++buffer_index) {
        const auto& desc = stage.buffers[buffer_index];
        const auto vsharp = desc.GetSharp(stage);
        if (g_dreams_scene_graphics_probe_budget != 0 && !desc.IsSpecial() &&
            vsharp.num_records != std::numeric_limits<u32>::max() &&
            vsharp.base_address != 0 && vsharp.GetSize() != 0) {
            for (u32 range_index = 0; range_index < dreams_scene_input_range_count;
                 ++range_index) {
                const auto& generated = g_dreams_scene_generated_ranges[range_index];
                if (!OverlapsDreamsRange(vsharp.base_address, vsharp.GetSize(), generated.base,
                                         generated.size)) {
                    continue;
                }
                const DreamsSceneGraphicsBindingTraceKey key{
                    .generation = g_dreams_scene_graphics_probe_generation,
                    .shader_hash = stage.pgm_hash,
                    .stage = stage.l_stage,
                    .binding_index = buffer_index,
                    .base = vsharp.base_address,
                    .size = vsharp.GetSize(),
                };
                if (g_dreams_scene_graphics_bindings.size() < 256 &&
                    std::ranges::find(g_dreams_scene_graphics_bindings, key) ==
                        g_dreams_scene_graphics_bindings.end()) {
                    g_dreams_scene_graphics_bindings.push_back(key);
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams scene data binding generation={} sequence={} shader={:#x} "
                        "stage={} binding={} write={} base={:#x}+{:#x} stride={} records={} "
                        "generated_slot={} generated={:#x}+{:#x}",
                        g_dreams_scene_graphics_probe_generation, g_compute_dispatch_sequence,
                        stage.pgm_hash, static_cast<u32>(stage.l_stage), buffer_index,
                        desc.is_written,
                        vsharp.base_address, vsharp.GetSize(), vsharp.stride, vsharp.num_records,
                        range_index, generated.base, generated.size);
                }
                break;
            }
        }
        if (dreams_scene_compute_consumer && desc.is_written && !desc.IsSpecial() &&
            RememberDreamsSceneGeneratedRange(vsharp.base_address, vsharp.GetSize())) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams scene data propagated generation={} sequence={} shader={:#x} "
                        "binding={} base={:#x}+{:#x} slot={}",
                        g_dreams_scene_graphics_probe_generation, g_compute_dispatch_sequence,
                        stage.pgm_hash, buffer_index, vsharp.base_address, vsharp.GetSize(),
                        g_dreams_scene_generated_range_count - 1);
        }
        if (dump_dreams_stage) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams producer shader={:#x} buffer={} sharp={} type={} write={} "
                        "formatted={} base={:#x} size={:#x} stride={} records={} used={:#x}",
                        stage.pgm_hash, buffer_index, desc.sharp_idx,
                        static_cast<u32>(desc.buffer_type), desc.is_written, desc.is_formatted,
                        vsharp.base_address, vsharp.GetSize(), vsharp.stride, vsharp.num_records,
                        static_cast<u32>(desc.used_types));
        }
        constexpr VAddr DreamsReplayResultProgress = 0x231c0061c;
        static u32 dreams_replay_result_binding_count{};
        if (TraceDreamsReplayResult() && dreams_replay_result_binding_count < 256 &&
            !desc.IsSpecial() && vsharp.base_address <= DreamsReplayResultProgress &&
            DreamsReplayResultProgress - vsharp.base_address < vsharp.GetSize()) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams replay result binding #{} shader={:#x} stage={} binding={} "
                        "write={} formatted={} base={:#x}+{:#x} stride={} records={} "
                        "progress_offset={:#x} types={:#x}",
                        dreams_replay_result_binding_count++, stage.pgm_hash,
                        static_cast<u32>(stage.l_stage), buffer_index, desc.is_written,
                        desc.is_formatted, vsharp.base_address, vsharp.GetSize(), vsharp.stride,
                        vsharp.num_records, DreamsReplayResultProgress - vsharp.base_address,
                        static_cast<u32>(desc.used_types));
        }
        if (TrackDreamsBufferWriters() &&
            (desc.is_written || TraceDreamsProducerSources()) && !desc.IsSpecial() &&
            vsharp.base_address != 0 && vsharp.GetSize() != 0) {
            RecordDreamsBufferWriter({
                .sequence = ++g_dreams_buffer_writer_sequence,
                .dispatch_sequence = g_compute_dispatch_sequence,
                .shader_hash = stage.pgm_hash,
                .stage = stage.l_stage,
                .kind = DreamsBufferWriter::Kind::Shader,
                .binding_index = buffer_index,
                .declared_write = desc.is_written,
                .base = vsharp.base_address,
                .size = vsharp.GetSize(),
                .stride = static_cast<u32>(vsharp.stride),
                .source = 0,
            });
        }
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            if (TraceDreamsProducerSources() &&
                !Shader::DreamsCompat::IsSpriteCullShader(stage.pgm_hash) &&
                g_dreams_sprite_batch_producer_sequence != 0 &&
                g_dreams_sprite_batch_consumer_trace_count < 128) {
                for (u32 output_index = 0;
                     output_index < g_dreams_sprite_batch_output_ranges.size(); ++output_index) {
                    const auto output = g_dreams_sprite_batch_output_ranges[output_index];
                    if (!OverlapsDreamsRange(vsharp.base_address, size, output.base, output.size)) {
                        continue;
                    }
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams sprite batch consumer #{} producer_dispatch={} dispatch={} "
                        "shader={:#x} stage={} binding={} output={} write={} formatted={} "
                        "type={} base={:#x}+{:#x} stride={} records={}",
                        ++g_dreams_sprite_batch_consumer_trace_count,
                        g_dreams_sprite_batch_producer_sequence, g_compute_dispatch_sequence,
                        stage.pgm_hash, static_cast<u32>(stage.l_stage), buffer_index, output_index,
                        desc.is_written, desc.is_formatted, static_cast<u32>(desc.buffer_type),
                        vsharp.base_address, size, vsharp.stride, vsharp.num_records);
                    break;
                }
            }
            if (TraceDreamsProducerSources() &&
                g_dreams_sprite_geometry_producer_sequence != 0 &&
                g_dreams_sprite_geometry_consumer_trace_count < 128 &&
                !(vsharp.stride == 0 &&
                  vsharp.num_records == std::numeric_limits<u32>::max())) {
                for (u32 output_index = 0;
                     output_index < g_dreams_sprite_geometry_output_ranges.size();
                     ++output_index) {
                    const auto output = g_dreams_sprite_geometry_output_ranges[output_index];
                    if (!OverlapsDreamsRange(vsharp.base_address, size, output.base, output.size)) {
                        continue;
                    }
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams sprite geometry consumer #{} producer_dispatch={} dispatch={} "
                        "shader={:#x} stage={} binding={} output={} write={} formatted={} "
                        "type={} base={:#x}+{:#x} stride={} records={}",
                        ++g_dreams_sprite_geometry_consumer_trace_count,
                        g_dreams_sprite_geometry_producer_sequence, g_compute_dispatch_sequence,
                        stage.pgm_hash, static_cast<u32>(stage.l_stage), buffer_index, output_index,
                        desc.is_written, desc.is_formatted, static_cast<u32>(desc.buffer_type),
                        vsharp.base_address, size, vsharp.stride, vsharp.num_records);
                    break;
                }
            }
            constexpr VAddr DreamsIndirectArgsAddress = 0x23b4fe900;
            static u32 dreams_args_binding_count{};
            if (TraceDreamsDiagnostics() &&
                vsharp.base_address <= DreamsIndirectArgsAddress &&
                DreamsIndirectArgsAddress < vsharp.base_address + size &&
                dreams_args_binding_count < 256) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams args binding #{} shader={:#x} write={} formatted={} base={:#x} "
                            "size={:#x} stride={} records={}",
                            dreams_args_binding_count++, stage.pgm_hash, desc.is_written,
                            desc.is_formatted, vsharp.base_address, size, vsharp.stride,
                            vsharp.num_records);
            }
            if (vsharp.stride == 0 && vsharp.num_records == std::numeric_limits<u32>::max() &&
                size > MaxUnboundedGuestBufferWindow) {
                static bool reported_unbounded_window = false;
                if (!reported_unbounded_window) {
                    LOG_WARNING(Render_Vulkan,
                                "Windowing unbounded guest buffer shader={:#x} stage={} "
                                "base={:#x} range={:#x} to {:#x}",
                                stage.pgm_hash, static_cast<u32>(stage.l_stage), vsharp.base_address,
                                size, MaxUnboundedGuestBufferWindow);
                    reported_unbounded_window = true;
                }
                size = MaxUnboundedGuestBufferWindow;
            }
            const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, size);
            constexpr VAddr DreamsQueueAddress = 0x2e3ae0000;
            static u32 dreams_queue_find_count{};
            if (TraceDreamsDiagnostics() &&
                vsharp.base_address == DreamsQueueAddress && dreams_queue_find_count < 12) {
                const auto& cache_buffer = buffer_cache.GetBuffer(buffer_id);
                LOG_WARNING(Render_Vulkan,
                            "Dreams queue find #{} shader={:#x} descriptor={} id={} guest={:#x}+"
                            "{:#x} cache={:#x}+{:#x} cache_offset={:#x}",
                            dreams_queue_find_count++, stage.pgm_hash, buffer_index,
                            buffer_id.index, vsharp.base_address, size, cache_buffer.CpuAddr(),
                            cache_buffer.SizeBytes(), cache_buffer.Offset(vsharp.base_address));
            }
            buffer_bindings.emplace_back(buffer_id, vsharp, size);
        } else {
            buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, 0);
        }
    }

    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const bool is_storage = desc.IsStorage(vsharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        // Buffer is not from the cache, either a special buffer or unbound.
        if (!buffer_id) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                const vk::AccessFlags2 access_mask =
                    desc.is_written
                        ? vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
                        : vk::AccessFlagBits2::eShaderRead;
                if (auto barrier = gds_buf->GetBarrier(
                        access_mask, vk::PipelineStageFlagBits2::eAllCommands, 0, true)) {
                    buffer_barriers.emplace_back(*barrier);
                }
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (liverpool->regs.clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = liverpool->regs.clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
                vsharp.base_address, size, desc.is_written, desc.is_formatted, buffer_id);
            constexpr VAddr DreamsQueueAddress = 0x2e3ae0000;
            static u32 dreams_queue_obtain_count{};
            if (TraceDreamsDiagnostics() &&
                vsharp.base_address == DreamsQueueAddress && dreams_queue_obtain_count < 12) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams queue obtain #{} shader={:#x} descriptor={} id={} host={:#x} "
                            "offset={:#x} range={:#x} write={}",
                            dreams_queue_obtain_count++, stage.pgm_hash, i, buffer_id.index,
                            reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(vk_buffer->Handle())),
                            offset, size, desc.is_written);
            }
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
            const vk::AccessFlags2 access_mask =
                desc.is_written
                    ? vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
                    : vk::AccessFlagBits2::eShaderRead;
            const bool force_dependency = desc.is_written && ForceDreamsStorageDependencies();
            if (auto barrier = vk_buffer->GetBarrier(
                    access_mask, vk::PipelineStageFlagBits2::eAllCommands, 0, force_dependency)) {
                buffer_barriers.emplace_back(*barrier);
            }
            const bool is_dreams_scene_stream =
                Common::ElfInfo::Instance().GameSerial() == "CUSA04301" &&
                stage.pgm_hash == 0x34267ace && i == 0;
            if (desc.is_written && (desc.is_formatted || is_dreams_scene_stream)) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size,
                                                      is_dreams_scene_stream);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eUniformBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }

    std::optional<std::filesystem::path> dreams_input_dump_dir;
    std::string_view dreams_input_dump_role;
    if (stage.pgm_hash == DreamsFleckFragmentShader) {
        dreams_input_dump_dir = ConsumeDreamsFleckDumpRequest();
        dreams_input_dump_role = "fleck";
    } else if (stage.pgm_hash == DreamsSpriteGeometryVertexShader) {
        dreams_input_dump_dir = ConsumeDreamsGeometryInputDumpRequest();
        dreams_input_dump_role = "geometry";
    }
    if (dreams_input_dump_dir) {
        const auto& output_dir = *dreams_input_dump_dir;
            constexpr u64 MaxDumpRange = 8_MB;
            LOG_WARNING(Render_Vulkan, "Dumping Dreams {} inputs to {}", dreams_input_dump_role,
                        output_dir.string());
            for (u32 i = 0; i < buffer_bindings.size(); ++i) {
                const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
                const auto& desc = stage.buffers[i];
                const bool gpu_modified =
                    buffer_id && buffer_cache.IsRegionGpuModified(vsharp.base_address, size);
                LOG_WARNING(Render_Vulkan,
                            "Dreams {} input buffer={} special={} write={} formatted={} "
                            "base={:#x} size={:#x} stride={} records={} gpu_modified={}",
                            dreams_input_dump_role, i, desc.IsSpecial(), desc.is_written,
                            desc.is_formatted,
                            vsharp.base_address, size, vsharp.stride, vsharp.num_records,
                            gpu_modified);
                if (!buffer_id || vsharp.base_address == 0 || size == 0) {
                    continue;
                }

                u32 writer_matches{};
                const VAddr input_end = vsharp.base_address + size;
                for (auto writer = g_dreams_buffer_writers.rbegin();
                     writer != g_dreams_buffer_writers.rend() && writer_matches < 24; ++writer) {
                    const VAddr writer_end = writer->base + writer->size;
                    if (writer->base >= input_end || vsharp.base_address >= writer_end) {
                        continue;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams {} input writer buffer={} match={} seq={} dispatch={} "
                                "kind={} shader={:#x} stage={} binding={} declared_write={} "
                                "range={:#x}+{:#x}",
                                dreams_input_dump_role, i, writer_matches, writer->sequence,
                                writer->dispatch_sequence,
                                static_cast<u32>(writer->kind), writer->shader_hash,
                                static_cast<u32>(writer->stage), writer->binding_index,
                                writer->declared_write, writer->base, writer->size);
                    ++writer_matches;
                }
                LOG_WARNING(Render_Vulkan, "Dreams {} input buffer={} writer_matches={}",
                            dreams_input_dump_role, i, writer_matches);

                const auto dump_range = [&](std::string_view suffix, VAddr address,
                                            u64 range_size) {
                    if (range_size == 0 || !memory->IsValidMapping(address, range_size)) {
                        LOG_WARNING(Render_Vulkan,
                                    "Dreams {} input buffer={} {} range is not mapped: "
                                    "{:#x}+{:#x}",
                                    dreams_input_dump_role, i, suffix, address, range_size);
                        return;
                    }
                    buffer_cache.ReadMemory(address, range_size);
                    const auto* data = std::bit_cast<const u8*>(address);
                    const u64 nonzero =
                        std::count_if(data, data + range_size, [](u8 value) { return value != 0; });
                    const auto file = output_dir /
                                      fmt::format("buffer-{}-{:#x}-{}.bin", i,
                                                  vsharp.base_address, suffix);
                    std::ofstream stream{file, std::ios::binary | std::ios::trunc};
                    stream.write(reinterpret_cast<const char*>(data), range_size);
                    LOG_WARNING(Render_Vulkan,
                                "Dreams {} input buffer={} {} dumped={:#x} nonzero_bytes={} "
                                "file={}",
                                dreams_input_dump_role, i, suffix, range_size, nonzero,
                                file.string());
                };

                const u64 head_size = std::min(size, MaxDumpRange);
                dump_range("head", vsharp.base_address, head_size);
                if (size > MaxDumpRange) {
                    const u64 tail_size = std::min(size - MaxDumpRange, MaxDumpRange);
                    dump_range("tail", vsharp.base_address + size - tail_size, tail_size);
                }
            }

            const auto userdata_file = output_dir / "flattened-userdata.bin";
            std::ofstream userdata{userdata_file, std::ios::binary | std::ios::trunc};
            userdata.write(reinterpret_cast<const char*>(stage.flattened_ud_buf.data()),
                           stage.flattened_ud_buf.size() * sizeof(u32));
            LOG_WARNING(Render_Vulkan, "Dreams {} flattened userdata words={} file={}",
                        dreams_input_dump_role, stage.flattened_ud_buf.size(),
                        userdata_file.string());
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // For loading/storing to explicit mip levels, when no native instruction support, bind an array
    // of descriptors consecutively, 1 for each mip level. The shader can index this with LOD
    // operand.
    // This array holds the size of each consecutive array with the number of bindings consumed.
    // This is currently always 1 for anything other than mip fallback arrays.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    struct DreamsDecoderConsumerKey {
        u64 shader_hash;
        u32 stage;
        u32 binding;
        u32 image_id;
        VAddr base;
        u32 width;
        u32 height;
        u32 depth;
        u32 base_level;
        u32 levels;
        u32 base_layer;
        u32 layers;

        bool operator==(const DreamsDecoderConsumerKey&) const = default;
    };
    static std::vector<DreamsDecoderConsumerKey> dreams_decoder_consumers;

    const auto bind_null_image = [&](bool is_storage) {
        auto& [image_id, desc] =
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
        desc.type = is_storage ? VideoCore::TextureCache::BindingType::Storage
                               : VideoCore::TextureCache::BindingType::Texture;
        desc.view_info.is_storage = is_storage;
    };

    u32 stage_image_index{};
    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (tsharp.Address() == 0 || tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
            bind_null_image(image_desc.is_written);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (TraceDreamsSceneStream() && stage.pgm_hash == 0x34267ace &&
                stage_image_index == 1) {
                static u32 volume_source_trace_count{};
                if (volume_source_trace_count++ < 8) {
                    std::array<u64, sizeof(AmdGpu::Image) / sizeof(u64)> raw{};
                    static_assert(sizeof(raw) == sizeof(tsharp));
                    std::memcpy(raw.data(), &tsharp, sizeof(tsharp));
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams volume source #{} id={} raw={:#x},{:#x},{:#x},{:#x} "
                        "guest={:#x}+{:#x} descriptor={}x{}x{} pitch={} levels={} type={} "
                        "format={}/{} tile={} bank={} cached={}x{}x{} cached_pitch={} flags={:#x}",
                        volume_source_trace_count, image_id.index, raw[0], raw[1], raw[2], raw[3],
                        tsharp.Address(), image->info.guest_size, tsharp.width + 1,
                        tsharp.height + 1, tsharp.depth + 1, tsharp.Pitch(), tsharp.NumLevels(),
                        AmdGpu::NameOf(tsharp.GetType()), AmdGpu::NameOf(tsharp.GetDataFmt()),
                        AmdGpu::NameOf(tsharp.GetNumberFmt()), AmdGpu::NameOf(tsharp.GetTileMode()),
                        tsharp.GetBankSwizzle(), image->info.size.width, image->info.size.height,
                        image->info.size.depth, image->info.pitch,
                    static_cast<u32>(image->flags));
                }
            }
            if (TraceDreamsFleckRendering() && stage.pgm_hash == DreamsFleckFragmentShader &&
                (stage_image_index == 4 || stage_image_index == 5)) {
                static u32 visibility_source_trace_count{};
                if (visibility_source_trace_count++ < 8) {
                    const VAddr target_base = image->info.guest_address;
                    const u64 target_size = image->info.guest_size;
                    const u64 target_end = target_base + target_size;
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams visibility source #{} binding={} id={} guest={:#x}+{:#x} "
                        "pitch={} extent={}x{}x{} format={} tile={} flags={:#x}",
                        visibility_source_trace_count, stage_image_index, image_id.index,
                        target_base, target_size, image->info.pitch, image->info.size.width,
                        image->info.size.height, image->info.size.depth,
                        vk::to_string(desc.view_info.format), AmdGpu::NameOf(image->info.tile_mode),
                        static_cast<u32>(image->flags));

                    u32 writer_matches{};
                    for (auto writer = g_dreams_image_writers.rbegin();
                         writer != g_dreams_image_writers.rend() && writer_matches < 24;
                         ++writer) {
                        const u64 writer_end = writer->base + writer->size;
                        if (writer->kind == DreamsImageWriter::Kind::Storage ||
                            writer->base >= target_end || target_base >= writer_end) {
                            continue;
                        }
                        LOG_WARNING(
                            Render_Vulkan,
                            "Dreams visibility writer source={} match={} seq={} dispatch={} "
                            "shader={:#x} stage={} kind={} target={} id={} range={:#x}+{:#x}",
                            stage_image_index, writer_matches, writer->sequence,
                            writer->dispatch_sequence, writer->shader_hash,
                            static_cast<u32>(writer->stage), static_cast<u32>(writer->kind),
                            writer->binding_index, writer->image_id, writer->base, writer->size);
                        ++writer_matches;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams visibility writer summary source={} matches={} history={}",
                                stage_image_index, writer_matches, g_dreams_image_writers.size());
                }
            }
            if (TraceDreamsProducerSources() && stage.pgm_hash == 0xc2a7fbf1 &&
                stage_image_index == 0) {
                static u32 scene_image_source_trace_count{};
                if (scene_image_source_trace_count++ < 8) {
                    const VAddr target_base = image->info.guest_address;
                    const u64 target_size = image->info.guest_size;
                    const u64 target_end = target_base + target_size;
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams scene image dispatch={} id={} guest={:#x}+{:#x} pitch={} "
                        "extent={}x{}x{} tile={} image_writers={} buffer_writers={}",
                        g_compute_dispatch_sequence, image_id.index, target_base, target_size,
                        image->info.pitch, image->info.size.width, image->info.size.height,
                        image->info.size.depth, AmdGpu::NameOf(image->info.tile_mode),
                        g_dreams_image_writers.size(), g_dreams_buffer_writers.size());

                    u32 image_matches{};
                    for (auto writer = g_dreams_image_writers.rbegin();
                         writer != g_dreams_image_writers.rend() && image_matches < 64; ++writer) {
                        const u64 writer_end = writer->base + writer->size;
                        if (writer->base >= target_end || target_base >= writer_end) {
                            continue;
                        }
                        LOG_WARNING(Render_Vulkan,
                                    "Dreams scene image writer seq={} dispatch={} kind={} "
                                    "shader={:#x} stage={} binding={} id={} range={:#x}+{:#x}",
                                    writer->sequence, writer->dispatch_sequence,
                                    static_cast<u32>(writer->kind), writer->shader_hash,
                                    static_cast<u32>(writer->stage), writer->binding_index,
                                    writer->image_id, writer->base, writer->size);
                        ++image_matches;
                    }

                    u32 buffer_matches{};
                    for (auto writer = g_dreams_buffer_writers.rbegin();
                         writer != g_dreams_buffer_writers.rend() && buffer_matches < 32;
                         ++writer) {
                        const u64 writer_end = writer->base + writer->size;
                        if (!writer->declared_write || writer->base >= target_end ||
                            target_base >= writer_end) {
                            continue;
                        }
                        LOG_WARNING(Render_Vulkan,
                                    "Dreams scene buffer writer seq={} dispatch={} kind={} "
                                    "shader={:#x} stage={} binding={} range={:#x}+{:#x}",
                                    writer->sequence, writer->dispatch_sequence,
                                    static_cast<u32>(writer->kind), writer->shader_hash,
                                    static_cast<u32>(writer->stage), writer->binding_index,
                                    writer->base, writer->size);
                        ++buffer_matches;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams scene image summary image_matches={} buffer_matches={}",
                                image_matches, buffer_matches);
                }
            }
            if (TraceDreamsFleckRendering() && !image_desc.is_written) {
                const DreamsImageWriter* decoder_writer{};
                for (auto writer = g_dreams_image_writers.rbegin();
                     writer != g_dreams_image_writers.rend(); ++writer) {
                    if (writer->shader_hash == DreamsFleckFragmentShader &&
                        OverlapsDreamsRange(writer->base, writer->size,
                                           image->info.guest_address, image->info.guest_size)) {
                        decoder_writer = &*writer;
                        break;
                    }
                }

                if (decoder_writer != nullptr && dreams_decoder_consumers.size() < 256) {
                    const DreamsDecoderConsumerKey key{
                        .shader_hash = stage.pgm_hash,
                        .stage = static_cast<u32>(stage.l_stage),
                        .binding = stage_image_index,
                        .image_id = image_id.index,
                        .base = image->info.guest_address,
                        .width = static_cast<u32>(tsharp.width) + 1,
                        .height = static_cast<u32>(tsharp.height) + 1,
                        .depth = static_cast<u32>(tsharp.depth) + 1,
                        .base_level = static_cast<u32>(desc.view_info.range.base.level),
                        .levels = desc.view_info.range.extent.levels,
                        .base_layer = static_cast<u32>(desc.view_info.range.base.layer),
                        .layers = desc.view_info.range.extent.layers,
                    };
                    if (std::ranges::find(dreams_decoder_consumers, key) ==
                        dreams_decoder_consumers.end()) {
                        dreams_decoder_consumers.push_back(key);
                        std::array<u64, sizeof(AmdGpu::Image) / sizeof(u64)> raw{};
                        static_assert(sizeof(raw) == sizeof(tsharp));
                        std::memcpy(raw.data(), &tsharp, sizeof(tsharp));
                        LOG_WARNING(
                            Render_Vulkan,
                            "Dreams decoder consumer #{} shader={:#x} stage={} binding={} id={} "
                            "raw={:#x},{:#x},{:#x},{:#x} guest={:#x}+{:#x} "
                            "descriptor={}x{}x{} pitch={} levels={} type={} format={}/{} tile={} "
                            "cached={}x{}x{} cached_pitch={} view_format={} level={}+{} "
                            "layer={}+{} writer_target={} writer_id={} writer_seq={}",
                            dreams_decoder_consumers.size(), stage.pgm_hash,
                            static_cast<u32>(stage.l_stage), stage_image_index, image_id.index,
                            raw[0], raw[1], raw[2], raw[3], image->info.guest_address,
                            image->info.guest_size, tsharp.width + 1, tsharp.height + 1,
                            tsharp.depth + 1, tsharp.Pitch(), tsharp.NumLevels(),
                            AmdGpu::NameOf(tsharp.GetType()), AmdGpu::NameOf(tsharp.GetDataFmt()),
                            AmdGpu::NameOf(tsharp.GetNumberFmt()), AmdGpu::NameOf(tsharp.GetTileMode()),
                            image->info.size.width, image->info.size.height, image->info.size.depth,
                            image->info.pitch, vk::to_string(desc.view_info.format),
                            desc.view_info.range.base.level, desc.view_info.range.extent.levels,
                            desc.view_info.range.base.layer, desc.view_info.range.extent.layers,
                            decoder_writer->binding_index, decoder_writer->image_id,
                            decoder_writer->sequence);
                    }
                }
            }
            if (TrackDreamsImageWriters() && image_desc.is_written) {
                RecordDreamsImageWriter({
                    .sequence = ++g_dreams_image_writer_sequence,
                    .dispatch_sequence = g_compute_dispatch_sequence,
                    .shader_hash = stage.pgm_hash,
                    .stage = stage.l_stage,
                    .kind = DreamsImageWriter::Kind::Storage,
                    .binding_index = stage_image_index,
                    .image_id = image_id.index,
                    .base = image->info.guest_address,
                    .size = image->info.guest_size,
                });
            }
            if (image->binding.is_bound) {
                // A duplicated image must remain in general layout if either binding is storage.
                // Track the first binding as well so this is independent of descriptor order.
                image->binding.force_general |=
                    image->binding.has_storage || image_desc.is_written;
            }
            image->binding.has_storage |= image_desc.is_written;
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
        ++stage_image_index;
    }

    // Second pass to re-bind images that were updated after binding
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            vk::ImageLayout descriptor_layout{};
            if ((image.binding.force_general || image.binding.is_target) &&
                !image.info.props.is_depth) {
                descriptor_layout = instance.IsAttachmentFeedbackLoopLayoutSupported() &&
                                            image.binding.is_target
                                        ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                        : vk::ImageLayout::eGeneral;
                image.Transit(descriptor_layout,
                              vk::AccessFlagBits2::eShaderRead |
                                  (image.info.props.is_depth
                                       ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                       : vk::AccessFlagBits2::eColorAttachmentWrite |
                                             vk::AccessFlagBits2::eColorAttachmentRead),
                              {});
            } else {
                if (is_storage) {
                    descriptor_layout = vk::ImageLayout::eGeneral;
                    image.Transit(descriptor_layout,
                                  vk::AccessFlagBits2::eShaderRead |
                                      vk::AccessFlagBits2::eShaderWrite,
                                  desc.view_info.range);
                } else {
                    descriptor_layout = image.info.props.is_depth
                                            ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                            : vk::ImageLayout::eShaderReadOnlyOptimal;
                    image.Transit(descriptor_layout, vk::AccessFlagBits2::eShaderRead,
                                  desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view, descriptor_layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    u32 sampler_index{};
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }

        u32 associated_binding = 0;
        for (u32 image_index = 0; image_index < sampler.associated_image; ++image_index) {
            associated_binding += image_descriptor_array_sizes[image_index];
        }
        if (associated_binding < image_bindings.size()) {
            const auto& [image_id, desc] = image_bindings[associated_binding];
            if (image_id) {
                const auto& image = texture_cache.GetImage(image_id);
                const auto view_format =
                    instance.GetSupportedFormat(desc.view_info.format, image.format_features);
                if (!instance.IsFormatSupported(
                        view_format, vk::FormatFeatureFlagBits2::eSampledImageFilterLinear)) {
                    if (TraceDreamsProducerSources()) {
                        static u32 unsupported_filter_trace_count{};
                        if (unsupported_filter_trace_count++ < 128) {
                            LOG_WARNING(
                                Render_Vulkan,
                                "Dreams integer filter shader={:#x} stage={} sampler={} image={} "
                                "binding={} guest={:#x}+{:#x} extent={}x{}x{} format={}",
                                stage.pgm_hash, static_cast<u32>(stage.l_stage), sampler_index,
                                sampler.associated_image, image_id.index,
                                image.info.guest_address, image.info.guest_size,
                                image.info.size.width, image.info.size.height,
                                image.info.size.depth, vk::to_string(view_format));
                        }
                    }
                    // Vulkan does not permit linear or anisotropic filtering for integer formats.
                    ssharp.xy_mag_filter.Assign(AmdGpu::Filter::Point);
                    ssharp.xy_min_filter.Assign(AmdGpu::Filter::Point);
                    if (ssharp.mip_filter == AmdGpu::MipFilter::Linear) {
                        ssharp.mip_filter.Assign(AmdGpu::MipFilter::Point);
                    }
                    ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
                }
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
        ++sampler_index;
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    u64 dreams_render_shader_hash{};
    Shader::LogicalStage dreams_render_stage = Shader::LogicalStage::Vertex;
    if (TrackDreamsImageWriters()) {
        for (const auto* shader : pipeline->GetStages()) {
            if (shader != nullptr &&
                (dreams_render_shader_hash == 0 ||
                 shader->l_stage == Shader::LogicalStage::Fragment)) {
                dreams_render_shader_hash = shader->pgm_hash;
                dreams_render_stage = shader->l_stage;
            }
        }
    }
    RenderState state{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        if (TrackDreamsImageWriters()) {
            RecordDreamsImageWriter({
                .sequence = ++g_dreams_image_writer_sequence,
                .dispatch_sequence = g_compute_dispatch_sequence,
                .shader_hash = dreams_render_shader_hash,
                .stage = dreams_render_stage,
                .kind = DreamsImageWriter::Kind::ColorTarget,
                .binding_index = cb,
                .image_id = image_id.index,
                .base = image->info.guest_address,
                .size = image->info.guest_size,
            });
        }
        texture_cache.UpdateImage(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        if (TrackDreamsImageWriters() &&
            (regs.depth_control.depth_write_enable || stencil_write)) {
            RecordDreamsImageWriter({
                .sequence = ++g_dreams_image_writer_sequence,
                .dispatch_sequence = g_compute_dispatch_sequence,
                .shader_hash = dreams_render_shader_hash,
                .stage = dreams_render_stage,
                .kind = DreamsImageWriter::Kind::DepthTarget,
                .binding_index = std::numeric_limits<u32>::max(),
                .image_id = image_id.index,
                .base = image.info.guest_address,
                .size = image.info.guest_size,
            });
        }
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        image.Transit(new_layout,
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                      desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    if (is_gds && TraceDreamsDiagnostics()) {
        static u32 dreams_gds_fill_count{};
        if (dreams_gds_fill_count < 256) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams GDS fill #{} offset={:#x} dword={} bytes={:#x} value={:#x}",
                        dreams_gds_fill_count++, address, address / sizeof(u32), num_bytes, value);
        }
    }
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
    if (TrackDreamsBufferWriters() && !is_gds && address != 0 && num_bytes != 0) {
        RecordDreamsBufferWriter({
            .sequence = ++g_dreams_buffer_writer_sequence,
            .dispatch_sequence = g_compute_dispatch_sequence,
            .shader_hash = 0,
            .stage = Shader::LogicalStage::Compute,
            .kind = DreamsBufferWriter::Kind::Fill,
            .binding_index = std::numeric_limits<u32>::max(),
            .declared_write = true,
            .base = address,
            .size = num_bytes,
            .stride = 0,
            .source = value,
        });
    }
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    const bool trace_gds_copy = dst_gds || src_gds;
    u32 dreams_scene_source_slot = std::numeric_limits<u32>::max();
    if (g_dreams_scene_graphics_probe_budget != 0 && !dst_gds && !src_gds) {
        for (u32 range_index = 0; range_index < g_dreams_scene_generated_range_count;
             ++range_index) {
            const auto& generated = g_dreams_scene_generated_ranges[range_index];
            if (OverlapsDreamsRange(src, num_bytes, generated.base, generated.size)) {
                dreams_scene_source_slot = range_index;
                break;
            }
        }
    }
    if (trace_gds_copy && TraceDreamsDiagnostics()) {
        static u32 dreams_gds_copy_count{};
        if (dreams_gds_copy_count < 512) {
            const bool src_cpu_modified =
                !src_gds && buffer_cache.IsRegionCpuModified(src, num_bytes);
            const bool src_gpu_modified =
                !src_gds && buffer_cache.IsRegionGpuModified(src, num_bytes);
            LOG_WARNING(Render_Vulkan,
                        "Dreams GDS copy #{} dst={:#x} src={:#x} bytes={:#x} dst_gds={} "
                        "src_gds={} src_cpu_modified={} src_gpu_modified={}",
                        dreams_gds_copy_count++, dst, src, num_bytes, dst_gds, src_gds,
                        src_cpu_modified, src_gpu_modified);
        }
    }
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
    if (dreams_scene_source_slot != std::numeric_limits<u32>::max() &&
        RememberDreamsSceneGeneratedRange(dst, num_bytes)) {
        LOG_WARNING(Render_Vulkan,
                    "Dreams scene data copied generation={} sequence={} source_slot={} "
                    "src={:#x} dst={:#x} bytes={:#x} slot={}",
                    g_dreams_scene_graphics_probe_generation, g_compute_dispatch_sequence,
                    dreams_scene_source_slot, src, dst, num_bytes,
                    g_dreams_scene_generated_range_count - 1);
    }
    if (TrackDreamsBufferWriters() && !dst_gds && dst != 0 && num_bytes != 0) {
        RecordDreamsBufferWriter({
            .sequence = ++g_dreams_buffer_writer_sequence,
            .dispatch_sequence = g_compute_dispatch_sequence,
            .shader_hash = 0,
            .stage = Shader::LogicalStage::Compute,
            .kind = DreamsBufferWriter::Kind::Copy,
            .binding_index = std::numeric_limits<u32>::max(),
            .declared_write = true,
            .base = dst,
            .size = num_bytes,
            .stride = 0,
            .source = src,
        });
    }

    // ReadGDSCounters is emitted as larger DMA segments rather than a direct 45-dword copy. Capture
    // both plausible counter windows when its distinctive segments appear.
    const bool is_counter_read_segment =
        src_gds && (num_bytes == 0xbf00 || num_bytes == 0xc00 || num_bytes == 0xb4);
    if (TraceDreamsDiagnostics() && is_counter_read_segment) {
        static u32 snapshot_count{};
        if (snapshot_count++ < 24) {
            scheduler.Finish();

            const auto* gds = buffer_cache.GetGdsBuffer();
            const auto log_window = [&](u32 offset, std::string_view name) {
                std::array<u32, 45> counters{};
                std::memcpy(counters.data(), gds->mapped_data.data() + offset, sizeof(counters));
                LOG_WARNING(Render_Vulkan,
                            "Dreams GDS snapshot #{} {}@{:#x} values[0..14]="
                            "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
                            snapshot_count, name, offset, counters[0], counters[1], counters[2],
                            counters[3], counters[4], counters[5], counters[6], counters[7],
                            counters[8], counters[9], counters[10], counters[11], counters[12],
                            counters[13], counters[14]);
                LOG_WARNING(Render_Vulkan,
                            "Dreams GDS snapshot #{} {} values[15..29]="
                            "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
                            snapshot_count, name, counters[15], counters[16], counters[17],
                            counters[18], counters[19], counters[20], counters[21], counters[22],
                            counters[23], counters[24], counters[25], counters[26], counters[27],
                            counters[28], counters[29]);
                LOG_WARNING(Render_Vulkan,
                            "Dreams GDS snapshot #{} {} values[30..44]="
                            "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
                            snapshot_count, name, counters[30], counters[31], counters[32],
                            counters[33], counters[34], counters[35], counters[36], counters[37],
                            counters[38], counters[39], counters[40], counters[41], counters[42],
                            counters[43], counters[44]);
            };
            LOG_WARNING(Render_Vulkan,
                        "Dreams GDS snapshot #{} copy dst={:#x} src={:#x} bytes={:#x} sequence={}",
                        snapshot_count, dst, src, num_bytes, g_compute_dispatch_sequence);
            log_window(0xc00, "low");
            log_window(0xbf00, "high");

            const u32 available = std::min<u32>(g_recent_compute_dispatch_index,
                                                g_recent_compute_dispatches.size());
            const u32 first = g_recent_compute_dispatch_index - available;
            for (u32 i = 0; i < available; ++i) {
                const auto& dispatch =
                    g_recent_compute_dispatches[(first + i) % g_recent_compute_dispatches.size()];
                LOG_WARNING(Render_Vulkan,
                            "Dreams recent compute snapshot={} age={} hash={:#x} dims={}x{}x{}",
                            snapshot_count, available - i, dispatch.hash, dispatch.dim_x,
                            dispatch.dim_y, dispatch.dim_z);
            }
        }
    }
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    RecordDreamsCpuWrite(addr, size);
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);
    return true;
}

void Rasterizer::ProcessDownloadImages() {
    texture_cache.ProcessDownloadImages();
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    static const bool dreams_clip_viewport_fallback_enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_CLIP_VIEWPORT_FALLBACK");
        return value != nullptr && value[0] == '1';
    }();
    const bool dreams_clip_viewport_fallback =
        dreams_clip_viewport_fallback_enabled &&
        Common::ElfInfo::Instance().GameSerial() == "CUSA04301" && regs.IsClipDisabled();
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        const bool use_clip_viewport_fallback =
            dreams_clip_viewport_fallback && i == 0 && vp.xscale == 0;
        if (vp.xscale == 0 && !use_clip_viewport_fallback) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
        if (use_clip_viewport_fallback) {
            static std::atomic<u32> trace_count{0};
            const u32 ordinal = trace_count.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 64) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams clip-disabled viewport fallback #{} viewport={}x{} "
                            "scissor={},{} {}x{} control={},{},{},{}",
                            ordinal, viewport.width, viewport.height, vp_scsr.top_left_x,
                            vp_scsr.top_left_y, vp_scsr.GetWidth(), vp_scsr.GetHeight(),
                            vp_ctl.xoffset_enable, vp_ctl.xscale_enable, vp_ctl.yoffset_enable,
                            vp_ctl.yscale_enable);
            }
        }
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
