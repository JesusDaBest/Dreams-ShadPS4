// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>

namespace Vulkan::DreamsDrawReplay {

/// Starts the offline Dreams draw-replay path using a self-contained capture directory.
int Run(const std::filesystem::path& bundle_path,
        const std::optional<std::filesystem::path>& candidate_directory = std::nullopt);

} // namespace Vulkan::DreamsDrawReplay
