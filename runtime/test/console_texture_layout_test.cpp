// The console's (GFX10.3) texture layout model in src/console_texture_layout.h, on hand-written descriptions only:
// the one measured allocation (512x512 BC7, 10 mips = 0x56000 bytes, 4 KiB aligned), the swizzle-mode picks and the
// layout-word readings, two-plane depth/stencil, 3D and array textures, and the descriptions the model rejects. The
// full corpus comparison lives in docs/textures.md (tools/tex_headers.py); this test needs no game files.
#include "../src/console_texture_layout.h"

#include <cstdint>
#include <cstdio>

namespace {

using namespace leonida::gfx10;

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "console_texture_layout_test: %s\n", what);
    ++g_failures;
}

void expect_equal(std::uint64_t got, std::uint64_t want, const char* what) {
    if (got == want) return;
    std::fprintf(stderr, "console_texture_layout_test: %s: got 0x%llx, want 0x%llx\n", what,
                 static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    ++g_failures;
}

ConsoleResourceDesc texture2d(std::uint64_t width, std::uint32_t height, std::uint16_t mips, std::uint32_t format) {
    ConsoleResourceDesc desc{};
    desc.dimension = kDimensionTexture2D;
    desc.width = width;
    desc.height = height;
    desc.depth_or_array_size = 1;
    desc.mip_levels = mips;
    desc.format = format;
    desc.sample_count = 1;
    return desc;
}

ConsoleLayoutStatus lay_out(const ConsoleResourceDesc& desc, ConsoleTextureLayout& layout) {
    return compute_console_texture_layout(desc, layout);
}

// The number VIctory states for the console (crates/victory-formats/src/texture.rs, read in the header's comment):
// 512x512 BC7, 10 mips, 0x56000 bytes, 4 KiB alignment.
void check_measured_bc7() {
    ConsoleResourceDesc desc = texture2d(512, 512, 10, 98);  // DXGI_FORMAT_BC7_UNORM
    ConsoleTextureLayout layout;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::ok, "512x512 BC7 lays out");
    expect(layout.mode == SwizzleMode::sw_4kb_s, "512x512 BC7 takes the library's SW_4KB_S pick");
    expect_equal(layout.mip_levels, 10, "512x512 BC7 mip count");
    expect_equal(layout.plane_count, 1, "512x512 BC7 plane count");
    expect_equal(layout.subresource_count(), 10, "512x512 BC7 subresource count");
    expect_equal(layout.allocation_size, 0x56000, "512x512 BC7 allocation");
    expect_equal(layout.alignment, 0x1000, "512x512 BC7 alignment");
    for (std::uint32_t i = 0; i < layout.subresource_count(); ++i) {
        ConsoleSubresource sub{};
        expect(layout.subresource(i, sub), "512x512 BC7 subresource in range");
        expect(sub.offset < layout.allocation_size, "512x512 BC7 subresource inside the allocation");
    }
    ConsoleSubresource past{};
    expect(!layout.subresource(layout.subresource_count(), past), "subresource past the end is refused");
}

void check_buffers_and_modes() {
    ConsoleResourceDesc buffer{};
    buffer.dimension = kDimensionBuffer;
    buffer.width = 65536;
    ConsoleTextureLayout layout;
    expect(lay_out(buffer, layout) == ConsoleLayoutStatus::not_texture, "a buffer is not a texture");

    // Render targets and depth buffers take the console's pick; both are measured picks (VIctory's rule here).
    ConsoleResourceDesc rt = texture2d(1920, 1080, 1, 10);  // R16G16B16A16_FLOAT
    rt.flags = kFlagRenderTarget;
    expect(lay_out(rt, layout) == ConsoleLayoutStatus::ok, "render target lays out");
    expect(layout.mode == SwizzleMode::sw_64kb_r_x, "render target takes SW_64KB_R_X");
    expect(layout.alignment >= 0x1000, "render target alignment is at least a page");

    ConsoleResourceDesc depth = texture2d(1024, 1024, 1, 45);  // D24_UNORM_S8_UINT
    depth.flags = kFlagDepthStencil;
    expect(lay_out(depth, layout) == ConsoleLayoutStatus::ok, "depth buffer lays out");
    expect(layout.mode == SwizzleMode::sw_64kb_z_x, "depth buffer takes SW_64KB_Z_X");

    // Layout words: 1 linear, 2/3 the public 64 KiB words, 0x100|n the console's swizzle number.
    ConsoleResourceDesc word = texture2d(256, 256, 1, 28);  // R8G8B8A8_UNORM
    word.layout = kLayoutLinear;
    expect(lay_out(word, layout) == ConsoleLayoutStatus::ok && layout.mode == SwizzleMode::linear,
           "layout word 1 is linear");
    for (std::uint32_t public_word : {kLayout64KbUndefined, kLayout64KbStandard}) {
        word.layout = public_word;
        expect(lay_out(word, layout) == ConsoleLayoutStatus::ok && layout.mode == SwizzleMode::sw_64kb_s,
               "layout words 2 and 3 read as SW_64KB_S");
    }
    word.layout = kLayoutSwizzleFlag | 5;  // SW_4KB_S in addrlib's numbering
    expect(lay_out(word, layout) == ConsoleLayoutStatus::ok && layout.mode == SwizzleMode::sw_4kb_s,
           "layout word 0x105 is SW_4KB_S");
    word.layout = kLayoutSwizzleFlag | 0x20;  // not a GFX10 mode
    expect(lay_out(word, layout) == ConsoleLayoutStatus::invalid, "an unknown swizzle number is invalid");
    word.layout = 0x201;  // not the 0x100 flag
    expect(lay_out(word, layout) == ConsoleLayoutStatus::invalid, "an unreadable layout word is invalid");
}

void check_depth_stencil_planes() {
    ConsoleResourceDesc desc = texture2d(1024, 1024, 1, 45);  // D24_UNORM_S8_UINT
    desc.flags = kFlagDepthStencil;
    ConsoleTextureLayout layout;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::ok, "D24S8 lays out");
    expect_equal(layout.plane_count, 2, "D24S8 has two planes");
    expect_equal(layout.subresource_count(), 2, "D24S8 has one subresource per plane");
    expect(layout.planes[1].offset >= layout.planes[0].size, "the stencil plane starts after the depth plane");
    expect_equal(layout.planes[1].offset % layout.planes[1].alignment, 0, "the stencil plane meets its alignment");
    expect(layout.planes[0].offset < layout.planes[1].offset, "planes are laid out in order");

    ConsoleSubresource depth{}, stencil{};
    expect(layout.subresource(0, depth) && layout.subresource(1, stencil), "D24S8 subresources resolve");
    expect_equal(depth.plane, 0, "subresource 0 is the depth plane");
    expect_equal(stencil.plane, 1, "subresource 1 is the stencil plane");
}

void check_3d_and_arrays() {
    ConsoleResourceDesc volume = texture2d(256, 256, 1, 28);
    volume.dimension = kDimensionTexture3D;
    volume.depth_or_array_size = 64;
    ConsoleTextureLayout layout;
    expect(lay_out(volume, layout) == ConsoleLayoutStatus::ok, "a 3D texture lays out");
    expect_equal(layout.array_size, 1, "a 3D texture has no array slices");
    expect_equal(layout.subresource_count(), 1, "a one-mip 3D texture has one subresource");
    ConsoleSubresource sub{};
    expect(layout.subresource(0, sub) && sub.depth == 64, "the 3D subresource keeps its depth");
    expect(sub.depth_stride >= volume.width * 4, "the 3D depth stride covers a slice");

    ConsoleResourceDesc array = texture2d(128, 128, 1, 28);
    array.depth_or_array_size = 4;
    expect(lay_out(array, layout) == ConsoleLayoutStatus::ok, "an array texture lays out");
    expect_equal(layout.array_size, 4, "the array keeps its slice count");
    expect_equal(layout.subresource_count(), 4, "the array has one subresource per slice");
    expect_equal(layout.size, layout.planes[0].slice_stride * 4, "the array size is four slice chains");
    ConsoleSubresource first{}, last{};
    expect(layout.subresource(0, first) && layout.subresource(3, last), "array subresources resolve");
    expect_equal(last.offset - first.offset, layout.planes[0].slice_stride * 3, "slices are one chain apart");
}

void check_rejections() {
    ConsoleTextureLayout layout;
    ConsoleResourceDesc desc = texture2d(64, 64, 8, 28);
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::invalid, "more mips than the chain has is invalid");

    desc = texture2d(64, 64, 1, 28);
    desc.dimension = kDimensionTexture1D;
    desc.height = 2;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::invalid, "a 1D texture with height 2 is invalid");

    desc = texture2d(64, 64, 1, 28);
    desc.sample_count = 3;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::invalid, "a non-power-of-two sample count is invalid");

    desc = texture2d(64, 64, 1, 28);
    desc.alignment = 96;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::invalid, "a non-power-of-two alignment is invalid");

    desc = texture2d(64, 64, 1, 9999);
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::unknown_format, "an unknown DXGI format has no element size");

    desc = texture2d(64, 64, 1, 28);
    desc.width = 0;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::invalid, "a zero width is invalid");

    // A 12-byte format (96-bit) is linear in addrlib's expanded 3-elements-per-texel form, and refused tiled.
    desc = texture2d(64, 64, 1, 6);  // R32G32B32_FLOAT
    desc.layout = kLayoutSwizzleFlag | 5;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::invalid, "a tiled 96-bit texture is invalid");
    desc.layout = kLayoutLinear;
    expect(lay_out(desc, layout) == ConsoleLayoutStatus::ok, "a linear 96-bit texture lays out");
    expect_equal(layout.planes[0].mips[0].width, 64 * 3, "the 96-bit mip expands to three 32-bit elements per texel");
}

}  // namespace

int main() {
    check_measured_bc7();
    check_buffers_and_modes();
    check_depth_stencil_planes();
    check_3d_and_arrays();
    check_rejections();
    if (g_failures) {
        std::fprintf(stderr, "console_texture_layout_test: %d failure%s\n", g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("console_texture_layout_test: the measured allocation, the picks and the rejections hold\n");
    return 0;
}
