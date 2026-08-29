// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/dreams_ordered_prefix_pass.h"

#include <array>
#include <limits>

#include "common/assert.h"
#include "video_core/host_shaders/dreams_ordered_prefix_comp.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

void DreamsOrderedPrefixPass::Create(vk::Device device) {
    constexpr std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    };
    descriptor_set_layout = Check<"create Dreams ordered-prefix descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = bindings.size(),
            .pBindings = bindings.data(),
        }));

    constexpr vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout =
        Check<"create Dreams ordered-prefix pipeline layout">(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &descriptor_set_layout.get(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        }));
    SetObjectName(device, pipeline_layout.get(), "Dreams ordered-prefix pipeline layout");

    const auto module =
        Compile(HostShaders::DREAMS_ORDERED_PREFIX_COMP, vk::ShaderStageFlagBits::eCompute, device);
    ASSERT(module);
    SetObjectName(device, module, "dreams_ordered_prefix.comp");

    const vk::ComputePipelineCreateInfo pipeline_info{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    pipeline = Check<"create Dreams ordered-prefix compute pipeline">(
        device.createComputePipelineUnique({}, pipeline_info));
    SetObjectName(device, pipeline.get(), "Dreams ordered-prefix pipeline");
    device.destroyShaderModule(module);
}

void DreamsOrderedPrefixPass::Run(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                                  vk::DeviceSize gds_range, u32 group_count, u32 counter_index,
                                  u32 scratch_base, u32 entry_stride) const {
    const std::array streams{
        Stream{group_count, counter_index, scratch_base},
    };
    RunBatch(cmdbuf, gds_buffer, gds_range, streams, entry_stride);
}

void DreamsOrderedPrefixPass::RunBatch(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                                       vk::DeviceSize gds_range, std::span<const Stream> streams,
                                       u32 entry_stride) const {
    RunBatchImpl(cmdbuf, gds_buffer, gds_range, streams, 0, false, entry_stride);
}

void DreamsOrderedPrefixPass::RunMatrix(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                                        vk::DeviceSize gds_range, const Matrix& matrix,
                                        u32 entry_stride) const {
    ASSERT_MSG(matrix.group_count != 0 && matrix.group_count <= MaxGroupCount,
               "Ordered-prefix matrix has invalid group count {}", matrix.group_count);
    ASSERT_MSG(matrix.stream_count != 0 && matrix.counter_columns != 0,
               "Ordered-prefix matrix has no streams or counter columns");
    ASSERT_MSG(entry_stride >= 2, "Ordered-prefix entries require payload and prefix dwords");
    if (matrix.group_count == 0 || matrix.group_count > MaxGroupCount ||
        matrix.stream_count == 0 || matrix.counter_columns == 0 || entry_stride < 2) {
        return;
    }

    const u64 counter_rows =
        (static_cast<u64>(matrix.stream_count) + matrix.counter_columns - 1) /
        matrix.counter_columns;
    const u64 counter_dwords =
        (counter_rows - 1) * matrix.counter_row_stride + matrix.counter_columns;
    const u64 scratch_dwords =
        static_cast<u64>(matrix.stream_count - 1) * matrix.scratch_stream_stride +
        static_cast<u64>(matrix.group_count - 1) * entry_stride + 2;
    const auto validate_range = [&](u64 first_dword, u64 dword_count) {
        const u64 end_dword = first_dword + dword_count;
        ASSERT_MSG(end_dword <= std::numeric_limits<u32>::max(),
                   "Ordered-prefix matrix GDS dword index overflow");
        ASSERT_MSG(gds_range == VK_WHOLE_SIZE || end_dword * sizeof(u32) <= gds_range,
                   "Ordered-prefix matrix range exceeds GDS buffer range");
    };
    validate_range(matrix.counter_base, counter_dwords);
    validate_range(matrix.scratch_base, scratch_dwords);

    const std::array before_scan_barriers{
        vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask =
                vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
            .buffer = gds_buffer,
            .offset = static_cast<vk::DeviceSize>(matrix.counter_base) * sizeof(u32),
            .size = counter_dwords * sizeof(u32),
        },
        vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask =
                vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
            .buffer = gds_buffer,
            .offset = static_cast<vk::DeviceSize>(matrix.scratch_base) * sizeof(u32),
            .size = scratch_dwords * sizeof(u32),
        },
    };
    cmdbuf.pipelineBarrier2({
        .bufferMemoryBarrierCount = static_cast<u32>(before_scan_barriers.size()),
        .pBufferMemoryBarriers = before_scan_barriers.data(),
    });

    const vk::DescriptorBufferInfo gds_info{
        .buffer = gds_buffer,
        .offset = 0,
        .range = gds_range,
    };
    const std::array writes{
        vk::WriteDescriptorSet{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &gds_info,
        },
    };
    PushConstants constants{
        .entry_stride = entry_stride,
        .matrix_group_count = matrix.group_count,
        .matrix_stream_count = matrix.stream_count,
        .matrix_counter_base = matrix.counter_base,
        .matrix_counter_row_stride = matrix.counter_row_stride,
        .matrix_counter_columns = matrix.counter_columns,
        .matrix_scratch_base = matrix.scratch_base,
        .matrix_scratch_stream_stride = matrix.scratch_stream_stride,
    };

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get());
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0, writes);
    cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(constants), &constants);
    cmdbuf.dispatch((matrix.stream_count + 7) / 8, 1, 1);

    const std::array after_scan_barriers{
        vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            .buffer = gds_buffer,
            .offset = static_cast<vk::DeviceSize>(matrix.counter_base) * sizeof(u32),
            .size = counter_dwords * sizeof(u32),
        },
        vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            .buffer = gds_buffer,
            .offset = static_cast<vk::DeviceSize>(matrix.scratch_base) * sizeof(u32),
            .size = scratch_dwords * sizeof(u32),
        },
    };
    cmdbuf.pipelineBarrier2({
        .bufferMemoryBarrierCount = static_cast<u32>(after_scan_barriers.size()),
        .pBufferMemoryBarriers = after_scan_barriers.data(),
    });
}

void DreamsOrderedPrefixPass::ClearGroupCount(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                                              vk::DeviceSize gds_range,
                                              u32 group_count_index) const {
    const vk::DeviceSize offset = static_cast<vk::DeviceSize>(group_count_index) * sizeof(u32);
    ASSERT_MSG(gds_range == VK_WHOLE_SIZE || offset + sizeof(u32) <= gds_range,
               "Ordered-prefix group-count slot exceeds GDS buffer range");

    const vk::BufferMemoryBarrier2 before_clear{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask =
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = gds_buffer,
        .offset = offset,
        .size = sizeof(u32),
    };
    cmdbuf.pipelineBarrier2({
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &before_clear,
    });
    cmdbuf.fillBuffer(gds_buffer, offset, sizeof(u32), 0);
    const vk::BufferMemoryBarrier2 after_clear{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .buffer = gds_buffer,
        .offset = offset,
        .size = sizeof(u32),
    };
    cmdbuf.pipelineBarrier2({
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &after_clear,
    });
}

void DreamsOrderedPrefixPass::RunFromGdsCount(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                                              vk::DeviceSize gds_range, u32 max_group_count,
                                              u32 group_count_index, u32 counter_index,
                                              u32 scratch_base, u32 entry_stride) const {
    const std::array streams{
        Stream{max_group_count, counter_index, scratch_base},
    };
    RunBatchFromGdsCount(cmdbuf, gds_buffer, gds_range, streams, group_count_index, entry_stride);
}

void DreamsOrderedPrefixPass::RunBatchFromGdsCount(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                                                   vk::DeviceSize gds_range,
                                                   std::span<const Stream> streams,
                                                   u32 group_count_index, u32 entry_stride) const {
    RunBatchImpl(cmdbuf, gds_buffer, gds_range, streams, group_count_index, true, entry_stride);
}

void DreamsOrderedPrefixPass::RunBatchImpl(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                                           vk::DeviceSize gds_range,
                                           std::span<const Stream> streams, u32 group_count_index,
                                           bool group_count_from_gds, u32 entry_stride) const {
    ASSERT_MSG(streams.size() <= MaxBatchStreams,
               "Ordered-prefix batch has {} streams; maximum is {}", streams.size(),
               MaxBatchStreams);
    if (streams.empty() || streams.size() > MaxBatchStreams) {
        return;
    }

    ASSERT_MSG(entry_stride >= 2, "Ordered-prefix entries require payload and prefix dwords");
    PushConstants constants{};
    constexpr size_t MaxBeforeBarrierCount = MaxBatchStreams * 2 + 1;
    constexpr size_t MaxAfterBarrierCount = MaxBatchStreams * 2;
    std::array<vk::BufferMemoryBarrier2, MaxBeforeBarrierCount> before_scan_barriers{};
    std::array<vk::BufferMemoryBarrier2, MaxAfterBarrierCount> after_scan_barriers{};
    u32 before_scan_barrier_count{};
    u32 after_scan_barrier_count{};

    const auto validate_range = [&](u64 first_dword, u64 dword_count) {
        const u64 end_dword = first_dword + dword_count;
        ASSERT_MSG(end_dword <= std::numeric_limits<u32>::max(),
                   "Ordered-prefix GDS dword index overflow");
        ASSERT_MSG(gds_range == VK_WHOLE_SIZE || end_dword * sizeof(u32) <= gds_range,
                   "Ordered-prefix range exceeds GDS buffer range");
    };
    const auto append_before_barrier = [&](u64 first_dword, u64 dword_count) {
        validate_range(first_dword, dword_count);
        ASSERT(before_scan_barrier_count < before_scan_barriers.size());
        before_scan_barriers[before_scan_barrier_count++] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask =
                vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
            .buffer = gds_buffer,
            .offset = first_dword * sizeof(u32),
            .size = dword_count * sizeof(u32),
        };
    };
    const auto append_after_barrier = [&](u64 first_dword, u64 dword_count) {
        validate_range(first_dword, dword_count);
        ASSERT(after_scan_barrier_count < after_scan_barriers.size());
        after_scan_barriers[after_scan_barrier_count++] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            .buffer = gds_buffer,
            .offset = first_dword * sizeof(u32),
            .size = dword_count * sizeof(u32),
        };
    };

    for (const Stream& stream : streams) {
        if (stream.group_count == 0) {
            continue;
        }

        ASSERT_MSG(stream.group_count <= MaxGroupCount,
                   "Ordered-prefix stream has {} groups; maximum is {}", stream.group_count,
                   MaxGroupCount);
        const u64 scratch_dwords =
            static_cast<u64>(stream.group_count - 1) * entry_stride + 2;
        append_before_barrier(stream.counter_index, 1);
        append_before_barrier(stream.scratch_base, scratch_dwords);
        append_after_barrier(stream.counter_index, 1);
        append_after_barrier(stream.scratch_base, scratch_dwords);

        auto& output = constants.streams[constants.stream_count++];
        output = {
            .group_count = stream.group_count,
            .counter_index = stream.counter_index,
            .scratch_base = stream.scratch_base,
            .group_count_index = group_count_index,
        };
    }
    if (constants.stream_count == 0) {
        return;
    }
    if (group_count_from_gds) {
        append_before_barrier(group_count_index, 1);
        constants.group_count_from_gds_mask = (1U << constants.stream_count) - 1;
    }
    cmdbuf.pipelineBarrier2({
        .bufferMemoryBarrierCount = before_scan_barrier_count,
        .pBufferMemoryBarriers = before_scan_barriers.data(),
    });

    const vk::DescriptorBufferInfo gds_info{
        .buffer = gds_buffer,
        .offset = 0,
        .range = gds_range,
    };
    const std::array writes{
        vk::WriteDescriptorSet{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &gds_info,
        },
    };
    constants.entry_stride = entry_stride;

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get());
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0, writes);
    cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(constants), &constants);
    cmdbuf.dispatch(1, 1, 1);

    cmdbuf.pipelineBarrier2({
        .bufferMemoryBarrierCount = after_scan_barrier_count,
        .pBufferMemoryBarriers = after_scan_barriers.data(),
    });
}

} // namespace Vulkan::HostPasses
