#pragma once
// Small TLS client (OpenSSL) for talking to printers on the local network: Bambu Lab printers
// speak MQTT on port 8883 and FTPS on port 990, both over TLS with their own certificate.
#include <memory>
#include <string>

namespace kst::net {

class TlsConn {
public:
    TlsConn();
    ~TlsConn();
    TlsConn(const TlsConn&) = delete;
    TlsConn& operator=(const TlsConn&) = delete;

    // Connects and does the TLS handshake. Printers use self signed certificates, so the
    // certificate is not checked (the connection stays on your own network). `reuse` resumes the
    // TLS session of another connection (FTPS data connections need that).
    bool connect(const std::string& host, int port, int timeoutMs, std::string& err, const TlsConn* reuse = nullptr);
    bool writeAll(const std::string& data);
    // Reads what is available, waiting up to timeoutMs. Returns bytes read, 0 on timeout, -1 on
    // closed or error.
    int read(char* buf, int len, int timeoutMs);
    // Reads one line ending in \n (FTP replies). Empty on timeout.
    std::string readLine(int timeoutMs);
    void close();
    bool open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Plain TCP check: true when something answers on host:port within the timeout.
bool canReach(const std::string& host, int port, int timeoutMs);

} // namespace kst::net
