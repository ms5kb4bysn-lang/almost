// Xbox-ABI queue and fence proxies (d3d12_xs_device.h) over synthetic COM objects, with no D3D12 runtime and no GPU:
//   - devices that stop at ID3D12Device, Device1, Device3 and Device9: the slots of a missing interface (45/46,
//     50, 75) are refused without reaching the device, the others get the real objects behind the proxies;
//   - fences with and without ID3D12Fence1: 11 or 12 public slots, QueryInterface and slot 11 to match;
//   - creation that fails, that returns S_OK with no object, a null IID or output, and an interface the real object
//     has but the proxy does not model;
//   - OpenSharedHandle of a fence (as ID3D12Fence, IUnknown, Fence1), of a non-fence object, of an invalid handle,
//     and with a null output or IID, with the references of every object it touches;
//   - allocation failures in the proxy (operator new, the arrays it unwraps), injected in this executable only;
//   - struct-returning slots 25/26/43 with the return pointer in RCX (the title's order) or RDX, console queue flags,
//     the list proxy's console-to-PC slot map, and the slot 133 / placed-heap (slot 14) backend hooks;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each).
// Every device, queue, fence and resource here is a mock that records the calls it gets. No game data, no Xbox DLLs.
#include "../shims/d3d12_xs_device.h"

#include <d3d12.h>
#include <windows.h>

#include <crtdbg.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace {

const char* g_label = "xs_proxy_mock";

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", g_label, __LINE__, #condition); return 1; } \
} while (0)

// ---- Allocation failures: this executable's global operator new fails the next g_fail_news calls ----

int g_fail_news = 0;     // single-threaded: armed right before the call it is meant for
int g_failed_news = 0;   // allocations refused so far

}  // namespace

// The annotations are vcruntime_new.h's, so /analyze sees the same contract as for the CRT's own operator new.
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
    if (g_fail_news > 0) {
        --g_fail_news;
        ++g_failed_news;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
_NODISCARD _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void __CRTDECL operator delete(void* p) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, std::size_t) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace xs = rt::d3d12_xs;

// ---- Mocks ----

enum class Kind { device, queue, fence, resource, allocator, command_list, heap };

constexpr unsigned kMockSlots = 128;
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};

struct Mock {
    void** vtable;
    Kind kind;
    long refs;
    int device_level;        // device: highest ID3D12DeviceN it answers (0 = ID3D12Device only)
    int list_level;          // command_list: highest ID3D12GraphicsCommandListN it answers (0 = base list only)
    bool fence1;             // fence: answers ID3D12Fence1; device: the fences it makes do
    bool allocator_tag;      // allocator: answers kLeonidaCommandAllocatorTag; device: the allocators it makes do
    bool create_null;        // device: creation returns create_result with no object
    HRESULT create_result;   // device: what creation returns (S_OK makes the object)
    Mock* last_child;        // device: the object its last creation or open made
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][6];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
    D3D12_COMMAND_QUEUE_DESC queue_desc; // device: the description its last CreateCommandQueue read
};

constexpr UINT kMockIncrement = 32;  // what a mock device's GetDescriptorHandleIncrementSize answers

Mock g_pool[256];
unsigned g_pool_used = 0;

void** mock_vtable();

Mock* new_mock(Kind kind) {
    if (g_pool_used == std::size(g_pool)) {
        std::fprintf(stderr, "%s: mock pool exhausted\n", g_label);
        std::exit(1);
    }
    Mock* m = &g_pool[g_pool_used++];
    std::memset(m, 0, sizeof(*m));
    m->vtable = mock_vtable();
    m->kind = kind;
    return m;
}

bool mock_has(const Mock* m, REFIID iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object))) return true;
    if (m->kind == Kind::device) {
        if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
        const IID levels[] = {__uuidof(ID3D12Device1), __uuidof(ID3D12Device2), __uuidof(ID3D12Device3),
                              __uuidof(ID3D12Device4), __uuidof(ID3D12Device5), __uuidof(ID3D12Device6),
                              __uuidof(ID3D12Device7), __uuidof(ID3D12Device8), __uuidof(ID3D12Device9),
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device10),
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device11),
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device12),
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device13),
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device14),
#endif
        };
        for (int i = 0; i < m->device_level && i < static_cast<int>(std::size(levels)); ++i)
            if (IsEqualIID(iid, levels[i])) return true;
        return false;
    }
    if (IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable))) return true;
    switch (m->kind) {
        case Kind::queue: return IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(iid, kExtraIid);
        case Kind::fence:
            return IsEqualIID(iid, __uuidof(ID3D12Fence)) || (m->fence1 && IsEqualIID(iid, __uuidof(ID3D12Fence1)));
        case Kind::resource: return IsEqualIID(iid, __uuidof(ID3D12Resource));
        case Kind::heap: return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap));
        case Kind::command_list: {
            if (IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
                IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList)))
                return true;
            const IID levels[] = {__uuidof(ID3D12GraphicsCommandList1), __uuidof(ID3D12GraphicsCommandList2),
                                  __uuidof(ID3D12GraphicsCommandList3), __uuidof(ID3D12GraphicsCommandList4),
                                  __uuidof(ID3D12GraphicsCommandList5), __uuidof(ID3D12GraphicsCommandList6)};
            for (int i = 0; i < m->list_level && i < static_cast<int>(std::size(levels)); ++i)
                if (IsEqualIID(iid, levels[i])) return true;
            return false;
        }
        case Kind::allocator:
            return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator)) ||
                   (m->allocator_tag && IsEqualIID(iid, xs::kLeonidaCommandAllocatorTag));
        default: return false;
    }
}

HRESULT mock_query(Mock* m, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
    *out = m;
    ++m->refs;
    return S_OK;
}

// A device creating (or opening) a `kind` object as `riid`: S_FALSE without an output, as the PC runtime answers a
// capability check; the device's create_result and create_null otherwise.
HRESULT mock_create(Mock* device, Kind kind, std::uint64_t riid, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (device->create_null || FAILED(device->create_result)) return device->create_result;
    Mock* child = new_mock(kind);
    child->fence1 = device->fence1;
    child->allocator_tag = device->allocator_tag;
    child->list_level = device->list_level;
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// `words` are the call's first five words (a1..a5), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3];
    if (m->kind == Kind::device) {
        switch (slot) {
            case 8:                                                                           // CreateCommandQueue
                if (a1) std::memcpy(&m->queue_desc, reinterpret_cast<const void*>(a1), sizeof(m->queue_desc));
                return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a2, a3));
            case 14: return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));    // CreateDescriptorHeap
            case 15: return kMockIncrement;                       // GetDescriptorHandleIncrementSize(type)
            // The struct-returning methods, as MSVC calls a member function: `this`, then the hidden return pointer
            // (a1), then the arguments; the pointer comes back in RAX. The values encode the arguments received.
            case 25: {                                            // GetResourceAllocationInfo(mask, count, descs)
                auto* info = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(a1);
                info->SizeInBytes = 0x10000ull * static_cast<std::uint32_t>(a3) + static_cast<std::uint32_t>(a2);
                info->Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                return a1;
            }
            case 26: {                                            // GetCustomHeapProperties(node mask, heap type)
                auto* props = reinterpret_cast<D3D12_HEAP_PROPERTIES*>(a1);
                props->Type = D3D12_HEAP_TYPE_CUSTOM;
                props->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
                props->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
                props->CreationNodeMask = static_cast<UINT>(a2);
                props->VisibleNodeMask = static_cast<UINT>(a3);
                return a1;
            }
            case 43: {                                            // GetAdapterLuid()
                auto* luid = reinterpret_cast<LUID*>(a1);
                luid->LowPart = 0x89abcdef;
                luid->HighPart = 0x1234;
                return a1;
            }
            case 9: return static_cast<std::uint32_t>(mock_create(m, Kind::allocator, a2, a3));  // CreateCmdAllocator
            case 75: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a3, a4));   // CreateCommandQueue1
            case 36: return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a3, a4));   // CreateFence
            case 32:                                                                          // OpenSharedHandle
                if (a1 == kFenceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a2, a3));
                if (a1 == kResourceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::resource, a2, a3));
                return static_cast<std::uint32_t>(E_INVALIDARG);
            case 34: case 35: case 46: copy_array(m, slot, a2, a1); break;  // (count, objects, ...)
            case 45: copy_array(m, slot, a1, a3); break;                    // (fences, values, count, ...)
            case 50: copy_array(m, slot, a3, a2); break;                    // (flags, count, objects, fence, value)
            default: break;
        }
        return S_OK;
    }
    if (m->kind == Kind::fence && slot == 8) return kCompletedValue;  // GetCompletedValue
    if (m->kind == Kind::command_list && slot == 28) copy_array(m, slot, a2, a1);  // SetDescriptorHeaps(count, heaps)
    return S_OK;
}

template <unsigned Slot>
std::uint64_t WINAPI mock_slot(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                               std::uint64_t a5) {
    if constexpr (Slot == 0) {
        return static_cast<std::uint32_t>(mock_query(self, reinterpret_cast<const IID*>(a1),
                                                     reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        return static_cast<std::uint64_t>(--self->refs);
    } else {
        // A caller may pass fewer words than this thunk declares (OpenSharedHandle passes three), so its frame
        // has no room past the four register homes. The words are copied into this frame and passed by pointer:
        // forwarding them as stack arguments let MSVC tail-call mock_call and write into the caller's saved
        // registers (an optimized build corrupted the test's own locals that way).
        ++self->calls[Slot];
        const std::uint64_t words[] = {a1, a2, a3, a4, a5};
        std::memcpy(self->args[Slot], words, sizeof(words));
        return mock_call(self, Slot, words);
    }
}

// PC ID3D12Device::CreateCommandList takes six words after `this`, one more than the generic mock thunk declares, so
// it gets its own entry in the table: the out pointer is a6 (first stack argument on x64) and has to be written.
HRESULT WINAPI mock_create_command_list(Mock* self, std::uint64_t node_mask, std::uint64_t type,
                                        std::uint64_t allocator, std::uint64_t initial_state, const IID* riid,
                                        void** out) {
    ++self->calls[12];
    self->args[12][0] = node_mask;
    self->args[12][1] = type;
    self->args[12][2] = allocator;
    self->args[12][3] = initial_state;
    self->args[12][4] = reinterpret_cast<std::uint64_t>(riid);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(Kind::command_list);
    child->list_level = self->list_level;
    self->last_child = child;
    return mock_query(child, riid, out);
}

// PC ID3D12GraphicsCommandList::CopyTextureRegion takes six words after `this`, one more than the generic mock thunk
// records: the source box is a6, the first stack argument.
std::uint64_t WINAPI mock_copy_texture_region(Mock* self, std::uint64_t dst, std::uint64_t x, std::uint64_t y,
                                              std::uint64_t z, std::uint64_t src, std::uint64_t box) {
    ++self->calls[16];
    const std::uint64_t words[] = {dst, x, y, z, src, box};
    std::memcpy(self->args[16], words, sizeof(words));
    return S_OK;
}

// PC ID3D12GraphicsCommandList1::OMSetDepthBounds takes two FLOAT arguments in XMM1/XMM2. The generic integer thunk
// would record the untouched integer registers, so this entry keeps the float signature and records the bit patterns.
void WINAPI mock_om_set_depth_bounds(Mock* self, float min, float max) {
    ++self->calls[62];
    std::uint32_t min_bits = 0, max_bits = 0;
    std::memcpy(&min_bits, &min, sizeof(min_bits));
    std::memcpy(&max_bits, &max, sizeof(max_bits));
    self->args[62][0] = min_bits;
    self->args[62][1] = max_bits;
}

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        table[16] = reinterpret_cast<void*>(&mock_copy_texture_region);
        table[62] = reinterpret_cast<void*>(&mock_om_set_depth_bounds);
        return true;
    }();
    (void)filled;
    return table;
}

// ---- Calls by slot, the way a title compiled against the console headers makes them ----

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t raw(const void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                  std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0,
                  std::uint64_t a7 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, a7, 0, 0, 0, 0,
                                                  0, 0, 0);
}

// A struct-returning device method the way the title calls it (game_scarlett_master.exe 1.0.2.1, slots 25/26/43):
// the slot read from the device's table, the hidden return pointer in RCX and the device in RDX, then the arguments.
std::uint64_t raw_rcx_first(const void* device, void* first, unsigned slot, std::uint64_t second,
                            std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    return reinterpret_cast<SlotFn>(vtable[slot])(first, second, a2, a3, a4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

HRESULT hr_of(std::uint64_t value) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(value));
}

template <class T>
std::uint64_t word(const T* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

const IID kQueueIid = __uuidof(ID3D12CommandQueue);
const IID kFenceIid = __uuidof(ID3D12Fence);
const IID kFence1Iid = __uuidof(ID3D12Fence1);
const IID kUnknownIid = __uuidof(IUnknown);
const IID kResourceIid = __uuidof(ID3D12Resource);
const IID kDevice1Iid = __uuidof(ID3D12Device1);
const D3D12_COMMAND_QUEUE_DESC kQueueDesc{};

// A device proxy over a new mock device. The proxy takes the mock's one reference.
void* make_device(Mock*& mock, int level, bool fence1, bool allocator_tag = true) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->fence1 = fence1;
    mock->allocator_tag = allocator_tag;
    mock->refs = 1;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device)))
        return nullptr;
    return device;
}

void* make_queue(void* device) {
    void* queue = nullptr;
    return SUCCEEDED(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&queue)))) ? queue : nullptr;
}

void* make_fence(void* device, const IID& iid = kFenceIid) {
    void* fence = nullptr;
    return SUCCEEDED(hr_of(raw(device, 36, 0, 0, word(&iid), word(&fence)))) ? fence : nullptr;
}

unsigned long release(void* object) {
    return static_cast<unsigned long>(raw(object, 2));
}

// ---- 1. Device interface levels ----

int check_levels() {
    struct Level { int level; unsigned slots; } const levels[] = {{0, 44}, {1, 47}, {3, 51}, {9, 76}};
    for (const Level& l : levels) {
        Mock* mock = nullptr;
        void* device = make_device(mock, l.level, true);
        EXPECT(device && xs::device_proxy_slot_count(device) == l.slots);
        void* queue = make_queue(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_queue_proxy(queue) && xs::is_fence_proxy(fence));
        const auto real_queue = word(xs::queue_proxy_real_queue(queue));
        const auto real_fence = word(xs::fence_proxy_real_fence(fence));
        EXPECT(real_queue && real_fence && real_queue != word(queue) && real_fence != word(fence));
        // What every residency call gets, with the proxies in it replaced by their real objects.
        const std::uint64_t objects[4] = {word(queue), word(fence), real_queue, 0};
        const std::uint64_t unwrapped[4] = {real_queue, real_fence, real_queue, 0};
        const std::uint64_t fences[2] = {word(fence), word(fence)};
        const std::uint64_t values[2] = {1, 2};
        const D3D12_RESIDENCY_PRIORITY priorities[4] = {};
        const auto same = [](const std::uint64_t* got, const std::uint64_t* want, int n) {
            return std::memcmp(got, want, n * sizeof(std::uint64_t)) == 0;
        };

        // 34 MakeResident, with a count whose high dword is not zero: the PC prototype takes a UINT.
        EXPECT(hr_of(raw(device, 34, 0xdeadbeef00000002ull, word(objects))) == S_OK);
        EXPECT(mock->calls[34] == 1 && static_cast<std::uint32_t>(mock->args[34][0]) == 2);
        EXPECT(same(mock->array[34], unwrapped, 2) && mock->array[34][2] == 0);
        // 31 CreateSharedHandle
        EXPECT(hr_of(raw(device, 31, word(fence), 0, GENERIC_ALL, 0, 0)) == S_OK);
        EXPECT(mock->calls[31] == 1 && mock->args[31][0] == real_fence);

        // 45/46/50/75 are not the PC's Device1/3/9 methods on the console (its table matches the PC only up to slot
        // 43; VIctory device.rs, XCL device_x.rs): 45/46 are refused without reaching the device, 50 (a callback
        // registration) is accepted without reaching it, and 75 is UnregisterPagePoolX (an unknown index is refused).
        (void)fences;
        (void)values;
        (void)priorities;
        EXPECT(hr_of(raw(device, 45, 0, 0, 2, 0, 0)) == E_NOTIMPL && mock->calls[45] == 0);
        EXPECT(hr_of(raw(device, 46, 4, word(objects), 0)) == E_NOTIMPL && mock->calls[46] == 0);
        EXPECT(hr_of(raw(device, 50, 0, 4, word(objects), word(fence), 1)) == S_OK && mock->calls[50] == 0);
        EXPECT(hr_of(raw(device, 75, 12345)) == E_INVALIDARG && mock->calls[75] == 0);
        std::uint32_t pool = 0;
        EXPECT(hr_of(raw(device, 74, 0x10000, 16, word(&pool))) == S_OK && pool != 0 && mock->calls[74] == 0);
        EXPECT(hr_of(raw(device, 75, pool)) == S_OK && hr_of(raw(device, 75, pool)) == E_INVALIDARG);

        Mock* const queue_mock = reinterpret_cast<Mock*>(real_queue);
        Mock* const fence_mock = reinterpret_cast<Mock*>(real_fence);
        EXPECT(release(queue) == 0 && release(fence) == 0);
        EXPECT(queue_mock->refs == 0 && fence_mock->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: devices up to ID3D12Device, Device1, Device3, Device9: 44/47/51/76 slots; 45/46 refused, 50 "
                "accepted, 74/75 page pools; residency arrays and fences unwrapped\n", g_label);
    return 0;
}

// ---- 2. Fences with and without ID3D12Fence1 ----

int check_fence1() {
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_fence_proxy(fence) && xs::fence_proxy_slot_count(fence) == (fence1 ? 12u : 11u));
        Mock* const fence_mock = mock->last_child;
        void* as_fence1 = nullptr;
        const HRESULT qi = hr_of(raw(fence, 0, word(&kFence1Iid), word(&as_fence1)));
        if (fence1) {
            EXPECT(qi == S_OK && as_fence1 == fence && release(as_fence1) == 1);
        } else {
            EXPECT(qi == E_NOINTERFACE && !as_fence1);
        }
        EXPECT(raw(fence, 8) == kCompletedValue);  // GetCompletedValue forwarded
        const HRESULT slot11 = hr_of(raw(fence, 11));
        if (fence1) {
            EXPECT(slot11 == S_OK && fence_mock->calls[11] == 1);
        } else {
            EXPECT(slot11 == E_NOTIMPL && fence_mock->calls[11] == 0);  // not forwarded past an 11-slot vtable
        }
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);

        // Asked for as ID3D12Fence1: made as ID3D12Fence, then refused when the fence lacks Fence1, and released.
        void* created1 = nullptr;
        const HRESULT made1 = hr_of(raw(device, 36, 0, 0, word(&kFence1Iid), word(&created1)));
        if (fence1) {
            EXPECT(made1 == S_OK && xs::is_fence_proxy(created1) && release(created1) == 0);
        } else {
            EXPECT(made1 == E_NOINTERFACE && !created1);
        }
        EXPECT(mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fences without and with ID3D12Fence1: 11/12 slots, QueryInterface, slot 11 and creation as "
                "Fence1 refused or forwarded to match\n", g_label);
    return 0;
}

// ---- 3. Creation failures and refused interfaces ----

int check_create_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* out = reinterpret_cast<void*>(1);

    mock->create_null = true;  // S_OK with no object
    mock->create_result = S_OK;
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_FAIL && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))) == E_FAIL && !out);
    mock->create_null = false;
    mock->create_result = E_OUTOFMEMORY;  // the real failure, passed on
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    mock->create_result = S_OK;

    // Console slots 74/75 (RegisterPagePoolX/UnregisterPagePoolX): answered by the proxy, never the PC device.
    const unsigned calls74 = mock->calls[74], calls75 = mock->calls[75];
    std::uint32_t pool_index = 0xffffffffu;
    EXPECT(hr_of(raw(device, 74, 0x10000, 16, word(&pool_index))) == S_OK && pool_index != 0xffffffffu);
    EXPECT(hr_of(raw(device, 75, pool_index)) == S_OK);
    EXPECT(mock->calls[74] == calls74 && mock->calls[75] == calls75);

    const unsigned calls8 = mock->calls[8];
    out = reinterpret_cast<void*>(1);  // a null IID: E_POINTER, the output cleared, nothing created
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), 0, word(&out))) == E_POINTER && !out && mock->calls[8] == calls8);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, 0, word(&out))) == E_POINTER && !out);
    // No output: the device's own answer (S_FALSE here), as the PC runtime gives it.
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), 0)) == S_FALSE && mock->calls[8] == calls8 + 1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), 0)) == S_FALSE);

    // An interface the real queue implements but the proxy does not model: created, refused, released.
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kExtraIid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = mock->last_child;
    void* extra = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(queue, 0, word(&kExtraIid), word(&extra))) == E_NOINTERFACE && !extra);
    EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && queue_mock->refs == 1);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: creation with no object (E_FAIL), a failing device, null IID and output, and an interface the "
                "proxy does not model leave no reference\n", g_label);
    return 0;
}

// ---- 4. OpenSharedHandle ----

int check_open_shared() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    for (const IID* iid : {&kFenceIid, &kUnknownIid}) {  // a fence, whatever it is asked for as, comes back wrapped
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kFenceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const fence_mock = mock->last_child;
        EXPECT(xs::is_fence_proxy(opened) && word(xs::fence_proxy_real_fence(opened)) == word(fence_mock));
        EXPECT(xs::command_object_proxy_ref_count(opened) == 1 && fence_mock->refs == 1);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs + 1);
        EXPECT(release(opened) == 0 && fence_mock->refs == 0);
    }
    void* out = reinterpret_cast<void*>(1);  // as Fence1, which this fence lacks: the real open fails, passed on
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFence1Iid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);

    for (const IID* iid : {&kResourceIid, &kUnknownIid}) {  // not a fence: the real object, unchanged
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kResourceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const resource_mock = mock->last_child;
        EXPECT(opened == resource_mock && !xs::is_fence_proxy(opened) && !xs::is_queue_proxy(opened));
        EXPECT(resource_mock->refs == 1 && release(opened) == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);

    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, 0xbad, word(&kFenceIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), 0)) == S_FALSE);  // the device's own answer
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, 0, word(&out))) == E_POINTER && !out);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: OpenSharedHandle wraps a fence (as ID3D12Fence and IUnknown), passes a resource, a failed open, "
                "an invalid handle and a null output through, with balanced references\n", g_label);
    return 0;
}

// ---- 5. Allocation failures in the proxy ----

int check_alloc_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    const std::uint64_t objects[2] = {word(queue), word(fence)};

    const auto failing = [&](auto call) {  // one allocation refused, the result of the call
        const int before = g_failed_news;
        g_fail_news = 1;
        const HRESULT hr = call();
        g_fail_news = 0;
        return g_failed_news == before + 1 ? hr : S_OK;
    };
    void* out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))); }) == E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);  // the real fence was made and released
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    // Up to 16 entries the unwrapped copy is inline: an armed failure is not even reached, and the call goes on.
    g_fail_news = 1;
    const HRESULT inline_result = hr_of(raw(device, 34, 2, word(objects)));
    const bool untouched = g_fail_news == 1;
    g_fail_news = 0;
    EXPECT(inline_result == S_OK && untouched && mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)));

    // A longer array is allocated; refused, the call returns E_OUTOFMEMORY and reaches no device.
    constexpr unsigned kLong = 32;
    std::uint64_t many[kLong], many_fences[kLong], values[kLong];
    D3D12_RESIDENCY_PRIORITY priorities[kLong] = {};
    for (unsigned i = 0; i < kLong; ++i) {
        many[i] = (i % 2 != 0) ? word(fence) : word(queue);
        many_fences[i] = word(fence);
        values[i] = 1;
    }
    const unsigned calls34 = mock->calls[34], calls35 = mock->calls[35], calls45 = mock->calls[45];
    const unsigned calls46 = mock->calls[46], calls50 = mock->calls[50];
    EXPECT(failing([&] { return hr_of(raw(device, 34, kLong, word(many))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 35, kLong, word(many))); }) == E_OUTOFMEMORY);
    // 45/46/50 are console methods now: 45/46 are refused, 50 is accepted, and the device is never reached.
    EXPECT(hr_of(raw(device, 45, word(many_fences), word(values), kLong, 0, 0)) == E_NOTIMPL);
    EXPECT(hr_of(raw(device, 46, kLong, word(many), word(priorities))) == E_NOTIMPL);
    EXPECT(hr_of(raw(device, 50, 0, kLong, word(many), word(fence), 1)) == S_OK);
    EXPECT(mock->calls[34] == calls34 && mock->calls[35] == calls35 && mock->calls[45] == calls45 &&
           mock->calls[46] == calls46 && mock->calls[50] == calls50);  // nothing reached the device
    // And without the failure the long array arrives unwrapped.
    EXPECT(hr_of(raw(device, 34, kLong, word(many))) == S_OK && mock->calls[34] == calls34 + 1);
    EXPECT(mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)) &&
           mock->array[34][1] == word(xs::fence_proxy_real_fence(fence)));

    // The device proxy itself: on failure the caller keeps its reference.
    Mock* other = new_mock(Kind::device);
    other->refs = 1;
    void* second = reinterpret_cast<void*>(1);
    EXPECT(failing([&] {
               return xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(other), __uuidof(ID3D12Device),
                                              &second);
           }) == E_OUTOFMEMORY);
    EXPECT(!second && other->refs == 1);

    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const fence_mock = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    EXPECT(release(queue) == 0 && release(fence) == 0 && queue_mock->refs == 0 && fence_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a refused allocation in queue, fence, shared-fence and device proxy creation and in the two "
                "unwrapped arrays returns E_OUTOFMEMORY, reaches no device and leaks nothing\n", g_label);
    return 0;
}

// ---- 6. Command allocators (device slot 9) ----

int check_allocators() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    // An allocator that answers Leonida's tag (its lists resolve a proxy) comes back wrapped.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, true);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
        Mock* const real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(allocator) && word(xs::allocator_proxy_real_allocator(allocator)) == word(real));
        EXPECT(mock->args[9][0] == D3D12_COMMAND_LIST_TYPE_DIRECT && real->refs == 1);
        EXPECT(xs::allocator_proxy_slot_count(allocator) == 9 && xs::device_proxy_ref_count(device) == device_refs + 1);
        // Reset (slot 8) reaches the real allocator. Slot 9 is the console's allocation-callback registration and is
        // accepted without reaching the PC allocator; every slot past it up to the table end is refused.
        EXPECT(hr_of(raw(allocator, 8)) == S_OK && real->calls[8] == 1);
        EXPECT(hr_of(raw(allocator, 9, 0x1234)) == S_OK && real->calls[9] == 0);
        for (unsigned slot = 10; slot < xs::allocator_proxy_table_size(); ++slot)
            EXPECT(hr_of(raw(allocator, slot)) == E_NOTIMPL && real->calls[slot] == 0);
        // The tag and the real object stay hidden: QueryInterface answers only the modelled interfaces.
        void* found = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(allocator, 0, word(&xs::kLeonidaCommandAllocatorTag), word(&found))) == E_NOINTERFACE &&
               !found);
        EXPECT(hr_of(raw(allocator, 0, word(&allocator_iid), word(&found))) == S_OK && found == allocator);
        EXPECT(release(found) == 1);
        // An allocator is a pageable: MakeResident gives the real runtime the real allocator.
        const std::uint64_t objects[1] = {word(allocator)};
        EXPECT(hr_of(raw(device, 34, 1, word(objects))) == S_OK && mock->array[34][0] == word(real));
        EXPECT(release(allocator) == 0 && real->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        // Null IID, null output, a failing or empty creation, an IID the allocator lacks.
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, 0, word(&out))) == E_POINTER && !out);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), 0)) == S_FALSE);  // the device's own answer
        mock->create_result = E_OUTOFMEMORY;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_OUTOFMEMORY && !out);
        mock->create_result = S_OK;
        mock->create_null = true;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_FAIL && !out);
        mock->create_null = false;
        const IID fence_iid = __uuidof(ID3D12Fence);
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        // A refused allocation for the proxy: E_OUTOFMEMORY, and the real allocator is released.
        g_fail_news = 1;
        out = reinterpret_cast<void*>(1);
        const HRESULT starved = hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out)));
        g_fail_news = 0;
        EXPECT(starved == E_OUTOFMEMORY && !out && mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    // An allocator without the tag (a PC runtime allocator, whose lists could not resolve a proxy) comes back as it
    // is, as the interface asked for.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, false);
        EXPECT(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        EXPECT(allocator == mock->last_child && !xs::is_allocator_proxy(allocator) && mock->last_child->refs == 1);
        Mock* const real = mock->last_child;
        EXPECT(release(allocator) == 0 && real->refs == 0);
        const IID fence_iid = __uuidof(ID3D12Fence);
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: allocators answering Leonida's tag are wrapped (Reset forwarded, tail refused, residency "
                "unwrapped, references balanced); others come back unwrapped; failures leave no reference\n", g_label);
    return 0;
}

// ---- 7. Xbox device slots from the public HLE (48, 73, 78, 79, 80) ----
//
// A device that answers ID3D12Device11 (80 PC slots) is the Vulkan layer's shape: slots 78/79 are inside its table
// (CreateReservedResource2/CreateSampler2 there) and 80 is just past it. None of the five may reach the mock's own
// slot: the mock's call counter proves the route, and the returned token/queue/list proves the handler ran.
int check_xbox_slots() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 80);
    xs::reset_device_proxy_stats();

    // 78 SetFrameIntervalX(output, interval_us, latency, flags): S_OK, not forwarded to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && xs::device_slot_hits(78) == 1);
    // 79 ScheduleFrameEventX(event, offset_us, pEvent, flags): S_OK, not forwarded to PC slot 79.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[79] == 0 && xs::device_slot_hits(79) == 1);

    // 80 WaitFrameEventX(event, timeout_ms, pEvent, flags, &token): a fresh non-zero token each time; a null token
    // pointer is E_POINTER; the call never blocks (this test would hang otherwise).
    std::uint64_t token = 0, token2 = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token != 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token2))) == S_OK && token2 != 0 && token2 != token);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, 0)) == E_POINTER);
    EXPECT(mock->calls[80] == 0 && xs::device_slot_hits(80) == 3);

    // 48 CreateCommandQueueX(desc, riid, out): the queue proxy, created through the PC slot 8 path.
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(&kQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[48] == 0 && mock->calls[8] == 1);
    EXPECT(mock->args[8][0] == word(&kQueueDesc));

    // 73 CreateCommandListX(desc, allocator, initialState, riid, out): the type is desc's first word, the allocator
    // proxy is unwrapped, the node mask is 0 and the PC six-argument CreateCommandList (slot 12) is reached.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{};
    desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.node_mask = 7;  // ignored by the console call, as the HLE does
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    // The list comes back in a console-layout proxy around the PC list: put pointer and limits (0x18..0x40) zero.
    EXPECT(list && xs::is_list_proxy(list));
    EXPECT(reinterpret_cast<Mock*>(xs::list_proxy_real_list(list))->kind == Kind::command_list);
    {
        const auto* header = static_cast<const std::uint64_t*>(list);
        EXPECT(header[3] == 0 && header[4] == 0 && header[5] == 0 && header[6] == 0 && header[7] == 0);
    }
    EXPECT(mock->calls[73] == 0 && mock->calls[12] == 1 && xs::device_slot_hits(73) == 1);
    EXPECT(mock->args[12][0] == 0 && mock->args[12][1] == D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT(mock->args[12][2] == word(real_allocator) && mock->args[12][3] == 0);
    // The PC list is created as ID3D12GraphicsCommandList, whatever the title asked for; the proxy answers that.
    EXPECT(IsEqualIID(*reinterpret_cast<const IID*>(mock->args[12][4]), __uuidof(ID3D12GraphicsCommandList)));

    // Release the list before the refusal calls below: the COM convention (and the handler) nulls the caller's
    // output on failure, so an invalid call would clear this pointer.
    EXPECT(release(list) == 0);

    // Missing description, allocator or output: E_INVALIDARG/E_POINTER before the real device is called.
    EXPECT(hr_of(raw(device, 73, 0, word(allocator), 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), 0, 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), 0)) == E_POINTER);

    EXPECT(xs::device_slot_overridden(48) && xs::device_slot_overridden(73) && xs::device_slot_overridden(78) &&
           xs::device_slot_overridden(79) && xs::device_slot_overridden(80));

    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slots 48/73/78/79/80 route to a queue, a list and frame-event tokens without reaching "
                "the mock's own PC slots\n", g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; Xbox slot 80's beyond-table case is not built\n", g_label);
    return 0;
#endif
}

// ---- 8. Environment overrides for the Xbox device slots (one process each, read once) ----

int run_env_slots(bool valid) {
    const char* frame_interval = valid ? "96" : "2";
    const char* schedule_event = valid ? "97" : "2";
    const char* wait_event = valid ? "98" : "2";
    const char* queue_x = valid ? "99" : "2";
    const char* list_x = valid ? "100" : "2";
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_FRAME_INTERVAL_SLOT", frame_interval) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_SCHEDULE_EVENT_SLOT", schedule_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_WAIT_EVENT_SLOT", wait_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_QUEUE_X_SLOT", queue_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_LIST_X_SLOT", list_x) != FALSE);

    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    std::uint64_t token = 0;
    if (valid) {
        // Every route moved: the new slot is handled, and the old default (44 or above, answered by nothing else)
        // is refused rather than forwarded to the PC device.
        EXPECT(hr_of(raw(device, 96, 0, 16667, 1, 0)) == S_OK && mock->calls[96] == 0);
        EXPECT(hr_of(raw(device, 97, 0, 0, 0, 0)) == S_OK && mock->calls[97] == 0);
        EXPECT(hr_of(raw(device, 98, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[98] == 0);
        void* queue = nullptr;
        EXPECT(hr_of(raw(device, 99, word(&kQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[99] == 0);
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct {
            std::uint32_t type, node_mask;
        } desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0};
        const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 100, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == E_NOTIMPL && mock->calls[78] == 0);
        EXPECT(release(list) == 0);
        EXPECT(release(allocator) == 0);
        EXPECT(release(queue) == 0);
    } else {
        // Values outside 3..127 are ignored: the five defaults still answer, and a PC slot that is not a default is
        // forwarded unchanged.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 0);
        EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK && mock->calls[79] == 0);
        EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[80] == 0);
        // A slot the console shares with the PC (0-43): forwarded.
        EXPECT(hr_of(raw(device, 33)) == S_OK && mock->calls[33] == 1);
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: environment slot overrides %s\n", g_label,
                valid ? "move all five routes" : "outside 3..127 are ignored");
    return 0;
}

// ---- 9. The refused tail and the guard region ----

int check_tail(bool want_guarded) {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    EXPECT(xs::command_object_tables_guarded() == want_guarded);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    // The console's newer queue layout: 14/15 are the console's own methods and stay in the proxy; Signal is 16,
    // which reaches the real queue's PC slot 14 with the real fence.
    EXPECT(hr_of(raw(queue, 14, word(fence), 5)) == S_OK && queue_mock->calls[14] == 0);
    EXPECT(hr_of(raw(queue, 16, word(fence), 5)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == word(xs::fence_proxy_real_fence(fence)) && queue_mock->args[14][1] == 5);
    // GetDesc (20, and 28 where a source puts it) forwards to the real queue's PC slot 18.
    D3D12_COMMAND_QUEUE_DESC desc{};
    raw(queue, 20, word(&desc));
    EXPECT(queue_mock->calls[18] == 1);
    raw(queue, 28, word(&desc));
    EXPECT(queue_mock->calls[18] == 2);
    // Everything from 36 (past ExecuteCommandListsX at 35) to the end of the table is refused.
    for (unsigned slot = 36; slot < xs::queue_proxy_table_size(); ++slot)
        EXPECT(hr_of(raw(queue, slot)) == E_NOTIMPL && queue_mock->calls[slot] == 0);
    // Fence slot 12 is the console's GetGpuAddress (a cell address), so the refused range starts at 13.
    for (unsigned slot = 13; slot < xs::fence_proxy_table_size(); ++slot)
        EXPECT(hr_of(raw(fence, slot)) == E_NOTIMPL);
    EXPECT(xs::queue_table_overruns() == 0 && xs::fence_table_overruns() == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    return 0;
}

// ---- 10. Struct-returning device slots 25/26/43, in the title's order and the PC's ----
//
// The title passes the hidden return pointer in RCX and the device in RDX; forwarded as they came, the PC device's
// `this` was the caller's buffer. Either order must fill the caller's buffer from the PC method and return it in RAX.

int check_struct_returns() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const D3D12_RESOURCE_DESC descs[2] = {};
    for (const bool xbox_order : {true, false}) {
        const auto call = [&](void* buffer, unsigned slot, std::uint64_t a2 = 0, std::uint64_t a3 = 0,
                              std::uint64_t a4 = 0) {
            return xbox_order ? raw_rcx_first(device, buffer, slot, word(device), a2, a3, a4)
                              : raw(device, slot, word(buffer), a2, a3, a4);
        };
        const unsigned calls25 = mock->calls[25], calls26 = mock->calls[26], calls43 = mock->calls[43];

        // 25 GetResourceAllocationInfo(visible mask, count, descs)
        D3D12_RESOURCE_ALLOCATION_INFO info;
        std::memset(&info, 0xcd, sizeof(info));
        EXPECT(call(&info, 25, 0x5, 2, word(descs)) == word(&info));
        EXPECT(info.SizeInBytes == 0x20005 && info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
        EXPECT(mock->calls[25] == calls25 + 1 && static_cast<std::uint32_t>(mock->args[25][1]) == 0x5 &&
               static_cast<std::uint32_t>(mock->args[25][2]) == 2 && mock->args[25][3] == word(descs));

        // 26 GetCustomHeapProperties(node mask, heap type)
        D3D12_HEAP_PROPERTIES props;
        std::memset(&props, 0xcd, sizeof(props));
        EXPECT(call(&props, 26, 1, D3D12_HEAP_TYPE_UPLOAD) == word(&props));
        EXPECT(props.Type == D3D12_HEAP_TYPE_CUSTOM && props.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK &&
               props.MemoryPoolPreference == D3D12_MEMORY_POOL_L0 && props.CreationNodeMask == 1 &&
               props.VisibleNodeMask == static_cast<UINT>(D3D12_HEAP_TYPE_UPLOAD));
        EXPECT(mock->calls[26] == calls26 + 1);

        // 43 GetAdapterLuid()
        LUID luid;
        std::memset(&luid, 0xcd, sizeof(luid));
        EXPECT(call(&luid, 43) == word(&luid));
        EXPECT(luid.LowPart == 0x89abcdef && luid.HighPart == 0x1234 && mock->calls[43] == calls43 + 1);
    }
    // A texture the console layout model knows gets the console's size and alignment; the PC runtime is still asked
    // first for its capability answer (valid versus UINT64_MAX), and its sizes are replaced. The zeroed descriptions
    // above are not textures and keep the PC answer.
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 512;
    texture.Height = 512;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 10;
    texture.Format = DXGI_FORMAT_BC7_UNORM;
    texture.SampleDesc.Count = 1;
    {
        D3D12_RESOURCE_ALLOCATION_INFO info;
        std::memset(&info, 0xcd, sizeof(info));
        const unsigned before25 = mock->calls[25];
        EXPECT(raw(device, 25, word(&info), 0, 1, word(&texture)) == word(&info));
        EXPECT(info.SizeInBytes == 0x56000 && info.Alignment == 0x1000);  // the measured console number
        EXPECT(mock->calls[25] == before25 + 1);  // the PC device answered the capability check
        // Two descriptions add their sizes; the stricter alignment wins.
        D3D12_RESOURCE_DESC pair[] = {texture, texture};
        EXPECT(raw_rcx_first(device, &info, 25, word(device), 0, 2, word(pair)) == word(&info));
        EXPECT(info.SizeInBytes == 2 * 0x56000 && info.Alignment == 0x1000);
        EXPECT(mock->calls[25] == before25 + 2);
    }

    // Neither word a device proxy: 0, the PC device not called, the buffers untouched.
    std::uint64_t first[4] = {0, 0, 0, 0}, second[4] = {0, 0, 0, 0};
    const unsigned calls25 = mock->calls[25];
    EXPECT(raw_rcx_first(device, first, 25, word(second), 1, 1, word(descs)) == 0);
    EXPECT(mock->calls[25] == calls25 && first[0] == 0 && second[0] == 0 && first[1] == 0 && second[1] == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: struct-returning slots 25/26/43 fill the caller's buffer and return it, with the buffer in RCX "
                "and the device in RDX or the other way round\n", g_label);
    return 0;
}

// ---- 11. Command queues with console flag bits (slots 8 and 48) ----

int check_queue_flags() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    struct Case { unsigned slot; UINT flags; } const cases[] = {{8, 2}, {8, 3}, {48, 2}, {48, 3}, {8, 1}, {48, 1}};
    for (const Case& c : cases) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(c.flags);
        desc.NodeMask = 1;
        const unsigned calls8 = mock->calls[8];
        std::memset(&mock->queue_desc, 0xcd, sizeof(mock->queue_desc));
        void* queue = nullptr;
        EXPECT(hr_of(raw(device, c.slot, word(&desc), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[8] == calls8 + 1 && mock->calls[48] == 0);
        // The PC device sees only DISABLE_GPU_TIMEOUT, the rest of the description as the title wrote it.
        EXPECT(static_cast<UINT>(mock->queue_desc.Flags) == (c.flags & D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT));
        EXPECT(mock->queue_desc.Type == desc.Type && mock->queue_desc.Priority == desc.Priority &&
               mock->queue_desc.NodeMask == desc.NodeMask);
        // A PC-only description reaches the device as the caller's own pointer; a console one as a copy.
        if (c.flags & ~static_cast<UINT>(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT)) {
            EXPECT(mock->args[8][0] != word(&desc));
        } else {
            EXPECT(mock->args[8][0] == word(&desc));
        }
        EXPECT(desc.Flags == static_cast<D3D12_COMMAND_QUEUE_FLAGS>(c.flags));  // the caller's copy unchanged
        Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
        EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: command queues from slots 8 and 48 with console flags 2/3 reach the PC device with only "
                "DISABLE_GPU_TIMEOUT; a PC description is passed as the caller's pointer\n", g_label);
    return 0;
}

// ---- 12. The list proxy's console slot map ----

unsigned total_calls(const Mock* m) {
    unsigned total = 0;
    for (const unsigned c : m->calls) total += c;
    return total;
}

int check_list_slot_map() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    mock->list_level = 6;  // the real list answers through ID3D12GraphicsCommandList6: 80 PC slots (DispatchMesh at 79)
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK && xs::is_allocator_proxy(allocator));
    const std::uint64_t real_allocator = word(xs::allocator_proxy_real_allocator(allocator));
    struct {
        std::uint32_t type, flags, node_mask, batch;
    } list_desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0, 0, 0};
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&list_desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    EXPECT(xs::is_list_proxy(list));
    Mock* const real = reinterpret_cast<Mock*>(xs::list_proxy_real_list(list));
    EXPECT(real && real->kind == Kind::command_list);

    // Two heap proxies for SetDescriptorHeaps.
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    struct { D3D12_DESCRIPTOR_HEAP_DESC desc; std::uint64_t address, size; } padded[2] = {{heap_desc, 0, 0},
                                                                                          {heap_desc, 0, 0}};
    padded[1].desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    void* heaps[2] = {};
    for (int i = 0; i < 2; ++i)
        EXPECT(hr_of(raw(device, 14, word(&padded[i]), word(&heap_iid), word(&heaps[i]))) == S_OK &&
               xs::is_heap_proxy(heaps[i]));
    const std::uint64_t real_heaps[2] = {word(xs::heap_proxy_real_heap(heaps[0])),
                                         word(xs::heap_proxy_real_heap(heaps[1]))};

    // console 32 SetDescriptorHeaps(count, heaps) -> PC 28, with the heap proxies unwrapped.
    unsigned before = total_calls(real);
    const std::uint64_t heap_words[2] = {word(heaps[0]), word(heaps[1])};
    raw(list, 32, 2, word(heap_words));
    EXPECT(real->calls[28] == 1 && real->calls[32] == 0 && total_calls(real) == before + 1);
    EXPECT(static_cast<std::uint32_t>(real->args[28][0]) == 2 && real->array[28][0] == real_heaps[0] &&
           real->array[28][1] == real_heaps[1]);
    EXPECT(heap_words[0] == word(heaps[0]));  // the caller's array is not rewritten

    // console 28 BeginQuery(heap, type, index) -> PC 52
    raw(list, 28, 0x1111, 2, 7);
    EXPECT(real->calls[52] == 1 && real->calls[28] == 1 && real->args[52][0] == 0x1111 && real->args[52][1] == 2 &&
           real->args[52][2] == 7);
    // console 29 EndQuery -> PC 53, console 50 OMSetRenderTargets -> PC 46
    raw(list, 29, 0x2222, 1, 3);
    EXPECT(real->calls[53] == 1 && real->args[53][0] == 0x2222);
    raw(list, 50, 1, 0x3333, 0, 0);
    EXPECT(real->calls[46] == 1 && real->calls[50] == 0 && real->args[46][1] == 0x3333);
    // console 51 ClearDepthStencilView(dsv, flags, depth, stencil, rects, ...) -> PC 47
    raw(list, 51, 0x4444, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0);
    EXPECT(real->calls[47] == 1 && real->calls[51] == 0 && real->args[47][0] == 0x4444 &&
           real->args[47][1] == D3D12_CLEAR_FLAG_DEPTH);
    // console 61 ExecuteIndirect(signature, max count, args, offset, count buffer, offset) -> PC 59
    raw(list, 61, 0x5555, 4, 0x6666, 0x10, 0x7777, 0x20);
    EXPECT(real->calls[59] == 1 && real->calls[61] == 0 && real->args[59][0] == 0x5555 && real->args[59][1] == 4 &&
           real->args[59][2] == 0x6666 && real->args[59][3] == 0x10 && real->args[59][4] == 0x7777);
    // console 27 ExecuteBundle(list) stays PC 27, with a list proxy unwrapped.
    raw(list, 27, word(list));
    EXPECT(real->calls[27] == 1 && real->args[27][0] == word(real));

    // console 70 OMSetDepthBounds(min, max) -> PC 62; its two arguments are FLOATs, so the proxy's entry keeps that
    // calling convention (the generic word thunk would never read XMM1/XMM2).
    using DepthBoundsFn = void(WINAPI*)(void*, float, float);
    void* const* const vtable = *reinterpret_cast<void* const* const*>(list);
    const unsigned calls62 = real->calls[62];
    reinterpret_cast<DepthBoundsFn>(vtable[70])(list, 0.25f, 0.75f);
    EXPECT(real->calls[62] == calls62 + 1 && real->calls[70] == 0);
    EXPECT(static_cast<std::uint32_t>(real->args[62][0]) == 0x3e800000u &&
           static_cast<std::uint32_t>(real->args[62][1]) == 0x3f400000u);

    // console 80 DispatchX(x, y, z, flags) -> PC 14 Dispatch(x, y, z), with the flags word dropped.
    raw(list, 80, 3, 5, 7, 0x8020);
    EXPECT(real->calls[14] == 1 && real->calls[80] == 0 && real->args[14][0] == 3 && real->args[14][1] == 5 &&
           real->args[14][2] == 7 && real->args[14][3] == 0);

    // console 85 CopyTextureRegionX(dst, x, y, z, src, box, flags) -> PC 16 CopyTextureRegion, same positions for
    // the two locations, the coordinates and the box; the trailing flags word is dropped.
    D3D12_TEXTURE_COPY_LOCATION dst_location{}, src_location{};
    D3D12_BOX box{1, 2, 3, 4, 5, 6};
    raw(list, 85, word(&dst_location), 7, 8, 9, word(&src_location), word(&box), 0x40);
    EXPECT(real->calls[16] == 1 && real->calls[85] == 0 && real->args[16][0] == word(&dst_location) &&
           real->args[16][1] == 7 && real->args[16][2] == 8 && real->args[16][3] == 9 &&
           real->args[16][4] == word(&src_location) && real->args[16][5] == word(&box));

    // console 86 CopyResourceX(dst, src, flags) -> PC 17 CopyResource(dst, src), with the flags word dropped.
    raw(list, 86, 0x1111, 0x2222, 0x40);
    EXPECT(real->calls[17] == 1 && real->calls[86] == 0 && real->args[17][0] == 0x1111 &&
           real->args[17][1] == 0x2222 && real->args[17][2] == 0);

    // console 100 DispatchMeshX(x, y, z, flags) -> PC 79 DispatchMesh; 129 (base layout DispatchMesh) already did.
    raw(list, 100, 4, 5, 6, 0x20);
    EXPECT(real->calls[79] == 1 && real->calls[100] == 0 && real->args[79][0] == 4 && real->args[79][1] == 5 &&
           real->args[79][2] == 6 && real->args[79][3] == 0);
    raw(list, 129, 7, 8, 9);
    EXPECT(real->calls[79] == 2 && real->args[79][0] == 7 && real->args[79][1] == 8 && real->args[79][2] == 9);

    // 131/132 (syncs) and 142 (state flush): no-ops that reach nothing.
    before = total_calls(real);
    EXPECT(raw(list, 131, 1, 2, 3, 4) == 0 && raw(list, 132, 1, 2, 3, 4) == 0 && raw(list, 142, 5) == 0);
    EXPECT(total_calls(real) == before);
    // Console-only or unmapped slots: 0 and nothing reached. 62/63/64/65 (memory copy/fill), 71/72 (address writes),
    // 73/74 (address waits) and 83 (an indexed draw from a VA) have no PC equivalent; 59/60 are the newer layout's
    // two unnamed additions.
    EXPECT(raw(list, 59, 1) == 0 && raw(list, 60, 1) == 0 && raw(list, 62, 1, 2, 3, 4) == 0 &&
           raw(list, 63, 1, 2, 3) == 0 && raw(list, 64, 1, 2, 3) == 0 && raw(list, 65, 1, 2, 3) == 0 &&
           raw(list, 71, 1, 2, 3, 4) == 0 && raw(list, 72, 1, 2, 3, 4) == 0 && raw(list, 73, 1, 2, 3, 4) == 0 &&
           raw(list, 74, 1, 2, 3, 4) == 0 && raw(list, 83, 1, 2, 3, 4) == 0);
    EXPECT(total_calls(real) == before);
    // 146 (barrier groups): one global UAV barrier through the PC list's ResourceBarrier (slot 26).
    const unsigned calls26 = real->calls[26];
    EXPECT(raw(list, 146, 3, 0x9000) == 0 && real->calls[26] == calls26 + 1 &&
           static_cast<std::uint32_t>(real->args[26][0]) == 1 && real->args[26][1] != 0);
    // 148 (AllocateUploadMemory): no unified range in this build, so null; the call reaches nothing.
    const unsigned after_groups = total_calls(real);
    EXPECT(raw(list, 148, 64, 16) == 0 && total_calls(real) == after_groups);
    before = after_groups;

    // 139 ResetX(allocator, pipeline, flags) -> PC 10 Reset(allocator, pipeline), the allocator proxy unwrapped.
    EXPECT(hr_of(raw(list, 139, word(allocator), 0x8888, 0xff)) == S_OK);
    EXPECT(real->calls[10] == 1 && real->args[10][0] == real_allocator && real->args[10][1] == 0x8888 &&
           total_calls(real) == before + 1);

    for (void* heap : heaps) {
        Mock* const heap_mock = reinterpret_cast<Mock*>(xs::heap_proxy_real_heap(heap));
        EXPECT(release(heap) == 0 && heap_mock->refs == 0);
    }
    EXPECT(release(list) == 0 && real->refs == 0);
    EXPECT(release(allocator) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console list slots reach their PC slots (32->28 heaps unwrapped, 28->52, 29->53, 50->46, 51->47, "
                "61->59, 70->62 with float arguments, 80->14, 85->16 with the box, 86->17, 100/129->79 with flags "
                "dropped, 139->10 allocator unwrapped, 146->one UAV barrier); 131/132/142 and the memory copy/fill, "
                "address write/wait and DrawIndexedX slots reach nothing\n",
                g_label);
    return 0;
}

// ---- 13. Device slot 133 and placed descriptor heaps (slot 14), through the backend hooks ----

struct AtAddressCall {
    unsigned calls;
    ID3D12Device* real;
    UINT64 address;
    const D3D12_RESOURCE_DESC* desc;
    IID riid;
    Mock* made;
} g_at_address{};

HRESULT test_create_at_address(ID3D12Device* real, UINT64 address, const D3D12_RESOURCE_DESC* desc, REFIID riid,
                               void** out) {
    ++g_at_address.calls;
    g_at_address.real = real;
    g_at_address.address = address;
    g_at_address.desc = desc;
    g_at_address.riid = riid;
    Mock* resource = new_mock(Kind::resource);
    g_at_address.made = resource;
    return mock_query(resource, &riid, out);
}

struct HeapAtCall {
    unsigned calls;
    ID3D12Device* real;
    const D3D12_DESCRIPTOR_HEAP_DESC* desc;
    UINT64 address;
    IID riid;
    Mock* made;
    HRESULT result;  // what the hook answers; S_OK makes a heap
} g_heap_at{};

HRESULT test_create_heap_at(ID3D12Device* real, const D3D12_DESCRIPTOR_HEAP_DESC* desc, UINT64 address, REFIID riid,
                            void** out) {
    ++g_heap_at.calls;
    g_heap_at.real = real;
    g_heap_at.desc = desc;
    g_heap_at.address = address;
    g_heap_at.riid = riid;
    *out = nullptr;
    if (FAILED(g_heap_at.result)) return g_heap_at.result;
    Mock* heap = new_mock(Kind::heap);
    g_heap_at.made = heap;
    return mock_query(heap, &riid, out);
}

int check_placed_hooks() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    xs::set_create_at_address(nullptr);
    xs::set_create_heap_at(nullptr);

    // Slot 133 (address, desc, layout, clear value, riid, out).
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 0x10000;
    constexpr std::uint64_t kAddress = 0x6e900000;
    void* out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 133, kAddress, word(&desc), 0xffffffff, 0, word(&kResourceIid), word(&out))) ==
           E_NOTIMPL && !out);
    xs::set_create_at_address(&test_create_at_address);
    out = reinterpret_cast<void*>(1);
    const HRESULT made = hr_of(raw(device, 133, kAddress, word(&desc), 0xffffffff, 0, word(&kResourceIid), word(&out)));
    const AtAddressCall at = g_at_address;
    void* invalid = reinterpret_cast<void*>(1);
    const HRESULT no_desc = hr_of(raw(device, 133, kAddress, 0, 0xffffffff, 0, word(&kResourceIid), word(&invalid)));
    xs::set_create_at_address(nullptr);
    EXPECT(made == S_OK && at.calls == 1 && at.real == reinterpret_cast<ID3D12Device*>(mock) &&
           at.address == kAddress && at.desc == &desc && IsEqualIID(at.riid, kResourceIid));
    EXPECT(out == at.made && at.made->refs == 1);
    EXPECT(no_desc == E_INVALIDARG && !invalid && g_at_address.calls == 1);
    EXPECT(release(out) == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 133, kAddress, word(&desc), 0xffffffff, 0, word(&kResourceIid), word(&out))) ==
           E_NOTIMPL && !out && g_at_address.calls == 1);  // hook removed again

    // Slot 14 with the console's {address, size} after the PC description.
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    constexpr std::uint64_t kHeapAddress = 0x6de00000;
    struct PlacedDesc {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        std::uint64_t address, size;
    };
    static_assert(sizeof(D3D12_DESCRIPTOR_HEAP_DESC) == 16 && offsetof(PlacedDesc, address) == 16);
    const PlacedDesc placed{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1},
                            kHeapAddress, 64ull * kMockIncrement};
    PlacedDesc mismatched = placed;
    mismatched.size += 1;

    xs::set_create_heap_at(&test_create_heap_at);
    // The size is count * increment: the hook makes the heap at the address, the PC CreateDescriptorHeap is not called.
    unsigned calls14 = mock->calls[14];
    void* heap = nullptr;
    const HRESULT placed_hr = hr_of(raw(device, 14, word(&placed), word(&heap_iid), word(&heap)));
    const HeapAtCall first = g_heap_at;
    const unsigned calls14_after_placed = mock->calls[14], calls15_after_placed = mock->calls[15];
    const std::uint64_t type15_after_placed = mock->args[15][0];
    // Size off by one: the normal PC heap.
    void* pc_heap = nullptr;
    const HRESULT pc_hr = hr_of(raw(device, 14, word(&mismatched), word(&heap_iid), word(&pc_heap)));
    const unsigned hook_calls_after_mismatch = g_heap_at.calls;
    const unsigned calls14_after_mismatch = mock->calls[14];
    const std::uint64_t desc14_after_mismatch = mock->args[14][0];
    // The hook refusing: made as a PC heap after all.
    g_heap_at.result = E_OUTOFMEMORY;
    void* fallback = nullptr;
    const HRESULT fallback_hr = hr_of(raw(device, 14, word(&placed), word(&heap_iid), word(&fallback)));
    g_heap_at.result = S_OK;
    xs::set_create_heap_at(nullptr);

    EXPECT(placed_hr == S_OK && first.calls == 1 && first.real == reinterpret_cast<ID3D12Device*>(mock) &&
           first.desc == &placed.desc && first.address == kHeapAddress &&
           IsEqualIID(first.riid, __uuidof(ID3D12DescriptorHeap)));
    EXPECT(calls14_after_placed == calls14 && calls15_after_placed >= 1 &&
           static_cast<std::uint32_t>(type15_after_placed) == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(xs::is_heap_proxy(heap) && word(xs::heap_proxy_real_heap(heap)) == word(first.made));
    EXPECT(pc_hr == S_OK && hook_calls_after_mismatch == 1 && calls14_after_mismatch == calls14 + 1);
    EXPECT(xs::is_heap_proxy(pc_heap) && desc14_after_mismatch == word(&mismatched));
    EXPECT(fallback_hr == S_OK && g_heap_at.calls == 2 && mock->calls[14] == calls14 + 2 && xs::is_heap_proxy(fallback));
    // No hook: a placed description is a PC heap, and the increment is not even asked for.
    const unsigned calls15 = mock->calls[15];
    void* unhooked = nullptr;
    EXPECT(hr_of(raw(device, 14, word(&placed), word(&heap_iid), word(&unhooked))) == S_OK &&
           xs::is_heap_proxy(unhooked));
    EXPECT(mock->calls[14] == calls14 + 3 && mock->calls[15] == calls15 && g_heap_at.calls == 2);

    for (void* h : {heap, pc_heap, fallback, unhooked}) {
        Mock* const heap_mock = reinterpret_cast<Mock*>(xs::heap_proxy_real_heap(h));
        EXPECT(release(h) == 0 && heap_mock->refs == 0);
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: slot 133 is E_NOTIMPL without a hook and forwards (address, desc, riid, out) to one; slot 14 "
                "takes a heap as placed only when size == count * increment\n", g_label);
    return 0;
}

// ---- Guard-region setup failures: this executable's import table, patched before the first proxy table ----

std::atomic<int> g_injected{0};
bool g_armed = false;
void* g_real = nullptr;

LPVOID WINAPI fail_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (g_armed && (type & MEM_RESERVE) && size == 64 * 1024) {
        ++g_injected;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reinterpret_cast<decltype(&VirtualAlloc)>(g_real)(address, size, type, protect);
}

BOOL WINAPI fail_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (g_armed && protect == PAGE_READONLY) {
        ++g_injected;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return reinterpret_cast<decltype(&VirtualProtect)>(g_real)(address, size, protect, old);
}

PVOID WINAPI fail_add_veh(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    ++g_injected;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

// Points this executable's import of `function` (by name) at `replacement`; the proxy code is linked into it.
bool patch_import(const char* function, void* replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); desc->Name; ++desc) {
        const DWORD lookup = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_real = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);  // through the replacement, not armed yet
            return true;
        }
    }
    return false;
}

int run_fault_mode(const char* mode) {
    struct Mode { const char* flag; const char* function; void* replacement; bool guarded; } const modes[] = {
        {"--fail-virtualalloc", "VirtualAlloc", reinterpret_cast<void*>(&fail_virtual_alloc), false},
        {"--fail-virtualprotect", "VirtualProtect", reinterpret_cast<void*>(&fail_virtual_protect), true},
        {"--fail-veh", "AddVectoredExceptionHandler", reinterpret_cast<void*>(&fail_add_veh), false},
    };
    for (const Mode& m : modes) {
        if (std::strcmp(mode, m.flag) != 0) continue;
        g_label = m.flag;
        EXPECT(patch_import(m.function, m.replacement));
        g_armed = true;
        const int failed = check_tail(m.guarded);
        // Disarmed once the tables exist: with a static CRT the CRT's own calls go through this import table too
        // (its exit path asks VirtualProtect for read-only pages), and only the proxy's calls are meant here.
        g_armed = false;
        if (failed) return 1;
        EXPECT(g_injected.load() > 0);  // the proxy's own call was the one refused
        std::printf("%s: %s refused %d time(s): the tables %s guarded, the proxies forward and refuse their tail as "
                    "before\n", m.flag, m.function, g_injected.load(), m.guarded ? "stay" : "are not");
        return 0;
    }
    return 2;
}

}  // namespace

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on: abort() (an
// exception that escapes, for one), the debug CRT's reports, and Windows' fault and critical-error boxes.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG  // the report functions exist only in the debug CRT (empty macros otherwise)
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

// ---- 14. Console pipeline-state slots ----

int check_pipeline_slots() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    // 86 CreatePipelineState(stream desc, riid, out) -> the PC Device2 method at 47, the description passed as it is.
    struct {
        std::uint64_t type, payload;
    } stream[] = {{2, 0x1234}};  // a GLOBAL_ROOT_SIGNATURE subobject
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), stream};
    const IID iid = __uuidof(ID3D12PipelineState);
    void* out = reinterpret_cast<void*>(1);
    const unsigned calls47 = mock->calls[47];
    EXPECT(hr_of(raw(device, 86, word(&desc), word(&iid), word(&out))) == S_OK);
    EXPECT(mock->calls[47] == calls47 + 1 && mock->args[47][0] == word(&desc) && mock->args[47][1] == word(&iid) &&
           mock->args[47][2] == word(&out) && !out);
    EXPECT(hr_of(raw(device, 86, word(&desc), 0, word(&out))) == E_POINTER && mock->calls[47] == calls47 + 1);
    // 57 DeserializeGraphicsPipelineStateX(state, riid, out): refused, the output cleared, no PC method reached.
    const unsigned calls = total_calls(mock);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 57, 0x1234, word(&iid), word(&out))) == E_NOTIMPL && !out);
    EXPECT(total_calls(mock) == calls);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: slot 86 forwards the stream to PC 47; slot 57 clears its output and refuses without reaching "
                "the device\n", g_label);
    return 0;
}

int main(int argc, char** argv) {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    if (check_tail(true) || check_levels() || check_fence1() || check_create_failures() || check_open_shared() ||
        check_alloc_failures() || check_allocators() || check_xbox_slots() || check_struct_returns() ||
        check_queue_flags() || check_list_slot_map() || check_placed_hooks() || check_pipeline_slots())
        return 1;
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
