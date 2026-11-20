#include "platform/Sock.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <algorithm>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kst::sock {

namespace {
#ifdef _WIN32
using RawSock = SOCKET;
inline RawSock raw(Sock s) { return (RawSock)s; }
bool waitFor(Sock s, bool write, int ms) {
    WSAPOLLFD p{};
    p.fd = raw(s);
    p.events = write ? POLLWRNORM : POLLRDNORM;
    int r = WSAPoll(&p, 1, ms);
    return r > 0;
}
#else
inline int raw(Sock s) { return (int)s; }
bool waitFor(Sock s, bool write, int ms) {
    pollfd p{};
    p.fd = raw(s);
    p.events = write ? POLLOUT : POLLIN;
    int r = ::poll(&p, 1, ms);
    return r > 0;
}
#endif
sockaddr_in loopback(int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}
} // namespace

bool init() {
#ifdef _WIN32
    static bool ok = [] {
        WSADATA w;
        return WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    return ok;
#else
    static bool ok = [] {
        signal(SIGPIPE, SIG_IGN);   // a client that hangs up must not end Kestral
        return true;
    }();
    return ok;
#endif
}

Sock listenLocal(int port) {
    init();
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return kBad;
    BOOL excl = TRUE;   // nobody else may share the port
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return kBad;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    sockaddr_in a = loopback(port);
    if (::bind(s, (sockaddr*)&a, sizeof a) != 0 || ::listen(s, 16) != 0) {
        close((Sock)s);
        return kBad;
    }
    return (Sock)s;
}

Sock acceptWait(Sock l, int timeoutMs) {
    if (l == kBad || !waitFor(l, false, timeoutMs)) return kBad;
#ifdef _WIN32
    SOCKET c = ::accept(raw(l), nullptr, nullptr);
    if (c == INVALID_SOCKET) return kBad;
#else
    int c = ::accept(raw(l), nullptr, nullptr);
    if (c < 0) return kBad;
    fcntl(c, F_SETFD, FD_CLOEXEC);
#endif
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    return (Sock)c;
}

Sock connectLocal(int port, int timeoutMs) {
    init();
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return kBad;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return kBad;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
    sockaddr_in a = loopback(port);
    int r = ::connect(s, (sockaddr*)&a, sizeof a);
    bool ok = r == 0;
    if (!ok && waitFor((Sock)s, true, timeoutMs)) {
        int e = 0;
#ifdef _WIN32
        int el = sizeof e;
#else
        socklen_t el = sizeof e;
#endif
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &el);
        ok = e == 0;
    }
    if (!ok) { close((Sock)s); return kBad; }
#ifdef _WIN32
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fl);
#endif
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    return (Sock)s;
}

int recvSome(Sock s, char* buf, int len, int timeoutMs) {
    if (s == kBad) return -1;
    if (!waitFor(s, false, timeoutMs)) return 0;
    int n = (int)::recv(raw(s), buf, len, 0);
    return n > 0 ? n : -1;
}

bool sendAll(Sock s, const char* d, size_t len) {
    if (s == kBad) return false;
    while (len > 0) {
#ifdef _WIN32
        int n = ::send(raw(s), d, (int)std::min<size_t>(len, 1 << 20), 0);
#else
        ssize_t n = ::send(raw(s), d, len, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        d += n;
        len -= (size_t)n;
    }
    return true;
}

void close(Sock s) {
    if (s == kBad) return;
#ifdef _WIN32
    ::closesocket(raw(s));
#else
    ::close(raw(s));
#endif
}

bool processAlive(int pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid, 0) == 0;
#endif
}

int selfPid() {
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)::getpid();
#endif
}

std::string randomHex(int bytes) {
    std::string raw((size_t)bytes, '\0');
    bool ok = false;
#ifdef _WIN32
    ok = BCryptGenRandom(nullptr, (PUCHAR)raw.data(), (ULONG)raw.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        ok = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
        std::fclose(f);
    }
#endif
    if (!ok) {
        std::random_device rd;
        for (auto& c : raw) c = (char)rd();
    }
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) { out += hx[c >> 4]; out += hx[c & 15]; }
    return out;
}

namespace {
std::mutex outMx;
void binaryStdio() {
    static bool done = [] {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        return true;
    }();
    (void)done;
}
} // namespace

bool readLine(std::string& line) {
    binaryStdio();
    line.clear();
    int c;
    while ((c = std::fgetc(stdin)) != EOF) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back((char)c);
    }
    return !line.empty();
}

void writeOut(const std::string& s) {
    binaryStdio();
    std::lock_guard<std::mutex> lk(outMx);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

} // namespace kst::sock
