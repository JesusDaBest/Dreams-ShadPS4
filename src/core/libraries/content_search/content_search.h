// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::ContentSearch {

s32 PS4_SYSV_ABI sceContentSearchInit(u64 init_param);
s32 PS4_SYSV_ABI sceContentSearchGetMyApplicationIndex(u64 out_index);
s32 PS4_SYSV_ABI sceContentSearchGetMetadataValue(u64 metadata_handle, u64 field_name, u64 value,
                                                  u64 value_size);
s32 PS4_SYSV_ABI sceContentSearchSearchContent(u64 condition, u64 sort, u64 result);
s32 PS4_SYSV_ABI sceContentSearchOpenMetadataByContentId(u64 content_id, u64 metadata_handle);
s32 PS4_SYSV_ABI sceContentSearchCloseMetadata(u64 metadata_handle);
s32 PS4_SYSV_ABI sceContentSearchGetMetadataFieldInfo(u64 field_name, u64 field_info);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::ContentSearch
