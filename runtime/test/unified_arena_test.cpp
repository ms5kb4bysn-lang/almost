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
//   7. A linear RGBA8 texture at a title address (create_texture_at_address): its image is device-local and holds
//      the console bytes at the mip's rows (a 60x40 texture at a 256-byte console pitch); a 3-mip chain holds every
//      level; GetGPUVirtualAddress is 0, the registry resolves, release clears it, and recreation at the same
//      address replaces it.
//   8. The same texels in one single-mip SW_4KB_S level, tiled into the title's memory with gfx10::TiledSurface2D:
//      the image holds them detiled.
//   9. A sampled texture over unreadable memory, and one at a misaligned address, become 1x1 placeholders: valid
//      descriptors registered under the address, never null. 0x101 itself is SW_256B_S now and mirrors where the bytes
//      can be read (see 8b).
//   9b. A 4K A8 render target with the console 0x101 word is a real optimal image of the full description, not
//      uploaded (the boot's A8 path).
//  10. A placed SRV (slots 70/71) at an address with no resource makes one from the description and keeps it in the
//      registry; the T# it writes names the title address. At a reserve-only address the view gets a 1x1 placeholder
//      and a valid 1x1 T#.
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
#include "../src/console_texture_layout.h"
#include "../src/gfx10_tiling.h"
#include "../src/gfx10_descriptors.h"

#include <cstddef>
#include <span>

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

D3D12_RESOURCE_DESC texture_desc(uint64_t width, UINT height, DXGI_FORMAT format, UINT32 layout) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(layout);
    return desc;
}

// The value at (x, y) of the synthetic patterns the texture tests write and expect.
uint32_t texel(uint32_t x, uint32_t y) {
    return (0x80402010u ^ (x * 0x00010101u) ^ (y * 0x01000100u) ^ (x * y * 0x00010001u));
}

// Builds the console bytes of one 2D RGBA8 texture (one slice, every mip, tail at offset 0) from a texel pattern.
template <class Texel>
bool build_console_rgba8(const leonida::gfx10::ConsoleTextureLayout& layout, uint8_t* memory, Texel texel_at) {
    namespace gfx = leonida::gfx10;
    if (layout.plane_count != 1 || layout.array_size != 1) return false;
    constexpr uint32_t kBytes = 4;
    gfx::TileMode mode;
    switch (layout.mode) {
    case gfx::SwizzleMode::sw_256b_s: mode = gfx::TileMode::sw_256b_s; break;
    case gfx::SwizzleMode::sw_256b_d: mode = gfx::TileMode::sw_256b_d; break;
    case gfx::SwizzleMode::sw_4kb_s: mode = gfx::TileMode::sw_4kb_s; break;
    case gfx::SwizzleMode::sw_64kb_d: mode = gfx::TileMode::sw_64kb_d; break;
    default: return false;
    }
    const gfx::ConsolePlaneLayout& plane = layout.planes[0];
    gfx::TiledSurface2D probe;
    if (!gfx::TiledSurface2D::make(mode, kBytes, 1, 1, probe)) return false;
    const uint32_t block_width = static_cast<uint32_t>(probe.tile_width());
    const uint32_t block_height = static_cast<uint32_t>(probe.tile_height());
    std::vector<std::byte> tail_block(std::size_t(block_width) * block_height * kBytes, std::byte{0});
    bool has_tail = false;
    for (std::uint32_t mip = 0; mip < layout.mip_levels; ++mip) {
        const gfx::ConsoleMipLayout& level = plane.mips[mip];
        if (level.in_tail) {
            has_tail = true;
            for (uint32_t y = 0; y < level.height; ++y)
                for (uint32_t x = 0; x < level.width; ++x) {
                    const uint32_t value = texel_at(x, y, mip);
                    std::memcpy(tail_block.data() +
                                    (std::size_t(level.tail_y + y) * block_width + level.tail_x + x) * kBytes,
                                &value, sizeof(value));
                }
            continue;
        }
        const std::uint64_t pitch = std::uint64_t(level.pitch) * kBytes;
        std::vector<std::byte> linear(std::size_t(pitch) * level.padded_height, std::byte{0});
        for (uint32_t y = 0; y < level.height; ++y)
            for (uint32_t x = 0; x < level.width; ++x) {
                const uint32_t value = texel_at(x, y, mip);
                std::memcpy(linear.data() + y * pitch + std::uint64_t(x) * kBytes, &value, sizeof(value));
            }
        gfx::TiledSurface2D surface;
        if (!gfx::TiledSurface2D::make(mode, kBytes, level.pitch, level.padded_height, surface)) return false;
        std::vector<std::byte> tiled(surface.tiled_size());
        if (!surface.tile(linear, tiled, std::size_t(pitch))) return false;
        if (plane.offset + level.offset + tiled.size() > layout.size) return false;
        std::memcpy(memory + plane.offset + level.offset, tiled.data(), tiled.size());
    }
    if (has_tail) {
        gfx::TiledSurface2D surface;
        if (!gfx::TiledSurface2D::make(mode, kBytes, block_width, block_height, surface)) return false;
        std::vector<std::byte> tiled(surface.tiled_size());
        if (!surface.tile(tail_block, tiled, std::size_t(block_width) * kBytes)) return false;
        if (plane.offset + tiled.size() > layout.size) return false;
        std::memcpy(memory + plane.offset, tiled.data(), tiled.size());
    }
    return true;
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

    // Copies one mip of `texture` into host memory through a readback buffer, at the footprint's row pitch.
    bool read_texture(ID3D12Resource* texture, UINT subresource, std::vector<uint8_t>& data, UINT64& row_pitch,
                      UINT& rows) {
        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 row_size = 0, total = 0;
        device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &row_size, &total);
        if (!total || !row_size) return false;
        Ref<ID3D12Resource> readback;
        if (FAILED(committed(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, &readback.p)))
            return false;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                             IID_PPV_ARGS(&list.p))))
            return false;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = subresource;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.p, ++value)) || FAILED(fence->SetEventOnCompletion(value, event))) return false;
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return false;
        void* mapped = nullptr;
        const D3D12_RANGE all{0, SIZE_T(total)};
        if (FAILED(readback->Map(0, &all, &mapped)) || !mapped) return false;
        data.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + size_t(total));
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        row_pitch = footprint.Footprint.RowPitch;
        return true;
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
void test_placed_heap(Gpu& gpu, XMemVirtualAllocFn virtual_alloc, uint64_t end) {    constexpr UINT kDescriptors = 1024;
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

// Reads one mip back through a readback buffer and compares every RGBA8 texel with the pattern the console bytes held.
bool texture_matches(Gpu& gpu, ID3D12Resource* texture, uint32_t width, uint32_t height, const char* what,
                     UINT mip = 0, uint32_t offset_x = 0, uint32_t offset_y = 0) {
    std::vector<uint8_t> data;
    UINT64 pitch = 0;
    UINT rows = 0;
    if (!check(gpu.read_texture(texture, mip, data, pitch, rows), "copy a texture at a title address into a buffer"))
        return false;
    if (!check(rows == height, "... the mip has the texture's rows")) return false;
    uint64_t bad = UINT64_MAX;
    for (uint32_t y = 0; y < height && bad == UINT64_MAX; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t actual = 0;
            std::memcpy(&actual, data.data() + uint64_t(y) * pitch + uint64_t(x) * 4, 4);
            if (actual != texel(x + offset_x, y + offset_y)) {
                bad = (uint64_t(y) << 32) | x;
                break;
            }
        }
    }
    if (bad != UINT64_MAX)
        std::printf("  %s: first difference at (%llu, %llu): 0x%08x, expected 0x%08x\n", what,
                    static_cast<unsigned long long>(bad >> 32), static_cast<unsigned long long>(bad & 0xffffffffu),
                    reinterpret_cast<const uint32_t*>(data.data())[(bad >> 32) * (pitch / 4) + (bad & 0xffffffffu)],
                    texel(static_cast<uint32_t>(bad & 0xffffffffu) + offset_x,
                          static_cast<uint32_t>(bad >> 32) + offset_y));
    return check(bad == UINT64_MAX, what);
}

// Steps 7-9: textures at title addresses, phase 1. A linear RGBA8 texture uploaded with the console row pitch; the
// same in one single-mip SW_4KB_S level, whose bytes are built with gfx10::TiledSurface2D; no GPU VA, the registry,
// release and recreation at the same address; 0x101 (SW_256B_S) and a tailed 4 KiB chain mirror; and the placeholders
// for memory that cannot be read (a reserve-only pool, a misaligned address).
void test_texture_at_address(Gpu& gpu, XMemVirtualAllocFn virtual_alloc, uint64_t end) {
    namespace gfx = leonida::gfx10;
    constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr uint32_t kElementBytes = 4;

    // 7. Linear: 60 texels wide at a 64-element (256-byte) pitch, so a row copy that trusts desc.Width is wrong.
    const uint32_t width = 60, height = 40;
    gfx::ConsoleResourceDesc linear_model{};
    linear_model.dimension = gfx::kDimensionTexture2D;
    linear_model.width = width;
    linear_model.height = height;
    linear_model.mip_levels = 1;
    linear_model.format = kFormat;
    linear_model.sample_count = 1;
    linear_model.layout = gfx::kLayoutLinear;
    gfx::ConsoleTextureLayout linear_layout;
    if (!check(gfx::compute_console_texture_layout(linear_model, linear_layout) == gfx::ConsoleLayoutStatus::ok &&
                   linear_layout.mode == gfx::SwizzleMode::linear,
               "the console model reads layout 1 as linear"))
        return;
    auto* const linear_memory = static_cast<uint8_t*>(virtual_alloc(
        nullptr, linear_layout.allocation_size, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
    if (!check(inside(linear_memory, linear_layout.allocation_size, end),
               "linear texture memory lands in the unified range"))
        return;
    std::memset(linear_memory, 0xee, size_t(linear_layout.allocation_size));  // padding must not be read
    const uint64_t linear_pitch = uint64_t(linear_layout.planes[0].mips[0].pitch) * kElementBytes;
    check(linear_pitch == 256, "the linear mip has a 256-byte row pitch for 60 RGBA8 texels");
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t value = texel(x, y);
            std::memcpy(linear_memory + y * linear_pitch + x * kElementBytes, &value, sizeof(value));
        }
    const uint64_t linear_address = reinterpret_cast<uintptr_t>(linear_memory);
    D3D12_RESOURCE_DESC linear_desc = texture_desc(width, height, kFormat, uint32_t(D3D12_TEXTURE_LAYOUT_ROW_MAJOR));
    Ref<ID3D12Resource> linear;
    const HRESULT linear_hr =
        rt::d3d12::create_texture_at_address(gpu.device, linear_address, &linear_desc,
                                             D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&linear.p));
    if (!check(SUCCEEDED(linear_hr) && linear.p, "a linear RGBA8 texture at a title address")) {
        std::printf("  create_texture_at_address: 0x%08lx\n", static_cast<unsigned long>(linear_hr));
        return;
    }
    check(linear->GetGPUVirtualAddress() == 0, "the texture at the address has no GPU VA (GetGPUVirtualAddress is 0)");
    check(rt::d3d12::placed_texture_at_address(gpu.device, linear_address) == linear.p,
          "the device's registry resolves the title address to the texture");
    texture_matches(gpu, linear.p, width, height, "the linear texture's texels come back at the mip's rows, not the pitch");

    // 7b. A multi-mip linear chain: every level is uploaded at its own console offset and pitch.
    const uint32_t multi_width = 64, multi_height = 32, multi_mips = 3;
    gfx::ConsoleResourceDesc multi_model{};
    multi_model.dimension = gfx::kDimensionTexture2D;
    multi_model.width = multi_width;
    multi_model.height = multi_height;
    multi_model.mip_levels = multi_mips;
    multi_model.format = kFormat;
    multi_model.sample_count = 1;
    multi_model.layout = gfx::kLayoutLinear;
    gfx::ConsoleTextureLayout multi_layout;
    if (!check(gfx::compute_console_texture_layout(multi_model, multi_layout) == gfx::ConsoleLayoutStatus::ok &&
                   multi_layout.mip_levels == multi_mips,
               "the console model takes a 3-mip linear chain"))
        return;
    auto* const multi_memory = static_cast<uint8_t*>(
        virtual_alloc(nullptr, multi_layout.allocation_size, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
    if (!check(inside(multi_memory, multi_layout.allocation_size, end), "the linear chain lands in the unified range"))
        return;
    std::memset(multi_memory, 0xee, size_t(multi_layout.allocation_size));
    for (uint32_t mip = 0; mip < multi_mips; ++mip) {
        const gfx::ConsoleMipLayout& level = multi_layout.planes[0].mips[mip];
        const uint64_t pitch = uint64_t(level.pitch) * kElementBytes;
        for (uint32_t y = 0; y < level.height; ++y)
            for (uint32_t x = 0; x < level.width; ++x) {
                const uint32_t value = texel(x + 137 * mip, y + 71 * mip);
                std::memcpy(multi_memory + level.offset + y * pitch + x * kElementBytes, &value, sizeof(value));
            }
    }
    const uint64_t multi_address = reinterpret_cast<uintptr_t>(multi_memory);
    D3D12_RESOURCE_DESC multi_desc =
        texture_desc(multi_width, multi_height, kFormat, uint32_t(D3D12_TEXTURE_LAYOUT_ROW_MAJOR));
    multi_desc.MipLevels = multi_mips;
    Ref<ID3D12Resource> multi;
    const HRESULT multi_hr =
        rt::d3d12::create_texture_at_address(gpu.device, multi_address, &multi_desc,
                                             D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&multi.p));
    if (!check(SUCCEEDED(multi_hr) && multi.p, "a 3-mip linear texture at a title address")) {
        std::printf("  create_texture_at_address: 0x%08lx\n", static_cast<unsigned long>(multi_hr));
        return;
    }
    texture_matches(gpu, multi.p, multi_width, multi_height, "mip 0 of the chain holds its texels");
    texture_matches(gpu, multi.p, multi_width / 4, multi_height / 4, "mip 2 of the chain holds its texels", 2,
                    137 * 2, 71 * 2);

    // 8. SW_4KB_S: the same texels, tiled into the title's bytes with gfx10::TiledSurface2D.
    const uint32_t tiled_width = 128, tiled_height = 64;
    gfx::ConsoleResourceDesc tiled_model{};
    tiled_model.dimension = gfx::kDimensionTexture2D;
    tiled_model.width = tiled_width;
    tiled_model.height = tiled_height;
    tiled_model.mip_levels = 1;
    tiled_model.format = kFormat;
    tiled_model.sample_count = 1;
    tiled_model.layout = gfx::kLayoutSwizzleFlag | static_cast<uint32_t>(gfx::SwizzleMode::sw_4kb_s);
    gfx::ConsoleTextureLayout tiled_layout;
    if (!check(gfx::compute_console_texture_layout(tiled_model, tiled_layout) == gfx::ConsoleLayoutStatus::ok &&
                   tiled_layout.mode == gfx::SwizzleMode::sw_4kb_s,
               "the console model reads 0x105 as SW_4KB_S"))
        return;
    auto* const tiled_memory = static_cast<uint8_t*>(
        virtual_alloc(nullptr, tiled_layout.allocation_size, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
    if (!check(inside(tiled_memory, tiled_layout.allocation_size, end),
               "SW_4KB_S texture memory lands in the unified range"))
        return;
    std::vector<std::byte> tiled_linear(size_t(tiled_width) * tiled_height * kElementBytes);
    for (uint32_t y = 0; y < tiled_height; ++y)
        for (uint32_t x = 0; x < tiled_width; ++x) {
            const uint32_t value = texel(x, y);
            std::memcpy(tiled_linear.data() + (size_t(y) * tiled_width + x) * kElementBytes, &value, sizeof(value));
        }
    gfx::TiledSurface2D surface;
    if (!check(gfx::TiledSurface2D::make(gfx::TileMode::sw_4kb_s, kElementBytes, tiled_width, tiled_height, surface),
               "the GFX10 SW_4KB_S detiler takes the level"))
        return;
    check(surface.tiled_size() == tiled_layout.planes[0].slice_stride,
          "... and sizes it as the console model does");
    if (!check(surface.tile(std::span<const std::byte>(tiled_linear),
                            std::span<std::byte>(reinterpret_cast<std::byte*>(tiled_memory), surface.tiled_size()),
                            size_t(tiled_width) * kElementBytes),
               "the expected texels tile into the title's bytes"))
        return;
    check(std::memcmp(tiled_memory, tiled_linear.data(),
                      std::min<size_t>(tiled_layout.planes[0].slice_stride, tiled_linear.size())) != 0,
          "the console bytes are tiled, not already rows");
    const uint64_t tiled_address = reinterpret_cast<uintptr_t>(tiled_memory);
    D3D12_RESOURCE_DESC tiled_desc = texture_desc(tiled_width, tiled_height, kFormat, tiled_model.layout);
    Ref<ID3D12Resource> tiled;
    const HRESULT tiled_hr =
        rt::d3d12::create_texture_at_address(gpu.device, tiled_address, &tiled_desc,
                                             D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&tiled.p));
    if (!check(SUCCEEDED(tiled_hr) && tiled.p, "a single-mip SW_4KB_S texture at a title address")) {
        std::printf("  create_texture_at_address: 0x%08lx\n", static_cast<unsigned long>(tiled_hr));
        return;
    }
    check(tiled->GetGPUVirtualAddress() == 0, "... and it has no GPU VA either");
    check(rt::d3d12::placed_texture_at_address(gpu.device, tiled_address) == tiled.p,
          "... and its address resolves in the registry");
    texture_matches(gpu, tiled.p, tiled_width, tiled_height, "the SW_4KB_S bytes detile back to the texels");

    // 8b. 0x101 is SW_256B_S (VIctory-fork resource.rs:150-157 reads 0x100 | n as addrlib mode n): a readable
    // 256-byte-tiled level mirrors like the larger modes, and the T# still names the title address.
    const uint32_t small_tile_width = 128, small_tile_height = 64;
    gfx::ConsoleResourceDesc b256_model{};
    b256_model.dimension = gfx::kDimensionTexture2D;
    b256_model.width = small_tile_width;
    b256_model.height = small_tile_height;
    b256_model.mip_levels = 3;
    b256_model.format = kFormat;
    b256_model.sample_count = 1;
    b256_model.layout = 0x101;
    gfx::ConsoleTextureLayout b256_layout;
    if (!check(gfx::compute_console_texture_layout(b256_model, b256_layout) == gfx::ConsoleLayoutStatus::ok &&
                   b256_layout.mode == gfx::SwizzleMode::sw_256b_s,
               "the console model reads 0x101 as SW_256B_S")) {
        return;
    }
    auto* const b256_memory = static_cast<uint8_t*>(
        virtual_alloc(nullptr, b256_layout.allocation_size, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
    if (!check(inside(b256_memory, b256_layout.allocation_size, end),
               "SW_256B_S texture memory lands in the unified range"))
        return;
    if (!check(build_console_rgba8(b256_layout, b256_memory,
                                   [](uint32_t x, uint32_t y, uint32_t mip) {
                                       return texel(x + 300 * mip, y + 120 * mip);
                                   }),
               "the expected texels tile into the 256-byte blocks"))
        return;
    const uint64_t b256_address = reinterpret_cast<uintptr_t>(b256_memory);
    D3D12_RESOURCE_DESC b256_desc = texture_desc(small_tile_width, small_tile_height, kFormat, 0x101);
    b256_desc.MipLevels = 3;
    Ref<ID3D12Resource> b256;
    if (check(SUCCEEDED(rt::d3d12::create_texture_at_address(gpu.device, b256_address, &b256_desc,
                                                             D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                                                             nullptr, IID_PPV_ARGS(&b256.p))) &&
                  b256.p,
              "a readable 0x101 SW_256B_S texture at a title address mirrors")) {
        check(rt::d3d12::placed_texture_at_address(gpu.device, b256_address) == b256.p,
              "... and its address resolves in the registry");
        texture_matches(gpu, b256.p, small_tile_width, small_tile_height,
                        "the SW_256B_S bytes detile back to the texels");
        texture_matches(gpu, b256.p, small_tile_width / 4, small_tile_height / 4,
                        "mip 2 of the SW_256B_S chain holds its texels", 2, 600, 240);
    }

    // 8c. SW_4KB_S with a mip tail (64x64, 4 mips: mips 2 and 3 share the leading 4 KiB block, smallest first).
    const uint32_t tail_width = 64, tail_height = 64, tail_mips = 4;
    gfx::ConsoleResourceDesc tail_model{};
    tail_model.dimension = gfx::kDimensionTexture2D;
    tail_model.width = tail_width;
    tail_model.height = tail_height;
    tail_model.mip_levels = tail_mips;
    tail_model.format = kFormat;
    tail_model.sample_count = 1;
    tail_model.layout = 0x105;
    gfx::ConsoleTextureLayout tail_layout;
    if (!check(gfx::compute_console_texture_layout(tail_model, tail_layout) == gfx::ConsoleLayoutStatus::ok &&
                   tail_layout.mode == gfx::SwizzleMode::sw_4kb_s && tail_layout.planes[0].first_tail_mip == 2 &&
                   tail_layout.planes[0].mips[2].in_tail && tail_layout.planes[0].mips[3].in_tail,
               "the console model packs the last two of four 4 KiB mips into the tail"))
        return;
    auto* const tail_memory = static_cast<uint8_t*>(
        virtual_alloc(nullptr, tail_layout.allocation_size, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE));
    if (!check(inside(tail_memory, tail_layout.allocation_size, end),
               "the tailed texture memory lands in the unified range"))
        return;
    if (!check(build_console_rgba8(tail_layout, tail_memory,
                                   [](uint32_t x, uint32_t y, uint32_t mip) {
                                       return texel(x + 200 * mip, y + 90 * mip);
                                   }),
               "the expected texels tile into the standard mips and the tail block"))
        return;
    const uint64_t tail_address = reinterpret_cast<uintptr_t>(tail_memory);
    D3D12_RESOURCE_DESC tail_desc = texture_desc(tail_width, tail_height, kFormat, 0x105);
    tail_desc.MipLevels = tail_mips;
    Ref<ID3D12Resource> tailed;
    if (check(SUCCEEDED(rt::d3d12::create_texture_at_address(gpu.device, tail_address, &tail_desc,
                                                             D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                                                             nullptr, IID_PPV_ARGS(&tailed.p))) &&
                  tailed.p,
              "a 4-mip SW_4KB_S texture with a tail mirrors")) {
        texture_matches(gpu, tailed.p, tail_width, tail_height, "mip 0 of the tailed texture holds its texels");
        texture_matches(gpu, tailed.p, 16, 16, "mip 2 from the tail block holds its texels", 2, 400, 180);
        texture_matches(gpu, tailed.p, 8, 8, "mip 3 from the tail block holds its texels", 3, 600, 270);
    }

    // 9. Release, registry, recreation. A newer texture at the address keeps the registry entry.
    linear.reset();
    check(rt::d3d12::placed_texture_at_address(gpu.device, linear_address) == nullptr,
          "releasing the texture removes its registry entry");
    Ref<ID3D12Resource> recreated;
    if (check(SUCCEEDED(rt::d3d12::create_texture_at_address(gpu.device, linear_address, &linear_desc,
                                                             D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                                                             nullptr, IID_PPV_ARGS(&recreated.p))) &&
                  recreated.p,
              "a texture is made again at the released address")) {
        check(recreated->GetGPUVirtualAddress() == 0, "... with no GPU VA");
        check(rt::d3d12::placed_texture_at_address(gpu.device, linear_address) == recreated.p,
              "... and the registry entry is the new texture");
        Ref<ID3D12Resource> newer;
        if (check(SUCCEEDED(rt::d3d12::create_texture_at_address(gpu.device, linear_address, &linear_desc,
                                                                 D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                                                                 nullptr, IID_PPV_ARGS(&newer.p))) &&
                      newer.p,
                  "a newer texture at the same address replaces it")) {
            check(rt::d3d12::placed_texture_at_address(gpu.device, linear_address) == newer.p,
                  "... the registry entry is the newer texture");
            recreated.reset();
            check(rt::d3d12::placed_texture_at_address(gpu.device, linear_address) == newer.p,
                  "... and releasing the older texture keeps the newer entry");
            newer.reset();
            check(rt::d3d12::placed_texture_at_address(gpu.device, linear_address) == nullptr,
                  "... whose release clears the entry");
        }
    }

    // 9b. A sampled texture over memory that cannot be read is a 1x1 placeholder, not a null resource: the view
    // descriptors the boot needs stay valid while the console bytes are not mirrored (reserve-only page pools).
    const uint64_t pool_address = tiled_address + 0x200000;  // nothing is committed there
    D3D12_RESOURCE_DESC ambiguous = texture_desc(tiled_width, tiled_height, kFormat, 0x101);
    Ref<ID3D12Resource> placeholder;
    if (check(SUCCEEDED(rt::d3d12::create_texture_at_address(gpu.device, pool_address, &ambiguous,
                                                             D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                                                             nullptr, IID_PPV_ARGS(&placeholder.p))) &&
                  placeholder.p,
              "a sampled 0x101 texture over unreadable memory gets a placeholder")) {
        const D3D12_RESOURCE_DESC made = placeholder->GetDesc();
        check(made.Width == 1 && made.Height == 1 && made.Format == kFormat,
              "... a 1x1 resource in the description's format");
        check(placeholder->GetGPUVirtualAddress() == 0, "... with no GPU VA");
        check(rt::d3d12::placed_texture_at_address(gpu.device, pool_address) == placeholder.p,
              "... registered under the title address");
    }
    // A description that cannot be mirrored because its address is off the console layout's alignment takes the same
    // placeholder path: a valid descriptor matters more than refusing the misplaced bytes.
    D3D12_RESOURCE_DESC misaligned = texture_desc(width, height, kFormat, uint32_t(D3D12_TEXTURE_LAYOUT_ROW_MAJOR));
    Ref<ID3D12Resource> off_alignment;
    check(SUCCEEDED(rt::d3d12::create_texture_at_address(gpu.device, linear_address + 128, &misaligned,
                                                         D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                                         IID_PPV_ARGS(&off_alignment.p))) &&
              off_alignment.p && off_alignment->GetDesc().Width == 1,
          "an unmirrorable address off the layout's alignment gets a placeholder too");

    // 9c. A render target keeps the full description as an optimal image, console swizzle word and all, with no
    // upload: the A8 4K path the boot needs. Nothing has to be readable at the address.
    constexpr uint32_t rt_width = 3840, rt_height = 2160;
    const uint64_t rt_address = linear_address + 0x400000;
    D3D12_RESOURCE_DESC rt_desc = texture_desc(rt_width, rt_height, DXGI_FORMAT_A8_UNORM, 0x101);
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Ref<ID3D12Resource> rt;
    if (check(SUCCEEDED(rt::d3d12::create_texture_at_address(gpu.device, rt_address, &rt_desc,
                                                             D3D12_BARRIER_LAYOUT_RENDER_TARGET, nullptr,
                                                             IID_PPV_ARGS(&rt.p))) &&
                  rt.p,
              "a 4K A8 render target with the console 0x101 word is a real image")) {
        const D3D12_RESOURCE_DESC made = rt->GetDesc();
        check(made.Width == rt_width && made.Height == rt_height && made.Format == DXGI_FORMAT_A8_UNORM,
              "... of the full description");
        check(rt::d3d12::vulkan_image(rt.p) != VK_NULL_HANDLE, "... backed by a Vulkan image (not uploaded)");
        check(rt->GetGPUVirtualAddress() == 0, "... with no GPU VA");
        check(rt::d3d12::placed_texture_at_address(gpu.device, rt_address) == rt.p,
              "... registered under the title address");
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.NumDescriptors = 1;
        Ref<ID3D12DescriptorHeap> rtv_heap;
        if (check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))),
                  "... an RTV heap")) {
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            gpu.device->CreateRenderTargetView(rt.p, nullptr, rtv);
            const UINT increment = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            bool written = false;
            for (UINT i = 0; i < increment && !written; ++i)
                written = reinterpret_cast<const std::uint8_t*>(rtv.ptr)[i] != 0;
            check(written, "... whose A8 render-target view is a real descriptor");
        }
    }

    // 10. A placed SRV (device slots 70/71) at an address with no resource makes one from the description and keeps
    // it in the registry; a second view at the address reuses it. The T# the view wrote names the title address.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    Ref<ID3D12DescriptorHeap> heap;
    if (!check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))), "a descriptor heap"))
        return;
    std::uint32_t view[16]{};
    view[0] = kFormat;
    view[1] = D3D12_SRV_DIMENSION_TEXTURE2D;
    view[2] = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view[4] = 0;  // MostDetailedMip
    view[5] = 1;  // MipLevels
    view[6] = 0;  // PlaneSlice
    view[7] = 0;  // ResourceMinLODClamp
    view[10] = static_cast<std::uint32_t>(linear_address);
    view[11] = static_cast<std::uint32_t>(linear_address >> 32);
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    rt::d3d12::create_placed_view(gpu.device, false, &linear_desc, view, handle);
    ID3D12Resource* const lazy = rt::d3d12::placed_texture_at_address(gpu.device, linear_address);
    if (check(lazy != nullptr, "a placed SRV at an address with no resource makes and registers one")) {
        check(lazy->GetGPUVirtualAddress() == 0, "... with no GPU VA");
        rt::d3d12::create_placed_view(gpu.device, false, &linear_desc, view, handle);
        check(rt::d3d12::placed_texture_at_address(gpu.device, linear_address) == lazy,
              "... and a second view at the address reuses the same texture");
        leonida::gfx10::ImageWords tsharp{};
        std::memcpy(tsharp.data(), reinterpret_cast<const void*>(handle.ptr), sizeof(tsharp));
        leonida::gfx10::Image image{};
        check(leonida::gfx10::is_image(tsharp) && leonida::gfx10::decode(tsharp, image) &&
                  image.address == linear_address && image.width == width && image.height == height,
              "... whose T# names the title address and the texture's size");
    }

    // 10b. A view at a reserve-only address whose texture cannot be mirrored makes a placeholder, and the T# names
    // the title address at 1x1: the boot's page-pool views are valid descriptors even before phase 2 streams them.
    const uint64_t pool_view_address = tiled_address + 0x600000;
    view[10] = static_cast<std::uint32_t>(pool_view_address);
    view[11] = static_cast<std::uint32_t>(pool_view_address >> 32);
    rt::d3d12::create_placed_view(gpu.device, false, &ambiguous, view, handle);
    ID3D12Resource* const pool_view = rt::d3d12::placed_texture_at_address(gpu.device, pool_view_address);
    if (check(pool_view != nullptr, "a placed SRV at a reserve-only address gets a placeholder")) {
        check(pool_view->GetDesc().Width == 1 && pool_view->GetDesc().Height == 1,
              "... a 1x1 resource");
        leonida::gfx10::ImageWords pool_tsharp{};
        std::memcpy(pool_tsharp.data(), reinterpret_cast<const void*>(handle.ptr), sizeof(pool_tsharp));
        leonida::gfx10::Image pool_image{};
        check(leonida::gfx10::is_image(pool_tsharp) && leonida::gfx10::decode(pool_tsharp, pool_image) &&
                  pool_image.address == pool_view_address && pool_image.width == 1 && pool_image.height == 1,
              "... whose T# names the title address at 1x1");
    }
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
        test_texture_at_address(gpu, virtual_alloc, end);
        result = g_failures ? 1 : 0;
    }
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return result;
}
