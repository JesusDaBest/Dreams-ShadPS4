// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/dreams_compat.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::LogicalStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = buffer.IsStorage(sharp) ? vk::DescriptorType::eStorageBuffer
                                                      : vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const bool uses_dreams_ordered_replay =
        Shader::DreamsCompat::UsesExactOrderedCountReplay(info->pgm_hash);
    const vk::SpecializationMapEntry phase_map_entry = {
        .constantID = Shader::DreamsCompat::OrderedPhaseSpecId,
        .offset = 0,
        .size = sizeof(u32),
    };
    const auto create_phase_pipeline = [&](u32 phase, std::string_view phase_name) {
        const vk::SpecializationInfo specialization_info = {
            .mapEntryCount = 1,
            .pMapEntries = &phase_map_entry,
            .dataSize = sizeof(phase),
            .pData = &phase,
        };
        const vk::PipelineShaderStageCreateInfo shader_ci = {
            .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
            .pSpecializationInfo = uses_dreams_ordered_replay ? &specialization_info : nullptr,
        };
        const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
            .flags = (info->uses_ordered_count || info->pgm_hash == 0x7ba4de5d)
                         ? vk::PipelineCreateFlagBits::eDispatchBase
                         : vk::PipelineCreateFlags{},
            .stage = shader_ci,
            .layout = *pipeline_layout,
        };
        auto [pipeline_result, pipe] =
            device.createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
        ASSERT_MSG(pipeline_result == vk::Result::eSuccess,
                   "Failed to create compute pipeline {}: {}", phase_name,
                   vk::to_string(pipeline_result));
        SetObjectName(device, *pipe, "Compute Pipeline {} {}", debug_str, phase_name);
        return std::move(pipe);
    };

    pipeline = create_phase_pipeline(Shader::DreamsCompat::OrderedPhaseNative, "native");
    if (uses_dreams_ordered_replay) {
        dreams_ordered_collect_pipeline =
            create_phase_pipeline(Shader::DreamsCompat::OrderedPhaseCollect, "collect");
        dreams_ordered_replay_pipeline =
            create_phase_pipeline(Shader::DreamsCompat::OrderedPhaseReplay, "replay");
    }
}

ComputePipeline::~ComputePipeline() = default;

vk::Pipeline ComputePipeline::HandleDreamsOrderedPhase(u32 phase) const noexcept {
    ASSERT_MSG(phase == Shader::DreamsCompat::OrderedPhaseNative ||
                   phase == Shader::DreamsCompat::OrderedPhaseCollect ||
                   phase == Shader::DreamsCompat::OrderedPhaseReplay,
               "Invalid Dreams ordered-count phase {}", phase);
    if (phase == Shader::DreamsCompat::OrderedPhaseCollect) {
        ASSERT_MSG(dreams_ordered_collect_pipeline,
                   "Dreams ordered-count collect pipeline was not constructed");
        return *dreams_ordered_collect_pipeline;
    }
    if (phase == Shader::DreamsCompat::OrderedPhaseReplay) {
        ASSERT_MSG(dreams_ordered_replay_pipeline,
                   "Dreams ordered-count replay pipeline was not constructed");
        return *dreams_ordered_replay_pipeline;
    }
    return *pipeline;
}

} // namespace Vulkan
