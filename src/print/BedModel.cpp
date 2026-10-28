#include "print/BedModel.h"
#include "core/Json.h"
#include "core/Log.h"
#include "print/Preview.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg.h"
#include "third_party/nanosvgrast.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace kst::print {

namespace fs = std::filesystem;
using json::Value;

namespace {

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// STL (binary or ASCII) to a flat shaded mesh, moved by off.
std::shared_ptr<MeshData> loadStl(const fs::path& p, glm::vec3 off) {
    std::string d = readAll(p);
    if (d.size() < 84) return nullptr;
    auto m = std::make_shared<MeshData>();
    auto addTri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        glm::vec3 n = glm::cross(b - a, c - a);
        float l = glm::length(n);
        if (l < 1e-12f) return;
        n /= l;
        uint32_t i0 = (uint32_t)(m->verts.size() / 6);
        for (auto v : {a, b, c}) {
            v += off;
            m->verts.insert(m->verts.end(), {v.x, v.y, v.z, n.x, n.y, n.z});
            m->bounds.add(v);
        }
        m->indices.insert(m->indices.end(), {i0, i0 + 1, i0 + 2});
        m->triFace.push_back(0);
    };
    uint32_t n = 0;
    std::memcpy(&n, d.data() + 80, 4);
    if (84ull + 50ull * n == d.size()) {
        for (uint32_t t = 0; t < n; ++t) {
            float f[12];
            std::memcpy(f, d.data() + 84 + 50ull * t, 48);
            addTri({f[3], f[4], f[5]}, {f[6], f[7], f[8]}, {f[9], f[10], f[11]});
        }
    } else {
        std::istringstream in(d);
        std::string w;
        std::vector<glm::vec3> vs;
        while (in >> w) {
            if (w == "vertex") {
                glm::vec3 v;
                in >> v.x >> v.y >> v.z;
                vs.push_back(v);
                if (vs.size() == 3) { addTri(vs[0], vs[1], vs[2]); vs.clear(); }
            } else if (w == "endloop") vs.clear();
        }
    }
    if (m->indices.empty()) return nullptr;
    m->faceCount = 1;
    return m;
}

void addQuad(MeshData& m, float x0, float y0, float x1, float y1, float z) {
    uint32_t b = (uint32_t)(m.verts.size() / 6);
    m.verts.insert(m.verts.end(), {x0, y0, z, 0, 0, 1, x1, y0, z, 0, 0, 1, x1, y1, z, 0, 0, 1, x0, y1, z, 0, 0, 1});
    m.indices.insert(m.indices.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    m.triFace.push_back(0);
    m.triFace.push_back(0);
    m.bounds.add(glm::vec3(x0, y0, z));
    m.bounds.add(glm::vec3(x1, y1, z));
    m.faceCount = 1;
}

// An image (RGBA, straight alpha) laid on the rectangle (x, y, w, h) in mm, image top at y + h
// (the way Orca maps its plate textures).
// Pixels snap to the nearest palette color (or a coarse quantization without one) and become
// flat quads, merged into rectangles per color.
void imageToDecals(const std::vector<uint8_t>& px, int W, int H, std::vector<uint32_t> palette, float x, float y, float w, float h,
                   float z, std::vector<BedDecal>& out) {
    if (W <= 0 || H <= 0) return;
    std::vector<int> cls((size_t)W * H, -1);
    std::map<uint32_t, int> quantIndex;
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            const uint8_t* p = &px[((size_t)r * W + c) * 4];
            if (p[3] < 128) continue;
            if (!palette.empty()) {
                int best = 0;
                int bd = 1 << 30;
                for (size_t k = 0; k < palette.size(); ++k) {
                    uint32_t q = palette[k];
                    int dr = (int)(q & 255) - p[0], dg = (int)((q >> 8) & 255) - p[1], db = (int)((q >> 16) & 255) - p[2];
                    int dd = dr * dr + dg * dg + db * db;
                    if (dd < bd) { bd = dd; best = (int)k; }
                }
                cls[(size_t)r * W + c] = best;
            } else {
                uint32_t q = 0xFF000000u | (uint32_t)(p[0] & 0xE0) | ((uint32_t)(p[1] & 0xE0) << 8) | ((uint32_t)(p[2] & 0xE0) << 16);
                auto it = quantIndex.find(q);
                if (it == quantIndex.end()) {
                    if (quantIndex.size() >= 24) continue;   // keep it to a handful of colors
                    it = quantIndex.emplace(q, (int)quantIndex.size()).first;
                }
                cls[(size_t)r * W + c] = it->second;
            }
        }
    if (palette.empty()) {
        palette.resize(quantIndex.size());
        for (auto& [q, i] : quantIndex) palette[(size_t)i] = q | 0x101010u;
    }
    std::vector<std::shared_ptr<MeshData>> meshes(palette.size());
    float pw = w / W, ph = h / H;
    std::vector<char> used((size_t)W * H, 0);
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            size_t i = (size_t)r * W + c;
            int k = cls[i];
            if (k < 0 || used[i]) continue;
            // Greedy rectangle: as wide as the run, then as tall as every row matches.
            int c1 = c;
            while (c1 + 1 < W && cls[(size_t)r * W + c1 + 1] == k && !used[(size_t)r * W + c1 + 1]) ++c1;
            int r1 = r;
            for (bool ok = true; ok && r1 + 1 < H;) {
                for (int cc = c; cc <= c1; ++cc)
                    if (cls[(size_t)(r1 + 1) * W + cc] != k || used[(size_t)(r1 + 1) * W + cc]) { ok = false; break; }
                if (ok) ++r1;
            }
            for (int rr = r; rr <= r1; ++rr)
                for (int cc = c; cc <= c1; ++cc) used[(size_t)rr * W + cc] = 1;
            if (!meshes[(size_t)k]) meshes[(size_t)k] = std::make_shared<MeshData>();
            addQuad(*meshes[(size_t)k], x + c * pw, y + h - (r1 + 1) * ph, x + (c1 + 1) * pw, y + h - r * ph, z);
        }
    for (size_t k = 0; k < meshes.size(); ++k)
        if (meshes[k]) out.push_back({meshes[k], palette[k]});
}

// An SVG or PNG file on a rectangle in mm (stretched to fit, like Orca's textures).
void addTexture(const fs::path& file, float x, float y, float w, float h, float z, std::vector<BedDecal>& out) {
    std::string ext = file.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    const float mmPerPx = 0.2f;
    if (ext == ".svg") {
        std::string text = readAll(file);
        if (text.empty()) return;
        NSVGimage* img = nsvgParse(text.data(), "px", 96.f);
        if (!img) return;
        if (img->width <= 0 || img->height <= 0) { nsvgDelete(img); return; }
        int W = std::clamp((int)std::lround(w / mmPerPx), 8, 2048);
        float scale = W / img->width;
        int Hsvg = std::clamp((int)std::lround(img->height * scale), 8, 4096);
        std::vector<uint8_t> px((size_t)W * Hsvg * 4, 0);
        NSVGrasterizer* rast = nsvgCreateRasterizer();
        nsvgRasterize(rast, img, 0, 0, scale, px.data(), W, Hsvg, W * 4);
        nsvgDeleteRasterizer(rast);
        std::vector<uint32_t> pal;
        for (NSVGshape* sh = img->shapes; sh; sh = sh->next) {
            if (!(sh->flags & NSVG_FLAGS_VISIBLE)) continue;
            if (sh->fill.type == NSVG_PAINT_COLOR) pal.push_back(sh->fill.color | 0xFF000000u);
            else if (sh->fill.type != NSVG_PAINT_NONE) { pal.clear(); break; }   // gradients: quantize instead
        }
        std::sort(pal.begin(), pal.end());
        pal.erase(std::unique(pal.begin(), pal.end()), pal.end());
        if (pal.size() > 24) pal.clear();
        nsvgDelete(img);
        imageToDecals(px, W, Hsvg, pal, x, y, w, h, z, out);
    } else if (ext == ".png") {
        std::string data = readAll(file);
        int W = 0, H = 0, n = 0;
        unsigned char* p = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(data.data()), (int)data.size(), &W, &H, &n, 4);
        if (!p) return;
        // Down to about a millimetre per pixel: textures are photos, the plate only needs the look.
        int tw = std::clamp((int)std::lround(w / 1.f), 8, 512), thh = std::clamp((int)std::lround(h / 1.f), 8, 512);
        std::vector<uint8_t> px((size_t)tw * thh * 4);
        for (int r = 0; r < thh; ++r)
            for (int c = 0; c < tw; ++c) {
                int sx = std::min(W - 1, c * W / tw), sy = std::min(H - 1, r * H / thh);
                std::memcpy(&px[((size_t)r * tw + c) * 4], p + ((size_t)sy * W + sx) * 4, 4);
            }
        stbi_image_free(p);
        imageToDecals(px, tw, thh, {}, x, y, w, h, z, out);
    }
}

std::shared_ptr<MeshData> gridMesh(float x0, float y0, float x1, float y1, float step, bool bold, float z) {
    auto m = std::make_shared<MeshData>();
    float hw = bold ? 0.32f : 0.16f;
    for (int i = 0;; ++i) {
        float x = x0 + i * step;
        if (x > x1 + 1e-3f) break;
        bool isBold = i % 5 == 0;
        if (isBold != bold) continue;
        addQuad(*m, x - hw, y0, x + hw, y1, z);
    }
    for (int i = 0;; ++i) {
        float y = y0 + i * step;
        if (y > y1 + 1e-3f) break;
        bool isBold = i % 5 == 0;
        if (isBold != bold) continue;
        addQuad(*m, x0, y - hw, x1, y + hw, z);
    }
    if (!bold) {   // the plate's outline, like Orca's contour lines
        addQuad(*m, x0 - hw, y0 - hw, x1 + hw, y0 + hw, z);
        addQuad(*m, x0 - hw, y1 - hw, x1 + hw, y1 + hw, z);
        addQuad(*m, x0 - hw, y0 - hw, x0 + hw, y1 + hw, z);
        addQuad(*m, x1 - hw, y0 - hw, x1 + hw, y1 + hw, z);
    }
    return m;
}

} // namespace

BedScene loadBedScene(const OrcaSetup& s, const std::string& machine, const std::string& bedTypeIn) {
    BedScene out;
    const Preset* mp = findPreset(s, "machine", machine);
    if (!mp) return out;
    std::string err;
    Value mj = Value::parse(resolvePreset(s, *mp, err));
    auto area = parsePrintableArea(mj.dump());
    out.plate = bedMesh(area);
    if (area.size() < 3) return out;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (auto& [x, y] : area) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
    float bw = x1 - x0, bh = y1 - y0;
    int step = std::min(bw, bh) >= 1200 ? 50 : std::min(bw, bh) >= 600 ? 20 : 10;
    out.gridThin = gridMesh(x0, y0, x1, y1, (float)step, false, -0.035f);
    out.gridBold = gridMesh(x0, y0, x1, y1, (float)step, true, -0.03f);

    // The printer model's own file (bed_model, bed_texture) sits in its vendor folder.
    const Preset* base = systemBase(s, *mp);
    std::string printerModel = base && !base->printerModel.empty() ? base->printerModel : mj["printer_model"].asString();
    std::vector<fs::path> vendorDirs;
    if (base) vendorDirs.push_back(u8path(base->path).parent_path().parent_path());
    std::string vendorName = vendorDirs.empty() ? std::string() : pathToU8(vendorDirs[0].filename());
    std::vector<fs::path> resRoots;
    if (!s.exe.empty() && s.exe.rfind("flatpak:", 0) != 0) {
        resRoots.push_back(u8path(s.exe).parent_path() / "resources");
        resRoots.push_back(u8path(s.exe).parent_path().parent_path() / "resources");
    }
    for (auto& r : resRoots)
        if (!vendorName.empty()) vendorDirs.push_back(r / "profiles" / u8path(vendorName));
    std::error_code ec;
    Value model;
    for (auto& vd : vendorDirs) {
        fs::path mf = vd / "machine" / u8path(printerModel + ".json");
        if (fs::exists(mf, ec)) { model = Value::parse(readAll(mf)); if (model.isObject()) break; }
    }
    auto findFile = [&](const std::string& name) -> fs::path {
        if (name.empty()) return {};
        for (auto& vd : vendorDirs)
            if (fs::exists(vd / u8path(name), ec)) return vd / u8path(name);
        return {};
    };
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    if (model.isObject()) {
        // Orca puts the model's origin at the middle of the printable area, just under the plate.
        fs::path stl = findFile(model["bed_model"].asString());
        if (!stl.empty()) {
            out.model = loadStl(stl, glm::vec3(cx, cy, -0.45f));
            out.modelFile = pathToU8(stl);
        }
    }
    bool bbl = vendorName == "BBL";
    if (bbl) {
        // Bambu: the plate type's name printed down the left side and along the front.
        std::string bt = bedTypeIn.empty() || bedTypeIn == "Default" ? (model.isObject() ? model["default_bed_type"].asString() : std::string()) : bedTypeIn;
        if (bt.empty()) bt = "Textured PEI Plate";
        struct Part { float x, y, w, h; const char* file; };
        std::vector<Part> parts;
        const Part bottomPc{74, -10, 148, 12, "bbl_bed_pc_bottom.svg"};
        if (bt == "Supertack Plate" || bt == "Cool Plate (SuperTack)") parts = {{9, 70, 12.5f, 170, "bbl_bed_st_left.svg"}, {74, -10, 148, 12, "bbl_bed_st_bottom.svg"}};
        else if (bt == "Cool Plate" || bt == "Smooth Cool Plate") parts = {{10, 130, 10, 110, "bbl_bed_pc_left.svg"}, bottomPc};
        else if (bt == "Textured Cool Plate") parts = {{10, 130, 10, 110, "orca_bed_pct_left.svg"}, bottomPc};
        else if (bt == "Engineering Plate") parts = {{7.5f, 90, 12.5f, 150, "bbl_bed_ep_left.svg"}, {74, -10, 148, 12, "bbl_bed_ep_bottom.svg"}};
        else if (bt == "High Temp Plate") parts = {{7.5f, 50, 12.5f, 190, "bbl_bed_pei_left.svg"}, {74, -10, 148, 12, "bbl_bed_pei_bottom.svg"}};
        else parts = {{10, 80, 10, 160, "bbl_bed_pte_left.svg"}, {74, -10, 148, 12, "bbl_bed_pte_bottom.svg"}};
        std::string endName = model.isObject() ? model["bottom_texture_end_name"].asString() : std::string();
        float xr = bw / 256.f, yr = bh / 256.f;
        for (auto& p : parts) {
            std::string file = p.file;
            if (!endName.empty() && file.find("_bottom.svg") != std::string::npos)
                file = file.substr(0, file.size() - 4) + "_" + endName + ".svg";
            for (auto& r : resRoots) {
                fs::path f = r / "images" / u8path(file);
                if (!fs::exists(f, ec)) continue;
                addTexture(f, x0 + p.x * xr, y0 + p.y * yr, p.w * xr, p.h * yr, -0.02f, out.decals);
                break;
            }
        }
    } else if (model.isObject()) {
        // Other vendors: their texture over the whole plate.
        fs::path tex = findFile(model["bed_texture"].asString());
        if (!tex.empty()) addTexture(tex, x0, y0, bw, bh, -0.02f, out.decals);
    }
    return out;
}

} // namespace kst::print
