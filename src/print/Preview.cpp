#include "print/Preview.h"
#include "core/Json.h"
#include "print/Orca.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace kst::print {

namespace {

struct FeatureInfo { const char* name; uint32_t rgb; };
// OrcaSlicer's preview colors.
const FeatureInfo kFeatures[kFeatureCount] = {
    {"Outer wall", 0xFF7D38}, {"Inner wall", 0xFFE64D}, {"Overhang wall", 0x1F1FFF}, {"Sparse infill", 0xB03029},
    {"Internal solid infill", 0x9654CC}, {"Top surface", 0xF04040}, {"Bottom surface", 0x7D7DFF}, {"Bridge", 0x4D80BA},
    {"Gap infill", 0xFFFFFF}, {"Ironing", 0xFF8C69}, {"Skirt", 0x00876E}, {"Brim", 0x00876E}, {"Support", 0x00FF00},
    {"Support interface", 0x008000}, {"Support transition", 0x004000}, {"Prime tower", 0xB3E3AB}, {"Custom", 0x5ED194},
    {"Travel", 0x384EC4}, {"Retract", 0xE51CE5}, {"Unretract", 0x4DE5E5}, {"Seams", 0xE5E5E5}, {"Filament changes", 0xC8C850},
};
// Orca's gradient (blue, slow / low, to red, fast / high).
const uint32_t kGradient[kBins] = {0x0B2C7A, 0x135985, 0x1C8891, 0x04D60F, 0xAAF200, 0xFCF903, 0xF5CE0A, 0xD16830, 0xC2523C, 0x942616, 0x751005};

int featureOf(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
    if (has("overhang")) return (int)Feature::OverhangWall;
    if (has("outer wall") || has("external perimeter")) return (int)Feature::OuterWall;
    if (has("wall") || has("perimeter")) return (int)Feature::InnerWall;   // inner wall, floating vertical shell
    if (has("support interface")) return (int)Feature::SupportInterface;
    if (has("support transition")) return (int)Feature::SupportTransition;
    if (has("support")) return (int)Feature::Support;
    if (has("sparse") || s == "internal infill") return (int)Feature::SparseInfill;
    if (has("top")) return (int)Feature::TopSurface;
    if (has("bottom")) return (int)Feature::BottomSurface;
    if (has("bridge")) return (int)Feature::Bridge;
    if (has("solid")) return (int)Feature::SolidInfill;
    if (has("gap")) return (int)Feature::GapFill;
    if (has("ironing")) return (int)Feature::Ironing;
    if (has("skirt")) return (int)Feature::Skirt;
    if (has("brim")) return (int)Feature::Brim;
    if (has("prime") || has("wipe tower")) return (int)Feature::PrimeTower;
    if (has("infill")) return (int)Feature::SparseInfill;
    return (int)Feature::Custom;
}

// Value after a letter in a G-code line ("X12.5"), false when the letter is not there.
bool param(const char* line, const char* end, char letter, float& out) {
    for (const char* p = line; p < end; ++p) {
        if (*p == ';') return false;
        if (*p == letter && (p == line || p[-1] == ' ' || p[-1] == '\t')) {
            char* e = nullptr;
            float v = std::strtof(p + 1, &e);
            if (e == p + 1) return false;
            out = v;
            return true;
        }
    }
    return false;
}

const char* after(const char* line, const char* end, const char* key) {
    size_t n = std::strlen(key);
    if ((size_t)(end - line) < n || std::strncmp(line, key, n) != 0) return nullptr;
    return line + n;
}

std::string readAll(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

const char* featureName(int f) { return f >= 0 && f < kFeatureCount ? kFeatures[f].name : "?"; }
uint32_t binColor(int b) {
    uint32_t rgb = kGradient[std::clamp(b, 0, kBins - 1)];
    return ((rgb >> 16) & 255) | (((rgb >> 8) & 255) << 8) | ((rgb & 255) << 16) | 0xFF000000u;
}
float segValue(const ToolSeg& s, float layerHeight, int mode) {
    switch (mode) {
    case ViewSpeed: return s.speed;
    case ViewHeight: return layerHeight;
    case ViewWidth: return s.width;
    case ViewFan: return s.fan;
    case ViewTemp: return s.temp;
    case ViewFlow: return s.width * layerHeight * s.speed;
    default: return 0.f;
    }
}
uint32_t featureColor(int f) {
    uint32_t rgb = f >= 0 && f < kFeatureCount ? kFeatures[f].rgb : 0x888888;
    // RGBA8 as the renderer reads it: R in the low byte.
    return ((rgb >> 16) & 255) | (((rgb >> 8) & 255) << 8) | ((rgb & 255) << 16) | 0xFF000000u;
}

Toolpaths parseGcode(const std::string& g, const std::atomic<bool>* cancel) {
    Toolpaths tp;
    float x = 0, y = 0, z = 0, e = 0;
    float width = 0.42f, layerH = 0.2f, feed = 1800.f;   // feed in mm/min
    bool relE = true, relXY = false;
    int feature = (int)Feature::Custom;
    int tool = 0;
    int fan = 0, temp = 0;
    uint32_t lineNo = 0;
    bool lastExtruded = false;
    int lastFeature = -1;
    ToolLayer* cur = nullptr;
    auto gtext = std::make_shared<std::string>(g);
    tp.gcode = gtext;
    tp.lineStart.reserve(g.size() / 24 + 16);
    bool sawLayer = false, adoptZ = false;   // moves before the first layer (purge line) belong to layer 1
    auto newLayer = [&](float zz) {
        tp.layers.push_back({});
        cur = &tp.layers.back();
        cur->z = zz;
        cur->height = layerH;
    };
    auto addSegF = [&](float x0, float y0, float x1, float y1, int feat) {
        if (!cur) newLayer(z);
        float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
        bool marker = featureIsMarker(feat);
        if (len < 1e-4f && !marker) return;
        float spd = feed / 60.f;
        tp.length[feat] += len;
        if (!marker) tp.seconds[feat] += len / std::max(spd, 0.1f);
        tp.used[feat] = true;
        if (marker) ++tp.count[feat];
        if (!featureIsMove(feat)) tp.toolUsed[tool & 15] = true;
        // Joins it to the previous segment when it carries straight on (curves stay smooth, far fewer pieces).
        if (!marker && !cur->segs.empty()) {
            ToolSeg& p = cur->segs.back();
            if (p.feature == feat && p.tool == tool && std::abs(p.width - width) < 1e-3f && std::abs(p.speed - spd) < 0.05f && p.fan == fan && p.temp == temp &&
                std::abs(p.x1 - x0) < 1e-4f && std::abs(p.y1 - y0) < 1e-4f) {
                float px = p.x1 - p.x0, py = p.y1 - p.y0, pl = std::sqrt(px * px + py * py);
                if (pl > 1e-6f) {
                    float cross = std::abs(px * dy - py * dx) / (pl * len), dot = (px * dx + py * dy) / (pl * len);
                    if (dot > 0 && cross < 0.01f) { p.x1 = x1; p.y1 = y1; p.line = lineNo; return; }
                }
            }
        }
        ToolSeg sg{x0, y0, x1, y1, featureIsMove(feat) ? 0.12f : width, (uint8_t)feat, (uint8_t)tool};
        sg.fan = (uint8_t)std::clamp(fan, 0, 100);
        sg.speed = spd;
        sg.temp = (uint16_t)std::clamp(temp, 0, 999);
        sg.line = lineNo;
        cur->segs.push_back(sg);
        if (!featureIsMove(feat)) {
            tp.bounds.add(glm::vec3(std::min(x0, x1), std::min(y0, y1), z - layerH));
            tp.bounds.add(glm::vec3(std::max(x0, x1), std::max(y0, y1), z));
        }
    };
    auto addSeg = [&](float x0, float y0, float x1, float y1) { addSegF(x0, y0, x1, y1, feature); };
    const char* p = g.data();
    const char* gend = p + g.size();
    size_t lines = 0;
    while (p < gend) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', (size_t)(gend - p)));
        const char* end = nl ? nl : gend;
        const char* line = p;
        p = nl ? nl + 1 : gend;
        tp.lineStart.push_back((uint32_t)(line - g.data()));
        lineNo = (uint32_t)tp.lineStart.size() - 1;
        while (line < end && (*line == ' ' || *line == '\t')) ++line;
        if (end > line && end[-1] == '\r') --end;
        if (line >= end) continue;
        if (cancel && (++lines & 0xFFFF) == 0 && cancel->load()) { tp.error = "Stopped"; return tp; }
        if (*line == ';') {
            const char* v;
            if ((v = after(line, end, "; FEATURE:")) || (v = after(line, end, ";TYPE:"))) {
                while (v < end && *v == ' ') ++v;
                feature = featureOf(std::string(v, end));
            } else if ((v = after(line, end, "; LINE_WIDTH:")) || (v = after(line, end, ";WIDTH:"))) {
                float w = std::strtof(v, nullptr);
                if (w > 0.01f && w < 5.f) width = w;
            } else if ((v = after(line, end, "; LAYER_HEIGHT:")) || (v = after(line, end, ";HEIGHT:"))) {
                float h = std::strtof(v, nullptr);
                if (h > 0.001f && h < 5.f) { layerH = h; if (cur) cur->height = h; }
            } else if ((v = after(line, end, "; Z_HEIGHT:")) || (v = after(line, end, ";Z:"))) {
                float zz = std::strtof(v, nullptr);
                if (cur && (cur->segs.empty() || adoptZ)) { cur->z = zz; cur->height = layerH; }
                adoptZ = false;
                z = zz;
            } else if (after(line, end, "; CHANGE_LAYER") || after(line, end, ";LAYER_CHANGE")) {
                if (!sawLayer && cur) adoptZ = true;
                else if (!cur || !cur->segs.empty()) newLayer(z);
                sawLayer = true;
            }
            continue;
        }
        if (*line == 'T' && line + 1 < end && line[1] >= '0' && line[1] <= '9') {   // filament change
            int t = std::atoi(line + 1);
            if (t >= 0 && t < 16) {
                if (t != tool && sawLayer) { ++tp.toolChanges; addSegF(x, y, x, y, (int)Feature::ToolChange); }
                tool = t;
            }
            continue;
        }
        if (*line != 'G' && *line != 'M') continue;
        int code = std::atoi(line + 1);
        if (*line == 'M') {
            float v;
            if (code == 82) relE = false;
            else if (code == 83) relE = true;
            else if (code == 106) {
                float pp = 0;
                bool hasP = param(line, end, 'P', pp);
                if ((!hasP || pp == 0 || pp == 1) && param(line, end, 'S', v)) fan = (int)std::lround(v / 255.f * 100.f);
            } else if (code == 107) fan = 0;
            else if ((code == 104 || code == 109) && param(line, end, 'S', v)) temp = (int)std::lround(v);
            continue;
        }
        if (code == 90) { relXY = false; relE = false; continue; }   // G90 sets E absolute too, unless M83 follows
        if (code == 91) { relXY = true; continue; }
        if (code == 92) { float v; if (param(line, end, 'E', v)) e = v; continue; }
        if (code == 28) { x = y = 0; continue; }
        if (code != 0 && code != 1 && code != 2 && code != 3) continue;
        float nx = x, ny = y, nz = z, ne = 0, v;
        if (param(line, end, 'F', v) && v > 0) feed = v;
        bool hasE = false;
        if (param(line, end, 'X', v)) nx = relXY ? x + v : v;
        if (param(line, end, 'Y', v)) ny = relXY ? y + v : v;
        if (param(line, end, 'Z', v)) nz = relXY ? z + v : v;
        if (param(line, end, 'E', v)) { hasE = true; ne = relE ? v : v - e; if (!relE) e = v; }
        bool extrude = hasE && ne > 1e-6f;
        bool moves = std::abs(nx - x) > 1e-5f || std::abs(ny - y) > 1e-5f;
        if (hasE && ne < -1e-6f) { addSegF(x, y, x, y, (int)Feature::Retract); lastExtruded = false; }
        else if (extrude && !moves) { addSegF(x, y, x, y, (int)Feature::Unretract); extrude = false; }
        if (extrude) { tp.filamentMm[feature] += ne; tp.toolMm[tool & 15] += ne; }
        if (extrude && moves && feature == (int)Feature::OuterWall && (!lastExtruded || lastFeature != feature)) addSegF(x, y, x, y, (int)Feature::Seam);
        if (!extrude && moves && sawLayer && cur) addSegF(x, y, nx, ny, (int)Feature::Travel);
        if (moves) { lastExtruded = extrude; if (extrude) lastFeature = feature; }
        if (nz != z) {
            // A new height while printing (spiral vase, or no layer comments): its own layer.
            if (extrude && cur && !cur->segs.empty() && std::abs(nz - cur->z) > 1e-3f) newLayer(nz);
            z = nz;
            if (cur && cur->segs.empty()) cur->z = z;
        }
        if (extrude) {
            if (code == 2 || code == 3) {
                float i = 0, j = 0;
                param(line, end, 'I', i);
                param(line, end, 'J', j);
                float cx = x + i, cy = y + j, r = std::sqrt(i * i + j * j);
                float a0 = std::atan2(y - cy, x - cx), a1 = std::atan2(ny - cy, nx - cx);
                float sweep = a1 - a0;
                const float tau = 6.28318531f;
                if (code == 2) { while (sweep >= 0) sweep -= tau; }   // clockwise
                else { while (sweep <= 0) sweep += tau; }
                if (std::abs(nx - x) < 1e-5f && std::abs(ny - y) < 1e-5f) sweep = code == 2 ? -tau : tau;   // full circle
                int n = std::clamp((int)std::ceil(std::abs(sweep) * r / 0.5f), 2, 128);
                float px = x, py = y;
                for (int k = 1; k <= n; ++k) {
                    float a = a0 + sweep * (float)k / (float)n;
                    float qx = k == n ? nx : cx + r * std::cos(a), qy = k == n ? ny : cy + r * std::sin(a);
                    addSeg(px, py, qx, qy);
                    px = qx; py = qy;
                }
            } else {
                addSeg(x, y, nx, ny);
            }
        }
        x = nx;
        y = ny;
    }
    // Drop layers with nothing printed (layer comments without printing, e.g. at the end).
    tp.layers.erase(std::remove_if(tp.layers.begin(), tp.layers.end(), [](const ToolLayer& l) {
                        for (auto& s : l.segs) if (!featureIsMove(s.feature)) return false;
                        return true;
                    }), tp.layers.end());
    for (auto& l : tp.layers) tp.segments += l.segs.size();
    // The range of each value (printed lines only), for the gradient views.
    for (int m = ViewSpeed; m < ViewModes; ++m) { tp.lo[m] = 1e30f; tp.hi[m] = -1e30f; }
    for (auto& l : tp.layers)
        for (auto& sg : l.segs) {
            if (featureIsMove(sg.feature)) continue;
            for (int m = ViewSpeed; m < ViewModes; ++m) {
                float v = segValue(sg, l.height, m);
                tp.lo[m] = std::min(tp.lo[m], v);
                tp.hi[m] = std::max(tp.hi[m], v);
            }
        }
    for (int m = ViewSpeed; m < ViewModes; ++m) if (tp.lo[m] > tp.hi[m]) tp.lo[m] = tp.hi[m] = 0;
    if (tp.layers.empty() && tp.error.empty()) tp.error = "The print file has no moves in it";
    return tp;
}

Toolpaths loadToolpaths(const std::string& path, const std::atomic<bool>* cancel) {
    std::string lower = path;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    std::string g;
    if (lower.size() > 4 && lower.rfind(".3mf") == lower.size() - 4) {
        g = readZipEntry(path, "Metadata/plate_1.gcode");
        for (int i = 2; g.empty() && i <= 16; ++i) g = readZipEntry(path, "Metadata/plate_" + std::to_string(i) + ".gcode");
    } else {
        g = readAll(path);
    }
    if (g.empty()) {
        Toolpaths tp;
        tp.error = "No G-code found in " + path;
        return tp;
    }
    return parseGcode(g, cancel);
}

MeshPtr toolpathMesh(const ToolLayer& layer, int feature, size_t upTo, int mode, float lo, float hi) {
    size_t n = 0, count = std::min(upTo, layer.segs.size());
    auto keyOf = [&](const ToolSeg& s) {
        if (mode == ViewFeature) return (int)s.feature;
        if (featureIsMove(s.feature)) return -1;   // the moves and markers keep their line type meshes
        if (mode == ViewTool) return (int)s.tool;
        float v = segValue(s, layer.height, mode);
        float t = hi > lo ? (v - lo) / (hi - lo) : 0.f;
        return std::clamp((int)(t * kBins), 0, kBins - 1);
    };
    for (size_t i = 0; i < count; ++i) n += keyOf(layer.segs[i]) == feature;
    if (!n) return nullptr;
    auto m = std::make_shared<MeshData>();
    m->verts.reserve(n * 6 * 6);
    m->indices.reserve(n * 12);
    m->triFace.reserve(n * 4);
    const float top = layer.z, h = std::max(layer.height, 0.02f);
    const float bot = top - h * 0.55f;   // the layer below stays clearly under the gaps between lines (no flicker)
    // A low ridge for each line: two bottom edges and a crest. The normals spread from straight up
    // at the crest to sideways at the edges, so it shades round like a real extruded line.
    for (size_t i = 0; i < count; ++i) {
        const ToolSeg& s = layer.segs[i];
        if (keyOf(s) != feature) continue;
        if (featureIsMarker(s.feature)) {
            // A marker: a small diamond at the spot (retracts, seams, filament changes).
            float r = 0.32f;
            uint32_t base = (uint32_t)(m->verts.size() / 6);
            const float px[6][3] = {{r, 0, 0}, {-r, 0, 0}, {0, r, 0}, {0, -r, 0}, {0, 0, r}, {0, 0, -r}};
            for (auto& q : px) m->verts.insert(m->verts.end(), {s.x0 + q[0], s.y0 + q[1], top + q[2], q[0] / r, q[1] / r, q[2] / r});
            const uint32_t tri[24] = {0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5};
            for (uint32_t k : tri) m->indices.push_back(base + k);
            for (int k = 0; k < 8; ++k) m->triFace.push_back(0);
            m->bounds.add(glm::vec3(s.x0 - r, s.y0 - r, top - r));
            m->bounds.add(glm::vec3(s.x0 + r, s.y0 + r, top + r));
            continue;
        }
        float dx = s.x1 - s.x0, dy = s.y1 - s.y0, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) continue;
        float ux = dx / len, uy = dy / len;
        float hw = s.width * 0.5f;
        float sx = -uy * hw, sy = ux * hw;   // to the left side
        // Lines meet without gaps: stretch each end by half a width.
        float ex = ux * hw * 0.5f, ey = uy * hw * 0.5f;
        float ax = s.x0 - ex, ay = s.y0 - ey, bx = s.x1 + ex, by = s.y1 + ey;
        uint32_t base = (uint32_t)(m->verts.size() / 6);
        auto v = [&](float px, float py, float pz, float nx, float ny, float nz) {
            m->verts.insert(m->verts.end(), {px, py, pz, nx, ny, nz});
        };
        const float k = 0.7071f;
        v(ax + sx, ay + sy, bot, -uy * k, ux * k, k);   // 0 left, start
        v(ax, ay, top, 0, 0, 1);                        // 1 crest, start
        v(ax - sx, ay - sy, bot, uy * k, -ux * k, k);   // 2 right, start
        v(bx + sx, by + sy, bot, -uy * k, ux * k, k);   // 3 left, end
        v(bx, by, top, 0, 0, 1);                        // 4 crest, end
        v(bx - sx, by - sy, bot, uy * k, -ux * k, k);   // 5 right, end
        const uint32_t t[12] = {0, 4, 3, 0, 1, 4, 1, 5, 4, 1, 2, 5};
        for (uint32_t i : t) m->indices.push_back(base + i);
        for (int i = 0; i < 4; ++i) m->triFace.push_back(0);
        m->bounds.add(glm::vec3(std::min(ax, bx) - hw, std::min(ay, by) - hw, bot));
        m->bounds.add(glm::vec3(std::max(ax, bx) + hw, std::max(ay, by) + hw, top));
    }
    m->faceCount = 1;
    return m;
}

std::vector<std::pair<float, float>> parsePrintableArea(const std::string& resolvedMachineJson) {
    std::vector<std::pair<float, float>> out;
    json::Value v = json::Value::parse(resolvedMachineJson);
    const json::Value& a = v["printable_area"];
    auto addPt = [&](const std::string& s) {
        size_t x = s.find('x');
        if (x == std::string::npos) return;
        // Not strtof on the whole text: "0x180" would read as the hex number 384.
        out.push_back({std::strtof(s.substr(0, x).c_str(), nullptr), std::strtof(s.substr(x + 1).c_str(), nullptr)});
    };
    if (a.isArray()) {
        for (auto& p : a.items()) addPt(p.asString());
    } else if (a.isString()) {
        std::string s = a.asString();
        size_t st = 0;
        while (st < s.size()) {
            size_t c = s.find(',', st);
            addPt(s.substr(st, c == std::string::npos ? std::string::npos : c - st));
            if (c == std::string::npos) break;
            st = c + 1;
        }
    }
    if (out.size() < 3) out.clear();
    return out;
}

namespace {

// A flat slab from a convex outline: top fan, rim; z0 bottom, z1 top.
void addSlab(MeshData& m, const std::vector<std::pair<float, float>>& outline, float z0, float z1, bool edges) {
    if (outline.size() < 3) return;
    uint32_t base = (uint32_t)(m.verts.size() / 6);
    float cx = 0, cy = 0;
    for (auto& [x, y] : outline) { cx += x; cy += y; }
    cx /= outline.size(); cy /= outline.size();
    m.verts.insert(m.verts.end(), {cx, cy, z1, 0, 0, 1});
    for (auto& [x, y] : outline) m.verts.insert(m.verts.end(), {x, y, z1, 0, 0, 1});
    uint32_t n = (uint32_t)outline.size();
    for (uint32_t i = 0; i < n; ++i) {
        m.indices.insert(m.indices.end(), {base, base + 1 + i, base + 1 + (i + 1) % n});
        m.triFace.push_back(0);
    }
    for (size_t i = 0; i < outline.size(); ++i) {
        auto [ax, ay] = outline[i];
        auto [bx, by] = outline[(i + 1) % outline.size()];
        float nx = by - ay, ny = -(bx - ax), l = std::sqrt(nx * nx + ny * ny);
        if (l < 1e-6f) continue;
        nx /= l; ny /= l;
        uint32_t b = (uint32_t)(m.verts.size() / 6);
        m.verts.insert(m.verts.end(), {ax, ay, z1, nx, ny, 0, bx, by, z1, nx, ny, 0, bx, by, z0, nx, ny, 0, ax, ay, z0, nx, ny, 0});
        m.indices.insert(m.indices.end(), {b, b + 3, b + 2, b, b + 2, b + 1});
        m.triFace.push_back(0);
        m.triFace.push_back(0);
        if (edges) {
            m.edgeVerts.insert(m.edgeVerts.end(), {ax, ay, z1, bx, by, z1});
            m.edgeCount++;
        }
    }
    for (auto& [x, y] : outline) { m.bounds.add(glm::vec3(x, y, z0)); m.bounds.add(glm::vec3(x, y, z1)); }
    m.faceCount = 1;
}

} // namespace

MeshPtr bedMesh(const std::vector<std::pair<float, float>>& outline) {
    auto m = std::make_shared<MeshData>();
    if (outline.size() < 3) return m;
    // The plate itself: the printable area, a thin sheet on the bed model (like OrcaSlicer's plate).
    addSlab(*m, outline, -0.4f, -0.06f, false);
    // Bounds stay the printable area exactly (placement and "off the plate" use them).
    m->bounds = Aabb();
    for (auto& [x, y] : outline) { m->bounds.add(glm::vec3(x, y, -0.4f)); m->bounds.add(glm::vec3(x, y, -0.06f)); }
    return m;
}

} // namespace kst::print
