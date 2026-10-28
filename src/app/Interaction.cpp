// Fusion style canvas interaction: preselection (faces, edges, sketch profiles, bodies), click and
// window selection, the radial marking menu on right click, drag arrows (manipulators) for
// distances, and the overlays that draw all of it.
#include "app/App.h"
#include "core/Log.h"
#include "kernel/Features.h"
#include "platform/Platform.h"
#include "sketch/Profiles.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kst {

using namespace ui;

namespace {

const double kPiD = 3.14159265358979323846;

#define kPreCol (ui::th(IM_COL32(255, 150, 90, 255)))     // preselect (hover): light Kestral orange
#define kSelCol (ui::th(IM_COL32(255, 96, 30, 255)))      // selected: Kestral orange

bool rayTri(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, double& t) {
    glm::dvec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
    double det = glm::dot(e1, p);
    if (std::abs(det) < 1e-14) return false;
    double inv = 1.0 / det;
    glm::dvec3 s = o - a;
    double u = glm::dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    glm::dvec3 q = glm::cross(s, e1);
    double v = glm::dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = glm::dot(e2, q) * inv;
    return t > 0;
}

float distToSeg(ImVec2 p, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 1e-6f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
    float qx = a.x + dx * t - p.x, qy = a.y + dy * t - p.y;
    return std::sqrt(qx * qx + qy * qy);
}

} // namespace

// ===================================================================== projection helpers

glm::dmat4 App::viewProj() const {
    float aspect = vk_.extent.height ? (float)vk_.extent.width / (float)vk_.extent.height : 1.f;
    return glm::dmat4(cam_.proj(aspect, sceneBounds_)) * glm::dmat4(cam_.view());
}

bool App::worldToScreen(const glm::dvec3& w, ImVec2& out) const {
    glm::dvec4 c = viewProj() * glm::dvec4(w, 1.0);
    if (c.w <= 1e-9) return false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    out.x = (float)((c.x / c.w * 0.5 + 0.5) * ds.x);
    out.y = (float)((c.y / c.w * 0.5 + 0.5) * ds.y);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::abs(out.x) < 1e7f && std::abs(out.y) < 1e7f;
}

bool App::rayAt(ImVec2 px, glm::dvec3& o, glm::dvec3& d) const {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x < 1 || ds.y < 1) return false;
    glm::dmat4 inv = glm::inverse(viewProj());
    double nx = px.x / ds.x * 2.0 - 1.0, ny = px.y / ds.y * 2.0 - 1.0;
    glm::dvec4 a = inv * glm::dvec4(nx, ny, 1.0, 1.0);
    glm::dvec4 b = inv * glm::dvec4(nx, ny, 0.5, 1.0);
    if (std::abs(a.w) < 1e-15 || std::abs(b.w) < 1e-15) return false;
    glm::dvec3 pa = glm::dvec3(a) / a.w, pb = glm::dvec3(b) / b.w;
    glm::dvec3 dir = pb - pa;
    double l = glm::length(dir);
    if (!(l > 1e-15)) return false;
    o = pa;
    d = dir / l;
    return true;
}

// World space hit of the ray under the cursor with one face of a body (or the whole mesh for mesh
// bodies). Used for orbit pivots, profile versus face priority and pick mapping.
bool App::hitPoint(uint32_t bodyId, uint32_t face1, ImVec2 px, glm::dvec3& out) const {
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return false;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.mesh) return false;
    const MeshData& m = *pit->second.mesh;
    glm::dvec3 o, d;
    if (!rayAt(px, o, d)) return false;
    glm::dmat4 xf(bit->second.xform);
    glm::dmat4 inv = glm::inverse(xf);
    glm::dvec3 lo = glm::dvec3(inv * glm::dvec4(o, 1.0));
    glm::dvec3 ld = glm::normalize(glm::dvec3(inv * glm::dvec4(d, 0.0)));
    bool filter = face1 != 0 && m.faceCount > 1;
    double best = 1e300;
    size_t nt = m.indices.size() / 3;
    for (size_t t = 0; t < nt; ++t) {
        if (filter && m.triFace[t] != face1 - 1) continue;
        const float* a = &m.verts[m.indices[t * 3] * 6];
        const float* b = &m.verts[m.indices[t * 3 + 1] * 6];
        const float* c = &m.verts[m.indices[t * 3 + 2] * 6];
        double tt;
        if (rayTri(lo, ld, {a[0], a[1], a[2]}, {b[0], b[1], b[2]}, {c[0], c[1], c[2]}, tt) && tt < best) best = tt;
    }
    if (best >= 1e300) return false;
    out = glm::dvec3(xf * glm::dvec4(lo + ld * best, 1.0));
    return true;
}

// ===================================================================== caches

const std::vector<std::vector<glm::dvec3>>* App::bodyEdges(uint32_t bodyId) {
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return nullptr;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return nullptr;
    const ShapeHandle* key = pit->second.shape.get();
    auto it = edgeCache_.find(key);
    if (it == edgeCache_.end()) {
        // Computed on a worker: a body with thousands of edges must never stall the viewport.
        // Until it lands, that body just has no edge hover (faces and bodies still pick).
        EdgeCacheEntry e;
        e.shape = pit->second.shape;
        edgeCache_.emplace(key, std::move(e));
        ShapePtr sh = pit->second.shape;
        JobSystem* jobs = jobs_.get();
        jobs_->submit("edges", [this, jobs, sh, key] {
            auto polys = std::make_shared<std::vector<std::vector<glm::dvec3>>>();
            kernel::edgePolylines(sh, 20000, *polys);
            // The seam of a round face is not an edge (it is not drawn, and nothing can fillet it):
            // keep its slot so indices still match the kernel, but give it nothing to hover.
            for (uint32_t sidx : kernel::seamEdges(sh))
                if (sidx < polys->size()) (*polys)[sidx].clear();
            jobs->post([this, sh, key, polys] {
                auto it2 = edgeCache_.find(key);
                if (it2 == edgeCache_.end() || it2->second.shape != sh) return;
                it2->second.polys = std::move(*polys);
                it2->second.ready = true;
                activity();
            });
        });
        return nullptr;
    }
    return it->second.ready ? &it->second.polys : nullptr;
}

const std::vector<sk::Profile>& App::sketchProfiles(const sk::Sketch& s) {
    ProfCache& c = profCache_[s.id];
    if (c.rev != s.rev) {
        c.profiles = sk::findProfiles(s);
        c.rev = s.rev;
    }
    return c.profiles;
}

// ===================================================================== selection

void App::clearSelection() {
    selSketches_.clear();
    selPlanes_.clear();
    if (selection_.empty() && selFaces_.empty() && selEdges_.empty() && selProfiles_.empty()) return;
    selection_.clear();
    selOrder_.clear();
    selFaces_.clear();
    selEdges_.clear();
    selProfiles_.clear();
    markSceneDirty();
    selectionChanged();
}

bool App::anySelection() const {
    return !selection_.empty() || !selFaces_.empty() || !selEdges_.empty() || !selProfiles_.empty();
}

std::vector<uint32_t> App::selectedBodies() const {
    std::set<uint32_t> s(selection_.begin(), selection_.end());
    for (auto& f : selFaces_) s.insert(f.first);
    for (auto& e : selEdges_) s.insert(e.first);
    return std::vector<uint32_t>(s.begin(), s.end());
}

uint32_t App::pickFilterNow() const {
    if (sk_.active) return PickEdge | PickFace;   // sketch Project tool
    if (measureMode_) return PickBody | PickFace | PickEdge;
    if (tdlg_.open || dlg_.open) return activePickFilter_;
    // Holding Ctrl flips the priority: with faces first, Ctrl picks (and highlights) whole bodies.
    // Shift adds to the selection (more faces, edges or, with Ctrl too, bodies).
    bool body = settings_.bodyPriority != ImGui::GetIO().KeyCtrl;
    if (body) return PickBody | PickProfile;
    return PickBody | PickFace | PickEdge | PickProfile;
}

void App::updateHover() {
    wdPhase_.store("hover highlight");
    HoverInfo h;
    if (!viewportHovered_ || drag_ == Drag::Orbit || drag_ == Drag::Pan || drag_ == Drag::Zoom || winSel_ || mm_.open ||
        manip_.dragging || (sk_.active && sk_.tool != "project") || sk_.choosingPlane) {
        hov_ = h;
        return;
    }
    uint32_t filt = pickFilterNow();
    ImVec2 m = ImGui::GetIO().MousePos;
    const float S = uiScale_;

    // 1. A corner, when the command is asking for one, then an edge under the cursor.
    if (filt & PickVertex) {
        float best = 10.f * S;
        int bestV = -1;
        uint32_t bestB = 0;
        for (auto& [id, b] : doc_.bodies) {
            if (!doc_.isBodyAlive(b)) continue;
            auto pit = doc_.parts.find(b.partId);
            if (pit == doc_.parts.end() || !pit->second.shape) continue;
            std::vector<glm::dvec3> pts;
            kernel::vertexPoints(pit->second.shape, b.xform, pts);
            for (uint32_t i = 0; i < pts.size(); ++i) {
                ImVec2 q;
                if (!worldToScreen(pts[i], q)) continue;
                float dd = std::hypot(m.x - q.x, m.y - q.y);
                if (dd < best) { best = dd; bestV = (int)i; bestB = id; }
            }
        }
        if (bestV >= 0) {
            h.kind = HoverKind::Vertex;
            h.body = bestB;
            h.index = (uint32_t)bestV;
            hov_ = h;
            return;
        }
    }
    uint32_t body = hoverBody_;
    if (body && !doc_.bodies.count(body)) body = 0;
    uint32_t edgeBody = body ? pickBodyFor(body) : 0;
    if ((filt & PickEdge) && edgeBody) {
        auto bit = doc_.bodies.find(edgeBody);
        const auto* polys = bit != doc_.bodies.end() ? bodyEdges(edgeBody) : nullptr;
        if (polys) {
            glm::dmat4 vp = viewProj() * glm::dmat4(bit->second.xform);
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            // Sliver edges from imported files often sit right on a big edge on screen. Among
            // edges inside the pick radius, the more prominent (longer on screen) one wins,
            // so clicking a long edge never grabs the tiny one lying beside it.
            float radius = 7.f * S;
            int bestE = -1;
            float bestDist = 0.f, bestLen = 0.f;
            // What the eye actually sees under the cursor: edges behind that surface are skipped, so
            // a click on a face never grabs a line on the far side of the model.
            glm::dvec3 ro, rd, hp;
            bool haveHit = rayAt(m, ro, rd) && hitPoint(body, hoverFace_, m, hp);
            double tHit = 0, tol = 0;
            if (haveHit) {
                rd = glm::normalize(rd);
                tHit = glm::dot(hp - ro, rd);
                double px = cam_.halfHeightAtTarget() * 2.0 / std::max(1.f, ds.y);
                if (settings_.perspective) px *= std::max(tHit, 1e-6) / std::max((double)cam_.distance, 1e-6);
                tol = px * 10.0 + sceneBounds_.diag() * 1e-4;
            }
            const glm::dmat4 bx(bit->second.xform);
            for (size_t i = 0; i < polys->size(); ++i) {
                const auto& pl = (*polys)[i];
                ImVec2 prev;
                glm::dvec3 prevW(0);
                bool have = false;
                float dd = 1e9f, len = 0.f;
                glm::dvec3 nearW(0);
                for (auto& w : pl) {
                    glm::dvec4 c = vp * glm::dvec4(w, 1.0);
                    if (c.w <= 1e-9) { have = false; continue; }
                    ImVec2 q((float)((c.x / c.w * 0.5 + 0.5) * ds.x), (float)((c.y / c.w * 0.5 + 0.5) * ds.y));
                    if (have) {
                        float dseg = distToSeg(m, prev, q);
                        if (dseg < dd) {
                            dd = dseg;
                            float sx = q.x - prev.x, sy = q.y - prev.y, l2 = sx * sx + sy * sy;
                            float u = l2 > 1e-6f ? std::clamp(((m.x - prev.x) * sx + (m.y - prev.y) * sy) / l2, 0.f, 1.f) : 0.f;
                            nearW = prevW + (w - prevW) * (double)u;
                        }
                        float dx = q.x - prev.x, dy = q.y - prev.y;
                        len += std::sqrt(dx * dx + dy * dy);
                    }
                    prev = q;
                    prevW = w;
                    have = true;
                }
                if (dd >= radius) continue;
                if (haveHit) {
                    glm::dvec3 wp = glm::dvec3(bx * glm::dvec4(nearW, 1.0));
                    if (glm::dot(wp - ro, rd) > tHit + tol) continue;   // hidden behind the face under the cursor
                }
                if (bestE < 0 || dd < bestDist - 2.f * S ||
                    (dd <= bestDist + 2.f * S && len > bestLen * 1.5f)) {
                    bestE = (int)i;
                    bestDist = std::min(bestDist == 0.f ? dd : bestDist, dd);
                    bestLen = len;
                }
            }
            if (bestE >= 0) {
                h.kind = HoverKind::Edge;
                h.body = edgeBody;
                h.index = (uint32_t)bestE;
                hov_ = h;
                return;
            }
        }
    }

    // 2. Sketch profiles of visible sketches (they sit on top of faces, like Fusion).
    if (filt & PickProfile) {
        glm::dvec3 o, d;
        if (rayAt(m, o, d)) {
            double bestT = 1e300;
            for (auto& [sid, s] : doc_.sketches) {
                if (!sketchShown(s)) continue;
                const sk::Plane& pl = s.plane;
                glm::dvec3 n = pl.normal();
                double den = glm::dot(n, d);
                if (std::abs(den) < 1e-9) continue;
                double t = glm::dot(pl.o - o, n) / den;
                if (t <= 0 || t >= bestT) continue;
                glm::dvec3 w = o + d * t;
                sk::Vec2 q = pl.toLocal(w);
                const auto& profs = sketchProfiles(s);
                // Smallest containing profile wins (a hole's disk over the ring around it).
                int pick = -1;
                double area = 1e300;
                for (size_t i = 0; i < profs.size(); ++i) {
                    const auto& p = profs[i];
                    if (q.x < p.lo.x || q.y < p.lo.y || q.x > p.hi.x || q.y > p.hi.y) continue;
                    if (!sk::pointInProfile(p, q)) continue;
                    if (std::abs(p.area) < area) { area = std::abs(p.area); pick = (int)i; }
                }
                if (pick < 0) continue;
                bestT = t;
                h.kind = HoverKind::Profile;
                h.body = sid;
                h.index = (uint32_t)pick;
                h.hasPoint = true;
                h.point = w;
            }
            if (h.kind == HoverKind::Profile && body) {
                // A body face in front of the profile wins.
                glm::dvec3 hp;
                if (hitPoint(body, hoverFace_, m, hp)) {
                    double tb = glm::dot(hp - o, d);
                    double tol = std::max(1e-3, sceneBounds_.diag() * 1e-5);
                    if (tb < bestT - tol) h = HoverInfo{};
                }
            }
            if (h.kind == HoverKind::Profile) {
                hov_ = h;
                return;
            }
        }
    }

    // 3. Face, or the whole body.
    if (body) {
        auto bit = doc_.bodies.find(body);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        bool brep = pit != doc_.parts.end() && pit->second.shape;
        if ((filt & PickFace) && brep && hoverFace_) {
            h.kind = HoverKind::Face;
            h.body = body;
            h.index = hoverFace_ - 1;
        } else if (filt & PickBody) {
            h.kind = HoverKind::Body;
            h.body = body;
        }
    }
    hov_ = h;
}

// The body that picks on `hovered` should refer to. While a modify feature (fillet, shell...)
// is being edited, its preview body stands in for the target body it replaces.
uint32_t App::pickBodyFor(uint32_t hovered) const {
    if (!tdlg_.open || hovered == 0) return hovered;
    auto bit = doc_.bodies.find(hovered);
    if (bit == doc_.bodies.end() || bit->second.featureId != tdlg_.featureId) return hovered;
    uint32_t slot = doc_.bodySlot(hovered);
    if (slot >= tdlg_.targets.size()) return hovered;
    const TargetRef& r = tdlg_.targets[slot];
    std::vector<uint32_t> ids = doc_.featureBodies(r.first);
    return r.second < ids.size() ? ids[r.second] : hovered;
}

void App::selectionChanged() {
    // Commands that work on the selection follow it live.
    if (tdlg_.open) syncToolFromSelection();
    if (dlg_.open) syncExtrudeFromSelection();
    activity();
}

void App::viewportClick(bool add, bool dbl) {
    wdPhase_.store("selecting");
    if (holeMode()) { holePlaceClick(); return; }
    HoverInfo h = hov_;
    auto toggle = [&](std::set<SubRef>& set, SubRef r) {
        if (set.count(r)) set.erase(r);
        else set.insert(r);
    };
    auto toggleBody = [&](uint32_t b) {
        if (selection_.count(b)) {
            selection_.erase(b);
            selOrder_.erase(std::remove(selOrder_.begin(), selOrder_.end(), b), selOrder_.end());
        } else {
            selection_.insert(b);
            selOrder_.push_back(b);
        }
    };
    bool inCommand = tdlg_.open || dlg_.open;
    // Inside a command clicks always add or remove (Fusion builds the command's selection).
    if (inCommand) add = true;
    if (h.kind == HoverKind::None) {
        if (!add) clearSelection();
        else if (tdlg_.open && (tdlg_.pickWait || (tdlg_.type == FeatureType::Split && tdlg_.option2 == 1))) syncToolFromSelection();
        return;
    }
    // A face of a command's own preview stands for the same face on the body it replaces.
    if (h.kind == HoverKind::Face && tdlg_.open) {
        uint32_t tb = pickBodyFor(h.body);
        if (tb != h.body) {
            glm::dvec3 p;
            auto bit = doc_.bodies.find(tb);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit == doc_.parts.end() || !pit->second.shape || !hitPoint(h.body, h.index + 1, ImGui::GetIO().MousePos, p)) return;
            double tol = std::max(1e-3, kernel::shapeDiag(pit->second.shape) * 1e-4);
            int fidx = kernel::faceNear(pit->second.shape, bit->second.xform, p, tol);
            if (fidx < 0) return;   // a face the command created itself
            h.body = tb;
            h.index = (uint32_t)fidx;
        }
    }
    if (!add) {
        selection_.clear();
        selOrder_.clear();
        selFaces_.clear();
        selEdges_.clear();
        selProfiles_.clear();
    }
    switch (h.kind) {
    case HoverKind::Edge: {
        if (dbl || (inCommand && tangentChain_)) {
            auto bit = doc_.bodies.find(h.body);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit != doc_.parts.end() && pit->second.shape) {
                bool removing = selEdges_.count({h.body, h.index}) && !dbl;
                for (uint32_t e : kernel::tangentChain(pit->second.shape, h.index)) {
                    if (removing) selEdges_.erase({h.body, e});
                    else selEdges_.insert({h.body, e});
                }
                break;
            }
        }
        toggle(selEdges_, {h.body, h.index});
        break;
    }
    case HoverKind::Vertex: {
        SubRef r{h.body, h.index};
        bool on = selEdges_.count(r) > 0;
        selEdges_.clear();
        if (!on) selEdges_.insert(r);
        break;
    }
    case HoverKind::Face:
        if (dbl && !inCommand) {
            selFaces_.erase({h.body, h.index});
            toggleBody(h.body);   // double click a face: the whole body
        } else {
            toggle(selFaces_, {h.body, h.index});
        }
        lastSelected_ = h.body;
        break;
    case HoverKind::Profile:
        toggle(selProfiles_, {h.body, h.index});
        break;
    case HoverKind::Body:
        toggleBody(h.body);
        lastSelected_ = h.body;
        break;
    default: break;
    }
    // Fusion: the browser opens to the part you clicked and marks it.
    if (h.kind == HoverKind::Edge || h.kind == HoverKind::Face || h.kind == HoverKind::Body || h.kind == HoverKind::Vertex) revealBody_ = h.body;
    markSceneDirty();
    selectionChanged();
}

void App::windowSelect(ImVec2 a, ImVec2 b, bool add) {
    wdPhase_.store("window selection");
    // Left to right: whole body inside the box. Right to left: anything touching it (crossing).
    bool crossing = b.x < a.x;
    ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
    if (!add && !(tdlg_.open || dlg_.open)) {
        selection_.clear();
        selFaces_.clear();
        selEdges_.clear();
        selProfiles_.clear();
    }
    uint32_t filt = pickFilterNow();
    bool edgesOnly = (tdlg_.open || dlg_.open) && (filt & PickEdge) && !(filt & PickBody);
    for (auto& [id, b2] : doc_.bodies) {
        if (!bodyShown(b2)) continue;
        auto pit = doc_.parts.find(b2.partId);
        if (pit == doc_.parts.end() || !pit->second.mesh) continue;
        if (edgesOnly) {
            const auto* polys = bodyEdges(id);
            if (!polys) continue;
            glm::dmat4 xf(b2.xform);
            for (size_t i = 0; i < polys->size(); ++i) {
                bool all = !(*polys)[i].empty(), anyIn = false;
                for (auto& w : (*polys)[i]) {
                    ImVec2 q;
                    bool in = worldToScreen(glm::dvec3(xf * glm::dvec4(w, 1.0)), q) && q.x >= lo.x && q.x <= hi.x && q.y >= lo.y && q.y <= hi.y;
                    all = all && in;
                    anyIn = anyIn || in;
                }
                if (crossing ? anyIn : all) selEdges_.insert({id, (uint32_t)i});
            }
            continue;
        }
        if (!(filt & PickBody)) continue;
        Aabb bb = pit->second.mesh->bounds.transformed(b2.xform);
        if (!bb.valid()) continue;
        bool all = true, anyIn = false;
        ImVec2 slo(1e9f, 1e9f), shi(-1e9f, -1e9f);
        for (int i = 0; i < 8; ++i) {
            glm::dvec3 c((i & 1) ? bb.max.x : bb.min.x, (i & 2) ? bb.max.y : bb.min.y, (i & 4) ? bb.max.z : bb.min.z);
            ImVec2 q;
            if (!worldToScreen(c, q)) { all = false; continue; }
            slo.x = std::min(slo.x, q.x); slo.y = std::min(slo.y, q.y);
            shi.x = std::max(shi.x, q.x); shi.y = std::max(shi.y, q.y);
            bool in = q.x >= lo.x && q.x <= hi.x && q.y >= lo.y && q.y <= hi.y;
            all = all && in;
            anyIn = anyIn || in;
        }
        bool overlap = !(shi.x < lo.x || slo.x > hi.x || shi.y < lo.y || slo.y > hi.y);
        if ((crossing ? (anyIn || overlap) : all) && !selection_.count(id)) {
            selection_.insert(id);
            selOrder_.push_back(id);
        }
    }
    markSceneDirty();
    selectionChanged();
}

// ===================================================================== overlays

void App::drawInteractionOverlay() {
    if (sk_.active && sk_.tool != "project") return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float S = uiScale_;
    drawHolePlacement(dl);

    auto drawEdge = [&](uint32_t bodyId, uint32_t idx, ImU32 col, float th) {
        auto bit = doc_.bodies.find(bodyId);
        if (bit == doc_.bodies.end()) return;
        const auto* polys = bodyEdges(bodyId);
        if (!polys || idx >= polys->size()) return;
        glm::dmat4 xf(bit->second.xform);
        ImVec2 prev;
        bool have = false;
        for (auto& w : (*polys)[idx]) {
            ImVec2 q;
            if (!worldToScreen(glm::dvec3(xf * glm::dvec4(w, 1.0)), q)) { have = false; continue; }
            if (have) dl->AddLine(prev, q, col, th);
            prev = q;
            have = true;
        }
    };
    auto fillProfile = [&](uint32_t sid, uint32_t idx, ImU32 col) {
        auto sit = doc_.sketches.find(sid);
        if (sit == doc_.sketches.end()) return;
        const auto& profs = sketchProfiles(sit->second);
        if (idx >= profs.size()) return;
        const auto& p = profs[idx];
        ImDrawListFlags old = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (size_t i = 0; i + 2 < p.tris.size(); i += 3) {
            ImVec2 a, b, c;
            if (worldToScreen(sit->second.plane.toWorld(p.tris[i]), a) && worldToScreen(sit->second.plane.toWorld(p.tris[i + 1]), b) &&
                worldToScreen(sit->second.plane.toWorld(p.tris[i + 2]), c))
                dl->AddTriangleFilled(a, b, c, col);
        }
        dl->Flags = old;
    };

    // Profiles of visible sketches are lightly shaded so you can see what E will extrude.
    for (auto& [sid, s] : doc_.sketches) {
        if (sk_.active || !sketchShown(s)) continue;
        const auto& profs = sketchProfiles(s);
        for (size_t i = 0; i < profs.size(); ++i) {
            bool sel = selProfiles_.count({sid, (uint32_t)i}) > 0;
            bool hov = hov_.kind == HoverKind::Profile && hov_.body == sid && hov_.index == i;
            fillProfile(sid, (uint32_t)i, sel ? ui::th(IM_COL32(255, 107, 53, 120)) : hov ? ui::th(IM_COL32(255, 160, 110, 110)) : ui::th(IM_COL32(120, 150, 190, 40)));
        }
    }
    auto drawVert = [&](uint32_t bodyId, uint32_t idx, ImU32 col) {
        auto bit = doc_.bodies.find(bodyId);
        if (bit == doc_.bodies.end()) return;
        auto pit = doc_.parts.find(bit->second.partId);
        if (pit == doc_.parts.end() || !pit->second.shape) return;
        glm::dvec3 w;
        if (!kernel::vertexPoint(pit->second.shape, bit->second.xform, idx, w)) return;
        ImVec2 q;
        if (!worldToScreen(w, q)) return;
        dl->AddCircleFilled(q, 5.f * S, col);
    };
    bool cornerPick = tdlg_.open && tdlg_.type == FeatureType::ConstructPoint && tdlg_.option == 3;
    if (!cornerPick)
        for (auto& e : selEdges_) drawEdge(e.first, e.second, kSelCol, 3.5f * S);
    else
        for (auto& e : selEdges_) drawVert(e.first, e.second, kSelCol);
    if (hov_.kind == HoverKind::Edge) drawEdge(hov_.body, hov_.index, kPreCol, 3.5f * S);
    if (hov_.kind == HoverKind::Vertex) drawVert(hov_.body, hov_.index, kPreCol);

    // Window selection rectangle: solid for window (left to right), dashed for crossing.
    if (winSel_) {
        ImVec2 a = winA_, b = ImGui::GetIO().MousePos;
        bool crossing = b.x < a.x;
        dl->AddRectFilled(a, b, crossing ? ui::th(IM_COL32(60, 200, 90, 30)) : ui::th(IM_COL32(40, 120, 240, 30)));
        ImU32 c = crossing ? ui::th(IM_COL32(40, 160, 70, 220)) : ui::th(IM_COL32(30, 100, 220, 220));
        if (!crossing) dl->AddRect(a, b, c, 0, 0, 1.2f * S);
        else {
            ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
            auto dash = [&](ImVec2 p, ImVec2 q) {
                float len = std::hypot(q.x - p.x, q.y - p.y);
                for (float t = 0; t < len; t += 8 * S) {
                    float t1 = std::min(len, t + 4 * S);
                    dl->AddLine(ImVec2(p.x + (q.x - p.x) * t / len, p.y + (q.y - p.y) * t / len),
                                ImVec2(p.x + (q.x - p.x) * t1 / len, p.y + (q.y - p.y) * t1 / len), c, 1.2f * S);
                }
            };
            dash(lo, ImVec2(hi.x, lo.y)); dash(ImVec2(hi.x, lo.y), hi); dash(hi, ImVec2(lo.x, hi.y)); dash(ImVec2(lo.x, hi.y), lo);
        }
    }

    // Orbit pivot dot (Fusion shows a small green dot where it orbits around).
    if (drag_ == Drag::Orbit && orbitPivotValid_) {
        ImVec2 q;
        if (worldToScreen(orbitPivot_, q)) {
            dl->AddCircleFilled(q, 5 * S, ui::th(IM_COL32(40, 190, 80, 230)));
            dl->AddCircle(q, 5 * S, ui::th(IM_COL32(255, 255, 255, 230)), 0, 1.5f * S);
        }
    }
    drawManipulator();
    drawMoveTriad();
}

// ===================================================================== manipulator (drag arrow)

void App::setManip(int target, const glm::dvec3& origin, const glm::dvec3& dir, double value) {
    if (manip_.dragging && manip_.target == target) return;   // keep the live drag stable
    manip_.active = true;
    manip_.minValue = -1e300;
    manip_.target = target;
    manip_.origin = origin;
    double l = glm::length(dir);
    manip_.dir = l > 1e-12 ? dir / l : glm::dvec3(0, 0, 1);
    manip_.value = value;
}

void App::drawManipulator() {
    manipHovered_ = false;
    if (!manip_.active) return;
    if (!(dlg_.open || tdlg_.open)) { manip_.active = false; manip_.dragging = false; return; }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float S = uiScale_;
    ImGuiIO& io = ImGui::GetIO();
    glm::dvec3 base = manip_.origin + manip_.dir * manip_.value;
    ImVec2 b0, b1;
    if (!worldToScreen(base, b0)) return;
    // Arrow is 60 px long on screen whatever the zoom.
    glm::dvec3 probe = base + (manip_.value < 0 ? -manip_.dir : manip_.dir) * std::max(1e-3, sceneBounds_.valid() ? sceneBounds_.diag() * 0.01 : 1.0);
    ImVec2 pp;
    if (!worldToScreen(probe, pp)) return;
    float dx = pp.x - b0.x, dy = pp.y - b0.y, dl2 = std::hypot(dx, dy);
    if (dl2 < 1e-3f) { dx = 0; dy = -1; dl2 = 1; }
    dx /= dl2; dy /= dl2;
    b1 = ImVec2(b0.x + dx * 60 * S, b0.y + dy * 60 * S);
    ImVec2 m = io.MousePos;
    bool hover = distToSeg(m, b0, b1) < 9 * S;
    ImU32 col = manip_.dragging ? ui::th(IM_COL32(255, 150, 30, 255)) : hover ? ui::th(IM_COL32(255, 175, 60, 255)) : ui::th(IM_COL32(60, 120, 230, 255));
    dl->AddLine(b0, b1, col, 3.f * S);
    ImVec2 n(-dy, dx);
    ImVec2 tip(b1.x + dx * 14 * S, b1.y + dy * 14 * S);
    dl->AddTriangleFilled(tip, ImVec2(b1.x + n.x * 7 * S, b1.y + n.y * 7 * S), ImVec2(b1.x - n.x * 7 * S, b1.y - n.y * 7 * S), col);
    dl->AddCircleFilled(b0, 4 * S, col);
    manipHovered_ = hover && viewportHovered_;

    // Closest point on the arrow's axis to the mouse ray: the dragged value.
    auto axisParam = [&](double& t) {
        glm::dvec3 o, d;
        if (!rayAt(m, o, d)) return false;
        glm::dvec3 u = manip_.dir, w0 = manip_.origin - o;
        double a = glm::dot(u, u), b = glm::dot(u, d), c = glm::dot(d, d), dd = glm::dot(u, w0), e = glm::dot(d, w0);
        double den = a * c - b * b;
        if (std::abs(den) < 1e-12) return false;
        t = (b * e - c * dd) / den;
        return std::isfinite(t);
    };
    if (!manip_.dragging && manipHovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !mm_.open) {
        double t;
        if (axisParam(t)) {
            manip_.dragging = true;
            manip_.grab = t - manip_.value;
        }
    }
    if (manip_.dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            manip_.dragging = false;
            manipCommit(manip_.value, true);
        } else {
            double t;
            if (axisParam(t)) {
                double v = t - manip_.grab;
                // Snap to a step that is about 4 px on screen (0.1, 0.5, 1, 5 ... mm).
                double wpp = 2.0 * cam_.halfHeightAtTarget() / std::max(1.f, io.DisplaySize.y);
                double raw = wpp * 4.0;
                double p10 = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-6))));
                double step = raw / p10 < 2 ? p10 : raw / p10 < 5 ? 2 * p10 : 5 * p10;
                v = std::round(v / step) * step;
                v = std::max(v, manip_.minValue);
                if (std::abs(v - manip_.value) > 1e-12) {
                    manip_.value = v;
                    manipCommit(v, false);
                }
            }
            // Value readout next to the arrow.
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.4g mm", manip_.value);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            ImVec2 p(m.x + 18 * S, m.y + 8 * S);
            dl->AddRectFilled(ImVec2(p.x - 4 * S, p.y - 2 * S), ImVec2(p.x + ts.x + 4 * S, p.y + ts.y + 2 * S), ui::th(IM_COL32(255, 255, 255, 235)), 3 * S);
            dl->AddText(p, ui::th(IM_COL32(30, 36, 46, 255)), buf);
            activity();
        }
    }
}

// Move / Copy: three axis arrows on the moved bodies (Fusion's triad, translation part).
void App::drawMoveTriad() {
    if (!(tdlg_.open && tdlg_.type == FeatureType::Move)) { triad_ = Triad{}; return; }
    // Where the bodies are now: the bounds of the Move feature's own outputs.
    Aabb bb;
    for (uint32_t bid : doc_.featureBodies(tdlg_.featureId)) {
        auto bit = doc_.bodies.find(bid);
        if (bit == doc_.bodies.end()) continue;
        auto pit = doc_.parts.find(bit->second.partId);
        if (pit != doc_.parts.end() && pit->second.mesh) bb.add(pit->second.mesh->bounds.transformed(bit->second.xform));
    }
    if (!bb.valid()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float S = uiScale_;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    glm::dvec3 c = triad_.dragging ? triad_.center : glm::dvec3(bb.center());
    ImVec2 c0;
    if (!worldToScreen(c, c0)) return;
    const glm::dvec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const ImU32 cols[3] = {ui::th(IM_COL32(220, 60, 60, 255)), ui::th(IM_COL32(60, 170, 70, 255)), ui::th(IM_COL32(50, 110, 230, 255))};
    char* bufs[3] = {tdlg_.v1, tdlg_.v2, tdlg_.v3};
    double unit = std::max(1e-3, sceneBounds_.valid() ? sceneBounds_.diag() * 0.01 : 1.0);
    int hoverAxis = -1;
    ImVec2 tips[3];
    for (int a = 0; a < 3; ++a) {
        ImVec2 p;
        if (!worldToScreen(c + axes[a] * unit, p)) { tips[a] = c0; continue; }
        float dx = p.x - c0.x, dy = p.y - c0.y, l = std::hypot(dx, dy);
        if (l < 1e-3f) { tips[a] = c0; continue; }   // axis pointing at the camera
        tips[a] = ImVec2(c0.x + dx / l * 75 * S, c0.y + dy / l * 75 * S);
        if (!triad_.dragging && distToSeg(m, c0, tips[a]) < 8 * S && viewportHovered_) hoverAxis = a;
    }
    for (int a = 0; a < 3; ++a) {
        bool hot = triad_.dragging ? triad_.axis == a : hoverAxis == a;
        ImU32 col = hot ? ui::th(IM_COL32(255, 160, 40, 255)) : cols[a];
        float dx = tips[a].x - c0.x, dy = tips[a].y - c0.y, l = std::hypot(dx, dy);
        if (l < 1.f) continue;
        dx /= l; dy /= l;
        dl->AddLine(c0, tips[a], col, 3.f * S);
        ImVec2 n(-dy, dx), tip(tips[a].x + dx * 13 * S, tips[a].y + dy * 13 * S);
        dl->AddTriangleFilled(tip, ImVec2(tips[a].x + n.x * 6 * S, tips[a].y + n.y * 6 * S),
                              ImVec2(tips[a].x - n.x * 6 * S, tips[a].y - n.y * 6 * S), col);
    }
    dl->AddCircleFilled(c0, 5 * S, ui::th(IM_COL32(250, 250, 250, 255)));
    dl->AddCircle(c0, 5 * S, ui::th(IM_COL32(60, 60, 60, 255)), 0, 1.5f);
    if (hoverAxis >= 0) manipHovered_ = true;

    auto axisParam = [&](int a, double& t) {
        glm::dvec3 o, d;
        if (!rayAt(m, o, d)) return false;
        glm::dvec3 u = axes[a], w0 = triad_.center - o;
        double A = 1, B = glm::dot(u, d), C = glm::dot(d, d), D = glm::dot(u, w0), E = glm::dot(d, w0);
        double den = A * C - B * B;
        if (std::abs(den) < 1e-12) return false;
        t = (B * E - C * D) / den;
        return std::isfinite(t);
    };
    if (!triad_.dragging && hoverAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !mm_.open) {
        triad_.center = c;
        triad_.axis = hoverAxis;
        double t;
        if (axisParam(hoverAxis, t)) {
            triad_.dragging = true;
            triad_.grab = t;
            triad_.startVal = 0;
            try { triad_.startVal = peval::eval(bufs[hoverAxis], doc_.params); } catch (const std::runtime_error&) {}
        }
    }
    if (triad_.dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            triad_.dragging = false;
            return;
        }
        double t;
        if (axisParam(triad_.axis, t)) {
            double wpp = 2.0 * cam_.halfHeightAtTarget() / std::max(1.f, io.DisplaySize.y);
            double raw = wpp * 4.0;
            double p10 = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-6))));
            double step = raw / p10 < 2 ? p10 : raw / p10 < 5 ? 2 * p10 : 5 * p10;
            double v = std::round((triad_.startVal + t - triad_.grab) / step) * step;
            char nvb[32];
            std::snprintf(nvb, sizeof nvb, "%.6g", v);
            std::string nv = nvb;
            if (nv != bufs[triad_.axis]) {
                std::snprintf(bufs[triad_.axis], sizeof tdlg_.v1, "%s", nv.c_str());
                applyToolDialog();
            }
            // Real time: a move is a pure transform, so place the preview bodies right now instead
            // of waiting for the recompute (which confirms the same place a moment later).
            {
                double off[3] = {0, 0, 0};
                for (int a = 0; a < 3; ++a)
                    try { off[a] = peval::eval(bufs[a], doc_.params); } catch (const std::runtime_error&) {}
                glm::mat4 T = glm::translate(glm::mat4(1.f), glm::vec3((float)off[0], (float)off[1], (float)off[2]));
                std::vector<uint32_t> outs = doc_.featureBodies(tdlg_.featureId);
                for (size_t k = 0; k < outs.size() && k < tdlg_.targets.size(); ++k) {
                    std::vector<uint32_t> src = doc_.featureBodies(tdlg_.targets[k].first);
                    if (tdlg_.targets[k].second >= src.size()) continue;
                    auto sb = doc_.bodies.find(src[tdlg_.targets[k].second]);
                    auto ob = doc_.bodies.find(outs[k]);
                    if (sb == doc_.bodies.end() || ob == doc_.bodies.end()) continue;
                    glm::mat4 want = T * sb->second.xform;
                    if (ob->second.xform != want) { ob->second.xform = want; markSceneDirty(); }
                }
            }
            char buf[48];
            std::snprintf(buf, sizeof buf, "%s %.4g mm", triad_.axis == 0 ? "X" : triad_.axis == 1 ? "Y" : "Z", v);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            ImVec2 p(m.x + 18 * S, m.y + 8 * S);
            dl->AddRectFilled(ImVec2(p.x - 4 * S, p.y - 2 * S), ImVec2(p.x + ts.x + 4 * S, p.y + ts.y + 2 * S), ui::th(IM_COL32(255, 255, 255, 235)), 3 * S);
            dl->AddText(p, ui::th(IM_COL32(30, 36, 46, 255)), buf);
        }
        manipHovered_ = true;
        activity();
    }
}

// ===================================================================== marking menu

namespace {
struct MMItem {
    std::string label;
    Icon icon = Icon::None;
    std::function<void()> run;
    bool enabled = true;
    bool sub = false;     // opens the sketch sub menu
};
} // namespace

void App::openMarkingMenu(ImVec2 at) {
    mm_ = MarkMenu{};
    mm_.open = true;
    mm_.center = at;
    mm_.origin = at;
    mm_.t0 = time_;
    activity();
}

void App::drawMarkingMenu() {
    if (!mm_.open) return;
    const float S = uiScale_;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const bool inCmd = dlg_.open || tdlg_.open;
    const bool sketchTool = sk_.active && (!sk_.tool.empty() || !sk_.clicks.empty());

    auto toolItem = [&](const char* id) {
        int ti = findTool(id);
        MMItem it;
        if (ti < 0) return it;
        it.label = tools_[ti].label;
        it.icon = tools_[ti].icon;
        std::string sid = id;
        it.run = [this, sid] { runTool(sid); };
        it.enabled = tools_[ti].available();
        return it;
    };
    // 8 wedges, clockwise from north: N NE E SE S SW W NW
    MMItem w[8];
    if (!mm_.sub) {
        if (!lastToolId_.empty() && findTool(lastToolId_) >= 0) {
            int ti = findTool(lastToolId_);
            w[0].label = "Repeat " + tools_[ti].label;
            w[0].icon = tools_[ti].icon;
            std::string id = lastToolId_;
            w[0].run = [this, id] { runTool(id); };
        } else {
            w[0].label = "Repeat";
            w[0].enabled = false;
        }
        w[1].label = "Delete";
        w[1].icon = Icon::None;
        w[1].run = [this] {
            if (sk_.active) sketchDelete();
            else deleteSelection();
        };
        w[1].enabled = sk_.active ? (!sk_.selEnt.empty() || !sk_.selCon.empty()) : anySelection();
        if (inCmd || sketchTool) {
            w[2].label = "OK";
            w[2].run = [this] { commandOk(); };
            w[6].label = "Cancel";
            w[6].run = [this] { commandCancel(); };
        } else if (sk_.active) {
            w[2] = toolItem("finish_sketch");
            w[6].label = "Undo";
            w[6].icon = Icon::Undo;
            w[6].run = [this] { undo(); };
            w[6].enabled = undo_.canUndo();
        } else {
            w[2] = toolItem("press_pull");
            w[6].label = "Undo";
            w[6].icon = Icon::Undo;
            w[6].run = [this] { undo(); };
            w[6].enabled = undo_.canUndo();
        }
        if (sk_.active) {
            w[3] = toolItem("dimension");
            w[5] = toolItem("trim");
        } else {
            w[3] = toolItem("hole");
            w[5] = toolItem("move");
        }
        w[4].label = "Sketch";
        w[4].icon = Icon::Sketch;
        w[4].sub = true;
        w[7].label = "Redo";
        w[7].icon = Icon::Redo;
        w[7].run = [this] { redo(); };
        w[7].enabled = undo_.canRedo();
    } else {
        w[0].label = sk_.active ? "Finish Sketch" : "Create Sketch";
        w[0].icon = sk_.active ? Icon::FinishSketch : Icon::Sketch;
        w[0].run = [this] {
            if (sk_.active) finishSketch();
            else beginCreateSketch();
        };
        w[1] = toolItem("rectangle");
        w[2] = toolItem("circle");
        w[3] = toolItem("dimension");
        w[4] = toolItem("line");
        w[5] = toolItem("offset");
        w[6] = toolItem("project");
        w[7] = toolItem("arc");
    }

    // Overflow list (context items) below the radial menu.
    std::vector<MMItem> list;
    if (!mm_.sub) {
        auto add = [&](const std::string& l, std::function<void()> f, bool en = true) {
            MMItem it;
            it.label = l;
            it.run = std::move(f);
            it.enabled = en;
            list.push_back(std::move(it));
        };
        std::vector<uint32_t> bodies = selectedBodies();
        bool sel = !bodies.empty();
        if (sk_.active) {
            add("Finish Sketch", [this] { finishSketch(); });
            add("Look At", [this] { sketchLookAt(); });
            add(sk_.construction ? "Normal / Construction (on)" : "Normal / Construction", [this] { sketchToggleConstruction(); });
        } else {
            if (sel && selFaces_.size() == 1 && selEdges_.empty()) {
                SubRef f = *selFaces_.begin();
                add("Create Sketch on Face", [this, f] { createSketchOnFace(f.first, f.second); });
                add("Look At", [this, f] { lookAtFace(f.first, f.second); });
            }
            uint32_t editF = 0;
            if (bodies.size() == 1) {
                auto bit = doc_.bodies.find(bodies[0]);
                if (bit != doc_.bodies.end()) editF = bit->second.featureId;
            }
            if (!selProfiles_.empty()) {
                uint32_t sid = selProfiles_.begin()->first;
                add("Edit Sketch", [this, sid] { editSketch(sid); });
            }
            if (editF) {
                int fi = doc_.featureIndex(editF);
                if (fi >= 0) add("Edit Feature (" + doc_.timeline[fi].name + ")", [this, editF] { editFeature(editF); });
            }
            add("Find in Window", [this] { fitView(true); }, sel);
            add("Isolate", [this, bodies] { isolate(bodies); }, sel);
            if (!isolated_.empty()) add("Unisolate", [this] { exitIsolate(); });
            add("Show / Hide   V", [this, bodies] { toggleVisibility(bodies); }, sel);
            add("Show All", [this] { showAll(); });
            add("Appearance...   A", [this, bodies] { selectBodies(bodies, false); appearanceOpen_ = true; appearanceUndoPushed_ = false; }, sel);
            if (bodies.size() == 1) {
                auto bit = doc_.bodies.find(bodies[0]);
                auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
                if (pit != doc_.parts.end() && !pit->second.shape && pit->second.isMesh)
                    add("Convert Mesh to Solid", [this, b = bodies[0]] { convertToSolid(b); });
            }
            add("Send to Slicer   Ctrl+P", [this] { sendToSlicer(); });
            add(settings_.bodyPriority ? "Selection: Body Priority (click for faces)" : "Selection: Face Priority (click for bodies)",
                [this] { settings_.bodyPriority = !settings_.bodyPriority; clearSelection(); });
        }
        add("Fit   F6", [this] { fitView(false); });
        add("Home View", [this] { runTool("home_view"); });
    }

    // Layout
    const float R = 100 * S, rIn = 22 * S;
    ImVec2 c = mm_.center;
    ImVec2 m = io.MousePos;
    float dx = m.x - c.x, dy = m.y - c.y, dist = std::hypot(dx, dy);
    int hot = -1;
    if (dist > rIn) {
        float ang = std::atan2(dx, -dy);   // 0 = north, clockwise positive
        if (ang < 0) ang += 2 * (float)kPiD;
        hot = (int)std::floor((ang + (float)kPiD / 8) / ((float)kPiD / 4)) % 8;
    }
    // Item rectangles
    ImVec2 rects0[8], rects1[8];
    for (int i = 0; i < 8; ++i) {
        float a = i * (float)kPiD / 4;
        ImVec2 p(c.x + std::sin(a) * R, c.y - std::cos(a) * R);
        std::string lbl = w[i].label.empty() ? "" : w[i].label + (w[i].sub ? "  >" : "");
        ImVec2 ts = ImGui::CalcTextSize(lbl.c_str());
        float bw = ts.x + (w[i].icon != Icon::None ? 30 : 16) * S, bh = 26 * S;
        float ax = std::sin(a), ay = -std::cos(a);
        // Items hang outward from the ring: left items end at it, right items start at it.
        ImVec2 r0(p.x - bw * 0.5f, p.y - bh * 0.5f);
        if (ax > 0.3f) r0.x = p.x - 12 * S;
        else if (ax < -0.3f) r0.x = p.x - bw + 12 * S;
        if (ay < -0.9f) r0.y = p.y - bh;
        else if (ay > 0.9f) r0.y = p.y;
        rects0[i] = r0;
        rects1[i] = ImVec2(r0.x + bw, r0.y + bh);
    }
    // In click mode the mouse may also sit directly on an item box.
    for (int i = 0; i < 8; ++i)
        if (m.x >= rects0[i].x && m.x <= rects1[i].x && m.y >= rects0[i].y && m.y <= rects1[i].y) hot = i;

    // Draw
    dl->AddCircleFilled(c, rIn, ui::th(IM_COL32(255, 255, 255, 230)), 32);
    dl->AddCircle(c, rIn, ui::th(IM_COL32(150, 158, 170, 255)), 32, 1.2f * S);
    if (hot >= 0 && dist > rIn) {
        float a0 = hot * (float)kPiD / 4 - (float)kPiD / 8 - (float)kPiD / 2, a1 = a0 + (float)kPiD / 4;
        dl->PathArcTo(c, rIn - 2 * S, a0, a1, 12);
        dl->PathStroke(ui::th(IM_COL32(30, 120, 240, 255)), 0, 4 * S);
    }
    if (mm_.sub) {
        dl->AddText(ImVec2(c.x - ImGui::CalcTextSize("Sketch").x * 0.5f, c.y - ImGui::GetTextLineHeight() * 0.5f), ui::th(IM_COL32(60, 66, 78, 255)), "Sketch");
    }
    for (int i = 0; i < 8; ++i) {
        if (w[i].label.empty()) continue;
        bool h = hot == i;
        bool en = w[i].enabled && (w[i].run || w[i].sub);
        dl->AddRectFilled(ImVec2(rects0[i].x + 1, rects0[i].y + 2), ImVec2(rects1[i].x + 1, rects1[i].y + 2), IM_COL32(0, 0, 0, 30), 5 * S);
        dl->AddRectFilled(rects0[i], rects1[i], h && en ? ui::th(IM_COL32(214, 232, 255, 255)) : ui::th(IM_COL32(252, 252, 253, 250)), 5 * S);
        dl->AddRect(rects0[i], rects1[i], h && en ? ui::th(IM_COL32(40, 120, 240, 255)) : ui::th(IM_COL32(180, 186, 196, 255)), 5 * S, 0, 1.f);
        float tx = rects0[i].x + 8 * S;
        if (w[i].icon != Icon::None) {
            drawIcon(dl, ImVec2(rects0[i].x + 14 * S, (rects0[i].y + rects1[i].y) * 0.5f), 17 * S, w[i].icon, en);
            tx += 14 * S;
        }
        std::string lbl = w[i].label + (w[i].sub ? "  >" : "");
        dl->AddText(ImVec2(tx, rects0[i].y + (26 * S - ImGui::GetTextLineHeight()) * 0.5f), en ? ui::th(IM_COL32(34, 40, 50, 255)) : ui::th(IM_COL32(160, 166, 176, 255)), lbl.c_str());
    }
    // Overflow list
    int listHot = -1;
    if (!list.empty()) {
        float lw = 0;
        for (auto& it : list) lw = std::max(lw, ImGui::CalcTextSize(it.label.c_str()).x);
        lw += 28 * S;
        float rowH = 22 * S;
        ImVec2 l0(c.x - lw * 0.5f, rects1[4].y + 10 * S);
        // Keep it on screen.
        float lh = rowH * list.size() + 8 * S;
        if (l0.y + lh > io.DisplaySize.y - 4) l0.y = std::max(4.f, io.DisplaySize.y - 4 - lh);
        l0.x = std::clamp(l0.x, 4.f, io.DisplaySize.x - lw - 4);
        ImVec2 l1(l0.x + lw, l0.y + lh);
        dl->AddRectFilled(ImVec2(l0.x + 1, l0.y + 2), ImVec2(l1.x + 1, l1.y + 2), IM_COL32(0, 0, 0, 30), 5 * S);
        dl->AddRectFilled(l0, l1, ui::th(IM_COL32(252, 252, 253, 250)), 5 * S);
        dl->AddRect(l0, l1, ui::th(IM_COL32(180, 186, 196, 255)), 5 * S);
        for (size_t i = 0; i < list.size(); ++i) {
            ImVec2 r0(l0.x + 4 * S, l0.y + 4 * S + rowH * i), r1(l1.x - 4 * S, r0.y + rowH);
            bool h = m.x >= r0.x && m.x <= r1.x && m.y >= r0.y && m.y <= r1.y;
            if (h) { listHot = (int)i; hot = -1; }
            if (h && list[i].enabled) dl->AddRectFilled(r0, r1, ui::th(IM_COL32(214, 232, 255, 255)), 3 * S);
            dl->AddText(ImVec2(r0.x + 8 * S, r0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                        list[i].enabled ? ui::th(IM_COL32(34, 40, 50, 255)) : ui::th(IM_COL32(160, 166, 176, 255)), list[i].label.c_str());
        }
    }

    auto fire = [&](const MMItem& it) {
        if (!it.enabled) return;
        if (it.sub) {
            // Sub menu opens centred on the Sketch item, gestures continue from there.
            mm_.sub = true;
            mm_.center = m;
            mm_.clickMode = mm_.clickMode || !ImGui::IsMouseDown(ImGuiMouseButton_Right);
            return;
        }
        mm_.open = false;
        if (it.run) {
            std::string err;
            platform::guarded("marking menu", it.run, err);
            if (!err.empty()) toast(err, ToastKind::Error);
        }
    };

    // Gesture: right button still held. Moving far toward an item and releasing runs it.
    if (!mm_.clickMode) {
        if (hot >= 0 && dist > R * 0.55f && w[hot].sub && ImGui::IsMouseDown(ImGuiMouseButton_Right) && dist > R * 0.9f) {
            fire(w[hot]);   // flick through "Sketch": open its sub menu under the cursor
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            float moved = std::hypot(m.x - mm_.origin.x, m.y - mm_.origin.y);
            if (listHot >= 0) fire(list[listHot]);
            else if (hot >= 0 && dist > rIn && (moved > 25 * S || mm_.sub)) fire(w[hot]);
            else mm_.clickMode = true;   // a plain right click: menu stays open for clicking
        }
    } else {
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (click) {
            if (listHot >= 0) fire(list[listHot]);
            else if (hot >= 0 && dist > rIn) fire(w[hot]);
            else mm_.open = false;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mm_.open = false;
        }
    }
    activity();
}

// ===================================================================== command helpers

void App::commandOk() {
    if (dlg_.open) { closeFeatureDialog(true); return; }
    if (tdlg_.open) { closeToolDialog(true); return; }
    if (sk_.active) {
        // OK inside a sketch tool ends the current chain / command.
        sk_.clicks.clear();
        sk_.step = 0;
        sk_.chainStart = 0;
        sk_.tool.clear();
    }
}

void App::commandCancel() {
    if (dlg_.open) { closeFeatureDialog(false); return; }
    if (tdlg_.open) { closeToolDialog(false); return; }
    if (sk_.active) sketchCancel();
}

void App::lookAtFace(uint32_t bodyId, uint32_t face) {
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return;
    glm::dvec3 c, n;
    bool planar = false;
    if (!kernel::faceInfo(pit->second.shape, face, c, n, planar)) return;
    glm::dmat4 xf(bit->second.xform);
    glm::vec3 wc = glm::vec3(xf * glm::dvec4(c, 1.0));
    glm::vec3 wn = glm::normalize(glm::vec3(xf * glm::dvec4(n, 0.0)));
    glm::quat r = Camera::lookRotation(wn, std::abs(wn.z) > 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1));
    cam_.animateTo(wc, cam_.distance, r);
    activity();
}

} // namespace kst
