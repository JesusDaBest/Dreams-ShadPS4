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
#include <span>
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
        // DS_ORDERED_COUNT must observe wave-creation order. The traversal shader has exactly one
        // 64-lane guest wave per workgroup, so dispatching indirect workgroups individually is a
        // safe (if slower) way to provide that ordering without a cross-workgroup GPU spin lock.
        // Keep an explicit opt-out for regression testing.
        return value == nullptr || std::string_view{value} != "0";
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

static bool OrderDreamsSceneCompact() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_SCENE_COMPACT_ORDERED");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool OrderDreamsVisibilityList(u64 shader_hash) {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_VISIBILITY_LIST_ORDERED");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301" &&
           (shader_hash == Shader::DreamsCompat::VisibilityListCompactShader ||
            shader_hash == Shader::DreamsCompat::VisibilityListCompactShaderAlt);
}

static bool StabilizeDreamsGraphicsBuffers() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_GRAPHICS_BUFFER_STABILIZE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool StabilizeDreamsComputeIndirectBuffers() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_COMPUTE_INDIRECT_STABILIZE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool PrewarmDreamsD25BdaTransforms() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_D25_BDA_PREWARM");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool PrewarmDreamsVs370BdaTransforms() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_VS370_BDA_PREWARM");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool PrewarmDreamsDccBdaConstants() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_DCC_BDA_PREWARM");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static void ApplyDreamsGraphicsBufferBarrier(Scheduler& scheduler) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    scheduler.EndRendering();
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

static void ApplyDreamsComputeIndirectBufferBarrier(Scheduler& scheduler) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    scheduler.EndRendering();
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

static bool TraceDreamsGatherStages() {
    static const bool enabled =
        std::getenv("SHADPS4_DREAMS_GATHER_STAGE_TRACE") != nullptr;
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static void DispatchDreamsTraversalOrdered(vk::CommandBuffer cmdbuf, u32 dim_x, u32 dim_y,
                                           u32 dim_z, u32 forced_batch_size = 0) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    const vk::DependencyInfo dependency{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };

    bool first = true;
    const u32 batch_size =
        forced_batch_size != 0 ? forced_batch_size : DreamsTraversalBatchSize();
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

static std::array<RecentComputeDispatch, 256> g_recent_compute_dispatches{};
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

static bool TraceDreamsModelRecord() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_MODEL_RECORD_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool OverlapsDreamsModelConfigGds(const VAddr address, const u64 size) {
    constexpr VAddr ModelConfigGdsBegin = 704 * sizeof(u32);
    constexpr VAddr ModelConfigGdsEnd = 729 * sizeof(u32);
    return size != 0 && address < ModelConfigGdsEnd && ModelConfigGdsBegin < address + size;
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

static bool TraceDreamsVisibilityLists() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_VISIBILITY_LIST_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static u32 DreamsVisibilityListTraceLimit() {
    static const u32 limit = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_VISIBILITY_LIST_TRACE_LIMIT");
        if (value == nullptr || value[0] == '\0') {
            return 16U;
        }
        char* end{};
        const u64 parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<u32>(std::clamp<u64>(parsed, 1, 64)) : 16U;
    }();
    return limit;
}

static u32 g_dreams_visibility_list_trace_remaining{};
static u32 g_dreams_visibility_list_trace_ordinal{};
static bool g_dreams_visibility_list_trace_initialized{};

static bool ConsumeDreamsVisibilityListTraceRequest() {
    if (!TraceDreamsVisibilityLists()) {
        return false;
    }

    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_VISIBILITY_LIST_TRACE_TRIGGER_FILE");
    if (trigger_file != nullptr && trigger_file[0] != '\0') {
        std::error_code error;
        if (std::filesystem::remove(trigger_file, error)) {
            g_dreams_visibility_list_trace_remaining = DreamsVisibilityListTraceLimit();
            LOG_WARNING(Render_Vulkan, "Dreams visibility list trace armed for {} dispatches",
                        g_dreams_visibility_list_trace_remaining);
        }
    } else if (!g_dreams_visibility_list_trace_initialized) {
        g_dreams_visibility_list_trace_initialized = true;
        g_dreams_visibility_list_trace_remaining = DreamsVisibilityListTraceLimit();
        LOG_WARNING(Render_Vulkan,
                    "Dreams visibility list trace auto-armed for {} dispatches",
                    g_dreams_visibility_list_trace_remaining);
    }

    if (g_dreams_visibility_list_trace_remaining == 0) {
        return false;
    }
    --g_dreams_visibility_list_trace_remaining;
    return true;
}

static u64 HashDreamsTraceWords(std::span<const u32> words) {
    u64 hash = 1469598103934665603ULL;
    for (const u32 word : words) {
        hash ^= word;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct DreamsVisibilityListConsumerSnapshot {
    bool valid{};
    u32 ordinal{};
    u64 shader_hash{};
    u64 dispatch_sequence{};
    VAddr output_base{};
    u32 active_records{};
    u32 sampled_records{};
    u64 output_hash{};
    std::vector<u32> words;
};

static DreamsVisibilityListConsumerSnapshot g_dreams_visibility_list_consumer_snapshot{};

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

static void ApplyDreamsDccBdaPrewarm(const GraphicsPipeline* pipeline,
                                     Core::MemoryManager* memory,
                                     VideoCore::BufferCache& buffer_cache) {
    if (!PrewarmDreamsDccBdaConstants()) {
        return;
    }

    const Shader::Info* fragment_info{};
    for (const auto* shader : pipeline->GetStages()) {
        if (shader != nullptr && shader->l_stage == Shader::LogicalStage::Fragment) {
            fragment_info = shader;
            break;
        }
    }
    if (fragment_info == nullptr || fragment_info->pgm_hash != DreamsFleckFragmentShader) {
        return;
    }

    constexpr VAddr GuestAddressLimit = VAddr{1} << 48;
    constexpr u64 ConstantBlockSize = 16;
    const bool has_user_data = fragment_info->user_data.size() >= 5;
    VAddr root_base{};
    u32 selector{};
    u64 block_offset{};
    VAddr constant_address{};
    bool address_in_range{};
    bool mapped{};
    bool registered_before{};
    bool registered_after{};
    bool gpu_modified{};
    VideoCore::BufferId buffer_id{};
    std::array<u32, ConstantBlockSize / sizeof(u32)> words{};

    if (has_user_data) {
        root_base = (VAddr{fragment_info->user_data[0]} |
                     (VAddr{fragment_info->user_data[1]} << 32)) &
                    (GuestAddressLimit - 1);
        selector = fragment_info->user_data[4];
        block_offset = u64{selector} * ConstantBlockSize;
        address_in_range =
            root_base != 0 && block_offset <= GuestAddressLimit - root_base &&
            ConstantBlockSize <= GuestAddressLimit - (root_base + block_offset);
        if (address_in_range) {
            constant_address = root_base + block_offset;
            mapped = memory->IsValidMapping(constant_address, ConstantBlockSize);
        }
        if (mapped) {
            registered_before =
                buffer_cache.IsRegionRegistered(constant_address, ConstantBlockSize);
            gpu_modified = buffer_cache.IsRegionGpuModified(constant_address, ConstantBlockSize);
            // Dynamic ReadConst has no ordinary descriptor to register this guest page. Register
            // and synchronize it before BindResources builds the shader's BDA page table.
            buffer_id = buffer_cache.FindBuffer(constant_address, ConstantBlockSize);
            buffer_cache.SynchronizeBuffersInRange(constant_address, ConstantBlockSize);
            registered_after =
                buffer_cache.IsRegionRegistered(constant_address, ConstantBlockSize);
            std::memcpy(words.data(), std::bit_cast<const void*>(constant_address),
                        ConstantBlockSize);
        }
    }

    static u32 log_count{};
    if (log_count < 32) {
        LOG_WARNING(
            Render_Vulkan,
            "Dreams dcc BDA prewarm #{} root={:#x} selector={} address={:#x}+{:#x} "
            "userdata={} in_range={} mapped={} registered_before={} registered_after={} "
            "gpu_modified={} buffer_id={} words={:#010x},{:#010x},{:#010x},{:#010x}",
            ++log_count, root_base, selector, constant_address, ConstantBlockSize, has_user_data,
            address_in_range, mapped, registered_before, registered_after, gpu_modified,
            buffer_id.index, words[0], words[1], words[2], words[3]);
    }
}

static bool TraceDreamsFleckRendering() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_FLECK_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static bool TraceDreamsTemporalImages() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_TEMPORAL_IMAGE_TRACE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static u32 DreamsTemporalImageTraceLimit() {
    static const u32 limit = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_TEMPORAL_IMAGE_TRACE_LIMIT");
        if (value == nullptr || value[0] == '\0') {
            return 16U;
        }
        char* end{};
        const u64 parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<u32>(std::clamp<u64>(parsed, 2, 32)) : 16U;
    }();
    return limit;
}

static u32 g_dreams_temporal_image_trace_remaining{};
static u32 g_dreams_temporal_image_trace_ordinal{};
static bool g_dreams_temporal_image_trace_initialized{};

static bool ConsumeDreamsTemporalImageTraceRequest() {
    if (!TraceDreamsTemporalImages()) {
        return false;
    }

    const char* trigger_file =
        std::getenv("SHADPS4_DREAMS_TEMPORAL_IMAGE_TRACE_TRIGGER_FILE");
    if (trigger_file != nullptr && trigger_file[0] != '\0') {
        std::error_code error;
        if (std::filesystem::remove(trigger_file, error)) {
            g_dreams_temporal_image_trace_remaining = DreamsTemporalImageTraceLimit();
            LOG_WARNING(Render_Vulkan, "Dreams temporal image trace armed for {} dispatches",
                        g_dreams_temporal_image_trace_remaining);
        }
    } else if (!g_dreams_temporal_image_trace_initialized) {
        g_dreams_temporal_image_trace_initialized = true;
        g_dreams_temporal_image_trace_remaining = DreamsTemporalImageTraceLimit();
        LOG_WARNING(Render_Vulkan, "Dreams temporal image trace auto-armed for {} dispatches",
                    g_dreams_temporal_image_trace_remaining);
    }

    if (g_dreams_temporal_image_trace_remaining == 0) {
        return false;
    }
    --g_dreams_temporal_image_trace_remaining;
    return true;
}

struct DreamsTemporalContentImage {
    bool valid{};
    bool written{};
    u32 binding{};
    u32 array_size{};
    VideoCore::ImageId image_id{};
    u64 image_uid{};
    VAddr descriptor_base{};
    u32 descriptor_width{};
    u32 descriptor_height{};
    u32 descriptor_depth{};
    u32 descriptor_pitch{};
    VAddr guest_base{};
    u64 guest_size{};
    u32 cached_width{};
    u32 cached_height{};
    u32 cached_depth{};
    u32 cached_pitch{};
    u32 bits{};
    u32 image_format{};
    u32 tile_mode{};
    u32 view_format{};
    u32 base_level{};
    u32 levels{};
    u32 base_layer{};
    u32 layers{};
    u64 linear_size{};
    u32 flattened_index{};
};

struct DreamsTemporalContentChainKey {
    VAddr image_a{};
    VAddr image_b{};
    u32 width{};
    u32 height{};
    u32 pitch{};

    bool operator==(const DreamsTemporalContentChainKey&) const = default;
};

struct DreamsTemporalContentChain {
    DreamsTemporalContentChainKey key;
    u32 dispatches{};
};

struct DreamsTemporalContentCaptureState {
    bool active{};
    u32 remaining{};
    u32 ordinal{};
    std::filesystem::path output_dir;
    std::vector<DreamsTemporalContentChain> chains;
};

static DreamsTemporalContentCaptureState g_dreams_temporal_content_capture{};

static bool EnableDreamsTemporalContentCapture() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_TEMPORAL_CONTENT_CAPTURE");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled && Common::ElfInfo::Instance().GameSerial() == "CUSA04301";
}

static u32 DreamsTemporalContentCaptureLimit() {
    static const u32 limit = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_TEMPORAL_CONTENT_CAPTURE_LIMIT");
        if (value == nullptr || value[0] == '\0') {
            return 4U;
        }
        char* end{};
        const u64 parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<u32>(std::clamp<u64>(parsed, 4, 8)) : 4U;
    }();
    return limit;
}

static bool BeginDreamsTemporalContentCaptureDispatch() {
    auto& capture = g_dreams_temporal_content_capture;
    if (!EnableDreamsTemporalContentCapture()) {
        return false;
    }

    if (!capture.active) {
        const char* trigger_file =
            std::getenv("SHADPS4_DREAMS_TEMPORAL_CONTENT_CAPTURE_TRIGGER_FILE");
        const char* output_dir =
            std::getenv("SHADPS4_DREAMS_TEMPORAL_CONTENT_CAPTURE_DIR");
        if (trigger_file == nullptr || trigger_file[0] == '\0' || output_dir == nullptr ||
            output_dir[0] == '\0') {
            return false;
        }
        std::error_code error;
        if (!std::filesystem::remove(trigger_file, error)) {
            return false;
        }

        capture.output_dir = output_dir;
        std::filesystem::create_directories(capture.output_dir, error);
        if (error) {
            LOG_ERROR(Render_Vulkan,
                      "Failed to create Dreams temporal content capture directory {}: {}",
                      capture.output_dir.string(), error.message());
            return false;
        }

        std::ofstream dispatches{capture.output_dir / "dispatches.tsv", std::ios::trunc};
        std::ofstream images{capture.output_dir / "images.tsv", std::ios::trunc};
        if (!dispatches || !images) {
            LOG_ERROR(Render_Vulkan,
                      "Failed to initialize Dreams temporal content capture metadata in {}",
                      capture.output_dir.string());
            return false;
        }
        dispatches << "ordinal\tdispatch_sequence\tchain\tchain_dispatch\tchain_key_a\t"
                      "chain_key_b\tdim_x\tdim_y\tdim_z\tsrt28_flat44_width_max\t"
                      "srt29_flat45_height_max\tsrt30_flat46_uv_scale_x\t"
                      "srt31_flat47_uv_scale_y\tsrt32_flat48_history_weight\n";
        images << "ordinal\tdispatch_sequence\tchain\tphase\trole\tbinding\tarray_size\t"
                  "valid\twritten\timage_id\timage_uid\tdescriptor_base\t"
                  "descriptor_width\tdescriptor_height\tdescriptor_depth\t"
                  "descriptor_pitch\tguest_base\tguest_size\tcached_width\t"
                  "cached_height\tcached_depth\tcached_pitch\tbits\timage_format\t"
                  "tile_mode\tview_format\tbase_level\tlevels\tbase_layer\tlayers\t"
                  "linear_size\tfile\n";

        capture.active = true;
        capture.remaining = DreamsTemporalContentCaptureLimit();
        capture.ordinal = 0;
        capture.chains.clear();
        LOG_WARNING(Render_Vulkan,
                    "Dreams temporal content capture armed for {} dispatches in {}",
                    capture.remaining, capture.output_dir.string());
    }

    return capture.remaining != 0;
}

static std::pair<u32, u32> AssignDreamsTemporalContentChain(
    const std::array<DreamsTemporalContentImage, 4>& images) {
    const auto image_address = [](const DreamsTemporalContentImage& image) {
        return image.valid ? image.guest_base : image.descriptor_base;
    };
    const VAddr output = image_address(images[0]);
    const VAddr history = image_address(images[1]);
    const DreamsTemporalContentChainKey key{
        .image_a = std::min(output, history),
        .image_b = std::max(output, history),
        .width = images[0].descriptor_width,
        .height = images[0].descriptor_height,
        .pitch = images[0].descriptor_pitch,
    };
    auto& chains = g_dreams_temporal_content_capture.chains;
    auto chain = std::ranges::find(chains, key, &DreamsTemporalContentChain::key);
    if (chain == chains.end()) {
        chain = chains.insert(chains.end(), {.key = key});
    }
    return {static_cast<u32>(std::distance(chains.begin(), chain)) + 1, ++chain->dispatches};
}

static void WriteDreamsTemporalContentImageMetadata(
    u32 ordinal, u64 dispatch_sequence, u32 chain, std::string_view phase,
    std::string_view role, const DreamsTemporalContentImage& image,
    const std::filesystem::path& filename) {
    std::ofstream stream{g_dreams_temporal_content_capture.output_dir / "images.tsv",
                         std::ios::app};
    stream << ordinal << '\t' << dispatch_sequence << '\t' << chain << '\t' << phase << '\t'
           << role << '\t' << image.binding << '\t' << image.array_size << '\t' << image.valid
           << '\t' << image.written << '\t' << image.image_id.index << '\t' << image.image_uid
           << '\t' << image.descriptor_base << '\t' << image.descriptor_width << '\t'
           << image.descriptor_height << '\t' << image.descriptor_depth << '\t'
           << image.descriptor_pitch << '\t' << image.guest_base << '\t' << image.guest_size
           << '\t' << image.cached_width << '\t' << image.cached_height << '\t'
           << image.cached_depth << '\t' << image.cached_pitch << '\t' << image.bits << '\t'
           << image.image_format << '\t' << image.tile_mode << '\t' << image.view_format << '\t'
           << image.base_level << '\t' << image.levels << '\t' << image.base_layer << '\t'
           << image.layers << '\t' << image.linear_size << '\t' << filename.filename().string()
           << '\n';
}

static void FinishDreamsTemporalContentCaptureDispatch() {
    auto& capture = g_dreams_temporal_content_capture;
    if (capture.remaining != 0) {
        --capture.remaining;
    }
    if (capture.remaining == 0) {
        capture.active = false;
        LOG_WARNING(Render_Vulkan, "Dreams temporal content capture complete: {}",
                    capture.output_dir.string());
    }
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

static u32 DreamsVisibilityTargetDumpCount() {
    static const u32 count = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_VISIBILITY_TARGET_DUMP_COUNT");
        if (value == nullptr || value[0] == '\0') {
            return 1U;
        }
        char* end{};
        const u64 parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<u32>(std::clamp<u64>(parsed, 1, 64)) : 1U;
    }();
    return count;
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
           TraceDreamsVisibilityCounts() || TraceDreamsVisibilityLists() ||
           TraceDreamsModelRecord();
}

static bool TrackDreamsImageWriters() {
    return TraceDreamsProducerSources() || TraceDreamsFleckRendering() ||
           TraceDreamsTemporalImages();
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
    static const u64 start_sequence = [] {
        const char* value = std::getenv("SHADPS4_TRACE_COMPUTE_START");
        return value != nullptr ? std::strtoull(value, nullptr, 0) : u64{0};
    }();
    static u32 count{};
    if (enabled && g_compute_dispatch_sequence >= start_sequence && count++ < 4096) {
        LOG_WARNING(Render_Vulkan,
                    "Compute dispatch sequence={} hash={:#x} indirect={} dims={}x{}x{}",
                    g_compute_dispatch_sequence, info.pgm_hash, indirect, dim_x, dim_y, dim_z);
    }
}

static bool ShouldProfileCompute() {
    static const bool enabled = std::getenv("SHADPS4_PROFILE_COMPUTE") != nullptr;
    static u32 count{};
    return enabled && count++ < 512;
}

static u32 g_dreams_gpu_profile_remaining{};
static u32 g_dreams_gpu_profile_ordinal{};

static void ArmDreamsGpuProfile() {
    g_dreams_gpu_profile_remaining = 512;
    g_dreams_gpu_profile_ordinal = 0;
}

static bool ProfileDreamsGpuAfterGather() {
    static const bool enabled =
        std::getenv("SHADPS4_DREAMS_GPU_PROFILE_AFTER_GATHER") != nullptr;
    return enabled;
}

static u32 ConsumeDreamsGpuProfileEvent() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA04301") {
        return 0;
    }
    const char* trigger_file = std::getenv("SHADPS4_DREAMS_GPU_PROFILE_TRIGGER_FILE");
    if (trigger_file != nullptr && trigger_file[0] != '\0') {
        std::error_code error;
        if (std::filesystem::remove(trigger_file, error)) {
            ArmDreamsGpuProfile();
            LOG_WARNING(Render_Vulkan, "Dreams one-shot GPU profile started");
        }
    }
    if (g_dreams_gpu_profile_remaining == 0) {
        return 0;
    }
    --g_dreams_gpu_profile_remaining;
    return ++g_dreams_gpu_profile_ordinal;
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
    ApplyDreamsDccBdaPrewarm(pipeline, memory, buffer_cache);
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

    const bool stabilize_graphics_buffers = StabilizeDreamsGraphicsBuffers();
    if (stabilize_graphics_buffers) {
        // BeginRendering and the graphics-only ranges can grow cache buffers after shader
        // descriptors were first resolved. Settle those ranges without changing access state,
        // then refresh only the shader buffer descriptors against the final cache union.
        buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers, true);
        if (is_indexed) {
            buffer_cache.BindIndexBuffer(index_offset, buffer_barriers, true);
        }
        const u32 changed_handles = RefreshBufferResources(pipeline, true);
        static u32 dreams_graphics_stabilize_direct_log_count{};
        if ((changed_handles != 0 || dreams_graphics_stabilize_direct_log_count == 0) &&
            dreams_graphics_stabilize_direct_log_count++ < 64) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams graphics stabilization direct indexed={} changed_handles={}",
                        is_indexed, changed_handles);
        }
    }

    buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset, buffer_barriers);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    if (stabilize_graphics_buffers) {
        ApplyDreamsGraphicsBufferBarrier(scheduler);
    }
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

    if (PrewarmDreamsVs370BdaTransforms()) {
        const auto& vertex_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
        if (vertex_info.pgm_hash == DreamsSpriteGeometryVertexShaderAlt) {
            constexpr VAddr GuestAddressLimit = VAddr{1} << 48;
            constexpr u64 TransformStride = 192;
            constexpr u64 TransformBlockOffset = 128;
            constexpr u64 TransformBlockSize = 64;

            const bool has_constants =
                vertex_info.user_data.size() >= 3 && vertex_info.flattened_ud_buf.size() > 37;
            VAddr table_base{};
            u64 transform_index{};
            u64 transform_offset{};
            VAddr transform_address{};
            bool address_in_range{};
            bool mapped{};
            bool registered_before{};
            bool gpu_modified{};
            VideoCore::BufferId buffer_id{};
            if (has_constants) {
                // VS 3706083c obtains the table pointer from SRT dwords 20/21, flattened to
                // dwords 36/37, then reads four 16-byte columns at index * 192 + 128.
                table_base = (VAddr{vertex_info.flattened_ud_buf[36]} |
                              (VAddr{vertex_info.flattened_ud_buf[37]} << 32)) &
                             (GuestAddressLimit - 1);
                transform_index = vertex_info.user_data[2];
                transform_offset = transform_index * TransformStride + TransformBlockOffset;
                address_in_range =
                    table_base != 0 && transform_offset <= GuestAddressLimit - table_base &&
                    TransformBlockSize <= GuestAddressLimit - (table_base + transform_offset);
                if (address_in_range) {
                    transform_address = table_base + transform_offset;
                    mapped = memory->IsValidMapping(transform_address, TransformBlockSize);
                }
                if (mapped) {
                    registered_before =
                        buffer_cache.IsRegionRegistered(transform_address, TransformBlockSize);
                    gpu_modified =
                        buffer_cache.IsRegionGpuModified(transform_address, TransformBlockSize);
                    // Do this before Rasterizer::BindResources snapshots ordinary descriptors.
                    // FindBuffer also publishes the guest page in the BDA page table.
                    buffer_id =
                        buffer_cache.FindBuffer(transform_address, TransformBlockSize);
                    buffer_cache.SynchronizeBuffersInRange(transform_address, TransformBlockSize);
                }
            }

            static u32 dreams_vs370_bda_prewarm_log_count{};
            static VAddr dreams_vs370_bda_prewarm_last_address = ~VAddr{};
            if (dreams_vs370_bda_prewarm_log_count == 0 ||
                (dreams_vs370_bda_prewarm_log_count < 16 &&
                 transform_address != dreams_vs370_bda_prewarm_last_address)) {
                dreams_vs370_bda_prewarm_last_address = transform_address;
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams VS370 BDA prewarm #{} base={:#x} transform={} address={:#x}+{:#x} "
                    "constants={} in_range={} mapped={} registered_before={} gpu_modified={} "
                    "buffer_id={}",
                    ++dreams_vs370_bda_prewarm_log_count, table_base, transform_index,
                    transform_address, TransformBlockSize, has_constants, address_in_range, mapped,
                    registered_before, gpu_modified, buffer_id.index);
            }
        }
    }

    if (PrewarmDreamsD25BdaTransforms()) {
        const auto& vertex_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
        if (vertex_info.pgm_hash == DreamsSpriteGeometryVertexShader) {
            constexpr VAddr GuestAddressLimit = VAddr{1} << 48;
            constexpr u64 TransformStride = 192;
            constexpr u64 TransformBlockOffset = 128;
            constexpr u64 TransformBlockSize = 64;

            const bool has_user_data = vertex_info.user_data.size() >= 3;
            VAddr table_base{};
            u64 transform_index{};
            u64 transform_offset{};
            VAddr transform_address{};
            bool address_in_range{};
            bool mapped{};
            bool gpu_modified{};
            VideoCore::BufferId buffer_id{};
            if (has_user_data) {
                table_base = (VAddr{vertex_info.user_data[0]} |
                              (VAddr{vertex_info.user_data[1]} << 32)) &
                             (GuestAddressLimit - 1);
                transform_index = vertex_info.user_data[2];
                transform_offset = transform_index * TransformStride + TransformBlockOffset;
                address_in_range =
                    table_base != 0 && transform_offset <= GuestAddressLimit - table_base &&
                    TransformBlockSize <= GuestAddressLimit - (table_base + transform_offset);
                if (address_in_range) {
                    transform_address = table_base + transform_offset;
                    mapped = memory->IsValidMapping(transform_address, TransformBlockSize);
                }
                if (mapped) {
                    gpu_modified =
                        buffer_cache.IsRegionGpuModified(transform_address, TransformBlockSize);
                    // Registering the range populates the BDA page table before the draw. A direct
                    // synchronization is still required: dynamic ReadConst bypasses ordinary
                    // shader descriptors, so descriptor refresh alone cannot upload this page.
                    buffer_id =
                        buffer_cache.FindBuffer(transform_address, TransformBlockSize);
                    buffer_cache.SynchronizeBuffersInRange(transform_address, TransformBlockSize);
                }
            }

            static u32 dreams_d25_bda_prewarm_log_count{};
            static VAddr dreams_d25_bda_prewarm_last_address = ~VAddr{};
            if (dreams_d25_bda_prewarm_log_count == 0 ||
                (dreams_d25_bda_prewarm_log_count < 16 &&
                 transform_address != dreams_d25_bda_prewarm_last_address)) {
                dreams_d25_bda_prewarm_last_address = transform_address;
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams d25 BDA prewarm #{} base={:#x} transform={} address={:#x}+{:#x} "
                    "userdata={} in_range={} mapped={} gpu_modified={} buffer_id={}",
                    ++dreams_d25_bda_prewarm_log_count, table_base, transform_index,
                    transform_address, TransformBlockSize, has_user_data, address_in_range, mapped,
                    gpu_modified, buffer_id.index);
            }
        }
    }

    PrepareRenderState(pipeline);
    ApplyDreamsDccBdaPrewarm(pipeline, memory, buffer_cache);
    if (!BindResources(pipeline)) {
        return;
    }

    const auto& visibility_list_vertex = pipeline->GetStage(Shader::LogicalStage::Vertex);
    if (g_dreams_visibility_list_consumer_snapshot.valid &&
        visibility_list_vertex.pgm_hash == DreamsSpriteGeometryVertexShaderAlt &&
        visibility_list_vertex.buffers.size() > 4 &&
        !visibility_list_vertex.buffers[3].IsSpecial() &&
        !visibility_list_vertex.buffers[4].IsSpecial()) {
        const auto list = visibility_list_vertex.buffers[3].GetSharp(visibility_list_vertex);
        const auto lookup = visibility_list_vertex.buffers[4].GetSharp(visibility_list_vertex);
        if (list.base_address == g_dreams_visibility_list_consumer_snapshot.output_base) {
            scheduler.Finish();
            VkDrawIndexedIndirectCommand command{};
            const VAddr command_address = arg_address + offset;
            const bool command_mapped =
                is_indexed && memory->IsValidMapping(command_address, sizeof(command));
            if (command_mapped) {
                buffer_cache.ReadMemory(command_address, sizeof(command));
                std::memcpy(&command, std::bit_cast<const void*>(command_address),
                            sizeof(command));
            }

            constexpr u32 MaxSampleRecords = 1024;
            const u64 first_record = command.firstInstance;
            const u64 available_records = list.GetSize() / (2 * sizeof(u32));
            const u32 sampled_records = command_mapped && first_record < available_records
                                            ? static_cast<u32>(std::min<u64>(
                                                  {command.instanceCount, MaxSampleRecords,
                                                   available_records - first_record}))
                                            : 0;
            const VAddr list_address =
                list.base_address + first_record * 2 * sizeof(u32);
            std::vector<u32> consumed_words;
            const u64 consumed_size = static_cast<u64>(sampled_records) * 2 * sizeof(u32);
            const bool list_gpu_modified_before =
                consumed_size != 0 &&
                buffer_cache.IsRegionGpuModified(list_address, consumed_size);
            if (consumed_size != 0 && memory->IsValidMapping(list_address, consumed_size)) {
                buffer_cache.ReadMemory(list_address, consumed_size);
                consumed_words.resize(static_cast<u64>(sampled_records) * 2);
                std::memcpy(consumed_words.data(), std::bit_cast<const void*>(list_address),
                            consumed_size);
            }

            u32 invalid_lookup_ids{};
            u32 max_low24_id{};
            u32 changed_since_dispatch{};
            for (u32 record = 0; record < consumed_words.size() / 2; ++record) {
                const u32 low24_id = consumed_words[record * 2] & 0x00ffffff;
                max_low24_id = std::max(max_low24_id, low24_id);
                const u64 lookup_offset = static_cast<u64>(low24_id) * 4 * sizeof(u32);
                invalid_lookup_ids +=
                    lookup_offset > lookup.GetSize() ||
                    2 * sizeof(u32) > lookup.GetSize() - lookup_offset;

                const u64 snapshot_record = first_record + record;
                if (snapshot_record >=
                    g_dreams_visibility_list_consumer_snapshot.words.size() / 2) {
                    ++changed_since_dispatch;
                    continue;
                }
                changed_since_dispatch +=
                    consumed_words[record * 2] !=
                        g_dreams_visibility_list_consumer_snapshot.words[snapshot_record * 2] ||
                    consumed_words[record * 2 + 1] !=
                        g_dreams_visibility_list_consumer_snapshot.words[snapshot_record * 2 + 1];
            }
            const u64 consumed_hash = HashDreamsTraceWords(consumed_words);
            LOG_WARNING(
                Render_Vulkan,
                "Dreams visibility list consume #{} producer_sequence={} shader={:#x} "
                "command={:#x} mapped={} instances={} first_instance={} active={} "
                "sampled={} truncated={} list={:#x}+{:#x} stride={} lookup={:#x}+{:#x} "
                "stride={} hash={:#x} producer_hash={:#x} changed_since_dispatch={} "
                "invalid_lookup_ids={} max_low24_id={}",
                g_dreams_visibility_list_consumer_snapshot.ordinal,
                g_dreams_visibility_list_consumer_snapshot.dispatch_sequence,
                g_dreams_visibility_list_consumer_snapshot.shader_hash, command_address,
                command_mapped, command.instanceCount, command.firstInstance,
                g_dreams_visibility_list_consumer_snapshot.active_records, sampled_records,
                command.instanceCount > sampled_records, list.base_address, list.GetSize(),
                list.stride, lookup.base_address, lookup.GetSize(), lookup.stride, consumed_hash,
                g_dreams_visibility_list_consumer_snapshot.output_hash,
                changed_since_dispatch, invalid_lookup_ids, max_low24_id);
            const u32 records_to_log = std::min<u32>(consumed_words.size() / 2, 8);
            for (u32 record = 0; record < records_to_log; ++record) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams visibility list consumed #{}[{}] values={:#x},{:#x} "
                            "low24_id={} flags={}",
                            g_dreams_visibility_list_consumer_snapshot.ordinal,
                            static_cast<u32>(first_record) + record,
                            consumed_words[record * 2], consumed_words[record * 2 + 1],
                            consumed_words[record * 2] & 0x00ffffff,
                            (consumed_words[record * 2 + 1] >> 16) & 3);
            }

            if (sampled_records != 0) {
                struct DreamsVsResourceSample {
                    std::string_view label;
                    u32 binding{};
                    u32 words_per_record{};
                    VAddr descriptor_base{};
                    u64 descriptor_size{};
                    u32 descriptor_stride{};
                    VAddr sampled_base{};
                    u64 sampled_span{};
                    bool gpu_modified_before{};
                    u32 invalid_records{};
                    std::vector<u32> keys;
                    std::vector<u32> words;
                };
                struct DreamsVsResourceHistory {
                    bool valid{};
                    u32 ordinal{};
                    u32 words_per_record{};
                    std::vector<u32> keys;
                    std::vector<u32> words;
                };
                static std::array<DreamsVsResourceHistory, 8> resource_history{};

                std::vector<u32> low24_ids(sampled_records);
                std::vector<u32> low16_ids(sampled_records);
                std::vector<u32> list_indices(sampled_records);
                for (u32 record = 0; record < sampled_records; ++record) {
                    low24_ids[record] = consumed_words[record * 2] & 0x00ffffff;
                    low16_ids[record] = consumed_words[record * 2 + 1] & 0xffff;
                    list_indices[record] = static_cast<u32>(first_record) + record;
                }

                const auto sample_indexed_binding =
                    [&](u32 binding, std::string_view label, std::span<const u32> keys,
                        u32 record_size) {
                        DreamsVsResourceSample sample{
                            .label = label,
                            .binding = binding,
                            .words_per_record =
                                static_cast<u32>(record_size / sizeof(u32)),
                        };
                        sample.keys.assign(keys.begin(), keys.end());
                        if (binding >= visibility_list_vertex.buffers.size() ||
                            visibility_list_vertex.buffers[binding].IsSpecial()) {
                            sample.invalid_records = static_cast<u32>(keys.size());
                            return sample;
                        }

                        const auto sharp =
                            visibility_list_vertex.buffers[binding].GetSharp(visibility_list_vertex);
                        sample.descriptor_base = sharp.base_address;
                        sample.descriptor_size = sharp.GetSize();
                        sample.descriptor_stride = sharp.stride;
                        std::vector<u32> valid_keys;
                        valid_keys.reserve(keys.size());
                        u64 min_offset = std::numeric_limits<u64>::max();
                        u64 max_end{};
                        for (const u32 key : keys) {
                            const u64 offset = static_cast<u64>(key) * record_size;
                            const bool valid = sharp.base_address != 0 &&
                                               offset <= sharp.GetSize() &&
                                               record_size <= sharp.GetSize() - offset &&
                                               memory->IsValidMapping(sharp.base_address + offset,
                                                                      record_size);
                            if (!valid) {
                                ++sample.invalid_records;
                                continue;
                            }
                            valid_keys.push_back(key);
                            min_offset = std::min(min_offset, offset);
                            max_end = std::max(max_end, offset + record_size);
                        }
                        std::ranges::sort(valid_keys);
                        valid_keys.erase(std::unique(valid_keys.begin(), valid_keys.end()),
                                         valid_keys.end());

                        if (!valid_keys.empty()) {
                            sample.sampled_base = sharp.base_address + min_offset;
                            sample.sampled_span = max_end - min_offset;
                            constexpr u64 MaxContiguousRead = 512_KB;
                            if (sample.sampled_span <= MaxContiguousRead &&
                                memory->IsValidMapping(sample.sampled_base,
                                                       sample.sampled_span)) {
                                sample.gpu_modified_before = buffer_cache.IsRegionGpuModified(
                                    sample.sampled_base, sample.sampled_span);
                                buffer_cache.ReadMemory(sample.sampled_base, sample.sampled_span);
                            } else {
                                for (const u32 key : valid_keys) {
                                    const VAddr record_address =
                                        sharp.base_address + static_cast<u64>(key) * record_size;
                                    sample.gpu_modified_before |=
                                        buffer_cache.IsRegionGpuModified(record_address,
                                                                         record_size);
                                    buffer_cache.ReadMemory(record_address, record_size);
                                }
                            }
                        }

                        sample.words.reserve(static_cast<u64>(keys.size()) *
                                             sample.words_per_record);
                        for (const u32 key : keys) {
                            const u64 offset = static_cast<u64>(key) * record_size;
                            const bool valid = sharp.base_address != 0 &&
                                               offset <= sharp.GetSize() &&
                                               record_size <= sharp.GetSize() - offset &&
                                               memory->IsValidMapping(sharp.base_address + offset,
                                                                      record_size);
                            const size_t old_size = sample.words.size();
                            sample.words.resize(old_size + sample.words_per_record);
                            if (valid) {
                                std::memcpy(sample.words.data() + old_size,
                                            std::bit_cast<const void*>(sharp.base_address + offset),
                                            record_size);
                            }
                        }
                        return sample;
                    };

                std::array<DreamsVsResourceSample, 5> samples{
                    sample_indexed_binding(0, "object", low16_ids, 432),
                    sample_indexed_binding(1, "id-data-a", low24_ids, sizeof(u32)),
                    sample_indexed_binding(2, "id-data-b", low24_ids, sizeof(u32)),
                    DreamsVsResourceSample{
                        .label = "visibility-list",
                        .binding = 3,
                        .words_per_record = 2,
                        .descriptor_base = list.base_address,
                        .descriptor_size = list.GetSize(),
                        .descriptor_stride = static_cast<u32>(list.stride),
                        .sampled_base = list_address,
                        .sampled_span = consumed_size,
                        .gpu_modified_before = list_gpu_modified_before,
                        .keys = std::move(list_indices),
                        .words = consumed_words,
                    },
                    sample_indexed_binding(4, "id-record", low24_ids, 4 * sizeof(u32)),
                };

                const auto log_writer_provenance = [&](const DreamsVsResourceSample& sample) {
                    if (sample.sampled_base == 0 || sample.sampled_span == 0) {
                        return;
                    }
                    u32 matches{};
                    for (auto writer = g_dreams_buffer_writers.rbegin();
                         writer != g_dreams_buffer_writers.rend() && matches < 2; ++writer) {
                        if (!writer->declared_write ||
                            !OverlapsDreamsRange(sample.sampled_base, sample.sampled_span,
                                                writer->base, writer->size)) {
                            continue;
                        }
                        LOG_WARNING(
                            Render_Vulkan,
                            "Dreams visibility VS writer #{} resource={} match={} sequence={} "
                            "dispatch={} shader={:#x} stage={} binding={} range={:#x}+{:#x} "
                            "stride={}",
                            g_dreams_visibility_list_consumer_snapshot.ordinal, sample.label,
                            matches, writer->sequence, writer->dispatch_sequence,
                            writer->shader_hash, static_cast<u32>(writer->stage),
                            writer->binding_index, writer->base, writer->size, writer->stride);
                        ++matches;
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams visibility VS writer summary #{} resource={} "
                                "matches={} history={}",
                                g_dreams_visibility_list_consumer_snapshot.ordinal, sample.label,
                                matches, g_dreams_buffer_writers.size());
                };

                const auto log_resource = [&](u32 history_index,
                                              const DreamsVsResourceSample& sample) {
                    auto& previous = resource_history[history_index];
                    u32 changed_words{};
                    u32 first_changed_record = std::numeric_limits<u32>::max();
                    u32 first_changed_word = std::numeric_limits<u32>::max();
                    u32 before_value{};
                    u32 after_value{};
                    bool keys_changed{};
                    if (previous.valid) {
                        const size_t key_count = std::max(previous.keys.size(), sample.keys.size());
                        for (size_t record = 0; record < key_count; ++record) {
                            if (record < previous.keys.size() && record < sample.keys.size() &&
                                previous.keys[record] == sample.keys[record]) {
                                continue;
                            }
                            keys_changed = true;
                            first_changed_record = static_cast<u32>(record);
                            break;
                        }
                        const size_t word_count =
                            std::max(previous.words.size(), sample.words.size());
                        for (size_t word = 0; word < word_count; ++word) {
                            const u32 before =
                                word < previous.words.size() ? previous.words[word] : 0;
                            const u32 after = word < sample.words.size() ? sample.words[word] : 0;
                            if (before == after) {
                                continue;
                            }
                            ++changed_words;
                            if (first_changed_word != std::numeric_limits<u32>::max()) {
                                continue;
                            }
                            const u32 words_per_record =
                                std::max(sample.words_per_record, 1U);
                            first_changed_record = static_cast<u32>(word / words_per_record);
                            first_changed_word = static_cast<u32>(word % words_per_record);
                            before_value = before;
                            after_value = after;
                        }
                    }
                    const u64 key_hash = HashDreamsTraceWords(sample.keys);
                    const u64 data_hash = HashDreamsTraceWords(sample.words);
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams visibility VS resource #{} name={} binding={} base={:#x}+{:#x} "
                        "stride={} sampled={:#x}+{:#x} records={} words_per_record={} "
                        "gpu_before={} invalid={} key_hash={:#x} data_hash={:#x} "
                        "previous=#{} keys_changed={} changed_words={} "
                        "first_record={} first_word={} value={:#x}->{:#x}",
                        g_dreams_visibility_list_consumer_snapshot.ordinal, sample.label,
                        sample.binding, sample.descriptor_base, sample.descriptor_size,
                        sample.descriptor_stride, sample.sampled_base, sample.sampled_span,
                        sample.keys.size(), sample.words_per_record,
                        sample.gpu_modified_before, sample.invalid_records, key_hash, data_hash,
                        previous.valid ? previous.ordinal : 0, keys_changed, changed_words,
                        first_changed_record, first_changed_word, before_value, after_value);
                    if (!previous.valid || keys_changed || changed_words != 0) {
                        log_writer_provenance(sample);
                    }
                    previous = {
                        .valid = true,
                        .ordinal = g_dreams_visibility_list_consumer_snapshot.ordinal,
                        .words_per_record = sample.words_per_record,
                        .keys = sample.keys,
                        .words = sample.words,
                    };
                    return data_hash;
                };

                std::array<u64, 8> resource_hashes{};
                for (u32 index = 0; index < samples.size(); ++index) {
                    resource_hashes[index] = log_resource(index, samples[index]);
                }

                DreamsVsResourceSample constants{
                    .label = "userdata-flat",
                    .binding = std::numeric_limits<u32>::max(),
                    .words_per_record = 7,
                    .keys = {0},
                };
                constants.words.resize(7);
                for (u32 index = 0; index < 4; ++index) {
                    constants.words[index] = index < visibility_list_vertex.user_data.size()
                                                 ? visibility_list_vertex.user_data[index]
                                                 : 0;
                }
                for (u32 index = 0; index < 3; ++index) {
                    constants.words[4 + index] =
                        36 + index < visibility_list_vertex.flattened_ud_buf.size()
                            ? visibility_list_vertex.flattened_ud_buf[36 + index]
                            : 0;
                }
                resource_hashes[5] = log_resource(5, constants);

                constexpr VAddr GuestAddressLimit = VAddr{1} << 48;
                const VAddr srt_base =
                    (VAddr{constants.words[0]} | (VAddr{constants.words[1]} << 32)) &
                    (GuestAddressLimit - 1);
                DreamsVsResourceSample srt_entries{
                    .label = "srt-dwords20-22",
                    .binding = std::numeric_limits<u32>::max(),
                    .words_per_record = 3,
                    .descriptor_base = srt_base,
                    .descriptor_size = 23 * sizeof(u32),
                    .sampled_base = srt_base + 20 * sizeof(u32),
                    .sampled_span = 3 * sizeof(u32),
                    .keys = {20},
                };
                if (srt_base != 0 &&
                    memory->IsValidMapping(srt_entries.sampled_base,
                                           srt_entries.sampled_span)) {
                    srt_entries.gpu_modified_before = buffer_cache.IsRegionGpuModified(
                        srt_entries.sampled_base, srt_entries.sampled_span);
                    if (srt_entries.gpu_modified_before) {
                        buffer_cache.ReadMemory(srt_entries.sampled_base,
                                                srt_entries.sampled_span);
                    }
                    srt_entries.words.resize(3);
                    std::memcpy(srt_entries.words.data(),
                                std::bit_cast<const void*>(srt_entries.sampled_base),
                                srt_entries.sampled_span);
                } else {
                    srt_entries.invalid_records = 1;
                }
                resource_hashes[6] = log_resource(6, srt_entries);

                const VAddr transform_base =
                    (VAddr{constants.words[4]} | (VAddr{constants.words[5]} << 32)) &
                    (GuestAddressLimit - 1);
                const u64 transform_offset =
                    static_cast<u64>(constants.words[2]) * 192 + 128;
                DreamsVsResourceSample transform{
                    .label = "dynamic-transform",
                    .binding = std::numeric_limits<u32>::max(),
                    .words_per_record = 16,
                    .descriptor_base = transform_base,
                    .descriptor_size = transform_offset + 64,
                    .sampled_base = transform_base + transform_offset,
                    .sampled_span = 64,
                    .keys = {constants.words[2]},
                };
                const bool transform_in_range =
                    transform_base != 0 && transform_offset <= GuestAddressLimit - transform_base &&
                    64 <= GuestAddressLimit - (transform_base + transform_offset);
                if (transform_in_range &&
                    memory->IsValidMapping(transform.sampled_base, transform.sampled_span)) {
                    transform.gpu_modified_before = buffer_cache.IsRegionGpuModified(
                        transform.sampled_base, transform.sampled_span);
                    if (transform.gpu_modified_before) {
                        buffer_cache.ReadMemory(transform.sampled_base, transform.sampled_span);
                    }
                    transform.words.resize(16);
                    std::memcpy(transform.words.data(),
                                std::bit_cast<const void*>(transform.sampled_base),
                                transform.sampled_span);
                } else {
                    transform.invalid_records = 1;
                }
                resource_hashes[7] = log_resource(7, transform);

                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams visibility VS frame #{} producer_sequence={} instances={} "
                    "hashes b0-4={:#x},{:#x},{:#x},{:#x},{:#x} constants={:#x} "
                    "srt={:#x} bda={:#x} userdata={:#x},{:#x},{:#x},{:#x} "
                    "flat36-38={:#x},{:#x},{:#x} srt_match={}",
                    g_dreams_visibility_list_consumer_snapshot.ordinal,
                    g_dreams_visibility_list_consumer_snapshot.dispatch_sequence,
                    sampled_records, resource_hashes[0], resource_hashes[1],
                    resource_hashes[2], resource_hashes[3], resource_hashes[4],
                    resource_hashes[5], resource_hashes[6], resource_hashes[7],
                    constants.words[0], constants.words[1], constants.words[2],
                    constants.words[3], constants.words[4], constants.words[5],
                    constants.words[6], srt_entries.words.size() == 3 &&
                                              srt_entries.words[0] == constants.words[4] &&
                                              srt_entries.words[1] == constants.words[5] &&
                                              srt_entries.words[2] == constants.words[6]);
            }
            g_dreams_visibility_list_consumer_snapshot.valid = false;
            g_dreams_visibility_list_consumer_snapshot.words.clear();
        }
    }
    const auto state = BeginRendering(pipeline);

    const auto dreams_graphics_key =
        GetDreamsGraphicsPipelineTraceKey(true, is_indexed, pipeline, regs);
    const u64 dreams_fragment_hash =
        dreams_graphics_key.hashes[static_cast<u32>(Shader::LogicalStage::Fragment)];
    static std::optional<std::filesystem::path> dreams_visibility_dump_dir;
    static u32 dreams_visibility_dump_draw_count{};
    static u32 dreams_visibility_dump_frame_index{};
    static u32 dreams_visibility_dump_frames_remaining{};
    if (dreams_fragment_hash == 0x2b6d3647) {
        if (auto output_dir = ConsumeDreamsVisibilityTargetDumpRequest()) {
            dreams_visibility_dump_dir = std::move(output_dir);
            dreams_visibility_dump_frame_index = 0;
            dreams_visibility_dump_frames_remaining = DreamsVisibilityTargetDumpCount();
        }
        if (dreams_visibility_dump_dir) {
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

    const bool stabilize_graphics_buffers = StabilizeDreamsGraphicsBuffers();
    if (stabilize_graphics_buffers) {
        buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers, true);
        if (is_indexed) {
            buffer_cache.BindIndexBuffer(0, buffer_barriers, true);
        }
        const u32 indirect_size = stride * max_count;
        if (indirect_size != 0) {
            buffer_cache.FindBuffer(arg_address + offset, indirect_size);
        }
        if (count_address != 0) {
            buffer_cache.FindBuffer(count_address, sizeof(u32));
        }
        const u32 changed_handles = RefreshBufferResources(pipeline, true);
        static u32 dreams_graphics_stabilize_indirect_log_count{};
        if ((changed_handles != 0 || dreams_graphics_stabilize_indirect_log_count == 0) &&
            dreams_graphics_stabilize_indirect_log_count++ < 64) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams graphics stabilization indirect indexed={} changed_handles={}",
                        is_indexed, changed_handles);
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
    if (stabilize_graphics_buffers) {
        ApplyDreamsGraphicsBufferBarrier(scheduler);
    }
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
                fmt::format("frame{:02}-{}-{:#x}-{}x{}-pitch{}-bits{}.bin",
                            dreams_visibility_dump_frame_index, role, image.info.guest_address,
                            image.info.size.width, image.info.size.height, image.info.pitch,
                            image.info.num_bits);
            texture_cache.DumpImageLinear(image_id, file);
            LOG_WARNING(Render_Vulkan,
                        "Dreams visibility target dumped frame={} role={} id={} guest={:#x} "
                        "extent={}x{} pitch={} bits={} file={}",
                        dreams_visibility_dump_frame_index, role, image_id.index,
                        image.info.guest_address, image.info.size.width, image.info.size.height,
                        image.info.pitch, image.info.num_bits, file.string());
        };
        dump_target("color", cb_descs[0].first);
        dump_target("depth", db_desc.first);
        ++dreams_visibility_dump_frame_index;
        if (--dreams_visibility_dump_frames_remaining == 0) {
            dreams_visibility_dump_dir.reset();
        }
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
        LOG_WARNING(Render_Vulkan,
                    "Dreams GPU profile #{} kind=dispatch hash={:#x} pre-fence begin",
                    gpu_profile_ordinal, cs.pgm_hash);
        scheduler.Finish();
        LOG_WARNING(Render_Vulkan,
                    "Dreams GPU profile #{} kind=dispatch hash={:#x} pre-fence complete",
                    gpu_profile_ordinal, cs.pgm_hash);
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

    if (TraceDreamsModelRecord() && cs.pgm_hash == 0xac97d610) {
        static u32 collect_bricks_trace_count{};
        if (collect_bricks_trace_count++ < 8) {
            scheduler.Finish();
            LOG_WARNING(Render_Vulkan,
                        "Dreams collect-bricks input #{} sequence={} dims={}x{}x{} buffers={} "
                        "flat_size={}",
                        collect_bricks_trace_count, g_compute_dispatch_sequence,
                        cs_program.dim_x, cs_program.dim_y, cs_program.dim_z, cs.buffers.size(),
                        cs.flattened_ud_buf.size());
            for (u32 index = 0; index < cs.buffers.size(); ++index) {
                const auto& desc = cs.buffers[index];
                const auto sharp = desc.GetSharp(cs);
                LOG_WARNING(Render_Vulkan,
                            "Dreams collect-bricks buffer #{} index={} special={} write={} "
                            "type={} base={:#x}+{:#x} stride={} records={}",
                            collect_bricks_trace_count, index, desc.IsSpecial(), desc.is_written,
                            static_cast<u32>(desc.buffer_type), sharp.base_address,
                            sharp.GetSize(), sharp.stride, sharp.num_records);
                if (index != 1 || desc.IsSpecial() || sharp.base_address == 0) {
                    continue;
                }
                const u64 dispatched_words = u64{cs_program.dim_x} * 64;
                const u64 read_size =
                    std::min<u64>(sharp.GetSize(), dispatched_words * sizeof(u32)) & ~u64{3};
                if (read_size == 0 ||
                    !memory->IsValidMapping(sharp.base_address, read_size)) {
                    continue;
                }
                const bool gpu_modified =
                    buffer_cache.IsRegionGpuModified(sharp.base_address, read_size);
                buffer_cache.ReadMemory(sharp.base_address, read_size);
                const auto* words = std::bit_cast<const u32*>(sharp.base_address);
                const u64 word_count = read_size / sizeof(u32);
                u64 nonzero{};
                u64 below_sentinel{};
                u64 direct_candidates{};
                u32 minimum = std::numeric_limits<u32>::max();
                u32 maximum{};
                for (u64 word_index = 0; word_index < word_count; ++word_index) {
                    const u32 word = words[word_index];
                    nonzero += word != 0;
                    below_sentinel += word < 0x0fffffffu;
                    direct_candidates += word < 0x0fffffffu && (word & 1u) != 0;
                    minimum = std::min(minimum, word);
                    maximum = std::max(maximum, word);
                }
                std::array<u32, 8> head{};
                std::memcpy(head.data(), words,
                            std::min<u64>(sizeof(head), read_size));
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams collect-bricks candidates #{} words={} gpu_modified={} nonzero={} "
                    "below_sentinel={} direct_bit0={} min={:#x} max={:#x} "
                    "head={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    collect_bricks_trace_count, word_count, gpu_modified, nonzero,
                    below_sentinel, direct_candidates, minimum, maximum, head[0], head[1],
                    head[2], head[3], head[4], head[5], head[6], head[7]);
            }
        }
    }

    static const u32 gather_post_diagnostic_limit = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_GATHER_POST_DIAG");
        if (value == nullptr) {
            return 0u;
        }
        if (value[0] == '\0') {
            return 8u;
        }
        char* end{};
        const u64 parsed = std::strtoull(value, &end, 0);
        return end != value ? static_cast<u32>(std::clamp<u64>(parsed, 1, 64)) : 8u;
    }();
    static u32 gather_post_diagnostic_count{};
    const bool gather_post_diagnostic =
        Common::ElfInfo::Instance().GameSerial() == "CUSA04301" &&
        cs.pgm_hash == Shader::DreamsCompat::GatherVoxelsShader &&
        !TraceDreamsGatherStages() &&
        gather_post_diagnostic_count < gather_post_diagnostic_limit;
    const u32 gather_post_diagnostic_ordinal =
        gather_post_diagnostic ? ++gather_post_diagnostic_count : 0;
    std::array<bool, 9> gather_post_buffer_ready{};
    std::array<bool, 9> gather_post_buffer_gpu_modified{};

    if ((TraceDreamsModelRecord() || TraceDreamsGatherStages()) &&
        cs.pgm_hash == 0x7ba4de5d &&
        std::getenv("SHADPS4_DREAMS_GATHER_INPUT_READBACK") != nullptr) {
        static u32 gather_input_trace_count{};
        if (gather_input_trace_count++ < 8) {
            scheduler.Finish();
            for (const u32 index : {7u, 8u, 12u, 13u, 14u}) {
                if (index >= cs.buffers.size() || cs.buffers[index].IsSpecial()) {
                    continue;
                }
                const auto sharp = cs.buffers[index].GetSharp(cs);
                const u64 read_size =
                    std::min<u64>(sharp.GetSize(), 16 * 1024 * 1024) & ~u64{3};
                if (sharp.base_address == 0 || read_size == 0 ||
                    !memory->IsValidMapping(sharp.base_address, read_size)) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams GatherVoxels input #{} unavailable base={:#x}+{:#x}",
                                index, sharp.base_address, sharp.GetSize());
                    continue;
                }
                const bool gpu_modified =
                    buffer_cache.IsRegionGpuModified(sharp.base_address, read_size);
                const bool head_gpu_modified =
                    buffer_cache.IsRegionGpuModified(sharp.base_address, sizeof(u32));
                const char* reupload_value =
                    std::getenv("SHADPS4_DREAMS_GATHER_INPUT_REUPLOAD");
                const bool force_reupload = index == 14 && reupload_value != nullptr &&
                                            std::string_view{reupload_value} == "1";
                // Opt-in A/B test: after synchronization, mark only candidate buffer 14 as
                // CPU-modified so ObtainBuffer must upload its CPU backing again.
                buffer_cache.ReadMemory(sharp.base_address, read_size, force_reupload);
                const bool cpu_modified_after_read =
                    buffer_cache.IsRegionCpuModified(sharp.base_address, read_size);
                const auto* words = std::bit_cast<const u32*>(sharp.base_address);
                const u64 word_count = read_size / sizeof(u32);
                u64 nonzero{};
                u64 over_1024{};
                u64 over_million{};
                u32 maximum{};
                u64 first_nonzero = std::numeric_limits<u64>::max();
                for (u64 word = 0; word < word_count; ++word) {
                    const u32 value = words[word];
                    nonzero += value != 0;
                    over_1024 += value > 1024;
                    over_million += value > 1'000'000;
                    maximum = std::max(maximum, value);
                    if (value != 0 && first_nonzero == std::numeric_limits<u64>::max()) {
                        first_nonzero = word;
                    }
                }
                std::array<u32, 8> head{};
                std::memcpy(head.data(), words, std::min<u64>(sizeof(head), read_size));
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams GatherVoxels input trace={} #{} base={:#x}+{:#x} stride={} "
                    "records={} gpu_modified={} head_gpu_modified={} force_reupload={} "
                    "cpu_modified_after_read={} "
                    "scanned={} nonzero={} "
                    "gt1024={} gt1m={} "
                    "max={:#x} head={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    gather_input_trace_count, index, sharp.base_address, sharp.GetSize(),
                    sharp.stride, sharp.num_records, gpu_modified, head_gpu_modified,
                    force_reupload, cpu_modified_after_read, word_count, nonzero, over_1024,
                    over_million, maximum,
                    head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
                if (index == 13 && first_nonzero != std::numeric_limits<u64>::max() &&
                    cs.buffers.size() > 12 && !cs.buffers[12].IsSpecial()) {
                    const auto pairs = cs.buffers[12].GetSharp(cs);
                    const u64 pair_stride = pairs.stride != 0 ? pairs.stride : 2 * sizeof(u32);
                    const u64 pair_offset = first_nonzero * pair_stride;
                    std::array<u32, 2> pair{};
                    const bool pair_mapped = pairs.base_address != 0 &&
                                             pair_offset + sizeof(pair) <= pairs.GetSize() &&
                                             memory->IsValidMapping(pairs.base_address + pair_offset,
                                                                    sizeof(pair));
                    if (pair_mapped) {
                        std::memcpy(pair.data(),
                                    std::bit_cast<const void*>(pairs.base_address + pair_offset),
                                    sizeof(pair));
                    }
                    LOG_WARNING(Render_Vulkan,
                                "Dreams GatherVoxels first hash bucket={} bound={:#x} "
                                "pair_mapped={} pair_key={:#x} pair_payload={:#x}",
                                first_nonzero, words[first_nonzero], pair_mapped, pair[0], pair[1]);
                }
            }
        }
    }

    struct DreamsModelConfigGdsTrace {
        bool enabled{};
        u32 ordinal{};
        u32 inner_counter_before{};
        u32 inner_required_before{};
        std::array<u32, 7> prefix_a_before{};
        std::array<u32, 18> prefix_b_before{};
        std::array<u32, 25> before{};
    } dreams_model_config_gds_trace{};
    if (TraceDreamsModelRecord() &&
        std::ranges::any_of(cs.buffers, [](const auto& desc) {
            return desc.buffer_type == Shader::BufferType::GdsBuffer;
        })) {
        static u32 model_config_gds_ordinal{};
        if (model_config_gds_ordinal < 4096) {
            scheduler.Finish();
            const auto* gds = buffer_cache.GetGdsBuffer();
            constexpr u64 ModelConfigGdsOffset = 704 * sizeof(u32);
            if (gds->mapped_data.size() >=
                ModelConfigGdsOffset + sizeof(dreams_model_config_gds_trace.before)) {
                dreams_model_config_gds_trace.enabled = true;
                dreams_model_config_gds_trace.ordinal = ++model_config_gds_ordinal;
                std::memcpy(&dreams_model_config_gds_trace.inner_counter_before,
                            gds->mapped_data.data() + 64 * sizeof(u32), sizeof(u32));
                std::memcpy(&dreams_model_config_gds_trace.inner_required_before,
                            gds->mapped_data.data() + 65 * sizeof(u32), sizeof(u32));
                std::memcpy(dreams_model_config_gds_trace.prefix_a_before.data(),
                            gds->mapped_data.data() + 196 * sizeof(u32),
                            sizeof(dreams_model_config_gds_trace.prefix_a_before));
                std::memcpy(dreams_model_config_gds_trace.prefix_b_before.data(),
                            gds->mapped_data.data() + 389 * sizeof(u32),
                            sizeof(dreams_model_config_gds_trace.prefix_b_before));
                std::memcpy(dreams_model_config_gds_trace.before.data(),
                            gds->mapped_data.data() + ModelConfigGdsOffset,
                            sizeof(dreams_model_config_gds_trace.before));
            }
        }
    }

    struct DreamsModelGdsTrace {
        bool enabled{};
        u32 ordinal{};
        std::array<u32, 17> before{};
    } dreams_model_gds_trace{};
    if (TraceDreamsModelRecord()) {
        u32* ordinal_counter = nullptr;
        static u32 model_accumulate_ordinal{};
        static u32 model_finalize_ordinal{};
        static u32 model_transfer_ordinal{};
        if (cs.pgm_hash == 0x5650b8f7) {
            ordinal_counter = &model_accumulate_ordinal;
        } else if (cs.pgm_hash == 0xd049fb84) {
            ordinal_counter = &model_finalize_ordinal;
        } else if (cs.pgm_hash == 0xcca80e03) {
            ordinal_counter = &model_transfer_ordinal;
        }
        if (ordinal_counter != nullptr && *ordinal_counter < 256) {
            scheduler.Finish();
            const auto* gds = buffer_cache.GetGdsBuffer();
            constexpr u64 ModelGdsOffset = 768 * sizeof(u32);
            if (gds->mapped_data.size() <
                ModelGdsOffset + sizeof(dreams_model_gds_trace.before)) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams model GDS trace skipped: mapped GDS is only {:#x} bytes",
                            gds->mapped_data.size());
            } else {
                dreams_model_gds_trace.enabled = true;
                dreams_model_gds_trace.ordinal = ++*ordinal_counter;
                std::memcpy(dreams_model_gds_trace.before.data(),
                            gds->mapped_data.data() + ModelGdsOffset,
                            sizeof(dreams_model_gds_trace.before));
                const auto& values = dreams_model_gds_trace.before;
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams model GDS pre shader={:#x} ordinal={} sequence={} dims={}x{}x{} "
                    "GDS768-784={},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
                    cs.pgm_hash, dreams_model_gds_trace.ordinal, g_compute_dispatch_sequence,
                    cs_program.dim_x, cs_program.dim_y, cs_program.dim_z, values[0], values[1],
                    values[2], values[3], values[4], values[5], values[6], values[7], values[8],
                    values[9], values[10], values[11], values[12], values[13], values[14],
                    values[15], values[16]);

                if (cs.pgm_hash == 0x5650b8f7 && dreams_model_gds_trace.ordinal <= 4) {
                    LOG_WARNING(
                        Render_Vulkan,
                        "Dreams model accumulate resources ordinal={} buffers={} flat_size={}",
                        dreams_model_gds_trace.ordinal, cs.buffers.size(),
                        cs.flattened_ud_buf.size());
                    for (u32 index = 0; index < cs.buffers.size(); ++index) {
                        const auto& desc = cs.buffers[index];
                        const auto sharp = desc.GetSharp(cs);
                        const u64 probe_size =
                            !desc.IsSpecial() && !desc.is_written
                                ? std::min<u64>(sharp.GetSize(), 8 * sizeof(u32))
                                : 0;
                        const bool mapped = sharp.base_address != 0 && probe_size != 0 &&
                                            memory->IsValidMapping(sharp.base_address, probe_size);
                        const bool cpu_modified = mapped && buffer_cache.IsRegionCpuModified(
                                                               sharp.base_address, probe_size);
                        const bool gpu_modified = mapped && buffer_cache.IsRegionGpuModified(
                                                               sharp.base_address, probe_size);
                        std::array<u32, 8> head{};
                        if (mapped) {
                            buffer_cache.ReadMemory(sharp.base_address, probe_size);
                            std::memcpy(head.data(),
                                        std::bit_cast<const void*>(sharp.base_address), probe_size);
                        }
                        LOG_WARNING(
                            Render_Vulkan,
                            "Dreams model accumulate buffer ordinal={} index={} special={} "
                            "write={} type={} base={:#x}+{:#x} stride={} records={} "
                            "cpu_modified={} gpu_modified={} head={:#x},{:#x},{:#x},{:#x},"
                            "{:#x},{:#x},{:#x},{:#x}",
                            dreams_model_gds_trace.ordinal, index, desc.IsSpecial(),
                            desc.is_written, static_cast<u32>(desc.buffer_type), sharp.base_address,
                            sharp.GetSize(), sharp.stride, sharp.num_records, cpu_modified,
                            gpu_modified, head[0], head[1], head[2], head[3], head[4], head[5],
                            head[6], head[7]);
                    }
                }
                const bool trace_model_finalize_inputs =
                    cs.pgm_hash == 0xd049fb84 &&
                    (dreams_model_gds_trace.ordinal == 4 ||
                     dreams_model_gds_trace.ordinal == 5 ||
                     dreams_model_gds_trace.ordinal == 90 ||
                     (dreams_model_gds_trace.ordinal >= 94 &&
                      dreams_model_gds_trace.ordinal <= 105));
                if (trace_model_finalize_inputs && cs.buffers.size() > 8) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams model finalize inputs ordinal={} buffers={} flat_size={}",
                                dreams_model_gds_trace.ordinal, cs.buffers.size(),
                                cs.flattened_ud_buf.size());
                    for (u32 index = 0; index < cs.buffers.size(); ++index) {
                        const auto& desc = cs.buffers[index];
                        const auto sharp = desc.GetSharp(cs);
                        LOG_WARNING(Render_Vulkan,
                                    "Dreams model finalize buffer ordinal={} index={} special={} "
                                    "write={} type={} base={:#x}+{:#x} stride={} records={} "
                                    "cpu_modified={} gpu_modified={}",
                                    dreams_model_gds_trace.ordinal, index, desc.IsSpecial(),
                                    desc.is_written, static_cast<u32>(desc.buffer_type),
                                    sharp.base_address, sharp.GetSize(), sharp.stride,
                                    sharp.num_records,
                                    sharp.base_address != 0 && sharp.GetSize() != 0 &&
                                        buffer_cache.IsRegionCpuModified(sharp.base_address,
                                                                         sharp.GetSize()),
                                    sharp.base_address != 0 && sharp.GetSize() != 0 &&
                                        buffer_cache.IsRegionGpuModified(sharp.base_address,
                                                                         sharp.GetSize()));
                    }

                    const auto work = cs.buffers[6].GetSharp(cs);
                    const auto source = cs.buffers[7].GetSharp(cs);
                    const u32 work_group_base = cs.user_data.size() > 4 ? cs.user_data[4] : 0;
                    const u32 allocation_mode = cs.user_data.size() > 5 ? cs.user_data[5] : 0;
                    const u64 work_offset = u64{work_group_base} * 6 * sizeof(u32);
                    const u64 work_bytes =
                        work_offset <= work.GetSize()
                            ? std::min<u64>(work.GetSize() - work_offset,
                                            u64{cs_program.dim_x} * 6 * sizeof(u32))
                            : 0;
                    const VAddr work_address = work.base_address + work_offset;
                    std::vector<u32> source_indices;
                    if (!cs.buffers[6].IsSpecial() && work.base_address != 0 &&
                        work_bytes >= 6 * sizeof(u32) &&
                        memory->IsValidMapping(work_address, work_bytes)) {
                        const bool work_gpu_modified =
                            buffer_cache.IsRegionGpuModified(work_address, work_bytes);
                        buffer_cache.ReadMemory(work_address, work_bytes);
                        const auto* words = std::bit_cast<const u32*>(work_address);
                        const u32 work_records = static_cast<u32>(work_bytes / (6 * sizeof(u32)));
                        for (u32 record = 0; record < work_records; ++record) {
                            const u32* values = words + record * 6;
                            if (std::ranges::find(source_indices, values[1]) ==
                                source_indices.end()) {
                                source_indices.push_back(values[1]);
                            }
                            if (record < 8 || (values[1] >= 5200 && values[1] < 5300)) {
                                LOG_WARNING(
                                    Render_Vulkan,
                                    "Dreams model finalize work ordinal={} record={} "
                                    "values={:#x},{},{:#x},{:#x},{:#x},{:#x}",
                                    dreams_model_gds_trace.ordinal, work_group_base + record,
                                    values[0], values[1], values[2], values[3], values[4],
                                    values[5]);
                            }
                        }
                        LOG_WARNING(Render_Vulkan,
                                    "Dreams model finalize work summary ordinal={} records={} "
                                    "group_base={} allocation_mode={} unique_sources={} "
                                    "gpu_modified_before={}",
                                    dreams_model_gds_trace.ordinal, work_records,
                                    work_group_base, allocation_mode, source_indices.size(),
                                    work_gpu_modified);
                    }

                    if (!cs.buffers[7].IsSpecial() && source.base_address != 0) {
                        for (const u32 source_index : source_indices) {
                            const u64 record_offset = u64{source_index} * sizeof(u32);
                            const u64 record_size = 24 * sizeof(u32);
                            if (record_offset > source.GetSize() ||
                                record_size > source.GetSize() - record_offset) {
                                LOG_WARNING(Render_Vulkan,
                                            "Dreams model finalize source ordinal={} index={} "
                                            "out_of_range offset={:#x} size={:#x}",
                                            dreams_model_gds_trace.ordinal, source_index,
                                            record_offset, source.GetSize());
                                continue;
                            }
                            const VAddr record_address = source.base_address + record_offset;
                            if (!memory->IsValidMapping(record_address, record_size)) {
                                continue;
                            }
                            const bool cpu_modified =
                                buffer_cache.IsRegionCpuModified(record_address, record_size);
                            const bool gpu_modified =
                                buffer_cache.IsRegionGpuModified(record_address, record_size);
                            buffer_cache.ReadMemory(record_address, record_size);
                            std::array<u32, 24> record{};
                            std::memcpy(record.data(), std::bit_cast<const void*>(record_address),
                                        record_size);
                            u32 nonzero{};
                            for (u32 word = 0; word < record_size / sizeof(u32); ++word) {
                                nonzero += record[word] != 0;
                            }
                            LOG_WARNING(
                                Render_Vulkan,
                                "Dreams model finalize source ordinal={} index={} address={:#x} "
                                "cpu_modified={} gpu_modified={} nonzero={} "
                                "head={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                                dreams_model_gds_trace.ordinal, source_index, record_address,
                                cpu_modified, gpu_modified, nonzero, record[0], record[1],
                                record[2], record[3], record[4], record[5], record[6], record[7]);

                            if (record[0] != std::numeric_limits<u32>::max() &&
                                !cs.buffers[3].IsSpecial()) {
                                const auto nodes = cs.buffers[3].GetSharp(cs);
                                const u32 node_index = record[0] & 0x00FFFFFF;
                                constexpr u64 NodeSize = 24 * sizeof(u32);
                                const u64 node_offset = u64{node_index} * NodeSize;
                                if (nodes.base_address != 0 && node_offset <= nodes.GetSize() &&
                                    NodeSize <= nodes.GetSize() - node_offset) {
                                    const VAddr node_address = nodes.base_address + node_offset;
                                    if (memory->IsValidMapping(node_address, NodeSize)) {
                                        const bool node_cpu_modified =
                                            buffer_cache.IsRegionCpuModified(node_address,
                                                                             NodeSize);
                                        const bool node_gpu_modified =
                                            buffer_cache.IsRegionGpuModified(node_address,
                                                                             NodeSize);
                                        buffer_cache.ReadMemory(node_address, NodeSize);
                                        std::array<u32, 24> node{};
                                        std::memcpy(node.data(),
                                                    std::bit_cast<const void*>(node_address),
                                                    NodeSize);
                                        u32 node_nonzero{};
                                        for (const u32 word : node) {
                                            node_nonzero += word != 0;
                                        }
                                        LOG_WARNING(
                                            Render_Vulkan,
                                            "Dreams model finalize node ordinal={} source_index={} "
                                            "packed={:#x} node={} address={:#x} cpu_modified={} "
                                            "gpu_modified={} nonzero={} head={:#x},{:#x},{:#x},"
                                            "{:#x},{:#x},{:#x},{:#x},{:#x}",
                                            dreams_model_gds_trace.ordinal, source_index,
                                            record[0], node_index, node_address,
                                            node_cpu_modified, node_gpu_modified, node_nonzero,
                                            node[0], node[1], node[2], node[3], node[4], node[5],
                                            node[6], node[7]);

                                        u32 node_matches{};
                                        const VAddr node_end = node_address + NodeSize;
                                        for (auto writer = g_dreams_buffer_writers.rbegin();
                                             writer != g_dreams_buffer_writers.rend() &&
                                             node_matches < 24;
                                             ++writer) {
                                            const VAddr writer_end = writer->base + writer->size;
                                            if (!writer->declared_write ||
                                                writer->base >= node_end ||
                                                node_address >= writer_end ||
                                                writer->dispatch_sequence >=
                                                    g_compute_dispatch_sequence) {
                                                continue;
                                            }
                                            LOG_WARNING(
                                                Render_Vulkan,
                                                "Dreams model finalize node writer ordinal={} "
                                                "node={} match={} writer_seq={} dispatch={} kind={} "
                                                "shader={:#x} binding={} range={:#x}+{:#x} "
                                                "stride={} source={:#x}",
                                                dreams_model_gds_trace.ordinal, node_index,
                                                node_matches, writer->sequence,
                                                writer->dispatch_sequence,
                                                static_cast<u32>(writer->kind), writer->shader_hash,
                                                writer->binding_index, writer->base, writer->size,
                                                writer->stride, writer->source);
                                            ++node_matches;
                                        }
                                        LOG_WARNING(
                                            Render_Vulkan,
                                            "Dreams model finalize node writers ordinal={} node={} "
                                            "matches={} history={}",
                                            dreams_model_gds_trace.ordinal, node_index,
                                            node_matches, g_dreams_buffer_writers.size());
                                    }
                                }
                            }

                            u32 matches{};
                            const VAddr record_end = record_address + record_size;
                            for (auto writer = g_dreams_buffer_writers.rbegin();
                                 writer != g_dreams_buffer_writers.rend() && matches < 24;
                                 ++writer) {
                                const VAddr writer_end = writer->base + writer->size;
                                if (!writer->declared_write || writer->base >= record_end ||
                                    record_address >= writer_end ||
                                    writer->dispatch_sequence >= g_compute_dispatch_sequence) {
                                    continue;
                                }
                                LOG_WARNING(
                                    Render_Vulkan,
                                    "Dreams model finalize source writer ordinal={} index={} "
                                    "match={} writer_seq={} dispatch={} kind={} shader={:#x} "
                                    "binding={} range={:#x}+{:#x} stride={} source={:#x}",
                                    dreams_model_gds_trace.ordinal, source_index, matches,
                                    writer->sequence, writer->dispatch_sequence,
                                    static_cast<u32>(writer->kind), writer->shader_hash,
                                    writer->binding_index, writer->base, writer->size,
                                    writer->stride, writer->source);
                                ++matches;
                            }
                            LOG_WARNING(Render_Vulkan,
                                        "Dreams model finalize source writers ordinal={} "
                                        "index={} matches={} history={}",
                                        dreams_model_gds_trace.ordinal, source_index, matches,
                                        g_dreams_buffer_writers.size());
                        }
                    }
                }
            }
        }
    }

    static u32 dreams_model_producer_cycle{};
    static u32 dreams_model_compact_ordinal{};
    static u32 dreams_model_post_ordinal{};
    static u64 dreams_model_compact_sequence{};
    static u64 dreams_model_post_sequence{};
    struct DreamsModelProducerTrace {
        bool enabled{};
        bool compact{};
        u32 ordinal{};
        u32 cycle{};
        VAddr output_base{};
        u64 output_size{};
    } dreams_model_producer_trace{};
    if (TraceDreamsModelRecord() &&
        (cs.pgm_hash == Shader::DreamsCompat::SceneCompactShader ||
         cs.pgm_hash == Shader::DreamsCompat::ModelBucketPostShader)) {
        dreams_model_producer_trace.compact =
            cs.pgm_hash == Shader::DreamsCompat::SceneCompactShader;
        if (dreams_model_producer_trace.compact) {
            dreams_model_producer_trace.ordinal = ++dreams_model_compact_ordinal;
            dreams_model_producer_trace.cycle = ++dreams_model_producer_cycle;
            dreams_model_compact_sequence = g_compute_dispatch_sequence;
        } else {
            dreams_model_producer_trace.ordinal = ++dreams_model_post_ordinal;
            dreams_model_producer_trace.cycle = dreams_model_producer_cycle;
            dreams_model_post_sequence = g_compute_dispatch_sequence;
        }
        const u32 output_index = dreams_model_producer_trace.compact ? 0 : 1;
        if (output_index < cs.buffers.size() && !cs.buffers[output_index].IsSpecial()) {
            const auto output = cs.buffers[output_index].GetSharp(cs);
            dreams_model_producer_trace.output_base = output.base_address;
            dreams_model_producer_trace.output_size = output.GetSize();
        }
        if (dreams_model_producer_trace.ordinal <= 256) {
            scheduler.Finish();
            auto* gds = buffer_cache.GetGdsBuffer();
            const u64 counter_offset =
                static_cast<u64>(Shader::DreamsCompat::ModelProducerTraceBaseDword) * sizeof(u32);
            const u64 counter_size =
                Shader::DreamsCompat::ModelProducerTraceCount * sizeof(u32);
            const u64 numeric_offset =
                static_cast<u64>(Shader::DreamsCompat::ModelProducerNumericBaseDword) *
                sizeof(u32);
            const u64 numeric_size =
                Shader::DreamsCompat::ModelProducerNumericCount * sizeof(u32);
            if (counter_offset + counter_size <= gds->SizeBytes() &&
                numeric_offset + numeric_size <= gds->SizeBytes()) {
                dreams_model_producer_trace.enabled = true;
                gds->Fill(counter_offset, counter_size, 0);
                gds->Fill(numeric_offset, numeric_size, 0);
                for (const u32 minimum_slot : {9u, 22u, 24u}) {
                    gds->Fill(counter_offset + minimum_slot * sizeof(u32), sizeof(u32),
                              0xffffffff);
                }
            } else {
                LOG_ERROR(Render_Vulkan,
                          "Dreams model producer trace range exceeds GDS size {:#x}",
                          gds->SizeBytes());
            }
        }
    }

    bool model_finalize_gate_trace =
        dreams_model_gds_trace.enabled &&
        cs.pgm_hash == Shader::DreamsCompat::ModelFinalizeShader;
    const u64 model_finalize_gate_trace_offset =
        static_cast<u64>(Shader::DreamsCompat::ModelFinalizeGateTraceBaseDword) * sizeof(u32);
    const u32 model_finalize_gate_trace_size =
        Shader::DreamsCompat::ModelFinalizeGateTraceCount * sizeof(u32);
    const u64 model_finalize_gate_numeric_offset =
        static_cast<u64>(Shader::DreamsCompat::ModelFinalizeGateNumericBaseDword) * sizeof(u32);
    const u32 model_finalize_gate_numeric_size =
        Shader::DreamsCompat::ModelFinalizeGateNumericCount * sizeof(u32);
    if (model_finalize_gate_trace) {
        // Finish earlier GDS users and clear the host-private ranges on the GPU immediately
        // before this finalize dispatch. This keeps every logged ordinal independent.
        scheduler.Finish();
        auto* gds = buffer_cache.GetGdsBuffer();
        if (model_finalize_gate_trace_offset + model_finalize_gate_trace_size >
                gds->SizeBytes() ||
            model_finalize_gate_numeric_offset + model_finalize_gate_numeric_size >
                gds->SizeBytes()) {
            LOG_ERROR(Render_Vulkan,
                      "Dreams d049 gate trace ranges counters={:#x}+{:#x} "
                      "numeric={:#x}+{:#x} exceed GDS size {:#x}",
                      model_finalize_gate_trace_offset, model_finalize_gate_trace_size,
                      model_finalize_gate_numeric_offset, model_finalize_gate_numeric_size,
                      gds->SizeBytes());
            model_finalize_gate_trace = false;
        } else {
            gds->Fill(model_finalize_gate_trace_offset, model_finalize_gate_trace_size, 0);
            gds->Fill(model_finalize_gate_numeric_offset, model_finalize_gate_numeric_size, 0);
        }
    }

    bool gather_stage_trace =
        TraceDreamsGatherStages() &&
        cs.pgm_hash == Shader::DreamsCompat::GatherVoxelsShader;
    const u64 gather_stage_trace_offset =
        static_cast<u64>(Shader::DreamsCompat::GatherStageTraceBaseDword) * sizeof(u32);
    const u32 gather_stage_trace_size =
        Shader::DreamsCompat::GatherStageTraceCount * sizeof(u32);
    const u64 gather_numeric_trace_offset =
        static_cast<u64>(Shader::DreamsCompat::GatherNumericTraceBaseDword) * sizeof(u32);
    const u32 gather_numeric_trace_size =
        Shader::DreamsCompat::GatherNumericTraceCount * sizeof(u32);
    if (gather_stage_trace) {
        const auto user_data = [&](u32 index) {
            return index < cs.user_data.size() ? cs.user_data[index] : 0;
        };
        const auto flat_data = [&](u32 index) {
            return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
        };
        constexpr VAddr GuestAddressMask = 0x0000FFFFFFFFFFFFULL;
        constexpr std::array<u64, 8> GatherConstantOffsets{
            0x170, 0x174, 0x180, 0x184, 0x188, 0x18c, 0x190, 0x194};
        const VAddr gather_constant_base =
            ((static_cast<VAddr>(user_data(1)) << 32) | user_data(0)) & GuestAddressMask;
        const VAddr gather_hash_constant_base =
            ((static_cast<VAddr>(user_data(3)) << 32) | user_data(2)) & GuestAddressMask;
        std::array<u32, GatherConstantOffsets.size()> gather_constant_cpu{};
        std::array<bool, GatherConstantOffsets.size()> gather_constant_mapped{};
        std::array<bool, GatherConstantOffsets.size()> gather_constant_gpu_modified{};
        for (u32 index = 0; index < GatherConstantOffsets.size(); ++index) {
            const VAddr address = gather_constant_base + GatherConstantOffsets[index];
            gather_constant_mapped[index] =
                gather_constant_base != 0 && memory->IsValidMapping(address, sizeof(u32));
            if (!gather_constant_mapped[index]) {
                continue;
            }
            std::memcpy(&gather_constant_cpu[index], std::bit_cast<const void*>(address),
                        sizeof(u32));
            gather_constant_gpu_modified[index] =
                buffer_cache.IsRegionGpuModified(address, sizeof(u32));
        }
        constexpr std::array<u64, 6> GatherHashConstantOffsets{0, 4, 8, 12, 16, 24};
        std::array<u32, GatherHashConstantOffsets.size()> gather_hash_constant_cpu{};
        std::array<bool, GatherHashConstantOffsets.size()> gather_hash_constant_mapped{};
        std::array<bool, GatherHashConstantOffsets.size()> gather_hash_constant_gpu_modified{};
        for (u32 index = 0; index < GatherHashConstantOffsets.size(); ++index) {
            const VAddr address = gather_hash_constant_base + GatherHashConstantOffsets[index];
            gather_hash_constant_mapped[index] =
                gather_hash_constant_base != 0 && memory->IsValidMapping(address, sizeof(u32));
            if (!gather_hash_constant_mapped[index]) {
                continue;
            }
            std::memcpy(&gather_hash_constant_cpu[index], std::bit_cast<const void*>(address),
                        sizeof(u32));
            gather_hash_constant_gpu_modified[index] =
                buffer_cache.IsRegionGpuModified(address, sizeof(u32));
        }
        LOG_WARNING(
            Render_Vulkan,
            "Dreams Gather stage trace begin dims={}x{}x{} "
            "ud0-10={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
            "flat96-103={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} cb_base={:#x} "
            "cb92-101={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
            "mapped={},{},{},{},{},{},{},{} gpu_modified={},{},{},{},{},{},{},{}",
            cs_program.dim_x, cs_program.dim_y, cs_program.dim_z, user_data(0), user_data(1),
            user_data(2), user_data(3), user_data(4), user_data(5), user_data(6), user_data(7),
            user_data(8), user_data(9), user_data(10), flat_data(96), flat_data(97),
            flat_data(98), flat_data(99), flat_data(100), flat_data(101), flat_data(102),
            flat_data(103), gather_constant_base, gather_constant_cpu[0], gather_constant_cpu[1],
            gather_constant_cpu[2], gather_constant_cpu[3], gather_constant_cpu[4],
            gather_constant_cpu[5], gather_constant_cpu[6], gather_constant_cpu[7],
            gather_constant_mapped[0], gather_constant_mapped[1], gather_constant_mapped[2],
            gather_constant_mapped[3], gather_constant_mapped[4], gather_constant_mapped[5],
            gather_constant_mapped[6], gather_constant_mapped[7],
            gather_constant_gpu_modified[0], gather_constant_gpu_modified[1],
            gather_constant_gpu_modified[2], gather_constant_gpu_modified[3],
            gather_constant_gpu_modified[4], gather_constant_gpu_modified[5],
            gather_constant_gpu_modified[6], gather_constant_gpu_modified[7]);
        LOG_WARNING(
            Render_Vulkan,
            "Dreams Gather hash constants flat104-109={:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
            "hash_cb_base={:#x} cb0-4,6={:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
            "mapped={},{},{},{},{},{} gpu_modified={},{},{},{},{},{}",
            flat_data(104), flat_data(105), flat_data(106), flat_data(107), flat_data(108),
            flat_data(109), gather_hash_constant_base, gather_hash_constant_cpu[0],
            gather_hash_constant_cpu[1], gather_hash_constant_cpu[2],
            gather_hash_constant_cpu[3], gather_hash_constant_cpu[4],
            gather_hash_constant_cpu[5],
            gather_hash_constant_mapped[0], gather_hash_constant_mapped[1],
            gather_hash_constant_mapped[2], gather_hash_constant_mapped[3],
            gather_hash_constant_mapped[4], gather_hash_constant_mapped[5],
            gather_hash_constant_gpu_modified[0], gather_hash_constant_gpu_modified[1],
            gather_hash_constant_gpu_modified[2], gather_hash_constant_gpu_modified[3],
            gather_hash_constant_gpu_modified[4], gather_hash_constant_gpu_modified[5]);
        // Finish all prior GDS users, then clear on the GPU so this is also correct when the
        // host-visible GDS allocation is not host coherent.
        scheduler.Finish();
        auto* gds = buffer_cache.GetGdsBuffer();
        if (gather_stage_trace_offset + gather_stage_trace_size > gds->SizeBytes() ||
            gather_numeric_trace_offset + gather_numeric_trace_size > gds->SizeBytes()) {
            LOG_ERROR(Render_Vulkan,
                      "Dreams Gather trace ranges stage={:#x}+{:#x} numeric={:#x}+{:#x} "
                      "exceed GDS size {:#x}",
                      gather_stage_trace_offset, gather_stage_trace_size,
                      gather_numeric_trace_offset, gather_numeric_trace_size, gds->SizeBytes());
            gather_stage_trace = false;
        } else {
            gds->Fill(gather_stage_trace_offset, gather_stage_trace_size, 0);
            gds->Fill(gather_numeric_trace_offset, gather_numeric_trace_size, 0);
        }
    }
    const auto read_gather_stage_trace = [&] {
        std::array<u32, Shader::DreamsCompat::GatherStageTraceCount> values{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        if (gather_stage_trace_offset + sizeof(values) <= gds->mapped_data.size()) {
            std::memcpy(values.data(), gds->mapped_data.data() + gather_stage_trace_offset,
                        sizeof(values));
        }
        return values;
    };
    const auto read_gather_numeric_trace = [&] {
        std::array<u32, Shader::DreamsCompat::GatherNumericTraceCount> values{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        if (gather_numeric_trace_offset + sizeof(values) <= gds->mapped_data.size()) {
            std::memcpy(values.data(), gds->mapped_data.data() + gather_numeric_trace_offset,
                        sizeof(values));
        }
        return values;
    };

    if (!BindResources(pipeline)) {
        return;
    }

    struct DreamsTemporalContentDispatchCapture {
        bool enabled{};
        u32 ordinal{};
        u32 chain{};
        u32 chain_dispatch{};
        std::array<DreamsTemporalContentImage, 4> images{};
        std::array<u32, 5> constants{};
        std::array<std::filesystem::path, 4> pre_files{};
        std::filesystem::path post_file;
    } temporal_content_capture{};
    constexpr std::array<std::string_view, 4> TemporalContentRoles{
        "output", "history", "current", "depth"};
    const auto collect_temporal_content_images = [&] {
        std::array<DreamsTemporalContentImage, 4> images{};
        u32 flattened_index{};
        for (u32 binding = 0; binding < images.size(); ++binding) {
            auto& entry = images[binding];
            entry.binding = binding;
            entry.flattened_index = flattened_index;
            if (binding >= cs.images.size()) {
                continue;
            }

            const auto& image_desc = cs.images[binding];
            const auto tsharp = image_desc.GetSharp(cs);
            const bool null_descriptor =
                tsharp.Address() == 0 ||
                tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid;
            entry.written = image_desc.is_written;
            entry.array_size = null_descriptor ? 1 : image_desc.NumBindings(cs);
            entry.descriptor_base = tsharp.Address();
            entry.descriptor_width = static_cast<u32>(tsharp.width) + 1;
            entry.descriptor_height = static_cast<u32>(tsharp.height) + 1;
            entry.descriptor_depth = static_cast<u32>(tsharp.depth) + 1;
            entry.descriptor_pitch = tsharp.Pitch();

            if (flattened_index < image_bindings.size()) {
                const auto& [image_id, desc] = image_bindings[flattened_index];
                entry.image_id = image_id;
                entry.view_format = static_cast<u32>(desc.view_info.format);
                entry.base_level = desc.view_info.range.base.level;
                entry.levels = desc.view_info.range.extent.levels;
                entry.base_layer = desc.view_info.range.base.layer;
                entry.layers = desc.view_info.range.extent.layers;
                if (image_id) {
                    const auto& image = texture_cache.GetImage(image_id);
                    entry.valid = true;
                    entry.image_uid = image.image_uid;
                    entry.guest_base = image.info.guest_address;
                    entry.guest_size = image.info.guest_size;
                    entry.cached_width = image.info.size.width;
                    entry.cached_height = image.info.size.height;
                    entry.cached_depth = image.info.size.depth;
                    entry.cached_pitch = image.info.pitch;
                    entry.bits = image.info.num_bits;
                    entry.image_format = static_cast<u32>(image.info.pixel_format);
                    entry.tile_mode = static_cast<u32>(image.info.tile_mode);
                    entry.linear_size =
                        static_cast<u64>(image.info.pitch) * image.info.size.height *
                        image.info.size.depth * image.info.resources.layers *
                        (image.info.num_bits / 8);
                }
            }
            flattened_index += entry.array_size;
        }
        return images;
    };
    const auto restore_temporal_content_images = [&](std::span<const DreamsTemporalContentImage>
                                                         images) {
        std::array<VideoCore::ImageId, 4> restored{};
        u32 restored_count{};
        for (const auto& entry : images) {
            if (!entry.valid || entry.flattened_index >= image_bindings.size() ||
                entry.flattened_index >= image_infos.size() ||
                std::find(restored.begin(), restored.begin() + restored_count, entry.image_id) !=
                    restored.begin() + restored_count) {
                continue;
            }
            const auto& [image_id, desc] = image_bindings[entry.flattened_index];
            if (image_id != entry.image_id) {
                continue;
            }
            auto access = vk::AccessFlags2{vk::AccessFlagBits2::eShaderRead};
            if (desc.type == VideoCore::TextureCache::BindingType::Storage) {
                access |= vk::AccessFlagBits2::eShaderWrite;
            }
            texture_cache.GetImage(image_id).Transit(
                image_infos[entry.flattened_index].imageLayout, access, desc.view_info.range);
            restored[restored_count++] = image_id;
        }
    };

    if (cs.pgm_hash == Shader::DreamsCompat::TemporalResolveShader &&
        BeginDreamsTemporalContentCaptureDispatch()) {
        auto& state = g_dreams_temporal_content_capture;
        temporal_content_capture.enabled = true;
        temporal_content_capture.ordinal = ++state.ordinal;
        temporal_content_capture.images = collect_temporal_content_images();
        for (u32 index = 0; index < temporal_content_capture.constants.size(); ++index) {
            // The shader's SRT dwords 28-32 are remapped after the 16-word user-data prefix.
            const u32 flat_index = 44 + index;
            temporal_content_capture.constants[index] =
                flat_index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[flat_index] : 0;
        }
        std::tie(temporal_content_capture.chain, temporal_content_capture.chain_dispatch) =
            AssignDreamsTemporalContentChain(temporal_content_capture.images);

        const auto image_address = [](const DreamsTemporalContentImage& image) {
            return image.valid ? image.guest_base : image.descriptor_base;
        };
        const VAddr output_address = image_address(temporal_content_capture.images[0]);
        const VAddr history_address = image_address(temporal_content_capture.images[1]);
        {
            std::ofstream dispatches{state.output_dir / "dispatches.tsv", std::ios::app};
            dispatches << temporal_content_capture.ordinal << '\t' << g_compute_dispatch_sequence
                       << '\t' << temporal_content_capture.chain << '\t'
                       << temporal_content_capture.chain_dispatch << '\t'
                       << std::min(output_address, history_address) << '\t'
                       << std::max(output_address, history_address) << '\t' << cs_program.dim_x
                       << '\t' << cs_program.dim_y << '\t' << cs_program.dim_z;
            for (const u32 value : temporal_content_capture.constants) {
                dispatches << '\t' << value;
            }
            dispatches << '\n';
        }

        boost::container::small_vector<VideoCore::TextureCache::LinearImageDump, 4> dumps;
        for (u32 binding = 0; binding < temporal_content_capture.images.size(); ++binding) {
            const auto& image = temporal_content_capture.images[binding];
            if (image.valid) {
                temporal_content_capture.pre_files[binding] =
                    state.output_dir /
                    fmt::format("dispatch-{:02}-chain-{:02}-chain-dispatch-{:02}-pre-b{}-{}.bin",
                                temporal_content_capture.ordinal, temporal_content_capture.chain,
                                temporal_content_capture.chain_dispatch, binding,
                                TemporalContentRoles[binding]);
                dumps.push_back({image.image_id, temporal_content_capture.pre_files[binding]});
            }
        }
        texture_cache.DumpImagesLinear(dumps);
        restore_temporal_content_images(temporal_content_capture.images);
        for (u32 binding = 0; binding < temporal_content_capture.images.size(); ++binding) {
            WriteDreamsTemporalContentImageMetadata(
                temporal_content_capture.ordinal, g_compute_dispatch_sequence,
                temporal_content_capture.chain, "pre", TemporalContentRoles[binding],
                temporal_content_capture.images[binding],
                temporal_content_capture.pre_files[binding]);
        }
        temporal_content_capture.post_file =
            state.output_dir /
            fmt::format("dispatch-{:02}-chain-{:02}-chain-dispatch-{:02}-post-b0-output.bin",
                        temporal_content_capture.ordinal, temporal_content_capture.chain,
                        temporal_content_capture.chain_dispatch);
        LOG_WARNING(Render_Vulkan,
                    "Dreams temporal content pre ordinal={} sequence={} chain={} chain_dispatch={} "
                    "output={:#x} history={:#x} bindings={} srt28-32/flat44-48={:#010x},{:#010x},"
                    "{:#010x},{:#010x},{:#010x}",
                    temporal_content_capture.ordinal, g_compute_dispatch_sequence,
                    temporal_content_capture.chain, temporal_content_capture.chain_dispatch,
                    output_address, history_address, image_bindings.size(),
                    temporal_content_capture.constants[0], temporal_content_capture.constants[1],
                    temporal_content_capture.constants[2], temporal_content_capture.constants[3],
                    temporal_content_capture.constants[4]);
    }

    struct DreamsGatherDeviceReadback {
        bool enabled{};
        u32 ordinal{};
        VAddr guest_address{};
        vk::Buffer source_buffer{};
        u64 source_offset{};
        u64 source_range{};
        u64 download_offset{};
        u8* download_data{};
        bool download_coherent{};
        std::array<u32, 8> guest_before{};
    } gather_device_readback{};
    const char* gather_device_readback_value =
        std::getenv("SHADPS4_DREAMS_GATHER_DEVICE_READBACK");
    if (cs.pgm_hash == Shader::DreamsCompat::GatherVoxelsShader &&
        gather_device_readback_value != nullptr &&
        std::string_view{gather_device_readback_value} == "1" && cs.buffers.size() > 14 &&
        !cs.buffers[14].IsSpecial() && buffer_infos.size() > 14) {
        static u32 gather_device_readback_count{};
        if (gather_device_readback_count < 8) {
            const auto sharp = cs.buffers[14].GetSharp(cs);
            constexpr u32 ProbeSize = 8 * sizeof(u32);
            if (sharp.base_address != 0 && sharp.GetSize() >= ProbeSize &&
                memory->IsValidMapping(sharp.base_address, ProbeSize)) {
                const auto& source = buffer_infos[14];
                auto& download =
                    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Download);
                const auto [download_data, download_offset] = download.Map(ProbeSize, 64);
                if (download_data != nullptr) {
                    download.Commit();
                    gather_device_readback.enabled = true;
                    gather_device_readback.ordinal = ++gather_device_readback_count;
                    gather_device_readback.guest_address = sharp.base_address;
                    gather_device_readback.source_buffer = source.buffer;
                    gather_device_readback.source_offset = source.offset;
                    gather_device_readback.source_range = source.range;
                    gather_device_readback.download_offset = download_offset;
                    gather_device_readback.download_data = download_data;
                    gather_device_readback.download_coherent = download.is_coherent;
                    std::memcpy(gather_device_readback.guest_before.data(),
                                std::bit_cast<const void*>(sharp.base_address), ProbeSize);

                    // Copy from the exact VkBuffer and offset already stored in descriptor 14.
                    // Do not refind the guest address: BindTextures may have replaced the cache
                    // buffer after BindBuffers built this descriptor.
                    scheduler.EndRendering();
                    const auto probe_cmdbuf = scheduler.CommandBuffer();
                    const vk::BufferMemoryBarrier2 source_pre = {
                        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                        .buffer = source.buffer,
                        .offset = source.offset,
                        .size = ProbeSize,
                    };
                    probe_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                        .bufferMemoryBarrierCount = 1,
                        .pBufferMemoryBarriers = &source_pre,
                    });
                    const vk::BufferCopy probe_copy = {
                        .srcOffset = source.offset,
                        .dstOffset = download_offset,
                        .size = ProbeSize,
                    };
                    probe_cmdbuf.copyBuffer(source.buffer, download.Handle(), probe_copy);
                    const std::array<vk::BufferMemoryBarrier2, 2> probe_post{{
                        {
                            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                            .buffer = source.buffer,
                            .offset = source.offset,
                            .size = ProbeSize,
                        },
                        {
                            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                            .buffer = download.Handle(),
                            .offset = download_offset,
                            .size = ProbeSize,
                        },
                    }};
                    probe_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                        .bufferMemoryBarrierCount = static_cast<u32>(probe_post.size()),
                        .pBufferMemoryBarriers = probe_post.data(),
                    });
                }
            }
        }
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    const bool profile = ShouldProfileCompute();
    const auto profile_start = std::chrono::steady_clock::now();
    u32 dispatch_x = cs_program.dim_x;
    u32 dispatch_base_x{};
    if (Common::ElfInfo::Instance().GameSerial() == "CUSA04301" &&
        cs.pgm_hash == 0x7ba4de5d) {
        static const bool diagnostic_skip =
            std::getenv("SHADPS4_DREAMS_GATHER_VOXELS_SKIP") != nullptr;
        static const u32 diagnostic_cap = [] {
            const char* value = std::getenv("SHADPS4_DREAMS_GATHER_VOXELS_CAP");
            return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0u;
        }();
        static const u32 diagnostic_base = [] {
            const char* value = std::getenv("SHADPS4_DREAMS_GATHER_VOXELS_BASE");
            return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0u;
        }();
        dispatch_base_x = std::min(diagnostic_base, dispatch_x);
        dispatch_x -= dispatch_base_x;
        if (diagnostic_skip) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams diagnostic: skipping GatherVoxelsChunk dispatch of {} groups",
                        dispatch_x);
            dispatch_x = 0;
        } else if (diagnostic_cap != 0 && dispatch_x > diagnostic_cap) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams diagnostic: limiting GatherVoxelsChunk dispatch base={} "
                        "remaining={} -> {} groups",
                        dispatch_base_x, dispatch_x, diagnostic_cap);
            dispatch_x = diagnostic_cap;
        }
    }
    static const u32 gather_batch_size = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_GATHER_VOXELS_BATCH");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0u;
    }();
    static const u32 gather_submit_batch_size = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_GATHER_VOXELS_SUBMIT_BATCH");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0u;
    }();
    const auto download_gather_diagnostic_output = [&] {
        if (!gather_post_diagnostic || gather_post_buffer_ready[2] || dispatch_x == 0 ||
            cs.buffers.size() <= 2 || cs.buffers[2].IsSpecial()) {
            return;
        }
        // ReadMemory appends the transfer to the current command buffer and fences it. Calling
        // this while the command buffer still contains the final Gather dispatch keeps the
        // diagnostic download in that same submit.
        const auto output = cs.buffers[2].GetSharp(cs);
        const u64 read_size = std::min<u64>(output.GetSize(), 16_MB) & ~u64{3};
        if (output.base_address == 0 || read_size < sizeof(u32) ||
            !memory->IsValidMapping(output.base_address, read_size)) {
            return;
        }
        gather_post_buffer_gpu_modified[2] =
            buffer_cache.IsRegionGpuModified(output.base_address, read_size);
        buffer_cache.ReadMemory(output.base_address, read_size);
        gather_post_buffer_ready[2] = true;
    };
    if (cs.pgm_hash == DreamsTraversalShader ||
        (cs.pgm_hash == Shader::DreamsCompat::SceneCompactShader &&
         OrderDreamsSceneCompact())) {
        DispatchDreamsTraversalOrdered(cmdbuf, cs_program.dim_x, cs_program.dim_y,
                                       cs_program.dim_z);
    } else if (cs.pgm_hash == 0x7ba4de5d && gather_submit_batch_size != 0 &&
               dispatch_x != 0) {
        for (const u32 index : {5u, 6u}) {
            if (index >= cs.buffers.size() || cs.buffers[index].IsSpecial()) {
                continue;
            }
            const auto sharp = cs.buffers[index].GetSharp(cs);
            LOG_WARNING(Render_Vulkan,
                        "Dreams diagnostic: GatherVoxelsChunk buffer #{} base={:#x}+{:#x} "
                        "stride={} records={} write={}",
                        index, sharp.base_address, sharp.GetSize(), sharp.stride,
                        sharp.num_records, cs.buffers[index].is_written);
        }
        const vk::MemoryBarrier2 gather_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                             vk::AccessFlagBits2::eShaderWrite,
        };
        const vk::DependencyInfo gather_dependency = {
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &gather_barrier,
        };
        for (u32 offset = 0; offset < dispatch_x; offset += gather_submit_batch_size) {
            const u32 count = std::min(gather_submit_batch_size, dispatch_x - offset);
            cmdbuf.dispatchBase(dispatch_base_x + offset, 0, 0, count, cs_program.dim_y,
                                cs_program.dim_z);
            const bool has_more = offset + count < dispatch_x;
            if (has_more) {
                cmdbuf.pipelineBarrier2(gather_dependency);
            } else {
                download_gather_diagnostic_output();
            }
            scheduler.Finish();
            std::array<u32, 3> gather_gds{};
            constexpr std::array<u32, 3> GatherGdsIndices{64, 65, 122};
            const auto* gds = buffer_cache.GetGdsBuffer();
            for (u32 index = 0; index < GatherGdsIndices.size(); ++index) {
                const u64 byte_offset =
                    static_cast<u64>(GatherGdsIndices[index]) * sizeof(u32);
                if (byte_offset + sizeof(u32) <= gds->mapped_data.size()) {
                    std::memcpy(&gather_gds[index], gds->mapped_data.data() + byte_offset,
                                sizeof(u32));
                }
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams diagnostic: GatherVoxelsChunk submit batch base={} groups={} "
                        "complete GDS64={} GDS65={} GDS122={}",
                        dispatch_base_x + offset, count, gather_gds[0], gather_gds[1],
                        gather_gds[2]);
            if (gather_stage_trace) {
                const auto stages = read_gather_stage_trace();
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams Gather stage trace cumulative base={} groups_done={} "
                    "entry={} active={} class_any={} hash_attempt={} hash_found={} "
                    "take_output={} reached_gds64={} sentinel={} local_lt8={} class_empty={} "
                    "cb93_positive={} local_and_cb={} final_pre_save={} class_reduce_reach={} "
                    "v47_nonzero={} v47_bit6={} v47_bit7={} v47_both={} v35_nonzero={} "
                    "input_reach={} input_any_nan={} input_all_exact_1={} input_any_ne_1={} "
                    "input_any_lt_0875={} input_any_eq_0875={} input_any_gt_1={} "
                    "producer_reach={} producer_any_nan={} producer_all_exact_1={} "
                    "producer_any_ne_1={} producer_any_lt_0875={} explicit_hit={} "
                    "explicit_fallback={} explicit_value_negative={} fallback_negative={} "
                    "fallback_positive={} low16_negative={} special_abs8={} direct={} "
                    "direct11_path={} sparse10_path={} sparse_l1_hit={} sparse_l1_miss={} "
                    "sparse_l2_hit={} sparse_l2_miss={} sparse_leaf_hit={} sparse_leaf_miss={} "
                    "pre67c_explicit_nonzero={} pre67c_fallback_nonzero={} "
                    "post67c_explicit_nonzero={} post67c_fallback_nonzero={} "
                    "root_in_range={} root_oob={} root_negative={} root_nonnegative={} "
                    "root_minus2={} root_other_negative={} sparse_l1_reach={} "
                    "probe_reach={} probe_within_limit={} probe_exhausted={} "
                    "key_equal={} key_mismatch={} x_underflow={} y_underflow={} "
                    "z_underflow={} v4_ge_2048={} v11_ge_2048={} v5_ge_2048={} "
                    "max_high_bit={} max_ffffffff={}",
                    dispatch_base_x, offset + count, stages[0], stages[1], stages[2], stages[3],
                    stages[4], stages[5], stages[6], stages[7], stages[8], stages[9], stages[10],
                    stages[11], stages[12], stages[13], stages[14], stages[15], stages[16],
                    stages[17], stages[18], stages[19], stages[20], stages[21], stages[22],
                    stages[23], stages[24], stages[25], stages[26], stages[27], stages[28],
                    stages[29], stages[30], stages[31], stages[32], stages[33], stages[34],
                    stages[35], stages[36], stages[37], stages[38], stages[39], stages[40],
                    stages[41], stages[42], stages[43], stages[44], stages[45], stages[46],
                    stages[47], stages[48], stages[49], stages[50], stages[51], stages[52],
                    stages[53], stages[54], stages[55], stages[56], stages[57], stages[58],
                    stages[59], stages[60], stages[61], stages[62], stages[63], stages[64],
                    stages[65], stages[66], stages[67], stages[68], stages[69], stages[70]);
            }
            if (has_more) {
                pipeline->BindResources(set_writes, buffer_barriers, push_data);
                cmdbuf = scheduler.CommandBuffer();
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
            }
        }
    } else if (cs.pgm_hash == 0x7ba4de5d && gather_batch_size != 0 && dispatch_x != 0) {
        const vk::MemoryBarrier2 gather_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                             vk::AccessFlagBits2::eShaderWrite,
        };
        const vk::DependencyInfo gather_dependency = {
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &gather_barrier,
        };
        for (u32 offset = 0; offset < dispatch_x; offset += gather_batch_size) {
            const u32 count = std::min(gather_batch_size, dispatch_x - offset);
            cmdbuf.dispatchBase(dispatch_base_x + offset, 0, 0, count, cs_program.dim_y,
                                cs_program.dim_z);
            if (offset + count < dispatch_x) {
                cmdbuf.pipelineBarrier2(gather_dependency);
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams diagnostic: batched GatherVoxelsChunk base={} groups={} batch={}",
                    dispatch_base_x, dispatch_x, gather_batch_size);
    } else if (dispatch_base_x != 0) {
        cmdbuf.dispatchBase(dispatch_base_x, 0, 0, dispatch_x, cs_program.dim_y,
                            cs_program.dim_z);
    } else {
        cmdbuf.dispatch(dispatch_x, cs_program.dim_y, cs_program.dim_z);
    }
    if (temporal_content_capture.enabled) {
        const auto& output = temporal_content_capture.images[0];
        if (output.valid) {
            const std::array dumps{VideoCore::TextureCache::LinearImageDump{
                output.image_id, temporal_content_capture.post_file}};
            texture_cache.DumpImagesLinear(dumps);
            restore_temporal_content_images(
                std::span<const DreamsTemporalContentImage>{&output, 1});
        }
        WriteDreamsTemporalContentImageMetadata(
            temporal_content_capture.ordinal, g_compute_dispatch_sequence,
            temporal_content_capture.chain, "post", TemporalContentRoles[0], output,
            temporal_content_capture.post_file);
        LOG_WARNING(Render_Vulkan,
                    "Dreams temporal content post ordinal={} sequence={} chain={} output_id={} "
                    "file={}",
                    temporal_content_capture.ordinal, g_compute_dispatch_sequence,
                    temporal_content_capture.chain, output.image_id.index,
                    temporal_content_capture.post_file.string());
        FinishDreamsTemporalContentCaptureDispatch();
    }
    download_gather_diagnostic_output();
    if (Common::ElfInfo::Instance().GameSerial() == "CUSA04301" &&
        cs.pgm_hash == 0x7ba4de5d &&
        (std::getenv("SHADPS4_DREAMS_GATHER_VOXELS_SYNC") != nullptr ||
         ProfileDreamsGpuAfterGather() || gather_post_diagnostic || gather_stage_trace ||
         gather_device_readback.enabled)) {
        const auto gather_start = std::chrono::steady_clock::now();
        scheduler.Finish();
        const auto gather_elapsed = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - gather_start)
                                        .count();
        std::array<u32, 3> gather_gds{};
        constexpr std::array<u32, 3> GatherGdsIndices{64, 65, 122};
        const auto* gds = buffer_cache.GetGdsBuffer();
        for (u32 index = 0; index < GatherGdsIndices.size(); ++index) {
            const u64 byte_offset =
                static_cast<u64>(GatherGdsIndices[index]) * sizeof(u32);
            if (byte_offset + sizeof(u32) <= gds->mapped_data.size()) {
                std::memcpy(&gather_gds[index], gds->mapped_data.data() + byte_offset,
                            sizeof(u32));
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams diagnostic: GatherVoxelsChunk completed {} groups in {:.3f}ms "
                    "GDS64={} GDS65={} GDS122={}",
                    dispatch_x, gather_elapsed, gather_gds[0], gather_gds[1], gather_gds[2]);
        if (gather_device_readback.enabled) {
            std::array<u32, 8> descriptor_head{};
            std::memcpy(descriptor_head.data(), gather_device_readback.download_data,
                        sizeof(descriptor_head));
            const auto& guest_before = gather_device_readback.guest_before;
            LOG_WARNING(
                Render_Vulkan,
                "Dreams Gather descriptor readback trace={} guest={:#x}+{:#x} "
                "descriptor_vk={:#x} descriptor_offset={:#x} descriptor_range={:#x} "
                "download_offset={:#x} coherent={} "
                "before={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x} "
                "descriptor={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                gather_device_readback.ordinal, gather_device_readback.guest_address,
                sizeof(descriptor_head),
                reinterpret_cast<uintptr_t>(
                    static_cast<VkBuffer>(gather_device_readback.source_buffer)),
                gather_device_readback.source_offset, gather_device_readback.source_range,
                gather_device_readback.download_offset,
                gather_device_readback.download_coherent, guest_before[0], guest_before[1],
                guest_before[2], guest_before[3], guest_before[4], guest_before[5],
                guest_before[6], guest_before[7], descriptor_head[0], descriptor_head[1],
                descriptor_head[2], descriptor_head[3], descriptor_head[4], descriptor_head[5],
                descriptor_head[6], descriptor_head[7]);
        }
        if (gather_stage_trace) {
            const auto stages = read_gather_stage_trace();
            LOG_WARNING(Render_Vulkan,
                        "Dreams Gather stage trace final base={} groups={} entry={} active={} "
                        "class_any={} hash_attempt={} hash_found={} take_output={} "
                        "reached_gds64={} sentinel={} local_lt8={} class_empty={} "
                        "cb93_positive={} local_and_cb={} final_pre_save={} "
                        "class_reduce_reach={} v47_nonzero={} v47_bit6={} v47_bit7={} "
                        "v47_both={} v35_nonzero={} input_reach={} input_any_nan={} "
                        "input_all_exact_1={} input_any_ne_1={} input_any_lt_0875={} "
                        "input_any_eq_0875={} input_any_gt_1={} producer_reach={} "
                        "producer_any_nan={} producer_all_exact_1={} producer_any_ne_1={} "
                        "producer_any_lt_0875={} explicit_hit={} explicit_fallback={} "
                        "explicit_value_negative={} fallback_negative={} fallback_positive={} "
                        "low16_negative={} special_abs8={} direct={} direct11_path={} "
                        "sparse10_path={} sparse_l1_hit={} sparse_l1_miss={} sparse_l2_hit={} "
                        "sparse_l2_miss={} sparse_leaf_hit={} sparse_leaf_miss={} "
                        "pre67c_explicit_nonzero={} pre67c_fallback_nonzero={} "
                        "post67c_explicit_nonzero={} post67c_fallback_nonzero={} "
                        "root_in_range={} root_oob={} root_negative={} root_nonnegative={} "
                        "root_minus2={} root_other_negative={} sparse_l1_reach={} "
                        "probe_reach={} probe_within_limit={} probe_exhausted={} "
                        "key_equal={} key_mismatch={} x_underflow={} y_underflow={} "
                        "z_underflow={} v4_ge_2048={} v11_ge_2048={} v5_ge_2048={} "
                        "max_high_bit={} max_ffffffff={}",
                        dispatch_base_x, dispatch_x, stages[0], stages[1], stages[2], stages[3],
                        stages[4], stages[5], stages[6], stages[7], stages[8], stages[9],
                        stages[10], stages[11], stages[12], stages[13], stages[14], stages[15],
                        stages[16], stages[17], stages[18], stages[19], stages[20], stages[21],
                        stages[22], stages[23], stages[24], stages[25], stages[26], stages[27],
                        stages[28], stages[29], stages[30], stages[31], stages[32], stages[33],
                        stages[34], stages[35], stages[36], stages[37], stages[38], stages[39],
                        stages[40], stages[41], stages[42], stages[43], stages[44], stages[45],
                        stages[46], stages[47], stages[48], stages[49], stages[50], stages[51],
                        stages[52], stages[53], stages[54], stages[55], stages[56], stages[57],
                        stages[58], stages[59], stages[60], stages[61], stages[62], stages[63],
                        stages[64], stages[65], stages[66], stages[67], stages[68], stages[69],
                        stages[70]);
            const auto numeric = read_gather_numeric_trace();
            LOG_WARNING(
                Render_Vulkan,
                "Dreams Gather numeric trace final base={} groups={} "
                "post28(valid,lane,v9)={:#x},{:#x},{:#x} "
                "pre180(valid,lane,v8,v11,v15)={:#x},{:#x},{:#x},{:#x},{:#x} "
                "post180_end184(valid,lane,v8,v11,v15)={:#x},{:#x},{:#x},{:#x},{:#x} "
                "post194_exec(valid,lane,v8,v11,v15)={:#x},{:#x},{:#x},{:#x},{:#x} "
                "post270(valid,lane,target_v5)={:#x},{:#x},{:#x} "
                "post2d0(valid,lane,bucket_v1)={:#x},{:#x},{:#x} "
                "pre2f4(valid,lane,bound_v18)={:#x},{:#x},{:#x} "
                "pre410(valid,lane,v1,v2,v3)={:#x},{:#x},{:#x},{:#x},{:#x} "
                "post418(valid,lane,v4,v11,v5)={:#x},{:#x},{:#x},{:#x},{:#x} "
                "gpu_head_valid={:#x} gpu_head0-7={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                dispatch_base_x, dispatch_x, numeric[0], numeric[1], numeric[2], numeric[3],
                numeric[4], numeric[5], numeric[6], numeric[7], numeric[8], numeric[9],
                numeric[10], numeric[11], numeric[12], numeric[13], numeric[14], numeric[15],
                numeric[16], numeric[17], numeric[28], numeric[29], numeric[30], numeric[31],
                numeric[32], numeric[33], numeric[34], numeric[35], numeric[36], numeric[18],
                numeric[19], numeric[20], numeric[21], numeric[22], numeric[23], numeric[24],
                numeric[25], numeric[26], numeric[27], numeric[45], numeric[37], numeric[38],
                numeric[39], numeric[40], numeric[41], numeric[42], numeric[43], numeric[44]);
        }
        if (gather_post_diagnostic) {
            const auto flat = [&](u32 index) {
                return index < cs.flattened_ud_buf.size() ? cs.flattened_ud_buf[index] : 0;
            };
            LOG_WARNING(Render_Vulkan,
                        "Dreams Gather post #{} sequence={} base={} groups={} flat_size={} "
                        "CB92(flat96)={:#x} CB93(flat97)={:#x}",
                        gather_post_diagnostic_ordinal, g_compute_dispatch_sequence,
                        dispatch_base_x, dispatch_x, cs.flattened_ud_buf.size(), flat(96), flat(97));

            for (const u32 buffer_index : {2u}) {
                if (buffer_index >= cs.buffers.size() || cs.buffers[buffer_index].IsSpecial()) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams Gather post #{} buffer #{} unavailable buffers={} "
                                "special={}",
                                gather_post_diagnostic_ordinal, buffer_index, cs.buffers.size(),
                                buffer_index < cs.buffers.size() &&
                                    cs.buffers[buffer_index].IsSpecial());
                    continue;
                }

                const auto& desc = cs.buffers[buffer_index];
                const auto sharp = desc.GetSharp(cs);
                constexpr u64 GatherDiagnosticReadLimit = 16_MB;
                const u64 read_size =
                    std::min<u64>(sharp.GetSize(), GatherDiagnosticReadLimit) & ~u64{3};
                if (sharp.base_address == 0 || read_size < sizeof(u32) ||
                    !memory->IsValidMapping(sharp.base_address, read_size)) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams Gather post #{} buffer #{} invalid base={:#x}+{:#x} "
                                "stride={} records={} read={:#x}",
                                gather_post_diagnostic_ordinal, buffer_index, sharp.base_address,
                                sharp.GetSize(), sharp.stride, sharp.num_records, read_size);
                    continue;
                }
                if (!gather_post_buffer_ready[buffer_index]) {
                    LOG_WARNING(Render_Vulkan,
                                "Dreams Gather post #{} buffer #{} was not synchronized in the "
                                "safe submission window",
                                gather_post_diagnostic_ordinal, buffer_index);
                    continue;
                }
                const auto* bytes = std::bit_cast<const u8*>(sharp.base_address);
                const u64 stride = sharp.stride != 0 ? sharp.stride : sizeof(u32);
                const u64 available_records = 1 + (read_size - sizeof(u32)) / stride;
                const u64 record_count =
                    sharp.num_records != 0
                        ? std::min<u64>(sharp.num_records, available_records)
                        : available_records;

                u64 zero{};
                u64 all_ones{};
                u64 key_sentinel{};
                u64 first_nonzero = std::numeric_limits<u64>::max();
                u64 first_all_ones = std::numeric_limits<u64>::max();
                u32 first_nonzero_value{};
                u32 minimum = std::numeric_limits<u32>::max();
                u32 maximum{};
                u64 hash = 1469598103934665603ULL;
                std::array<u32, 8> head{};
                for (u64 record = 0; record < record_count; ++record) {
                    u32 value{};
                    std::memcpy(&value, bytes + record * stride, sizeof(value));
                    if (record < head.size()) {
                        head[record] = value;
                    }
                    zero += value == 0;
                    all_ones += value == std::numeric_limits<u32>::max();
                    key_sentinel += value == 0x0fffffffu;
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                    hash ^= value;
                    hash *= 1099511628211ULL;
                    if (value != 0 && first_nonzero == std::numeric_limits<u64>::max()) {
                        first_nonzero = record;
                        first_nonzero_value = value;
                    }
                    if (value == std::numeric_limits<u32>::max() &&
                        first_all_ones == std::numeric_limits<u64>::max()) {
                        first_all_ones = record;
                    }
                }
                if (record_count == 0) {
                    minimum = 0;
                }
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams Gather post #{} buffer #{} base={:#x}+{:#x} stride={} "
                    "records={} scanned={} bytes={:#x} truncated={} write={} gpu_modified={} "
                    "zero={} ffffffff={} 0fffffff={} nonzero={} min={:#x} max={:#x} "
                    "first_nonzero={} value={:#x} first_ffffffff={} hash={:#x} "
                    "head={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                    gather_post_diagnostic_ordinal, buffer_index, sharp.base_address,
                    sharp.GetSize(), sharp.stride, sharp.num_records, record_count, read_size,
                    read_size < sharp.GetSize(), desc.is_written,
                    gather_post_buffer_gpu_modified[buffer_index], zero, all_ones, key_sentinel,
                    record_count - zero, minimum, maximum, first_nonzero, first_nonzero_value,
                    first_all_ones, hash, head[0], head[1], head[2], head[3], head[4], head[5],
                    head[6], head[7]);
            }
        }
        if (ProfileDreamsGpuAfterGather()) {
            ArmDreamsGpuProfile();
            LOG_WARNING(Render_Vulkan,
                        "Dreams one-shot GPU profile auto-armed after GatherVoxelsChunk "
                        "sequence={}",
                        g_compute_dispatch_sequence);
        }
    }
    if (gpu_profile_ordinal != 0) {
        LOG_WARNING(Render_Vulkan,
                    "Dreams GPU profile #{} kind=dispatch hash={:#x} post-fence begin",
                    gpu_profile_ordinal, cs.pgm_hash);
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
    if (dreams_model_config_gds_trace.enabled) {
        scheduler.Finish();
        std::array<u32, 25> after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(after.data(), gds->mapped_data.data() + 704 * sizeof(u32), sizeof(after));
        u32 changed{};
        for (u32 index = 0; index < after.size(); ++index) {
            changed += after[index] != dreams_model_config_gds_trace.before[index];
        }
        const u32 source_base =
            cs.flattened_ud_buf.size() > 21 ? cs.flattened_ud_buf[21] : 0;
        u32 inner_counter_after{};
        std::memcpy(&inner_counter_after, gds->mapped_data.data() + 64 * sizeof(u32),
                    sizeof(u32));
        u32 inner_required_after{};
        std::memcpy(&inner_required_after, gds->mapped_data.data() + 65 * sizeof(u32),
                    sizeof(u32));
        if (cs.pgm_hash == 0xac97d610) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams collect-bricks GDS sequence={} dims={}x{}x{} "
                        "dword64={}->{} dword65={}->{}",
                        g_compute_dispatch_sequence, cs_program.dim_x, cs_program.dim_y,
                        cs_program.dim_z, dreams_model_config_gds_trace.inner_counter_before,
                        inner_counter_after, dreams_model_config_gds_trace.inner_required_before,
                        inner_required_after);
        }
        if (inner_counter_after != dreams_model_config_gds_trace.inner_counter_before) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams inner-model GDS64 shader={:#x} ordinal={} sequence={} "
                        "dims={}x{}x{} value={:#x}->{:#x} source_base={:#x}",
                        cs.pgm_hash, dreams_model_config_gds_trace.ordinal,
                        g_compute_dispatch_sequence, cs_program.dim_x, cs_program.dim_y,
                        cs_program.dim_z, dreams_model_config_gds_trace.inner_counter_before,
                        inner_counter_after, source_base);
        }
        std::array<u32, 7> prefix_a_after{};
        std::array<u32, 18> prefix_b_after{};
        std::memcpy(prefix_a_after.data(), gds->mapped_data.data() + 196 * sizeof(u32),
                    sizeof(prefix_a_after));
        std::memcpy(prefix_b_after.data(), gds->mapped_data.data() + 389 * sizeof(u32),
                    sizeof(prefix_b_after));
        const bool prefix_shader =
            cs.pgm_hash == 0xe275eba8 || cs.pgm_hash == 0xd2d439f3 ||
            cs.pgm_hash == 0xe165304b || cs.pgm_hash == 0x9f3303a7 ||
            cs.pgm_hash == 0xe9d99694;
        if (prefix_shader) {
            static u32 prefix_shader_direct_count{};
            if (prefix_shader_direct_count++ < 128) {
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams model prefix direct shader={:#x} sequence={} dims={}x{}x{} "
                    "GDS64={}->{} A196-202={},{},{},{},{},{},{} -> "
                    "{},{},{},{},{},{},{} B389,391,406={},{},{} -> {},{},{}",
                    cs.pgm_hash, g_compute_dispatch_sequence, cs_program.dim_x, cs_program.dim_y,
                    cs_program.dim_z, dreams_model_config_gds_trace.inner_counter_before,
                    inner_counter_after, dreams_model_config_gds_trace.prefix_a_before[0],
                    dreams_model_config_gds_trace.prefix_a_before[1],
                    dreams_model_config_gds_trace.prefix_a_before[2],
                    dreams_model_config_gds_trace.prefix_a_before[3],
                    dreams_model_config_gds_trace.prefix_a_before[4],
                    dreams_model_config_gds_trace.prefix_a_before[5],
                    dreams_model_config_gds_trace.prefix_a_before[6], prefix_a_after[0],
                    prefix_a_after[1], prefix_a_after[2], prefix_a_after[3], prefix_a_after[4],
                    prefix_a_after[5], prefix_a_after[6],
                    dreams_model_config_gds_trace.prefix_b_before[0],
                    dreams_model_config_gds_trace.prefix_b_before[2],
                    dreams_model_config_gds_trace.prefix_b_before[17], prefix_b_after[0],
                    prefix_b_after[2], prefix_b_after[17]);
            }
        }
        if (cs.pgm_hash == 0xcca80e03 && source_base != 0) {
            static std::array<u32, 16> seen_transfer_sources{};
            static u32 seen_transfer_source_count{};
            const bool seen = std::find(seen_transfer_sources.begin(),
                                        seen_transfer_sources.begin() + seen_transfer_source_count,
                                        source_base) !=
                              seen_transfer_sources.begin() + seen_transfer_source_count;
            if (!seen && seen_transfer_source_count < seen_transfer_sources.size()) {
                seen_transfer_sources[seen_transfer_source_count++] = source_base;
                VAddr output_base{};
                u64 output_size{};
                if (!cs.buffers.empty() && !cs.buffers[0].IsSpecial()) {
                    const auto result = cs.buffers[0].GetSharp(cs);
                    output_base = result.base_address;
                    output_size = result.GetSize();
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams model transfer source discovered source_base={:#x} "
                            "sequence={} output={:#x}+{:#x}",
                            source_base, g_compute_dispatch_sequence, output_base, output_size);
                for (u32 back = 0; back < g_recent_compute_dispatches.size(); ++back) {
                    const u32 slot = (g_recent_compute_dispatch_index - 1 - back) &
                                     (g_recent_compute_dispatches.size() - 1);
                    const auto& recent = g_recent_compute_dispatches[slot];
                    LOG_WARNING(Render_Vulkan,
                                "Dreams model transfer history source={:#x} back={} hash={:#x} "
                                "dims={}x{}x{}",
                                source_base, back, recent.hash, recent.dim_x, recent.dim_y,
                                recent.dim_z);
                }
            }
        }
        const bool config_transfer = cs.pgm_hash == 0xcca80e03 && source_base == 0xb00;
        if (changed != 0 || config_transfer) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams model-config GDS shader={:#x} ordinal={} sequence={} "
                        "dims={}x{}x{} changed={} source_base={:#x}",
                        cs.pgm_hash, dreams_model_config_gds_trace.ordinal,
                        g_compute_dispatch_sequence, cs_program.dim_x, cs_program.dim_y,
                        cs_program.dim_z, changed, source_base);
            if (config_transfer) {
                static u32 model_config_history_count{};
                if (model_config_history_count++ < 3) {
                    for (u32 back = 0; back < g_recent_compute_dispatches.size(); ++back) {
                        const u32 slot =
                            (g_recent_compute_dispatch_index - 1 - back) &
                            (g_recent_compute_dispatches.size() - 1);
                        const auto& recent = g_recent_compute_dispatches[slot];
                        LOG_WARNING(Render_Vulkan,
                                    "Dreams model-config history back={} sequence={} hash={:#x} "
                                    "dims={}x{}x{}",
                                    back, g_compute_dispatch_sequence - back, recent.hash,
                                    recent.dim_x, recent.dim_y, recent.dim_z);
                    }
                }
            }
            for (u32 index = 0; index < after.size(); ++index) {
                if (!config_transfer &&
                    dreams_model_config_gds_trace.before[index] == after[index]) {
                    continue;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams model-config GDS index={} value={:#x}->{:#x}", 704 + index,
                            dreams_model_config_gds_trace.before[index], after[index]);
            }
        }
    }
    if (dreams_model_producer_trace.enabled) {
        scheduler.Finish();
        std::array<u32, Shader::DreamsCompat::ModelProducerTraceCount> counters{};
        std::array<u32, Shader::DreamsCompat::ModelProducerNumericCount> numeric{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        const u64 counter_offset =
            static_cast<u64>(Shader::DreamsCompat::ModelProducerTraceBaseDword) * sizeof(u32);
        const u64 numeric_offset =
            static_cast<u64>(Shader::DreamsCompat::ModelProducerNumericBaseDword) * sizeof(u32);
        std::memcpy(counters.data(), gds->mapped_data.data() + counter_offset, sizeof(counters));
        std::memcpy(numeric.data(), gds->mapped_data.data() + numeric_offset, sizeof(numeric));
        if (dreams_model_producer_trace.compact) {
            const u32 capacity_fail = counters[5] >= counters[6] ? counters[5] - counters[6] : 0;
            LOG_WARNING(
                Render_Vulkan,
                "Dreams model producer compact cycle={} ordinal={} sequence={} "
                "output={:#x}+{:#x} eligible={} class0={} class1={} class2={} class3={} "
                "final_class1={} stores={} capacity_fail={} index_min={} index_max={}",
                dreams_model_producer_trace.cycle, dreams_model_producer_trace.ordinal,
                g_compute_dispatch_sequence, dreams_model_producer_trace.output_base,
                dreams_model_producer_trace.output_size, counters[0], counters[1], counters[2],
                counters[3], counters[4], counters[5], counters[6], capacity_fail, counters[9],
                counters[10]);
            const u32 sample_count = std::min(counters[6], 16u);
            for (u32 sample = 0; sample < sample_count; ++sample) {
                LOG_WARNING(Render_Vulkan,
                            "Dreams model producer compact store cycle={} sample={} "
                            "index={} handle={:#x}",
                            dreams_model_producer_trace.cycle, sample, numeric[sample * 2],
                            numeric[sample * 2 + 1]);
            }
            for (u32 wave = 0; wave < 8; ++wave) {
                const u32 base = 32 + wave * 8;
                LOG_WARNING(Render_Vulkan,
                            "Dreams model producer compact wave cycle={} wave={} s33={} "
                            "flat21={} flat42={} flat43={} binary={} class={} index={} capacity={}",
                            dreams_model_producer_trace.cycle, wave, numeric[base],
                            numeric[base + 1], numeric[base + 2], numeric[base + 3],
                            numeric[base + 4], numeric[base + 5], numeric[base + 6],
                            numeric[base + 7]);
            }
        } else {
            LOG_WARNING(
                Render_Vulkan,
                "Dreams model producer post cycle={} ordinal={} sequence={} "
                "output={:#x}+{:#x} active={} zero_range={} nonzero_range={} underflow={} "
                "range_sum={} invalidates={} base_min={} base_max={} end_min={} end_max={}",
                dreams_model_producer_trace.cycle, dreams_model_producer_trace.ordinal,
                g_compute_dispatch_sequence, dreams_model_producer_trace.output_base,
                dreams_model_producer_trace.output_size, counters[16], counters[17], counters[18],
                counters[19], counters[20], counters[21], counters[22], counters[23], counters[24],
                counters[25]);
            const u32 sample_count = std::min(counters[16], 16u);
            for (u32 sample = 0; sample < sample_count; ++sample) {
                const u32 base = 96 + sample * 4;
                LOG_WARNING(Render_Vulkan,
                            "Dreams model producer post range cycle={} entry={} base={} end={} "
                            "length={} capacity={}",
                            dreams_model_producer_trace.cycle, sample, numeric[base],
                            numeric[base + 1], numeric[base + 2], numeric[base + 3]);
            }
        }
    }
    if (dreams_model_gds_trace.enabled) {
        scheduler.Finish();
        std::array<u32, 17> after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(after.data(), gds->mapped_data.data() + 768 * sizeof(u32), sizeof(after));
        u32 changed{};
        for (u32 index = 0; index < after.size(); ++index) {
            changed += after[index] != dreams_model_gds_trace.before[index];
        }
        LOG_WARNING(
            Render_Vulkan,
            "Dreams model GDS post shader={:#x} ordinal={} sequence={} changed={} "
            "GDS768-784={},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
            cs.pgm_hash, dreams_model_gds_trace.ordinal, g_compute_dispatch_sequence, changed,
            after[0], after[1], after[2], after[3], after[4], after[5], after[6], after[7],
            after[8], after[9], after[10], after[11], after[12], after[13], after[14], after[15],
            after[16]);
        if (cs.pgm_hash == 0xcca80e03 && !cs.buffers.empty() &&
            !cs.buffers[0].IsSpecial()) {
            const auto result = cs.buffers[0].GetSharp(cs);
            std::array<u32, 17> output{};
            const u64 read_size = std::min<u64>(result.GetSize(), sizeof(output)) & ~u64{3};
            if (result.base_address != 0 && read_size != 0 &&
                memory->IsValidMapping(result.base_address, read_size)) {
                buffer_cache.ReadMemory(result.base_address, read_size);
                std::memcpy(output.data(), std::bit_cast<const void*>(result.base_address),
                            read_size);
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams model transfer result ordinal={} base={:#x}+{:#x} "
                        "count={} min={},{},{} max={},{},{} progress={} flat21={:#x}",
                        dreams_model_gds_trace.ordinal, result.base_address, result.GetSize(),
                        output[10], output[11], output[12], output[13], output[14], output[15],
                        output[16], output[7],
                        cs.flattened_ud_buf.size() > 21 ? cs.flattened_ud_buf[21] : 0);
        }
    }
    if (model_finalize_gate_trace) {
        std::array<u32, Shader::DreamsCompat::ModelFinalizeGateTraceCount> counters{};
        std::array<u32, Shader::DreamsCompat::ModelFinalizeGateNumericCount> numeric{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        if (model_finalize_gate_trace_offset + sizeof(counters) <= gds->mapped_data.size()) {
            std::memcpy(counters.data(),
                        gds->mapped_data.data() + model_finalize_gate_trace_offset,
                        sizeof(counters));
        }
        if (model_finalize_gate_numeric_offset + sizeof(numeric) <= gds->mapped_data.size()) {
            std::memcpy(numeric.data(),
                        gds->mapped_data.data() + model_finalize_gate_numeric_offset,
                        sizeof(numeric));
        }
        LOG_WARNING(
            Render_Vulkan,
            "Dreams d049 gate ordinal={} sequence={} producer_cycle={} "
            "compact_sequence={} post_sequence={} reach={} zero_flags={} "
            "ticket_mismatch={} float_le={} iteration_zero={} or_gate={} final_and={} "
            "float_fail={} ticket_equal={} flags_nonzero={} or_fail={} final_fail={} "
            "value_nan={} threshold_nan={} threshold_negative={} value_negative={}",
            dreams_model_gds_trace.ordinal, g_compute_dispatch_sequence,
            dreams_model_producer_cycle, dreams_model_compact_sequence,
            dreams_model_post_sequence, counters[0], counters[1], counters[2], counters[3],
            counters[4], counters[5], counters[6],
            counters[7], counters[8], counters[9], counters[10], counters[11], counters[12],
            counters[13], counters[14], counters[15]);
        for (u32 workgroup = 0;
             workgroup < Shader::DreamsCompat::ModelFinalizeGateNumericWorkgroups;
             ++workgroup) {
            const u32 base = workgroup * Shader::DreamsCompat::ModelFinalizeGateNumericStride;
            if (numeric[base + 7] == 0) {
                continue;
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams d049 gate sample ordinal={} workgroup={} "
                        "v7={:#x} v16={:#x} state={:#x} ticket={:#x} "
                        "value={:#x} threshold={:#x} iteration={:#x}",
                        dreams_model_gds_trace.ordinal, workgroup, numeric[base],
                        numeric[base + 1], numeric[base + 2], numeric[base + 3],
                        numeric[base + 4], numeric[base + 5], numeric[base + 6]);
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

    struct DreamsVisibilityListDispatchTrace {
        bool enabled{};
        bool indexed_input{};
        u32 ordinal{};
        u32 count_index{};
        u32 counter_a_index{};
        u32 counter_b_index{};
        u32 active_records{};
        u32 sampled_records{};
        VAddr output_base{};
        u64 output_size{};
        u32 output_stride{};
        std::array<u32, 3> dims{};
        std::array<u32, 3> gds_before{};
        std::vector<u32> input_words;
        std::vector<u32> output_before;
    } visibility_list_trace{};
    constexpr u64 DreamsVisibilityListShader = 0x7aa925e9;
    constexpr u64 DreamsVisibilityListShaderAlt = 0x016b9f6a;
    const bool is_dreams_visibility_list =
        cs.pgm_hash == DreamsVisibilityListShader ||
        cs.pgm_hash == DreamsVisibilityListShaderAlt;
    if (is_dreams_visibility_list && ConsumeDreamsVisibilityListTraceRequest()) {
        visibility_list_trace.enabled = true;
        visibility_list_trace.indexed_input = cs.pgm_hash == DreamsVisibilityListShader;
        visibility_list_trace.ordinal = ++g_dreams_visibility_list_trace_ordinal;
        visibility_list_trace.count_index = visibility_list_trace.indexed_input ? 324 : 325;
        visibility_list_trace.counter_a_index = visibility_list_trace.indexed_input ? 351 : 353;
        visibility_list_trace.counter_b_index = visibility_list_trace.indexed_input ? 352 : 354;

        scheduler.Finish();
        buffer_cache.ReadMemory(args_address, sizeof(visibility_list_trace.dims));
        if (memory->IsValidMapping(args_address, sizeof(visibility_list_trace.dims))) {
            std::memcpy(visibility_list_trace.dims.data(),
                        std::bit_cast<const void*>(args_address),
                        sizeof(visibility_list_trace.dims));
        }

        const auto* gds = buffer_cache.GetGdsBuffer();
        const auto read_gds = [&](u32 index) {
            u32 value{};
            const u64 offset = static_cast<u64>(index) * sizeof(u32);
            if (offset + sizeof(value) <= gds->mapped_data.size()) {
                std::memcpy(&value, gds->mapped_data.data() + offset, sizeof(value));
            }
            return value;
        };
        visibility_list_trace.gds_before = {
            read_gds(visibility_list_trace.count_index),
            read_gds(visibility_list_trace.counter_a_index),
            read_gds(visibility_list_trace.counter_b_index),
        };
        visibility_list_trace.active_records =
            std::min(visibility_list_trace.gds_before[0], 1U << 20);
        constexpr u32 MaxSampleRecords = 1024;
        visibility_list_trace.sampled_records =
            std::min(visibility_list_trace.active_records, MaxSampleRecords);

        const auto read_buffer_words = [&](const auto& sharp, u64 wanted_words,
                                           std::vector<u32>& words) {
            const u64 word_count = std::min<u64>(wanted_words, sharp.GetSize() / sizeof(u32));
            const u64 read_size = word_count * sizeof(u32);
            if (sharp.base_address == 0 || read_size == 0 ||
                !memory->IsValidMapping(sharp.base_address, read_size)) {
                return;
            }
            buffer_cache.ReadMemory(sharp.base_address, read_size);
            words.resize(word_count);
            std::memcpy(words.data(), std::bit_cast<const void*>(sharp.base_address), read_size);
        };

        const auto log_recent_writers = [&](std::string_view role, VAddr base, u64 size) {
            if (!visibility_list_trace.indexed_input || visibility_list_trace.ordinal > 8) {
                return;
            }
            constexpr u32 MaxWriters = 8;
            u32 logged{};
            bool truncated{};
            for (auto writer = g_dreams_buffer_writers.rbegin();
                 writer != g_dreams_buffer_writers.rend(); ++writer) {
                if (!writer->declared_write ||
                    !OverlapsDreamsRange(base, size, writer->base, writer->size)) {
                    continue;
                }
                if (logged == MaxWriters) {
                    truncated = true;
                    break;
                }
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams visibility list writer #{} role={} match={} seq={} dispatch={} "
                    "kind={} shader={:#x} stage={} binding={} range={:#x}+{:#x} stride={} "
                    "source={:#x}",
                    visibility_list_trace.ordinal, role, logged, writer->sequence,
                    writer->dispatch_sequence, static_cast<u32>(writer->kind), writer->shader_hash,
                    static_cast<u32>(writer->stage), writer->binding_index, writer->base,
                    writer->size, writer->stride, writer->source);
                ++logged;
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams visibility list writer summary #{} role={} target={:#x}+{:#x} "
                        "logged={} truncated={} history={}",
                        visibility_list_trace.ordinal, role, base, size, logged, truncated,
                        g_dreams_buffer_writers.size());
        };

        if (!cs.buffers.empty() && !cs.buffers[0].IsSpecial()) {
            const auto output = cs.buffers[0].GetSharp(cs);
            visibility_list_trace.output_base = output.base_address;
            visibility_list_trace.output_size = output.GetSize();
            visibility_list_trace.output_stride = output.stride;
            read_buffer_words(output, static_cast<u64>(visibility_list_trace.sampled_records) * 2,
                              visibility_list_trace.output_before);
        }
        if (cs.buffers.size() > 1 && !cs.buffers[1].IsSpecial()) {
            const auto input = cs.buffers[1].GetSharp(cs);
            const u64 input_words =
                static_cast<u64>(visibility_list_trace.sampled_records) *
                (visibility_list_trace.indexed_input ? 1 : 2);
            read_buffer_words(input, input_words, visibility_list_trace.input_words);
            const u64 active_input_size =
                std::min<u64>(input.GetSize(),
                              static_cast<u64>(visibility_list_trace.active_records) *
                                  (visibility_list_trace.indexed_input ? sizeof(u32)
                                                                       : 2 * sizeof(u32)));
            log_recent_writers("candidate", input.base_address, active_input_size);
            LOG_WARNING(
                Render_Vulkan,
                "Dreams visibility list input #{} binding=1 base={:#x}+{:#x} stride={} "
                "records={} sampled={} hash={:#x}",
                visibility_list_trace.ordinal, input.base_address, input.GetSize(), input.stride,
                input.num_records, visibility_list_trace.sampled_records,
                HashDreamsTraceWords(visibility_list_trace.input_words));
        }

        u32 invalid_source_refs{};
        std::vector<u32> source_sample_words;
        if (visibility_list_trace.indexed_input && cs.buffers.size() > 2 &&
            !cs.buffers[2].IsSpecial()) {
            const auto source = cs.buffers[2].GetSharp(cs);
            log_recent_writers("source", source.base_address, source.GetSize());
            const u64 source_records = source.GetSize() / (2 * sizeof(u32));
            for (u32 record = 0; record < visibility_list_trace.input_words.size(); ++record) {
                const u32 selector = visibility_list_trace.input_words[record];
                invalid_source_refs += (selector >> 10) >= source_records;
            }
            const u32 records_to_log =
                std::min<u32>(visibility_list_trace.input_words.size(), 8);
            for (u32 record = 0; record < records_to_log; ++record) {
                const u32 selector = visibility_list_trace.input_words[record];
                const u32 source_index = selector >> 10;
                std::array<u32, 2> values{};
                bool valid = source_index < source_records;
                const VAddr source_address =
                    source.base_address + static_cast<u64>(source_index) * 2 * sizeof(u32);
                valid = valid && memory->IsValidMapping(source_address, sizeof(values));
                if (valid) {
                    buffer_cache.ReadMemory(source_address, sizeof(values));
                    std::memcpy(values.data(), std::bit_cast<const void*>(source_address),
                                sizeof(values));
                    source_sample_words.insert(source_sample_words.end(), values.begin(),
                                               values.end());
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams visibility list candidate #{}[{}] selector={:#x} "
                            "source_index={} valid={} values={:#x},{:#x} flags={}",
                            visibility_list_trace.ordinal, record, selector, source_index, valid,
                            values[0], values[1], (values[1] >> 16) & 3);
            }
            LOG_WARNING(
                Render_Vulkan,
                "Dreams visibility list source #{} binding=2 base={:#x}+{:#x} stride={} "
                "records={} checked={} invalid={} sampled_hash={:#x}",
                visibility_list_trace.ordinal, source.base_address, source.GetSize(),
                source.stride, source.num_records, visibility_list_trace.input_words.size(),
                invalid_source_refs, HashDreamsTraceWords(source_sample_words));
        } else {
            const u32 records_to_log = std::min<u32>(
                static_cast<u32>(visibility_list_trace.input_words.size() / 2), 8);
            for (u32 record = 0; record < records_to_log; ++record) {
                const u32 value0 = visibility_list_trace.input_words[record * 2];
                const u32 value1 = visibility_list_trace.input_words[record * 2 + 1];
                LOG_WARNING(Render_Vulkan,
                            "Dreams visibility list candidate #{}[{}] values={:#x},{:#x} "
                            "flags={}",
                            visibility_list_trace.ordinal, record, value0, value1,
                            (value1 >> 16) & 3);
            }
        }

        LOG_WARNING(
            Render_Vulkan,
            "Dreams visibility list pre #{} sequence={} shader={:#x} args={:#x} "
            "dims={}x{}x{} active={} sampled={} truncated={} gds_count[{}]={} "
            "gds_a[{}]={} gds_b[{}]={} output={:#x}+{:#x} stride={} "
            "output_pre_hash={:#x} remaining={}",
            visibility_list_trace.ordinal, g_compute_dispatch_sequence, cs.pgm_hash,
            args_address, visibility_list_trace.dims[0], visibility_list_trace.dims[1],
            visibility_list_trace.dims[2], visibility_list_trace.active_records,
            visibility_list_trace.sampled_records,
            visibility_list_trace.active_records > visibility_list_trace.sampled_records,
            visibility_list_trace.count_index, visibility_list_trace.gds_before[0],
            visibility_list_trace.counter_a_index, visibility_list_trace.gds_before[1],
            visibility_list_trace.counter_b_index, visibility_list_trace.gds_before[2],
            visibility_list_trace.output_base, visibility_list_trace.output_size,
            visibility_list_trace.output_stride,
            HashDreamsTraceWords(visibility_list_trace.output_before),
            g_dreams_visibility_list_trace_remaining);
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
    const bool ordered_dreams_visibility_list = OrderDreamsVisibilityList(cs.pgm_hash);
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
        (ordered_dreams_traversal && OrderDreamsIndirectTraversal()) ||
        ordered_dreams_visibility_list;
    if (inspect_ordered_dims) {
        scheduler.Finish();
    }
    if (inspect_ordered_dims) {
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
    if (ordered_dreams_visibility_list) {
        static u32 activation_log_count{};
        if (activation_log_count < 32) {
            ++activation_log_count;
            LOG_WARNING(Render_Vulkan,
                        "Dreams ordered visibility-list activation #{} hash={:#x} args={:#x} "
                        "mapped={} dims={}x{}x{} batch=1",
                        activation_log_count, cs.pgm_hash, args_address, have_ordered_dims,
                        ordered_dims[0], ordered_dims[1], ordered_dims[2]);
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

    struct DreamsModelConfigIndirectGdsTrace {
        bool enabled{};
        u32 ordinal{};
        u32 inner_counter_before{};
        std::array<u32, 7> prefix_a_before{};
        std::array<u32, 18> prefix_b_before{};
        std::array<u32, 3> dims{};
        std::array<u32, 25> before{};
    } dreams_model_config_gds_trace{};
    if (TraceDreamsModelRecord() &&
        std::ranges::any_of(cs.buffers, [](const auto& desc) {
            return desc.buffer_type == Shader::BufferType::GdsBuffer;
        })) {
        static u32 model_config_indirect_gds_ordinal{};
        if (model_config_indirect_gds_ordinal < 8192) {
            scheduler.Finish();
            const auto* gds = buffer_cache.GetGdsBuffer();
            constexpr u64 ModelConfigGdsOffset = 704 * sizeof(u32);
            if (gds->mapped_data.size() >=
                ModelConfigGdsOffset + sizeof(dreams_model_config_gds_trace.before)) {
                dreams_model_config_gds_trace.enabled = true;
                dreams_model_config_gds_trace.ordinal = ++model_config_indirect_gds_ordinal;
                std::memcpy(&dreams_model_config_gds_trace.inner_counter_before,
                            gds->mapped_data.data() + 64 * sizeof(u32), sizeof(u32));
                std::memcpy(dreams_model_config_gds_trace.prefix_a_before.data(),
                            gds->mapped_data.data() + 196 * sizeof(u32),
                            sizeof(dreams_model_config_gds_trace.prefix_a_before));
                std::memcpy(dreams_model_config_gds_trace.prefix_b_before.data(),
                            gds->mapped_data.data() + 389 * sizeof(u32),
                            sizeof(dreams_model_config_gds_trace.prefix_b_before));
                std::memcpy(dreams_model_config_gds_trace.before.data(),
                            gds->mapped_data.data() + ModelConfigGdsOffset,
                            sizeof(dreams_model_config_gds_trace.before));
            }
        }
    }

    const bool dreams_model_prefix_indirect =
        cs.pgm_hash == 0xe165304b || cs.pgm_hash == 0x9f3303a7 ||
        cs.pgm_hash == 0xe9d99694 || cs.pgm_hash == 0xd2d439f3;
    if (dreams_model_config_gds_trace.enabled && dreams_model_prefix_indirect) {
        buffer_cache.ReadMemory(args_address, sizeof(dreams_model_config_gds_trace.dims));
        if (memory->IsValidMapping(args_address, sizeof(dreams_model_config_gds_trace.dims))) {
            std::memcpy(dreams_model_config_gds_trace.dims.data(),
                        std::bit_cast<const void*>(args_address),
                        sizeof(dreams_model_config_gds_trace.dims));
        }

        static std::vector<std::pair<u64, VAddr>> logged_model_args_writers;
        const std::pair writer_key{cs.pgm_hash, args_address};
        if (std::ranges::find(logged_model_args_writers, writer_key) ==
            logged_model_args_writers.end()) {
            logged_model_args_writers.push_back(writer_key);
            const u64 args_end = args_address + sizeof(dreams_model_config_gds_trace.dims);
            u32 matches{};
            for (auto writer = g_dreams_buffer_writers.rbegin();
                 writer != g_dreams_buffer_writers.rend() && matches < 32; ++writer) {
                const u64 writer_end = writer->base + writer->size;
                if (!writer->declared_write || writer->dispatch_sequence >= g_compute_dispatch_sequence ||
                    writer->base >= args_end || args_address >= writer_end) {
                    continue;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams model args writer consumer={:#x} args={:#x} match={} "
                            "writer_seq={} dispatch={} kind={} shader={:#x} binding={} "
                            "range={:#x}+{:#x} stride={} source={:#x}",
                            cs.pgm_hash, args_address, matches++, writer->sequence,
                            writer->dispatch_sequence, static_cast<u32>(writer->kind),
                            writer->shader_hash, writer->binding_index, writer->base, writer->size,
                            writer->stride, writer->source);
            }
            LOG_WARNING(Render_Vulkan,
                        "Dreams model args writer summary consumer={:#x} args={:#x} "
                        "matches={} history={}",
                        cs.pgm_hash, args_address, matches, g_dreams_buffer_writers.size());
        }
    }

    const bool stabilize_compute_indirect_buffers = StabilizeDreamsComputeIndirectBuffers();
    if (stabilize_compute_indirect_buffers) {
        // The indirect-argument lookup can grow a cached buffer and retire shader-buffer handles
        // selected by BindResources. Preflight that range, refresh shader descriptors and their
        // barriers against the settled union, then obtain the final indirect handle below.
        if (size != 0) {
            buffer_cache.FindBuffer(args_address, size);
        }
        const u32 changed_handles =
            RefreshBufferResources(pipeline, true, "compute indirect");
        static u32 dreams_compute_indirect_stabilize_log_count{};
        if ((changed_handles != 0 || dreams_compute_indirect_stabilize_log_count == 0) &&
            dreams_compute_indirect_stabilize_log_count++ < 64) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams compute indirect stabilization hash={:#x} changed_handles={}",
                        cs.pgm_hash, changed_handles);
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
    if (stabilize_compute_indirect_buffers) {
        ApplyDreamsComputeIndirectBufferBarrier(scheduler);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    const auto profile_start = std::chrono::steady_clock::now();
    if (have_ordered_dims) {
        DispatchDreamsTraversalOrdered(cmdbuf, ordered_dims[0], ordered_dims[1], ordered_dims[2],
                                       ordered_dreams_visibility_list ? 1U : 0U);
    } else {
        cmdbuf.dispatchIndirect(buffer->Handle(), base);
    }
    if (visibility_list_trace.enabled) {
        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        const auto read_gds = [&](u32 index) {
            u32 value{};
            const u64 byte_offset = static_cast<u64>(index) * sizeof(u32);
            if (byte_offset + sizeof(value) <= gds->mapped_data.size()) {
                std::memcpy(&value, gds->mapped_data.data() + byte_offset, sizeof(value));
            }
            return value;
        };
        const std::array<u32, 3> gds_after{
            read_gds(visibility_list_trace.count_index),
            read_gds(visibility_list_trace.counter_a_index),
            read_gds(visibility_list_trace.counter_b_index),
        };

        std::vector<u32> output_after;
        const u64 wanted_words = static_cast<u64>(visibility_list_trace.sampled_records) * 2;
        const u64 available_words = visibility_list_trace.output_size / sizeof(u32);
        const u64 word_count = std::min(wanted_words, available_words);
        const u64 read_size = word_count * sizeof(u32);
        if (visibility_list_trace.output_base != 0 && read_size != 0 &&
            memory->IsValidMapping(visibility_list_trace.output_base, read_size)) {
            buffer_cache.ReadMemory(visibility_list_trace.output_base, read_size);
            output_after.resize(word_count);
            std::memcpy(output_after.data(),
                        std::bit_cast<const void*>(visibility_list_trace.output_base), read_size);
        }

        const u32 comparable_records = std::min<u32>(
            visibility_list_trace.output_before.size() / 2, output_after.size() / 2);
        u32 changed_records{};
        u32 first_changed = std::numeric_limits<u32>::max();
        std::array<u32, 4> flag_counts{};
        u32 max_low24_id{};
        for (u32 record = 0; record < output_after.size() / 2; ++record) {
            const u32 value0 = output_after[record * 2];
            const u32 value1 = output_after[record * 2 + 1];
            ++flag_counts[(value1 >> 16) & 3];
            max_low24_id = std::max(max_low24_id, value0 & 0x00ffffff);
            if (record >= comparable_records ||
                visibility_list_trace.output_before[record * 2] != value0 ||
                visibility_list_trace.output_before[record * 2 + 1] != value1) {
                ++changed_records;
                first_changed = std::min(first_changed, record);
            }
        }
        const u32 delta_a =
            gds_after[1] >= visibility_list_trace.gds_before[1]
                ? gds_after[1] - visibility_list_trace.gds_before[1]
                : 0;
        const u32 delta_b =
            gds_after[2] >= visibility_list_trace.gds_before[2]
                ? gds_after[2] - visibility_list_trace.gds_before[2]
                : 0;
        const u64 output_hash = HashDreamsTraceWords(output_after);
        LOG_WARNING(
            Render_Vulkan,
            "Dreams visibility list post #{} sequence={} shader={:#x} active={} sampled={} "
            "gds_count[{}]={}->{} gds_a[{}]={}->{} delta={} reset={} "
            "gds_b[{}]={}->{} delta={} reset={} changed={} first_changed={} "
            "hash={:#x} flags0-3={},{},{},{} max_low24_id={}",
            visibility_list_trace.ordinal, g_compute_dispatch_sequence, cs.pgm_hash,
            visibility_list_trace.active_records, visibility_list_trace.sampled_records,
            visibility_list_trace.count_index, visibility_list_trace.gds_before[0], gds_after[0],
            visibility_list_trace.counter_a_index, visibility_list_trace.gds_before[1],
            gds_after[1], delta_a, gds_after[1] < visibility_list_trace.gds_before[1],
            visibility_list_trace.counter_b_index, visibility_list_trace.gds_before[2],
            gds_after[2], delta_b, gds_after[2] < visibility_list_trace.gds_before[2],
            changed_records, first_changed, output_hash, flag_counts[0], flag_counts[1],
            flag_counts[2], flag_counts[3], max_low24_id);
        const u32 records_to_log = std::min<u32>(output_after.size() / 2, 8);
        for (u32 record = 0; record < records_to_log; ++record) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams visibility list output #{}[{}] values={:#x},{:#x} "
                        "low24_id={} flags={}",
                        visibility_list_trace.ordinal, record, output_after[record * 2],
                        output_after[record * 2 + 1], output_after[record * 2] & 0x00ffffff,
                        (output_after[record * 2 + 1] >> 16) & 3);
        }

        if (g_dreams_visibility_list_consumer_snapshot.valid) {
            LOG_WARNING(
                Render_Vulkan,
                "Dreams visibility list consumer snapshot overwritten old=#{} sequence={} "
                "shader={:#x} output={:#x}",
                g_dreams_visibility_list_consumer_snapshot.ordinal,
                g_dreams_visibility_list_consumer_snapshot.dispatch_sequence,
                g_dreams_visibility_list_consumer_snapshot.shader_hash,
                g_dreams_visibility_list_consumer_snapshot.output_base);
        }
        g_dreams_visibility_list_consumer_snapshot = {
            .valid = true,
            .ordinal = visibility_list_trace.ordinal,
            .shader_hash = cs.pgm_hash,
            .dispatch_sequence = g_compute_dispatch_sequence,
            .output_base = visibility_list_trace.output_base,
            .active_records = visibility_list_trace.active_records,
            .sampled_records = static_cast<u32>(output_after.size() / 2),
            .output_hash = output_hash,
            .words = std::move(output_after),
        };
    }
    if (dreams_model_config_gds_trace.enabled) {
        scheduler.Finish();
        std::array<u32, 25> after{};
        const auto* gds = buffer_cache.GetGdsBuffer();
        std::memcpy(after.data(), gds->mapped_data.data() + 704 * sizeof(u32), sizeof(after));
        u32 changed{};
        for (u32 index = 0; index < after.size(); ++index) {
            changed += after[index] != dreams_model_config_gds_trace.before[index];
        }
        if (changed != 0) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams model-config indirect GDS shader={:#x} ordinal={} sequence={} "
                        "args={:#x} changed={}",
                        cs.pgm_hash, dreams_model_config_gds_trace.ordinal,
                        g_compute_dispatch_sequence, args_address, changed);
            for (u32 index = 0; index < after.size(); ++index) {
                if (dreams_model_config_gds_trace.before[index] == after[index]) {
                    continue;
                }
                LOG_WARNING(Render_Vulkan,
                            "Dreams model-config indirect GDS index={} value={:#x}->{:#x}",
                            704 + index, dreams_model_config_gds_trace.before[index], after[index]);
            }
        }
        if (dreams_model_prefix_indirect) {
            static u32 prefix_shader_indirect_count{};
            if (prefix_shader_indirect_count++ < 256) {
                u32 inner_counter_after{};
                std::array<u32, 7> prefix_a_after{};
                std::array<u32, 18> prefix_b_after{};
                std::memcpy(&inner_counter_after,
                            gds->mapped_data.data() + 64 * sizeof(u32), sizeof(u32));
                std::memcpy(prefix_a_after.data(),
                            gds->mapped_data.data() + 196 * sizeof(u32),
                            sizeof(prefix_a_after));
                std::memcpy(prefix_b_after.data(),
                            gds->mapped_data.data() + 389 * sizeof(u32),
                            sizeof(prefix_b_after));
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams model prefix indirect shader={:#x} sequence={} args={:#x} "
                    "dims={}x{}x{} GDS64={}->{} A196-202={},{},{},{},{},{},{} -> "
                    "{},{},{},{},{},{},{} B389,391,406={},{},{} -> {},{},{}",
                    cs.pgm_hash, g_compute_dispatch_sequence, args_address,
                    dreams_model_config_gds_trace.dims[0],
                    dreams_model_config_gds_trace.dims[1],
                    dreams_model_config_gds_trace.dims[2],
                    dreams_model_config_gds_trace.inner_counter_before, inner_counter_after,
                    dreams_model_config_gds_trace.prefix_a_before[0],
                    dreams_model_config_gds_trace.prefix_a_before[1],
                    dreams_model_config_gds_trace.prefix_a_before[2],
                    dreams_model_config_gds_trace.prefix_a_before[3],
                    dreams_model_config_gds_trace.prefix_a_before[4],
                    dreams_model_config_gds_trace.prefix_a_before[5],
                    dreams_model_config_gds_trace.prefix_a_before[6], prefix_a_after[0],
                    prefix_a_after[1], prefix_a_after[2], prefix_a_after[3], prefix_a_after[4],
                    prefix_a_after[5], prefix_a_after[6],
                    dreams_model_config_gds_trace.prefix_b_before[0],
                    dreams_model_config_gds_trace.prefix_b_before[2],
                    dreams_model_config_gds_trace.prefix_b_before[17], prefix_b_after[0],
                    prefix_b_after[2], prefix_b_after[17]);
            }
        }
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

    RefreshBufferResources(pipeline);
    return true;
}

u32 Rasterizer::RefreshBufferResources(const Pipeline* pipeline, bool trace_handle_changes,
                                       std::string_view trace_role) {
    // Finding or refreshing another resource can grow a cached buffer and retire every
    // buffer it overlaps. Resolve the complete shader-buffer union again, then update the existing
    // descriptor infos in place. Descriptor write order and binding numbers remain unchanged.
    struct BufferRefreshBinding {
        const Shader::Info* stage;
        u32 descriptor_index;
        u32 buffer_info_index;
        u32 push_offset_index;
        VideoCore::BufferId buffer_id;
        AmdGpu::Buffer sharp;
        u64 size;
    };
    boost::container::static_vector<BufferRefreshBinding, Shader::NUM_BUFFERS> refresh_bindings;
    boost::container::static_vector<vk::Buffer, Shader::NUM_BUFFERS> retired_buffer_handles;
    const size_t old_barrier_count = buffer_barriers.size();
    u32 changed_handle_count{};
    u32 logged_handle_count{};
    u32 buffer_info_index{};
    u32 push_offset_index{};
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (u32 descriptor_index = 0; descriptor_index < stage->buffers.size();
             ++descriptor_index, ++buffer_info_index, ++push_offset_index) {
            const auto& desc = stage->buffers[descriptor_index];
            const auto sharp = desc.GetSharp(*stage);
            if (desc.IsSpecial() || sharp.base_address == 0 || sharp.GetSize() == 0) {
                continue;
            }
            u64 size = memory->ClampRangeSize(sharp.base_address, sharp.GetSize());
            if (sharp.stride == 0 &&
                sharp.num_records == std::numeric_limits<u32>::max() &&
                size > MaxUnboundedGuestBufferWindow) {
                size = MaxUnboundedGuestBufferWindow;
            }
            refresh_bindings.push_back({
                .stage = stage,
                .descriptor_index = descriptor_index,
                .buffer_info_index = buffer_info_index,
                .push_offset_index = push_offset_index,
                .buffer_id = {},
                .sharp = sharp,
                .size = size,
            });
        }
    }

    // Pre-find every range before obtaining any of them. A later FindBuffer may join and retire
    // an earlier result; ObtainBuffer detects that, matching BindBuffers' existing two-pass rule.
    for (auto& refresh : refresh_bindings) {
        refresh.buffer_id = buffer_cache.FindBuffer(refresh.sharp.base_address, refresh.size);
    }
    for (const auto& refresh : refresh_bindings) {
        const auto& desc = refresh.stage->buffers[refresh.descriptor_index];
        const bool is_storage = desc.IsStorage(refresh.sharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        const auto old_info = buffer_infos[refresh.buffer_info_index];
        const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
            refresh.sharp.base_address, refresh.size, desc.is_written, desc.is_formatted,
            refresh.buffer_id);
        const u32 offset_aligned = Common::AlignDown(offset, alignment);
        const u32 adjust = offset - offset_aligned;
        ASSERT(adjust % 4 == 0);
        push_data.AddOffset(refresh.push_offset_index, adjust);
        auto& buffer_info = buffer_infos[refresh.buffer_info_index];
        buffer_info = vk::DescriptorBufferInfo{
            vk_buffer->Handle(), offset_aligned, refresh.size + adjust};
        const bool descriptor_changed = old_info.buffer != buffer_info.buffer ||
                                        old_info.offset != buffer_info.offset ||
                                        old_info.range != buffer_info.range;
        const bool handle_changed = old_info.buffer != buffer_info.buffer;
        changed_handle_count += handle_changed;
        static u32 dreams_graphics_changed_handle_log_count{};
        static u32 dreams_compute_indirect_changed_handle_log_count{};
        u32& dreams_changed_handle_log_count =
            trace_role == "compute indirect" ? dreams_compute_indirect_changed_handle_log_count
                                             : dreams_graphics_changed_handle_log_count;
        if (trace_handle_changes && handle_changed && logged_handle_count < 4 &&
            dreams_changed_handle_log_count++ < 64) {
            ++logged_handle_count;
            LOG_WARNING(
                Render_Vulkan,
                "Dreams {} stabilization shader={:#x} descriptor={} old_vk={:#x} "
                "new_vk={:#x} guest={:#x}+{:#x}",
                trace_role, refresh.stage->pgm_hash, refresh.descriptor_index,
                reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(old_info.buffer)),
                reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(buffer_info.buffer)),
                refresh.sharp.base_address, refresh.size);
        }
        if (old_info.buffer && old_info.buffer != buffer_info.buffer &&
            std::ranges::find(retired_buffer_handles, old_info.buffer) ==
                retired_buffer_handles.end()) {
            retired_buffer_handles.push_back(old_info.buffer);
        }

        const vk::AccessFlags2 access_mask =
            desc.is_written
                ? vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
                : vk::AccessFlagBits2::eShaderRead;
        const bool force_dependency = desc.is_written && ForceDreamsStorageDependencies();
        if (auto barrier = vk_buffer->GetBarrier(
                access_mask, vk::PipelineStageFlagBits2::eAllCommands, 0, force_dependency)) {
            buffer_barriers.emplace_back(*barrier);
        }

        const char* gather_binding_value =
            std::getenv("SHADPS4_DREAMS_GATHER_BINDING_TRACE");
        static u32 gather_final_binding_trace_count{};
        if (refresh.stage->pgm_hash == Shader::DreamsCompat::GatherVoxelsShader &&
            (refresh.descriptor_index == 2 || refresh.descriptor_index == 14) &&
            gather_binding_value != nullptr &&
            std::string_view{gather_binding_value} == "1" &&
            gather_final_binding_trace_count++ < 16) {
            LOG_WARNING(
                Render_Vulkan,
                "Dreams Gather final binding trace={} desc_index={} input_id={} "
                "guest={:#x}+{:#x} cache_guest={:#x}+{:#x} old_vk={:#x} "
                "old_offset={:#x} old_range={:#x} final_vk={:#x} final_offset={:#x} "
                "final_range={:#x} adjust={:#x} push_offset={} changed={}",
                gather_final_binding_trace_count, refresh.descriptor_index,
                refresh.buffer_id.index, refresh.sharp.base_address, refresh.size,
                vk_buffer->CpuAddr(), vk_buffer->SizeBytes(),
                reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(old_info.buffer)),
                static_cast<u64>(old_info.offset), static_cast<u64>(old_info.range),
                reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(buffer_info.buffer)),
                static_cast<u64>(buffer_info.offset), static_cast<u64>(buffer_info.range), adjust,
                push_data.buf_offsets[refresh.push_offset_index], descriptor_changed);
        }
    }

    // A cache join can retire a buffer after its initial barrier was collected. Remove barriers
    // for replaced handles from the original prefix; the final barriers appended above must stay.
    for (size_t barrier_index = old_barrier_count; barrier_index != 0; --barrier_index) {
        const auto buffer = buffer_barriers[barrier_index - 1].buffer;
        const bool was_replaced =
            std::ranges::find(retired_buffer_handles, buffer) != retired_buffer_handles.end();
        const bool is_still_bound = std::ranges::any_of(
            buffer_infos, [buffer](const auto& info) { return info.buffer == buffer; });
        if (was_replaced && !is_still_bound) {
            buffer_barriers.erase(buffer_barriers.begin() + (barrier_index - 1));
        }
    }

    return changed_handle_count;
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
            const char* gather_binding_value =
                std::getenv("SHADPS4_DREAMS_GATHER_BINDING_TRACE");
            static u32 gather_binding_trace_count{};
            if (stage.pgm_hash == Shader::DreamsCompat::GatherVoxelsShader &&
                (i == 2 || i == 14) &&
                gather_binding_value != nullptr &&
                std::string_view{gather_binding_value} == "1" &&
                gather_binding_trace_count++ < 16) {
                u32 guest_head{};
                const bool guest_head_mapped =
                    memory->IsValidMapping(vsharp.base_address, sizeof(guest_head));
                if (guest_head_mapped) {
                    std::memcpy(&guest_head, std::bit_cast<const void*>(vsharp.base_address),
                                sizeof(guest_head));
                }
                const auto& buffer_info = buffer_infos.back();
                LOG_WARNING(
                    Render_Vulkan,
                    "Dreams Gather binding trace={} desc_index={} sharp_index={} "
                    "buffer_binding={} unified_binding={} input_id={} guest={:#x}+{:#x} "
                    "stride={} records={} write={} formatted={} cpu_modified={} "
                    "gpu_modified={} guest_head_mapped={} guest_head={:#x} "
                    "cache_guest={:#x}+{:#x} vk={:#x} obtain_offset={:#x} alignment={} "
                    "aligned={:#x} adjust={:#x} descriptor_vk={:#x} "
                    "descriptor_offset={:#x} descriptor_range={:#x} push_offset={}",
                    gather_binding_trace_count, i, desc.sharp_idx, binding.buffer,
                    binding.unified, buffer_id.index, vsharp.base_address, size, vsharp.stride,
                    vsharp.num_records, desc.is_written, desc.is_formatted,
                    buffer_cache.IsRegionCpuModified(vsharp.base_address, size),
                    buffer_cache.IsRegionGpuModified(vsharp.base_address, size),
                    guest_head_mapped, guest_head, vk_buffer->CpuAddr(), vk_buffer->SizeBytes(),
                    reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(vk_buffer->Handle())),
                    offset, alignment, offset_aligned, adjust,
                    reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(buffer_info.buffer)),
                    static_cast<u64>(buffer_info.offset), static_cast<u64>(buffer_info.range),
                    push_data.buf_offsets[binding.buffer]);
            }
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
    const u64 dreams_image_writer_sequence_before = g_dreams_image_writer_sequence;
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
                                  (image.binding.has_storage
                                       ? vk::AccessFlags2{vk::AccessFlagBits2::eShaderWrite}
                                       : vk::AccessFlags2{}) |
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

    if (stage.pgm_hash == Shader::DreamsCompat::TemporalResolveShader &&
        ConsumeDreamsTemporalImageTraceRequest()) {
        struct TemporalImageEntry {
            bool valid{};
            bool written{};
            u32 binding{};
            u32 array_element{};
            u32 image_id{};
            u64 image_uid{};
            VAddr descriptor_base{};
            VAddr guest_base{};
            u64 guest_size{};
            u32 descriptor_width{};
            u32 descriptor_height{};
            u32 descriptor_pitch{};
            u32 cached_width{};
            u32 cached_height{};
            u32 cached_pitch{};
            u32 view_format{};
            u32 base_level{};
            u32 levels{};
            u32 base_layer{};
            u32 layers{};
        };
        struct TemporalDispatch {
            u32 ordinal{};
            u64 sequence{};
            std::vector<TemporalImageEntry> images;
        };
        static std::deque<TemporalDispatch> history;

        const u32 ordinal = ++g_dreams_temporal_image_trace_ordinal;
        std::vector<TemporalImageEntry> current;
        current.reserve(image_bindings.size());
        u32 flattened_index{};
        for (u32 stage_binding = 0; stage_binding < stage.images.size(); ++stage_binding) {
            const auto& image_desc = stage.images[stage_binding];
            const auto tsharp = image_desc.GetSharp(stage);
            const u32 array_size = image_descriptor_array_sizes[stage_binding];
            for (u32 array_element = 0; array_element < array_size;
                 ++array_element, ++flattened_index) {
                TemporalImageEntry entry{
                    .written = image_desc.is_written,
                    .binding = stage_binding,
                    .array_element = array_element,
                    .descriptor_base = tsharp.Address(),
                    .descriptor_width = static_cast<u32>(tsharp.width) + 1,
                    .descriptor_height = static_cast<u32>(tsharp.height) + 1,
                    .descriptor_pitch = tsharp.Pitch(),
                };
                if (flattened_index < image_bindings.size()) {
                    const auto& [image_id, desc] = image_bindings[flattened_index];
                    entry.view_format = static_cast<u32>(desc.view_info.format);
                    entry.base_level = desc.view_info.range.base.level;
                    entry.levels = desc.view_info.range.extent.levels;
                    entry.base_layer = desc.view_info.range.base.layer;
                    entry.layers = desc.view_info.range.extent.layers;
                    if (image_id) {
                        const auto& image = texture_cache.GetImage(image_id);
                        entry.valid = true;
                        entry.image_id = image_id.index;
                        entry.image_uid = image.image_uid;
                        entry.guest_base = image.info.guest_address;
                        entry.guest_size = image.info.guest_size;
                        entry.cached_width = image.info.size.width;
                        entry.cached_height = image.info.size.height;
                        entry.cached_pitch = image.info.pitch;
                    }
                }
                current.push_back(entry);
            }
        }

        const auto same_descriptor = [](const TemporalImageEntry& lhs,
                                        const TemporalImageEntry& rhs) {
            return lhs.valid && rhs.valid && lhs.descriptor_base == rhs.descriptor_base &&
                   lhs.descriptor_width == rhs.descriptor_width &&
                   lhs.descriptor_height == rhs.descriptor_height &&
                   lhs.descriptor_pitch == rhs.descriptor_pitch &&
                   lhs.view_format == rhs.view_format && lhs.base_level == rhs.base_level &&
                   lhs.levels == rhs.levels && lhs.base_layer == rhs.base_layer &&
                   lhs.layers == rhs.layers;
        };
        const auto same_cached_view = [](const TemporalImageEntry& lhs,
                                         const TemporalImageEntry& rhs) {
            return lhs.valid && rhs.valid && lhs.image_uid == rhs.image_uid &&
                   lhs.view_format == rhs.view_format && lhs.base_level == rhs.base_level &&
                   lhs.levels == rhs.levels && lhs.base_layer == rhs.base_layer &&
                   lhs.layers == rhs.layers;
        };

        const auto& cs_program = liverpool->GetCsRegs();
        LOG_WARNING(Render_Vulkan,
                    "Dreams temporal images #{} dispatch={} dims={}x{}x{} bindings={} "
                    "history={}",
                    ordinal, g_compute_dispatch_sequence, cs_program.dim_x, cs_program.dim_y,
                    cs_program.dim_z, current.size(), history.size());

        for (const auto& entry : current) {
            u32 descriptor_source_ordinal{};
            u64 descriptor_source_sequence{};
            u32 uid_source_ordinal{};
            u64 uid_source_sequence{};
            u32 overlap_source_ordinal{};
            u64 overlap_source_sequence{};
            if (!entry.written) {
                for (auto dispatch = history.rbegin(); dispatch != history.rend(); ++dispatch) {
                    for (const auto& prior : dispatch->images) {
                        if (!prior.written) {
                            continue;
                        }
                        if (descriptor_source_ordinal == 0 && same_descriptor(prior, entry)) {
                            descriptor_source_ordinal = dispatch->ordinal;
                            descriptor_source_sequence = dispatch->sequence;
                        }
                        if (uid_source_ordinal == 0 && same_cached_view(prior, entry)) {
                            uid_source_ordinal = dispatch->ordinal;
                            uid_source_sequence = dispatch->sequence;
                        }
                        if (overlap_source_ordinal == 0 &&
                            OverlapsDreamsRange(prior.guest_base, prior.guest_size,
                                               entry.guest_base, entry.guest_size)) {
                            overlap_source_ordinal = dispatch->ordinal;
                            overlap_source_sequence = dispatch->sequence;
                        }
                    }
                }
            }

            const DreamsImageWriter* latest_writer{};
            if (!entry.written) {
                for (auto writer = g_dreams_image_writers.rbegin();
                     writer != g_dreams_image_writers.rend(); ++writer) {
                    if (writer->sequence > dreams_image_writer_sequence_before ||
                        !OverlapsDreamsRange(writer->base, writer->size, entry.guest_base,
                                            entry.guest_size)) {
                        continue;
                    }
                    latest_writer = &*writer;
                    break;
                }
            }

            LOG_WARNING(
                Render_Vulkan,
                "Dreams temporal binding #{} binding={} element={} write={} valid={} id={} uid={} "
                "descriptor={:#x} {}x{} pitch={} cached={:#x}+{:#x} {}x{} pitch={} "
                "view={} level={}+{} layer={}+{} temporal_descriptor_source={}@{} "
                "temporal_uid_source={}@{} temporal_overlap_source={}@{} "
                "writer={} dispatch={} shader={:#x} kind={} target={} id={}",
                ordinal, entry.binding, entry.array_element, entry.written, entry.valid,
                entry.image_id, entry.image_uid, entry.descriptor_base, entry.descriptor_width,
                entry.descriptor_height, entry.descriptor_pitch, entry.guest_base,
                entry.guest_size, entry.cached_width, entry.cached_height, entry.cached_pitch,
                entry.view_format, entry.base_level, entry.levels, entry.base_layer, entry.layers,
                descriptor_source_ordinal, descriptor_source_sequence, uid_source_ordinal,
                uid_source_sequence, overlap_source_ordinal, overlap_source_sequence,
                latest_writer != nullptr ? latest_writer->sequence : 0,
                latest_writer != nullptr ? latest_writer->dispatch_sequence : 0,
                latest_writer != nullptr ? latest_writer->shader_hash : 0,
                latest_writer != nullptr ? static_cast<u32>(latest_writer->kind) : 0,
                latest_writer != nullptr ? latest_writer->binding_index : 0,
                latest_writer != nullptr ? latest_writer->image_id : 0);
        }

        u64 immediate_descriptor_mask{};
        u64 immediate_uid_mask{};
        u64 immediate_overlap_mask{};
        u64 current_descriptor_alias_mask{};
        u64 current_uid_alias_mask{};
        if (!history.empty()) {
            for (const auto& prior : history.back().images) {
                if (!prior.written) {
                    continue;
                }
                for (const auto& entry : current) {
                    if (entry.written || entry.binding >= 64) {
                        continue;
                    }
                    immediate_descriptor_mask |=
                        static_cast<u64>(same_descriptor(prior, entry)) << entry.binding;
                    immediate_uid_mask |=
                        static_cast<u64>(same_cached_view(prior, entry)) << entry.binding;
                    immediate_overlap_mask |=
                        static_cast<u64>(OverlapsDreamsRange(
                            prior.guest_base, prior.guest_size, entry.guest_base,
                            entry.guest_size))
                        << entry.binding;
                }
            }
        }
        for (const auto& output : current) {
            if (!output.written) {
                continue;
            }
            for (const auto& input : current) {
                if (input.written || input.binding >= 64) {
                    continue;
                }
                current_descriptor_alias_mask |=
                    static_cast<u64>(same_descriptor(output, input)) << input.binding;
                current_uid_alias_mask |=
                    static_cast<u64>(same_cached_view(output, input)) << input.binding;
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Dreams temporal chain #{} previous={}@{} immediate_descriptor_mask={:#x} "
                    "immediate_uid_mask={:#x} immediate_overlap_mask={:#x} "
                    "current_descriptor_alias_mask={:#x} current_uid_alias_mask={:#x}",
                    ordinal, history.empty() ? 0 : history.back().ordinal,
                    history.empty() ? 0 : history.back().sequence, immediate_descriptor_mask,
                    immediate_uid_mask, immediate_overlap_mask, current_descriptor_alias_mask,
                    current_uid_alias_mask);

        history.push_back({
            .ordinal = ordinal,
            .sequence = g_compute_dispatch_sequence,
            .images = std::move(current),
        });
        while (history.size() > 32) {
            history.pop_front();
        }
        if (g_dreams_temporal_image_trace_remaining == 0) {
            LOG_WARNING(Render_Vulkan, "Dreams temporal image trace complete");
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
    if (is_gds && TraceDreamsModelRecord() &&
        OverlapsDreamsModelConfigGds(address, num_bytes)) {
        LOG_WARNING(Render_Vulkan,
                    "Dreams model-config GDS fill sequence={} offset={:#x} bytes={:#x} "
                    "value={:#x}",
                    g_compute_dispatch_sequence, address, num_bytes, value);
    }
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
    if (TraceDreamsModelRecord() &&
        ((dst_gds && OverlapsDreamsModelConfigGds(dst, num_bytes)) ||
         (src_gds && OverlapsDreamsModelConfigGds(src, num_bytes)))) {
        LOG_WARNING(Render_Vulkan,
                    "Dreams model-config GDS copy sequence={} dst={:#x} src={:#x} bytes={:#x} "
                    "dst_gds={} src_gds={}",
                    g_compute_dispatch_sequence, dst, src, num_bytes, dst_gds, src_gds);
    }
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
