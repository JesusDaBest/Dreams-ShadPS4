// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/memory_patcher.h"
#include "common/signal_context.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/dreams_trace_state.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
static constexpr DWORD MS_CPP_EXCEPTION = 0xE06D7363;
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

static bool DreamsCsgCompletionTraceEnabled() noexcept {
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("SHADPS4_DREAMS_CSG_COMPLETION_TRACE", value,
                                       sizeof(value)) != 0 &&
               value[0] == '1';
    }();
    return enabled;
}

static void AppendDreamsCsgCompletionTrace(const char* buffer, const int length) noexcept {
    if (length <= 0) {
        return;
    }
    // Keep one append-only handle for the run. Opening and closing a file at every breakpoint can
    // materially perturb the short waits this trace is intended to measure.
    static const HANDLE file = CreateFileW(
        L"dreams-csg-completion-trace.txt", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
    }
}

static bool DreamsModelBoundaryTraceEnabled() noexcept {
    static const bool enabled = [] {
        char value[2]{};
        const bool boundary =
            GetEnvironmentVariableA("SHADPS4_DREAMS_MODEL_BOUNDARY_TRACE", value,
                                    sizeof(value)) != 0 &&
            value[0] == '1';
        char record_value[2]{};
        const bool full_record =
            GetEnvironmentVariableA("SHADPS4_DREAMS_MODEL_RECORD_TRACE", record_value,
                                    sizeof(record_value)) != 0 &&
            record_value[0] == '1';
        return boundary && !full_record;
    }();
    return enabled;
}

static bool DreamsModelBoundaryCaptureEnabled() noexcept {
    return DreamsModelBoundaryTraceEnabled() &&
           GetFileAttributesW(L"dreams-model-boundary.capture") != INVALID_FILE_ATTRIBUTES;
}

static void AppendDreamsModelBoundaryTrace(const char* buffer, const int length) noexcept {
    if (length <= 0) {
        return;
    }
    // Keep one handle for the run so this diagnostic does not add an open/close pair to every
    // CSG replay. The capture marker controls which model builds are written.
    static const HANDLE file = CreateFileW(
        L"dreams-model-boundary-trace.txt", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
    }
}

static u64 DreamsQpcElapsedMicroseconds(const u64 start, const u64 end,
                                        const u64 frequency) noexcept {
    if (start == 0 || end < start || frequency == 0) {
        return 0;
    }
    const u64 delta = end - start;
    return (delta / frequency) * 1'000'000ULL +
           ((delta % frequency) * 1'000'000ULL) / frequency;
}

static void AppendDreamsSculptProvenanceTrace(const char* buffer, const int length) noexcept {
    if (length <= 0) {
        return;
    }
    const HANDLE file = CreateFileW(L"dreams-sculpt-provenance.txt", FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
}

static void AppendDreamsVisibleSculptsTrace(const char* buffer, const int length) noexcept {
    if (length <= 0) {
        return;
    }
    const HANDLE file = CreateFileW(L"dreams-visible-sculpts-trace.txt", FILE_APPEND_DATA,
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

static bool ReadDreamsBytes(const HANDLE process, const u64 address, void* destination,
                            const SIZE_T size) noexcept {
    if (address == 0 || destination == nullptr || size == 0) {
        return false;
    }
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), destination, size,
                             &bytes_read) &&
           bytes_read == size;
}

static u64 HashDreamsBytes(const void* source, const SIZE_T size) noexcept {
    constexpr u64 FnvOffsetBasis = 1469598103934665603ULL;
    constexpr u64 FnvPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const u8*>(source);
    u64 hash = FnvOffsetBasis;
    for (SIZE_T index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }
    return hash;
}

struct DreamsProcessHash {
    u64 requested_size{};
    u64 captured_size{};
    u64 hash{};
    std::array<u32, 4> head_words{};
    bool valid{};
    bool complete{};
};

static DreamsProcessHash HashDreamsProcessBytes(const HANDLE process, const u64 address,
                                                const u64 requested_size,
                                                const u64 capture_limit) noexcept {
    constexpr u64 FnvOffsetBasis = 1469598103934665603ULL;
    constexpr u64 FnvPrime = 1099511628211ULL;
    constexpr SIZE_T ChunkSize = 64 * 1024;
    static thread_local std::array<u8, ChunkSize> chunk{};

    DreamsProcessHash result{};
    result.requested_size = requested_size;
    result.hash = FnvOffsetBasis;
    if (requested_size == 0) {
        result.valid = true;
        result.complete = true;
        return result;
    }
    if (address == 0 || requested_size > ~0ULL - address) {
        return result;
    }

    const u64 capture_size = std::min(requested_size, capture_limit);
    while (result.captured_size < capture_size) {
        const SIZE_T read_size = static_cast<SIZE_T>(
            std::min<u64>(ChunkSize, capture_size - result.captured_size));
        if (!ReadDreamsBytes(process, address + result.captured_size, chunk.data(), read_size)) {
            return result;
        }
        if (result.captured_size == 0) {
            std::memcpy(result.head_words.data(), chunk.data(),
                        std::min(read_size, sizeof(result.head_words)));
        }
        for (SIZE_T index = 0; index < read_size; ++index) {
            result.hash ^= chunk[index];
            result.hash *= FnvPrime;
        }
        result.captured_size += read_size;
    }
    result.valid = true;
    result.complete = result.captured_size == requested_size;
    return result;
}

static bool IsDreamsVisibleSculptsWarningSource(const HANDLE process,
                                                const u64 address) noexcept {
    if (address == 0) {
        return false;
    }

    // The source form may contain Dreams' inline tag bytes around each occurrence of
    // "sculptures". Match the complete ordered wording while allowing those tag bytes.
    std::array<char, 512> source{};
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address), source.data(),
                           source.size() - 1, &bytes_read) ||
        bytes_read == 0) {
        return false;
    }
    source[std::min(bytes_read, source.size() - 1)] = '\0';

    const char* cursor = source.data();
    if (std::strncmp(cursor, "Some ", 5) != 0) {
        return false;
    }
    cursor = std::strstr(cursor + 5, "sculptures");
    if (cursor == nullptr) {
        return false;
    }
    cursor = std::strstr(cursor + std::strlen("sculptures"),
                         "have stopped drawing, because too many overlapping");
    if (cursor == nullptr) {
        return false;
    }
    cursor = std::strstr(cursor + std::strlen(
                                     "have stopped drawing, because too many overlapping"),
                         "sculptures");
    if (cursor == nullptr) {
        return false;
    }
    return std::strstr(cursor + std::strlen("sculptures"), "are visible at once.") != nullptr;
}

static bool HandleDreamsVisibleSculptsTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr std::array<u64, 8> DreamsTranslationLookupOffsets{
        0x148a3a0, 0x148a530, 0x148a6c0, 0x148a780,
        0x148a8c0, 0x148ac00, 0x148ad40, 0x148ada0,
    };
    constexpr u64 VisibleSculptsWarningKey = 0x583aabbb;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 lookup_offset = breakpoint_address - MemoryPatcher::g_eboot_address;
    bool is_translation_lookup = false;
    for (const u64 offset : DreamsTranslationLookupOffsets) {
        is_translation_lookup |= lookup_offset == offset;
    }
    if (!is_translation_lookup) {
        return false;
    }

    CONTEXT* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    u64 raw_key = 0;
    bool source_match = false;
    if (lookup_offset == 0x148a6c0) {
        source_match = IsDreamsVisibleSculptsWarningSource(process, context->Rdi);
    } else if (lookup_offset == 0x148a8c0) {
        raw_key = context->Rdi;
    } else if (lookup_offset == 0x148ad40 || lookup_offset == 0x148ada0) {
        raw_key = context->Rsi != 0 ? ReadDreamsU64(process, context->Rsi) : 0;
    } else {
        raw_key = context->Rdi != 0 ? ReadDreamsU64(process, context->Rdi) : 0;
    }
    if (static_cast<u32>(raw_key) == VisibleSculptsWarningKey || source_match) {
        const u64 caller = ReadDreamsU64(process, context->Rsp);
        const u64 frame0 = context->Rbp;
        const u64 frame1 = frame0 != 0 ? ReadDreamsU64(process, frame0) : 0;
        const u64 return0 = frame0 != 0 ? ReadDreamsU64(process, frame0 + 8) : 0;
        const u64 return1 = frame1 != 0 ? ReadDreamsU64(process, frame1 + 8) : 0;
        char buffer[768]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "visible_sculpts_warning tick=%llu thread=%lu key=0x%016llx source_match=%u "
            "lookup_offset=0x%016llx "
            "caller=0x%016llx caller_offset=0x%016llx frame_return0=0x%016llx "
            "frame_return1=0x%016llx rdi=0x%016llx rsi=0x%016llx rdx=0x%016llx "
            "rcx=0x%016llx r8=0x%016llx r9=0x%016llx\r\n",
            static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            static_cast<unsigned long long>(raw_key),
            source_match ? 1u : 0u,
            static_cast<unsigned long long>(lookup_offset),
            static_cast<unsigned long long>(caller),
            caller >= MemoryPatcher::g_eboot_address
                ? static_cast<unsigned long long>(caller - MemoryPatcher::g_eboot_address)
                : 0,
            static_cast<unsigned long long>(return0),
            static_cast<unsigned long long>(return1),
            static_cast<unsigned long long>(context->Rdi),
            static_cast<unsigned long long>(context->Rsi),
            static_cast<unsigned long long>(context->Rdx),
            static_cast<unsigned long long>(context->Rcx),
            static_cast<unsigned long long>(context->R8),
            static_cast<unsigned long long>(context->R9));
        AppendDreamsVisibleSculptsTrace(buffer, length);

        // This trace only needs the first request for the warning. Restore every lookup
        // prologue immediately so later translation lookups run at full speed.
        constexpr u8 OriginalInstruction = 0x55;
        for (const u64 offset : DreamsTranslationLookupOffsets) {
            void* address = reinterpret_cast<void*>(MemoryPatcher::g_eboot_address + offset);
            SIZE_T restored = 0;
            WriteProcessMemory(process, address, &OriginalInstruction,
                               sizeof(OriginalInstruction), &restored);
            FlushInstructionCache(process, address, sizeof(OriginalInstruction));
        }
    }

    // The breakpoint replaces the original one-byte `push rbp` prologue.
    context->Rsp -= sizeof(u64);
    SIZE_T bytes_written = 0;
    WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                       sizeof(u64), &bytes_written);
    context->Rip = breakpoint_address + 1;
    return true;
}

static bool DreamsPairQueueCaptureEnabled() noexcept {
    return GetFileAttributesW(L"dreams-pair-queue.capture") != INVALID_FILE_ATTRIBUTES;
}

static bool HandleDreamsSceneReadyHandoff(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsSceneReadyHandoffOffset = 0x9ab22e;
    constexpr u64 DreamsMainRendererOffset = 0x948760;
    constexpr u64 DreamsRootPhaseOffset = 0x27b099;
    constexpr u64 DreamsRootReadyOffset = 0x27b0a9;
    constexpr u64 DreamsRootReadyInputOffset = 0x27b0b0;
    constexpr u64 DreamsRootFallbackInputOffset = 0x27b0ac;
    constexpr u64 DreamsRootAsyncInputOffset = 0x27b0e4;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 guest_offset = breakpoint_address - MemoryPatcher::g_eboot_address;
    if (guest_offset != DreamsSceneReadyHandoffOffset) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u64 manager = context->Rdi;
    const u64 source_root = manager != 0 ? ReadDreamsU64(process, manager + 0x8) : 0;
    const u64 root = manager != 0 ? ReadDreamsU64(process, manager + 0x10) : 0;
    const u8 phase = root != 0 ? ReadDreamsU8(process, root + DreamsRootPhaseOffset) : 0xff;
    const u8 ready_before =
        root != 0 ? ReadDreamsU8(process, root + DreamsRootReadyOffset) : 0xff;
    const u8 ready_input =
        root != 0 ? ReadDreamsU8(process, root + DreamsRootReadyInputOffset) : 0;
    const u8 fallback_input =
        root != 0 ? ReadDreamsU8(process, root + DreamsRootFallbackInputOffset) : 0;
    const u8 async_input =
        root != 0 ? ReadDreamsU8(process, root + DreamsRootAsyncInputOffset) : 0;
    char repair_value[2]{};
    const bool repair_enabled =
        GetEnvironmentVariableA("SHADPS4_DREAMS_SCENE_READY_HANDOFF", repair_value,
                                sizeof(repair_value)) != 0 &&
        repair_value[0] == '1';
    const bool repaired = repair_enabled && root != 0 && phase == 3 && ready_before == 0 &&
                          ready_input != 0 && (async_input == 0 || fallback_input == 0);
    if (repaired) {
        constexpr u8 Ready = 1;
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(root + DreamsRootReadyOffset), &Ready,
                           sizeof(Ready), &bytes_written);
    }

    static std::atomic<u32> handoff_trace_count{0};
    const u32 ordinal = handoff_trace_count.fetch_add(1, std::memory_order_relaxed);
    char trace_value[2]{};
    const bool trace_enabled =
        GetEnvironmentVariableA("SHADPS4_DREAMS_SCENE_WAKE_TRACE", trace_value,
                                sizeof(trace_value)) != 0 &&
        trace_value[0] == '1';
    if (ordinal < (trace_enabled ? 4096u : 512u) || repaired) {
        char buffer[768]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "scene_ready_handoff=%u tick=%llu thread=%lu manager=0x%016llx "
            "source=0x%016llx/%u:{phase=%u ready=%u input=%u} "
            "root=0x%016llx/%u:{phase=%u ready=%u->%u input=%u fallback=%u async=%u} "
            "args={rdx=%llu r12=%llu} repaired=%u trace=%u\r\n",
            ordinal, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            static_cast<unsigned long long>(manager),
            static_cast<unsigned long long>(source_root),
            source_root != 0 ? ReadDreamsRootCount(process, source_root) : 0,
            source_root != 0 ? ReadDreamsU8(process, source_root + DreamsRootPhaseOffset) : 0xff,
            source_root != 0 ? ReadDreamsU8(process, source_root + DreamsRootReadyOffset) : 0xff,
            source_root != 0 ? ReadDreamsU8(process, source_root + DreamsRootReadyInputOffset)
                             : 0xff,
            static_cast<unsigned long long>(root), root != 0 ? ReadDreamsRootCount(process, root) : 0,
            phase, ready_before,
            root != 0 ? ReadDreamsU8(process, root + DreamsRootReadyOffset) : 0xff, ready_input,
            fallback_input, async_input, static_cast<unsigned long long>(context->Rdx),
            static_cast<unsigned long long>(context->R12), repaired ? 1 : 0,
            trace_enabled ? 1 : 0);
        AppendDreamsCpuRootTrace(buffer, length);
    }

    const u64 return_address = breakpoint_address + 5;
    context->Rsp -= sizeof(return_address);
    SIZE_T bytes_written = 0;
    WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &return_address,
                       sizeof(return_address), &bytes_written);
    context->Rip = MemoryPatcher::g_eboot_address + DreamsMainRendererOffset;
    return true;
}

static bool HandleDreamsSceneBuilderGateTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsSceneBuilderGateObservedOffset = 0x987976;
    constexpr u64 DreamsRootPhaseOffset = 0x27b099;
    constexpr u64 DreamsRootReadyOffset = 0x27b0a9;
    constexpr u64 DreamsRootReadyInputOffset = 0x27b0b0;
    constexpr u64 DreamsRootFallbackInputOffset = 0x27b0ac;
    constexpr u64 DreamsRootAsyncInputOffset = 0x27b0e4;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    if (breakpoint_address - MemoryPatcher::g_eboot_address !=
        DreamsSceneBuilderGateObservedOffset) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u64 root = ReadDreamsU64(process, context->Rsp + 0x650);
    const u64 manager = ReadDreamsU64(process, context->Rsp + 0x658);
    const u8 stack_gate = ReadDreamsU8(process, context->Rsp + 0x26c);
    const u8 mode = ReadDreamsU8(process, context->Rsp + 0x288);
    const bool zero = (context->EFlags & (1u << 6)) != 0;

    static std::atomic<u32> gate_trace_count{0};
    const u32 ordinal = gate_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (ordinal < 4096) {
        char buffer[640]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "scene_builder_gate=%u tick=%llu thread=%lu root=0x%016llx/%u "
            "manager=0x%016llx cmp={value=%u zf=%u} mode=%u "
            "state={phase=%u ready=%u input=%u fallback=%u async=%u}\r\n",
            ordinal, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            static_cast<unsigned long long>(root),
            root != 0 ? ReadDreamsRootCount(process, root) : 0,
            static_cast<unsigned long long>(manager), stack_gate, zero ? 1 : 0, mode,
            root != 0 ? ReadDreamsU8(process, root + DreamsRootPhaseOffset) : 0xff,
            root != 0 ? ReadDreamsU8(process, root + DreamsRootReadyOffset) : 0xff,
            root != 0 ? ReadDreamsU8(process, root + DreamsRootReadyInputOffset) : 0xff,
            root != 0 ? ReadDreamsU8(process, root + DreamsRootFallbackInputOffset) : 0xff,
            root != 0 ? ReadDreamsU8(process, root + DreamsRootAsyncInputOffset) : 0xff);
        AppendDreamsCpuRootTrace(buffer, length);
    }

    // The replaced instruction is `mov rax, [rsp + 0x650]`.
    context->Rax = root;
    context->Rip = breakpoint_address + 8;
    return true;
}

static bool HandleDreamsSceneCacheBootstrap(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsSceneCacheBootstrapOffset = 0x8b74c0;
    constexpr u64 DreamsSceneCacheBootstrapResumeOffset = 0x8b74c6;
    constexpr u64 DreamsSceneCacheFullBuildOffset = 0x8b7b89;
    constexpr u64 DreamsRootIdentityOffset = 0x1c2bf90;
    constexpr u64 DreamsRootReadyOffset = 0x27b0a9;
    constexpr u64 DreamsRootReadyInputOffset = 0x27b0b0;
    constexpr u64 DreamsRootGenerationOffset = 0x27b0c0;
    constexpr u64 DreamsCachedIdentityOffset = 0x1cbfc8;
    constexpr u64 DreamsIncrementalTable0SizeOffset = 0x243ff0;
    constexpr u64 DreamsIncrementalTable1SizeOffset = 0x244010;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    if (breakpoint_address - MemoryPatcher::g_eboot_address !=
        DreamsSceneCacheBootstrapOffset) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u64 root = context->R9;
    const u64 builder = context->R15;
    const u64 generation = ReadDreamsU64(process, root + DreamsRootGenerationOffset);
    const u64 root_identity = ReadDreamsU64(process, root + DreamsRootIdentityOffset);
    const u64 cached_identity = ReadDreamsU64(process, builder + DreamsCachedIdentityOffset);
    const u8 ready = ReadDreamsU8(process, root + DreamsRootReadyOffset);
    const u8 ready_input = ReadDreamsU8(process, root + DreamsRootReadyInputOffset);

    const u64 table0_size =
        ReadDreamsU64(process, builder + DreamsIncrementalTable0SizeOffset);
    const u64 table1_size =
        ReadDreamsU64(process, builder + DreamsIncrementalTable1SizeOffset);
    const bool tables_empty = table0_size == 0 && table1_size == 0;
    const bool missing_cache = cached_identity == ~0ull && root_identity != ~0ull;
    const bool bootstrap = generation != 0 && ready != 0 && ready_input != 0 && missing_cache &&
                           tables_empty;
    const bool full_build = generation == 0 || bootstrap;

    static std::atomic<u32> cache_bootstrap_trace_count{0};
    const u32 ordinal = cache_bootstrap_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (ordinal < 256 || bootstrap) {
        char buffer[512]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "scene_cache_bootstrap=%u thread=%lu root=0x%016llx/%u builder=0x%016llx "
            "identity=0x%016llx/0x%016llx generation=%llu ready=%u,%u tables=%llu,%llu "
            "bootstrap=%u full=%u\r\n",
            ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(root),
            ReadDreamsRootCount(process, root), static_cast<unsigned long long>(builder),
            static_cast<unsigned long long>(root_identity),
            static_cast<unsigned long long>(cached_identity),
            static_cast<unsigned long long>(generation), ready, ready_input,
            static_cast<unsigned long long>(table0_size),
            static_cast<unsigned long long>(table1_size), bootstrap ? 1 : 0,
            full_build ? 1 : 0);
        AppendDreamsCpuRootTrace(buffer, length);
    }

    context->Rip = MemoryPatcher::g_eboot_address +
                   (full_build ? DreamsSceneCacheFullBuildOffset
                               : DreamsSceneCacheBootstrapResumeOffset);
    return true;
}

static bool HandleDreamsPairQueueTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsType1RendererOffset = 0x1008bc0;
    constexpr u64 DreamsPairQueueAppendOffset = 0x130b8a0;
    constexpr u64 DreamsPairQueuePublishOffset = 0xc20bc5;
    constexpr u64 DreamsFrameTagOffset = 0x789687c;
    constexpr u64 DreamsQueueATagOffset = 0x78754e0;
    constexpr u64 DreamsQueueACursorOffset = 0x78754e4;
    constexpr u64 DreamsQueueAPreviousOffset = 0x78754ec;
    constexpr u64 DreamsQueueALimitOffset = 0x78754f0;
    constexpr u64 DreamsQueueBTagOffset = 0x7875588;
    constexpr u64 DreamsQueueBCursorOffset = 0x787558c;
    constexpr u64 DreamsQueueBLimitOffset = 0x7875598;
    constexpr u64 DreamsPublishedCountOffset = 0x78755a0;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 guest_offset = breakpoint_address - MemoryPatcher::g_eboot_address;
    if (guest_offset != DreamsType1RendererOffset &&
        guest_offset != DreamsPairQueueAppendOffset &&
        guest_offset != DreamsPairQueuePublishOffset) {
        return false;
    }

    const HANDLE process = GetCurrentProcess();
    CONTEXT* context = exception->ContextRecord;
    const bool capture = DreamsPairQueueCaptureEnabled();
    const u64 base = MemoryPatcher::g_eboot_address;

    if (guest_offset == DreamsType1RendererOffset) {
        static std::atomic<u32> type1_trace_count{0};
        const u32 ordinal = type1_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (capture && ordinal < 16384) {
            // The guest function reads this argument from [rbp + 0x38] after its one-byte
            // `push rbp` prologue, so at the entry breakpoint it is at the original rsp + 0x30.
            const u32 packed_index = ReadDreamsU32(process, context->Rsp + 0x30);
            const s32 index = static_cast<s32>(packed_index << 10) >> 10;
            const u32 root_count = ReadDreamsRootCount(process, context->Rdi);
            const u64 object = index >= 0 && static_cast<u32>(index) < root_count
                                   ? ReadDreamsU64(process, context->Rdi + 0x10958c0 +
                                                               static_cast<u64>(index) * 8)
                                   : 0;
            char buffer[512]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "type1_entry=%u tick=%llu thread=%lu caller=0x%016llx root=0x%016llx/%u "
                "packed=0x%08x index=%d object=0x%016llx id=0x%08x header=0x%08x "
                "slot=%u aux_slot=%u resource=0x%08x suppress=%u\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp)),
                static_cast<unsigned long long>(context->Rdi), root_count, packed_index, index,
                static_cast<unsigned long long>(object),
                object != 0 ? ReadDreamsU32(process, object) : 0,
                object != 0 ? ReadDreamsU32(process, object + 0xc) : 0,
                object != 0 ? ReadDreamsU16(process, object + 0x40) : 0xffff,
                object != 0 ? ReadDreamsU16(process, object + 0x42) : 0xffff,
                object != 0 ? ReadDreamsU32(process, object + 0x170) : 0,
                object != 0 ? ReadDreamsU8(process, object + 0x45) : 0xff);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        // Emulate the replaced one-byte `push rbp`.
        context->Rsp -= sizeof(u64);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 1;
        return true;
    }

    if (guest_offset == DreamsPairQueueAppendOffset) {
        static std::atomic<u32> append_trace_count{0};
        const u32 ordinal = append_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (capture && ordinal < 16384) {
            const u64 caller = ReadDreamsU64(process, context->Rsp);
            const u64 caller_offset = caller - base;
            const bool type1_caller = caller_offset == 0x100acc1 || caller_offset == 0x100aff2;
            const u64 object = type1_caller ? ReadDreamsU64(process, context->Rbp - 0x160) : 0;
            u64 match_46 = 0;
            u64 match_1480 = 0;
            u64 match_id = 0;
            for (u32 word = 0; word < 0xc8 / sizeof(u32); ++word) {
                const u32 value = ReadDreamsU32(process, context->Rdi + word * sizeof(u32));
                if (value == 0x46) {
                    match_46 |= 1ull << word;
                }
                if (value == 0x1480) {
                    match_1480 |= 1ull << word;
                }
                if (value == 0xfffefc81 || value == 0xfffefc83 || value == 0xfffefc85 ||
                    value == 0xfffefc87) {
                    match_id |= 1ull << word;
                }
            }

            char buffer[1024]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "pair_append=%u tick=%llu thread=%lu caller=0x%016llx/0x%08llx "
                "object=0x%016llx id=0x%08x src=0x%016llx aux=0x%016llx "
                "frame=%u tags=%u,%u "
                "cursor=%u/%u,%u/%u matches=0x%013llx,0x%013llx,0x%013llx "
                "words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,"
                "%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(),
                static_cast<unsigned long long>(caller),
                static_cast<unsigned long long>(caller_offset),
                static_cast<unsigned long long>(object),
                object != 0 ? ReadDreamsU32(process, object) : 0,
                static_cast<unsigned long long>(context->Rdi),
                static_cast<unsigned long long>(context->Rsi),
                ReadDreamsU32(process, base + DreamsFrameTagOffset),
                ReadDreamsU32(process, base + DreamsQueueATagOffset),
                ReadDreamsU32(process, base + DreamsQueueBTagOffset),
                ReadDreamsU32(process, base + DreamsQueueACursorOffset),
                ReadDreamsU32(process, base + DreamsQueueALimitOffset),
                ReadDreamsU32(process, base + DreamsQueueBCursorOffset),
                ReadDreamsU32(process, base + DreamsQueueBLimitOffset),
                static_cast<unsigned long long>(match_46),
                static_cast<unsigned long long>(match_1480),
                static_cast<unsigned long long>(match_id),
                ReadDreamsU32(process, context->Rdi + 0x00),
                ReadDreamsU32(process, context->Rdi + 0x04),
                ReadDreamsU32(process, context->Rdi + 0x08),
                ReadDreamsU32(process, context->Rdi + 0x0c),
                ReadDreamsU32(process, context->Rdi + 0x10),
                ReadDreamsU32(process, context->Rdi + 0x14),
                ReadDreamsU32(process, context->Rdi + 0x18),
                ReadDreamsU32(process, context->Rdi + 0x1c),
                ReadDreamsU32(process, context->Rdi + 0x20),
                ReadDreamsU32(process, context->Rdi + 0x24),
                ReadDreamsU32(process, context->Rdi + 0x28),
                ReadDreamsU32(process, context->Rdi + 0x2c),
                ReadDreamsU32(process, context->Rdi + 0xa8),
                ReadDreamsU32(process, context->Rdi + 0xac),
                ReadDreamsU32(process, context->Rdi + 0xc0),
                ReadDreamsU32(process, context->Rdi + 0xc4));
            AppendDreamsCpuRootTrace(buffer, length);
        }

        // Emulate the replaced one-byte `push rbp`.
        context->Rsp -= sizeof(u64);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 1;
        return true;
    }

    const u32 published = static_cast<u32>(context->Rdx);
    if (capture) {
        static std::atomic<u32> publish_trace_count{0};
        const u32 ordinal = publish_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 8192) {
            char buffer[448]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "pair_publish=%u tick=%llu thread=%lu frame=%u tags=%u,%u "
                "cursor=%u previous=%u limit=%u b=%u/%u delta=%u old=%u\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(),
                ReadDreamsU32(process, base + DreamsFrameTagOffset),
                ReadDreamsU32(process, base + DreamsQueueATagOffset),
                ReadDreamsU32(process, base + DreamsQueueBTagOffset),
                ReadDreamsU32(process, base + DreamsQueueACursorOffset),
                ReadDreamsU32(process, base + DreamsQueueAPreviousOffset),
                ReadDreamsU32(process, base + DreamsQueueALimitOffset),
                ReadDreamsU32(process, base + DreamsQueueBCursorOffset),
                ReadDreamsU32(process, base + DreamsQueueBLimitOffset), published,
                ReadDreamsU32(process, base + DreamsPublishedCountOffset));
            AppendDreamsCpuRootTrace(buffer, length);
        }
    }

    // Emulate `mov dword ptr [published_count], edx`.
    SIZE_T bytes_written = 0;
    WriteProcessMemory(process, reinterpret_cast<void*>(base + DreamsPublishedCountOffset),
                       &published, sizeof(published), &bytes_written);
    context->Rip = breakpoint_address + 6;
    return true;
}

static bool DreamsModelRecordCaptureEnabled() noexcept {
    return GetFileAttributesW(L"dreams-model-record.capture") != INVALID_FILE_ATTRIBUTES;
}

static bool DreamsModelInputTraceEnabled() noexcept {
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("SHADPS4_DREAMS_MODEL_INPUT_TRACE", value,
                                       sizeof(value)) != 0 &&
               value[0] == '1';
    }();
    return enabled;
}

static void AppendDreamsModelInputTrace(const char* buffer, const int length) noexcept {
    if (length <= 0) {
        return;
    }
    static std::atomic<u32> line_count{};
    const u32 line = line_count.fetch_add(1, std::memory_order_relaxed);
    if (line >= 512) {
        return;
    }
    const HANDLE file = CreateFileW(L"dreams-model-input-trace.txt", FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    line == 0 ? CREATE_ALWAYS : OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
}

static bool ActivateDreamsCsgActionTrace(const u32 target) noexcept {
    std::array<wchar_t, 1024> arm_path{};
    const DWORD arm_length = GetEnvironmentVariableW(L"SHADPS4_DREAMS_CSG_TRACE_ARM_FILE",
                                                     arm_path.data(), arm_path.size());
    if (arm_length == 0 || arm_length >= arm_path.size()) {
        return false;
    }

    std::array<wchar_t, 1024> trigger_path{};
    const DWORD trigger_length =
        GetEnvironmentVariableW(L"SHADPS4_DREAMS_CSG_TRACE_TRIGGER_FILE", trigger_path.data(),
                                trigger_path.size());
    std::array<wchar_t, 1024> dependency_trigger_path{};
    const DWORD dependency_trigger_length = GetEnvironmentVariableW(
        L"SHADPS4_DREAMS_DEP_TRACE_TRIGGER_FILE", dependency_trigger_path.data(),
        dependency_trigger_path.size());
    if (trigger_length == 0 || trigger_length >= trigger_path.size() ||
        dependency_trigger_length == 0 ||
        dependency_trigger_length >= dependency_trigger_path.size() ||
        CompareStringOrdinal(trigger_path.data(), -1, dependency_trigger_path.data(), -1,
                             FALSE) != CSTR_EQUAL) {
        return false;
    }

    const DWORD arm_attributes = GetFileAttributesW(arm_path.data());
    if (arm_attributes == INVALID_FILE_ATTRIBUTES ||
        (arm_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    if (!MoveFileExW(arm_path.data(), trigger_path.data(), 0)) {
        const DWORD error = GetLastError();
        char trace[224]{};
        const int trace_length = _snprintf_s(
            trace, sizeof(trace), _TRUNCATE,
            "csg_action_trace=activation_failed tick=%llu target=%u error=%lu\r\n",
            static_cast<unsigned long long>(GetTickCount64()), target, error);
        AppendDreamsCpuRootTrace(trace, trace_length);
        return false;
    }

    char trace[192]{};
    const int trace_length = _snprintf_s(
        trace, sizeof(trace), _TRUNCATE, "csg_action_trace=begin tick=%llu target=%u\r\n",
        static_cast<unsigned long long>(GetTickCount64()), target);
    AppendDreamsCpuRootTrace(trace, trace_length);
    return true;
}

static bool HandleDreamsModelBoundaryTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsModelBuildOffset = 0x720950;
    constexpr u64 DreamsModelBuildReturnOffset = 0x723870;
    constexpr u64 DreamsCsgCallOffset = 0x12850c1;
    constexpr u64 DreamsCsgReturnBranchOffset = 0x12850c8;
    constexpr u64 DreamsCsgInputDispatchOffset = 0x128087d;
    constexpr u64 DreamsCsgReplayResultOffset = 0x1281f5e;
    constexpr u64 DreamsCsgReplayCountOffset = 0x1281f6e;
    constexpr u64 DreamsModelAggregateCountOffset = 0x1282390;
    constexpr u64 DreamsModelResultPointerOffset = 0x77ed7d8;
    constexpr u64 DreamsCoordinateListPointerOffset = 0x782ed50;
    constexpr u64 DreamsCoordinateListCountOffset = 0x782ed58;
    constexpr u32 CoordinateSampleLimit = 4096;

    if (!DreamsModelBoundaryTraceEnabled() || exception == nullptr ||
        exception->ExceptionRecord == nullptr || exception->ContextRecord == nullptr ||
        MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 base = MemoryPatcher::g_eboot_address;
    const u64 guest_offset = breakpoint_address - base;
    if (guest_offset != DreamsModelBuildOffset &&
        guest_offset != DreamsModelBuildReturnOffset && guest_offset != DreamsCsgCallOffset &&
        guest_offset != DreamsCsgReturnBranchOffset &&
        guest_offset != DreamsCsgInputDispatchOffset &&
        guest_offset != DreamsCsgReplayResultOffset &&
        guest_offset != DreamsCsgReplayCountOffset &&
        guest_offset != DreamsModelAggregateCountOffset) {
        return false;
    }

    struct BoundaryState {
        u32 build_sequence{};
        u32 model_id{};
        u32 target{};
        u32 csg_sequence{};
        u64 csg_request_address{};
        bool first_csg_input_pending{};
        bool active{};
    };
    static thread_local BoundaryState state{};
    static std::atomic<u32> build_sequence{0};
    static thread_local std::array<u32, CoordinateSampleLimit> coordinate_sample{};

    CONTEXT* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();

    const auto trace_snapshot = [&](const char* kind, const u64 result, const u32 extra,
                                    const u32 status) noexcept {
        if (!state.active) {
            return;
        }
        const u64 list_pointer =
            ReadDreamsU64(process, base + DreamsCoordinateListPointerOffset);
        const u32 list_count = ReadDreamsU32(process, base + DreamsCoordinateListCountOffset);
        const u32 sample_count = std::min(list_count, CoordinateSampleLimit);
        const bool list_valid =
            sample_count == 0 ||
            ReadDreamsBytes(process, list_pointer, coordinate_sample.data(),
                            static_cast<SIZE_T>(sample_count) * sizeof(u32));
        const u64 list_hash =
            list_valid && sample_count != 0
                ? HashDreamsBytes(coordinate_sample.data(),
                                  static_cast<SIZE_T>(sample_count) * sizeof(u32))
                : 0;
        u32 min_x = 0xffffffffu;
        u32 min_y = 0xffffffffu;
        u32 min_z = 0xffffffffu;
        u32 max_x = 0;
        u32 max_y = 0;
        u32 max_z = 0;
        if (list_valid) {
            for (u32 index = 0; index < sample_count; ++index) {
                const u32 packed = coordinate_sample[index];
                const u32 x = packed & 0x3ffu;
                const u32 y = (packed >> 10) & 0x3ffu;
                const u32 z = (packed >> 20) & 0x3ffu;
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                min_z = std::min(min_z, z);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
                max_z = std::max(max_z, z);
            }
        }

        char buffer[1280]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "model_boundary kind=%s tick=%llu thread=%lu build=%u model=%u target=%u "
            "csg=%u status=%u extra=%u result=0x%016llx "
            "values=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
            "min=%u,%u,%u max=%u,%u,%u accum=%u,%u,%u,%u,%u,%u "
            "list=0x%016llx count=%u sampled=%u valid=%u hash=0x%016llx "
            "sample_min=%u,%u,%u sample_max=%u,%u,%u\r\n",
            kind, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            state.build_sequence, state.model_id, state.target, state.csg_sequence, status, extra,
            static_cast<unsigned long long>(result), ReadDreamsU32(process, result),
            ReadDreamsU32(process, result + 0x4), ReadDreamsU32(process, result + 0x8),
            ReadDreamsU32(process, result + 0xc), ReadDreamsU32(process, result + 0x18),
            ReadDreamsU32(process, result + 0x1c), ReadDreamsU32(process, result + 0x20),
            ReadDreamsU32(process, result + 0x24), ReadDreamsU32(process, result + 0x28),
            ReadDreamsU32(process, result + 0x48), ReadDreamsU32(process, result + 0x2c),
            ReadDreamsU32(process, result + 0x30), ReadDreamsU32(process, result + 0x34),
            ReadDreamsU32(process, result + 0x38), ReadDreamsU32(process, result + 0x3c),
            ReadDreamsU32(process, result + 0x40), ReadDreamsU32(process, result + 0x50),
            ReadDreamsU32(process, result + 0x54), ReadDreamsU32(process, result + 0x58),
            ReadDreamsU32(process, result + 0x5c), ReadDreamsU32(process, result + 0x60),
            ReadDreamsU32(process, result + 0x64),
            static_cast<unsigned long long>(list_pointer), list_count, sample_count,
            list_valid ? 1 : 0, static_cast<unsigned long long>(list_hash), min_x, min_y, min_z,
            max_x, max_y, max_z);
        AppendDreamsModelBoundaryTrace(buffer, length);
    };

    if (guest_offset == DreamsModelBuildOffset) {
        state.build_sequence = build_sequence.fetch_add(1, std::memory_order_relaxed);
        state.model_id = static_cast<u32>(context->R8);
        state.target = static_cast<u32>(context->Rcx);
        state.csg_sequence = 0;
        state.csg_request_address = 0;
        state.first_csg_input_pending = false;
        state.active = DreamsModelBoundaryCaptureEnabled();
        if (state.active) {
            char buffer[384]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_boundary kind=build tick=%llu thread=%lu build=%u model=%u target=%u "
                "args=%u,%u,%u,%u source=0x%016llx caller=0x%016llx\r\n",
                static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                state.build_sequence, state.model_id, state.target,
                static_cast<u32>(context->Rdi), static_cast<u32>(context->Rdx),
                static_cast<u32>(context->R8), static_cast<u32>(context->R9),
                static_cast<unsigned long long>(context->Rsi),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp)));
            AppendDreamsModelBoundaryTrace(buffer, length);
        }
        // Emulate `push rbp`.
        context->Rsp -= sizeof(u64);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 1;
        return true;
    }

    if (guest_offset == DreamsModelBuildReturnOffset) {
        if (state.active) {
            char buffer[320]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_boundary kind=return tick=%llu thread=%lu build=%u model=%u target=%u "
                "csg=%u status=%u frame_target=%u\r\n",
                static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                state.build_sequence, state.model_id, state.target, state.csg_sequence,
                static_cast<u32>(context->Rax), ReadDreamsU32(process, context->Rsp + 0x9c));
            AppendDreamsModelBoundaryTrace(buffer, length);
        }
        state.active = false;
        // Emulate `mov rcx, qword ptr [r15]`.
        context->Rcx = ReadDreamsU64(process, context->R15);
        context->Rip = breakpoint_address + 3;
        return true;
    }

    if (guest_offset == DreamsCsgCallOffset) {
        if (state.active) {
            constexpr u32 ConfigWordsPerRecord = 0x60 / sizeof(u32);
            constexpr u32 MaxConfigRecordCount = 0x1d4c0;
            constexpr u32 MaxDumpedConfigRecords = 64;
            ++state.csg_sequence;
            state.csg_request_address = context->Rdi;
            state.first_csg_input_pending = true;
            const u32 config_record_count = static_cast<u32>(context->R8);
            const bool config_count_valid = config_record_count <= MaxConfigRecordCount;
            const u64 config_size =
                config_count_valid ? static_cast<u64>(config_record_count) * 0x60 : 0;
            const DreamsProcessHash config_hash =
                config_count_valid
                    ? HashDreamsProcessBytes(process, context->Rdx, config_size, config_size)
                    : DreamsProcessHash{};
            const u32 dumped_config_records =
                std::min(config_record_count, MaxDumpedConfigRecords);
            static thread_local std::array<u32,
                                           ConfigWordsPerRecord * MaxDumpedConfigRecords>
                config{};
            std::array<u32, 0x90 / sizeof(u32)> request{};
            const SIZE_T config_dump_size =
                static_cast<SIZE_T>(dumped_config_records) * 0x60;
            const bool config_dump_valid =
                config_dump_size == 0 ||
                ReadDreamsBytes(process, context->Rdx, config.data(), config_dump_size);
            const bool config_valid = config_count_valid && config_hash.valid &&
                                      config_hash.complete && config_dump_valid;
            const bool request_valid =
                ReadDreamsBytes(process, context->Rdi, request.data(), sizeof(request));
            const u64 request_hash =
                request_valid ? HashDreamsBytes(request.data(), sizeof(request)) : 0;
            char buffer[2048]{};
            int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_boundary kind=csg_call tick=%llu thread=%lu build=%u model=%u target=%u "
                "csg=%u request=0x%016llx config=0x%016llx rsi=%u r8=%u r9=%u flags=%u "
                "request_valid=%u request_hash=0x%016llx config_valid=%u "
                "config_records=%u config_bytes=%llu dumped_records=%u dump_truncated=%u "
                "config_over_limit=%u "
                "config_hash=0x%016llx request_words=",
                static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                state.build_sequence, state.model_id, state.target, state.csg_sequence,
                static_cast<unsigned long long>(context->Rdi),
                static_cast<unsigned long long>(context->Rdx), static_cast<u32>(context->Rsi),
                static_cast<u32>(context->R8), static_cast<u32>(context->R9),
                ReadDreamsU32(process, context->Rsp), request_valid ? 1 : 0,
                static_cast<unsigned long long>(request_hash), config_valid ? 1 : 0,
                config_record_count, static_cast<unsigned long long>(config_size),
                dumped_config_records,
                dumped_config_records != config_record_count ? 1 : 0,
                config_count_valid ? 0 : 1,
                static_cast<unsigned long long>(config_hash.hash));
            const auto append_words = [&](const std::span<const u32> words) noexcept {
                for (SIZE_T index = 0; index < words.size() && length > 0 &&
                                             static_cast<SIZE_T>(length) < sizeof(buffer);
                     ++index) {
                    const int appended = _snprintf_s(
                        buffer + length, sizeof(buffer) - static_cast<SIZE_T>(length), _TRUNCATE,
                        index + 1 == words.size() ? "%08x" : "%08x,", words[index]);
                    if (appended <= 0) {
                        length = -1;
                        break;
                    }
                    length += appended;
                }
            };
            append_words(request);
            if (length > 0 && static_cast<SIZE_T>(length) + 2 < sizeof(buffer)) {
                buffer[length++] = '\r';
                buffer[length++] = '\n';
            }
            AppendDreamsModelBoundaryTrace(buffer, length);

            if (config_dump_valid) {
                for (u32 record = 0; record < dumped_config_records; ++record) {
                    char record_buffer[768]{};
                    int record_length = _snprintf_s(
                        record_buffer, sizeof(record_buffer), _TRUNCATE,
                        "model_boundary kind=csg_edit tick=%llu thread=%lu build=%u model=%u "
                        "target=%u csg=%u record=%u hash=0x%016llx words=",
                        static_cast<unsigned long long>(GetTickCount64()),
                        GetCurrentThreadId(), state.build_sequence, state.model_id, state.target,
                        state.csg_sequence, record,
                        static_cast<unsigned long long>(HashDreamsBytes(
                            config.data() + record * ConfigWordsPerRecord, 0x60)));
                    for (u32 word = 0; word < ConfigWordsPerRecord && record_length > 0 &&
                                           static_cast<SIZE_T>(record_length) <
                                               sizeof(record_buffer);
                         ++word) {
                        const int appended = _snprintf_s(
                            record_buffer + record_length,
                            sizeof(record_buffer) - static_cast<SIZE_T>(record_length),
                            _TRUNCATE, word + 1 == ConfigWordsPerRecord ? "%08x" : "%08x,",
                            config[record * ConfigWordsPerRecord + word]);
                        record_length = appended > 0 ? record_length + appended : -1;
                    }
                    if (record_length > 0 &&
                        static_cast<SIZE_T>(record_length) + 2 < sizeof(record_buffer)) {
                        record_buffer[record_length++] = '\r';
                        record_buffer[record_length++] = '\n';
                    }
                    AppendDreamsModelBoundaryTrace(record_buffer, record_length);
                }
            }
        }
        // Emulate `call 0x1280290`.
        const u64 return_address = breakpoint_address + 5;
        context->Rsp -= sizeof(return_address);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &return_address,
                           sizeof(return_address), &bytes_written);
        context->Rip = base + 0x1280290;
        return true;
    }

    if (guest_offset == DreamsCsgInputDispatchOffset) {
        if (state.active && state.first_csg_input_pending) {
            constexpr u64 MaxInputCaptureBytes = 8ULL * 1024 * 1024;
            state.first_csg_input_pending = false;
            const u64 request_address = ReadDreamsU64(process, context->Rbp - 0x118);
            std::array<u32, 4> srd4{};
            std::array<u32, 4> srd8{};
            const bool srd4_valid =
                ReadDreamsBytes(process, context->Rbp - 0x50, srd4.data(), sizeof(srd4));
            const bool srd8_valid =
                ReadDreamsBytes(process, context->Rbp - 0x70, srd8.data(), sizeof(srd8));
            const auto decode_address = [&](const u32 low_word, const u32 high_word) noexcept {
                return static_cast<u64>(low_word) |
                       (static_cast<u64>(high_word & 0xfffu) << 32);
            };
            const u64 input4_address =
                srd4_valid ? decode_address(srd4[0], srd4[1]) : 0;
            const u64 input8_address =
                srd8_valid ? decode_address(srd8[0], srd8[1]) : 0;
            const u64 input4_size = srd4_valid ? static_cast<u64>(srd4[2]) * 4 : 0;
            const u64 input8_size = srd8_valid ? static_cast<u64>(srd8[2]) * 8 : 0;
            const DreamsProcessHash input4 = HashDreamsProcessBytes(
                process, input4_address, input4_size, MaxInputCaptureBytes);
            const DreamsProcessHash input8 = HashDreamsProcessBytes(
                process, input8_address, input8_size, MaxInputCaptureBytes);
            char buffer[1536]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_boundary kind=csg_input tick=%llu thread=%lu build=%u model=%u "
                "target=%u csg=%u stage=ChunkSoftBlendEvaluate request=0x%016llx "
                "call_request=0x%016llx request_match=%u "
                "srd4_valid=%u srd4=%08x,%08x,%08x,%08x input4=0x%016llx "
                "bytes=%llu captured=%llu cpu_backing_valid=%u complete=%u "
                "cpu_backing_hash=0x%016llx head=%08x,%08x,%08x,%08x "
                "srd8_valid=%u srd8=%08x,%08x,%08x,%08x input8=0x%016llx "
                "bytes=%llu captured=%llu cpu_backing_valid=%u complete=%u "
                "cpu_backing_hash=0x%016llx head=%08x,%08x,%08x,%08x\r\n",
                static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                state.build_sequence, state.model_id, state.target, state.csg_sequence,
                static_cast<unsigned long long>(request_address),
                static_cast<unsigned long long>(state.csg_request_address),
                request_address == state.csg_request_address ? 1 : 0,
                srd4_valid ? 1 : 0, srd4[0], srd4[1], srd4[2], srd4[3],
                static_cast<unsigned long long>(input4_address),
                static_cast<unsigned long long>(input4.requested_size),
                static_cast<unsigned long long>(input4.captured_size), input4.valid ? 1 : 0,
                input4.complete ? 1 : 0, static_cast<unsigned long long>(input4.hash),
                input4.head_words[0], input4.head_words[1], input4.head_words[2],
                input4.head_words[3], srd8_valid ? 1 : 0, srd8[0], srd8[1], srd8[2],
                srd8[3], static_cast<unsigned long long>(input8_address),
                static_cast<unsigned long long>(input8.requested_size),
                static_cast<unsigned long long>(input8.captured_size), input8.valid ? 1 : 0,
                input8.complete ? 1 : 0, static_cast<unsigned long long>(input8.hash),
                input8.head_words[0], input8.head_words[1], input8.head_words[2],
                input8.head_words[3]);
            AppendDreamsModelBoundaryTrace(buffer, length);
        }
        // Emulate `mov esi, r12d` immediately before the first CSG dispatch.
        context->Rsi = static_cast<u32>(context->R12);
        context->Rip = breakpoint_address + 3;
        return true;
    }

    if (guest_offset == DreamsCsgReplayResultOffset) {
        trace_snapshot("replay_result", context->R12,
                       ReadDreamsU32(process, context->Rbp - 0x160),
                       ReadDreamsU64(process, context->Rbp - 0xf8) != 0 ? 0 : 1);
        // Emulate `mov rdx, qword ptr [rbp - 0xf8]`.
        context->Rdx = ReadDreamsU64(process, context->Rbp - 0xf8);
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (guest_offset == DreamsCsgReplayCountOffset) {
        trace_snapshot("replay_count", context->R12, ReadDreamsU32(process, context->R12 + 8), 0);
        // Emulate `mov eax, dword ptr [r12 + 8]`.
        context->Rax = ReadDreamsU32(process, context->R12 + 8);
        context->Rip = breakpoint_address + 5;
        return true;
    }

    if (guest_offset == DreamsCsgReturnBranchOffset) {
        const u64 result = ReadDreamsU64(process, base + DreamsModelResultPointerOffset);
        trace_snapshot("csg_return", result, static_cast<u32>(context->Rax),
                       static_cast<u32>(context->Rax));
        // Emulate `jne 0x12850d3` using the flags from `test eax, eax`.
        const bool zero = (context->EFlags & (1u << 6)) != 0;
        context->Rip = zero ? breakpoint_address + 2 : base + 0x12850d3;
        return true;
    }

    trace_snapshot("aggregate", context->Rbx, static_cast<u32>(context->R15), 0);
    // Emulate `je 0x1283692`.
    const bool zero = (context->EFlags & (1u << 6)) != 0;
    context->Rip = zero ? base + 0x1283692 : breakpoint_address + 6;
    return true;
}

static bool HandleDreamsModelRecordTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsModelBuildOffset = 0x720950;
    constexpr u64 DreamsModelResetOffset = 0x7200b0;
    constexpr u64 DreamsModelInnerReturnBranchOffset = 0x721111;
    constexpr u64 DreamsModelInnerOutputGateOffset = 0x1271397;
    constexpr u64 DreamsModelBuildReturnOffset = 0x723870;
    constexpr u64 DreamsModelPublishOffset = 0x12676e0;
    constexpr u64 DreamsModelReplayResultOffset = 0x1281f5e;
    constexpr u64 DreamsModelEmptyResultBranchOffset = 0x1282390;
    constexpr u64 DreamsModelInvalidBoundsBranchOffset = 0x12823ae;
    constexpr u64 DreamsModelContextValidWriteOffset = 0x1282443;
    constexpr u64 DreamsModelContextValidGateOffset = 0x129217f;
    constexpr u64 DreamsModelTablePointerOffset = 0x97aafc8;
    constexpr u64 DreamsModelContextBaseOffset = 0x77ed7d0;
    constexpr u64 DreamsModelResultPointerOffset = 0x77ed7d8;
    constexpr u64 DreamsModelReadyFlagOffset = 0x77ed7c4;
    constexpr u64 DreamsModelContextStride = 0x20270;
    constexpr u64 DreamsModelContextValidOffset = 0x202f0;
    constexpr u64 DreamsModelRecordStride = 0x120;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 guest_offset = breakpoint_address - MemoryPatcher::g_eboot_address;
    if (guest_offset != DreamsModelBuildOffset && guest_offset != DreamsModelResetOffset &&
        guest_offset != DreamsModelInnerReturnBranchOffset &&
        guest_offset != DreamsModelInnerOutputGateOffset &&
        guest_offset != DreamsModelBuildReturnOffset &&
        guest_offset != DreamsModelPublishOffset && guest_offset != DreamsModelReplayResultOffset &&
        guest_offset != DreamsModelEmptyResultBranchOffset &&
        guest_offset != DreamsModelInvalidBoundsBranchOffset &&
        guest_offset != DreamsModelContextValidWriteOffset &&
        guest_offset != DreamsModelContextValidGateOffset) {
        return false;
    }

    CONTEXT* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u64 base = MemoryPatcher::g_eboot_address;
    const bool capture = DreamsModelRecordCaptureEnabled();
    const u64 table = ReadDreamsU64(process, base + DreamsModelTablePointerOffset);
    static thread_local u32 model_input_epoch{};

    const auto trace_result = [&](const char* kind, const u32 ordinal, const u64 result,
                                  const u32 context_index, const u32 extra) {
        if (!capture || ordinal >= 4096) {
            return;
        }
        const u64 context_base = base + DreamsModelContextBaseOffset;
        const u64 context_address =
            context_base + static_cast<u64>(context_index) * DreamsModelContextStride;
        char buffer[768]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "%s=%u tick=%llu thread=%lu context=%u valid=%u ready=%u result=0x%016llx "
            "extra=%u values=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
            "min=%u,%u,%u max=%u,%u,%u accum=%u,%u,%u,%u,%u,%u\r\n",
            kind, ordinal, static_cast<unsigned long long>(GetTickCount64()),
            GetCurrentThreadId(), context_index,
            ReadDreamsU8(process, context_address + DreamsModelContextValidOffset),
            ReadDreamsU8(process, base + DreamsModelReadyFlagOffset),
            static_cast<unsigned long long>(result), extra, ReadDreamsU32(process, result),
            ReadDreamsU32(process, result + 0x4), ReadDreamsU32(process, result + 0x8),
            ReadDreamsU32(process, result + 0xc), ReadDreamsU32(process, result + 0x18),
            ReadDreamsU32(process, result + 0x1c), ReadDreamsU32(process, result + 0x20),
            ReadDreamsU32(process, result + 0x24), ReadDreamsU32(process, result + 0x28),
            ReadDreamsU32(process, result + 0x48), ReadDreamsU32(process, result + 0x2c),
            ReadDreamsU32(process, result + 0x30), ReadDreamsU32(process, result + 0x34),
            ReadDreamsU32(process, result + 0x38), ReadDreamsU32(process, result + 0x3c),
            ReadDreamsU32(process, result + 0x40), ReadDreamsU32(process, result + 0x50),
            ReadDreamsU32(process, result + 0x54), ReadDreamsU32(process, result + 0x58),
            ReadDreamsU32(process, result + 0x5c), ReadDreamsU32(process, result + 0x60),
            ReadDreamsU32(process, result + 0x64));
        AppendDreamsCpuRootTrace(buffer, length);
    };

    const auto emulate_push_rbp = [&] {
        context->Rsp -= sizeof(u64);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 1;
    };

    if (guest_offset == DreamsModelBuildOffset) {
        static std::atomic<u32> build_count{0};
        const u32 ordinal = build_count.fetch_add(1, std::memory_order_relaxed);
        if (DreamsModelInputTraceEnabled()) {
            model_input_epoch = DreamsTrace::BeginModelBuild(
                GetCurrentThreadId(), static_cast<u32>(context->Rcx),
                static_cast<u32>(context->Rdi), static_cast<u32>(context->Rdx),
                static_cast<u32>(context->R8), static_cast<u32>(context->R9));
            const auto model = DreamsTrace::ReadModelBuild();
            char trace[320]{};
            const int trace_length = _snprintf_s(
                trace, sizeof(trace), _TRUNCATE,
                "model_input_begin epoch=%u tick=%llu thread=%lu rcx=%u rdi=%u rdx=%u "
                "model_id=%u peer_id=%u scratch=0x%016llx caller=0x%016llx\r\n",
                model.epoch, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(), model.arg_rcx, model.arg_rdi, model.arg_rdx, model.model_id,
                model.peer_id, static_cast<unsigned long long>(context->Rsi),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp)));
            AppendDreamsModelInputTrace(trace, trace_length);
        }
        ActivateDreamsCsgActionTrace(static_cast<u32>(context->Rcx));
        if (capture && ordinal < 4096) {
            char buffer[512]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_build=%u tick=%llu thread=%lu caller=0x%016llx target=%u "
                "source=0x%016llx args=%u,%u,%u,%u table=0x%016llx\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp)),
                static_cast<u32>(context->Rcx),
                static_cast<unsigned long long>(context->Rsi),
                static_cast<u32>(context->Rdi), static_cast<u32>(context->Rdx),
                static_cast<u32>(context->R8), static_cast<u32>(context->R9),
                static_cast<unsigned long long>(table));
            AppendDreamsCpuRootTrace(buffer, length);
        }
        emulate_push_rbp();
        return true;
    }

    if (guest_offset == DreamsModelResetOffset) {
        static std::atomic<u32> reset_count{0};
        const u32 ordinal = reset_count.fetch_add(1, std::memory_order_relaxed);
        if (capture && ordinal < 1024) {
            u32 ready_count = 0;
            u32 first_ready = 0xffffffff;
            if (table != 0) {
                for (u32 index = 0; index < 128; ++index) {
                    if (ReadDreamsU32(process, table + index * DreamsModelRecordStride + 0xc4) !=
                        0) {
                        ++ready_count;
                        if (first_ready == 0xffffffff) {
                            first_ready = index;
                        }
                    }
                }
            }
            char buffer[384]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_reset=%u tick=%llu thread=%lu caller=0x%016llx table=0x%016llx "
                "ready_0_127=%u first=%u i46=%08x,%08x,%08x\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp)),
                static_cast<unsigned long long>(table), ready_count, first_ready,
                table != 0 ? ReadDreamsU32(process, table + 0x46 * DreamsModelRecordStride) : 0,
                table != 0
                    ? ReadDreamsU32(process, table + 0x46 * DreamsModelRecordStride + 0xc4)
                    : 0,
                table != 0
                    ? ReadDreamsU32(process, table + 0x46 * DreamsModelRecordStride + 0xd0)
                    : 0);
            AppendDreamsCpuRootTrace(buffer, length);
        }
        emulate_push_rbp();
        return true;
    }

    if (guest_offset == DreamsModelPublishOffset) {
        static std::atomic<u32> publish_count{0};
        const u32 ordinal = publish_count.fetch_add(1, std::memory_order_relaxed);
        const u32 index = static_cast<u32>(context->Rdi);
        const u64 source = context->Rsi;
        const u64 destination = table != 0 ? table + index * DreamsModelRecordStride : 0;
        if (capture && ordinal < 4096) {
            char buffer[640]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_publish=%u tick=%llu thread=%lu caller=0x%016llx index=%u "
                "source=0x%016llx destination=0x%016llx "
                "src=%08x,%08x,%08x,%08x,%08x dst=%08x,%08x,%08x,%08x,%08x\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp)), index,
                static_cast<unsigned long long>(source),
                static_cast<unsigned long long>(destination), ReadDreamsU32(process, source),
                ReadDreamsU32(process, source + 4), ReadDreamsU32(process, source + 0xc0),
                ReadDreamsU32(process, source + 0xc4), ReadDreamsU32(process, source + 0xd0),
                ReadDreamsU32(process, destination), ReadDreamsU32(process, destination + 4),
                ReadDreamsU32(process, destination + 0xc0),
                ReadDreamsU32(process, destination + 0xc4),
                ReadDreamsU32(process, destination + 0xd0));
            AppendDreamsCpuRootTrace(buffer, length);
        }
        emulate_push_rbp();
        return true;
    }

    if (guest_offset == DreamsModelInnerReturnBranchOffset) {
        static std::atomic<u32> inner_return_count{0};
        const u32 ordinal = inner_return_count.fetch_add(1, std::memory_order_relaxed);
        const u32 context_index =
            static_cast<u32>(ReadDreamsU64(process, context->Rsp + 0x68));
        const u64 result = ReadDreamsU64(process, base + DreamsModelResultPointerOffset);
        trace_result("model_inner_return", ordinal, result, context_index,
                     static_cast<u32>(context->Rax));
        const bool zero = (context->EFlags & (1u << 6)) != 0;
        context->Rip = zero ? base + 0x723684 : breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsModelInnerOutputGateOffset) {
        static std::atomic<u32> inner_output_gate_count{0};
        const u32 ordinal = inner_output_gate_count.fetch_add(1, std::memory_order_relaxed);
        if (capture && ordinal < 4096) {
            const u64 output = context->R14;
            char buffer[640]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "model_inner_output_gate=%u tick=%llu thread=%lu output=0x%016llx "
                "count=%u companion=%u limit=%u start=%u current_chunk=%llu last_chunk=%llu "
                "input_work=%llu mode=%u\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()),
                GetCurrentThreadId(), static_cast<unsigned long long>(output),
                ReadDreamsU32(process, output), ReadDreamsU32(process, output + 4),
                ReadDreamsU32(process, output + 0xe8),
                ReadDreamsU32(process, context->Rsp + 0x18),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp + 0x10)),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp + 0x70)),
                static_cast<unsigned long long>(ReadDreamsU64(process, context->Rsp + 0x40)),
                static_cast<u32>(context->R13));
            AppendDreamsCpuRootTrace(buffer, length);
        }
        // Emulate `mov eax, r12d`.
        context->Rax = static_cast<u32>(context->R12);
        context->Rip = breakpoint_address + 3;
        return true;
    }

    if (guest_offset == DreamsModelReplayResultOffset) {
        static std::atomic<u32> replay_count{0};
        const u32 ordinal = replay_count.fetch_add(1, std::memory_order_relaxed);
        const u32 context_index = static_cast<u32>(
            ReadDreamsU64(process, context->Rbp - 0x178) / DreamsModelContextStride);
        const u64 result = context->R12;
        trace_result("model_replay_result", ordinal, result, context_index,
                     ReadDreamsU32(process, context->Rbp - 0x160));
        context->Rdx = ReadDreamsU64(process, context->Rbp - 0xf8);
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (guest_offset == DreamsModelEmptyResultBranchOffset) {
        static std::atomic<u32> empty_result_count{0};
        const u32 ordinal = empty_result_count.fetch_add(1, std::memory_order_relaxed);
        trace_result("model_aggregate_count", ordinal, context->Rbx,
                     static_cast<u32>(context->Rdi), static_cast<u32>(context->R15));
        const bool zero = (context->EFlags & (1u << 6)) != 0;
        context->Rip = zero ? base + 0x1283692 : breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsModelInvalidBoundsBranchOffset) {
        static std::atomic<u32> invalid_bounds_count{0};
        const u32 ordinal = invalid_bounds_count.fetch_add(1, std::memory_order_relaxed);
        trace_result("model_aggregate_bounds", ordinal, context->Rbx,
                     static_cast<u32>(context->Rdi), static_cast<u32>(context->Rdx & 7));
        const bool zero = (context->EFlags & (1u << 6)) != 0;
        context->Rip = zero ? breakpoint_address + 6 : base + 0x1283692;
        return true;
    }

    if (guest_offset == DreamsModelContextValidWriteOffset) {
        static std::atomic<u32> valid_write_count{0};
        const u32 ordinal = valid_write_count.fetch_add(1, std::memory_order_relaxed);
        const u64 valid_address =
            context->Rax + context->R14 + DreamsModelContextValidOffset;
        const u32 context_index = static_cast<u32>(context->Rax / DreamsModelContextStride);
        trace_result("model_context_valid", ordinal, context->Rbx, context_index, 1);
        const u8 one = 1;
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(valid_address), &one, sizeof(one),
                           &bytes_written);
        context->Rip = breakpoint_address + 9;
        return true;
    }

    if (guest_offset == DreamsModelContextValidGateOffset) {
        static std::atomic<u32> valid_gate_count{0};
        const u32 ordinal = valid_gate_count.fetch_add(1, std::memory_order_relaxed);
        const u32 context_index =
            static_cast<u32>(context->Rsi / DreamsModelContextStride);
        const u64 result = ReadDreamsU64(process, base + DreamsModelResultPointerOffset);
        trace_result("model_context_gate", ordinal, result, context_index,
                     (context->EFlags & (1u << 6)) != 0 ? 0 : 1);
        const bool zero = (context->EFlags & (1u << 6)) != 0;
        context->Rip = zero ? base + 0x1293b15 : breakpoint_address + 6;
        return true;
    }

    // `mov rcx, qword ptr [r15]` at the common return path. The aligned model-build frame keeps
    // the target index at rsp+0x9c and the candidate publication record at rsp+0xe0.
    static std::atomic<u32> return_count{0};
    const u32 ordinal = return_count.fetch_add(1, std::memory_order_relaxed);
    if (DreamsModelInputTraceEnabled()) {
        const bool matched = DreamsTrace::CompleteModelBuild(
            model_input_epoch, GetCurrentThreadId(), static_cast<u32>(context->Rax));
        const auto model = DreamsTrace::ReadModelBuild();
        char trace[256]{};
        const int trace_length = _snprintf_s(
            trace, sizeof(trace), _TRUNCATE,
            "model_input_return epoch=%u tick=%llu thread=%lu matched=%u rcx=%u "
            "frame_target=%u model_id=%u status=%u\r\n",
            model.epoch, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            matched, model.arg_rcx, ReadDreamsU32(process, context->Rsp + 0x9c), model.model_id,
            model.status);
        AppendDreamsModelInputTrace(trace, trace_length);
        model_input_epoch = 0;
    }
    if (capture && ordinal < 4096) {
        const u64 source = context->Rsp + 0xe0;
        const u32 index = ReadDreamsU32(process, context->Rsp + 0x9c);
        char buffer[576]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "model_return=%u tick=%llu thread=%lu status=%u target=%u aux=%u "
            "source=0x%016llx src=%08x,%08x,%08x,%08x,%08x "
            "dst_c4=%08x\r\n",
            ordinal, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            static_cast<u32>(context->Rax), index,
            ReadDreamsU32(process, context->Rsp + 0x78),
            static_cast<unsigned long long>(source), ReadDreamsU32(process, source),
            ReadDreamsU32(process, source + 4), ReadDreamsU32(process, source + 0xc0),
            ReadDreamsU32(process, source + 0xc4), ReadDreamsU32(process, source + 0xd0),
            table != 0 ? ReadDreamsU32(process, table + index * DreamsModelRecordStride + 0xc4)
                       : 0);
        AppendDreamsCpuRootTrace(buffer, length);
    }
    context->Rcx = ReadDreamsU64(process, context->R15);
    context->Rip = breakpoint_address + 3;
    return true;
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

struct DreamsSculptResourceScan {
    bool attempted{};
    bool complete{};
    bool unresolved{};
    bool invalid_chain{};
    u32 steps{};
    s32 index{-1};
    u64 object{};
    u32 id{};
    u32 header{};
    u16 primary{0xffff};
    u16 auxiliary{0xffff};
    u16 selected{0xffff};
    u8 selected_kind{0xff};
    u64 primary_writer{};
    u64 auxiliary_writer{};
    u32 type1_objects{};
    u32 handles{};
    u32 active_handles{};
    u32 resolved_payloads{};
    u32 missing_payloads{};
    s32 first_missing_payload_index{-1};
    u64 first_missing_payload_object{};
    u16 first_missing_payload_slot{0xffff};
    u8 first_missing_payload_kind{0xff};
    u8 first_missing_payload_active{};
    u32 first_missing_payload_value{};
    u64 first_missing_payload_writer{};
};

struct DreamsSculptProvenanceState {
    bool active{};
    bool tracked{};
    bool bootstrap_enabled{};
    bool empty_incremental_exit{};
    bool cache_committed{};
    bool first_complete{};
    bool second_started{};
    bool exit_seen{};
    bool hash_preflight_valid{};
    bool second_preflight_valid{};
    u64 invocation{};
    u64 manager{};
    u64 root{};
    u64 builder{};
    u64 output{};
    u64 root_identity{};
    u64 cached_identity{};
    u64 generation{};
    u64 table0_active{};
    u64 table1_active{};
    u64 committed_identity{};
    u64 first_emit_object{};
    u32 root_count{};
    u32 frame{};
    u32 records_before{};
    u32 emit_count{};
    u32 hash_active{};
    u32 hash_missing{};
    u32 hash_first_index{0xffffffffu};
    u32 hash_first_target{};
    u32 second_count{};
    u32 second_active{};
    u32 second_type1{};
    u32 second_sculpt_family{};
    u32 second_stroke{};
    u32 second_group_valid{};
    u32 second_group_invalid{};
    u32 second_type1_group_valid{};
    u32 second_first_type1_index{0xffffffffu};
    u32 prepare_calls{};
    u32 prepare_type1{};
    u32 prepare_returns{};
    u32 prepare_return_type1{};
    u32 output_gate_count{};
    u32 output_gate_null{};
    u32 type1_dispatch_count{};
    u32 resource_select_count{};
    u32 resource_invalid_slots{};
    u32 resource_active{};
    u32 resource_missing_payloads{};
    u32 resource_resolved_payloads{};
    u32 resource_model_ready{};
    u32 resource_model_unready{};
    u32 first_resource_payload{0xffffffffu};
    u32 first_resource_model_bits{};
    u32 record_gate_count{};
    s32 hash_first_work{};
    s32 second_first_type1_mapped{-1};
    u64 hash_first_object{};
    u64 second_first_type1_object{};
    u64 first_output_gate{};
    u64 first_resource_object{};
    u64 first_record_gate_object{};
    u64 first_record_gate_output{};
    s32 first_count{-1};
    u16 second_first_type1_group{0xffff};
    u16 first_resource_slot{0xffff};
    u8 phase{0xff};
    u8 ready{0xff};
    u8 ready_input{0xff};
    u8 second_first_type1_group_valid{0xff};
    u8 first_resource_active{0xff};
    u8 first_record_gate_guard{0xff};
    DreamsSculptResourceScan resource_scan{};
};

static thread_local DreamsSculptProvenanceState dreams_sculpt_provenance{};
static std::atomic<u64> dreams_sculpt_provenance_invocation{0};
static std::atomic<u32> dreams_sculpt_hash_preflight_count{0};
static std::atomic<u32> dreams_sculpt_second_preflight_count{0};
static std::atomic<u32> dreams_sculpt_resource_scan_count{0};
static std::atomic<u32> dreams_sculpt_summary_count{0};
static constexpr u32 DreamsSculptLifecycleTargetObjectId = 0xffff7886u;
static std::atomic<u32> dreams_sculpt_lifecycle_target_slot{0xffffffffu};
static std::atomic<u32> dreams_sculpt_resource_write_sequence{0};
static std::array<std::atomic<u64>, 1024> dreams_sculpt_resource_references{};
static std::array<std::atomic<u64>, 65536> dreams_sculpt_resource_last_write{};

static void MarkDreamsSculptResourceReference(const u16 slot) noexcept {
    if (slot == 0xffff) {
        return;
    }
    dreams_sculpt_resource_references[slot >> 6].fetch_or(1ull << (slot & 63),
                                                          std::memory_order_relaxed);
}

static bool IsDreamsSculptResourceReferenced(const u64 slot) noexcept {
    return slot < 65536 &&
           (dreams_sculpt_resource_references[slot >> 6].load(std::memory_order_relaxed) &
            (1ull << (slot & 63))) != 0;
}

static void RecordDreamsSculptResourceWrite(const u64 slot, const u8 kind,
                                            const u32 value) noexcept {
    // Provenance tracing discovers the relevant sculpt slot only after the builder has
    // attempted its preflight walk. Keep the latest write for every valid slot so that the
    // first unresolved/missing resource can still be tied to the write which preceded that
    // discovery. This state is diagnostic-only and is populated only while the provenance
    // breakpoints are enabled.
    if (slot >= dreams_sculpt_resource_last_write.size()) {
        return;
    }
    const u64 sequence = dreams_sculpt_resource_write_sequence.fetch_add(
                             1, std::memory_order_relaxed) &
                         0x0fffffffull;
    const u64 packed = (sequence << 36) | (static_cast<u64>(kind & 0xf) << 32) | value;
    dreams_sculpt_resource_last_write[slot].store(packed, std::memory_order_relaxed);
}

static void ScanDreamsFirstUnresolvedSculptResource(const HANDLE process, const u64 base,
                                                    DreamsSculptProvenanceState& state) noexcept {
    constexpr u64 DreamsFirstType1Offset = 0x279484;
    constexpr u64 DreamsNextType1Offset = 0x19397d0;
    constexpr u64 DreamsObjectPointersOffset = 0x10958c0;
    constexpr u64 DreamsSpecialResourceEnableOffset = 0x28ca538;
    constexpr u64 DreamsSpecialResource0Offset = 0x28ca4d0;
    constexpr u64 DreamsResourceTableOffset = 0x44d1730;

    auto& scan = state.resource_scan;
    scan.attempted = true;
    if (state.root == 0 || state.root_count == 0) {
        scan.invalid_chain = true;
        return;
    }

    s32 encoded = static_cast<s32>(ReadDreamsU32(process, state.root + DreamsFirstType1Offset));
    for (u32 step = 0; step <= state.root_count; ++step) {
        scan.steps = step;
        if (encoded == -1) {
            scan.complete = true;
            return;
        }

        // Dreams stores the object index in the signed low 22 bits of the packed link. Match
        // the guest's `shl 10; sar 10` sequence rather than shifting the packed value right.
        const s32 index = static_cast<s32>(static_cast<u32>(encoded) << 10) >> 10;
        if (index < 0 || static_cast<u32>(index) >= state.root_count) {
            scan.invalid_chain = true;
            scan.index = index;
            return;
        }

        const u64 object = ReadDreamsU64(
            process, state.root + DreamsObjectPointersOffset + static_cast<u64>(index) * 8);
        if (object != 0 && (ReadDreamsU32(process, object + 0xc) & 0x7f) == 1 &&
            ReadDreamsU32(process, object + 0x170) != 0xffffffffu) {
            ++scan.type1_objects;
            const u32 id = ReadDreamsU32(process, object);
            const u32 header = ReadDreamsU32(process, object + 0xc);
            const u16 primary = ReadDreamsU16(process, object + 0x40);
            const u16 auxiliary = ReadDreamsU16(process, object + 0x42);
            if (id == DreamsSculptLifecycleTargetObjectId && auxiliary != 0xffff) {
                dreams_sculpt_lifecycle_target_slot.store(auxiliary,
                                                          std::memory_order_relaxed);
            }
            MarkDreamsSculptResourceReference(primary);
            MarkDreamsSculptResourceReference(auxiliary);
            u64 selected_address = 0;
            u8 selected_kind = 0xff;

            if (ReadDreamsU8(process, base + DreamsSpecialResourceEnableOffset) != 0) {
                for (u8 special = 0; special < 4; ++special) {
                    const u64 special_address =
                        base + DreamsSpecialResource0Offset + static_cast<u64>(special) * 0x1c;
                    if (ReadDreamsU16(process, special_address) != 0xffff &&
                        (id == ReadDreamsU32(process, special_address - 8) ||
                         id == ReadDreamsU32(process, special_address - 4))) {
                        selected_address = special_address;
                        selected_kind = static_cast<u8>(2 + special);
                        break;
                    }
                }
            }

            if (selected_address == 0 && auxiliary != 0xffff) {
                const u64 resource_table = ReadDreamsU64(process, base + DreamsResourceTableOffset);
                if (resource_table != 0 &&
                    ReadDreamsU8(process, resource_table + static_cast<u64>(auxiliary) * 0x38 +
                                              0x1c) != 0) {
                    selected_address = object + 0x42;
                    selected_kind = 1;
                }
            }
            if (selected_address == 0) {
                selected_address = object + 0x40;
                selected_kind = 0;
            }

            const u16 selected = ReadDreamsU16(process, selected_address);
            if (selected == 0xffff) {
                scan.unresolved = true;
                scan.index = index;
                scan.object = object;
                scan.id = id;
                scan.header = header;
                scan.primary = primary;
                scan.auxiliary = auxiliary;
                scan.selected = selected;
                scan.selected_kind = selected_kind;
                scan.primary_writer =
                    primary != 0xffff
                        ? dreams_sculpt_resource_last_write[primary].load(std::memory_order_relaxed)
                        : 0;
                scan.auxiliary_writer =
                    auxiliary != 0xffff
                        ? dreams_sculpt_resource_last_write[auxiliary].load(
                              std::memory_order_relaxed)
                        : 0;
                return;
            }

            ++scan.handles;
            if (selected_kind <= 1) {
                const u64 resource_table =
                    ReadDreamsU64(process, base + DreamsResourceTableOffset);
                if (resource_table != 0) {
                    const u64 resource = resource_table + static_cast<u64>(selected) * 0x38;
                    const u8 active = ReadDreamsU8(process, resource + 0x1c);
                    const u32 payload = ReadDreamsU32(process, resource + 0x20);
                    scan.active_handles += active != 0 ? 1 : 0;
                    if (payload == 0xffffffffu) {
                        ++scan.missing_payloads;
                        if (scan.first_missing_payload_index < 0) {
                            scan.first_missing_payload_index = index;
                            scan.first_missing_payload_object = object;
                            scan.first_missing_payload_slot = selected;
                            scan.first_missing_payload_kind = selected_kind;
                            scan.first_missing_payload_active = active;
                            scan.first_missing_payload_value = payload;
                            scan.first_missing_payload_writer =
                                dreams_sculpt_resource_last_write[selected].load(
                                    std::memory_order_relaxed);
                        }
                    } else {
                        ++scan.resolved_payloads;
                    }
                }
            }
        }

        encoded = static_cast<s32>(ReadDreamsU32(
            process, state.root + DreamsNextType1Offset + static_cast<u64>(index) * 4));
    }
    scan.invalid_chain = true;
}

static void PreflightDreamsSculptHashTable(const HANDLE process, CONTEXT* context,
                                           DreamsSculptProvenanceState& state) noexcept {
    constexpr u32 MaxTableEntries = 4096;
    constexpr u32 MaxRootEntries = 16384;
    constexpr u64 WorkEntryStride = 0x1c;
    constexpr u64 TableEntryStride = 0x20;
    constexpr u64 DreamsObjectPointersOffset = 0x10958c0;
    static thread_local std::array<u8, MaxTableEntries * TableEntryStride> table_snapshot{};
    static thread_local std::array<u32, MaxTableEntries> sorted_keys{};
    static thread_local std::array<u8, MaxRootEntries * WorkEntryStride> work_snapshot{};

    if (state.builder == 0 || state.root == 0 || state.first_count <= 0 ||
        static_cast<u32>(state.first_count) > MaxRootEntries) {
        return;
    }

    const u64 table = ReadDreamsU64(process, state.builder + 0x244000);
    const u64 capacity = ReadDreamsU64(process, state.builder + 0x244008);
    if (table == 0 || capacity == 0 || capacity > MaxTableEntries) {
        return;
    }

    SIZE_T table_bytes_read = 0;
    const SIZE_T table_bytes = static_cast<SIZE_T>(capacity * TableEntryStride);
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(table), table_snapshot.data(),
                           table_bytes, &table_bytes_read) ||
        table_bytes_read != table_bytes) {
        return;
    }

    const SIZE_T work_bytes = static_cast<SIZE_T>(state.first_count) * WorkEntryStride;
    SIZE_T work_bytes_read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(context->Rsp + 0x24250),
                           work_snapshot.data(), work_bytes, &work_bytes_read) ||
        work_bytes_read != work_bytes) {
        return;
    }

    for (u64 slot = 0; slot < capacity; ++slot) {
        std::memcpy(&sorted_keys[slot], table_snapshot.data() + slot * TableEntryStride,
                    sizeof(u32));
    }
    std::sort(sorted_keys.begin(), sorted_keys.begin() + static_cast<size_t>(capacity));

    state.hash_preflight_valid = true;
    for (u32 index = 0; index < static_cast<u32>(state.first_count); ++index) {
        u32 target = 0;
        s32 work_count = 0;
        const u8* entry = work_snapshot.data() + static_cast<u64>(index) * WorkEntryStride;
        std::memcpy(&target, entry, sizeof(target));
        std::memcpy(&work_count, entry + sizeof(target), sizeof(work_count));
        if (work_count <= 0) {
            continue;
        }

        ++state.hash_active;
        if (!std::binary_search(sorted_keys.begin(),
                                sorted_keys.begin() + static_cast<size_t>(capacity), target)) {
            ++state.hash_missing;
            if (state.hash_first_index == 0xffffffffu) {
                state.hash_first_index = index;
                state.hash_first_target = target;
                state.hash_first_work = work_count;
                state.hash_first_object = ReadDreamsU64(
                    process, state.root + DreamsObjectPointersOffset + static_cast<u64>(index) * 8);
            }
        }
    }
}

static void PreflightDreamsSculptSecondPass(const HANDLE process, CONTEXT* context,
                                            DreamsSculptProvenanceState& state) noexcept {
    constexpr u32 MaxRootEntries = 16384;
    constexpr u64 DreamsObjectPointersOffset = 0x10958c0;
    constexpr u64 DreamsObjectGroupsOffset = 0x1ac7554;
    constexpr u64 DreamsObjectTypesOffset = 0x1acf558;
    constexpr u64 GroupMappingOffset = 0x2425c;
    constexpr u64 GroupMappingStride = 0x1c;
    constexpr u64 BuilderGroupStride = 0x78;
    constexpr u64 BuilderGroupValidOffset = 0x1cc049;
    static thread_local std::array<u8, MaxRootEntries> active_snapshot{};
    static thread_local std::array<u8, MaxRootEntries> type_snapshot{};
    static thread_local std::array<u16, MaxRootEntries> group_snapshot{};

    const u64 root = ReadDreamsU64(process, context->Rsp + 0xa0);
    const u32 count = ReadDreamsU32(process, context->Rsp + 0x40);
    if (root == 0 || root != state.root || state.builder == 0 || count < 2 ||
        count > MaxRootEntries) {
        return;
    }

    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(context->Rsp + 0x20248),
                           active_snapshot.data(), count, &bytes_read) ||
        bytes_read != count) {
        return;
    }
    if (!ReadProcessMemory(process,
                           reinterpret_cast<const void*>(root + DreamsObjectTypesOffset),
                           type_snapshot.data(), count, &bytes_read) ||
        bytes_read != count) {
        return;
    }
    const SIZE_T group_bytes = static_cast<SIZE_T>(count) * sizeof(u16);
    if (!ReadProcessMemory(process,
                           reinterpret_cast<const void*>(root + DreamsObjectGroupsOffset),
                           group_snapshot.data(), group_bytes, &bytes_read) ||
        bytes_read != group_bytes) {
        return;
    }

    state.second_preflight_valid = true;
    state.second_count = count;
    for (u32 index = 1; index < count; ++index) {
        if (active_snapshot[index] == 0) {
            continue;
        }

        ++state.second_active;
        const u8 type = type_snapshot[index];
        const bool exact_type1 = type == 1;
        if (exact_type1) {
            ++state.second_type1;
        }
        if ((type & 0x7f) == 1) {
            ++state.second_sculpt_family;
        }
        if (type == 0x11) {
            ++state.second_stroke;
        }

        const u16 group = group_snapshot[index];
        s32 mapped = -1;
        bytes_read = 0;
        const bool mapping_read =
            ReadProcessMemory(process,
                              reinterpret_cast<const void*>(context->Rsp + GroupMappingOffset +
                                                            static_cast<u64>(group) *
                                                                GroupMappingStride),
                              &mapped, sizeof(mapped), &bytes_read) &&
            bytes_read == sizeof(mapped);
        u8 group_valid = 0xff;
        bool validity_read = false;
        if (mapping_read && mapped >= 0 && static_cast<u32>(mapped) < MaxRootEntries) {
            bytes_read = 0;
            validity_read =
                ReadProcessMemory(process,
                                  reinterpret_cast<const void*>(
                                      state.builder + static_cast<u64>(mapped) * BuilderGroupStride +
                                      BuilderGroupValidOffset),
                                  &group_valid, sizeof(group_valid), &bytes_read) &&
                bytes_read == sizeof(group_valid);
        }
        const bool valid = validity_read && group_valid != 0;
        if (valid) {
            ++state.second_group_valid;
            if (exact_type1) {
                ++state.second_type1_group_valid;
            }
        } else {
            ++state.second_group_invalid;
        }

        if (exact_type1 && state.second_first_type1_index == 0xffffffffu) {
            state.second_first_type1_index = index;
            state.second_first_type1_object = ReadDreamsU64(
                process, root + DreamsObjectPointersOffset + static_cast<u64>(index) * 8);
            state.second_first_type1_group = group;
            state.second_first_type1_mapped = mapping_read ? mapped : -1;
            state.second_first_type1_group_valid = validity_read ? group_valid : 0xff;
        }
    }
}

static void ApplyDreamsCmp32Flags(CONTEXT* context, const u32 lhs, const u32 rhs) noexcept {
    constexpr u32 CarryFlag = 1u << 0;
    constexpr u32 ParityFlag = 1u << 2;
    constexpr u32 AuxiliaryCarryFlag = 1u << 4;
    constexpr u32 ZeroFlag = 1u << 6;
    constexpr u32 SignFlag = 1u << 7;
    constexpr u32 OverflowFlag = 1u << 11;
    constexpr u32 UpdatedFlags = CarryFlag | ParityFlag | AuxiliaryCarryFlag | ZeroFlag |
                                 SignFlag | OverflowFlag;
    const u32 result = lhs - rhs;
    u32 flags = context->EFlags & ~UpdatedFlags;
    if (lhs < rhs) {
        flags |= CarryFlag;
    }
    u8 parity = static_cast<u8>(result);
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;
    if ((parity & 1) == 0) {
        flags |= ParityFlag;
    }
    if ((lhs & 0xf) < (rhs & 0xf)) {
        flags |= AuxiliaryCarryFlag;
    }
    if (result == 0) {
        flags |= ZeroFlag;
    }
    if ((result & 0x80000000u) != 0) {
        flags |= SignFlag;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x80000000u) != 0) {
        flags |= OverflowFlag;
    }
    context->EFlags = flags;
}

static std::atomic<u32> dreams_sculpt_lifecycle_target_decisions{};
static std::atomic<u32> dreams_sculpt_lifecycle_watermark_writes{};
static std::atomic<u32> dreams_sculpt_lifecycle_active_writes{};
static std::atomic<u32> dreams_sculpt_lifecycle_reset_writes{};
static std::atomic<bool> dreams_sculpt_lifecycle_target_active{};
static std::atomic<bool> dreams_sculpt_lifecycle_restored{};

static void RestoreDreamsSculptLifecycleBreakpoints(const HANDLE process, const u64 base) noexcept {
    if (dreams_sculpt_lifecycle_restored.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    constexpr std::array<std::pair<u64, u8>, 7> Points{{
        {0x9c47cc, 0x3b},
        {0x9c47d4, 0x66},
        {0x9c486a, 0x66},
        {0x9aaf51, 0x89},
        {0x71cb3a, 0x43},
        {0x71e626, 0x41},
        {0x72a9e9, 0x86},
    }};
    for (const auto [offset, byte] : Points) {
        void* address = reinterpret_cast<void*>(base + offset);
        SIZE_T written = 0;
        WriteProcessMemory(process, address, &byte, sizeof(byte), &written);
        FlushInstructionCache(process, address, sizeof(byte));
    }
}

static bool HandleDreamsSculptLifecycleTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 PromotionCompareOffset = 0x9c47cc;
    constexpr u64 PromotionCommitOffset = 0x9c47d4;
    constexpr u64 PendingAssignmentOffset = 0x9c486a;
    constexpr u64 WatermarkWriteOffset = 0x9aaf51;
    constexpr u64 ActiveWriteOffset = 0x71cb3a;
    constexpr u64 AlternateActiveWriteOffset = 0x71e626;
    constexpr u64 ResetWriteOffset = 0x72a9e9;
    constexpr u64 ResourceTableOffset = 0x44d1730;
    constexpr u64 ResourceFloorOffset = 0x44d4b40;
    constexpr u64 ResourceCurrentOffset = 0x44d4b44;
    constexpr u64 ResourceWatermarkOffset = 0x44d4b48;
    constexpr u64 ActiveSceneRootHolderOffset = 0x4555e60;
    constexpr u32 MaxWatermarkLogs = 16;
    constexpr u32 MaxResetLogs = 64;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }
    const u64 base = MemoryPatcher::g_eboot_address;
    const u64 address = reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 offset = address - base;
    if (offset != PromotionCompareOffset && offset != PromotionCommitOffset &&
        offset != PendingAssignmentOffset &&
        offset != WatermarkWriteOffset && offset != ActiveWriteOffset &&
        offset != AlternateActiveWriteOffset && offset != ResetWriteOffset) {
        return false;
    }

    CONTEXT* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    const u64 caller = ReadDreamsU64(process, context->Rsp);
    const u32 floor = ReadDreamsU32(process, base + ResourceFloorOffset);
    const u32 current = ReadDreamsU32(process, base + ResourceCurrentOffset);
    const u32 watermark = ReadDreamsU32(process, base + ResourceWatermarkOffset);

    if (offset == PendingAssignmentOffset) {
        const u64 object = context->R14;
        const u32 id = object != 0 ? ReadDreamsU32(process, object) : 0;
        const u16 previous = object != 0 ? ReadDreamsU16(process, object + 0x42) : 0xffff;
        const u16 slot = static_cast<u16>(context->Rax);
        if (id == DreamsSculptLifecycleTargetObjectId) {
            dreams_sculpt_lifecycle_target_slot.store(slot, std::memory_order_relaxed);
            const u64 root_holder = ReadDreamsU64(process, base + ActiveSceneRootHolderOffset);
            const u64 root = root_holder != 0 ? ReadDreamsU64(process, root_holder) : 0;
            char buffer[640]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "sculpt_lifecycle kind=pending tick=%llu thread=%lu root=0x%016llx "
                "object=0x%016llx id=0x%08x auxiliary=%u->%u resources=%u,%u,%u "
                "context=0x%016llx,0x%016llx,0x%016llx caller=0x%016llx "
                "caller_offset=0x%016llx\r\n",
                static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                static_cast<unsigned long long>(root),
                static_cast<unsigned long long>(object), id, previous, slot, floor, current,
                watermark, static_cast<unsigned long long>(context->R13),
                static_cast<unsigned long long>(context->R14),
                static_cast<unsigned long long>(context->R15),
                static_cast<unsigned long long>(caller),
                caller >= base ? static_cast<unsigned long long>(caller - base) : 0);
            AppendDreamsSculptProvenanceTrace(buffer, length);
        }
        SIZE_T written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(object + 0x42), &slot, sizeof(slot),
                           &written);
        context->Rip = address + 5;
        return true;
    }

    if (offset == ResetWriteOffset) {
        const u64 table = context->Rdi;
        const u64 entry_offset = context->Rax;
        const u64 target = table + entry_offset + 0x1c;
        const bool aligned = entry_offset % 0x38 == 0;
        const u64 slot = aligned ? entry_offset / 0x38 : ~0ull;
        const u8 replacement = static_cast<u8>(context->Rdx);
        const u8 previous = static_cast<u8>(InterlockedExchange8(
            reinterpret_cast<volatile char*>(target), static_cast<char>(replacement)));
        const u32 ordinal =
            dreams_sculpt_lifecycle_reset_writes.fetch_add(1, std::memory_order_relaxed);
        const u32 target_slot =
            dreams_sculpt_lifecycle_target_slot.load(std::memory_order_relaxed);
        if (aligned && (ordinal < MaxResetLogs || slot == target_slot)) {
            char buffer[512]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "sculpt_lifecycle kind=reset ordinal=%u tick=%llu thread=%lu "
                "table=0x%016llx entry=0x%016llx target=0x%016llx slot=%llu "
                "value=%u->%u resources=%u,%u,%u caller=0x%016llx "
                "caller_offset=0x%016llx\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                static_cast<unsigned long long>(table),
                static_cast<unsigned long long>(entry_offset),
                static_cast<unsigned long long>(target), static_cast<unsigned long long>(slot),
                previous, replacement, floor, current, watermark,
                static_cast<unsigned long long>(caller),
                caller >= base ? static_cast<unsigned long long>(caller - base) : 0);
            AppendDreamsSculptProvenanceTrace(buffer, length);
        }
        context->Rdx = (context->Rdx & ~0xffull) | previous;
        context->Rip = address + 4;
        if (aligned && slot == target_slot && previous != 0 && replacement == 0 &&
            dreams_sculpt_lifecycle_target_active.load(std::memory_order_relaxed)) {
            RestoreDreamsSculptLifecycleBreakpoints(process, base);
        }
        return true;
    }

    if (offset == ActiveWriteOffset || offset == AlternateActiveWriteOffset) {
        const u64 table = offset == ActiveWriteOffset ? context->R11 : context->R8;
        const u64 entry_offset = offset == ActiveWriteOffset ? context->R15 : context->Rbx;
        const u64 target = table + entry_offset + 0x1c;
        const bool aligned = entry_offset % 0x38 == 0;
        const u64 slot = aligned ? entry_offset / 0x38 : ~0ull;
        const u8 replacement = static_cast<u8>(context->Rax);
        const u8 previous = static_cast<u8>(InterlockedExchange8(
            reinterpret_cast<volatile char*>(target), static_cast<char>(replacement)));
        const u32 target_slot =
            dreams_sculpt_lifecycle_target_slot.load(std::memory_order_relaxed);
        if (aligned && (slot == target_slot || target_slot == 0xffffffffu)) {
            const u32 ordinal =
                dreams_sculpt_lifecycle_active_writes.fetch_add(1, std::memory_order_relaxed);
            const u64 root_holder = ReadDreamsU64(process, base + ActiveSceneRootHolderOffset);
            const u64 root = root_holder != 0 ? ReadDreamsU64(process, root_holder) : 0;
            char buffer[768]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "sculpt_lifecycle kind=active ordinal=%u tick=%llu thread=%lu "
                "site=0x%08llx root=0x%016llx table=0x%016llx entry=0x%016llx "
                "target=0x%016llx slot=%llu value=%u->%u resources=%u,%u,%u "
                "context=0x%016llx,0x%016llx,0x%016llx caller=0x%016llx "
                "caller_offset=0x%016llx\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(root),
                static_cast<unsigned long long>(table),
                static_cast<unsigned long long>(entry_offset),
                static_cast<unsigned long long>(target), static_cast<unsigned long long>(slot),
                previous, replacement, floor, current, watermark,
                static_cast<unsigned long long>(context->R13),
                static_cast<unsigned long long>(context->R14),
                static_cast<unsigned long long>(context->R15),
                static_cast<unsigned long long>(caller),
                caller >= base ? static_cast<unsigned long long>(caller - base) : 0);
            AppendDreamsSculptProvenanceTrace(buffer, length);
            if (slot == target_slot && replacement != 0) {
                dreams_sculpt_lifecycle_target_active.store(true, std::memory_order_relaxed);
                // The target can complete tens of seconds after the initial builder attempts.
                // Re-arm the bounded summaries so the next invocation records whether the
                // newly active auxiliary handle is selected and the scene cache commits.
                dreams_sculpt_resource_scan_count.store(0, std::memory_order_relaxed);
                dreams_sculpt_summary_count.store(0, std::memory_order_relaxed);
            }
        }

        context->Rax = (context->Rax & ~0xffull) | previous;
        context->Rip = address + 5;
        return true;
    }

    if (offset == WatermarkWriteOffset) {
        const u32 value = static_cast<u32>(context->Rax);
        const u32 ordinal =
            dreams_sculpt_lifecycle_watermark_writes.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < MaxWatermarkLogs) {
            const u64 root_holder = ReadDreamsU64(process, base + ActiveSceneRootHolderOffset);
            const u64 root = root_holder != 0 ? ReadDreamsU64(process, root_holder) : 0;
            char buffer[512]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "sculpt_lifecycle kind=watermark ordinal=%u tick=%llu thread=%lu "
                "root=0x%016llx caller=0x%016llx caller_offset=0x%016llx "
                "resources=%u,%u,%u value=%u\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                static_cast<unsigned long long>(root),
                static_cast<unsigned long long>(caller),
                caller >= base ? static_cast<unsigned long long>(caller - base) : 0, floor,
                current, watermark, value);
            AppendDreamsSculptProvenanceTrace(buffer, length);
        }
        SIZE_T written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(base + ResourceWatermarkOffset),
                           &value, sizeof(value), &written);
        context->Rip = address + 6;
        return true;
    }

    const u64 object = context->R14;
    const u32 id = object != 0 ? ReadDreamsU32(process, object) : 0;
    if (offset == PromotionCompareOffset) {
        const u32 rank = static_cast<u32>(context->Rdx);
        const u16 primary = object != 0 ? ReadDreamsU16(process, object + 0x40) : 0xffff;
        const u16 auxiliary = object != 0 ? ReadDreamsU16(process, object + 0x42) : 0xffff;
        u8 active = 0xff;
        u32 payload = 0xffffffffu;
        const u64 table = ReadDreamsU64(process, base + ResourceTableOffset);
        if (table != 0 && auxiliary != 0xffff) {
            active = ReadDreamsU8(process, table + static_cast<u64>(auxiliary) * 0x38 + 0x1c);
            payload = ReadDreamsU32(process,
                                    table + static_cast<u64>(auxiliary) * 0x38 + 0x20);
        }
        if (id == DreamsSculptLifecycleTargetObjectId) {
            dreams_sculpt_lifecycle_target_slot.store(auxiliary, std::memory_order_relaxed);
            const u32 ordinal = dreams_sculpt_lifecycle_target_decisions.fetch_add(
                1, std::memory_order_relaxed);
            char buffer[640]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "sculpt_lifecycle kind=compare ordinal=%u tick=%llu thread=%lu "
                "root=0x%016llx object=0x%016llx id=0x%08x slots=%u,%u "
                "rank=%u active=%u payload=0x%08x resources=%u,%u,%u "
                "promote=%u caller=0x%016llx caller_offset=0x%016llx\r\n",
                ordinal, static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
                static_cast<unsigned long long>(context->R15),
                static_cast<unsigned long long>(object), id, primary, auxiliary, rank, active,
                payload, floor, current, watermark, rank < watermark ? 1u : 0u,
                static_cast<unsigned long long>(caller),
                caller >= base ? static_cast<unsigned long long>(caller - base) : 0);
            AppendDreamsSculptProvenanceTrace(buffer, length);
        }
        ApplyDreamsCmp32Flags(context, rank, watermark);
        context->Rip = address + 6;
        return true;
    }

    // Emulate the replaced `mov word ptr [r14 + 0x40], cx`. The following guest instruction
    // still clears the auxiliary slot normally.
    const u16 slot = static_cast<u16>(context->Rcx);
    if (id == DreamsSculptLifecycleTargetObjectId) {
        char buffer[512]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "sculpt_lifecycle kind=promote tick=%llu thread=%lu root=0x%016llx "
            "object=0x%016llx id=0x%08x slot=%u resources=%u,%u,%u "
            "caller=0x%016llx caller_offset=0x%016llx\r\n",
            static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            static_cast<unsigned long long>(context->R15),
            static_cast<unsigned long long>(object), id, slot, floor, current, watermark,
            static_cast<unsigned long long>(caller),
            caller >= base ? static_cast<unsigned long long>(caller - base) : 0);
        AppendDreamsSculptProvenanceTrace(buffer, length);
        RestoreDreamsSculptLifecycleBreakpoints(process, base);
    }
    SIZE_T written = 0;
    WriteProcessMemory(process, reinterpret_cast<void*>(object + 0x40), &slot, sizeof(slot),
                       &written);
    context->Rip = address + 5;
    return true;
}

static bool HandleDreamsSculptProvenanceTrace(EXCEPTION_POINTERS* exception) noexcept {
    constexpr u64 DreamsBuilderOffset = 0x8b7380;
    constexpr u64 DreamsEmptyIncrementalExitOffset = 0x8b7511;
    constexpr u64 DreamsBuilderExitOffset = 0x8b751b;
    constexpr u64 DreamsCacheCommitOffset = 0x8b8113;
    constexpr u64 DreamsFirstCompleteOffset = 0x8b851f;
    constexpr u64 DreamsSecondStartOffset = 0x8b8860;
    constexpr u64 DreamsObjectPrepareCallOffset = 0x8b8a22;
    constexpr u64 DreamsObjectPrepareReturnOffset = 0x8b8a31;
    constexpr u64 DreamsOutputGateOffset = 0x8b8a67;
    constexpr u64 DreamsType1DispatchOffset = 0x8b8cd5;
    constexpr u64 DreamsResourceSelectOffset = 0x8b95df;
    constexpr u64 DreamsRecordGateOffset = 0x8b9867;
    constexpr u64 DreamsRecordEmitOffset = 0x8ba0a9;
    constexpr u64 DreamsPostBuilderOffset = 0x987998;
    constexpr u64 DreamsResourceAuxAssignOffset = 0x71c622;
    constexpr u64 DreamsResourcePrimaryAssignOffset = 0x71c8b8;
    constexpr u64 DreamsResourceAssignOffset = 0x71cd98;
    constexpr u64 DreamsResourceCreateOffset = 0x72aa3e;
    constexpr u64 DreamsResourceRetireOffset = 0x729da5;
    constexpr u64 DreamsBuilderGlobalOffset = 0x4550d98;
    constexpr u64 DreamsOutputGlobalOffset = 0x6667e68;
    constexpr u64 DreamsFrameTagOffset = 0x789687c;

    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr || MemoryPatcher::g_eboot_address == 0) {
        return false;
    }

    const u64 breakpoint_address =
        reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 base = MemoryPatcher::g_eboot_address;
    const u64 guest_offset = breakpoint_address - base;
    if (guest_offset != DreamsBuilderOffset &&
        guest_offset != DreamsEmptyIncrementalExitOffset &&
        guest_offset != DreamsBuilderExitOffset && guest_offset != DreamsCacheCommitOffset &&
        guest_offset != DreamsFirstCompleteOffset && guest_offset != DreamsSecondStartOffset &&
        guest_offset != DreamsObjectPrepareCallOffset &&
        guest_offset != DreamsObjectPrepareReturnOffset && guest_offset != DreamsOutputGateOffset &&
        guest_offset != DreamsType1DispatchOffset && guest_offset != DreamsResourceSelectOffset &&
        guest_offset != DreamsRecordGateOffset &&
        guest_offset != DreamsRecordEmitOffset && guest_offset != DreamsPostBuilderOffset &&
        guest_offset != DreamsResourceAuxAssignOffset &&
        guest_offset != DreamsResourcePrimaryAssignOffset &&
        guest_offset != DreamsResourceAssignOffset && guest_offset != DreamsResourceCreateOffset &&
        guest_offset != DreamsResourceRetireOffset) {
        return false;
    }

    CONTEXT* context = exception->ContextRecord;
    const HANDLE process = GetCurrentProcess();
    auto& state = dreams_sculpt_provenance;

    if (guest_offset == DreamsResourceAuxAssignOffset ||
        guest_offset == DreamsResourcePrimaryAssignOffset ||
        guest_offset == DreamsResourceAssignOffset || guest_offset == DreamsResourceCreateOffset ||
        guest_offset == DreamsResourceRetireOffset) {
        u64 target = 0;
        u32 value = 0;
        u64 instruction_size = 0;
        u8 kind = 0;
        if (guest_offset == DreamsResourceAuxAssignOffset) {
            target = context->R13 + 0x1c;
            value = static_cast<u32>(context->Rax);
            instruction_size = 4;
            kind = 1;
        } else if (guest_offset == DreamsResourcePrimaryAssignOffset) {
            target = context->R13;
            value = static_cast<u32>(context->Rax);
            instruction_size = 4;
            kind = 2;
        } else if (guest_offset == DreamsResourceAssignOffset) {
            target = context->R15 + context->R14 + 0x20;
            value = static_cast<u32>(context->Rax);
            instruction_size = 5;
            kind = 3;
        } else if (guest_offset == DreamsResourceCreateOffset) {
            target = context->Rdi + context->Rax + 0x20;
            value = static_cast<u32>(context->Rdx);
            instruction_size = 4;
            kind = 4;
        } else {
            target = context->R10;
            value = 0xffffffffu;
            instruction_size = 7;
            kind = 5;
        }

        const u64 resource_table = ReadDreamsU64(process, base + 0x44d1730);
        u64 slot = ~0ull;
        if (resource_table != 0 && target >= resource_table + 0x20 &&
            (target - resource_table - 0x20) % 0x38 == 0) {
            slot = (target - resource_table - 0x20) / 0x38;
        }
        RecordDreamsSculptResourceWrite(slot, kind, value);

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(target), &value, sizeof(value),
                           &bytes_written);
        context->Rip = breakpoint_address + instruction_size;
        return true;
    }

    if (guest_offset == DreamsBuilderOffset) {
        state = {};
        state.active = true;
        state.manager = context->Rdi;
        state.root = context->Rdi != 0 ? ReadDreamsU64(process, context->Rdi + 0x10) : 0;
        state.root_count = ReadDreamsRootCount(process, state.root);
        state.tracked = state.root_count >= 4000;
        if (state.tracked) {
            state.invocation =
                dreams_sculpt_provenance_invocation.fetch_add(1, std::memory_order_relaxed);
            state.builder = ReadDreamsU64(process, base + DreamsBuilderGlobalOffset);
            state.output = ReadDreamsU64(process, base + DreamsOutputGlobalOffset);
            state.frame = ReadDreamsU32(process, base + DreamsFrameTagOffset);
            state.root_identity = ReadDreamsU64(process, state.root + 0x1c2bf90);
            state.cached_identity = ReadDreamsU64(process, state.builder + 0x1cbfc8);
            state.generation = ReadDreamsU64(process, state.root + 0x27b0c0);
            state.table0_active = ReadDreamsU64(process, state.builder + 0x243ff0);
            state.table1_active = ReadDreamsU64(process, state.builder + 0x244010);
            state.phase = ReadDreamsU8(process, state.root + 0x27b099);
            state.ready = ReadDreamsU8(process, state.root + 0x27b0a9);
            state.ready_input = ReadDreamsU8(process, state.root + 0x27b0b0);
            state.records_before =
                state.output != 0 ? ReadDreamsU32(process, state.output + 0x31ff38) : 0;
            char retry_value[2]{};
            state.bootstrap_enabled =
                GetEnvironmentVariableA("SHADPS4_DREAMS_SCENE_READY_HANDOFF", retry_value,
                                        sizeof(retry_value)) != 0 &&
                retry_value[0] == '1';
            if (!state.bootstrap_enabled) {
                state.bootstrap_enabled =
                    GetEnvironmentVariableA("SHADPS4_DREAMS_SCENE_CACHE_BOOTSTRAP_ONLY",
                                            retry_value, sizeof(retry_value)) != 0 &&
                    retry_value[0] == '1';
            }
        }

        context->Rsp -= sizeof(u64);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &context->Rbp,
                           sizeof(u64), &bytes_written);
        context->Rip = breakpoint_address + 1;
        return true;
    }

    if (guest_offset == DreamsEmptyIncrementalExitOffset) {
        if (state.active && state.tracked) {
            state.empty_incremental_exit = true;
        }
        context->Rip = base + DreamsBuilderExitOffset;
        return true;
    }

    if (guest_offset == DreamsBuilderExitOffset) {
        if (state.active && state.tracked) {
            state.exit_seen = true;
        }
        context->Rax = ReadDreamsU64(process, base + 0x1c56cb8);
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (guest_offset == DreamsCacheCommitOffset) {
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->R15 + 0x1cbfc8),
                           &context->Rax, sizeof(context->Rax), &bytes_written);
        if (state.active && state.tracked) {
            state.cache_committed = true;
            state.committed_identity = context->Rax;
        }
        context->Rip = breakpoint_address + 7;
        return true;
    }

    if (guest_offset == DreamsFirstCompleteOffset) {
        const s32 count = static_cast<s32>(ReadDreamsU32(process, context->Rsp + 0x40));
        if (state.active && state.tracked) {
            state.first_complete = true;
            state.first_count = count;
            const u32 preflight =
                dreams_sculpt_hash_preflight_count.fetch_add(1, std::memory_order_relaxed);
            if (preflight < 4) {
                PreflightDreamsSculptHashTable(process, context, state);
            }
        }
        context->Rip = base + (count <= 0 ? 0x8b875b : 0x8b852a);
        return true;
    }

    if (guest_offset == DreamsSecondStartOffset) {
        if (state.active && state.tracked) {
            state.second_started = true;
            const u32 preflight =
                dreams_sculpt_second_preflight_count.fetch_add(1, std::memory_order_relaxed);
            if (preflight < 4) {
                PreflightDreamsSculptSecondPass(process, context, state);
            }
        }
        context->R12 = 1;
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsObjectPrepareCallOffset) {
        const u8 type = ReadDreamsU8(process, context->Rsp + 0xc0);
        if (state.active && state.tracked) {
            ++state.prepare_calls;
            state.prepare_type1 += type == 1 ? 1 : 0;
        }
        context->Rdx = context->R14;
        context->Rip = breakpoint_address + 3;
        return true;
    }

    if (guest_offset == DreamsObjectPrepareReturnOffset) {
        const u64 root = ReadDreamsU64(process, context->Rsp + 0xa0);
        const u8 type = ReadDreamsU8(process, context->Rsp + 0xc0);
        if (state.active && state.tracked) {
            ++state.prepare_returns;
            state.prepare_return_type1 += type == 1 ? 1 : 0;
        }
        context->Rdi = root;
        context->Rip = base + (type == 1 ? 0x8b8a47 : 0x8b8870);
        return true;
    }

    if (guest_offset == DreamsOutputGateOffset) {
        const u64 output = ReadDreamsU64(process, base + DreamsOutputGlobalOffset);
        if (state.active && state.tracked) {
            if (state.output_gate_count == 0) {
                state.first_output_gate = output;
            }
            ++state.output_gate_count;
            state.output_gate_null += output == 0 ? 1 : 0;
        }
        context->Rip = base + (output == 0 ? 0x8bb997 : 0x8b8a75);
        return true;
    }

    if (guest_offset == DreamsType1DispatchOffset) {
        const u64 object = ReadDreamsU64(
            process, context->R10 + 0x10958c0 + static_cast<u64>(context->R14) * 8);
        if (state.active && state.tracked) {
            ++state.type1_dispatch_count;
        }
        context->Rbx = object;
        context->Rip = breakpoint_address + 8;
        return true;
    }

    if (guest_offset == DreamsResourceSelectOffset) {
        const u16 slot = ReadDreamsU16(process, context->Rax);
        u8 active = 0xff;
        u32 payload = 0xffffffffu;
        u32 model_bits = 0;
        bool model_value_read = false;
        if (slot != 0xffff) {
            MarkDreamsSculptResourceReference(slot);
            const u64 resource_table = ReadDreamsU64(process, base + 0x44d1730);
            if (resource_table != 0) {
                const u64 resource = resource_table + static_cast<u64>(slot) * 0x38;
                active = ReadDreamsU8(process, resource + 0x1c);
                payload = ReadDreamsU32(process, resource + 0x20);
                if (payload != 0xffffffffu) {
                    const u64 model_table = ReadDreamsU64(process, base + 0x97aafc8);
                    if (model_table != 0) {
                        SIZE_T bytes_read = 0;
                        model_value_read =
                            ReadProcessMemory(process,
                                              reinterpret_cast<const void*>(
                                                  model_table +
                                                  static_cast<u64>(payload) * 0x120 + 0xc4),
                                              &model_bits, sizeof(model_bits), &bytes_read) &&
                            bytes_read == sizeof(model_bits);
                    }
                }
            }
        }
        if (state.active && state.tracked) {
            if (state.resource_select_count == 0) {
                state.first_resource_object = context->Rbx;
                state.first_resource_slot = slot;
                state.first_resource_active = active;
                state.first_resource_payload = payload;
                state.first_resource_model_bits = model_bits;
            }
            ++state.resource_select_count;
            state.resource_invalid_slots += slot == 0xffff ? 1 : 0;
            state.resource_active += active != 0 && active != 0xff ? 1 : 0;
            state.resource_missing_payloads +=
                slot != 0xffff && payload == 0xffffffffu ? 1 : 0;
            state.resource_resolved_payloads +=
                slot != 0xffff && payload != 0xffffffffu ? 1 : 0;
            state.resource_model_ready +=
                model_value_read && (model_bits & 0x7fffffffu) != 0 ? 1 : 0;
            state.resource_model_unready +=
                model_value_read && (model_bits & 0x7fffffffu) == 0 ? 1 : 0;
        }
        context->R11 = slot;
        context->Rip = breakpoint_address + 4;
        return true;
    }

    if (guest_offset == DreamsRecordGateOffset) {
        const u64 output = ReadDreamsU64(process, base + DreamsOutputGlobalOffset);
        const u8 guard = ReadDreamsU8(process, base + 0x6667e70);
        if (state.active && state.tracked) {
            if (state.record_gate_count == 0) {
                state.first_record_gate_object = context->Rbx;
                state.first_record_gate_output = output;
                state.first_record_gate_guard = guard;
            }
            ++state.record_gate_count;
        }
        context->Rax = (context->Rax & ~0xffull) | guard;
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (guest_offset == DreamsRecordEmitOffset) {
        const u64 count_address = context->R15 + 0x31ff38;
        const u32 previous = ReadDreamsU32(process, count_address);
        const u32 result = previous + 1;
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(count_address), &result,
                           sizeof(result), &bytes_written);
        ApplyDreamsIncFlags(context, previous, result);
        if (state.active && state.tracked) {
            if (state.emit_count == 0) {
                state.first_emit_object = context->Rbx;
            }
            ++state.emit_count;
        }
        context->Rip = breakpoint_address + 7;
        return true;
    }

    const u64 post_root = ReadDreamsU64(process, context->Rsp + 0x650);
    if (state.active && state.tracked && post_root == state.root) {
        const u32 resource_scan =
            dreams_sculpt_resource_scan_count.fetch_add(1, std::memory_order_relaxed);
        if (resource_scan < 8) {
            ScanDreamsFirstUnresolvedSculptResource(process, base, state);
        }

        const u64 cached_after = ReadDreamsU64(process, state.builder + 0x1cbfc8);
        const u32 records_after =
            state.output != 0 ? ReadDreamsU32(process, state.output + 0x31ff38) : 0;
        const u32 summary = dreams_sculpt_summary_count.fetch_add(1, std::memory_order_relaxed);
        if (summary < 64) {
            char buffer[2048]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "sculpt_provenance=%llu frame=%u thread=%lu root=0x%016llx/%u "
                "entry={identity=0x%016llx cached=0x%016llx generation=%llu phase=%u "
                "ready=%u,%u tables=%llu,%llu retry=%u records=%u} "
                "path={empty_exit=%u commit=%u/0x%016llx first=%u/%d "
                "hash=%u/%u/%u first_miss=%u:0x%08x:%d:0x%016llx second=%u "
                "emits=%u first_emit=0x%016llx exit=%u} "
                "resource_scan={attempted=%u complete=%u unresolved=%u invalid=%u steps=%u "
                "index=%d object=0x%016llx id=0x%08x header=0x%08x slots=%u,%u,%u "
                "kind=%u writers=0x%016llx,0x%016llx "
                "objects=%u handles=%u active=%u payloads=%u/%u "
                "first_payload=%d:0x%016llx:%u:%u:%u:0x%08x:0x%016llx} "
                "post={cached=0x%016llx records=%u}\r\n",
                static_cast<unsigned long long>(state.invocation), state.frame,
                GetCurrentThreadId(), static_cast<unsigned long long>(state.root),
                state.root_count, static_cast<unsigned long long>(state.root_identity),
                static_cast<unsigned long long>(state.cached_identity),
                static_cast<unsigned long long>(state.generation), state.phase, state.ready,
                state.ready_input, static_cast<unsigned long long>(state.table0_active),
                static_cast<unsigned long long>(state.table1_active),
                state.bootstrap_enabled ? 1 : 0, state.records_before,
                state.empty_incremental_exit ? 1 : 0, state.cache_committed ? 1 : 0,
                static_cast<unsigned long long>(state.committed_identity),
                state.first_complete ? 1 : 0, state.first_count,
                state.hash_preflight_valid ? 1 : 0, state.hash_active, state.hash_missing,
                state.hash_first_index, state.hash_first_target, state.hash_first_work,
                static_cast<unsigned long long>(state.hash_first_object),
                state.second_started ? 1 : 0, state.emit_count,
                static_cast<unsigned long long>(state.first_emit_object),
                state.exit_seen ? 1 : 0, state.resource_scan.attempted ? 1 : 0,
                state.resource_scan.complete ? 1 : 0,
                state.resource_scan.unresolved ? 1 : 0,
                state.resource_scan.invalid_chain ? 1 : 0, state.resource_scan.steps,
                state.resource_scan.index,
                static_cast<unsigned long long>(state.resource_scan.object),
                state.resource_scan.id, state.resource_scan.header,
                state.resource_scan.primary, state.resource_scan.auxiliary,
                state.resource_scan.selected, state.resource_scan.selected_kind,
                static_cast<unsigned long long>(state.resource_scan.primary_writer),
                static_cast<unsigned long long>(state.resource_scan.auxiliary_writer),
                state.resource_scan.type1_objects, state.resource_scan.handles,
                state.resource_scan.active_handles, state.resource_scan.resolved_payloads,
                state.resource_scan.missing_payloads,
                state.resource_scan.first_missing_payload_index,
                static_cast<unsigned long long>(
                    state.resource_scan.first_missing_payload_object),
                state.resource_scan.first_missing_payload_slot,
                state.resource_scan.first_missing_payload_kind,
                state.resource_scan.first_missing_payload_active,
                state.resource_scan.first_missing_payload_value,
                static_cast<unsigned long long>(
                    state.resource_scan.first_missing_payload_writer),
                static_cast<unsigned long long>(cached_after), records_after);
            AppendDreamsSculptProvenanceTrace(buffer, length);

            char second_buffer[1536]{};
            const int second_length = _snprintf_s(
                second_buffer, sizeof(second_buffer), _TRUNCATE,
                "sculpt_second=%llu frame=%u "
                "preflight={valid=%u count=%u active=%u type1=%u/%u stroke=%u "
                "groups=%u/%u type1_valid=%u first=%u:0x%016llx:%u:%d:%u} "
                "milestones={prepare=%u/%u return=%u/%u output=%u/%u:0x%016llx "
                "dispatch=%u resource=%u invalid=%u active=%u payload=%u/%u model=%u/%u "
                "first=0x%016llx:%u:%u:0x%08x:0x%08x gate=%u "
                "first_gate=0x%016llx:%u:0x%016llx}\r\n",
                static_cast<unsigned long long>(state.invocation), state.frame,
                state.second_preflight_valid ? 1 : 0, state.second_count,
                state.second_active, state.second_type1, state.second_sculpt_family,
                state.second_stroke, state.second_group_valid, state.second_group_invalid,
                state.second_type1_group_valid, state.second_first_type1_index,
                static_cast<unsigned long long>(state.second_first_type1_object),
                state.second_first_type1_group, state.second_first_type1_mapped,
                state.second_first_type1_group_valid, state.prepare_calls, state.prepare_type1,
                state.prepare_returns, state.prepare_return_type1, state.output_gate_count,
                state.output_gate_null, static_cast<unsigned long long>(state.first_output_gate),
                state.type1_dispatch_count, state.resource_select_count,
                state.resource_invalid_slots, state.resource_active,
                state.resource_resolved_payloads, state.resource_missing_payloads,
                state.resource_model_ready, state.resource_model_unready,
                static_cast<unsigned long long>(state.first_resource_object),
                state.first_resource_slot, state.first_resource_active,
                state.first_resource_payload, state.first_resource_model_bits,
                state.record_gate_count,
                static_cast<unsigned long long>(state.first_record_gate_object),
                state.first_record_gate_guard,
                static_cast<unsigned long long>(state.first_record_gate_output));
            AppendDreamsSculptProvenanceTrace(second_buffer, second_length);
        }
    }
    state.active = false;
    context->Rax = ReadDreamsU64(process, context->Rsp + 0x650);
    context->Rip = breakpoint_address + 8;
    return true;
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
    constexpr u64 DreamsObjectAppendOffset = 0x9372ae;
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
        case 0x948b60:
        case 0x948c6c:
        case 0x999ab5:
        case 0x99afec:
        case 0xd8bffe:
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
    const auto is_parent_write = [](const u64 offset) noexcept {
        switch (offset) {
        case 0x7afa84:
        case 0x8b6f8f:
        case 0x8f939e:
        case 0x9291c2:
        case 0x93738e:
        case 0x99d78e:
        case 0x99dc13:
        case 0xab0141:
        case 0xab77f0:
        case 0xab8018:
        case 0xac9ff9:
        case 0xb479a9:
            return true;
        default:
            return false;
        }
    };
    constexpr u64 DreamsParentStageImportOffset = 0x8b6d05;
    constexpr u64 DreamsParentRebuildCompleteOffset = 0x99de43;
    const auto is_ready_lifecycle = [](const u64 offset) noexcept {
        switch (offset) {
        case 0xa36485:
        case 0xa36bbf:
        case 0xa370c2:
        case 0xa370ef:
        case 0xa3f7a0:
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
        guest_offset != DreamsObjectAppendOffset &&
        guest_offset != DreamsModelWorkerInspectOffset &&
        guest_offset != DreamsModelWorkerDequeueOffset &&
        guest_offset != DreamsModelWorkerSignalOffset && !is_model_worker_stage(guest_offset) &&
        !is_model_compute_stage(guest_offset) && !is_model_replay_stage(guest_offset) &&
        !is_ready_write(guest_offset) && guest_offset != DreamsParentStageImportOffset &&
        guest_offset != DreamsParentRebuildCompleteOffset &&
        !is_parent_write(guest_offset) &&
        !is_ready_lifecycle(guest_offset)) {
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

    if (guest_offset == DreamsObjectAppendOffset) {
        const u64 root = context->Rdi;
        const u32 old_count = ReadDreamsRootCount(process, root);
        const u32 index = static_cast<u32>(context->R15);
        const u32 new_count = static_cast<u32>(context->Rbx);
        const u64 object = context->R12;
        const u64 caller = ReadDreamsU64(process, context->Rsp);
        const u64 caller_guest = caller >= MemoryPatcher::g_eboot_address
                                     ? caller - MemoryPatcher::g_eboot_address
                                     : 0;
        const u32 id = object != 0 ? ReadDreamsU32(process, object) : 0;
        const u32 header = object != 0 ? ReadDreamsU32(process, object + 0xc) : 0;
        const u16 object_parent = object != 0 ? ReadDreamsU16(process, object + 4) : 0xffff;
        const u8 ready_a = ReadDreamsU8(process, root + 0x27b0a9);
        const u8 ready_b = ReadDreamsU8(process, root + 0x27b0b0);
        const u8 phase = ReadDreamsU8(process, root + 0x27b099);
        char buffer[640]{};
        const int length = _snprintf_s(
            buffer, sizeof(buffer), _TRUNCATE,
            "object_append thread=%lu root=0x%016llx old=%u index=%u new=%u "
            "object=0x%016llx id=0x%08x header=0x%08x parent=%u "
            "phase=%u ready=%u,%u caller=0x%016llx guest_caller=0x%08llx "
            "r13=0x%016llx rsp=0x%016llx\r\n",
            GetCurrentThreadId(), static_cast<unsigned long long>(root), old_count, index,
            new_count, static_cast<unsigned long long>(object), id, header, object_parent, phase,
            ready_a, ready_b, static_cast<unsigned long long>(caller),
            static_cast<unsigned long long>(caller_guest),
            static_cast<unsigned long long>(context->R13),
            static_cast<unsigned long long>(context->Rsp));
        AppendDreamsCpuRootTrace(buffer, length);

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(root + 0x10b58c0), &new_count,
                           sizeof(new_count), &bytes_written);
        context->Rip = breakpoint_address + 6;
        return true;
    }

    if (is_ready_lifecycle(guest_offset)) {
        u64 root = 0;
        u64 event_pointer = 0;
        const char* kind = nullptr;
        switch (guest_offset) {
        case 0xa36485:
            root = context->Rsi;
            event_pointer = context->R8 + 0x680;
            kind = "start_primary";
            break;
        case 0xa36bbf:
            root = context->Rsi;
            event_pointer = context->R8 + 0x680;
            kind = "start_alternate";
            break;
        case 0xa370c2:
            root = context->R14;
            event_pointer = ReadDreamsU64(process, context->Rsp + 0x230);
            kind = "dispatch_check";
            break;
        case 0xa370ef:
            root = context->Rdi;
            event_pointer = context->Rdx + 0x680;
            kind = "complete_primary";
            break;
        case 0xa3f7a0:
            root = context->Rdi;
            event_pointer = context->Rdx + 0x680;
            kind = "complete_alternate";
            break;
        default:
            return false;
        }

        static std::atomic<u32> ready_lifecycle_trace_count{0};
        const u32 ordinal = ready_lifecycle_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 16384 && (ordinal < 8192 || ReadDreamsRootCount(process, root) <= 64)) {
            char buffer[448]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "ready_lifecycle=%u thread=%lu kind=%s offset=0x%08llx "
                "root=0x%016llx/%u event_ptr=0x%016llx event=0x%02x\r\n",
                ordinal, GetCurrentThreadId(), kind,
                static_cast<unsigned long long>(guest_offset),
                static_cast<unsigned long long>(root), ReadDreamsRootCount(process, root),
                static_cast<unsigned long long>(event_pointer),
                event_pointer != 0 ? ReadDreamsU8(process, event_pointer) : 0xff);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        if (guest_offset == 0xa370c2) {
            context->Rax = event_pointer;
            context->Rip = breakpoint_address + 8;
            return true;
        }

        const u64 return_address = breakpoint_address + 5;
        context->Rsp -= sizeof(return_address);
        SIZE_T bytes_written = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(context->Rsp), &return_address,
                           sizeof(return_address), &bytes_written);
        context->Rip = MemoryPatcher::g_eboot_address +
                       (guest_offset == 0xa36485 || guest_offset == 0xa36bbf ? 0xa44b00
                                                                            : 0xa44f90);
        return true;
    }

    if (guest_offset == DreamsParentStageImportOffset) {
        const u64 stage = context->R12;
        const u64 root = context->R13;
        const u32 start = ReadDreamsU32(process, stage + 0x244020);
        const u32 total = ReadDreamsU32(process, stage + 0x244024);
        static std::atomic<u32> parent_import_trace_count{0};
        const u32 ordinal = parent_import_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 4096) {
            char buffer[1024]{};
            int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "parent_import=%u thread=%lu root=0x%016llx/%u stage=0x%016llx "
                "start=%u total=%u mode=%u",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(root),
                ReadDreamsRootCount(process, root), static_cast<unsigned long long>(stage), start,
                total, ReadDreamsU32(process, context->Rbp - 0x3c));
            constexpr std::array<u32, 5> Indices = {13, 19, 21, 23, 24};
            for (const u32 index : Indices) {
                if (length <= 0 || static_cast<size_t>(length) >= sizeof(buffer) ||
                    index < start || index >= total) {
                    continue;
                }
                const u64 object = ReadDreamsU64(
                    process, stage + 0x260050 + static_cast<u64>(index) * sizeof(u64));
                const u16 parent = ReadDreamsU16(
                    process, stage + 0x280050 + static_cast<u64>(index) * sizeof(u16));
                const u16 object_parent = object != 0 ? ReadDreamsU16(process, object + 4) : 0xffff;
                const u32 id = object != 0 ? ReadDreamsU32(process, object) : 0;
                const u32 header = object != 0 ? ReadDreamsU32(process, object + 0xc) : 0;
                const u8 anchor = object != 0 ? ReadDreamsU8(process, object + 0x45) : 0;
                const int appended = _snprintf_s(
                    buffer + length, sizeof(buffer) - static_cast<size_t>(length), _TRUNCATE,
                    " i%u={p%u,op%u,id%08x,h%08x,a%u}", index, parent, object_parent, id,
                    header, anchor);
                if (appended <= 0) {
                    break;
                }
                length += appended;
            }
            if (length > 0 && static_cast<size_t>(length) + 2 < sizeof(buffer)) {
                buffer[length++] = '\r';
                buffer[length++] = '\n';
                buffer[length] = '\0';
                AppendDreamsCpuRootTrace(buffer, length);
            }
        }
        context->R10 = total;
        context->Rip = breakpoint_address + 8;
        return true;
    }

    if (guest_offset == DreamsParentRebuildCompleteOffset) {
        const u64 root = context->Rsi;
        const u32 count = static_cast<u32>(context->Rax);
        static std::atomic<u32> parent_rebuild_trace_count{0};
        const u32 ordinal = parent_rebuild_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 4096 && count <= 64) {
            char buffer[768]{};
            int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "parent_rebuild=%u thread=%lu root=0x%016llx/%u count=%u",
                ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(root),
                ReadDreamsRootCount(process, root), count);
            constexpr std::array<u32, 5> Indices = {13, 19, 21, 23, 24};
            for (const u32 index : Indices) {
                if (length <= 0 || static_cast<size_t>(length) >= sizeof(buffer) || index >= count) {
                    continue;
                }
                const u64 object = ReadDreamsU64(
                    process, root + 0x10958c0 + static_cast<u64>(index) * sizeof(u64));
                const u16 parent = ReadDreamsU16(
                    process, root + 0x1ac7554 + static_cast<u64>(index) * sizeof(u16));
                const u32 id = object != 0 ? ReadDreamsU32(process, object) : 0;
                const u32 header = object != 0 ? ReadDreamsU32(process, object + 0xc) : 0;
                const u8 anchor = object != 0 ? ReadDreamsU8(process, object + 0x45) : 0;
                const int appended = _snprintf_s(
                    buffer + length, sizeof(buffer) - static_cast<size_t>(length), _TRUNCATE,
                    " i%u={p%u,id%08x,h%08x,a%u}", index, parent, id, header, anchor);
                if (appended <= 0) {
                    break;
                }
                length += appended;
            }
            if (length > 0 && static_cast<size_t>(length) + 2 < sizeof(buffer)) {
                buffer[length++] = '\r';
                buffer[length++] = '\n';
                buffer[length] = '\0';
                AppendDreamsCpuRootTrace(buffer, length);
            }
        }
        context->R14 = context->Rsi;
        context->Rip = breakpoint_address + 3;
        return true;
    }

    if (is_parent_write(guest_offset)) {
        u64 root = 0;
        u64 index = 0;
        u16 parent = 0;
        u64 instruction_size = 0;
        switch (guest_offset) {
        case 0x7afa84:
            root = context->Rdi;
            index = context->Rcx;
            parent = static_cast<u16>(context->Rax);
            instruction_size = 8;
            break;
        case 0x8b6f8f:
            root = context->R13;
            index = context->R15;
            parent = static_cast<u16>(context->Rcx);
            instruction_size = 9;
            break;
        case 0x8f939e:
            root = context->R14;
            index = context->Rcx;
            parent = static_cast<u16>(context->Rax);
            instruction_size = 9;
            break;
        case 0x9291c2:
            root = context->R12;
            index = context->Rax;
            parent = static_cast<u16>(context->Rdx);
            instruction_size = 9;
            break;
        case 0x93738e:
            root = context->R13;
            index = context->Rax;
            parent = static_cast<u16>(context->Rcx);
            instruction_size = 9;
            break;
        case 0x99d78e:
            root = context->R14;
            parent = static_cast<u16>(context->Rax);
            instruction_size = 8;
            break;
        case 0x99dc13:
            root = context->Rsi;
            index = context->Rcx;
            parent = static_cast<u16>(context->Rdx);
            instruction_size = 8;
            break;
        case 0xab0141:
            root = context->Rbx;
            index = context->Rcx;
            parent = static_cast<u16>(context->Rax);
            instruction_size = 8;
            break;
        case 0xab77f0:
            root = context->Rcx;
            index = context->Rdx;
            parent = static_cast<u16>(context->Rax);
            instruction_size = 8;
            break;
        case 0xab8018:
            root = context->Rsi;
            index = context->Rdx;
            parent = static_cast<u16>(context->Rax);
            instruction_size = 8;
            break;
        case 0xac9ff9:
            root = context->Rbx;
            index = context->Rdx;
            parent = static_cast<u16>(context->Rcx);
            instruction_size = 8;
            break;
        case 0xb479a9:
            root = context->Rdi;
            index = context->R8;
            parent = static_cast<u16>(context->Rdx);
            instruction_size = 9;
            break;
        default:
            return false;
        }

        const u32 root_count = ReadDreamsRootCount(process, root);
        const u64 object = index < 0x4000
                               ? ReadDreamsU64(process, root + 0x10958c0 + index * sizeof(u64))
                               : 0;
        const u32 header = object != 0 ? ReadDreamsU32(process, object + 0xc) : 0;
        const u32 id = object != 0 ? ReadDreamsU32(process, object) : 0;
        static std::atomic<u32> parent_write_trace_count{0};
        const u32 ordinal = parent_write_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 16384 && (ordinal < 4096 || root_count <= 64)) {
            char buffer[512]{};
            const int length = _snprintf_s(
                buffer, sizeof(buffer), _TRUNCATE,
                "parent_write=%u thread=%lu offset=0x%08llx root=0x%016llx/%u "
                "index=%llu parent=%u object=0x%016llx id=0x%08x header=0x%08x\r\n",
                ordinal, GetCurrentThreadId(),
                static_cast<unsigned long long>(guest_offset),
                static_cast<unsigned long long>(root), root_count,
                static_cast<unsigned long long>(index), parent,
                static_cast<unsigned long long>(object), id, header);
            AppendDreamsCpuRootTrace(buffer, length);
        }

        SIZE_T bytes_written = 0;
        WriteProcessMemory(process,
                           reinterpret_cast<void*>(root + 0x1ac7554 + index * sizeof(u16)),
                           &parent, sizeof(parent), &bytes_written);
        context->Rip = breakpoint_address + instruction_size;
        return true;
    }

    if (is_ready_write(guest_offset)) {
        u64 root = 0;
        u64 field_offset = 0;
        u8 value = 0;
        u64 instruction_size = 0;
        switch (guest_offset) {
        case 0x948b60:
            root = context->R15;
            field_offset = 0x27b0a9;
            value = static_cast<u8>(context->Rax);
            instruction_size = 7;
            break;
        case 0x948c6c:
            root = context->R15;
            field_offset = 0x27b0b0;
            value = static_cast<u8>(context->Rax);
            instruction_size = 7;
            break;
        case 0x999ab5:
            root = context->R12;
            field_offset = 0x27b0a9;
            value = static_cast<u8>(context->Rax);
            instruction_size = 8;
            break;
        case 0x99afec:
            root = context->R12;
            field_offset = 0x27b0b0;
            value = static_cast<u8>(context->Rax);
            instruction_size = 8;
            break;
        case 0xd8bffe:
            root = context->Rsi;
            field_offset = 0x27b0b0;
            value = static_cast<u8>(context->Rax);
            instruction_size = 6;
            break;
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
        const u64 remap = context->Rsp + 0x10248;
        const u64 pending = context->Rsp + 0x248;
        char force_value[2]{};
        const bool force_stroke_active =
            GetEnvironmentVariableA("SHADPS4_DREAMS_FORCE_STROKE_ACTIVE", force_value,
                                    sizeof(force_value)) != 0 &&
            force_value[0] == '1';
        char force_sculpt_value[2]{};
        const bool force_latest_sculpt_active =
            GetEnvironmentVariableA("SHADPS4_DREAMS_FORCE_LATEST_SCULPT_ACTIVE",
                                    force_sculpt_value, sizeof(force_sculpt_value)) != 0 &&
            force_sculpt_value[0] == '1';

        if (root_count <= 32) {
            static std::atomic<u32> active_map_trace_count{0};
            for (u32 index = 0; index < root_count; ++index) {
                const u8 type = ReadDreamsU8(process, root + 0x1acf558 + index);
                const u8 cached_type = type & 0x7f;
                if (cached_type != 0x11 && cached_type != 0x01) {
                    continue;
                }

                const u8 before = ReadDreamsU8(process, active_map + index);
                u8 after = before;
                const bool force_this_stroke = force_stroke_active && cached_type == 0x11;
                const bool force_this_sculpt = force_latest_sculpt_active &&
                                               cached_type == 0x01 && index + 1 == root_count;
                if ((force_this_stroke || force_this_sculpt) && before == 0) {
                    after = 1;
                    SIZE_T bytes_written = 0;
                    WriteProcessMemory(process, reinterpret_cast<void*>(active_map + index), &after,
                                       sizeof(after), &bytes_written);
                }

                const u32 ordinal =
                    active_map_trace_count.fetch_add(1, std::memory_order_relaxed);
                if (ordinal < 2048) {
                    const u64 object = ReadDreamsU64(process, root + 0x10958c0 + index * 8);
                    const u32 header = ReadDreamsU32(process, object + 0xc);
                    const u32 object_type = header & 0x7f;
                    const u64 metadata = MemoryPatcher::g_eboot_address + 0x9f29f90 +
                                         static_cast<u64>(object_type) * 0xb02;
                    const u16 field_e2 = ReadDreamsU16(process, metadata + 0xe2);
                    const u16 field_e4 = ReadDreamsU16(process, metadata + 0xe4);
                    const u8 suppress_e2 =
                        field_e2 != 0 ? ReadDreamsU8(process, object + field_e2) : 0;
                    const u8 suppress_e4 =
                        field_e4 != 0 ? ReadDreamsU8(process, object + field_e4) : 0;
                    const u16 parent =
                        ReadDreamsU16(process, root + 0x1ac7554 + index * sizeof(u16));
                    const bool parent_valid = parent < root_count;
                    const u64 parent_object =
                        parent_valid
                            ? ReadDreamsU64(process, root + 0x10958c0 + parent * sizeof(u64))
                            : 0;
                    const u16 grandparent =
                        parent_valid
                            ? ReadDreamsU16(process,
                                           root + 0x1ac7554 + parent * sizeof(u16))
                            : 0xffff;
                    const bool grandparent_valid = grandparent < root_count;
                    char buffer[1024]{};
                    const int length = _snprintf_s(
                        buffer, sizeof(buffer), _TRUNCATE,
                        "active_map=%u thread=%lu root=0x%016llx/%u index=%u "
                        "object=0x%016llx header=0x%08x pending=0x%08x remap=0x%08x "
                        "e2=0x%04x/%u e4=0x%04x/%u lifecycle=%u state=0x%02x "
                        "parent=%u pending=0x%08x remap=0x%08x type=0x%02x "
                        "parent_object=0x%016llx id=0x%08x header=0x%08x anchor=%u "
                        "grandparent=%u remap=0x%08x "
                        "active=%u->%u forced_stroke=%u forced_sculpt=%u\r\n",
                        ordinal, GetCurrentThreadId(), static_cast<unsigned long long>(root),
                        root_count, index, static_cast<unsigned long long>(object), header,
                        ReadDreamsU32(process, pending + index * sizeof(u32)),
                        ReadDreamsU32(process, remap + index * sizeof(u32)), field_e2,
                        suppress_e2, field_e4, suppress_e4,
                        ReadDreamsU8(process, object + 0x244),
                        ReadDreamsU8(process, root + 0x1a40e38 + index), parent,
                        parent_valid
                            ? ReadDreamsU32(process, pending + parent * sizeof(u32))
                            : 0xffffffffu,
                        parent_valid ? ReadDreamsU32(process, remap + parent * sizeof(u32))
                                     : 0xffffffffu,
                        parent_valid ? ReadDreamsU8(process, root + 0x1acf558 + parent) : 0xff,
                        static_cast<unsigned long long>(parent_object),
                        parent_object != 0 ? ReadDreamsU32(process, parent_object) : 0xffffffffu,
                        parent_object != 0 ? ReadDreamsU32(process, parent_object + 0xc)
                                           : 0xffffffffu,
                        parent_object != 0 ? ReadDreamsU8(process, parent_object + 0x45) : 0xff,
                        grandparent,
                        grandparent_valid
                            ? ReadDreamsU32(process, remap + grandparent * sizeof(u32))
                            : 0xffffffffu,
                        before, after, force_this_stroke ? 1 : 0,
                        force_this_sculpt ? 1 : 0);
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
        struct DreamsCsgCompletionTimingState {
            u64 compute_start_qpc{};
            u64 wait_start_qpc{};
            u64 event_start_qpc{};
            u32 sequence{};
            u32 wait_iteration{};
        };
        static thread_local DreamsCsgCompletionTimingState csg_timing{};
        static const u64 qpc_frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return static_cast<u64>(value.QuadPart);
        }();
        const auto trace_pipe = [&](const char* kind, const u64 pipe, const u64 wait_result,
                                    const u32 out_count) noexcept {
            LARGE_INTEGER qpc_value{};
            QueryPerformanceCounter(&qpc_value);
            const u64 qpc = static_cast<u64>(qpc_value.QuadPart);
            if (guest_offset == 0x12850c1) {
                ++csg_timing.sequence;
                csg_timing.compute_start_qpc = qpc;
                csg_timing.wait_start_qpc = 0;
                csg_timing.event_start_qpc = 0;
                csg_timing.wait_iteration = 0;
            } else if (guest_offset == 0x1287bd0) {
                csg_timing.wait_start_qpc = qpc;
                csg_timing.event_start_qpc = 0;
                csg_timing.wait_iteration = 0;
            } else if (guest_offset == 0x1287c83) {
                ++csg_timing.wait_iteration;
                csg_timing.event_start_qpc = qpc;
            }

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
            if (DreamsCsgCompletionTraceEnabled()) {
                const u64 since_compute_us = DreamsQpcElapsedMicroseconds(
                    csg_timing.compute_start_qpc, qpc, qpc_frequency);
                const u64 since_wait_us = DreamsQpcElapsedMicroseconds(
                    csg_timing.wait_start_qpc, qpc, qpc_frequency);
                const u64 event_wait_us = guest_offset == 0x1287c88
                                              ? DreamsQpcElapsedMicroseconds(
                                                    csg_timing.event_start_qpc, qpc, qpc_frequency)
                                              : 0;
                const u32 observed_target = guest_offset == 0x1287d23
                                                ? static_cast<u32>(context->R13)
                                                : target;
                if (ordinal < 16384) {
                    char buffer[768]{};
                    const int length = _snprintf_s(
                        buffer, sizeof(buffer), _TRUNCATE,
                        "csg_completion=%u sequence=%u iteration=%u tick=%llu qpc=%llu/%llu "
                        "thread=%lu kind=%s offset=0x%08llx pipe=0x%016llx "
                        "completion_ptr=0x%016llx completion=%llu target=%u observed_target=%u "
                        "equeue=0x%016llx commands=0x%016llx,0x%016llx,0x%016llx "
                        "wait_result=0x%016llx out=%u since_compute_us=%llu since_wait_us=%llu "
                        "event_wait_us=%llu rsp=0x%016llx\r\n",
                        ordinal, csg_timing.sequence, csg_timing.wait_iteration,
                        static_cast<unsigned long long>(GetTickCount64()),
                        static_cast<unsigned long long>(qpc),
                        static_cast<unsigned long long>(qpc_frequency), GetCurrentThreadId(), kind,
                        static_cast<unsigned long long>(guest_offset),
                        static_cast<unsigned long long>(pipe),
                        static_cast<unsigned long long>(completion_pointer),
                        static_cast<unsigned long long>(completion), target, observed_target,
                        static_cast<unsigned long long>(equeue),
                        static_cast<unsigned long long>(command_begin),
                        static_cast<unsigned long long>(command_cursor),
                        static_cast<unsigned long long>(command_end),
                        static_cast<unsigned long long>(wait_result), out_count,
                        static_cast<unsigned long long>(since_compute_us),
                        static_cast<unsigned long long>(since_wait_us),
                        static_cast<unsigned long long>(event_wait_us),
                        static_cast<unsigned long long>(context->Rsp));
                    AppendDreamsCsgCompletionTrace(buffer, length);
                } else if (ordinal == 16384) {
                    static constexpr char Truncated[] =
                        "csg_completion_trace_truncated=1 limit=16384\r\n";
                    AppendDreamsCsgCompletionTrace(Truncated, sizeof(Truncated) - 1);
                }
            } else if (ordinal < 256 || DreamsStampTraceCaptureEnabled()) {
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
        handled = HandleDreamsVisibleSculptsTrace(pExp) ||
                  HandleDreamsSceneReadyHandoff(pExp) ||
                  HandleDreamsSceneBuilderGateTrace(pExp) ||
                  HandleDreamsSceneCacheBootstrap(pExp) ||
                  HandleDreamsSculptLifecycleTrace(pExp) ||
                  HandleDreamsSculptProvenanceTrace(pExp) ||
                  HandleDreamsModelBoundaryTrace(pExp) ||
                  HandleDreamsModelRecordTrace(pExp) ||
                  HandleDreamsPairQueueTrace(pExp) ||
                  HandleDreamsOfflineLimitsTrace(pExp) ||
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
    case MS_CPP_EXCEPTION:
        // This is the first-chance exception used by the MSVC C++ runtime. It must reach the
        // language exception handler; treating it as an unhandled emulator fault can initiate an
        // asynchronous shutdown even when the exception is caught normally.
        return EXCEPTION_CONTINUE_SEARCH;
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
