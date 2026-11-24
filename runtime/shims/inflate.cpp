// zlib / DEFLATE decompression (inflate.h), from RFC 1950 and RFC 1951.
//
// Huffman codes are decoded with a 9-bit first-level table and a canonical walk for longer codes: most literal and
// distance codes are 9 bits or shorter, so a symbol usually costs one table lookup.
#include "inflate.h"

#include <cstring>

namespace rt::inflate {
namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 9;

struct Huffman {
    uint16_t count[kMaxBits + 1];  // codes of each length
    uint16_t symbol[288];          // symbols in canonical order
    // fast[code reversed, kFastBits bits] = symbol | length << 9; 0 when the code is longer than kFastBits.
    uint16_t fast[1 << kFastBits];
};

// A simple, exact bit reader: tracks the total number of bits consumed so running past the end is detectable.
class Reader {
public:
    Reader(const uint8_t* in, size_t size) : in_(in), size_(size) {}

    // Peeks n (<= 24) bits, padding with zeros past the end.
    uint32_t peek(int n) {
        fill(n);
        return uint32_t(buffer_ & ((uint64_t(1) << n) - 1));
    }
    void drop(int n) {
        buffer_ >>= n;
        have_ -= n;
        consumed_ += size_t(n);
    }
    uint32_t take(int n) {
        if (n == 0) return 0;
        const uint32_t v = peek(n);
        drop(n);
        return v;
    }
    void align() { drop(have_ & 7); }
    bool overrun() const { return consumed_ > size_ * 8; }
    size_t bytes_used() const { return (consumed_ + 7) / 8; }
    // For stored blocks: the byte position of the next unread byte (after align()).
    size_t byte_pos() const { return consumed_ / 8; }
    void skip_bytes_to(size_t byte) {
        // Discard the buffer and restart at `byte`.
        buffer_ = 0;
        have_ = 0;
        next_ = byte;
        consumed_ = byte * 8;
    }

private:
    void fill(int n) {
        while (have_ < n) {
            const uint64_t b = next_ < size_ ? in_[next_] : 0;
            ++next_;
            buffer_ |= b << have_;
            have_ += 8;
        }
    }

    const uint8_t* in_;
    size_t size_;
    size_t next_ = 0;
    uint64_t buffer_ = 0;
    int have_ = 0;
    size_t consumed_ = 0;
};

uint32_t reverse_bits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

// Builds the tables from code lengths. Returns false for an over-subscribed set; an incomplete set is allowed (a
// single distance code is legal, RFC 1951 3.2.7), and decoding a missing code fails.
bool build(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    std::memset(h.fast, 0, sizeof(h.fast));
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    h.count[0] = 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    uint16_t offsets[kMaxBits + 2];
    offsets[1] = 0;
    for (int len = 1; len <= kMaxBits; ++len) offsets[len + 1] = uint16_t(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offsets[lengths[i]]++] = uint16_t(i);
    // Fast table: walk canonical codes of length <= kFastBits.
    uint32_t code = 0;
    int index = 0;
    for (int len = 1; len <= kFastBits; ++len) {
        for (int k = 0; k < h.count[len]; ++k) {
            const uint32_t rev = reverse_bits(code, len);
            const uint16_t entry = uint16_t(h.symbol[index] | (len << 9));
            for (uint32_t fill = rev; fill < (1u << kFastBits); fill += 1u << len) h.fast[fill] = entry;
            ++code;
            ++index;
        }
        code <<= 1;
    }
    return true;
}

// Decodes one symbol; -1 for a code not in the table.
int decode(Reader& r, const Huffman& h) {
    const uint16_t e = h.fast[r.peek(kFastBits)];
    if (e) {
        r.drop(e >> 9);
        return e & 0x1ff;
    }
    // Canonical walk, one bit at a time (codes longer than kFastBits, or a missing code).
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= int(r.take(1));
        const int count = h.count[len];
        if (code - first < count) return h.symbol[index + code - first];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Fixed {
    Huffman lit, dist;
    Fixed() {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; ++i) l[i] = 8;
        for (; i < 256; ++i) l[i] = 9;
        for (; i < 280; ++i) l[i] = 7;
        for (; i < 288; ++i) l[i] = 8;
        build(lit, l, 288);
        uint8_t d[30];
        for (i = 0; i < 30; ++i) d[i] = 5;
        build(dist, d, 30);
    }
};

const Fixed& fixed_tables() {
    static const Fixed f;
    return f;
}

Status codes(Reader& r, const Huffman& lit, const Huffman& dist, uint8_t* out, size_t cap, size_t& n) {
    for (;;) {
        const int sym = decode(r, lit);
        if (r.overrun()) return Status::truncated;
        if (sym < 0) return Status::bad_data;
        if (sym < 256) {
            if (n >= cap) return Status::output_full;
            out[n++] = uint8_t(sym);
        } else if (sym == 256) {
            return Status::ok;
        } else {
            const int li = sym - 257;
            if (li >= 29) return Status::bad_data;
            const size_t len = kLengthBase[li] + r.take(kLengthExtra[li]);
            const int ds = decode(r, dist);
            if (ds < 0 || ds >= 30) return r.overrun() ? Status::truncated : Status::bad_data;
            const size_t d = kDistBase[ds] + r.take(kDistExtra[ds]);
            if (r.overrun()) return Status::truncated;
            if (d > n) return Status::bad_data;
            if (len > cap - n) return Status::output_full;
            uint8_t* dst = out + n;
            const uint8_t* src = dst - d;
            if (d >= len) std::memcpy(dst, src, len);
            else
                for (size_t i = 0; i < len; ++i) dst[i] = src[i];  // overlapping: byte by byte
            n += len;
        }
    }
}

Status dynamic(Reader& r, uint8_t* out, size_t cap, size_t& n) {
    const int hlit = int(r.take(5)) + 257, hdist = int(r.take(5)) + 1, hclen = int(r.take(4)) + 4;
    if (hlit > 286 || hdist > 30) return Status::bad_data;
    static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t cl[19] = {};
    for (int i = 0; i < hclen; ++i) cl[order[i]] = uint8_t(r.take(3));
    if (r.overrun()) return Status::truncated;
    Huffman clh;
    if (!build(clh, cl, 19)) return Status::bad_data;
    uint8_t lengths[286 + 30] = {};
    int i = 0;
    while (i < hlit + hdist) {
        const int sym = decode(r, clh);
        if (r.overrun()) return Status::truncated;
        if (sym < 0) return Status::bad_data;
        if (sym < 16) {
            lengths[i++] = uint8_t(sym);
            continue;
        }
        int repeat = 0;
        uint8_t value = 0;
        if (sym == 16) {
            if (i == 0) return Status::bad_data;
            value = lengths[i - 1];
            repeat = 3 + int(r.take(2));
        } else if (sym == 17) {
            repeat = 3 + int(r.take(3));
        } else {
            repeat = 11 + int(r.take(7));
        }
        if (i + repeat > hlit + hdist) return Status::bad_data;
        while (repeat--) lengths[i++] = value;
    }
    if (lengths[256] == 0) return Status::bad_data;  // no end-of-block code
    Huffman lit, dist;
    if (!build(lit, lengths, hlit) || !build(dist, lengths + hlit, hdist)) return Status::bad_data;
    return codes(r, lit, dist, out, cap, n);
}

}  // namespace

Status raw(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t* out_size, size_t* in_used) {
    Reader r(in, in_size);
    size_t n = 0;
    Status s = Status::ok;
    for (bool last = false; !last && s == Status::ok;) {
        last = r.take(1) != 0;
        const uint32_t type = r.take(2);
        if (r.overrun()) {
            s = Status::truncated;
            break;
        }
        if (type == 0) {
            r.align();
            const size_t p = r.byte_pos();
            if (p + 4 > in_size) {
                s = Status::truncated;
                break;
            }
            const uint32_t len = in[p] | uint32_t(in[p + 1]) << 8;
            const uint32_t nlen = in[p + 2] | uint32_t(in[p + 3]) << 8;
            if ((len ^ 0xffff) != nlen) {
                s = Status::bad_data;
                break;
            }
            if (p + 4 + len > in_size) {
                s = Status::truncated;
                break;
            }
            if (len > out_capacity - n) {
                s = Status::output_full;
                break;
            }
            std::memcpy(out + n, in + p + 4, len);
            n += len;
            r.skip_bytes_to(p + 4 + len);
        } else if (type == 1) {
            s = codes(r, fixed_tables().lit, fixed_tables().dist, out, out_capacity, n);
        } else if (type == 2) {
            s = dynamic(r, out, out_capacity, n);
        } else {
            s = Status::bad_data;
        }
    }
    if (out_size) *out_size = n;
    if (in_used) *in_used = r.bytes_used();
    return s;
}

uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler) {
    uint32_t a = adler & 0xffff, b = adler >> 16;
    while (size) {
        const size_t chunk = size < 5552 ? size : 5552;  // largest n with no 32-bit overflow before the modulo
        for (size_t i = 0; i < chunk; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        data += chunk;
        size -= chunk;
    }
    return b << 16 | a;
}

Status zlib(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t* out_size) {
    if (out_size) *out_size = 0;
    if (in_size < 2) return Status::truncated;
    const uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
        return Status::bad_header;
    size_t n = 0, used = 0;
    const Status s = raw(in + 2, in_size - 2, out, out_capacity, &n, &used);
    if (out_size) *out_size = n;
    if (s != Status::ok) return s;
    const size_t t = 2 + used;
    if (t + 4 > in_size) return Status::truncated;
    const uint32_t expected = uint32_t(in[t]) << 24 | uint32_t(in[t + 1]) << 16 | uint32_t(in[t + 2]) << 8 | in[t + 3];
    return adler32(out, n) == expected ? Status::ok : Status::bad_checksum;
}

const char* status_name(Status status) {
    switch (status) {
    case Status::ok: return "ok";
    case Status::bad_header: return "bad header";
    case Status::bad_data: return "bad data";
    case Status::truncated: return "truncated";
    case Status::output_full: return "output full";
    case Status::bad_checksum: return "bad checksum";
    }
    return "?";
}

}  // namespace rt::inflate
