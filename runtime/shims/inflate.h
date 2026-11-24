// zlib (RFC 1950) and raw DEFLATE (RFC 1951) decompression, for the DirectStorage shim (xcrdapi.cpp), whose requests
// can name zlib-compressed sources. Written from the RFCs; standard library only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::inflate {

enum class Status {
    ok,
    bad_header,     // not a zlib stream: CM is not 8, the check bits fail, or a preset dictionary is required
    bad_data,       // an invalid block type, code lengths or distance
    truncated,      // the input ends before the final block (or before the Adler-32 trailer)
    output_full,    // the stream produces more than out_capacity bytes
    bad_checksum,   // the Adler-32 trailer does not match the output
};

// Inflates a raw DEFLATE stream into out[0, out_capacity). *out_size is the number of bytes produced, also on failure.
Status raw(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t* out_size,
           size_t* in_used = nullptr);

// Inflates a zlib stream (2-byte header, DEFLATE data, big-endian Adler-32 trailer) and checks the trailer.
Status zlib(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t* out_size);

uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

const char* status_name(Status status);

}  // namespace rt::inflate
