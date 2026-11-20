// Pictures of the design for the assistant (look_at_design): a software render, so it runs on a
// worker thread and never touches the user's view or the graphics card.
#include "app/Thumbnails.h"

#include <algorithm>
#include <cmath>

namespace kst::thumb {

std::vector<uint8_t> renderView(const std::vector<Item>& items, int W, int H, glm::vec3 eyeDir, glm::vec3 upHint) {
    const int SS = 2;   // drawn at twice the size, averaged down
    const int w = W * SS, h = H * SS;
    const glm::vec3 eye = glm::normalize(eyeDir);
    glm::vec3 right = glm::cross(-eye, upHint);
    if (glm::length(right) < 1e-4f) right = glm::cross(-eye, glm::vec3(0, 1, 0));
    right = glm::normalize(right);
    const glm::vec3 up = glm::cross(right, -eye);

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
    // Background: Kestral's dark viewport with a soft vertical fade.
    std::vector<glm::vec3> col((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        float t = (float)y / (float)h;
        glm::vec3 c = glm::mix(glm::vec3(0.23f, 0.24f, 0.27f), glm::vec3(0.15f, 0.155f, 0.17f), t);
        std::fill(col.begin() + (size_t)y * w, col.begin() + (size_t)(y + 1) * w, c);
    }
    std::vector<float> depth((size_t)w * h, -1e30f);
    std::vector<glm::vec3> nrm((size_t)w * h, glm::vec3(0));
    bool any = x0 <= x1;
    float scale = 1, cx = 0, cy = 0;
    if (any) {
        float sx = std::max(x1 - x0, 1e-6f), sy = std::max(y1 - y0, 1e-6f);
        scale = std::min(w * 0.84f / sx, h * 0.84f / sy);
        cx = (x0 + x1) * 0.5f;
        cy = (y0 + y1) * 0.5f;
    }
    auto toScreen = [&](const glm::vec3& p) {
        return glm::vec3((glm::dot(p, right) - cx) * scale + w * 0.5f, h * 0.5f - (glm::dot(p, up) - cy) * scale, glm::dot(p, eye));
    };
    const glm::vec3 light = glm::normalize(eye * 0.75f + up * 0.55f - right * 0.35f);
    for (auto& it : items) {
        if (!it.mesh) continue;
        const MeshData& m = *it.mesh;
        const auto& v = m.verts;
        glm::mat3 nm = glm::mat3(it.xform);
        size_t nv = m.vertexCount();
        std::vector<glm::vec3> sp(nv), sn(nv);
        for (size_t i = 0; i < nv; ++i) {
            sp[i] = toScreen(glm::vec3(it.xform * glm::vec4(v[i * 6], v[i * 6 + 1], v[i * 6 + 2], 1.f)));
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
                    if (d <= depth[k]) continue;
                    depth[k] = d;
                    glm::vec3 n = glm::normalize(w0 * sn[ia] + w1 * sn[ib] + w2 * sn[ic] + glm::vec3(1e-9f));
                    if (glm::dot(n, eye) < 0) n = -n;
                    nrm[k] = n;
                    float diff = std::max(0.f, glm::dot(n, light));
                    float spec = std::pow(std::max(0.f, glm::dot(glm::reflect(-light, n), eye)), 32.f) * 0.22f;
                    col[k] = glm::clamp(base * (0.34f + 0.66f * diff) + glm::vec3(spec), 0.f, 1.f);
                }
            }
        }
    }
    // Silhouettes and steps: darken where the depth jumps.
    float dr = std::max(d1 - d0, 1e-6f);
    std::vector<glm::vec3> shaded = col;
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
            size_t k = (size_t)y * w + x;
            if (depth[k] < -1e29f) continue;
            float jump = 0;
            for (size_t nk : {k - 1, k + 1, k - (size_t)w, k + (size_t)w}) {
                if (depth[nk] < -1e29f) { jump = 1; break; }
                jump = std::max(jump, (depth[k] - depth[nk]) / dr);
            }
            if (jump > 0.015f) shaded[k] = col[k] * 0.35f;
        }
    col.swap(shaded);
    // The B-rep edges (the lines Kestral draws on the model), where they are not hidden.
    for (auto& it : items) {
        if (!it.mesh) continue;
        const auto& ev = it.mesh->edgeVerts;
        for (size_t i = 0; i + 5 < ev.size(); i += 6) {
            glm::vec3 a = toScreen(glm::vec3(it.xform * glm::vec4(ev[i], ev[i + 1], ev[i + 2], 1.f)));
            glm::vec3 b = toScreen(glm::vec3(it.xform * glm::vec4(ev[i + 3], ev[i + 4], ev[i + 5], 1.f)));
            float len = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
            int n = std::max(1, (int)std::ceil(len));
            for (int s = 0; s <= n; ++s) {
                float t = (float)s / (float)n;
                float px = a.x + (b.x - a.x) * t, py = a.y + (b.y - a.y) * t, pd = a.z + (b.z - a.z) * t;
                for (int oy = -1; oy <= 0; ++oy)
                    for (int ox = -1; ox <= 0; ++ox) {
                        int xi = (int)px + ox, yi = (int)py + oy;
                        if (xi < 0 || yi < 0 || xi >= w || yi >= h) continue;
                        size_t k = (size_t)yi * w + xi;
                        if (depth[k] > pd + dr * 0.004f) continue;   // behind a surface
                        col[k] = glm::vec3(0.08f, 0.08f, 0.09f);
                    }
            }
        }
    }
    // Axis sign in the bottom left: X red, Y green, Z blue.
    {
        glm::vec3 o(70.f * SS / 2, h - 70.f * SS / 2, 0);
        const glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const glm::vec3 cols[3] = {{0.92f, 0.3f, 0.28f}, {0.36f, 0.8f, 0.36f}, {0.35f, 0.55f, 0.98f}};
        for (int k = 0; k < 3; ++k) {
            float ex = glm::dot(axes[k], right) * 44.f * SS / 2, ey = -glm::dot(axes[k], up) * 44.f * SS / 2;
            int n = (int)std::ceil(std::max(std::abs(ex), std::abs(ey))) + 1;
            for (int s = 0; s <= n; ++s) {
                float t = (float)s / (float)n;
                for (int oy = -2; oy <= 2; ++oy)
                    for (int ox = -2; ox <= 2; ++ox) {
                        int xi = (int)(o.x + ex * t) + ox, yi = (int)(o.y + ey * t) + oy;
                        if (xi >= 0 && yi >= 0 && xi < w && yi < h) col[(size_t)yi * w + xi] = cols[k];
                    }
            }
        }
    }
    std::vector<uint8_t> out((size_t)W * H * 4, 255);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            glm::vec3 sum(0);
            for (int j = 0; j < SS; ++j)
                for (int i = 0; i < SS; ++i) sum += col[(size_t)(y * SS + j) * w + (x * SS + i)];
            sum /= (float)(SS * SS);
            uint8_t* o = &out[((size_t)y * W + x) * 4];
            o[0] = (uint8_t)std::lround(std::clamp(sum.x, 0.f, 1.f) * 255.f);
            o[1] = (uint8_t)std::lround(std::clamp(sum.y, 0.f, 1.f) * 255.f);
            o[2] = (uint8_t)std::lround(std::clamp(sum.z, 0.f, 1.f) * 255.f);
        }
    return out;
}

} // namespace kst::thumb
