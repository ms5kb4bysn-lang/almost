// The unified GPU address arena end to end: the title's memory, allocated through the xmem shim, is GPU memory at the
// address the title uses (rt::xmem::configure_unified, the xmem memory observer and the unified arena of
// d3d12_vulkan_device.cpp; docs/d3d12_vulkan.md 3.3). No runtime copies are involved: what the CPU writes at an address
// the GPU reads through a buffer at that address, and what the GPU writes there the CPU reads in place.
//
//   1. configure_unified(N GiB) before the device exists; SKIP (77) without the range, without
//      VK_EXT_external_memory_host, or when the device made its ordinary arena instead (create_buffer_at_address then
//      answers E_NOTIMPL).
//   2. XMemVirtualAlloc (address 0, MEM_RESERVE | MEM_COMMIT) lands inside [64 KiB, N GiB).
//   3. create_buffer_at_address over it: GetGPUVirtualAddress() is that address.
//   4. GPU reads the CPU's pattern (CopyBufferRegion into a readback buffer); GPU writes (CopyBufferRegion from an upload
//      buffer) are read by the CPU at the address itself.
//   5. Physical pages in a reserve-only 64 KiB-page window: the GPU reads the CPU's pattern through a buffer at the
//      window; after an unmap and a remap of the pages in another order at the same addresses it reads the new order
//      (the old imports were dropped, not kept bound).
//   6. A placed descriptor heap (create_descriptor_heap_at): CPU and GPU starts are the address, and a descriptor
//      written through the CPU handle lands in that memory.
//
// `--gib N` sets the range (default 8). Synthetic data only; no game content.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../shims/d3d12_com.h"
#include "../shims/shims.h"
#include "../shims/xmem.h"

namespace {

using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using XMemAllocatePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, uint64_t*, uint64_t*);
using XMemFreePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, const uint64_t*);
using XMemMapPhysicalPagesFn = void*(WINAPI*)(void*, uint64_t, const uint64_t*);
constexpr DWORD kXmem64kPages = 0x20400000;
constexpr uint64_t kPage64k = 0x10000;

int g_checks = 0;
int g_failures = 0;

bool check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!condition) ++g_failures;
    return condition;
}

template <class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

uint32_t pattern(uint64_t i, uint32_t seed) { return static_cast<uint32_t>(i * 2654435761u) ^ seed; }

void fill(void* at, uint64_t bytes, uint32_t seed) {
    auto* words = static_cast<uint32_t*>(at);
    for (uint64_t i = 0; i < bytes / 4; ++i) words[i] = pattern(i, seed);
}

// The first word index where `at` differs from the pattern, or UINT64_MAX.
uint64_t mismatch(const void* at, uint64_t bytes, uint32_t seed) {
    const auto* words = static_cast<const volatile uint32_t*>(at);
    for (uint64_t i = 0; i < bytes / 4; ++i)
        if (words[i] != pattern(i, seed)) return i;
    return UINT64_MAX;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

struct Gpu {
    ID3D12Device* device = nullptr;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12Fence> fence;
    uint64_t value = 0;
    HANDLE event = nullptr;

    bool init(ID3D12Device* d) {
        device = d;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event && SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue.p))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p)));
    }
    ~Gpu() {
        if (event) CloseHandle(event);
    }

    // Records one copy of `bytes` from src+src_offset to dst+dst_offset, submits it and waits for it.
    bool copy(ID3D12Resource* dst, uint64_t dst_offset, ID3D12Resource* src, uint64_t src_offset, uint64_t bytes) {
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                             IID_PPV_ARGS(&list.p))))
            return false;
        list->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.p, ++value)) || FAILED(fence->SetEventOnCompletion(value, event))) return false;
        return WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    }

    HRESULT committed(D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        const D3D12_RESOURCE_DESC desc = buffer_desc(bytes);
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(out));
    }
};

bool inside(const void* p, uint64_t size, uint64_t end) {
    const auto a = reinterpret_cast<uintptr_t>(p);
    return p && a >= kPage64k && a + size <= end;
}

// Steps 2-4: committed title memory, a buffer at its address, and the GPU round trip both ways.
void test_committed(Gpu& gpu, XMemVirtualAllocFn virtual_alloc, uint64_t end) {
    constexpr uint64_t kBytes = 3 * kPage64k;
    auto* const memory = static_cast<uint8_t*>(
        virtual_alloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
    if (!check(inside(memory, kBytes, end), "XMemVirtualAlloc(MEM_RESERVE | MEM_COMMIT) lands in the unified range"))
        return;
    std::printf("  title memory at 0x%llx\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(memory)));
    fill(memory, kBytes, 0xa5a5f00d);

    const D3D12_RESOURCE_DESC desc = buffer_desc(kBytes);
    Ref<ID3D12Resource> at;
    const HRESULT hr = rt::d3d12::create_buffer_at_address(gpu.device, reinterpret_cast<uintptr_t>(memory), &desc,
                                                           IID_PPV_ARGS(&at.p));
    if (!check(SUCCEEDED(hr) && at.p, "create_buffer_at_address over the title's memory")) return;
    check(at->GetGPUVirtualAddress() == reinterpret_cast<uintptr_t>(memory),
          "the buffer's GPU virtual address is the CPU address");
    check(mismatch(memory, kBytes, 0xa5a5f00d) == UINT64_MAX, "the buffer is the memory as it was (not zeroed)");

    // GPU reads what the CPU wrote.
    Ref<ID3D12Resource> readback;
    if (!check(SUCCEEDED(gpu.committed(D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST,
                                       &readback.p)),
               "readback buffer"))
        return;
    const bool copied = gpu.copy(readback.p, 0, at.p, 0, kBytes);
    check(copied, "CopyBufferRegion from the buffer at the address completes");
    void* mapped = nullptr;
    const D3D12_RANGE all{0, SIZE_T(kBytes)};
    if (copied && SUCCEEDED(readback->Map(0, &all, &mapped)) && mapped) {
        const uint64_t bad = mismatch(mapped, kBytes, 0xa5a5f00d);
        if (bad != UINT64_MAX)
            std::printf("  first difference at byte 0x%llx: 0x%08x\n", static_cast<unsigned long long>(bad * 4),
                        static_cast<const uint32_t*>(mapped)[bad]);
        check(bad == UINT64_MAX, "the GPU reads the CPU's pattern at the address");
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
    } else {
        check(false, "the GPU reads the CPU's pattern at the address (readback Map)");
    }

    // GPU writes, the CPU reads in place: one 64 KiB page in the middle, the pages around it untouched.
    Ref<ID3D12Resource> upload;
    if (!check(SUCCEEDED(gpu.committed(D3D12_HEAP_TYPE_UPLOAD, kPage64k, D3D12_RESOURCE_STATE_GENERIC_READ,
                                       &upload.p)),
               "upload buffer"))
        return;
    if (FAILED(upload->Map(0, nullptr, &mapped)) || !mapped) {
        check(false, "upload Map");
        return;
    }
    fill(mapped, kPage64k, 0x5eed1234);
    upload->Unmap(0, nullptr);
    check(gpu.copy(at.p, kPage64k, upload.p, 0, kPage64k), "CopyBufferRegion into the buffer at the address completes");
    check(mismatch(memory + kPage64k, kPage64k, 0x5eed1234) == UINT64_MAX,
          "the CPU reads the GPU's write at the address itself");
    bool around = mismatch(memory, kPage64k, 0xa5a5f00d) == UINT64_MAX;
    {
        // The third page holds the pattern's words from index 2 * 16384 on.
        const auto* words = reinterpret_cast<const uint32_t*>(memory + 2 * kPage64k);
        for (uint64_t i = 0; i < kPage64k / 4 && around; ++i) around = words[i] == pattern(2 * kPage64k / 4 + i, 0xa5a5f00d);
    }
    check(around, "... and the pages around it keep the CPU's pattern");
}

struct PhysicalApi {
    XMemVirtualAllocFn virtual_alloc;
    XMemAllocatePhysicalPagesFn allocate;
    XMemFreePhysicalPagesFn free_pages;
    XMemMapPhysicalPagesFn map;
};

// Reads [0, bytes) of `resource` through the GPU into a readback buffer and compares it with the expected pages.
bool gpu_reads_pages(Gpu& gpu, ID3D12Resource* resource, uint64_t pages, const uint32_t* seeds) {
    Ref<ID3D12Resource> readback;
    if (FAILED(gpu.committed(D3D12_HEAP_TYPE_READBACK, pages * kPage64k, D3D12_RESOURCE_STATE_COPY_DEST,
                             &readback.p)) ||
        !gpu.copy(readback.p, 0, resource, 0, pages * kPage64k))
        return false;
    void* mapped = nullptr;
    const D3D12_RANGE all{0, SIZE_T(pages * kPage64k)};
    if (FAILED(readback->Map(0, &all, &mapped)) || !mapped) return false;
    bool ok = true;
    for (uint64_t i = 0; i < pages; ++i) {
        const uint64_t bad = mismatch(static_cast<uint8_t*>(mapped) + i * kPage64k, kPage64k, seeds[i]);
        if (bad != UINT64_MAX) {
            std::printf("  page %llu: first difference at word %llu\n", static_cast<unsigned long long>(i),
                        static_cast<unsigned long long>(bad));
            ok = false;
        }
    }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return ok;
}

// Step 5: physical pages mapped into a reserve-only 64 KiB-page window.
void test_physical(Gpu& gpu, const PhysicalApi& api, uint64_t end) {
    constexpr uint64_t kPages = 4;
    auto* const window = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x23, PAGE_READWRITE));
    if (!check(inside(window, kPages * kPage64k, end), "a reserve-only 64 KiB-page window lands in the unified range"))
        return;
    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    if (!check(api.allocate(0x280, &count, pages) == TRUE && count == kPages, "XMemAllocatePhysicalPages")) return;
    if (!check(api.map(window, kPages, pages) == window, "XMemMapPhysicalPages into the window")) {
        api.free_pages(kPages, pages);
        return;
    }
    const uint32_t seeds[kPages] = {0x11110000, 0x22220000, 0x33330000, 0x44440000};
    for (uint64_t i = 0; i < kPages; ++i) fill(window + i * kPage64k, kPage64k, seeds[i]);

    {
        const D3D12_RESOURCE_DESC desc = buffer_desc(kPages * kPage64k);
        Ref<ID3D12Resource> at;
        const HRESULT hr = rt::d3d12::create_buffer_at_address(gpu.device, reinterpret_cast<uintptr_t>(window), &desc,
                                                               IID_PPV_ARGS(&at.p));
        if (check(SUCCEEDED(hr) && at.p, "create_buffer_at_address over the mapped window")) {
            check(at->GetGPUVirtualAddress() == reinterpret_cast<uintptr_t>(window),
                  "the window buffer's GPU virtual address is the window's address");
            check(gpu_reads_pages(gpu, at.p, kPages, seeds), "the GPU reads the CPU's pattern in the mapped pages");

            // Unmap (the imports are unbound and freed), then map the same pages in reverse order at the same
            // addresses: a stale import would still show the old order to the GPU.
            check(api.map(window, kPages, nullptr) == window, "XMemMapPhysicalPages unmaps the window");
            uint64_t reversed[kPages];
            uint32_t reversed_seeds[kPages];
            for (uint64_t i = 0; i < kPages; ++i) {
                reversed[i] = pages[kPages - 1 - i];
                reversed_seeds[i] = seeds[kPages - 1 - i];
            }
            check(api.map(window, kPages, reversed) == window, "the pages map again, in reverse order");
            check(mismatch(window, kPage64k, reversed_seeds[0]) == UINT64_MAX, "... the CPU sees the new order");
            check(gpu_reads_pages(gpu, at.p, kPages, reversed_seeds),
                  "the GPU sees the new order too (the unmapped imports were dropped)");
            check(api.map(window, kPages, nullptr) == window, "XMemMapPhysicalPages unmaps the window again");
        }
    }
    // The device still works after the unbinds: one more round trip through fresh default memory.
    Ref<ID3D12Resource> scratch, readback;
    check(SUCCEEDED(gpu.committed(D3D12_HEAP_TYPE_DEFAULT, kPage64k, D3D12_RESOURCE_STATE_COMMON, &scratch.p)) &&
              SUCCEEDED(gpu.committed(D3D12_HEAP_TYPE_READBACK, kPage64k, D3D12_RESOURCE_STATE_COPY_DEST,
                                      &readback.p)) &&
              gpu.copy(readback.p, 0, scratch.p, 0, kPage64k),
          "the device still copies after the pages were unmapped");
    check(api.free_pages(kPages, pages) == TRUE, "XMemFreePhysicalPages");
}

// Step 6: a descriptor heap in title memory.
void test_placed_heap(Gpu& gpu, XMemVirtualAllocFn virtual_alloc, uint64_t end) {
    constexpr UINT kDescriptors = 1024;
    const UINT increment = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint64_t bytes = uint64_t{kDescriptors} * increment;
    auto* const memory = static_cast<uint8_t*>(
        virtual_alloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
    if (!check(inside(memory, bytes, end), "heap memory lands in the unified range")) return;
    std::memset(memory, 0xcd, size_t(bytes));
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kDescriptors;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = rt::d3d12::create_descriptor_heap_at(gpu.device, &desc, reinterpret_cast<uintptr_t>(memory),
                                                            IID_PPV_ARGS(&heap.p));
    if (!check(SUCCEEDED(hr) && heap.p, "create_descriptor_heap_at over the title's memory")) return;
    check(heap->GetCPUDescriptorHandleForHeapStart().ptr == reinterpret_cast<uintptr_t>(memory),
          "the placed heap's CPU start is the address");
    check(heap->GetGPUDescriptorHandleForHeapStart().ptr == reinterpret_cast<uintptr_t>(memory),
          "the placed heap's GPU start is the address");
    check(memory[0] == 0xcd && memory[bytes - 1] == 0xcd, "the placed heap is the memory as it was (not zeroed)");

    // A descriptor written through the CPU handle lands in that memory.
    Ref<ID3D12Resource> buffer;
    if (FAILED(gpu.committed(D3D12_HEAP_TYPE_DEFAULT, kPage64k, D3D12_RESOURCE_STATE_COMMON, &buffer.p))) {
        check(false, "a buffer for the descriptor");
        return;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{buffer->GetGPUVirtualAddress(), 256};
    D3D12_CPU_DESCRIPTOR_HANDLE slot = heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += 5 * SIZE_T(increment);
    gpu.device->CreateConstantBufferView(&cbv, slot);
    bool written = false;
    for (UINT i = 0; i < increment && !written; ++i) written = memory[5 * increment + i] != 0xcd;
    bool neighbours = true;
    for (UINT i = 0; i < increment; ++i)
        neighbours &= memory[4 * increment + i] == 0xcd && memory[6 * increment + i] == 0xcd;
    check(written && neighbours, "a descriptor written through the CPU handle lands in the title's memory");
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t gib = 8;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gib") == 0 && i + 1 < argc) {
            gib = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: unified_arena_test [--gib N]\n");
            return 2;
        }
    }
    if (gib == 0 || gib > 1024) {
        std::fprintf(stderr, "usage: unified_arena_test [--gib N] (1..1024)\n");
        return 2;
    }
    const uint64_t end = gib << 30;

    // 1. The range before the device: the device's arena is made over it when the device is created.
    const uint64_t reserved = rt::xmem::configure_unified(end);
    if (!reserved) {
        std::printf("SKIP unified_arena: configure_unified(%llu GiB) reserved nothing (no placeholder API?)\n",
                    static_cast<unsigned long long>(gib));
        return 77;
    }
    std::printf("unified range [64 KiB, %llu GiB): %.1f GiB reserved\n", static_cast<unsigned long long>(gib),
                double(reserved) / double(1ull << 30));

    const auto virtual_alloc = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const PhysicalApi physical{
        virtual_alloc,
        reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages")),
        reinterpret_cast<XMemFreePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages")),
        reinterpret_cast<XMemMapPhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages")),
    };
    if (!virtual_alloc || !physical.allocate || !physical.free_pages || !physical.map) {
        std::printf("FAIL the xmem.dll exports are not registered\n");
        return 1;
    }

    int result = 0;
    {
        Ref<ID3D12Device> device;
        if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p)))) {
            std::printf("SKIP unified_arena: no Vulkan device\n");
            return 77;
        }
        const rt::d3d12::VulkanDeviceInfo* info = rt::d3d12::vulkan_device_info(device.p);
        if (!info || !info->external_memory_host) {
            std::printf("SKIP unified_arena: the device has no VK_EXT_external_memory_host\n");
            return 77;
        }
        // Without the unified arena (a range past maxBufferSize, a sparse buffer the driver refused, an import that
        // failed) the device has its ordinary arena, and a buffer at a title address is refused.
        {
            auto* const probe = static_cast<uint8_t*>(
                virtual_alloc(nullptr, kPage64k, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
            const D3D12_RESOURCE_DESC desc = buffer_desc(kPage64k);
            Ref<ID3D12Resource> at;
            const HRESULT hr = probe ? rt::d3d12::create_buffer_at_address(
                                           device.p, reinterpret_cast<uintptr_t>(probe), &desc, IID_PPV_ARGS(&at.p))
                                     : E_OUTOFMEMORY;
            if (hr == E_NOTIMPL) {
                std::printf("SKIP unified_arena: the device made its ordinary arena (maxBufferSize %llu MiB for a "
                            "%llu GiB range)\n",
                            static_cast<unsigned long long>(info->max_buffer_size >> 20),
                            static_cast<unsigned long long>(gib));
                return 77;
            }
            check(SUCCEEDED(hr), "the device has the unified arena (a buffer at a title address is accepted)");
        }

        Gpu gpu;
        if (!check(gpu.init(device.p), "queue and fence")) return 1;
        test_committed(gpu, virtual_alloc, end);
        test_physical(gpu, physical, end);
        test_placed_heap(gpu, virtual_alloc, end);
        result = g_failures ? 1 : 0;
    }
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return result;
}
