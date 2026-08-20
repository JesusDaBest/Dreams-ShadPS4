// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/memory_patcher.h"
#include "common/signal_context.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

#ifndef _WIN32
namespace Libraries::Kernel {
void SigactionHandler(int native_signum, siginfo_t* inf, ucontext_t* raw_context);
extern std::array<OrbisKernelExceptionHandler, 32> Handlers;
} // namespace Libraries::Kernel
#endif

namespace Core {

#if defined(_WIN32)

static void AppendDreamsCpuRootTrace(const char* buffer, const int length) noexcept {
    if (length <= 0) {
        return;
    }
    const HANDLE file = CreateFileW(L"dreams-cpu-root-trace.txt", FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
}

static bool DreamsStampTraceEnabled() noexcept {
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("SHADPS4_DREAMS_STAMP_TRACE", value, sizeof(value)) != 0 &&
               value[0] == '1';
    }();
    return enabled;
}

static bool DreamsStampTraceCaptureEnabled() noexcept {
    return DreamsStampTraceEnabled() &&
           GetFileAttributesW(L"dreams-stamp-trace.capture") != INVALID_FILE_ATTRIBUTES;
}

static std::atomic<u64> dreams_active_scene_publish_tick{0};
static std::atomic<u64> dreams_active_scene_root{0};

static u32 ReadDreamsRootCount(const HANDLE process, const u64 root) noexcept {
    if (root == 0) {
        return 0;
    }
    u32 count = 0;
    SIZE_T bytes_read = 0;
    ReadProcessMemory(process, reinterpret_cast<const void*>(root + 0x10b58c0), &count,
                      sizeof(count), &bytes_read);
    return count;
}

static u64 ReadDreamsU64(const HANDLE process, const u64 address) noexcept {
    u64 value = 0;
    SIZE_T bytes_read = 0;
    ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value),
                      &bytes_read);
    return value;
}

static u32 ReadDreamsU32(const HANDLE process, const u64 address) noexcept {
    u32 value = 0;
    SIZE_T bytes_read = 0;
    ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value),
                      &bytes_read);
    return value;
}

static u8 ReadDreamsU8(const HANDLE process, const u64 address) noexcept {
    u8 value = 0;
    SIZE_T bytes_read = 0;
    ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value),
                      &bytes_read);
    return value;
}

static u16 ReadDreamsU16(const HANDLE process, const u64 address) noexcept {
    u16 value = 0;
    SIZE_T bytes_read = 0;
    ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value),
                      &bytes_read);
    return value;
}

static bool HandleDreamsRetirementWatermarkWrite(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsRetirementWatermarkWriteOffset = 0x9aaf51;
    constexpr u64 DreamsResourceFloorGlobalOffset = 0x44d4b40;
    constexpr u64 DreamsResourceCurrentGlobalOffset = 0x44d4b44;
    constexpr u64 DreamsResourceWatermarkGlobalOffset = 0x44d4b48;
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    if (breakpoint_address - MemoryPatcher::g_eboot_address !=
        DreamsRetirementWatermarkWriteOffset) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u32 original_value = static_cast<u32>(context->Rax);
    const u32 resource_floor = ReadDreamsU32(
        process, MemoryPatcher::g_eboot_address + DreamsResourceFloorGlobalOffset);
    const u32 resource_current = ReadDreamsU32(
        process, MemoryPatcher::g_eboot_address + DreamsResourceCurrentGlobalOffset);
    const u32 resource_watermark = ReadDreamsU32(
        process, MemoryPatcher::g_eboot_address + DreamsResourceWatermarkGlobalOffset);
    const u64 now = GetTickCount64();
    const u64 publish_tick = dreams_active_scene_publish_tick.load(std::memory_order_relaxed);
    const bool recent_scene_publish = DreamsStampTraceCaptureEnabled() && publish_tick != 0 &&
                                      now - publish_tick <= 250;

    u32 guarded_value = original_value;
    if (recent_scene_publish && original_value < resource_current &&
        (resource_floor == resource_current || resource_floor == resource_current + 1)) {
        guarded_value = resource_current;
    }

    SIZE_T bytes_written = 0;
    WriteProcessMemory(
        process,
        reinterpret_cast<void*>(MemoryPatcher::g_eboot_address +
                                DreamsResourceWatermarkGlobalOffset),
        &guarded_value, sizeof(guarded_value), &bytes_written);

    if (DreamsStampTraceCaptureEnabled() || guarded_value != original_value) {
        char buffer[384]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "watermark_write thread=%lu root=0x%016llx age=%llu recent=%u "
            "resources=%u,%u,%u value=%u->%u guarded=%u\r\n",
            GetCurrentThreadId(),
            static_cast<unsigned long long>(
                dreams_active_scene_root.load(std::memory_order_relaxed)),
            publish_tick == 0 ? ~0ull : static_cast<unsigned long long>(now - publish_tick),
            recent_scene_publish ? 1 : 0, resource_floor, resource_current, resource_watermark,
            original_value, guarded_value, guarded_value != original_value ? 1 : 0);
        AppendDreamsCpuRootTrace(buffer, length);
    }

    // Emulate the replaced `mov dword ptr [watermark], eax`.
    context->Rip = breakpoint_address + 6;
    return true;
}

static void ApplyDreamsIncFlags(CONTEXT* context, const u32 previous, const u32 result) noexcept {
    constexpr u32 ParityFlag = 1u << 2;
    constexpr u32 AuxiliaryCarryFlag = 1u << 4;
    constexpr u32 ZeroFlag = 1u << 6;
    constexpr u32 SignFlag = 1u << 7;
    constexpr u32 OverflowFlag = 1u << 11;
    constexpr u32 UpdatedFlags =
        ParityFlag | AuxiliaryCarryFlag | ZeroFlag | SignFlag | OverflowFlag;

    u32 flags = context->EFlags & ~UpdatedFlags;
    u8 parity = static_cast<u8>(result);
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;
    if ((parity & 1) == 0) {
        flags |= ParityFlag;
    }
    if ((previous & 0xf) == 0xf) {
        flags |= AuxiliaryCarryFlag;
    }
    if (result == 0) {
        flags |= ZeroFlag;
    }
    if ((result & 0x80000000u) != 0) {
        flags |= SignFlag;
    }
    if (previous == 0x7fffffffu) {
        flags |= OverflowFlag;
    }
    context->EFlags = flags;
}

static bool HandleDreamsSaveQuotaTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsSaveQuotaInputsOffset = 0x552c76;
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    if (breakpoint_address - MemoryPatcher::g_eboot_address != DreamsSaveQuotaInputsOffset) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u32 total_blocks = ReadDreamsU32(process, context->Rsp + 0x200);
    const u32 free_blocks = ReadDreamsU32(process, context->Rsp + 0x208);
    const u32 requested_bytes = ReadDreamsU32(process, context->Rsp + 0x28);
    const u64 metadata_bytes = ReadDreamsU64(process, context->R15 + 0xca50b0);
    const u64 savedata_bytes = ReadDreamsU64(process, context->R15 + 0xca50b8);

    char buffer[512]{};
    const int length = _snprintf_s(
        buffer, sizeof(buffer), _TRUNCATE,
        "save_quota thread=%lu object=0x%016llx total_blocks=%u free_blocks=%u "
        "requested_bytes=0x%08x metadata_bytes=0x%016llx savedata_bytes=0x%016llx "
        "rax=0x%016llx rbx=0x%016llx\r\n",
        GetCurrentThreadId(), static_cast<unsigned long long>(context->R15), total_blocks,
        free_blocks, requested_bytes, static_cast<unsigned long long>(metadata_bytes),
        static_cast<unsigned long long>(savedata_bytes),
        static_cast<unsigned long long>(context->Rax),
        static_cast<unsigned long long>(context->Rbx));
    AppendDreamsCpuRootTrace(buffer, length);

    // Emulate the replaced `mov r13d, dword ptr [rsp + 0x208]`.
    context->R13 = free_blocks;
    context->Rip = MemoryPatcher::g_eboot_address + 0x552c7e;
    return true;
}

static bool HandleDreamsOfflineLimitsTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsOfflineLimitsResponseOffset = 0x5676c0;
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    if (breakpoint_address - MemoryPatcher::g_eboot_address !=
        DreamsOfflineLimitsResponseOffset) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u64 response = ReadDreamsU64(process, context->Rdx + 0x8);
    const u64 response_size = ReadDreamsU64(process, context->Rdx + 0x10);
    const u64 status = ReadDreamsU64(process, context->Rdx + 0x18);
    char response_text[2049]{};
    SIZE_T bytes_read = 0;
    const SIZE_T requested = static_cast<SIZE_T>(std::min<u64>(response_size, 2048));
    if (response != 0 && requested != 0) {
        ReadProcessMemory(process, reinterpret_cast<const void*>(response), response_text,
                          requested, &bytes_read);
    }
    for (SIZE_T i = 0; i < bytes_read; ++i) {
        const unsigned char value = static_cast<unsigned char>(response_text[i]);
        if (value < 0x20 && value != '\r' && value != '\n' && value != '\t') {
            response_text[i] = ' ';
        }
    }
    response_text[bytes_read] = '\0';

    char header[256]{};
    const int header_length = _snprintf_s(
        header, sizeof(header), _TRUNCATE,
        "offline_limits thread=%lu status=%llu response=0x%016llx bytes=%llu captured=%llu\r\n",
        GetCurrentThreadId(), static_cast<unsigned long long>(status),
        static_cast<unsigned long long>(response),
        static_cast<unsigned long long>(response_size),
        static_cast<unsigned long long>(bytes_read));
    AppendDreamsCpuRootTrace(header, header_length);
    AppendDreamsCpuRootTrace(response_text, static_cast<int>(bytes_read));
    AppendDreamsCpuRootTrace("\r\n", 2);

    // Emulate the replaced `mov rsi, qword ptr [rdx + 8]`.
    context->Rsi = response;
    context->Rip = MemoryPatcher::g_eboot_address + DreamsOfflineLimitsResponseOffset + 4;
    return true;
}

static bool HandleDreamsCpuRootTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsRecordBuilderOffset = 0x8b7380;
    constexpr u64 DreamsActiveMapOffset = 0x8b81c4;
    constexpr u64 DreamsFirstPrepassCompleteOffset = 0x8b851f;
    constexpr u64 DreamsGroupHashLookupOffset = 0x8b872f;
    constexpr u64 DreamsSecondPrepassStartOffset = 0x8b8860;
    constexpr u64 DreamsSecondPrepassStepOffset = 0x8b8870;
    constexpr u64 DreamsObjectPrepareCallOffset = 0x8b8a22;
    constexpr u64 DreamsObjectPrepareReturnOffset = 0x8b8a31;
    constexpr u64 DreamsOutputGateOffset = 0x8b8a67;
    constexpr u64 DreamsObjectDispatchOffset = 0x8b8c53;
    constexpr u64 DreamsStrokeDispatchOffset = 0x8b8cfd;
    constexpr u64 DreamsStrokeLookupPrepareOffset = 0x8b8ffd;
    constexpr u64 DreamsStrokeLookupResultOffset = 0x8b905e;
    constexpr u64 DreamsRecordGateOffset = 0x8b9867;
    constexpr u64 DreamsRecordEmitOffset = 0x8ba0a9;
    constexpr u64 DreamsStrokeRecordEmitOffset = 0x8bb372;
    constexpr u64 DreamsStrokeRecordConsumeOffset = 0xeb8dd7;
    constexpr u64 DreamsStrokeRecordVisibleOffset = 0xeb8f95;
    constexpr u64 DreamsStrokeRecordQueuedOffset = 0xeb9153;
    constexpr u64 DreamsGlobalRecordConsumeOffset = 0x13ce236;
    constexpr u64 DreamsGlobalRecordOffset = 0x13ce28f;
    constexpr u64 DreamsGlobalRecordCountOffset = 0x84de770;
    constexpr u64 DreamsGpuBatchBuildOffset = 0x13d6788;
    constexpr u64 DreamsGpuBatchDispatchOffset = 0x13d68f7;
    constexpr u64 DreamsGpuStagingCountGlobalOffset = 0x7da9f88;
    constexpr u64 DreamsGpuCombinedCountGlobalOffset = 0x7daa024;
    constexpr u64 DreamsGpuStagingPointerGlobalOffset = 0x7daa038;
    constexpr u64 DreamsOutputContextGlobalOffset = 0x6667e68;
    constexpr u64 DreamsRecordEnableGlobalOffset = 0x6667e70;
    constexpr u64 DreamsHighDispatchOffset = 0x101e400;
    constexpr u64 DreamsSceneGateTraceOffset = 0x9c44b4;
    constexpr u64 DreamsSceneGateResumeOffset = 0x9c44c0;
    constexpr u64 DreamsRootPublishTraceOffset = 0x9c469f;
    constexpr u64 DreamsResourcePromotionOffset = 0x9c47d4;
    constexpr u64 DreamsResourcePromotionResumeOffset = 0x9c47f2;
    const auto is_unresolved_consumer = [](const u64 offset) noexcept {
        switch (offset) {
        case 0x76977f:
        case 0x98c8d8:
        case 0x98d21d:
        case 0xa47644:
        case 0xc1259c:
        case 0xc131aa:
        case 0xbb090a:
        case 0xebb4c4:
        case 0xebbbad:
        case 0xeba582:
        case 0x1098ead:
        case 0x10aee07:
        case 0x13bcfc9:
        case 0x13bd509:
        case 0x14f5b6b:
            return true;
        default:
            return false;
        }
    };
    const auto is_unresolved_bitset = [](const u64 offset) noexcept {
        return offset == 0x15c0984 || offset == 0x15c1624;
    };
    constexpr u64 DreamsResourceFloorGlobalOffset = 0x44d4b40;
    constexpr u64 DreamsResourceCurrentGlobalOffset = 0x44d4b44;
    constexpr u64 DreamsResourceWatermarkGlobalOffset = 0x44d4b48;
    constexpr u64 DreamsResourceAuxAssignOffset = 0x71c622;
    constexpr u64 DreamsResourcePrimaryAssignOffset = 0x71c8b8;
    constexpr u64 DreamsResourceAssignOffset = 0x71cd98;
    constexpr u64 DreamsResourceCreateOffset = 0x72aa3e;
    constexpr u64 DreamsResourceRetireOffset = 0x729da5;
    constexpr u64 DreamsModelWorkerInspectOffset = 0x71bba4;
    constexpr u64 DreamsModelWorkerDequeueOffset = 0x71bd3a;
    constexpr u64 DreamsModelWorkerSignalOffset = 0x72aa94;
    const auto is_model_compute_stage = [](const u64 offset) noexcept {
        switch (offset) {
        case 0x12850c1:
        case 0x1287bd0:
        case 0x1287bf2:
        case 0x1287bf7:
        case 0x1287c60:
        case 0x1287c83:
        case 0x1287c88:
        case 0x1287d23:
        case 0x1287e1b:
        case 0x1287e20:
            return true;
        default:
            return false;
        }
    };
    const auto is_model_replay_stage = [](const u64 offset) noexcept {
        return offset == 0x12803c0 || offset == 0x1281f5e || offset == 0x1281f6e ||
               offset == 0x1281f7e;
    };
    const auto is_model_worker_stage = [](const u64 offset) noexcept {
        switch (offset) {
        case 0x71be68:
        case 0x71bed9:
        case 0x71bf1b:
        case 0x71c022:
        case 0x71c06b:
        case 0x71c148:
        case 0x71c175:
        case 0x71c17d:
        case 0x71c188:
        case 0x71c18d:
        case 0x71c192:
        case 0x71c5fe:
        case 0x71c60d:
        case 0x7204b6:
        case 0x720500:
        case 0x720513:
        case 0x7205e0:
        case 0x7205e6:
        case 0x7205eb:
        case 0x720650:
        case 0x7208e3:
        case 0x7208f8:
            return true;
        default:
            return false;
        }
    };
    const auto is_ready_write = [](const u64 offset) noexcept {
        switch (offset) {
        case 0x972397:
        case 0x9be805:
        case 0x9beb05:
        case 0x9bf0b0:
        case 0x9bfec7:
        case 0xa14334:
        case 0xa143fa:
        case 0xa1637d:
        case 0xaad456:
        case 0x9bea17:
        case 0x9c04a4:
        case 0xa03b5e:
        case 0xa13fdf:
        case 0xa7df65:
        case 0xd8c019:
        case 0x949921:
        case 0x965a98:
        case 0x96b12d:
        case 0xa45205:
        case 0xa7df55:
        case 0x8ce8e0:
        case 0x8ceb05:
        case 0x9fd521:
        case 0xa00559:
        case 0xa3f896:
        case 0xa4120e:
        case 0xa44b41:
        case 0xa44cec:
            return true;
        default:
            return false;
        }
    };
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 guest_offset = breakpoint_address - MemoryPatcher::g_eboot_address;
    if (guest_offset != DreamsRecordBuilderOffset && guest_offset != DreamsActiveMapOffset &&
        guest_offset != DreamsFirstPrepassCompleteOffset &&
        guest_offset != DreamsGroupHashLookupOffset &&
        guest_offset != DreamsSecondPrepassStartOffset &&
        guest_offset != DreamsSecondPrepassStepOffset &&
        guest_offset != DreamsObjectPrepareCallOffset &&
        guest_offset != DreamsObjectPrepareReturnOffset && guest_offset != DreamsOutputGateOffset &&
        guest_offset != DreamsObjectDispatchOffset && guest_offset != DreamsStrokeDispatchOffset &&
        guest_offset != DreamsStrokeLookupPrepareOffset &&
        guest_offset != DreamsStrokeLookupResultOffset && guest_offset != DreamsRecordGateOffset &&
        guest_offset != DreamsRecordEmitOffset && guest_offset != DreamsStrokeRecordEmitOffset &&
        guest_offset != DreamsStrokeRecordConsumeOffset &&
        guest_offset != DreamsStrokeRecordVisibleOffset &&
        guest_offset != DreamsStrokeRecordQueuedOffset &&
        guest_offset != DreamsGlobalRecordConsumeOffset &&
        guest_offset != DreamsGlobalRecordOffset &&
        guest_offset != DreamsGpuBatchBuildOffset &&
        guest_offset != DreamsGpuBatchDispatchOffset &&
        guest_offset != DreamsHighDispatchOffset && guest_offset != DreamsSceneGateTraceOffset &&
        guest_offset != DreamsRootPublishTraceOffset &&
        guest_offset != DreamsResourcePromotionOffset && !is_unresolved_consumer(guest_offset) &&
        !is_unresolved_bitset(guest_offset) &&
        guest_offset != DreamsResourceAuxAssignOffset &&
        guest_offset != DreamsResourcePrimaryAssignOffset &&
        guest_offset != DreamsResourceAssignOffset && guest_offset != DreamsResourceCreateOffset &&
        guest_offset != DreamsResourceRetireOffset &&
        guest_offset != DreamsModelWorkerInspectOffset &&
        guest_offset != DreamsModelWorkerDequeueOffset &&
        guest_offset != DreamsModelWorkerSignalOffset && !is_model_worker_stage(guest_offset) &&
        !is_model_compute_stage(guest_offset) && !is_model_replay_stage(guest_offset) &&
        !is_ready_write(guest_offset)) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    SIZE_T bytes_read = 0;

    if (is_unresolved_bitset(guest_offset)) {
        char guard_value[2]{};
        const bool guard_unresolved =
            GetEnvironmentVariableA("SHADPS4_DREAMS_GUARD_UNRESOLVED_RESOURCE", guard_value,
                                    sizeof(guard_value)) != 0 &&
            guard_value[0] == '1';
        const bool skipped = guard_unresolved && static_cast<u32>(context->Rcx) == 0xffffffffu;
        char buffer[256]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "unresolved_bitset thread=%lu resource_index=0x%08x guarded=%u skipped=%u\r\n",
            GetCurrentThreadId(), static_cast<u32>(context->Rcx), guard_unresolved ? 1 : 0,
            skipped ? 1 : 0);
        AppendDreamsCpuRootTrace(buffer, length);
        if (skipped) {
            context->Rip = MemoryPatcher::g_eboot_address + guest_offset + 0x45;
        } else {
            context->Rsi = context->Rcx;
            context->Rip = breakpoint_address + 3;
        }
        return true;
    }

    if (is_unresolved_consumer(guest_offset)) {
        u64 resource_index = 0;
        u64 skip_offset = 0;
        u64* destination = nullptr;
        switch (guest_offset) {
        case 0x76977f:
            resource_index = context->Rdx;
            destination = &context->Rcx;
            skip_offset = 0x76a1c8;
            break;
        case 0x98c8d8:
            resource_index = context->Rcx;
            destination = &context->Rax;
            skip_offset = 0x98c990;
            break;
        case 0x98d21d:
            resource_index = context->Rdx;
            destination = &context->Rcx;
            skip_offset = 0x98d2f0;
            break;
        case 0xa47644:
            resource_index = context->Rax;
            destination = &context->Rsi;
            skip_offset = 0xa47751;
            break;
        case 0xc1259c:
            resource_index = context->Rax;
            destination = &context->Rdx;
            skip_offset = 0xc126d0;
            break;
        case 0xc131aa:
            resource_index = context->Rcx;
            destination = &context->Rdx;
            skip_offset = 0xc132a0;
            break;
        case 0xbb090a:
            resource_index = context->Rcx;
            destination = &context->Rcx;
            skip_offset = 0xbb08a0;
            break;
        case 0xebb4c4:
            resource_index = context->Rdx;
            destination = &context->Rdi;
            skip_offset = 0xebb3d0;
            break;
        case 0xebbbad:
            resource_index = context->Rdx;
            destination = &context->Rdi;
            skip_offset = 0xebbac0;
            break;
        case 0xeba582:
            resource_index = context->R12;
            destination = &context->Rsi;
            skip_offset = 0xeba47e;
            break;
        case 0x1098ead:
            resource_index = context->Rdx;
            destination = &context->Rcx;
            skip_offset = 0x109999c;
            break;
        case 0x10aee07:
            resource_index = context->Rdx;
            destination = &context->Rbx;
            skip_offset = 0x10aecd6;
            break;
        case 0x13bcfc9:
            resource_index = context->Rdx;
            destination = &context->Rcx;
            skip_offset = 0x13bcf3d;
            break;
        case 0x13bd509:
            resource_index = context->Rcx;
            destination = &context->Rsi;
            skip_offset = 0x13bdb50;
            break;
        case 0x14f5b6b:
            resource_index = context->Rcx;
            destination = &context->Rsi;
            skip_offset = 0x14f5b8a;
            break;
        default:
            return false;
        }
        char guard_value[2]{};
        const bool guard_unresolved =
            GetEnvironmentVariableA("SHADPS4_DREAMS_GUARD_UNRESOLVED_RESOURCE", guard_value,
                                    sizeof(guard_value)) != 0 &&
            guard_value[0] == '1';
        const bool skipped = guard_unresolved && static_cast<u32>(resource_index) == 0xffffffffu;
        static std::atomic<u32> unresolved_consumer_trace_count{0};
        const u32 ordinal =
            unresolved_consumer_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 1024 || skipped) {
            char buffer[256]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "unresolved_consumer=%u thread=%lu offset=0x%08llx resource_index=0x%08x "
                "guarded=%u skipped=%u\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(guest_offset),
                static_cast<u32>(resource_index), guard_unresolved ? 1 : 0, skipped ? 1 : 0);
            AppendDreamsCpuRootTrace(buffer, length);
        }
        if (skipped) {
            context->Rip = MemoryPatcher::g_eboot_address + skip_offset;
        } else {
            *destination = resource_index + resource_index * 8;
            context->Rip = breakpoint_address + 4;
        }
        return true;
    }

    if (guest_offset == DreamsResourcePromotionOffset) {
        const u16 slot = static_cast<u16>(context->Rcx);
        const u64 table = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + 0x44d1730);
        const u32 resource_index =
            ReadDreamsU32(process, table + static_cast<u64>(slot) * 0x38 + 0x20);
        char guard_value[2]{};
        const bool guard_unresolved =
            GetEnvironmentVariableA("SHADPS4_DREAMS_GUARD_RESOURCE_PROMOTION", guard_value,
                                    sizeof(guard_value)) != 0 &&
            guard_value[0] == '1';
        const bool deferred = guard_unresolved && resource_index == 0xffffffffu;

        if (deferred) {
            // Match the original not-yet-promoted branch (`mov eax, ecx`).
            context->Rax = static_cast<u32>(context->Rcx);
        } else {
            const u16 retired_slot = 0xffff;
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process, reinterpret_cast<void*>(context->R14 + 0x40), &slot,
                               sizeof(slot), &bytes_written);
            WriteProcessMemory(process, reinterpret_cast<void*>(context->R14 + 0x42),
                               &retired_slot, sizeof(retired_slot), &bytes_written);
        }

        static std::atomic<u32> promotion_trace_count{0};
        const u32 ordinal = promotion_trace_count.fetch_add(1, std::memory_order_relaxed);
        const bool stamp_capture = DreamsStampTraceCaptureEnabled();
        if (stamp_capture || (!DreamsStampTraceEnabled() && (ordinal < 1024 || !deferred))) {
            char buffer[320]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "resource_promotion=%u thread=%lu object=0x%016llx slot=%u "
                "resource_index=0x%08x guarded=%u deferred=%u\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(context->R14), slot,
                resource_index, guard_unresolved ? 1 : 0, deferred ? 1 : 0);
            AppendDreamsCpuRootTrace(buffer, length);
        }
        context->Rip = MemoryPatcher::g_eboot_address + DreamsResourcePromotionResumeOffset;
        return true;
    }

    if (guest_offset == DreamsResourceAuxAssignOffset ||
        guest_offset == DreamsResourcePrimaryAssignOffset ||
        guest_offset == DreamsResourceAssignOffset || guest_offset == DreamsResourceCreateOffset ||
        guest_offset == DreamsResourceRetireOffset) {
        u64 target = 0;
        u32 value = 0;
        u64 instruction_size = 0;
        const char* kind = nullptr;
        if (guest_offset == DreamsResourceAuxAssignOffset) {
            target = context->R13 + 0x1c;
            value = static_cast<u32>(context->Rax);
            instruction_size = 4;
            kind = "assign_aux";
        } else if (guest_offset == DreamsResourcePrimaryAssignOffset) {
            target = context->R13;
            value = static_cast<u32>(context->Rax);
            instruction_size = 4;
            kind = "assign_primary";
        } else if (guest_offset == DreamsResourceAssignOffset) {
            target = context->R15 + context->R14 + 0x20;
            value = static_cast<u32>(context->Rax);
            instruction_size = 5;
            kind = "assign";
        } else if (guest_offset == DreamsResourceCreateOffset) {
            target = context->Rdi + context->Rax + 0x20;
            value = static_cast<u32>(context->Rdx);
            instruction_size = 4;
            kind = "create";
        } else {
            target = context->R10;
            value = 0xffffffffu;
            instruction_size = 7;
            kind = "retire";
        }

        const u64 table = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + 0x44d1730);
        u64 slot = ~0ull;
        if (target >= table + 0x20 && (target - table - 0x20) % 0x38 == 0) {
            slot = (target - table - 0x20) / 0x38;
        }
        const u32 resource_floor = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceFloorGlobalOffset);
        const u32 resource_current = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceCurrentGlobalOffset);
        const u32 resource_watermark = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceWatermarkGlobalOffset);
        const u32 rank = slot != ~0ull ? ReadDreamsU32(process, table + slot * 0x38 + 0xc)
                                      : 0xffffffffu;
        static std::atomic<u32> resource_write_trace_count{0};
        const u32 ordinal = resource_write_trace_count.fetch_add(1, std::memory_order_relaxed);
        const bool stamp_capture = DreamsStampTraceCaptureEnabled();
        if (stamp_capture || (!DreamsStampTraceEnabled() && (ordinal < 4096 || slot < 128))) {
            char buffer[448]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "resource_write=%u thread=%lu kind=%s offset=0x%08llx table=0x%016llx "
                "target=0x%016llx slot=%llu rank=%u resources=%u,%u,%u "
                "value=0x%08x previous=0x%08x\r\n",
                ordinal, GetCurrentThreadId(), kind,
                static_cast<unsigned long long>(guest_offset),
                static_cast<unsigned long long>(table),
                static_cast<unsigned long long>(target),
                static_cast<unsigned long long>(slot), rank, resource_floor, resource_current,
                resource_watermark, value, ReadDreamsU32(process, target));
            AppendDreamsCpuRootTrace(buffer, length);
        }

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(target), &value, sizeof(value),
                           &bytes_written);
        context->Rip = breakpoint_address + instruction_size;
        return true;
    }

    if (is_ready_write(guest_offset)) {
        u64 root = 0;
        u64 field_offset = 0;
        u8 value = 0;
        u64 instruction_size = 0;
        switch (guest_offset) {
        case 0x972397:
            root = context->Rax;
            field_offset = 0x27b0a9;
            value = 1;
            instruction_size = 7;
            break;
        case 0x9be805:
        case 0x9beb05:
        case 0x9bf0b0:
        case 0x9bfec7:
            root = context->R11;
            field_offset = 0x27b0a9;
            value = 1;
            instruction_size = 8;
            break;
        case 0xa14334:
        case 0xa143fa:
        case 0xa1637d:
            root = context->R14;
            field_offset = 0x27b0a9;
            value = 1;
            instruction_size = 8;
            break;
        case 0xaad456:
            root = context->Rax;
            field_offset = 0x27b0a9;
            value = 1;
            instruction_size = 7;
            break;
        case 0x9bea17:
        case 0x9c04a4:
            root = context->R11;
            field_offset = 0x27b0a9;
            instruction_size = 8;
            break;
        case 0xa03b5e:
        case 0xa13fdf:
            root = context->R14;
            field_offset = 0x27b0a9;
            instruction_size = 8;
            break;
        case 0xa7df65:
            root = context->R15;
            field_offset = 0x27b0a9;
            instruction_size = 8;
            break;
        case 0xd8c019:
            root = context->Rsi;
            field_offset = 0x27b0a9;
            instruction_size = 7;
            break;
        case 0x949921:
            root = context->R14;
            field_offset = 0x27b0b0;
            value = 1;
            instruction_size = 8;
            break;
        case 0x965a98:
            root = context->Rcx;
            field_offset = 0x27b0b0;
            value = 1;
            instruction_size = 7;
            break;
        case 0x96b12d:
            root = context->Rax;
            field_offset = 0x27b0b0;
            value = 1;
            instruction_size = 7;
            break;
        case 0xa45205:
            root = context->Rbx;
            field_offset = 0x27b0b0;
            value = 1;
            instruction_size = 7;
            break;
        case 0xa7df55:
            root = context->R15;
            field_offset = 0x27b0b0;
            value = 1;
            instruction_size = 8;
            break;
        case 0x8ce8e0:
        case 0x8ceb05:
            root = context->Rsi;
            field_offset = 0x27b0b0;
            instruction_size = 7;
            break;
        case 0x9fd521:
        case 0xa00559:
            root = context->Rbx;
            field_offset = 0x27b0b0;
            instruction_size = 7;
            break;
        case 0xa3f896:
            root = context->Rdx;
            field_offset = 0x27b0b0;
            instruction_size = 7;
            break;
        case 0xa4120e:
            root = context->R15;
            field_offset = 0x27b0b0;
            instruction_size = 8;
            break;
        case 0xa44b41:
        case 0xa44cec:
            root = context->Rsi;
            field_offset = 0x27b0b0;
            instruction_size = 7;
            break;
        default:
            return false;
        }

        const u8 previous = ReadDreamsU8(process, root + field_offset);
        static std::atomic<u32> ready_write_trace_count{0};
        const u32 ordinal = ready_write_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 4096) {
            char buffer[384]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "ready_write=%u thread=%lu offset=0x%08llx caller=0x%016llx "
                "root=0x%016llx/%u field=0x%llx value=%u->%u\r\n",
                ordinal, GetCurrentThreadId(),
                static_cast<unsigned long long>(guest_offset),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp)),
                static_cast<unsigned long long>(root), ReadDreamsRootCount(process, root),
                static_cast<unsigned long long>(field_offset), previous, value);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(root + field_offset), &value,
                           sizeof(value), &bytes_written);
        context->Rip = breakpoint_address + instruction_size;
        return true;
    }

    if (guest_offset == DreamsRecordBuilderOffset) {
        const u64 root = ReadDreamsU64(process, context->Rdi + 0x10);
        const u32 root_count = ReadDreamsRootCount(process, root);
        const u64 output = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + DreamsOutputContextGlobalOffset);
        const u8 guard = ReadDreamsU8(
            process, MemoryPatcher::g_eboot_address + DreamsRecordEnableGlobalOffset);
        const u64 caller = ReadDreamsU64(process, context->Rsp);
        const u8 ready_a_before = ReadDreamsU8(process, root + 0x27b0a9);
        const u8 ready_b_before = ReadDreamsU8(process, root + 0x27b0b0);
        u8 ready_a_after = ready_a_before;
        u8 ready_b_after = ready_b_before;
        char force_value[2]{};
        const bool force_stroke_active =
            GetEnvironmentVariableA("SHADPS4_DREAMS_FORCE_STROKE_ACTIVE", force_value,
                                    sizeof(force_value)) != 0 &&
            force_value[0] == '1';
        if (force_stroke_active && root_count >= 3 && root_count <= 32) {
            ready_a_after = 1;
            ready_b_after = 1;
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process, reinterpret_cast<void*>(root + 0x27b0a9), &ready_a_after,
                               sizeof(ready_a_after), &bytes_written);
            WriteProcessMemory(process, reinterpret_cast<void*>(root + 0x27b0b0), &ready_b_after,
                               sizeof(ready_b_after), &bytes_written);
        }

        static std::atomic<u32> builder_trace_count{0};
        const u32 ordinal = builder_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 512) {
            char buffer[448]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "builder=%u thread=%lu caller=0x%016llx context=0x%016llx root=0x%016llx/%u "
                "output=0x%016llx guard=%u ready=%u,%u->%u,%u forced=%u records=%u\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(caller),
                static_cast<unsigned long long>(context->Rdi),
                static_cast<unsigned long long>(root), root_count,
                static_cast<unsigned long long>(output), guard, ready_a_before, ready_b_before,
                ready_a_after, ready_b_after, force_stroke_active ? 1 : 0,
                output != 0 ? ReadDreamsU32(process, output + 0x31ff38) : 0);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        context->Rsp -= sizeof(u64);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 1;
        return true;
    }

    if (guest_offset == DreamsActiveMapOffset) {
        const u64 root = ReadDreamsU64(process, context->Rsp + 0x28);
        const u32 root_count = ReadDreamsRootCount(process, root);
        const u64 active_map = context->Rsp + 0x20248;
        char force_value[2]{};
        const bool force_stroke_active =
            GetEnvironmentVariableA("SHADPS4_DREAMS_FORCE_STROKE_ACTIVE", force_value,
                                    sizeof(force_value)) != 0 &&
            force_value[0] == '1';

        if (root_count <= 32) {
            static std::atomic<u32> active_map_trace_count{0};
            for (u32 index = 0; index < root_count; ++index) {
                const u8 type = ReadDreamsU8(process, root + 0x1acf558 + index);
                if ((type & 0x7f) != 0x11) {
                    continue;
                }

                const u8 before = ReadDreamsU8(process, active_map + index);
                u8 after = before;
                if (force_stroke_active && before == 0) {
                    after = 1;
                    SIZE_T bytes_written = 0;
                    WriteProcessMemory(process, reinterpret_cast<void*>(active_map + index), &after,
                                       sizeof(after), &bytes_written);
                }

                const u32 ordinal =
                    active_map_trace_count.fetch_add(1, std::memory_order_relaxed);
                if (ordinal < 2048) {
                    const u64 object = ReadDreamsU64(process, root + 0x10958c0 + index * 8);
                    char buffer[384]{};
                    const int length = _snprintf_s(
                        buffer, sizeof(buffer), _TRUNCATE,
                        "active_map=%u thread=%lu root=0x%016llx/%u index=%u "
                        "object=0x%016llx header=0x%08x active=%u->%u forced=%u\r\n",
                        ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(root),
                        root_count, index, static_cast<unsigned long long>(object),
                        ReadDreamsU32(process, object + 0xc), before, after,
                        force_stroke_active ? 1 : 0);
                    AppendDreamsCpuRootTrace(buffer, length);
                }
            }
        }

        context->Rax = ReadDreamsU64(process, context->Rsp + 0xe8);
        context->Rip = breakpoint_address + 8;
        return true;
    }

    if (guest_offset == DreamsFirstPrepassCompleteOffset) {
        const s32 count = static_cast<s32>(ReadDreamsU32(process, context->Rsp + 0x40));
        char buffer[192]{};
        const int length = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                       "builder_phase=first_complete thread=%lu count=%d\r\n",
                                       GetCurrentThreadId(), count);
        AppendDreamsCpuRootTrace(buffer, length);
        context->Rip = MemoryPatcher::g_eboot_address +
                       (count <= 0 ? 0x8b875b : 0x8b852a);
        return true;
    }

    if (guest_offset == DreamsGroupHashLookupOffset) {
        const u32 target = static_cast<u32>(context->Rdx);
        const u64 mask = context->Rsi;
        const u64 start = context->Rcx & mask;
        const u64 scan_count = mask < 0x100000 ? mask + 1 : 0x100000;
        u64 matched_slot = 0;
        u64 scanned = 0;
        bool matched = false;
        for (; scanned < scan_count; ++scanned) {
            const u64 slot = (start + scanned) & mask;
            if (ReadDreamsU32(process, context->R9 + slot * 0x20) == target) {
                matched = true;
                matched_slot = slot;
                break;
            }
        }

        char guard_value[2]{};
        const bool skip_unresolved =
            GetEnvironmentVariableA("SHADPS4_DREAMS_SKIP_UNRESOLVED_BUILDER_RESOURCE", guard_value,
                                    sizeof(guard_value)) != 0 &&
            guard_value[0] == '1';
        char buffer[320]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_hash thread=%lu target=0x%08x table=0x%016llx mask=0x%llx start=0x%llx "
            "matched=%u slot=0x%llx scanned=%llu synthesized=%u\r\n",
            GetCurrentThreadId(), target, static_cast<unsigned long long>(context->R9),
            static_cast<unsigned long long>(mask), static_cast<unsigned long long>(start),
            matched ? 1 : 0, static_cast<unsigned long long>(matched_slot),
            static_cast<unsigned long long>(scanned + (matched ? 1 : 0)),
            !matched && skip_unresolved ? 1 : 0);
        AppendDreamsCpuRootTrace(buffer, length);

        if (matched) {
            context->Rcx = matched_slot;
            context->Rip = MemoryPatcher::g_eboot_address + 0x8b8540;
        } else if (skip_unresolved) {
            const u64 zero = 0;
            const u32 group_index = static_cast<u32>(context->R10);
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process,
                               reinterpret_cast<void*>(context->R13 + context->R15 + 0x1cc040),
                               &zero, sizeof(zero), &bytes_written);
            WriteProcessMemory(process,
                               reinterpret_cast<void*>(context->Rsp + context->R11 + 0x2425c),
                               &group_index, sizeof(group_index), &bytes_written);
            context->Rbx = static_cast<u32>(context->R10) + 1;
            context->Rip = MemoryPatcher::g_eboot_address + 0x8b8563;
        } else {
            context->Rip = MemoryPatcher::g_eboot_address + 0x8b8739;
        }
        return true;
    }

    if (guest_offset == DreamsSecondPrepassStartOffset) {
        const u64 root = ReadDreamsU64(process, context->Rsp + 0xa0);
        char buffer[224]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=second_start thread=%lu root=0x%016llx/%u\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(root),
            ReadDreamsRootCount(process, root));
        AppendDreamsCpuRootTrace(buffer, length);
        context->R12 = 1;
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsSecondPrepassStepOffset) {
        const u64 root = ReadDreamsU64(process, context->Rsp + 0xa0);
        char buffer[256]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=second_step thread=%lu root=0x%016llx/%u index=%llu\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(root),
            ReadDreamsRootCount(process, root), static_cast<unsigned long long>(context->R12));
        AppendDreamsCpuRootTrace(buffer, length);
        ++context->R12;
        context->Rip = breakpoint_address + 3;
        return true;
    }

    if (guest_offset == DreamsObjectPrepareCallOffset) {
        const u64 root = ReadDreamsU64(process, context->Rsp + 0xa0);
        const u8 type = ReadDreamsU8(process, context->Rsp + 0xc0);
        char buffer[288]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=prepare_call thread=%lu root=0x%016llx/%u index=%llu type=0x%02x "
            "buffer=0x%016llx\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(root),
            ReadDreamsRootCount(process, root), static_cast<unsigned long long>(context->R12), type,
            static_cast<unsigned long long>(context->R14));
        AppendDreamsCpuRootTrace(buffer, length);
        context->Rdx = context->R14;
        context->Rip = breakpoint_address + 3;
        return true;
    }

    if (guest_offset == DreamsObjectPrepareReturnOffset) {
        const u64 root = ReadDreamsU64(process, context->Rsp + 0xa0);
        const u8 type = ReadDreamsU8(process, context->Rsp + 0xc0);
        char buffer[256]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=prepare_return thread=%lu root=0x%016llx/%u index=%llu type=0x%02x\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(root),
            ReadDreamsRootCount(process, root), static_cast<unsigned long long>(context->R12), type);
        AppendDreamsCpuRootTrace(buffer, length);
        context->Rdi = root;
        context->Rip = MemoryPatcher::g_eboot_address + (type == 1 ? 0x8b8a47 : 0x8b8870);
        return true;
    }

    if (guest_offset == DreamsOutputGateOffset) {
        const u64 output = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + DreamsOutputContextGlobalOffset);
        char buffer[192]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=output_gate thread=%lu output=0x%016llx\r\n", GetCurrentThreadId(),
            static_cast<unsigned long long>(output));
        AppendDreamsCpuRootTrace(buffer, length);
        context->Rip = MemoryPatcher::g_eboot_address + (output == 0 ? 0x8bb997 : 0x8b8a75);
        return true;
    }

    if (guest_offset == DreamsObjectDispatchOffset) {
        const u64 root = context->R10;
        const u32 index = static_cast<u32>(context->R14);
        const u8 active = ReadDreamsU8(process, context->Rsp + 0x20248 + index);
        const u8 type = ReadDreamsU8(process, root + 0x1acf558 + index);
        char buffer[256]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=dispatch thread=%lu root=0x%016llx/%u index=%u active=%u type=0x%02x\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(root),
            ReadDreamsRootCount(process, root), index, active, type);
        AppendDreamsCpuRootTrace(buffer, length);
        if (active == 0) {
            context->Rip = MemoryPatcher::g_eboot_address + 0x8b8c40;
        } else {
            context->Rax = (context->Rax & ~0xffull) | type;
            context->Rip = MemoryPatcher::g_eboot_address + 0x8b8c66;
        }
        return true;
    }

    if (guest_offset == DreamsStrokeDispatchOffset) {
        const u64 object = ReadDreamsU64(process, context->R10 + 0x10958c0 + context->R14 * 8);
        char buffer[256]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=stroke_dispatch thread=%lu index=%llu object=0x%016llx header=0x%08x\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(context->R14),
            static_cast<unsigned long long>(object), ReadDreamsU32(process, object + 0xc));
        AppendDreamsCpuRootTrace(buffer, length);
        context->R13 = object;
        context->Rip = breakpoint_address + 8;
        return true;
    }

    if (guest_offset == DreamsStrokeLookupPrepareOffset) {
        const u64 output = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + DreamsOutputContextGlobalOffset);
        char buffer[224]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "builder_phase=stroke_lookup_prepare thread=%lu object=0x%016llx output=0x%016llx\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(context->R13),
            static_cast<unsigned long long>(output));
        AppendDreamsCpuRootTrace(buffer, length);
        context->Rax = output;
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (guest_offset == DreamsStrokeLookupResultOffset) {
        const u64 root = ReadDreamsU64(process, context->Rsp + 0x28);
        const u32 root_count = ReadDreamsRootCount(process, root);
        static std::atomic<u32> stroke_lookup_trace_count{0};
        const u32 ordinal = stroke_lookup_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (root_count <= 32 && ordinal < 2048) {
            char buffer[384]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "stroke_lookup=%u thread=%lu root=0x%016llx/%u object=0x%016llx "
                "result=0x%08x c0=0x%08x c4=0x%08x mode=%u\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(root), root_count,
                static_cast<unsigned long long>(context->R13), static_cast<u32>(context->Rax),
                ReadDreamsU32(process, context->R13 + 0xc0),
                ReadDreamsU32(process, context->R13 + 0xc4),
                ReadDreamsU8(process, context->R13 + 0xcd));
            AppendDreamsCpuRootTrace(buffer, length);
        }

        context->R10 = root;
        context->Rip = breakpoint_address + 5;
        return true;
    }

    if (guest_offset == DreamsRecordGateOffset) {
        const u64 output = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + DreamsOutputContextGlobalOffset);
        const u8 guard = ReadDreamsU8(
            process, MemoryPatcher::g_eboot_address + DreamsRecordEnableGlobalOffset);
        const u32 object_header = ReadDreamsU32(process, context->Rbx + 0xc);

        static std::atomic<u32> gate_trace_count{0};
        const u32 ordinal = gate_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 1024) {
            char buffer[384]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "record_gate=%u thread=%lu caller=0x%016llx object=0x%016llx "
                "header=0x%08x guard=%u output=0x%016llx records=%u\r\n",
                ordinal, GetCurrentThreadId(),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rbp + 8)),
                static_cast<unsigned long long>(context->Rbx), object_header, guard,
                static_cast<unsigned long long>(output),
                output != 0 ? ReadDreamsU32(process, output + 0x31ff38) : 0);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        context->Rax = (context->Rax & ~0xffull) | guard;
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsRecordEmitOffset) {
        const u64 count_address = context->R15 + 0x31ff38;
        const u32 previous = ReadDreamsU32(process, count_address);
        const u32 result = previous + 1;

        static std::atomic<u32> emit_trace_count{0};
        const u32 ordinal = emit_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 1024) {
            char buffer[320]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "record_emit=%u thread=%lu caller=0x%016llx output=0x%016llx "
                "object=0x%016llx before=%u after=%u\r\n",
                ordinal, GetCurrentThreadId(),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rbp + 8)),
                static_cast<unsigned long long>(context->R15),
                static_cast<unsigned long long>(context->Rbx), previous, result);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(count_address), &result, sizeof(result),
                           &bytes_written);
        ApplyDreamsIncFlags(context, previous, result);
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (guest_offset == DreamsStrokeRecordEmitOffset) {
        const u64 count_address = context->Rbx + 0xb9fd20;
        const u32 previous = ReadDreamsU32(process, count_address);
        const u32 result = previous + 1;

        static std::atomic<u32> stroke_emit_trace_count{0};
        const u32 ordinal = stroke_emit_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 2048) {
            char buffer[320]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "stroke_emit=%u thread=%lu output=0x%016llx object=0x%016llx "
                "before=%u after=%u\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(context->Rbx),
                static_cast<unsigned long long>(context->R13), previous, result);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(count_address), &result, sizeof(result),
                           &bytes_written);
        ApplyDreamsIncFlags(context, previous, result);
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsStrokeRecordConsumeOffset) {
        const u32 records = ReadDreamsU32(process, context->R12 + 0xb9fd20);
        static std::atomic<u32> stroke_consume_trace_count{0};
        const u32 ordinal = stroke_consume_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 2048) {
            char buffer[256]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "stroke_consume=%u thread=%lu output=0x%016llx records=%u\r\n", ordinal,
                GetCurrentThreadId(), static_cast<unsigned long long>(context->R12), records);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        context->Rbx = context->R12 + 0xdc98d8;
        context->Rip = breakpoint_address + 8;
        return true;
    }

    if (guest_offset == DreamsStrokeRecordVisibleOffset) {
        const u32 queued = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsGlobalRecordCountOffset);
        static std::atomic<u32> stroke_visible_trace_count{0};
        const u32 ordinal = stroke_visible_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 2048) {
            char buffer[384]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "stroke_visible=%u thread=%lu base=0x%016llx output=0x%016llx "
                "record=0x%016llx index=%llu queued_before=%u resource=0x%08x flags=0x%08x\r\n",
                ordinal, GetCurrentThreadId(),
                static_cast<unsigned long long>(MemoryPatcher::g_eboot_address),
                static_cast<unsigned long long>(context->R12),
                static_cast<unsigned long long>(context->R15),
                static_cast<unsigned long long>(context->R13), queued,
                ReadDreamsU32(process, context->R15 + 0x11c),
                ReadDreamsU32(process, context->R15));
            AppendDreamsCpuRootTrace(buffer, length);
        }
        context->Rip = breakpoint_address + 11;
        return true;
    }

    if (guest_offset == DreamsStrokeRecordQueuedOffset) {
        const u32 queued = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsGlobalRecordCountOffset);
        static std::atomic<u32> stroke_queued_trace_count{0};
        const u32 ordinal = stroke_queued_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 2048) {
            char buffer[224]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "stroke_queued=%u thread=%lu queued_after=%u\r\n", ordinal,
                GetCurrentThreadId(), queued);
            AppendDreamsCpuRootTrace(buffer, length);
        }
        context->Rip = MemoryPatcher::g_eboot_address + 0xeb8e16;
        return true;
    }

    if (guest_offset == DreamsGlobalRecordConsumeOffset) {
        const u32 queued = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsGlobalRecordCountOffset);
        static std::atomic<u32> global_consume_trace_count{0};
        const u32 ordinal = global_consume_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 2048) {
            char buffer[224]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "global_consume=%u thread=%lu queued=%u\r\n", ordinal,
                GetCurrentThreadId(), queued);
            AppendDreamsCpuRootTrace(buffer, length);
        }
        context->Rax = queued;
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsGlobalRecordOffset) {
        const u32 resource = ReadDreamsU32(process, context->R15 + 0x11c);
        static std::atomic<u32> global_record_trace_count{0};
        const u32 ordinal = global_record_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 2048) {
            char buffer[288]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "global_record=%u thread=%lu record=0x%016llx resource=0x%08x "
                "flags=0x%08x object=0x%08x\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(context->R15),
                resource, ReadDreamsU32(process, context->R15),
                ReadDreamsU32(process, context->R15 + 4));
            AppendDreamsCpuRootTrace(buffer, length);
        }
        context->Rax = resource;
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (guest_offset == DreamsGpuBatchBuildOffset) {
        const u64 image_base = MemoryPatcher::g_eboot_address;
        const u32 count = ReadDreamsU32(process, image_base + DreamsGpuStagingCountGlobalOffset);
        const u32 combined =
            ReadDreamsU32(process, image_base + DreamsGpuCombinedCountGlobalOffset);
        const u64 staging =
            ReadDreamsU64(process, image_base + DreamsGpuStagingPointerGlobalOffset);
        static std::atomic<u32> gpu_batch_build_trace_count{0};
        if (count != 0) {
            const u32 ordinal =
                gpu_batch_build_trace_count.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 256) {
                char buffer[320]{};
                const int length = _snprintf_s(
                    buffer, sizeof(buffer), _TRUNCATE,
                    "gpu_batch_build=%u thread=%lu count=%u combined=%u staging=0x%016llx "
                    "constants=0x%016llx\r\n",
                    ordinal, GetCurrentThreadId(), count, combined,
                    static_cast<unsigned long long>(staging),
                    static_cast<unsigned long long>(context->R14));
                AppendDreamsCpuRootTrace(buffer, length);
            }
        }
        context->Rax = count;
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsGpuBatchDispatchOffset) {
        const u64 image_base = MemoryPatcher::g_eboot_address;
        const u32 count = ReadDreamsU32(process, image_base + DreamsGpuStagingCountGlobalOffset);
        const u32 combined =
            ReadDreamsU32(process, image_base + DreamsGpuCombinedCountGlobalOffset);
        const u64 staging =
            ReadDreamsU64(process, image_base + DreamsGpuStagingPointerGlobalOffset);
        static std::atomic<u32> gpu_batch_dispatch_trace_count{0};
        if (count != 0) {
            const u32 ordinal =
                gpu_batch_dispatch_trace_count.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 256) {
                char buffer[512]{};
                const int length = _snprintf_s(
                    buffer, sizeof(buffer), _TRUNCATE,
                    "gpu_batch_dispatch=%u thread=%lu count=%u groups=%u combined=%u "
                    "staging=0x%016llx constants=0x%016llx descriptor=0x%016llx "
                    "resource=0x%016llx constants_count=%u\r\n",
                    ordinal, GetCurrentThreadId(), count, (count + 63) >> 6, combined,
                    static_cast<unsigned long long>(staging),
                    static_cast<unsigned long long>(context->R14),
                    static_cast<unsigned long long>(context->Rax),
                    static_cast<unsigned long long>(ReadDreamsU64(process, context->R14 + 0x38)),
                    ReadDreamsU32(process, context->R14 + 0x84));
                AppendDreamsCpuRootTrace(buffer, length);
            }
        }
        context->Rcx = count;
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsModelWorkerInspectOffset ||
        guest_offset == DreamsModelWorkerDequeueOffset ||
        guest_offset == DreamsModelWorkerSignalOffset) {
        const u64 table = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + 0x44d1730);
        const u32 queue_tail = ReadDreamsU32(process, table + 0x38030) & 0xffff;
        const u32 queue_head = ReadDreamsU32(process, table + 0x38032) & 0xffff;
        const u32 worker_state = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + 0x44dcc38);
        const u32 resource_floor = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceFloorGlobalOffset);
        const u32 resource_current = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceCurrentGlobalOffset);
        const u32 resource_watermark = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceWatermarkGlobalOffset);
        const char* kind = guest_offset == DreamsModelWorkerInspectOffset
                               ? "inspect"
                               : guest_offset == DreamsModelWorkerDequeueOffset ? "dequeue"
                                                                                : "signal";
        static std::atomic<u32> worker_trace_count{0};
        const u32 ordinal = worker_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (DreamsStampTraceCaptureEnabled() || ordinal < 512) {
            char buffer[384]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_worker=%u thread=%lu kind=%s state=%u table=0x%016llx "
                "queue=%u,%u resources=%u,%u,%u\r\n",
                ordinal, GetCurrentThreadId(), kind, worker_state,
                static_cast<unsigned long long>(table), queue_head, queue_tail, resource_floor,
                resource_current, resource_watermark);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        if (guest_offset == DreamsModelWorkerInspectOffset) {
            // Emulate `mov rax, qword ptr [resource_table]`.
            context->Rax = table;
        } else if (guest_offset == DreamsModelWorkerDequeueOffset) {
            // Emulate `mov r9, qword ptr [resource_table]`.
            context->R9 = table;
        } else {
            // Emulate `lea rdi, [worker_condition]`.
            context->Rdi = MemoryPatcher::g_eboot_address + 0x44dcc30;
        }
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (is_model_compute_stage(guest_offset)) {
        const auto emulate_call = [&](const u64 target_offset) noexcept {
            const u64 return_address = breakpoint_address + 5;
            context->Rsp -= sizeof(return_address);
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &return_address,
                               sizeof(return_address), &bytes_written);
            context->Rip = MemoryPatcher::g_eboot_address + target_offset;
        };
        const auto trace_pipe = [&](const char* kind, const u64 pipe, const u64 wait_result,
                                    const u32 out_count) noexcept {
            const u64 completion_pointer = pipe != 0 ? ReadDreamsU64(process, pipe + 0xb8) : 0;
            const u64 completion =
                completion_pointer != 0 ? ReadDreamsU64(process, completion_pointer) : 0;
            const u32 target = pipe != 0 ? ReadDreamsU32(process, pipe + 0xd8) : 0;
            const u64 equeue = pipe != 0 ? ReadDreamsU64(process, pipe + 0x128) : 0;
            const u64 command_begin = pipe != 0 ? ReadDreamsU64(process, pipe + 0xe0) : 0;
            const u64 command_cursor = pipe != 0 ? ReadDreamsU64(process, pipe + 0xe8) : 0;
            const u64 command_end = pipe != 0 ? ReadDreamsU64(process, pipe + 0xf0) : 0;
            static std::atomic<u32> compute_trace_count{0};
            const u32 ordinal = compute_trace_count.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 256 || DreamsStampTraceCaptureEnabled()) {
                char buffer[512]{};
                const int length = _snprintf_s(
                    buffer, sizeof(buffer), _TRUNCATE,
                    "model_compute=%u thread=%lu kind=%s offset=0x%08llx pipe=0x%016llx "
                    "completion_ptr=0x%016llx completion=%llu target=%u equeue=%llu "
                    "commands=0x%016llx,0x%016llx,0x%016llx wait=0x%016llx out=%u "
                    "rsp=0x%016llx\r\n",
                    ordinal, GetCurrentThreadId(), kind,
                    static_cast<unsigned long long>(guest_offset),
                    static_cast<unsigned long long>(pipe),
                    static_cast<unsigned long long>(completion_pointer),
                    static_cast<unsigned long long>(completion), target,
                    static_cast<unsigned long long>(equeue),
                    static_cast<unsigned long long>(command_begin),
                    static_cast<unsigned long long>(command_cursor),
                    static_cast<unsigned long long>(command_end),
                    static_cast<unsigned long long>(wait_result), out_count,
                    static_cast<unsigned long long>(context->Rsp));
                AppendDreamsCpuRootTrace(buffer, length);
            }
        };

        switch (guest_offset) {
        case 0x12850c1:
            trace_pipe("compute_call", 0, 0, 0);
            emulate_call(0x1280290);
            break;
        case 0x1287bd0: {
            trace_pipe("wait_entry", context->Rdi, 0, 0);
            const u64 saved_rbp = context->Rbp;
            context->Rsp -= sizeof(saved_rbp);
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &saved_rbp,
                               sizeof(saved_rbp), &bytes_written);
            context->Rip = breakpoint_address + 1;
            break;
        }
        case 0x1287bf2:
            trace_pipe("submit_call", context->Rbx, 0, 0);
            emulate_call(0x1287cf0);
            break;
        case 0x1287bf7:
            trace_pipe("submit_return", context->Rbx, context->Rax, 0);
            context->Rcx = ReadDreamsU32(process, context->Rbx + 0x120);
            context->Rip = breakpoint_address + 6;
            break;
        case 0x1287c60: {
            trace_pipe("wait_loop", context->Rbx, 0, 0);
            const u32 zero = 0;
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process, reinterpret_cast<void*>(context->Rbp - 0x54), &zero,
                               sizeof(zero), &bytes_written);
            context->Rip = breakpoint_address + 7;
            break;
        }
        case 0x1287c83:
            trace_pipe("equeue_wait", context->Rbx, 0,
                       ReadDreamsU32(process, context->Rbp - 0x54));
            emulate_call(0x15f9b20);
            break;
        case 0x1287c88: {
            const u32 out_count = ReadDreamsU32(process, context->Rbp - 0x54);
            trace_pipe("equeue_return", context->Rbx, context->Rax, out_count);
            context->Rax = ReadDreamsU64(process, context->R13);
            context->Rip = breakpoint_address + 4;
            break;
        }
        case 0x1287d23: {
            trace_pipe("target_publish", context->Rdi, 0, 0);
            const u32 target = static_cast<u32>(context->R13);
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process, reinterpret_cast<void*>(context->Rdi + 0xd8), &target,
                               sizeof(target), &bytes_written);
            context->Rip = breakpoint_address + 7;
            break;
        }
        case 0x1287e1b:
            trace_pipe("packet_emit", context->R15 - 0xe0, 0, 0);
            emulate_call(0x2b9a0);
            break;
        case 0x1287e20:
            trace_pipe("packet_emitted", context->R15 - 0xe0, context->Rax, 0);
            context->Rsp += 0x18;
            context->Rip = breakpoint_address + 4;
            break;
        default:
            return false;
        }
        return true;
    }

    if (is_model_replay_stage(guest_offset)) {
        const u64 result = context->R12;
        const u64 result_source = ReadDreamsU64(process, context->Rbp - 0xf8);
        const u64 replay_span = ReadDreamsU64(process, context->Rbp - 0xe0);
        const u64 replay_groups = ReadDreamsU64(process, context->Rbp - 0x180);
        static std::atomic<u32> replay_trace_count{0};
        const u32 ordinal = replay_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 256 || DreamsStampTraceCaptureEnabled()) {
            char buffer[512]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_replay=%u thread=%lu offset=0x%08llx result=0x%016llx "
                "values=%u,%u,%u,%u progress=%u expected=%u source=0x%016llx span=%llu groups=%llu "
                "rsi=%llu rbx=%llu rsp=0x%016llx\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(guest_offset),
                static_cast<unsigned long long>(result), ReadDreamsU32(process, result),
                ReadDreamsU32(process, result + 4), ReadDreamsU32(process, result + 8),
                ReadDreamsU32(process, result + 12), ReadDreamsU32(process, result + 0x1c),
                ReadDreamsU32(process, context->Rbp - 0x160),
                static_cast<unsigned long long>(result_source),
                static_cast<unsigned long long>(replay_span),
                static_cast<unsigned long long>(replay_groups),
                static_cast<unsigned long long>(context->Rsi),
                static_cast<unsigned long long>(context->Rbx),
                static_cast<unsigned long long>(context->Rsp));
            AppendDreamsCpuRootTrace(buffer, length);
        }

        switch (guest_offset) {
        case 0x12803c0:
            context->Rax = ReadDreamsU64(
                process, MemoryPatcher::g_eboot_address + 0x782ed40);
            context->Rip = breakpoint_address + 7;
            break;
        case 0x1281f5e:
            context->Rdx = result_source;
            context->Rip = breakpoint_address + 7;
            break;
        case 0x1281f6e:
            context->Rax = ReadDreamsU32(process, result + 8);
            context->Rip = breakpoint_address + 5;
            break;
        case 0x1281f7e:
            context->Rip = MemoryPatcher::g_eboot_address + 0x12803c0;
            break;
        default:
            return false;
        }
        return true;
    }

    if (is_model_worker_stage(guest_offset)) {
        const u64 table = ReadDreamsU64(
            process, MemoryPatcher::g_eboot_address + 0x44d1730);
        const u32 active_slot = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + 0x44dcc3c);
        const u32 resource_floor = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceFloorGlobalOffset);
        const u32 resource_current = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceCurrentGlobalOffset);
        const u32 resource_watermark = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceWatermarkGlobalOffset);
        static std::atomic<u32> model_stage_trace_count{0};
        const u32 ordinal = model_stage_trace_count.fetch_add(1, std::memory_order_relaxed);
        const bool allocator_stage = guest_offset >= 0x7204b6 && guest_offset <= 0x7208f8;
        const u64 allocator = allocator_stage ? context->R11 : 0;
        const u64 allocation_bits = allocator != 0 ? ReadDreamsU64(process, allocator) : 0;
        const u64 allocation_bits_end =
            allocator != 0 ? ReadDreamsU64(process, allocator + 0x8) : 0;
        const u32 allocation_limit =
            allocator != 0 ? ReadDreamsU32(process, allocator + 0x18) : 0;
        const u32 allocation_count =
            allocator != 0 ? ReadDreamsU32(process, allocator + 0x2c) : 0;
        const u64 allocation_bits0 =
            allocation_bits != 0 ? ReadDreamsU64(process, allocation_bits) : 0;
        const u64 allocation_bits1 = allocation_bits != 0 &&
                                             allocation_bits + sizeof(u64) < allocation_bits_end
                                         ? ReadDreamsU64(process, allocation_bits + sizeof(u64))
                                         : 0;
        const u32 model_generation = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + 0x44dcc44);
        if (ordinal < 512 || DreamsStampTraceCaptureEnabled()) {
            char buffer[640]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_stage=%u thread=%lu offset=0x%08llx slot=%u table=0x%016llx "
                "resources=%u,%u,%u model_generation=%u eboot=0x%016llx "
                "allocator=0x%016llx bits=0x%016llx..0x%016llx bit0=0x%016llx "
                "bit1=0x%016llx limit=%u count=%u r9=%u r12=%u r14=%u "
                "rsp=0x%016llx\r\n",
                ordinal, GetCurrentThreadId(),
                static_cast<unsigned long long>(guest_offset), active_slot,
                static_cast<unsigned long long>(table), resource_floor, resource_current,
                resource_watermark, model_generation,
                static_cast<unsigned long long>(MemoryPatcher::g_eboot_address),
                static_cast<unsigned long long>(allocator),
                static_cast<unsigned long long>(allocation_bits),
                static_cast<unsigned long long>(allocation_bits_end),
                static_cast<unsigned long long>(allocation_bits0),
                static_cast<unsigned long long>(allocation_bits1), allocation_limit,
                allocation_count, static_cast<u32>(context->R9),
                static_cast<u32>(context->R12), static_cast<u32>(context->R14),
                static_cast<unsigned long long>(context->Rsp));
            AppendDreamsCpuRootTrace(buffer, length);
        }

        const auto emulate_call = [&](const u64 target_offset) noexcept {
            const u64 return_address = breakpoint_address + 5;
            context->Rsp -= sizeof(return_address);
            SIZE_T bytes_written = 0;
            WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &return_address,
                               sizeof(return_address), &bytes_written);
            context->Rip = MemoryPatcher::g_eboot_address + target_offset;
        };

        switch (guest_offset) {
        case 0x71be68:
            context->Rax = ReadDreamsU64(process, context->Rsp + 0x70);
            context->Rip = breakpoint_address + 5;
            break;
        case 0x71bed9:
        case 0x71c022:
            emulate_call(0xc5a060);
            break;
        case 0x71bf1b:
            context->Rax = ReadDreamsU8(process, context->Rsp + 0x20a);
            context->Rip = breakpoint_address + 8;
            break;
        case 0x71c06b:
            context->Rax = (context->Rax & ~0xffull) |
                           ReadDreamsU8(process, context->Rsp + 0x209);
            context->Rip = breakpoint_address + 7;
            break;
        case 0x71c148:
            context->Rax = ReadDreamsU64(process, context->Rsp + 0x88);
            context->Rip = breakpoint_address + 8;
            break;
        case 0x71c175:
            emulate_call(0xc5b330);
            break;
        case 0x71c17d:
            context->Rbx = ReadDreamsU64(process, context->Rsp + 0xa0);
            context->Rip = breakpoint_address + 8;
            break;
        case 0x71c188:
            emulate_call(0x15f7fb0);
            break;
        case 0x71c18d:
            emulate_call(0x15f7e10);
            break;
        case 0x71c192:
            context->Rcx = ReadDreamsU64(process, context->Rsp + 0x70);
            context->Rip = breakpoint_address + 5;
            break;
        case 0x71c5fe:
            context->Rdi = ReadDreamsU64(
                process, MemoryPatcher::g_eboot_address + 0x44d4a90);
            context->Rip = breakpoint_address + 7;
            break;
        case 0x71c60d:
            emulate_call(0x720440);
            break;
        case 0x7204b6:
            context->Rsi = ReadDreamsU64(process, context->R11);
            context->Rip = breakpoint_address + 3;
            break;
        case 0x720500:
            context->Rbx = ~context->Rbx;
            context->Rip = breakpoint_address + 3;
            break;
        case 0x720513:
        case 0x7205eb:
            context->Rax = model_generation;
            context->Rip = breakpoint_address + 6;
            break;
        case 0x7205e0:
            context->Rdi = context->Rbx;
            context->Rip = breakpoint_address + 3;
            break;
        case 0x7205e6:
            emulate_call(0x15f7e60);
            break;
        case 0x720650:
            context->Rsi = ReadDreamsU16(
                process, context->R15 + context->Rbx * sizeof(u64) + 0x14);
            context->Rip = breakpoint_address + 6;
            break;
        case 0x7208e3:
            context->Rdi = MemoryPatcher::g_eboot_address + 0x16e508a;
            context->Rip = breakpoint_address + 7;
            break;
        case 0x7208f8:
            context->Rdx = 1;
            context->Rip = breakpoint_address + 5;
            break;
        default:
            return false;
        }
        return true;
    }

    if (guest_offset == DreamsHighDispatchOffset) {
        u64 root = 0;
        u64 render_state = 0;
        ReadProcessMemory(process, reinterpret_cast<const void*>(context->Rdi), &root, sizeof(root),
                          &bytes_read);
        ReadProcessMemory(process, reinterpret_cast<const void*>(context->Rdi + 0x38),
                          &render_state, sizeof(render_state), &bytes_read);

        static std::atomic<u32> high_trace_count{0};
        const u32 ordinal = high_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 512) {
            char buffer[320]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "high=%u thread=%lu context=0x%016llx root=0x%016llx count=%u "
                "state=0x%016llx rsp=0x%016llx\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(context->Rdi),
                static_cast<unsigned long long>(root), ReadDreamsRootCount(process, root),
                static_cast<unsigned long long>(render_state),
                static_cast<unsigned long long>(context->Rsp));
            AppendDreamsCpuRootTrace(buffer, length);
        }

        // The trace replaces the one-byte `push rbp` at the function entry with INT3.
        context->Rsp -= sizeof(u64);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 1;
        return true;
    }

    if (guest_offset == DreamsSceneGateTraceOffset) {
        u64 active_root = 0;
        ReadProcessMemory(process, reinterpret_cast<const void*>(context->Rsp + 0x378),
                          &active_root, sizeof(active_root), &bytes_read);
        static std::atomic<u32> gate_trace_count{0};
        const u32 ordinal = gate_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 128) {
            char buffer[480]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "gate=%u thread=%lu active=0x%016llx/%u r10=0x%016llx/%u "
                "r15=0x%016llx/%u rbx=0x%016llx/%u rax=0x%016llx rsi=0x%016llx\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(active_root),
                ReadDreamsRootCount(process, active_root),
                static_cast<unsigned long long>(context->R10),
                ReadDreamsRootCount(process, context->R10),
                static_cast<unsigned long long>(context->R15),
                ReadDreamsRootCount(process, context->R15),
                static_cast<unsigned long long>(context->Rbx),
                ReadDreamsRootCount(process, context->Rbx),
                static_cast<unsigned long long>(context->Rax),
                static_cast<unsigned long long>(context->Rsi));
            AppendDreamsCpuRootTrace(buffer, length);
        }
        context->Rip = MemoryPatcher::g_eboot_address + DreamsSceneGateResumeOffset;
        return true;
    }

    if (guest_offset == DreamsRootPublishTraceOffset) {
        u64 active_root = 0;
        u64 previous_root = 0;
        ReadProcessMemory(process, reinterpret_cast<const void*>(context->Rsp + 0x378),
                          &active_root, sizeof(active_root), &bytes_read);
        ReadProcessMemory(process, reinterpret_cast<const void*>(context->Rbx + 0x10),
                          &previous_root, sizeof(previous_root), &bytes_read);
        const u32 root_count = ReadDreamsRootCount(process, active_root);
        const u32 resource_floor = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceFloorGlobalOffset);
        const u32 resource_current = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceCurrentGlobalOffset);
        const u32 resource_watermark_before = ReadDreamsU32(
            process, MemoryPatcher::g_eboot_address + DreamsResourceWatermarkGlobalOffset);
        u32 resource_watermark_after = resource_watermark_before;
        const u8 phase = ReadDreamsU8(process, active_root + 0x27b099);
        char handoff_value[2]{};
        const bool handoff_pending_retirement =
            GetEnvironmentVariableA("SHADPS4_DREAMS_PENDING_RETIREMENT_HANDOFF", handoff_value,
                                    sizeof(handoff_value)) != 0 &&
            handoff_value[0] == '1';
        const bool handoff_capture_active =
            handoff_pending_retirement && DreamsStampTraceCaptureEnabled();
        if (handoff_capture_active && root_count >= 3 && root_count <= 32 && phase == 3 &&
            resource_current != 0) {
            dreams_active_scene_root.store(active_root, std::memory_order_relaxed);
            dreams_active_scene_publish_tick.store(GetTickCount64(), std::memory_order_relaxed);
        } else if (handoff_pending_retirement) {
            dreams_active_scene_root.store(0, std::memory_order_relaxed);
            dreams_active_scene_publish_tick.store(0, std::memory_order_relaxed);
        }
        if (handoff_capture_active && root_count >= 3 && root_count <= 32) {
            if (phase == 3 && resource_floor == resource_current + 1 &&
                resource_watermark_before < resource_current) {
                // Unblock the exact one-generation stall without exposing the pending resource.
                resource_watermark_after = resource_current;
            } else if (resource_floor == resource_current &&
                       resource_watermark_before < resource_current) {
                // Release resources accumulated while earlier scene generations were stalled.
                resource_watermark_after = resource_current;
            }
        }
        char promote_value[2]{};
        const bool promote_active_watermark =
            GetEnvironmentVariableA("SHADPS4_DREAMS_PROMOTE_ACTIVE_WATERMARK", promote_value,
                                    sizeof(promote_value)) != 0 &&
            promote_value[0] == '1';
        if (promote_active_watermark && root_count >= 3 && root_count <= 32 &&
            resource_current == resource_floor &&
            resource_current == resource_watermark_after + 1) {
            resource_watermark_after = resource_current;
        }
        if (resource_watermark_after != resource_watermark_before) {
            SIZE_T bytes_written = 0;
            WriteProcessMemory(
                process,
                reinterpret_cast<void*>(MemoryPatcher::g_eboot_address +
                                        DreamsResourceWatermarkGlobalOffset),
                &resource_watermark_after, sizeof(resource_watermark_after), &bytes_written);
        }
        char activate_value[2]{};
        const bool activate_pending_resource =
            GetEnvironmentVariableA("SHADPS4_DREAMS_ACTIVATE_PENDING_RESOURCE", activate_value,
                                    sizeof(activate_value)) != 0 &&
            activate_value[0] == '1';
        char alias_value[2]{};
        const bool alias_pending_resource =
            GetEnvironmentVariableA("SHADPS4_DREAMS_ALIAS_PENDING_RESOURCE", alias_value,
                                    sizeof(alias_value)) != 0 &&
            alias_value[0] == '1';
        u32 activated_pending = 0;
        u32 aliased_pending = 0;
        u32 alias_index = 0xffffffffu;
        if (activate_pending_resource && root_count >= 3 && root_count <= 32 &&
            resource_watermark_after != 0) {
            const u32 index = ReadDreamsU32(process, active_root + 0x279484);
            if (index < root_count) {
                const u64 object =
                    ReadDreamsU64(process, active_root + 0x10958c0 + static_cast<u64>(index) * 8);
                const u32 object_header = ReadDreamsU32(process, object + 0xc);
                const u32 slots = ReadDreamsU32(process, object + 0x40);
                const u16 active_slot = static_cast<u16>(slots);
                const u16 pending_slot = static_cast<u16>(slots >> 16);
                const u64 table = ReadDreamsU64(
                    process, MemoryPatcher::g_eboot_address + 0x44d1730);
                if (alias_pending_resource && pending_slot != 0 && pending_slot != 0xffff) {
                    const u64 pending_index_address =
                        table + static_cast<u64>(pending_slot) * 0x38 + 0x20;
                    alias_index = ReadDreamsU32(process, pending_index_address);
                    const u32 fallback_index = ReadDreamsU32(process, table + 0x20);
                    if (alias_index == 0xffffffffu && fallback_index != 0xffffffffu) {
                        SIZE_T bytes_written = 0;
                        WriteProcessMemory(process, reinterpret_cast<void*>(pending_index_address),
                                           &fallback_index, sizeof(fallback_index), &bytes_written);
                        alias_index = fallback_index;
                        aliased_pending = 1;
                    }
                }
                if (object != 0 && (object_header & 0x7f) == 1 && active_slot == 0xffff &&
                    pending_slot != 0xffff &&
                    ReadDreamsU32(process, table + static_cast<u64>(pending_slot) * 0x38 + 0xc) <
                        resource_watermark_after) {
                    const u32 promoted_slots = static_cast<u32>(pending_slot) | 0xffff0000u;
                    SIZE_T bytes_written = 0;
                    WriteProcessMemory(process, reinterpret_cast<void*>(object + 0x40),
                                       &promoted_slots, sizeof(promoted_slots), &bytes_written);
                    activated_pending = 1;
                }
            }
        }
        static std::atomic<u32> publish_trace_count{0};
        const u32 ordinal = publish_trace_count.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<u32> last_resource_floor{0xffffffffu};
        static std::atomic<u32> last_resource_current{0xffffffffu};
        static std::atomic<u32> last_resource_watermark{0xffffffffu};
        const u32 previous_floor =
            last_resource_floor.exchange(resource_floor, std::memory_order_relaxed);
        const u32 previous_current =
            last_resource_current.exchange(resource_current, std::memory_order_relaxed);
        const u32 previous_watermark =
            last_resource_watermark.exchange(resource_watermark_after, std::memory_order_relaxed);
        const bool resources_changed = previous_floor != resource_floor ||
                                       previous_current != resource_current ||
                                       previous_watermark != resource_watermark_after;
        const bool stamp_capture = DreamsStampTraceCaptureEnabled();
        if (stamp_capture || (!DreamsStampTraceEnabled() && (ordinal < 128 || resources_changed))) {
            char buffer[512]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "publish=%u thread=%lu previous=0x%016llx/%u active=0x%016llx/%u "
                "rax=0x%016llx/%u r10=0x%016llx/%u r15=0x%016llx/%u "
                "phase=%u resources=%u,%u,%u->%u handoff=%u promoted=%u activated=%u aliased=%u "
                "alias_index=0x%08x\r\n",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(previous_root),
                ReadDreamsRootCount(process, previous_root),
                static_cast<unsigned long long>(active_root), root_count,
                static_cast<unsigned long long>(context->Rax),
                ReadDreamsRootCount(process, context->Rax),
                static_cast<unsigned long long>(context->R10),
                ReadDreamsRootCount(process, context->R10),
                static_cast<unsigned long long>(context->R15),
                ReadDreamsRootCount(process, context->R15), phase, resource_floor, resource_current,
                resource_watermark_before, resource_watermark_after,
                handoff_capture_active ? 1 : 0,
                promote_active_watermark ? 1 : 0, activated_pending, aliased_pending, alias_index);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rbx + 0x10), &context->Rax,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 4;
        return true;
    }

    return false;
}

static void WriteUnhandledExceptionDiagnostic(EXCEPTION_POINTERS* exception) noexcept {
    if (exception == nullptr || exception->ExceptionRecord == nullptr) {
        return;
    }

    const auto* record = exception->ExceptionRecord;
    const auto* context = exception->ContextRecord;
    const ULONG_PTR operation = record->NumberParameters > 0 ? record->ExceptionInformation[0] : 0;
    const ULONG_PTR fault_address =
        record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0;

    char buffer[1024]{};
    const int length = _snprintf_s(
        buffer, sizeof(buffer), _TRUNCATE,
        "code=0x%08lx exception=0x%016llx operation=%llu fault=0x%016llx thread=%lu\r\n"
        "eboot=0x%016llx guest_offset=0x%016llx\r\n"
        "rip=0x%016llx rsp=0x%016llx rbp=0x%016llx rax=0x%016llx rbx=0x%016llx "
        "rcx=0x%016llx rdx=0x%016llx rsi=0x%016llx rdi=0x%016llx\r\n",
        record->ExceptionCode, reinterpret_cast<unsigned long long>(record->ExceptionAddress),
        static_cast<unsigned long long>(operation), static_cast<unsigned long long>(fault_address),
        GetCurrentThreadId(), static_cast<unsigned long long>(MemoryPatcher::g_eboot_address),
        context != nullptr && context->Rip >= MemoryPatcher::g_eboot_address
            ? context->Rip - MemoryPatcher::g_eboot_address
            : 0,
        context != nullptr ? context->Rip : 0,
        context != nullptr ? context->Rsp : 0, context != nullptr ? context->Rbp : 0,
        context != nullptr ? context->Rax : 0, context != nullptr ? context->Rbx : 0,
        context != nullptr ? context->Rcx : 0, context != nullptr ? context->Rdx : 0,
        context != nullptr ? context->Rsi : 0, context != nullptr ? context->Rdi : 0);
    if (length <= 0) {
        return;
    }

    const HANDLE file = CreateFileW(L"shadps4-unhandled-exception.txt", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    const auto* signals = Signals::Instance();
    // Windows static guest red-zone protection
    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_BREAKPOINT:
        handled = HandleDreamsOfflineLimitsTrace(pExp) ||
                  HandleDreamsRetirementWatermarkWrite(pExp) ||
                  HandleDreamsSaveQuotaTrace(pExp) || HandleDreamsCpuRootTrace(pExp);
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Windows static guest red-zone protection
    const bool report_unhandled = use_static_windows_guest_red_zone_protection
                                      ? static_protection_exception
                                      : code != EXCEPTION_BREAKPOINT;
    if (report_unhandled) { // Windows static guest red-zone protection
        WriteUnhandledExceptionDiagnostic(pExp);
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            // If the guest has installed a custom signal handler, and the access violation didn't
            // come from HLE memory tracking, pass the signal on
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (!signals->DispatchIllegalInstruction(raw_context)) {
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
            UNREACHABLE_MSG("Unhandled illegal instruction at code address {}: {}",
                            fmt::ptr(code_address), DisassembleInstruction(code_address));
        }
        break;
    default:
        if (sig == SIGSLEEP) {
            // Sleep thread until signal is received again
            sigset_t sigset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGSLEEP);
            sigwait(&sigset, &sig);
        }
        break;
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to register access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to register illegal instruction signal handler.");
    ASSERT_MSG(sigaction(SIGSLEEP, &action, nullptr) == 0,
               "Failed to register sleep signal handler.");
#endif
}

SignalDispatch::~SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(RemoveVectoredExceptionHandler(handle), "Failed to remove exception handler.");
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to remove access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to remove illegal instruction signal handler.");
#endif
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
