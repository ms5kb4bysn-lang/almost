// What other shims need from the xmem shim (xmem.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::xmem {

// Writes `size` bytes into the physical pages `pages[0, page_count)` (page numbers from XMemAllocatePhysicalPages),
// starting `offset` bytes into the first, as if they were one contiguous range. Each page is written through a view of
// its own section, so this works whether or not the title has mapped the page. False for a page number that is not
// live or a range past the last page; bytes before that page have been written.
bool write_physical(const uint64_t* pages, size_t page_count, size_t offset, const void* data, size_t size);

// What the title does with GPU-visible memory, for a GPU layer that backs its buffers with the title's own pages (the
// console's unified memory; runtime/test/vk_host_import_probe.cpp shows such pages import into Vulkan). Every callback
// runs with xmem's lock held and must not call back into xmem. Any may be null.
struct MemoryObserver {
    // An XMemVirtualAlloc reservation that pages are later mapped or committed into: a placeholder reservation, or a
    // reserve-only window in a 64 KiB page mode. `attributes` is the title's XALLOC attribute word.
    void (*reserved)(void* context, uintptr_t base, size_t size, uint64_t attributes);
    // Private memory XMemVirtualAlloc committed, in place of a placeholder or anywhere else.
    void (*committed)(void* context, uintptr_t base, size_t size, uint64_t attributes);
    // A 64 KiB physical page mapped at `address`: the view of `section` at `offset` (what a Vulkan import names).
    void (*page_mapped)(void* context, uintptr_t address, void* section, uint64_t offset);
    // The page at `address` unmapped; the address is a placeholder again.
    void (*page_unmapped)(void* context, uintptr_t address);
};

// The unified range: reserves the free parts of [64 KiB, `bytes`) as placeholders, and from then on every
// XMemVirtualAlloc that lets the system choose its address is carved from them instead, so the title's GPU-visible
// memory lies where one GPU buffer can cover it at offset == address (the translated shaders' heap_base + VA, the
// 32-bit descriptor-table pointers and the BVH builder's 2^40 limit; docs/d3d12_vulkan.md section 3.3). Call once,
// after the title's images are mapped (their ranges stay theirs) and before any of its code runs. Returns the bytes
// reserved (0: none; already configured, or no placeholder API). main.cpp calls it for LEONIDA_UNIFIED_GB.
uint64_t configure_unified(uint64_t bytes);
// Whether a unified range exists, and its end (it starts at 64 KiB).
bool unified_range(uint64_t* end);

// Installs `observer` (null: removes it), then replays what is live: every reservation and every mapped page, so an
// observer that attaches after the title began still sees all of it. One observer at a time.
void set_memory_observer(void* context, const MemoryObserver* observer);

}  // namespace rt::xmem
