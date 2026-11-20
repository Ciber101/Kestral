// Sketch curves in the measure tool: the lines, arcs, circles, ellipses and splines of every visible
// sketch can be picked like body edges (Fusion lets you measure to sketch geometry too).
#include "app/App.h"
#include "core/Curves.h"
#include "kernel/Features.h"

#include <imgui.h>

#include <cmath>

namespace kst {

namespace {
constexpr double kTau = 6.283185307179586;
}

bool App::sketchCurveGeom(uint32_t sid, uint32_t eid, std::vector<glm::dvec3>& poly, std::vector<CurveSeg>* segs, glm::dvec3* normal) const {
    poly.clear();
    if (segs) segs->clear();
    auto sit = doc_.sketches.find(sid);
    if (sit == doc_.sketches.end()) return false;
    const sk::Sketch& s = sit->second;
    auto eit = s.ents.find(eid);
    if (eit == s.ents.end()) return false;
    const sk::Entity& e = eit->second;
    const sk::Plane& pl = s.plane;
    glm::dvec3 n = glm::normalize(pl.normal());
    if (normal) *normal = n;
    auto W = [&](const sk::Vec2& p) { return pl.toWorld(p); };
    std::vector<sk::Vec2> pts2;
    CurveSeg one;
    bool exact = true;
    switch (e.type) {
    case sk::EntType::Line: {
        sk::Vec2 a = s.pt(e.a), b = s.pt(e.b);
        pts2 = {a, b};
        one.kind = CurveSeg::Kind::Line;
        one.a = W(a);
        one.b = W(b);
        break;
    }
    case sk::EntType::Circle: {
        sk::Vec2 c = s.pt(e.c);
        for (int i = 0; i <= 96; ++i) {
            double t = kTau * i / 96;
            pts2.push_back(c + sk::Vec2(std::cos(t) * e.r, std::sin(t) * e.r));
        }
        one.kind = CurveSeg::Kind::Circle;
        one.center = W(c);
        one.radius = e.r;
        one.a = W(c + sk::Vec2(e.r, 0));
        one.normal = n;
        break;
    }
    case sk::EntType::Arc: {
        sk::Vec2 c = s.pt(e.c), a = s.pt(e.a), b = s.pt(e.b);
        double r = std::hypot(a.x - c.x, a.y - c.y);
        double t0 = std::atan2(a.y - c.y, a.x - c.x), t1 = std::atan2(b.y - c.y, b.x - c.x);
        double sw = t1 - t0;
        while (sw <= 0) sw += kTau;   // counter clockwise from a to b
        int nseg = std::max(8, (int)(sw / kTau * 96));
        for (int i = 0; i <= nseg; ++i) {
            double t = t0 + sw * i / nseg;
            pts2.push_back(c + sk::Vec2(std::cos(t) * r, std::sin(t) * r));
        }
        one.kind = CurveSeg::Kind::Arc;
        one.center = W(c);
        one.a = W(a);
        one.b = W(b);
        one.radius = r;
        one.normal = n;
        break;
    }
    case sk::EntType::Ellipse:
    case sk::EntType::EllipArc: {
        sk::Sketch::EllipseGeom g;
        if (!s.ellipseGeom(e, g)) return false;
        for (int i = 0; i <= 96; ++i) pts2.push_back(s.ellipsePoint(g, (double)i / 96));
        bool full = e.type == sk::EntType::Ellipse;
        one.kind = full ? CurveSeg::Kind::Ellipse : CurveSeg::Kind::EllipArc;
        one.center = W(g.c);
        one.majorDir = pl.x * std::cos(g.rot) + pl.y * std::sin(g.rot);
        one.radius = g.major;
        one.minor = g.minor;
        one.ang0 = g.a0;
        one.sweep = g.sweep;
        one.normal = n;
        break;
    }
    case sk::EntType::Spline: pts2 = s.splinePoly(e, 12); exact = false; break;
    case sk::EntType::CtrlSpline: pts2 = s.ctrlSplinePoly(e, 12); exact = false; break;
    default: return false;
    }
    for (auto& p : pts2) poly.push_back(W(p));
    if (poly.size() < 2) return false;
    if (segs) {
        if (exact) segs->push_back(one);
        else
            for (size_t i = 1; i < poly.size(); ++i) {
                CurveSeg c;
                c.kind = CurveSeg::Kind::Line;
                c.a = poly[i - 1];
                c.b = poly[i];
                segs->push_back(c);
            }
    }
    return true;
}

bool App::sketchCurveAt(float mx, float my, uint32_t& sid, uint32_t& eid, float& dist) {
    sid = eid = 0;
    dist = 1e9f;
    const float radius = 9.f * uiScale_;
    auto segDist = [](float px, float py, ImVec2 a, ImVec2 b) {
        float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        float u = l2 > 1e-6f ? std::clamp(((px - a.x) * dx + (py - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
        float qx = a.x + dx * u - px, qy = a.y + dy * u - py;
        return std::sqrt(qx * qx + qy * qy);
    };
    std::vector<glm::dvec3> poly;
    for (auto& [id, s] : doc_.sketches) {
        if (!s.visible || !doc_.isFeatureActive(s.featureId)) continue;
        for (auto& [ei, e] : s.ents) {
            if (e.type == sk::EntType::Point || e.locked) continue;
            if (!sketchCurveGeom(id, ei, poly, nullptr, nullptr)) continue;
            ImVec2 prev;
            bool have = false;
            for (auto& w : poly) {
                ImVec2 q;
                if (!worldToScreen(w, q)) { have = false; continue; }
                if (have) {
                    float d = segDist(mx, my, prev, q);
                    if (d < dist) { dist = d; sid = id; eid = ei; }
                }
                prev = q;
                have = true;
            }
        }
    }
    if (dist > radius) { sid = eid = 0; return false; }
    return true;
}

// A sketch point (corner, end, center, the origin) under the cursor: measured to as a point.
bool App::sketchPointUnder(float mx, float my, uint32_t& sid, uint32_t& eid, float& dist) {
    sid = eid = 0;
    dist = 1e9f;
    const float radius = 9.f * uiScale_;
    for (auto& [id, s] : doc_.sketches) {
        if (!s.visible || !doc_.isFeatureActive(s.featureId)) continue;
        for (auto& [ei, e] : s.ents) {
            if (e.type != sk::EntType::Point) continue;
            if (e.locked && ei != sk::Sketch::kOrigin) continue;
            ImVec2 q;
            if (!worldToScreen(s.plane.toWorld(s.pt(ei)), q)) continue;
            float d = std::hypot(q.x - mx, q.y - my);
            if (d < dist) { dist = d; sid = id; eid = ei; }
        }
    }
    if (dist > radius) { sid = eid = 0; return false; }
    return true;
}

bool App::sketchPointWorld(uint32_t sid, uint32_t eid, glm::dvec3& w) const {
    auto sit = doc_.sketches.find(sid);
    if (sit == doc_.sketches.end()) return false;
    auto eit = sit->second.ents.find(eid);
    if (eit == sit->second.ents.end() || eit->second.type != sk::EntType::Point) return false;
    w = sit->second.plane.toWorld(sit->second.pt(eid));
    return true;
}

void App::drawSketchCurve(uint32_t sid, uint32_t eid, unsigned int col, float width) {
    if (glm::dvec3 w; sketchPointWorld(sid, eid, w)) {
        ImVec2 q;
        if (worldToScreen(w, q)) {
            ImDrawList* pdl = ImGui::GetForegroundDrawList();
            pdl->AddCircleFilled(q, width * 1.6f, col);
            pdl->AddCircle(q, width * 2.6f, col, 16, width * 0.6f);
        }
        return;
    }
    std::vector<glm::dvec3> poly;
    if (!sketchCurveGeom(sid, eid, poly, nullptr, nullptr)) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 prev;
    bool have = false;
    for (auto& w : poly) {
        ImVec2 q;
        if (!worldToScreen(w, q)) { have = false; continue; }
        if (have) dl->AddLine(prev, q, col, width);
        prev = q;
        have = true;
    }
}

} // namespace kst
