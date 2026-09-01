// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/dreams_draw_replay.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/dreams_compat.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/sampler.h"

namespace Vulkan::DreamsDrawReplay {
namespace {

constexpr u32 VerticesPerInstance = 8;
constexpr u32 InterfaceWordsPerVertex = 16;
constexpr u32 ReplayWordsPerVertex = 17;
constexpr u32 ExpectedIndexCount = 14;
constexpr u32 MaximumReplayInstances = Shader::DreamsCompat::Vs370InterfaceCaptureMaxInstances;
constexpr u32 ExpectedValidityMask = 0x0001ffff;
constexpr u32 VsBufferCount = 5;
constexpr u32 ReplayDescriptorCount = 16;
// Match BufferCache's runtime GDS allocation so opt-in captures in the final private range remain
// valid when the same shader is compiled and replayed by the standalone harness.
constexpr vk::DeviceSize GdsBufferBytes = 52ULL * 1024 * 1024;
constexpr std::array<u32, VsBufferCount> ExpectedVsBufferStrides{432, 4, 4, 8, 16};
constexpr std::array<u16, ExpectedIndexCount> ExpectedIndices{
    2, 3, 0, 1, 5, 3, 7, 2, 6, 0, 4, 5, 6, 7,
};
constexpr std::array B535CandidateBankFiles{
    "b535-input-records.bin",
    "b535-dropped-records.bin",
    "b535-input-b1-id-a.bin",
    "b535-input-b2-id-b.bin",
    "b535-input-b4-id-record.bin",
    "b535-input-b0-object-keys.bin",
    "b535-input-b0-objects.bin",
};
constexpr std::string_view AtlasHeader =
    "slot\tlow24_id\tbrick_x\tbrick_y\tbrick_z\tsource_x\tsource_y\tsource_z\t"
    "output_offset\tbyte_size\twidth\theight\tdepth";

using KeyValues = std::unordered_map<std::string, std::string>;

struct DrawIndexedCommand {
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    s32 vertex_offset;
    u32 first_instance;
};
static_assert(sizeof(DrawIndexedCommand) == 5 * sizeof(u32));

struct AtlasRegion {
    u32 slot;
    u32 low24_id;
    s32 brick_x;
    s32 brick_y;
    s32 brick_z;
    s32 source_x;
    s32 source_y;
    s32 source_z;
    u64 output_offset;
    u64 byte_size;
    u32 width;
    u32 height;
    u32 depth;
};

struct CapturedDynamicState {
    std::vector<vk::Viewport> viewports;
    std::vector<vk::Rect2D> scissors;
    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};
    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};
    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};
    bool stencil_test_enabled{};
    vk::StencilOpState stencil_front{};
    vk::StencilOpState stencil_back{};
    bool primitive_restart_enabled{};
    bool rasterizer_discard_enabled{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};
    float line_width{};
    bool feedback_loop_enabled{};
    std::array<float, 4> blend_constants{};
    std::vector<vk::ColorComponentFlags> color_write_masks;
};

struct CapturedRenderState {
    bool color_clear{};
    std::array<u32, 4> color_clear_raw{};
    bool has_depth{};
    bool depth_clear{};
    bool has_stencil{};
    bool stencil_clear{};
    std::array<u32, 4> depth_clear_raw{};
};

struct ReplayBundle {
    KeyValues manifest;
    KeyValues dynamic_state;
    KeyValues render_state;
    DrawIndexedCommand draw{};
    GraphicsPipelineKey graphics_key{};
    Shader::PushData push_data{};
    AmdGpu::Sampler sampler{};
    bool sampler_is_compare{};
    std::vector<u8> indices;
    std::vector<u8> visibility;
    std::vector<u8> vs_spv_bytes;
    std::vector<u8> fs_spv_bytes;
    std::vector<u8> vk_pipeline_cache;
    std::array<std::vector<u8>, VsBufferCount> vs_buffers;
    std::array<AmdGpu::Buffer, VsBufferCount> vs_buffer_descriptors{};
    std::vector<u8> vs_srt_flat;
    std::vector<u8> vs_transform_page;
    std::vector<u8> fs_srt_flat;
    std::vector<u8> fs_bda_constants;
    std::vector<u8> fs_bda_page;
    std::vector<u8> atlas_bricks;
    std::vector<u8> fs_image1;
    std::vector<u8> post_color;
    std::vector<u8> post_depth;
    std::vector<u8> pre_color;
    std::vector<u8> pre_depth;
    std::vector<u32> post_vs_records;
    std::vector<AtlasRegion> atlas_regions;
    std::unordered_map<u32, bool> captured_low24;
    std::unordered_map<u32, bool> captured_low16;
    std::unordered_map<u64, bool> captured_pairs;
    std::vector<std::array<u32, 2>> b535_input_records;
    std::vector<std::array<u32, 2>> b535_dropped_records;
    u64 fs_constant_address{};
    u64 fs_srt_page_base{};
    u64 transform_page_base{};
    u32 atlas_width{};
    u32 atlas_height{};
    u32 atlas_depth{};
    u32 image1_width{};
    u32 image1_height{};
    u32 render_width{};
    u32 render_height{};
    u32 render_layers{};
    u32 instance_count{};
    u64 first_record{};
    std::array<vk::ComponentSwizzle, 4> atlas_swizzle{};
    std::array<vk::ComponentSwizzle, 4> image1_swizzle{};
    std::array<vk::ComponentSwizzle, 4> color_swizzle{};
    std::array<vk::ComponentSwizzle, 4> depth_swizzle{};
    vk::ImageLayout atlas_layout{};
    vk::ImageLayout image1_layout{};
    vk::ImageLayout color_layout{};
    vk::ImageLayout depth_layout{};
    CapturedDynamicState dynamic{};
    CapturedRenderState render{};
};

struct LoadResult {
    std::optional<ReplayBundle> bundle;
    std::string stage;
    std::string error;
    std::vector<std::string> missing_files;
};

bool WriteStatus(const std::filesystem::path& bundle_path, std::string_view stage,
                 std::string_view error, std::span<const std::string> missing_files = {},
                 bool parity_verified = false) {
    const std::vector<std::string> missing{missing_files.begin(), missing_files.end()};
    nlohmann::json status{
        {"schema", 2},    {"parity_verified", parity_verified}, {"stage", stage},
        {"error", error}, {"bundle", bundle_path.string()},     {"missing_files", missing},
    };
    std::ofstream output(bundle_path / "status.json", std::ios::trunc);
    if (!output) {
        LOG_ERROR(Render_Vulkan, "Dreams replay could not write status.json in {}",
                  bundle_path.string());
        return false;
    }
    output << status.dump(2) << '\n';
    return static_cast<bool>(output);
}

std::optional<std::vector<u8>> ReadBytes(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > std::numeric_limits<size_t>::max()) {
        error = "cannot determine size of " + path.filename().string();
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open " + path.filename().string();
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        error = "short or unstable read of " + path.filename().string();
        return std::nullopt;
    }
    return bytes;
}

std::optional<KeyValues> ReadKeyValues(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open " + path.filename().string();
        return std::nullopt;
    }
    KeyValues values;
    std::string line;
    u32 line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line == "key\tvalue") {
            continue;
        }
        const auto separator = line.find('\t');
        if (separator == std::string::npos || separator == 0) {
            error = path.filename().string() + ": malformed row " + std::to_string(line_number);
            return std::nullopt;
        }
        auto [it, inserted] = values.emplace(line.substr(0, separator), line.substr(separator + 1));
        if (!inserted) {
            error = path.filename().string() + ": duplicate key " + it->first;
            return std::nullopt;
        }
    }
    if (!input.eof()) {
        error = "failed while reading " + path.filename().string();
        return std::nullopt;
    }
    return values;
}

template <typename T>
std::optional<T> ParseInteger(std::string_view text) {
    static_assert(std::is_integral_v<T>);
    int base = 10;
    bool negative = false;
    if constexpr (std::is_signed_v<T>) {
        if (!text.empty() && text.front() == '-') {
            negative = true;
            text.remove_prefix(1);
        }
    }
    if (text.starts_with("0x") || text.starts_with("0X")) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned magnitude{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), magnitude, base);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    if constexpr (std::is_signed_v<T>) {
        if (negative) {
            constexpr auto limit = Unsigned{std::numeric_limits<T>::max()} + 1;
            if (magnitude > limit) {
                return std::nullopt;
            }
            if (magnitude == limit) {
                return std::numeric_limits<T>::min();
            }
            return static_cast<T>(-static_cast<T>(magnitude));
        }
        if (magnitude > static_cast<Unsigned>(std::numeric_limits<T>::max())) {
            return std::nullopt;
        }
    }
    return static_cast<T>(magnitude);
}

template <typename T>
std::optional<T> RequiredInteger(const KeyValues& values, std::string_view key,
                                 std::string& error) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
        error = "missing metadata key " + std::string{key};
        return std::nullopt;
    }
    auto value = ParseInteger<T>(it->second);
    if (!value) {
        error = "invalid integer for metadata key " + std::string{key};
    }
    return value;
}

std::optional<float> ParseFloat(std::string_view text) {
    float value{};
    const auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<float> RequiredFloat(const KeyValues& values, std::string_view key,
                                   std::string& error) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
        error = "missing metadata key " + std::string{key};
        return std::nullopt;
    }
    auto value = ParseFloat(it->second);
    if (!value) {
        error = "invalid float for metadata key " + std::string{key};
    }
    return value;
}

std::optional<bool> RequiredBool(const KeyValues& values, std::string_view key,
                                 std::string& error) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
        error = "missing metadata key " + std::string{key};
        return std::nullopt;
    }
    if (it->second == "true") {
        return true;
    }
    if (it->second == "false") {
        return false;
    }
    const auto value = ParseInteger<u32>(it->second);
    if (!value) {
        error = "invalid boolean for metadata key " + std::string{key};
        return std::nullopt;
    }
    if (*value > 1) {
        error = "invalid boolean for metadata key " + std::string{key};
        return std::nullopt;
    }
    return *value != 0;
}

std::optional<std::vector<std::string_view>> SplitCsv(std::string_view text,
                                                      size_t expected_count) {
    std::vector<std::string_view> fields;
    fields.reserve(expected_count);
    while (true) {
        const auto separator = text.find(',');
        fields.push_back(separator == std::string_view::npos ? text : text.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
    }
    if (fields.size() != expected_count ||
        std::ranges::any_of(fields, [](std::string_view field) { return field.empty(); })) {
        return std::nullopt;
    }
    return fields;
}

std::optional<std::array<u32, 4>> RequiredU32x4(const KeyValues& values, std::string_view key,
                                                std::string& error) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
        error = "missing metadata key " + std::string{key};
        return std::nullopt;
    }
    const auto fields = SplitCsv(it->second, 4);
    if (!fields) {
        error = "invalid u32 vector for metadata key " + std::string{key};
        return std::nullopt;
    }
    std::array<u32, 4> output{};
    for (u32 index = 0; index < output.size(); ++index) {
        const auto value = ParseInteger<u32>((*fields)[index]);
        if (!value) {
            error = "invalid u32 vector for metadata key " + std::string{key};
            return std::nullopt;
        }
        output[index] = *value;
    }
    return output;
}

std::optional<std::array<vk::ComponentSwizzle, 4>> RequiredSwizzle(const KeyValues& values,
                                                                   std::string_view key,
                                                                   std::string& error) {
    const auto raw = RequiredU32x4(values, key, error);
    if (!raw) {
        return std::nullopt;
    }
    std::array<vk::ComponentSwizzle, 4> swizzle{};
    for (u32 index = 0; index < swizzle.size(); ++index) {
        if ((*raw)[index] > static_cast<u32>(vk::ComponentSwizzle::eA)) {
            error = "invalid component mapping for metadata key " + std::string{key};
            return std::nullopt;
        }
        swizzle[index] = static_cast<vk::ComponentSwizzle>((*raw)[index]);
    }
    return swizzle;
}

std::optional<CapturedDynamicState> ParseDynamicState(const KeyValues& values, std::string& error) {
    CapturedDynamicState state{};
    const auto viewport_count = RequiredInteger<u32>(values, "viewport_count", error);
    const auto scissor_count = RequiredInteger<u32>(values, "scissor_count", error);
    const auto depth_test = RequiredBool(values, "depth_test_enabled", error);
    const auto depth_write = RequiredBool(values, "depth_write_enabled", error);
    const auto depth_compare = RequiredInteger<u32>(values, "depth_compare_op", error);
    const auto depth_bounds = RequiredBool(values, "depth_bounds_test_enabled", error);
    const auto depth_bounds_min = RequiredFloat(values, "depth_bounds_min", error);
    const auto depth_bounds_max = RequiredFloat(values, "depth_bounds_max", error);
    const auto depth_bias = RequiredBool(values, "depth_bias_enabled", error);
    const auto depth_bias_constant = RequiredFloat(values, "depth_bias_constant", error);
    const auto depth_bias_clamp = RequiredFloat(values, "depth_bias_clamp", error);
    const auto depth_bias_slope = RequiredFloat(values, "depth_bias_slope", error);
    const auto stencil_test = RequiredBool(values, "stencil_test_enabled", error);
    const auto primitive_restart = RequiredBool(values, "primitive_restart_enabled", error);
    const auto rasterizer_discard = RequiredBool(values, "rasterizer_discard_enabled", error);
    const auto cull_mode = RequiredInteger<u32>(values, "cull_mode", error);
    const auto front_face = RequiredInteger<u32>(values, "front_face", error);
    const auto line_width = RequiredFloat(values, "line_width", error);
    const auto feedback_loop = RequiredBool(values, "feedback_loop_enabled", error);
    if (!viewport_count || !scissor_count || !depth_test || !depth_write || !depth_compare ||
        !depth_bounds || !depth_bounds_min || !depth_bounds_max || !depth_bias ||
        !depth_bias_constant || !depth_bias_clamp || !depth_bias_slope || !stencil_test ||
        !primitive_restart || !rasterizer_discard || !cull_mode || !front_face || !line_width ||
        !feedback_loop) {
        return std::nullopt;
    }
    if (*viewport_count == 0 || *viewport_count > 16 || *scissor_count != *viewport_count ||
        *depth_compare > static_cast<u32>(vk::CompareOp::eAlways) || *cull_mode > 3 ||
        *front_face > static_cast<u32>(vk::FrontFace::eClockwise) || *line_width <= 0.0f) {
        error = "dynamic-state.tsv contains an unsupported state value";
        return std::nullopt;
    }
    state.depth_test_enabled = *depth_test;
    state.depth_write_enabled = *depth_write;
    state.depth_compare_op = static_cast<vk::CompareOp>(*depth_compare);
    state.depth_bounds_test_enabled = *depth_bounds;
    state.depth_bounds_min = *depth_bounds_min;
    state.depth_bounds_max = *depth_bounds_max;
    state.depth_bias_enabled = *depth_bias;
    state.depth_bias_constant = *depth_bias_constant;
    state.depth_bias_clamp = *depth_bias_clamp;
    state.depth_bias_slope = *depth_bias_slope;
    state.stencil_test_enabled = *stencil_test;
    state.primitive_restart_enabled = *primitive_restart;
    state.rasterizer_discard_enabled = *rasterizer_discard;
    state.cull_mode = static_cast<vk::CullModeFlags>(*cull_mode);
    state.front_face = static_cast<vk::FrontFace>(*front_face);
    state.line_width = *line_width;
    state.feedback_loop_enabled = *feedback_loop;

    const auto blend_it = values.find("blend_constants");
    const auto front_ops_it = values.find("stencil_front_ops");
    const auto back_ops_it = values.find("stencil_back_ops");
    if (blend_it == values.end() || front_ops_it == values.end() || back_ops_it == values.end()) {
        error = "dynamic-state.tsv is missing vector state";
        return std::nullopt;
    }
    const auto blend_fields = SplitCsv(blend_it->second, 4);
    const auto front_fields = SplitCsv(front_ops_it->second, 4);
    const auto back_fields = SplitCsv(back_ops_it->second, 4);
    if (!blend_fields || !front_fields || !back_fields) {
        error = "dynamic-state.tsv has malformed vector state";
        return std::nullopt;
    }
    for (u32 index = 0; index < 4; ++index) {
        const auto blend = ParseFloat((*blend_fields)[index]);
        const auto front = ParseInteger<u32>((*front_fields)[index]);
        const auto back = ParseInteger<u32>((*back_fields)[index]);
        if (!blend || !front || !back || *front > static_cast<u32>(vk::CompareOp::eAlways) ||
            *back > static_cast<u32>(vk::CompareOp::eAlways)) {
            error = "dynamic-state.tsv has invalid blend/stencil vector state";
            return std::nullopt;
        }
        state.blend_constants[index] = *blend;
    }
    const auto make_stencil = [&](const std::vector<std::string_view>& fields,
                                  std::string_view prefix) -> std::optional<vk::StencilOpState> {
        std::array<u32, 4> ops{};
        for (u32 index = 0; index < ops.size(); ++index) {
            const auto value = ParseInteger<u32>(fields[index]);
            if (!value ||
                (index < 3 && *value > static_cast<u32>(vk::StencilOp::eDecrementAndWrap)) ||
                (index == 3 && *value > static_cast<u32>(vk::CompareOp::eAlways))) {
                return std::nullopt;
            }
            ops[index] = *value;
        }
        const auto reference =
            RequiredInteger<u32>(values, std::string{prefix} + "_reference", error);
        const auto write_mask =
            RequiredInteger<u32>(values, std::string{prefix} + "_write_mask", error);
        const auto compare_mask =
            RequiredInteger<u32>(values, std::string{prefix} + "_compare_mask", error);
        if (!reference || !write_mask || !compare_mask) {
            return std::nullopt;
        }
        return vk::StencilOpState{
            .failOp = static_cast<vk::StencilOp>(ops[0]),
            .passOp = static_cast<vk::StencilOp>(ops[1]),
            .depthFailOp = static_cast<vk::StencilOp>(ops[2]),
            .compareOp = static_cast<vk::CompareOp>(ops[3]),
            .compareMask = *compare_mask,
            .writeMask = *write_mask,
            .reference = *reference,
        };
    };
    const auto front = make_stencil(*front_fields, "stencil_front");
    const auto back = make_stencil(*back_fields, "stencil_back");
    if (!front || !back) {
        if (error.empty()) {
            error = "dynamic-state.tsv has invalid stencil state";
        }
        return std::nullopt;
    }
    state.stencil_front = *front;
    state.stencil_back = *back;

    state.viewports.reserve(*viewport_count);
    state.scissors.reserve(*scissor_count);
    state.color_write_masks.reserve(*viewport_count);
    for (u32 index = 0; index < *viewport_count; ++index) {
        const auto viewport_it = values.find("viewport_" + std::to_string(index));
        const auto scissor_it = values.find("scissor_" + std::to_string(index));
        if (viewport_it == values.end() || scissor_it == values.end()) {
            error = "dynamic-state.tsv is missing viewport/scissor state";
            return std::nullopt;
        }
        const auto viewport_fields = SplitCsv(viewport_it->second, 6);
        const auto scissor_fields = SplitCsv(scissor_it->second, 4);
        if (!viewport_fields || !scissor_fields) {
            error = "dynamic-state.tsv has malformed viewport/scissor state";
            return std::nullopt;
        }
        std::array<float, 6> viewport_values{};
        for (u32 component = 0; component < viewport_values.size(); ++component) {
            const auto value = ParseFloat((*viewport_fields)[component]);
            if (!value) {
                error = "dynamic-state.tsv has an invalid viewport";
                return std::nullopt;
            }
            viewport_values[component] = *value;
        }
        const auto x = ParseInteger<s32>((*scissor_fields)[0]);
        const auto y = ParseInteger<s32>((*scissor_fields)[1]);
        const auto width = ParseInteger<u32>((*scissor_fields)[2]);
        const auto height = ParseInteger<u32>((*scissor_fields)[3]);
        if (!x || !y || !width || !height) {
            error = "dynamic-state.tsv has an invalid scissor";
            return std::nullopt;
        }
        state.viewports.emplace_back(vk::Viewport{
            .x = viewport_values[0],
            .y = viewport_values[1],
            .width = viewport_values[2],
            .height = viewport_values[3],
            .minDepth = viewport_values[4],
            .maxDepth = viewport_values[5],
        });
        state.scissors.emplace_back(vk::Rect2D{
            .offset = {.x = *x, .y = *y},
            .extent = {.width = *width, .height = *height},
        });
    }
    const auto mask = RequiredInteger<u32>(values, "color_write_mask_0", error);
    if (!mask || *mask > 0xf) {
        if (error.empty()) {
            error = "dynamic-state.tsv has an invalid color write mask";
        }
        return std::nullopt;
    }
    state.color_write_masks.push_back(static_cast<vk::ColorComponentFlags>(*mask));
    return state;
}

std::optional<std::array<u32, 3>> ParseDimensions(std::string_view text) {
    std::array<u32, 3> dimensions{};
    for (u32 index = 0; index < dimensions.size(); ++index) {
        const auto separator = text.find('x');
        const auto part = separator == std::string_view::npos ? text : text.substr(0, separator);
        const auto value = ParseInteger<u32>(part);
        if (!value) {
            return std::nullopt;
        }
        dimensions[index] = *value;
        if (index + 1 == dimensions.size()) {
            if (separator != std::string_view::npos) {
                return std::nullopt;
            }
        } else {
            if (separator == std::string_view::npos) {
                return std::nullopt;
            }
            text.remove_prefix(separator + 1);
        }
    }
    return dimensions;
}

u64 HashBytes(std::span<const u8> bytes) {
    u64 hash = 1469598103934665603ULL;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct ReplayCandidate {
    std::string name;
    std::filesystem::path source;
    std::vector<u8> visibility;
    u32 instance_count{};
};

struct CandidateLoadResult {
    std::vector<ReplayCandidate> candidates;
    std::string error;
};

CandidateLoadResult LoadReplayCandidates(const std::filesystem::path& directory,
                                         const ReplayBundle& baseline) {
    CandidateLoadResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) {
        result.error = "candidate path is not a readable directory";
        return result;
    }

    std::vector<std::filesystem::path> paths;
    for (std::filesystem::directory_iterator it{directory, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec && it->path().extension() == ".bin") {
            paths.emplace_back(it->path());
        }
    }
    if (ec) {
        result.error = "candidate directory could not be enumerated: " + ec.message();
        return result;
    }
    std::ranges::sort(paths, {}, [](const auto& path) { return path.filename().string(); });
    if (paths.empty()) {
        result.error = "candidate directory contains no .bin files";
        return result;
    }

    const u64 record_capacity = baseline.vs_buffer_descriptors[3].GetSize() / (2 * sizeof(u32));
    if (baseline.first_record > record_capacity) {
        result.error = "captured visibility first record exceeds binding 3";
        return result;
    }
    const u64 available_records = record_capacity - baseline.first_record;

    result.candidates.reserve(paths.size());
    for (const auto& path : paths) {
        std::string error;
        auto bytes = ReadBytes(path, error);
        if (!bytes) {
            result.error = std::move(error);
            return result;
        }
        if (bytes->empty() || bytes->size() % (2 * sizeof(u32)) != 0) {
            result.error = path.filename().string() +
                           " is not a non-empty little-endian U32x2 list";
            return result;
        }
        const u64 records = bytes->size() / (2 * sizeof(u32));
        if (records > MaximumReplayInstances || records > available_records) {
            result.error = path.filename().string() +
                           " has more records than the exact replay can draw";
            return result;
        }
        for (u64 record = 0; record < records; ++record) {
            std::array<u32, 2> words{};
            std::memcpy(words.data(), bytes->data() + record * sizeof(words), sizeof(words));
            const u64 packed = u64{words[0]} | (u64{words[1]} << 32);
            if (!baseline.captured_pairs.contains(packed)) {
                result.error = fmt::format(
                    "{} record {} is not an exact captured Dreams pair ({:#x}, {:#x})",
                    path.filename().string(), record, words[0], words[1]);
                return result;
            }
        }
        const std::string name = path.stem().string();
        if (name.find_first_of("\t\r\n") != std::string::npos) {
            result.error = path.filename().string() + " has a name unsafe for results.tsv";
            return result;
        }
        result.candidates.emplace_back(ReplayCandidate{
            .name = name,
            .source = path,
            .visibility = std::move(*bytes),
            .instance_count = static_cast<u32>(records),
        });
    }
    return result;
}

bool ValidateHash(const ReplayBundle& bundle, std::string_view filename, std::span<const u8> bytes,
                  std::string& error, std::string_view override_key = {}) {
    const std::string key =
        override_key.empty() ? "hash_" + std::string{filename} : std::string{override_key};
    const auto expected = RequiredInteger<u64>(bundle.manifest, key, error);
    if (!expected) {
        return false;
    }
    const u64 actual = HashBytes(bytes);
    if (actual != *expected) {
        error = std::string{filename} + " hash mismatch";
        return false;
    }
    return true;
}

template <typename T>
bool CopyExact(std::span<const u8> bytes, T& output, std::string_view filename,
               std::string& error) {
    if (bytes.size() != sizeof(T)) {
        error = std::string{filename} + " has " + std::to_string(bytes.size()) +
                " bytes; expected " + std::to_string(sizeof(T));
        return false;
    }
    std::memcpy(&output, bytes.data(), sizeof(T));
    return true;
}

struct CapturedBufferMetadata {
    u64 base_address{};
    u64 size{};
    u32 stride{};
    u32 oob_records{};
};

std::optional<CapturedBufferMetadata> ParseBufferMetadata(std::string_view text) {
    constexpr std::string_view StrideToken = ";stride=";
    constexpr std::string_view ModifiedToken = ";gpu_modified=";
    constexpr std::string_view OobToken = ";oob_records=";
    const auto plus = text.find('+');
    const auto stride_at = text.find(StrideToken, plus == std::string_view::npos ? 0 : plus + 1);
    const auto modified_at =
        text.find(ModifiedToken, stride_at == std::string_view::npos ? 0 : stride_at + 1);
    const auto oob_at =
        text.find(OobToken, modified_at == std::string_view::npos ? 0 : modified_at + 1);
    if (plus == std::string_view::npos || stride_at == std::string_view::npos ||
        modified_at == std::string_view::npos || oob_at == std::string_view::npos ||
        !(stride_at > plus && modified_at > stride_at && oob_at > modified_at)) {
        return std::nullopt;
    }
    const auto base = ParseInteger<u64>(text.substr(0, plus));
    const auto size = ParseInteger<u64>(text.substr(plus + 1, stride_at - (plus + 1)));
    const auto stride = ParseInteger<u32>(text.substr(
        stride_at + StrideToken.size(), modified_at - (stride_at + StrideToken.size())));
    const auto modified = text.substr(modified_at + ModifiedToken.size(),
                                      oob_at - (modified_at + ModifiedToken.size()));
    const auto oob = ParseInteger<u32>(text.substr(oob_at + OobToken.size()));
    if (!base || !size || !stride || !oob || (modified != "true" && modified != "false")) {
        return std::nullopt;
    }
    return CapturedBufferMetadata{
        .base_address = *base,
        .size = *size,
        .stride = *stride,
        .oob_records = *oob,
    };
}

bool IsRecordInBounds(const AmdGpu::Buffer& descriptor, u32 key, u32 record_size) {
    const u64 offset = u64{key} * record_size;
    const u64 size = descriptor.GetSize();
    return descriptor.base_address != 0 && offset <= size && record_size <= size - offset;
}

bool BytesAreZero(std::span<const u8> bytes) {
    return std::ranges::all_of(bytes, [](u8 value) { return value == 0; });
}

bool ReconstructVertexBuffers(ReplayBundle& bundle,
                              const std::unordered_map<std::string, std::vector<u8>>& files,
                              std::string& error) {
    if (!CopyExact(files.at("vs-buffer-descriptors.bin"), bundle.vs_buffer_descriptors,
                   "vs-buffer-descriptors.bin", error)) {
        return false;
    }
    std::array<CapturedBufferMetadata, VsBufferCount> metadata{};
    for (u32 binding = 0; binding < VsBufferCount; ++binding) {
        const auto& descriptor = bundle.vs_buffer_descriptors[binding];
        const std::string key = "b" + std::to_string(binding) + "_descriptor";
        const auto it = bundle.manifest.find(key);
        const auto parsed = it == bundle.manifest.end() ? std::optional<CapturedBufferMetadata>{}
                                                        : ParseBufferMetadata(it->second);
        if (!parsed || !descriptor.Valid() ||
            descriptor.stride != ExpectedVsBufferStrides[binding] ||
            descriptor.base_address != parsed->base_address ||
            descriptor.GetSize() != parsed->size || descriptor.stride != parsed->stride ||
            descriptor.GetSize() == 0) {
            error = key + " does not match its captured raw descriptor";
            return false;
        }
        metadata[binding] = *parsed;
        bundle.vs_buffers[binding].assign(descriptor.GetSize(), 0);
    }

    const auto first_record = RequiredInteger<u64>(bundle.manifest, "first_record", error);
    const auto unique_objects = RequiredInteger<u32>(bundle.manifest, "unique_objects", error);
    if (!first_record || !unique_objects) {
        return false;
    }
    bundle.first_record = *first_record;
    const auto& visibility = files.at("b3-list.bin");
    const u64 visibility_bytes = u64{bundle.instance_count} * ExpectedVsBufferStrides[3];
    const auto& visibility_descriptor = bundle.vs_buffer_descriptors[3];
    const u64 visibility_offset = *first_record * ExpectedVsBufferStrides[3];
    if (visibility.size() != visibility_bytes ||
        *first_record > visibility_descriptor.GetSize() / ExpectedVsBufferStrides[3] ||
        bundle.instance_count >
            visibility_descriptor.GetSize() / ExpectedVsBufferStrides[3] - *first_record ||
        visibility_offset > bundle.vs_buffers[3].size() ||
        visibility.size() > bundle.vs_buffers[3].size() - visibility_offset) {
        error = "b3-list.bin does not fit the captured visibility descriptor";
        return false;
    }
    std::memcpy(bundle.vs_buffers[3].data() + visibility_offset, visibility.data(),
                visibility.size());
    bundle.visibility = visibility;

    std::vector<u32> low24(bundle.instance_count);
    std::vector<u32> low16(bundle.instance_count);
    for (u32 record = 0; record < bundle.instance_count; ++record) {
        std::array<u32, 2> words{};
        std::memcpy(words.data(), visibility.data() + u64{record} * sizeof(words), sizeof(words));
        low24[record] = words[0] & 0x00ffffff;
        low16[record] = words[1] & 0x0000ffff;
        bundle.captured_low24.emplace(low24[record], true);
        bundle.captured_low16.emplace(low16[record], true);
        bundle.captured_pairs.emplace(u64{words[0]} | (u64{words[1]} << 32), true);
    }
    if (metadata[3].oob_records != 0) {
        error = "b3 descriptor metadata reports impossible out-of-range draw records";
        return false;
    }

    const auto scatter = [&](u32 binding, std::span<const u32> keys, std::span<const u8> payload,
                              u32 record_size, std::string_view filename) {
        if (payload.size() != u64{keys.size()} * record_size) {
            error = std::string{filename} + " has the wrong per-record payload size";
            return false;
        }
        std::unordered_map<u32, u32> first_occurrence;
        u32 oob_records{};
        for (u32 record = 0; record < keys.size(); ++record) {
            const u32 key = keys[record];
            const auto source = payload.subspan(u64{record} * record_size, record_size);
            if (!IsRecordInBounds(bundle.vs_buffer_descriptors[binding], key, record_size)) {
                ++oob_records;
                if (!BytesAreZero(source)) {
                    error = std::string{filename} +
                            " contains nonzero data for an out-of-range descriptor load";
                    return false;
                }
                continue;
            }
            const auto [it, inserted] = first_occurrence.emplace(key, record);
            if (!inserted) {
                const auto prior = payload.subspan(u64{it->second} * record_size, record_size);
                if (!std::ranges::equal(source, prior)) {
                    error = std::string{filename} +
                            " contains conflicting payloads for one repeated key";
                    return false;
                }
                continue;
            }
            std::memcpy(bundle.vs_buffers[binding].data() + u64{key} * record_size, source.data(),
                        source.size());
        }
        if (oob_records != metadata[binding].oob_records) {
            error = std::string{filename} + " out-of-range count disagrees with the manifest";
            return false;
        }
        return true;
    };
    if (!scatter(1, low24, files.at("b1-id-a.bin"), ExpectedVsBufferStrides[1], "b1-id-a.bin") ||
        !scatter(2, low24, files.at("b2-id-b.bin"), ExpectedVsBufferStrides[2], "b2-id-b.bin") ||
        !scatter(4, low24, files.at("b4-id-record.bin"), ExpectedVsBufferStrides[4],
                 "b4-id-record.bin")) {
        return false;
    }
    const auto& object_key_bytes = files.at("b0-object-keys.bin");
    const auto& objects = files.at("b0-objects.bin");
    if (object_key_bytes.size() % sizeof(u32) != 0 ||
        object_key_bytes.size() / sizeof(u32) != *unique_objects ||
        objects.size() != u64{*unique_objects} * ExpectedVsBufferStrides[0]) {
        error = "b0 object key/payload files have inconsistent sizes";
        return false;
    }
    std::vector<u32> object_keys(*unique_objects);
    if (!object_key_bytes.empty()) {
        std::memcpy(object_keys.data(), object_key_bytes.data(), object_key_bytes.size());
    }
    if (!std::ranges::is_sorted(object_keys, std::ranges::less{}) ||
        std::adjacent_find(object_keys.begin(), object_keys.end()) != object_keys.end()) {
        error = "b0-object-keys.bin is not strictly sorted and unique";
        return false;
    }
    auto expected_object_keys = low16;
    std::ranges::sort(expected_object_keys);
    expected_object_keys.erase(
        std::unique(expected_object_keys.begin(), expected_object_keys.end()),
        expected_object_keys.end());
    if (object_keys != expected_object_keys ||
        !scatter(0, object_keys, objects, ExpectedVsBufferStrides[0], "b0-objects.bin")) {
        if (error.empty()) {
            error = "b0-object-keys.bin does not match the visibility-list object keys";
        }
        return false;
    }
    return true;
}

bool ReconstructB535CandidateBank(
    ReplayBundle& bundle, const std::unordered_map<std::string, std::vector<u8>>& files,
    std::string& error) {
    const u32 present = std::ranges::count_if(B535CandidateBankFiles, [&](const char* name) {
        return files.contains(name);
    });
    if (present == 0) {
        return true;
    }
    if (present != B535CandidateBankFiles.size()) {
        error = "B535 candidate bank is incomplete";
        return false;
    }

    const auto& record_bytes = files.at("b535-input-records.bin");
    const auto& dropped_bytes = files.at("b535-dropped-records.bin");
    if (record_bytes.empty() || record_bytes.size() % (2 * sizeof(u32)) != 0 ||
        dropped_bytes.size() % (2 * sizeof(u32)) != 0) {
        error = "B535 candidate record files are not little-endian U32x2 lists";
        return false;
    }
    const u64 record_count = record_bytes.size() / (2 * sizeof(u32));
    const u64 dropped_count = dropped_bytes.size() / (2 * sizeof(u32));
    const auto expected_records = RequiredInteger<u64>(bundle.manifest, "b535_input_records", error);
    const auto expected_dropped =
        RequiredInteger<u64>(bundle.manifest, "b535_dropped_records", error);
    if (!expected_records || !expected_dropped || *expected_records != record_count ||
        *expected_dropped != dropped_count || record_count > std::numeric_limits<u32>::max()) {
        if (error.empty()) {
            error = "B535 candidate record counts disagree with the manifest";
        }
        return false;
    }

    bundle.b535_input_records.resize(record_count);
    bundle.b535_dropped_records.resize(dropped_count);
    std::memcpy(bundle.b535_input_records.data(), record_bytes.data(), record_bytes.size());
    if (!dropped_bytes.empty()) {
        std::memcpy(bundle.b535_dropped_records.data(), dropped_bytes.data(), dropped_bytes.size());
    }
    std::unordered_map<u64, bool> input_pairs;
    std::vector<u32> low24(record_count);
    std::vector<u32> low16(record_count);
    for (u32 index = 0; index < record_count; ++index) {
        const auto& pair = bundle.b535_input_records[index];
        const u64 packed = u64{pair[0]} | (u64{pair[1]} << 32);
        if (!input_pairs.emplace(packed, true).second) {
            error = "B535 candidate input list contains a duplicate pair";
            return false;
        }
        bundle.captured_pairs.emplace(packed, true);
        low24[index] = pair[0] & 0x00ffffff;
        low16[index] = pair[1] & 0x0000ffff;
    }
    for (u64 offset = 0; offset < bundle.visibility.size(); offset += 2 * sizeof(u32)) {
        std::array<u32, 2> pair{};
        std::memcpy(pair.data(), bundle.visibility.data() + offset, sizeof(pair));
        const u64 packed = u64{pair[0]} | (u64{pair[1]} << 32);
        if (!input_pairs.contains(packed)) {
            error = "baseline visibility pair is absent from the aligned B535 input bank";
            return false;
        }
    }
    for (const auto& pair : bundle.b535_dropped_records) {
        const u64 packed = u64{pair[0]} | (u64{pair[1]} << 32);
        if (!input_pairs.contains(packed)) {
            error = "B535 dropped record is absent from the captured input bank";
            return false;
        }
    }

    const auto scatter = [&](u32 binding, std::span<const u32> keys,
                              std::span<const u8> payload, u32 record_size,
                              std::unordered_map<u32, bool>& captured,
                              std::string_view filename, std::string_view oob_key) {
        if (payload.size() != u64{keys.size()} * record_size) {
            error = std::string{filename} + " has the wrong per-record payload size";
            return false;
        }
        const auto expected_oob = RequiredInteger<u32>(bundle.manifest, oob_key, error);
        if (!expected_oob) {
            return false;
        }
        std::unordered_map<u32, u32> first_occurrence;
        u32 actual_oob{};
        for (u32 record = 0; record < keys.size(); ++record) {
            const u32 key = keys[record];
            const auto source = payload.subspan(u64{record} * record_size, record_size);
            const auto [first, inserted] = first_occurrence.emplace(key, record);
            if (!inserted) {
                const auto prior = payload.subspan(u64{first->second} * record_size, record_size);
                if (!std::ranges::equal(source, prior)) {
                    error = std::string{filename} +
                            " contains conflicting payloads for one repeated key";
                    return false;
                }
                continue;
            }
            if (!IsRecordInBounds(bundle.vs_buffer_descriptors[binding], key, record_size)) {
                ++actual_oob;
                if (!BytesAreZero(source)) {
                    error = std::string{filename} +
                            " contains nonzero data for an out-of-range descriptor load";
                    return false;
                }
                captured.emplace(key, true);
                continue;
            }
            auto target = std::span<u8>{bundle.vs_buffers[binding]}.subspan(
                u64{key} * record_size, record_size);
            if (captured.contains(key) && !std::ranges::equal(source, target)) {
                error = std::string{filename} + " conflicts with the baseline row for its key";
                return false;
            }
            std::memcpy(target.data(), source.data(), source.size());
            captured.emplace(key, true);
        }
        if (actual_oob != *expected_oob) {
            error = std::string{filename} + " out-of-range count disagrees with the manifest";
            return false;
        }
        return true;
    };

    auto captured_b1 = bundle.captured_low24;
    auto captured_b2 = bundle.captured_low24;
    auto captured_b4 = bundle.captured_low24;
    if (!scatter(1, low24, files.at("b535-input-b1-id-a.bin"),
                 ExpectedVsBufferStrides[1], captured_b1,
                 "b535-input-b1-id-a.bin", "b535_input_b1_oob") ||
        !scatter(2, low24, files.at("b535-input-b2-id-b.bin"),
                 ExpectedVsBufferStrides[2], captured_b2,
                 "b535-input-b2-id-b.bin", "b535_input_b2_oob") ||
        !scatter(4, low24, files.at("b535-input-b4-id-record.bin"),
                 ExpectedVsBufferStrides[4], captured_b4,
                 "b535-input-b4-id-record.bin", "b535_input_b4_oob")) {
        return false;
    }
    for (const u32 key : low24) {
        bundle.captured_low24.emplace(key, true);
    }

    const auto& object_key_bytes = files.at("b535-input-b0-object-keys.bin");
    const auto& object_bytes = files.at("b535-input-b0-objects.bin");
    if (object_key_bytes.size() % sizeof(u32) != 0) {
        error = "B535 object-key bank is not a U32 list";
        return false;
    }
    std::vector<u32> object_keys(object_key_bytes.size() / sizeof(u32));
    if (!object_key_bytes.empty()) {
        std::memcpy(object_keys.data(), object_key_bytes.data(), object_key_bytes.size());
    }
    auto expected_object_keys = low16;
    std::ranges::sort(expected_object_keys);
    expected_object_keys.erase(
        std::unique(expected_object_keys.begin(), expected_object_keys.end()),
        expected_object_keys.end());
    if (object_keys != expected_object_keys ||
        !scatter(0, object_keys, object_bytes, ExpectedVsBufferStrides[0],
                 bundle.captured_low16, "b535-input-b0-objects.bin", "b535_input_b0_oob")) {
        if (error.empty()) {
            error = "B535 object-key bank does not match the input record object keys";
        }
        return false;
    }
    return true;
}

std::optional<std::vector<AtlasRegion>> ReadAtlasRegions(const std::filesystem::path& path,
                                                         std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open atlas-regions.tsv";
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(input, line)) {
        error = "atlas-regions.tsv is empty";
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != AtlasHeader) {
        error = "atlas-regions.tsv has an unsupported header";
        return std::nullopt;
    }
    std::vector<AtlasRegion> regions;
    u32 line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            error = "atlas-regions.tsv has an empty data row";
            return std::nullopt;
        }
        std::array<std::string_view, 13> fields{};
        std::string_view remaining = line;
        for (u32 index = 0; index < fields.size(); ++index) {
            const auto separator = remaining.find('\t');
            fields[index] =
                separator == std::string_view::npos ? remaining : remaining.substr(0, separator);
            if (index + 1 == fields.size()) {
                if (separator != std::string_view::npos) {
                    error = "atlas-regions.tsv has too many fields on row " +
                            std::to_string(line_number);
                    return std::nullopt;
                }
            } else {
                if (separator == std::string_view::npos) {
                    error = "atlas-regions.tsv has too few fields on row " +
                            std::to_string(line_number);
                    return std::nullopt;
                }
                remaining.remove_prefix(separator + 1);
            }
        }
        AtlasRegion region{};
        const auto slot = ParseInteger<u32>(fields[0]);
        const auto low24 = ParseInteger<u32>(fields[1]);
        const auto brick_x = ParseInteger<s32>(fields[2]);
        const auto brick_y = ParseInteger<s32>(fields[3]);
        const auto brick_z = ParseInteger<s32>(fields[4]);
        const auto source_x = ParseInteger<s32>(fields[5]);
        const auto source_y = ParseInteger<s32>(fields[6]);
        const auto source_z = ParseInteger<s32>(fields[7]);
        const auto output_offset = ParseInteger<u64>(fields[8]);
        const auto byte_size = ParseInteger<u64>(fields[9]);
        const auto width = ParseInteger<u32>(fields[10]);
        const auto height = ParseInteger<u32>(fields[11]);
        const auto depth = ParseInteger<u32>(fields[12]);
        if (!slot || !low24 || !brick_x || !brick_y || !brick_z || !source_x || !source_y ||
            !source_z || !output_offset || !byte_size || !width || !height || !depth) {
            error = "atlas-regions.tsv has an invalid number on row " + std::to_string(line_number);
            return std::nullopt;
        }
        region = {*slot,     *low24,         *brick_x,   *brick_y, *brick_z, *source_x, *source_y,
                  *source_z, *output_offset, *byte_size, *width,   *height,  *depth};
        regions.push_back(region);
    }
    if (!input.eof()) {
        error = "failed while reading atlas-regions.tsv";
        return std::nullopt;
    }
    return regions;
}

bool ValidateFileIndex(const std::filesystem::path& bundle_path, const ReplayBundle& bundle,
                       std::string& error) {
    auto index_bytes = ReadBytes(bundle_path / "files.tsv", error);
    if (!index_bytes) {
        return false;
    }
    const auto expected_index_bytes =
        RequiredInteger<u64>(bundle.manifest, "files_tsv_bytes", error);
    const auto expected_aggregate =
        RequiredInteger<u64>(bundle.manifest, "aggregate_sorted_hash", error);
    if (!expected_index_bytes || !expected_aggregate) {
        return false;
    }
    if (index_bytes->size() != *expected_index_bytes ||
        HashBytes(*index_bytes) != *expected_aggregate) {
        error = "files.tsv size or aggregate hash does not match manifest";
        return false;
    }
    std::ifstream input(bundle_path / "files.tsv", std::ios::binary);
    if (!input) {
        error = "cannot open files.tsv";
        return false;
    }
    std::string line;
    if (!std::getline(input, line)) {
        error = "files.tsv is empty";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != "file\tsize\thash") {
        error = "files.tsv has an unsupported header";
        return false;
    }
    std::string prior;
    u32 entries{};
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto first = line.find('\t');
        const auto second =
            first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            line.find('\t', second + 1) != std::string::npos) {
            error = "files.tsv has a malformed row";
            return false;
        }
        const std::string name = line.substr(0, first);
        if (name.empty() || name == "manifest.tsv" || name == "files.tsv" ||
            std::filesystem::path{name}.filename().string() != name ||
            (!prior.empty() && prior >= name)) {
            error = "files.tsv is unsafe, self-referential, or unsorted";
            return false;
        }
        const auto size =
            ParseInteger<u64>(std::string_view{line}.substr(first + 1, second - first - 1));
        const auto hash = ParseInteger<u64>(std::string_view{line}.substr(second + 1));
        if (!size || !hash) {
            error = "files.tsv has an invalid size/hash";
            return false;
        }
        auto bytes = ReadBytes(bundle_path / name, error);
        if (!bytes || bytes->size() != *size || HashBytes(*bytes) != *hash) {
            if (error.empty()) {
                error = name + " does not match files.tsv";
            }
            return false;
        }
        prior = name;
        ++entries;
    }
    const auto expected_entries =
        RequiredInteger<u32>(bundle.manifest, "file_index_entries", error);
    if (!expected_entries || entries != *expected_entries) {
        if (error.empty()) {
            error = "files.tsv entry count does not match manifest";
        }
        return false;
    }
    return true;
}

LoadResult LoadBundle(const std::filesystem::path& bundle_path) {
    LoadResult result{.stage = "required_files"};
    constexpr std::array RequiredFiles{
        "manifest.tsv",
        "files.tsv",
        "draw.bin",
        "indices.bin",
        "vs.spv",
        "fs.spv",
        "b0-object-keys.bin",
        "b0-objects.bin",
        "b1-id-a.bin",
        "b2-id-b.bin",
        "b3-list.bin",
        "b4-id-record.bin",
        "vs-buffer-descriptors.bin",
        "vs-srt-flat.bin",
        "vs-transform.bin",
        "bda-page-transform.bin",
        "push-data.bin",
        "graphics-key.bin",
        "dynamic-state.tsv",
        "render-state.tsv",
        "sampler.bin",
        "atlas-regions.tsv",
        "atlas-bricks-r8.bin",
        "fs-image1-r8.bin",
        "fs-srt-flat.bin",
        "fs-bda-constants.bin",
        "bda-page-fs-srt.bin",
        "vs-interface.bin",
        "vs-render-target-index.bin",
        "vs-validity.bin",
        "post-color.bin",
        "post-depth.bin",
        "pre-color.bin",
        "pre-depth.bin",
    };
    for (const auto* filename : RequiredFiles) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(bundle_path / filename, ec) || ec) {
            result.missing_files.emplace_back(filename);
        }
    }
    if (!result.missing_files.empty()) {
        result.error = "capture bundle is incomplete";
        return result;
    }

    result.stage = "metadata";
    std::string error;
    auto manifest = ReadKeyValues(bundle_path / "manifest.tsv", error);
    auto dynamic_state = ReadKeyValues(bundle_path / "dynamic-state.tsv", error);
    auto render_state = ReadKeyValues(bundle_path / "render-state.tsv", error);
    if (!manifest || !dynamic_state || !render_state) {
        result.error = std::move(error);
        return result;
    }
    ReplayBundle bundle{
        .manifest = std::move(*manifest),
        .dynamic_state = std::move(*dynamic_state),
        .render_state = std::move(*render_state),
    };
    const auto schema = RequiredInteger<u32>(bundle.manifest, "schema", error);
    const auto instances = RequiredInteger<u32>(bundle.manifest, "instances", error);
    const auto index_size = RequiredInteger<u32>(bundle.manifest, "index_size", error);
    const auto expected_validity =
        RequiredInteger<u32>(bundle.manifest, "expected_validity_mask", error);
    const auto invalid_vertices = RequiredInteger<u32>(bundle.manifest, "invalid_vertices", error);
    const auto fs_constant_address =
        RequiredInteger<u64>(bundle.manifest, "fs_constant_address", error);
    if (!schema || !instances || !index_size || !expected_validity || !invalid_vertices ||
        !fs_constant_address) {
        result.error = std::move(error);
        return result;
    }
    if ((*schema != 2 && *schema != 3 && *schema != 4) || *instances == 0 ||
        *instances > MaximumReplayInstances ||
        *index_size != sizeof(u16) || *expected_validity != ExpectedValidityMask ||
        *invalid_vertices != 0 || *fs_constant_address == 0) {
        result.error = "capture manifest does not describe a supported 14-index replay contract";
        return result;
    }
    if (*schema == 4) {
        const auto source = bundle.manifest.find("vs_indexed_capture_source");
        if (source == bundle.manifest.end() || source->second != "bound_vulkan") {
            result.error =
                "schema 4 capture requires vs_indexed_capture_source=bound_vulkan";
            return result;
        }
    }
    bundle.fs_constant_address = *fs_constant_address;
    bundle.instance_count = *instances;

    const auto atlas_dimensions_it = bundle.manifest.find("atlas_dimensions");
    const auto image1_dimensions_it = bundle.manifest.find("fs_image1_dimensions");
    if (atlas_dimensions_it == bundle.manifest.end() ||
        image1_dimensions_it == bundle.manifest.end()) {
        result.error = "capture manifest is missing image dimensions";
        return result;
    }
    const auto atlas_dimensions = ParseDimensions(atlas_dimensions_it->second);
    const auto image1_dimensions = ParseDimensions(image1_dimensions_it->second);
    if (!atlas_dimensions || !image1_dimensions ||
        *atlas_dimensions != std::array<u32, 3>{2048, 2048, 128} ||
        *image1_dimensions != std::array<u32, 3>{128, 128, 1}) {
        result.error = "capture images do not match the exact 2048x2048x128 and 128x128 contract";
        return result;
    }
    bundle.atlas_width = (*atlas_dimensions)[0];
    bundle.atlas_height = (*atlas_dimensions)[1];
    bundle.atlas_depth = (*atlas_dimensions)[2];
    bundle.image1_width = (*image1_dimensions)[0];
    bundle.image1_height = (*image1_dimensions)[1];

    const auto render_width = RequiredInteger<u32>(bundle.render_state, "render_width", error);
    const auto render_height = RequiredInteger<u32>(bundle.render_state, "render_height", error);
    const auto render_layers = RequiredInteger<u32>(bundle.render_state, "render_layers", error);
    const auto color_attachments =
        RequiredInteger<u32>(bundle.render_state, "color_attachment_count", error);
    if (!render_width || !render_height || !render_layers || !color_attachments) {
        result.error = std::move(error);
        return result;
    }
    if (*render_width == 0 || *render_height == 0 || *render_layers != 1 ||
        *color_attachments != 1) {
        result.error = "render-state.tsv is outside the supported single-color replay contract";
        return result;
    }
    bundle.render_width = *render_width;
    bundle.render_height = *render_height;
    bundle.render_layers = *render_layers;

    auto parsed_dynamic = ParseDynamicState(bundle.dynamic_state, error);
    const auto color_clear = RequiredBool(bundle.render_state, "color0_is_clear", error);
    const auto color_clear_raw = RequiredU32x4(bundle.render_state, "color0_clear_raw", error);
    const auto has_depth = RequiredBool(bundle.render_state, "depth_has_depth", error);
    const auto depth_clear = RequiredBool(bundle.render_state, "depth_clear", error);
    const auto has_stencil = RequiredBool(bundle.render_state, "depth_has_stencil", error);
    const auto stencil_clear = RequiredBool(bundle.render_state, "stencil_clear", error);
    const auto depth_clear_raw = RequiredU32x4(bundle.render_state, "depth_clear_raw", error);
    if (!parsed_dynamic || !color_clear || !color_clear_raw || !has_depth || !depth_clear ||
        !has_stencil || !stencil_clear || !depth_clear_raw) {
        result.error = std::move(error);
        return result;
    }
    bundle.dynamic = std::move(*parsed_dynamic);
    bundle.render = {
        .color_clear = *color_clear,
        .color_clear_raw = *color_clear_raw,
        .has_depth = *has_depth,
        .depth_clear = *depth_clear,
        .has_stencil = *has_stencil,
        .stencil_clear = *stencil_clear,
        .depth_clear_raw = *depth_clear_raw,
    };
    if (!bundle.render.has_depth || bundle.render.has_stencil ||
        bundle.dynamic.stencil_test_enabled || bundle.dynamic.feedback_loop_enabled) {
        result.error = "captured depth/stencil or feedback-loop state is outside this exact replay";
        return result;
    }

    const auto validate_view = [&](const KeyValues& values, std::string_view prefix,
                                   u32 expected_binding_type, AmdGpu::ImageType expected_type,
                                   vk::Format expected_format, u32 expected_layers,
                                   bool expected_storage, bool fs_view,
                                   std::array<vk::ComponentSwizzle, 4>& swizzle,
                                   vk::ImageLayout& layout) {
        const std::string base{prefix};
        const auto binding = RequiredInteger<u32>(values, base + "_binding_type", error);
        const auto type = RequiredInteger<u32>(values, base + "_view_type", error);
        const auto format = RequiredInteger<u32>(values, base + "_view_format", error);
        const auto base_level = RequiredInteger<u32>(values, base + "_view_base_level", error);
        const auto levels = RequiredInteger<u32>(values, base + "_view_levels", error);
        const auto base_layer = RequiredInteger<u32>(values, base + "_view_base_layer", error);
        const auto layers = RequiredInteger<u32>(values, base + "_view_layers", error);
        const auto min_lod = RequiredInteger<u32>(values, base + "_view_min_lod", error);
        const auto storage = RequiredBool(values, base + "_view_is_storage", error);
        const auto mapping = RequiredSwizzle(values, base + "_view_swizzle", error);
        const auto format_name = values.find(base + "_view_format_name");
        const auto backing_layout = values.find(base + (fs_view ? "_layout" : "_backing_layout"));
        const auto backing_layout_name =
            values.find(base + (fs_view ? "_layout_name" : "_backing_layout_name"));
        if (!binding || !type || !format || !base_level || !levels || !base_layer || !layers ||
            !min_lod || !storage || !mapping || format_name == values.end() ||
            backing_layout == values.end() || backing_layout_name == values.end()) {
            return false;
        }
        const auto parsed_layout = ParseInteger<u32>(backing_layout->second);
        if (*binding != expected_binding_type || *type != static_cast<u32>(expected_type) ||
            *format != static_cast<u32>(expected_format) ||
            format_name->second != vk::to_string(expected_format) || *base_level != 0 ||
            *levels != 1 || *base_layer != 0 || *layers != expected_layers || *min_lod != 0 ||
            *storage != expected_storage || !parsed_layout ||
            backing_layout_name->second !=
                vk::to_string(static_cast<vk::ImageLayout>(*parsed_layout))) {
            error = base + " has an unsupported image-view contract";
            return false;
        }
        if (fs_view) {
            const auto descriptor = values.find(base + "_descriptor_raw");
            const auto words = descriptor == values.end()
                                   ? std::optional<std::vector<std::string_view>>{}
                                   : SplitCsv(descriptor->second, 8);
            if (!words) {
                error = base + " is missing its raw eight-dword descriptor";
                return false;
            }
            for (const auto word : *words) {
                if (!ParseInteger<u32>(word)) {
                    error = base + " has an invalid raw descriptor";
                    return false;
                }
            }
        }
        swizzle = *mapping;
        layout = static_cast<vk::ImageLayout>(*parsed_layout);
        return true;
    };
    if (!validate_view(bundle.manifest, "fs_image0", 0, AmdGpu::ImageType::Color3D,
                       vk::Format::eR8Unorm, 1, false, true, bundle.atlas_swizzle,
                       bundle.atlas_layout) ||
        !validate_view(bundle.manifest, "fs_image1", 0, AmdGpu::ImageType::Color2D,
                       vk::Format::eR8Unorm, 1, false, true, bundle.image1_swizzle,
                       bundle.image1_layout) ||
        !validate_view(bundle.render_state, "color0", 2,
                       bundle.render_layers > 1 ? AmdGpu::ImageType::Color2DArray
                                                : AmdGpu::ImageType::Color2D,
                       vk::Format::eR32G32Uint, bundle.render_layers, false, false,
                       bundle.color_swizzle, bundle.color_layout) ||
        !validate_view(
            bundle.render_state, "depth", 3,
            bundle.render_layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D,
            vk::Format::eD32Sfloat, bundle.render_layers, bundle.dynamic.depth_write_enabled, false,
            bundle.depth_swizzle, bundle.depth_layout)) {
        result.error = std::move(error);
        return result;
    }
    const auto color_render_layout =
        RequiredInteger<u32>(bundle.render_state, "color0_layout", error);
    const auto depth_render_layout =
        RequiredInteger<u32>(bundle.render_state, "depth_layout", error);
    const auto color_render_layout_name = bundle.render_state.find("color0_layout_name");
    const auto depth_render_layout_name = bundle.render_state.find("depth_layout_name");
    if (!color_render_layout || !depth_render_layout ||
        color_render_layout_name == bundle.render_state.end() ||
        depth_render_layout_name == bundle.render_state.end()) {
        result.error = std::move(error);
        return result;
    }
    const auto exact_color_layout = static_cast<vk::ImageLayout>(*color_render_layout);
    const auto exact_depth_layout = static_cast<vk::ImageLayout>(*depth_render_layout);
    if (color_render_layout_name->second != vk::to_string(exact_color_layout) ||
        depth_render_layout_name->second != vk::to_string(exact_depth_layout) ||
        bundle.color_layout != exact_color_layout || bundle.depth_layout != exact_depth_layout) {
        result.error = "render attachment and backing layouts do not describe one exact state";
        return result;
    }
    bundle.color_layout = exact_color_layout;
    bundle.depth_layout = exact_depth_layout;
    const auto sampled_layout_ok = [](vk::ImageLayout layout) {
        return layout == vk::ImageLayout::eShaderReadOnlyOptimal ||
               layout == vk::ImageLayout::eGeneral || layout == vk::ImageLayout::eReadOnlyOptimal;
    };
    const auto color_layout_ok = [](vk::ImageLayout layout) {
        return layout == vk::ImageLayout::eColorAttachmentOptimal ||
               layout == vk::ImageLayout::eAttachmentOptimal || layout == vk::ImageLayout::eGeneral;
    };
    const auto depth_layout_ok = [](vk::ImageLayout layout) {
        return layout == vk::ImageLayout::eDepthAttachmentOptimal ||
               layout == vk::ImageLayout::eDepthStencilAttachmentOptimal ||
               layout == vk::ImageLayout::eAttachmentOptimal || layout == vk::ImageLayout::eGeneral;
    };
    if (!sampled_layout_ok(bundle.atlas_layout) || !sampled_layout_ok(bundle.image1_layout) ||
        !color_layout_ok(bundle.color_layout) || !depth_layout_ok(bundle.depth_layout)) {
        result.error = "captured image layouts are unsupported by the exact replay path";
        return result;
    }
    const auto image0_resource =
        RequiredInteger<u32>(bundle.manifest, "fs_image0_shader_resource_index", error);
    const auto image1_resource =
        RequiredInteger<u32>(bundle.manifest, "fs_image1_shader_resource_index", error);
    const auto image0_flat =
        RequiredInteger<u32>(bundle.manifest, "fs_image0_flattened_binding_index", error);
    const auto image1_flat =
        RequiredInteger<u32>(bundle.manifest, "fs_image1_flattened_binding_index", error);
    const auto image0_sharp = RequiredInteger<u32>(bundle.manifest, "fs_image0_sharp_index", error);
    const auto image1_sharp = RequiredInteger<u32>(bundle.manifest, "fs_image1_sharp_index", error);
    if (!image0_resource || !image1_resource || !image0_flat || !image1_flat || !image0_sharp ||
        !image1_sharp) {
        result.error = std::move(error);
        return result;
    }
    if (*image0_resource == *image1_resource || *image0_flat != 0 || *image1_flat != 1 ||
        *image0_sharp != 16 || *image1_sharp != 24) {
        result.error = "FS image resource order does not match captured bindings 3 and 4";
        return result;
    }

    result.stage = "binary_payloads";
    std::unordered_map<std::string, std::vector<u8>> files;
    for (const auto* filename : RequiredFiles) {
        if (std::string_view{filename}.ends_with(".bin") ||
            std::string_view{filename}.ends_with(".spv") ||
            std::string_view{filename}.ends_with(".tsv")) {
            auto bytes = ReadBytes(bundle_path / filename, error);
            if (!bytes) {
                result.error = std::move(error);
                return result;
            }
            files.emplace(filename, std::move(*bytes));
        }
    }
    const auto pipeline_cache_path = bundle_path / "vk-pipeline-cache.bin";
    std::error_code pipeline_cache_ec;
    if (std::filesystem::is_regular_file(pipeline_cache_path, pipeline_cache_ec) &&
        !pipeline_cache_ec) {
        auto bytes = ReadBytes(pipeline_cache_path, error);
        if (!bytes) {
            result.error = std::move(error);
            return result;
        }
        files.emplace("vk-pipeline-cache.bin", std::move(*bytes));
    }
    u32 b535_bank_files{};
    for (const char* filename : B535CandidateBankFiles) {
        std::error_code bank_ec;
        b535_bank_files +=
            std::filesystem::is_regular_file(bundle_path / filename, bank_ec) && !bank_ec;
    }
    if ((*schema >= 3 && b535_bank_files != B535CandidateBankFiles.size()) ||
        (b535_bank_files != 0 && b535_bank_files != B535CandidateBankFiles.size())) {
        result.error = "capture bundle contains an incomplete B535 candidate bank";
        return result;
    }
    if (b535_bank_files == B535CandidateBankFiles.size()) {
        for (const char* filename : B535CandidateBankFiles) {
            auto bytes = ReadBytes(bundle_path / filename, error);
            if (!bytes) {
                result.error = std::move(error);
                return result;
            }
            files.emplace(filename, std::move(*bytes));
        }
    }

    const auto check_hash = [&](std::string_view filename, std::string_view key = {}) {
        return ValidateHash(bundle, filename, files.at(std::string{filename}), error, key);
    };
    constexpr std::array StandardHashFiles{
        "draw.bin",
        "indices.bin",
        "vs.spv",
        "fs.spv",
        "b0-object-keys.bin",
        "b0-objects.bin",
        "b1-id-a.bin",
        "b2-id-b.bin",
        "b3-list.bin",
        "b4-id-record.bin",
        "vs-buffer-descriptors.bin",
        "vs-srt-flat.bin",
        "vs-transform.bin",
        "bda-page-transform.bin",
        "push-data.bin",
        "graphics-key.bin",
        "dynamic-state.tsv",
        "render-state.tsv",
        "sampler.bin",
        "atlas-bricks-r8.bin",
        "fs-image1-r8.bin",
        "fs-srt-flat.bin",
        "fs-bda-constants.bin",
        "bda-page-fs-srt.bin",
        "post-color.bin",
        "post-depth.bin",
        "pre-color.bin",
        "pre-depth.bin",
    };
    for (const auto* filename : StandardHashFiles) {
        if (!check_hash(filename)) {
            result.error = std::move(error);
            return result;
        }
    }
    if (files.contains("vk-pipeline-cache.bin") && !check_hash("vk-pipeline-cache.bin")) {
        result.error = std::move(error);
        return result;
    }
    if (!check_hash("vs-interface.bin", "interface_values_hash") ||
        !check_hash("vs-render-target-index.bin", "layer_hash") ||
        !check_hash("vs-validity.bin", "validity_hash")) {
        result.error = std::move(error);
        return result;
    }
    if (!check_hash("atlas-regions.tsv") || !check_hash("files.tsv")) {
        result.error = std::move(error);
        return result;
    }
    if (!ValidateFileIndex(bundle_path, bundle, error)) {
        result.error = std::move(error);
        return result;
    }

    if (!CopyExact(files.at("draw.bin"), bundle.draw, "draw.bin", error) ||
        !CopyExact(files.at("graphics-key.bin"), bundle.graphics_key, "graphics-key.bin", error) ||
        !CopyExact(files.at("push-data.bin"), bundle.push_data, "push-data.bin", error) ||
        !CopyExact(files.at("sampler.bin"), bundle.sampler, "sampler.bin", error)) {
        result.error = std::move(error);
        return result;
    }
    if (bundle.sampler.border_color_type == AmdGpu::BorderColor::Custom) {
        result.error = "custom sampler border color was not captured";
        return result;
    }
    const auto sampler_clamp_x = RequiredInteger<u32>(bundle.manifest, "sampler_clamp_x", error);
    const auto sampler_clamp_y = RequiredInteger<u32>(bundle.manifest, "sampler_clamp_y", error);
    const auto sampler_clamp_z = RequiredInteger<u32>(bundle.manifest, "sampler_clamp_z", error);
    const auto sampler_mag = RequiredInteger<u32>(bundle.manifest, "sampler_xy_mag_filter", error);
    const auto sampler_min = RequiredInteger<u32>(bundle.manifest, "sampler_xy_min_filter", error);
    const auto sampler_z = RequiredInteger<u32>(bundle.manifest, "sampler_z_filter", error);
    const auto sampler_mip = RequiredInteger<u32>(bundle.manifest, "sampler_mip_filter", error);
    if (!sampler_clamp_x || !sampler_clamp_y || !sampler_clamp_z || !sampler_mag || !sampler_min ||
        !sampler_z || !sampler_mip) {
        result.error = std::move(error);
        return result;
    }
    if (*sampler_clamp_x != static_cast<u32>(bundle.sampler.clamp_x.Value()) ||
        *sampler_clamp_y != static_cast<u32>(bundle.sampler.clamp_y.Value()) ||
        *sampler_clamp_z != static_cast<u32>(bundle.sampler.clamp_z.Value()) ||
        *sampler_mag != static_cast<u32>(bundle.sampler.xy_mag_filter.Value()) ||
        *sampler_min != static_cast<u32>(bundle.sampler.xy_min_filter.Value()) ||
        *sampler_z != static_cast<u32>(bundle.sampler.z_filter.Value()) ||
        *sampler_mip != static_cast<u32>(bundle.sampler.mip_filter.Value())) {
        result.error = "sampler.bin does not match its decoded manifest state";
        return result;
    }
    if (bundle.manifest.contains("effective_sampler_raw")) {
        const auto effective_valid =
            RequiredBool(bundle.manifest, "effective_sampler_valid", error);
        const auto effective_compare =
            RequiredBool(bundle.manifest, "effective_sampler_compare", error);
        const auto effective_raw = RequiredU32x4(bundle.manifest, "effective_sampler_raw", error);
        if (!effective_valid || !effective_compare || !effective_raw) {
            result.error = std::move(error);
            return result;
        }
        if (!*effective_valid) {
            result.error = "capture did not identify the effective live sampler";
            return result;
        }
        static_assert(sizeof(bundle.sampler) == sizeof(*effective_raw));
        std::memcpy(&bundle.sampler, effective_raw->data(), sizeof(bundle.sampler));
        bundle.sampler_is_compare = *effective_compare;
        if (bundle.sampler.border_color_type == AmdGpu::BorderColor::Custom) {
            result.error = "effective custom sampler border color was not captured";
            return result;
        }
    }
    if (bundle.draw.index_count != ExpectedIndexCount ||
        bundle.draw.instance_count != bundle.instance_count || bundle.draw.first_index != 0 ||
        bundle.draw.vertex_offset != 0 || bundle.draw.first_instance != 0) {
        result.error = "draw.bin is not the exact captured 14-index command";
        return result;
    }
    const auto fragment_stage = static_cast<u32>(Shader::LogicalStage::Fragment);
    const auto vertex_stage = static_cast<u32>(Shader::LogicalStage::Vertex);
    bool stage_contract = bundle.graphics_key.stage_hashes[fragment_stage] != 0 &&
                          bundle.graphics_key.stage_hashes[vertex_stage] != 0;
    for (u32 stage = 0; stage < bundle.graphics_key.stage_hashes.size(); ++stage) {
        if (stage != fragment_stage && stage != vertex_stage) {
            stage_contract &= bundle.graphics_key.stage_hashes[stage] == 0;
        }
    }
    stage_contract &=
        bundle.graphics_key.prim_type == AmdGpu::PrimitiveType::TriangleStrip &&
        bundle.graphics_key.mrt_mask == 1 && bundle.graphics_key.num_color_attachments == 1 &&
        bundle.graphics_key.patch_control_points == 0 && bundle.graphics_key.num_samples == 1 &&
        bundle.graphics_key.depth_samples == 1 && bundle.graphics_key.color_samples[0] == 1 &&
        bundle.graphics_key.color_buffers[0].data_format == AmdGpu::DataFormat::Format32_32 &&
        bundle.graphics_key.color_buffers[0].num_format == AmdGpu::NumberFormat::Uint &&
        bundle.graphics_key.z_format == AmdGpu::DepthBuffer::ZFormat::Z32Float &&
        bundle.graphics_key.stencil_format == AmdGpu::DepthBuffer::StencilFormat::Invalid;
    stage_contract &= !bundle.graphics_key.blend_controls[0].enable;
    if (!stage_contract || bundle.dynamic.viewports.size() != 1 ||
        bundle.dynamic.scissors.size() != 1 || bundle.dynamic.primitive_restart_enabled) {
        result.error = "captured pipeline is outside the exact ce3 VS/FS triangle-strip contract";
        return result;
    }
    if (files.at("indices.bin").size() != u64{ExpectedIndexCount} * *index_size) {
        result.error = "indices.bin has the wrong size";
        return result;
    }
    for (u32 index = 0; index < ExpectedIndexCount; ++index) {
        u16 value{};
        std::memcpy(&value, files.at("indices.bin").data() + u64{index} * sizeof(value),
                    sizeof(value));
        if (value != ExpectedIndices[index]) {
            result.error = "indices.bin is not the exact ce3 14-index strip";
            return result;
        }
    }

    const u64 vertex_count = u64{bundle.instance_count} * VerticesPerInstance;
    const auto& interface_bytes = files.at("vs-interface.bin");
    const auto& layer_bytes = files.at("vs-render-target-index.bin");
    const auto& validity_bytes = files.at("vs-validity.bin");
    if (interface_bytes.size() != vertex_count * InterfaceWordsPerVertex * sizeof(u32) ||
        layer_bytes.size() != vertex_count * sizeof(u32) ||
        validity_bytes.size() != vertex_count * sizeof(u32)) {
        result.error = "captured post-VS interface files have inconsistent sizes";
        return result;
    }
    bundle.post_vs_records.resize(vertex_count * ReplayWordsPerVertex);
    for (u64 vertex = 0; vertex < vertex_count; ++vertex) {
        u32 validity{};
        std::memcpy(&validity, validity_bytes.data() + vertex * sizeof(u32), sizeof(u32));
        if (validity != ExpectedValidityMask) {
            result.error = "vs-validity.bin contains an incomplete vertex record";
            return result;
        }
        std::memcpy(bundle.post_vs_records.data() + vertex * ReplayWordsPerVertex,
                    interface_bytes.data() + vertex * InterfaceWordsPerVertex * sizeof(u32),
                    InterfaceWordsPerVertex * sizeof(u32));
        u32 layer{};
        std::memcpy(&layer, layer_bytes.data() + vertex * sizeof(u32), sizeof(u32));
        if (layer >= bundle.render_layers) {
            result.error = "vs-render-target-index.bin references an uncaptured render layer";
            return result;
        }
        std::memcpy(bundle.post_vs_records.data() + vertex * ReplayWordsPerVertex +
                        InterfaceWordsPerVertex,
                    layer_bytes.data() + vertex * sizeof(u32), sizeof(u32));
    }

    const auto fs_words = RequiredInteger<u64>(bundle.manifest, "fs_spirv_words", error);
    const auto vs_words = RequiredInteger<u64>(bundle.manifest, "vs_spirv_words", error);
    const auto push_bytes = RequiredInteger<u64>(bundle.manifest, "push_data_bytes", error);
    const auto key_bytes = RequiredInteger<u64>(bundle.manifest, "graphics_key_bytes", error);
    const auto sampler_bytes = RequiredInteger<u64>(bundle.manifest, "sampler_bytes", error);
    if (!fs_words || !vs_words || !push_bytes || !key_bytes || !sampler_bytes) {
        result.error = std::move(error);
        return result;
    }
    constexpr u32 SpirvMagic = 0x07230203;
    u32 fs_magic{};
    u32 vs_magic{};
    if (files.at("fs.spv").size() >= sizeof(u32)) {
        std::memcpy(&fs_magic, files.at("fs.spv").data(), sizeof(u32));
    }
    if (files.at("vs.spv").size() >= sizeof(u32)) {
        std::memcpy(&vs_magic, files.at("vs.spv").data(), sizeof(u32));
    }
    if (files.at("fs.spv").size() != *fs_words * sizeof(u32) || fs_magic != SpirvMagic ||
        files.at("vs.spv").size() != *vs_words * sizeof(u32) || vs_magic != SpirvMagic ||
        *push_bytes != sizeof(Shader::PushData) || *key_bytes != sizeof(GraphicsPipelineKey) ||
        *sampler_bytes != sizeof(AmdGpu::Sampler)) {
        result.error = "shader or ABI payload size does not match this shadPS4 build";
        return result;
    }

    auto regions = ReadAtlasRegions(bundle_path / "atlas-regions.tsv", error);
    const auto unique_bricks = RequiredInteger<u32>(bundle.manifest, "atlas_unique_bricks", error);
    const auto bricks_bytes = RequiredInteger<u64>(bundle.manifest, "atlas_bricks_bytes", error);
    const auto variable_bytes = RequiredBool(bundle.manifest, "atlas_brick_bytes_variable", error);
    const auto halo_xy_policy = bundle.manifest.find("atlas_halo_xy_policy");
    const auto halo_z_policy = bundle.manifest.find("atlas_halo_z_policy");
    if (!regions || !unique_bricks || !bricks_bytes || !variable_bytes ||
        halo_xy_policy == bundle.manifest.end() || halo_z_policy == bundle.manifest.end()) {
        result.error = std::move(error);
        return result;
    }
    if (!*variable_bytes || halo_xy_policy->second != "clip_to_image" ||
        halo_z_policy->second != "wrap" || regions->size() != *unique_bricks ||
        files.at("atlas-bricks-r8.bin").size() != *bricks_bytes) {
        result.error = "atlas brick payload does not match its manifest";
        return result;
    }
    u64 expected_offset{};
    for (u32 index = 0; index < regions->size(); ++index) {
        const auto& region = (*regions)[index];
        const s32 requested_x = region.brick_x * 8 - 1;
        const s32 requested_y = region.brick_y * 8 - 1;
        const s32 requested_z = region.brick_z * 8 - 1;
        const s32 expected_x = std::max(requested_x, 0);
        const s32 expected_y = std::max(requested_y, 0);
        const s32 expected_z = ((requested_z % s32(bundle.atlas_depth)) + s32(bundle.atlas_depth)) %
                               s32(bundle.atlas_depth);
        const u32 expected_width = static_cast<u32>(
            std::max(0, std::min(requested_x + 10, s32(bundle.atlas_width)) - expected_x));
        const u32 expected_height = static_cast<u32>(
            std::max(0, std::min(requested_y + 10, s32(bundle.atlas_height)) - expected_y));
        const u64 expected_bytes = u64{expected_width} * expected_height * 10;
        if (region.slot != index || region.low24_id > 0x00ffffff ||
            region.brick_x != s32(region.low24_id & 0xff) ||
            region.brick_y != s32((region.low24_id >> 8) & 0xff) ||
            region.brick_z != s32((region.low24_id >> 16) & 0xff) ||
            region.source_x != expected_x || region.source_y != expected_y ||
            region.source_z != expected_z || region.width != expected_width ||
            region.height != expected_height || region.depth != 10 ||
            region.byte_size != expected_bytes || region.output_offset != expected_offset ||
            region.byte_size == 0 ||
            static_cast<u64>(region.source_x) + region.width > bundle.atlas_width ||
            static_cast<u64>(region.source_y) + region.height > bundle.atlas_height ||
            static_cast<u64>(region.source_z) >= bundle.atlas_depth ||
            region.output_offset > *bricks_bytes ||
            region.byte_size > *bricks_bytes - region.output_offset) {
            result.error = "atlas-regions.tsv contains an out-of-contract region";
            return result;
        }
        if (index != 0 && (*regions)[index - 1].low24_id >= region.low24_id) {
            result.error = "atlas-regions.tsv is not sorted by unique low24 ID";
            return result;
        }
        expected_offset += region.byte_size;
    }
    if (expected_offset != *bricks_bytes) {
        result.error = "atlas-regions.tsv does not tightly cover atlas-bricks-r8.bin";
        return result;
    }
    bundle.atlas_regions = std::move(*regions);

    const auto post_color_bytes = RequiredInteger<u64>(bundle.manifest, "post_color_bytes", error);
    const auto post_depth_bytes = RequiredInteger<u64>(bundle.manifest, "post_depth_bytes", error);
    const auto post_color_width = RequiredInteger<u32>(bundle.manifest, "post_color_width", error);
    const auto post_color_height =
        RequiredInteger<u32>(bundle.manifest, "post_color_height", error);
    const auto post_color_depth = RequiredInteger<u32>(bundle.manifest, "post_color_depth", error);
    const auto post_color_layers =
        RequiredInteger<u32>(bundle.manifest, "post_color_layers", error);
    const auto post_color_bits = RequiredInteger<u32>(bundle.manifest, "post_color_bits", error);
    const auto post_depth_width = RequiredInteger<u32>(bundle.manifest, "post_depth_width", error);
    const auto post_depth_height =
        RequiredInteger<u32>(bundle.manifest, "post_depth_height", error);
    const auto post_depth_depth = RequiredInteger<u32>(bundle.manifest, "post_depth_depth", error);
    const auto post_depth_layers =
        RequiredInteger<u32>(bundle.manifest, "post_depth_layers", error);
    const auto post_depth_bits = RequiredInteger<u32>(bundle.manifest, "post_depth_bits", error);
    const auto post_color_row_length =
        RequiredInteger<u32>(bundle.manifest, "post_color_row_length", error);
    const auto post_color_image_height =
        RequiredInteger<u32>(bundle.manifest, "post_color_buffer_image_height", error);
    const auto post_depth_row_length =
        RequiredInteger<u32>(bundle.manifest, "post_depth_row_length", error);
    const auto post_depth_image_height =
        RequiredInteger<u32>(bundle.manifest, "post_depth_buffer_image_height", error);
    const auto post_color_layout =
        RequiredInteger<u32>(bundle.manifest, "post_color_layout", error);
    const auto post_depth_layout =
        RequiredInteger<u32>(bundle.manifest, "post_depth_layout", error);
    if (!post_color_bytes || !post_depth_bytes || !post_color_width || !post_color_height ||
        !post_color_depth || !post_color_layers || !post_color_bits || !post_depth_width ||
        !post_depth_height || !post_depth_depth || !post_depth_layers || !post_depth_bits ||
        !post_color_row_length || !post_color_image_height || !post_depth_row_length ||
        !post_depth_image_height || !post_color_layout || !post_depth_layout) {
        result.error = std::move(error);
        return result;
    }
    const auto color_format = bundle.manifest.find("post_color_format");
    const auto depth_format = bundle.manifest.find("post_depth_format");
    const auto post_color_layout_name = bundle.manifest.find("post_color_layout_name");
    const auto post_depth_layout_name = bundle.manifest.find("post_depth_layout_name");
    if (color_format == bundle.manifest.end() || depth_format == bundle.manifest.end() ||
        post_color_layout_name == bundle.manifest.end() ||
        post_depth_layout_name == bundle.manifest.end() || color_format->second != "R32G32Uint" ||
        depth_format->second != "D32Sfloat" || *post_color_width != bundle.render_width ||
        *post_color_height != bundle.render_height || *post_color_depth != 1 ||
        *post_color_layers != bundle.render_layers || *post_color_bits != 64 ||
        *post_depth_width != bundle.render_width || *post_depth_height != bundle.render_height ||
        *post_depth_depth != 1 || *post_depth_layers != bundle.render_layers ||
        *post_depth_bits != 32 || *post_color_row_length != bundle.render_width ||
        *post_color_image_height != bundle.render_height ||
        *post_depth_row_length != bundle.render_width ||
        *post_depth_image_height != bundle.render_height ||
        static_cast<vk::ImageLayout>(*post_color_layout) != bundle.color_layout ||
        static_cast<vk::ImageLayout>(*post_depth_layout) != bundle.depth_layout ||
        post_color_layout_name->second != vk::to_string(bundle.color_layout) ||
        post_depth_layout_name->second != vk::to_string(bundle.depth_layout) ||
        *post_color_bytes !=
            u64{bundle.render_width} * bundle.render_height * bundle.render_layers * 8 ||
        *post_depth_bytes !=
            u64{bundle.render_width} * bundle.render_height * bundle.render_layers * 4 ||
        files.at("post-color.bin").size() != *post_color_bytes ||
        files.at("post-depth.bin").size() != *post_depth_bytes) {
        result.error = "post-draw oracle format or size is outside the exact replay contract";
        return result;
    }
    const auto pre_color_bytes = RequiredInteger<u64>(bundle.manifest, "pre_color_bytes", error);
    const auto pre_depth_bytes = RequiredInteger<u64>(bundle.manifest, "pre_depth_bytes", error);
    const auto pre_color_width = RequiredInteger<u32>(bundle.manifest, "pre_color_width", error);
    const auto pre_color_height = RequiredInteger<u32>(bundle.manifest, "pre_color_height", error);
    const auto pre_color_depth = RequiredInteger<u32>(bundle.manifest, "pre_color_depth", error);
    const auto pre_color_layers = RequiredInteger<u32>(bundle.manifest, "pre_color_layers", error);
    const auto pre_color_bits = RequiredInteger<u32>(bundle.manifest, "pre_color_bits", error);
    const auto pre_depth_width = RequiredInteger<u32>(bundle.manifest, "pre_depth_width", error);
    const auto pre_depth_height = RequiredInteger<u32>(bundle.manifest, "pre_depth_height", error);
    const auto pre_depth_depth = RequiredInteger<u32>(bundle.manifest, "pre_depth_depth", error);
    const auto pre_depth_layers = RequiredInteger<u32>(bundle.manifest, "pre_depth_layers", error);
    const auto pre_depth_bits = RequiredInteger<u32>(bundle.manifest, "pre_depth_bits", error);
    const auto pre_color_row_length =
        RequiredInteger<u32>(bundle.manifest, "pre_color_row_length", error);
    const auto pre_color_image_height =
        RequiredInteger<u32>(bundle.manifest, "pre_color_buffer_image_height", error);
    const auto pre_depth_row_length =
        RequiredInteger<u32>(bundle.manifest, "pre_depth_row_length", error);
    const auto pre_depth_image_height =
        RequiredInteger<u32>(bundle.manifest, "pre_depth_buffer_image_height", error);
    const auto pre_color_layout = RequiredInteger<u32>(bundle.manifest, "pre_color_layout", error);
    const auto pre_depth_layout = RequiredInteger<u32>(bundle.manifest, "pre_depth_layout", error);
    const auto pre_color_format = bundle.manifest.find("pre_color_format");
    const auto pre_depth_format = bundle.manifest.find("pre_depth_format");
    const auto pre_color_layout_name = bundle.manifest.find("pre_color_layout_name");
    const auto pre_depth_layout_name = bundle.manifest.find("pre_depth_layout_name");
    if (!pre_color_bytes || !pre_depth_bytes || !pre_color_width || !pre_color_height ||
        !pre_color_depth || !pre_color_layers || !pre_color_bits || !pre_depth_width ||
        !pre_depth_height || !pre_depth_depth || !pre_depth_layers || !pre_depth_bits ||
        !pre_color_row_length || !pre_color_image_height || !pre_depth_row_length ||
        !pre_depth_image_height || !pre_color_layout || !pre_depth_layout ||
        pre_color_format == bundle.manifest.end() || pre_depth_format == bundle.manifest.end() ||
        pre_color_layout_name == bundle.manifest.end() ||
        pre_depth_layout_name == bundle.manifest.end()) {
        result.error = std::move(error);
        return result;
    }
    if (pre_color_format->second != "R32G32Uint" || pre_depth_format->second != "D32Sfloat" ||
        *pre_color_width != *post_color_width || *pre_color_height != *post_color_height ||
        *pre_color_depth != *post_color_depth || *pre_color_layers != *post_color_layers ||
        *pre_color_bits != *post_color_bits || *pre_color_bytes != *post_color_bytes ||
        *pre_depth_width != *post_depth_width || *pre_depth_height != *post_depth_height ||
        *pre_depth_depth != *post_depth_depth || *pre_depth_layers != *post_depth_layers ||
        *pre_depth_bits != *post_depth_bits || *pre_depth_bytes != *post_depth_bytes ||
        *pre_color_row_length != bundle.render_width ||
        *pre_color_image_height != bundle.render_height ||
        *pre_depth_row_length != bundle.render_width ||
        *pre_depth_image_height != bundle.render_height ||
        static_cast<vk::ImageLayout>(*pre_color_layout) != bundle.color_layout ||
        static_cast<vk::ImageLayout>(*pre_depth_layout) != bundle.depth_layout ||
        pre_color_layout_name->second != vk::to_string(bundle.color_layout) ||
        pre_depth_layout_name->second != vk::to_string(bundle.depth_layout) ||
        files.at("pre-color.bin").size() != *pre_color_bytes ||
        files.at("pre-depth.bin").size() != *pre_depth_bytes) {
        result.error = "pre-draw attachment payload does not match the post-draw oracle contract";
        return result;
    }

    const auto image1_expected = u64{bundle.image1_width} * bundle.image1_height;
    const auto bda_page_size = RequiredInteger<u64>(bundle.manifest, "bda_page_size", error);
    const auto fs_srt_page_base = RequiredInteger<u64>(bundle.manifest, "fs_srt_page_base", error);
    const auto fs_srt_root_in_page =
        RequiredInteger<u64>(bundle.manifest, "fs_srt_root_in_page", error);
    const auto fs_srt_flat_base =
        RequiredInteger<u32>(bundle.manifest, "fs_srt_flat_base_dword", error);
    const auto fs_srt_flat_words =
        RequiredInteger<u32>(bundle.manifest, "fs_srt_flat_words", error);
    if (!bda_page_size || !fs_srt_page_base || !fs_srt_root_in_page || !fs_srt_flat_base ||
        !fs_srt_flat_words) {
        result.error = std::move(error);
        return result;
    }
    if (files.at("fs-image1-r8.bin").size() != image1_expected ||
        files.at("fs-bda-constants.bin").size() != 16 || *bda_page_size != 16 * 1024 ||
        files.at("bda-page-fs-srt.bin").size() != *bda_page_size ||
        (*fs_srt_page_base & (*bda_page_size - 1)) != 0 || *fs_srt_root_in_page >= *bda_page_size ||
        *fs_srt_flat_base != 16 || *fs_srt_flat_words != 19 ||
        (files.at("fs-srt-flat.bin").size() != 19 * sizeof(u32) &&
         files.at("fs-srt-flat.bin").size() != 35 * sizeof(u32))) {
        result.error = "fragment resource payload has an invalid size";
        return result;
    }
    if (bundle.fs_constant_address < *fs_srt_page_base) {
        result.error = "FS constant guest address precedes its captured BDA page";
        return result;
    }
    const u64 constant_offset = bundle.fs_constant_address - *fs_srt_page_base;
    const auto bda_page = std::span<const u8>{files.at("bda-page-fs-srt.bin")};
    if (constant_offset + files.at("fs-bda-constants.bin").size() > *bda_page_size ||
        !std::ranges::equal(
            files.at("fs-bda-constants.bin"),
            bda_page.subspan(constant_offset, files.at("fs-bda-constants.bin").size()))) {
        result.error = "FS BDA constant slice does not match its captured 16 KiB guest page";
        return result;
    }
    bundle.fs_srt_page_base = *fs_srt_page_base;
    if (*fs_srt_page_base >= (u64{1} << 40)) {
        result.error = "captured FS BDA page is outside the PS4 40-bit address contract";
        return result;
    }

    if (!ReconstructVertexBuffers(bundle, files, error)) {
        result.error = std::move(error);
        return result;
    }
    if (!ReconstructB535CandidateBank(bundle, files, error)) {
        result.error = std::move(error);
        return result;
    }
    const auto transform_page_base =
        RequiredInteger<u64>(bundle.manifest, "transform_page_base", error);
    const auto transform_address_in_page =
        RequiredInteger<u64>(bundle.manifest, "transform_address_in_page", error);
    const auto transform_root = RequiredInteger<u64>(bundle.manifest, "transform_root", error);
    const auto transform_index = RequiredInteger<u32>(bundle.manifest, "transform_index", error);
    const auto transform_address =
        RequiredInteger<u64>(bundle.manifest, "transform_address", error);
    if (!transform_page_base || !transform_address_in_page || !transform_root || !transform_index ||
        !transform_address) {
        result.error = std::move(error);
        return result;
    }
    const auto& captured_vs_srt = files.at("vs-srt-flat.bin");
    const auto& captured_transform = files.at("vs-transform.bin");
    const auto& transform_page = files.at("bda-page-transform.bin");
    if (captured_vs_srt.size() != 23 * sizeof(u32) ||
        captured_transform.size() != 16 * sizeof(u32) || transform_page.size() != *bda_page_size ||
        (*transform_page_base & (*bda_page_size - 1)) != 0 ||
        *transform_page_base >= (u64{1} << 40) || *transform_address_in_page >= *bda_page_size ||
        captured_transform.size() > *bda_page_size - *transform_address_in_page ||
        *transform_address != *transform_page_base + *transform_address_in_page ||
        *transform_root > std::numeric_limits<u64>::max() - (u64{*transform_index} * 192 + 128) ||
        *transform_address != *transform_root + u64{*transform_index} * 192 + 128 ||
        !std::ranges::equal(captured_transform,
                            std::span<const u8>{transform_page}.subspan(
                                *transform_address_in_page, captured_transform.size()))) {
        result.error = "vertex transform payload does not match its captured BDA page";
        return result;
    }
    std::array<u32, 23> vs_srt_words{};
    std::memcpy(vs_srt_words.data(), captured_vs_srt.data(), captured_vs_srt.size());
    constexpr std::array<u32, VsBufferCount> VsSrtDescriptorOrder{3, 0, 1, 4, 2};
    for (u32 slot = 0; slot < VsSrtDescriptorOrder.size(); ++slot) {
        std::array<u32, 4> descriptor_words{};
        std::memcpy(descriptor_words.data(),
                    &bundle.vs_buffer_descriptors[VsSrtDescriptorOrder[slot]],
                    sizeof(descriptor_words));
        if (!std::ranges::equal(descriptor_words,
                                std::span<const u32>{vs_srt_words}.subspan(slot * 4, 4))) {
            result.error = "VS flattened SRT buffer descriptor order is inconsistent";
            return result;
        }
    }
    const u64 flattened_transform_root =
        (u64{vs_srt_words[20]} | (u64{vs_srt_words[21]} << 32)) & ((u64{1} << 48) - 1);
    if (flattened_transform_root != *transform_root) {
        result.error = "VS flattened SRT does not contain the captured transform root";
        return result;
    }
    bundle.transform_page_base = *transform_page_base;

    bundle.indices = std::move(files.at("indices.bin"));
    bundle.vs_spv_bytes = std::move(files.at("vs.spv"));
    bundle.fs_spv_bytes = std::move(files.at("fs.spv"));
    if (auto it = files.find("vk-pipeline-cache.bin"); it != files.end()) {
        bundle.vk_pipeline_cache = std::move(it->second);
    }
    bundle.vs_srt_flat.resize(39 * sizeof(u32));
    std::memcpy(bundle.vs_srt_flat.data() + 16 * sizeof(u32), captured_vs_srt.data(),
                captured_vs_srt.size());
    bundle.vs_transform_page = std::move(files.at("bda-page-transform.bin"));
    if (files.at("fs-srt-flat.bin").size() == 19 * sizeof(u32)) {
        // The capture stores flattened SRT dwords 16..34. The generated FS indexes binding 0
        // using the original flattened indices, so preserve that base instead of rebasing them.
        bundle.fs_srt_flat.resize(35 * sizeof(u32));
        std::memcpy(bundle.fs_srt_flat.data() + 16 * sizeof(u32),
                    files.at("fs-srt-flat.bin").data(), files.at("fs-srt-flat.bin").size());
    } else {
        bundle.fs_srt_flat = std::move(files.at("fs-srt-flat.bin"));
    }
    bundle.fs_bda_constants = std::move(files.at("fs-bda-constants.bin"));
    bundle.fs_bda_page = std::move(files.at("bda-page-fs-srt.bin"));
    bundle.atlas_bricks = std::move(files.at("atlas-bricks-r8.bin"));
    bundle.fs_image1 = std::move(files.at("fs-image1-r8.bin"));
    bundle.post_color = std::move(files.at("post-color.bin"));
    bundle.post_depth = std::move(files.at("post-depth.bin"));
    bundle.pre_color = std::move(files.at("pre-color.bin"));
    bundle.pre_depth = std::move(files.at("pre-depth.bin"));

    result.stage = "validated";
    result.bundle = std::move(bundle);
    return result;
}

std::optional<u32> FindMemoryType(const Instance& instance, u32 compatible_types,
                                  vk::MemoryPropertyFlags required,
                                  vk::MemoryPropertyFlags preferred = {}) {
    const auto properties = instance.GetPhysicalDevice().getMemoryProperties();
    std::optional<u32> fallback;
    for (u32 index = 0; index < properties.memoryTypeCount; ++index) {
        if ((compatible_types & (1U << index)) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return index;
        }
        fallback = index;
    }
    return fallback;
}

struct AllocatedBuffer {
    // Memory precedes its bound buffer so reverse destruction releases the buffer first.
    vk::UniqueDeviceMemory memory;
    vk::UniqueBuffer buffer;
    vk::DeviceSize size{};
    vk::DeviceAddress address{};
};

struct AllocatedImage {
    // Memory precedes the image and view for safe reverse destruction.
    vk::UniqueDeviceMemory memory;
    vk::UniqueImage image;
    vk::UniqueImageView view;
    vk::Extent3D extent{};
    u32 layers{};
    vk::Format format{};
};

std::optional<AllocatedBuffer> CreateBuffer(const Instance& instance, vk::DeviceSize size,
                                            vk::BufferUsageFlags usage,
                                            std::span<const u8> initial_data, bool device_address,
                                            std::string& error) {
    if (size == 0 || initial_data.size() > size) {
        error = "invalid replay buffer size";
        return std::nullopt;
    }
    const auto device = instance.GetDevice();
    if (device_address) {
        usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
    }
    const vk::BufferCreateInfo create_info{
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    auto [buffer_result, buffer] = device.createBufferUnique(create_info);
    if (buffer_result != vk::Result::eSuccess) {
        error = "Vulkan buffer creation failed: " + vk::to_string(buffer_result);
        return std::nullopt;
    }
    const auto requirements = device.getBufferMemoryRequirements(*buffer);
    const auto memory_type = FindMemoryType(instance, requirements.memoryTypeBits,
                                            vk::MemoryPropertyFlagBits::eHostVisible |
                                                vk::MemoryPropertyFlagBits::eHostCoherent);
    if (!memory_type) {
        error = "no coherent host-visible memory type for replay buffer";
        return std::nullopt;
    }
    const vk::MemoryAllocateFlagsInfo flags_info{
        .flags =
            device_address ? vk::MemoryAllocateFlagBits::eDeviceAddress : vk::MemoryAllocateFlags{},
    };
    const vk::MemoryAllocateInfo allocate_info{
        .pNext = device_address ? &flags_info : nullptr,
        .allocationSize = requirements.size,
        .memoryTypeIndex = *memory_type,
    };
    auto [memory_result, memory] = device.allocateMemoryUnique(allocate_info);
    if (memory_result != vk::Result::eSuccess) {
        error = "Vulkan buffer memory allocation failed: " + vk::to_string(memory_result);
        return std::nullopt;
    }
    if (const auto bind_result = device.bindBufferMemory(*buffer, *memory, 0);
        bind_result != vk::Result::eSuccess) {
        error = "Vulkan buffer memory bind failed: " + vk::to_string(bind_result);
        return std::nullopt;
    }
    void* mapped{};
    if (const auto map_result = device.mapMemory(*memory, 0, size, {}, &mapped);
        map_result != vk::Result::eSuccess) {
        error = "Vulkan buffer map failed: " + vk::to_string(map_result);
        return std::nullopt;
    }
    std::memset(mapped, 0, static_cast<size_t>(size));
    if (!initial_data.empty()) {
        std::memcpy(mapped, initial_data.data(), initial_data.size());
    }
    device.unmapMemory(*memory);
    AllocatedBuffer result{
        .memory = std::move(memory),
        .buffer = std::move(buffer),
        .size = size,
    };
    if (device_address) {
        result.address =
            device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *result.buffer});
        if (result.address == 0) {
            error = "Vulkan returned a null shader device address";
            return std::nullopt;
        }
    }
    return result;
}

std::optional<AllocatedImage> CreateImage(const Instance& instance, vk::ImageType image_type,
                                          vk::ImageViewType view_type, vk::Format format,
                                          vk::Extent3D extent, u32 layers,
                                          vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                                          std::array<vk::ComponentSwizzle, 4> swizzle,
                                          std::string& error) {
    const auto device = instance.GetDevice();
    vk::ImageCreateFlags image_flags =
        vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage;
    if (image_type == vk::ImageType::e3D) {
        image_flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            image_flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    const vk::ImageCreateInfo create_info{
        .flags = image_flags,
        .imageType = image_type,
        .format = format,
        .extent = extent,
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    auto [image_result, image] = device.createImageUnique(create_info);
    if (image_result != vk::Result::eSuccess) {
        error = "Vulkan image creation failed: " + vk::to_string(image_result);
        return std::nullopt;
    }
    const auto requirements = device.getImageMemoryRequirements(*image);
    const auto memory_type = FindMemoryType(instance, requirements.memoryTypeBits, {},
                                            vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (!memory_type) {
        error = "no compatible memory type for replay image";
        return std::nullopt;
    }
    const vk::MemoryAllocateInfo allocate_info{
        .allocationSize = requirements.size,
        .memoryTypeIndex = *memory_type,
    };
    auto [memory_result, memory] = device.allocateMemoryUnique(allocate_info);
    if (memory_result != vk::Result::eSuccess) {
        error = "Vulkan image memory allocation failed: " + vk::to_string(memory_result);
        return std::nullopt;
    }
    if (const auto bind_result = device.bindImageMemory(*image, *memory, 0);
        bind_result != vk::Result::eSuccess) {
        error = "Vulkan image memory bind failed: " + vk::to_string(bind_result);
        return std::nullopt;
    }
    vk::ImageViewUsageCreateInfo view_usage{
        .usage = usage & ~vk::ImageUsageFlagBits::eStorage,
    };
    const vk::ImageViewCreateInfo view_info{
        .pNext = &view_usage,
        .image = *image,
        .viewType = view_type,
        .format = format,
        .components = {.r = swizzle[0], .g = swizzle[1], .b = swizzle[2], .a = swizzle[3]},
        .subresourceRange =
            {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
    };
    auto [view_result, view] = device.createImageViewUnique(view_info);
    if (view_result != vk::Result::eSuccess) {
        error = "Vulkan image-view creation failed: " + vk::to_string(view_result);
        return std::nullopt;
    }
    return AllocatedImage{
        .memory = std::move(memory),
        .image = std::move(image),
        .view = std::move(view),
        .extent = extent,
        .layers = layers,
        .format = format,
    };
}

std::optional<std::vector<u8>> ReadMappedBuffer(vk::Device device, const AllocatedBuffer& buffer,
                                                std::string& error) {
    void* mapped{};
    if (const auto result = device.mapMemory(*buffer.memory, 0, buffer.size, {}, &mapped);
        result != vk::Result::eSuccess) {
        error = "Vulkan readback map failed: " + vk::to_string(result);
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<size_t>(buffer.size));
    std::memcpy(bytes.data(), mapped, bytes.size());
    device.unmapMemory(*buffer.memory);
    return bytes;
}

bool WriteBinary(const std::filesystem::path& path, std::span<const u8> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool WriteMappedBuffer(vk::Device device, const AllocatedBuffer& buffer, vk::DeviceSize offset,
                       std::span<const u8> bytes, std::string& error) {
    if (offset > buffer.size || bytes.size() > buffer.size - offset) {
        error = "replay buffer write is out of bounds";
        return false;
    }
    void* mapped{};
    if (const auto result = device.mapMemory(*buffer.memory, offset, bytes.size(), {}, &mapped);
        result != vk::Result::eSuccess) {
        error = "Vulkan replay buffer map failed: " + vk::to_string(result);
        return false;
    }
    std::memcpy(mapped, bytes.data(), bytes.size());
    device.unmapMemory(*buffer.memory);
    return true;
}

template <typename T>
std::span<const u8> ByteSpan(std::span<const T> values) {
    return {reinterpret_cast<const u8*>(values.data()), values.size_bytes()};
}

vk::ImageMemoryBarrier ImageBarrier(vk::Image image, vk::ImageAspectFlags aspect,
                                    vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                    vk::AccessFlags source_access, vk::AccessFlags target_access,
                                    u32 layers = 1) {
    return {
        .srcAccessMask = source_access,
        .dstAccessMask = target_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange =
            {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
    };
}

struct ReplayOutcome {
    bool completed{};
    bool parity{};
    bool deterministic{};
    u64 color_hash{};
    u64 depth_hash{};
    u64 changed_color_pixels{};
    u64 changed_depth_pixels{};
    std::string error;
};

struct ReplayOptions {
    std::filesystem::path output_directory;
    std::string output_stem{"replay"};
    bool compare_capture{true};
    bool write_warm_outputs{true};
};

u64 CountChangedElements(std::span<const u8> current, std::span<const u8> before,
                         u64 element_bytes) {
    if (element_bytes == 0 || current.size() != before.size() ||
        current.size() % element_bytes != 0) {
        return 0;
    }
    u64 changed{};
    for (u64 offset = 0; offset < current.size(); offset += element_bytes) {
        changed += !std::ranges::equal(current.subspan(offset, element_bytes),
                                       before.subspan(offset, element_bytes));
    }
    return changed;
}

ReplayOutcome RunExactReplay(const Instance& instance, const ReplayBundle& bundle,
                             const ReplayOptions& options) {
    constexpr vk::DeviceSize GuestPageCount = u64{1} << (40 - 14);
    constexpr vk::DeviceSize PageTableBytes = GuestPageCount * sizeof(vk::DeviceAddress);
    constexpr vk::DeviceSize FaultBufferBytes = GuestPageCount / 32 * sizeof(u32);
    constexpr vk::DeviceSize BdaPageBytes = 16 * 1024;

    ReplayOutcome outcome;
    std::string error;
    const auto device = instance.GetDevice();
    const auto physical = instance.GetPhysicalDevice();
    const auto properties = physical.getProperties();
    const auto features = physical.getFeatures();
    if (properties.limits.maxStorageBufferRange < PageTableBytes) {
        outcome.error = "GPU maxStorageBufferRange cannot bind the exact 40-bit BDA page table";
        return outcome;
    }
    if (bundle.dynamic.depth_bounds_test_enabled && !instance.IsDepthBoundsSupported()) {
        outcome.error = "captured depth-bounds state is unsupported by this GPU";
        return outcome;
    }
    if (bundle.graphics_key.depth_clamp_enable && !features.depthClamp) {
        outcome.error = "captured depth-clamp state is unsupported by this GPU";
        return outcome;
    }
    if (bundle.graphics_key.polygon_mode != AmdGpu::PolygonMode::Fill &&
        !features.fillModeNonSolid) {
        outcome.error = "captured non-fill polygon mode is unsupported by this GPU";
        return outcome;
    }
    if (bundle.dynamic.line_width != 1.0f && !features.wideLines) {
        outcome.error = "captured wide-line state is unsupported by this GPU";
        return outcome;
    }
    if (bundle.graphics_key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::Last &&
        !instance.IsProvokingVertexSupported()) {
        outcome.error = "captured last-vertex provoking mode needs VK_EXT_provoking_vertex";
        return outcome;
    }
    if (!bundle.graphics_key.depth_clip_enable && !instance.IsDepthClipEnableSupported()) {
        outcome.error = "captured disabled depth clipping needs VK_EXT_depth_clip_enable";
        return outcome;
    }
    if (bundle.graphics_key.clip_space == AmdGpu::ClipSpace::MinusWToW &&
        !instance.IsDepthClipControlSupported()) {
        outcome.error = "captured -W..W clip space needs VK_EXT_depth_clip_control";
        return outcome;
    }
    if (instance.MaxPushDescriptors() < ReplayDescriptorCount) {
        outcome.error = "GPU push-descriptor limit is below the replay's sixteen bindings";
        return outcome;
    }

    const auto make_buffer = [&](vk::DeviceSize size, vk::BufferUsageFlags usage,
                                 std::span<const u8> data = {}, bool address = false) {
        return CreateBuffer(instance, size, usage, data, address, error);
    };

    auto index_buffer =
        make_buffer(bundle.indices.size(), vk::BufferUsageFlagBits::eIndexBuffer, bundle.indices);
    const u32 indirect_count = 1;
    auto indirect_buffer =
        make_buffer(sizeof(bundle.draw), vk::BufferUsageFlagBits::eIndirectBuffer,
                    ByteSpan<DrawIndexedCommand>(std::span{&bundle.draw, 1}));
    auto indirect_count_buffer =
        make_buffer(sizeof(indirect_count), vk::BufferUsageFlagBits::eIndirectBuffer,
                    ByteSpan<u32>(std::span{&indirect_count, 1}));
    std::array<std::optional<AllocatedBuffer>, VsBufferCount> vs_buffers{};
    for (u32 binding = 0; binding < VsBufferCount; ++binding) {
        vs_buffers[binding] =
            make_buffer(bundle.vs_buffers[binding].size(), vk::BufferUsageFlagBits::eStorageBuffer,
                        bundle.vs_buffers[binding]);
        if (!vs_buffers[binding]) {
            outcome.error = std::move(error);
            return outcome;
        }
    }
    auto clip_planes_buffer = make_buffer(sizeof(u32), vk::BufferUsageFlagBits::eStorageBuffer);
    auto vs_srt_buffer = make_buffer(bundle.vs_srt_flat.size(),
                                     vk::BufferUsageFlagBits::eStorageBuffer, bundle.vs_srt_flat);
    auto fs_srt_buffer = make_buffer(bundle.fs_srt_flat.size(),
                                     vk::BufferUsageFlagBits::eStorageBuffer, bundle.fs_srt_flat);
    auto fs_bda_page_buffer = make_buffer(BdaPageBytes, vk::BufferUsageFlagBits::eStorageBuffer,
                                          bundle.fs_bda_page, true);
    auto transform_bda_page_buffer = make_buffer(
        BdaPageBytes, vk::BufferUsageFlagBits::eStorageBuffer, bundle.vs_transform_page, true);
    auto gds_buffer = make_buffer(GdsBufferBytes, vk::BufferUsageFlagBits::eStorageBuffer);
    if (!index_buffer || !indirect_buffer || !indirect_count_buffer || !clip_planes_buffer ||
        !vs_srt_buffer || !fs_srt_buffer || !fs_bda_page_buffer || !transform_bda_page_buffer ||
        !gds_buffer) {
        outcome.error = std::move(error);
        return outcome;
    }

    auto page_table_buffer = make_buffer(PageTableBytes, vk::BufferUsageFlagBits::eStorageBuffer);
    auto fault_buffer = make_buffer(FaultBufferBytes, vk::BufferUsageFlagBits::eStorageBuffer);
    if (!page_table_buffer || !fault_buffer) {
        outcome.error = std::move(error);
        return outcome;
    }
    const auto map_guest_page = [&](u64 guest_page_base, vk::DeviceAddress host_address) {
        const u64 page_index = guest_page_base >> 14;
        return WriteMappedBuffer(device, *page_table_buffer, page_index * sizeof(host_address),
                                 ByteSpan<vk::DeviceAddress>(std::span{&host_address, 1}), error);
    };
    if (!map_guest_page(bundle.fs_srt_page_base, fs_bda_page_buffer->address) ||
        !map_guest_page(bundle.transform_page_base, transform_bda_page_buffer->address)) {
        outcome.error = std::move(error);
        return outcome;
    }
    static_assert((Shader::DreamsCompat::Vs370InterfaceCaptureArmDword + 1) * sizeof(u32) <=
                  GdsBufferBytes);
    if (!WriteMappedBuffer(device, *gds_buffer,
                           u64{Shader::DreamsCompat::Vs370InterfaceCaptureArmDword} * sizeof(u32),
                           ByteSpan<u32>(std::span{&bundle.instance_count, 1}), error)) {
        outcome.error = std::move(error);
        return outcome;
    }

    vk::ImageUsageFlags color_image_usage =
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::eStorage;
    if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        color_image_usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
    }
    auto atlas = CreateImage(
        instance, vk::ImageType::e3D, vk::ImageViewType::e3D, vk::Format::eR8Unorm,
        {bundle.atlas_width, bundle.atlas_height, bundle.atlas_depth}, 1, color_image_usage,
        vk::ImageAspectFlagBits::eColor, bundle.atlas_swizzle, error);
    auto image1 =
        CreateImage(instance, vk::ImageType::e2D, vk::ImageViewType::e2D, vk::Format::eR8Unorm,
                    {bundle.image1_width, bundle.image1_height, 1}, 1, color_image_usage,
                    vk::ImageAspectFlagBits::eColor, bundle.image1_swizzle, error);
    const vk::ImageUsageFlags depth_usage =
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst |
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
    auto color =
        CreateImage(instance, vk::ImageType::e2D, vk::ImageViewType::e2D, vk::Format::eR32G32Uint,
                    {bundle.render_width, bundle.render_height, 1}, 1, color_image_usage,
                    vk::ImageAspectFlagBits::eColor, bundle.color_swizzle, error);
    auto depth =
        CreateImage(instance, vk::ImageType::e2D, vk::ImageViewType::e2D, vk::Format::eD32Sfloat,
                    {bundle.render_width, bundle.render_height, 1}, 1, depth_usage,
                    vk::ImageAspectFlagBits::eDepth, bundle.depth_swizzle, error);
    if (!atlas || !image1 || !color || !depth) {
        outcome.error = std::move(error);
        return outcome;
    }

    auto atlas_staging = make_buffer(std::max<size_t>(bundle.atlas_bricks.size(), 1),
                                     vk::BufferUsageFlagBits::eTransferSrc, bundle.atlas_bricks);
    auto image1_staging = make_buffer(bundle.fs_image1.size(),
                                      vk::BufferUsageFlagBits::eTransferSrc, bundle.fs_image1);
    auto pre_color_staging = make_buffer(bundle.pre_color.size(),
                                         vk::BufferUsageFlagBits::eTransferSrc, bundle.pre_color);
    auto pre_depth_staging = make_buffer(bundle.pre_depth.size(),
                                         vk::BufferUsageFlagBits::eTransferSrc, bundle.pre_depth);
    auto color_readback =
        make_buffer(bundle.post_color.size(), vk::BufferUsageFlagBits::eTransferDst);
    auto depth_readback =
        make_buffer(bundle.post_depth.size(), vk::BufferUsageFlagBits::eTransferDst);
    auto warm_color_readback =
        make_buffer(bundle.post_color.size(), vk::BufferUsageFlagBits::eTransferDst);
    auto warm_depth_readback =
        make_buffer(bundle.post_depth.size(), vk::BufferUsageFlagBits::eTransferDst);
    auto atlas_readback =
        make_buffer(bundle.atlas_bricks.size(), vk::BufferUsageFlagBits::eTransferDst);
    auto image1_readback =
        make_buffer(bundle.fs_image1.size(), vk::BufferUsageFlagBits::eTransferDst);
    if (!atlas_staging || !image1_staging || !pre_color_staging || !pre_depth_staging ||
        !color_readback || !depth_readback || !warm_color_readback || !warm_depth_readback ||
        !atlas_readback || !image1_readback) {
        outcome.error = std::move(error);
        return outcome;
    }

    std::array<vk::DescriptorSetLayoutBinding, ReplayDescriptorCount> descriptor_bindings{};
    for (u32 binding = 0; binding < descriptor_bindings.size(); ++binding) {
        descriptor_bindings[binding] = {
            .binding = binding,
            .descriptorType = binding <= 2 || binding >= 6 ? vk::DescriptorType::eStorageBuffer
                              : binding <= 4               ? vk::DescriptorType::eSampledImage
                                                           : vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = binding >= 6 ? vk::ShaderStageFlagBits::eVertex
                                       : vk::ShaderStageFlagBits::eFragment,
        };
    }
    const vk::DescriptorSetLayoutCreateInfo descriptor_layout_info{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(descriptor_bindings.size()),
        .pBindings = descriptor_bindings.data(),
    };
    auto [descriptor_layout_result, descriptor_layout] =
        device.createDescriptorSetLayoutUnique(descriptor_layout_info);
    if (descriptor_layout_result != vk::Result::eSuccess) {
        outcome.error =
            "descriptor-set layout creation failed: " + vk::to_string(descriptor_layout_result);
        return outcome;
    }
    const vk::PushConstantRange push_range{
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };
    const vk::DescriptorSetLayout descriptor_layout_handle = *descriptor_layout;
    const vk::PipelineLayoutCreateInfo pipeline_layout_info{
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_layout_handle,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    auto [pipeline_layout_result, pipeline_layout] =
        device.createPipelineLayoutUnique(pipeline_layout_info);
    if (pipeline_layout_result != vk::Result::eSuccess) {
        outcome.error = "pipeline-layout creation failed: " + vk::to_string(pipeline_layout_result);
        return outcome;
    }

    std::vector<u32> vs_words(bundle.vs_spv_bytes.size() / sizeof(u32));
    std::memcpy(vs_words.data(), bundle.vs_spv_bytes.data(), bundle.vs_spv_bytes.size());
    std::vector<u32> fs_words(bundle.fs_spv_bytes.size() / sizeof(u32));
    std::memcpy(fs_words.data(), bundle.fs_spv_bytes.data(), bundle.fs_spv_bytes.size());
    const vk::ShaderModule vertex_module = CompileSPV(vs_words, device);
    const vk::ShaderModule fragment_module = CompileSPV(fs_words, device);
    const auto destroy_modules = [&] {
        device.destroyShaderModule(vertex_module);
        device.destroyShaderModule(fragment_module);
    };
    const std::array shader_stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vertex_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fragment_module,
            .pName = "main",
        },
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = LiverpoolToVK::PrimitiveType(bundle.graphics_key.prim_type),
    };
    const vk::PipelineTessellationStateCreateInfo tessellation_state{
        .patchControlPoints = bundle.graphics_key.patch_control_points,
    };
    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control{
        .negativeOneToOne = bundle.graphics_key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
    };
    vk::PipelineRasterizationDepthClipStateCreateInfoEXT depth_clip{
        .depthClipEnable = bundle.graphics_key.depth_clip_enable,
    };
    vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{
        .provokingVertexMode =
            bundle.graphics_key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                ? vk::ProvokingVertexModeEXT::eFirstVertex
                : vk::ProvokingVertexModeEXT::eLastVertex,
    };
    const void* raster_next{};
    if (instance.IsDepthClipEnableSupported()) {
        depth_clip.pNext = raster_next;
        raster_next = &depth_clip;
    }
    if (instance.IsProvokingVertexSupported()) {
        provoking.pNext = raster_next;
        raster_next = &provoking;
    }
    const vk::PipelineRasterizationStateCreateInfo rasterization{
        .pNext = raster_next,
        .depthClampEnable =
            bundle.graphics_key.depth_clamp_enable &&
            (!bundle.graphics_key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
        .rasterizerDiscardEnable = false,
        .polygonMode = LiverpoolToVK::PolygonMode(bundle.graphics_key.polygon_mode),
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    const auto& blend_control = bundle.graphics_key.blend_controls[0];
    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = blend_control.enable,
        .srcColorBlendFactor = LiverpoolToVK::BlendFactor(blend_control.color_src_factor),
        .dstColorBlendFactor = LiverpoolToVK::BlendFactor(blend_control.color_dst_factor),
        .colorBlendOp = LiverpoolToVK::BlendOp(blend_control.color_func),
        .srcAlphaBlendFactor = LiverpoolToVK::BlendFactor(blend_control.separate_alpha_blend
                                                              ? blend_control.alpha_src_factor
                                                              : blend_control.color_src_factor),
        .dstAlphaBlendFactor = LiverpoolToVK::BlendFactor(blend_control.separate_alpha_blend
                                                              ? blend_control.alpha_dst_factor
                                                              : blend_control.color_dst_factor),
        .alphaBlendOp =
            LiverpoolToVK::BlendOp(blend_control.separate_alpha_blend ? blend_control.alpha_func
                                                                      : blend_control.color_func),
        .colorWriteMask = instance.IsDynamicColorWriteMaskSupported()
                              ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                              : bundle.graphics_key.write_masks[0],
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = instance.IsLogicOpSupported() &&
                         bundle.graphics_key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(bundle.graphics_key.logic_op),
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };
    std::vector<vk::DynamicState> dynamic_states{
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };
    if (instance.IsDepthBoundsSupported()) {
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    }
    const vk::PipelineDynamicStateCreateInfo dynamic_state_info{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::Format color_format = vk::Format::eR32G32Uint;
    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
    std::ranges::transform(
        bundle.graphics_key.color_samples, color_samples.begin(), [&instance](u8 samples) {
            return samples ? LiverpoolToVK::NumSamples(samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
        });
    const vk::AttachmentSampleCountInfoAMD mixed_samples{
        .colorAttachmentCount = bundle.graphics_key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples = LiverpoolToVK::NumSamples(
            bundle.graphics_key.depth_samples, instance.GetDepthSampleCounts()),
    };
    const vk::PipelineRenderingCreateInfo rendering_pipeline{
        .pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = vk::Format::eD32Sfloat,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering_pipeline,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = instance.IsVertexInputDynamicState() ? nullptr : &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation_state,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisampling,
        .pDepthStencilState =
            instance.IsExtendedDynamicState3Supported() ? nullptr : &depth_stencil,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state_info,
        .layout = *pipeline_layout,
    };
    if (!bundle.vk_pipeline_cache.empty()) {
        constexpr size_t PipelineCacheHeaderBytes = 4 * sizeof(u32) + VK_UUID_SIZE;
        if (bundle.vk_pipeline_cache.size() < PipelineCacheHeaderBytes) {
            destroy_modules();
            outcome.error = "captured Vulkan pipeline cache has a truncated header";
            return outcome;
        }
        std::array<u32, 4> header{};
        std::memcpy(header.data(), bundle.vk_pipeline_cache.data(), sizeof(header));
        const auto expected_uuid = instance.GetPipelineCacheUUID();
        const auto captured_uuid =
            std::span<const u8>{bundle.vk_pipeline_cache}.subspan(sizeof(header), VK_UUID_SIZE);
        if (header[0] < PipelineCacheHeaderBytes ||
            header[1] != static_cast<u32>(vk::PipelineCacheHeaderVersion::eOne) ||
            header[2] != properties.vendorID || header[3] != properties.deviceID ||
            !std::ranges::equal(captured_uuid, expected_uuid)) {
            destroy_modules();
            outcome.error = "captured Vulkan pipeline cache is incompatible with this GPU/driver";
            return outcome;
        }
    }
    const vk::PipelineCacheCreateInfo replay_pipeline_cache_info{
        .initialDataSize = bundle.vk_pipeline_cache.size(),
        .pInitialData =
            bundle.vk_pipeline_cache.empty() ? nullptr : bundle.vk_pipeline_cache.data(),
    };
    auto [pipeline_cache_result, replay_pipeline_cache] =
        device.createPipelineCacheUnique(replay_pipeline_cache_info);
    if (pipeline_cache_result != vk::Result::eSuccess) {
        destroy_modules();
        outcome.error = "pipeline-cache creation failed: " + vk::to_string(pipeline_cache_result);
        return outcome;
    }
    auto [pipeline_result, pipeline] =
        device.createGraphicsPipelineUnique(*replay_pipeline_cache, pipeline_info);
    destroy_modules();
    if (pipeline_result != vk::Result::eSuccess) {
        outcome.error = "graphics-pipeline creation failed: " + vk::to_string(pipeline_result);
        return outcome;
    }

    VideoCore::Sampler replay_sampler{instance, bundle.sampler, {}, bundle.sampler_is_compare};
    const vk::CommandPoolCreateInfo pool_info{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    auto [pool_result, command_pool] = device.createCommandPoolUnique(pool_info);
    if (pool_result != vk::Result::eSuccess) {
        outcome.error = "command-pool creation failed: " + vk::to_string(pool_result);
        return outcome;
    }
    const vk::CommandBufferAllocateInfo command_allocate{
        .commandPool = *command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::CommandBuffer command;
    if (const auto result = device.allocateCommandBuffers(&command_allocate, &command);
        result != vk::Result::eSuccess) {
        outcome.error = "command-buffer allocation failed: " + vk::to_string(result);
        return outcome;
    }
    if (const auto result =
            command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        result != vk::Result::eSuccess) {
        outcome.error = "command-buffer begin failed: " + vk::to_string(result);
        return outcome;
    }

    std::array initial_barriers{
        ImageBarrier(*atlas->image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits::eTransferWrite),
        ImageBarrier(*image1->image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits::eTransferWrite),
        ImageBarrier(*color->image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits::eTransferWrite),
        ImageBarrier(*depth->image, vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits::eTransferWrite),
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, initial_barriers);
    command.clearColorImage(*atlas->image, vk::ImageLayout::eTransferDstOptimal,
                            vk::ClearColorValue{.uint32 = std::array<u32, 4>{0, 0, 0, 0}},
                            initial_barriers[0].subresourceRange);
    for (const auto& region : bundle.atlas_regions) {
        const u32 first_depth = std::min(region.depth, bundle.atlas_depth - u32(region.source_z));
        const auto copy_part = [&](u64 buffer_offset, u32 source_z, u32 depth_count) {
            if (depth_count == 0) {
                return;
            }
            const vk::BufferImageCopy copy{
                .bufferOffset = buffer_offset,
                .bufferRowLength = region.width,
                .bufferImageHeight = region.height,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {region.source_x, region.source_y, s32(source_z)},
                .imageExtent = {region.width, region.height, depth_count},
            };
            command.copyBufferToImage(*atlas_staging->buffer, *atlas->image,
                                      vk::ImageLayout::eTransferDstOptimal, copy);
        };
        copy_part(region.output_offset, u32(region.source_z), first_depth);
        copy_part(region.output_offset + u64{first_depth} * region.width * region.height, 0,
                  region.depth - first_depth);
    }
    const auto atlas_to_readback =
        ImageBarrier(*atlas->image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, atlas_to_readback);
    for (const auto& region : bundle.atlas_regions) {
        const u32 first_depth = std::min(region.depth, bundle.atlas_depth - u32(region.source_z));
        const auto copy_part = [&](u64 buffer_offset, u32 source_z, u32 depth_count) {
            if (depth_count == 0) {
                return;
            }
            const vk::BufferImageCopy copy{
                .bufferOffset = buffer_offset,
                .bufferRowLength = region.width,
                .bufferImageHeight = region.height,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {region.source_x, region.source_y, s32(source_z)},
                .imageExtent = {region.width, region.height, depth_count},
            };
            command.copyImageToBuffer(*atlas->image, vk::ImageLayout::eTransferSrcOptimal,
                                      *atlas_readback->buffer, copy);
        };
        copy_part(region.output_offset, u32(region.source_z), first_depth);
        copy_part(region.output_offset + u64{first_depth} * region.width * region.height, 0,
                  region.depth - first_depth);
    }
    const vk::BufferImageCopy image1_copy{
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageExtent = {bundle.image1_width, bundle.image1_height, 1},
    };
    command.copyBufferToImage(*image1_staging->buffer, *image1->image,
                              vk::ImageLayout::eTransferDstOptimal, image1_copy);
    const auto image1_to_readback =
        ImageBarrier(*image1->image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, image1_to_readback);
    command.copyImageToBuffer(*image1->image, vk::ImageLayout::eTransferSrcOptimal,
                              *image1_readback->buffer, image1_copy);
    const vk::BufferImageCopy color_copy{
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageExtent = {bundle.render_width, bundle.render_height, 1},
    };
    const vk::BufferImageCopy depth_copy{
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eDepth,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageExtent = {bundle.render_width, bundle.render_height, 1},
    };
    command.copyBufferToImage(*pre_color_staging->buffer, *color->image,
                              vk::ImageLayout::eTransferDstOptimal, color_copy);
    command.copyBufferToImage(*pre_depth_staging->buffer, *depth->image,
                              vk::ImageLayout::eTransferDstOptimal, depth_copy);

    std::array ready_barriers{
        ImageBarrier(*atlas->image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eTransferSrcOptimal, bundle.atlas_layout,
                     vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead),
        ImageBarrier(*image1->image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eTransferSrcOptimal, bundle.image1_layout,
                     vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead),
        ImageBarrier(
            *color->image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
            bundle.color_layout, vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite),
        ImageBarrier(*depth->image, vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eTransferDstOptimal, bundle.depth_layout,
                     vk::AccessFlagBits::eTransferWrite,
                     vk::AccessFlagBits::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits::eDepthStencilAttachmentWrite),
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eFragmentShader |
                                vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                vk::PipelineStageFlagBits::eLateFragmentTests,
                            {}, {}, {}, ready_barriers);

    const vk::RenderingAttachmentInfo color_attachment{
        .imageView = *color->view,
        .imageLayout = bundle.color_layout,
        .loadOp =
            bundle.render.color_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.color = vk::ClearColorValue{.uint32 = bundle.render.color_clear_raw}},
    };
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = *depth->view,
        .imageLayout = bundle.depth_layout,
        .loadOp =
            bundle.render.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil =
                               vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                              bundle.render.depth_clear_raw[0])}},
    };
    const vk::RenderingInfo rendering_info{
        .renderArea = {.offset = {0, 0}, .extent = {bundle.render_width, bundle.render_height}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
        .pDepthAttachment = &depth_attachment,
    };
    const std::array buffer_infos{
        vk::DescriptorBufferInfo{
            .buffer = *fs_srt_buffer->buffer, .offset = 0, .range = fs_srt_buffer->size},
        vk::DescriptorBufferInfo{
            .buffer = *page_table_buffer->buffer, .offset = 0, .range = page_table_buffer->size},
        vk::DescriptorBufferInfo{
            .buffer = *fault_buffer->buffer, .offset = 0, .range = fault_buffer->size},
        vk::DescriptorBufferInfo{
            .buffer = *vs_buffers[0]->buffer, .offset = 0, .range = vs_buffers[0]->size},
        vk::DescriptorBufferInfo{
            .buffer = *vs_buffers[1]->buffer, .offset = 0, .range = vs_buffers[1]->size},
        vk::DescriptorBufferInfo{
            .buffer = *vs_buffers[2]->buffer, .offset = 0, .range = vs_buffers[2]->size},
        vk::DescriptorBufferInfo{
            .buffer = *vs_buffers[3]->buffer, .offset = 0, .range = vs_buffers[3]->size},
        vk::DescriptorBufferInfo{
            .buffer = *vs_buffers[4]->buffer, .offset = 0, .range = vs_buffers[4]->size},
        vk::DescriptorBufferInfo{
            .buffer = *clip_planes_buffer->buffer, .offset = 0, .range = clip_planes_buffer->size},
        vk::DescriptorBufferInfo{
            .buffer = *vs_srt_buffer->buffer, .offset = 0, .range = vs_srt_buffer->size},
        vk::DescriptorBufferInfo{
            .buffer = *page_table_buffer->buffer, .offset = 0, .range = page_table_buffer->size},
        vk::DescriptorBufferInfo{
            .buffer = *fault_buffer->buffer, .offset = 0, .range = fault_buffer->size},
        vk::DescriptorBufferInfo{
            .buffer = *gds_buffer->buffer, .offset = 0, .range = gds_buffer->size},
    };
    const std::array image_infos{
        vk::DescriptorImageInfo{.imageView = *atlas->view, .imageLayout = bundle.atlas_layout},
        vk::DescriptorImageInfo{.imageView = *image1->view, .imageLayout = bundle.image1_layout},
        vk::DescriptorImageInfo{.sampler = replay_sampler.Handle()},
    };
    std::array<vk::WriteDescriptorSet, ReplayDescriptorCount> writes{};
    for (u32 binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = descriptor_bindings[binding].descriptorType,
        };
    }
    writes[0].pBufferInfo = &buffer_infos[0];
    writes[1].pBufferInfo = &buffer_infos[1];
    writes[2].pBufferInfo = &buffer_infos[2];
    writes[3].pImageInfo = &image_infos[0];
    writes[4].pImageInfo = &image_infos[1];
    writes[5].pImageInfo = &image_infos[2];
    for (u32 binding = 6; binding < writes.size(); ++binding) {
        writes[binding].pBufferInfo = &buffer_infos[binding - 3];
    }
    if (instance.IsVertexInputDynamicState()) {
        constexpr std::array<vk::VertexInputBindingDescription2EXT, 0> no_bindings{};
        constexpr std::array<vk::VertexInputAttributeDescription2EXT, 0> no_attributes{};
        command.setVertexInputEXT(no_bindings, no_attributes);
    }
    command.bindIndexBuffer(*index_buffer->buffer, 0, vk::IndexType::eUint16);
    command.pushConstants(*pipeline_layout, AllGraphicsStageBits, 0, sizeof(bundle.push_data),
                          &bundle.push_data);
    command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, writes);

    command.setViewportWithCount(bundle.dynamic.viewports);
    command.setScissorWithCount(bundle.dynamic.scissors);
    command.setDepthTestEnable(bundle.dynamic.depth_test_enabled);
    command.setDepthWriteEnable(bundle.dynamic.depth_write_enabled);
    if (bundle.dynamic.depth_test_enabled) {
        command.setDepthCompareOp(bundle.dynamic.depth_compare_op);
    }
    if (instance.IsDepthBoundsSupported()) {
        command.setDepthBoundsTestEnable(bundle.dynamic.depth_bounds_test_enabled);
        if (bundle.dynamic.depth_bounds_test_enabled) {
            command.setDepthBounds(bundle.dynamic.depth_bounds_min,
                                   bundle.dynamic.depth_bounds_max);
        }
    }
    command.setDepthBiasEnable(bundle.dynamic.depth_bias_enabled);
    if (bundle.dynamic.depth_bias_enabled) {
        command.setDepthBias(bundle.dynamic.depth_bias_constant, bundle.dynamic.depth_bias_clamp,
                             bundle.dynamic.depth_bias_slope);
    }
    command.setStencilTestEnable(bundle.dynamic.stencil_test_enabled);
    if (bundle.dynamic.stencil_test_enabled) {
        command.setStencilOp(vk::StencilFaceFlagBits::eFront, bundle.dynamic.stencil_front.failOp,
                             bundle.dynamic.stencil_front.passOp,
                             bundle.dynamic.stencil_front.depthFailOp,
                             bundle.dynamic.stencil_front.compareOp);
        command.setStencilOp(vk::StencilFaceFlagBits::eBack, bundle.dynamic.stencil_back.failOp,
                             bundle.dynamic.stencil_back.passOp,
                             bundle.dynamic.stencil_back.depthFailOp,
                             bundle.dynamic.stencil_back.compareOp);
        command.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                    bundle.dynamic.stencil_front.reference);
        command.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                    bundle.dynamic.stencil_front.writeMask);
        command.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                      bundle.dynamic.stencil_front.compareMask);
    }
    command.setPrimitiveRestartEnable(bundle.dynamic.primitive_restart_enabled);
    command.setRasterizerDiscardEnable(bundle.dynamic.rasterizer_discard_enabled);
    command.setCullMode(bundle.dynamic.cull_mode);
    command.setFrontFace(bundle.dynamic.front_face);
    command.setBlendConstants(bundle.dynamic.blend_constants.data());
    if (instance.IsDynamicColorWriteMaskSupported()) {
        command.setColorWriteMaskEXT(0, bundle.graphics_key.write_masks);
    }
    command.setLineWidth(bundle.dynamic.line_width);

    const auto record_draw = [&] {
        command.beginRendering(rendering_info);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
        command.drawIndexedIndirectCount(*indirect_buffer->buffer, 0,
                                         *indirect_count_buffer->buffer, 0, 1,
                                         sizeof(DrawIndexedCommand));
        command.endRendering();
    };
    record_draw();

    std::array finished_barriers{
        ImageBarrier(*color->image, vk::ImageAspectFlagBits::eColor, bundle.color_layout,
                     vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eTransferRead),
        ImageBarrier(*depth->image, vk::ImageAspectFlagBits::eDepth, bundle.depth_layout,
                     vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                     vk::AccessFlagBits::eTransferRead),
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, finished_barriers);
    command.copyImageToBuffer(*color->image, vk::ImageLayout::eTransferSrcOptimal,
                              *color_readback->buffer, color_copy);
    command.copyImageToBuffer(*depth->image, vk::ImageLayout::eTransferSrcOptimal,
                              *depth_readback->buffer, depth_copy);

    std::array restore_barriers{
        ImageBarrier(*color->image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eTransferDstOptimal,
                     vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite),
        ImageBarrier(*depth->image, vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eTransferDstOptimal,
                     vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite),
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, restore_barriers);
    command.copyBufferToImage(*pre_color_staging->buffer, *color->image,
                              vk::ImageLayout::eTransferDstOptimal, color_copy);
    command.copyBufferToImage(*pre_depth_staging->buffer, *depth->image,
                              vk::ImageLayout::eTransferDstOptimal, depth_copy);

    std::array warm_ready_barriers{
        ImageBarrier(
            *color->image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
            bundle.color_layout, vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite),
        ImageBarrier(*depth->image, vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eTransferDstOptimal, bundle.depth_layout,
                     vk::AccessFlagBits::eTransferWrite,
                     vk::AccessFlagBits::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits::eDepthStencilAttachmentWrite),
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eAllGraphics, {}, {}, {},
                            warm_ready_barriers);
    record_draw();
    command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                vk::PipelineStageFlagBits::eLateFragmentTests,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, finished_barriers);
    command.copyImageToBuffer(*color->image, vk::ImageLayout::eTransferSrcOptimal,
                              *warm_color_readback->buffer, color_copy);
    command.copyImageToBuffer(*depth->image, vk::ImageLayout::eTransferSrcOptimal,
                              *warm_depth_readback->buffer, depth_copy);
    std::array host_barriers{
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *color_readback->buffer,
            .offset = 0,
            .size = color_readback->size,
        },
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *depth_readback->buffer,
            .offset = 0,
            .size = depth_readback->size,
        },
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *warm_color_readback->buffer,
            .offset = 0,
            .size = warm_color_readback->size,
        },
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *warm_depth_readback->buffer,
            .offset = 0,
            .size = warm_depth_readback->size,
        },
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *fault_buffer->buffer,
            .offset = 0,
            .size = fault_buffer->size,
        },
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *gds_buffer->buffer,
            .offset = u64{Shader::DreamsCompat::Vs370InterfaceCaptureBaseDword} * sizeof(u32),
            .size = u64{Shader::DreamsCompat::Vs370InterfaceCaptureDwordCount} * sizeof(u32),
        },
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *atlas_readback->buffer,
            .offset = 0,
            .size = atlas_readback->size,
        },
        vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *image1_readback->buffer,
            .offset = 0,
            .size = image1_readback->size,
        },
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer |
                                vk::PipelineStageFlagBits::eVertexShader |
                                vk::PipelineStageFlagBits::eFragmentShader,
                            vk::PipelineStageFlagBits::eHost, {}, {}, host_barriers, {});
    if (const auto result = command.end(); result != vk::Result::eSuccess) {
        outcome.error = "command-buffer end failed: " + vk::to_string(result);
        return outcome;
    }
    const vk::SubmitInfo submit{
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
    };
    const auto queue = instance.GetGraphicsQueue();
    if (const auto result = queue.submit(submit); result != vk::Result::eSuccess) {
        outcome.error = "replay queue submission failed: " + vk::to_string(result);
        return outcome;
    }
    if (const auto result = queue.waitIdle(); result != vk::Result::eSuccess) {
        outcome.error = "replay queue wait failed: " + vk::to_string(result);
        return outcome;
    }

    auto replay_color = ReadMappedBuffer(device, *color_readback, error);
    auto replay_depth = ReadMappedBuffer(device, *depth_readback, error);
    auto warm_replay_color = ReadMappedBuffer(device, *warm_color_readback, error);
    auto warm_replay_depth = ReadMappedBuffer(device, *warm_depth_readback, error);
    auto faults = ReadMappedBuffer(device, *fault_buffer, error);
    auto gds = ReadMappedBuffer(device, *gds_buffer, error);
    auto replay_atlas = ReadMappedBuffer(device, *atlas_readback, error);
    auto replay_image1 = ReadMappedBuffer(device, *image1_readback, error);
    if (!replay_color || !replay_depth || !warm_replay_color || !warm_replay_depth || !faults ||
        !gds || !replay_atlas || !replay_image1) {
        outcome.error = std::move(error);
        return outcome;
    }
    const auto color_path = options.output_directory / (options.output_stem + "-color.bin");
    const auto depth_path = options.output_directory / (options.output_stem + "-depth.bin");
    bool outputs_written = WriteBinary(color_path, *replay_color) &&
                           WriteBinary(depth_path, *replay_depth);
    if (options.write_warm_outputs) {
        outputs_written &=
            WriteBinary(options.output_directory / (options.output_stem + "-warm-color.bin"),
                        *warm_replay_color) &&
            WriteBinary(options.output_directory / (options.output_stem + "-warm-depth.bin"),
                        *warm_replay_depth);
    }
    if (!outputs_written) {
        outcome.error = "could not write replay attachment outputs";
        return outcome;
    }
    outcome.color_hash = HashBytes(*replay_color);
    outcome.depth_hash = HashBytes(*replay_depth);
    const u64 render_pixels = u64{bundle.render_width} * bundle.render_height * bundle.render_layers;
    const u64 color_pixel_bytes = render_pixels == 0 ? 0 : replay_color->size() / render_pixels;
    const u64 depth_pixel_bytes = render_pixels == 0 ? 0 : replay_depth->size() / render_pixels;
    outcome.changed_color_pixels =
        CountChangedElements(*replay_color, bundle.pre_color, color_pixel_bytes);
    outcome.changed_depth_pixels =
        CountChangedElements(*replay_depth, bundle.pre_depth, depth_pixel_bytes);
    outcome.completed = true;
    if (std::ranges::any_of(*faults, [](u8 value) { return value != 0; })) {
        outcome.error = "vertex or fragment shader reported an uncaptured BDA page fault";
        return outcome;
    }
    if (*replay_atlas != bundle.atlas_bricks) {
        outcome.error = "replayed atlas regions did not survive upload byte-for-byte";
        return outcome;
    }
    if (*replay_image1 != bundle.fs_image1) {
        outcome.error = "replayed fragment image1 did not survive upload byte-for-byte";
        return outcome;
    }
    const auto read_gds_word = [&](u32 dword) {
        u32 value{};
        std::memcpy(&value, gds->data() + u64{dword} * sizeof(u32), sizeof(value));
        return value;
    };
    const auto gds_bytes = std::span<const u8>{*gds};
    const auto interface_bytes = gds_bytes.subspan(
        u64{Shader::DreamsCompat::Vs370InterfaceCaptureBaseDword} * sizeof(u32),
        u64{Shader::DreamsCompat::Vs370InterfaceCaptureValueCount} * sizeof(u32));
    const auto layer_bytes = gds_bytes.subspan(
        u64{Shader::DreamsCompat::Vs370InterfaceCaptureLayerBaseDword} * sizeof(u32),
        u64{Shader::DreamsCompat::Vs370InterfaceCaptureLayerCount} * sizeof(u32));
    const auto validity_bytes = gds_bytes.subspan(
        u64{Shader::DreamsCompat::Vs370InterfaceCaptureValidityBaseDword} * sizeof(u32),
        u64{Shader::DreamsCompat::Vs370InterfaceCaptureValidityCount} * sizeof(u32));
    if (!WriteBinary(options.output_directory / (options.output_stem + "-vs-interface.bin"),
                     interface_bytes) ||
        !WriteBinary(options.output_directory /
                         (options.output_stem + "-vs-render-target-index.bin"),
                     layer_bytes) ||
        !WriteBinary(options.output_directory / (options.output_stem + "-vs-validity.bin"),
                     validity_bytes)) {
        outcome.error = "could not write replayed vertex-interface diagnostics";
        return outcome;
    }
    for (u32 instance_index = 0; instance_index < bundle.instance_count; ++instance_index) {
        for (u32 vertex_index = 0; vertex_index < VerticesPerInstance; ++vertex_index) {
            const u32 vertex = instance_index * VerticesPerInstance + vertex_index;
            const u32 validity_dword =
                Shader::DreamsCompat::Vs370InterfaceCaptureValidityBaseDword + vertex;
            if (read_gds_word(validity_dword) != ExpectedValidityMask) {
                outcome.error = "captured vertex shader did not emit a complete interface at "
                                "instance " +
                                std::to_string(instance_index) + ", vertex " +
                                std::to_string(vertex_index);
                return outcome;
            }
            if (!options.compare_capture) {
                continue;
            }
            for (u32 component = 0; component < InterfaceWordsPerVertex; ++component) {
                const u32 replay_value =
                    read_gds_word(Shader::DreamsCompat::Vs370InterfaceCaptureBaseDword +
                                  vertex * InterfaceWordsPerVertex + component);
                const u32 expected_value =
                    bundle.post_vs_records[vertex * ReplayWordsPerVertex + component];
                if (replay_value != expected_value) {
                    outcome.error = fmt::format(
                        "captured vertex shader interface mismatch at instance {}, vertex {}, "
                        "component {}: replay={:#010x} expected={:#010x}",
                        instance_index, vertex_index, component, replay_value, expected_value);
                    return outcome;
                }
            }
            const u32 replay_layer =
                read_gds_word(Shader::DreamsCompat::Vs370InterfaceCaptureLayerBaseDword + vertex);
            const u32 expected_layer =
                bundle.post_vs_records[vertex * ReplayWordsPerVertex + InterfaceWordsPerVertex];
            if (replay_layer != expected_layer) {
                outcome.error = "captured vertex shader layer mismatch at instance " +
                                std::to_string(instance_index) + ", vertex " +
                                std::to_string(vertex_index);
                return outcome;
            }
        }
    }
    const bool cold_warm_color_equal = *replay_color == *warm_replay_color;
    const bool cold_warm_depth_equal = *replay_depth == *warm_replay_depth;
    outcome.deterministic = cold_warm_color_equal && cold_warm_depth_equal;
    if (!cold_warm_color_equal || !cold_warm_depth_equal) {
        outcome.error = std::string{"offline repeated draw changed output: cold/warm color="} +
                        (cold_warm_color_equal ? "equal" : "different") +
                        ", depth=" + (cold_warm_depth_equal ? "equal" : "different");
        return outcome;
    }
    if (!options.compare_capture) {
        outcome.parity = true;
        return outcome;
    }
    const bool color_equal = *replay_color == bundle.post_color;
    const bool depth_equal = *replay_depth == bundle.post_depth;
    const bool warm_oracle_color_equal = *warm_replay_color == bundle.post_color;
    const bool warm_oracle_depth_equal = *warm_replay_depth == bundle.post_depth;
    outcome.parity = color_equal && depth_equal;
    if (!outcome.parity) {
        outcome.error =
            std::string{"exact oracle mismatch: color="} + (color_equal ? "equal" : "different") +
            ", depth=" + (depth_equal ? "equal" : "different") +
            "; second draw unchanged; warm/oracle color=" +
            (warm_oracle_color_equal ? "equal" : "different") + ", depth=" +
            (warm_oracle_depth_equal ? "equal" : "different");
    }
    return outcome;
}

bool RunOffscreenSmoke(const Instance& instance) {
    constexpr vk::DeviceSize PixelBytes = 4;
    const auto device = instance.GetDevice();

    // Declare memory before its bound resource so reverse destruction releases the resource first.
    vk::UniqueDeviceMemory image_memory{};
    vk::UniqueImage image{};
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = vk::Extent3D{.width = 1, .height = 1, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    auto [image_result, created_image] = device.createImageUnique(image_info);
    if (image_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to create image: {}",
                  vk::to_string(image_result));
        return false;
    }
    image = std::move(created_image);

    const auto image_requirements = device.getImageMemoryRequirements(*image);
    const auto image_type = FindMemoryType(instance, image_requirements.memoryTypeBits, {},
                                           vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (!image_type) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: no compatible image memory type");
        return false;
    }
    const vk::MemoryAllocateInfo image_allocate = {
        .allocationSize = image_requirements.size,
        .memoryTypeIndex = *image_type,
    };
    auto [image_memory_result, allocated_image_memory] =
        device.allocateMemoryUnique(image_allocate);
    if (image_memory_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to allocate image memory: {}",
                  vk::to_string(image_memory_result));
        return false;
    }
    image_memory = std::move(allocated_image_memory);
    if (const auto result = device.bindImageMemory(*image, *image_memory, 0);
        result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to bind image memory: {}",
                  vk::to_string(result));
        return false;
    }

    vk::UniqueDeviceMemory readback_memory{};
    vk::UniqueBuffer readback{};
    const vk::BufferCreateInfo buffer_info = {
        .size = PixelBytes,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    auto [buffer_result, created_buffer] = device.createBufferUnique(buffer_info);
    if (buffer_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to create readback buffer: {}",
                  vk::to_string(buffer_result));
        return false;
    }
    readback = std::move(created_buffer);

    const auto buffer_requirements = device.getBufferMemoryRequirements(*readback);
    const auto buffer_type = FindMemoryType(instance, buffer_requirements.memoryTypeBits,
                                            vk::MemoryPropertyFlagBits::eHostVisible |
                                                vk::MemoryPropertyFlagBits::eHostCoherent);
    if (!buffer_type) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: no coherent host-visible memory type");
        return false;
    }
    const vk::MemoryAllocateInfo buffer_allocate = {
        .allocationSize = buffer_requirements.size,
        .memoryTypeIndex = *buffer_type,
    };
    auto [buffer_memory_result, allocated_buffer_memory] =
        device.allocateMemoryUnique(buffer_allocate);
    if (buffer_memory_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to allocate readback memory: {}",
                  vk::to_string(buffer_memory_result));
        return false;
    }
    readback_memory = std::move(allocated_buffer_memory);
    if (const auto result = device.bindBufferMemory(*readback, *readback_memory, 0);
        result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to bind readback memory: {}",
                  vk::to_string(result));
        return false;
    }

    const vk::CommandPoolCreateInfo pool_info = {
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    auto [pool_result, command_pool] = device.createCommandPoolUnique(pool_info);
    if (pool_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to create command pool: {}",
                  vk::to_string(pool_result));
        return false;
    }

    const vk::CommandBufferAllocateInfo command_allocate = {
        .commandPool = *command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::CommandBuffer command;
    if (const auto result = device.allocateCommandBuffers(&command_allocate, &command);
        result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to allocate command buffer: {}",
                  vk::to_string(result));
        return false;
    }

    if (const auto result =
            command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to begin command buffer: {}",
                  vk::to_string(result));
        return false;
    }

    const vk::ImageSubresourceRange color_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier to_clear = {
        .srcAccessMask = {},
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *image,
        .subresourceRange = color_range,
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_clear);

    const vk::ClearColorValue smoke_color{std::array<float, 4>{1.0f, 0.0f, 1.0f, 1.0f}};
    command.clearColorImage(*image, vk::ImageLayout::eTransferDstOptimal, smoke_color, color_range);

    const vk::ImageMemoryBarrier to_copy = {
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *image,
        .subresourceRange = color_range,
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_copy);

    const vk::BufferImageCopy copy = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            vk::ImageSubresourceLayers{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = vk::Offset3D{.x = 0, .y = 0, .z = 0},
        .imageExtent = vk::Extent3D{.width = 1, .height = 1, .depth = 1},
    };
    command.copyImageToBuffer(*image, vk::ImageLayout::eTransferSrcOptimal, *readback, copy);

    const vk::BufferMemoryBarrier to_host = {
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = *readback,
        .offset = 0,
        .size = PixelBytes,
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, {}, to_host, {});
    if (const auto result = command.end(); result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to end command buffer: {}",
                  vk::to_string(result));
        return false;
    }

    const vk::SubmitInfo submit = {
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
    };
    const auto queue = instance.GetGraphicsQueue();
    if (const auto result = queue.submit(submit); result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: queue submit failed: {}",
                  vk::to_string(result));
        return false;
    }
    if (const auto result = queue.waitIdle(); result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: queue wait failed: {}",
                  vk::to_string(result));
        return false;
    }

    void* mapped{};
    if (const auto result = device.mapMemory(*readback_memory, 0, PixelBytes, {}, &mapped);
        result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: failed to map readback memory: {}",
                  vk::to_string(result));
        return false;
    }
    std::array<u8, PixelBytes> pixel{};
    std::memcpy(pixel.data(), mapped, pixel.size());
    device.unmapMemory(*readback_memory);

    constexpr std::array<u8, PixelBytes> Expected{255, 0, 255, 255};
    if (pixel != Expected) {
        LOG_ERROR(Render_Vulkan, "Dreams replay smoke: unexpected readback [{}, {}, {}, {}]",
                  pixel[0], pixel[1], pixel[2], pixel[3]);
        return false;
    }
    return true;
}

int RunCandidateBatch(const Instance& instance, const ReplayBundle& baseline,
                      const std::filesystem::path& bundle_path,
                      const std::filesystem::path& candidate_directory) {
    auto loaded = LoadReplayCandidates(candidate_directory, baseline);
    if (!loaded.error.empty()) {
        LOG_ERROR(Render_Vulkan, "Dreams replay candidate directory rejected: {}", loaded.error);
        return 2;
    }

    const auto output_directory = bundle_path / "candidate-results";
    std::error_code fs_error;
    std::filesystem::create_directories(output_directory, fs_error);
    if (fs_error) {
        LOG_ERROR(Render_Vulkan, "Could not create Dreams candidate output directory {}: {}",
                  output_directory.string(), fs_error.message());
        return 2;
    }
    std::ofstream tsv(output_directory / "results.tsv", std::ios::trunc);
    if (!tsv) {
        LOG_ERROR(Render_Vulkan, "Could not create Dreams candidate results.tsv");
        return 2;
    }
    tsv << "ordinal\tname\tsource\tinstances\tcompleted\tdeterministic\tcolor_hash\t"
           "depth_hash\tchanged_color_pixels\tchanged_depth_pixels\tcolor_file\tdepth_file\t"
           "error\n";

    nlohmann::json report{
        {"schema", 1},
        {"bundle", bundle_path.string()},
        {"candidate_directory", candidate_directory.string()},
        {"gpu", instance.GetModelName()},
        {"candidates", nlohmann::json::array()},
    };
    ReplayBundle candidate_bundle = baseline;
    bool failed{};
    for (u32 ordinal = 0; ordinal < loaded.candidates.size(); ++ordinal) {
        const auto& candidate = loaded.candidates[ordinal];
        std::ranges::fill(candidate_bundle.vs_buffers[3], u8{});
        const u64 visibility_offset = candidate_bundle.first_record * 2 * sizeof(u32);
        std::memcpy(candidate_bundle.vs_buffers[3].data() + visibility_offset,
                    candidate.visibility.data(), candidate.visibility.size());
        candidate_bundle.visibility = candidate.visibility;
        candidate_bundle.instance_count = candidate.instance_count;
        candidate_bundle.draw.instance_count = candidate.instance_count;

        const std::string stem = fmt::format("candidate-{:04}", ordinal);
        const ReplayOptions options{
            .output_directory = output_directory,
            .output_stem = stem,
            .compare_capture = false,
            .write_warm_outputs = false,
        };
        LOG_INFO(Render_Vulkan, "Replaying Dreams candidate {}/{} name={} instances={}",
                 ordinal + 1, loaded.candidates.size(), candidate.name,
                 candidate.instance_count);
        auto outcome = RunExactReplay(instance, candidate_bundle, options);
        const bool succeeded = outcome.completed && outcome.parity && outcome.error.empty();
        failed |= !succeeded;
        const std::string color_file = stem + "-color.bin";
        const std::string depth_file = stem + "-depth.bin";
        std::string tsv_error = outcome.error;
        std::ranges::replace(tsv_error, '\t', ' ');
        std::ranges::replace(tsv_error, '\r', ' ');
        std::ranges::replace(tsv_error, '\n', ' ');
        tsv << ordinal << '\t' << candidate.name << '\t' << candidate.source.filename().string()
            << '\t' << candidate.instance_count << '\t' << outcome.completed << '\t'
            << outcome.deterministic << '\t' << fmt::format("{:#x}", outcome.color_hash) << '\t'
            << fmt::format("{:#x}", outcome.depth_hash) << '\t'
            << outcome.changed_color_pixels << '\t' << outcome.changed_depth_pixels << '\t'
            << color_file << '\t' << depth_file << '\t' << tsv_error << '\n';
        tsv.flush();

        report["candidates"].push_back({
            {"ordinal", ordinal},
            {"name", candidate.name},
            {"source", candidate.source.filename().string()},
            {"source_hash", fmt::format("{:#x}", HashBytes(candidate.visibility))},
            {"instances", candidate.instance_count},
            {"completed", outcome.completed},
            {"deterministic", outcome.deterministic},
            {"color_hash", fmt::format("{:#x}", outcome.color_hash)},
            {"depth_hash", fmt::format("{:#x}", outcome.depth_hash)},
            {"changed_color_pixels", outcome.changed_color_pixels},
            {"changed_depth_pixels", outcome.changed_depth_pixels},
            {"color_file", color_file},
            {"depth_file", depth_file},
            {"error", outcome.error},
        });
    }
    std::ofstream json_output(output_directory / "results.json", std::ios::trunc);
    if (!json_output) {
        LOG_ERROR(Render_Vulkan, "Could not create Dreams candidate results.json");
        return 2;
    }
    json_output << report.dump(2) << '\n';
    if (!json_output || !tsv) {
        LOG_ERROR(Render_Vulkan, "Could not finish Dreams candidate result files");
        return 2;
    }
    if (failed) {
        LOG_ERROR(Render_Vulkan,
                  "One or more Dreams replay candidates failed; inspect {}/results.tsv",
                  output_directory.string());
        return 3;
    }
    LOG_INFO(Render_Vulkan, "Dreams replay completed {} candidates; results are in {}",
             loaded.candidates.size(), output_directory.string());
    return 0;
}

} // Anonymous namespace

int Run(const std::filesystem::path& bundle_path,
        const std::optional<std::filesystem::path>& candidate_directory) {
    if (!std::filesystem::is_directory(bundle_path)) {
        LOG_ERROR(Render_Vulkan, "Dreams replay bundle is not a directory: {}",
                  bundle_path.string());
        return 2;
    }

    LOG_INFO(Render_Vulkan, "Starting offline Dreams draw replay from {}", bundle_path.string());
    auto loaded = LoadBundle(bundle_path);
    if (!loaded.bundle) {
        WriteStatus(bundle_path, loaded.stage, loaded.error, loaded.missing_files);
        LOG_ERROR(Render_Vulkan, "Dreams replay rejected bundle at {}: {}", loaded.stage,
                  loaded.error);
        return 2;
    }

    Instance instance{HeadlessDevice, EmulatorSettings.GetGpuId(),
                      EmulatorSettings.IsVkValidationEnabled(),
                      EmulatorSettings.IsVkCrashDiagnosticEnabled()};
    if (candidate_directory) {
        return RunCandidateBatch(instance, *loaded.bundle, bundle_path, *candidate_directory);
    }
    const ReplayOptions options{.output_directory = bundle_path};
    auto replay = RunExactReplay(instance, *loaded.bundle, options);
    if (!replay.completed) {
        WriteStatus(bundle_path, "draw_execution", replay.error);
        LOG_ERROR(Render_Vulkan, "Offline Dreams replay execution failed on {}: {}",
                  instance.GetModelName(), replay.error);
        return 3;
    }
    if (!replay.parity) {
        WriteStatus(bundle_path, "oracle_compare", replay.error);
        LOG_ERROR(Render_Vulkan, "Offline Dreams replay did not match the capture on {}: {}",
                  instance.GetModelName(), replay.error);
        return 4;
    }
    WriteStatus(bundle_path, "parity_verified", {}, {}, true);
    LOG_INFO(Render_Vulkan, "Offline Dreams replay exactly matched color/depth on {}",
             instance.GetModelName());
    return 0;
}

} // namespace Vulkan::DreamsDrawReplay
