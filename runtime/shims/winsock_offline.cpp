// winsock_offline: Winsock for a mapped title, loopback only.
//
// game_scarlett_master.exe 1.0.2.1 imports 25 functions from ws2_32.dll (23 by ordinal, plus getaddrinfo and
// freeaddrinfo; docs/runtime_imports.md), which the loader used to bind to the host's, so a --run could open real
// connections. ivall/xcl (GPL-2.0-only; facts only) records that this title's crash reporter sends a minidump to an
// outside server at every crash (HANDOFF.md, "Retail game bring-up"). Nothing here goes online (CONTRIBUTING.md), so
// every way the imported set can reach another machine is refused, and loopback keeps working for in-process use:
//   - connect and sendto to an address that is not loopback (127.0.0.0/8, ::1, ::ffff:127.0.0.0/104) fail with
//     WSAENETUNREACH before the host sees them, so no packet leaves;
//   - bind to the any-address is bound to loopback instead, and bind to another non-loopback address fails, so
//     nothing listens on the network;
//   - getaddrinfo and gethostbyname resolve only "localhost" and numeric loopback addresses; any other name fails
//     with WSAHOST_NOT_FOUND without a DNS query;
//   - the other 20 imported functions are the host's.
// Each refused destination or name is logged once ("[rt] winsock: offline, refused ..."). The module is owned by the
// shims (library_loader.cpp), so a run-time GetProcAddress on ws2_32.dll gets these names only: a function outside
// the imported set (ConnectEx, WSAConnect, ...) is not served, which fails closed.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "shims.h"

namespace {

HMODULE host_ws2() {
    static const HMODULE h = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return h;
}

void* host(const char* name) { return host_ws2() ? reinterpret_cast<void*>(GetProcAddress(host_ws2(), name)) : nullptr; }

template <typename F>
F host_fn(const char* name) {
    return reinterpret_cast<F>(host(name));
}

void set_error(int error) {
    if (auto f = host_fn<void(WSAAPI*)(int)>("WSASetLastError")) f(error);
}

bool loopback(const sockaddr* a, int len) {
    if (!a) return false;
    if (a->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in))) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(a);
        return (reinterpret_cast<const unsigned char*>(&in->sin_addr)[0]) == 127;
    }
    if (a->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6))) {
        const unsigned char* b = reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr.u.Byte;
        static constexpr unsigned char kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        static constexpr unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        return std::memcmp(b, kLoop, 16) == 0 || (std::memcmp(b, kMapped, 12) == 0 && b[12] == 127);
    }
    return false;
}

bool any_address(const sockaddr* a, int len) {
    if (!a) return false;
    if (a->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in)))
        return reinterpret_cast<const sockaddr_in*>(a)->sin_addr.s_addr == 0;
    if (a->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6))) {
        static constexpr unsigned char kAny[16] = {};
        return std::memcmp(reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr.u.Byte, kAny, 16) == 0;
    }
    return false;
}

std::string describe(const sockaddr* a, int len) {
    char text[INET6_ADDRSTRLEN + 16] = "?";
    if (!a) return "(null)";
    auto ntop = host_fn<PCSTR(WSAAPI*)(INT, const VOID*, PSTR, size_t)>("inet_ntop");
    if (a->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in)) && ntop) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(a);
        ntop(AF_INET, &in->sin_addr, text, sizeof(text));
        return std::string(text) + ":" + std::to_string(_byteswap_ushort(in->sin_port));
    }
    if (a->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6)) && ntop) {
        const auto* in = reinterpret_cast<const sockaddr_in6*>(a);
        ntop(AF_INET6, &in->sin6_addr, text, sizeof(text));
        return "[" + std::string(text) + "]:" + std::to_string(_byteswap_ushort(in->sin6_port));
    }
    return "family " + std::to_string(a->sa_family);
}

void refused(const char* call, const std::string& what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::lock_guard lock(mutex);
    if (seen.size() < 256 && seen.insert(std::string(call) + " " + what).second)
        std::fprintf(stderr, "[rt] winsock: offline, refused %s %s\n", call, what.c_str());
}

// A name that resolves without the network: "localhost" or a numeric loopback address.
bool loopback_name(const char* node) {
    if (!node) return true;  // a null node is the local any/loopback address (getaddrinfo semantics)
    if (_stricmp(node, "localhost") == 0) return true;
    auto pton = host_fn<INT(WSAAPI*)(INT, PCSTR, PVOID)>("inet_pton");
    if (!pton) return false;
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    if (pton(AF_INET, node, &v4.sin_addr) == 1) return loopback(reinterpret_cast<sockaddr*>(&v4), sizeof(v4));
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    if (pton(AF_INET6, node, &v6.sin6_addr) == 1) return loopback(reinterpret_cast<sockaddr*>(&v6), sizeof(v6));
    return false;
}

int WSAAPI shim_connect(SOCKET s, const sockaddr* name, int len) {
    if (!loopback(name, len)) {
        refused("connect", describe(name, len));
        set_error(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return host_fn<int(WSAAPI*)(SOCKET, const sockaddr*, int)>("connect")(s, name, len);
}

int WSAAPI shim_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int to_len) {
    if (to && !loopback(to, to_len)) {
        refused("sendto", describe(to, to_len));
        set_error(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return host_fn<int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int)>("sendto")(s, buf, len, flags, to,
                                                                                              to_len);
}

int WSAAPI shim_bind(SOCKET s, const sockaddr* name, int len) {
    auto bind = host_fn<int(WSAAPI*)(SOCKET, const sockaddr*, int)>("bind");
    if (any_address(name, len)) {
        // Listen on loopback only, at the port the title asked for.
        sockaddr_storage local{};
        std::memcpy(&local, name, static_cast<size_t>(len) < sizeof(local) ? static_cast<size_t>(len) : sizeof(local));
        if (local.ss_family == AF_INET)
            reinterpret_cast<sockaddr_in*>(&local)->sin_addr.s_addr = _byteswap_ulong(INADDR_LOOPBACK);
        else
            reinterpret_cast<sockaddr_in6*>(&local)->sin6_addr.u.Byte[15] = 1;
        return bind(s, reinterpret_cast<const sockaddr*>(&local), len);
    }
    if (name && !loopback(name, len)) {
        refused("bind", describe(name, len));
        set_error(WSAEADDRNOTAVAIL);
        return SOCKET_ERROR;
    }
    return bind(s, name, len);
}

INT WSAAPI shim_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
    if (result) *result = nullptr;
    if (!loopback_name(node)) {
        refused("getaddrinfo", node);
        set_error(WSAHOST_NOT_FOUND);
        return WSAHOST_NOT_FOUND;
    }
    return host_fn<INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*)>("getaddrinfo")(node, service, hints,
                                                                                            result);
}

hostent* WSAAPI shim_gethostbyname(const char* name) {
    if (!name || !loopback_name(name)) {
        refused("gethostbyname", name ? name : "(null)");
        set_error(WSAHOST_NOT_FOUND);
        return nullptr;
    }
    return host_fn<hostent*(WSAAPI*)(const char*)>("gethostbyname")(name);
}

// The ordinals of ws2_32.dll's classic exports, as the title imports them.
#define WS_SHIM(ordinal, name, fn) \
    {"ws2_32.dll", "#" #ordinal, reinterpret_cast<void*>(fn)}, {"ws2_32.dll", name, reinterpret_cast<void*>(fn)}
#define WS_HOST(ordinal, name) WS_SHIM(ordinal, name, host(name))

}  // namespace

SHIM_TABLE(winsock_offline,
    WS_SHIM(2, "bind", &shim_bind),
    WS_SHIM(4, "connect", &shim_connect),
    WS_SHIM(20, "sendto", &shim_sendto),
    WS_SHIM(52, "gethostbyname", &shim_gethostbyname),
    {"ws2_32.dll", "getaddrinfo", reinterpret_cast<void*>(&shim_getaddrinfo)},
    {"ws2_32.dll", "freeaddrinfo", host("freeaddrinfo")},
    WS_HOST(1, "accept"),
    WS_HOST(3, "closesocket"),
    WS_HOST(6, "getsockname"),
    WS_HOST(7, "getsockopt"),
    WS_HOST(9, "htons"),
    WS_HOST(10, "ioctlsocket"),
    WS_HOST(11, "inet_addr"),
    WS_HOST(13, "listen"),
    WS_HOST(16, "recv"),
    WS_HOST(17, "recvfrom"),
    WS_HOST(18, "select"),
    WS_HOST(19, "send"),
    WS_HOST(21, "setsockopt"),
    WS_HOST(22, "shutdown"),
    WS_HOST(23, "socket"),
    WS_HOST(57, "gethostname"),
    WS_HOST(111, "WSAGetLastError"),
    WS_HOST(115, "WSAStartup"),
    WS_HOST(151, "__WSAFDIsSet"),
);
