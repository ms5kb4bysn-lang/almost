// Checks the registered xg_xs.dll shim against the call shapes measured in the game executable (runtime/shims/xg_xs.cpp).
//
// Both exports are resolved through rt::shims::find, so this is the table the runtime installs. The checks:
// - XGSuggestSwizzleMode writes the complete console layout word (0x100 | ADDR_SW_*), the encoding the game's own
//   fill helpers write for a mode they already know, and leaves the caller's word alone for a description the model
//   cannot lay out.
// - XGCreateTextureComputer returns S_OK for a description the model knows, a per-create reference-counted object,
//   and E_NOTIMPL plus a usable object for one it does not (two measured callers ignore the HRESULT).
// - Slot 2 fills the measured layout record: the plane slice stride, and per mip its size, offset, slice size and row
//   pitch. The 512x512 BC7 10-mip numbers (0x56000 total, 4 KiB alignment) are the measured console ones.
// - Slots 8 and 9 copy a subresource for a linear layout with a matching source pitch and refuse anything else
//   without writing; slot 10 is unmeasured and refuses without writing.
// - Release destroys the object at zero refs and other computers stay usable.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"
#include "../src/console_texture_layout.h"

namespace {

using CreateFn = HRESULT(__stdcall*)(const void*, void**);
using SuggestFn = HRESULT(__stdcall*)(const void*, uint32_t, uint32_t*);
using AddRefFn = ULONG(__stdcall*)(void*);
using ReleaseFn = ULONG(__stdcall*)(void*);
using Slot2Fn = HRESULT(__stdcall*)(void*, void*);
using Slot8Fn = HRESULT(__stdcall*)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
using Slot9Fn = HRESULT(__stdcall*)(void*, void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
using Slot10Fn = HRESULT(__stdcall*)(void*, void*, void*, void*, void*, uint32_t, void*);
constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);

// D3D12_RESOURCE_DESC with its x64 tail padding, the description both exports take.
struct Desc {
    uint32_t dimension;
    uint32_t padding0;
    uint64_t alignment;
    uint64_t width;
    uint32_t height;
    uint16_t depth_or_array_size;
    uint16_t mip_levels;
    uint32_t format;
    uint32_t sample_count;
    uint32_t sample_quality;
    uint32_t layout;
    uint32_t flags;
    uint32_t padding1;
};
static_assert(sizeof(Desc) == 0x38, "the console description is a D3D12_RESOURCE_DESC");

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

Desc texture2d(uint64_t width, uint32_t height, uint16_t mips, uint32_t format) {
    Desc desc{};
    desc.dimension = 3;  // D3D12_RESOURCE_DIMENSION_TEXTURE2D
    desc.width = width;
    desc.height = height;
    desc.depth_or_array_size = 1;
    desc.mip_levels = mips;
    desc.format = format;
    desc.sample_count = 1;
    return desc;
}

template <typename T>
T field(const uint8_t* record, size_t offset) {
    T value{};
    std::memcpy(&value, record + offset, sizeof(value));
    return value;
}

constexpr uint32_t kDXGI_BC7_UNORM = 98;
constexpr uint32_t kDXGI_R8G8B8A8_UNORM = 28;
constexpr uint32_t kDXGI_R32_FLOAT = 39;
constexpr uint32_t kLayoutSwizzle = 0x100;
constexpr uint32_t kSwizzle4KbS = 5, kSwizzle64KbZx = 24, kSwizzle64KbRx = 27;

}  // namespace

int main() {
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<SuggestFn>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create != nullptr, "XGCreateTextureComputer is registered");
    check(suggest != nullptr, "XGSuggestSwizzleMode is registered");
    check(rt::shims::owns_dll("xg_xs.dll"), "the registry owns xg_xs.dll");
    if (!create || !suggest) return 1;

    // ---- the suggested mode encoding ----
    Desc bc7 = texture2d(512, 512, 10, kDXGI_BC7_UNORM);
    bc7.layout = 0;  // the game's fill helper zeroes the field before it asks
    uint32_t mode = 0x1234;
    check(suggest(&bc7, 0, &mode) == S_OK, "XGSuggestSwizzleMode lays out 512x512 BC7");
    check(mode == (kLayoutSwizzle | kSwizzle4KbS),
          "XGSuggestSwizzleMode writes 0x105: the full word, SW_4KB_S (the game's own encoding)");

    Desc depth = texture2d(1024, 1024, 1, kDXGI_R32_FLOAT);
    depth.flags = 2;  // D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
    mode = 0;
    check(suggest(&depth, 0, &mode) == S_OK && mode == (kLayoutSwizzle | kSwizzle64KbZx),
          "a depth texture gets 0x118 (SW_64KB_Z_X)");

    Desc target = texture2d(512, 512, 1, kDXGI_R8G8B8A8_UNORM);
    target.flags = 1;  // D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    mode = 0;
    check(suggest(&target, 0, &mode) == S_OK && mode == (kLayoutSwizzle | kSwizzle64KbRx),
          "a render target gets 0x11b (SW_64KB_R_X)");

    Desc linear_1d = texture2d(256, 1, 1, kDXGI_R8G8B8A8_UNORM);
    linear_1d.dimension = 2;  // D3D12_RESOURCE_DIMENSION_TEXTURE1D
    mode = 0;
    check(suggest(&linear_1d, 0, &mode) == S_OK && mode == kLayoutSwizzle,
          "a 1D texture gets 0x100 (linear)");

    Desc unknown = texture2d(64, 64, 1, 0);  // DXGI_FORMAT_UNKNOWN
    mode = 0x1234;
    check(suggest(&unknown, 0, &mode) == kNotImpl, "an unknown format returns E_NOTIMPL");
    check(mode == 0x1234, "an unknown format leaves the caller's layout word alone");
    check(suggest(&bc7, 0, nullptr) == E_POINTER, "XGSuggestSwizzleMode(desc, 0, nullptr) is E_POINTER");
    check(suggest(nullptr, 0, &mode) == E_INVALIDARG, "XGSuggestSwizzleMode(nullptr, 0, &mode) is E_INVALIDARG");

    // ---- create and slot 2: layout for a known description ----
    check(create(&bc7, nullptr) == E_POINTER, "XGCreateTextureComputer(desc, nullptr) is E_POINTER");
    void* computer = nullptr;
    check(create(&bc7, &computer) == S_OK && computer != nullptr,
          "XGCreateTextureComputer returns S_OK and an object for 512x512 BC7");
    if (!computer) return 1;

    // The model itself: the measured console numbers the shim's record is built from.
    leonida::gfx10::ConsoleResourceDesc model_desc{};
    model_desc.dimension = 3;
    model_desc.width = 512;
    model_desc.height = 512;
    model_desc.depth_or_array_size = 1;
    model_desc.mip_levels = 10;
    model_desc.format = kDXGI_BC7_UNORM;
    model_desc.sample_count = 1;
    model_desc.layout = kLayoutSwizzle | kSwizzle4KbS;
    leonida::gfx10::ConsoleTextureLayout model;
    const bool modelled = leonida::gfx10::compute_console_texture_layout(model_desc, model) ==
                          leonida::gfx10::ConsoleLayoutStatus::ok;
    check(modelled && model.allocation_size == 0x56000 && model.alignment == 0x1000,
          "the console layout of 512x512 BC7 10 mips is 0x56000 bytes, 0x1000 aligned");

    void* const* vtable = *static_cast<void* const* const*>(computer);
    uint8_t out[0x18 + 2 * 0x6c8];
    std::memset(out, 0xcd, sizeof(out));
    check(reinterpret_cast<Slot2Fn>(vtable[2])(computer, out) == S_OK, "slot 2 fills the layout record");
    check(all_bytes(out, 0x18, 0), "slot 2 zeroes the 0x18-byte header");
    const uint8_t* record = out + 0x18;
    check(field<uint64_t>(record, 0x10) == 0x56000, "the plane slice stride is the whole 0x56000-byte chain");
    check(field<uint64_t>(record, 0x38 + 0x00) == 0x40000, "mip 0 is 0x40000 bytes (512x512 BC7)");
    check(field<uint64_t>(record, 0x38 + 0x08) == 0x16000, "mip 0 starts at 0x16000 (the mip tail is first)");
    check(field<uint64_t>(record, 0x38 + 0x10) == 0x40000, "mip 0's single depth slice is 0x40000 bytes");
    check(field<uint32_t>(record, 0x38 + 0x1c) == 0x800, "mip 0's row pitch is 0x800 bytes");
    check(field<uint64_t>(record, 0x38 + 0x70 + 0x00) == 0x10000 && field<uint64_t>(record, 0x38 + 0x70 + 0x08) == 0x6000,
          "mip 1 is 0x10000 bytes at 0x6000");
    check(field<uint64_t>(record, 0x38 + 2 * 0x70 + 0x08) == 0x2000 &&
              field<uint64_t>(record, 0x38 + 3 * 0x70 + 0x08) == 0x1000,
          "mips 2 and 3 descend towards the mip tail");
    bool inside = true, entries_inside = true;
    for (uint32_t i = 0; i < 10; ++i) {
        leonida::gfx10::ConsoleSubresource sub;
        if (!model.subresource(i, sub) || sub.offset >= model.allocation_size) inside = false;
        const uint8_t* e = record + 0x38 + i * 0x70;
        if (field<uint64_t>(e, 0x08) >= model.allocation_size) entries_inside = false;
        if (field<uint64_t>(e, 0x00) > model.allocation_size) entries_inside = false;
    }
    check(inside, "every subresource offset from the model is inside the 0x56000-byte allocation");
    check(entries_inside, "every slot-2 mip offset and size is inside the allocation");
    check(field<uint64_t>(record, 0x38 + 9 * 0x70 + 0x08) < 0x1000 + 1,
          "the last mip sits in the 4 KiB mip tail at the start of the texture");

    // A depth/stencil description has two planes: slot 2 writes one record for each (the second plane starts at
    // out+0x18+0x6c8; the caller at exe+0x5121980 indexes the records by plane).
    Desc d24s8 = texture2d(256, 256, 1, 45);  // DXGI_FORMAT_D24_UNORM_S8_UINT
    d24s8.layout = kLayoutSwizzle | kSwizzle64KbZx;
    leonida::gfx10::ConsoleResourceDesc ds_desc{};
    ds_desc.dimension = 3;
    ds_desc.width = 256;
    ds_desc.height = 256;
    ds_desc.depth_or_array_size = 1;
    ds_desc.mip_levels = 1;
    ds_desc.format = 45;
    ds_desc.sample_count = 1;
    ds_desc.layout = kLayoutSwizzle | kSwizzle64KbZx;
    leonida::gfx10::ConsoleTextureLayout ds_model;
    const bool ds_modelled = leonida::gfx10::compute_console_texture_layout(ds_desc, ds_model) ==
                             leonida::gfx10::ConsoleLayoutStatus::ok;
    check(ds_modelled && ds_model.plane_count == 2, "D24S8 lays out as two planes");
    void* ds_computer = nullptr;
    check(create(&d24s8, &ds_computer) == S_OK, "a D24S8 description gets a computer");
    if (ds_computer && ds_modelled) {
        void* const* dv = *static_cast<void* const* const*>(ds_computer);
        uint8_t ds_out[0x18 + 2 * 0x6c8];
        std::memset(ds_out, 0xcd, sizeof(ds_out));
        check(reinterpret_cast<Slot2Fn>(dv[2])(ds_computer, ds_out) == S_OK, "slot 2 fills both plane records");
        check(field<uint64_t>(ds_out + 0x18, 0x10) == ds_model.planes[0].slice_stride,
              "plane 0's slice stride is the model's");
        check(field<uint64_t>(ds_out + 0x18 + 0x6c8, 0x10) == ds_model.planes[1].slice_stride,
              "plane 1's slice stride is the model's (the second record)");
        check(field<uint64_t>(ds_out + 0x18 + 0x6c8, 0x38 + 0x08) == ds_model.planes[1].mips[0].offset,
              "plane 1's mip offset is the stencil plane's");
        check(reinterpret_cast<ReleaseFn>(dv[1])(ds_computer) == 0, "the D24S8 computer releases cleanly");
    } else if (ds_computer) {
        reinterpret_cast<ReleaseFn>(*static_cast<void* const* const*>(ds_computer)[1])(ds_computer);
    }

    // A second computer is a separate object, and releasing one leaves the other usable.
    void* computer2 = nullptr;
    check(create(&bc7, &computer2) == S_OK && computer2 != nullptr && computer2 != computer,
          "a second XGCreateTextureComputer is a different object (no static singleton)");
    auto add_ref = reinterpret_cast<AddRefFn>(vtable[0]);
    auto release = reinterpret_cast<ReleaseFn>(vtable[1]);
    check(add_ref(computer) == 2, "AddRef raises the count to 2");
    check(release(computer) == 1, "Release lowers it to 1");
    check(reinterpret_cast<Slot2Fn>(vtable[2])(computer, out) == S_OK, "the object still works at one reference");
    check(release(computer) == 0, "Release destroys the object at zero");
    uint8_t out2[0x18 + 2 * 0x6c8];
    check(reinterpret_cast<Slot2Fn>(vtable[2])(computer2, out2) == S_OK,
          "the other computer is unaffected by the first one's destruction");
    check(release(computer2) == 0, "the second object is destroyed at zero too");

    // An unknown description still hands out an object (two measured callers ignore the HRESULT), refusing work.
    void* unknown_computer = nullptr;
    check(create(&unknown, &unknown_computer) == kNotImpl && unknown_computer != nullptr,
          "an unknown description returns E_NOTIMPL but still stores a computer");
    if (unknown_computer) {
        uint8_t unknown_out[0x18 + 0x6c8];
        std::memset(unknown_out, 0xcd, sizeof(unknown_out));
        check(reinterpret_cast<Slot2Fn>(vtable[2])(unknown_computer, unknown_out) == kNotImpl,
              "the unknown computer's slot 2 returns E_NOTIMPL");
        check(all_bytes(unknown_out, 0x18, 0) && all_bytes(unknown_out + 0x18, 0x6c8, 0),
              "the unknown computer's slot 2 zeroes the header and plane record");
        check(release(unknown_computer) == 0, "the unknown computer releases cleanly");
    }

    // ---- slots 8 and 9: the safe subset (linear layout, matching row pitch) ----
    Desc rgba = texture2d(64, 64, 1, kDXGI_R8G8B8A8_UNORM);
    rgba.layout = 1;  // D3D12_TEXTURE_LAYOUT_ROW_MAJOR / console linear
    void* linear = nullptr;
    check(create(&rgba, &linear) == S_OK && linear != nullptr, "a linear 64x64 RGBA8 texture gets a computer");
    if (linear) {
        void* const* lv = *static_cast<void* const* const*>(linear);
        uint8_t dst[0x4000 + 0x100];
        uint8_t src[0x4000 + 0x100];
        std::memset(dst, 0xcd, sizeof(dst));
        for (size_t i = 0; i < sizeof(src); ++i) src[i] = static_cast<uint8_t>(i * 37 + 11);
        check(reinterpret_cast<Slot8Fn>(lv[8])(linear, dst, 0, 0, src, 256, 0x4000) == S_OK,
              "slot 8 copies a linear 64x64 RGBA8 subresource with a matching pitch");
        check(std::memcmp(dst, src, 0x4000) == 0, "slot 8's copy is byte for byte the source");
        std::memset(dst, 0xcd, sizeof(dst));
        check(reinterpret_cast<Slot8Fn>(lv[8])(linear, dst, 0, 0, src, 260, 0x4000) == kNotImpl,
              "slot 8 refuses a source pitch the layout does not have");
        check(all_bytes(dst, sizeof(dst), 0xcd), "the refused slot 8 writes nothing");
        std::memset(dst, 0xcd, sizeof(dst));
        check(reinterpret_cast<Slot8Fn>(lv[8])(linear, dst, 0, 3, src, 256, 0x4000) == E_INVALIDARG,
              "slot 8 refuses a subresource past the texture");
        std::memset(dst, 0xcd, sizeof(dst));
        check(reinterpret_cast<Slot9Fn>(lv[9])(linear, dst, 0, 0, 0, src, 256, 0x4000) == S_OK &&
                  std::memcmp(dst, src, 0x4000) == 0,
              "slot 9's measured shape accepts the same linear copy");
        check(release(linear) == 0, "the linear computer releases cleanly");
    }

    // A BC7 (tiled) layout is not a plain copy: slots 8/9 refuse and write nothing.
    void* bc7_computer = nullptr;
    check(create(&bc7, &bc7_computer) == S_OK && bc7_computer != nullptr, "the BC7 computer exists for the refusal");
    if (bc7_computer) {
        void* const* bv = *static_cast<void* const* const*>(bc7_computer);
        uint8_t dst[0x56000], src[0x40000];
        std::memset(dst, 0xcd, sizeof(dst));
        std::memset(src, 0x5a, sizeof(src));
        check(reinterpret_cast<Slot8Fn>(bv[8])(bc7_computer, dst, 0, 0, src, 0x800, 0x40000) == kNotImpl,
              "slot 8 refuses a tiled layout (the byte tiling is not measured)");
        check(all_bytes(dst, sizeof(dst), 0xcd), "the refused tiled slot 8 writes nothing");
        check(release(bc7_computer) == 0, "the BC7 computer releases cleanly");
    }

    // ---- slot 10: unmeasured, refuses and writes nothing ----
    void* slot10_computer = nullptr;
    check(create(&bc7, &slot10_computer) == S_OK, "a computer for the slot-10 check");
    if (slot10_computer) {
        void* const* sv = *static_cast<void* const* const*>(slot10_computer);
        uint8_t a[0x30], b[0x30], c[0x30], d[0x30], records[0x120];
        std::memset(a, 0xcd, sizeof(a));
        std::memset(b, 0xcd, sizeof(b));
        std::memset(c, 0xcd, sizeof(c));
        std::memset(d, 0xcd, sizeof(d));
        std::memset(records, 0xcd, sizeof(records));
        check(reinterpret_cast<Slot10Fn>(sv[10])(slot10_computer, a, b, c, d, 0, records) == kNotImpl,
              "slot 10 returns E_NOTIMPL (its tile table is unmeasured)");
        check(all_bytes(a, sizeof(a), 0xcd) && all_bytes(b, sizeof(b), 0xcd) && all_bytes(c, sizeof(c), 0xcd) &&
                  all_bytes(d, sizeof(d), 0xcd) && all_bytes(records, sizeof(records), 0xcd),
              "slot 10 writes nothing");
        check(release(slot10_computer) == 0, "the slot-10 computer releases cleanly");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
