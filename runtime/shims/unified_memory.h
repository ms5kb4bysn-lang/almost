// The unified range between xmem (runtime/shims/xmem.cpp) and the GPU layer (d3d12_vulkan_device.cpp).
//
// The GPU layer is a static library that many tests link without xmem, so it does not call rt::xmem directly: xmem
// fills this table at static initialisation when it is linked in (gta6rt), and the layer finds it empty otherwise and
// keeps its ordinary arena. See rt::xmem::configure_unified and the unified arena in d3d12_vulkan_device.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "xmem.h"

namespace rt::unified {

struct Api {
    bool (*range)(uint64_t* end) = nullptr;                           // rt::xmem::unified_range
    void* (*commit)(size_t bytes, size_t alignment) = nullptr;        // rt::xmem::commit_unified
    bool (*release)(void* base, size_t bytes) = nullptr;              // rt::xmem::release_unified
    void (*observe)(void* context, const rt::xmem::MemoryObserver*) = nullptr;  // rt::xmem::set_memory_observer
};

inline Api api;

// The unified range's end, or 0 when there is none (or xmem is not linked).
inline uint64_t range_end() {
    uint64_t end = 0;
    return api.range && api.range(&end) ? end : 0;
}

}  // namespace rt::unified
