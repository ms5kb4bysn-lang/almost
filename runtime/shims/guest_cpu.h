#pragma once

// Console (Zen 2) instructions an x86-64 PC host may not have. The Xbox Series CPU is an AMD Zen 2, so the title's
// compiled code uses AMD-only instructions unconditionally; on an Intel host the first one raises
// STATUS_ILLEGAL_INSTRUCTION (0xC000001D) and takes the process down. This handler emulates the ones the title
// reaches:
//   - CLZERO (0F 01 FC): zero the 64-byte cache line holding RAX. The title clears buffers with it in a short loop
//     (game_scarlett_master.exe 1.0.2.1 at exe+0x4e8aa70: clzero; add rax, 0x40; cmp; jb).
//   - MONITORX/MWAITX (0F 01 FA / 0F 01 FB): no-ops. The title uses them to sleep on the fence's completed value
//     (device slot 36 / fence slot 12); skipping the wait leaves its loop re-reading the cell, which is correct.
// Anything else keeps its #UD. Installed before any guest code runs, so DLL TLS callbacks see it too.
//
// Not for x86 hosts without the instructions: the handler only emulates on the fault, and a CPU that has the
// instruction never faults.

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace rt::guest_cpu {

inline LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    const auto* at = static_cast<const std::uint8_t*>(info->ExceptionRecord->ExceptionAddress);
    if (!at || at[0] != 0x0F || at[1] != 0x01) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    if (at[2] == 0xFC) {  // CLZERO
        const std::uint64_t line = context.Rax & ~std::uint64_t{63};
        std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(line)), 0, 64);
        context.Rip += 3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (at[2] == 0xFA || at[2] == 0xFB) {  // MONITORX / MWAITX
        context.Rip += 3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

inline void install() {
    static PVOID handler_handle = AddVectoredExceptionHandler(1, &handler);
    (void)handler_handle;
}

}  // namespace rt::guest_cpu
