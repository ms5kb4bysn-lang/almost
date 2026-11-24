// xg_xs.dll: the console's texture layout library. The game executable (1.0.2.1) imports two functions from it.
//
// Everything below was read statically from game_scarlett_master.exe (image base 0x140000000; RVAs are exe+offset).
//
// The description both exports take is a D3D12_RESOURCE_DESC, 0x38 bytes, field for field:
//   +0x00 u32 Dimension         (1 buffer, 2 1D, 3 2D, 4 3D)
//   +0x08 u64 Alignment
//   +0x10 u64 Width
//   +0x18 u32 Height
//   +0x1c u16 DepthOrArraySize
//   +0x1e u16 MipLevels         (0 = the full chain)
//   +0x20 u32 Format            (DXGI_FORMAT number)
//   +0x24 u32 SampleCount
//   +0x28 u32 SampleQuality
//   +0x2c u32 Layout            (the console swizzle word)
//   +0x30 u32 Flags             (D3D12_RESOURCE_FLAGS)
// Measured from the two copies of the game's fill helper (exe+0x4ecc940 and exe+0xc4b030), which build the description
// from the game's own 14-byte texture record and then call XGCreateTextureComputer with it:
//   record +0x00 u16 width, +0x02 u16 height, +0x04 u16 depth/array, +0x06 u8 depth class (0 1D, else 2D, 3 3D),
//   +0x07 u8 format, +0x08 u8 swizzle byte, +0x09 u8 sample count, +0x0a u8 mip levels, +0x0b..+0x0d flag bytes.
// The helper's layout field (+0x2c) is first zeroed and written as 0x100 | n for a byte n it already knows, 0x100 for
// byte 0 (linear), 0x102 and 0x10a for two special cases, and left to XGSuggestSwizzleMode when the byte is 0xff.
//
// XGSuggestSwizzleMode(const desc*, flags, uint32_t* mode), call sites exe+0xc4b3ce and exe+0x4eccca2 (one per copy of
// the fill helper): the second argument is 0 and the third is the address of the description's own +0x2c field. The
// helper does not touch the field after the call (exe+0x4eccca7 stores the HRESULT and returns), and in the known-mode
// path it writes 0x100 | n itself. The library therefore writes the complete word 0x100 | mode, not a raw ADDR_SW_*
// number. These two sites are only reached when the game's swizzle byte is 0xff.
//
// XGCreateTextureComputer(const desc*, computer** out), call sites exe+0x344235, exe+0x979c75, exe+0x16f7141 and
// exe+0x5121a5e. Two sites test the HRESULT, two do not, so the object is always stored in *out, even on failure.
// The computer's vtable slots the game calls:
//   slot 1  Release (three sites; slot 0 AddRef is the standard partner, no site calls it)
//   slot 2  WriteLayout(computer, void* out) at exe+0x979c9f and exe+0x5121a84. The output is
//             0x18 bytes of header (never read by either caller) followed by one 0x6c8-byte record per plane:
//               record +0x10  qword, the plane's slice stride (used at exe+0x5121854 as slice * this + mip offset)
//               record +0x38 + 0x70*mip, a per-mip record (at most 15; 0x38 + 15*0x70 = 0x6c8):
//                 +0x00 qword, bytes of the mip across its depth slices (exe+0x512186a uses it as a range length,
//                       exe+0x979cb6 as an allocation size for a detile buffer in the view's method at exe+0x979e30)
//                 +0x08 qword, byte offset of the mip inside the plane (exe+0x512186a adds it)
//                 +0x10 qword, bytes of one depth slice of the mip (exe+0x979cca, exe+0x5121874)
//                 +0x1c dword, row pitch in bytes (exe+0x979cac and exe+0x5121879; the view compares it with its own
//                       linear pitch at exe+0x979e4b)
//             The offsets and strides come from runtime/src/console_texture_layout.h, the same model device slot 25
//             uses to size the allocation, so the game's buffer holds exactly these bytes.
//           Fields +0x04..+0x07, +0x0c..+0x1b (per mip record) and all of the header are not read by either call site
//           and are zeroed.
//   slot 8  CopySubresource(computer, dst, 0, subresource, src, src row pitch, src size) at exe+0x16f71ff, in a loop
//           over subresources (exe+0x16f7100). The first argument is the GPU-visible buffer the caller allocated from
//           the console allocation size (exe+0x193df48: the result of exe+0x2474690, an allocator, sized by
//           GetResourceAllocationInfo at exe+0x193df1d). The call writes texture data, it does not return a structure.
//           Implemented only for the case that is a copy rather than a tiling: a linear layout whose source row pitch
//           matches the layout's pitch. Every other layout returns E_NOTIMPL and writes nothing (see below).
//   slot 9  CopySubresource with one extra zero argument, called by the texture view's method 5 (exe+0x979e96) as
//           (dst, 0, subresource, 0, src, pitch, size); implemented like slot 8.
//   slot 10 WriteTileTable at exe+0x344283, seven arguments: (computer, u32* count, void* counts, void* counts2,
//           u32* value, 0, void* records). The first caller (exe+0x51fbba9) reads byte +0 of the third argument as a
//           record count and per-record 12-byte {u32, u16, u16} entries from the seventh, so a wrong count or entry
//           changes its texture cache. No field meaning is measured, so this slot returns E_NOTIMPL and writes
//           nothing; its 0x20-byte output structure is zeroed by the caller (exe+0x344290..0x344294).
//
// Unmeasured, and deliberately not guessed: the byte-level tiling (slot 8/9 for tiled and block-compressed layouts),
// the tile table (slot 10), and the fields of the slot-2 record listed as zeroed above. The object is created per
// XGCreateTextureComputer call and reference counted (AddRef/Release); one create site keeps it in its texture view
// (exe+0x979c6e stores the out pointer at view+8) and more than one can be alive at the same time.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "shims.h"
#include "../src/console_texture_layout.h"

namespace {

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr size_t kSlots = 32;

namespace gfx = leonida::gfx10;

// D3D12_RESOURCE_DESC with its tail padding: 0x38 bytes on x64. Field offsets match the game's fill helpers.
struct ResourceDesc {
    std::uint32_t dimension;
    std::uint32_t padding0;
    std::uint64_t alignment;
    std::uint64_t width;
    std::uint32_t height;
    std::uint16_t depth_or_array_size;
    std::uint16_t mip_levels;
    std::uint32_t format;
    std::uint32_t sample_count;
    std::uint32_t sample_quality;
    std::uint32_t layout;
    std::uint32_t flags;
    std::uint32_t padding1;
};
static_assert(sizeof(ResourceDesc) == 0x38, "the console description is a D3D12_RESOURCE_DESC");

// The slot-2 output: a 0x18-byte header and one record per plane.
constexpr std::uint32_t kLayoutHeaderBytes = 0x18;
constexpr std::uint32_t kPlaneRecordBytes = 0x6c8;
constexpr std::uint32_t kMipRecordOffset = 0x38;
constexpr std::uint32_t kMipRecordBytes = 0x70;
constexpr std::uint32_t kMaxMipRecords = 15;  // (0x6c8 - 0x38) / 0x70; GFX10 reaches at most 15 mips

// Prints once per entry point; `slot` is the vtable index for a computer method, or -1 for an export.
void warn_once(std::atomic<bool>& warned, const char* what, int slot, const void* caller) {
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    char name[64];
    if (slot < 0) std::snprintf(name, sizeof(name), "%s", what);
    else std::snprintf(name, sizeof(name), "%s%d", what, slot);
    std::fprintf(stderr, "[rt] xg_xs: %s: not implemented; E_NOTIMPL (caller %s, logged once)\n", name,
                 rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
}

struct Computer {
    const void* const* vtable;
    std::atomic<std::uint32_t> refs{1};
    bool known = false;
    gfx::ConsoleResourceDesc desc{};
    gfx::ConsoleTextureLayout layout{};
};

bool read_desc(const void* raw, gfx::ConsoleResourceDesc& out) {
    if (!raw) return false;
    const auto* desc = static_cast<const ResourceDesc*>(raw);
    out.dimension = desc->dimension;
    out.alignment = desc->alignment;
    out.width = desc->width;
    out.height = desc->height;
    out.depth_or_array_size = desc->depth_or_array_size;
    out.mip_levels = desc->mip_levels;
    out.format = desc->format;
    out.sample_count = desc->sample_count;
    out.sample_quality = desc->sample_quality;
    out.layout = desc->layout;
    out.flags = desc->flags;
    return true;
}

// Bytes of one row of a mip, from the model's padded pitch (elements) and the plane's element size.
std::uint64_t row_bytes(const gfx::ConsolePlaneLayout& plane, const gfx::ConsoleMipLayout& mip) {
    return std::uint64_t{mip.pitch} * plane.element_bytes;
}

// Writes the measured part of the slot-2 layout structure. Returns S_OK for a description the model knows; an
// unknown description zeroes the header and the plane records the caller asked for and returns E_NOTIMPL.
HRESULT __stdcall computer_write_layout(Computer* self, void* out) {
    if (!out) return E_POINTER;
    const std::uint32_t planes = self->known ? self->layout.plane_count : 1;
    std::memset(out, 0, kLayoutHeaderBytes + std::size_t{planes} * kPlaneRecordBytes);
    if (!self->known) return kNotImpl;

    auto* base = static_cast<std::uint8_t*>(out);
    for (std::uint32_t plane_index = 0; plane_index < self->layout.plane_count; ++plane_index) {
        const gfx::ConsolePlaneLayout& plane = self->layout.planes[plane_index];
        auto* record = base + kLayoutHeaderBytes + std::size_t{plane_index} * kPlaneRecordBytes;
        std::memcpy(record + 0x10, &plane.slice_stride, sizeof(plane.slice_stride));  // slice stride (qword)
        const std::uint32_t mips = std::min<std::uint32_t>(self->layout.mip_levels, kMaxMipRecords);
        for (std::uint32_t mip_index = 0; mip_index < mips; ++mip_index) {
            const gfx::ConsoleMipLayout& mip = plane.mips[mip_index];
            const std::uint64_t pitch = row_bytes(plane, mip);
            const std::uint64_t slice = pitch * mip.padded_height;
            const std::uint64_t total = slice * std::max(mip.depth, 1u);
            const auto pitch32 = static_cast<std::uint32_t>(pitch);
            auto* entry = record + kMipRecordOffset + std::size_t{mip_index} * kMipRecordBytes;
            std::memcpy(entry + 0x00, &total, sizeof(total));   // bytes of the mip over its depth slices
            std::memcpy(entry + 0x08, &mip.offset, sizeof(mip.offset));  // offset inside the plane
            std::memcpy(entry + 0x10, &slice, sizeof(slice));   // bytes of one depth slice
            std::memcpy(entry + 0x1c, &pitch32, sizeof(pitch32));  // row pitch in bytes (fits 32 bits)
        }
    }
    return S_OK;
}

// Copies one subresource from a linear source into a linear destination at the model's offset. Only called when the
// layout is linear and the source pitch equals the layout's row pitch: then the copy is the whole operation (no
// tiling or format change). Everything else is refused without writing, so a tiled upload cannot corrupt the
// caller's buffer through a layout this model is not known to match byte for byte.
HRESULT copy_linear_subresource(Computer* self, void* dst, std::uint32_t index, const void* src,
                                std::uint32_t src_pitch, std::uint32_t src_size) {
    if (!self->known) return kNotImpl;
    if (!dst || !src) return E_INVALIDARG;
    if (self->layout.mode != gfx::SwizzleMode::linear) return kNotImpl;
    gfx::ConsoleSubresource sub;
    if (!self->layout.subresource(index, sub)) return E_INVALIDARG;
    const gfx::ConsolePlaneLayout& plane = self->layout.planes[sub.plane];
    const gfx::ConsoleMipLayout& mip = plane.mips[sub.mip];
    const std::uint64_t pitch = row_bytes(plane, mip);
    if (pitch == 0 || pitch > 0xffffffffu) return kNotImpl;
    if (src_pitch != pitch) return kNotImpl;  // a padded or block-compressed source: not a plain copy
    const std::uint64_t rows = std::min<std::uint64_t>(mip.padded_height, src_size / src_pitch);
    const std::uint64_t bytes = rows * pitch;
    if (sub.offset > self->layout.allocation_size || bytes > self->layout.allocation_size - sub.offset)
        return kNotImpl;
    std::memcpy(static_cast<std::uint8_t*>(dst) + sub.offset, src, static_cast<std::size_t>(bytes));
    return S_OK;
}

ULONG __stdcall computer_add_ref(Computer* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG __stdcall computer_release(Computer* self) {
    const std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete self;
    return left;
}

HRESULT __stdcall computer_slot2(Computer* self, void* out) { return computer_write_layout(self, out); }

HRESULT __stdcall computer_slot8(Computer* self, void* dst, std::uint32_t /*zero*/, std::uint32_t index,
                                 const void* src, std::uint32_t src_pitch, std::uint32_t src_size) {
    return copy_linear_subresource(self, dst, index, src, src_pitch, src_size);
}

HRESULT __stdcall computer_slot9(Computer* self, void* dst, std::uint32_t /*zero*/, std::uint32_t index,
                                 std::uint32_t /*zero2*/, const void* src, std::uint32_t src_pitch,
                                 std::uint32_t src_size) {
    return copy_linear_subresource(self, dst, index, src, src_pitch, src_size);
}

HRESULT __stdcall computer_slot10(Computer* /*self*/, void* /*count*/, void* /*counts*/, void* /*counts2*/,
                                  void* /*value*/, std::uint32_t /*zero*/, void* /*records*/) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "texture computer slot ", 10, _ReturnAddress());
    return kNotImpl;
}

template <size_t Slot>
HRESULT __stdcall computer_slot(Computer*) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "texture computer slot ", static_cast<int>(Slot), _ReturnAddress());
    return kNotImpl;
}

template <size_t... Slot>
constexpr auto make_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0   ? reinterpret_cast<void*>(&computer_add_ref)
         : Slot == 1 ? reinterpret_cast<void*>(&computer_release)
         : Slot == 2 ? reinterpret_cast<void*>(&computer_slot2)
         : Slot == 8 ? reinterpret_cast<void*>(&computer_slot8)
         : Slot == 9 ? reinterpret_cast<void*>(&computer_slot9)
         : Slot == 10 ? reinterpret_cast<void*>(&computer_slot10)
                      : reinterpret_cast<void*>(&computer_slot<Slot>))...};
}

const auto kVtable = make_vtable(std::make_index_sequence<kSlots>{});

HRESULT __stdcall XGCreateTextureComputer(const void* raw_desc, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!raw_desc) return E_INVALIDARG;
    auto* computer = new (std::nothrow) Computer;
    if (!computer) return E_OUTOFMEMORY;
    computer->vtable = kVtable.data();
    read_desc(raw_desc, computer->desc);
    // The model may refuse a description it has no size for. The object is still handed out (two measured callers
    // use *out without checking the HRESULT); its slots then refuse without writing.
    computer->known =
        gfx::compute_console_texture_layout(computer->desc, computer->layout) == gfx::ConsoleLayoutStatus::ok;
    *out = computer;
    return computer->known ? S_OK : kNotImpl;
}

HRESULT __stdcall XGSuggestSwizzleMode(const void* raw_desc, uint32_t /*flags*/, uint32_t* mode) {
    if (!mode) return E_POINTER;
    if (!raw_desc) return E_INVALIDARG;
    gfx::ConsoleResourceDesc desc;
    read_desc(raw_desc, desc);
    gfx::ConsoleFormat format;
    if (!gfx::console_format(desc.format, format)) return kNotImpl;
    if (desc.dimension < gfx::kDimensionTexture1D || desc.dimension > gfx::kDimensionTexture3D) return kNotImpl;
    // The measured callers write 0x100 | n themselves for a known mode and never touch the field after this call:
    // the library must return the complete console layout word, not a raw ADDR_SW_* number.
    *mode = gfx::kLayoutSwizzleFlag | static_cast<std::uint32_t>(gfx::suggest_console_swizzle_mode(desc, format));
    return S_OK;
}

}  // namespace

SHIM_TABLE(xg_xs,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
