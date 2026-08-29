// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

#extension GL_ARB_shader_viewport_layer_array : require

// One record contains the raw post-VS bits captured for a single vertex:
// Position, Param0, Param1, Param2, and the render-target layer. Keeping the
// storage as uints prevents the host from canonicalizing NaNs or otherwise
// changing a floating-point bit pattern before it reaches the fragment stage.
layout(set = 0, binding = 6, std430) readonly buffer CapturedPostVs {
    uint words[];
} captured_post_vs;

layout(location = 0) out vec4 out_attr0;
layout(location = 1) out vec4 out_attr1;
layout(location = 2) out vec4 out_attr2;

const uint VerticesPerInstance = 8u;
const uint WordsPerVertex = 17u;

vec4 LoadVec4(uint word_offset) {
    return uintBitsToFloat(uvec4(captured_post_vs.words[word_offset + 0u],
                                captured_post_vs.words[word_offset + 1u],
                                captured_post_vs.words[word_offset + 2u],
                                captured_post_vs.words[word_offset + 3u]));
}

void main() {
    const uint record_index =
        uint(gl_InstanceIndex) * VerticesPerInstance + uint(gl_VertexIndex);
    const uint word_offset = record_index * WordsPerVertex;

    gl_Position = LoadVec4(word_offset + 0u);
    out_attr0 = LoadVec4(word_offset + 4u);
    out_attr1 = LoadVec4(word_offset + 8u);
    out_attr2 = LoadVec4(word_offset + 12u);
    gl_Layer = int(captured_post_vs.words[word_offset + 16u]);
}
