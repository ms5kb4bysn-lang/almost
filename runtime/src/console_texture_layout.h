#pragma once
// The console's (Xbox Series, AMD GFX10.3 / RDNA2) memory layout of a texture: its size and alignment, the swizzle
// mode it gets, and where each mip, array slice and plane starts. The runtime needs the console's numbers, not the
// PC driver's, wherever the game sizes memory from them: GetResourceAllocationInfo (Xbox device slot 25) feeds an
// 82 MiB render-target pool whose use the executable asserts to within 2 MiB (1.0.2.1, exe+0x247af2d), and the
// texture streamer keeps console sizes in its tables.
//
// Sources, and what is measured versus assumed:
// - The size algorithm follows AMD's address library (addrlib), as shipped in Mesa (src/amd/addrlib) and AMD PAL
//   (src/core/imported/addrlib, read at commit c5e80007): Gfx10Lib::ComputeSurfaceInfoMacroTiled,
//   ComputeSurfaceInfoMicroTiled and HwlComputeSurfaceInfoLinear, Lib::ComputeThinBlockDimension,
//   ComputeThickBlockDimension and GetMipTailDim, Gfx10Lib::GetMaxNumMipsInTail, the dsMipmapHtileFix (set on every
//   GFX10 part but Navi10) and the swizzle-mode masks of gfx10addrlib.h. The tests compare it with addrlib itself,
//   compiled outside the repository (runtime/test/console_texture_layout_test.cpp says how). addrlib's notice:
//
//     Copyright (C) 2007-2026 Advanced Micro Devices, Inc. All rights reserved.
//     SPDX-License-Identifier: MIT
//     Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//     documentation files (the "Software"), to deal in the Software without restriction, including without
//     limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//     Software, and to permit persons to whom the Software is furnished to do so, subject to the following
//     conditions: the above copyright notice and this permission notice shall be included in all copies or
//     substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//     OR IMPLIED.
//
// - Measured on the retail package: for 2D SW_4KB_S and SW_64KB_D textures (mip chain, mip tail at offset 0, mip 0
//   last) this rule gives the exact allocation span of all 356,413 single-slice TTPF descriptors with a known element
//   size, and of the 697 arrays, cubemaps and volumes (docs/textures.md "The layout is the AMD mip tail",
//   docs/textures_arrays.md; tools/tex_headers.py compact_surface_layout and friends). Thick 3D, the _X/_T modes,
//   MSAA, 256 B and linear layouts are addrlib's word only.
// - Marvisak/VIctory (GPL-3.0, crates/victory-gpu/src/resource.rs and crates/victory-formats/src/texture.rs, read
//   at commit ffe310eb) is the source of: the layout word reading (0 = the library picks, 1 = linear,
//   0x100 | n = swizzle mode n in addrlib's ADDR_SW_* numbering); the pick for word 0 (1D linear, depth SW_64KB_Z_X,
//   render target SW_64KB_R_X, otherwise SW_4KB_S); depth formats with stencil as two planes (depth, then a 1-byte
//   stencil plane aligned to its own block); the console-only DXGI numbers 116-120 and 189-191; and the allocation
//   rule (the layout's own alignment, 4 KiB for 4 KiB modes where the PC says 64 KiB, unless the description asks
//   for more, and the size rounded up to it). VIctory states that a 512x512 BC7 10-mip texture is 0x56000 bytes with
//   4 KiB alignment on the console; the test checks that number.
//
// Hypotheses (not measured here):
// - The pick for layout word 0 is VIctory's; what XGSuggestSwizzleMode returns on the console is unmeasured. Two
//   additions of ours: a multisampled colour texture without the render-target flag takes SW_64KB_R_X (addrlib
//   refuses MSAA in S/D modes), and 12-byte formats are always linear (addrlib refuses them tiled).
// - Layout word 0x101 (seen at slot 76, docs/d3d12_vulkan.md 4.2) reads as SW_256B_S under the 0x100 | n rule. If
//   it really means "tiled, the library's pick", pass kLayoutLibraryPick instead; the log at slot 25 should say.
// - D3D12's public 64KB_UNDEFINED_SWIZZLE (2) and 64KB_STANDARD_SWIZZLE (3) words are taken as SW_64KB_S.
// - No compression metadata: the console adds DCC, HTILE, CMASK or FMASK to render targets and depth buffers unless
//   told not to, and its sizes for those may be larger than these. addrlib computes them from the GPU's pipe
//   configuration (GB_ADDR_CONFIG), which is not measured for the Series X or S.
// - Array slices follow addrlib's addressing: slice-major, each slice holding its whole mip chain. The streamed TTPF
//   bytes of 64 KiB 2D arrays are mip-major instead (docs/textures_arrays.md); the total is the same, and whether
//   the streamer reorders them is open.
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace leonida::gfx10 {

// Swizzle modes by hardware number (addrlib's ADDR_SW_* order); only the modes GFX10 has.
enum class SwizzleMode : std::uint32_t {
    linear = 0,
    sw_256b_s = 1,
    sw_256b_d = 2,
    sw_4kb_s = 5,
    sw_4kb_d = 6,
    sw_64kb_s = 9,
    sw_64kb_d = 10,
    sw_64kb_s_t = 17,
    sw_64kb_d_t = 18,
    sw_4kb_s_x = 21,
    sw_4kb_d_x = 22,
    sw_64kb_z_x = 24,
    sw_64kb_s_x = 25,
    sw_64kb_d_x = 26,
    sw_64kb_r_x = 27,
};

// D3D12_RESOURCE_DESC, field for field (the Xbox description starts with the same 56 bytes).
struct ConsoleResourceDesc {
    std::uint32_t dimension = 0;  // D3D12_RESOURCE_DIMENSION: 1 buffer, 2 1D, 3 2D, 4 3D
    std::uint64_t alignment = 0;
    std::uint64_t width = 0;
    std::uint32_t height = 1;
    std::uint16_t depth_or_array_size = 1;
    std::uint16_t mip_levels = 1;  // 0: the full chain
    std::uint32_t format = 0;      // DXGI_FORMAT number
    std::uint32_t sample_count = 1;
    std::uint32_t sample_quality = 0;
    std::uint32_t layout = 0;  // the Xbox layout word, see kLayout*
    std::uint32_t flags = 0;   // D3D12_RESOURCE_FLAGS
};

inline constexpr std::uint32_t kDimensionBuffer = 1, kDimensionTexture1D = 2, kDimensionTexture2D = 3,
                               kDimensionTexture3D = 4;
inline constexpr std::uint32_t kFlagRenderTarget = 0x1, kFlagDepthStencil = 0x2, kFlagUnorderedAccess = 0x4;
inline constexpr std::uint32_t kLayoutLibraryPick = 0, kLayoutLinear = 1, kLayout64KbUndefined = 2,
                               kLayout64KbStandard = 3, kLayoutSwizzleFlag = 0x100;

enum class ConsoleLayoutStatus {
    ok,
    not_texture,     // a buffer or an unknown dimension: the caller sizes it
    unknown_format,  // no element size for this DXGI number (1-bit, video, packed 4:2:2)
    invalid,         // the description makes no texture, or the mode is not allowed for it
};

struct ConsoleFormat {
    std::uint32_t element_bytes = 0;  // per texel, or per 4x4 block
    std::uint32_t block = 1;          // texels per element side: 1, or 4 for block compression
    bool stencil = false;             // a second, 1-byte stencil plane
};

// Element size by DXGI format number; false for formats without one here.
inline bool console_format(std::uint32_t dxgi, ConsoleFormat& out) {
    std::uint32_t bytes = 0, block = 1;
    bool stencil = false;
    if (dxgi >= 1 && dxgi <= 4) bytes = 16;                                   // R32G32B32A32
    else if (dxgi >= 5 && dxgi <= 8) bytes = 12;                              // R32G32B32 (linear only)
    else if (dxgi >= 9 && dxgi <= 18) bytes = 8;                              // R16G16B16A16, R32G32
    else if (dxgi >= 19 && dxgi <= 22) bytes = 4, stencil = true;            // R32G8X24 family
    else if (dxgi >= 23 && dxgi <= 43) bytes = 4;                             // 10:10:10:2 ... R32
    else if (dxgi >= 44 && dxgi <= 47) bytes = 4, stencil = true;            // R24G8 family
    else if (dxgi >= 48 && dxgi <= 59) bytes = 2;                             // R8G8, R16
    else if (dxgi >= 60 && dxgi <= 65) bytes = 1;                             // R8, A8
    else if (dxgi == 67) bytes = 4;                                           // R9G9B9E5
    else if ((dxgi >= 70 && dxgi <= 72) || (dxgi >= 79 && dxgi <= 81)) bytes = 8, block = 4;   // BC1, BC4
    else if ((dxgi >= 73 && dxgi <= 78) || (dxgi >= 82 && dxgi <= 84) || (dxgi >= 94 && dxgi <= 99))
        bytes = 16, block = 4;                                                // BC2, BC3, BC5, BC6H, BC7
    else if (dxgi == 85 || dxgi == 86 || dxgi == 115) bytes = 2;              // B5G6R5, B5G5R5A1, B4G4R4A4
    else if (dxgi >= 87 && dxgi <= 93) bytes = 4;                             // B8G8R8A8/X8, R10G10B10_XR
    // Console-only numbers, per VIctory: 10-bit float colour (116, 117), 16-bit depth with stencil (118-120),
    // signed 10-bit colour (189), two 4-bit channels (190), 24/8 colour (191).
    else if (dxgi == 116 || dxgi == 117 || dxgi == 189 || dxgi == 191) bytes = 4;
    else if (dxgi >= 118 && dxgi <= 120) bytes = 2, stencil = true;
    else if (dxgi == 190) bytes = 1;
    else return false;
    out = {bytes, block, stencil};
    return true;
}

struct ConsoleMipLayout {
    std::uint32_t width = 0, height = 0, depth = 0;  // in elements (texels or blocks); depth 1 unless 3D
    std::uint32_t pitch = 0, padded_height = 0;      // padded, in elements (in the tail: the tail's share)
    std::uint64_t offset = 0;                        // first byte in slice 0 (thick 3D: slab 0), from the plane
    bool in_tail = false;
    std::uint32_t tail_offset = 0;     // byte offset inside the tail block
    std::uint32_t tail_x = 0, tail_y = 0;  // element position the mip starts at inside the tail block
};

struct ConsolePlaneLayout {
    std::uint64_t offset = 0;  // from the start of the texture
    std::uint64_t alignment = 0;  // the swizzle block's bytes (256 for linear)
    std::uint32_t element_bytes = 0;
    std::uint32_t block_width = 0, block_height = 0, block_depth = 1;  // swizzle block, in elements
    bool thick = false;              // 3D blocks spanning block_depth depth slices (3D S and D modes)
    std::uint32_t slices = 0;        // array slices, depth slices (thin 3D) or depth slabs (thick 3D)
    std::uint64_t slice_stride = 0;  // bytes from one slice (or slab) to the next: its whole mip chain
    std::uint64_t size = 0;
    std::uint32_t first_tail_mip = 0;  // mips.size() without a tail
    std::vector<ConsoleMipLayout> mips;
};

struct ConsoleSubresource {
    std::uint64_t offset = 0;  // from the start of the texture
    std::uint32_t plane = 0, mip = 0, slice = 0;
    std::uint32_t width = 0, height = 0, depth = 1;  // in elements
    std::uint64_t depth_stride = 0;  // 3D: bytes between slices (thin) or slabs of block_depth slices (thick)
};

struct ConsoleTextureLayout {
    SwizzleMode mode = SwizzleMode::linear;
    std::uint32_t mip_levels = 0;
    std::uint32_t array_size = 0;  // 1 for 3D
    std::uint32_t plane_count = 0;
    std::uint64_t size = 0;             // bytes the planes span
    std::uint64_t alignment = 0;        // the largest plane block, or more if the description asks
    std::uint64_t allocation_size = 0;  // size rounded up to alignment: what GetResourceAllocationInfo reports
    std::array<ConsolePlaneLayout, 2> planes;

    std::uint32_t subresource_count() const { return mip_levels * array_size * plane_count; }

    // D3D12 numbering: mip + slice * mip_levels + plane * mip_levels * array_size.
    bool subresource(std::uint32_t index, ConsoleSubresource& out) const {
        if (index >= subresource_count()) return false;
        const std::uint32_t mip = index % mip_levels, slice = index / mip_levels % array_size,
                            plane = index / (mip_levels * array_size);
        const ConsolePlaneLayout& p = planes[plane];
        const ConsoleMipLayout& m = p.mips[mip];
        out.plane = plane;
        out.mip = mip;
        out.slice = slice;
        out.width = m.width;
        out.height = m.height;
        out.depth = m.depth;
        out.offset = p.offset + std::uint64_t{slice} * p.slice_stride + m.offset;
        out.depth_stride = m.depth > 1 ? p.slice_stride : 0;
        return true;
    }
};

namespace layout_detail {

inline constexpr std::uint32_t kMaxExtent = 16384;  // GFX10 image descriptors: 14-bit extents
inline constexpr std::uint32_t kMaxMips = 16;
inline constexpr std::uint32_t kBlock256[5][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
inline constexpr std::uint32_t kBlock256Thick[5][3] = {{8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}};
inline constexpr std::uint32_t kBlock1KThick[5][3] = {{16, 8, 8}, {8, 8, 8}, {8, 8, 4}, {8, 4, 4}, {4, 4, 4}};

constexpr std::uint32_t bit(SwizzleMode mode) {
    const std::uint32_t n = static_cast<std::uint32_t>(mode);
    return n < 32 ? 1u << n : 0u;  // a layout word can name any byte; a shift by 32 is undefined
}
constexpr std::uint32_t mask(std::initializer_list<SwizzleMode> modes) {
    std::uint32_t value = 0;
    for (SwizzleMode mode : modes) value |= bit(mode);
    return value;
}

using M = SwizzleMode;
inline constexpr std::uint32_t kKnown = mask({M::linear, M::sw_256b_s, M::sw_256b_d, M::sw_4kb_s, M::sw_4kb_d,
                                              M::sw_64kb_s, M::sw_64kb_d, M::sw_64kb_s_t, M::sw_64kb_d_t,
                                              M::sw_4kb_s_x, M::sw_4kb_d_x, M::sw_64kb_z_x, M::sw_64kb_s_x,
                                              M::sw_64kb_d_x, M::sw_64kb_r_x});
inline constexpr std::uint32_t k256B = mask({M::sw_256b_s, M::sw_256b_d});
inline constexpr std::uint32_t k4KB = mask({M::sw_4kb_s, M::sw_4kb_d, M::sw_4kb_s_x, M::sw_4kb_d_x});
inline constexpr std::uint32_t kStandard = mask({M::sw_256b_s, M::sw_4kb_s, M::sw_64kb_s, M::sw_64kb_s_t,
                                                 M::sw_4kb_s_x, M::sw_64kb_s_x});
inline constexpr std::uint32_t kDisplay = mask({M::sw_256b_d, M::sw_4kb_d, M::sw_64kb_d, M::sw_64kb_d_t,
                                                M::sw_4kb_d_x, M::sw_64kb_d_x});
// gfx10addrlib.h Gfx10Rsrc1dSwModeMask, Gfx10Rsrc3dSwModeMask (2D takes every mode).
inline constexpr std::uint32_t k1D = mask({M::linear, M::sw_64kb_r_x, M::sw_64kb_z_x});
inline constexpr std::uint32_t k3D = mask({M::linear, M::sw_4kb_s, M::sw_64kb_s, M::sw_64kb_s_t, M::sw_4kb_s_x,
                                           M::sw_64kb_z_x, M::sw_64kb_s_x, M::sw_64kb_d_x, M::sw_64kb_r_x});

constexpr std::uint32_t log2(std::uint32_t value) {
    std::uint32_t result = 0;
    while (value > 1) value >>= 1, ++result;
    return result;
}
constexpr std::uint32_t align(std::uint32_t value, std::uint32_t to) { return (value + to - 1) / to * to; }
constexpr std::uint32_t shift_ceil(std::uint32_t value, std::uint32_t shift) {
    return (value + (1u << shift) - 1) >> shift;
}

struct PlaneInput {
    std::uint32_t dimension;  // kDimensionTexture*
    SwizzleMode mode;
    std::uint32_t element_bytes;   // 1..16, power of two (12-byte formats come in as 4 with width * 3)
    std::uint32_t width, height;   // in elements
    std::uint32_t texel_width, texel_height, block, expand;  // for the data sizes of the mips
    std::uint32_t slices;          // depth (3D) or array size
    std::uint32_t mips, samples;
};

// Gfx10Lib::ValidateNonSwModeParams and ValidateSwModeParams, for the cases a D3D12 description can reach.
inline bool mode_allowed(const PlaneInput& in, bool depth, bool block_compressed) {
    const std::uint32_t m = bit(in.mode);
    if (!(m & kKnown)) return false;
    const bool msaa = in.samples > 1;
    if (in.dimension == kDimensionTexture1D && (!(m & k1D) || msaa)) return false;
    if (in.dimension == kDimensionTexture3D && (!(m & k3D) || msaa)) return false;
    if (msaa && in.mips > 1) return false;
    if (in.mode == M::linear) return !depth && !msaa;
    if (in.mode == M::sw_64kb_z_x) return in.element_bytes <= 8 && !block_compressed && (!msaa || (depth && in.element_bytes <= 4));
    if (m & (kStandard | kDisplay)) return !depth && !msaa && (!(m & k256B) || in.dimension != kDimensionTexture3D);
    return !depth;  // SW_64KB_R_X
}

inline void compute_plane(const PlaneInput& in, ConsolePlaneLayout& out) {
    const std::uint32_t bpe = in.element_bytes, elog = log2(bpe);
    const bool three_d = in.dimension == kDimensionTexture3D;
    const std::uint32_t m = bit(in.mode);
    out = {};
    out.element_bytes = bpe;
    out.mips.resize(in.mips);
    // Data sizes: texels halve rounding down; placement (below) halves elements rounding up, as addrlib does.
    for (std::uint32_t i = 0; i < in.mips; ++i) {
        out.mips[i].width = (std::max(in.texel_width >> i, 1u) + in.block - 1) / in.block * in.expand;
        out.mips[i].height = (std::max(in.texel_height >> i, 1u) + in.block - 1) / in.block;
        out.mips[i].depth = three_d ? std::max(in.slices >> i, 1u) : 1;
    }
    out.first_tail_mip = in.mips;

    if (in.mode == M::linear) {  // HwlComputeSurfaceInfoLinear
        const std::uint32_t pitch_align = 256 / bpe;
        std::uint64_t chain = 0;
        for (std::uint32_t i = in.mips; i-- > 0;) {
            ConsoleMipLayout& mip = out.mips[i];
            mip.pitch = align(shift_ceil(in.width, i), pitch_align);
            mip.padded_height = shift_ceil(in.height, i);
            mip.offset = chain;
            chain += std::uint64_t{mip.pitch} * mip.padded_height * bpe;
        }
        out.alignment = 256;
        out.block_width = pitch_align;
        out.block_height = 1;
        out.slices = in.slices;
        out.slice_stride = chain;
        out.size = chain * in.slices;
        return;
    }

    const std::uint32_t block_log2 = (m & k256B) ? 8 : (m & k4KB) ? 12 : 16;
    const std::uint64_t block_bytes = std::uint64_t{1} << block_log2;
    const bool thick = three_d && (m & (kStandard | kDisplay));
    std::uint32_t bw, bh, bd = 1;
    if (thick) {  // ComputeThickBlockDimension
        const std::uint32_t amp = (block_log2 - 10) / 3, rest = (block_log2 - 10) % 3;
        bw = kBlock1KThick[elog][0] << amp;
        bh = kBlock1KThick[elog][1] << (amp + rest / 2);
        bd = kBlock1KThick[elog][2] << (amp + (rest != 0 ? 1 : 0));
    } else {  // ComputeThinBlockDimension
        const std::uint32_t samples_log2 = log2(in.samples);
        const std::uint32_t elements_log2 = block_log2 - elog - samples_log2;
        const bool wide = (samples_log2 & 1) == 0 || (block_log2 & 1) != 0;
        const std::uint32_t w = (elements_log2 + (wide ? 1 : 0)) / 2;
        bw = 1u << w;
        bh = 1u << (elements_log2 - w);
    }
    out.alignment = block_bytes;
    out.block_width = bw;
    out.block_height = bh;
    out.block_depth = bd;
    out.thick = thick;
    const std::uint32_t slices = align(in.slices, bd);

    if (block_log2 == 8) {  // ComputeSurfaceInfoMicroTiled: no mip tail
        std::uint64_t chain = 0;
        for (std::uint32_t i = in.mips; i-- > 0;) {
            ConsoleMipLayout& mip = out.mips[i];
            mip.pitch = align(shift_ceil(in.width, i), bw);
            mip.padded_height = align(shift_ceil(in.height, i), bh);
            mip.offset = chain;
            chain += std::uint64_t{mip.pitch} * mip.padded_height * bpe;
        }
        out.slices = in.slices;
        out.slice_stride = chain;
        out.size = chain * in.slices;
        return;
    }

    // ComputeSurfaceInfoMacroTiled
    out.slices = slices / bd;
    if (in.mips == 1) {
        ConsoleMipLayout& mip = out.mips[0];
        mip.pitch = align(in.width, bw);
        mip.padded_height = align(in.height, bh);
        const std::uint64_t slice = std::uint64_t{mip.pitch} * mip.padded_height * bpe * in.samples;
        out.slice_stride = slice * bd;
        out.size = slice * slices;
        return;
    }

    std::uint32_t tail_w = bw, tail_h = bh;  // GetMipTailDim (only w and h decide; depth never enters the test)
    if (thick) {
        if (block_log2 % 3 == 0) tail_h >>= 1;
        else if (block_log2 % 3 == 1) tail_w >>= 1;
    } else {
        tail_w >>= 1;
    }
    const std::uint32_t effective = thick ? block_log2 - (block_log2 - 8) / 3 : block_log2;  // GetMaxNumMipsInTail
    const std::uint32_t max_in_tail = effective <= 11 ? 1 + (1u << (effective - 9)) : effective - 4;
    std::uint32_t fixed_w = tail_w, fixed_h = tail_h;
    if (in.mode == M::sw_64kb_z_x && elog <= 1) {  // dsMipmapHtileFix: 8/16-bit depth enter the tail as 32-bit
        fixed_w /= kBlock256[elog][0] / kBlock256[2][0];
        fixed_h /= kBlock256[elog][1] / kBlock256[2][1];
    }
    std::uint64_t chain = 0;
    std::uint64_t slab_sizes[kMaxMips]{};
    for (std::uint32_t i = 0; i < in.mips; ++i) {
        const std::uint32_t w = shift_ceil(in.width, i), h = shift_ceil(in.height, i);
        if (w <= fixed_w && h <= fixed_h && in.mips - i <= max_in_tail) {
            out.first_tail_mip = i;
            chain += block_bytes / bd;
            break;
        }
        ConsoleMipLayout& mip = out.mips[i];
        mip.pitch = align(w, bw);
        mip.padded_height = align(h, bh);
        const std::uint64_t slice = std::uint64_t{mip.pitch} * mip.padded_height * bpe;
        slab_sizes[i] = slice * bd;
        chain += slice;
    }
    out.slice_stride = chain * bd;
    out.size = chain * slices;

    std::uint64_t offset = out.first_tail_mip < in.mips ? block_bytes : 0;
    for (std::uint32_t i = out.first_tail_mip; i-- > 0;) {
        out.mips[i].offset = offset;
        offset += slab_sizes[i];
    }
    const std::uint32_t micro_w = thick ? kBlock256Thick[elog][0] : kBlock256[elog][0];
    const std::uint32_t micro_h = thick ? kBlock256Thick[elog][1] : kBlock256[elog][1];
    std::uint32_t pitch = tail_w, rows = tail_h;
    for (std::uint32_t i = out.first_tail_mip; i < in.mips; ++i) {
        const std::uint32_t k = max_in_tail - 1 - (i - out.first_tail_mip);
        const std::uint32_t tail_offset = k > 6 ? 16u << k : k << 8;
        // Address bits 8.. of the offset: odd ones x, even ones y.
        std::uint32_t x = 0, y = 0;
        for (std::uint32_t b = 0; b < 6; ++b) {
            x |= ((tail_offset >> (9 + 2 * b)) & 1) << b;
            y |= ((tail_offset >> (8 + 2 * b)) & 1) << b;
        }
        if (block_log2 & 1) {
            std::swap(x, y);
            if (elog & 1) y = (y << 1) | (x & 1), x >>= 1;
        }
        ConsoleMipLayout& mip = out.mips[i];
        mip.in_tail = true;
        mip.offset = tail_offset;
        mip.tail_offset = tail_offset;
        mip.tail_x = x * micro_w;
        mip.tail_y = y * micro_h;
        mip.pitch = pitch;
        mip.padded_height = rows;
        pitch = std::max(pitch >> 1, micro_w);
        rows = std::max(rows >> 1, micro_h);
    }
}

}  // namespace layout_detail

// The mode the console's layout library is taken to pick when the layout word leaves it open (VIctory's rule plus
// the two additions in the header comment). Assumes a known format.
inline SwizzleMode suggest_console_swizzle_mode(const ConsoleResourceDesc& desc, const ConsoleFormat& format) {
    if (desc.dimension == kDimensionTexture1D || format.element_bytes == 12) return SwizzleMode::linear;
    if (desc.flags & kFlagDepthStencil) return SwizzleMode::sw_64kb_z_x;
    if ((desc.flags & kFlagRenderTarget) || desc.sample_count > 1) return SwizzleMode::sw_64kb_r_x;
    return SwizzleMode::sw_4kb_s;
}

// The swizzle mode a layout word names, or the library's pick for word 0. False for a word with no reading.
inline bool console_swizzle_mode(const ConsoleResourceDesc& desc, const ConsoleFormat& format, SwizzleMode& out) {
    switch (desc.layout) {
    case kLayoutLibraryPick: out = suggest_console_swizzle_mode(desc, format); return true;
    case kLayoutLinear: out = SwizzleMode::linear; return true;
    case kLayout64KbUndefined:
    case kLayout64KbStandard: out = SwizzleMode::sw_64kb_s; return true;
    default:
        if ((desc.layout & ~0xffu) != kLayoutSwizzleFlag) return false;
        if ((desc.layout & 0xffu) > 31) return false;  // the known modes are bits of a 32-bit mask
        out = static_cast<SwizzleMode>(desc.layout & 0xff);
        return true;
    }
}

// Computes the console layout of a texture description. `out` is written only on ok.
inline ConsoleLayoutStatus compute_console_texture_layout(const ConsoleResourceDesc& desc, ConsoleTextureLayout& out) {
    using namespace layout_detail;
    if (desc.dimension < kDimensionTexture1D || desc.dimension > kDimensionTexture3D)
        return ConsoleLayoutStatus::not_texture;
    ConsoleFormat format;
    if (!console_format(desc.format, format)) return ConsoleLayoutStatus::unknown_format;
    const std::uint32_t samples = std::max(desc.sample_count, 1u);
    const bool three_d = desc.dimension == kDimensionTexture3D;
    if (desc.width < 1 || desc.width > kMaxExtent || desc.height < 1 || desc.height > kMaxExtent ||
        desc.depth_or_array_size < 1 || (three_d && desc.depth_or_array_size > kMaxExtent) ||
        (desc.dimension == kDimensionTexture1D && desc.height != 1) || samples > 16 || (samples & (samples - 1)) ||
        (desc.alignment & (desc.alignment - 1)))
        return ConsoleLayoutStatus::invalid;
    const auto width = static_cast<std::uint32_t>(desc.width);
    const std::uint32_t depth = three_d ? desc.depth_or_array_size : 1;
    const std::uint32_t levels = log2(std::max({width, desc.height, depth})) + 1;
    const std::uint32_t mips = desc.mip_levels ? desc.mip_levels : levels;
    if (mips > levels || mips > kMaxMips) return ConsoleLayoutStatus::invalid;
    SwizzleMode mode;
    if (!console_swizzle_mode(desc, format, mode)) return ConsoleLayoutStatus::invalid;
    const bool depth_flag = (desc.flags & kFlagDepthStencil) != 0;

    ConsoleTextureLayout result;
    result.mode = mode;
    result.mip_levels = mips;
    result.array_size = three_d ? 1 : desc.depth_or_array_size;
    result.plane_count = format.stencil ? 2 : 1;
    for (std::uint32_t plane = 0; plane < result.plane_count; ++plane) {
        PlaneInput in{desc.dimension, mode, plane ? 1 : format.element_bytes,
                      (width + format.block - 1) / format.block, (desc.height + format.block - 1) / format.block,
                      width, desc.height, format.block, 1, desc.depth_or_array_size, mips, samples};
        if (in.element_bytes == 12) {  // 96-bit: addrlib's expanded format, three 32-bit elements per texel
            if (mode != SwizzleMode::linear) return ConsoleLayoutStatus::invalid;
            in.element_bytes = 4;
            in.width *= 3;
            in.expand = 3;
        }
        if (!mode_allowed(in, depth_flag, format.block > 1)) return ConsoleLayoutStatus::invalid;
        compute_plane(in, result.planes[plane]);
    }
    std::uint64_t alignment = 0;
    for (std::uint32_t plane = 0; plane < result.plane_count; ++plane) {
        ConsolePlaneLayout& p = result.planes[plane];
        if (plane) p.offset = (result.size + p.alignment - 1) / p.alignment * p.alignment;
        result.size = p.offset + p.size;
        alignment = std::max(alignment, p.alignment);
    }
    result.alignment = std::max<std::uint64_t>(alignment, desc.alignment);
    result.allocation_size = (result.size + result.alignment - 1) / result.alignment * result.alignment;
    out = std::move(result);
    return ConsoleLayoutStatus::ok;
}

}  // namespace leonida::gfx10
