#pragma once
// Plain TCP on this computer only (127.0.0.1), for Kestral's MCP server and its stdio bridge.
// Kept apart from everything else so the Windows socket headers stay in one file.
#include <cstddef>
#include <cstdint>
#include <string>

namespace kst::sock {

using Sock = intptr_t;
constexpr Sock kBad = -1;

bool init();                                   // once per process (Windows: WSAStartup)
Sock listenLocal(int port);                    // 127.0.0.1:port, kBad when taken
Sock acceptWait(Sock listener, int timeoutMs);  // kBad on timeout or error
Sock connectLocal(int port, int timeoutMs);    // kBad when nothing answers
// Bytes read (> 0), 0 on timeout, -1 when closed or on error.
int recvSome(Sock s, char* buf, int len, int timeoutMs);
bool sendAll(Sock s, const char* data, size_t len);
inline bool sendAll(Sock s, const std::string& d) { return sendAll(s, d.data(), d.size()); }
void close(Sock s);

bool processAlive(int pid);
int selfPid();
std::string randomHex(int bytes);   // from the system's secure random source

// The bridge's stdin and stdout (binary, no newline translation).
bool readLine(std::string& line);    // false at end of input
void writeOut(const std::string& s);  // whole write, flushed, safe from several threads

} // namespace kst::sock
