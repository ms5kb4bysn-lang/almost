// winsock_offline (shims/winsock_offline.cpp): the title's ws2_32 imports reach loopback only. No packet leaves the
// machine in this test: every non-loopback destination is refused before the host's Winsock is called.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../shims/shims.h"

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

template <typename F>
F shim(const char* name) {
    return reinterpret_cast<F>(rt::shims::find("ws2_32.dll", name));
}

}  // namespace

int main() {
    std::printf("-- the imported set --\n");
    static constexpr struct {
        int ordinal;
        const char* name;
        bool gated;
    } kImports[] = {{1, "accept", false},       {2, "bind", true},          {3, "closesocket", false},
                    {4, "connect", true},       {6, "getsockname", false},  {7, "getsockopt", false},
                    {9, "htons", false},        {10, "ioctlsocket", false}, {11, "inet_addr", false},
                    {13, "listen", false},      {16, "recv", false},        {17, "recvfrom", false},
                    {18, "select", false},      {19, "send", false},        {20, "sendto", true},
                    {21, "setsockopt", false},  {22, "shutdown", false},    {23, "socket", false},
                    {52, "gethostbyname", true}, {57, "gethostname", false}, {111, "WSAGetLastError", false},
                    {115, "WSAStartup", false}, {151, "__WSAFDIsSet", false}};
    const HMODULE ws2 = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bool ordinals = ws2 != nullptr, served = true, host_ones = true;
    for (const auto& i : kImports) {
        const std::string ord = "#" + std::to_string(i.ordinal);
        void* by_ordinal = rt::shims::find("ws2_32.dll", ord);
        void* by_name = rt::shims::find("ws2_32.dll", i.name);
        served &= by_ordinal && by_ordinal == by_name;
        // The table's ordinals name the same functions as in the host's ws2_32.dll.
        ordinals &= ws2 && GetProcAddress(ws2, MAKEINTRESOURCEA(i.ordinal)) == GetProcAddress(ws2, i.name);
        if (!i.gated) host_ones &= by_name == reinterpret_cast<void*>(GetProcAddress(ws2, i.name));
    }
    check(served, "all 23 imported ordinals are served, by ordinal and by name alike");
    check(ordinals, "each ordinal in the table is that function in the host's ws2_32.dll");
    check(host_ones, "the functions that cannot reach another machine are the host's");
    check(rt::shims::find("ws2_32.dll", "getaddrinfo") && rt::shims::find("ws2_32.dll", "freeaddrinfo"),
          "getaddrinfo and freeaddrinfo are served");
    check(rt::shims::owns_dll("ws2_32.dll"), "the shims own ws2_32.dll (GetProcAddress serves only this set)");
    check(!rt::shims::find("ws2_32.dll", "WSAConnect"), "a function outside the imported set is not served");

    auto startup = shim<int(WSAAPI*)(WORD, WSADATA*)>("WSAStartup");
    auto last_error = shim<int(WSAAPI*)()>("WSAGetLastError");
    auto make = shim<SOCKET(WSAAPI*)(int, int, int)>("socket");
    auto close = shim<int(WSAAPI*)(SOCKET)>("closesocket");
    auto do_bind = shim<int(WSAAPI*)(SOCKET, const sockaddr*, int)>("bind");
    auto do_connect = shim<int(WSAAPI*)(SOCKET, const sockaddr*, int)>("connect");
    auto do_listen = shim<int(WSAAPI*)(SOCKET, int)>("listen");
    auto sockname = shim<int(WSAAPI*)(SOCKET, sockaddr*, int*)>("getsockname");
    auto do_sendto = shim<int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int)>("sendto");
    auto resolve = shim<INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*)>("getaddrinfo");
    auto free_info = shim<void(WSAAPI*)(PADDRINFOA)>("freeaddrinfo");
    auto by_name = shim<hostent*(WSAAPI*)(const char*)>("gethostbyname");
    WSADATA data{};
    if (!startup || startup(MAKEWORD(2, 2), &data) != 0) {
        check(false, "WSAStartup");
        return 1;
    }

    std::printf("\n-- outbound --\n");
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);
    // TEST-NET-1 (RFC 5737): not routed, so even a broken shim would send nothing anywhere.
    inet_pton(AF_INET, "192.0.2.1", &remote.sin_addr);
    SOCKET tcp = make(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(do_connect(tcp, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == SOCKET_ERROR &&
              last_error() == WSAENETUNREACH,
          "connect to a public address fails with WSAENETUNREACH");
    close(tcp);
    sockaddr_in6 remote6{};
    remote6.sin6_family = AF_INET6;
    remote6.sin6_port = htons(443);
    inet_pton(AF_INET6, "2001:db8::1", &remote6.sin6_addr);
    SOCKET tcp6 = make(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    check(tcp6 == INVALID_SOCKET ||
              (do_connect(tcp6, reinterpret_cast<sockaddr*>(&remote6), sizeof(remote6)) == SOCKET_ERROR &&
               last_error() == WSAENETUNREACH),
          "connect to a public IPv6 address fails with WSAENETUNREACH");
    if (tcp6 != INVALID_SOCKET) close(tcp6);
    SOCKET udp = make(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check(do_sendto(udp, "x", 1, 0, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == SOCKET_ERROR &&
              last_error() == WSAENETUNREACH,
          "sendto a public address fails with WSAENETUNREACH");

    std::printf("\n-- names --\n");
    ADDRINFOA* info = reinterpret_cast<ADDRINFOA*>(1);
    check(resolve("example.com", "80", nullptr, &info) == WSAHOST_NOT_FOUND && info == nullptr,
          "getaddrinfo of a public name fails with WSAHOST_NOT_FOUND and no result");
    check(resolve("10.0.0.1", "80", nullptr, &info) == WSAHOST_NOT_FOUND, "a numeric non-loopback address is refused");
    info = nullptr;
    check(resolve("localhost", "80", nullptr, &info) == 0 && info, "getaddrinfo(\"localhost\") resolves");
    if (info) free_info(info);
    info = nullptr;
    check(resolve("127.0.0.1", "80", nullptr, &info) == 0 && info, "getaddrinfo(\"127.0.0.1\") resolves");
    if (info) free_info(info);
    check(by_name("example.com") == nullptr && last_error() == WSAHOST_NOT_FOUND,
          "gethostbyname of a public name fails with WSAHOST_NOT_FOUND");

    std::printf("\n-- local --\n");
    SOCKET server = make(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in any{};
    any.sin_family = AF_INET;  // INADDR_ANY, port 0
    check(do_bind(server, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0, "bind to the any-address succeeds");
    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    sockname(server, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    check(bound.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && bound.sin_port != 0,
          "... and is bound to loopback, not every interface");
    check(do_listen(server, 1) == 0, "listen on it");
    SOCKET client = make(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(do_connect(client, reinterpret_cast<sockaddr*>(&bound), bound_len) == 0, "connect to loopback works");
    close(client);
    close(server);
    sockaddr_in lan{};
    lan.sin_family = AF_INET;
    inet_pton(AF_INET, "192.168.1.50", &lan.sin_addr);
    SOCKET other = make(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check(do_bind(other, reinterpret_cast<sockaddr*>(&lan), sizeof(lan)) == SOCKET_ERROR &&
              last_error() == WSAEADDRNOTAVAIL,
          "bind to a non-loopback address fails with WSAEADDRNOTAVAIL");
    close(other);
    sockaddr_in loop{};
    loop.sin_family = AF_INET;
    loop.sin_port = htons(9);
    inet_pton(AF_INET, "127.0.0.1", &loop.sin_addr);
    check(do_sendto(udp, "x", 1, 0, reinterpret_cast<sockaddr*>(&loop), sizeof(loop)) == 1,
          "sendto loopback goes to the host");
    close(udp);

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
