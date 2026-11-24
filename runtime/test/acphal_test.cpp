// Checks the registered audio coprocessor shims (runtime/shims/acphal.cpp) against the call shapes measured in the
// retail executable. Every export is resolved through rt::shims::find, so this is the table the runtime installs.
//
// This is a boot bring-up shim: the AcpHal checks are about non-null, zeroed, consistent dummy objects; the xapu
// checks drive the real command/result state machine the title polls. The measured field offsets are asserted where
// the game's code reads them:
// - AcpHalAllocateShapeContexts takes a descriptor, count at +0x10, array pointer out at +0x38 (call 0x43aec73).
// - AcpHalCreate returns an object whose slots 0, 1, 2 and 5 the title calls (0x43aedba, 0x43ae84f, 0x43aee2f,
//   0x43ae88b).
// - ApuAlloc's first argument is an out pointer (0x43aed13 reads it after the call), not a heap handle.
// - XApuConnect's input is 0x18 bytes, output {memory, event}, handle last (0x9ae9890 / 0x9ae98a8 / 0x9ae8960).
//   Commands carry a 24-byte id, process commands are 0x40 bytes and results 0x50 (0x4ee9945 / 0x4f20b7e / 0x4eaa8a3).
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

using XApuConnectFn = HRESULT(__stdcall*)(const void*, void*, void**);
using XApuDisconnectFn = HRESULT(__stdcall*)(void*, uint32_t);
using XApuEnqueueCommandFn = HRESULT(__stdcall*)(void*, const void*, uint32_t*);
using XApuDequeueResultFn = HRESULT(__stdcall*)(void*, void*);
using XApuGetResultCountFn = uint32_t(__stdcall*)(void*, uint32_t*);

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

// xapu's packed ABI as the title builds and reads it (0x4ee9945 / 0x4f20b7e / 0x4eaa8a3).
constexpr HRESULT kXapuQueueFull = static_cast<HRESULT>(0x8AC81000u);
constexpr HRESULT kXapuPendingResults = static_cast<HRESULT>(0x8AC81001u);
constexpr HRESULT kXapuInvalidCommand = static_cast<HRESULT>(0x8AC81002u);
constexpr HRESULT kXapuInvalidStream = static_cast<HRESULT>(0x8AC81003u);

#pragma pack(push, 1)
struct XApuCommandId {
    uint32_t kind;
    uint32_t stream;
    uint32_t sequence;
    uint32_t options;
    uint64_t context;
};
struct XApuConnectInput {
    uint32_t process;
    uint32_t streams;
    uint32_t commands_per_stream;
    uint32_t memory;
    uint32_t frames_per_output;
    uint32_t options;
};
struct XApuConnectOutput {
    void* memory;
    HANDLE result_event;
};
struct XApuActivateCommand {
    XApuCommandId id;
    uint32_t channels;
    uint32_t init_length;
    const void* init;
    float start_pitch;
    uint32_t processing_length;
    void* processing;
};
struct XApuProcessCommand {
    XApuCommandId id;
    float pitch_ramp;
    float target_pitch;
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t input_length;
    const uint8_t* input;
    uint32_t output_length;
    uint8_t* output;
};
struct XApuResult {
    XApuCommandId id;
    uint32_t state;
    uint64_t state_data;
    uint64_t usec;
    HRESULT error;
    uint32_t frames;
    uint32_t output_length;
    uint8_t* output;
    float pitch;
    uint32_t input_length;
    const uint8_t* input;
};
#pragma pack(pop)
static_assert(sizeof(XApuCommandId) == 0x18, "the command id is 24 bytes");
static_assert(sizeof(XApuConnectInput) == 0x18, "six 32-bit connect fields");
static_assert(sizeof(XApuConnectOutput) == 0x10, "memory and result event");
static_assert(sizeof(XApuActivateCommand) == 0x38, "the activate command");
static_assert(sizeof(XApuProcessCommand) == 0x40, "the process command built at 0x4f20b7e");
static_assert(sizeof(XApuResult) == 0x50, "the result the title reads at 0x4eaa8a3");
static_assert(offsetof(XApuResult, state) == 0x18 && offsetof(XApuResult, state_data) == 0x1c &&
                  offsetof(XApuResult, usec) == 0x24 && offsetof(XApuResult, error) == 0x2c &&
                  offsetof(XApuResult, frames) == 0x30 && offsetof(XApuResult, output_length) == 0x34 &&
                  offsetof(XApuResult, output) == 0x38 && offsetof(XApuResult, pitch) == 0x40 &&
                  offsetof(XApuResult, input_length) == 0x44 && offsetof(XApuResult, input) == 0x48,
              "packed result offsets");

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

    // ---- xapu: the title's connect -> two commands -> two results -> pending ------------------------------------
    XApuConnectInput input{};
    input.process = 1;
    input.streams = 256;
    input.commands_per_stream = 2;
    input.memory = 0x104;  // the title's static input: 4 KiB pages, 0x104000 bytes
    input.options = 1;
    XApuConnectOutput connect_out{};
    void* handle = nullptr;
    check(xconnect(&input, &connect_out, &handle) == S_OK && handle != nullptr && connect_out.memory != nullptr,
          "XApuConnect returns S_OK, a non-null handle and the connect memory");
    check(connect_out.result_event == nullptr, "options=1 does not create a result event");
    auto* memory = static_cast<uint8_t*>(connect_out.memory);
    check(memory && memory[0] == 0 && memory[0x103fff] == 0,
          "the title's memory=0x104 pages is a zeroed 0x104000-byte block");
    check(xconnect(nullptr, &connect_out, &handle) == E_INVALIDARG, "XApuConnect(null input) is E_INVALIDARG");
    check(xconnect(&input, nullptr, &handle) == E_INVALIDARG, "XApuConnect(null output) is E_INVALIDARG");
    check(xconnect(&input, &connect_out, nullptr) == E_INVALIDARG, "XApuConnect(null handle) is E_INVALIDARG");

    XApuActivateCommand activate{};
    activate.id = XApuCommandId{1, 3, 7, 0, 0x1234};
    activate.channels = 2;
    XApuProcessCommand process{};
    process.id = XApuCommandId{3, 3, 8, 0, 0x5678};
    process.target_pitch = 1.0f;
    process.frame_count = 960;
    process.input_length = 8;
    process.input = memory + 0x100;
    process.output_length = 0xf00 * 2;
    process.output = memory + 0x14000;
    uint32_t total = 0;
    check(xenqueue(handle, &activate, &total) == S_OK && total == 1,
          "XApuEnqueueCommand queues an activate and writes total=1");
    check(xenqueue(handle, &process, &total) == S_OK && total == 2,
          "XApuEnqueueCommand queues a process and writes total=2");
    check(xenqueue(handle, &process, &total) == kXapuQueueFull,
          "a third command on the same stream returns QUEUE_FULL");
    XApuProcessCommand stray = process;
    stray.id.stream = 256;
    check(xenqueue(handle, &stray, &total) == kXapuInvalidStream, "stream >= streams returns INVALID_STREAM");
    stray = process;
    stray.id.stream = 4;  // stream 3 is full, so the kind check is reached on an empty stream
    stray.id.kind = 9;
    check(xenqueue(handle, &stray, &total) == kXapuInvalidCommand, "an unknown kind returns INVALID_COMMAND");
    check(xenqueue(handle, nullptr, &total) == E_INVALIDARG, "XApuEnqueueCommand(null command) is E_INVALIDARG");

    uint32_t dequeued = 0xdeadbeef;
    check(xcount(handle, &dequeued) == 2 && dequeued == 0,
          "XApuGetResultCount reports 2 queued and 0 dequeued before the pops");
    XApuResult result{};
    std::memset(&result, 0xcd, sizeof(result));
    check(xdequeue(handle, &result) == S_OK, "XApuDequeueResult pops the activate result");
    check(result.id.kind == 1 && result.id.stream == 3 && result.id.sequence == 7 &&
              result.id.context == 0x1234 && result.error == S_OK,
          "the activate result echoes the 24-byte id and S_OK");
    check(result.state == 0 && result.frames == 0 && result.output_length == 0,
          "the activate result has zero state, frames and output_length");
    std::memset(&result, 0xcd, sizeof(result));
    check(xdequeue(handle, &result) == S_OK, "XApuDequeueResult pops the process result");
    check(result.id.kind == 3 && result.id.stream == 3 && result.id.context == 0x5678 && result.error == S_OK,
          "the process result echoes the id and S_OK");
    check(result.frames == 0 && result.output_length == 0 && result.output == process.output &&
              result.input == process.input && result.input_length == process.input_length,
          "the process result is silent and echoes its buffers and input length");
    check(result.pitch == 1.0f, "the result pitch is the nominal 1.0");
    std::memset(&result, 0xcd, sizeof(result));
    check(xdequeue(handle, &result) == kXapuPendingResults, "an empty queue returns PENDING_RESULTS");
    check(all_bytes(reinterpret_cast<const uint8_t*>(&result), sizeof(result), 0xcd),
          "a pending dequeue writes nothing");
    dequeued = 0;
    check(xcount(handle, &dequeued) == 0 && dequeued == 2,
          "after two pops the count is 0 and the dequeued total is 2");
    check(xdequeue(nullptr, &result) == E_INVALIDARG, "XApuDequeueResult(null handle) is E_INVALIDARG");
    check(xdisconnect(handle, 0x20) == S_OK, "XApuDisconnect frees the connection");
    check(xdisconnect(handle, 0x20) == E_INVALIDARG, "a second XApuDisconnect is E_INVALIDARG");

    // Connect with a byte memory size and the result-event option; a signaled command wakes the event.
    XApuConnectInput event_input{1, 4, 2, 0x1000, 0, 8};
    XApuConnectOutput event_out{};
    void* event_handle = nullptr;
    check(xconnect(&event_input, &event_out, &event_handle) == S_OK && event_out.memory != nullptr &&
              event_out.result_event != nullptr,
          "connect with options=8 creates the result event");
    auto* event_memory = static_cast<uint8_t*>(event_out.memory);
    check(event_memory[0] == 0 && event_memory[0xfff] == 0, "a byte memory size of 0x1000 is taken literally");
    XApuCommandId notify{4, 1, 1, 1, 0};
    check(xenqueue(event_handle, &notify, &total) == S_OK, "a notify command with the signal option queues");
    check(WaitForSingleObject(event_out.result_event, 0) == WAIT_OBJECT_0,
          "the signal option sets the connect result event");
    check(xdequeue(event_handle, &result) == S_OK && result.id.kind == 4, "the notify result is queued");
    check(xdisconnect(event_handle, 0x20) == S_OK, "the event connection disconnects");

    // Invalid connect shapes.
    XApuConnectInput bad{1, 4, 2, 0x1000, 0, 1};
    bad.process = 2;
    check(xconnect(&bad, &connect_out, &handle) == E_NOTIMPL, "process=2 is E_NOTIMPL");
    bad = XApuConnectInput{1, 0, 2, 0x1000, 0, 1};
    check(xconnect(&bad, &connect_out, &handle) == E_INVALIDARG, "streams=0 is E_INVALIDARG");
    bad = XApuConnectInput{1, 4, 17, 0x1000, 0, 1};
    check(xconnect(&bad, &connect_out, &handle) == E_INVALIDARG, "commands_per_stream=17 is E_INVALIDARG");
    bad = XApuConnectInput{1, 4, 2, 0x1000, 1, 1};
    check(xconnect(&bad, &connect_out, &handle) == E_INVALIDARG, "frames_per_output=1 is E_INVALIDARG");

    // ---- chad DSP commands --------------------------------------------------------------------------------------
    uint8_t context[0x500];
    std::memset(context, 0, sizeof(context));
    void* dsp_handle = context;
    uint8_t dsp_command[0x40] = {};
    check(dconnect(0x1122334455667788ull, 0x20, 0x40, 0x80, 1, context) == 0,
          "XDspConnectWithMaximumStreamLimit(..., 0x80, ...) returns 0 (success)");
    check(dactivate(dsp_handle, 1, 2, 3) == 0, "XDspActivate returns 0");
    check(dsubmit(dsp_handle, dsp_command) == 0, "XDspSubmitCommand returns 0");
    check(ddeactivate(dsp_handle) == 0, "XDspDeactivate returns 0");
    check(ddisconnect(dsp_handle, 0x20) == 0, "XDspDisconnect returns 0");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
