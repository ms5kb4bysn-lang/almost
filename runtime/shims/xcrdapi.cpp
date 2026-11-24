// The console's DirectStorage (xcrdapi.dll): DStorageGetFactory, and the factory, file, status-array and queue objects
// it hands out.
//
// The surface is what game_scarlett_master.exe 1.0.2.1 calls, measured at its call sites:
//   - DStorageGetFactory(&IID 62959d11-d45f-4884-a0a3-f7862ea243ae, &factory) at exe+0x1d8e4f0; the title breaks
//     (int3, exe+0x1d8e505) when it fails or leaves the factory null, then calls slot 8 (+0x40) with 0x2000000 and
//     breaks again on a failure.
//   - factory +0x20 (path, &IID 43704f9f-..., &file) after GetFileAttributesW on the same path, so the path is UTF-16
//     (exe+0x28c8c1e, exe+0x28c9d54); +0x28 (1, "fiDeviceDirectStorage", &IID cd66d752-..., &array) (exe+0x28c9522);
//     +0x50 (&desc, &IID 3c02cb0c-..., &queue) with a 0x20-byte description (exe+0x28c95c7, int3 on failure); +0x10
//     (Release) at shutdown (exe+0x28cb203).
// The queue and status-array slots, the 0x48-byte request and its option bits follow Marvisak/VIctory's
// crates/victory-hle/src/modules/dstorage.rs (GPL-3.0; facts only, see docs/related_projects.md), which reads them
// from the same title's call sites: queue +0x18 EnqueueRequest, +0x20 EnqueueStatus, +0x30 Submit, +0x40 Close; status
// array +0x18 IsComplete, +0x20 GetHResult. Slots nobody calls log their offset and caller once and return E_NOTIMPL.
//
// This first version runs a queue's requests on the thread that submits them, in order. Each request reads its
// source (a file opened through the GAME:/overlay shim, or memory), inflates zlib data, and writes the destination (an
// address, or 64 KiB physical pages through rt::xmem). BCPack-compressed requests are refused with E_NOTIMPL and
// counted: their decoder is the next increment.
#include "inflate.h"
#include "shims.h"
#include "xmem.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr GUID kFactoryIid{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID kFileIid{0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID kStatusArrayIid{0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID kQueueIid{0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
constexpr GUID kUnknownIid{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr HRESULT kPending = static_cast<HRESULT>(0x8000000Au);       // E_PENDING
constexpr HRESULT kEndOfFile = static_cast<HRESULT>(0x80070026u);     // HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)
constexpr HRESULT kInvalidData = static_cast<HRESULT>(0x8007000Du);   // HRESULT_FROM_WIN32(ERROR_INVALID_DATA)

// Request options.
constexpr uint64_t kZlib = 1;                     // inflate a zlib (RFC 1950) stream
constexpr uint64_t kBcpackMask = 0xfull << 1;     // bits 1-4: BCPack-decode to this BC format (0: none)
constexpr uint64_t kPageDestination = 1ull << 21; // destination is an array of 64 KiB page numbers
constexpr uint64_t kMemorySource = 1ull << 22;    // source is an address, not a file object
constexpr uint64_t kKnownOptions = kZlib | kBcpackMask | kPageDestination | kMemorySource;
constexpr size_t kPage = 0x10000;

struct Request {
    uint64_t options;
    void* destination;     // an address, or with kPageDestination an array of page numbers
    uint64_t page_offset;  // with kPageDestination: the low 16 bits are the offset into the first page
    uint32_t destination_size;
    const void* source;    // a file object, or with kMemorySource an address
    uint64_t offset;
    uint32_t source_size;
    uint32_t intermediate_size;  // between zlib and BCPack
    uint64_t tag;
    const char* name;
};
static_assert(sizeof(Request) == 0x48, "the title's request is 0x48 bytes");

struct QueueDesc {
    uint64_t source_type;
    uint16_t capacity;
    int8_t priority;
    const char* name;
    uint64_t unknown;  // the title sets 1
};
static_assert(sizeof(QueueDesc) == 0x20, "the title's queue description is 0x20 bytes");

bool same(const GUID* a, const GUID& b) { return a && std::memcmp(a, &b, sizeof(GUID)) == 0; }

std::string guid_text(const GUID* g) {
    if (!g) return "null";
    char text[40];
    std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2,
                  g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
                  g->Data4[7]);
    return text;
}

std::string narrow(const wchar_t* w) {
    if (!w) return "(null)";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Logging: every line is prefixed, and repeats of one kind of failure are capped.
std::atomic<uint32_t> g_logged_failures{0};
constexpr uint32_t kFailureLogLimit = 64;

// Counters, printed at exit and by tests.
struct Stats {
    std::atomic<uint64_t> requests{0}, failed{0}, zlib{0}, bcpack_refused{0}, pages{0}, memory{0}, bytes_out{0};
} g_stats;

// ---------------------------------------------------------------------------------------------------------------------
// Objects: a method table first, then a reference count. The factory is a process-wide singleton.

struct Header {
    void* const* vtbl;
    std::atomic<ULONG> refs{1};
    const GUID* iid;
    const char* kind;
    void (*destroy)(Header*);
};

HRESULT STDMETHODCALLTYPE query_interface(Header* self, const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (same(iid, *self->iid) || same(iid, kUnknownIid)) {
        self->refs.fetch_add(1);
        *out = self;
        return S_OK;
    }
    std::fprintf(stderr, "[rt] xcrdapi: %s QueryInterface(%s): E_NOINTERFACE\n", self->kind, guid_text(iid).c_str());
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE add_ref(Header* self) { return self->refs.fetch_add(1) + 1; }

ULONG STDMETHODCALLTYPE release(Header* self) {
    const ULONG left = self->refs.fetch_sub(1) - 1;
    if (left == 0 && self->destroy) self->destroy(self);
    return left;
}

// A slot no measured call site uses: logged once per object kind and slot, with the caller.
template <int Kind, int Slot>
HRESULT STDMETHODCALLTYPE missing(Header* self) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xcrdapi: %s slot %d (+0x%x) is not implemented; called from %s\n",
                     self ? self->kind : "?", Slot, Slot * 8,
                     rt::shims::module_offset(reinterpret_cast<uint64_t>(_ReturnAddress())).c_str());
    return E_NOTIMPL;
}

template <typename F>
void* fn(F f) {
    return reinterpret_cast<void*>(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Files.

struct File {
    Header header;
    HANDLE handle;
    std::string path;
};

void destroy_file(Header* h) {
    auto* f = reinterpret_cast<File*>(h);
    CloseHandle(f->handle);
    delete f;
}

void* const kFileVtbl[8] = {fn(&query_interface), fn(&add_ref), fn(&release), fn(&missing<1, 3>),
                            fn(&missing<1, 4>),   fn(&missing<1, 5>), fn(&missing<1, 6>), fn(&missing<1, 7>)};

// ---------------------------------------------------------------------------------------------------------------------
// Status arrays.

struct StatusArray {
    Header header;
    std::vector<std::atomic<HRESULT>> entries;
    std::string name;
};

void destroy_status_array(Header* h) { delete reinterpret_cast<StatusArray*>(h); }

HRESULT entry_result(StatusArray* self, uint32_t index) {
    return index < self->entries.size() ? self->entries[index].load(std::memory_order_acquire) : E_INVALIDARG;
}

// The title polls entry 0 in a Sleep(1) loop (VIctory).
bool STDMETHODCALLTYPE status_is_complete(StatusArray* self, uint32_t index) {
    return entry_result(self, index) != kPending;
}

HRESULT STDMETHODCALLTYPE status_get_hresult(StatusArray* self, uint32_t index) { return entry_result(self, index); }

void* const kStatusArrayVtbl[8] = {fn(&query_interface),    fn(&add_ref),          fn(&release),
                                   fn(&status_is_complete), fn(&status_get_hresult), fn(&missing<2, 5>),
                                   fn(&missing<2, 6>),      fn(&missing<2, 7>)};

// ---------------------------------------------------------------------------------------------------------------------
// Executing a request.

HRESULT fail(const Request& r, HRESULT hr, const char* why) {
    g_stats.failed.fetch_add(1);
    if (g_logged_failures.fetch_add(1) < kFailureLogLimit)
        std::fprintf(stderr,
                     "[rt] xcrdapi: request failed (%s, 0x%08lx): options 0x%llx offset 0x%llx source %u destination "
                     "%u intermediate %u name %s\n",
                     why, static_cast<unsigned long>(hr), static_cast<unsigned long long>(r.options),
                     static_cast<unsigned long long>(r.offset), r.source_size, r.destination_size, r.intermediate_size,
                     r.name ? r.name : "");
    return hr;
}

HRESULT read_file(const File* file, uint64_t offset, uint8_t* out, size_t size) {
    size_t done = 0;
    while (done < size) {
        OVERLAPPED at{};
        const uint64_t pos = offset + done;
        at.Offset = static_cast<DWORD>(pos);
        at.OffsetHigh = static_cast<DWORD>(pos >> 32);
        const DWORD want = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(file->handle, out + done, want, &got, &at)) {
            const DWORD error = GetLastError();
            return error == ERROR_HANDLE_EOF ? kEndOfFile : HRESULT_FROM_WIN32(error);
        }
        if (got == 0) return kEndOfFile;
        done += got;
    }
    return S_OK;
}

// The source bytes: a view of the title's memory, or a buffer read from the file.
HRESULT read_source(const Request& r, std::vector<uint8_t>& buffer, const uint8_t*& bytes) {
    if (r.options & kMemorySource) {
        bytes = static_cast<const uint8_t*>(r.source);
        return S_OK;
    }
    buffer.resize(r.source_size);
    const HRESULT hr = read_file(static_cast<const File*>(r.source), r.offset, buffer.data(), buffer.size());
    bytes = buffer.data();
    return hr;
}

// Produces the destination's bytes with `fill(out, capacity, &produced)` and stores them: in place for an address,
// through rt::xmem for physical pages.
template <typename Fill>
HRESULT into_destination(const Request& r, size_t capacity, Fill fill) {
    if (!(r.options & kPageDestination)) return fill(static_cast<uint8_t*>(r.destination), capacity, nullptr);
    g_stats.pages.fetch_add(1);
    std::vector<uint8_t> data(capacity);
    size_t produced = 0;
    const HRESULT hr = fill(data.data(), capacity, &produced);
    if (FAILED(hr)) return hr;
    const size_t offset = static_cast<size_t>(r.page_offset & 0xffff);
    const size_t pages = (offset + produced + kPage - 1) / kPage;
    return rt::xmem::write_physical(static_cast<const uint64_t*>(r.destination), pages, offset, data.data(), produced)
               ? S_OK
               : E_INVALIDARG;
}

HRESULT execute(const Request& r) {
    g_stats.requests.fetch_add(1);
    if (r.options & ~kKnownOptions) return fail(r, E_NOTIMPL, "unsupported options");
    if (!r.source || !r.destination) return fail(r, E_INVALIDARG, "no source or destination");
    if (r.options & kMemorySource) g_stats.memory.fetch_add(1);
    const uint64_t bc = (r.options & kBcpackMask) >> 1;
    if (bc) {
        g_stats.bcpack_refused.fetch_add(1);
        return fail(r, E_NOTIMPL, "BCPack is not implemented yet");
    }
    if (!(r.options & kZlib)) {
        if (r.source_size > r.destination_size) return fail(r, E_INVALIDARG, "source larger than destination");
        const HRESULT hr = into_destination(r, r.source_size, [&](uint8_t* out, size_t size, size_t* produced) {
            HRESULT h = S_OK;
            if (r.options & kMemorySource) std::memcpy(out, r.source, size);
            else h = read_file(static_cast<const File*>(r.source), r.offset, out, size);
            if (produced) *produced = size;
            return h;
        });
        if (FAILED(hr)) return fail(r, hr, "copying");
        g_stats.bytes_out.fetch_add(r.source_size);
        return S_OK;
    }
    g_stats.zlib.fetch_add(1);
    std::vector<uint8_t> buffer;
    const uint8_t* source = nullptr;
    HRESULT hr = read_source(r, buffer, source);
    if (FAILED(hr)) return fail(r, hr, "reading the source");
    size_t out_size = 0;
    hr = into_destination(r, r.destination_size, [&](uint8_t* out, size_t capacity, size_t* produced) {
        const auto s = rt::inflate::zlib(source, r.source_size, out, capacity, &out_size);
        if (produced) *produced = out_size;
        // A stream that ends short of the destination is accepted (VIctory: the title's chunks fill it exactly).
        return s == rt::inflate::Status::ok ? S_OK : kInvalidData;
    });
    if (FAILED(hr)) return fail(r, hr, "zlib");
    g_stats.bytes_out.fetch_add(out_size);
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queues.

struct Command {
    bool is_status;
    Request request;
    StatusArray* array;  // holds a reference while queued
    uint32_t index;
};

struct Queue {
    Header header;
    std::mutex mutex;
    std::vector<Command> unsubmitted;
    uint16_t capacity;
    int8_t priority;
    std::string name;
    bool closed = false;
    HRESULT first_failure = S_OK;  // since the last status entry
};

void destroy_queue(Header* h);

// Runs the submitted commands in order, with the queue's mutex held. A status entry gets the first failure of the
// requests before it (since the previous entry), or S_OK.
void run(Queue* q) {
    std::vector<Command> commands;
    commands.swap(q->unsubmitted);
    for (Command& c : commands) {
        if (!c.is_status) {
            const HRESULT hr = execute(c.request);
            if (FAILED(hr) && SUCCEEDED(q->first_failure)) q->first_failure = hr;
            continue;
        }
        c.array->entries[c.index].store(q->first_failure, std::memory_order_release);
        q->first_failure = S_OK;
        release(&c.array->header);
    }
}

void enqueue(Queue* q, const Command& c) {
    const std::lock_guard lock(q->mutex);
    if (q->closed) {
        if (c.is_status) release(&c.array->header);
        return;
    }
    q->unsubmitted.push_back(c);
    // The console submits by itself once half the queue is enqueued (VIctory).
    if (q->unsubmitted.size() >= std::max<size_t>(q->capacity / 2, 1)) run(q);
}

void STDMETHODCALLTYPE queue_enqueue_request(Queue* q, const Request* request) {
    if (!request) return;
    enqueue(q, Command{false, *request, nullptr, 0});
}

void STDMETHODCALLTYPE queue_enqueue_status(Queue* q, StatusArray* array, uint32_t index) {
    if (!array) return;
    if (index >= array->entries.size()) {
        std::fprintf(stderr, "[rt] xcrdapi: status entry %u out of range (%zu)\n", index, array->entries.size());
        return;
    }
    array->entries[index].store(kPending, std::memory_order_release);
    add_ref(&array->header);
    enqueue(q, Command{true, {}, array, index});
}

void STDMETHODCALLTYPE queue_submit(Queue* q) {
    const std::lock_guard lock(q->mutex);
    run(q);
}

// Ends the queue: what was not submitted never completes. The title calls this, not Release (VIctory).
void STDMETHODCALLTYPE queue_close(Queue* q) {
    const std::lock_guard lock(q->mutex);
    q->closed = true;
    for (Command& c : q->unsubmitted)
        if (c.is_status) release(&c.array->header);
    q->unsubmitted.clear();
}

void destroy_queue(Header* h) {
    auto* q = reinterpret_cast<Queue*>(h);
    queue_close(q);
    delete q;
}

void* const kQueueVtbl[16] = {fn(&query_interface),   fn(&add_ref),
                              fn(&release),           fn(&queue_enqueue_request),
                              fn(&queue_enqueue_status), fn(&missing<3, 5>),
                              fn(&queue_submit),      fn(&missing<3, 7>),
                              fn(&queue_close),       fn(&missing<3, 9>),
                              fn(&missing<3, 10>),    fn(&missing<3, 11>),
                              fn(&missing<3, 12>),    fn(&missing<3, 13>),
                              fn(&missing<3, 14>),    fn(&missing<3, 15>)};

// ---------------------------------------------------------------------------------------------------------------------
// The factory.

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

// The file-system shim's CreateFileW, so console paths (GAME:\...) and the overlay apply; the host's otherwise.
CreateFileWFn create_file() {
    static const CreateFileWFn f = [] {
        void* shim = rt::shims::find("kernel32.dll", "CreateFileW");
        return shim ? reinterpret_cast<CreateFileWFn>(shim) : &::CreateFileW;
    }();
    return f;
}

HRESULT STDMETHODCALLTYPE factory_open_file(Header*, const wchar_t* path, const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!path) return E_INVALIDARG;
    if (!same(iid, kFileIid) && !same(iid, kUnknownIid))
        std::fprintf(stderr, "[rt] xcrdapi: OpenFile with interface %s\n", guid_text(iid).c_str());
    const HANDLE h = create_file()(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "[rt] xcrdapi: OpenFile(%s) failed: %lu\n", narrow(path).c_str(), error);
        return HRESULT_FROM_WIN32(error);
    }
    auto* f = new File{{kFileVtbl, {1}, &kFileIid, "file", &destroy_file}, h, narrow(path)};
    *out = f;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_create_status_array(Header*, uint32_t capacity, const char* name, const GUID* iid,
                                                      void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!same(iid, kStatusArrayIid) && !same(iid, kUnknownIid))
        std::fprintf(stderr, "[rt] xcrdapi: CreateStatusArray with interface %s\n", guid_text(iid).c_str());
    auto* a = new StatusArray{{kStatusArrayVtbl, {1}, &kStatusArrayIid, "status array", &destroy_status_array},
                              std::vector<std::atomic<HRESULT>>(capacity),
                              name ? name : ""};
    for (auto& e : a->entries) e.store(S_OK);
    *out = a;
    return S_OK;
}

// The title asks for 32 MiB and breaks if this fails. Requests run without a staging buffer here.
HRESULT STDMETHODCALLTYPE factory_set_staging_buffer_size(Header*, uint32_t size) {
    std::fprintf(stderr, "[rt] xcrdapi: staging buffer size %u\n", size);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_create_queue(Header*, const QueueDesc* desc, const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!same(iid, kQueueIid) && !same(iid, kUnknownIid))
        std::fprintf(stderr, "[rt] xcrdapi: CreateQueue with interface %s\n", guid_text(iid).c_str());
    auto* q = new Queue{{kQueueVtbl, {1}, &kQueueIid, "queue", &destroy_queue}, {}, {}, desc->capacity,
                        desc->priority, desc->name ? desc->name : ""};
    std::fprintf(stderr, "[rt] xcrdapi: queue \"%s\": source type %llu, capacity %u, priority %d, unknown %llu\n",
                 q->name.c_str(), static_cast<unsigned long long>(desc->source_type), desc->capacity, desc->priority,
                 static_cast<unsigned long long>(desc->unknown));
    *out = q;
    return S_OK;
}

void* const kFactoryVtbl[16] = {fn(&query_interface),
                                fn(&add_ref),
                                fn(&release),
                                fn(&missing<0, 3>),
                                fn(&factory_open_file),
                                fn(&factory_create_status_array),
                                fn(&missing<0, 6>),
                                fn(&missing<0, 7>),
                                fn(&factory_set_staging_buffer_size),
                                fn(&missing<0, 9>),
                                fn(&factory_create_queue),
                                fn(&missing<0, 11>),
                                fn(&missing<0, 12>),
                                fn(&missing<0, 13>),
                                fn(&missing<0, 14>),
                                fn(&missing<0, 15>)};

Header g_factory{kFactoryVtbl, {1}, &kFactoryIid, "factory", nullptr};  // never destroyed

struct ExitSummary {
    ~ExitSummary() {
        if (!g_stats.requests.load()) return;
        std::fprintf(stderr,
                     "[rt] xcrdapi: %llu requests (%llu zlib, %llu from memory, %llu into pages), %llu failed (%llu "
                     "BCPack refused), %llu bytes written\n",
                     static_cast<unsigned long long>(g_stats.requests.load()),
                     static_cast<unsigned long long>(g_stats.zlib.load()),
                     static_cast<unsigned long long>(g_stats.memory.load()),
                     static_cast<unsigned long long>(g_stats.pages.load()),
                     static_cast<unsigned long long>(g_stats.failed.load()),
                     static_cast<unsigned long long>(g_stats.bcpack_refused.load()),
                     static_cast<unsigned long long>(g_stats.bytes_out.load()));
    }
} g_exit_summary;

HRESULT WINAPI DStorageGetFactory(const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!same(iid, kFactoryIid) && !same(iid, kUnknownIid)) {
        std::fprintf(stderr, "[rt] xcrdapi: DStorageGetFactory(%s): E_NOINTERFACE\n", guid_text(iid).c_str());
        return E_NOINTERFACE;
    }
    *out = &g_factory;
    return S_OK;
}

}  // namespace

SHIM_TABLE(xcrdapi,
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&DStorageGetFactory)},
);
