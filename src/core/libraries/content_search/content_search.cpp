// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "content_search.h"

#include <bit>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/memory.h"

namespace Libraries::ContentSearch {

static bool TryWriteS32(u64 address, s32 value) {
    if (address == 0) {
        return false;
    }

    auto* memory = Common::Singleton<Core::MemoryManager>::Instance();
    if (memory == nullptr || !memory->IsValidMapping(address, sizeof(value))) {
        return false;
    }

    *std::bit_cast<s32*>(address) = value;
    return true;
}

s32 PS4_SYSV_ABI sceContentSearchInit(u64 init_param) {
    LOG_INFO(Lib_AppContent, "sceContentSearchInit: init_param={:#x}", init_param);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchGetMyApplicationIndex(u64 out_index) {
    const bool wrote_index = TryWriteS32(out_index, 0);
    LOG_INFO(Lib_AppContent, "sceContentSearchGetMyApplicationIndex: out_index={:#x} wrote={}",
             out_index, wrote_index);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchGetMetadataValue(u64 metadata_handle, u64 field_name, u64 value,
                                                  u64 value_size) {
    LOG_INFO(Lib_AppContent,
             "sceContentSearchGetMetadataValue: metadata_handle={:#x} field_name={:#x} value={:#x} "
             "value_size={:#x}",
             metadata_handle, field_name, value, value_size);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchSearchContent(u64 condition, u64 sort, u64 result) {
    LOG_INFO(Lib_AppContent,
             "sceContentSearchSearchContent: condition={:#x} sort={:#x} result={:#x}", condition,
             sort, result);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchOpenMetadataByContentId(u64 content_id, u64 metadata_handle) {
    LOG_INFO(Lib_AppContent,
             "sceContentSearchOpenMetadataByContentId: content_id={:#x} metadata_handle={:#x}",
             content_id, metadata_handle);
    TryWriteS32(metadata_handle, 1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchCloseMetadata(u64 metadata_handle) {
    LOG_INFO(Lib_AppContent, "sceContentSearchCloseMetadata: metadata_handle={:#x}",
             metadata_handle);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchGetMetadataFieldInfo(u64 field_name, u64 field_info) {
    LOG_INFO(Lib_AppContent,
             "sceContentSearchGetMetadataFieldInfo: field_name={:#x} field_info={:#x}", field_name,
             field_info);
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("dPj4ZtRcIWk", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchInit);
    LIB_FUNCTION("FRT4EYtZU1Y", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMyApplicationIndex);
    LIB_FUNCTION("ruNe-FgCzO8", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMetadataValue);
    LIB_FUNCTION("TEW3IKxYfXc", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchSearchContent);
    LIB_FUNCTION("bjAlYWwRTJA", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchOpenMetadataByContentId);
    LIB_FUNCTION("-YbpaF0XS-I", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchCloseMetadata);
    LIB_FUNCTION("EbNufIY0Zvc", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMetadataFieldInfo);
}

} // namespace Libraries::ContentSearch
