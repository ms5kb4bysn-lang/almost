// BCPack decoding for the DirectStorage shim (xcrdapi.cpp): the Xbox Series codec that packs a BC1-BC7 block stream
// into interleaved rANS lanes. Standard library only.
//
// A C++ port of the repository's clean-room decoder, tools/bcpack.py (the lane-array model and the block rebuilders;
// docs/textures.md describes the format), following its Rust port of the lane model in
// tools/bcpack_calibration/src/model.rs. The constant tables are generated from tools/bcpack_tables.py and
// tools/bcpack_tables_bptc.py; the rANS probability models in them are those of Microsoft's silicon-rans
// (mdu_minirans.v, MIT licence, see third_party/bcpack-rs/THIRD_PARTY_NOTICES.md), and the BPTC partition and anchor
// tables are the public D3D11 / Khronos ones. The rest is this project's (GPL-3.0).
//
// Formats are DSTORAGE_BCPACK_MODE numbers: 1 BC1, 3 BC3, 4 BC4, 5 BC5, 6 BC6H, 7 BC7. BC2 (2) has no decoder: no
// title data uses it.
//
// The format predicts each block from one of the 256 blocks before it; references before the first block of a stream
// read a built-in history the codec keeps. Only part of that history is known (docs/textures.md): BC1 has 252
// confirmed entries, BC4 and BC5 a few, BC3 none confirmed, and BC6H and BC7 none at all (their entries are not in the
// repository). Strict decoding refuses a stream that needs an entry that is not confirmed, exactly as
// `bcpack.decode(..., allow_provisional=False)` does; lenient decoding uses the provisional entries, a neutral value
// for unknown ones, keeps a block that fails a consistency check, and counts the blocks it had to approximate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt::bcpack {

enum class Status {
    ok,
    unsupported_format,  // not 1, 3, 4, 5, 6 or 7
    bad_size,            // the output is not a whole, non-zero number of blocks
    bad_stream,          // the rANS lanes do not decode (truncated, corrupt, or an impossible symbol)
    trailing_data,       // non-zero bytes after the last lane finished
    history,             // strict: needs a built-in history entry or BC7 signature that is not confirmed
    inconsistent,        // strict: a rebuilt BC6H / BC7 block breaks an anchor or p-bit rule
};

enum class Mode { strict, lenient };

struct Info {
    size_t bytes_used = 0;    // stream bytes the lanes consumed
    size_t approximated = 0;  // lenient: blocks rebuilt from a provisional or unknown entry, or kept inconsistent
    char detail[160] = {};    // why a decode failed, or what lenient decoding approximated first
};

// Bytes per block of a format (8 for BC1/BC4, 16 for BC3/BC5/BC6H/BC7), 0 for one without a decoder.
size_t block_bytes(unsigned format);

// Decodes a BCPack stream into exactly out_size bytes of BCn blocks. Zero bytes after the stream (the container's
// padding) are allowed.
Status decode(unsigned format, const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size,
              Mode mode = Mode::strict, Info* info = nullptr);

// The entropy layer alone: one symbol per lane per 8 output bytes, lane-major (lanes[lane * symbols + k]).
Status decode_lanes(unsigned format, const uint8_t* in, size_t in_size, size_t out_size,
                    std::vector<uint16_t>& lanes, Info* info = nullptr);

const char* status_name(Status status);

}  // namespace rt::bcpack
