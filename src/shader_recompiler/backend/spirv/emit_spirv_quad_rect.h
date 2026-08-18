// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"

namespace Shader {
struct FragmentRuntimeInfo;
}

namespace Shader::Backend::SPIRV {

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      const FragmentRuntimeInfo& fs_info,
                                                      u32 vs_output_param_mask = ~0U,
                                                      bool passthrough_point_size = false,
                                                      bool passthrough_layer = false,
                                                      bool passthrough_viewport = false);

} // namespace Shader::Backend::SPIRV
