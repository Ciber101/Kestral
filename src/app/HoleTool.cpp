// Hole tool (H): no sketch needed. Click a face and a hole goes right there, snapping to the grid,
// to corners, edge middles, circle centers and the face center. Click more spots for more holes;
// H again (or Enter) finishes, Esc cancels. Simple, counterbore and countersink holes.
#include "app/App.h"
#include "kernel/Features.h"
#include "kernel/ShapeHandle.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kst {

using namespace ui;

bool App::holeMode() const {
    return holePlace_.active && tdlg_.open && tdlg_.type == FeatureType::Hole;
}

void App::startHolePlacement() {
    if (sk_.active) finishSketch();
    clearSelection();
    startToolFeature(FeatureType::Hole);
    if (!tdlg_.open || tdlg_.type != FeatureType::Hole) return;
    holePlace_.active = true;
    holePlace_.valid = false;
    activePickFilter_ = PickFace;
    std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "5");
    tdlg_.v2[0] = 0;   // through
    tdlg_.option = 0;
    applyToolDialog();
    toast("Click faces to place holes. H or Enter when done.");
}

namespace {

std::string fmtT(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Two directions across a plane, lined up with the world axes where possible (so a grid on a
// top face runs along X and Y like the ground grid).
void planeAxes(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v) {
    glm::dvec3 an = glm::abs(n);
    if (an.z >= an.x && an.z >= an.y) u = {1, 0, 0};
    else if (an.y >= an.x) u = {1, 0, 0};
    else u = {0, 1, 0};
    u = glm::normalize(u - n * glm::dot(u, n));
    v = glm::normalize(glm::cross(n, u));
}

} // namespace

void App::updateHolePlacement() {
    holePlace_.valid = false;
    if (!holeMode() || hov_.kind != HoverKind::Face || !viewportHovered_) return;
    uint32_t bodyId = hov_.body;
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return;
    ImVec2 m = ImGui::GetIO().MousePos;
    glm::dvec3 hit;
    if (!hitPoint(bodyId, hov_.index + 1, m, hit)) return;
    glm::dmat4 xf(bit->second.xform);
    glm::dvec3 c, n;
    bool planar = false;
    if (!kernel::faceInfo(pit->second.shape, hov_.index, c, n, planar)) return;
    c = glm::dvec3(xf * glm::dvec4(c, 1.0));
    n = glm::normalize(glm::dvec3(xf * glm::dvec4(n, 0.0)));
    glm::dvec3 pos = hit;
    std::string snap;
    if (planar) {
        pos = hit - n * glm::dot(hit - c, n);   // on the face's plane exactly
        const float S = uiScale_;
        float best = 14.f * S;
        auto consider = [&](const glm::dvec3& w, const char* name) {
            ImVec2 q;
            if (!worldToScreen(w, q)) return;
            float d = std::hypot(q.x - m.x, q.y - m.y);
            if (d < best) { best = d; pos = w; snap = name; }
        };
        if (holePlace_.snapEdges) {
            consider(c, "Face center");
            if (const auto* polys = bodyEdges(bodyId)) {
                double tol = std::max(1e-3, kernel::shapeDiag(pit->second.shape) * 1e-5);
                for (auto& pl : *polys) {
                    if (pl.size() < 2) continue;
                    std::vector<glm::dvec3> w;
                    w.reserve(pl.size());
                    bool onFace = true;
                    for (auto& p : pl) {
                        glm::dvec3 q = glm::dvec3(xf * glm::dvec4(p, 1.0));
                        if (std::abs(glm::dot(q - c, n)) > tol * 10) { onFace = false; break; }
                        w.push_back(q);
                    }
                    if (!onFace) continue;
                    bool closed = glm::length(w.front() - w.back()) < tol * 10;
                    if (closed) {
                        // A round edge (hole, boss): its center.
                        glm::dvec3 ctr(0);
                        for (size_t i = 0; i + 1 < w.size(); ++i) ctr += w[i];
                        ctr /= (double)std::max<size_t>(1, w.size() - 1);
                        consider(ctr, "Circle center");
                        continue;
                    }
                    consider(w.front(), "Corner");
                    consider(w.back(), "Corner");
                    // Middle of the edge by length.
                    double len = 0;
                    for (size_t i = 1; i < w.size(); ++i) len += glm::length(w[i] - w[i - 1]);
                    double half = len * 0.5, acc = 0;
                    for (size_t i = 1; i < w.size(); ++i) {
                        double s = glm::length(w[i] - w[i - 1]);
                        if (acc + s >= half && s > 0) { consider(glm::mix(w[i - 1], w[i], (half - acc) / s), "Edge middle"); break; }
                        acc += s;
                    }
                }
            }
        }
        if (snap.empty() && holePlace_.snapGrid && holePlace_.grid > 0) {
            // Grid on the face's plane, lined up with the world origin.
            glm::dvec3 u, v;
            planeAxes(n, u, v);
            glm::dvec3 base = n * glm::dot(c, n);   // the world origin dropped onto the plane
            double g = holePlace_.grid;
            double a = std::round(glm::dot(pos - base, u) / g) * g, b = std::round(glm::dot(pos - base, v) / g) * g;
            pos = base + u * a + v * b;
            snap = "Grid";
        }
    }
    holePlace_.valid = true;
    holePlace_.pos = pos;
    holePlace_.dir = -n;   // into the material
    holePlace_.snapName = snap;
    activity();
}

void App::holePlaceClick() {
    if (!holePlace_.valid) return;
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (fi < 0 || !doc_.timeline[fi].hole) return;
    auto hp = std::make_shared<HoleParams>(*doc_.timeline[fi].hole);
    hp->points.push_back({holePlace_.pos, holePlace_.dir});
    doc_.timeline[fi].hole = hp;
    doc_.touch();
    dirty_ = true;
    applyToolDialog();
}

void App::drawHoleDialogPlacement() {
    int fi = doc_.featureIndex(tdlg_.featureId);
    size_t n = fi >= 0 && doc_.timeline[fi].hole ? doc_.timeline[fi].hole->points.size() : 0;
    ImGui::TextColored(n ? ImVec4(0.2f, 0.6f, 0.3f, 1) : ImVec4(0.12f, 0.43f, 0.9f, 1),
                       n ? "%zu hole%s placed. Click more faces to add." : "Click a face to place a hole.", n, n == 1 ? "" : "s");
    ImGui::Checkbox("Snap to grid", &holePlace_.snapGrid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * uiScale_);
    ImGui::InputFloat("mm##hgrid", &holePlace_.grid, 0, 0, "%.2f");
    holePlace_.grid = std::clamp(holePlace_.grid, 0.01f, 1000.f);
    ImGui::Checkbox("Snap to corners, edge middles and centers", &holePlace_.snapEdges);
    if (n > 0 && ImGui::SmallButton("Remove the last hole")) {
        auto hp = std::make_shared<HoleParams>(*doc_.timeline[fi].hole);
        hp->points.pop_back();
        doc_.timeline[fi].hole = hp;
        doc_.touch();
        applyToolDialog();
    }
    ImGui::Separator();
}

void App::drawHolePlacement(ImDrawList* dl) {
    if (!holeMode()) return;
    const float S = uiScale_;
    double r = 2.5;
    try { if (tdlg_.v1[0]) r = std::max(0.05, peval::eval(tdlg_.v1, doc_.params) * 0.5); } catch (const std::runtime_error&) {}
    auto ring = [&](const glm::dvec3& c, const glm::dvec3& d, ImU32 col, float th) {
        glm::dvec3 u, v;
        planeAxes(glm::normalize(d), u, v);
        ImVec2 prev;
        bool have = false;
        for (int i = 0; i <= 40; ++i) {
            double t = i / 40.0 * 6.283185307;
            ImVec2 q;
            if (!worldToScreen(c + (u * std::cos(t) + v * std::sin(t)) * r, q)) { have = false; continue; }
            if (have) dl->AddLine(prev, q, col, th);
            prev = q;
            have = true;
        }
    };
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (fi >= 0 && doc_.timeline[fi].hole)
        for (auto& p : doc_.timeline[fi].hole->points) ring(p.pos, p.dir, IM_COL32(235, 95, 40, 255), 2.f * S);
    if (!holePlace_.valid) return;
    ring(holePlace_.pos, holePlace_.dir, IM_COL32(255, 120, 40, 255), 2.f * S);
    ImVec2 q;
    if (worldToScreen(holePlace_.pos, q)) {
        dl->AddLine(ImVec2(q.x - 6 * S, q.y), ImVec2(q.x + 6 * S, q.y), IM_COL32(255, 120, 40, 255), 1.5f * S);
        dl->AddLine(ImVec2(q.x, q.y - 6 * S), ImVec2(q.x, q.y + 6 * S), IM_COL32(255, 120, 40, 255), 1.5f * S);
        if (!holePlace_.snapName.empty()) {
            ImVec2 tp(q.x + 12 * S, q.y + 10 * S);
            ImVec2 ts = ImGui::CalcTextSize(holePlace_.snapName.c_str());
            dl->AddRectFilled(ImVec2(tp.x - 4 * S, tp.y - 2 * S), ImVec2(tp.x + ts.x + 4 * S, tp.y + ts.y + 2 * S), col::Card, 3 * S);
            dl->AddText(tp, col::Text, holePlace_.snapName.c_str());
        }
    }
}

std::string App::addPlacedHoles(const std::vector<std::pair<glm::dvec3, glm::dvec3>>& pts, double dia, double depth, int kind,
                                double extraDia, double extra, const std::vector<TargetRef>& only,
                                double spotDia, double spotDepth, double drillAngle, bool tapped, double pitch) {
    if (pts.empty()) return "No hole positions given.";
    if (!(dia > 0)) return "The diameter must be greater than zero.";
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    clearSelection();
    startToolFeature(FeatureType::Hole);
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (!tdlg_.open || fi < 0 || !doc_.timeline[fi].hole) return "Could not start Hole.";
    auto hp = std::make_shared<HoleParams>(*doc_.timeline[fi].hole);
    for (auto& [p, d] : pts) hp->points.push_back({p, glm::length(d) > 1e-12 ? glm::normalize(d) : glm::dvec3(0, 0, -1)});
    hp->targets = only;
    doc_.timeline[fi].hole = hp;
    std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", fmtT(dia).c_str());
    if (depth > 0) std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", fmtT(depth).c_str());
    else tdlg_.v2[0] = 0;
    tdlg_.option = std::clamp(kind, 0, 2);
    if (kind == 1) {
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", fmtT(extraDia > dia ? extraDia : dia * 1.8).c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", fmtT(extra > 0 ? extra : dia * 0.6).c_str());
    } else if (kind == 2) {
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", fmtT(extraDia > dia ? extraDia : dia * 2).c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", fmtT(extra > 0 ? extra : 90).c_str());
    }
    tdlg_.v5[0] = tdlg_.v6[0] = tdlg_.v7[0] = tdlg_.v8[0] = 0;
    if (spotDia > dia && spotDepth > 0) {
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", fmtT(spotDia).c_str());
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", fmtT(spotDepth).c_str());
    }
    if (drillAngle > 1 && depth > 0) std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%s", fmtT(drillAngle).c_str());
    tdlg_.flag = tapped && pitch > 0;
    if (tdlg_.flag) std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%s", fmtT(pitch).c_str());
    applyToolDialog();
    closeToolDialog(true);
    std::string msg = "Added a Hole step for " + std::to_string(pts.size()) + (pts.size() == 1 ? " hole" : " holes");
    if (!only.empty()) msg += " cutting only the named bodies";
    msg += ". If a timeline error is appended, the cut did not happen.";
    return msg;
}

} // namespace kst
