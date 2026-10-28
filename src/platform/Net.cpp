#include "platform/Net.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SockT = SOCKET;
#define KST_BAD_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
#define KST_BAD_SOCK (-1)
#define closesock ::close
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <chrono>
#include <cstring>
#include <mutex>

namespace kst::net {

namespace {

void initOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
        OPENSSL_init_ssl(0, nullptr);
    });
}

bool setBlocking(SockT s, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK)) == 0;
#endif
}

// Waits until the socket can be read (or written). False on timeout.
bool waitSock(SockT s, bool forWrite, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd = s;
    p.events = forWrite ? POLLOUT : POLLIN;
    return WSAPoll(&p, 1, timeoutMs) > 0;
#else
    pollfd p{};
    p.fd = s;
    p.events = forWrite ? POLLOUT : POLLIN;
    return poll(&p, 1, timeoutMs) > 0;
#endif
}

SockT connectTcp(const std::string& host, int port, int timeoutMs, std::string& err) {
    initOnce();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        err = "Could not find " + host + " on the network";
        return KST_BAD_SOCK;
    }
    SockT s = KST_BAD_SOCK;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == KST_BAD_SOCK) continue;
        setBlocking(s, false);
        int rc = ::connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool ok = rc == 0;
        if (!ok && waitSock(s, true, timeoutMs)) {
            int soErr = 0;
            socklen_t len = sizeof soErr;
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len);
            ok = soErr == 0;
        }
        if (ok) {
            setBlocking(s, true);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            break;
        }
        closesock(s);
        s = KST_BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == KST_BAD_SOCK) err = "Could not connect to " + host + ":" + std::to_string(port) + " (is the printer on and on this network?)";
    return s;
}

} // namespace

struct TlsConn::Impl {
    SockT sock = KST_BAD_SOCK;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    std::string pending;   // bytes read past a line end (readLine)
};

TlsConn::TlsConn() : d_(new Impl) {}
TlsConn::~TlsConn() { close(); }

bool TlsConn::open() const { return d_->ssl != nullptr; }

bool TlsConn::connect(const std::string& host, int port, int timeoutMs, std::string& err, const TlsConn* reuse) {
    close();
    d_->sock = connectTcp(host, port, timeoutMs, err);
    if (d_->sock == KST_BAD_SOCK) return false;
    d_->ctx = SSL_CTX_new(TLS_client_method());
    if (!d_->ctx) { err = "TLS is not available"; close(); return false; }
    SSL_CTX_set_verify(d_->ctx, SSL_VERIFY_NONE, nullptr);   // printers: self signed, local network
    SSL_CTX_set_session_cache_mode(d_->ctx, SSL_SESS_CACHE_CLIENT);
    d_->ssl = SSL_new(d_->ctx);
    SSL_set_fd(d_->ssl, (int)d_->sock);
    if (reuse && reuse->d_->ssl) {
        SSL_SESSION* sess = SSL_get1_session(reuse->d_->ssl);
        if (sess) { SSL_set_session(d_->ssl, sess); SSL_SESSION_free(sess); }
    }
    // Handshake with a time limit (blocking socket with a receive timeout).
#ifdef _WIN32
    DWORD tv = (DWORD)timeoutMs;
#else
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
#endif
    setsockopt(d_->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(d_->sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
    if (SSL_connect(d_->ssl) != 1) {
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
        err = "Secure connection to " + host + " failed (" + buf + ")";
        close();
        return false;
    }
    return true;
}

bool TlsConn::writeAll(const std::string& data) {
    if (!d_->ssl) return false;
    size_t off = 0;
    while (off < data.size()) {
        int n = SSL_write(d_->ssl, data.data() + off, (int)std::min<size_t>(data.size() - off, 1 << 20));
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

int TlsConn::read(char* buf, int len, int timeoutMs) {
    if (!d_->ssl) return -1;
    if (!d_->pending.empty()) {
        int n = (int)std::min<size_t>((size_t)len, d_->pending.size());
        std::memcpy(buf, d_->pending.data(), (size_t)n);
        d_->pending.erase(0, (size_t)n);
        return n;
    }
    if (SSL_pending(d_->ssl) == 0 && !waitSock(d_->sock, false, timeoutMs)) return 0;
    int n = SSL_read(d_->ssl, buf, len);
    if (n > 0) return n;
    int e = SSL_get_error(d_->ssl, n);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
    return -1;
}

std::string TlsConn::readLine(int timeoutMs) {
    auto t0 = std::chrono::steady_clock::now();
    std::string line;
    for (;;) {
        size_t nl = d_->pending.find('\n');
        if (nl != std::string::npos) {
            line = d_->pending.substr(0, nl + 1);
            d_->pending.erase(0, nl + 1);
            return line;
        }
        int left = timeoutMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (left <= 0) return {};
        char buf[1024];
        if (!d_->ssl) return {};
        if (SSL_pending(d_->ssl) == 0 && !waitSock(d_->sock, false, left)) return {};
        int n = SSL_read(d_->ssl, buf, sizeof buf);
        if (n <= 0) {
            int e = SSL_get_error(d_->ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return {};
        }
        d_->pending.append(buf, (size_t)n);
    }
}

void TlsConn::close() {
    if (d_->ssl) {
        SSL_shutdown(d_->ssl);
        SSL_free(d_->ssl);
        d_->ssl = nullptr;
    }
    if (d_->ctx) { SSL_CTX_free(d_->ctx); d_->ctx = nullptr; }
    if (d_->sock != KST_BAD_SOCK) { closesock(d_->sock); d_->sock = KST_BAD_SOCK; }
    d_->pending.clear();
}

bool canReach(const std::string& host, int port, int timeoutMs) {
    std::string err;
    SockT s = connectTcp(host, port, timeoutMs, err);
    if (s == KST_BAD_SOCK) return false;
    closesock(s);
    return true;
}

} // namespace kst::net
