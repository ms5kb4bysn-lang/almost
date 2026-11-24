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
// XApuConnect(queue_a, queue_b, void** out_handle) -> HRESULT                call 0x43aee69
//   rcx = 0x9ae9890, rdx = 0x9ae98a8 (two queue descriptors) and r8 = 0x9ae8960. 0x9ae8960 is the handle every other
//   xapu call reads (Disconnect 0x43ae924, Enqueue sites, GetResultCount/Dequeue 0x4eaa849/0x4eaa890), so the third
//   argument is the out handle. The shim writes a static dummy handle there and returns S_OK.
//   The second argument is also written: its first qword is read as the command-buffer base at 0x4e64b45, 0x4f20afe
//   and 0x4f20b64, the init at 0x5a8797c zeroes it and no other direct write in the image was found, so connect is the
//   likely filler. The shim stores a zeroed 0x200000-byte block there (0x14000 + 0xff * 0x1e00 = 0x1f4000 is the
//   largest offset the title computes). Hypothesis: that this slot is a connect out structure; the buffer itself is
//   only ever memcpy'd into by the title and passed back in commands, which this shim drops.
//
// XApuDisconnect(handle, 0x20) -> HRESULT                                    call 0x43ae930
//   Also frees the dummy command buffer the connect above allocated.
// XApuEnqueueCommand(handle, command*, 0) -> HRESULT                         calls 0x4ee9978, 0x4f2092d, 0x4f20bb8, 0x483d5
// XApuDequeueResult(handle, result*) -> HRESULT                              call 0x4eaa89a, negative result tested
// XApuGetResultCount(handle) -> uint32                                       call 0x4eaa86b
//   The command queue is connected once and torn down once. The shim accepts every command and reports no results:
//   GetResultCount returns 0, so the dequeue loop exits without calling DequeueResult; DequeueResult itself returns
//   S_FALSE (1, non-negative) and writes nothing if it is called anyway.
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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
uint8_t g_xapu_handle[0x100] = {};  // opaque dummy; the title only stores and passes the pointer
// The title reads its xapu command-buffer base from the second XApuConnect argument (0x9ae98a8), as a qword at +0,
// at 0x4e64b45, 0x4f20afe and 0x4f20b64. Its init at 0x5a8797c zeroes that qword and no other direct write was found,
// so connect is the likely writer. 0x200000 covers the largest offset the title computes:
// 0x14000 + 0xff * 0x1e00 (256 streams, the count written at 0x5a87a4d) = 0x1f4000.
constexpr size_t kXapuCommandBufferSize = 0x200000;
void* g_xapu_command_buffer = nullptr;

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

HRESULT __stdcall XApuConnect(void* queue_a, void* queue_b, void** out_handle) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    if (!out_handle) return E_POINTER;

    void* buffer = nullptr;
    {
        const std::lock_guard lock(g_mutex);
        if (!g_xapu_command_buffer) g_xapu_command_buffer = alloc_zeroed(kXapuCommandBufferSize);
        buffer = g_xapu_command_buffer;
    }
    if (!buffer) return E_OUTOFMEMORY;
    if (queue_b) std::memcpy(queue_b, &buffer, sizeof(buffer));  // see the comment on g_xapu_command_buffer
    *out_handle = g_xapu_handle;
    log_once(logged, caller, "XApuConnect(queue_a=%p, queue_b=%p, out=%p) -> S_OK, handle %p, buffer %p", queue_a,
             queue_b, static_cast<void*>(out_handle), static_cast<void*>(g_xapu_handle), buffer);
    return S_OK;
}

HRESULT __stdcall XApuDisconnect(void* handle, uint32_t flags) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    void* buffer = nullptr;
    {
        const std::lock_guard lock(g_mutex);
        buffer = g_xapu_command_buffer;
        g_xapu_command_buffer = nullptr;
    }
    if (buffer) _aligned_free(buffer);
    log_once(logged, caller, "XApuDisconnect(handle=%p, flags=0x%x) -> S_OK, freed buffer %p", handle, flags, buffer);
    return S_OK;
}

HRESULT __stdcall XApuEnqueueCommand(void* handle, const void* command, uint32_t flags) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller, "XApuEnqueueCommand(handle=%p, command=%p, flags=0x%x) -> S_OK (dropped)", handle,
             command, flags);
    return S_OK;
}

HRESULT __stdcall XApuDequeueResult(void* handle, void* result) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller, "XApuDequeueResult(handle=%p, result=%p) -> S_FALSE (no result, nothing written)", handle,
             result);
    return S_FALSE;
}

uint32_t __stdcall XApuGetResultCount(void* handle) {
    static std::atomic<bool> logged{false};
    const void* caller = _ReturnAddress();
    log_once(logged, caller, "XApuGetResultCount(handle=%p) -> 0", handle);
    return 0;
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
