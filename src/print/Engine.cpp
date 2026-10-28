#include "print/Engine.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Types.h"
#include "platform/Platform.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace kst::print {

namespace fs = std::filesystem;
using json::Value;

namespace {

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool writeText(const fs::path& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << t;
    return (bool)f;
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    return s.substr(a);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// engine.json in a version folder: {"version": "...", "exe": "relative path"}.
Engine readEngineDir(const fs::path& dir) {
    Engine e;
    Value v = Value::parse(readText(dir / "engine.json"));
    if (!v.isObject()) return e;
    fs::path exe = dir / u8path(v["exe"].asString());
    std::error_code ec;
    if (v["exe"].asString().empty() || !fs::exists(exe, ec)) return e;
    e.exe = pathToU8(exe);
    e.version = v["version"].asString();
    e.managed = true;
    return e;
}

fs::path findFile(const fs::path& root, const std::string& name) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && lower(pathToU8(it->path().filename())) == lower(name)) return it->path();
    }
    return {};
}

// SHA-256 (small, self contained: used once per download).
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t n = 0;
    unsigned long long len = 0;
    static uint32_t rotr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const unsigned char* p, size_t k) {
        len += k;
        while (k) {
            size_t take = std::min(k, 64 - n);
            std::memcpy(buf + n, p, take);
            n += take; p += take; k -= take;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    std::string hex() {
        unsigned long long bits = len * 8;
        unsigned char one = 0x80, zero = 0;
        add(&one, 1);
        while (n != 56) add(&zero, 1);
        unsigned char lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
        add(lb, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

std::string sha256File(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    Sha256 s;
    std::vector<char> b(1 << 20);
    while (f) {
        f.read(b.data(), (std::streamsize)b.size());
        std::streamsize got = f.gcount();
        if (got > 0) s.add(reinterpret_cast<unsigned char*>(b.data()), (size_t)got);
    }
    return s.hex();
}

} // namespace

std::string engineRoot() {
#ifdef _WIN32
    return platform::appDataDir() + "/engine";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.local/share";
    return base + "/kestral/engine";
#endif
}

void applyPendingEngine() {
    fs::path root = u8path(engineRoot());
    std::string pend = trim(readText(root / "pending.txt"));
    if (pend.empty()) return;
    std::error_code ec;
    Engine e = readEngineDir(root / u8path(pend));
    if (e.exe.empty()) {
        log::warn("The downloaded OrcaSlicer " + pend + " is incomplete: keeping the current engine");
        fs::remove(root / "pending.txt", ec);
        return;
    }
    std::string cur = trim(readText(root / "current.txt"));
    if (!cur.empty() && cur != pend) writeText(root / "previous.txt", cur);
    writeText(root / "current.txt", pend);
    fs::remove(root / "pending.txt", ec);
    // Keep the new one and the one before it, nothing older.
    std::string prev = trim(readText(root / "previous.txt"));
    for (auto& d : fs::directory_iterator(root, ec)) {
        if (!d.is_directory(ec)) continue;
        std::string n = pathToU8(d.path().filename());
        if (n != pend && n != prev && n.rfind("download", 0) != 0) fs::remove_all(d.path(), ec);
    }
    log::info("Slicer engine is now OrcaSlicer " + pend);
}

Engine managedEngine() {
    fs::path root = u8path(engineRoot());
    std::string cur = trim(readText(root / "current.txt"));
    if (cur.empty()) return {};
    Engine e = readEngineDir(root / u8path(cur));
    if (e.exe.empty()) {
        std::string prev = trim(readText(root / "previous.txt"));
        if (!prev.empty()) e = readEngineDir(root / u8path(prev));
    }
    return e;
}

std::string pendingEngineVersion() {
    return trim(readText(u8path(engineRoot()) / "pending.txt"));
}

int compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v, std::string& tail) {
        std::vector<int> n;
        size_t i = (v.size() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
        while (i < v.size()) {
            if (!std::isdigit((unsigned char)v[i])) break;
            int x = 0;
            while (i < v.size() && std::isdigit((unsigned char)v[i])) x = x * 10 + (v[i++] - '0');
            n.push_back(x);
            if (i < v.size() && v[i] == '.') ++i;
            else break;
        }
        tail = i < v.size() ? v.substr(i) : std::string();
        return n;
    };
    std::string ta, tb;
    auto pa = parts(a, ta), pb = parts(b, tb);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (ta == tb) return 0;
    if (ta.empty()) return 1;   // 2.5.0 is newer than 2.5.0-beta
    if (tb.empty()) return -1;
    return ta < tb ? -1 : 1;
}

std::string engineVersionOf(const std::string& exe) {
    static std::mutex m;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lk(m);
        auto it = cache.find(exe);
        if (it != cache.end()) return it->second;
    }
    std::string v;
    if (!exe.empty() && exe.rfind("flatpak:", 0) != 0) v = platform::fileVersion(exe);   // Windows: Orca prints its help to its own console
    if (v.empty() && !exe.empty()) {
        platform::ProcResult pr = platform::runProcess(exe, {"--help"}, 30000);
        // First line: "OrcaSlicer-2.4.2:"
        size_t p = pr.output.find("OrcaSlicer-");
        if (p != std::string::npos) {
            size_t e = pr.output.find_first_of(":\r\n ", p);
            v = pr.output.substr(p + 11, e == std::string::npos ? std::string::npos : e - p - 11);
        }
    }
    std::lock_guard<std::mutex> lk(m);
    cache[exe] = v;
    return v;
}

EngineRelease latestEngineRelease(const std::atomic<bool>* cancel) {
    EngineRelease r;
    auto res = platform::httpGet("https://api.github.com/repos/SoftFever/OrcaSlicer/releases/latest",
                                 {{"Accept", "application/vnd.github+json"}, {"User-Agent", "Kestral"}}, 20000, cancel);
    if (!res.error.empty() || res.status != 200) {
        r.error = res.error.empty() ? "GitHub answered " + std::to_string(res.status) : res.error;
        return r;
    }
    Value v = Value::parse(res.body);
    r.tag = v["tag_name"].asString();
    r.version = r.tag.size() && (r.tag[0] == 'v' || r.tag[0] == 'V') ? r.tag.substr(1) : r.tag;
    r.notesUrl = v["html_url"].asString();
    // The portable Windows build, or the Linux AppImage (Ubuntu 24.04 build first).
    int best = -1;
    for (auto& a : v["assets"].items()) {
        std::string n = a["name"].asString(), ln = lower(n);
        int score = -1;
#ifdef _WIN32
        if (ln.find("windows") != std::string::npos && ln.size() > 12 && ln.rfind("portable.zip") == ln.size() - 12 &&
            ln.find("arm64") == std::string::npos && ln.find("validator") == std::string::npos)
            score = 1;
#else
        if (ln.find("linux") != std::string::npos && ln.find(".appimage") != std::string::npos && ln.find("aarch64") == std::string::npos &&
            ln.find("arm64") == std::string::npos && ln.find("validator") == std::string::npos)
            score = ln.find("ubuntu2404") != std::string::npos ? 2 : 1;
#endif
        if (score > best) {
            best = score;
            r.assetName = n;
            r.url = a["browser_download_url"].asString();
            r.size = (long long)a["size"].asNumber(0);
            std::string dg = a["digest"].asString();
            r.sha256 = dg.rfind("sha256:", 0) == 0 ? lower(dg.substr(7)) : std::string();
        }
    }
    if (r.url.empty()) r.error = "OrcaSlicer " + r.version + " has no download for this system";
    return r;
}

bool unzipAll(const std::string& zipPath, const std::string& destDir, std::string& error, const std::function<void(double)>& progress) {
    std::ifstream f(u8path(zipPath), std::ios::binary);
    if (!f) { error = "Could not open " + zipPath; return false; }
    std::string z((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (z.size() < 22) { error = "The download is not a zip file"; return false; }
    auto u16 = [&](size_t o) { return (uint32_t)(uint8_t)z[o] | (uint32_t)(uint8_t)z[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22;; --i) {
        if (u32(i) == 0x06054b50) { eocd = i; break; }
        if (i == 0 || z.size() - i > 70000) break;
    }
    if (eocd == std::string::npos) { error = "The download is not a zip file"; return false; }
    size_t count = u16(eocd + 10), p = u32(eocd + 16);
    fs::path dest = u8path(destDir);
    std::error_code ec;
    for (size_t k = 0; k < count; ++k) {
        if (p + 46 > z.size() || u32(p) != 0x02014b50) { error = "The zip file is damaged"; return false; }
        uint32_t method = u16(p + 10), csize = u32(p + 20), usize = u32(p + 24);
        size_t nlen = u16(p + 28), xlen = u16(p + 30), clen = u16(p + 32);
        uint32_t local = u32(p + 42);
        std::string name = z.substr(p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        for (auto& c : name) if (c == '\\') c = '/';
        if (name.empty() || name.find("..") != std::string::npos || name[0] == '/') continue;   // never outside the folder
        fs::path out = dest / u8path(name);
        if (name.back() == '/') { fs::create_directories(out, ec); continue; }
        fs::create_directories(out.parent_path(), ec);
        if (local + 30 > z.size()) { error = "The zip file is damaged"; return false; }
        size_t data = local + 30 + u16(local + 26) + u16(local + 28);
        if (data + csize > z.size()) { error = "The zip file is damaged"; return false; }
        std::string bytes;
        if (method == 0) {
            bytes = z.substr(data, csize);
        } else if (method == 8) {
            bytes.resize(usize);
            z_stream s{};
            s.next_in = (Bytef*)&z[data];
            s.avail_in = csize;
            s.next_out = (Bytef*)bytes.data();
            s.avail_out = usize;
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { error = "Could not unpack " + name; return false; }
            int rc = inflate(&s, Z_FINISH);
            inflateEnd(&s);
            if (rc != Z_STREAM_END || s.total_out != usize) { error = "Could not unpack " + name; return false; }
        } else {
            error = "Unsupported packing in " + name;
            return false;
        }
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        o.write(bytes.data(), (std::streamsize)bytes.size());
        if (!o) { error = "Could not write " + pathToU8(out); return false; }
        if (progress && k % 50 == 0) progress((double)k / (double)std::max<size_t>(1, count));
    }
    return true;
}

bool installEngine(const EngineRelease& r, const std::function<void(const std::string&, double)>& progress,
                   const std::atomic<bool>* cancel, std::string& error) {
    if (r.url.empty() || r.version.empty()) { error = "Nothing to install"; return false; }
    fs::path root = u8path(engineRoot());
    fs::path dl = root / "download";
    std::error_code ec;
    fs::remove_all(dl, ec);
    fs::create_directories(dl, ec);
    fs::path file = dl / u8path(r.assetName.empty() ? "orca.bin" : r.assetName);
    auto say = [&](const std::string& t, double f) { if (progress) progress(t, f); };
    say("Downloading OrcaSlicer " + r.version, 0);
    auto res = platform::httpDownload(r.url, pathToU8(file), 30 * 60 * 1000, cancel, [&](long long got, long long) {
        say("Downloading OrcaSlicer " + r.version, r.size > 0 ? (double)got / (double)r.size : -1);
    });
    if (cancel && cancel->load()) { error = "Stopped"; fs::remove_all(dl, ec); return false; }
    if (!res.error.empty()) { error = res.error; fs::remove_all(dl, ec); return false; }
    if (!r.sha256.empty()) {
        say("Checking the download", -1);
        if (sha256File(file) != r.sha256) { error = "The download is damaged (checksum does not match). Try again."; fs::remove_all(dl, ec); return false; }
    }
    fs::path target = root / u8path(r.version);
    fs::remove_all(target, ec);
    fs::create_directories(target, ec);
    fs::path exe;
#ifdef _WIN32
    say("Unpacking", 0);
    if (!unzipAll(pathToU8(file), pathToU8(target), error, [&](double f) { say("Unpacking", f); })) { fs::remove_all(target, ec); return false; }
    exe = findFile(target, "orca-slicer.exe");
#else
    say("Unpacking", -1);
    fs::permissions(file, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write, ec);
    platform::ProcResult pr = platform::runProcess(pathToU8(file), {"--appimage-extract"}, 10 * 60 * 1000, cancel, pathToU8(target));
    if (pr.exitCode != 0) { error = "Could not unpack the AppImage: " + pr.error; fs::remove_all(target, ec); return false; }
    exe = target / "squashfs-root" / "AppRun";
#endif
    fs::remove_all(dl, ec);
    if (exe.empty() || !fs::exists(exe, ec)) { error = "The download has no OrcaSlicer program in it"; fs::remove_all(target, ec); return false; }
    // Make sure it runs before it replaces anything.
    say("Checking that it runs", -1);
    platform::ProcResult run = platform::runProcess(pathToU8(exe), {"--help"}, 60000, cancel, pathToU8(target));
    if (run.timedOut || run.exitCode != 0) {
        error = "The new OrcaSlicer does not start on this computer (exit " + std::to_string(run.exitCode) + ")";
        fs::remove_all(target, ec);
        return false;
    }
    std::string got = engineVersionOf(pathToU8(exe));
    if (got.empty()) got = r.version;
    Value info = Value::object();
    info.set("version", Value(got));
    info.set("exe", Value(pathToU8(fs::relative(exe, target, ec))));
    info.set("from", Value(r.url));
    if (!writeText(target / "engine.json", info.dump()) || !writeText(root / "pending.txt", got)) {
        error = "Could not write to " + pathToU8(root);
        return false;
    }
    log::info("OrcaSlicer " + got + " installed for Kestral; it is used after a restart");
    return true;
}

} // namespace kst::print
