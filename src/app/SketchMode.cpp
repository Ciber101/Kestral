// Sketch mode: plane selection, drawing tools, constraints, dimensions, trim, mirror, and the
// overlay that draws the sketch on top of the 3D view. The solver lives in sketch/Solver.cpp.
#include "app/App.h"
#include "kernel/Features.h"
#include "core/Log.h"
#include "sketch/Profiles.h"
#include "sketch/Solver.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace kst {

using sk::CType;
using sk::EntType;
using sk::Entity;
using sk::Sketch;
using sk::Vec2;
using ui::Icon;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2 * kPi;

enum PickMode { PickPoints = 1, PickCurves = 2, PickLocked = 4 };

const std::set<std::string>& createToolIds() {
    static const std::set<std::string> ids = {"line", "rectangle", "rect_center", "circle", "circle_2pt", "circle_3pt",
                                              "ellipse", "ellipse_arc", "arc", "arc_center", "polygon", "polygon_in", "slot", "slot_overall", "slot_center", "slot_arc", "point", "cross_point", "spline", "control_spline", "rect_3pt", "text"};
    return ids;
}
bool isCreateTool(const std::string& t) { return createToolIds().count(t) > 0; }
bool isConstraintTool(const std::string& t) {
    static const std::set<std::string> ids = {"c_coincident", "c_midpoint", "c_concentric", "c_collinear", "c_parallel",
                                              "c_perpendicular", "c_hv", "c_tangent", "c_equal", "c_symmetric", "c_fix"};
    return ids.count(t) > 0;
}

// p0 and p1 are one side. The width is the perpendicular distance from that side to c.
bool rect3Corners(Vec2 a, Vec2 b, Vec2 c, Vec2 out[4]) {
    Vec2 d = b - a;
    double len = glm::length(d);
    if (len < 1e-9) return false;
    Vec2 n(-d.y / len, d.x / len);
    double w = glm::dot(c - a, n);
    if (std::abs(w) < 1e-6) return false;
    out[0] = a;
    out[1] = b;
    out[2] = b + n * w;
    out[3] = a + n * w;
    return true;
}

double wrap2pi(double a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a;
}

Vec2 perp(Vec2 v) { return {-v.y, v.x}; }
double cross2(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

std::string fmtNum(double v, int maxDecimals = 4) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", maxDecimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

// View transform helper for the active sketch plane.
struct PlaneView {
    const SketchState& st;
    const sk::Plane& pl;
    bool toScreen(Vec2 p, ImVec2& out) const {
        glm::dvec4 c = st.vp * glm::dvec4(pl.toWorld(p), 1.0);
        if (c.w <= 1e-9) return false;
        out.x = (float)((c.x / c.w * 0.5 + 0.5) * st.W);
        out.y = (float)((c.y / c.w * 0.5 + 0.5) * st.H);
        return std::isfinite(out.x) && std::isfinite(out.y);
    }
    // Ray under a screen position, in world space.
    void ray(ImVec2 s, glm::dvec3& o, glm::dvec3& d) const {
        double nx = s.x / st.W * 2.0 - 1.0, ny = s.y / st.H * 2.0 - 1.0;
        glm::dvec4 a = st.invVp * glm::dvec4(nx, ny, 1.0, 1.0);
        glm::dvec4 b = st.invVp * glm::dvec4(nx, ny, 0.5, 1.0);
        glm::dvec3 pa = glm::dvec3(a) / a.w, pb = glm::dvec3(b) / b.w;
        o = pa;
        d = glm::normalize(pb - pa);
    }
    bool toPlane(ImVec2 s, Vec2& out) const {
        glm::dvec3 o, d;
        ray(s, o, d);
        return pl.intersect(o, d, out);
    }
};

std::string snapText(const Sketch& s, const SnapInfo& si) {
    switch (si.kind) {
    case SnapKind::Point: {
        if (si.point == 0 && si.curve == 0xFFFFFFFFu) return "Face center";
        if (si.point == Sketch::kOrigin) return "Origin";
        for (auto& [id, e] : s.ents)
            if ((e.type == EntType::Circle || e.type == EntType::Arc) && e.c == si.point) return "Center";
        return "Endpoint";
    }
    case SnapKind::OnCurve: return si.curve == Sketch::kXAxis ? "On X axis" : si.curve == Sketch::kYAxis ? "On Y axis" : "On curve";
    case SnapKind::Midpoint: return "Midpoint";
    case SnapKind::Horizontal: return "Horizontal";
    case SnapKind::Vertical: return "Vertical";
    default: return {};
    }
}

SnapInfo computeSnap(const Sketch& s, const Vec2& raw, double tol, bool useGrid, double grid, bool infer, bool hasAnchor, Vec2 anchor) {
    SnapInfo r;
    r.pos = raw;
    // Closest point, closest edge middle, and the face center compete. The nearest one wins, so a
    // corner does not steal the middle of an edge when the view is zoomed out.
    double bestPt = tol, bestMid = tol * 1.6, bestFace = tol * 1.8;
    uint32_t bp = 0, mc = 0;
    Vec2 mp, face;
    bool haveMid = false, haveFace = false;
    for (auto& [id, e] : s.ents) {
        if (e.type != EntType::Point) continue;
        if (e.locked && id != Sketch::kOrigin) continue;
        double d = glm::length(Vec2(e.x, e.y) - raw);
        if (d < bestPt) { bestPt = d; bp = id; }
    }
    for (auto& [id, e] : s.ents) {
        if (e.type != EntType::Line) continue;
        if (id == Sketch::kXAxis || id == Sketch::kYAxis) continue;
        Vec2 m = s.center(e);
        double d = glm::length(m - raw);
        if (d < bestMid) { bestMid = d; haveMid = true; mp = m; mc = id; }
    }
    {
        Vec2 sum(0);
        int nf = 0;
        for (auto& [id, c] : s.cons) {
            if (c.type != CType::Fix) continue;
            const Entity* pe = s.find(c.e[0]);
            if (!pe || pe->type != EntType::Point) continue;
            sum += s.pt(c.e[0]);
            ++nf;
        }
        if (nf >= 3) {
            face = sum / (double)nf;
            bestFace = glm::length(face - raw);
            haveFace = bestFace < tol * 1.8;
        }
    }
    if (bp && (!haveMid || bestPt <= bestMid) && (!haveFace || bestPt <= bestFace)) {
        r.kind = SnapKind::Point; r.point = bp; r.pos = s.pt(bp); return r;
    }
    if (haveFace && (!haveMid || bestFace <= bestMid)) {
        r.kind = SnapKind::Point;
        r.point = 0;
        r.curve = 0xFFFFFFFFu;   // snapText: "Face center"
        r.pos = face;
        return r;
    }
    if (haveMid) { r.kind = SnapKind::Midpoint; r.pos = mp; r.curve = mc; return r; }
    // 2. curves (axes are infinite)
    double best = tol;
    uint32_t bc = 0;
    Vec2 bq = raw;
    {
        double dx = std::abs(raw.y);
        if (dx < best) { best = dx; bc = Sketch::kXAxis; bq = {raw.x, 0}; }
        double dy = std::abs(raw.x);
        if (dy < best) { best = dy; bc = Sketch::kYAxis; bq = {0, raw.y}; }
    }
    for (auto& [id, e] : s.ents) {
        if (e.type == EntType::Point || e.locked) continue;
        Vec2 q;
        double d = s.distanceTo(e, raw, &q);
        if (d < best) { best = d; bc = id; bq = q; }
    }
    if (bc) {
        const Entity* e = s.find(bc);
        if (e && e->type == EntType::Line && !e->locked) {
            Vec2 mid = s.center(*e);
            if (glm::length(mid - raw) < tol * 1.3) { r.kind = SnapKind::Midpoint; r.pos = mid; r.curve = bc; return r; }
        }
        r.kind = SnapKind::OnCurve; r.pos = bq; r.curve = bc;
        return r;
    }
    // 3. horizontal / vertical inference from the anchor
    if (infer && hasAnchor) {
        double dx = std::abs(raw.x - anchor.x), dy = std::abs(raw.y - anchor.y);
        if (dy < tol * 0.6 && dx > tol) {
            r.kind = SnapKind::Horizontal;
            r.pos.y = anchor.y;
            if (useGrid && grid > 0) r.pos.x = std::round(raw.x / grid) * grid;
            return r;
        }
        if (dx < tol * 0.6 && dy > tol) {
            r.kind = SnapKind::Vertical;
            r.pos.x = anchor.x;
            if (useGrid && grid > 0) r.pos.y = std::round(raw.y / grid) * grid;
            return r;
        }
    }
    // 4. grid
    if (useGrid && grid > 0) {
        r.kind = SnapKind::Grid;
        r.pos = Vec2(std::round(raw.x / grid) * grid, std::round(raw.y / grid) * grid);
    }
    return r;
}

double niceStep(double minUnits) {
    if (minUnits <= 0) return 1;
    double p = std::pow(10.0, std::floor(std::log10(minUnits)));
    double m = minUnits / p;
    double k = m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10;
    return k * p;
}

void computeTransforms(SketchState& st, const Camera& cam, const Aabb& bounds) {
    ImGuiIO& io = ImGui::GetIO();
    st.W = std::max(io.DisplaySize.x, 1.f);
    st.H = std::max(io.DisplaySize.y, 1.f);
    st.view = glm::dmat4(cam.view());
    st.proj = glm::dmat4(cam.proj(st.W / st.H, bounds));
    st.vp = st.proj * st.view;
    st.invVp = glm::inverse(st.vp);
}

// Removes a curve and the constraints that mention it. Points left without any curve are removed too.
void removeCurveOnly(Sketch& s, uint32_t id) {
    const Entity* e = s.find(id);
    if (!e) return;
    std::vector<uint32_t> pts;
    if (e->type == EntType::Line) pts = {e->a, e->b};
    else if (e->type == EntType::Circle) pts = {e->c};
    else if (e->type == EntType::Arc) pts = {e->c, e->a, e->b};
    s.ents.erase(id);
    for (auto it = s.cons.begin(); it != s.cons.end();) {
        bool hit = false;
        for (int k = 0; k < 3; ++k) if (it->second.e[k] == id) hit = true;
        if (hit) it = s.cons.erase(it); else ++it;
    }
    for (uint32_t p : pts) {
        const Entity* pe = s.find(p);
        if (!pe || pe->locked) continue;
        bool used = false;
        for (auto& [oid, o] : s.ents) {
            if (o.type == EntType::Point) continue;
            if (o.a == p || o.b == p || o.c == p) { used = true; break; }
        }
        if (used) continue;
        s.ents.erase(p);
        for (auto it = s.cons.begin(); it != s.cons.end();) {
            bool hit = false;
            for (int k = 0; k < 3; ++k) if (it->second.e[k] == p) hit = true;
            if (hit) it = s.cons.erase(it); else ++it;
        }
    }
}

// Existing point at pos, or a new one tied to whatever curve passes through it.
uint32_t attachPoint(Sketch& s, Vec2 pos, uint32_t exceptEnt) {
    for (auto& [id, e] : s.ents)
        if (e.type == EntType::Point && (!e.locked || id == Sketch::kOrigin) && glm::length(Vec2(e.x, e.y) - pos) < 1e-6) return id;
    uint32_t p = s.addPoint(pos);
    for (auto& [id, e] : s.ents) {
        if (id == exceptEnt || e.type == EntType::Point || e.locked) continue;
        if (s.distanceTo(e, pos) < 1e-6) { s.addConstraint(CType::PointOnObject, p, id); break; }
    }
    return p;
}

void collectGeomPoints(const Entity& e, std::set<uint32_t>& pts) {
    if (e.type == EntType::Point) pts.insert(e.id);
    else if (e.type == EntType::Line) { pts.insert(e.a); pts.insert(e.b); }
    else if (e.type == EntType::Circle || e.type == EntType::Ellipse) pts.insert(e.c);
    else if (e.type == EntType::Arc || e.type == EntType::EllipArc) { pts.insert(e.c); pts.insert(e.a); pts.insert(e.b); }
    else if (e.type == EntType::Spline || e.type == EntType::CtrlSpline)
        for (uint32_t p : e.pts) pts.insert(p);
}

} // namespace

// ===================================================================== lifecycle

Sketch* App::curSketch() {
    auto it = doc_.sketches.find(sk_.sketchId);
    return it == doc_.sketches.end() ? nullptr : &it->second;
}

bool App::sketchShown(const Sketch& s) const {
    // A used sketch is hidden, but Split still has to see its lines.
    bool showForTool = tdlg_.open && ((tdlg_.type == FeatureType::Split && tdlg_.option2 == 1) ||
                                      (tdlg_.type == FeatureType::Pattern && tdlg_.option2 == 2));
    if (!s.visible && !showForTool) return false;
    int fi = doc_.featureIndex(s.featureId);
    if (fi < 0) return true;
    return fi < doc_.markerPos && !doc_.timeline[fi].suppressed;
}

static void resetTransient(SketchState& st) {
    st.tool.clear();
    st.step = 0;
    st.clicks.clear();
    st.picks.clear();
    st.selEnt.clear();
    st.selCon.clear();
    st.hoverEnt = st.hoverCon = 0;
    st.hoverPlane = -1;
    st.pressed = st.dragging = st.boxSel = false;
    st.dragOrig.clear();
    st.dimPlacing = false;
    st.dimEdit = 0;
    st.chainStart = 0;
    st.cursorValid = false;
}

void App::beginCreateSketch(const std::string& pendingTool) {
    if (sk_.active) finishSketch();
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    // A flat face picked first: sketch on it straight away (Fusion).
    if (selFaces_.size() == 1 && selEdges_.empty()) {
        SubRef f = *selFaces_.begin();
        clearSelection();
        createSketchOnFace(f.first, f.second, pendingTool);
        if (sk_.active) return;
    }
    resetTransient(sk_);
    sk_.choosingPlane = true;
    sk_.pendingTool = pendingTool;
    selection_.clear();
    markSceneDirty();
    toast("Select a plane or a planar face to sketch on");
    activity();
}

void App::createSketchOn(sk::Plane plane, std::string tool) {
    pushUndo("Create Sketch");
    Sketch s;
    s.id = doc_.newId();
    int n = 1;
    for (;; ++n) {
        std::string cand = "Sketch" + std::to_string(n);
        bool taken = false;
        for (auto& [id, o] : doc_.sketches) if (o.name == cand) taken = true;
        if (!taken) { s.name = cand; break; }
    }
    s.plane = plane;
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::Sketch;
    f.name = s.name;
    f.sketchId = s.id;
    s.featureId = f.id;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    uint32_t sid = s.id;
    doc_.sketches[sid] = std::move(s);
    doc_.touch();

    resetTransient(sk_);
    sk_.active = false;
    sk_.choosingPlane = false;
    sk_.pendingTool.clear();
    editSketch(sid);
    if (!tool.empty()) setSketchTool(tool);
}

void App::editSketch(uint32_t id) {
    auto it = doc_.sketches.find(id);
    if (it == doc_.sketches.end()) return;
    if (sk_.active && sk_.sketchId != id) finishSketch();
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    bool already = sk_.active && sk_.sketchId == id;
    if (!already) {
        // Fusion rolls the timeline back to the sketch while you edit it.
        int fi = doc_.featureIndex(it->second.featureId);
        if (fi >= 0 && fi + 1 < doc_.markerPos) {
            savedMarker_ = doc_.markerPos;
            doc_.markerPos = fi + 1;
        }
        clearSelection();
    }
    if (!already) resetTransient(sk_);
    sk_.active = true;
    sk_.choosingPlane = false;
    sk_.sketchId = id;
    isolated_.clear();
    selection_.clear();
    it->second.visible = true;
    sk::computeDof(it->second);
    if (!already) sketchLookAt();
    markSceneDirty();
    doc_.touch();
    activity();
}

void App::leaveSketchMode() {
    resetTransient(sk_);
    sk_.active = false;
    sk_.choosingPlane = false;
    sk_.pendingTool.clear();
    markSceneDirty();
    activity();
}

void App::finishSketch() {
    if (!sk_.active) return;
    Sketch* s = curSketch();
    uint32_t sid = s ? s->id : 0;
    if (s && s->geometryCount() == 0) {
        // Nothing drawn: drop the empty sketch and its timeline entry.
        uint32_t fid = s->featureId;
        for (int i = 0; i < (int)doc_.timeline.size(); ++i) {
            if (doc_.timeline[i].id == fid) {
                doc_.timeline.erase(doc_.timeline.begin() + i);
                if (doc_.markerPos > i) doc_.markerPos--;
                if (savedMarker_ > i) savedMarker_--;
                break;
            }
        }
        doc_.sketches.erase(sid);
    } else if (s) {
        sk::computeDof(*s);
        if (sketchInUse(sid)) s->visible = false;   // a feature is built on it: its lines get out of the way
    }
    doc_.touch();
    uint32_t fid = s && doc_.sketches.count(sid) ? doc_.sketches[sid].featureId : 0;
    leaveSketchMode();
    // Everything after the sketch rebuilds in the background (signatures skip what did not change).
    int fi = fid ? doc_.featureIndex(fid) : -1;
    restoreMarkerAfterEdit();
    if (fi >= 0) scheduleRecompute(fi);
}

// ===================================================================== tools

void App::sketchToolCommand(const std::string& id) {
    if (!sk_.active) {
        if (isCreateTool(id) || id == "project") { beginCreateSketch(id); return; }
        toast("Start a sketch first: choose Create Sketch, then a plane");
        return;
    }
    setSketchTool(id);
}

void App::setSketchTool(const std::string& id) {
    Sketch* s = curSketch();
    if (!s) return;
    sk_.tool = id == "select" ? std::string() : id;
    sk_.step = 0;
    sk_.clicks.clear();
    sk_.picks.clear();
    sk_.dimPlacing = false;
    sk_.chainStart = 0;
    lastToolId_ = id;
    if (id == "construction") {
        sk_.tool.clear();
        sketchToggleConstruction();
        return;
    }
    if (id == "mirror") {
        bool any = false;
        for (uint32_t e : sk_.selEnt) { const Entity* en = s->find(e); if (en && en->type != EntType::Point) any = true; }
        if (!any) { toast("Select the objects to mirror first, then choose Mirror"); sk_.tool.clear(); return; }
        toast("Click the mirror line");
    }
    if (id == "sk_xform") {
        bool any = false;
        for (uint32_t e : sk_.selEnt) { const Entity* en = s->find(e); if (en && en->type != EntType::Point && !en->locked) any = true; }
        if (!any) { toast("Select the curves first, then choose Move / Copy"); sk_.tool.clear(); return; }
    }
    if (id == "sk_pattern") {
        bool any = false;
        for (uint32_t e : sk_.selEnt) { const Entity* en = s->find(e); if (en && en->type != EntType::Point && !en->locked) any = true; }
        if (!any) { toast("Select the curves first, then choose Pattern"); sk_.tool.clear(); return; }
    }
    if (id == "offset") {
        for (uint32_t e : sk_.selEnt) {
            const Entity* en = s->find(e);
            if (en && en->type != EntType::Point && !en->locked) { sketchOffsetPick(e); break; }
        }
        sk_.selEnt.clear();
    }
    if (id == "cross_point") toast("Crossing: click where two curves meet");
    if (id == "sk_intersect") toast("Intersect: click a body");
    if (id == "sk_onto_face") {
        bool any = false;
        for (uint32_t e : sk_.selEnt) {
            const Entity* en = s->find(e);
            if (en && en->type != EntType::Point && !en->locked) any = true;
        }
        if (!any) { toast("Select the curves first, then choose Onto Face"); sk_.tool.clear(); return; }
        toast("Onto Face: click a face. The curves drop along the sketch normal");
    }
    if (isConstraintTool(id) && !sk_.selEnt.empty()) {
        for (uint32_t e : sk_.selEnt) sk_.picks.push_back(e);
        applyConstraintTool();
    }
    activity();
}

void App::sketchToggleConstruction() {
    Sketch* s = curSketch();
    if (!s) return;
    bool any = false;
    for (uint32_t id : sk_.selEnt) { const Entity* e = s->find(id); if (e && e->type != EntType::Point && !e->locked) any = true; }
    if (!any) {
        sk_.construction = !sk_.construction;
        toast(sk_.construction ? "New geometry will be construction" : "New geometry will be normal");
        return;
    }
    pushUndo("Construction");
    for (uint32_t id : sk_.selEnt) {
        Entity* e = s->find(id);
        if (e && e->type != EntType::Point && !e->locked) e->construction = !e->construction;
    }
    sketchFinishOp();
}

bool App::sketchSolve(const sk::SolveOptions& o, bool report) {
    Sketch* s = curSketch();
    if (!s) return false;
    for (auto& [id, c] : s->cons) c.failed = false;
    auto t0 = std::chrono::steady_clock::now();
    sk::SolveResult r = sk::solve(*s, o);
    sk_.lastSolveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!r.ok) {
        for (uint32_t id : r.bad) { auto it = s->cons.find(id); if (it != s->cons.end()) it->second.failed = true; }
        if (report) toast("That does not fit the existing constraints", ToastKind::Warning);
    }
    return r.ok;
}

void App::sketchFinishOp() {
    Sketch* s = curSketch();
    if (!s) return;
    s->touch();
    sk::computeDof(*s);
    dirty_ = true;
    activity();
}

uint32_t App::sketchMakePoint(Sketch& s, const SnapInfo& si) {
    if (si.kind == SnapKind::Point && si.point && s.find(si.point)) return si.point;
    uint32_t p = s.addPoint(si.pos);
    if (si.kind == SnapKind::OnCurve && si.curve) s.addConstraint(CType::PointOnObject, p, si.curve);
    else if (si.kind == SnapKind::Midpoint && si.curve) s.addConstraint(CType::Midpoint, p, si.curve);
    return p;
}

uint32_t App::pickEntityAt(const Sketch& s, int mode, uint32_t exclude) const {
    const double ppu = std::max(sk_.ppu, 1e-9);
    double best = 1e30;
    uint32_t hit = 0;
    const double ptTol = 9.0, curveTol = 7.0;
    for (auto& [id, e] : s.ents) {
        if (id == exclude) continue;
        if (e.locked && !(mode & PickLocked)) continue;
        if (e.type == EntType::Point) {
            if (!(mode & PickPoints)) continue;
            double d = glm::length(Vec2(e.x, e.y) - sk_.raw) * ppu;
            if (d < ptTol && d - 4.0 < best) { best = d - 4.0; hit = id; }
        } else {
            if (!(mode & PickCurves)) continue;
            double d;
            if (e.locked) d = (id == Sketch::kXAxis ? std::abs(sk_.raw.y) : std::abs(sk_.raw.x)) * ppu;
            else d = s.distanceTo(e, sk_.raw) * ppu;
            if (d < curveTol && d < best) { best = d; hit = id; }
        }
    }
    return hit;
}

// ===================================================================== per frame update

namespace {
const double kPlaneHalfMin = 40.0;
sk::Plane originPlane(int i) {
    sk::Plane p;
    if (i == 0) { p.x = {1, 0, 0}; p.y = {0, 1, 0}; p.name = "XY"; }
    else if (i == 1) { p.x = {1, 0, 0}; p.y = {0, 0, 1}; p.name = "XZ"; }
    else { p.x = {0, 1, 0}; p.y = {0, 0, 1}; p.name = "YZ"; }
    return p;
}
void rayFromScreen(const SketchState& st, ImVec2 m, glm::dvec3& o, glm::dvec3& d) {
    double nx = m.x / st.W * 2.0 - 1.0, ny = m.y / st.H * 2.0 - 1.0;
    glm::dvec4 a = st.invVp * glm::dvec4(nx, ny, 1.0, 1.0);
    glm::dvec4 b = st.invVp * glm::dvec4(nx, ny, 0.5, 1.0);
    o = glm::dvec3(a) / a.w;
    d = glm::normalize(glm::dvec3(b) / b.w - o);
}
int pickMode(const std::string& t) {
    if (t.empty()) return PickPoints | PickCurves;
    if (isCreateTool(t)) return 0;
    if (t == "dimension" || t == "c_coincident" || t == "c_symmetric") return PickPoints | PickCurves | PickLocked;
    if (t == "c_midpoint" || t == "c_hv" || t == "c_fix") return PickPoints | PickCurves;
    if (t == "c_concentric" || t == "trim" || t == "extend" || t == "offset" || t == "break") return PickCurves;
    if (t == "sk_fillet" || t == "sk_chamfer") return PickPoints | PickCurves;
    if (t == "project" || t == "sk_intersect" || t == "sk_onto_face") return 0;
    if (t == "mirror" || t == "c_collinear" || t == "c_parallel" || t == "c_perpendicular" || t == "c_tangent" || t == "c_equal")
        return PickCurves | PickLocked;
    return PickPoints | PickCurves;
}
} // namespace

static double planeHalfSize(const Aabb& b) { return std::max(b.valid() ? (double)b.diag() * 0.35 : 0.0, kPlaneHalfMin * 1.5); }

void App::sketchUpdate() {
    ImGuiIO& io = ImGui::GetIO();
    computeTransforms(sk_, cam_, sceneBounds_);

    if (sk_.choosingPlane) {
        sk_.hoverPlane = -1;
        if (viewportHovered_) {
            glm::dvec3 o, d;
            rayFromScreen(sk_, io.MousePos, o, d);
            double half = planeHalfSize(sceneBounds_), bestT = 1e300;
            for (int i = 0; i < 3; ++i) {
                sk::Plane p = originPlane(i);
                Vec2 q;
                if (!p.intersect(o, d, q)) continue;
                glm::dvec3 w = p.toWorld(q);
                double t = glm::dot(w - o, d);
                if (t <= 0) continue;
                // Pickable region: the drawn square (corner at 0..half along each axis, like Fusion's origin planes)
                if (q.x >= -half * 0.1 && q.x <= half && q.y >= -half * 0.1 && q.y <= half && t < bestT) { bestT = t; sk_.hoverPlane = i; }
            }
        }
        return;
    }
    Sketch* s = curSketch();
    if (!s) { leaveSketchMode(); return; }
    PlaneView pv{sk_, s->plane};

    sk_.cursorValid = false;
    if (viewportHovered_ || sk_.dragging || sk_.pressed || sk_.boxSel) {
        Vec2 raw;
        if (pv.toPlane(io.MousePos, raw)) { sk_.raw = raw; sk_.cursorValid = true; }
    }
    {
        ImVec2 a, b;
        Vec2 at = sk_.cursorValid ? sk_.raw : Vec2(0, 0);
        if (pv.toScreen(at, a) && pv.toScreen(at + Vec2(1, 0), b)) {
            double d = std::hypot((double)(b.x - a.x), (double)(b.y - a.y));
            if (d > 1e-9) sk_.ppu = d;
        }
        sk_.gridStep = niceStep(26.0 / std::max(sk_.ppu, 1e-9));
    }

    sk_.hoverEnt = sk_.hoverCon = 0;
    if (sk_.cursorValid && !sk_.dragging && !sk_.boxSel && viewportHovered_) {
        auto inside = [&](const ImVec4& r) { return io.MousePos.x >= r.x && io.MousePos.x <= r.z && io.MousePos.y >= r.y && io.MousePos.y <= r.w; };
        for (auto& [id, r] : sk_.dimRects) if (inside(r)) { sk_.hoverCon = id; break; }
        if (!sk_.hoverCon && sk_.optCons)
            for (auto& [id, r] : sk_.conRects) if (inside(r)) { sk_.hoverCon = id; break; }
        if (!sk_.hoverCon) sk_.hoverEnt = pickEntityAt(*s, pickMode(sk_.tool));
    }
    if (sk_.cursorValid && isCreateTool(sk_.tool)) {
        bool infer = sk_.tool == "line" && !sk_.clicks.empty();
        Vec2 anchor = infer ? sk_.clicks[0].pos : Vec2(0, 0);
        double tol = 9.0 / std::max(sk_.ppu, 1e-9);
        sk_.snap = computeSnap(*s, sk_.raw, tol, sk_.optSnap, sk_.gridStep, infer, infer, anchor);
        sk_.snapLabel = snapText(*s, sk_.snap);
    }
    sketchHud();
    if (sk_.dimPlacing && sk_.cursorValid) {
        sk::Constraint c;
        if (dimensionFor(*s, sk_.picks, sk_.raw, c)) { sk_.dim = c; sk_.dim.label = sk_.raw; sk_.dim.labelSet = true; }
    }
}

// ===================================================================== plane choice

bool App::createSketchOnFace(uint32_t bodyId, uint32_t face, std::string tool) {
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return false;
    auto pit = doc_.parts.find(bit->second.partId);
    glm::dvec3 o, n;
    if (pit == doc_.parts.end() || !pit->second.shape || !kernel::facePlane(*pit->second.shape, face, o, n)) {
        toast("Only flat faces can hold a sketch. Pick a flat face or an origin plane.", ToastKind::Warning);
        return false;
    }
    glm::dmat4 M(bit->second.xform);
    glm::dvec3 ow = glm::dvec3(M * glm::dvec4(o, 1.0));
    glm::dvec3 nw = glm::normalize(glm::dmat3(M) * n);
    sk::Plane p;
    // Axes follow the world like Fusion: on a top face X stays X and Y stays Y.
    if (std::abs(nw.z) > 0.999) p.x = {nw.z > 0 ? 1.0 : -1.0, 0, 0};
    else p.x = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), nw));
    p.y = glm::cross(nw, p.x);
    p.o = nw * glm::dot(nw, ow);   // world origin projected onto the face plane
    p.name = "Face";
    createSketchOn(p, tool);
    // Fusion: the face's own edges come in projected, so new curves snap to them.
    if (sk_.active)
        if (Sketch* s = curSketch()) {
            std::vector<uint32_t> fe = kernel::faceEdges(pit->second.shape, face);
            if (fe.size() <= 400 && projectEdgesInto(*s, bodyId, fe) > 0) {
                sketchSolve({}, false);
                sketchFinishOp();
            }
        }
    return true;
}

void App::createSketchOnFace(uint32_t bodyId, uint32_t face) { createSketchOnFace(bodyId, face, std::string()); }

void App::sketchPlaneChoice() {
    ImGuiIO& io = ImGui::GetIO();
    bool throughBodies = io.KeyAlt;
    // Construction planes (offset planes) first: they sit wherever you put them.
    {
        glm::dvec3 ro, rd;
        double bestT = 1e300;
        const sk::Plane* best = nullptr;
        double half = std::max(20.0, sceneBounds_.valid() ? sceneBounds_.diag() * 0.3 : 60.0);
        if (rayAt(io.MousePos, ro, rd))
            for (auto& [fid, pl] : doc_.constructPlanes) {
                if (!doc_.isFeatureActive(fid)) continue;
                sk::Vec2 q;
                if (!pl.intersect(ro, rd, q) || std::abs(q.x) > half || std::abs(q.y) > half) continue;
                double t = glm::dot(pl.toWorld(q) - ro, rd);
                if (t > 0 && t < bestT) { bestT = t; best = &pl; }
            }
        if (best && (hoverBody_ == 0 || throughBodies)) {
            createSketchOn(*best, sk_.pendingTool);
            return;
        }
    }
    if (sk_.hoverPlane >= 0 && (hoverBody_ == 0 || throughBodies)) {
        createSketchOn(originPlane(sk_.hoverPlane), sk_.pendingTool);
        return;
    }
    if (hoverBody_ != 0 && hoverFace_ != 0) {
        createSketchOnFace(hoverBody_, hoverFace_ - 1, sk_.pendingTool);
        return;
    }
    toast("Click an origin plane or a flat face (hold Alt to pick an origin plane through a body)");
}

// ===================================================================== input

bool App::sketchKeys() {
    if (!sk_.active && !sk_.choosingPlane) return false;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    if (ImGui::GetIO().WantTextInput && !sk_.choosingPlane) return false;
    if (pressed(ImGuiKey_Escape)) {
        if (sk_.choosingPlane) { sk_.choosingPlane = false; sk_.pendingTool.clear(); markSceneDirty(); return true; }
        sketchCancel();
        return true;
    }
    if (sk_.choosingPlane) return false;
    // Heads-up value boxes: numbers go straight in, Tab moves on, Enter places.
    auto& h = sk_.hud;
    if (h.n > 0) {
        ImGuiIO& io = ImGui::GetIO();
        bool took = false;
        for (ImWchar c : io.InputQueueCharacters) {
            if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-' || c == '+' || c == '*' || c == '/' || c == '(' || c == ')') {
                h.buf[h.focus].push_back(c == ',' ? '.' : (char)c);
                took = true;
            }
        }
        if (took) { activity(); return true; }
        if (pressed(ImGuiKey_Backspace) && !h.buf[h.focus].empty()) { h.buf[h.focus].pop_back(); return true; }
        if (pressed(ImGuiKey_Tab)) { h.focus = (h.focus + 1) % h.n; return true; }
        bool typed = !h.buf[0].empty() || (h.n > 1 && !h.buf[1].empty());
        if (typed && (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter))) { sketchClick(); return true; }
    }
    if (pressed(ImGuiKey_Delete) || pressed(ImGuiKey_Backspace)) { sketchDelete(); return true; }
    if (pressed(ImGuiKey_Enter) && sk_.tool == "line") { sk_.clicks.clear(); sk_.step = 0; sk_.chainStart = 0; return true; }
    if (pressed(ImGuiKey_Enter) && (sk_.tool == "spline" || sk_.tool == "control_spline") && sk_.clicks.size() >= 2) {
        sketchCommitSpline(false, sk_.tool == "control_spline");
        return true;
    }
    return false;
}

void App::sketchCancel() {
    if (sk_.dimPlacing) { sk_.dimPlacing = false; sk_.picks.clear(); return; }
    if (sk_.dimEdit) { sk_.dimEdit = 0; return; }
    if (!sk_.clicks.empty() || !sk_.picks.empty() || sk_.step) {
        sk_.clicks.clear(); sk_.picks.clear(); sk_.step = 0; sk_.chainStart = 0;
        return;
    }
    if (!sk_.tool.empty()) { sk_.tool.clear(); return; }
    if (!sk_.selEnt.empty() || !sk_.selCon.empty()) { sk_.selEnt.clear(); sk_.selCon.clear(); return; }
    finishSketch();   // nothing left to cancel: Esc leaves the sketch (keeping it)
}

void App::sketchDelete() {
    Sketch* s = curSketch();
    if (!s || (sk_.selEnt.empty() && sk_.selCon.empty())) return;
    pushUndo("Delete");
    for (uint32_t c : sk_.selCon) s->cons.erase(c);
    s->deleteEntities(sk_.selEnt);
    sk_.selEnt.clear();
    sk_.selCon.clear();
    sketchSolve({}, false);
    sketchFinishOp();
}

bool App::sketchMouseDown() {
    if (sk_.choosingPlane) { sketchPlaneChoice(); return true; }
    Sketch* s = curSketch();
    if (!s) return false;
    if (!sk_.cursorValid) return true;
    ImGuiIO& io = ImGui::GetIO();
    // Double click a dimension to edit it.
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && sk_.hoverCon) {
        auto it = s->cons.find(sk_.hoverCon);
        if (it != s->cons.end() && sk::isDimension(it->second.type)) {
            const sk::Constraint& c = it->second;
            if (!c.expr.empty()) std::snprintf(sk_.dimBuf, sizeof sk_.dimBuf, "%s", c.expr.c_str());
            else {
                double v = c.type == CType::Angle ? std::abs(c.value) * 180.0 / kPi : std::abs(c.value);
                std::snprintf(sk_.dimBuf, sizeof sk_.dimBuf, "%s", fmtNum(v).c_str());
            }
            sk_.dimEdit = c.id;
            sk_.dimFocus = true;
            return true;
        }
    }
    if (sk_.tool.empty()) {
        sk_.pressed = true;
        sk_.pressPos = io.MousePos;
        sk_.pressEnt = sk_.hoverEnt;
        sk_.pressCon = sk_.hoverCon;
        sk_.dragStart = sk_.raw;
        return true;
    }
    sketchClick();
    return true;
}

void App::sketchMouseMove() {
    Sketch* s = curSketch();
    if (!s) return;
    ImGuiIO& io = ImGui::GetIO();
    if (sk_.pressed && !sk_.dragging && !sk_.boxSel) {
        float dx = io.MousePos.x - sk_.pressPos.x, dy = io.MousePos.y - sk_.pressPos.y;
        if (dx * dx + dy * dy > 25.f) {
            if (sk_.pressEnt && !sk_.pressCon) {
                // Start moving geometry.
                std::set<uint32_t> ents;
                if (sk_.selEnt.count(sk_.pressEnt)) ents = sk_.selEnt;
                else ents.insert(sk_.pressEnt);
                std::set<uint32_t> pts;
                for (uint32_t id : ents) {
                    const Entity* e = s->find(id);
                    if (!e || e->locked) continue;
                    collectGeomPoints(*e, pts);
                }
                sk_.dragOrig.clear();
                for (uint32_t p : pts) {
                    const Entity* pe = s->find(p);
                    if (pe && !pe->locked) sk_.dragOrig[p] = Vec2(pe->x, pe->y);
                }
                if (!sk_.dragOrig.empty()) {
                    pushUndo("Move");
                    sk_.dragging = true;
                }
            } else if (!sk_.pressCon) {
                sk_.boxSel = true;
                sk_.boxA = sk_.pressPos;
            }
        }
    }
    if (sk_.dragging && sk_.cursorValid) {
        Vec2 delta = sk_.raw - sk_.dragStart;
        sk::SolveOptions o;
        for (auto& [id, p0] : sk_.dragOrig) o.drags.push_back({id, p0 + delta});
        o.maxIter = 40;
        sketchSolve(o, false);
        s->touch();
        activity();
    }
}

void App::sketchMouseUp() {
    Sketch* s = curSketch();
    ImGuiIO& io = ImGui::GetIO();
    if (!s) { sk_.pressed = sk_.dragging = sk_.boxSel = false; return; }
    if (sk_.dragging) {
        sketchSolve({}, false);
        sketchFinishOp();
        sk_.dragging = false;
        sk_.pressed = false;
        sk_.dragOrig.clear();
        return;
    }
    if (sk_.boxSel) {
        PlaneView pv{sk_, s->plane};
        ImVec2 a = sk_.boxA, b = io.MousePos;
        ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        bool add = io.KeyCtrl || io.KeyShift;
        if (!add) { sk_.selEnt.clear(); sk_.selCon.clear(); }
        auto in = [&](Vec2 p) { ImVec2 q; return pv.toScreen(p, q) && q.x >= lo.x && q.x <= hi.x && q.y >= lo.y && q.y <= hi.y; };
        for (auto& [id, e] : s->ents) {
            if (e.locked) continue;
            bool all;
            if (e.type == EntType::Point) all = in({e.x, e.y});
            else if (e.type == EntType::Line) all = in(s->pt(e.a)) && in(s->pt(e.b));
            else {
                Sketch::ArcGeom g;
                if (!s->arcGeom(e, g)) continue;
                all = in(g.c - Vec2(g.r)) && in(g.c + Vec2(g.r));
                if (e.type == EntType::Arc) all = in(s->pt(e.a)) && in(s->pt(e.b)) && in(g.c);
            }
            if (all) sk_.selEnt.insert(id);
        }
        sk_.boxSel = false;
        sk_.pressed = false;
        return;
    }
    if (sk_.pressed) {
        bool add = io.KeyCtrl || io.KeyShift;
        if (sk_.pressCon) {
            if (!add) { sk_.selEnt.clear(); sk_.selCon.clear(); }
            if (!sk_.selCon.erase(sk_.pressCon)) sk_.selCon.insert(sk_.pressCon);
        } else if (sk_.pressEnt) {
            if (!add) { sk_.selEnt.clear(); sk_.selCon.clear(); }
            if (!sk_.selEnt.erase(sk_.pressEnt)) sk_.selEnt.insert(sk_.pressEnt);
        } else if (!add) {
            sk_.selEnt.clear();
            sk_.selCon.clear();
        }
        sk_.pressed = false;
    }
}

// ===================================================================== drawing tools

namespace {

bool circumcenter(Vec2 a, Vec2 b, Vec2 c, Vec2& o, double& r) {
    double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(d) < 1e-12) return false;
    double aa = glm::dot(a, a), bb = glm::dot(b, b), cc = glm::dot(c, c);
    o.x = (aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / d;
    o.y = (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / d;
    r = glm::length(a - o);
    return std::isfinite(o.x) && std::isfinite(o.y) && r > 1e-9;
}

// A slot whose centerline is the arc through p1, mid and p3. hw is half the width.
struct ArcSlot {
    Vec2 o, sa, sb;
    double R = 0, hw = 0, sweep = 0;
    bool line = false;
    bool ok = false;
};

ArcSlot makeArcSlot(Vec2 p1, Vec2 mid, Vec2 p3, double hw) {
    ArcSlot g;
    if (!circumcenter(p1, mid, p3, g.o, g.R) || !(g.R > 1e-6)) return g;
    double a1 = std::atan2(p1.y - g.o.y, p1.x - g.o.x);
    double a2 = std::atan2(mid.y - g.o.y, mid.x - g.o.x);
    double a3 = std::atan2(p3.y - g.o.y, p3.x - g.o.x);
    double rel3 = wrap2pi(a3 - a1);
    double rel2 = wrap2pi(a2 - a1);
    if (rel2 <= rel3 + 1e-9) { g.sa = p1; g.sb = p3; g.sweep = rel3; }
    else { g.sa = p3; g.sb = p1; g.sweep = wrap2pi(a1 - a3); }
    if (g.sweep < 1e-4 || g.sweep > kTwoPi - 1e-4) return g;
    g.line = true;
    if (!(hw > 1e-6) || hw >= g.R - 1e-6) return g;
    g.hw = hw;
    g.ok = true;
    return g;
}

// Vertices of a regular polygon. rp is the clicked radius point. For the circumscribed form the click gives the
// apothem direction; for the inscribed form it is a vertex. cr receives the reference circle radius.
std::vector<Vec2> polygonVerts(Vec2 c, Vec2 rp, int n, bool circumscribed, double& cr) {
    std::vector<Vec2> v;
    Vec2 d = rp - c;
    double R = glm::length(d);
    if (R < 1e-9 || n < 3) return v;
    double phase = std::atan2(d.y, d.x);
    double vr = circumscribed ? R / std::cos(kPi / n) : R;
    double start = circumscribed ? phase - kPi / n : phase;
    cr = R;
    for (int i = 0; i < n; ++i) {
        double a = start + kTwoPi * i / n;
        v.push_back(c + Vec2(std::cos(a), std::sin(a)) * vr);
    }
    return v;
}

} // namespace

// Values typed into the heads-up boxes become dimensions on what the click just created.
void App::sketchClick() {
    Sketch* s = curSketch();
    if (!s) return;
    SketchState::Hud hud = sk_.hud;
    std::string tool = sk_.tool;
    uint32_t idBefore = s->nextId;
    bool typed = hud.n > 0 && (!hud.buf[0].empty() || (hud.n > 1 && !hud.buf[1].empty()));
    sketchClickImpl();
    sk_.hud = SketchState::Hud{};   // fresh boxes for the next segment
    s = curSketch();
    if (!s || !typed || s->nextId == idBefore) return;
    auto val = [&](int i, double& v) {
        if (i >= hud.n || hud.buf[i].empty()) return false;
        try { v = peval::eval(hud.buf[i], doc_.params); return std::isfinite(v); } catch (const std::runtime_error&) { return false; }
    };
    std::vector<uint32_t> lines, circles;
    for (auto it = s->ents.lower_bound(idBefore); it != s->ents.end(); ++it) {
        if (it->second.construction && tool != "line") continue;
        if (it->second.type == EntType::Line) lines.push_back(it->first);
        else if (it->second.type == EntType::Circle) circles.push_back(it->first);
    }
    bool added = false;
    auto addDim = [&](CType t, uint32_t a, uint32_t b, double v) {
        s->addConstraint(t, a, b, 0, v);
        added = true;
    };
    double v = 0;
    if (tool == "line" && !lines.empty()) {
        const Entity* l = s->find(lines.back());
        if (val(0, v) && v > 1e-9) addDim(CType::Distance, l->a, l->b, std::abs(v));
        if (val(1, v)) {
            Vec2 d = s->pt(l->b) - s->pt(l->a);
            double a = std::atan2(d.y, d.x);
            addDim(CType::Angle, Sketch::kXAxis, l->id, a);
        }
    } else if ((tool == "rectangle" || tool == "rect_center") && !lines.empty()) {
        uint32_t hl = 0, vl = 0;
        for (uint32_t id : lines) {
            if (!hl && s->hasConstraint(CType::Horizontal, id)) hl = id;
            if (!vl && s->hasConstraint(CType::Vertical, id)) vl = id;
        }
        if (hl && val(0, v) && v > 1e-9) addDim(CType::Distance, s->find(hl)->a, s->find(hl)->b, std::abs(v));
        if (vl && val(1, v) && v > 1e-9) addDim(CType::Distance, s->find(vl)->a, s->find(vl)->b, std::abs(v));
    } else if (tool == "rect_3pt" && lines.size() >= 2) {
        if (val(0, v) && v > 1e-9) addDim(CType::Distance, s->find(lines[1])->a, s->find(lines[1])->b, std::abs(v));
    } else if ((tool == "circle" || tool == "circle_2pt") && !circles.empty()) {
        if (val(0, v) && v > 1e-9) addDim(CType::Diameter, circles.back(), 0, std::abs(v));
    } else if ((tool == "polygon" || tool == "polygon_in") && !circles.empty()) {
        if (val(0, v) && v > 1e-9) addDim(CType::Diameter, circles.back(), 0, std::abs(v));
    }
    if (added) {
        if (!sketchSolve({}, false)) toast("The typed value conflicts with other constraints", ToastKind::Warning);
        sketchFinishOp();
    }
}

// Sets up the value boxes for the current tool step and bends the cursor point to typed values.
void App::sketchHud() {
    auto& h = sk_.hud;
    const std::string& t = sk_.tool;
    int n = 0;
    const char* l0 = nullptr;
    const char* l1 = nullptr;
    if (!sk_.clicks.empty()) {
        size_t c = sk_.clicks.size();
        if (t == "line" && c == 1) { n = 2; l0 = "Length"; l1 = "Angle"; }
        else if ((t == "rectangle" || t == "rect_center") && c == 1) { n = 2; l0 = "Width"; l1 = "Height"; }
        else if (t == "rect_3pt" && c == 1) { n = 2; l0 = "Length"; l1 = "Angle"; }
        else if (t == "rect_3pt" && c >= 2) { n = 1; l0 = "Width"; }
        else if (t == "slot_overall" && c == 1) { n = 1; l0 = "Length"; }
        else if (t == "slot_overall" && c >= 2) { n = 1; l0 = "Width"; }
        else if (t == "slot_center" && c == 1) { n = 2; l0 = "Length"; l1 = "Angle"; }
        else if (t == "slot_center" && c >= 2) { n = 1; l0 = "Width"; }
        else if (t == "slot_arc" && c >= 3) { n = 1; l0 = "Width"; }
        else if ((t == "circle" || t == "circle_2pt" || t == "polygon" || t == "polygon_in") && c == 1) { n = 1; l0 = "Diameter"; }
    }
    if (t == "offset" && !sk_.picks.empty()) { n = 1; l0 = "Offset"; }
    std::string key = n ? t + "#" + std::to_string(sk_.clicks.size()) : std::string();
    if (key != h.key) {
        h = SketchState::Hud{};
        h.key = key;
    }
    h.n = n;
    h.label[0] = l0;
    h.label[1] = l1;
    if (!n || !sk_.cursorValid) return;
    if (t == "offset") {
        h.live[0] = std::abs(sketchOffsetDistance(sk_.raw));
        return;
    }
    auto num = [&](int i, double& v) {
        if (h.buf[i].empty()) return false;
        try { v = peval::eval(h.buf[i], doc_.params); return std::isfinite(v); } catch (const std::runtime_error&) { return false; }
    };
    Vec2 a = sk_.clicks[0].pos, p = sk_.snap.pos;
    bool over = false;
    if (t == "line") {
        Vec2 d = p - a;
        double len = glm::length(d), ang = std::atan2(d.y, d.x);
        h.live[0] = len;
        h.live[1] = ang * 180.0 / kPi;
        double L, A;
        bool hl = num(0, L), ha = num(1, A);
        if (hl || ha) {
            double a2 = ha ? A * kPi / 180.0 : ang;
            double l2 = hl ? std::abs(L) : len;
            p = a + Vec2(std::cos(a2), std::sin(a2)) * l2;
            over = true;
        }
    } else if (t == "rect_3pt" && sk_.clicks.size() == 1) {
        Vec2 d = p - a;
        double len = glm::length(d), ang = std::atan2(d.y, d.x);
        h.live[0] = len;
        h.live[1] = ang * 180.0 / kPi;
        double L, A;
        bool hl = num(0, L), ha = num(1, A);
        if (hl || ha) {
            double a2 = ha ? A * kPi / 180.0 : ang;
            double l2 = hl ? std::abs(L) : len;
            p = a + Vec2(std::cos(a2), std::sin(a2)) * l2;
            over = true;
        }
    } else if (t == "rect_3pt" && sk_.clicks.size() >= 2) {
        Vec2 b = sk_.clicks[1].pos, d = b - a;
        double len = glm::length(d);
        if (len > 1e-9) {
            Vec2 u = d / len, n(-u.y, u.x);
            double along = glm::dot(p - a, u);
            double w = glm::dot(p - a, n);
            h.live[0] = std::abs(w);
            double W;
            if (num(0, W) && std::abs(W) > 1e-9) {
                double s = w >= 0 ? 1.0 : -1.0;
                p = a + u * along + n * s * std::abs(W);
                over = true;
            }
        }
    } else if (t == "slot_overall" && sk_.clicks.size() == 1) {
        Vec2 d = p - a;
        h.live[0] = glm::length(d);
        double L;
        if (num(0, L) && std::abs(L) > 1e-9) {
            double len = glm::length(d);
            Vec2 u = len > 1e-12 ? d / len : Vec2(1, 0);
            p = a + u * std::abs(L);
            over = true;
        }
    } else if (t == "slot_overall" && sk_.clicks.size() >= 2) {
        Vec2 b = sk_.clicks[1].pos, d = b - a;
        double len = glm::length(d);
        if (len > 1e-9) {
            Vec2 u = d / len, n = perp(u);
            double side = glm::dot(p - a, n);
            h.live[0] = std::abs(side) * 2.0;
            double W;
            if (num(0, W) && std::abs(W) > 1e-9) {
                double s = side >= 0 ? 1.0 : -1.0;
                p = (a + b) * 0.5 + n * s * std::abs(W) * 0.5;
                over = true;
            }
        }
    } else if (t == "slot_center" && sk_.clicks.size() == 1) {
        Vec2 d = p - a;
        double len = glm::length(d), ang = std::atan2(d.y, d.x);
        h.live[0] = len;
        h.live[1] = ang * 180.0 / kPi;
        double L, A;
        bool hl = num(0, L), ha = num(1, A);
        if (hl || ha) {
            double a2 = ha ? A * kPi / 180.0 : ang;
            double l2 = hl ? std::abs(L) : len;
            p = a + Vec2(std::cos(a2), std::sin(a2)) * l2;
            over = true;
        }
    } else if (t == "slot_center" && sk_.clicks.size() >= 2) {
        Vec2 b = sk_.clicks[1].pos, d = b - a;
        double len = glm::length(d);
        if (len > 1e-9) {
            Vec2 u = d / len, n = perp(u);
            double side = glm::dot(p - a, n);
            h.live[0] = std::abs(side) * 2.0;
            double W;
            if (num(0, W) && std::abs(W) > 1e-9) {
                double s = side >= 0 ? 1.0 : -1.0;
                p = b + n * s * std::abs(W) * 0.5;
                over = true;
            }
        }
    } else if (t == "slot_arc" && sk_.clicks.size() >= 3) {
        Vec2 o; double R;
        if (circumcenter(sk_.clicks[0].pos, sk_.clicks[1].pos, sk_.clicks[2].pos, o, R)) {
            double dist = glm::length(p - o);
            h.live[0] = std::abs(dist - R) * 2.0;
            double W;
            if (num(0, W) && std::abs(W) > 1e-9 && std::abs(W) * 0.5 < R - 1e-6) {
                double side = dist >= R ? 1.0 : -1.0;
                Vec2 u = dist > 1e-9 ? (p - o) / dist : Vec2(1, 0);
                p = o + u * (R + side * std::abs(W) * 0.5);
                over = true;
            }
        }
    } else if (t == "rectangle" || t == "rect_center") {
        double k = t == "rect_center" ? 2.0 : 1.0;
        Vec2 d = p - a;
        h.live[0] = std::abs(d.x) * k;
        h.live[1] = std::abs(d.y) * k;
        double W, H;
        bool hw = num(0, W), hh = num(1, H);
        if (hw || hh) {
            double sx = d.x >= 0 ? 1 : -1, sy = d.y >= 0 ? 1 : -1;
            if (hw) d.x = sx * std::abs(W) / k;
            if (hh) d.y = sy * std::abs(H) / k;
            p = a + d;
            over = true;
        }
    } else {
        double k = t == "circle_2pt" ? 1.0 : 2.0;   // cursor distance -> diameter
        Vec2 d = p - a;
        double r = glm::length(d);
        h.live[0] = r * k;
        double D;
        if (num(0, D) && std::abs(D) > 1e-9) {
            Vec2 u = r > 1e-12 ? d / r : Vec2(1, 0);
            p = a + u * (std::abs(D) / k);
            over = true;
        }
    }
    if (over) {
        sk_.snap.pos = p;
        sk_.snap.kind = SnapKind::None;
        sk_.snap.point = 0;
        sk_.snap.curve = 0;
        sk_.snapLabel.clear();
    }
}

void App::sketchCommitSpline(bool closed, bool control) {
    Sketch* s = curSketch();
    if (!s || sk_.clicks.size() < (closed ? 3u : 2u)) return;
    std::vector<SnapInfo> pts = sk_.clicks;
    bool cons = sk_.construction;
    Sketch backup = *s;
    pushUndo(control ? "Control Point Spline" : "Spline");
    std::vector<uint32_t> ids;
    for (auto& c : pts) ids.push_back(sketchMakePoint(*s, c));
    if (control) s->addCtrlSpline(ids, closed, cons);
    else s->addSpline(ids, closed, cons);
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("Could not add that spline", ToastKind::Warning);
    } else {
        sketchFinishOp();
    }
    sk_.clicks.clear();
    sk_.step = 0;
}

void App::sketchClickImpl() {
    Sketch* s = curSketch();
    if (!s || !sk_.cursorValid) return;
    const std::string t = sk_.tool;
    SnapInfo si = sk_.snap;
    if (t == "point") { sketchPointAt(si); return; }
    if (t == "cross_point") { sketchCrossingClick(); return; }
    if (t == "text") { sketchTextAt(si); return; }
    if (t == "sk_fillet") { if (sk_.hoverEnt) sketchFilletAt(sk_.hoverEnt, sk_.raw); return; }
    if (t == "sk_chamfer") { if (sk_.hoverEnt) sketchChamferAt(sk_.hoverEnt, sk_.raw); return; }
    if (t == "extend") { if (sk_.hoverEnt) sketchExtendAt(sk_.hoverEnt, sk_.raw); return; }
    if (t == "project") { sketchProjectClick(); return; }
    if (t == "sk_intersect") { sketchIntersectClick(); return; }
    if (t == "sk_onto_face") { sketchProjectFaceClick(); return; }
    if (t == "offset") {
        if (sk_.picks.empty()) {
            if (sk_.hoverEnt) sketchOffsetPick(sk_.hoverEnt);
            return;
        }
        double d = sketchOffsetDistance(sk_.raw);
        if (!sk_.hud.buf[0].empty()) {
            try {
                double v = std::abs(peval::eval(sk_.hud.buf[0], doc_.params));
                d = d < 0 ? -v : v;
            } catch (const std::runtime_error&) {}
        }
        sketchOffsetCommit(d);
        return;
    }
    auto& cl = sk_.clicks;
    const bool cons = sk_.construction;
    ImGuiIO& io = ImGui::GetIO();

    auto op = [&](const char* label, const std::function<void()>& body) -> bool {
        Sketch backup = *s;
        pushUndo(label);
        body();
        if (!sketchSolve({}, false)) {
            *s = backup;
            undo_.dropLast();
            toast("Could not add that: it conflicts with existing constraints", ToastKind::Warning);
            return false;
        }
        sketchFinishOp();
        return true;
    };

    if (t == "spline" || t == "control_spline") {
        bool control = t == "control_spline";
        bool close = cl.size() >= 3 && glm::length(si.pos - cl[0].pos) < 1e-4;
        if (close) {
            sketchCommitSpline(true, control);
            return;
        }
        cl.push_back(si);
        sk_.step = 1;
        if (io.MouseDoubleClicked[0] && cl.size() >= 2) sketchCommitSpline(false, control);
        return;
    }

    if (t == "line") {
        if (cl.empty()) { cl.push_back(si); sk_.step = 1; return; }
        SnapInfo a = cl[0];
        if (glm::length(si.pos - a.pos) < 1e-6) return;
        bool closing = false;
        op("Line", [&] {
            uint32_t pa = sketchMakePoint(*s, a);
            uint32_t pb = sketchMakePoint(*s, si);
            if (pa == pb) return;
            uint32_t ln = s->addLine(pa, pb, cons);
            if (si.kind == SnapKind::Horizontal) s->addConstraint(CType::Horizontal, ln);
            else if (si.kind == SnapKind::Vertical) s->addConstraint(CType::Vertical, ln);
            if (!sk_.chainStart) sk_.chainStart = pa;
            closing = pb == sk_.chainStart;
            SnapInfo n;
            n.kind = SnapKind::Point; n.point = pb; n.pos = s->pt(pb);
            cl[0] = n;
        });
        if (closing || io.MouseDoubleClicked[0]) { cl.clear(); sk_.step = 0; sk_.chainStart = 0; }
        return;
    }

    if (t == "rectangle" || t == "rect_center") {
        if (cl.empty()) { cl.push_back(si); sk_.step = 1; return; }
        SnapInfo a = cl[0];
        Vec2 d = si.pos - a.pos;
        if (std::abs(d.x) < 1e-6 || std::abs(d.y) < 1e-6) return;
        op("Rectangle", [&] {
            if (t == "rectangle") {
                uint32_t p0 = sketchMakePoint(*s, a), p2 = sketchMakePoint(*s, si);
                uint32_t p1 = s->addPoint({si.pos.x, a.pos.y}), p3 = s->addPoint({a.pos.x, si.pos.y});
                uint32_t l0 = s->addLine(p0, p1, cons), l1 = s->addLine(p1, p2, cons), l2 = s->addLine(p2, p3, cons), l3 = s->addLine(p3, p0, cons);
                s->addConstraint(CType::Horizontal, l0); s->addConstraint(CType::Horizontal, l2);
                s->addConstraint(CType::Vertical, l1); s->addConstraint(CType::Vertical, l3);
            } else {
                uint32_t c = sketchMakePoint(*s, a);
                Vec2 cc = s->pt(c), h(std::abs(d.x), std::abs(d.y));
                uint32_t p0 = s->addPoint(cc + Vec2(-h.x, -h.y)), p1 = s->addPoint(cc + Vec2(h.x, -h.y));
                uint32_t p2 = s->addPoint(cc + Vec2(h.x, h.y)), p3 = s->addPoint(cc + Vec2(-h.x, h.y));
                uint32_t l0 = s->addLine(p0, p1, cons), l1 = s->addLine(p1, p2, cons), l2 = s->addLine(p2, p3, cons), l3 = s->addLine(p3, p0, cons);
                s->addConstraint(CType::Horizontal, l0); s->addConstraint(CType::Horizontal, l2);
                s->addConstraint(CType::Vertical, l1); s->addConstraint(CType::Vertical, l3);
                uint32_t diag = s->addLine(p0, p2, true);
                s->addConstraint(CType::Midpoint, c, diag);
            }
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "rect_3pt") {
        if ((int)cl.size() < 2) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 corner[4];
        if (!rect3Corners(cl[0].pos, cl[1].pos, si.pos, corner)) return;
        op("3-Point Rectangle", [&] {
            uint32_t p0 = sketchMakePoint(*s, cl[0]);
            uint32_t p1 = sketchMakePoint(*s, cl[1]);
            uint32_t p2 = s->addPoint(corner[2]);
            uint32_t p3 = s->addPoint(corner[3]);
            uint32_t l0 = s->addLine(p0, p1, cons), l1 = s->addLine(p1, p2, cons);
            uint32_t l2 = s->addLine(p2, p3, cons), l3 = s->addLine(p3, p0, cons);
            s->addConstraint(CType::Perpendicular, l0, l1);
            s->addConstraint(CType::Perpendicular, l0, l3);
            s->addConstraint(CType::Parallel, l0, l2);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "circle") {
        if (cl.empty()) { cl.push_back(si); sk_.step = 1; return; }
        double r = glm::length(si.pos - cl[0].pos);
        if (r < 1e-6) return;
        op("Circle", [&] {
            uint32_t c = sketchMakePoint(*s, cl[0]);
            s->addCircle(c, r, cons);
        });
        cl.clear(); sk_.step = 0;
        return;
    }
    if (t == "ellipse" || t == "ellipse_arc") {
        int need = t == "ellipse" ? 3 : 5;
        if ((int)cl.size() + 1 < need) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 c = cl[0].pos, maj = cl[1].pos - c;
        double major = glm::length(maj);
        if (major < 1e-6) return;
        double rot = std::atan2(maj.y, maj.x);
        Vec2 u = maj / major, n(-u.y, u.x);
        Vec2 minorPt = t == "ellipse" ? si.pos : cl[2].pos;
        double minor = std::abs(glm::dot(minorPt - c, n));
        if (minor < 1e-6) minor = glm::length(minorPt - c);
        if (minor < 1e-6) return;
        if (t == "ellipse") {
            op("Ellipse", [&] {
                uint32_t cp = sketchMakePoint(*s, cl[0]);
                s->addEllipse(cp, major, minor, rot, cons);
            });
        } else {
            op("Elliptical arc", [&] {
                uint32_t cp = sketchMakePoint(*s, cl[0]);
                uint32_t a = sketchMakePoint(*s, cl[3]);
                uint32_t b = sketchMakePoint(*s, si);
                s->addEllipArc(cp, major, minor, rot, a, b, cons);
            });
        }
        cl.clear(); sk_.step = 0;
        return;
    }
    if (t == "circle_2pt") {
        if (cl.empty()) { cl.push_back(si); sk_.step = 1; return; }
        double r = glm::length(si.pos - cl[0].pos) * 0.5;
        if (r < 1e-6) return;
        op("Circle", [&] {
            uint32_t c = s->addPoint((si.pos + cl[0].pos) * 0.5);
            s->addCircle(c, r, cons);
        });
        cl.clear(); sk_.step = 0;
        return;
    }
    if (t == "circle_3pt") {
        if (cl.size() < 2) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 o; double r;
        if (!circumcenter(cl[0].pos, cl[1].pos, si.pos, o, r)) { toast("Those three points are in a line", ToastKind::Warning); return; }
        op("Circle", [&] {
            uint32_t c = s->addPoint(o);
            s->addCircle(c, r, cons);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "arc") {
        if (cl.size() < 2) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 o; double r;
        if (!circumcenter(cl[0].pos, cl[1].pos, si.pos, o, r)) { toast("Those three points are in a line", ToastKind::Warning); return; }
        Vec2 A = cl[0].pos, B = cl[1].pos, C = si.pos;
        double aA = std::atan2(A.y - o.y, A.x - o.x);
        double relC = wrap2pi(std::atan2(C.y - o.y, C.x - o.x) - aA);
        double relB = wrap2pi(std::atan2(B.y - o.y, B.x - o.x) - aA);
        bool ccw = relC < relB;
        op("Arc", [&] {
            uint32_t pa = sketchMakePoint(*s, cl[0]), pb = sketchMakePoint(*s, cl[1]);
            uint32_t c = s->addPoint(o);
            if (ccw) s->addArc(c, pa, pb, cons);
            else s->addArc(c, pb, pa, cons);
        });
        cl.clear(); sk_.step = 0;
        return;
    }
    if (t == "arc_center") {
        if (cl.size() < 2) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 o = cl[0].pos, S = cl[1].pos;
        double r = glm::length(S - o);
        Vec2 dir = si.pos - o;
        if (r < 1e-6 || glm::length(dir) < 1e-6) return;
        Vec2 E = o + dir / glm::length(dir) * r;
        op("Arc", [&] {
            uint32_t c = sketchMakePoint(*s, cl[0]);
            uint32_t ps = sketchMakePoint(*s, cl[1]);
            uint32_t pe = s->addPoint(E);
            s->addArc(c, ps, pe, cons);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "polygon" || t == "polygon_in") {
        if (cl.empty()) { cl.push_back(si); sk_.step = 1; return; }
        double cr = 0;
        bool circum = t == "polygon" && sk_.polyCircumscribed;
        auto verts = polygonVerts(cl[0].pos, si.pos, sk_.polySides, circum, cr);
        if (verts.empty()) return;
        op(circum ? "Polygon" : "Inscribed Polygon", [&] {
            uint32_t c = sketchMakePoint(*s, cl[0]);
            std::vector<uint32_t> pts, lines;
            for (auto& v : verts) pts.push_back(s->addPoint(v));
            for (size_t i = 0; i < pts.size(); ++i) lines.push_back(s->addLine(pts[i], pts[(i + 1) % pts.size()], cons));
            uint32_t circle = s->addCircle(c, cr, true);
            for (size_t i = 0; i + 1 < lines.size(); ++i) s->addConstraint(CType::Equal, lines[i], lines[i + 1]);
            if (circum) for (uint32_t l : lines) s->addConstraint(CType::Tangent, l, circle);
            else for (uint32_t p : pts) s->addConstraint(CType::PointOnObject, p, circle);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "slot") {
        if (cl.size() < 2) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 c1 = cl[0].pos, c2 = cl[1].pos, u = c2 - c1;
        double L = glm::length(u);
        if (L < 1e-6) { cl.clear(); sk_.step = 0; return; }
        u /= L;
        Vec2 n = perp(u);
        double hw = std::abs(glm::dot(si.pos - c1, n));
        if (hw < 1e-6) return;
        op("Slot", [&] {
            uint32_t pc1 = sketchMakePoint(*s, cl[0]), pc2 = sketchMakePoint(*s, cl[1]);
            uint32_t p1 = s->addPoint(c1 - n * hw), p2 = s->addPoint(c2 - n * hw), p3 = s->addPoint(c2 + n * hw), p4 = s->addPoint(c1 + n * hw);
            uint32_t l1 = s->addLine(p1, p2, cons);
            uint32_t a2 = s->addArc(pc2, p2, p3, cons);
            uint32_t l2 = s->addLine(p3, p4, cons);
            uint32_t a1 = s->addArc(pc1, p4, p1, cons);
            s->addConstraint(CType::Tangent, l1, a1); s->addConstraint(CType::Tangent, l1, a2);
            s->addConstraint(CType::Tangent, l2, a1); s->addConstraint(CType::Tangent, l2, a2);
            s->addConstraint(CType::Equal, a1, a2);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "slot_overall") {
        if (cl.size() < 2) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 a = cl[0].pos, b = cl[1].pos, d = b - a;
        double L = glm::length(d);
        if (L < 1e-6) { cl.clear(); sk_.step = 0; return; }
        Vec2 u = d / L, n = perp(u);
        double hw = std::abs(glm::dot(si.pos - a, n));
        if (hw < 1e-6 || hw * 2.0 >= L - 1e-6) return;
        Vec2 c1 = a + u * hw, c2 = b - u * hw;
        op("Overall Slot", [&] {
            uint32_t pc1 = s->addPoint(c1), pc2 = s->addPoint(c2);
            uint32_t p1 = s->addPoint(c1 - n * hw), p2 = s->addPoint(c2 - n * hw);
            uint32_t p3 = s->addPoint(c2 + n * hw), p4 = s->addPoint(c1 + n * hw);
            uint32_t l1 = s->addLine(p1, p2, cons);
            uint32_t a2 = s->addArc(pc2, p2, p3, cons);
            uint32_t l2 = s->addLine(p3, p4, cons);
            uint32_t a1 = s->addArc(pc1, p4, p1, cons);
            s->addConstraint(CType::Tangent, l1, a1); s->addConstraint(CType::Tangent, l1, a2);
            s->addConstraint(CType::Tangent, l2, a1); s->addConstraint(CType::Tangent, l2, a2);
            s->addConstraint(CType::Equal, a1, a2);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "slot_center") {
        if (cl.size() < 2) { cl.push_back(si); sk_.step = (int)cl.size(); return; }
        Vec2 c = cl[0].pos, e = cl[1].pos, d = e - c;
        double half = glm::length(d);
        if (half < 1e-6) return;
        Vec2 u = d / half, n = perp(u);
        double hw = std::abs(glm::dot(si.pos - c, n));
        if (hw < 1e-6) return;
        Vec2 c2 = e, c1 = c - d;
        op("Center Point Slot", [&] {
            uint32_t mid = sketchMakePoint(*s, cl[0]);
            uint32_t pc2 = sketchMakePoint(*s, cl[1]);
            uint32_t pc1 = s->addPoint(c1);
            uint32_t axis = s->addLine(pc1, pc2, true);
            s->addConstraint(CType::Midpoint, mid, axis);
            uint32_t p1 = s->addPoint(c1 - n * hw), p2 = s->addPoint(c2 - n * hw);
            uint32_t p3 = s->addPoint(c2 + n * hw), p4 = s->addPoint(c1 + n * hw);
            uint32_t l1 = s->addLine(p1, p2, cons);
            uint32_t a2 = s->addArc(pc2, p2, p3, cons);
            uint32_t l2 = s->addLine(p3, p4, cons);
            uint32_t a1 = s->addArc(pc1, p4, p1, cons);
            s->addConstraint(CType::Tangent, l1, a1); s->addConstraint(CType::Tangent, l1, a2);
            s->addConstraint(CType::Tangent, l2, a1); s->addConstraint(CType::Tangent, l2, a2);
            s->addConstraint(CType::Equal, a1, a2);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "slot_arc") {
        if (cl.size() < 3) {
            if (cl.size() == 2) {
                Vec2 o; double r;
                if (!circumcenter(cl[0].pos, cl[1].pos, si.pos, o, r)) {
                    toast("Those three points are in a line", ToastKind::Warning);
                    return;
                }
            }
            cl.push_back(si);
            sk_.step = (int)cl.size();
            return;
        }
        Vec2 o; double R;
        if (!circumcenter(cl[0].pos, cl[1].pos, cl[2].pos, o, R)) return;
        double hw = std::abs(glm::length(si.pos - o) - R);
        ArcSlot g = makeArcSlot(cl[0].pos, cl[1].pos, cl[2].pos, hw);
        if (!g.ok) return;
        auto rad = [&](Vec2 q) { return (q - g.o) / g.R; };
        Vec2 os = g.o + rad(g.sa) * (g.R + g.hw), oe = g.o + rad(g.sb) * (g.R + g.hw);
        Vec2 is = g.o + rad(g.sa) * (g.R - g.hw), ie = g.o + rad(g.sb) * (g.R - g.hw);
        op("3-Point Arc Slot", [&] {
            uint32_t cpt = s->addPoint(g.o);
            uint32_t pa = s->addPoint(g.sa), pb = s->addPoint(g.sb);
            uint32_t pm = sketchMakePoint(*s, cl[1]);
            uint32_t outer = s->addArc(cpt, s->addPoint(os), s->addPoint(oe), cons);
            uint32_t inner = s->addArc(cpt, s->addPoint(is), s->addPoint(ie), cons);
            uint32_t capB = s->addArc(pb, s->find(outer)->b, s->find(inner)->b, cons);
            uint32_t capA = s->addArc(pa, s->find(inner)->a, s->find(outer)->a, cons);
            s->addConstraint(CType::Tangent, outer, capA, 0, -1.0);
            s->addConstraint(CType::Tangent, outer, capB, 0, -1.0);
            s->addConstraint(CType::Tangent, inner, capA, 0, 1.0);
            s->addConstraint(CType::Tangent, inner, capB, 0, 1.0);
            s->addConstraint(CType::Equal, capA, capB);
            uint32_t spine = s->addArc(cpt, pa, pb, true);
            s->addConstraint(CType::PointOnObject, pm, spine);
        });
        cl.clear(); sk_.step = 0;
        return;
    }

    if (t == "dimension") { dimensionTool(); return; }
    if (isConstraintTool(t)) {
        uint32_t id = sk_.hoverEnt;
        if (!id) return;
        if (std::find(sk_.picks.begin(), sk_.picks.end(), id) == sk_.picks.end()) sk_.picks.push_back(id);
        applyConstraintTool();
        return;
    }
    if (t == "trim") {
        if (sk_.hoverEnt) trimAt(sk_.hoverEnt, sk_.raw);
        return;
    }
    if (t == "break") {
        if (sk_.hoverEnt) breakAt(sk_.hoverEnt);
        return;
    }
    if (t == "sk_xform" || t == "sk_pattern") return;
    if (t == "mirror") {
        const Entity* e = sk_.hoverEnt ? s->find(sk_.hoverEnt) : nullptr;
        if (e && e->type == EntType::Line && !sk_.selEnt.count(e->id)) mirrorSelection(e->id);
        return;
    }
}

// ===================================================================== dimensions

bool App::dimensionFor(const Sketch& s, const std::vector<uint32_t>& picks, Vec2 cur, sk::Constraint& out) const {
    out = sk::Constraint{};
    auto E = [&](uint32_t id) { return s.find(id); };
    auto pointPair = [&](uint32_t pa, uint32_t pb) {
        Vec2 a = s.pt(pa), b = s.pt(pb);
        Vec2 lo = glm::min(a, b), hi = glm::max(a, b);
        bool inX = cur.x >= lo.x && cur.x <= hi.x, inY = cur.y >= lo.y && cur.y <= hi.y;
        out.e[0] = pa; out.e[1] = pb;
        if (inX && !inY && hi.x - lo.x > 1e-9) { out.type = CType::HDistance; out.value = b.x - a.x; }
        else if (inY && !inX && hi.y - lo.y > 1e-9) { out.type = CType::VDistance; out.value = b.y - a.y; }
        else { out.type = CType::Distance; out.value = glm::length(b - a); }
        return true;
    };
    auto pointLine = [&](uint32_t pt, uint32_t ln) {
        const Entity* l = E(ln);
        if (l->a == pt || l->b == pt) return false;
        Vec2 a = s.pt(l->a), b = s.pt(l->b);
        Vec2 u = b - a;
        double len = glm::length(u);
        if (len < 1e-12) return false;
        double sd = cross2(u / len, s.pt(pt) - a);
        out.type = CType::PointLineDistance;
        out.e[0] = pt; out.e[1] = ln;
        out.value = std::abs(sd);
        out.value2 = sd < 0 ? -1.0 : 1.0;
        return true;
    };
    if (picks.size() == 1) {
        const Entity* e = E(picks[0]);
        if (!e) return false;
        if (e->type == EntType::Line) return pointPair(e->a, e->b);
        if (e->type == EntType::Circle) { out.type = CType::Diameter; out.e[0] = e->id; out.value = e->r * 2.0; return true; }
        if (e->type == EntType::Arc) { out.type = CType::Radius; out.e[0] = e->id; out.value = glm::length(s.pt(e->a) - s.pt(e->c)); return true; }
        return false;
    }
    if (picks.size() >= 2) {
        const Entity *a = E(picks[0]), *b = E(picks[1]);
        if (!a || !b || a->id == b->id) return false;
        auto isCirc = [](const Entity* e) { return e->type == EntType::Circle || e->type == EntType::Arc; };
        if (a->type == EntType::Point && b->type == EntType::Point) return pointPair(a->id, b->id);
        if (a->type == EntType::Point && b->type == EntType::Line) return pointLine(a->id, b->id);
        if (a->type == EntType::Line && b->type == EntType::Point) return pointLine(b->id, a->id);
        if (a->type == EntType::Line && b->type == EntType::Line) {
            Vec2 d0 = s.pt(a->b) - s.pt(a->a), d1 = s.pt(b->b) - s.pt(b->a);
            double l0 = glm::length(d0), l1 = glm::length(d1);
            if (l0 < 1e-12 || l1 < 1e-12) return false;
            if (std::abs(cross2(d0 / l0, d1 / l1)) < 1e-9) return pointLine(a->a, b->id);
            out.type = CType::Angle;
            out.e[0] = a->id; out.e[1] = b->id;
            out.value = std::atan2(cross2(d0, d1), glm::dot(d0, d1));
            return true;
        }
        if (isCirc(a) && b->type == EntType::Point) return pointPair(a->c, b->id);
        if (a->type == EntType::Point && isCirc(b)) return pointPair(a->id, b->c);
        if (isCirc(a) && isCirc(b)) return pointPair(a->c, b->c);
    }
    return false;
}

void App::dimensionTool() {
    Sketch* s = curSketch();
    if (!s) return;
    if (sk_.dimPlacing) {
        sk::Constraint c = sk_.dim;
        pushUndo("Dimension");
        uint32_t id = s->addConstraint(c.type, c.e[0], c.e[1], c.e[2], c.value);
        sk::Constraint& nc = s->cons[id];
        nc.value2 = c.value2;
        nc.label = sk_.raw;
        nc.labelSet = true;
        sk_.picks.clear();
        sk_.dimPlacing = false;
        sketchFinishOp();
        double v = nc.type == CType::Angle ? std::abs(nc.value) * 180.0 / kPi : std::abs(nc.value);
        std::snprintf(sk_.dimBuf, sizeof sk_.dimBuf, "%s", fmtNum(v).c_str());
        sk_.dimEdit = id;
        sk_.dimFocus = true;
        return;
    }
    uint32_t id = sk_.hoverEnt;
    if (!id) return;
    if (std::find(sk_.picks.begin(), sk_.picks.end(), id) != sk_.picks.end()) return;
    sk_.picks.push_back(id);
    sk::Constraint probe;
    if (dimensionFor(*s, sk_.picks, sk_.raw, probe)) {
        sk_.dim = probe;
        sk_.dimPlacing = true;
    } else if (sk_.picks.size() >= 2) {
        toast("That combination cannot be dimensioned", ToastKind::Warning);
        sk_.picks.clear();
    }
}

void App::drawDimensionEditor() {
    if (!sk_.active || !sk_.dimEdit) return;
    Sketch* s = curSketch();
    if (!s) { sk_.dimEdit = 0; return; }
    auto it = s->cons.find(sk_.dimEdit);
    if (it == s->cons.end()) { sk_.dimEdit = 0; return; }
    sk::Constraint& c = it->second;
    PlaneView pv{sk_, s->plane};
    ImVec2 at(sk_.W * 0.5f, sk_.H * 0.5f);
    auto rit = sk_.dimRects.find(c.id);
    if (rit != sk_.dimRects.end()) at = ImVec2(rit->second.x, rit->second.w + 6.f);
    const float S = uiScale_;
    ImGui::SetNextWindowPos(at);
    ImGui::SetNextWindowSize(ImVec2(150 * S, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 6 * S));
    ImGui::Begin("##dimedit", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav);
    const char* unit = c.type == CType::Angle ? "deg" : "mm";
    ImGui::TextDisabled("%s  (%s)", c.name.c_str(), unit);
    if (c.driven) ImGui::TextDisabled("Reports the size. It does not drive it.");
    if (sk_.dimFocus) { ImGui::SetKeyboardFocusHere(); sk_.dimFocus = false; }
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##dimval", sk_.dimBuf, sizeof sk_.dimBuf,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::End();
    ImGui::PopStyleVar();
    if (esc) { sk_.dimEdit = 0; return; }
    if (!enter) return;
    if (c.driven) {
        toast("That dimension is driven. Turn Driven off to change the size.", ToastKind::Warning);
        sk_.dimEdit = 0;
        return;
    }
    // Numbers, or an expression referencing user parameters (e.g. "width / 2").
    double v = 0;
    std::string expr;
    std::string perr;
    try {
        v = peval::eval(sk_.dimBuf, doc_.params);
        if (!peval::plainNumber(sk_.dimBuf)) expr = sk_.dimBuf;
    } catch (const std::runtime_error& e) {
        v = std::atof(sk_.dimBuf);
        if (v == 0.0) {
            toast(std::string(e.what()) + "  (define parameters with Change Parameters)", ToastKind::Warning);
            return;
        }
    }
    uint32_t id = c.id;
    sk_.dimEdit = 0;
    if (!(v > 0.0) && c.type != CType::HDistance && c.type != CType::VDistance) { toast("Enter a value greater than zero", ToastKind::Warning); return; }
    if (v == 0.0) { toast("Enter a value other than zero", ToastKind::Warning); return; }
    Sketch backup = *s;
    pushUndo("Edit dimension");
    sk::Constraint& cc = s->cons[id];
    if (cc.type == CType::Angle) cc.value = (cc.value < 0 ? -1.0 : 1.0) * std::abs(v) * kPi / 180.0;
    else if (cc.type == CType::HDistance || cc.type == CType::VDistance) cc.value = (cc.value < 0 ? -1.0 : 1.0) * std::abs(v);
    else cc.value = std::abs(v);
    cc.expr = expr;
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("That value conflicts with the other constraints", ToastKind::Warning);
        return;
    }
    sketchFinishOp();
}

void App::sketchSetDriven(uint32_t constraintId, bool driven) {
    Sketch* s = curSketch();
    if (!s) return;
    auto it = s->cons.find(constraintId);
    if (it == s->cons.end() || !sk::isDimension(it->second.type) || it->second.driven == driven) return;
    Sketch backup = *s;
    pushUndo(driven ? "Driven dimension" : "Driving dimension");
    sk::Constraint& c = s->cons[constraintId];
    c.driven = driven;
    c.expr.clear();
    double m = s->measureDimension(c);
    if (std::isfinite(m)) c.value = m;
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("That conflicts with the other constraints", ToastKind::Warning);
        return;
    }
    sketchFinishOp();
}

// ===================================================================== constraints

void App::applyConstraintTool() {
    Sketch* s = curSketch();
    if (!s) return;
    const std::string t = sk_.tool;
    auto& P = sk_.picks;
    struct Pending { CType t; uint32_t e0, e1, e2; double v, v2; };
    std::vector<Pending> out;
    auto ent = [&](size_t i) { return s->find(P[i]); };
    auto isPt = [](const Entity* e) { return e && e->type == EntType::Point; };
    auto isLine = [](const Entity* e) { return e && e->type == EntType::Line; };
    auto isCirc = [](const Entity* e) { return e && (e->type == EntType::Circle || e->type == EntType::Arc); };
    bool invalid = false;
    const char* why = "Those objects cannot take that constraint";

    if (t == "c_coincident") {
        if (P.size() < 2) return;
        const Entity *a = ent(0), *b = ent(1);
        if (isPt(a) && isPt(b)) out.push_back({CType::Coincident, a->id, b->id, 0, 0, 0});
        else if (isPt(a) && b && !isPt(b)) out.push_back({CType::PointOnObject, a->id, b->id, 0, 0, 0});
        else if (isPt(b) && a && !isPt(a)) out.push_back({CType::PointOnObject, b->id, a->id, 0, 0, 0});
        else invalid = true;
    } else if (t == "c_midpoint") {
        if (P.size() < 2) return;
        const Entity *a = ent(0), *b = ent(1);
        if (isPt(a) && isLine(b)) out.push_back({CType::Midpoint, a->id, b->id, 0, 0, 0});
        else if (isPt(b) && isLine(a)) out.push_back({CType::Midpoint, b->id, a->id, 0, 0, 0});
        else invalid = true;
    } else if (t == "c_concentric") {
        if (P.size() < 2) return;
        const Entity *a = ent(0), *b = ent(1);
        if (isCirc(a) && isCirc(b)) out.push_back({CType::Coincident, a->c, b->c, 0, 0, 0});
        else invalid = true;
    } else if (t == "c_collinear" || t == "c_parallel" || t == "c_perpendicular") {
        if (P.size() < 2) return;
        const Entity *a = ent(0), *b = ent(1);
        CType ct = t == "c_collinear" ? CType::Collinear : t == "c_parallel" ? CType::Parallel : CType::Perpendicular;
        if (isLine(a) && isLine(b)) out.push_back({ct, a->id, b->id, 0, 0, 0});
        else invalid = true;
    } else if (t == "c_hv") {
        const Entity* a = ent(0);
        if (isLine(a)) {
            Vec2 d = s->pt(a->b) - s->pt(a->a);
            out.push_back({std::abs(d.x) >= std::abs(d.y) ? CType::Horizontal : CType::Vertical, a->id, 0, 0, 0, 0});
        } else if (isPt(a)) {
            if (P.size() < 2) return;
            const Entity* b = ent(1);
            if (!isPt(b)) invalid = true;
            else {
                Vec2 d = Vec2(b->x, b->y) - Vec2(a->x, a->y);
                out.push_back({std::abs(d.x) >= std::abs(d.y) ? CType::Horizontal : CType::Vertical, a->id, b->id, 0, 0, 0});
            }
        } else invalid = true;
    } else if (t == "c_tangent") {
        if (P.size() < 2) return;
        const Entity *a = ent(0), *b = ent(1);
        if ((isLine(a) && isCirc(b)) || (isCirc(a) && isLine(b))) out.push_back({CType::Tangent, a->id, b->id, 0, 0, 0});
        else if (isCirc(a) && isCirc(b)) {
            Sketch::ArcGeom ga, gb;
            s->arcGeom(*a, ga); s->arcGeom(*b, gb);
            double d = glm::length(ga.c - gb.c);
            bool external = std::abs(d - (ga.r + gb.r)) <= std::abs(d - std::abs(ga.r - gb.r));
            out.push_back({CType::Tangent, a->id, b->id, 0, external ? 1.0 : -1.0, 0});
        } else invalid = true;
    } else if (t == "c_equal") {
        if (P.size() < 2) return;
        const Entity *a = ent(0), *b = ent(1);
        if ((isLine(a) && isLine(b)) || (isCirc(a) && isCirc(b))) out.push_back({CType::Equal, a->id, b->id, 0, 0, 0});
        else invalid = true;
    } else if (t == "c_symmetric") {
        if (P.size() < 3) return;
        const Entity *a = ent(0), *b = ent(1), *l = ent(2);
        if (isPt(a) && isPt(b) && isLine(l)) out.push_back({CType::Symmetric, a->id, b->id, l->id, 0, 0});
        else invalid = true;
    } else if (t == "c_fix") {
        const Entity* a = ent(0);
        auto fixPoint = [&](uint32_t pid) {
            const Entity* p = s->find(pid);
            if (!p || p->locked) return;
            out.push_back({CType::Fix, pid, 0, 0, p->x, p->y});
        };
        if (isPt(a)) fixPoint(a->id);
        else if (isLine(a)) { fixPoint(a->a); fixPoint(a->b); }
        else if (isCirc(a)) fixPoint(a->c);
        else invalid = true;
    }
    if (invalid) { toast(why, ToastKind::Warning); P.clear(); return; }
    P.clear();
    if (out.empty()) return;

    // Fix toggles: an existing Fix on the point is removed instead.
    bool allDuplicates = true;
    for (auto& p : out) if (!s->hasConstraint(p.t, p.e0, p.e1, p.e2)) allDuplicates = false;
    if (allDuplicates) {
        if (t == "c_fix") {
            pushUndo("Unfix");
            for (auto& p : out)
                for (auto it = s->cons.begin(); it != s->cons.end();) {
                    if (it->second.type == CType::Fix && it->second.e[0] == p.e0) it = s->cons.erase(it); else ++it;
                }
            sketchSolve({}, false);
            sketchFinishOp();
        } else toast("That constraint already exists");
        return;
    }
    Sketch backup = *s;
    pushUndo("Constraint");
    for (auto& p : out) {
        if (s->hasConstraint(p.t, p.e0, p.e1, p.e2)) continue;
        uint32_t id = s->addConstraint(p.t, p.e0, p.e1, p.e2, p.v);
        s->cons[id].value2 = p.v2;
    }
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("That constraint conflicts with the existing ones", ToastKind::Warning);
        return;
    }
    sketchFinishOp();
}

// ===================================================================== trim and mirror

void App::trimAt(uint32_t ent, Vec2 click) {
    Sketch* s = curSketch();
    if (!s) return;
    const Entity* ep = s->find(ent);
    if (!ep || ep->locked || ep->type == EntType::Point) return;
    const Entity E = *ep;
    std::vector<double> splits = sk::curveSplitParams(*s, ent);
    double tc = sk::curveParamAt(*s, ent, click);
    const bool closed = E.type == EntType::Circle;
    Sketch::ArcGeom g;
    Vec2 la(0), lb(0);
    if (E.type == EntType::Line) { la = s->pt(E.a); lb = s->pt(E.b); }
    else s->arcGeom(E, g);
    auto at = [&](double t) -> Vec2 { return E.type == EntType::Line ? la + (lb - la) * t : s->arcPoint(g, t); };

    double t0 = 0, t1 = 1;
    bool whole = false;
    if (!closed) {
        double lo = -1, hi = 2;
        for (double t : splits) {
            if (t <= tc - 1e-9) lo = std::max(lo, t);
            if (t >= tc + 1e-9) hi = std::min(hi, t);
        }
        t0 = lo < 0 ? 0.0 : lo;
        t1 = hi > 1 ? 1.0 : hi;
        whole = t0 <= 1e-9 && t1 >= 1.0 - 1e-9;
    } else if (splits.size() < 2) {
        whole = true;
    } else {
        double lo = -1, hi = 2;
        for (double t : splits) { if (t <= tc) lo = std::max(lo, t); else hi = std::min(hi, t); }
        if (lo < 0) lo = splits.back() - 1.0;
        if (hi > 1.5) hi = splits.front() + 1.0;
        t0 = lo; t1 = hi;
    }

    Sketch backup = *s;
    pushUndo("Trim");
    std::vector<sk::Constraint> oldCons;
    for (auto& [id, c] : s->cons)
        if (c.e[0] == ent || c.e[1] == ent || c.e[2] == ent) oldCons.push_back(c);
    std::vector<uint32_t> pieces;
    if (!whole) {
        if (E.type == EntType::Line) {
            if (t0 > 1e-9) pieces.push_back(s->addLine(E.a, attachPoint(*s, at(t0), ent), E.construction));
            if (t1 < 1.0 - 1e-9) pieces.push_back(s->addLine(attachPoint(*s, at(t1), ent), E.b, E.construction));
        } else if (E.type == EntType::Arc) {
            if (t0 > 1e-9) pieces.push_back(s->addArc(E.c, E.a, attachPoint(*s, at(t0), ent), E.construction));
            if (t1 < 1.0 - 1e-9) pieces.push_back(s->addArc(E.c, attachPoint(*s, at(t1), ent), E.b, E.construction));
        } else {
            uint32_t qs = attachPoint(*s, at(t1), ent), qe = attachPoint(*s, at(t0), ent);
            if (qs != qe) pieces.push_back(s->addArc(E.c, qs, qe, E.construction));
        }
        // Carry over constraints that stay meaningful on the remaining pieces.
        for (const sk::Constraint& oc : oldCons) {
            bool copyAll = false, copyFirst = false;
            if (E.type == EntType::Line)
                copyAll = oc.type == CType::Horizontal || oc.type == CType::Vertical || oc.type == CType::Parallel ||
                          oc.type == CType::Perpendicular || oc.type == CType::Collinear || oc.type == CType::Tangent ||
                          (oc.type == CType::Symmetric && oc.e[2] == ent);
            else
                copyFirst = oc.type == CType::Radius || oc.type == CType::Diameter || oc.type == CType::Equal;
            if (!copyAll && !copyFirst) continue;
            for (size_t i = 0; i < pieces.size(); ++i) {
                if (copyFirst && i > 0) break;
                uint32_t e[3] = {oc.e[0], oc.e[1], oc.e[2]};
                for (auto& x : e) if (x == ent) x = pieces[i];
                if (s->hasConstraint(oc.type, e[0], e[1], e[2])) continue;
                uint32_t id = s->addConstraint(oc.type, e[0], e[1], e[2], oc.value);
                s->cons[id].value2 = oc.value2;
                s->cons[id].label = oc.label;
                s->cons[id].labelSet = oc.labelSet;
            }
        }
    }
    removeCurveOnly(*s, ent);
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("Could not trim that", ToastKind::Warning);
        return;
    }
    sk_.selEnt.clear();
    sketchFinishOp();
}

void App::breakAt(uint32_t ent) {
    Sketch* s = curSketch();
    if (!s) return;
    const Entity* ep = s->find(ent);
    if (!ep || ep->locked || ep->type == EntType::Point) return;
    if (ep->type != EntType::Line && ep->type != EntType::Arc && ep->type != EntType::Circle) {
        toast("Break works on lines, arcs, and circles", ToastKind::Warning);
        return;
    }
    const Entity E = *ep;
    std::vector<double> cuts;
    for (double t : sk::curveSplitParams(*s, ent)) {
        if (E.type == EntType::Circle || (t > 1e-9 && t < 1.0 - 1e-9)) cuts.push_back(t);
    }
    if ((E.type == EntType::Circle && cuts.size() < 2) || (E.type != EntType::Circle && cuts.empty())) {
        toast("Nothing crosses that curve", ToastKind::Warning);
        return;
    }
    Sketch::ArcGeom g;
    Vec2 la(0), lb(0);
    if (E.type == EntType::Line) { la = s->pt(E.a); lb = s->pt(E.b); }
    else if (!s->arcGeom(E, g)) return;
    auto at = [&](double t) -> Vec2 { return E.type == EntType::Line ? la + (lb - la) * t : s->arcPoint(g, t); };

    Sketch backup = *s;
    pushUndo("Break");
    std::vector<sk::Constraint> oldCons;
    for (auto& [id, c] : s->cons)
        if (c.e[0] == ent || c.e[1] == ent || c.e[2] == ent) oldCons.push_back(c);
    std::vector<uint32_t> pieces;
    if (E.type == EntType::Line || E.type == EntType::Arc) {
        uint32_t prev = E.a;
        for (double t : cuts) {
            uint32_t p = attachPoint(*s, at(t), ent);
            if (p == prev) continue;
            pieces.push_back(E.type == EntType::Line ? s->addLine(prev, p, E.construction) : s->addArc(E.c, prev, p, E.construction));
            prev = p;
        }
        if (prev != E.b)
            pieces.push_back(E.type == EntType::Line ? s->addLine(prev, E.b, E.construction) : s->addArc(E.c, prev, E.b, E.construction));
    } else {
        std::vector<uint32_t> pts;
        for (double t : cuts) pts.push_back(attachPoint(*s, at(t), ent));
        for (size_t i = 0; i < pts.size(); ++i) {
            uint32_t a = pts[i], b = pts[(i + 1) % pts.size()];
            if (a == b) continue;
            pieces.push_back(s->addArc(E.c, a, b, E.construction));
        }
    }
    if (pieces.size() < 2) {
        *s = backup;
        undo_.dropLast();
        toast("Nothing crosses that curve", ToastKind::Warning);
        return;
    }
    for (const sk::Constraint& oc : oldCons) {
        bool copy = false;
        if (E.type == EntType::Line)
            copy = oc.type == CType::Horizontal || oc.type == CType::Vertical || oc.type == CType::Parallel ||
                   oc.type == CType::Perpendicular || oc.type == CType::Collinear || oc.type == CType::Tangent ||
                   (oc.type == CType::Symmetric && oc.e[2] == ent);
        else
            copy = oc.type == CType::Radius || oc.type == CType::Diameter || oc.type == CType::Equal;
        if (!copy) continue;
        for (uint32_t piece : pieces) {
            uint32_t e[3] = {oc.e[0], oc.e[1], oc.e[2]};
            for (auto& x : e) if (x == ent) x = piece;
            if (s->hasConstraint(oc.type, e[0], e[1], e[2])) continue;
            uint32_t id = s->addConstraint(oc.type, e[0], e[1], e[2], oc.value);
            s->cons[id].value2 = oc.value2;
            s->cons[id].label = oc.label;
            s->cons[id].labelSet = oc.labelSet;
        }
    }
    removeCurveOnly(*s, ent);
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("Could not break that", ToastKind::Warning);
        return;
    }
    sk_.selEnt.clear();
    sketchFinishOp();
}

bool App::sketchTransform(bool translate, bool rot, bool scale, bool copy, double dx, double dy, double angleDeg, double factor, Vec2 pivot) {
    Sketch* s = curSketch();
    if (!s) return false;
    std::vector<uint32_t> ids;
    for (uint32_t id : sk_.selEnt) {
        const Entity* e = s->find(id);
        if (e && !e->locked && e->type != EntType::Point) ids.push_back(id);
    }
    if (ids.empty()) {
        toast("Select sketch curves first", ToastKind::Warning);
        return false;
    }
    if (scale && !(factor > 1e-9)) {
        toast("Scale needs a factor greater than zero", ToastKind::Warning);
        return false;
    }
    double ang = angleDeg * kPi / 180.0;
    double cs = std::cos(ang), sn = std::sin(ang);
    auto apply = [&](Vec2 p) {
        Vec2 q = p;
        if (scale) q = pivot + (q - pivot) * factor;
        if (rot) {
            Vec2 d = q - pivot;
            q = pivot + Vec2(cs * d.x - sn * d.y, sn * d.x + cs * d.y);
        }
        if (translate) q += Vec2(dx, dy);
        return q;
    };

    Sketch backup = *s;
    pushUndo(copy ? "Copy" : scale ? "Scale" : rot ? "Rotate" : "Move");
    std::map<uint32_t, uint32_t> pmap;
    auto mapPt = [&](uint32_t pid) -> uint32_t {
        if (!pid) return 0;
        auto it = pmap.find(pid);
        if (it != pmap.end()) return it->second;
        const Entity* e = s->find(pid);
        if (!e || e->type != EntType::Point) return pid;
        uint32_t np = s->addPoint(s->pt(pid));
        pmap[pid] = np;
        return np;
    };
    std::vector<uint32_t> work = ids;
    if (copy) {
        work.clear();
        for (uint32_t id : ids) {
            const Entity* ep = s->find(id);
            if (!ep) continue;
            Entity e = *ep;
            uint32_t nid = 0;
            if (e.type == EntType::Line) nid = s->addLine(mapPt(e.a), mapPt(e.b), e.construction);
            else if (e.type == EntType::Circle) nid = s->addCircle(mapPt(e.c), e.r, e.construction);
            else if (e.type == EntType::Arc) nid = s->addArc(mapPt(e.c), mapPt(e.a), mapPt(e.b), e.construction);
            else if (e.type == EntType::Ellipse) nid = s->addEllipse(mapPt(e.c), e.r, e.x, e.y, e.construction);
            else if (e.type == EntType::EllipArc) nid = s->addEllipArc(mapPt(e.c), e.r, e.x, e.y, mapPt(e.a), mapPt(e.b), e.construction);
            else if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) {
                std::vector<uint32_t> np;
                for (uint32_t p : e.pts) np.push_back(mapPt(p));
                nid = e.type == EntType::CtrlSpline ? s->addCtrlSpline(np, e.loop, e.construction) : s->addSpline(np, e.loop, e.construction);
            }
            if (nid) work.push_back(nid);
        }
    }
    std::set<uint32_t> pts;
    for (uint32_t id : work) {
        const Entity* e = s->find(id);
        if (e) collectGeomPoints(*e, pts);
    }
    for (uint32_t p : pts) {
        Entity* e = s->find(p);
        if (!e || e->locked || e->type != EntType::Point) continue;
        s->setPt(p, apply(Vec2(e->x, e->y)));
    }
    if (scale || rot) {
        for (uint32_t id : work) {
            Entity* e = s->find(id);
            if (!e) continue;
            if (scale && (e->type == EntType::Circle || e->type == EntType::Ellipse || e->type == EntType::EllipArc)) {
                e->r *= factor;
                if (e->type != EntType::Circle) e->x *= factor;
            }
            if (rot && (e->type == EntType::Ellipse || e->type == EntType::EllipArc)) e->y += ang;
        }
    }
    if (!copy) {
        std::set<uint32_t> sel(ids.begin(), ids.end());
        for (auto& note : s->notes) {
            if (note.ents.empty()) continue;
            bool all = true;
            for (uint32_t id : note.ents) if (!sel.count(id)) all = false;
            if (!all) continue;
            note.origin = apply(note.origin);
            if (scale) note.height *= factor;
        }
    }
    if (work.empty() || !sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast(copy ? "Could not copy that" : "That conflicts with the sketch's constraints", ToastKind::Warning);
        return false;
    }
    if (copy) {
        sk_.selEnt.clear();
        for (uint32_t id : work) sk_.selEnt.insert(id);
    }
    sketchFinishOp();
    return true;
}

bool App::sketchPattern(bool circular, int nx, int ny, double sx, double sy, double angleDeg, Vec2 pivot) {
    Sketch* s = curSketch();
    if (!s) return false;
    std::vector<uint32_t> ids;
    for (uint32_t id : sk_.selEnt) {
        const Entity* e = s->find(id);
        if (e && !e->locked && e->type != EntType::Point) ids.push_back(id);
    }
    if (ids.empty()) {
        toast("Select sketch curves first", ToastKind::Warning);
        return false;
    }
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;
    double step = 0;
    if (circular) {
        ny = 1;
        if (nx < 2) { toast("A pattern needs at least two copies", ToastKind::Warning); return false; }
        if (std::abs(angleDeg) < 1e-6) { toast("Angle must not be zero", ToastKind::Warning); return false; }
        step = angleDeg / nx;
        double mod = std::fmod(std::abs(step), 360.0);
        if (mod < 1e-4 || std::abs(mod - 360.0) < 1e-4) {
            toast("Two copies land on the same spot", ToastKind::Warning);
            return false;
        }
    } else {
        if (nx * ny < 2) { toast("A pattern needs at least two copies", ToastKind::Warning); return false; }
        if (nx > 1 && std::abs(sx) < 1e-9) { toast("Spacing must not be zero", ToastKind::Warning); return false; }
        if (ny > 1 && std::abs(sy) < 1e-9) { toast("Spacing must not be zero", ToastKind::Warning); return false; }
    }
    if ((long)nx * ny > 500) { toast("That is more than 500 copies", ToastKind::Warning); return false; }

    Sketch backup = *s;
    pushUndo("Pattern");
    std::vector<uint32_t> made;
    auto duplicate = [&](const std::vector<uint32_t>& src, std::map<uint32_t, uint32_t>& pmap) {
        auto mapPt = [&](uint32_t pid) -> uint32_t {
            if (!pid) return 0;
            auto it = pmap.find(pid);
            if (it != pmap.end()) return it->second;
            const Entity* e = s->find(pid);
            if (!e || e->type != EntType::Point) return pid;
            uint32_t np = s->addPoint(s->pt(pid));
            pmap[pid] = np;
            return np;
        };
        std::vector<uint32_t> out;
        for (uint32_t id : src) {
            const Entity* ep = s->find(id);
            if (!ep) continue;
            Entity e = *ep;
            uint32_t nid = 0;
            if (e.type == EntType::Line) nid = s->addLine(mapPt(e.a), mapPt(e.b), e.construction);
            else if (e.type == EntType::Circle) nid = s->addCircle(mapPt(e.c), e.r, e.construction);
            else if (e.type == EntType::Arc) nid = s->addArc(mapPt(e.c), mapPt(e.a), mapPt(e.b), e.construction);
            else if (e.type == EntType::Ellipse) nid = s->addEllipse(mapPt(e.c), e.r, e.x, e.y, e.construction);
            else if (e.type == EntType::EllipArc) nid = s->addEllipArc(mapPt(e.c), e.r, e.x, e.y, mapPt(e.a), mapPt(e.b), e.construction);
            else if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) {
                std::vector<uint32_t> np;
                for (uint32_t p : e.pts) np.push_back(mapPt(p));
                nid = e.type == EntType::CtrlSpline ? s->addCtrlSpline(np, e.loop, e.construction) : s->addSpline(np, e.loop, e.construction);
            }
            if (nid) out.push_back(nid);
        }
        return out;
    };
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            if (ix == 0 && iy == 0) continue;
            std::map<uint32_t, uint32_t> pmap;
            std::vector<uint32_t> copy = duplicate(ids, pmap);
            double ang = circular ? step * ix * kPi / 180.0 : 0;
            double cs = std::cos(ang), sn = std::sin(ang);
            Vec2 shift(sx * ix, sy * iy);
            auto apply = [&](Vec2 p) {
                if (!circular) return p + shift;
                Vec2 d = p - pivot;
                return pivot + Vec2(cs * d.x - sn * d.y, sn * d.x + cs * d.y);
            };
            std::set<uint32_t> pts;
            for (uint32_t id : copy) {
                const Entity* e = s->find(id);
                if (e) collectGeomPoints(*e, pts);
            }
            for (uint32_t p : pts) {
                Entity* e = s->find(p);
                if (!e || e->locked || e->type != EntType::Point) continue;
                s->setPt(p, apply(Vec2(e->x, e->y)));
            }
            if (circular) {
                for (uint32_t id : copy) {
                    Entity* e = s->find(id);
                    if (e && (e->type == EntType::Ellipse || e->type == EntType::EllipArc)) e->y += ang;
                }
            }
            made.insert(made.end(), copy.begin(), copy.end());
        }
    }
    if (made.empty() || !sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("Could not pattern that", ToastKind::Warning);
        return false;
    }
    sketchFinishOp();
    return true;
}

void App::mirrorSelection(uint32_t lineId) {
    Sketch* s = curSketch();
    if (!s) return;
    const Entity* ml = s->find(lineId);
    if (!ml || ml->type != EntType::Line) return;
    Vec2 A = s->pt(ml->a), B = s->pt(ml->b);
    Vec2 u = B - A;
    double len = glm::length(u);
    if (len < 1e-12) return;
    u /= len;
    auto refl = [&](Vec2 p) { Vec2 d = p - A; return (A + u * glm::dot(d, u)) * 2.0 - p; };
    auto onAxis = [&](Vec2 p) { return std::abs(cross2(u, p - A)) < 1e-6; };

    Sketch backup = *s;
    pushUndo("Mirror");
    std::map<uint32_t, uint32_t> pmap;
    auto mirrorPoint = [&](uint32_t pid) -> uint32_t {
        auto it = pmap.find(pid);
        if (it != pmap.end()) return it->second;
        Vec2 p = s->pt(pid);
        if (onAxis(p)) { pmap[pid] = pid; return pid; }
        uint32_t np = s->addPoint(refl(p));
        pmap[pid] = np;
        s->addConstraint(CType::Symmetric, pid, np, lineId);
        return np;
    };
    std::vector<uint32_t> ids(sk_.selEnt.begin(), sk_.selEnt.end());
    int made = 0;
    for (uint32_t id : ids) {
        const Entity* ep = s->find(id);
        if (!ep || ep->locked || ep->type == EntType::Point) continue;
        Entity e = *ep;
        if (e.type == EntType::Line) {
            uint32_t na = mirrorPoint(e.a), nb = mirrorPoint(e.b);
            if (na != nb) { s->addLine(na, nb, e.construction); ++made; }
        } else if (e.type == EntType::Circle) {
            uint32_t nc = mirrorPoint(e.c);
            if (nc != e.c) {
                uint32_t c2 = s->addCircle(nc, e.r, e.construction);
                s->addConstraint(CType::Equal, id, c2);
                ++made;
            }
        } else if (e.type == EntType::Arc) {
            uint32_t nc = mirrorPoint(e.c), na = mirrorPoint(e.a), nb = mirrorPoint(e.b);
            if (nc != e.c) { s->addArc(nc, nb, na, e.construction); ++made; }
        }
    }
    if (!made || !sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast(made ? "Mirror conflicts with existing constraints" : "Nothing to mirror", ToastKind::Warning);
        return;
    }
    sk_.tool.clear();
    sketchFinishOp();
}

// ===================================================================== overlay drawing

namespace {

#define kBlue (ui::th(IM_COL32(24, 104, 220, 255)))
#define kDark (ui::th(IM_COL32(28, 32, 40, 255)))
#define kHover (ui::th(IM_COL32(255, 160, 100, 255)))
#define kSelect (ui::th(IM_COL32(255, 96, 30, 255)))
#define kDim (ui::th(IM_COL32(18, 110, 70, 255)))
#define kBad (ui::th(IM_COL32(220, 50, 50, 255)))
#define kFaint (ui::th(IM_COL32(120, 130, 145, 210)))

bool toScr(const SketchState& st, const sk::Plane& pl, Vec2 p, ImVec2& o) {
    glm::dvec4 c = st.vp * glm::dvec4(pl.toWorld(p), 1.0);
    if (c.w <= 1e-9) return false;
    o.x = (float)((c.x / c.w * 0.5 + 0.5) * st.W);
    o.y = (float)((c.y / c.w * 0.5 + 0.5) * st.H);
    return std::isfinite(o.x) && std::isfinite(o.y) && std::abs(o.x) < 1e7f && std::abs(o.y) < 1e7f;
}

void strokePath(ImDrawList* dl, const std::vector<ImVec2>& p, ImU32 col, float th, bool dashed, float dash = 7.f, float gap = 4.f) {
    if (p.size() < 2) return;
    if (!dashed) { dl->AddPolyline(p.data(), (int)p.size(), col, 0, th); return; }
    float acc = 0;
    bool on = true;
    for (size_t i = 1; i < p.size(); ++i) {
        ImVec2 a = p[i - 1], b = p[i];
        float seg = std::hypot(b.x - a.x, b.y - a.y);
        if (seg < 1e-4f) continue;
        float pos = 0;
        while (pos < seg) {
            float lim = on ? dash : gap;
            float step = std::min(lim - acc, seg - pos);
            if (on) {
                float t0 = pos / seg, t1 = (pos + step) / seg;
                dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0), ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), col, th);
            }
            pos += step;
            acc += step;
            if (acc >= lim - 1e-4f) { on = !on; acc = 0; }
        }
    }
}

bool curvePath(const SketchState& st, const Sketch& s, const Entity& e, std::vector<ImVec2>& out) {
    out.clear();
    const sk::Plane& pl = s.plane;
    ImVec2 q;
    if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) {
        auto poly = e.type == EntType::CtrlSpline ? s.ctrlSplinePoly(e, 8) : s.splinePoly(e, 8);
        for (auto& p : poly) {
            if (!toScr(st, pl, p, q)) return false;
            out.push_back(q);
        }
        return out.size() >= 2;
    }
    if (e.type == EntType::Ellipse || e.type == EntType::EllipArc) {
        Sketch::EllipseGeom g;
        if (!s.ellipseGeom(e, g)) return false;
        int n = 64;
        for (int i = 0; i <= n; ++i) {
            if (!toScr(st, pl, s.ellipsePoint(g, (double)i / n), q)) return false;
            out.push_back(q);
        }
        return out.size() >= 2;
    }
    if (e.type == EntType::Line) {
        if (!toScr(st, pl, s.pt(e.a), q)) return false;
        out.push_back(q);
        if (!toScr(st, pl, s.pt(e.b), q)) return false;
        out.push_back(q);
        return true;
    }
    Sketch::ArcGeom g;
    if (!s.arcGeom(e, g)) return false;
    double rpx = g.r * std::max(st.ppu, 1e-6);
    double step = 2.0 * std::acos(std::clamp(1.0 - 0.35 / std::max(rpx, 0.5), -1.0, 1.0));
    step = std::clamp(step, kTwoPi / 720.0, kPi / 4);
    int n = std::max(2, (int)std::ceil(g.sweep / step));
    for (int i = 0; i <= n; ++i) {
        if (!toScr(st, pl, s.arcPoint(g, (double)i / n), q)) return false;
        out.push_back(q);
    }
    return true;
}

void arrowHead(ImDrawList* dl, ImVec2 tip, ImVec2 dirToward, float size, ImU32 col) {
    float dx = dirToward.x - tip.x, dy = dirToward.y - tip.y, l = std::hypot(dx, dy);
    if (l < 1e-4f) return;
    dx /= l; dy /= l;
    ImVec2 b(tip.x + dx * size, tip.y + dy * size);
    ImVec2 n(-dy * size * 0.32f, dx * size * 0.32f);
    dl->AddTriangleFilled(tip, ImVec2(b.x + n.x, b.y + n.y), ImVec2(b.x - n.x, b.y - n.y), col);
}

void textBox(ImDrawList* dl, ImVec2 center, const std::string& txt, ImU32 col, ImVec4* rectOut, float S, bool bg = true) {
    ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
    ImVec2 p0(center.x - ts.x * 0.5f - 3 * S, center.y - ts.y * 0.5f - 1 * S), p1(center.x + ts.x * 0.5f + 3 * S, center.y + ts.y * 0.5f + 1 * S);
    if (bg) dl->AddRectFilled(p0, p1, ui::th(IM_COL32(255, 255, 255, 215)), 3 * S);
    dl->AddText(ImVec2(p0.x + 3 * S, p0.y + 1 * S), col, txt.c_str());
    if (rectOut) *rectOut = ImVec4(p0.x, p0.y, p1.x, p1.y);
}

Icon conIcon(CType t) {
    switch (t) {
    case CType::Coincident: case CType::PointOnObject: return Icon::ConCoincident;
    case CType::Horizontal: return Icon::ConHorizontal;
    case CType::Vertical: return Icon::ConVertical;
    case CType::Parallel: return Icon::ConParallel;
    case CType::Perpendicular: return Icon::ConPerpendicular;
    case CType::Equal: return Icon::ConEqual;
    case CType::Tangent: return Icon::ConTangent;
    case CType::Midpoint: return Icon::ConMidpoint;
    case CType::Symmetric: return Icon::ConSymmetric;
    case CType::Fix: return Icon::ConFix;
    case CType::Collinear: return Icon::ConCollinear;
    default: return Icon::None;
    }
}

Vec2 entAnchor(const Sketch& s, uint32_t id) {
    const Entity* e = s.find(id);
    if (!e) return {0, 0};
    if (e->type == EntType::Point) return {e->x, e->y};
    if (e->type == EntType::Line) return s.center(*e);
    Sketch::ArcGeom g;
    if (!s.arcGeom(*e, g)) return {0, 0};
    return s.arcPoint(g, e->type == EntType::Circle ? 0.125 : 0.5);
}

std::string dimText(const sk::Constraint& c) {
    if (!c.expr.empty()) {
        // Parameter driven: show the expression, like Fusion shows fx: values.
        std::string e = c.expr;
        if (e.size() > 24) e = e.substr(0, 23) + "\xE2\x80\xA6";
        return e;
    }
    switch (c.type) {
    case CType::Radius: return "R" + fmtNum(std::abs(c.value), 3);
    case CType::Diameter: return "\xC3\x98" + fmtNum(std::abs(c.value), 3);
    case CType::Angle: return fmtNum(std::abs(c.value) * 180.0 / kPi, 2) + "\xC2\xB0";
    default: return fmtNum(std::abs(c.value), 3);
    }
}

// Draws one dimension. Returns the text rectangle.
ImVec4 drawDimension(ImDrawList* dl, const SketchState& st, const Sketch& s, const sk::Constraint& c, ImU32 col, float S, double unitPerPx) {
    const sk::Plane& pl = s.plane;
    auto P = [&](Vec2 p, ImVec2& o) { return toScr(st, pl, p, o); };
    float th = 1.4f * S;
    ImVec4 rect(0, 0, 0, 0);
    std::string txt = dimText(c);
    if (c.driven) txt = "(" + txt + ")";
    Vec2 label = c.label;
    ImVec2 a, b, t;
    auto line = [&](Vec2 u, Vec2 v) { ImVec2 p, q; if (P(u, p) && P(v, q)) dl->AddLine(p, q, col, th); };
    auto arrows = [&](Vec2 u, Vec2 v) {
        ImVec2 p, q;
        if (!P(u, p) || !P(v, q)) return;
        arrowHead(dl, p, q, 9 * S, col);
        arrowHead(dl, q, p, 9 * S, col);
    };
    switch (c.type) {
    case CType::Distance: case CType::HDistance: case CType::VDistance: {
        Vec2 p0 = s.pt(c.e[0]), p1 = s.pt(c.e[1]);
        Vec2 d0, d1;
        double gap = 3 * unitPerPx;
        if (c.type == CType::Distance) {
            Vec2 u = p1 - p0;
            double L = glm::length(u);
            if (L < 1e-12) return rect;
            u /= L;
            Vec2 n = perp(u);
            double off = glm::dot(label - p0, n);
            if (std::abs(off) < gap * 2) off = off < 0 ? -gap * 2 : gap * 2;
            d0 = p0 + n * off; d1 = p1 + n * off;
            double sg = off < 0 ? -1 : 1;
            line(p0 + n * (sg * gap), d0 + n * (sg * 5 * unitPerPx));
            line(p1 + n * (sg * gap), d1 + n * (sg * 5 * unitPerPx));
        } else if (c.type == CType::HDistance) {
            double y = label.y;
            d0 = {p0.x, y}; d1 = {p1.x, y};
            double s0 = y >= p0.y ? 1 : -1, s1 = y >= p1.y ? 1 : -1;
            line({p0.x, p0.y + s0 * gap}, {p0.x, y + s0 * 5 * unitPerPx});
            line({p1.x, p1.y + s1 * gap}, {p1.x, y + s1 * 5 * unitPerPx});
        } else {
            double x = label.x;
            d0 = {x, p0.y}; d1 = {x, p1.y};
            double s0 = x >= p0.x ? 1 : -1, s1 = x >= p1.x ? 1 : -1;
            line({p0.x + s0 * gap, p0.y}, {x + s0 * 5 * unitPerPx, p0.y});
            line({p1.x + s1 * gap, p1.y}, {x + s1 * 5 * unitPerPx, p1.y});
        }
        line(d0, d1);
        arrows(d0, d1);
        Vec2 u = d1 - d0;
        double L = glm::length(u);
        Vec2 tp = L > 1e-12 ? d0 + u * std::clamp(glm::dot(label - d0, u) / (L * L), 0.0, 1.0) : d0;
        if (P(tp, t)) textBox(dl, ImVec2(t.x, t.y - 9 * S), txt, col, &rect, S);
        break;
    }
    case CType::Radius: case CType::Diameter: {
        const Entity* e = s.find(c.e[0]);
        Sketch::ArcGeom g;
        if (!e || !s.arcGeom(*e, g)) return rect;
        Vec2 d = label - g.c;
        double dl2 = glm::length(d);
        Vec2 u = dl2 > 1e-9 ? d / dl2 : Vec2(1, 0);
        Vec2 edge = g.c + u * g.r;
        Vec2 far = dl2 > g.r ? label : edge;
        if (c.type == CType::Radius) {
            line(g.c, far);
            ImVec2 p, q;
            if (P(edge, p) && P(g.c, q)) arrowHead(dl, p, q, 9 * S, col);
        } else {
            Vec2 opp = g.c - u * g.r;
            line(opp, far);
            ImVec2 p, q, r2;
            if (P(edge, p) && P(opp, q) && P(g.c, r2)) { arrowHead(dl, p, r2, 9 * S, col); arrowHead(dl, q, r2, 9 * S, col); }
        }
        if (P(far, t)) textBox(dl, ImVec2(t.x, t.y - 9 * S), txt, col, &rect, S);
        break;
    }
    case CType::Angle: {
        const Entity *l0 = s.find(c.e[0]), *l1 = s.find(c.e[1]);
        if (!l0 || !l1) return rect;
        Vec2 a0 = s.pt(l0->a), d0 = s.pt(l0->b) - a0, a1 = s.pt(l1->a), d1 = s.pt(l1->b) - a1;
        double den = cross2(d0, d1);
        if (std::abs(den) < 1e-12) return rect;
        double tt = cross2(a1 - a0, d1) / den;
        Vec2 V = a0 + d0 * tt;
        double R = glm::length(label - V);
        if (R < 4 * unitPerPx) R = 4 * unitPerPx;
        double ang0 = std::atan2(d0.y, d0.x);
        double sweep = c.value;
        // The arc sits between the two lines in the direction of the measured angle.
        std::vector<ImVec2> arc;
        int n = std::max(6, (int)(std::abs(sweep) / 0.08));
        for (int i = 0; i <= n; ++i) {
            double a = ang0 + sweep * i / n;
            ImVec2 q;
            if (P(V + Vec2(std::cos(a), std::sin(a)) * R, q)) arc.push_back(q);
        }
        if (arc.size() >= 2) {
            dl->AddPolyline(arc.data(), (int)arc.size(), col, 0, th);
            arrowHead(dl, arc.front(), arc[1], 9 * S, col);
            arrowHead(dl, arc.back(), arc[arc.size() - 2], 9 * S, col);
        }
        double am = ang0 + sweep * 0.5;
        ImVec2 q;
        if (P(V + Vec2(std::cos(am), std::sin(am)) * R * 1.0, q)) textBox(dl, ImVec2(q.x, q.y), txt, col, &rect, S);
        break;
    }
    case CType::PointLineDistance: {
        const Entity* l = s.find(c.e[1]);
        if (!l) return rect;
        Vec2 la = s.pt(l->a), u = s.pt(l->b) - la;
        double L = glm::length(u);
        if (L < 1e-12) return rect;
        u /= L;
        Vec2 p = s.pt(c.e[0]);
        Vec2 foot = la + u * glm::dot(p - la, u);
        line(p, foot);
        arrows(p, foot);
        Vec2 mid = (p + foot) * 0.5;
        if (P(mid, t)) textBox(dl, ImVec2(t.x + 14 * S, t.y), txt, col, &rect, S);
        break;
    }
    default: break;
    }
    return rect;
}

} // namespace

void App::sketchLookAt() {
    Sketch* s = curSketch();
    if (!s) return;
    const sk::Plane& pl = s->plane;
    glm::vec3 x = glm::vec3(pl.x), y = glm::vec3(pl.y), n = glm::vec3(pl.normal());
    glm::quat r = glm::normalize(glm::quat_cast(glm::mat3(x, y, n)));
    glm::vec3 target = glm::vec3(pl.o);
    float dist = 220.f;
    Vec2 lo, hi;
    s->bounds(lo, hi);
    if (hi.x >= lo.x) {
        Vec2 c = (lo + hi) * 0.5;
        target = glm::vec3(pl.toWorld(c));
        dist = (float)std::max(glm::length(hi - lo) * 1.6, 40.0);
    } else if (sceneBounds_.valid()) {
        glm::vec3 c = sceneBounds_.center();
        target = glm::vec3(pl.o) + x * glm::dot(c - glm::vec3(pl.o), x) + y * glm::dot(c - glm::vec3(pl.o), y);
        dist = std::max(sceneBounds_.diag() * 1.4f, 60.f);
    }
    cam_.animateTo(target, dist, r);
    activity();
}

void App::drawSketchOverlay() {
    if (doc_.sketches.empty() && !sk_.choosingPlane) return;
    computeTransforms(sk_, cam_, sceneBounds_);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float S = uiScale_;
    ImGuiIO& io = ImGui::GetIO();
    std::vector<ImVec2> path;

    // Other visible sketches: thin dark lines.
    for (auto& [id, s] : doc_.sketches) {
        if (sk_.active && id == sk_.sketchId) continue;
        if (!sketchShown(s)) continue;
        for (auto& [eid, e] : s.ents) {
            if (e.type == EntType::Point || e.locked) continue;
            if (curvePath(sk_, s, e, path)) strokePath(dl, path, e.construction ? ui::th(IM_COL32(90, 110, 150, 150)) : ui::th(IM_COL32(40, 70, 130, 230)), 1.3f * S, e.construction);
        }
    }

    if (sk_.choosingPlane) {
        double half = planeHalfSize(sceneBounds_);
        for (int i = 0; i < 3; ++i) {
            sk::Plane p = originPlane(i);
            Sketch tmp;
            tmp.plane = p;
            ImVec2 q[4];
            bool ok = toScr(sk_, p, {0, 0}, q[0]) && toScr(sk_, p, {half, 0}, q[1]) && toScr(sk_, p, {half, half}, q[2]) && toScr(sk_, p, {0, half}, q[3]);
            if (!ok) continue;
            bool hov = sk_.hoverPlane == i;
            dl->AddConvexPolyFilled(q, 4, hov ? ui::th(IM_COL32(255, 107, 53, 120)) : ui::th(IM_COL32(255, 107, 53, 45)));
            dl->AddPolyline(q, 4, hov ? ui::th(IM_COL32(255, 90, 30, 255)) : ui::th(IM_COL32(255, 107, 53, 190)), ImDrawFlags_Closed, 1.6f * S);
            dl->AddText(ImVec2(q[2].x - 22 * S, q[2].y - 16 * S), ui::th(IM_COL32(190, 70, 25, 235)), p.name.c_str());
        }
        const char* hint = "Select a plane or a flat face to sketch on   (Esc to cancel)";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        float cx = io.DisplaySize.x * 0.5f, cy = (34 + 76 + 22) * S;
        dl->AddRectFilled(ImVec2(cx - ts.x * 0.5f - 12 * S, cy - 4 * S), ImVec2(cx + ts.x * 0.5f + 12 * S, cy + ts.y + 6 * S), ui::th(IM_COL32(30, 36, 48, 225)), 6 * S);
        dl->AddText(ImVec2(cx - ts.x * 0.5f, cy + 1 * S), ui::th(IM_COL32(255, 255, 255, 255)), hint);
        return;
    }
    if (!sk_.active) return;
    Sketch* sp = curSketch();
    if (!sp) return;
    Sketch& s = *sp;
    const sk::Plane& pl = s.plane;

    // ---- grid and axes
    {
        ImVec2 corners[4] = {{0, 0}, {sk_.W, 0}, {sk_.W, sk_.H}, {0, sk_.H}};
        Vec2 lo(1e30), hi(-1e30);
        bool ok = true;
        for (auto& c : corners) {
            glm::dvec3 o, d;
            rayFromScreen(sk_, c, o, d);
            Vec2 q;
            if (!pl.intersect(o, d, q)) { ok = false; break; }
            lo = glm::min(lo, q); hi = glm::max(hi, q);
        }
        if (ok && hi.x - lo.x < 1e7 && hi.y - lo.y < 1e7) {
            double step = sk_.gridStep;
            while ((hi.x - lo.x) / step > 160 || (hi.y - lo.y) / step > 160) step *= 5;
            if (sk_.optGrid) {
                long x0 = (long)std::floor(lo.x / step), x1 = (long)std::ceil(hi.x / step);
                long y0 = (long)std::floor(lo.y / step), y1 = (long)std::ceil(hi.y / step);
                for (long ix = x0; ix <= x1; ++ix) {
                    ImVec2 p, q;
                    if (!toScr(sk_, pl, {ix * step, lo.y}, p) || !toScr(sk_, pl, {ix * step, hi.y}, q)) continue;
                    dl->AddLine(p, q, ix % 5 == 0 ? ui::th(IM_COL32(150, 160, 175, 90)) : ui::th(IM_COL32(160, 170, 185, 45)), 1.f);
                }
                for (long iy = y0; iy <= y1; ++iy) {
                    ImVec2 p, q;
                    if (!toScr(sk_, pl, {lo.x, iy * step}, p) || !toScr(sk_, pl, {hi.x, iy * step}, q)) continue;
                    dl->AddLine(p, q, iy % 5 == 0 ? ui::th(IM_COL32(150, 160, 175, 90)) : ui::th(IM_COL32(160, 170, 185, 45)), 1.f);
                }
            }
            ImVec2 p, q;
            if (toScr(sk_, pl, {lo.x, 0}, p) && toScr(sk_, pl, {hi.x, 0}, q)) dl->AddLine(p, q, ui::th(IM_COL32(210, 70, 70, 150)), 1.2f * S);
            if (toScr(sk_, pl, {0, lo.y}, p) && toScr(sk_, pl, {0, hi.y}, q)) dl->AddLine(p, q, ui::th(IM_COL32(60, 170, 90, 150)), 1.2f * S);
        }
    }

    // ---- profiles (closed regions)
    if (sk_.optProfiles && !sk_.dragging) {
        if (sk_.profSketch != s.id || sk_.profRev != s.rev) {
            sk_.profiles = sk::findProfiles(s);
            sk_.profSketch = s.id;
            sk_.profRev = s.rev;
        }
        ImDrawListFlags oldFlags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // shared triangle edges must not show seams
        for (auto& pr : sk_.profiles) {
            for (size_t i = 0; i + 2 < pr.tris.size(); i += 3) {
                ImVec2 a, b, c;
                if (toScr(sk_, pl, pr.tris[i], a) && toScr(sk_, pl, pr.tris[i + 1], b) && toScr(sk_, pl, pr.tris[i + 2], c))
                    dl->AddTriangleFilled(a, b, c, ui::th(IM_COL32(70, 140, 235, 55)));
            }
        }
        dl->Flags = oldFlags;
    }

    // ---- curves
    auto isPick = [&](uint32_t id) { return std::find(sk_.picks.begin(), sk_.picks.end(), id) != sk_.picks.end(); };
    for (auto& [id, e] : s.ents) {
        if (e.type == EntType::Point || (e.locked && sk_.tool.empty())) continue;
        if (e.locked) continue;
        if (!curvePath(sk_, s, e, path)) continue;
        bool sel = sk_.selEnt.count(id) > 0, hov = sk_.hoverEnt == id || isPick(id);
        bool defined = s.defined.count(id) > 0;
        ImU32 col = sel ? kSelect : hov ? kHover : defined ? kDark : kBlue;
        if (e.construction && !sel && !hov) col = defined ? ui::th(IM_COL32(60, 66, 78, 200)) : ui::th(IM_COL32(60, 120, 215, 190));
        float th = (sel ? 3.0f : hov ? 2.6f : e.construction ? 1.3f : 1.9f) * S;
        if (sel) strokePath(dl, path, ui::th(IM_COL32(255, 107, 53, 70)), th + 4 * S, false);
        strokePath(dl, path, col, th, e.construction);
    }
    // ---- points
    if (sk_.optPoints || !sk_.tool.empty()) {
        for (auto& [id, e] : s.ents) {
            if (e.type != EntType::Point) continue;
            if (e.locked && id != Sketch::kOrigin) continue;
            ImVec2 q;
            if (!toScr(sk_, pl, {e.x, e.y}, q)) continue;
            bool sel = sk_.selEnt.count(id) > 0, hov = sk_.hoverEnt == id || isPick(id);
            bool defined = e.locked || s.defined.count(id) > 0;
            float r = (sel || hov ? 4.5f : 3.0f) * S;
            ImU32 col = sel ? kSelect : hov ? kHover : defined ? kDark : kBlue;
            if (id == Sketch::kOrigin) {
                dl->AddCircle(q, 5 * S, ui::th(IM_COL32(60, 66, 78, 220)), 16, 1.4f * S);
                dl->AddCircleFilled(q, 1.8f * S, ui::th(IM_COL32(60, 66, 78, 220)));
            } else {
                dl->AddRectFilled(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), col);
            }
        }
    }
    // axes can be hovered while picking (dimension / constraint tools)
    if (sk_.hoverEnt == Sketch::kXAxis || sk_.hoverEnt == Sketch::kYAxis) {
        ImVec2 p, q;
        bool x = sk_.hoverEnt == Sketch::kXAxis;
        Vec2 a = x ? Vec2(-1e5, 0) : Vec2(0, -1e5), b = x ? Vec2(1e5, 0) : Vec2(0, 1e5);
        if (toScr(sk_, pl, a, p) && toScr(sk_, pl, b, q)) dl->AddLine(p, q, kHover, 2.4f * S);
    }

    // ---- dimensions
    sk_.dimRects.clear();
    sk_.conRects.clear();
    double unitPerPx = 1.0 / std::max(sk_.ppu, 1e-9);
    if (sk_.optDims) {
        for (auto& [id, c] : s.cons) {
            if (!sk::isDimension(c.type)) continue;
            sk::Constraint cc = c;
            if (!cc.labelSet) {
                // Default label position: beside the geometry.
                Vec2 a = entAnchor(s, cc.e[0]);
                cc.label = a + Vec2(12, 12) * unitPerPx * 3.0;
            }
            bool sel = sk_.selCon.count(id) > 0, hov = sk_.hoverCon == id;
            if (c.driven) {
                cc.expr.clear();
                double m = s.measureDimension(c);
                if (std::isfinite(m)) cc.value = m;
            }
            ImU32 col = c.failed ? kBad : sel ? kSelect : hov ? kHover : c.driven ? ui::th(IM_COL32(90, 98, 112, 255)) : kDim;
            ImVec4 r = drawDimension(dl, sk_, s, cc, col, S, unitPerPx);
            if (r.z > r.x) sk_.dimRects[id] = r;
        }
        if (sk_.dimPlacing && sk_.cursorValid) drawDimension(dl, sk_, s, sk_.dim, ui::th(IM_COL32(18, 110, 70, 170)), S, unitPerPx);
    }

    // ---- constraint glyphs
    if (sk_.optCons) {
        std::map<std::pair<int, int>, int> stack;
        float gs = 16.f * S;
        for (auto& [id, c] : s.cons) {
            Icon ic = conIcon(c.type);
            if (ic == Icon::None) continue;
            int count = 1;
            uint32_t ids[2] = {c.e[0], c.e[1]};
            bool pair = c.type == CType::Parallel || c.type == CType::Perpendicular || c.type == CType::Equal || c.type == CType::Tangent || c.type == CType::Collinear;
            if (c.type == CType::Horizontal || c.type == CType::Vertical) { ids[0] = c.e[1] ? c.e[0] : c.e[0]; }
            if (pair) count = 2;
            for (int k = 0; k < count; ++k) {
                Vec2 anchor;
                if (c.type == CType::Symmetric) anchor = (s.pt(c.e[0]) + s.pt(c.e[1])) * 0.5;
                else if ((c.type == CType::Horizontal || c.type == CType::Vertical) && c.e[1]) anchor = (s.pt(c.e[0]) + s.pt(c.e[1])) * 0.5;
                else anchor = entAnchor(s, ids[k]);
                ImVec2 q;
                if (!toScr(sk_, pl, anchor, q)) continue;
                auto key = std::make_pair((int)(q.x / gs), (int)(q.y / gs));
                int n = stack[key]++;
                ImVec2 ctr(q.x + 13 * S + n * (gs + 2 * S), q.y - 13 * S);
                bool sel = sk_.selCon.count(id) > 0, hov = sk_.hoverCon == id;
                ImVec2 p0(ctr.x - gs * 0.5f, ctr.y - gs * 0.5f), p1(ctr.x + gs * 0.5f, ctr.y + gs * 0.5f);
                dl->AddRectFilled(p0, p1, c.failed ? ui::th(IM_COL32(255, 220, 220, 245)) : ui::th(IM_COL32(255, 255, 255, 235)), 3 * S);
                dl->AddRect(p0, p1, c.failed ? kBad : sel ? kSelect : hov ? kHover : ui::th(IM_COL32(150, 158, 170, 255)), 3 * S, 0, (sel || hov ? 2.f : 1.f));
                ui::drawIcon(dl, ctr, gs * 0.78f, ic, true);
                if (k == 0) sk_.conRects[id] = ImVec4(p0.x, p0.y, p1.x, p1.y);
            }
        }
    }

    // ---- tool preview and cursor
    if (sk_.cursorValid && viewportHovered_ && !sk_.dragging) {
        const std::string& t = sk_.tool;
        ImVec2 cur;
        Vec2 sp2 = sk_.snap.pos;
        bool drawing = isCreateTool(t);
        ImU32 pc = ui::th(IM_COL32(24, 104, 220, 210));
        std::string readout;
        auto S2 = [&](Vec2 p, ImVec2& o) { return toScr(sk_, pl, p, o); };
        auto previewPath = [&](const std::vector<Vec2>& pts, bool closed) {
            std::vector<ImVec2> sp;
            for (auto& p : pts) { ImVec2 q; if (S2(p, q)) sp.push_back(q); }
            if (sp.size() < 2) return;
            if (closed) sp.push_back(sp.front());
            strokePath(dl, sp, pc, 1.6f * S, true);
        };
        auto circlePts = [&](Vec2 c, double r, double a0, double sweep) {
            std::vector<Vec2> v;
            int n = std::max(8, (int)(std::abs(sweep) / 0.1));
            for (int i = 0; i <= n; ++i) { double a = a0 + sweep * i / n; v.push_back(c + Vec2(std::cos(a), std::sin(a)) * r); }
            return v;
        };
        auto drawHud = [&](ImVec2 cur) {
            const auto& h = sk_.hud;
            if (h.n <= 0) return false;
            {
                // Fusion style value boxes next to the cursor.
                float x = cur.x + 18 * S, y = cur.y + 14 * S;
                for (int i = 0; i < h.n; ++i) {
                    bool typedI = !h.buf[i].empty();
                    char live[32];
                    std::snprintf(live, sizeof live, "%s%s", fmtNum(h.live[i], 2).c_str(), (h.label[i] && std::string(h.label[i]) == "Angle") ? "\xC2\xB0" : "");
                    std::string val = typedI ? h.buf[i] : std::string(live);
                    ImVec2 ls = ImGui::CalcTextSize(h.label[i]);
                    ImVec2 vs = ImGui::CalcTextSize(val.c_str());
                    float bw = std::max(vs.x + 12 * S, 56 * S), bh = ImGui::GetTextLineHeight() + 6 * S;
                    dl->AddText(ImVec2(x, y + 3 * S), ui::th(IM_COL32(60, 66, 78, 255)), h.label[i]);
                    ImVec2 b0(x + ls.x + 5 * S, y), b1(b0.x + bw, y + bh);
                    bool foc = h.focus == i;
                    dl->AddRectFilled(b0, b1, ui::th(IM_COL32(255, 255, 255, 245)), 3 * S);
                    dl->AddRect(b0, b1, foc ? ui::th(IM_COL32(30, 110, 235, 255)) : ui::th(IM_COL32(170, 176, 186, 255)), 3 * S, 0, foc ? 1.6f * S : 1.f);
                    dl->AddText(ImVec2(b0.x + 6 * S, y + 3 * S), typedI ? ui::th(IM_COL32(20, 24, 30, 255)) : ui::th(IM_COL32(120, 126, 136, 255)), val.c_str());
                    if (foc && typedI && std::fmod(ImGui::GetTime(), 1.0) < 0.6)
                        dl->AddLine(ImVec2(b0.x + 6 * S + vs.x + 1, y + 3 * S), ImVec2(b0.x + 6 * S + vs.x + 1, y + bh - 3 * S), ui::th(IM_COL32(20, 24, 30, 255)), 1.f);
                    x = b1.x + 12 * S;
                }
                activity();
            }
            return true;
        };
        if (t == "offset" && !sk_.picks.empty()) {
            double d = sketchOffsetDistance(sk_.raw);
            if (!sk_.hud.buf[0].empty()) {
                try {
                    double v = std::abs(peval::eval(sk_.hud.buf[0], doc_.params));
                    d = d < 0 ? -v : v;
                } catch (const std::runtime_error&) {}
            }
            std::vector<std::pair<Vec2, Vec2>> ols;
            std::vector<std::tuple<Vec2, double, double, double>> oas;
            if (sketchOffsetPreview(d, ols, oas)) {
                for (auto& [a, b] : ols) previewPath({a, b}, false);
                for (auto& [c, r, a0, sw] : oas) previewPath(circlePts(c, r, a0, sw), false);
            }
            ImVec2 cq;
            if (S2(sk_.raw, cq)) drawHud(cq);
        }
        if (drawing && !sk_.clicks.empty()) {
            auto& cl = sk_.clicks;
            if (t == "spline" || t == "control_spline") {
                std::vector<Vec2> fit;
                for (auto& c : cl) fit.push_back(c.pos);
                fit.push_back(sp2);
                bool closing = cl.size() >= 3 && glm::length(sp2 - cl[0].pos) < 1e-4;
                if (t == "control_spline") previewPath(fit, false);
                previewPath(t == "control_spline" ? Sketch::sampleCtrlSpline(fit, closing, 8) : Sketch::sampleFitSpline(fit, closing, 8), false);
            } else if (t == "line") {
                previewPath({cl[0].pos, sp2}, false);
                Vec2 d = sp2 - cl[0].pos;
                readout = fmtNum(glm::length(d), 2) + " mm   " + fmtNum(std::atan2(d.y, d.x) * 180 / kPi, 1) + "\xC2\xB0";
            } else if (t == "rect_3pt") {
                if (cl.size() == 1) {
                    previewPath({cl[0].pos, sp2}, false);
                    readout = fmtNum(glm::length(sp2 - cl[0].pos), 2);
                } else {
                    Vec2 corner[4];
                    if (rect3Corners(cl[0].pos, cl[1].pos, sp2, corner)) {
                        previewPath({corner[0], corner[1], corner[2], corner[3]}, true);
                        readout = fmtNum(glm::length(corner[1] - corner[0]), 2) + " x " + fmtNum(glm::length(corner[2] - corner[1]), 2);
                    } else previewPath({cl[0].pos, cl[1].pos}, false);
                }
            } else if (t == "rectangle") {
                Vec2 a = cl[0].pos;
                previewPath({a, {sp2.x, a.y}, sp2, {a.x, sp2.y}}, true);
                readout = fmtNum(std::abs(sp2.x - a.x), 2) + " x " + fmtNum(std::abs(sp2.y - a.y), 2);
            } else if (t == "rect_center") {
                Vec2 c = cl[0].pos, h(std::abs(sp2.x - c.x), std::abs(sp2.y - c.y));
                previewPath({c + Vec2(-h.x, -h.y), c + Vec2(h.x, -h.y), c + Vec2(h.x, h.y), c + Vec2(-h.x, h.y)}, true);
                readout = fmtNum(h.x * 2, 2) + " x " + fmtNum(h.y * 2, 2);
            } else if (t == "circle") {
                double r = glm::length(sp2 - cl[0].pos);
                previewPath(circlePts(cl[0].pos, r, 0, kTwoPi), false);
                previewPath({cl[0].pos, sp2}, false);
                readout = "\xC3\x98 " + fmtNum(r * 2, 2);
            } else if (t == "ellipse" || t == "ellipse_arc") {
                if (cl.size() == 1) {
                    previewPath({cl[0].pos, sp2}, false);
                    readout = "Long " + fmtNum(glm::length(sp2 - cl[0].pos), 2);
                } else {
                    Vec2 c = cl[0].pos, maj = cl[1].pos - c;
                    double major = glm::length(maj);
                    if (major > 1e-6) {
                        double rot = std::atan2(maj.y, maj.x);
                        Vec2 u = maj / major, n(-u.y, u.x);
                        Vec2 mp = cl.size() >= 3 ? cl[2].pos : sp2;
                        double minor = std::abs(glm::dot(mp - c, n));
                        if (minor < 1e-6) minor = glm::length(mp - c);
                        std::vector<Vec2> pts;
                        double cs = std::cos(rot), sn = std::sin(rot);
                        for (int i = 0; i <= 64; ++i) {
                            double a = kTwoPi * i / 64.0;
                            double lx = major * std::cos(a), ly = minor * std::sin(a);
                            pts.push_back(c + Vec2(cs * lx - sn * ly, sn * lx + cs * ly));
                        }
                        previewPath(pts, false);
                        readout = fmtNum(major * 2, 2) + " x " + fmtNum(minor * 2, 2);
                    }
                }
            } else if (t == "circle_2pt") {
                Vec2 c = (cl[0].pos + sp2) * 0.5;
                double r = glm::length(sp2 - cl[0].pos) * 0.5;
                previewPath(circlePts(c, r, 0, kTwoPi), false);
                readout = "\xC3\x98 " + fmtNum(r * 2, 2);
            } else if (t == "circle_3pt" || t == "arc") {
                if (cl.size() == 1) previewPath({cl[0].pos, sp2}, false);
                else {
                    Vec2 o; double r;
                    if (circumcenter(cl[0].pos, cl[1].pos, sp2, o, r)) {
                        if (t == "circle_3pt") previewPath(circlePts(o, r, 0, kTwoPi), false);
                        else {
                            double aA = std::atan2(cl[0].pos.y - o.y, cl[0].pos.x - o.x);
                            double relC = wrap2pi(std::atan2(sp2.y - o.y, sp2.x - o.x) - aA);
                            double relB = wrap2pi(std::atan2(cl[1].pos.y - o.y, cl[1].pos.x - o.x) - aA);
                            previewPath(circlePts(o, r, aA, relC < relB ? relB : relB - kTwoPi), false);
                        }
                        readout = "R " + fmtNum(r, 2);
                    }
                }
            } else if (t == "arc_center") {
                if (cl.size() == 1) { previewPath({cl[0].pos, sp2}, false); readout = "R " + fmtNum(glm::length(sp2 - cl[0].pos), 2); }
                else {
                    Vec2 o = cl[0].pos;
                    double r = glm::length(cl[1].pos - o);
                    double a0 = std::atan2(cl[1].pos.y - o.y, cl[1].pos.x - o.x);
                    double sw = wrap2pi(std::atan2(sp2.y - o.y, sp2.x - o.x) - a0);
                    previewPath(circlePts(o, r, a0, sw), false);
                    readout = fmtNum(sw * 180 / kPi, 1) + "\xC2\xB0";
                }
            } else if (t == "polygon" || t == "polygon_in") {
                double cr = 0;
                auto v = polygonVerts(cl[0].pos, sp2, sk_.polySides, t == "polygon" && sk_.polyCircumscribed, cr);
                if (!v.empty()) previewPath(v, true);
                readout = std::to_string(sk_.polySides) + " sides";
            } else if (t == "slot") {
                if (cl.size() == 1) previewPath({cl[0].pos, sp2}, false);
                else {
                    Vec2 c1 = cl[0].pos, c2 = cl[1].pos, u = c2 - c1;
                    double L = glm::length(u);
                    if (L > 1e-9) {
                        u /= L;
                        Vec2 n = perp(u);
                        double hw = std::abs(glm::dot(sp2 - c1, n));
                        std::vector<Vec2> v;
                        auto a2 = circlePts(c2, hw, std::atan2(-n.y, -n.x), kPi);
                        auto a1 = circlePts(c1, hw, std::atan2(n.y, n.x), kPi);
                        v.insert(v.end(), a2.begin(), a2.end());
                        v.insert(v.end(), a1.begin(), a1.end());
                        previewPath(v, true);
                        readout = "Width " + fmtNum(hw * 2, 2);
                    }
                }
            } else if (t == "slot_overall") {
                if (cl.size() == 1) {
                    previewPath({cl[0].pos, sp2}, false);
                    readout = fmtNum(glm::length(sp2 - cl[0].pos), 2);
                } else {
                    Vec2 a = cl[0].pos, b = cl[1].pos, d = b - a;
                    double L = glm::length(d);
                    if (L > 1e-9) {
                        Vec2 u = d / L, n = perp(u);
                        double hw = std::abs(glm::dot(sp2 - a, n));
                        if (hw > 1e-9 && hw * 2.0 < L) {
                            Vec2 c1 = a + u * hw, c2 = b - u * hw;
                            auto cap2 = circlePts(c2, hw, std::atan2(-n.y, -n.x), kPi);
                            auto cap1 = circlePts(c1, hw, std::atan2(n.y, n.x), kPi);
                            std::vector<Vec2> v;
                            v.insert(v.end(), cap2.begin(), cap2.end());
                            v.insert(v.end(), cap1.begin(), cap1.end());
                            previewPath(v, true);
                            readout = fmtNum(L, 2) + " x " + fmtNum(hw * 2, 2);
                        } else previewPath({a, b}, false);
                    }
                }
            } else if (t == "slot_center") {
                if (cl.size() == 1) {
                    previewPath({cl[0].pos, sp2}, false);
                    readout = fmtNum(glm::length(sp2 - cl[0].pos), 2);
                } else {
                    Vec2 c = cl[0].pos, e = cl[1].pos, d = e - c;
                    double half = glm::length(d);
                    if (half > 1e-9) {
                        Vec2 n = perp(d / half);
                        double hw = std::abs(glm::dot(sp2 - c, n));
                        Vec2 c1 = c - d, c2 = e;
                        if (hw > 1e-9) {
                            auto cap2 = circlePts(c2, hw, std::atan2(-n.y, -n.x), kPi);
                            auto cap1 = circlePts(c1, hw, std::atan2(n.y, n.x), kPi);
                            std::vector<Vec2> v;
                            v.insert(v.end(), cap2.begin(), cap2.end());
                            v.insert(v.end(), cap1.begin(), cap1.end());
                            previewPath(v, true);
                        readout = fmtNum(half * 2, 2) + " x " + fmtNum(hw * 2, 2);
                    } else previewPath({c1, c2}, false);
                    }
                }
            } else if (t == "slot_arc") {
                if (cl.size() == 1) previewPath({cl[0].pos, sp2}, false);
                else if (cl.size() == 2) {
                    Vec2 o; double r;
                    if (circumcenter(cl[0].pos, cl[1].pos, sp2, o, r)) {
                        double a0 = std::atan2(cl[0].pos.y - o.y, cl[0].pos.x - o.x);
                        double sw = wrap2pi(std::atan2(cl[1].pos.y - o.y, cl[1].pos.x - o.x) - a0);
                        double rel = wrap2pi(std::atan2(sp2.y - o.y, sp2.x - o.x) - a0);
                        if (rel < sw) previewPath(circlePts(o, r, a0, sw), false);
                        else previewPath(circlePts(o, r, std::atan2(sp2.y - o.y, sp2.x - o.x), wrap2pi(a0 - std::atan2(sp2.y - o.y, sp2.x - o.x))), false);
                    } else previewPath({cl[0].pos, cl[1].pos, sp2}, false);
                } else {
                    Vec2 o; double R;
                    if (circumcenter(cl[0].pos, cl[1].pos, cl[2].pos, o, R)) {
                        double hw = std::abs(glm::length(sp2 - o) - R);
                        ArcSlot g = makeArcSlot(cl[0].pos, cl[1].pos, cl[2].pos, hw);
                        if (g.ok) {
                            double a0 = std::atan2(g.sa.y - g.o.y, g.sa.x - g.o.x);
                            previewPath(circlePts(g.o, g.R + g.hw, a0, g.sweep), false);
                            previewPath(circlePts(g.o, g.R - g.hw, a0, g.sweep), false);
                            Vec2 rs = (g.sa - g.o) / g.R, re = (g.sb - g.o) / g.R;
                            previewPath(circlePts(g.sa, g.hw, std::atan2(-rs.y, -rs.x), kPi), false);
                            previewPath(circlePts(g.sb, g.hw, std::atan2(re.y, re.x), kPi), false);
                            readout = "Width " + fmtNum(g.hw * 2, 2);
                        } else previewPath(circlePts(o, R, std::atan2(cl[0].pos.y - o.y, cl[0].pos.x - o.x), wrap2pi(std::atan2(cl[2].pos.y - o.y, cl[2].pos.x - o.x) - std::atan2(cl[0].pos.y - o.y, cl[0].pos.x - o.x))), false);
                    }
                }
            }
        }
        if (drawing && S2(sp2, cur)) {
            // Snap marker
            const SnapInfo& sn = sk_.snap;
            if (sn.kind == SnapKind::Horizontal || sn.kind == SnapKind::Vertical) {
                ImVec2 a;
                if (!sk_.clicks.empty() && S2(sk_.clicks[0].pos, a)) strokePath(dl, {a, cur}, ui::th(IM_COL32(255, 140, 30, 200)), 1.2f * S, true);
            }
            if (sn.kind == SnapKind::Point) dl->AddRect(ImVec2(cur.x - 6 * S, cur.y - 6 * S), ImVec2(cur.x + 6 * S, cur.y + 6 * S), kHover, 0, 0, 2.f * S);
            else if (sn.kind == SnapKind::Midpoint) dl->AddTriangle(ImVec2(cur.x, cur.y - 7 * S), ImVec2(cur.x - 6 * S, cur.y + 5 * S), ImVec2(cur.x + 6 * S, cur.y + 5 * S), kHover, 2.f * S);
            else if (sn.kind == SnapKind::OnCurve) dl->AddCircle(cur, 5.5f * S, kHover, 14, 2.f * S);
            else dl->AddCircleFilled(cur, 3.f * S, ui::th(IM_COL32(24, 104, 220, 220)));
            if (drawHud(cur)) {
            } else {
                std::string tip = sk_.snapLabel;
                if (!readout.empty()) tip = tip.empty() ? readout : readout + "   " + tip;
                if (!tip.empty()) textBox(dl, ImVec2(cur.x + 16 * S + ImGui::CalcTextSize(tip.c_str()).x * 0.5f, cur.y + 18 * S), tip, ui::th(IM_COL32(40, 46, 58, 255)), nullptr, S);
            }
        }
    }
    // box select
    if (sk_.boxSel) {
        ImVec2 a = sk_.boxA, b = io.MousePos;
        dl->AddRectFilled(a, b, ui::th(IM_COL32(0, 150, 255, 30)));
        dl->AddRect(a, b, ui::th(IM_COL32(0, 150, 255, 200)));
    }
}

// ===================================================================== palette

void App::drawSketchPalette() {
    if (!sk_.active) return;
    Sketch* s = curSketch();
    if (!s) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = 210 * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - w - 12 * S, vp->Pos.y + 250 * S));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * S, 8 * S));
    ImGui::Begin("##skpalette", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("SKETCH PALETTE");
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::TextDisabled("Options");
    if (ImGui::Button(sk_.construction ? "Construction" : "Normal", ImVec2(-1, 0))) sk_.construction = !sk_.construction;
    ImGui::SetItemTooltip("Line type for new geometry (X)");
    if (ImGui::Button("Look At", ImVec2(-1, 0))) sketchLookAt();
    ImGui::Checkbox("Sketch Grid", &sk_.optGrid);
    ImGui::Checkbox("Snap", &sk_.optSnap);
    ImGui::Checkbox("Show Profile", &sk_.optProfiles);
    ImGui::Checkbox("Show Points", &sk_.optPoints);
    ImGui::Checkbox("Show Dimensions", &sk_.optDims);
    ImGui::Checkbox("Show Constraints", &sk_.optCons);
    if (sk_.tool == "polygon" || sk_.tool == "polygon_in") {
        ImGui::Separator();
        ImGui::SetNextItemWidth(80 * S);
        ImGui::InputInt("Sides", &sk_.polySides);
        sk_.polySides = std::clamp(sk_.polySides, 3, 64);
        if (sk_.tool == "polygon") ImGui::Checkbox("Circumscribed", &sk_.polyCircumscribed);
    }
    if (sk_.tool == "sk_xform") {
        ImGui::Separator();
        ImGui::RadioButton("Move", &sk_.xformKind, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Rotate", &sk_.xformKind, 1);
        ImGui::RadioButton("Scale", &sk_.xformKind, 2);
        ImGui::SameLine();
        ImGui::Checkbox("Copy", &sk_.xformCopy);
        ImGui::SetNextItemWidth(-1);
        if (sk_.xformKind == 0) {
            ImGui::InputText("X##xf", sk_.xformX, sizeof sk_.xformX);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Y##xf", sk_.xformY, sizeof sk_.xformY);
        } else if (sk_.xformKind == 1) {
            ImGui::InputText("Angle##xf", sk_.xformAng, sizeof sk_.xformAng);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Pivot X##xf", sk_.xformPx, sizeof sk_.xformPx);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Pivot Y##xf", sk_.xformPy, sizeof sk_.xformPy);
        } else {
            ImGui::InputText("Factor##xf", sk_.xformFac, sizeof sk_.xformFac);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Pivot X##xf", sk_.xformPx, sizeof sk_.xformPx);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Pivot Y##xf", sk_.xformPy, sizeof sk_.xformPy);
        }
        if (ImGui::Button("Apply", ImVec2(-1, 0))) {
            double dx = std::atof(sk_.xformX), dy = std::atof(sk_.xformY);
            double ang = std::atof(sk_.xformAng), fac = std::atof(sk_.xformFac);
            Vec2 pivot(std::atof(sk_.xformPx), std::atof(sk_.xformPy));
            bool tr = sk_.xformKind == 0, rt = sk_.xformKind == 1, sc = sk_.xformKind == 2;
            sketchTransform(tr, rt, sc, sk_.xformCopy, dx, dy, ang, fac, pivot);
        }
    }
    if (sk_.tool == "sk_pattern") {
        ImGui::Separator();
        ImGui::RadioButton("Rectangular", &sk_.patKind, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Circular", &sk_.patKind, 1);
        ImGui::SetNextItemWidth(-1);
        if (sk_.patKind == 0) {
            ImGui::InputText("Count X##pat", sk_.patNx, sizeof sk_.patNx);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Spacing X##pat", sk_.patSx, sizeof sk_.patSx);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Count Y##pat", sk_.patNy, sizeof sk_.patNy);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Spacing Y##pat", sk_.patSy, sizeof sk_.patSy);
        } else {
            ImGui::InputText("Count##pat", sk_.patCount, sizeof sk_.patCount);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Angle##pat", sk_.patAng, sizeof sk_.patAng);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Pivot X##pat", sk_.xformPx, sizeof sk_.xformPx);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("Pivot Y##pat", sk_.xformPy, sizeof sk_.xformPy);
        }
        if (ImGui::Button("Apply##pat", ImVec2(-1, 0))) {
            Vec2 pivot(std::atof(sk_.xformPx), std::atof(sk_.xformPy));
            if (sk_.patKind == 0)
                sketchPattern(false, std::atoi(sk_.patNx), std::atoi(sk_.patNy), std::atof(sk_.patSx), std::atof(sk_.patSy), 0, pivot);
            else
                sketchPattern(true, std::atoi(sk_.patCount), 1, 0, 0, std::atof(sk_.patAng), pivot);
        }
    }
    if (sk_.tool == "text") {
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##sktext", sk_.textBuf, sizeof sk_.textBuf);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##sktexth", sk_.textH, sizeof sk_.textH);
        ImGui::SetItemTooltip("Height in mm");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##skfont", sk_.textFont, sizeof sk_.textFont);
        ImGui::SetItemTooltip("Font name, such as Arial. Leave it blank for the block letters.");
        if (sk_.textNote && ImGui::Button("Update", ImVec2(-1, 0))) sketchTextUpdate();
        ImGui::TextDisabled(sk_.textNote ? "Click Update to rewrite it." : "Click to place the outlines.");
    }
    ImGui::Separator();
    if (sk_.selCon.size() == 1) {
        auto cit = s->cons.find(*sk_.selCon.begin());
        if (cit != s->cons.end() && sk::isDimension(cit->second.type)) {
            bool driven = cit->second.driven;
            if (ImGui::Checkbox("Driven", &driven)) sketchSetDriven(cit->first, driven);
            ImGui::SetItemTooltip("Reports the length and does not drive it");
        }
    }
    if (s->dof == 0) ImGui::TextColored(ImVec4(0.1f, 0.55f, 0.25f, 1.f), "Fully constrained");
    else if (s->dof > 0) ImGui::TextDisabled("%d degrees of freedom", s->dof);
    else ImGui::TextDisabled("Not solved yet");
    ImGui::TextDisabled("%d curves, %d constraints", s->geometryCount(), (int)s->cons.size());
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.63f, 0.35f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.70f, 0.40f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("FINISH SKETCH", ImVec2(-1, 28 * S))) finishSketch();
    ImGui::PopStyleColor(3);
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace kst
