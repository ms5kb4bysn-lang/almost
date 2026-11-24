// The DirectStorage shim (shims/xcrdapi.cpp) and its inflate (shims/inflate.cpp), without game data.
//
// The objects are driven through their method tables at the slots the title calls (see xcrdapi.cpp): the factory's
// OpenFile, CreateStatusArray, SetStagingBufferSize and CreateQueue; the queue's EnqueueRequest, EnqueueStatus, Submit
// and Close; the status array's IsComplete and GetHResult. Requests read a temporary file or memory, raw or zlib, into
// an address or into physical pages from the xmem shim. The zlib streams are Python zlib output over make_input(n).
#include "../shims/inflate.h"
#include "../shims/shims.h"
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
    bc.options = kMemorySource | (7ull << 1);  // BCPack to BC7: not implemented yet
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
    check(Api::result(array, 2) == E_NOTIMPL, "a BCPack request fails its entry with E_NOTIMPL");
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
    release(queue);
    release(array);
}

}  // namespace

int main() {
    test_inflate();
    Api api;
    test_factory(api);
    if (api.factory) {
        test_requests(api);
        test_page_destination(api);
    }
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
