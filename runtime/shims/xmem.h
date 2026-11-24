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

}  // namespace rt::xmem
