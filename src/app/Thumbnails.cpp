#include "app/Thumbnails.h"
#include "platform/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace kst::thumb {

// ===================================================================== software render

std::vector<uint8_t> render(const std::vector<Item>& items, int size) {
    const int S = size * 2;   // drawn at twice the size, then averaged down: smooth edges
    const glm::vec3 eye = glm::normalize(glm::vec3(1.f, -1.f, 0.8f));   // same as the home view
    const glm::vec3 right = glm::normalize(glm::cross(-eye, glm::vec3(0, 0, 1)));
    const glm::vec3 up = glm::cross(right, -eye);

    // Fit: project every vertex once to find the extents.
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, d0 = 1e30f, d1 = -1e30f;
    for (auto& it : items) {
        if (!it.mesh) continue;
        const auto& v = it.mesh->verts;
        for (size_t i = 0; i + 5 < v.size(); i += 6) {
            glm::vec3 p = glm::vec3(it.xform * glm::vec4(v[i], v[i + 1], v[i + 2], 1.f));
            float px = glm::dot(p, right), py = glm::dot(p, up), pd = glm::dot(p, eye);
            x0 = std::min(x0, px); x1 = std::max(x1, px);
            y0 = std::min(y0, py); y1 = std::max(y1, py);
            d0 = std::min(d0, pd); d1 = std::max(d1, pd);
        }
    }
    if (x0 > x1) return {};
    float span = std::max({x1 - x0, y1 - y0, 1e-6f});
    float scale = S * 0.88f / span;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;

    std::vector<float> depth((size_t)S * S, -1e30f);
    std::vector<glm::vec3> colorBuf((size_t)S * S, glm::vec3(0));
    const glm::vec3 light = glm::normalize(eye * 0.7f + up * 0.6f - right * 0.35f);

    for (auto& it : items) {
        if (!it.mesh) continue;
        const MeshData& m = *it.mesh;
        const auto& v = m.verts;
        glm::mat3 nm = glm::mat3(it.xform);
        size_t nv = m.vertexCount();
        std::vector<glm::vec3> sp(nv), sn(nv);
        for (size_t i = 0; i < nv; ++i) {
            glm::vec3 p = glm::vec3(it.xform * glm::vec4(v[i * 6], v[i * 6 + 1], v[i * 6 + 2], 1.f));
            sp[i] = glm::vec3((glm::dot(p, right) - cx) * scale + S * 0.5f, S * 0.5f - (glm::dot(p, up) - cy) * scale, glm::dot(p, eye));
            glm::vec3 n = nm * glm::vec3(v[i * 6 + 3], v[i * 6 + 4], v[i * 6 + 5]);
            float l = glm::length(n);
            sn[i] = l > 1e-8f ? n / l : glm::vec3(0, 0, 1);
        }
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
            glm::vec3 base = it.color;
            size_t tri = t / 3;
            if (tri < m.triFace.size() && m.triFace[tri] < m.faceColors.size()) {
                uint32_t fc = m.faceColors[m.triFace[tri]];
                if (fc >> 24) base = glm::vec3((fc & 255) / 255.f, ((fc >> 8) & 255) / 255.f, ((fc >> 16) & 255) / 255.f);
            }
            const glm::vec3 &a = sp[ia], &b = sp[ib], &c = sp[ic];
            float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::abs(area) < 1e-9f) continue;
            int minX = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x})));
            int maxX = std::min(S - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
            int minY = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y})));
            int maxY = std::min(S - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
            float inv = 1.f / area;
            for (int y = minY; y <= maxY; ++y) {
                float py = y + 0.5f;
                for (int x = minX; x <= maxX; ++x) {
                    float px = x + 0.5f;
                    float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
                    float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
                    float w2 = 1.f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float d = w0 * a.z + w1 * b.z + w2 * c.z;
                    size_t k = (size_t)y * S + x;
                    if (d <= depth[k]) continue;
                    depth[k] = d;
                    glm::vec3 n = glm::normalize(w0 * sn[ia] + w1 * sn[ib] + w2 * sn[ic] + glm::vec3(1e-9f));
                    if (glm::dot(n, eye) < 0) n = -n;   // seen from behind (open meshes)
                    float diff = std::max(0.f, glm::dot(n, light));
                    float rim = std::pow(1.f - std::max(0.f, glm::dot(n, eye)), 3.f) * 0.15f;
                    float spec = std::pow(std::max(0.f, glm::dot(glm::reflect(-light, n), eye)), 24.f) * 0.25f;
                    colorBuf[k] = glm::clamp(base * (0.38f + 0.62f * diff) + glm::vec3(spec + rim), 0.f, 1.f);
                }
            }
        }
    }

    // Outline where the depth jumps (silhouettes and steps), like a drawing.
    float dr = std::max(d1 - d0, 1e-6f);
    for (int y = 1; y < S - 1; ++y)
        for (int x = 1; x < S - 1; ++x) {
            size_t k = (size_t)y * S + x;
            if (depth[k] < -1e29f) continue;
            float jump = 0;
            for (size_t nk : {k - 1, k + 1, k - S, k + S}) {
                if (depth[nk] < -1e29f) { jump = 1; break; }
                jump = std::max(jump, (depth[k] - depth[nk]) / dr);
            }
            if (jump > 0.02f) colorBuf[k] *= 0.62f;
        }

    std::vector<uint8_t> out((size_t)size * size * 4, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            glm::vec3 sum(0);
            int cov = 0;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    size_t k = (size_t)(y * 2 + j) * S + (x * 2 + i);
                    if (depth[k] > -1e29f) { sum += colorBuf[k]; ++cov; }
                }
            uint8_t* o = &out[((size_t)y * size + x) * 4];
            if (cov) {
                sum /= (float)cov;
                o[0] = (uint8_t)std::lround(sum.x * 255.f);
                o[1] = (uint8_t)std::lround(sum.y * 255.f);
                o[2] = (uint8_t)std::lround(sum.z * 255.f);
                o[3] = (uint8_t)(cov * 255 / 4);
            }
        }
    return out;
}

// ===================================================================== files

std::string pathFor(const std::string& designPath) {
    std::string k = designPath;
    for (auto& c : k) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        c = (char)std::tolower((unsigned char)c);
#endif
    }
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : k) { h ^= c; h *= 1099511628211ull; }
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.png", (unsigned long long)h);
    static const std::string dir = [] {
        std::string d = platform::appDataDir() + "/thumbs";
        std::error_code ec;
        std::filesystem::create_directories(u8path(d), ec);
        return d;
    }();
    return dir + "/" + name;
}

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v);
}
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
    put32(o, (uint32_t)data.size());
    size_t start = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    put32(o, crc32(&o[start], o.size() - start));
}

} // namespace

bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    if (w <= 0 || h <= 0 || rgba.size() != (size_t)w * h * 4) return false;
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);   // no filter
        raw.insert(raw.end(), rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4);
    }
    // zlib stream of uncompressed blocks (a preview is small; keeps the reader trivial).
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, b << 16 | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});   // 8 bit RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write((const char*)png.data(), (std::streamsize)png.size());
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(u8path(tmp), u8path(path), ec);
    return !ec;
}

bool readPng(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 33 || std::memcmp(d.data() + 1, "PNG", 3) != 0) return false;
    std::vector<uint8_t> z;
    size_t p = 8;
    w = h = 0;
    while (p + 12 <= d.size()) {
        uint32_t len = get32(&d[p]);
        if (p + 12 + len > d.size()) return false;
        const char* type = (const char*)&d[p + 4];
        const uint8_t* body = &d[p + 8];
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)get32(body);
            h = (int)get32(body + 4);
            if (body[8] != 8 || body[9] != 6 || body[12] != 0) return false;
        } else if (!std::memcmp(type, "IDAT", 4)) {
            z.insert(z.end(), body, body + len);
        }
        p += 12 + len;
    }
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || z.size() < 2) return false;
    std::vector<uint8_t> raw;
    size_t q = 2;
    for (;;) {
        if (q + 5 > z.size()) return false;
        uint8_t hdr = z[q];
        if ((hdr >> 1) & 3) return false;   // compressed: not one of ours
        size_t n = z[q + 1] | (size_t)z[q + 2] << 8;
        q += 5;
        if (q + n > z.size()) return false;
        raw.insert(raw.end(), z.begin() + q, z.begin() + q + n);
        q += n;
        if (hdr & 1) break;
    }
    if (raw.size() < (size_t)(w * 4 + 1) * h) return false;
    rgba.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &raw[(size_t)y * (w * 4 + 1)];
        if (row[0] != 0) return false;
        std::memcpy(&rgba[(size_t)y * w * 4], row + 1, (size_t)w * 4);
    }
    return true;
}

} // namespace kst::thumb
