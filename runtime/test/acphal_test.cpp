// Checks the registered audio coprocessor shims (runtime/shims/acphal.cpp) against the call shapes measured in the
// retail executable. Every export is resolved through rt::shims::find, so this is the table the runtime installs.
//
// This is a boot bring-up shim: the checks are about non-null, zeroed, consistent dummy objects and non-negative
// results, not about audio. The measured field offsets are asserted where the game's code reads them:
// - AcpHalAllocateShapeContexts takes a descriptor, count at +0x10, array pointer out at +0x38 (call 0x43aec73).
// - AcpHalCreate returns an object whose slots 0, 1, 2 and 5 the title calls (0x43aedba, 0x43ae84f, 0x43aee2f,
//   0x43ae88b).
// - ApuAlloc's first argument is an out pointer (0x43aed13 reads it after the call), not a heap handle.
// - XApuConnect's third argument is the out handle (0x9ae8960, the global Disconnect reads back at 0x43ae924) and
//   its second argument's first qword is the command-buffer base the title reads at 0x4e64b45 / 0x4f20afe / 0x4f20b64.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using AcpHalCreateFn = HRESULT(__stdcall*)(void**, uint32_t);
using AcpHalAllocateShapeContextsFn = HRESULT(__stdcall*)(void*);
using AcpHalReleaseShapeContextsFn = HRESULT(__stdcall*)(void*);
using ApuCreateHeapFn = HRESULT(__stdcall*)(uint32_t, uint32_t);
using ApuAllocFn = void*(__stdcall*)(void**, uint32_t, size_t, uint32_t);
using ApuFreeFn = void(__stdcall*)(void*);
using ApuIsVirtualAddressValidFn = BOOL(__stdcall*)(const void*, uint32_t);
using ApuMapVirtualAddressFn = uint32_t(__stdcall*)(const void*);
using ApuMapApuAddressFn = void*(__stdcall*)(uint32_t);

using XApuConnectFn = HRESULT(__stdcall*)(void*, void*, void**);
using XApuDisconnectFn = HRESULT(__stdcall*)(void*, uint32_t);
using XApuEnqueueCommandFn = HRESULT(__stdcall*)(void*, const void*, uint32_t);
using XApuDequeueResultFn = HRESULT(__stdcall*)(void*, void*);
using XApuGetResultCountFn = uint32_t(__stdcall*)(void*);

using XDspConnectFn = int(__stdcall*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);
using XDspActivateFn = int(__stdcall*)(void*, uint32_t, uint32_t, uint32_t);
using XDspSubmitCommandFn = int(__stdcall*)(void*, void*);
using XDspDeactivateFn = int(__stdcall*)(void*);
using XDspDisconnectFn = int(__stdcall*)(void*, uint32_t);

using AcpHalSlotFn = intptr_t(__stdcall*)(void*);
using AcpHalInitFn = intptr_t(__stdcall*)(void*, uint32_t, uint32_t);
using AcpHalCommandFn = intptr_t(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);

// The title's descriptor at 0x43aec4b: a zeroed prefix, the count at +0x10 and the array pointer written at +0x38.
struct ShapeContextsDesc {
    uint64_t reserved0;
    uint64_t reserved1;
    uint64_t count;
    uint8_t reserved2[0x20];
    void* contexts;
};
static_assert(sizeof(ShapeContextsDesc) == 0x40, "the measured descriptor is 0x40 bytes");
static_assert(offsetof(ShapeContextsDesc, count) == 0x10, "the title reads the count at desc + 0x10");
static_assert(offsetof(ShapeContextsDesc, contexts) == 0x38, "the title reads the array pointer at desc + 0x38");

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

bool all_bytes(const uint8_t* p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != value) return false;
    return true;
}

}  // namespace

int main() {
    // ---- registration -------------------------------------------------------------------------------------------
    auto create = reinterpret_cast<AcpHalCreateFn>(rt::shims::find("acphal.dll", "AcpHalCreate"));
    auto allocate = reinterpret_cast<AcpHalAllocateShapeContextsFn>(
        rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
    auto release_contexts = reinterpret_cast<AcpHalReleaseShapeContextsFn>(
        rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
    auto create_heap = reinterpret_cast<ApuCreateHeapFn>(rt::shims::find("acphal.dll", "ApuCreateHeap"));
    auto alloc = reinterpret_cast<ApuAllocFn>(rt::shims::find("acphal.dll", "ApuAlloc"));
    auto free_block = reinterpret_cast<ApuFreeFn>(rt::shims::find("acphal.dll", "ApuFree"));
    auto valid = reinterpret_cast<ApuIsVirtualAddressValidFn>(
        rt::shims::find("acphal.dll", "ApuIsVirtualAddressValid"));
    auto map_virtual = reinterpret_cast<ApuMapVirtualAddressFn>(
        rt::shims::find("acphal.dll", "ApuMapVirtualAddress"));
    auto map_apu = reinterpret_cast<ApuMapApuAddressFn>(rt::shims::find("acphal.dll", "ApuMapApuAddress"));

    auto xconnect = reinterpret_cast<XApuConnectFn>(rt::shims::find("xapu.dll", "XApuConnect"));
    auto xdisconnect = reinterpret_cast<XApuDisconnectFn>(rt::shims::find("xapu.dll", "XApuDisconnect"));
    auto xenqueue = reinterpret_cast<XApuEnqueueCommandFn>(rt::shims::find("xapu.dll", "XApuEnqueueCommand"));
    auto xdequeue = reinterpret_cast<XApuDequeueResultFn>(rt::shims::find("xapu.dll", "XApuDequeueResult"));
    auto xcount = reinterpret_cast<XApuGetResultCountFn>(rt::shims::find("xapu.dll", "XApuGetResultCount"));

    auto dconnect = reinterpret_cast<XDspConnectFn>(
        rt::shims::find("chad.dll", "XDspConnectWithMaximumStreamLimit"));
    auto dactivate = reinterpret_cast<XDspActivateFn>(rt::shims::find("chad.dll", "XDspActivate"));
    auto dsubmit = reinterpret_cast<XDspSubmitCommandFn>(rt::shims::find("chad.dll", "XDspSubmitCommand"));
    auto ddeactivate = reinterpret_cast<XDspDeactivateFn>(rt::shims::find("chad.dll", "XDspDeactivate"));
    auto ddisconnect = reinterpret_cast<XDspDisconnectFn>(rt::shims::find("chad.dll", "XDspDisconnect"));

    check(create && allocate && release_contexts && create_heap && alloc && free_block && valid && map_virtual &&
              map_apu,
          "all nine acphal.dll exports are registered");
    check(xconnect && xdisconnect && xenqueue && xdequeue && xcount, "all five xapu.dll exports are registered");
    check(dconnect && dactivate && dsubmit && ddeactivate && ddisconnect,
          "all five chad.dll exports are registered");
    check(rt::shims::owns_dll("acphal.dll") && rt::shims::owns_dll("xapu.dll") && rt::shims::owns_dll("chad.dll"),
          "the registry owns acphal.dll, xapu.dll and chad.dll");
    if (g_failures) return 1;

    // ---- AcpHalCreate and the object the title calls through ----------------------------------------------------
    void* object = nullptr;
    check(create(&object, 0) == S_OK && object != nullptr, "AcpHalCreate stores a non-null object and returns S_OK");
    check(create(nullptr, 0) == E_POINTER, "AcpHalCreate(nullptr, 0) is E_POINTER");
    if (!object) return 1;

    auto* vtable = *static_cast<void* const* const*>(object);
    check(vtable != nullptr, "the AcpHal object starts with a vtable");
    if (vtable) {
        check(reinterpret_cast<AcpHalInitFn>(vtable[0])(object, 0x1000, 0x1000) == 0,
              "slot 0 (init 0x1000, 0x1000) returns 0, the non-negative result the title tests");
        uint32_t command_word = 0xc0;
        check(reinterpret_cast<AcpHalCommandFn>(vtable[2])(object, 0x12, 0xf1f1f1f1u, 0xffffffffu, &command_word, 0) ==
                  0,
              "slot 2 (command 0x12) returns 0");
        check(reinterpret_cast<AcpHalSlotFn>(vtable[1])(object) >= 0, "slot 1 (teardown stop) returns non-negative");
        check(reinterpret_cast<AcpHalSlotFn>(vtable[5])(object) == 0, "slot 5 (teardown release) returns 0");
        object = nullptr;  // slot 5 is the measured release, the shim frees the object there
    }

    // ---- AcpHalAllocateShapeContexts: descriptor layout and a zeroed array --------------------------------------
    ShapeContextsDesc desc{};
    desc.count = 0x15e;  // the measured count
    check(allocate(&desc) == S_OK, "AcpHalAllocateShapeContexts(desc) returns S_OK");
    check(desc.contexts != nullptr, "the shim writes the array pointer at desc + 0x38");
    if (desc.contexts) {
        const size_t bytes = static_cast<size_t>(desc.count) * 0x40;
        check(all_bytes(static_cast<const uint8_t*>(desc.contexts), bytes, 0),
              "the shape-contexts array is zeroed for count * 0x40 bytes");
        check(desc.reserved0 == 0 && desc.reserved1 == 0 && all_bytes(desc.reserved2, sizeof(desc.reserved2), 0),
              "the shim leaves the descriptor's zeroed prefix alone");
    }
    check(allocate(nullptr) == E_POINTER, "AcpHalAllocateShapeContexts(nullptr) is E_POINTER");
    check(release_contexts(desc.contexts) == S_OK, "AcpHalReleaseShapeContexts(handle) returns S_OK");
    desc.contexts = nullptr;

    // ---- APU heap, allocation, free and address mapping ---------------------------------------------------------
    check(create_heap(0x1c700000u, 0x226e00u) == S_OK,
          "ApuCreateHeap(0x1c700000, 0x226e00) returns S_OK (the measured sizes, not an out pointer)");

    void* block = nullptr;
    constexpr size_t kMeasuredApuSize = 0x222e00;
    check(alloc(&block, 0, kMeasuredApuSize, 0x800) == block && block != nullptr,
          "ApuAlloc writes a non-null block to its first argument and returns it");
    check(alloc(nullptr, 0, 0x1000, 0x800) == nullptr, "ApuAlloc(nullptr, ...) returns null");
    if (block) {
        check(reinterpret_cast<uintptr_t>(block) % 0x1000 == 0, "the APU block is 0x1000-aligned");
        check(static_cast<uint8_t*>(block)[0] == 0 && static_cast<uint8_t*>(block)[kMeasuredApuSize - 1] == 0,
              "the APU block is zeroed across the requested size");
    }

    check(valid(block, 0x40) == TRUE && valid(block, 0x80) == TRUE && valid(block, 0x800) == TRUE,
          "ApuIsVirtualAddressValid is TRUE for all three measured sizes");
    check(valid(nullptr, 0x40) == FALSE, "ApuIsVirtualAddressValid(nullptr) is FALSE");

    const uint32_t id = map_virtual(block);
    check(id != 0, "ApuMapVirtualAddress gives a non-zero id");
    check(map_virtual(block) == id, "ApuMapVirtualAddress is stable for the same pointer");
    auto* interior = static_cast<uint8_t*>(block) + 0x1700;  // the title's second record pointer
    const uint32_t interior_id = map_virtual(interior);
    check(interior_id != 0 && interior_id != id, "a different pointer gets a different id");
    check(map_apu(id) == block, "ApuMapApuAddress(id) round-trips to the original pointer");
    check(map_apu(interior_id) == interior, "ApuMapApuAddress(interior id) round-trips too");
    check(map_apu(0xdeadbeefu) == nullptr, "ApuMapApuAddress of an id the shim never issued is null");
    check(map_virtual(nullptr) == 0, "ApuMapVirtualAddress(nullptr) is 0");

    free_block(interior);  // the title frees base + 0x1700 as well; an interior pointer is a no-op
    free_block(block);
    free_block(block);  // double free is a no-op

    // ---- xapu command queue -------------------------------------------------------------------------------------
    void* command_queue[4] = {};
    void* result_queue[4] = {};
    uint8_t command[0x60];
    std::memset(command, 0x5a, sizeof(command));
    void* handle = nullptr;
    check(xconnect(command_queue, result_queue, &handle) == S_OK && handle != nullptr,
          "XApuConnect writes a non-null handle to its third argument and returns S_OK");
    check(result_queue[0] != nullptr,
          "XApuConnect fills the second descriptor's first qword with a command buffer");
    check(xconnect(command_queue, result_queue, nullptr) == E_POINTER, "XApuConnect(..., nullptr) is E_POINTER");
    check(xenqueue(handle, command, 0) == S_OK, "XApuEnqueueCommand returns S_OK");
    check(xcount(handle) == 0, "XApuGetResultCount returns 0 (no results)");
    uint8_t result[0x60];
    std::memset(result, 0xcd, sizeof(result));
    check(xdequeue(handle, result) == S_FALSE, "XApuDequeueResult returns S_FALSE, non-negative");
    check(all_bytes(result, sizeof(result), 0xcd), "XApuDequeueResult writes nothing");
    check(xdisconnect(handle, 0x20) == S_OK, "XApuDisconnect(handle, 0x20) returns S_OK");

    // ---- chad DSP commands --------------------------------------------------------------------------------------
    uint8_t context[0x500];
    std::memset(context, 0, sizeof(context));
    check(dconnect(0x1122334455667788ull, 0x20, 0x40, 0x80, 1, context) == 0,
          "XDspConnectWithMaximumStreamLimit(..., 0x80, ...) returns 0 (success)");
    check(dactivate(handle, 1, 2, 3) == 0, "XDspActivate returns 0");
    check(dsubmit(handle, command) == 0, "XDspSubmitCommand returns 0");
    check(ddeactivate(handle) == 0, "XDspDeactivate returns 0");
    check(ddisconnect(handle, 0x20) == 0, "XDspDisconnect returns 0");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
