// The DirectStorage shim (shims/xcrdapi.cpp) and its inflate (shims/inflate.cpp), without game data.
//
// The objects are driven through their method tables at the slots the title calls (see xcrdapi.cpp): the factory's
// OpenFile, CreateStatusArray, SetStagingBufferSize and CreateQueue; the queue's EnqueueRequest, EnqueueStatus, Submit
// and Close; the status array's IsComplete and GetHResult. Requests read a temporary file or memory, raw or zlib, into
// an address or into physical pages from the xmem shim. The zlib streams are Python zlib output over make_input(n).
// The BCPack decoder (shims/bcpack.cpp) is checked against synthetic streams that tools/bcpack.py decodes
// (bcpack_test_vectors.h), and BCPack requests, with and without zlib, through the queue.
#include "../shims/bcpack.h"
#include "../shims/inflate.h"
#include "../shims/shims.h"
#include "bcpack_test_vectors.h"
#include "xcrdapi_test_streams.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

std::vector<uint8_t> make_input(size_t n) {
    static const char* const words[16] = {"the ",  "quick ", "brown ", "fox ",  "jumps ", "over ",  "lazy ",  "dog ",
                                          "and ",  "runs ",  "far ",   "away ", "from ",  "home\n", "zebra ", "xylophone "};
    std::vector<uint8_t> s;
    uint32_t x = 12345;
    while (s.size() < n) {
        x = (x * 1103515245u + 12345u) & 0x7fffffffu;
        const char* w = words[(x >> 16) & 15];
        s.insert(s.end(), w, w + std::strlen(w));
    }
    s.resize(n);
    return s;
}

// --- inflate -------------------------------------------------------------------------------------------------------

void test_inflate_stream(const char* name, const uint8_t* z, size_t z_size, uint32_t size) {
    using rt::inflate::Status;
    const std::vector<uint8_t> want = make_input(size);
    std::vector<uint8_t> out(size + 16, 0xcd);
    size_t produced = 0;
    const Status s = rt::inflate::zlib(z, z_size, out.data(), out.size(), &produced);
    std::string label = std::string(name) + ": inflates to the input";
    check(s == Status::ok && produced == size && std::memcmp(out.data(), want.data(), size) == 0, label.c_str());

    std::vector<uint8_t> tight(size);
    label = std::string(name) + ": an exactly sized output is enough";
    check(rt::inflate::zlib(z, z_size, tight.data(), tight.size(), &produced) == Status::ok, label.c_str());
    if (size > 1) {
        std::vector<uint8_t> small(size - 1);
        label = std::string(name) + ": one byte short is output_full";
        check(rt::inflate::zlib(z, z_size, small.data(), small.size(), &produced) == Status::output_full,
              label.c_str());
    }
    label = std::string(name) + ": a cut stream is truncated";
    check(rt::inflate::zlib(z, z_size / 2, out.data(), out.size(), &produced) == Status::truncated ||
              rt::inflate::zlib(z, z_size / 2, out.data(), out.size(), &produced) == Status::bad_data,
          label.c_str());
    std::vector<uint8_t> bad(z, z + z_size);
    bad.back() ^= 0x01;
    label = std::string(name) + ": a wrong Adler-32 is bad_checksum";
    check(rt::inflate::zlib(bad.data(), bad.size(), out.data(), out.size(), &produced) == Status::bad_checksum,
          label.c_str());
}

void test_inflate() {
    std::printf("\n-- inflate --\n");
    test_inflate_stream("stored block", k_stored_z, sizeof(k_stored_z), k_stored_size);
    test_inflate_stream("fixed Huffman", k_fixed_z, sizeof(k_fixed_z), k_fixed_size);
    test_inflate_stream("dynamic Huffman", k_dynamic_z, sizeof(k_dynamic_z), k_dynamic_size);
    test_inflate_stream("past the 32 KiB window", k_long_z, sizeof(k_long_z), k_long_size);

    using rt::inflate::Status;
    uint8_t out[16];
    size_t n = 0;
    const uint8_t not_deflate[] = {0x79, 0x9c, 0, 0};  // CM 9
    check(rt::inflate::zlib(not_deflate, sizeof(not_deflate), out, sizeof(out), &n) == Status::bad_header,
          "a header whose method is not 8 is bad_header");
    const uint8_t bad_check[] = {0x78, 0x9d, 0, 0};
    check(rt::inflate::zlib(bad_check, sizeof(bad_check), out, sizeof(out), &n) == Status::bad_header,
          "a header whose check bits fail is bad_header");
    const uint8_t dictionary[] = {0x78, 0xbb, 0, 0, 0, 0};  // FDICT set, check bits valid
    check(rt::inflate::zlib(dictionary, sizeof(dictionary), out, sizeof(out), &n) == Status::bad_header,
          "a stream that needs a preset dictionary is bad_header");
    const uint8_t reserved_block[] = {0x07};  // BFINAL 1, BTYPE 3
    check(rt::inflate::raw(reserved_block, sizeof(reserved_block), out, sizeof(out), &n) == Status::bad_data,
          "block type 3 is bad_data");
    const uint8_t empty_fixed[] = {0x03, 0x00};  // a final fixed block holding only end-of-block
    check(rt::inflate::raw(empty_fixed, sizeof(empty_fixed), out, sizeof(out), &n) == Status::ok && n == 0,
          "an empty fixed block inflates to nothing");
    check(rt::inflate::adler32(reinterpret_cast<const uint8_t*>("Wikipedia"), 9) == 0x11E60398u,
          "Adler-32 of \"Wikipedia\" is 0x11E60398");
}

// --- BCPack ----------------------------------------------------------------------------------------------------------

std::vector<uint8_t> from_hex(const char* hex) {
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> out(std::strlen(hex) / 2);
    for (size_t k = 0; k < out.size(); ++k) out[k] = static_cast<uint8_t>(nibble(hex[2 * k]) << 4 | nibble(hex[2 * k + 1]));
    return out;
}

uint64_t fnv1a(const uint8_t* data, size_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t k = 0; k < size; ++k) h = (h ^ data[k]) * 0x100000001b3ull;
    return h;
}

// The vector's stream with its container padding.
std::vector<uint8_t> packed_of(const bcpack_vectors::Vector& v) {
    std::vector<uint8_t> packed = from_hex(v.packed);
    packed.resize(packed.size() + v.padding, 0);
    return packed;
}

bool matches(const bcpack_vectors::Vector& v, const std::vector<uint8_t>& out) {
    if (out.size() != v.out_size || fnv1a(out.data(), out.size()) != v.hash) return false;
    return !v.expected || from_hex(v.expected) == out;
}

void test_bcpack() {
    std::printf("\n-- BCPack --\n");
    using rt::bcpack::Mode;
    using rt::bcpack::Status;
    int formats_strict[8] = {}, formats_lenient[8] = {};
    for (const auto& v : bcpack_vectors::k_vectors) {
        const std::vector<uint8_t> packed = packed_of(v);
        std::vector<uint8_t> out(v.out_size, 0xcd);
        rt::bcpack::Info info;
        const Status strict = rt::bcpack::decode(v.format, packed.data(), packed.size(), out.data(), out.size(),
                                                 Mode::strict, &info);
        const bool strict_ok = std::strcmp(v.strict, "ok") == 0;
        std::string label = std::string(v.name) + ": strict decoding is " + v.strict;
        check(std::strcmp(rt::bcpack::status_name(strict), v.strict) == 0, label.c_str());
        if (strict != Status::ok && std::strcmp(rt::bcpack::status_name(strict), v.strict) != 0)
            std::printf("      (%s: %s)\n", rt::bcpack::status_name(strict), info.detail);
        if (strict_ok) {
            label = std::string(v.name) + ": strict blocks match tools/bcpack.py";
            check(strict == Status::ok && matches(v, out), label.c_str());
            label = std::string(v.name) + ": the lanes consume the stream up to its padding";
            check(info.bytes_used == packed.size() - v.padding, label.c_str());
            if (strict == Status::ok && matches(v, out)) ++formats_strict[v.format];
        }
        std::vector<uint8_t> lenient(v.out_size, 0xcd);
        rt::bcpack::Info linfo;
        const Status ls = rt::bcpack::decode(v.format, packed.data(), packed.size(), lenient.data(), lenient.size(),
                                             Mode::lenient, &linfo);
        label = std::string(v.name) + ": lenient blocks match tools/bcpack.py (allow_provisional)";
        check(ls == Status::ok && matches(v, lenient), label.c_str());
        if (ls != Status::ok) std::printf("      (%s: %s)\n", rt::bcpack::status_name(ls), linfo.detail);
        label = std::string(v.name) + (strict_ok ? ": lenient decoding approximates nothing"
                                                 : ": lenient decoding counts the approximated blocks");
        check(strict_ok ? linfo.approximated == 0 : linfo.approximated > 0, label.c_str());
        if (!strict_ok && ls == Status::ok && matches(v, lenient)) ++formats_lenient[v.format];
    }
    for (unsigned f : {1u, 3u, 4u, 5u, 6u, 7u}) {
        const std::string label = "BC" + std::string(f == 6 ? "6H" : std::to_string(f)) +
                                  ": strict and lenient vectors decode byte-exact";
        check(formats_strict[f] >= 2 && formats_lenient[f] >= 2, label.c_str());
    }

    // The one-block BC7 example of docs/textures.md ("From a chunk entry to bcpack.decode").
    const std::vector<uint8_t> doc = from_hex(
        "101010101010101004040303030304040101010101010140000000000000024002027546281800000101df5f4b7c0341"
        "0202024110101010040404031010101040041001030303044001080001010104000000080000000041020994e4db27a4"
        "410511fe6b020005");
    uint8_t block[16] = {};
    check(rt::bcpack::decode(7, doc.data(), doc.size(), block, 16) == Status::ok &&
              from_hex("40317204a56819be023456789abcdef0") == std::vector<uint8_t>(block, block + 16),
          "the BC7 example of docs/textures.md decodes to its block");

    // Refusals.
    const auto& v = bcpack_vectors::k_vectors[0];  // BC1, strict
    const std::vector<uint8_t> packed = packed_of(v);
    std::vector<uint8_t> out(v.out_size + 64);
    check(rt::bcpack::block_bytes(2) == 0 && rt::bcpack::block_bytes(8) == 0 && rt::bcpack::block_bytes(1) == 8 &&
              rt::bcpack::block_bytes(7) == 16,
          "block sizes: BC1 8, BC7 16; BC2 and format 8 have no decoder");
    check(rt::bcpack::decode(2, packed.data(), packed.size(), out.data(), 16) == Status::unsupported_format,
          "BC2 is unsupported_format");
    check(rt::bcpack::decode(1, packed.data(), packed.size(), out.data(), v.out_size - 4) == Status::bad_size &&
              rt::bcpack::decode(1, packed.data(), packed.size(), out.data(), 0) == Status::bad_size,
          "an output that is not a whole number of blocks is bad_size");
    check(rt::bcpack::decode(1, packed.data(), packed.size() / 2, out.data(), v.out_size) == Status::bad_stream,
          "a cut stream is bad_stream");
    check(rt::bcpack::decode(1, packed.data(), packed.size(), out.data(), v.out_size + 64) == Status::bad_stream,
          "a stream too short for its output is bad_stream");
    std::vector<uint8_t> trailing(packed);
    trailing.push_back(0);
    trailing.push_back(1);
    check(rt::bcpack::decode(1, trailing.data(), trailing.size(), out.data(), v.out_size) == Status::trailing_data,
          "a non-zero byte after the stream is trailing_data");
    std::vector<uint8_t> flipped(packed);
    flipped.back() ^= 0xff;
    check(rt::bcpack::decode(1, flipped.data(), flipped.size(), out.data(), v.out_size) != Status::ok,
          "a corrupt last byte does not decode");
    std::vector<uint16_t> lanes;
    rt::bcpack::Info info;
    check(rt::bcpack::decode_lanes(1, packed.data(), packed.size(), v.out_size, lanes, &info) == Status::ok &&
              lanes.size() == 12 * (v.out_size / 8) && info.bytes_used == packed.size(),
          "decode_lanes gives 12 lanes of one symbol per block for BC1");
}

// --- DirectStorage ---------------------------------------------------------------------------------------------------

constexpr GUID kFactoryIid{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID kFileIid{0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID kStatusArrayIid{0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID kQueueIid{0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
constexpr HRESULT kPending = static_cast<HRESULT>(0x8000000Au);

struct Request {
    uint64_t options;
    void* destination;
    uint64_t page_offset;
    uint32_t destination_size;
    const void* source;
    uint64_t offset;
    uint32_t source_size;
    uint32_t intermediate_size;
    uint64_t tag;
    const char* name;
};
static_assert(sizeof(Request) == 0x48);

struct QueueDesc {
    uint64_t source_type;
    uint16_t capacity;
    int8_t priority;
    const char* name;
    uint64_t unknown;
};

constexpr uint64_t kZlib = 1, kPageDestination = 1ull << 21, kMemorySource = 1ull << 22;

using GetFactoryFn = HRESULT(WINAPI*)(const GUID*, void**);

template <typename F>
F slot(void* object, int index) {
    return reinterpret_cast<F>((*static_cast<void***>(object))[index]);
}

ULONG release(void* o) { return slot<ULONG(STDMETHODCALLTYPE*)(void*)>(o, 2)(o); }

struct Api {
    void* factory = nullptr;
    HRESULT open(const wchar_t* path, void** file) {
        return slot<HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, const GUID*, void**)>(factory, 4)(
            factory, path, &kFileIid, file);
    }
    HRESULT status_array(uint32_t capacity, void** out) {
        return slot<HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, const char*, const GUID*, void**)>(factory, 5)(
            factory, capacity, "test", &kStatusArrayIid, out);
    }
    HRESULT staging(uint32_t size) { return slot<HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t)>(factory, 8)(factory, size); }
    HRESULT queue(uint16_t capacity, void** out) {
        QueueDesc desc{0, capacity, 0, "test queue", 1};
        return slot<HRESULT(STDMETHODCALLTYPE*)(void*, const QueueDesc*, const GUID*, void**)>(factory, 10)(
            factory, &desc, &kQueueIid, out);
    }
    static void enqueue(void* q, const Request& r) {
        slot<void(STDMETHODCALLTYPE*)(void*, const Request*)>(q, 3)(q, &r);
    }
    static void enqueue_status(void* q, void* array, uint32_t index) {
        slot<void(STDMETHODCALLTYPE*)(void*, void*, uint32_t)>(q, 4)(q, array, index);
    }
    static void submit(void* q) { slot<void(STDMETHODCALLTYPE*)(void*)>(q, 6)(q); }
    static void close(void* q) { slot<void(STDMETHODCALLTYPE*)(void*)>(q, 8)(q); }
    static bool complete(void* a, uint32_t i) { return slot<bool(STDMETHODCALLTYPE*)(void*, uint32_t)>(a, 3)(a, i); }
    static HRESULT result(void* a, uint32_t i) { return slot<HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t)>(a, 4)(a, i); }
};

std::wstring temp_file(const std::vector<uint8_t>& bytes) {
    wchar_t dir[MAX_PATH] = {}, path[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"xcr", 0, path);
    const HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    return path;
}

void test_factory(Api& api) {
    std::printf("\n-- factory --\n");
    auto get = reinterpret_cast<GetFactoryFn>(rt::shims::find("xcrdapi.dll", "DStorageGetFactory"));
    check(get != nullptr, "the registry serves xcrdapi.dll!DStorageGetFactory");
    check(rt::shims::owns_dll("xcrdapi.dll"), "the shims own xcrdapi.dll");
    if (!get) return;
    void* other = reinterpret_cast<void*>(1);
    check(get(&kQueueIid, &other) == E_NOINTERFACE && other == nullptr,
          "DStorageGetFactory refuses another interface and clears the output");
    check(get(&kFactoryIid, &api.factory) == S_OK && api.factory, "DStorageGetFactory gives the factory for its IID");
    void* again = nullptr;
    check(get(&kFactoryIid, &again) == S_OK && again == api.factory, "the factory is one object");
    check(api.staging(0x2000000) == S_OK, "SetStagingBufferSize(32 MiB), which the title requires, succeeds");
    void* missing = reinterpret_cast<void*>(1);
    check(api.open(L"Z:\\definitely\\not\\here.bin", &missing) < 0 && missing == nullptr,
          "OpenFile of a missing file fails and leaves the output null");
}

void test_requests(Api& api) {
    std::printf("\n-- requests --\n");
    const std::vector<uint8_t> plain = make_input(k_dynamic_size);
    std::vector<uint8_t> contents(plain);
    contents.insert(contents.end(), k_dynamic_z, k_dynamic_z + sizeof(k_dynamic_z));  // a zlib chunk after the text
    const std::wstring path = temp_file(contents);

    void *file = nullptr, *array = nullptr, *queue = nullptr;
    check(api.open(path.c_str(), &file) == S_OK && file, "OpenFile opens a host file");
    check(api.status_array(4, &array) == S_OK && array, "CreateStatusArray(4)");
    check(api.queue(64, &queue) == S_OK && queue, "CreateQueue");
    if (!file || !array || !queue) return;
    check(Api::complete(array, 0) && Api::result(array, 0) == S_OK, "a new status entry is complete with S_OK");
    check(Api::result(array, 9) == E_INVALIDARG, "an entry past the end is E_INVALIDARG");

    // 1: a raw read from the file at an offset, and a zlib chunk from the file.
    std::vector<uint8_t> raw(1000, 0), inflated(k_dynamic_size + 8, 0);
    Request r{};
    r.destination = raw.data();
    r.destination_size = 1000;
    r.source = file;
    r.offset = 123;
    r.source_size = 1000;
    Api::enqueue(queue, r);
    Request z{};
    z.options = kZlib;
    z.destination = inflated.data();
    z.destination_size = static_cast<uint32_t>(inflated.size());
    z.source = file;
    z.offset = plain.size();
    z.source_size = sizeof(k_dynamic_z);
    Api::enqueue(queue, z);
    Api::enqueue_status(queue, array, 0);
    check(!Api::complete(array, 0) && Api::result(array, 0) == kPending, "an enqueued entry is pending before Submit");
    Api::submit(queue);
    check(Api::complete(array, 0) && Api::result(array, 0) == S_OK, "after Submit the entry is S_OK");
    check(std::memcmp(raw.data(), plain.data() + 123, 1000) == 0, "the raw read holds the file's bytes at its offset");
    check(std::memcmp(inflated.data(), plain.data(), plain.size()) == 0, "the zlib chunk inflates into the destination");

    // 2: memory sources, raw and zlib; a failure is reported by the next entry only.
    std::vector<uint8_t> copy(300, 0), from_memory(k_fixed_size, 0);
    Request m{};
    m.options = kMemorySource;
    m.destination = copy.data();
    m.destination_size = 300;
    m.source = plain.data();
    m.source_size = 300;
    Api::enqueue(queue, m);
    Request mz{};
    mz.options = kMemorySource | kZlib;
    mz.destination = from_memory.data();
    mz.destination_size = k_fixed_size;
    mz.source = k_fixed_z;
    mz.source_size = sizeof(k_fixed_z);
    Api::enqueue(queue, mz);
    Api::enqueue_status(queue, array, 1);
    Request bc{};
    bc.options = kMemorySource | (2ull << 1);  // BCPack to BC2: no decoder
    bc.destination = copy.data();
    bc.destination_size = 16;
    bc.source = plain.data();
    bc.source_size = 16;
    Api::enqueue(queue, bc);
    Api::enqueue_status(queue, array, 2);
    Api::enqueue(queue, m);
    Api::enqueue_status(queue, array, 3);
    Api::submit(queue);
    check(Api::result(array, 1) == S_OK, "memory sources, raw and zlib, succeed");
    check(std::memcmp(copy.data(), plain.data(), 300) == 0 &&
              std::memcmp(from_memory.data(), make_input(k_fixed_size).data(), k_fixed_size) == 0,
          "memory sources land in their destinations");
    check(Api::result(array, 2) == E_NOTIMPL, "a BCPack request to BC2 fails its entry with E_NOTIMPL");
    check(Api::result(array, 3) == S_OK, "the entry after it reports only the requests since the failure");

    // 3: corrupt zlib data fails with ERROR_INVALID_DATA; a source larger than its destination is refused.
    std::vector<uint8_t> bad(k_dynamic_z, k_dynamic_z + sizeof(k_dynamic_z));
    bad[bad.size() / 2] ^= 0xff;
    Request c = mz;
    c.source = bad.data();
    c.source_size = static_cast<uint32_t>(bad.size());
    c.destination = inflated.data();
    c.destination_size = static_cast<uint32_t>(inflated.size());
    Api::enqueue(queue, c);
    Api::enqueue_status(queue, array, 0);
    Api::submit(queue);
    check(FAILED(Api::result(array, 0)), "corrupt zlib data fails its entry");
    Request big = m;
    big.destination_size = 10;
    Api::enqueue(queue, big);
    Api::enqueue_status(queue, array, 1);
    Api::submit(queue);
    check(Api::result(array, 1) == E_INVALIDARG, "a raw source larger than its destination is E_INVALIDARG");

    // 4: auto-submit at half the capacity, and Close dropping what was not submitted.
    void* small_queue = nullptr;
    check(api.queue(4, &small_queue) == S_OK, "CreateQueue with capacity 4");
    if (small_queue) {
        std::vector<uint8_t> a(300, 0);
        Request s = m;
        s.destination = a.data();
        Api::enqueue(small_queue, s);
        Api::enqueue(small_queue, s);  // the second of four submits by itself
        check(std::memcmp(a.data(), plain.data(), 300) == 0, "a queue submits by itself at half its capacity");
        Api::enqueue_status(small_queue, array, 2);
        check(Api::result(array, 2) == kPending, "an entry enqueued alone waits for Submit");
        Api::close(small_queue);
        check(Api::result(array, 2) == kPending, "Close drops what was not submitted; its entry never completes");
        release(small_queue);
    }

    release(queue);
    release(array);
    release(file);
    DeleteFileW(path.c_str());
}

void test_bcpack_requests(Api& api) {
    std::printf("\n-- BCPack requests --\n");
    using bcpack_vectors::k_vectors;
    const auto& bc7 = k_vectors[bcpack_vectors::k_zlib_vector];      // BC7, strict
    const auto& lenient = k_vectors[2];                               // BC1, needs the provisional history
    const std::vector<uint8_t> packed = packed_of(bc7);
    const std::vector<uint8_t> zpacked = from_hex(bcpack_vectors::k_zlib_packed);
    const std::vector<uint8_t> lpacked = packed_of(lenient);
    // The file: a zlib-compressed BCPack stream at offset 7.
    std::vector<uint8_t> contents(7, 0x5a);
    contents.insert(contents.end(), zpacked.begin(), zpacked.end());
    const std::wstring path = temp_file(contents);

    void *file = nullptr, *array = nullptr, *queue = nullptr;
    api.open(path.c_str(), &file);
    api.status_array(4, &array);
    api.queue(64, &queue);
    check(file && array && queue, "a file, a status array and a queue for BCPack requests");
    if (!file || !array || !queue) return;

    std::vector<uint8_t> from_memory(bc7.out_size, 0xcd), from_file(bc7.out_size, 0xcd), approx(lenient.out_size, 0xcd);
    Request m{};
    m.options = kMemorySource | (7ull << 1);
    m.destination = from_memory.data();
    m.destination_size = static_cast<uint32_t>(bc7.out_size);
    m.source = packed.data();
    m.source_size = static_cast<uint32_t>(packed.size());
    Api::enqueue(queue, m);
    Request z{};
    z.options = kZlib | (7ull << 1);
    z.destination = from_file.data();
    z.destination_size = static_cast<uint32_t>(bc7.out_size);
    z.source = file;
    z.offset = 7;
    z.source_size = static_cast<uint32_t>(zpacked.size());
    z.intermediate_size = static_cast<uint32_t>(packed.size() + 100);  // room to spare: the stream ends first
    Api::enqueue(queue, z);
    Request l = m;
    l.options = kMemorySource | (1ull << 1);
    l.destination = approx.data();
    l.destination_size = static_cast<uint32_t>(lenient.out_size);
    l.source = lpacked.data();
    l.source_size = static_cast<uint32_t>(lpacked.size());
    Api::enqueue(queue, l);
    Api::enqueue_status(queue, array, 0);
    Api::submit(queue);
    check(Api::result(array, 0) == S_OK, "BCPack requests from memory, from a zlib file chunk and on history succeed");
    check(matches(bc7, from_memory), "a BC7 BCPack request from memory decodes into its destination");
    check(matches(bc7, from_file), "zlib then BCPack: the inflated stream decodes into the destination");
    check(matches(lenient, approx), "a BC1 stream on provisional history entries decodes as tools/bcpack.py does");

    // A corrupt stream fails with ERROR_INVALID_DATA, and so does a zlib stream larger than intermediate_size.
    std::vector<uint8_t> bad(packed);
    bad[packed.size() / 2] ^= 0x5a;
    Request c = m;
    c.source = bad.data();
    Api::enqueue(queue, c);
    Api::enqueue_status(queue, array, 1);
    Request small = z;
    small.intermediate_size = static_cast<uint32_t>(packed.size() / 2);
    Api::enqueue(queue, small);
    Api::enqueue_status(queue, array, 2);
    Api::submit(queue);
    check(Api::result(array, 1) == static_cast<HRESULT>(0x8007000Du), "a corrupt BCPack stream is ERROR_INVALID_DATA");
    check(Api::result(array, 2) == static_cast<HRESULT>(0x8007000Du),
          "a zlib stream that inflates past intermediate_size is ERROR_INVALID_DATA");

    release(queue);
    release(array);
    release(file);
    DeleteFileW(path.c_str());
}

using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using XMemAllocatePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, uint64_t*, uint64_t*);
using XMemMapPhysicalPagesFn = void*(WINAPI*)(void*, uint64_t, const uint64_t*);
constexpr DWORD kXmem64kPages = 0x20400000;
constexpr SIZE_T kPage = 0x10000;

void test_page_destination(Api& api) {
    std::printf("\n-- physical page destinations --\n");
    const auto valloc = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto allocate =
        reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto map = reinterpret_cast<XMemMapPhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    check(valloc && allocate && map, "the xmem shim's physical-page functions are registered");
    if (!valloc || !allocate || !map) return;
    uint64_t pages[2] = {};
    uint64_t count = 2;
    check(allocate(0x280, &count, pages) == TRUE && count == 2, "two physical pages");
    // Written in reverse order, so the request's page list is not the allocation order.
    const uint64_t order[2] = {pages[1], pages[0]};

    void *array = nullptr, *queue = nullptr;
    api.status_array(1, &array);
    api.queue(16, &queue);
    if (!array || !queue) return;
    const std::vector<uint8_t> want = make_input(k_long_size);  // 40000 bytes: crosses into the second page
    constexpr uint64_t kOffset = 0xc000;
    Request r{};
    r.options = kPageDestination | kMemorySource | kZlib;
    r.destination = const_cast<uint64_t*>(order);
    r.page_offset = kOffset;
    r.destination_size = k_long_size;
    r.source = k_long_z;
    r.source_size = sizeof(k_long_z);
    Api::enqueue(queue, r);
    Api::enqueue_status(queue, array, 0);
    Api::submit(queue);
    check(Api::result(array, 0) == S_OK, "a zlib request into physical pages succeeds");

    auto* window = static_cast<uint8_t*>(valloc(nullptr, 2 * kPage, MEM_RESERVE | kXmem64kPages, 0x23, PAGE_READWRITE));
    check(window && map(window, 2, order) == window, "the pages map in the request's order");
    if (window) {
        check(std::memcmp(window + kOffset, want.data(), want.size()) == 0,
              "the bytes start at the page offset and continue into the next page in list order");
    }

    // A BCPack request into the same pages, starting just before the page boundary.
    const auto& v = bcpack_vectors::k_vectors[bcpack_vectors::k_zlib_vector];
    const std::vector<uint8_t> packed = packed_of(v);
    const uint64_t at = kPage - 100;
    Request b{};
    b.options = kPageDestination | kMemorySource | (7ull << 1);
    b.destination = const_cast<uint64_t*>(order);
    b.page_offset = at;
    b.destination_size = static_cast<uint32_t>(v.out_size);
    b.source = packed.data();
    b.source_size = static_cast<uint32_t>(packed.size());
    Api::enqueue(queue, b);
    Api::enqueue_status(queue, array, 0);
    Api::submit(queue);
    check(Api::result(array, 0) == S_OK, "a BCPack request into physical pages succeeds");
    if (window) {
        check(matches(v, std::vector<uint8_t>(window + at, window + at + v.out_size)),
              "the BCPack blocks land across the page boundary");
    }
    release(queue);
    release(array);
}

}  // namespace

int main() {
    test_inflate();
    test_bcpack();
    Api api;
    test_factory(api);
    if (api.factory) {
        test_requests(api);
        test_bcpack_requests(api);
        test_page_destination(api);
    }
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
