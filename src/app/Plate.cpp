// The plate in the Slice tab's Prepare view, worked like OrcaSlicer's: every part sits on a build
// plate. Click to pick, Shift or Ctrl click to add, Shift drag a box to pick many. Drag to move,
// M / R / S for the move, rotate and scale handles, F to lay on a face, Delete, Ctrl+A, arrow keys,
// right click for the menu. Arrange packs a plate. Slicing uses exactly this placement (OrcaSlicer's
// own arrange stays off), so the preview lines up with what was placed.
//
// Each part carries its own geometry (mesh, B-rep, color), so a tab keeps working whatever design
// is open, and one tab can hold parts of several designs (drag a tab onto another to combine them).
#include "app/App.h"
#include "core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace kst {

using namespace ui;

namespace {

glm::mat4 rotationBetween(glm::vec3 a, glm::vec3 b) {
    a = glm::normalize(a);
    b = glm::normalize(b);
    float d = glm::clamp(glm::dot(a, b), -1.f, 1.f);
    glm::vec3 axis = glm::cross(a, b);
    if (glm::length(axis) < 1e-6f) {
        if (d > 0) return glm::mat4(1.f);
        glm::vec3 any = std::abs(a.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        return glm::rotate(glm::mat4(1.f), glm::pi<float>(), glm::normalize(glm::cross(a, any)));
    }
    return glm::rotate(glm::mat4(1.f), std::acos(d), glm::normalize(axis));
}

float distSegPx(ImVec2 p, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float u = l2 > 1e-6f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
    return std::hypot(p.x - (a.x + dx * u), p.y - (a.y + dy * u));
}

} // namespace

Aabb App::plateBedRect() const {
    if (slice_.bed && slice_.bed->bounds.valid()) return slice_.bed->bounds;
    Aabb a;
    a.add(glm::vec3(0, 0, 0));
    a.add(glm::vec3(256, 256, 0));
    return a;
}

std::string App::plateTabKey() const {
    return sliceActive_ >= 0 && sliceActive_ < (int)sliceDocs_.size() ? sliceDocs_[(size_t)sliceActive_].key : sliceDocKey();
}

bool App::plateInFilter(uint32_t oid) const {
    if (slice_.plateOnly.empty()) return true;
    auto it = slice_.plate.find(oid);
    if (it == slice_.plate.end()) return false;
    if (it->second.src != sliceDocKey()) return true;   // parts brought in from other designs always show
    return it->second.bodyId && slice_.plateOnly.count(it->second.bodyId) != 0;
}

void App::plateSync() {
    // Parts of the open design follow it (moved, filleted, recolored); its new bodies join this
    // design's own tab and its gone ones leave. Parts of other designs keep what they carry.
    const std::string cur = sliceDocKey();
    const bool own = plateOwns(cur);
    for (auto it = slice_.plateOnly.begin(); it != slice_.plateOnly.end();)
        it = doc_.bodies.count(*it) ? std::next(it) : slice_.plateOnly.erase(it);
    std::map<uint64_t, uint32_t> byStable;
    for (auto& [oid, o] : slice_.plate)
        if (o.src == cur) byStable[o.stable] = oid;
    std::set<uint32_t> seen;
    bool added = false;
    std::vector<std::pair<uint32_t, int>> newOnes;
    const bool rebuilding = recomputeQueuedFrom_ >= 0 || recomputeRunning_ || recomputeResults_;
    for (auto& [id, b] : doc_.bodies) {
        if (!bodyShown(b)) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit == doc_.parts.end() || !pit->second.mesh || pit->second.mesh->indices.empty()) continue;
        uint64_t st = Document::overrideKey(b.featureId, doc_.bodySlot(id));
        auto f = byStable.find(st);
        uint32_t oid = 0;
        if (f != byStable.end()) {
            oid = f->second;
        } else {
            if (!own) continue;
            if (!slice_.plateOnly.empty() && !slice_.plateOnly.count(id)) continue;
            oid = slice_.nextObj++;
            auto& o = slice_.plate[oid];
            auto mem = slice_.ex.gone.find(cur + "|" + std::to_string(st));
            if (mem != slice_.ex.gone.end()) {
                o = mem->second;   // it was here before (hidden, undone, rebuilt): its place and settings come back
                slice_.ex.gone.erase(mem);
            } else {
                o.pos.push_back(glm::vec2(plateBedRect().center()));
                o.plateOf.push_back(slice_.activePlate);
                newOnes.push_back({oid, 0});
                added = true;
            }
            o.src = cur;
            o.stable = st;
            byStable[st] = oid;
        }
        seen.insert(oid);
        auto& o = slice_.plate[oid];
        if (o.mesh != pit->second.mesh || o.bodyXform != b.xform) o.cacheValid = false;
        o.bodyId = id;
        o.name = b.name;
        o.mesh = pit->second.mesh;
        o.shape = pit->second.shape;
        o.bodyXform = b.xform;
        o.color = bodyPrintColor(b);
    }
    // This design's parts whose body is gone (or hidden) leave its own tab. In a tab they were
    // brought into, they stay as they were.
    for (auto it = slice_.plate.begin(); it != slice_.plate.end();) {
        auto& o = it->second;
        if (o.src == cur && !seen.count(it->first)) {
            if (rebuilding && o.mesh) { o.bodyId = 0; ++it; continue; }   // the design is rebuilding: it comes back
            if (own || !o.mesh) {
                if (o.mesh) slice_.ex.gone[cur + "|" + std::to_string(o.stable)] = o;   // remembered, in case it returns
                it = slice_.plate.erase(it);
                continue;
            }
            o.bodyId = 0;
        }
        if (!o.mesh) { it = slice_.plate.erase(it); continue; }
        ++it;
    }
    for (auto it = slice_.plateSelSet.begin(); it != slice_.plateSelSet.end();)
        it = slice_.plate.count(it->first) ? std::next(it) : slice_.plateSelSet.erase(it);
    if (!slice_.plate.count(slice_.plateSel)) slice_.plateSel = 0;
    sliceAutoFils();
    if (added) {
        // The first parts of a tab are packed; later ones find free spots and leave the rest alone.
        if ((int)newOnes.size() == (int)slice_.plate.size()) plateArrange();
        else platePlaceNew(newOnes);
    }
}

const Aabb& App::plateCore(uint32_t oid) {
    auto& o = slice_.plate[oid];
    if (o.cacheValid) return o.core;
    o.core = Aabb();
    if (o.mesh) {
        const MeshData& m = *o.mesh;
        glm::vec3 c = m.bounds.transformed(o.bodyXform).center();
        glm::mat4 core = glm::rotate(glm::mat4(1.f), glm::radians(o.rotZ), glm::vec3(0, 0, 1)) * o.lay *
                         glm::scale(glm::mat4(1.f), o.scale) * glm::translate(glm::mat4(1.f), -c) * o.bodyXform;
        // Exact: every vertex (a rotated box would leave the part floating above the bed).
        for (size_t i = 0; i + 2 < m.verts.size(); i += 6) o.core.add(glm::vec3(core * glm::vec4(m.verts[i], m.verts[i + 1], m.verts[i + 2], 1.f)));
    }
    o.cacheValid = true;
    return o.core;
}

glm::mat4 App::plateXform(uint32_t oid, int copy) {
    auto it = slice_.plate.find(oid);
    if (it == slice_.plate.end() || !it->second.mesh) return glm::mat4(1.f);
    auto& o = it->second;
    glm::vec3 c = o.mesh->bounds.transformed(o.bodyXform).center();
    glm::mat4 core = glm::rotate(glm::mat4(1.f), glm::radians(o.rotZ), glm::vec3(0, 0, 1)) * o.lay *
                     glm::scale(glm::mat4(1.f), o.scale) * glm::translate(glm::mat4(1.f), -c) * o.bodyXform;
    const Aabb& cb = plateCore(oid);
    int kk = o.pos.empty() ? 0 : std::clamp(copy, 0, (int)o.pos.size() - 1);
    glm::vec2 p = o.pos.empty() ? glm::vec2(0) : o.pos[(size_t)kk];
    p += plateOffset(plateOfCopy(o, kk));   // its build plate
    glm::vec3 shift(p.x - cb.center().x, p.y - cb.center().y, cb.valid() ? -cb.min.z : 0.f);
    return glm::translate(glm::mat4(1.f), shift) * core;
}

bool App::plateInstanceOnBed(uint32_t oid, int copy) {
    auto& o = slice_.plate[oid];
    const Aabb& cb = plateCore(oid);
    if (!cb.valid() || o.pos.empty()) return true;
    glm::vec2 p = o.pos[(size_t)std::clamp(copy, 0, (int)o.pos.size() - 1)];
    glm::vec2 half = glm::vec2(cb.max - cb.min) * 0.5f;
    Aabb bed = plateBedRect();
    const float e = 0.01f;
    return p.x - half.x >= bed.min.x - e && p.x + half.x <= bed.max.x + e && p.y - half.y >= bed.min.y - e && p.y + half.y <= bed.max.y + e;
}

// Packs parts in rows (biggest first, 6 mm apart), plate after plate. Parts that do not fit go to the
// next plate that is not locked, and new plates are made when every plate is full (like Orca).
void App::platePack(std::vector<std::pair<uint32_t, int>> items, std::vector<int> plates) {
    struct Item { uint32_t body; int copy; float w, h; };
    std::vector<Item> list;
    for (auto [id, k] : items) {
        const Aabb& cb = plateCore(id);
        if (!cb.valid()) continue;
        list.push_back({id, k, cb.max.x - cb.min.x, cb.max.y - cb.min.y});
    }
    if (list.empty()) return;
    std::stable_sort(list.begin(), list.end(), [](const Item& a, const Item& b) { return a.h * a.w > b.h * b.w; });
    Aabb bed = plateBedRect();
    const float gap = 6.f;
    float bw = bed.max.x - bed.min.x, bh = bed.max.y - bed.min.y;
    size_t pi = 0;
    auto nextPlate = [&]() {
        if (pi + 1 < plates.size()) return plates[++pi];
        if (slice_.plates >= 36) return plates[pi];
        plateNewPlate(false);
        plates.push_back(slice_.plates - 1);
        ++pi;
        return plates[pi];
    };
    if (plates.empty()) plates.push_back(0);
    int cur = plates[0];
    struct Placed { size_t i; glm::vec2 at; };
    std::vector<Placed> onPlate;
    float x = 0, y = 0, rowH = 0, usedW = 0;
    auto flush = [&](int plate) {
        // Center what went onto this plate.
        float usedH = y + rowH;
        glm::vec2 off(bed.center().x - usedW * 0.5f, bed.center().y - usedH * 0.5f);
        for (auto& p : onPlate) {
            auto& o = slice_.plate[list[p.i].body];
            int k = list[p.i].copy;
            if ((int)o.plateOf.size() < (int)o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
            o.pos[(size_t)k] = p.at + off;
            o.plateOf[(size_t)k] = plate;
        }
        onPlate.clear();
        x = y = rowH = usedW = 0;
    };
    for (size_t i = 0; i < list.size(); ++i) {
        float w = list[i].w, h = list[i].h;
        if (x > 0 && x + w > bw) { y += rowH + gap; x = 0; rowH = 0; }
        if (!onPlate.empty() && y + h > bh) {   // this plate is full
            flush(cur);
            cur = nextPlate();
        }
        onPlate.push_back({i, glm::vec2(x + w * 0.5f, y + h * 0.5f)});
        x += w + gap;
        rowH = std::max(rowH, h);
        usedW = std::max(usedW, x - gap);
    }
    flush(cur);
    sceneDirty_ = true;
}

void App::plateArrange() {
    // Everything on plates that are not locked, packed again (new plates when it does not fit).
    std::vector<int> plates;
    for (int p = 0; p < slice_.plates; ++p)
        if (!(p < (int)slice_.ex.plateLocked.size() && slice_.ex.plateLocked[(size_t)p])) plates.push_back(p);
    if (plates.empty()) return;
    std::set<int> free(plates.begin(), plates.end());
    std::vector<std::pair<uint32_t, int>> items;
    for (auto& [id, o] : slice_.plate) {
        if (!o.onPlate || !plateInFilter(id)) continue;
        for (int k = 0; k < (int)o.pos.size(); ++k)
            if (free.count(plateOfCopy(o, k))) items.push_back({id, k});
    }
    platePack(items, plates);
}

void App::plateArrangeOne(int plateIdx) {
    if (plateIdx < (int)slice_.ex.plateLocked.size() && slice_.ex.plateLocked[(size_t)plateIdx]) return;
    std::vector<std::pair<uint32_t, int>> items;
    for (auto& [id, o] : slice_.plate) {
        if (!o.onPlate || !plateInFilter(id)) continue;
        for (int k = 0; k < (int)o.pos.size(); ++k)
            if (plateOfCopy(o, k) == plateIdx) items.push_back({id, k});
    }
    // What does not fit goes to plates after it that are free, or new ones.
    std::vector<int> plates = {plateIdx};
    for (int p = 0; p < slice_.plates; ++p) {
        if (p == plateIdx || (p < (int)slice_.ex.plateLocked.size() && slice_.ex.plateLocked[(size_t)p])) continue;
        bool empty = true;
        for (auto& [id, o] : slice_.plate)
            if (o.onPlate)
                for (int k = 0; k < (int)o.pos.size(); ++k) empty &= plateOfCopy(o, k) != p;
        if (empty) plates.push_back(p);
    }
    platePack(items, plates);
}

void App::platePlaceNew(const std::vector<std::pair<uint32_t, int>>& items) {
    // New parts find a free spot without moving the ones already placed: the plate in front first,
    // then the others, then a new plate.
    Aabb bed = plateBedRect();
    const float gap = 6.f;
    for (auto [id, k] : items) {
        const Aabb& cb = plateCore(id);
        if (!cb.valid()) continue;
        glm::vec2 half = glm::vec2(cb.max - cb.min) * 0.5f;
        auto& o = slice_.plate[id];
        if ((int)o.plateOf.size() < (int)o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
        std::vector<int> order = {slice_.activePlate};
        for (int p = 0; p < slice_.plates; ++p) if (p != slice_.activePlate) order.push_back(p);
        bool done = false;
        for (int p : order) {
            if (p < (int)slice_.ex.plateLocked.size() && slice_.ex.plateLocked[(size_t)p]) continue;
            // Boxes already on this plate.
            std::vector<std::pair<glm::vec2, glm::vec2>> boxes;
            for (auto& [j, q] : slice_.plate) {
                if (!q.onPlate || !plateInFilter(j)) continue;
                const Aabb& qb = plateCore(j);
                if (!qb.valid()) continue;
                glm::vec2 qh = glm::vec2(qb.max - qb.min) * 0.5f;
                for (int c = 0; c < (int)q.pos.size(); ++c) {
                    if (j == id && c == k) continue;
                    if (plateOfCopy(q, c) != p) continue;
                    boxes.push_back({q.pos[(size_t)c] - qh, q.pos[(size_t)c] + qh});
                }
            }
            auto fits = [&](glm::vec2 c) {
                glm::vec2 lo = c - half, hi = c + half;
                if (lo.x < bed.min.x || lo.y < bed.min.y || hi.x > bed.max.x || hi.y > bed.max.y) return false;
                for (auto& [blo, bhi] : boxes)
                    if (lo.x < bhi.x + gap && hi.x > blo.x - gap && lo.y < bhi.y + gap && hi.y > blo.y - gap) return false;
                return true;
            };
            // Spots from the middle outwards, so the first part lands in the center.
            glm::vec2 ctr(bed.center());
            float step = 4.f, best = 1e30f;
            glm::vec2 bestAt(0);
            for (float yy = bed.min.y + half.y; yy <= bed.max.y - half.y + 0.01f; yy += step)
                for (float xx = bed.min.x + half.x; xx <= bed.max.x - half.x + 0.01f; xx += step) {
                    glm::vec2 c(xx, yy);
                    float d = glm::length(c - ctr);
                    if (d < best && fits(c)) { best = d; bestAt = c; }
                }
            if (best < 1e29f) {
                o.pos[(size_t)k] = bestAt;
                o.plateOf[(size_t)k] = p;
                done = true;
                break;
            }
        }
        if (!done) {
            plateNewPlate(false);
            o.pos[(size_t)k] = glm::vec2(bed.center());
            o.plateOf[(size_t)k] = slice_.plates - 1;
        }
    }
    sceneDirty_ = true;
}

void App::plateLayFlat(uint32_t oid, int face) {
    auto it = slice_.plate.find(oid);
    if (it == slice_.plate.end() || !it->second.mesh) return;
    auto& o = it->second;
    const MeshData& m = *o.mesh;
    // Normals as they point now (the up axis turn does not change which way is down).
    glm::mat4 cur = o.lay * glm::scale(glm::mat4(1.f), o.scale) * o.bodyXform;
    auto vtx = [&](uint32_t i) { return glm::vec3(cur * glm::vec4(m.verts[i * 6], m.verts[i * 6 + 1], m.verts[i * 6 + 2], 1.f)); };
    glm::vec3 n(0);
    if (face >= 0) {
        for (size_t t = 0; t * 3 + 2 < m.indices.size(); ++t) {
            if (t >= m.triFace.size() || (int)m.triFace[t] != face) continue;
            glm::vec3 a = vtx(m.indices[t * 3]), b2 = vtx(m.indices[t * 3 + 1]), c = vtx(m.indices[t * 3 + 2]);
            n += glm::cross(b2 - a, c - a);   // area weighted
        }
    } else {
        // The biggest flat area pointing one way (normals grouped to about 1 degree).
        std::map<std::tuple<int, int, int>, std::pair<float, glm::vec3>> groups;
        for (size_t t = 0; t * 3 + 2 < m.indices.size(); ++t) {
            glm::vec3 a = vtx(m.indices[t * 3]), b2 = vtx(m.indices[t * 3 + 1]), c = vtx(m.indices[t * 3 + 2]);
            glm::vec3 cr = glm::cross(b2 - a, c - a);
            float area = glm::length(cr);
            if (area < 1e-9f) continue;
            glm::vec3 u = cr / area;
            auto key = std::make_tuple((int)std::lround(u.x * 60), (int)std::lround(u.y * 60), (int)std::lround(u.z * 60));
            auto& g = groups[key];
            g.first += area;
            g.second += cr;
        }
        float best = 0;
        for (auto& [k, g] : groups)
            if (g.first > best) { best = g.first; n = g.second; }
    }
    if (glm::length(n) < 1e-9f) return;
    o.lay = rotationBetween(n, glm::vec3(0, 0, -1)) * o.lay;
    o.cacheValid = false;
    sceneDirty_ = true;
}

// ===================================================================== selection on the plate

bool App::plateIsSel(uint32_t oid, int copy) const {
    return slice_.plateSelSet.count({oid, copy}) > 0 || (slice_.plateSel == oid && slice_.plateSelCopy == copy && slice_.plateSelSet.empty());
}

void App::plateSelect(uint32_t oid, int copy, bool add) {
    if (!add) slice_.plateSelSet.clear();
    if (oid) {
        if (add && slice_.plateSelSet.count({oid, copy})) {
            slice_.plateSelSet.erase({oid, copy});   // Ctrl / Shift click a picked part: drop it
            if (slice_.plateSel == oid) {
                slice_.plateSel = slice_.plateSelSet.empty() ? 0 : slice_.plateSelSet.begin()->first;
                slice_.plateSelCopy = slice_.plateSelSet.empty() ? 0 : slice_.plateSelSet.begin()->second;
            }
        } else {
            slice_.plateSelSet.insert({oid, copy});
            slice_.plateSel = oid;
            slice_.plateSelCopy = copy;
        }
    } else if (!add) {
        slice_.plateSel = 0;
    }
    sceneDirty_ = true;
}

std::vector<std::pair<uint32_t, int>> App::plateSelection() const {
    std::vector<std::pair<uint32_t, int>> out(slice_.plateSelSet.begin(), slice_.plateSelSet.end());
    if (out.empty() && slice_.plateSel && slice_.plate.count(slice_.plateSel)) out.push_back({slice_.plateSel, slice_.plateSelCopy});
    return out;
}

glm::vec2 App::plateSelCenter() {
    // Middle of the picked parts, in world (plate row) coordinates.
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (auto [oid, k] : plateSelection()) {
        auto it = slice_.plate.find(oid);
        if (it == slice_.plate.end() || k >= (int)it->second.pos.size()) continue;
        glm::vec2 w = it->second.pos[(size_t)k] + plateOffset(plateOfCopy(it->second, k));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
    return lo.x > hi.x ? glm::vec2(0) : (lo + hi) * 0.5f;
}

void App::plateDeleteSelection() {
    // Copies go first; a part's last copy takes the part off the plate.
    auto sel = plateSelection();
    std::sort(sel.begin(), sel.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first < b.first : a.second > b.second; });
    for (auto [oid, k] : sel) {
        auto it = slice_.plate.find(oid);
        if (it == slice_.plate.end()) continue;
        auto& o = it->second;
        if (o.pos.size() > 1 && k < (int)o.pos.size()) {
            o.pos.erase(o.pos.begin() + k);
            if (k < (int)o.plateOf.size()) o.plateOf.erase(o.plateOf.begin() + k);
        } else if (!plateOwns(o.src)) {
            slice_.plate.erase(it);   // brought in from another design: gone from this tab
        } else {
            o.onPlate = false;
        }
    }
    plateSelect(0, 0, false);
}

void App::plateAddCopies(int n) {
    for (auto [oid, k] : plateSelection()) {
        auto it = slice_.plate.find(oid);
        if (it == slice_.plate.end()) continue;
        auto& o = it->second;
        if ((int)o.plateOf.size() < (int)o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
        int pl = plateOfCopy(o, k);
        for (int i = 0; i < n; ++i) {
            o.pos.push_back(o.pos.empty() ? glm::vec2(plateBedRect().center()) : o.pos[(size_t)std::min(k, (int)o.pos.size() - 1)] + glm::vec2(10.f * (i + 1)));
            o.plateOf.push_back(pl);
        }
        plateArrangeOne(pl);
    }
}

// ===================================================================== handles (M, R, S)

bool App::plateGizmoGeom(ImVec2& c, float& r, ImVec2& ax, ImVec2& ay) {
    if (!slice_.gizmo || plateSelection().empty()) return false;
    glm::vec2 cw = slice_.gizmoDrag ? slice_.gizmoCenter : plateSelCenter();
    if (!worldToScreen(glm::dvec3(cw.x, cw.y, 0.0), c)) return false;
    // A screen size that does not change with zoom.
    ImVec2 px, py;
    const float L = 70 * uiScale_;
    if (!worldToScreen(glm::dvec3(cw.x + 1.0, cw.y, 0.0), px) || !worldToScreen(glm::dvec3(cw.x, cw.y + 1.0, 0.0), py)) return false;
    auto dirTo = [&](ImVec2 p) {
        float dx = p.x - c.x, dy = p.y - c.y, l = std::max(1e-4f, std::hypot(dx, dy));
        return ImVec2(c.x + dx / l * L, c.y + dy / l * L);
    };
    ax = dirTo(px);
    ay = dirTo(py);
    r = L * 0.85f;
    return true;
}

void App::drawPlateGizmo() {
    ImVec2 c, ax, ay;
    float r;
    if (!plateGizmoGeom(c, r, ax, ay)) return;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float S = uiScale_;
    ImVec2 m = ImGui::GetIO().MousePos;
    if (slice_.gizmo == 1) {
        bool hx = slice_.gizmoDrag == 1 || (!slice_.gizmoDrag && distSegPx(m, c, ax) < 8 * S);
        bool hy = slice_.gizmoDrag == 2 || (!slice_.gizmoDrag && !hx && distSegPx(m, c, ay) < 8 * S);
        auto arrow = [&](ImVec2 tip, ImU32 col, bool hot) {
            float w = hot ? 4.f * S : 2.6f * S;
            fg->AddLine(c, tip, col, w);
            float dx = tip.x - c.x, dy = tip.y - c.y, l = std::max(1e-4f, std::hypot(dx, dy));
            dx /= l; dy /= l;
            float h = 12 * S;
            fg->AddTriangleFilled(ImVec2(tip.x + dx * h, tip.y + dy * h), ImVec2(tip.x - dy * h * 0.5f, tip.y + dx * h * 0.5f),
                                  ImVec2(tip.x + dy * h * 0.5f, tip.y - dx * h * 0.5f), col);
        };
        arrow(ax, hx ? IM_COL32(255, 140, 120, 255) : IM_COL32(232, 72, 72, 255), hx);
        arrow(ay, hy ? IM_COL32(150, 240, 140, 255) : IM_COL32(96, 200, 90, 255), hy);
        fg->AddCircleFilled(c, 7 * S, slice_.gizmoDrag == 3 ? col::Print : IM_COL32(240, 240, 240, 230), 20);
    } else if (slice_.gizmo == 2) {
        bool hot = slice_.gizmoDrag == 3 || std::abs(std::hypot(m.x - c.x, m.y - c.y) - r) < 9 * S;
        fg->AddCircle(c, r, hot ? col::Print : IM_COL32(72, 140, 255, 255), 64, hot ? 4.f * S : 2.6f * S);
        if (slice_.gizmoDrag == 3) {
            float a = 0;
            for (auto& [oid, r0] : slice_.gizmoRot0) { a = slice_.plate.count(oid) ? slice_.plate[oid].rotZ - r0 : 0; break; }
            char t[32];
            std::snprintf(t, sizeof t, "%.1f deg", a);
            fg->AddText(ImVec2(c.x + r + 8 * S, c.y - 8 * S), IM_COL32_WHITE, t);
        }
    } else if (slice_.gizmo == 3) {
        ImVec2 h(c.x + r * 0.7f, c.y - r * 0.7f);
        bool hot = slice_.gizmoDrag == 3 || (std::abs(m.x - h.x) < 9 * S && std::abs(m.y - h.y) < 9 * S);
        fg->AddLine(c, h, IM_COL32(220, 220, 220, 200), 2.f * S);
        fg->AddRectFilled(ImVec2(h.x - 7 * S, h.y - 7 * S), ImVec2(h.x + 7 * S, h.y + 7 * S), hot ? col::Print : IM_COL32(240, 200, 60, 255), 2 * S);
        if (slice_.gizmoDrag == 3 && slice_.plateSel && slice_.plate.count(slice_.plateSel)) {
            char t[32];
            std::snprintf(t, sizeof t, "%.0f%%", slice_.plate[slice_.plateSel].scale.x * 100.f);
            fg->AddText(ImVec2(h.x + 12 * S, h.y - 8 * S), IM_COL32_WHITE, t);
        }
    }
}

// ===================================================================== mouse and keys on the plate

bool App::handlePlateInput(bool hovered) {
    // True when the plate took the mouse (the view does not orbit then).
    ImGuiIO& io = ImGui::GetIO();
    const float S = uiScale_;
    auto hit = [&](ImVec2 m, glm::vec2& out) {
        glm::dvec3 o, d;
        if (!rayAt(m, o, d) || std::abs(d.z) < 1e-9) return false;
        double t = -o.z / d.z;
        if (t <= 0) return false;
        out = glm::vec2(o.x + d.x * t, o.y + d.y * t);
        return true;
    };
    auto under = [&]() -> std::pair<uint32_t, int> {
        if (hoverBody_ < 1000) return {0, 0};
        size_t i = hoverBody_ - 1000;
        return i < slice_.pickMap.size() ? slice_.pickMap[i] : std::pair<uint32_t, int>{0, 0};
    };
    auto startGroup = [&](glm::vec2 hitW) {
        slice_.gizmoStart = hitW;
        slice_.gizmoCenter = plateSelCenter();
        slice_.gizmoPos0.clear();
        slice_.gizmoRot0.clear();
        slice_.gizmoScl0.clear();
        for (auto [oid, k] : plateSelection()) {
            auto& o = slice_.plate[oid];
            if (k < (int)o.pos.size()) slice_.gizmoPos0[{oid, k}] = o.pos[(size_t)k] + plateOffset(plateOfCopy(o, k));
            slice_.gizmoRot0[oid] = o.rotZ;
            slice_.gizmoScl0[oid] = o.scale;
        }
    };
    auto placeAt = [&](uint32_t oid, int k, glm::vec2 w) {
        auto& o = slice_.plate[oid];
        if (k >= (int)o.pos.size()) return;
        int pl = plateAt(w);
        if ((int)o.plateOf.size() < (int)o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
        o.plateOf[(size_t)k] = pl;
        o.pos[(size_t)k] = w - plateOffset(pl);
    };

    // ---- dragging: parts, handles, or a selection box
    if (slice_.plateDrag || slice_.gizmoDrag) {
        glm::vec2 p;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && hit(io.MousePos, p)) {
            glm::vec2 d = p - slice_.gizmoStart;
            bool moving = slice_.plateDrag || slice_.gizmo == 1;
            if (moving) {
                if (slice_.gizmoDrag == 1) d.y = 0;
                if (slice_.gizmoDrag == 2) d.x = 0;
                for (auto& [key, w0] : slice_.gizmoPos0) placeAt(key.first, key.second, w0 + d);
                if (!slice_.gizmoPos0.empty()) slice_.activePlate = plateAt(slice_.gizmoPos0.begin()->second + d);
            } else if (slice_.gizmo == 2) {
                glm::vec2 c = slice_.gizmoCenter, a = slice_.gizmoStart - c, b = p - c;
                float ang = glm::degrees(std::atan2(b.y, b.x) - std::atan2(a.y, a.x));
                if (io.KeyShift) ang = std::round(ang / 15.f) * 15.f;
                float rr = glm::radians(ang), cs = std::cos(rr), sn = std::sin(rr);
                for (auto& [key, w0] : slice_.gizmoPos0) {
                    glm::vec2 v = w0 - c;
                    placeAt(key.first, key.second, c + glm::vec2(v.x * cs - v.y * sn, v.x * sn + v.y * cs));
                }
                for (auto& [oid, r0] : slice_.gizmoRot0) {
                    auto& o = slice_.plate[oid];
                    o.rotZ = std::fmod(r0 + ang + 720.f, 360.f);
                    o.cacheValid = false;
                }
            } else if (slice_.gizmo == 3) {
                float d0 = std::max(1.f, glm::length(slice_.gizmoStart - slice_.gizmoCenter));
                float f = std::clamp(glm::length(p - slice_.gizmoCenter) / d0, 0.02f, 50.f);
                if (io.KeyShift) f = std::max(0.05f, std::round(f * 20.f) / 20.f);
                for (auto& [oid, s0] : slice_.gizmoScl0) {
                    auto& o = slice_.plate[oid];
                    o.scale = s0 * f;
                    o.cacheValid = false;
                }
            }
            sceneDirty_ = true;
            activity();
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { slice_.plateDrag = false; slice_.gizmoDrag = 0; }
        return true;
    }
    if (slice_.boxSel) {
        ImVec2 a = slice_.boxA, b = io.MousePos;
        ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        ImGui::GetForegroundDrawList()->AddRectFilled(lo, hi, IM_COL32(255, 107, 53, 40));
        ImGui::GetForegroundDrawList()->AddRect(lo, hi, col::Print);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            slice_.boxSel = false;
            if (!io.KeyCtrl) plateSelect(0, 0, false);
            for (auto& [oid, o] : slice_.plate) {
                if (!o.onPlate || !plateInFilter(oid)) continue;
                for (int k = 0; k < (int)o.pos.size(); ++k) {
                    glm::vec2 w = o.pos[(size_t)k] + plateOffset(plateOfCopy(o, k));
                    ImVec2 q;
                    if (worldToScreen(glm::dvec3(w.x, w.y, 0.0), q) && q.x >= lo.x && q.x <= hi.x && q.y >= lo.y && q.y <= hi.y) plateSelect(oid, k, true);
                }
            }
        }
        activity();
        return true;
    }
    if (!hovered) return false;

    // ---- painting (a brush: hold the button and sweep over faces) and measuring
    if (slice_.paintMode) {
        bool l = ImGui::IsMouseDown(ImGuiMouseButton_Left), r = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        auto [body, copy] = under();
        if ((l || r) && body && hoverFace_ > 0) {
            auto& o = slice_.plate[body];
            uint32_t f = hoverFace_ - 1;
            auto& m = slice_.paintMode == 1 ? o.paintColor : slice_.paintMode == 4 ? o.paintSeam : o.paintSupport;
            int v = slice_.paintMode == 1 ? (l ? slice_.paintFil : 0) : (l ? 1 : 2);
            auto it = m.find(f);
            int was = it == m.end() ? 0 : it->second;
            if (was != v) {
                if (v == 0) m.erase(f);
                else m[f] = v;
                sceneDirty_ = true;
            }
        }
        if (l || r) { activity(); return true; }
        return false;
    }
    if (slice_.measure && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto [body, copy] = under();
        glm::vec3 h;
        if (body && plateRayHit(body, copy, io.MousePos, h)) {
            if (slice_.measurePts.size() >= 2) slice_.measurePts.clear();
            slice_.measurePts.push_back(h);
        }
        return true;
    }

    // ---- clicks
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto [body, copy] = under();
        glm::vec2 p;
        bool onPlane = hit(io.MousePos, p);
        if (slice_.layMode) {
            if (body) {
                plateLayFlat(body, hoverFace_ ? (int)hoverFace_ - 1 : -1);
                plateSelect(body, copy, false);
            }
            slice_.layMode = false;
            return true;
        }
        // A handle under the cursor wins.
        ImVec2 gc, gax, gay;
        float gr;
        if (onPlane && plateGizmoGeom(gc, gr, gax, gay)) {
            ImVec2 m = io.MousePos;
            int g = 0;
            if (slice_.gizmo == 1) {
                if (std::hypot(m.x - gc.x, m.y - gc.y) < 9 * S) g = 3;
                else if (distSegPx(m, gc, gax) < 8 * S) g = 1;
                else if (distSegPx(m, gc, gay) < 8 * S) g = 2;
            } else if (slice_.gizmo == 2) {
                if (std::abs(std::hypot(m.x - gc.x, m.y - gc.y) - gr) < 9 * S) g = 3;
            } else if (slice_.gizmo == 3) {
                ImVec2 h(gc.x + gr * 0.7f, gc.y - gr * 0.7f);
                if (std::abs(m.x - h.x) < 9 * S && std::abs(m.y - h.y) < 9 * S) g = 3;
            }
            if (g) {
                startGroup(p);
                slice_.gizmoDrag = g;
                if (slice_.gizmo == 3) {
                    // Scale by how far the handle goes from the middle (start at the handle).
                    glm::vec2 hw;
                    if (hit(ImVec2(gc.x + gr * 0.7f, gc.y - gr * 0.7f), hw)) slice_.gizmoStart = hw;
                }
                return true;
            }
        }
        if (body) {
            bool add = io.KeyShift || io.KeyCtrl;
            if (add) plateSelect(body, copy, true);
            else if (!plateIsSel(body, copy)) plateSelect(body, copy, false);
            else { slice_.plateSel = body; slice_.plateSelCopy = copy; }
            auto& o = slice_.plate[body];
            slice_.activePlate = plateOfCopy(o, copy);
            if (onPlane && plateIsSel(body, copy)) {
                startGroup(p);
                slice_.plateDrag = true;   // every picked part moves together
            }
            sceneDirty_ = true;
            return true;
        }
        if (io.KeyShift) {   // Shift drag on an empty spot: a selection box
            slice_.boxSel = true;
            slice_.boxA = io.MousePos;
            return true;
        }
        if (!plateSelection().empty()) plateSelect(0, 0, false);
        // A click on an empty spot picks the plate under it (the one Slice plate slices).
        if (slice_.plates > 1 && onPlane) {
            int pl = plateAt(p);
            if (pl != slice_.activePlate) { slice_.activePlate = pl; sceneDirty_ = true; }
        }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto [body, copy] = under();
        if (body) {
            if (!plateIsSel(body, copy)) plateSelect(body, copy, false);
            slice_.ctxOpen = true;
            return true;
        }
    }

    // ---- keys (OrcaSlicer's)
    if (!io.WantTextInput) {
        auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
        bool any = !plateSelection().empty();
        if (io.KeyCtrl && key(ImGuiKey_A)) {
            plateSelect(0, 0, false);
            for (auto& [oid, o] : slice_.plate)
                if (o.onPlate && plateInFilter(oid))
                    for (int k = 0; k < (int)o.pos.size(); ++k) plateSelect(oid, k, true);
        }
        if (any && !io.KeyCtrl) {
            if (key(ImGuiKey_Delete) || key(ImGuiKey_Backspace)) plateDeleteSelection();
            if (key(ImGuiKey_M)) slice_.gizmo = slice_.gizmo == 1 ? 0 : 1;
            if (key(ImGuiKey_R)) slice_.gizmo = slice_.gizmo == 2 ? 0 : 2;
            if (key(ImGuiKey_S)) slice_.gizmo = slice_.gizmo == 3 ? 0 : 3;
            if (key(ImGuiKey_F)) slice_.layMode = true;
            if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) plateAddCopies(1);
            if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) {
                for (auto [oid, k] : plateSelection()) {
                    auto& o = slice_.plate[oid];
                    if (o.pos.size() > 1) { o.pos.pop_back(); if (o.plateOf.size() > o.pos.size()) o.plateOf.resize(o.pos.size()); }
                }
                plateSelect(slice_.plateSel, 0, false);
            }
            // Arrow keys nudge 1 mm (Shift: 10 mm), the way Orca's do.
            float st = io.KeyShift ? 10.f : 1.f;
            glm::vec2 nd(0);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nd.x -= st;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nd.x += st;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nd.y += st;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nd.y -= st;
            if (nd != glm::vec2(0)) {
                for (auto [oid, k] : plateSelection()) {
                    auto& o = slice_.plate[oid];
                    if (k < (int)o.pos.size()) o.pos[(size_t)k] += nd;
                }
                sceneDirty_ = true;
            }
            if (key(ImGuiKey_Escape)) { slice_.gizmo = 0; plateSelect(0, 0, false); }
        }
        if (io.KeyCtrl && key(ImGuiKey_C)) slice_.clip = plateSelection();
        if (io.KeyCtrl && key(ImGuiKey_V) && !slice_.clip.empty()) {
            auto keep = slice_.plateSelSet;
            slice_.plateSelSet = std::set<std::pair<uint32_t, int>>(slice_.clip.begin(), slice_.clip.end());
            plateAddCopies(1);
            slice_.plateSelSet = keep;
        }
    }
    return false;
}

// ===================================================================== right click menu

void App::drawPlateContextMenu() {
    if (slice_.ctxOpen) { ImGui::OpenPopup("##platectx"); slice_.ctxOpen = false; }
    if (!ImGui::BeginPopup("##platectx")) return;
    auto sel = plateSelection();
    std::string what = sel.size() == 1 && slice_.plate.count(sel[0].first) ? slice_.plate[sel[0].first].name : std::to_string(sel.size()) + " parts";
    ImGui::TextDisabled("%s", what.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del")) plateDeleteSelection();
    if (ImGui::MenuItem("Add a copy", "+")) plateAddCopies(1);
    if (ImGui::BeginMenu("Add copies")) {
        for (int n : {2, 3, 4, 5, 8, 10})
            if (ImGui::MenuItem((std::to_string(n) + " more").c_str())) plateAddCopies(n);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Move", "M", slice_.gizmo == 1)) slice_.gizmo = slice_.gizmo == 1 ? 0 : 1;
    if (ImGui::MenuItem("Rotate", "R", slice_.gizmo == 2)) slice_.gizmo = slice_.gizmo == 2 ? 0 : 2;
    if (ImGui::MenuItem("Scale", "S", slice_.gizmo == 3)) slice_.gizmo = slice_.gizmo == 3 ? 0 : 3;
    if (ImGui::MenuItem("Rotate 90 degrees")) {
        std::set<uint32_t> done;
        for (auto [oid, k] : sel)
            if (done.insert(oid).second) { auto& o = slice_.plate[oid]; o.rotZ = std::fmod(o.rotZ + 90.f, 360.f); o.cacheValid = false; }
    }
    if (ImGui::MenuItem("Lay flat (biggest face)")) for (auto [oid, k] : sel) plateLayFlat(oid, -1);
    if (ImGui::MenuItem("Lay on face...", "F")) slice_.layMode = true;
    if (ImGui::MenuItem("Center on its plate")) {
        Aabb b = plateBedRect();
        for (auto [oid, k] : sel) { auto& o = slice_.plate[oid]; if (k < (int)o.pos.size()) o.pos[(size_t)k] = glm::vec2(b.center()); }
    }
    if (ImGui::BeginMenu("Move to plate")) {
        for (int p = 0; p <= slice_.plates; ++p) {
            std::string l = p == slice_.plates ? "A new plate" : "Plate " + std::to_string(p + 1);
            if (ImGui::MenuItem(l.c_str())) {
                if (p == slice_.plates) slice_.plates = std::min(36, slice_.plates + 1);
                for (auto [oid, k] : sel) {
                    auto& o = slice_.plate[oid];
                    if ((int)o.plateOf.size() < (int)o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
                    if (k < (int)o.plateOf.size()) o.plateOf[(size_t)k] = p;
                }
                slice_.activePlate = p;
                plateArrangeOne(p);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset placement")) {
        for (auto [oid, k] : sel) { auto& o = slice_.plate[oid]; o.rotZ = 0; o.lay = glm::mat4(1.f); o.scale = glm::vec3(1.f); o.cacheValid = false; }
    }
    ImGui::Separator();
    bool allColor = true;
    for (auto [oid, k] : sel) allColor &= slice_.plate[oid].useColor;
    if (ImGui::MenuItem("Print in its own color", nullptr, allColor)) {
        for (auto [oid, k] : sel) { slice_.plate[oid].useColor = !allColor; slice_.plate[oid].autoFil = true; }
        sliceAutoFils();
        sceneDirty_ = true;
    }
    ImGui::SetItemTooltip("Off: it prints with filament 1, whatever color it has in the design");
    if (ImGui::BeginMenu("Change filament")) {
        sliceFilsEnsure();
        if (ImGui::MenuItem("By its color (automatic)")) {
            for (auto [oid, k] : sel) { slice_.plate[oid].autoFil = true; slice_.plate[oid].useColor = true; }
            sliceAutoFils();
            sceneDirty_ = true;
        }
        for (int f = 0; f < (int)slice_.ex.fils.size(); ++f) {
            std::string lb = std::to_string(f + 1) + "   " + sliceFilPreset(f);
            glm::vec3 c = sliceFilColor(f);
            ImVec2 q = ImGui::GetCursorScreenPos();
            if (ImGui::MenuItem(("      " + lb + "##cf" + std::to_string(f)).c_str())) {
                for (auto [oid, k] : sel) { slice_.plate[oid].autoFil = false; slice_.plate[oid].fil = f; }
                sliceAutoFils();
                sceneDirty_ = true;
            }
            float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + h + 2, q.y + h + 2),
                                                      IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 255), 2.f);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Auto orient", "Q")) { for (auto [oid, k] : sel) plateAutoOrient(oid); plateArrangeOne(slice_.activePlate); }
    if (ImGui::MenuItem("Split to objects", nullptr, false, sel.size() == 1)) plateSplit(sel[0].first, false);
    if (ImGui::MenuItem("Split to parts", nullptr, false, sel.size() == 1)) plateSplit(sel[0].first, true);
    if (ImGui::MenuItem("Cut...", "C", false, sel.size() == 1)) { slice_.plateSel = sel[0].first; slice_.cutMode = true; slice_.cutZ = std::max(0.5f, (plateCore(sel[0].first).max.z - plateCore(sel[0].first).min.z) * 0.5f); }
    if (ImGui::MenuItem("Add settings for this object...", nullptr, false, sel.size() == 1)) { slice_.plateSel = sel[0].first; slice_.objectsView = true; slice_.objAddOpen = true; }
    ImGui::Separator();
    if (ImGui::MenuItem("Arrange this plate", "A")) plateArrangeOne(slice_.activePlate);
    if (ImGui::MenuItem("Select all", "Ctrl+A")) {
        for (auto& [oid, o] : slice_.plate)
            if (o.onPlate && plateInFilter(oid))
                for (int k = 0; k < (int)o.pos.size(); ++k) plateSelect(oid, k, true);
    }
    sceneDirty_ = true;
    ImGui::EndPopup();
}

// ===================================================================== Objects panel (sidebar)

void App::drawSlicePlatePanel(float w) {
    const float S = uiScale_;
    auto indent = [&] { ImGui::SetCursorPosX(12 * S); };
    float inner = w - 24 * S;
    // A selection came in from the design tab: say so, and offer everything again with one click.
    if (!slice_.plateOnly.empty()) {
        int shown = 0;
        for (auto& [id, o] : slice_.plate)
            if (plateInFilter(id) && o.onPlate) ++shown;
        indent();
        ImGui::TextDisabled("%d selected bod%s on the plate", shown, shown == 1 ? "y" : "ies");
        ImGui::SameLine();
        if (ImGui::SmallButton("Show all bodies")) {
            slice_.plateOnly.clear();
            sceneDirty_ = true;
        }
        ImGui::SetItemTooltip("Put every visible body back on the plate");
    }
    // The list: on the plate or not, its print color, name, copies. Parts from other designs show
    // the design they came from.
    ImGui::SetCursorPosX(0);
    float listH = std::min(6.f, (float)std::max<size_t>(1, slice_.plate.size())) * ImGui::GetFrameHeightWithSpacing() + 6 * S;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 3 * S));
    ImGui::BeginChild("##platelist", ImVec2(w, listH), ImGuiChildFlags_AlwaysUseWindowPadding);
    const std::string tabKey = plateTabKey();
    for (auto& [id, o] : slice_.plate) {
        if (!plateInFilter(id)) continue;
        ImGui::PushID((int)id);
        bool on = o.onPlate;
        if (ImGui::Checkbox("##on", &on)) { o.onPlate = on; sceneDirty_ = true; }
        ImGui::SetItemTooltip(on ? "On the plate (untick to leave it out of this print)" : "Not on the plate");
        ImGui::SameLine();
        // Its print color: click to print it with filament 1 instead (or back to its color).
        float sw = ImGui::GetFrameHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##col", ImVec2(sw, sw))) { o.useColor = !o.useColor; sceneDirty_ = true; }
        ImGui::SetItemTooltip(o.useColor ? "Prints in this color (its own filament). Click to use filament 1 instead."
                                         : "Prints with filament 1. Click to print it in its own color.");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 cc = ImGui::ColorConvertFloat4ToU32(ImVec4(o.color.r, o.color.g, o.color.b, 1.f));
        dl->AddRectFilled(ImVec2(p.x + 3 * S, p.y + 3 * S), ImVec2(p.x + sw - 3 * S, p.y + sw - 3 * S), o.useColor ? cc : col::Chip, 3 * S);
        if (!o.useColor) dl->AddLine(ImVec2(p.x + 4 * S, p.y + sw - 4 * S), ImVec2(p.x + sw - 4 * S, p.y + 4 * S), col::TextDim, 1.5f * S);
        ImGui::SameLine();
        std::string label = (o.name.empty() ? "Part" : o.name) + (o.pos.size() > 1 ? "  x" + std::to_string(o.pos.size()) : "");
        if (o.src != tabKey) label += "   (" + pathToU8(u8path(o.src).stem()) + ")";
        bool sel = false;
        for (int k = 0; k < (int)o.pos.size(); ++k) sel |= plateIsSel(id, k);
        bool out = false;
        for (int k = 0; k < (int)o.pos.size(); ++k) out |= on && !plateInstanceOnBed(id, k);
        if (out) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Error));
        if (ImGui::Selectable(label.c_str(), sel)) plateSelect(id, 0, ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (!sel) plateSelect(id, 0, false);
            slice_.ctxOpen = true;
        }
        if (out) { ImGui::PopStyleColor(); ImGui::SetItemTooltip("Part of it is off the plate"); }
        ImGui::PopID();
    }
    if (slice_.plate.empty()) ImGui::TextDisabled("Nothing visible to print");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // The selected part: position, rotation, scale, copies.
    if (!slice_.plateSel || !slice_.plate.count(slice_.plateSel)) {
        indent();
        ImGui::TextDisabled("Click a part. Shift drag: pick many.\nM move, R rotate, S scale, F lay on face.");
        return;
    }
    auto& o = slice_.plate[slice_.plateSel];
    int k = std::clamp(slice_.plateSelCopy, 0, std::max(0, (int)o.pos.size() - 1));
    auto field = [&](const char* label, float* v, const char* unit, float step) {
        indent();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(110 * S);
        ImGui::SetNextItemWidth(inner - 98 * S - ImGui::CalcTextSize("mm").x - 8 * S);
        bool ch = ImGui::DragFloat((std::string("##") + label).c_str(), v, step, 0, 0, "%.2f");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", unit);
        return ch;
    };
    if (!o.pos.empty()) {
        glm::vec2 p = o.pos[(size_t)k];
        if (field("Position X", &p.x, "mm", 0.5f) | field("Position Y", &p.y, "mm", 0.5f)) { o.pos[(size_t)k] = p; sceneDirty_ = true; }
    }
    if (field("Rotate", &o.rotZ, "deg", 1.f)) { o.cacheValid = false; sceneDirty_ = true; }
    float pct = o.scale.x * 100.f;
    if (field("Scale", &pct, "%", 1.f) && pct > 1.f) { o.scale = glm::vec3(pct / 100.f); o.cacheValid = false; sceneDirty_ = true; }
    {
        const Aabb& cb = plateCore(slice_.plateSel);
        indent();
        if (cb.valid()) ImGui::TextDisabled("Size  %.1f x %.1f x %.1f mm", cb.max.x - cb.min.x, cb.max.y - cb.min.y, cb.max.z - cb.min.z);
    }
    indent();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Copies");
    ImGui::SameLine(110 * S);
    if (ImGui::Button("-##cp") && o.pos.size() > 1) {
        o.pos.pop_back();
        if (o.plateOf.size() > o.pos.size()) o.plateOf.resize(o.pos.size());
        slice_.plateSelCopy = 0;
        sceneDirty_ = true;
    }
    ImGui::SameLine();
    ImGui::Text("%d", (int)o.pos.size());
    ImGui::SameLine();
    if (ImGui::Button("+##cp")) {
        if ((int)o.plateOf.size() < (int)o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
        o.pos.push_back(o.pos.empty() ? glm::vec2(plateBedRect().center()) : o.pos.back() + glm::vec2(10.f, 10.f));
        o.plateOf.push_back(slice_.activePlate);
        plateArrangeOne(slice_.activePlate);
    }
    indent();
    if (ImGui::Button("Lay flat")) plateLayFlat(slice_.plateSel, -1);
    ImGui::SetItemTooltip("Turn it onto its biggest flat face");
    ImGui::SameLine();
    bool lm = slice_.layMode;
    if (lm) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::Accent));
    if (ImGui::Button("Lay on face...")) slice_.layMode = !slice_.layMode;
    if (lm) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Then click the face that should sit on the plate (F)");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        o.rotZ = 0;
        o.lay = glm::mat4(1.f);
        o.scale = glm::vec3(1.f);
        o.cacheValid = false;
        sceneDirty_ = true;
    }
    ImGui::SetItemTooltip("Back to how it sits in the design");
}

} // namespace kst
