// AcpHal.dll, xapu.dll and chad.dll: the console audio coprocessor's hardware abstraction and command queues.
//
// This is a BOOT BRING-UP SHIM. It hands the title non-null, zeroed, consistent objects and success results so the
// audio initialisation path can run to its end on a PC that has no APU. Nothing here processes audio, synthesises
// audio commands or touches an XMA/APU buffer: the command queues accept and drop commands, and the mapping functions
// hand out stable dummy ids. It exists because the retail executable allocates its audio state through these imports
// and dereferences the results immediately; with the imports unimplemented (returning 0) it memsets through a null
// shape-contexts pointer right after AcpHalAllocateShapeContexts and dies at 0xc0000005.
//
// The shapes below are read from the retail executable (1.0.2.1) at these RVAs, statically; `docs/audio_hal_exe_calls.md`
// has the import census and `runtime/src/pe_loader.cpp` the resolver. Argument order is from the registers set before
// each call, not from public documentation (there is none).
//
// AcpHalCreate(void** out, 0) -> HRESULT                                      call 0x43aed98
//   *out = object at 0x9ae98b8. The title calls vtable slot 0 as (this, 0x1000, 0x1000) at 0x43aedba, slot 2 as
//   (this, command, ...) at 0x43aee2f / 0x78521 / 0x92619 / 0x43a7416, and slot 1 and slot 5 during teardown at
//   0x43ae84f and 0x43ae88b. Every slot here returns 0 and the object is 0x1000 zeroed bytes with that vtable.
//
// AcpHalAllocateShapeContexts(desc) -> HRESULT                                call 0x43aec73
//   The argument is NOT an out pointer. At 0x43aec4b rcx is the address of a static descriptor (0x9ae98d0); the
//   title writes a qword count 0x15e at desc+0x10 and reads it back as the element count at 0x43aec80, and reads the
//   array pointer this call must write from desc+0x38 at 0x43aec87. It then memsets count * 0x40 bytes at 0x43aec94
//   (return address 0x43aec99, the crash in the report). The shim allocates max(0x1000, count * 0x40) zeroed bytes
//   and stores that pointer at desc+0x38, leaving the rest of the descriptor alone. The layout is measured; the field
//   meanings are not.
//
// AcpHalReleaseShapeContexts(handle) -> HRESULT                               call 0x43ae899, result unused
//   The call site does not reload rcx after the AcpHal object's slot 5, so the argument is not reliable. The shim
//   ignores it and frees the last shape-contexts block (there is one measured allocate site and one release).
//
// ApuCreateHeap(size, 0x226e00) -> HRESULT                                   call 0x43ad933
//   No out pointer: rcx is `0x1c7` or `0xff` from 0x25a8750, shifted left 20 (0x1c700000 / 0xff00000, sizes in
//   bytes), and rdx is 0x226e00. The shim keeps a non-null internal heap descriptor with those sizes and returns
//   S_OK. Treating rcx as an out pointer would write to a 476 MiB address.
//
// ApuAlloc(void** out, 0, size, 0x800) -> address                              call 0x43a6126 (wrapper 0x43a6110)
//   The first argument is an out pointer, not a heap handle: the wrapper is called at 0x43aed13 with rcx pointing at
//   a zeroed stack qword, and the caller reads [rsi] into r15 at 0x43aed18 and memsets through it. A second caller at
//   0x1e74620 passes the address of a global. The shim allocates a zeroed, 0x1000-aligned block of `size` bytes,
//   stores it in *out and also returns it (the measured callers ignore rax; the return contract is unmeasured).
//
// ApuFree(ptr)                                                                tail jump 0x43aea10
//   One argument. The title also passes interior pointers (base + 0x1700) from its 0x1900-byte records, so an
//   interior pointer is a no-op; only an exact allocation base is freed. Unknown pointers are ignored, never freed.
//
// ApuIsVirtualAddressValid(ptr, 0x40 | 0x80 | 0x800) -> byte                 calls 0x78790, 0x787e3, 0x78afd, 0x78b41
// ApuMapVirtualAddress(ptr) -> uint32                                        calls 0x7879c, 0x787ef, 0x78b09, 0x78b4d
//   The map call reloads only rcx, so it takes the pointer alone (the size constant is the caller's leftover edx).
//   The shim keeps a table keyed by pointer: the same pointer always gets the same id, and MapApuAddress(id) gives
//   the pointer back. Ids start at 0x1000; 0 means no mapping.
//
// ApuMapApuAddress(u32) -> pointer                                           calls 0x783f6, 0x78485
//   Returns nullptr for an id the shim did not hand out; both measured sites test for null and skip their copy.
//
// XApuConnect(input*, output*, handle**) -> HRESULT                            call 0x43aee69
//   input is the 24-byte block at RVA 0x9ae9890: process=1, streams=0x100, commands_per_stream=2, memory=0x104,
//   frames_per_output=0, options=1 (so no result event is created). The memory field is 4 KiB pages (0x104000 bytes);
//   the title's buffer arithmetic at 0x4e64b97 / 0x4f20b71 reaches 0x14000 + stream * 0x1e00, so the block must be
//   that size. output at 0x9ae98a8 is {void* memory; HANDLE result_event}; the title reads memory at +0. The third
//   argument is the out handle (0x9ae8960), which every other xapu call reads. Each connect is a separate object.
//
// XApuDisconnect(handle, 0x20) -> HRESULT                                    call 0x43ae930
//   Closes the result event, frees the connect memory and destroys the connection.
//
// XApuEnqueueCommand(handle, command*, u32* total) -> HRESULT                 calls 0x4ee9978, 0x4f2092d, 0x4f20bb8, 0x483d5
// XApuDequeueResult(handle, result*) -> HRESULT                              call 0x4eaa89a, negative result tested
// XApuGetResultCount(handle, u32* total) -> uint32                           call 0x4eaa86b
//   Commands start with a 24-byte packed id {kind, stream, sequence, options, context}; process commands are 0x40
//   bytes and results 0x50. The shim keeps a per-stream queue bounded by commands_per_stream and a FIFO of results:
//   - enqueue validates stream < streams and the per-stream queue, then queues one result per command. Activate,
//     deactivate, notify and reset echo the id with S_OK. Process echoes the command's input/output pointers and
//     input_length, with frames and output_length zero: there is no Opus decoder, so the result is valid silence.
//     Invalid kinds return 0x8AC81002, an invalid stream 0x8AC81003, a full per-stream queue 0x8AC81000.
//   - *total receives the running accepted-command count when non-null.
//   - dequeue pops the FIFO and writes the packed 0x50-byte result; when empty it returns 0x8AC81001
//     (PENDING_RESULTS), the value the title polls for. GetResultCount returns the queued count and writes the
//     dequeued total.
//   The packed sizes and offsets are the public VIctory fork's facts and are checked against the title: the command
//   builders above and the result consumer at 0x4eaa8a3 read id.kind at +0, id.stream at +4 and output_length at
//   +0x34. No audio is decoded; every process result is silence.
//
// XDspConnectWithMaximumStreamLimit(a, b, c, 0x80, 1, context) -> int         call 0x5b5262e
//   Six arguments; the call compares the result with 0 and treats 0 as success (sete). 0 is returned.
// XDspActivate(...) -> int                                                   call 0x11d475d, result tested (0 succeeds)
// XDspSubmitCommand(a, b) -> int                                             call 0x4e5b20d, 0 is success (sete)
// XDspDeactivate(handle) -> int                                              tail jump 0x1166020
// XDspDisconnect(handle, reason) -> int                                      tail jump 0x43aea00
//   All return 0. No DSP streams, commands or results are modelled.
//
// Logging: the first call of each export prints its arguments and the value written to stderr, once per process, so
// the next boot shows which shapes actually ran. There is no per-call logging.
#include <windows.h>
#include <intrin.h>
#include <malloc.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shims.h"

namespace {

constexpr size_t kAcpHalObjectSize = 0x1000;
constexpr size_t kShapeContextSize = 0x40;  // the title memsets count * this many bytes
constexpr size_t kMinBlock = 0x1000;
constexpr uint32_t kMaxShapeContexts = 0x10000;  // 4 MiB table; the measured count is 0x15e
constexpr uint32_t kFirstApuId = 0x1000;

std::mutex g_mutex;
void* g_shape_contexts = nullptr;
size_t g_shape_contexts_size = 0;
std::unordered_set<void*> g_acphal_objects;
std::unordered_map<void*, size_t> g_apu_blocks;  // exact base -> size
std::unordered_map<const void*, uint32_t> g_virtual_to_apu;
std::unordered_map<uint32_t, void*> g_apu_to_virtual;
uint32_t g_next_apu_id = kFirstApuId;

// ---- xapu: the title's command/result ABI ----------------------------------------------------------------
//
// The title's connect input is the 24-byte block at RVA 0x9ae9890: process=1, streams=0x100,
// commands_per_stream=2, memory=0x104, frames_per_output=0, options=1. The memory field is 4 KiB pages there:
// 0x104 << 12 = 0x104000 bytes, the size its buffer arithmetic needs. Callers that already pass bytes (0x1000 or
// more) are taken literally. The connect output at 0x9ae98a8 is {void* memory; HANDLE event}; the title reads the
// memory pointer at +0 (0x4e64b45, 0x4f20afe, 0x4f20b64).
//
// Commands and results are the packed layouts the title builds at 0x4ee9945 / 0x4f2091d / 0x4f20b7e and reads at
// 0x4eaa8a3: a 24-byte CommandId (kind, stream, sequence, options, context), a 0x40-byte process command, and a
// 0x50-byte result. The public VIctory fork documents the same sizes (facts only); the result offsets below are
// asserted at compile time and checked against the title's reads.
constexpr HRESULT kXapuQueueFull = static_cast<HRESULT>(0x8AC81000u);
constexpr HRESULT kXapuPendingResults = static_cast<HRESULT>(0x8AC81001u);
constexpr HRESULT kXapuInvalidCommand = static_cast<HRESULT>(0x8AC81002u);
constexpr HRESULT kXapuInvalidStream = static_cast<HRESULT>(0x8AC81003u);

constexpr uint32_t kXapuDecodeOpus = 1;
constexpr uint32_t kXapuActivate = 1;
constexpr uint32_t kXapuDeactivate = 2;
constexpr uint32_t kXapuProcess = 3;
constexpr uint32_t kXapuNotify = 4;
constexpr uint32_t kXapuReset = 5;
constexpr uint32_t kXapuSignalOption = 1;      // command options: signal the result event
constexpr uint32_t kXapuResultEventOption = 8; // connect options: create output.result_event
constexpr size_t kXapuMinMemory = 0x10000;      // a zero memory field still gets a usable buffer
constexpr size_t kXapuPageShift = 12;
constexpr size_t kXapuPageThreshold = 0x1000;   // below this the title's memory field is pages
constexpr size_t kXapuMaxMemory = 0x40000000u;  // 1 GiB cap for an unmeasured garbage size

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
static_assert(sizeof(XApuCommandId) == 0x18, "the title's command id is 24 bytes (4 dwords + a qword)");
static_assert(sizeof(XApuConnectInput) == 0x18, "six 32-bit connect fields");
static_assert(sizeof(XApuConnectOutput) == 0x10, "memory pointer and result event");
static_assert(sizeof(XApuProcessCommand) == 0x40, "the process command built at 0x4f20b7e is 0x40 bytes");
static_assert(sizeof(XApuResult) == 0x50, "the result the title reads at 0x4eaa8a3 is 0x50 bytes");
static_assert(offsetof(XApuResult, state) == 0x18 && offsetof(XApuResult, state_data) == 0x1c &&
                  offsetof(XApuResult, usec) == 0x24 && offsetof(XApuResult, error) == 0x2c &&
                  offsetof(XApuResult, frames) == 0x30 && offsetof(XApuResult, output_length) == 0x34 &&
                  offsetof(XApuResult, output) == 0x38 && offsetof(XApuResult, pitch) == 0x40 &&
                  offsetof(XApuResult, input_length) == 0x44 && offsetof(XApuResult, input) == 0x48,
              "packed result offsets");

struct XApuConnection {
    void* memory = nullptr;
    size_t memory_size = 0;
    HANDLE result_event = nullptr;
    uint32_t streams = 0;
    uint32_t commands_per_stream = 0;
    uint32_t frames_per_output = 0;
    uint32_t options = 0;
    std::vector<uint32_t> queued;   // per stream
    std::deque<XApuResult> results; // FIFO of undelivered results
    uint32_t commands = 0;
    uint32_t dequeued = 0;
};
std::mutex g_xapu_mutex;
std::unordered_set<XApuConnection*> g_xapu_connections;

struct ApuHeap {
    uint32_t bytes;
    uint32_t reserved;
    bool created;
};
ApuHeap g_heap = {0, 0, false};

void log_once(std::atomic<bool>& logged, const void* caller, const char* fmt, ...) {
    if (logged.exchange(true, std::memory_order_relaxed)) return;
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[rt] acphal: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, " (caller %s, logged once)\n",
                 rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
    va_end(args);
}

void* alloc_zeroed(size_t size, size_t alignment = kMinBlock) {
    void* p = _aligned_malloc(size ? size : 1, alignment);
    if (p) std::memset(p, 0, size ? size : 1);
    return p;
}

// ---- AcpHal object: a 0x1000-byte zeroed block with a vtable of no-ops ------------------------------------------

template <size_t Slot>
intptr_t __stdcall acphal_object_slot(void*) {
    return 0;
}

intptr_t __stdcall acphal_object_release(void* self) {
    bool ours = false;
    {
        const std::lock_guard lock(g_mutex);
        ours = g_acphal_objects.erase(self) != 0;
    }
    if (ours) _aligned_free(self);
    return 0;
}

template <size_t Slot>
constexpr void* acphal_slot_entry() {
    if constexpr (Slot == 5) {
        return reinterpret_cast<void*>(&acphal_object_release);
    } else {
        return reinterpret_cast<void*>(&acphal_object_slot<Slot>);
    }
}

template <size_t... Slot>
constexpr auto make_acphal_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{acphal_slot_entry<Slot>()...};
}

const auto g_acphal_vtable = make_acphal_vtable(std::make_index_sequence<32>{});

HRESULT __stdcall AcpHalCreate(void** out, uint32_t flags) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!out) return E_POINTER;
    void* object = alloc_zeroed(kAcpHalObjectSize);
    if (!object) return E_OUTOFMEMORY;
    *static_cast<void* const**>(object) = g_acphal_vtable.data();
    {
        const std::lock_guard lock(g_mutex);
        g_acphal_objects.insert(object);
    }
    *out = object;
    log_once(logged, caller, "AcpHalCreate(out=%p, flags=0x%x) -> S_OK, object %p", static_cast<void*>(out), flags,
             object);
    return S_OK;
}

// The title's descriptor around the call at 0x43aec4b: count at +0x10, array pointer written at +0x38.
HRESULT __stdcall AcpHalAllocateShapeContexts(void* descriptor) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!descriptor) return E_POINTER;

    auto* base = static_cast<uint8_t*>(descriptor);
    uint64_t raw_count = 0;
    std::memcpy(&raw_count, base + 0x10, sizeof(raw_count));
    uint32_t count = raw_count > 0xffffffffu ? kMaxShapeContexts : static_cast<uint32_t>(raw_count);
    if (count == 0) count = 1;
    if (count > kMaxShapeContexts) count = kMaxShapeContexts;  // a sane cap for an unmeasured garbage count

    const size_t bytes = count > kMinBlock / kShapeContextSize ? static_cast<size_t>(count) * kShapeContextSize
                                                               : kMinBlock;
    void* block = alloc_zeroed(bytes);
    if (!block) return E_OUTOFMEMORY;

    {
        const std::lock_guard lock(g_mutex);
        if (g_shape_contexts) _aligned_free(g_shape_contexts);
        g_shape_contexts = block;
        g_shape_contexts_size = bytes;
    }
    std::memcpy(base + 0x38, &block, sizeof(block));

    log_once(logged, caller, "AcpHalAllocateShapeContexts(desc=%p, count=%u) -> S_OK, contexts %p (%zu bytes)",
             descriptor, count, block, bytes);
    return S_OK;
}

HRESULT __stdcall AcpHalReleaseShapeContexts(void* handle) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    void* freed = nullptr;
    size_t bytes = 0;
    {
        const std::lock_guard lock(g_mutex);
        freed = g_shape_contexts;
        bytes = g_shape_contexts_size;
        g_shape_contexts = nullptr;
        g_shape_contexts_size = 0;
    }
    if (freed) _aligned_free(freed);
    log_once(logged, caller, "AcpHalReleaseShapeContexts(handle=%p) -> S_OK, freed %p (%zu bytes)", handle, freed,
             bytes);
    return S_OK;
}

// ---- APU heap and buffers ---------------------------------------------------------------------------------------

HRESULT __stdcall ApuCreateHeap(uint32_t bytes, uint32_t reserved) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    {
        const std::lock_guard lock(g_mutex);
        g_heap.bytes = bytes;
        g_heap.reserved = reserved;
        g_heap.created = true;
    }
    log_once(logged, caller, "ApuCreateHeap(size=0x%x, second=0x%x) -> S_OK, internal heap %p", bytes, reserved,
             static_cast<void*>(&g_heap));
    return S_OK;
}

// ApuAlloc(out, 0, size, 0x800): writes the block to *out and returns the same pointer. The measured callers read
// *out; rax is ignored at both sites, so the return contract is a hypothesis (the block address is the useful value).
void* __stdcall ApuAlloc(void** out, uint32_t zero, size_t size, uint32_t alignment) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!out) return nullptr;
    void* block = alloc_zeroed(size);
    if (!block) {
        *out = nullptr;
        return nullptr;
    }
    {
        const std::lock_guard lock(g_mutex);
        g_apu_blocks[block] = size;
    }
    *out = block;
    log_once(logged, caller, "ApuAlloc(out=%p, zero=0x%x, size=0x%zx, align=0x%x) -> %p", static_cast<void*>(out),
             zero, size, alignment, block);
    return block;
}

void __stdcall ApuFree(void* pointer) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    bool freed = false;
    {
        const std::lock_guard lock(g_mutex);
        const auto it = g_apu_blocks.find(pointer);
        if (it != g_apu_blocks.end()) {
            g_apu_blocks.erase(it);
            freed = true;
        }
    }
    if (freed) _aligned_free(pointer);
    log_once(logged, caller, "ApuFree(%p) -> freed %s (interior and unknown pointers are left alone)", pointer,
             freed ? "yes" : "no");
}

BOOL __stdcall ApuIsVirtualAddressValid(const void* pointer, uint32_t size) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    const BOOL valid = pointer != nullptr;
    log_once(logged, caller, "ApuIsVirtualAddressValid(%p, 0x%x) -> %d", pointer, size, valid);
    return valid;
}

uint32_t __stdcall ApuMapVirtualAddress(const void* pointer) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!pointer) return 0;
    uint32_t id = 0;
    {
        const std::lock_guard lock(g_mutex);
        const auto it = g_virtual_to_apu.find(pointer);
        if (it != g_virtual_to_apu.end()) {
            id = it->second;
        } else {
            id = g_next_apu_id++;
            if (g_next_apu_id == 0) g_next_apu_id = kFirstApuId;
            while (g_apu_to_virtual.count(id)) id = g_next_apu_id++;
            g_virtual_to_apu.emplace(pointer, id);
            g_apu_to_virtual.emplace(id, const_cast<void*>(pointer));
        }
    }
    log_once(logged, caller, "ApuMapVirtualAddress(%p) -> id 0x%x", pointer, id);
    return id;
}

void* __stdcall ApuMapApuAddress(uint32_t id) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    void* pointer = nullptr;
    {
        const std::lock_guard lock(g_mutex);
        const auto it = g_apu_to_virtual.find(id);
        if (it != g_apu_to_virtual.end()) pointer = it->second;
    }
    log_once(logged, caller, "ApuMapApuAddress(0x%x) -> %p", id, pointer);
    return pointer;
}

// ---- xapu command queue -----------------------------------------------------------------------------------------

XApuConnection* find_xapu_connection_locked(void* handle) {
    if (!handle) return nullptr;
    const auto it = g_xapu_connections.find(static_cast<XApuConnection*>(handle));
    return it == g_xapu_connections.end() ? nullptr : *it;
}

HRESULT __stdcall XApuConnect(const void* input_ptr, void* output_ptr, void** out_handle) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!input_ptr || !output_ptr || !out_handle) return E_INVALIDARG;

    XApuConnectInput input{};
    std::memcpy(&input, input_ptr, sizeof(input));
    if (input.process != kXapuDecodeOpus) return E_NOTIMPL;
    if (input.streams == 0 || input.streams > 500 || input.commands_per_stream == 0 ||
        input.commands_per_stream > 16 || input.frames_per_output != 0)
        return E_INVALIDARG;

    // The title's static input has memory = 0x104, which is 4 KiB pages (0x104000 bytes); its buffer arithmetic
    // reaches past 0x14000 + stream * 0x1e00. A byte size of 0x1000 or more is taken literally.
    uint64_t memory_bytes = input.memory;
    if (memory_bytes != 0 && memory_bytes < kXapuPageThreshold) memory_bytes <<= kXapuPageShift;
    if (memory_bytes == 0) memory_bytes = kXapuMinMemory;
    if (memory_bytes > kXapuMaxMemory) return E_INVALIDARG;

    void* memory = alloc_zeroed(static_cast<size_t>(memory_bytes), 16);
    if (!memory) return E_OUTOFMEMORY;

    HANDLE event = nullptr;
    if (input.options & kXapuResultEventOption) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            _aligned_free(memory);
            return FAILED(hr) ? hr : E_FAIL;
        }
    }

    XApuConnection* connection = new (std::nothrow) XApuConnection;
    if (!connection) {
        if (event) CloseHandle(event);
        _aligned_free(memory);
        return E_OUTOFMEMORY;
    }
    connection->memory = memory;
    connection->memory_size = static_cast<size_t>(memory_bytes);
    connection->result_event = event;
    connection->streams = input.streams;
    connection->commands_per_stream = input.commands_per_stream;
    connection->frames_per_output = input.frames_per_output;
    connection->options = input.options;
    try {
        connection->queued.assign(input.streams, 0);
    } catch (...) {
        if (event) CloseHandle(event);
        _aligned_free(memory);
        delete connection;
        return E_OUTOFMEMORY;
    }

    {
        const std::lock_guard lock(g_xapu_mutex);
        g_xapu_connections.insert(connection);
    }

    XApuConnectOutput output{memory, event};
    std::memcpy(output_ptr, &output, sizeof(output));
    *out_handle = connection;
    log_once(logged, caller,
             "XApuConnect(process=%u, streams=%u, commands_per_stream=%u, memory=0x%x/0x%zx bytes, "
             "frames_per_output=%u, options=0x%x) -> S_OK, memory %p, event %p, handle %p",
             input.process, input.streams, input.commands_per_stream, input.memory,
             static_cast<size_t>(memory_bytes), input.frames_per_output, input.options, memory, event,
             static_cast<void*>(connection));
    return S_OK;
}

HRESULT __stdcall XApuDisconnect(void* handle, uint32_t flags) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    XApuConnection* connection = nullptr;
    {
        const std::lock_guard lock(g_xapu_mutex);
        connection = find_xapu_connection_locked(handle);
        if (!connection) return E_INVALIDARG;
        g_xapu_connections.erase(connection);
    }
    const HANDLE event = connection->result_event;
    void* memory = connection->memory;
    const size_t memory_size = connection->memory_size;
    if (event) CloseHandle(event);
    _aligned_free(memory);
    delete connection;
    log_once(logged, caller, "XApuDisconnect(handle=%p, flags=0x%x) -> S_OK, freed memory %p (0x%zx bytes), event %p",
             handle, flags, memory, memory_size, event);
    return S_OK;
}

HRESULT __stdcall XApuEnqueueCommand(void* handle, const void* command_ptr, uint32_t* total) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!handle || !command_ptr) return E_INVALIDARG;

    XApuCommandId id{};
    std::memcpy(&id, command_ptr, sizeof(id));

    size_t queued_after = 0;
    uint32_t total_after = 0;
    {
        const std::lock_guard lock(g_xapu_mutex);
        XApuConnection* connection = find_xapu_connection_locked(handle);
        if (!connection) return E_INVALIDARG;
        if (id.stream >= connection->streams) return kXapuInvalidStream;
        if (connection->queued[id.stream] >= connection->commands_per_stream) return kXapuQueueFull;

        XApuResult result{};
        result.id = id;
        result.error = S_OK;
        result.pitch = 1.0f;
        switch (id.kind) {
            case kXapuActivate:
            case kXapuDeactivate:
            case kXapuNotify:
            case kXapuReset:
                break;
            case kXapuProcess: {
                XApuProcessCommand command{};
                std::memcpy(&command, command_ptr, sizeof(command));
                // No Opus decoder: the result is valid but silent. Echo the packet/output buffers the title gave
                // us, so its consumers keep valid pointers; zero frames and output length mean no PCM was produced.
                result.input = command.input;
                result.input_length = command.input_length;
                result.output = command.output;
                result.output_length = 0;
                result.frames = 0;
                break;
            }
            default:
                return kXapuInvalidCommand;
        }

        connection->queued[id.stream]++;
        connection->commands++;
        connection->results.push_back(result);
        if (total) *total = connection->commands;
        if ((id.options & kXapuSignalOption) && connection->result_event) SetEvent(connection->result_event);
        queued_after = connection->results.size();
        total_after = connection->commands;
    }

    log_once(logged, caller,
             "XApuEnqueueCommand(handle=%p, kind=%u, stream=%u, sequence=%u, options=0x%x, context=0x%llx) -> S_OK, "
             "queued %zu, total %u",
             handle, id.kind, id.stream, id.sequence, id.options, static_cast<unsigned long long>(id.context),
             queued_after, total_after);
    return S_OK;
}

HRESULT __stdcall XApuDequeueResult(void* handle, void* result_ptr) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!handle || !result_ptr) return E_INVALIDARG;

    XApuResult result{};
    uint32_t kind = 0, stream = 0;
    size_t left = 0;
    {
        const std::lock_guard lock(g_xapu_mutex);
        XApuConnection* connection = find_xapu_connection_locked(handle);
        if (!connection) return E_INVALIDARG;
        if (connection->results.empty()) return kXapuPendingResults;
        result = connection->results.front();
        connection->results.pop_front();
        if (result.id.stream < connection->queued.size()) connection->queued[result.id.stream]--;
        connection->dequeued++;
        kind = result.id.kind;
        stream = result.id.stream;
        left = connection->results.size();
    }
    std::memcpy(result_ptr, &result, sizeof(result));
    log_once(logged, caller, "XApuDequeueResult(handle=%p, result=%p) -> S_OK, kind=%u stream=%u, %zu left", handle,
             result_ptr, kind, stream, left);
    return S_OK;
}

uint32_t __stdcall XApuGetResultCount(void* handle, uint32_t* total) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    uint32_t count = 0;
    uint32_t dequeued = 0;
    {
        const std::lock_guard lock(g_xapu_mutex);
        XApuConnection* connection = find_xapu_connection_locked(handle);
        if (!connection) return 0;
        count = static_cast<uint32_t>(connection->results.size());
        dequeued = connection->dequeued;
        if (total) *total = dequeued;
    }
    log_once(logged, caller, "XApuGetResultCount(handle=%p, total=%p) -> %u queued, %u dequeued", handle,
             static_cast<void*>(total), count, dequeued);
    return count;
}


// ---- chad DSP commands ------------------------------------------------------------------------------------------

int __stdcall XDspConnectWithMaximumStreamLimit(uint64_t a, uint32_t b, uint32_t c, uint32_t max_streams,
                                                uint32_t fourth, void* context) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller,
             "XDspConnectWithMaximumStreamLimit(0x%llx, 0x%x, 0x%x, 0x%x, 0x%x, ctx=%p) -> 0 (success)",
             static_cast<unsigned long long>(a), b, c, max_streams, fourth, context);
    return 0;
}

int __stdcall XDspActivate(void* handle, uint32_t a, uint32_t b, uint32_t c) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller, "XDspActivate(handle=%p, 0x%x, 0x%x, 0x%x) -> 0 (success)", handle, a, b, c);
    return 0;
}

int __stdcall XDspSubmitCommand(void* a, void* b) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller, "XDspSubmitCommand(%p, %p) -> 0 (success, dropped)", a, b);
    return 0;
}

int __stdcall XDspDeactivate(void* handle) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller, "XDspDeactivate(handle=%p) -> 0", handle);
    return 0;
}

int __stdcall XDspDisconnect(void* handle, uint32_t reason) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller, "XDspDisconnect(handle=%p, reason=0x%x) -> 0", handle, reason);
    return 0;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
);

SHIM_TABLE(xapu,
    {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&XApuConnect)},
    {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&XApuDisconnect)},
    {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&XApuEnqueueCommand)},
    {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&XApuDequeueResult)},
    {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&XApuGetResultCount)},
);

SHIM_TABLE(chad,
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", reinterpret_cast<void*>(&XDspConnectWithMaximumStreamLimit)},
    {"chad.dll", "XDspActivate", reinterpret_cast<void*>(&XDspActivate)},
    {"chad.dll", "XDspSubmitCommand", reinterpret_cast<void*>(&XDspSubmitCommand)},
    {"chad.dll", "XDspDeactivate", reinterpret_cast<void*>(&XDspDeactivate)},
    {"chad.dll", "XDspDisconnect", reinterpret_cast<void*>(&XDspDisconnect)},
);
