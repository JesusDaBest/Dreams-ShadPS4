// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
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
static void DispatchDreamsTraversalOrdered(vk::CommandBuffer cmdbuf, u32 dim_x, u32 dim_y,
                                           u32 dim_z) {
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
    for (u32 z = 0; z < dim_z; ++z) {
        for (u32 y = 0; y < dim_y; ++y) {
            for (u32 x = 0; x < dim_x; ++x) {
                if (!first) {
                    cmdbuf.pipelineBarrier2(dependency);
                }
                // DS_ORDERED_COUNT is defined in guest wave-creation order. Each Dreams
                // traversal workgroup is one wave, so dispatch them individually in that order.
                cmdbuf.dispatchBase(x, y, z, 1, 1, 1);
                first = false;
            }
        }
    }
}

struct DreamsBufferWriter {
    u64 sequence;
    u64 shader_hash;
    Shader::LogicalStage stage;
    VAddr base;
    u64 size;
    u32 stride;
};

static std::deque<DreamsBufferWriter> g_dreams_buffer_writers;
static u64 g_dreams_buffer_writer_sequence{};

struct RecentComputeDispatch {
    u64 hash{};
    u32 dim_x{};
    u32 dim_y{};
    u32 dim_z{};
};

static std::array<RecentComputeDispatch, 32> g_recent_compute_dispatches{};
static u32 g_recent_compute_dispatch_index{};
static u64 g_compute_dispatch_sequence{};

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

static bool TraceDreamsCsgReplay() {
    const char* value = std::getenv("SHADPS4_DREAMS_CSG_TRACE");
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

static void TraceComputeShader(const Shader::Info& info, u32 dim_x, u32 dim_y, u32 dim_z) {
    g_recent_compute_dispatches[g_recent_compute_dispatch_index++ %
                                g_recent_compute_dispatches.size()] = {
        .hash = info.pgm_hash,
        .dim_x = dim_x,
        .dim_y = dim_y,
        .dim_z = dim_z,
    };
    ++g_compute_dispatch_sequence;

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

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

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

    ResetBindings();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

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
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }
    TraceComputeShader(cs, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    if (ShouldSkipComputeShader(cs)) {
        return;
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

    const bool is_dreams_producer =
        Common::ElfInfo::Instance().GameSerial() == "CUSA04301" && cs.pgm_hash == 0x2bfebd3c;
    const u32 dreams_producer_record_count =
        is_dreams_producer && cs.flattened_ud_buf.size() > 41 ? cs.flattened_ud_buf[41] : 0;
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

    static std::vector<u32> dreams_scene_gds_before{};
    static bool captured_dreams_scene_gds{};
    if (TraceDreamsOrderedCounters() &&
        cs.pgm_hash == Shader::DreamsCompat::SceneCompactShader) {
        scheduler.Finish();
        const auto* gds = buffer_cache.GetGdsBuffer();
        dreams_scene_gds_before.resize(gds->mapped_data.size() / sizeof(u32));
        std::memcpy(dreams_scene_gds_before.data(), gds->mapped_data.data(),
                    dreams_scene_gds_before.size() * sizeof(u32));
        captured_dreams_scene_gds = true;
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
            if (!scan.mapped || scan.gpu_modified) {
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
    TraceComputeShader(cs, 0, 0, 0);
    if (ShouldSkipComputeShader(cs)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const bool profile_dreams = ShouldProfileDreamsTraversal(cs);
    const bool profile = ShouldProfileCompute() || profile_dreams;
    const VAddr args_address = address + offset;
    const bool is_dreams_csg_replay = cs.pgm_hash == 0x8b19605c;
    static u32 dreams_csg_replay_trace_count{};
    const bool trace_dreams_csg_replay = TraceDreamsCsgReplay() && is_dreams_csg_replay &&
                                         dreams_csg_replay_trace_count++ < 12;
    VAddr dreams_csg_state_address{};
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

    const bool inspect_ordered_dims = profile_dreams || trace_dreams_progress;
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
    if (profile) {
        scheduler.Finish();
    }
    if (trace_dreams_csg_replay) {
        scheduler.Finish();
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
        if (dump_dreams_stage) {
            LOG_WARNING(Render_Vulkan,
                        "Dreams producer shader={:#x} buffer={} sharp={} type={} write={} "
                        "formatted={} base={:#x} size={:#x} stride={} records={} used={:#x}",
                        stage.pgm_hash, buffer_index, desc.sharp_idx,
                        static_cast<u32>(desc.buffer_type), desc.is_written, desc.is_formatted,
                        vsharp.base_address, vsharp.GetSize(), vsharp.stride, vsharp.num_records,
                        static_cast<u32>(desc.used_types));
        }
        if (TraceDreamsBufferDependencies() && desc.is_written && !desc.IsSpecial() &&
            vsharp.base_address != 0 && vsharp.GetSize() != 0) {
            g_dreams_buffer_writers.push_back({
                .sequence = ++g_dreams_buffer_writer_sequence,
                .shader_hash = stage.pgm_hash,
                .stage = stage.l_stage,
                .base = vsharp.base_address,
                .size = vsharp.GetSize(),
                .stride = static_cast<u32>(vsharp.stride),
            });
            constexpr size_t MaxDreamsBufferWriterHistory = 65536;
            if (g_dreams_buffer_writers.size() > MaxDreamsBufferWriterHistory) {
                g_dreams_buffer_writers.pop_front();
            }
        }
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
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
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
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
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
            }
            if (desc.is_written && desc.is_formatted) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
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

    const auto bind_null_image = [&](bool is_storage) {
        auto& [image_id, desc] =
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
        desc.type = is_storage ? VideoCore::TextureCache::BindingType::Storage
                               : VideoCore::TextureCache::BindingType::Texture;
        desc.view_info.is_storage = is_storage;
    };

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
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
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

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
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
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
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
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    const bool trace_gds_copy = dst_gds || src_gds;
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
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
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
