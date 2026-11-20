// Illustrations of a design (see Illustration.h). Everything is drawn here in software: the parts
// shaded or flat, outlines and the model's edges as lines, balloons and text as filled outlines,
// at twice the size and averaged down so every edge is smooth.
#include "app/Illustration.h"
#include "sketch/Text.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace kst::illus {

namespace {

struct Rgb { float r, g, b; };

std::string defaultFont() {
#ifdef _WIN32
    return "Segoe UI";
#else
    return "DejaVu Sans";
#endif
}

void put32(std::string& v, uint32_t x) {
    v.push_back((char)(x >> 24)); v.push_back((char)(x >> 16)); v.push_back((char)(x >> 8)); v.push_back((char)x);
}

void chunk(std::string& png, const char* type, const std::string& data) {
    put32(png, (uint32_t)data.size());
    std::string td = std::string(type, 4) + data;
    png += td;
    put32(png, (uint32_t)crc32(0L, (const Bytef*)td.data(), (uInt)td.size()));
}

std::string encodePng(const std::vector<uint8_t>& rgba, int w, int h) {
    std::string raw;
    raw.reserve((size_t)h * (w * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(1);   // Sub filter
        const uint8_t* row = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w * 4; ++x) raw.push_back((char)(uint8_t)(row[x] - (x >= 4 ? row[x - 4] : 0)));
    }
    uLongf zn = compressBound((uLong)raw.size());
    std::string z(zn, '\0');
    if (compress2((Bytef*)z.data(), &zn, (const Bytef*)raw.data(), (uLong)raw.size(), 6) != Z_OK) return {};
    z.resize(zn);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);   // 8 bit RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += t[n & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16;
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += '=';
    }
    return out;
}

std::string xmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

std::string hex(Rgb c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", (int)std::lround(std::clamp(c.r, 0.f, 1.f) * 255), (int)std::lround(std::clamp(c.g, 0.f, 1.f) * 255),
                  (int)std::lround(std::clamp(c.b, 0.f, 1.f) * 255));
    return b;
}

// The picture being drawn, at the supersampled size: color, coverage, depth and which part.
struct Canvas {
    int w = 0, h = 0;
    std::vector<Rgb> col;
    std::vector<float> alpha;
    std::vector<float> depth;
    std::vector<int> part;
    void init(int W, int H, Rgb bg, float bgA) {
        w = W;
        h = H;
        col.assign((size_t)w * h, bg);
        alpha.assign((size_t)w * h, bgA);
        depth.assign((size_t)w * h, -1e30f);
        part.assign((size_t)w * h, -1);
    }
    void blend(int x, int y, Rgb c, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        size_t k = (size_t)y * w + x;
        Rgb& d = col[k];
        float da = alpha[k];
        float oa = a + da * (1 - a);
        if (oa <= 1e-6f) return;
        d.r = (c.r * a + d.r * da * (1 - a)) / oa;
        d.g = (c.g * a + d.g * da * (1 - a)) / oa;
        d.b = (c.b * a + d.b * da * (1 - a)) / oa;
        alpha[k] = oa;
    }
    void disk(float cx, float cy, float r, Rgb c) {
        int x0 = (int)std::floor(cx - r - 1), x1 = (int)std::ceil(cx + r + 1), y0 = (int)std::floor(cy - r - 1), y1 = (int)std::ceil(cy + r + 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                float a = std::clamp(r + 0.5f - d, 0.f, 1.f);
                if (a > 0) blend(x, y, c, a);
            }
    }
    void line(float ax, float ay, float bx, float by, float width, Rgb c) {
        float r = width * 0.5f;
        int x0 = (int)std::floor(std::min(ax, bx) - r - 1), x1 = (int)std::ceil(std::max(ax, bx) + r + 1);
        int y0 = (int)std::floor(std::min(ay, by) - r - 1), y1 = (int)std::ceil(std::max(ay, by) + r + 1);
        float dx = bx - ax, dy = by - ay, ll = dx * dx + dy * dy;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float px = x + 0.5f - ax, py = y + 0.5f - ay;
                float t = ll > 0 ? std::clamp((px * dx + py * dy) / ll, 0.f, 1.f) : 0.f;
                float d = std::hypot(px - dx * t, py - dy * t);
                float a = std::clamp(r + 0.5f - d, 0.f, 1.f);
                if (a > 0) blend(x, y, c, a);
            }
    }
    // Even-odd fill of closed loops (glyph outlines), 4 sub-rows per pixel for smooth edges.
    void fillLoops(const std::vector<std::vector<glm::vec2>>& loops, Rgb c) {
        float y0 = 1e30f, y1 = -1e30f, x0 = 1e30f, x1 = -1e30f;
        for (auto& l : loops)
            for (auto& p : l) { y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); }
        if (y0 > y1) return;
        int iy0 = std::max(0, (int)std::floor(y0)), iy1 = std::min(h - 1, (int)std::ceil(y1));
        int ix0 = std::max(0, (int)std::floor(x0)), ix1 = std::min(w - 1, (int)std::ceil(x1));
        if (ix0 > ix1) return;
        std::vector<float> cov((size_t)(ix1 - ix0 + 1));
        std::vector<float> xs;
        const int sub = 4;
        for (int y = iy0; y <= iy1; ++y) {
            std::fill(cov.begin(), cov.end(), 0.f);
            for (int s = 0; s < sub; ++s) {
                float sy = y + (s + 0.5f) / sub;
                xs.clear();
                for (auto& l : loops)
                    for (size_t i = 0; i < l.size(); ++i) {
                        const glm::vec2 &a = l[i], &b = l[(i + 1) % l.size()];
                        if ((a.y <= sy) != (b.y <= sy)) xs.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
                    }
                std::sort(xs.begin(), xs.end());
                for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                    float fa = std::max(xs[i], (float)ix0), fb = std::min(xs[i + 1], (float)ix1 + 1);
                    for (int x = (int)std::floor(fa); x < (int)std::ceil(fb); ++x) {
                        float c0 = std::max(fa, (float)x), c1 = std::min(fb, (float)x + 1);
                        if (c1 > c0 && x >= ix0 && x <= ix1) cov[(size_t)(x - ix0)] += (c1 - c0) / sub;
                    }
                }
            }
            for (int x = ix0; x <= ix1; ++x) blend(x, y, c, std::min(1.f, cov[(size_t)(x - ix0)]));
        }
    }
};

// Text as filled outlines. Returns its width; draw = false only measures. align: 0 left, 1 center.
float drawText(Canvas* cv, const std::string& font, const std::string& text, float x, float baseline, float height, Rgb c, int align) {
    auto loops = sk::textOutlines(font, text, height);
    float maxX = 0;
    for (auto& l : loops) for (auto& p : l) maxX = std::max(maxX, (float)p.x);
    if (text.find(' ') != std::string::npos && loops.empty()) maxX = 0;
    if (!cv) return maxX;
    float ox = align == 1 ? x - maxX * 0.5f : x;
    std::vector<std::vector<glm::vec2>> pl;
    pl.reserve(loops.size());
    for (auto& l : loops) {
        std::vector<glm::vec2> q;
        q.reserve(l.size());
        for (auto& p : l) q.push_back({ox + (float)p.x, baseline - (float)p.y});
        pl.push_back(std::move(q));
    }
    cv->fillLoops(pl, c);
    return maxX;
}

} // namespace

const char* viewName(View v) {
    switch (v) {
    case View::Iso: return "Isometric";
    case View::Front: return "Front";
    case View::Top: return "Top";
    case View::Right: return "Right";
    case View::Left: return "Left";
    case View::Back: return "Back";
    case View::Bottom: return "Bottom";
    case View::Current: return "Current view";
    }
    return "";
}

const char* styleName(Style s) {
    switch (s) {
    case Style::Shaded: return "Shaded";
    case Style::Toon: return "Illustration";
    case Style::LineArt: return "Line art";
    case Style::ShadedNoLines: return "Shaded, no lines";
    }
    return "";
}

bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::string png = encodePng(rgba, w, h);
    if (png.empty()) return false;
#ifdef _WIN32
    std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
#else
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
#endif
    if (!f) return false;
    f.write(png.data(), (std::streamsize)png.size());
    return (bool)f;
}

Result render(const std::vector<Part>& partsIn, const Options& o, bool wantSvg) {
    Result res;
    const int W = std::clamp(o.width, 64, 8192), H = std::clamp(o.height, 64, 8192);
    const int SS = (long long)W * H <= 2304000 ? 2 : 1;   // twice the size up to about 1920 x 1200
    const int w = W * SS, h = H * SS;
    const std::string font = o.font.empty() ? defaultFont() : o.font;
    res.width = W;
    res.height = H;

    // ---- view direction (eye: from the model toward the camera) and screen up
    glm::vec3 eye, upHint(0, 0, 1);
    switch (o.view) {
    case View::Front: eye = {0, -1, 0}; break;
    case View::Back: eye = {0, 1, 0}; break;
    case View::Right: eye = {1, 0, 0}; break;
    case View::Left: eye = {-1, 0, 0}; break;
    case View::Top: eye = {0, 0, 1}; upHint = {0, 1, 0}; break;
    case View::Bottom: eye = {0, 0, -1}; upHint = {0, 1, 0}; break;
    case View::Current: eye = -o.camFwd; upHint = o.camUp; break;
    default: eye = glm::normalize(glm::vec3(1, -1, 1)); break;   // true isometric, front right top
    }
    eye = glm::normalize(eye);
    glm::vec3 right = glm::cross(-eye, upHint);
    if (glm::length(right) < 1e-4f) right = glm::cross(-eye, glm::vec3(0, 1, 0));
    right = glm::normalize(right);
    const glm::vec3 up = glm::cross(right, -eye);

    // ---- exploded: each part moves away from the middle of the assembly
    std::vector<Part> parts = partsIn;
    {
        Aabb all;
        std::vector<glm::vec3> centers(parts.size());
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!parts[i].mesh) continue;
            Aabb b;
            const Aabb& mb = parts[i].mesh->bounds;
            if (mb.valid())
                for (int k = 0; k < 8; ++k)
                    b.add(glm::vec3(parts[i].xform * glm::vec4(k & 1 ? mb.max.x : mb.min.x, k & 2 ? mb.max.y : mb.min.y, k & 4 ? mb.max.z : mb.min.z, 1)));
            centers[i] = b.center();
            all.add(b);
        }
        if (o.explode > 0.001f && all.valid()) {
            glm::vec3 c = all.center();
            for (size_t i = 0; i < parts.size(); ++i)
                parts[i].xform = glm::translate(glm::mat4(1.f), (centers[i] - c) * o.explode) * parts[i].xform;
        }
    }

    // ---- colors
    const bool dark = o.background == Background::Dark;
    const Rgb bg = dark ? Rgb{0.13f, 0.14f, 0.16f} : Rgb{1, 1, 1};
    const float bgA = o.background == Background::Transparent ? 0.f : 1.f;
    const Rgb ink = dark && o.style == Style::LineArt ? Rgb{0.92f, 0.93f, 0.95f} : Rgb{0.08f, 0.09f, 0.1f};
    const Rgb accent{1.f, 0.42f, 0.21f};
    const bool lines = o.style != Style::ShadedNoLines;

    // ---- layout: the model fits its area (smaller when balloons go around it, the list takes the right side)
    const bool list = o.partsList && o.numbers;
    float areaX0 = 0, areaX1 = (float)w * (list ? 0.74f : 1.f), areaY0 = 0, areaY1 = (float)h;
    float fill = o.numbers ? 0.66f : 0.86f;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, d0 = 1e30f, d1 = -1e30f;
    for (auto& p : parts) {
        if (!p.mesh) continue;
        const auto& v = p.mesh->verts;
        for (size_t i = 0; i + 5 < v.size(); i += 6) {
            glm::vec3 q = glm::vec3(p.xform * glm::vec4(v[i], v[i + 1], v[i + 2], 1.f));
            float px = glm::dot(q, right), py = glm::dot(q, up), pd = glm::dot(q, eye);
            x0 = std::min(x0, px); x1 = std::max(x1, px);
            y0 = std::min(y0, py); y1 = std::max(y1, py);
            d0 = std::min(d0, pd); d1 = std::max(d1, pd);
        }
    }
    Canvas cv;
    cv.init(w, h, bg, bgA);
    if (x0 > x1) {   // nothing to draw: just the background
        res.rgba.assign((size_t)W * H * 4, 0);
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            res.rgba[i * 4] = (uint8_t)(bg.r * 255); res.rgba[i * 4 + 1] = (uint8_t)(bg.g * 255); res.rgba[i * 4 + 2] = (uint8_t)(bg.b * 255);
            res.rgba[i * 4 + 3] = (uint8_t)(bgA * 255);
        }
        return res;
    }
    float aw = areaX1 - areaX0, ah = areaY1 - areaY0;
    float scale = std::min(aw * fill / std::max(x1 - x0, 1e-6f), ah * fill / std::max(y1 - y0, 1e-6f));
    float cxw = (x0 + x1) * 0.5f, cyw = (y0 + y1) * 0.5f;
    float ocx = (areaX0 + areaX1) * 0.5f, ocy = (areaY0 + areaY1) * 0.5f;
    auto toScreen = [&](const glm::vec3& p) {
        return glm::vec3((glm::dot(p, right) - cxw) * scale + ocx, ocy - (glm::dot(p, up) - cyw) * scale, glm::dot(p, eye));
    };
    const float dr = std::max(d1 - d0, 1e-6f);

    // ---- faces
    const glm::vec3 light = glm::normalize(up * 0.75f + eye * 0.55f - right * 0.3f);
    for (int pi = 0; pi < (int)parts.size(); ++pi) {
        const Part& p = parts[(size_t)pi];
        if (!p.mesh) continue;
        const MeshData& m = *p.mesh;
        const auto& v = m.verts;
        glm::mat3 nm = glm::mat3(p.xform);
        size_t nv = m.vertexCount();
        std::vector<glm::vec3> sp(nv), sn(nv);
        for (size_t i = 0; i < nv; ++i) {
            sp[i] = toScreen(glm::vec3(p.xform * glm::vec4(v[i * 6], v[i * 6 + 1], v[i * 6 + 2], 1.f)));
            glm::vec3 n = nm * glm::vec3(v[i * 6 + 3], v[i * 6 + 4], v[i * 6 + 5]);
            float l = glm::length(n);
            sn[i] = l > 1e-8f ? n / l : glm::vec3(0, 0, 1);
        }
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
            glm::vec3 base = p.color;
            size_t tri = t / 3;
            if (tri < m.triFace.size() && m.triFace[tri] < m.faceColors.size()) {
                uint32_t fc = m.faceColors[m.triFace[tri]];
                if (fc >> 24) base = glm::vec3((fc & 255) / 255.f, ((fc >> 8) & 255) / 255.f, ((fc >> 16) & 255) / 255.f);
            }
            const glm::vec3 &a = sp[ia], &b = sp[ib], &c = sp[ic];
            float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::abs(area) < 1e-9f) continue;
            // Flat shading per triangle for the illustration styles: crisp facets read better.
            glm::vec3 fn = glm::normalize(sn[ia] + sn[ib] + sn[ic] + glm::vec3(1e-9f));
            int minX = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x})));
            int maxX = std::min(w - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
            int minY = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y})));
            int maxY = std::min(h - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
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
                    size_t k = (size_t)y * w + x;
                    if (d <= cv.depth[k]) continue;
                    cv.depth[k] = d;
                    cv.part[k] = pi;
                    Rgb out;
                    if (o.style == Style::LineArt) {
                        out = dark ? Rgb{0.18f, 0.19f, 0.21f} : Rgb{1, 1, 1};
                    } else if (o.style == Style::Toon) {
                        glm::vec3 n = glm::dot(fn, eye) < 0 ? -fn : fn;
                        float df = glm::dot(n, light);
                        float tone = df > 0.8f ? 1.0f : df > 0.45f ? 0.84f : df > 0.1f ? 0.7f : 0.6f;
                        glm::vec3 c3 = glm::clamp(base * tone + glm::vec3(0.04f), 0.f, 1.f);
                        out = {c3.x, c3.y, c3.z};
                    } else {
                        glm::vec3 n = glm::normalize(w0 * sn[ia] + w1 * sn[ib] + w2 * sn[ic] + glm::vec3(1e-9f));
                        if (glm::dot(n, eye) < 0) n = -n;
                        float df = std::max(0.f, glm::dot(n, light));
                        float spec = std::pow(std::max(0.f, glm::dot(glm::reflect(-light, n), eye)), 32.f) * 0.2f;
                        glm::vec3 c3 = glm::clamp(base * (0.38f + 0.62f * df) + glm::vec3(spec), 0.f, 1.f);
                        out = {c3.x, c3.y, c3.z};
                    }
                    cv.col[k] = out;
                    cv.alpha[k] = 1.f;
                }
            }
        }
    }
    // The faces alone (for the SVG, which draws its own lines on top).
    std::vector<Rgb> facesCol;
    std::vector<float> facesA;
    if (wantSvg && o.style != Style::LineArt) { facesCol = cv.col; facesA = cv.alpha; }

    // ---- lines: outlines around parts and where the depth jumps, then the model's edges
    const float pxOut = std::max(1.f, 2.4f * o.lineWeight * H / 1080.f), pxIn = std::max(0.8f, 1.2f * o.lineWeight * H / 1080.f);
    if (lines) {
        std::vector<uint8_t> edge((size_t)w * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t k = (size_t)y * w + x;
                if (cv.part[k] < 0) continue;
                auto differs = [&](int nx, int ny) {
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) return true;
                    size_t nk = (size_t)ny * w + nx;
                    if (cv.part[nk] != cv.part[k]) return true;
                    return std::abs(cv.depth[k] - cv.depth[nk]) / dr > 0.012f;
                };
                if (differs(x + 1, y) || differs(x - 1, y) || differs(x, y + 1) || differs(x, y - 1)) edge[k] = 1;
            }
        float r = pxOut * SS * 0.5f;
        int ri = (int)std::ceil(r);
        std::vector<float> mask((size_t)w * h, 0.f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!edge[(size_t)y * w + x]) continue;
                for (int dy = -ri; dy <= ri; ++dy)
                    for (int dx = -ri; dx <= ri; ++dx) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        float a = std::clamp(r + 0.5f - std::hypot((float)dx, (float)dy), 0.f, 1.f);
                        float& mk = mask[(size_t)yy * w + xx];
                        mk = std::max(mk, a);
                    }
            }
        for (size_t k = 0; k < mask.size(); ++k)
            if (mask[k] > 0) cv.blend((int)(k % w), (int)(k / w), ink, mask[k]);
        // B-rep edges where nothing covers them.
        float ri2 = pxIn * SS * 0.5f;
        for (auto& p : parts) {
            if (!p.mesh) continue;
            const auto& ev = p.mesh->edgeVerts;
            for (size_t i = 0; i + 5 < ev.size(); i += 6) {
                glm::vec3 a = toScreen(glm::vec3(p.xform * glm::vec4(ev[i], ev[i + 1], ev[i + 2], 1.f)));
                glm::vec3 b = toScreen(glm::vec3(p.xform * glm::vec4(ev[i + 3], ev[i + 4], ev[i + 5], 1.f)));
                float len = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                int n = std::max(1, (int)std::ceil(len));
                for (int s = 0; s <= n; ++s) {
                    float t = (float)s / (float)n;
                    float px = a.x + (b.x - a.x) * t, py = a.y + (b.y - a.y) * t, pd = a.z + (b.z - a.z) * t;
                    int xi = (int)px, yi = (int)py;
                    if (xi < 0 || yi < 0 || xi >= w || yi >= h) continue;
                    if (cv.depth[(size_t)yi * w + xi] > pd + dr * 0.004f) continue;
                    int rr = (int)std::ceil(ri2);
                    for (int dy = -rr; dy <= rr; ++dy)
                        for (int dx = -rr; dx <= rr; ++dx) {
                            float aa = std::clamp(ri2 + 0.5f - std::hypot(dx + 0.5f - (px - xi), dy + 0.5f - (py - yi)), 0.f, 1.f);
                            if (aa > 0) cv.blend(xi + dx, yi + dy, ink, aa * 0.9f);
                        }
                }
            }
        }
    }

    // ---- numbered balloons: one number per distinct part (copies of the same part share it)
    struct Balloon { int group; float ax, ay; float theta; float bx = 0, by = 0; };
    std::vector<Balloon> balloons;
    std::vector<std::string> groupName;
    std::vector<int> groupCount;
    float R = H * 0.021f * SS, dotR = std::max(1.5f, H * 0.0035f * SS);
    if (o.numbers) {
        std::map<const MeshData*, int> groupOf;
        std::vector<int> partGroup(parts.size(), -1);
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!parts[i].mesh) continue;
            auto it = groupOf.find(parts[i].mesh.get());
            if (it == groupOf.end()) {
                it = groupOf.emplace(parts[i].mesh.get(), (int)groupName.size()).first;
                std::string nm = parts[i].name;
                // "drop sensor 4:2" and "drop sensor 4:1" are the same part.
                size_t colon = nm.rfind(':');
                if (colon != std::string::npos && colon + 1 < nm.size() && std::isdigit((unsigned char)nm[colon + 1])) nm = nm.substr(0, colon);
                groupName.push_back(nm);
                groupCount.push_back(0);
            }
            partGroup[i] = it->second;
            groupCount[(size_t)it->second]++;
        }
        // Where each part shows most: the instance with the most visible pixels, at its visible middle.
        size_t np = parts.size();
        std::vector<double> sx(np, 0), sy(np, 0);
        std::vector<long long> cnt(np, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int pi = cv.part[(size_t)y * w + x];
                if (pi < 0) continue;
                sx[(size_t)pi] += x; sy[(size_t)pi] += y; cnt[(size_t)pi]++;
            }
        std::vector<int> best(groupName.size(), -1);
        for (size_t i = 0; i < np; ++i) {
            int g = partGroup[i];
            if (g < 0 || !cnt[i]) continue;
            if (best[(size_t)g] < 0 || cnt[i] > cnt[(size_t)best[(size_t)g]]) best[(size_t)g] = (int)i;
        }
        long long minPix = (long long)w * h / 4000;
        std::vector<float> cxs(np), cys(np), bestD(np, 1e30f);
        for (size_t i = 0; i < np; ++i)
            if (cnt[i]) { cxs[i] = (float)(sx[i] / cnt[i]); cys[i] = (float)(sy[i] / cnt[i]); }
        std::vector<float> axs(np), ays(np);
        // The visible pixel of the part nearest its middle (an L shaped part's middle may be in the air).
        for (int y = 0; y < h; y += 2)
            for (int x = 0; x < w; x += 2) {
                int pi = cv.part[(size_t)y * w + x];
                if (pi < 0) continue;
                float d = std::hypot(x - cxs[(size_t)pi], y - cys[(size_t)pi]);
                if (d < bestD[(size_t)pi]) { bestD[(size_t)pi] = d; axs[(size_t)pi] = (float)x; ays[(size_t)pi] = (float)y; }
            }
        float mx = ocx, my = ocy;
        for (size_t g = 0; g < groupName.size(); ++g) {
            int i = best[g];
            if (i < 0 || cnt[(size_t)i] < minPix) continue;
            Balloon b;
            b.group = (int)g;
            b.ax = axs[(size_t)i];
            b.ay = ays[(size_t)i];
            b.theta = std::atan2(b.ay - my, b.ax - mx);
            balloons.push_back(b);
        }
        // Around the model on an ellipse, in order around it, pushed apart so none overlap.
        float ex = (x1 - x0) * scale * 0.5f + R * 2.4f, ey = (y1 - y0) * scale * 0.5f + R * 2.4f;
        ex = std::min(ex, aw * 0.5f - R * 1.4f);
        ey = std::min(ey, ah * 0.5f - R * 1.4f);
        std::sort(balloons.begin(), balloons.end(), [](const Balloon& a, const Balloon& b) { return a.theta < b.theta; });
        int nb = (int)balloons.size();
        auto place = [&](Balloon& b) { b.bx = mx + std::cos(b.theta) * ex; b.by = my + std::sin(b.theta) * ey; };
        for (auto& b : balloons) place(b);
        for (int it = 0; it < 200 && nb > 1; ++it) {
            bool moved = false;
            for (int i = 0; i < nb; ++i) {
                Balloon& a = balloons[(size_t)i];
                Balloon& b = balloons[(size_t)((i + 1) % nb)];
                float d = std::hypot(b.bx - a.bx, b.by - a.by);
                if (d < R * 2.5f) {
                    float push = 0.004f + (R * 2.5f - d) / (ex + ey) * 0.25f;
                    a.theta -= push;
                    b.theta += push;
                    place(a);
                    place(b);
                    moved = true;
                }
            }
            if (!moved) break;
        }
        // Then each one moves out along its line from the middle until it sits clear of the parts.
        auto clear = [&](float cx, float cy) {
            for (int a = 0; a < 16; ++a)
                for (float rr : {R * 1.25f, R * 0.6f}) {
                    int x = (int)(cx + std::cos(a * 0.3927f) * rr), y = (int)(cy + std::sin(a * 0.3927f) * rr);
                    if (x >= 0 && y >= 0 && x < w && y < h && cv.part[(size_t)y * w + x] >= 0) return false;
                }
            return true;
        };
        for (auto& b : balloons) {
            float dx = b.bx - mx, dy = b.by - my, l = std::max(1e-3f, std::hypot(dx, dy));
            dx /= l;
            dy /= l;
            for (int step = 0; step < 60 && !clear(b.bx, b.by); ++step) {
                float nx = b.bx + dx * R * 0.25f, ny = b.by + dy * R * 0.25f;
                if (nx < areaX0 + R * 1.3f || nx > areaX1 - R * 1.3f || ny < areaY0 + R * 1.3f || ny > areaY1 - R * 1.3f) break;
                b.bx = nx;
                b.by = ny;
            }
        }
        // Numbers in order around the model, starting at the top left, clockwise.
        std::stable_sort(balloons.begin(), balloons.end(), [](const Balloon& a, const Balloon& b) {
            auto key = [](float t) { float k = t + 2.4f; while (k < 0) k += 6.2832f; while (k >= 6.2832f) k -= 6.2832f; return k; };
            return key(a.theta) < key(b.theta);
        });
        for (auto& b : balloons) {
            float dx = b.bx - b.ax, dy = b.by - b.ay, l = std::max(1e-3f, std::hypot(dx, dy));
            cv.line(b.ax, b.ay, b.bx - dx / l * R, b.by - dy / l * R, std::max(1.f, pxIn * SS * 0.9f), ink);
            cv.disk(b.ax, b.ay, dotR, ink);
        }
        int num = 0;
        for (auto& b : balloons) {
            ++num;
            cv.disk(b.bx, b.by, R, accent);
            cv.disk(b.bx, b.by, R - std::max(1.5f, R * 0.12f), Rgb{1, 1, 1});
            drawText(&cv, font, std::to_string(num), b.bx, b.by + R * 0.36f, R * 1.0f, Rgb{0.1f, 0.1f, 0.12f}, 1);
        }
        res.partsNumbered = num;
    }

    // ---- parts list on the right: number, name, how many
    float listX = areaX1 + w * 0.01f, listY = h * 0.08f, rowH = std::min(H * 0.042f * SS, (h * 0.86f) / std::max<size_t>(1, balloons.size() + 1));
    if (list && !balloons.empty()) {
        Rgb txt = dark ? Rgb{0.92f, 0.93f, 0.95f} : Rgb{0.1f, 0.1f, 0.12f};
        drawText(&cv, font, "Parts", listX, listY, rowH * 0.62f, txt, 0);
        int num = 0;
        for (auto& b : balloons) {
            ++num;
            float y = listY + rowH * (num + 0.3f);
            float rr = rowH * 0.36f;
            cv.disk(listX + rr, y - rr * 0.35f, rr, accent);
            cv.disk(listX + rr, y - rr * 0.35f, rr * 0.8f, Rgb{1, 1, 1});
            drawText(&cv, font, std::to_string(num), listX + rr, y - rr * 0.35f + rr * 0.36f, rr * 1.0f, Rgb{0.1f, 0.1f, 0.12f}, 1);
            std::string nm = groupName[(size_t)b.group];
            if (nm.size() > 30) nm = nm.substr(0, 28) + "..";
            int q = groupCount[(size_t)b.group];
            if (q > 1) nm += "  x" + std::to_string(q);
            drawText(&cv, font, nm, listX + rr * 2.6f, y, rowH * 0.46f, txt, 0);
        }
    }

    // ---- down to the final size
    res.rgba.assign((size_t)W * H * 4, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < SS; ++j)
                for (int i = 0; i < SS; ++i) {
                    size_t k = (size_t)(y * SS + j) * w + (x * SS + i);
                    float al = cv.alpha[k];
                    r += cv.col[k].r * al; g += cv.col[k].g * al; b += cv.col[k].b * al; a += al;
                }
            uint8_t* o8 = &res.rgba[((size_t)y * W + x) * 4];
            float n = (float)(SS * SS);
            if (a > 0) { r /= a; g /= a; b /= a; }
            o8[0] = (uint8_t)std::lround(std::clamp(r, 0.f, 1.f) * 255);
            o8[1] = (uint8_t)std::lround(std::clamp(g, 0.f, 1.f) * 255);
            o8[2] = (uint8_t)std::lround(std::clamp(b, 0.f, 1.f) * 255);
            o8[3] = (uint8_t)std::lround(std::clamp(a / n, 0.f, 1.f) * 255);
        }

    // ---- SVG: faces as a picture underneath (except line art), lines, balloons and text as vectors
    if (wantSvg) {
        std::ostringstream s;
        float k = 1.f / SS;   // canvas pixels to SVG units (output pixels)
        s << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H << "\" viewBox=\"0 0 " << W << " " << H << "\">\n";
        s << "<!-- Kestral illustration -->\n";
        if (bgA > 0) s << "<rect width=\"100%\" height=\"100%\" fill=\"" << hex(bg) << "\"/>\n";
        if (!facesCol.empty()) {
            std::vector<uint8_t> fr((size_t)W * H * 4, 0);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float r = 0, g = 0, b = 0, a = 0;
                    for (int j = 0; j < SS; ++j)
                        for (int i = 0; i < SS; ++i) {
                            size_t kk = (size_t)(y * SS + j) * w + (x * SS + i);
                            // Only the parts: the background comes from the SVG itself.
                            float al = cv.part[kk] >= 0 ? facesA[kk] : 0.f;
                            r += facesCol[kk].r * al; g += facesCol[kk].g * al; b += facesCol[kk].b * al; a += al;
                        }
                    uint8_t* o8 = &fr[((size_t)y * W + x) * 4];
                    if (a > 0) { r /= a; g /= a; b /= a; }
                    o8[0] = (uint8_t)std::lround(std::clamp(r, 0.f, 1.f) * 255);
                    o8[1] = (uint8_t)std::lround(std::clamp(g, 0.f, 1.f) * 255);
                    o8[2] = (uint8_t)std::lround(std::clamp(b, 0.f, 1.f) * 255);
                    o8[3] = (uint8_t)std::lround(std::clamp(a / (SS * SS), 0.f, 1.f) * 255);
                }
            s << "<image width=\"" << W << "\" height=\"" << H << "\" href=\"data:image/png;base64," << base64(encodePng(fr, W, H)) << "\"/>\n";
        }
        if (lines) {
            // Silhouettes: mesh edges between a face turned toward the camera and one turned away (or open
            // edges), plus the model's edges, kept where nothing covers them.
            auto visibleRuns = [&](glm::vec3 a, glm::vec3 b, std::ostringstream& out) {
                float len = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                int n = std::max(1, (int)std::ceil(len / 1.5f));
                bool on = false;
                for (int si = 0; si <= n; ++si) {
                    float t = (float)si / (float)n;
                    float px = a.x + (b.x - a.x) * t, py = a.y + (b.y - a.y) * t, pd = a.z + (b.z - a.z) * t;
                    int xi = (int)px, yi = (int)py;
                    bool vis = xi >= 0 && yi >= 0 && xi < w && yi < h && cv.depth[(size_t)yi * w + xi] <= pd + dr * 0.006f;
                    if (vis) {
                        out << (on ? " L" : " M") << std::lround(px * k * 10) / 10.0 << " " << std::lround(py * k * 10) / 10.0;
                        on = true;
                    } else {
                        on = false;
                    }
                }
            };
            std::ostringstream outline, inner;
            for (auto& p : parts) {
                if (!p.mesh) continue;
                const MeshData& m = *p.mesh;
                size_t nv = m.vertexCount();
                std::vector<glm::vec3> wp(nv);
                for (size_t i = 0; i < nv; ++i) wp[i] = glm::vec3(p.xform * glm::vec4(m.verts[i * 6], m.verts[i * 6 + 1], m.verts[i * 6 + 2], 1.f));
                // Weld vertices that sit on the same spot (each face has its own copies).
                std::unordered_map<long long, uint32_t> weld;
                std::vector<uint32_t> wid(nv);
                float q = std::max(1e-4f, glm::length(m.bounds.max - m.bounds.min) * 1e-5f);
                for (size_t i = 0; i < nv; ++i) {
                    long long kx = std::llround(wp[i].x / q), ky = std::llround(wp[i].y / q), kz = std::llround(wp[i].z / q);
                    long long key = (kx * 73856093LL) ^ (ky * 19349663LL) ^ (kz * 83492791LL);
                    auto it = weld.emplace(key, (uint32_t)i).first;
                    wid[i] = it->second;
                }
                struct E { int t0 = -1, t1 = -1; uint32_t a, b; };
                std::unordered_map<unsigned long long, E> edges;
                size_t nt = m.indices.size() / 3;
                std::vector<float> facing(nt, 0);
                for (size_t t = 0; t < nt; ++t) {
                    uint32_t ii[3] = {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]};
                    if (ii[0] >= nv || ii[1] >= nv || ii[2] >= nv) continue;
                    glm::vec3 fnrm = glm::cross(wp[ii[1]] - wp[ii[0]], wp[ii[2]] - wp[ii[0]]);
                    facing[t] = glm::dot(fnrm, eye);
                    for (int e = 0; e < 3; ++e) {
                        uint32_t a = wid[ii[e]], b = wid[ii[(e + 1) % 3]];
                        if (a == b) continue;
                        unsigned long long key = (unsigned long long)std::min(a, b) << 32 | std::max(a, b);
                        E& ed = edges[key];
                        ed.a = ii[e];
                        ed.b = ii[(e + 1) % 3];
                        if (ed.t0 < 0) ed.t0 = (int)t; else ed.t1 = (int)t;
                    }
                }
                for (auto& [key, ed] : edges) {
                    bool sil = ed.t1 < 0 || ((facing[(size_t)ed.t0] > 0) != (facing[(size_t)ed.t1] > 0));
                    if (!sil) continue;
                    visibleRuns(toScreen(wp[ed.a]), toScreen(wp[ed.b]), outline);
                }
                const auto& ev = m.edgeVerts;
                for (size_t i = 0; i + 5 < ev.size(); i += 6)
                    visibleRuns(toScreen(glm::vec3(p.xform * glm::vec4(ev[i], ev[i + 1], ev[i + 2], 1.f))),
                                toScreen(glm::vec3(p.xform * glm::vec4(ev[i + 3], ev[i + 4], ev[i + 5], 1.f))), inner);
            }
            s << "<g fill=\"none\" stroke=\"" << hex(ink) << "\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
            s << "<path stroke-width=\"" << pxIn << "\" d=\"" << inner.str() << "\"/>\n";
            s << "<path stroke-width=\"" << pxOut << "\" d=\"" << outline.str() << "\"/>\n";
            s << "</g>\n";
        }
        if (!balloons.empty()) {
            std::string fam = xmlEsc(font) + ", Segoe UI, Arial, sans-serif";
            int num = 0;
            s << "<g font-family=\"" << fam << "\">\n";
            for (auto& b : balloons) {
                ++num;
                float dx = b.bx - b.ax, dy = b.by - b.ay, l = std::max(1e-3f, std::hypot(dx, dy));
                s << "<line x1=\"" << b.ax * k << "\" y1=\"" << b.ay * k << "\" x2=\"" << (b.bx - dx / l * R) * k << "\" y2=\"" << (b.by - dy / l * R) * k
                  << "\" stroke=\"" << hex(ink) << "\" stroke-width=\"" << std::max(1.f, pxIn * 0.9f) << "\"/>\n";
                s << "<circle cx=\"" << b.ax * k << "\" cy=\"" << b.ay * k << "\" r=\"" << dotR * k << "\" fill=\"" << hex(ink) << "\"/>\n";
                s << "<circle cx=\"" << b.bx * k << "\" cy=\"" << b.by * k << "\" r=\"" << (R - std::max(1.5f, R * 0.06f)) * k << "\" fill=\"#ffffff\" stroke=\""
                  << hex(accent) << "\" stroke-width=\"" << std::max(1.5f, R * 0.12f) * k << "\"/>\n";
                s << "<text x=\"" << b.bx * k << "\" y=\"" << (b.by + R * 0.36f) * k << "\" font-size=\"" << R * k << "\" text-anchor=\"middle\" fill=\"#1a1a1f\">"
                  << num << "</text>\n";
            }
            if (list) {
                Rgb txt = dark ? Rgb{0.92f, 0.93f, 0.95f} : Rgb{0.1f, 0.1f, 0.12f};
                s << "<text x=\"" << listX * k << "\" y=\"" << listY * k << "\" font-size=\"" << rowH * 0.62f * k << "\" font-weight=\"600\" fill=\"" << hex(txt) << "\">Parts</text>\n";
                num = 0;
                for (auto& b : balloons) {
                    ++num;
                    float y = listY + rowH * (num + 0.3f), rr = rowH * 0.36f;
                    std::string nm = groupName[(size_t)b.group];
                    int qn = groupCount[(size_t)b.group];
                    if (qn > 1) nm += "  x" + std::to_string(qn);
                    s << "<circle cx=\"" << (listX + rr) * k << "\" cy=\"" << (y - rr * 0.35f) * k << "\" r=\"" << rr * 0.9f * k << "\" fill=\"#ffffff\" stroke=\""
                      << hex(accent) << "\" stroke-width=\"" << rr * 0.2f * k << "\"/>\n";
                    s << "<text x=\"" << (listX + rr) * k << "\" y=\"" << (y - rr * 0.35f + rr * 0.36f) * k << "\" font-size=\"" << rr * k
                      << "\" text-anchor=\"middle\" fill=\"#1a1a1f\">" << num << "</text>\n";
                    s << "<text x=\"" << (listX + rr * 2.6f) * k << "\" y=\"" << y * k << "\" font-size=\"" << rowH * 0.46f * k << "\" fill=\"" << hex(txt) << "\">"
                      << xmlEsc(nm) << "</text>\n";
                }
            }
            s << "</g>\n";
        }
        s << "</svg>\n";
        res.svg = s.str();
    }
    return res;
}

} // namespace kst::illus
