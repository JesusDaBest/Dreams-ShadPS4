// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::HostPasses {

class DreamsOrderedPrefixPass {
public:
    static constexpr u32 MaxBatchStreams = 5;
    static constexpr u32 MaxGroupCount = 65536;

    struct Stream {
        // Exact for RunBatch and an upper bound for RunBatchFromGdsCount.
        u32 group_count;
        u32 counter_index;
        u32 scratch_base;
    };

    struct Matrix {
        u32 group_count;
        u32 stream_count;
        u32 counter_base;
        u32 counter_row_stride;
        u32 counter_columns;
        u32 scratch_base;
        u32 scratch_stream_stride;
    };

    void Create(vk::Device device);

    void Run(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer, vk::DeviceSize gds_range,
             u32 group_count, u32 counter_index, u32 scratch_base, u32 entry_stride) const;
    void RunBatch(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer, vk::DeviceSize gds_range,
                  std::span<const Stream> streams, u32 entry_stride) const;
    void RunMatrix(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer, vk::DeviceSize gds_range,
                   const Matrix& matrix, u32 entry_stride) const;
    void ClearGroupCount(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer, vk::DeviceSize gds_range,
                         u32 group_count_index) const;
    void RunFromGdsCount(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer, vk::DeviceSize gds_range,
                         u32 max_group_count, u32 group_count_index, u32 counter_index,
                         u32 scratch_base, u32 entry_stride) const;
    void RunBatchFromGdsCount(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer,
                              vk::DeviceSize gds_range, std::span<const Stream> streams,
                              u32 group_count_index, u32 entry_stride) const;

private:
    struct StreamConstants {
        u32 group_count;
        u32 counter_index;
        u32 scratch_base;
        u32 group_count_index;
    };

    struct PushConstants {
        std::array<StreamConstants, MaxBatchStreams> streams;
        u32 stream_count;
        u32 entry_stride;
        u32 group_count_from_gds_mask;
        u32 matrix_group_count;
        u32 matrix_stream_count;
        u32 matrix_counter_base;
        u32 matrix_counter_row_stride;
        u32 matrix_counter_columns;
        u32 matrix_scratch_base;
        u32 matrix_scratch_stream_stride;
    };
    static_assert(sizeof(StreamConstants) == 4 * sizeof(u32));
    static_assert(offsetof(PushConstants, stream_count) ==
                  MaxBatchStreams * sizeof(StreamConstants));
    static_assert(sizeof(PushConstants) == 30 * sizeof(u32));

    void RunBatchImpl(vk::CommandBuffer cmdbuf, vk::Buffer gds_buffer, vk::DeviceSize gds_range,
                      std::span<const Stream> streams, u32 group_count_index,
                      bool group_count_from_gds, u32 entry_stride) const;

    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline pipeline{};
};

} // namespace Vulkan::HostPasses
