#include "print/Zip.h"
#include "core/Types.h"

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace kst::print {

bool readZip(const std::string& path, std::vector<ZipEntry>& out, std::string& error) {
    out.clear();
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) { error = "Could not open " + path; return false; }
    std::string z((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (z.size() < 22) { error = "Not a zip file"; return false; }
    auto u16 = [&](size_t o) { return (uint32_t)(uint8_t)z[o] | (uint32_t)(uint8_t)z[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22;; --i) {
        if (u32(i) == 0x06054b50) { eocd = i; break; }
        if (i == 0 || z.size() - i > 70000) break;
    }
    if (eocd == std::string::npos) { error = "Not a zip file"; return false; }
    size_t count = u16(eocd + 10), p = u32(eocd + 16);
    for (size_t k = 0; k < count; ++k) {
        if (p + 46 > z.size() || u32(p) != 0x02014b50) { error = "The zip file is damaged"; return false; }
        uint32_t method = u16(p + 10), csize = u32(p + 20), usize = u32(p + 24);
        size_t nlen = u16(p + 28), xlen = u16(p + 30), clen = u16(p + 32);
        uint32_t local = u32(p + 42);
        ZipEntry e;
        e.name = z.substr(p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (local + 30 > z.size()) { error = "The zip file is damaged"; return false; }
        size_t data = local + 30 + u16(local + 26) + u16(local + 28);
        if (data + csize > z.size()) { error = "The zip file is damaged"; return false; }
        if (method == 0) {
            e.data = z.substr(data, csize);
        } else if (method == 8) {
            e.data.resize(usize);
            z_stream s{};
            s.next_in = (Bytef*)&z[data];
            s.avail_in = csize;
            s.next_out = (Bytef*)e.data.data();
            s.avail_out = usize;
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { error = "Could not unpack " + e.name; return false; }
            int rc = inflate(&s, Z_FINISH);
            inflateEnd(&s);
            if (rc != Z_STREAM_END || s.total_out != usize) { error = "Could not unpack " + e.name; return false; }
        } else {
            error = "Unsupported packing in " + e.name;
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

bool writeZip(const std::string& path, const std::vector<ZipEntry>& entries, std::string& error) {
    std::string out, cd;
    auto put16 = [](std::string& s, uint32_t v) { s.push_back((char)(v & 255)); s.push_back((char)((v >> 8) & 255)); };
    auto put32 = [&](std::string& s, uint32_t v) { put16(s, v & 0xFFFF); put16(s, v >> 16); };
    for (auto& e : entries) {
        if (e.data.size() > 0xFFFFFFF0u) { error = e.name + " is too big for a zip file"; return false; }
        uint32_t crc = (uint32_t)crc32(0L, (const Bytef*)e.data.data(), (uInt)e.data.size());
        std::string packed;
        uint32_t method = 0;
        if (e.data.size() > 64) {
            z_stream s{};
            if (deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                packed.resize(deflateBound(&s, (uLong)e.data.size()));
                s.next_in = (Bytef*)e.data.data();
                s.avail_in = (uInt)e.data.size();
                s.next_out = (Bytef*)packed.data();
                s.avail_out = (uInt)packed.size();
                int rc = deflate(&s, Z_FINISH);
                packed.resize(s.total_out);
                deflateEnd(&s);
                if (rc == Z_STREAM_END && packed.size() < e.data.size()) method = 8;
            }
        }
        const std::string& body = method == 8 ? packed : e.data;
        uint32_t offset = (uint32_t)out.size();
        // Local header
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, method); put16(out, 0); put16(out, 0x21);
        put32(out, crc); put32(out, (uint32_t)body.size()); put32(out, (uint32_t)e.data.size());
        put16(out, (uint32_t)e.name.size()); put16(out, 0);
        out += e.name;
        out += body;
        // Central directory entry
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, method); put16(cd, 0); put16(cd, 0x21);
        put32(cd, crc); put32(cd, (uint32_t)body.size()); put32(cd, (uint32_t)e.data.size());
        put16(cd, (uint32_t)e.name.size()); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0);
        put32(cd, offset);
        cd += e.name;
    }
    uint32_t cdStart = (uint32_t)out.size();
    out += cd;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, (uint32_t)entries.size()); put16(out, (uint32_t)entries.size());
    put32(out, (uint32_t)cd.size()); put32(out, cdStart); put16(out, 0);
    std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
    f.write(out.data(), (std::streamsize)out.size());
    if (!f) { error = "Could not write " + path; return false; }
    return true;
}

} // namespace kst::print
