// The console CPU's AMD-only instructions (runtime/shims/guest_cpu.h) on this host: CLZERO must zero the 64-byte
// line at RAX and continue, and MONITORX/MWAITX must be skipped, whether the CPU implements them or the handler
// emulates them. Hand-built machine code, no game data.
#include "../shims/guest_cpu.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "guest_cpu_test: %s\n", what);
    ++g_failures;
}

// mov rax, rcx; <bytes>; ret
void* make_stub(const std::uint8_t* bytes, std::size_t count) {
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) return nullptr;
    auto* code = static_cast<std::uint8_t*>(page);
    code[0] = 0x48;  // mov rax, rcx
    code[1] = 0x89;
    code[2] = 0xC8;
    std::memcpy(code + 3, bytes, count);
    code[3 + count] = 0xC3;  // ret
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    return page;
}

}  // namespace

int main() {
    rt::guest_cpu::install();
    alignas(64) std::uint8_t line[128];
    std::memset(line, 0xab, sizeof(line));
    std::uint8_t clzero[] = {0x0F, 0x01, 0xFC};
    void* page = make_stub(clzero, sizeof(clzero));
    if (!page) {
        std::fprintf(stderr, "guest_cpu_test: VirtualAlloc failed\n");
        return 1;
    }
    reinterpret_cast<void (*)(void*)>(page)(line);  // CLZERO clears the aligned line holding RAX
    const bool zeroed = [&] {
        for (std::size_t i = 0; i < 64; ++i)
            if (line[i] != 0) return false;
        return true;
    }();
    expect(zeroed, "CLZERO zeroed the line at RAX");
    const bool untouched = [&] {
        for (std::size_t i = 64; i < 128; ++i)
            if (line[i] != 0xab) return false;
        return true;
    }();
    expect(untouched, "CLZERO left the neighbouring line alone");
    VirtualFree(page, 0, MEM_RELEASE);

    std::uint8_t monitor[] = {0x0F, 0x01, 0xFA, 0x0F, 0x01, 0xFB};  // monitorx; mwaitx
    page = make_stub(monitor, sizeof(monitor));
    if (!page) return 1;
    reinterpret_cast<void (*)(void*)>(page)(nullptr);  // skipped, not executed
    expect(true, "MONITORX/MWAITX did not fault");
    VirtualFree(page, 0, MEM_RELEASE);

    if (g_failures) return 1;
    std::printf("guest_cpu_test: CLZERO, MONITORX and MWAITX are emulated where the host lacks them\n");
    return 0;
}
