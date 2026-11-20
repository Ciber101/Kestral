// Fillet, chamfer, shell, press pull, patterns, mirror, combine, split, hole, move, remove, sweep,
// loft and construction planes. Every command works the Fusion way: whatever is selected when you
// start it is used, and clicking in the canvas while its dialog is open adds or removes picks.
// Fillets never crash: the kernel falls back edge by edge and reports the maximum radius.
#include "app/App.h"
#include "app/MechTools.h"
#include "core/Log.h"
#include "kernel/Features.h"
#include "platform/Platform.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kst {

namespace {

std::string fmtT(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

const char* featureTitle(FeatureType t) {
    switch (t) {
    case FeatureType::Fillet: return "FILLET###tooldlg";
    case FeatureType::Chamfer: return "CHAMFER###tooldlg";
    case FeatureType::Shell: return "SHELL###tooldlg";
    case FeatureType::PressPull: return "PRESS PULL###tooldlg";
    case FeatureType::Pattern: return "PATTERN###tooldlg";
    case FeatureType::Mirror: return "MIRROR###tooldlg";
    case FeatureType::Combine: return "COMBINE###tooldlg";
    case FeatureType::Split: return "SPLIT BODY###tooldlg";
    case FeatureType::SplitFace: return "SPLIT FACE###tooldlg";
    case FeatureType::Hole: return "HOLE###tooldlg";
    case FeatureType::Sweep: return "SWEEP###tooldlg";
    case FeatureType::Loft: return "LOFT###tooldlg";
    case FeatureType::Move: return "MOVE/COPY###tooldlg";
    case FeatureType::ConstructPlane: return "OFFSET PLANE###tooldlg";
    case FeatureType::ConstructAxis: return "AXIS###tooldlg";
    case FeatureType::ConstructPoint: return "POINT###tooldlg";
    case FeatureType::Remove: return "REMOVE###tooldlg";
    case FeatureType::Gear: return "GEAR###tooldlg";
    case FeatureType::Thread: return "THREAD###tooldlg";
    case FeatureType::Wing: return "WING###tooldlg";
    case FeatureType::Dovetail: return "DOVETAIL###tooldlg";
    case FeatureType::Blade: return "BLADES###tooldlg";
    case FeatureType::Align: return "ALIGN###tooldlg";
    case FeatureType::Draft: return "DRAFT###tooldlg";
    case FeatureType::Coil: return "COIL###tooldlg";
    case FeatureType::Pipe: return "PIPE###tooldlg";
    case FeatureType::ReplaceFace: return "REPLACE FACE###tooldlg";
    case FeatureType::Thicken: return "THICKEN###tooldlg";
    default: return "FEATURE###tooldlg";
    }
}

int kindOfType(FeatureType t) {
    if (t == FeatureType::Fillet) return 0;
    if (t == FeatureType::Chamfer) return 1;
    if (t == FeatureType::Shell) return 2;
    if (t == FeatureType::PressPull) return 3;
    if (t == FeatureType::Draft) return 4;
    if (t == FeatureType::ReplaceFace) return 5;
    if (t == FeatureType::Thicken) return 6;
    return -1;
}

const char* baseName(FeatureType t) {
    switch (t) {
    case FeatureType::Fillet: return "Fillet";
    case FeatureType::Chamfer: return "Chamfer";
    case FeatureType::Shell: return "Shell";
    case FeatureType::PressPull: return "PressPull";
    case FeatureType::Pattern: return "Pattern";
    case FeatureType::Mirror: return "Mirror";
    case FeatureType::Combine: return "Combine";
    case FeatureType::Split: return "Split";
    case FeatureType::SplitFace: return "SplitFace";
    case FeatureType::Hole: return "Hole";
    case FeatureType::Sweep: return "Sweep";
    case FeatureType::Loft: return "Loft";
    case FeatureType::Move: return "Move";
    case FeatureType::Remove: return "Remove";
    case FeatureType::Gear: return "Gear";
    case FeatureType::Thread: return "Thread";
    case FeatureType::Wing: return "Wing";
    case FeatureType::Dovetail: return "Dovetail";
    case FeatureType::Blade: return "Blades";
    case FeatureType::Align: return "Align";
    case FeatureType::ConstructPlane: return "Plane";
    case FeatureType::ConstructAxis: return "Axis";
    case FeatureType::ConstructPoint: return "Point";
    case FeatureType::Draft: return "Draft";
    case FeatureType::Coil: return "Coil";
    case FeatureType::Pipe: return "Pipe";
    case FeatureType::Rib: return "Rib";
    case FeatureType::Web: return "Web";
    case FeatureType::Emboss: return "Emboss";
    case FeatureType::Boss: return "Boss";
    case FeatureType::Snap: return "Snap";
    case FeatureType::Rest: return "Rest";
    case FeatureType::Lip: return "Lip";
    case FeatureType::Silhouette: return "Silhouette";
    case FeatureType::Joint: return "Joint";
    case FeatureType::AsBuilt: return "AsBuilt";
    case FeatureType::Rigid: return "Rigid";
    case FeatureType::ReplaceFace: return "ReplaceFace";
    case FeatureType::Thicken: return "Thicken";
    default: return "Feature";
    }
}

std::string defaultName(const Document& doc, FeatureType t) {
    int n = 0;
    for (auto& ft : doc.timeline)
        if (ft.type == t) ++n;
    return std::string(baseName(t)) + std::to_string(n + 1);
}

// What a pattern's first box picks: bodies, or faces when it copies faces. A circular pattern also
// takes an edge for its axis straight away.
int patternObjectFilter(bool faces, int kind) {
    if (faces) return 2;        // App::PickFace: a hole's edge would only get in the way
    int f = 1;                  // App::PickBody
    if (kind == 1) f |= 4;      // App::PickEdge (a circular pattern takes it as the axis)
    return f;
}

bool bodyPickTool(FeatureType t) {
    return t == FeatureType::Pattern || t == FeatureType::Mirror || t == FeatureType::Combine ||
           t == FeatureType::Split || t == FeatureType::SplitFace || t == FeatureType::Move || t == FeatureType::Remove || t == FeatureType::Dovetail ||
           t == FeatureType::Joint || t == FeatureType::AsBuilt || t == FeatureType::Rigid;
}

} // namespace

// ===================================================================== selection helpers

TargetRef App::refOfBody(uint32_t bodyId) const {
    auto it = doc_.bodies.find(bodyId);
    if (it == doc_.bodies.end() || it->second.featureId == 0) return {0, 0};
    return {it->second.featureId, doc_.bodySlot(bodyId)};
}

std::vector<std::pair<uint32_t, uint32_t>> App::selectionRefs() const {
    std::vector<TargetRef> out;
    for (uint32_t id : selectedBodies()) {
        TargetRef r = refOfBody(id);
        if (r.first == 0) continue;
        if (tdlg_.open && r.first == tdlg_.featureId) continue;   // the command's own preview
        out.push_back(r);
    }
    return out;
}

// Rebuilds the open command's inputs from the canvas selection, then recomputes the preview.
void App::syncToolFromSelection() {
    if (!tdlg_.open) return;
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (fi < 0) return;
    FeatureType t = tdlg_.type;
    bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && viewportHovered_;
    auto distSeg = [](ImVec2 p, ImVec2 a, ImVec2 b) {
        ImVec2 ab(b.x - a.x, b.y - a.y);
        float l2 = ab.x * ab.x + ab.y * ab.y;
        float u = l2 < 1e-6f ? 0.f : std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2, 0.f, 1.f);
        return std::hypot(p.x - (a.x + ab.x * u), p.y - (a.y + ab.y * u));
    };
    if (t == FeatureType::Align) {
        // The click arrives here on mouse release (viewportClick), so IsMouseClicked has already passed.
        bool clicked = !winSel_ && viewportHovered_ &&
                       (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left));
        AlignSnap snapPick;
        if (clicked && alignSnapAt(snapPick, nullptr)) {
            {
                AlignParams::Pick pk = snapPick.pick;
                std::string label = snapPick.label;
                if (!label.empty()) {
                    AlignParams& a = tdlg_.align;
                    if (!a.from.ref.first) {
                        a.from = pk;
                        a.fromName = label;
                        alignFromWorld_ = snapPick.world;
                        alignFromShown_ = true;
                        if (std::find(a.targets.begin(), a.targets.end(), pk.ref) == a.targets.end()) a.targets.insert(a.targets.begin(), pk.ref);
                        toast("Now click the circle or face it should land on");
                    } else if (pk.ref == a.from.ref) {
                        toast("Pick the destination on the body that stays where it is", ToastKind::Warning);
                    } else {
                        a.to = pk;
                        a.toName = label;
                        alignFromShown_ = false;
                        a.targets.erase(std::remove(a.targets.begin(), a.targets.end(), pk.ref), a.targets.end());
                        if (a.targets.empty()) a.targets.push_back(a.from.ref);
                    }
                    selFaces_.clear();
                    selEdges_.clear();
                }
            }
        }
        applyToolDialog();
        return;
    }
    if (t == FeatureType::Loft) {
        syncLoftPicks();
        applyToolDialog();
        return;
    }
    if (t == FeatureType::Split && tdlg_.option2 == 1) {
        // Picks arrive from viewportClick on mouse release, so the press frame has passed.
        bool clk = (clicked || ImGui::IsMouseReleased(ImGuiMouseButton_Left)) && viewportHovered_;
        if (clk) {
            ImVec2 m = ImGui::GetIO().MousePos;
            uint32_t sid = 0, ent = 0;
            float best = 9.f * uiScale_;
            for (auto& [id, s] : doc_.sketches) {
                if (!sketchShown(s)) continue;
                for (auto& [eid, e] : s.ents) {
                    if (e.type != sk::EntType::Line) continue;
                    ImVec2 sa, sb;
                    if (!worldToScreen(s.plane.toWorld(s.pt(e.a)), sa) || !worldToScreen(s.plane.toWorld(s.pt(e.b)), sb)) continue;
                    float d = distSeg(m, sa, sb);
                    if (d < best) { best = d; sid = id; ent = eid; }
                }
            }
            bool took = false;
            if (sid) {
                const sk::Sketch& s = doc_.sketches.at(sid);
                const sk::Entity& e = s.ents.at(ent);
                glm::dvec3 a = s.plane.toWorld(s.pt(e.a)), b = s.plane.toWorld(s.pt(e.b)), dir = b - a;
                glm::dvec3 nrm = glm::length(dir) > 1e-9 ? glm::cross(glm::normalize(dir), s.plane.normal()) : glm::dvec3(0);
                if (glm::length(nrm) < 1e-9) toast("That line cannot define a cut", ToastKind::Warning);
                else {
                    tdlg_.option = 3;
                    tdlg_.axisO = a;
                    tdlg_.axisD = glm::normalize(nrm);
                    tdlg_.axisName = "Sketch line";
                    tdlg_.selA = (int)sid;
                    tdlg_.selB = (int)ent;
                    tdlg_.axisR = 0;
                    took = true;
                }
            } else if (!selFaces_.empty()) {
                SubRef fc = *selFaces_.begin();
                auto bit = doc_.bodies.find(fc.first);
                auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
                glm::dvec3 c, n;
                bool planar = false;
                double rad = 0;
                if (pit != doc_.parts.end() && pit->second.shape && kernel::faceInfo(pit->second.shape, fc.second, c, n, planar) && planar) {
                    glm::dmat4 xf(bit->second.xform);
                    tdlg_.option = 3;
                    tdlg_.axisO = glm::dvec3(xf * glm::dvec4(c, 1.0));
                    tdlg_.axisD = glm::normalize(glm::dmat3(xf) * n);
                    tdlg_.axisR = 0;
                    tdlg_.axisName = "Face of " + bit->second.name;
                    tdlg_.selA = tdlg_.selB = 0;
                    took = true;
                } else if (pit != doc_.parts.end() && pit->second.shape && kernel::faceCylinder(pit->second.shape, fc.second, c, n, rad)) {
                    // A round face: the cut follows that cylinder, carried on past the face's own ends
                    // (Fusion's Split Body with a curved face), e.g. a liner cut free of its water jacket.
                    glm::dmat4 xf(bit->second.xform);
                    tdlg_.option = 3;
                    tdlg_.axisO = glm::dvec3(xf * glm::dvec4(c, 1.0));
                    tdlg_.axisD = glm::normalize(glm::dmat3(xf) * n);
                    tdlg_.axisR = rad;
                    char nm[96];
                    std::snprintf(nm, sizeof nm, "Round face of %s (R %.2f)", bit->second.name.c_str(), rad);
                    tdlg_.axisName = nm;
                    tdlg_.selA = tdlg_.selB = 0;
                    took = true;
                } else toast("Pick a flat or round (cylinder) face to split with", ToastKind::Warning);
            } else if (!selEdges_.empty()) {
                SubRef ed = *selEdges_.begin();
                auto bit = doc_.bodies.find(ed.first);
                auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
                kernel::EdgeGeom g;
                if (pit != doc_.parts.end() && pit->second.shape && kernel::edgeGeom(pit->second.shape, bit->second.xform, ed.second, g) && g.type == 0 &&
                    glm::length(g.b - g.a) > 1e-9) {
                    glm::dvec3 dir = glm::normalize(g.b - g.a), fn(0);
                    bool got = false;
                    int nf = (int)kernel::faceCount(pit->second.shape);
                    for (int fi = 0; fi < nf && !got; ++fi)
                        for (uint32_t k : kernel::faceEdges(pit->second.shape, (uint32_t)fi))
                            if (k == ed.second) {
                                glm::dvec3 c, n;
                                bool planar = false;
                                if (kernel::faceInfo(pit->second.shape, (uint32_t)fi, c, n, planar) && planar) {
                                    fn = glm::normalize(glm::dmat3(bit->second.xform) * n);
                                    got = true;
                                }
                            }
                    glm::dvec3 sn = got ? glm::cross(dir, fn) : glm::cross(dir, std::abs(dir.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0));
                    if (glm::length(sn) < 1e-8) toast("That edge cannot define a cut", ToastKind::Warning);
                    else {
                        tdlg_.option = 3;
                        tdlg_.axisO = (g.a + g.b) * 0.5;
                        tdlg_.axisD = glm::normalize(sn);
                        tdlg_.axisName = "Edge of " + bit->second.name;
                        tdlg_.selA = tdlg_.selB = 0;
                        tdlg_.axisR = 0;
                        took = true;
                    }
                } else toast("Pick a straight edge to split along", ToastKind::Warning);
            }
            if (took) {
                tdlg_.option2 = 0;
                activePickFilter_ = PickBody;
                selFaces_.clear();
                selEdges_.clear();
            }
        }
        applyToolDialog();
        return;
    }
    if (t == FeatureType::Pattern && tdlg_.option2 == 1 && tdlg_.pickWait &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) && viewportHovered_) {
        // A sketch circle or arc (its center, square to its sketch) or a sketch line as the axis.
        ImVec2 m = ImGui::GetIO().MousePos;
        float best = 8.f * uiScale_;
        bool hit = false;
        glm::dvec3 ho(0), hd(0, 0, 1);
        std::string hname;
        for (auto& [id, s] : doc_.sketches) {
            if (!sketchShown(s)) continue;
            for (auto& [eid, e] : s.ents) {
                if (e.type != sk::EntType::Line && e.type != sk::EntType::Circle && e.type != sk::EntType::Arc) continue;
                if (e.locked) continue;
                std::vector<glm::dvec3> w;
                auto W = [&](sk::Vec2 p) { return s.plane.toWorld(p); };
                if (e.type == sk::EntType::Line) {
                    w = {W(s.pt(e.a)), W(s.pt(e.b))};
                } else {
                    sk::Sketch::ArcGeom g;
                    if (!s.arcGeom(e, g)) continue;
                    for (int i = 0; i <= 32; ++i) w.push_back(W(s.arcPoint(g, i / 32.0)));
                }
                for (size_t i = 1; i < w.size(); ++i) {
                    ImVec2 sa, sb;
                    if (!worldToScreen(w[i - 1], sa) || !worldToScreen(w[i], sb)) continue;
                    float d = distSeg(m, sa, sb);
                    if (d >= best) continue;
                    best = d;
                    hit = true;
                    if (e.type == sk::EntType::Line) {
                        ho = w.front();
                        hd = w.back() - w.front();
                        hname = "Sketch line";
                    } else {
                        ho = W(s.pt(e.c));
                        hd = s.plane.normal();
                        hname = e.type == sk::EntType::Circle ? "Center of a sketch circle" : "Center of a sketch arc";
                    }
                }
            }
        }
        if (hit && glm::length(hd) > 1e-9) {
            tdlg_.axisO = ho;
            tdlg_.axisD = glm::normalize(hd);
            tdlg_.option = 3;
            tdlg_.axisName = hname;
            tdlg_.pickWait = false;
            activePickFilter_ = patternObjectFilter(tdlg_.patFaces, 1);
            selFaces_ = tdlg_.patFaceSel;
            selEdges_.clear();
            applyToolDialog();
            return;
        }
    }
    if (t == FeatureType::Pattern && tdlg_.option2 == 2 && tdlg_.pickWait) {
        bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left) && viewportHovered_;
        if (released) {
            ImVec2 m = ImGui::GetIO().MousePos;
            uint32_t sid = 0, ent = 0;
            float best = 8.f * uiScale_;
            auto worldPts = [&](const sk::Sketch& s, const sk::Entity& e, std::vector<glm::dvec3>& w) {
                w.clear();
                if (e.locked || e.construction || e.type == sk::EntType::Point) return;
                auto W = [&](sk::Vec2 p) { return s.plane.toWorld(p); };
                if (e.type == sk::EntType::Line) {
                    w.push_back(W(s.pt(e.a)));
                    w.push_back(W(s.pt(e.b)));
                } else if (e.type == sk::EntType::Arc || e.type == sk::EntType::Circle) {
                    sk::Sketch::ArcGeom g;
                    if (!s.arcGeom(e, g)) return;
                    for (int i = 0; i <= 24; ++i) w.push_back(W(s.arcPoint(g, i / 24.0)));
                } else if (e.type == sk::EntType::Spline || e.type == sk::EntType::CtrlSpline) {
                    auto poly = e.type == sk::EntType::CtrlSpline ? s.ctrlSplinePoly(e, 4) : s.splinePoly(e, 4);
                    for (auto& p : poly) w.push_back(W(p));
                }
            };
            for (auto& [id, s] : doc_.sketches) {
                if (!sketchShown(s)) continue;
                for (auto& [eid, e] : s.ents) {
                    std::vector<glm::dvec3> w;
                    worldPts(s, e, w);
                    for (size_t i = 1; i < w.size(); ++i) {
                        ImVec2 sa, sb;
                        if (!worldToScreen(w[i - 1], sa) || !worldToScreen(w[i], sb)) continue;
                        float d = distSeg(m, sa, sb);
                        if (d < best) { best = d; sid = id; ent = eid; }
                    }
                }
            }
            bool took = false;
            if (sid) {
                tdlg_.selA = (int)sid;
                tdlg_.selB = (int)ent;
                tdlg_.pathBody = {0, 0};
                tdlg_.pathEdge = 0;
                tdlg_.axisName = "Sketch curve";
                took = true;
            } else if (hov_.kind == HoverKind::Edge) {
                TargetRef r = refOfBody(hov_.body);
                if (r.first && r.first != tdlg_.featureId) {
                    tdlg_.selA = tdlg_.selB = 0;
                    tdlg_.pathBody = r;
                    tdlg_.pathEdge = hov_.index;
                    auto bit = doc_.bodies.find(hov_.body);
                    tdlg_.axisName = bit == doc_.bodies.end() ? "Edge" : "Edge of " + bit->second.name;
                    took = true;
                }
            }
            if (took) {
                tdlg_.pickWait = false;
                activePickFilter_ = patternObjectFilter(tdlg_.patFaces, 2);
                selEdges_.clear();
                selFaces_ = tdlg_.patFaceSel;
            } else toast("Pick a sketch curve or an edge", ToastKind::Warning);
        }
        applyToolDialog();
        return;
    }
    if (t == FeatureType::Rib || t == FeatureType::Web || t == FeatureType::Emboss ||
        t == FeatureType::Boss || t == FeatureType::Snap || t == FeatureType::Rest || t == FeatureType::Lip ||
        t == FeatureType::Silhouette) {
        tdlg_.targets = selectionRefs();
    }
    int kind = kindOfType(t);
    if (kind >= 0) {
        std::vector<TargetRef> targets;
        std::vector<std::pair<uint32_t, uint32_t>> edges, faces;
        auto targetIndex = [&](uint32_t bodyId) -> int {
            TargetRef r = refOfBody(bodyId);
            if (r.first == 0 || r.first == tdlg_.featureId) return -1;
            for (size_t i = 0; i < targets.size(); ++i)
                if (targets[i] == r) return (int)i;
            targets.push_back(r);
            return (int)targets.size() - 1;
        };
        auto shapeOf = [&](uint32_t bodyId) -> ShapePtr {
            auto bit = doc_.bodies.find(bodyId);
            if (bit == doc_.bodies.end()) return nullptr;
            auto pit = doc_.parts.find(bit->second.partId);
            return pit == doc_.parts.end() ? nullptr : pit->second.shape;
        };
        if (kind <= 1 && !(t == FeatureType::Fillet && tdlg_.option2 == 1)) {
            for (auto& [b, e] : selEdges_) {
                int ti = targetIndex(b);
                if (ti >= 0) edges.push_back({(uint32_t)ti, e});
            }
            // A picked face fillets all of its edges (Fusion).
            for (auto& [b, fc] : selFaces_) {
                ShapePtr sh = shapeOf(b);
                int ti = sh ? targetIndex(b) : -1;
                if (ti < 0) continue;
                for (uint32_t e : kernel::faceEdges(sh, fc))
                    if (std::find(edges.begin(), edges.end(), std::make_pair((uint32_t)ti, e)) == edges.end())
                        edges.push_back({(uint32_t)ti, e});
            }
        } else if (t == FeatureType::Fillet && tdlg_.option2 == 1) {
            if (!selFaces_.empty()) {
                SubRef fc = *selFaces_.begin();
                selFaces_ = {fc};
                int ti = targetIndex(fc.first);
                if (ti >= 0) faces.push_back({(uint32_t)ti, fc.second});
            }
        } else {
            for (auto& [b, fc] : selFaces_) {
                int ti = targetIndex(b);
                if (ti >= 0) faces.push_back({(uint32_t)ti, fc});
            }
            if (t == FeatureType::Shell && faces.empty())
                for (uint32_t b : selection_) targetIndex(b);   // closed shell of a whole body
        }
        tdlg_.targets = targets;
        tdlg_.edges = edges;
        tdlg_.faces = faces;
        tdlg_.edgeTotal = tdlg_.faceTotal = 0;
        if (targets.size() == 1) {
            std::vector<uint32_t> ids = doc_.featureBodies(targets[0].first);
            if (targets[0].second < ids.size()) {
                ShapePtr sh = shapeOf(ids[targets[0].second]);
                if (sh) {
                    tdlg_.edgeTotal = kernel::edgeCount(sh);
                    tdlg_.faceTotal = kernel::faceCount(sh);
                }
            }
        }
        // Press pull arrow on the first face.
        if (t == FeatureType::PressPull && !selFaces_.empty()) {
            auto f0 = *selFaces_.begin();
            auto bit = doc_.bodies.find(f0.first);
            ShapePtr sh = shapeOf(f0.first);
            glm::dvec3 c, n;
            bool planar;
            if (sh && bit != doc_.bodies.end() && kernel::faceInfo(sh, f0.second, c, n, planar)) {
                glm::dmat4 xf(bit->second.xform);
                double v = 0;
                try { v = peval::eval(tdlg_.v1, doc_.params); } catch (const std::runtime_error&) {}
                setManip(2, glm::dvec3(xf * glm::dvec4(c, 1.0)), glm::normalize(glm::dvec3(xf * glm::dvec4(n, 0.0))), v, planar);
            }
        }
        // Fillet / chamfer: a drag arrow on the first edge pointing into the material, like Fusion:
        // push it into the body for a bigger radius.
        if ((t == FeatureType::Fillet || t == FeatureType::Chamfer) && !manip_.dragging) {
            bool placed = false;
            if (!edges.empty() && edges[0].first < targets.size()) {
                std::vector<uint32_t> ids = doc_.featureBodies(targets[edges[0].first].first);
                uint32_t bid = targets[edges[0].first].second < ids.size() ? ids[targets[edges[0].first].second] : 0;
                auto bit = doc_.bodies.find(bid);
                ShapePtr sh = shapeOf(bid);
                glm::dvec3 mid, out;
                if (sh && bit != doc_.bodies.end() && kernel::edgeFrame(sh, edges[0].second, mid, out)) {
                    glm::dmat4 xf(bit->second.xform);
                    double v = 1;
                    try { v = peval::eval(tdlg_.v1, doc_.params); } catch (const std::runtime_error&) {}
                    setManip(2, glm::dvec3(xf * glm::dvec4(mid, 1.0)), -glm::normalize(glm::dvec3(xf * glm::dvec4(out, 0.0))), v);
                    manip_.minValue = 0.01;
                    placed = true;
                }
            }
            if (!placed) manip_.active = false;
        }
    } else if (bodyPickTool(t)) {
        // Circular pattern: a clicked edge sets the axis (Fusion lets you pick a hole's edge).
        if (t == FeatureType::Pattern && tdlg_.option2 == 1 && !selEdges_.empty()) {
            SubRef e = *selEdges_.begin();
            selEdges_.clear();
            auto bit = doc_.bodies.find(e.first);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            glm::dvec3 o, d;
            if (pit != doc_.parts.end() && pit->second.shape && kernel::edgeAxis(pit->second.shape, e.second, o, d)) {
                glm::dmat4 xf(bit->second.xform);
                tdlg_.axisO = glm::dvec3(xf * glm::dvec4(o, 1.0));
                tdlg_.axisD = glm::normalize(glm::dmat3(xf) * d);
                tdlg_.option = 3;
                tdlg_.axisName = "Picked edge";
            } else {
                toast("That edge has no axis. Pick a round edge or a straight edge.", ToastKind::Warning);
            }
            if (tdlg_.option == 3 && tdlg_.pickWait) {
                tdlg_.pickWait = false;
                activePickFilter_ = patternObjectFilter(tdlg_.patFaces, 1);
            }
        }
        // ...or a face: a round one turns around its own axis, a flat one around its normal. While
        // the axis box is waiting, the face just clicked (not one of the faces being copied) is it.
        SubRef axisFace{0, 0};
        bool haveAxisFace = false;
        if (t == FeatureType::Pattern && tdlg_.option2 == 1 && tdlg_.pickWait) {
            for (const SubRef& s : selFaces_)
                if (!tdlg_.patFaceSel.count(s)) { axisFace = s; haveAxisFace = true; break; }
            selFaces_ = tdlg_.patFaceSel;
        }
        if (t == FeatureType::Pattern && tdlg_.option2 == 1 && haveAxisFace) {
            SubRef fc = axisFace;
            auto bit = doc_.bodies.find(fc.first);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            glm::dvec3 o, d;
            bool planar = false, ok = false;
            if (pit != doc_.parts.end() && pit->second.shape) {
                ok = kernel::faceRoundAxis(pit->second.shape, fc.second, o, d);
                if (!ok) ok = kernel::faceInfo(pit->second.shape, fc.second, o, d, planar) && planar;
            }
            if (ok) {
                glm::dmat4 xf(bit->second.xform);
                tdlg_.axisO = glm::dvec3(xf * glm::dvec4(o, 1.0));
                tdlg_.axisD = glm::normalize(glm::dmat3(xf) * d);
                tdlg_.option = 3;
                tdlg_.axisName = (planar ? "Normal of a face of " : "Axis of a round face of ") + bit->second.name;
                tdlg_.pickWait = false;
                activePickFilter_ = patternObjectFilter(tdlg_.patFaces, 1);
            } else {
                toast("That face has no axis. Pick a round face (cylinder, cone, hole) or a flat face.", ToastKind::Warning);
            }
        }
        // Mirror: a clicked flat face sets the mirror plane (Fusion's Mirror Plane box).
        if (t == FeatureType::Mirror && !selFaces_.empty()) {
            SubRef fc = *selFaces_.begin();
            selFaces_.clear();
            auto bit = doc_.bodies.find(fc.first);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            glm::dvec3 c, n;
            bool planar = false;
            if (pit != doc_.parts.end() && pit->second.shape && kernel::faceInfo(pit->second.shape, fc.second, c, n, planar) && planar) {
                glm::dmat4 xf(bit->second.xform);
                tdlg_.axisO = glm::dvec3(xf * glm::dvec4(c, 1.0));
                tdlg_.axisD = glm::normalize(glm::dmat3(xf) * n);
                tdlg_.option = 3;
                tdlg_.axisName = "Face of " + bit->second.name;
                tdlg_.option2 = 0;
                tdlg_.mirrorWaitPlane = false;
                activePickFilter_ = PickBody;
            } else {
                toast("Pick a flat face for the mirror plane", ToastKind::Warning);
            }
        }
        std::vector<TargetRef> refs = selectionRefs();
        if (t == FeatureType::Combine || t == FeatureType::Dovetail || t == FeatureType::Joint || t == FeatureType::AsBuilt) {
            // Target = the first body picked, tools = the rest (Fusion's two selection boxes).
            std::vector<TargetRef> ordered;
            for (uint32_t b : selOrder_) {
                TargetRef r = refOfBody(b);
                if (r.first && std::find(refs.begin(), refs.end(), r) != refs.end() &&
                    std::find(ordered.begin(), ordered.end(), r) == ordered.end())
                    ordered.push_back(r);
            }
            for (auto& r : refs)
                if (std::find(ordered.begin(), ordered.end(), r) == ordered.end()) ordered.push_back(r);
            tdlg_.targets.clear();
            tdlg_.tools.clear();
            if (t == FeatureType::Dovetail) {
                tdlg_.targets = ordered;
                ordered.clear();
            }
            if (!ordered.empty()) tdlg_.targets.push_back(ordered[0]);
            for (size_t i = 1; i < ordered.size(); ++i) tdlg_.tools.push_back(ordered[i]);
        } else if (t == FeatureType::Pattern && tdlg_.patFaces) {
            // Faces to copy: what is picked now, unless the axis or path box has the clicks.
            if (!tdlg_.pickWait) {
                std::set<SubRef> keep;
                for (const SubRef& s : selFaces_) {
                    TargetRef r = refOfBody(s.first);
                    if (r.first && r.first != tdlg_.featureId) keep.insert(s);
                }
                tdlg_.patFaceSel = keep;
                selFaces_ = keep;
            }
            tdlg_.targets.clear();
        } else {
            tdlg_.targets = refs;
        }
    } else if (t == FeatureType::Thread) {
        syncThreadFromSelection();
    } else if (t == FeatureType::ConstructAxis && tdlg_.option == 1 && !selEdges_.empty()) {
        SubRef ed = *selEdges_.begin();
        selEdges_ = {ed};
        TargetRef r = refOfBody(ed.first);
        auto bit = doc_.bodies.find(ed.first);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        kernel::EdgeGeom g;
        if (r.first && r.first != tdlg_.featureId && pit != doc_.parts.end() && pit->second.shape &&
            kernel::edgeGeom(pit->second.shape, bit->second.xform, ed.second, g) && g.type == 0) {
            tdlg_.targets = {r};
            tdlg_.selA = (int)ed.second;
            tdlg_.edgeTotal = kernel::edgeCount(pit->second.shape);
        } else {
            toast("Pick a straight edge", ToastKind::Warning);
            selEdges_.clear();
            tdlg_.targets.clear();
        }
    } else if (t == FeatureType::ConstructAxis && tdlg_.option == 2 && !selFaces_.empty()) {
        SubRef fc = *selFaces_.begin();
        selFaces_ = {fc};
        TargetRef r = refOfBody(fc.first);
        auto bit = doc_.bodies.find(fc.first);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        kernel::RoundFace rf;
        if (r.first && r.first != tdlg_.featureId && pit != doc_.parts.end() && pit->second.shape &&
            kernel::roundFace(pit->second.shape, bit->second.xform, fc.second, rf)) {
            tdlg_.targets = {r};
            tdlg_.selA = (int)fc.second;
            tdlg_.faceTotal = kernel::faceCount(pit->second.shape);
        } else {
            toast("Pick a round face", ToastKind::Warning);
            selFaces_.clear();
            tdlg_.targets.clear();
        }
    } else if (t == FeatureType::ConstructPlane && doc_.timeline[fi].cplane && doc_.timeline[fi].cplane->kind == 4 && !selFaces_.empty()) {
        SubRef fc = *selFaces_.begin();
        selFaces_ = {fc};
        TargetRef r = refOfBody(fc.first);
        auto bit = doc_.bodies.find(fc.first);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        kernel::RoundFace rf;
        if (r.first && r.first != tdlg_.featureId && pit != doc_.parts.end() && pit->second.shape &&
            kernel::roundFace(pit->second.shape, bit->second.xform, fc.second, rf)) {
            tdlg_.targets = {r};
            tdlg_.selA = (int)fc.second;
            tdlg_.faceTotal = kernel::faceCount(pit->second.shape);
        } else {
            toast("Pick a cylinder face", ToastKind::Warning);
            selFaces_.clear();
            tdlg_.targets.clear();
        }
    } else if (t == FeatureType::ConstructPoint && tdlg_.option == 1 && !selEdges_.empty()) {
        SubRef ed = *selEdges_.begin();
        selEdges_ = {ed};
        TargetRef r = refOfBody(ed.first);
        auto bit = doc_.bodies.find(ed.first);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        kernel::EdgeGeom g;
        if (r.first && r.first != tdlg_.featureId && pit != doc_.parts.end() && pit->second.shape &&
            kernel::edgeGeom(pit->second.shape, bit->second.xform, ed.second, g)) {
            tdlg_.targets = {r};
            tdlg_.selA = (int)ed.second;
            tdlg_.edgeTotal = kernel::edgeCount(pit->second.shape);
        } else {
            toast("Pick an edge", ToastKind::Warning);
            selEdges_.clear();
            tdlg_.targets.clear();
        }
    } else if (t == FeatureType::ConstructPoint && tdlg_.option == 3 && !selEdges_.empty()) {
        SubRef ed = *selEdges_.begin();
        selEdges_ = {ed};
        TargetRef r = refOfBody(ed.first);
        auto bit = doc_.bodies.find(ed.first);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        glm::dvec3 w;
        if (r.first && r.first != tdlg_.featureId && pit != doc_.parts.end() && pit->second.shape &&
            kernel::vertexPoint(pit->second.shape, bit->second.xform, ed.second, w)) {
            tdlg_.targets = {r};
            tdlg_.selA = (int)ed.second;
            tdlg_.edgeTotal = kernel::vertexCount(pit->second.shape);
        } else {
            toast("Pick a corner", ToastKind::Warning);
            selEdges_.clear();
            tdlg_.targets.clear();
        }
    } else if (t == FeatureType::ConstructPoint && tdlg_.option == 2 && !selFaces_.empty()) {
        SubRef fc = *selFaces_.begin();
        selFaces_ = {fc};
        TargetRef r = refOfBody(fc.first);
        auto bit = doc_.bodies.find(fc.first);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        glm::dvec3 c, n;
        bool planar = false;
        if (r.first && r.first != tdlg_.featureId && pit != doc_.parts.end() && pit->second.shape &&
            kernel::faceInfo(pit->second.shape, fc.second, c, n, planar) && planar) {
            tdlg_.targets = {r};
            tdlg_.selA = (int)fc.second;
            tdlg_.faceTotal = kernel::faceCount(pit->second.shape);
        } else {
            toast("Pick a flat face", ToastKind::Warning);
            selFaces_.clear();
            tdlg_.targets.clear();
        }
    }
    applyToolDialog();
}

// ===================================================================== command start

void App::startToolFeature(FeatureType type) {
    focusFieldBuf_ = nullptr;
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    if (measureMode_) runMeasure();
    int kind = kindOfType(type);

    pushUndo(defaultName(doc_, type));
    Feature f;
    f.id = doc_.newId();
    f.type = type;
    f.name = defaultName(doc_, type);
    if (kind >= 0) {
        auto m = std::make_shared<ModifyParams>();
        m->kind = kind;
        // A value that suits the size of what is selected.
        double diag = 0;
        for (uint32_t b : selectedBodies()) {
            auto bit = doc_.bodies.find(b);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit != doc_.parts.end() && pit->second.shape) diag = std::max(diag, kernel::shapeDiag(pit->second.shape));
        }
        double v = type == FeatureType::Shell ? std::clamp(diag * 0.04, 0.4, 3.0) :
                   type == FeatureType::PressPull ? std::clamp(diag * 0.1, 1.0, 20.0) : std::clamp(diag * 0.03, 0.5, 5.0);
        if (diag <= 0) v = type == FeatureType::PressPull ? 5.0 : 1.0;
        v = v < 2 ? std::round(v * 10) / 10 : std::round(v);
        // Fillet, chamfer and press pull wait at 0 for a drag or a typed value (Fusion); shell
        // keeps a sensible wall thickness.
        if (type == FeatureType::Draft) v = 5;
        if (type == FeatureType::ReplaceFace) v = 10;
        if (type == FeatureType::Fillet || type == FeatureType::Chamfer || type == FeatureType::PressPull) v = 0;
        m->valueExpr = fmtT(v);
        f.modify = m;
    } else if (type == FeatureType::Pattern) {
        f.pattern = std::make_shared<PatternParams>();
    } else if (type == FeatureType::Mirror) {
        f.mirror = std::make_shared<MirrorParams>();
    } else if (type == FeatureType::Combine) {
        f.combine = std::make_shared<CombineParams>();
    } else if (type == FeatureType::Split) {
        f.split = std::make_shared<SplitParams>();
    } else if (type == FeatureType::SplitFace) {
        auto m = std::make_shared<SplitFaceParams>();
        m->offsetExpr = "";
        f.splitFace = m;
    } else if (type == FeatureType::Hole) {
        f.hole = std::make_shared<HoleParams>();
    } else if (type == FeatureType::Move) {
        f.move = std::make_shared<MoveParams>();
        f.move->together = true;
    } else if (type == FeatureType::Remove) {
        f.remove = std::make_shared<RemoveParams>();
    } else if (type == FeatureType::Gear) {
        f.gear = std::make_shared<GearParams>(defaultGear());
    } else if (type == FeatureType::Thread) {
        f.thread = std::make_shared<ThreadParams>();
    } else if (type == FeatureType::Wing) {
        f.wing = std::make_shared<WingParams>(defaultWing());
    } else if (type == FeatureType::Dovetail) {
        f.dovetail = std::make_shared<DovetailParams>(defaultDovetail());
    } else if (type == FeatureType::Blade) {
        f.blade = std::make_shared<BladeParams>(defaultBlade());
    } else if (type == FeatureType::Rib) {
        f.rib = std::make_shared<RibParams>();
    } else if (type == FeatureType::Web) {
        f.web = std::make_shared<RibParams>();
    } else if (type == FeatureType::Emboss) {
        f.emboss = std::make_shared<EmbossParams>();
    } else if (type == FeatureType::Boss) {
        f.boss = std::make_shared<BossParams>();
    } else if (type == FeatureType::Snap) {
        f.snap = std::make_shared<SnapParams>();
    } else if (type == FeatureType::Rest) {
        f.rest = std::make_shared<RestParams>();
    } else if (type == FeatureType::Lip) {
        f.lip = std::make_shared<LipParams>();
    } else if (type == FeatureType::Silhouette) {
        f.silhouette = std::make_shared<SilhouetteParams>();
    } else if (type == FeatureType::Joint) {
        f.joint = std::make_shared<JointParams>();
    } else if (type == FeatureType::AsBuilt) {
        f.asbuilt = std::make_shared<JointParams>();
    } else if (type == FeatureType::Rigid) {
        f.rigid = std::make_shared<RigidParams>();
    } else if (type == FeatureType::Coil || type == FeatureType::Pipe) {
        auto p = std::make_shared<SweepSolidParams>();
        if (type == FeatureType::Pipe) { p->diaExpr = "10"; p->heightExpr = "40"; p->sectionExpr = "0"; }
        f.pathSolid = p;
    } else if (type == FeatureType::Align) {
        auto a = std::make_shared<AlignParams>();
        a->targets = selectionRefs();
        f.align = a;
    }

    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    doc_.touch();
    dirty_ = true;
    stampActiveComponent(f.id);

    tdlg_ = ToolDlg{};
    tdlg_.open = true;
    tdlg_.featureId = f.id;
    tdlg_.type = type;
    tdlg_.focus = false;
    fillToolDlgFromParams(f);
    activePickFilter_ = kind == 0 || kind == 1 ? (PickEdge | PickFace) : kind >= 2 ? PickFace : bodyPickTool(type) ? PickBody : 0;
    if (type == FeatureType::Rib || type == FeatureType::Web || type == FeatureType::Emboss || type == FeatureType::Boss || type == FeatureType::Snap || type == FeatureType::Rest || type == FeatureType::Lip || type == FeatureType::Silhouette) activePickFilter_ = PickBody;
    if (type == FeatureType::Thread) {
        activePickFilter_ = PickFace;
        selEdges_.clear();
        selection_.clear();
    }
    if (type == FeatureType::Gear || type == FeatureType::Wing || type == FeatureType::Blade) clearSelection();
    if (type == FeatureType::Align) {
        activePickFilter_ = PickFace | PickEdge;
        toast("Click a circle, a face or a corner on the part to move");
    }
    // Faces picked for an edge tool become their edges; edges picked for a face tool are dropped.
    if (kind == 2 || kind == 3 || kind == 5 || kind == 6) selEdges_.clear();
    if (type == FeatureType::Pattern && selection_.empty() && !selFaces_.empty()) {
        // Faces picked first (a hole's wall): the pattern copies those faces.
        tdlg_.patFaces = true;
        tdlg_.patFaceSel = selFaces_;
        selEdges_.clear();
        activePickFilter_ = PickFace;
    } else if (bodyPickTool(type)) {
        // Faces and edges stand for their bodies.
        for (uint32_t b : selectedBodies()) {
            if (!selection_.count(b)) selOrder_.push_back(b);
            selection_.insert(b);
        }
        selFaces_.clear();
        selEdges_.clear();
    }
    selProfiles_.clear();
    markSceneDirty();
    syncToolFromSelection();
    if (kind >= 0 && tdlg_.targets.empty())
        toast(kind <= 1 ? "Click the edges (or faces) to round" : "Click the faces to use");
    else if (type == FeatureType::Pattern && tdlg_.patFaces)
        toast("Faces picked. Press Pick axis, then click what to go around");
    else if (bodyPickTool(type) && tdlg_.targets.empty())
        toast(type == FeatureType::Combine ? "Click the target body, then the tool bodies" : "Click the bodies to use");
}

void App::runFillet() { startToolFeature(FeatureType::Fillet); }
void App::runChamfer() { startToolFeature(FeatureType::Chamfer); }
void App::runShell() { startToolFeature(FeatureType::Shell); }
void App::runThicken() { startToolFeature(FeatureType::Thicken); }

void App::runPressPull() {
    // Q on an edge rounds it, on a face moves it (Fusion's Press Pull).
    if (!anySelection()) {
        if (hov_.kind == HoverKind::Edge) selEdges_.insert({hov_.body, hov_.index});
        else if (hov_.kind == HoverKind::Face) selFaces_.insert({hov_.body, hov_.index});
        markSceneDirty();
    }
    if (!selEdges_.empty() && selFaces_.empty()) startToolFeature(FeatureType::Fillet);
    else startToolFeature(FeatureType::PressPull);
}

void App::runRectPattern() { startToolFeature(FeatureType::Pattern); }

void App::runCircPattern() {
    startToolFeature(FeatureType::Pattern);
    if (tdlg_.open && tdlg_.type == FeatureType::Pattern) {
        tdlg_.option2 = 1;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "6");
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "360");
        tdlg_.option = 2;
        // Bodies (or faces) first; ones already picked go straight to the axis (a round or flat face, or an edge).
        tdlg_.patFaces = tdlg_.targets.empty() && !selFaces_.empty();
        tdlg_.patFaceSel = tdlg_.patFaces ? selFaces_ : std::set<SubRef>{};
        tdlg_.pickWait = !tdlg_.targets.empty() || !tdlg_.patFaceSel.empty();
        tdlg_.axisName.clear();
        activePickFilter_ = tdlg_.pickWait ? (PickFace | PickEdge) : patternObjectFilter(tdlg_.patFaces, 1);
        applyToolDialog();
    }
}

void App::runPathPattern() {
    startToolFeature(FeatureType::Pattern);
    if (tdlg_.open && tdlg_.type == FeatureType::Pattern) {
        tdlg_.option2 = 2;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "4");
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "20");
        tdlg_.option = 0;
        tdlg_.option3 = 0;
        tdlg_.flag = false;
        tdlg_.selA = tdlg_.selB = 0;
        tdlg_.pathBody = {0, 0};
        tdlg_.pathEdge = 0;
        tdlg_.axisName.clear();
        tdlg_.patFaces = tdlg_.targets.empty() && !selFaces_.empty();
        tdlg_.patFaceSel = tdlg_.patFaces ? selFaces_ : std::set<SubRef>{};
        tdlg_.pickWait = !tdlg_.targets.empty() || !tdlg_.patFaceSel.empty();
        activePickFilter_ = tdlg_.pickWait ? PickEdge : patternObjectFilter(tdlg_.patFaces, 2);
        applyToolDialog();
    }
}

void App::runMirror() {
    startToolFeature(FeatureType::Mirror);
    if (!tdlg_.open || tdlg_.type != FeatureType::Mirror) return;
    // No copy lands on top of the original: it waits for the mirror plane (a face or an origin plane).
    tdlg_.mirrorWaitPlane = true;
    applyToolDialog();
    // Bodies already picked: go straight to the mirror plane, like Fusion's next selection box.
    if (!tdlg_.targets.empty()) {
        tdlg_.option2 = 1;
        activePickFilter_ = PickFace;
        toast("Click a flat face to mirror across, or choose an origin plane");
    }
}
void App::runCombine() { startToolFeature(FeatureType::Combine); }
void App::runSplitBody() { startToolFeature(FeatureType::Split); }
void App::runSplitFace() { startToolFeature(FeatureType::SplitFace); }
void App::runRemove() { startToolFeature(FeatureType::Remove); }
void App::runMove() { startToolFeature(FeatureType::Move); }

void App::runScale() {
    startToolFeature(FeatureType::Move);
    if (!tdlg_.open || tdlg_.type != FeatureType::Move) return;
    int n = 0;
    for (auto& ft : doc_.timeline)
        if (ft.type == FeatureType::Move && ft.move && ft.move->op == 1) ++n;
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (fi >= 0 && doc_.timeline[fi].move) {
        doc_.timeline[fi].name = "Scale" + std::to_string(n + 1);
        doc_.timeline[fi].move->op = 1;
    }
    tdlg_.option = 1;
    std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "1");
    std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "1");
    std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "1");
    applyToolDialog();
}

void App::runReplaceFace() { startToolFeature(FeatureType::ReplaceFace); }

void App::makeSolidPrimitive(bool cylinder) {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    sk::Plane pl;
    pl.x = {1, 0, 0};
    pl.y = {0, 1, 0};
    pl.name = "XY";
    createSketchOn(pl, std::string());
    sk::Sketch* s = curSketch();
    if (!s) { toast(cylinder ? "Could not start the cylinder" : "Could not start the box", ToastKind::Error); return; }
    if (cylinder) s->addCircle(s->addPoint({0, 0}), 5);
    else {
        uint32_t p0 = s->addPoint({0, 0}), p1 = s->addPoint({10, 0}), p2 = s->addPoint({10, 10}), p3 = s->addPoint({0, 10});
        s->addLine(p0, p1); s->addLine(p1, p2); s->addLine(p2, p3); s->addLine(p3, p0);
    }
    s->touch();
    uint32_t sid = s->id;
    finishSketch();
    clearSelection();
    selProfiles_.insert({sid, 0});
    runExtrude();
    if (!dlg_.open) { toast("Could not extrude that", ToastKind::Error); return; }
    std::snprintf(dlg_.dist, sizeof dlg_.dist, "10");
    dlg_.opAuto = false;
    dlg_.op = 0;
    applyDialogValues();
    const char* stem = cylinder ? "Cyl" : "Box";
    int n = 0;
    for (auto& ft : doc_.timeline)
        if (ft.name.rfind(stem, 0) == 0) ++n;
    std::string nm = std::string(stem) + std::to_string(n + 1);
    int fi = doc_.featureIndex(dlg_.featureId);
    if (fi >= 0) doc_.timeline[fi].name = nm;
    aiNameOutput(dlg_.featureId, nm, 0);
    closeFeatureDialog(true);
    toast(cylinder ? "Made " + nm + ", diameter 10, height 10" : "Made " + nm + ", 10 mm on each side");
}

void App::runBox() { makeSolidPrimitive(false); }
void App::runCylinder() { makeSolidPrimitive(true); }

namespace {
sk::Plane primitiveXY() {
    sk::Plane pl;
    pl.x = {1, 0, 0};
    pl.y = {0, 1, 0};
    pl.name = "XY";
    return pl;
}
// Sketch Y is world Z, so a revolve about the sketch Y axis spins about Z.
sk::Plane primitiveAxis() {
    sk::Plane pl;
    pl.x = {-1, 0, 0};
    pl.y = {0, 0, 1};
    pl.name = "Axis";
    return pl;
}
} // namespace

sk::Sketch* App::openPrimitiveSketch(const sk::Plane& pl, const char* fail) {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    createSketchOn(pl, std::string());
    sk::Sketch* s = curSketch();
    if (!s) toast(fail, ToastKind::Error);
    return s;
}

bool App::extrudePrimitive(uint32_t sid, int which, const char* dist, const char* fail) {
    clearSelection();
    auto sit = doc_.sketches.find(sid);
    if (sit == doc_.sketches.end()) { toast(fail, ToastKind::Error); return false; }
    selProfiles_.clear();
    const auto& profs = sketchProfiles(sit->second);
    if (which < 0) {
        int pick = -1;
        double best = -1;
        for (size_t i = 0; i < profs.size(); ++i) {
            if (!profs[i].holes.empty()) { pick = (int)i; break; }
            if (profs[i].area > best) { best = profs[i].area; pick = (int)i; }
        }
        which = pick;
    }
    if (which < 0 || (size_t)which >= profs.size()) { toast(fail, ToastKind::Error); return false; }
    selProfiles_.insert({sid, (uint32_t)which});
    runExtrude();
    if (!dlg_.open) { toast(fail, ToastKind::Error); return false; }
    std::snprintf(dlg_.dist, sizeof dlg_.dist, "%s", dist);
    dlg_.opAuto = false;
    dlg_.op = 0;
    applyDialogValues();
    return true;
}

bool App::revolvePrimitive(uint32_t sid, const char* fail) {
    clearSelection();
    auto sit = doc_.sketches.find(sid);
    if (sit == doc_.sketches.end()) { toast(fail, ToastKind::Error); return false; }
    selProfiles_.clear();
    const auto& profs = sketchProfiles(sit->second);
    if (profs.empty()) { toast(fail, ToastKind::Error); return false; }
    for (size_t i = 0; i < profs.size(); ++i) selProfiles_.insert({sid, (uint32_t)i});
    runRevolve();
    if (!dlg_.open) { toast(fail, ToastKind::Error); return false; }
    std::snprintf(dlg_.angle, sizeof dlg_.angle, "360");
    dlg_.axis = 0;
    dlg_.opAuto = false;
    dlg_.op = 0;
    applyDialogValues();
    return true;
}

void App::closeNamedPrimitive(const char* stem, const char* suffix) {
    int n = 0;
    for (auto& ft : doc_.timeline)
        if (ft.name.rfind(stem, 0) == 0) ++n;
    std::string nm = std::string(stem) + std::to_string(n + 1);
    int fi = doc_.featureIndex(dlg_.featureId);
    if (fi >= 0) doc_.timeline[fi].name = nm;
    aiNameOutput(dlg_.featureId, nm, 0);
    closeFeatureDialog(true);
    toast(std::string("Made ") + nm + suffix);
}

void App::runSphere() {
    sk::Sketch* s = openPrimitiveSketch(primitiveAxis(), "Could not start the sphere");
    if (!s) return;
    uint32_t c = s->addPoint({0, 0}), a0 = s->addPoint({0, -5}), b0 = s->addPoint({0, 5});
    s->addArc(c, a0, b0);
    s->addLine(b0, a0);
    s->touch();
    uint32_t sid = s->id;
    finishSketch();
    if (!revolvePrimitive(sid, "Could not revolve the sphere")) return;
    closeNamedPrimitive("Sphere", ", diameter 10");
}

void App::runCone() {
    sk::Sketch* s = openPrimitiveSketch(primitiveAxis(), "Could not start the cone");
    if (!s) return;
    uint32_t p0 = s->addPoint({0, 0}), p1 = s->addPoint({5, 0}), p2 = s->addPoint({0, 10});
    s->addLine(p0, p1); s->addLine(p1, p2); s->addLine(p2, p0);
    s->touch();
    uint32_t sid = s->id;
    finishSketch();
    if (!revolvePrimitive(sid, "Could not revolve the cone")) return;
    closeNamedPrimitive("Cone", ", bottom diameter 10, height 10");
}

void App::runTorus() {
    sk::Sketch* s = openPrimitiveSketch(primitiveAxis(), "Could not start the torus");
    if (!s) return;
    s->addCircle(s->addPoint({10, 0}), 2);
    s->touch();
    uint32_t sid = s->id;
    finishSketch();
    if (!revolvePrimitive(sid, "Could not revolve the torus")) return;
    closeNamedPrimitive("Torus", ", centerline diameter 20, tube diameter 4");
}

void App::runWedge() {
    sk::Sketch* s = openPrimitiveSketch(primitiveXY(), "Could not start the wedge");
    if (!s) return;
    uint32_t p0 = s->addPoint({0, 0}), p1 = s->addPoint({10, 0}), p2 = s->addPoint({0, 10});
    s->addLine(p0, p1); s->addLine(p1, p2); s->addLine(p2, p0);
    s->touch();
    uint32_t sid = s->id;
    finishSketch();
    if (!extrudePrimitive(sid, 0, "10", "Could not extrude the wedge")) return;
    closeNamedPrimitive("Wedge", ", 10 mm on each side");
}

void App::runTube() {
    sk::Sketch* s = openPrimitiveSketch(primitiveXY(), "Could not start the tube");
    if (!s) return;
    s->addCircle(s->addPoint({0, 0}), 5);
    s->addCircle(s->addPoint({0, 0}), 3);
    s->touch();
    uint32_t sid = s->id;
    finishSketch();
    if (!extrudePrimitive(sid, -1, "10", "Could not extrude the tube")) return;
    closeNamedPrimitive("Tube", ", diameter 10, hole 6, height 10");
}

void App::runAlign() { startToolFeature(FeatureType::Align); }

void App::runHole() {
    // H again closes the hole tool (keeping the holes placed so far).
    if (holeMode()) {
        bool any = false;
        int fi = doc_.featureIndex(tdlg_.featureId);
        if (fi >= 0 && doc_.timeline[fi].hole) any = !doc_.timeline[fi].hole->points.empty();
        if (any) commandOk();
        else commandCancel();
        return;
    }
    // Circles picked in a sketch: one hole at each. Otherwise click right on the faces (no sketch).
    uint32_t sid = 0;
    if (!selProfiles_.empty()) sid = selProfiles_.begin()->first;
    if (!sid) {
        startHolePlacement();
        return;
    }
    if (sk_.active) finishSketch();
    auto sit = doc_.sketches.find(sid);
    bool any = false;
    if (sit != doc_.sketches.end())
        for (auto& [id, e] : sit->second.ents)
            if (e.type == sk::EntType::Circle && !e.construction) any = true;
    if (!any) {
        startHolePlacement();
        return;
    }
    clearSelection();
    startToolFeature(FeatureType::Hole);
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (tdlg_.open && fi >= 0 && doc_.timeline[fi].hole) {
        auto hp = std::make_shared<HoleParams>(*doc_.timeline[fi].hole);
        hp->sketchId = sid;
        doc_.timeline[fi].hole = hp;
        applyToolDialog();
    }
}

std::vector<std::pair<uint32_t, std::string>> App::sketchesBeforeMarker() const {
    std::vector<std::pair<uint32_t, std::string>> out;
    for (int i = 0; i < doc_.markerPos && i < (int)doc_.timeline.size(); ++i)
        if (doc_.timeline[i].type == FeatureType::Sketch) {
            auto it = doc_.sketches.find(doc_.timeline[i].sketchId);
            if (it != doc_.sketches.end()) out.push_back({it->second.id, it->second.name});
        }
    return out;
}

void App::runSweep() {
    auto sk = sketchesBeforeMarker();
    if (sk.size() < 2) {
        toast("Sweep needs two sketches: a closed profile and an open path", ToastKind::Warning);
        return;
    }
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    pushUndo("Sweep");
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::Sweep;
    f.name = defaultName(doc_, FeatureType::Sweep);
    auto m = std::make_shared<MultiSketchParams>();
    m->sketchIds = {sk[sk.size() - 2].first, sk[sk.size() - 1].first};   // profile, path
    f.multi = m;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    doc_.touch();
    dirty_ = true;
    tdlg_ = ToolDlg{};
    tdlg_.open = true;
    tdlg_.featureId = f.id;
    tdlg_.type = f.type;
    tdlg_.sketchIds = m->sketchIds;
    std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "0");
    activePickFilter_ = 0;
    clearSelection();
    applyToolDialog();
}

void App::runLoft() {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    pushUndo("Loft");
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::Loft;
    f.name = defaultName(doc_, FeatureType::Loft);
    // Fusion's Loft: click the sections in order, sketch profiles or flat faces of bodies.
    auto m = std::make_shared<MultiSketchParams>();
    f.multi = m;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    doc_.touch();
    dirty_ = true;
    tdlg_ = ToolDlg{};
    tdlg_.open = true;
    tdlg_.featureId = f.id;
    tdlg_.type = f.type;
    activePickFilter_ = PickProfile | PickFace;
    clearSelection();
    applyToolDialog();
}

// The loft's sections follow the canvas picks, kept in the order they were clicked.
void App::syncLoftPicks() {
    std::vector<std::pair<int, SubRef>> now;
    for (auto& r : selProfiles_) now.push_back({0, r});
    for (auto& r : selFaces_) now.push_back({1, r});
    auto has = [](const std::vector<std::pair<int, SubRef>>& v, const std::pair<int, SubRef>& x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    for (size_t i = 0; i < tdlg_.loftSel.size();) {
        if (!has(now, tdlg_.loftSel[i])) {
            tdlg_.loftSel.erase(tdlg_.loftSel.begin() + (ptrdiff_t)i);
            if (i < tdlg_.loftPicks.size()) tdlg_.loftPicks.erase(tdlg_.loftPicks.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
    for (auto& it : now) {
        if (has(tdlg_.loftSel, it)) continue;
        LoftPick lp;
        if (it.first == 0) {
            auto sit = doc_.sketches.find(it.second.first);
            if (sit == doc_.sketches.end()) continue;
            const auto& profs = sketchProfiles(sit->second);
            if (it.second.second >= profs.size()) continue;
            lp.sketchId = it.second.first;
            lp.pt = profileSamplePoint(profs[it.second.second]);
        } else {
            uint32_t bid = it.second.first;
            auto bit = doc_.bodies.find(bid);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            glm::dvec3 c, n;
            bool planar = false;
            TargetRef ref = refOfBody(bid);
            if (ref.first == tdlg_.featureId) {   // the loft's own result: it cannot loft from itself
                toast("That face belongs to this loft's own result. Pick faces of other bodies.", ToastKind::Warning);
                selFaces_.erase(it.second);
                continue;
            }
            if (pit == doc_.parts.end() || !pit->second.shape || !ref.first ||
                !kernel::faceInfo(pit->second.shape, it.second.second, c, n, planar) || !planar) {
                toast("Loft takes flat faces and sketch profiles (that face is curved)", ToastKind::Warning);
                selFaces_.erase(it.second);
                continue;
            }
            lp.face.feature = ref.first;
            lp.face.slot = ref.second;
            lp.face.face = it.second.second;
            lp.face.faceTotal = kernel::faceCount(pit->second.shape);
        }
        tdlg_.loftPicks.push_back(lp);
        tdlg_.loftSel.push_back(it);
    }
}

void App::runDraft() { startToolFeature(FeatureType::Draft); }
void App::runCoil() { startToolFeature(FeatureType::Coil); }
void App::runRib() { startToolFeature(FeatureType::Rib); }
void App::runWeb() { startToolFeature(FeatureType::Web); }
void App::runEmboss() { startToolFeature(FeatureType::Emboss); }
void App::runBoss() { startToolFeature(FeatureType::Boss); }
void App::runSnap() { startToolFeature(FeatureType::Snap); }
void App::runRest() { startToolFeature(FeatureType::Rest); }
void App::runLip() { startToolFeature(FeatureType::Lip); }
void App::runSilhouette() { startToolFeature(FeatureType::Silhouette); }
void App::runJoint() { startToolFeature(FeatureType::Joint); }
void App::runAsBuilt() { startToolFeature(FeatureType::AsBuilt); }
void App::runRigid() { startToolFeature(FeatureType::Rigid); }
void App::runPipe() { startToolFeature(FeatureType::Pipe); }

void App::startConstructPlane(int kind) {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    const char* undoName = kind == 1 ? "Plane at Angle" : kind == 2 ? "Midplane" : kind == 3 ? "Plane through 3 Points" : kind == 4 ? "Tangent Plane" : "Offset Plane";
    pushUndo(undoName);
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::ConstructPlane;
    int n = 0;
    for (auto& ft : doc_.timeline)
        if (ft.type == FeatureType::ConstructPlane && ft.cplane && ft.cplane->kind == kind) ++n;
    f.name = (kind == 1 ? "Angle" : kind == 2 ? "Mid" : kind == 3 ? "Pts" : kind == 4 ? "Tan" : "Plane") + std::to_string(n + 1);
    auto m = std::make_shared<ConstructPlaneParams>();
    m->kind = kind;
    m->base = 0;
    m->offsetExpr = kind == 1 ? "45" : kind == 3 ? "0" : "25";
    if (kind == 2) {
        for (int i = std::min(doc_.markerPos, (int)doc_.timeline.size()) - 1; i >= 0; --i)
            if (doc_.timeline[i].type == FeatureType::ConstructPlane) { m->refPlane = doc_.timeline[i].id; break; }
    }
    f.cplane = m;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    updateConstructPlane(f);
    doc_.touch();
    dirty_ = true;
    tdlg_ = ToolDlg{};
    tdlg_.open = true;
    tdlg_.featureId = f.id;
    tdlg_.type = f.type;
    tdlg_.focus = false;
    tdlg_.option2 = 0;
    tdlg_.selA = (int)m->refPlane;
    std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", m->offsetExpr.c_str());
    if (kind == 3) {
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", m->x1.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", m->y1.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", m->z1.c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", m->x2.c_str());
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", m->y2.c_str());
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", m->z2.c_str());
        std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%s", m->x3.c_str());
        std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%s", m->y3.c_str());
        std::snprintf(tdlg_.v9, sizeof tdlg_.v9, "%s", m->z3.c_str());
    }
    if (kind == 4) std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "0");
    activePickFilter_ = kind == 4 ? PickFace : 0;
    applyToolDialog();
}

void App::runOffsetPlane() { startConstructPlane(0); }
void App::runAnglePlane() { startConstructPlane(1); }
void App::runMidPlane() { startConstructPlane(2); }
void App::runThreePointPlane() { startConstructPlane(3); }
void App::runTangentPlane() { startConstructPlane(4); }

void App::runAxis() {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    pushUndo("Axis");
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::ConstructAxis;
    f.name = defaultName(doc_, f.type);
    auto m = std::make_shared<ConstructAxisParams>();
    f.caxis = m;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    updateConstructAxis(f);
    doc_.touch();
    dirty_ = true;
    tdlg_ = ToolDlg{};
    tdlg_.open = true;
    tdlg_.featureId = f.id;
    tdlg_.type = f.type;
    std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", m->ax.c_str());
    std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", m->ay.c_str());
    std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", m->az.c_str());
    std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", m->bx.c_str());
    std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", m->by.c_str());
    std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", m->bz.c_str());
    activePickFilter_ = 0;
    applyToolDialog();
}

void App::runPoint() {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    pushUndo("Point");
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::ConstructPoint;
    f.name = defaultName(doc_, f.type);
    auto m = std::make_shared<ConstructPointParams>();
    f.cpoint = m;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    updateConstructPoint(f);
    doc_.touch();
    dirty_ = true;
    tdlg_ = ToolDlg{};
    tdlg_.open = true;
    tdlg_.featureId = f.id;
    tdlg_.type = f.type;
    std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", m->x.c_str());
    std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", m->y.c_str());
    std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", m->z.c_str());
    activePickFilter_ = 0;
    applyToolDialog();
}

void App::runSection() {
    sectionOpen_ = true;
    sectionAxis_ = std::clamp(doc_.section.axis, 0, 2);
    sectionFlip_ = doc_.section.flip;
    std::snprintf(sectionOff_, sizeof sectionOff_, "%g", doc_.section.offset);
}

std::string App::applySection(bool on) {
    double off = 0;
    try {
        if (sectionOff_[0]) off = peval::eval(sectionOff_, doc_.params);
    } catch (const std::runtime_error& ex) {
        return ex.what();
    }
    doc_.section.on = on;
    doc_.section.axis = std::clamp(sectionAxis_, 0, 2);
    doc_.section.offset = off;
    doc_.section.flip = sectionFlip_;
    doc_.touch();
    dirty_ = true;
    return {};
}

void App::runMeasure() {
    measureMode_ = !measureMode_;
    clearMeasure();
    if (measureMode_ && sk_.active) {
        // In a sketch: whatever was being drawn stops (Fusion), the sketch stays open.
        sk_.tool.clear();
        sk_.step = 0;
        sk_.clicks.clear();
        sk_.picks.clear();
        sk_.dimPlacing = false;
        sk_.chainStart = 0;
        sk_.hoverEnt = sk_.hoverCon = 0;
        sk_.snapRefPoly.clear();
    }
    if (measureMode_) toast(sk_.active ? "Measure: click sketch points, lines, circles, or the part. Esc goes back to the sketch."
                                       : "Measure: Distance, Angle, Radius, Area, or Loop. Esc exits.");
    activity();
}

void App::runInterference() {
    std::vector<uint32_t> sel = selectedBodies();
    if (sel.size() < 2) {
        toast("Select two or more bodies to check for interference", ToastKind::Warning);
        return;
    }
    // Capture shapes on the main thread: workers never read the live document.
    struct Item { std::string name; ShapePtr shape; glm::mat4 xf; };
    std::vector<Item> items;
    for (uint32_t id : sel) {
        auto bit = doc_.bodies.find(id);
        if (bit == doc_.bodies.end()) continue;
        auto pit = doc_.parts.find(bit->second.partId);
        if (pit == doc_.parts.end() || !pit->second.shape) continue;
        items.push_back({bit->second.name, pit->second.shape, bit->second.xform});
    }
    if (items.size() < 2) {
        toast("Interference needs solid bodies (mesh bodies have no exact volume)", ToastKind::Warning);
        return;
    }
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Checking interference");
    jobs_->submit("interference", [this, jobs, items] {
        std::string report;
        int hits = 0;
        for (size_t i = 0; i < items.size(); ++i)
            for (size_t j = i + 1; j < items.size(); ++j) {
                if (!kernel::shapesTouch(items[i].shape, items[i].xf, items[j].shape, items[j].xf)) continue;
                try {
                    // Touching faces are fine; only a shared volume counts.
                    std::vector<kernel::BoolTarget> t{{items[j].shape, items[j].xf}};
                    auto placed = kernel::applyBool(kernel::BoolOp::Intersect, items[i].shape, t);
                    if (!placed.empty() && kernel::shapeDiag(placed[0]) > 1e-4) {
                        ++hits;
                        report += items[i].name + " / " + items[j].name + "\n";
                    }
                } catch (const std::exception&) {
                    // no shared volume
                }
            }
        jobs->post([this, jobs, hits, report] {
            jobs->setStatus("");
            if (hits == 0) toast("No interference between the selected bodies", ToastKind::Success);
            else toast("Interference between:\n" + report, ToastKind::Warning);
        });
    }, [this, jobs](const std::string& err) {
        jobs->setStatus("");
        toast("Interference check failed: " + err, ToastKind::Warning);
    });
}

void App::convertToSolid(uint32_t bodyId) {
    auto it = doc_.bodies.find(bodyId);
    if (it == doc_.bodies.end()) return;
    auto pit = doc_.parts.find(it->second.partId);
    if (pit == doc_.parts.end()) return;
    if (pit->second.shape) {
        toast("That body is already a solid");
        return;
    }
    pushUndo("Convert to Solid");
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::MeshConvert;
    f.name = "Convert " + it->second.name;
    auto m = std::make_shared<ModifyParams>();
    m->kind = 4;
    m->targets = {{it->second.featureId, doc_.bodySlot(bodyId)}};
    f.modify = m;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    doc_.touch();
    dirty_ = true;
    scheduleRecompute(pos);
    toast("Converting in the background (sewn solid, may take a moment)");
}

void App::drawMeasurePanel() {
    if (!measureMode_) return;
    const float S = uiScale_;
    auto num = [](double v) {
        char b[48];
        std::snprintf(b, sizeof b, "%.4f", v);
        std::string t = b;
        while (t.find('.') != std::string::npos && !t.empty() && t.back() == '0') t.pop_back();
        if (!t.empty() && t.back() == '.') t.pop_back();
        return t == "-0" ? std::string("0") : t;
    };
    // What was measured, in the viewport: the closest points (orange, with the value) and, for round
    // picks, the line between their centers (blue).
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        auto seg = [&](const glm::dvec3& a, const glm::dvec3& b, ImU32 col, const std::string& label) {
            ImVec2 sa, sb;
            if (!worldToScreen(a, sa) || !worldToScreen(b, sb)) return;
            dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 160), 4.f * S);
            dl->AddLine(sa, sb, col, 2.f * S);
            dl->AddCircleFilled(sa, 4.f * S, col);
            dl->AddCircleFilled(sb, 4.f * S, col);
            if (label.empty()) return;
            ImVec2 mid((sa.x + sb.x) * 0.5f + 8 * S, (sa.y + sb.y) * 0.5f - 8 * S);
            ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddRectFilled(ImVec2(mid.x - 4 * S, mid.y - 2 * S), ImVec2(mid.x + ts.x + 4 * S, mid.y + ts.y + 2 * S), IM_COL32(25, 27, 32, 220), 4 * S);
            dl->AddText(mid, col, label.c_str());
        };
        // Sketch curves: the picked ones stay lit, the one under the cursor lights up.
        for (auto& pk : measurePicks_)
            if (pk.sketch) drawSketchCurve(pk.sketch, pk.ent, IM_COL32(70, 150, 255, 255), 3.5f * S);
        if (viewportHovered_ && !measureBusy_) {
            uint32_t hs = 0, he = 0;
            float hd = 0;
            ImVec2 mp = ImGui::GetIO().MousePos;
            uint32_t ps = 0, pe = 0;
            float pd = 0;
            if (sketchPointUnder(mp.x, mp.y, ps, pe, pd) && (hov_.kind != HoverKind::Edge || pd < 7.f * S))
                drawSketchCurve(ps, pe, IM_COL32(255, 190, 70, 255), 3.5f * S);
            else if (sketchCurveAt(mp.x, mp.y, hs, he, hd) && (hov_.kind != HoverKind::Edge || hd < 5.f * S))
                drawSketchCurve(hs, he, IM_COL32(255, 190, 70, 255), 3.5f * S);
        }
        if (measureCenterLine_) seg(measureCa_, measureCb_, IM_COL32(90, 170, 255, 255), "");
        if (measureLine_ && measureDist_ >= 0) seg(measurePa_, measurePb_, IM_COL32(255, 140, 60, 255), num(measureDist_) + " mm");
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 150 * S, vp->Pos.y + 118 * S), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(320 * S, 0), ImGuiCond_Always);
    bool open = true;
    ImGui::Begin("MEASURE###measure", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    const char* kinds[] = {"Distance", "Angle", "Radius", "Area", "Loop", "Alignment (level, centered)"};
    int before = measureKind_;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##mkind", &measureKind_, kinds, 6) && measureKind_ != before) {
        clearMeasure();
        activity();
    }
    if (measurePicks_.empty()) {
        if (measureKind_ == 0) ImGui::TextDisabled("Click two edges, faces, bodies or sketch lines");
        else if (measureKind_ == 1) ImGui::TextDisabled("Click two straight edges or flat faces");
        else if (measureKind_ == 2) ImGui::TextDisabled("Click a round face or a circular edge");
        else if (measureKind_ == 3) ImGui::TextDisabled("Click a face");
        else if (measureKind_ == 5) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Click two faces or edges. It tells you if they are parallel, level (two flat faces) and centered, and by how much they are off. A ring's flat face and a cylinder's top, or two round faces, show if they are on one axis.");
            ImGui::PopTextWrapPos();
        }
        else ImGui::TextDisabled("Click a face, or an edge of a loop");
    }
    for (size_t i = 0; i < measurePicks_.size() && i < 2; ++i) {
        ImGui::Text("%d. %s", (int)i + 1, measurePicks_[i].name.c_str());
        if (!measurePicks_[i].info.empty()) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("   %s", measurePicks_[i].info.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::Spacing();
    if (measureBusy_) ImGui::TextUnformatted("measuring...");
    else if (measureDist_ >= 0 && measureKind_ == 5) {
        // The verdict first (bold), then each check with its number.
        ImGui::PushTextWrapPos(0.f);
        for (size_t i = 0; i < measureLines_.size(); ++i) {
            if (i == 0) {
                ImGui::PushFont(fonts_.bold, 0.f);
                bool good = measureLines_[0] == "Lined up";
                ImGui::TextColored(good ? ImVec4(0.35f, 0.8f, 0.45f, 1.f) : ImVec4(1.f, 0.55f, 0.3f, 1.f), "%s", measureLines_[0].c_str());
                ImGui::PopFont();
            } else if (i + 1 == measureLines_.size()) {
                ImGui::TextDisabled("%s", measureLines_[i].c_str());
            } else {
                ImGui::TextUnformatted(measureLines_[i].c_str());
            }
        }
        ImGui::PopTextWrapPos();
        if (measureCenterLine_) ImGui::TextDisabled("Blue: from one center to the other.");
    } else if (measureDist_ >= 0) {
        const char* unit[] = {"mm", "deg", "mm", "mm2", "mm"};
        int k = std::clamp(measureKind_, 0, 4);
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::Text("%s  %s %s", kinds[k], num(measureDist_).c_str(), unit[k]);
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.f);
        for (auto& l : measureLines_) ImGui::TextUnformatted(l.c_str());
        ImGui::PopTextWrapPos();
        if (measureKind_ == 0 && measureLine_) ImGui::TextDisabled("Orange: closest points. Blue: centers.");
    } else if ((int)measurePicks_.size() >= (measureKind_ <= 1 || measureKind_ == 5 ? 2 : 1)) ImGui::TextUnformatted("-");
    ImGui::Spacing();
    if (ImGui::Button("Clear")) clearMeasure();
    ImGui::SameLine();
    if (ImGui::Button("Close")) open = false;
    ImGui::End();
    if (!open) runMeasure();
}

void App::drawOriginPlanes() {
    // Fusion style origin: the XY, XZ and YZ squares from the corner at 0, the Z axis and the point.
    // The ground shader draws X and Y; the plane picker draws its own squares while choosing.
    if (!settings_.showOrigin || sk_.choosingPlane) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float S = uiScale_;
    double half = std::max(sceneBounds_.valid() ? (double)sceneBounds_.diag() * 0.35 : 0.0, 60.0);
    const glm::dvec3 ax[3][2] = {{{1, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}}};
    const char* names[3] = {"XY", "XZ", "YZ"};
    for (int i = 0; i < 3; ++i) {
        glm::dvec3 u = ax[i][0] * half, v = ax[i][1] * half;
        glm::dvec3 c[4] = {glm::dvec3(0), u, u + v, v};
        ImVec2 q[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k)
            if (!worldToScreen(c[k], q[k])) ok = false;
        if (!ok) continue;
        dl->AddConvexPolyFilled(q, 4, ui::th(IM_COL32(255, 107, 53, 24)));
        dl->AddPolyline(q, 4, ui::th(IM_COL32(255, 107, 53, 140)), ImDrawFlags_Closed, 1.2f * S);
        dl->AddText(ImVec2(q[2].x - 22 * S, q[2].y - 16 * S), ui::th(IM_COL32(190, 70, 25, 190)), names[i]);
    }
    ImVec2 z0, z1, o;
    if (worldToScreen({0, 0, -half}, z0) && worldToScreen({0, 0, half}, z1)) dl->AddLine(z0, z1, IM_COL32(60, 110, 230, 220), 1.6f * S);
    if (worldToScreen({0, 0, 0}, o)) {
        dl->AddCircleFilled(o, 3.5f * S, ui::th(IM_COL32(250, 250, 250, 240)));
        dl->AddCircle(o, 3.5f * S, ui::th(IM_COL32(40, 50, 70, 230)), 0, 1.2f * S);
    }
}

void App::drawConstructPlanes() {
    drawOriginPlanes();
    drawCosmeticThreads();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float S = uiScale_;
    if (!doc_.constructAxes.empty() || !doc_.constructPoints.empty()) {
        double half = std::max(40.0, sceneBounds_.valid() ? sceneBounds_.diag() * 0.45 : 80.0);
        for (auto& [fid, ax] : doc_.constructAxes) {
            if (!doc_.isFeatureActive(fid)) continue;
            glm::dvec3 a = ax.origin - ax.direction * half;
            glm::dvec3 b = ax.origin + ax.direction * half;
            ImVec2 pa, pb, po;
            if (worldToScreen(a, pa) && worldToScreen(b, pb))
                dl->AddLine(pa, pb, IM_COL32(40, 120, 220, 210), 1.6f);
            if (worldToScreen(ax.origin, po)) {
                dl->AddCircleFilled(po, 3.f * S, IM_COL32(40, 120, 220, 230));
                int fi = doc_.featureIndex(fid);
                if (fi >= 0) dl->AddText(ImVec2(po.x + 6, po.y - 14), IM_COL32(30, 90, 180, 230), doc_.timeline[fi].name.c_str());
            }
        }
        for (auto& [fid, p] : doc_.constructPoints) {
            if (!doc_.isFeatureActive(fid)) continue;
            ImVec2 s;
            if (!worldToScreen(p, s)) continue;
            dl->AddCircleFilled(s, 4.f * S, IM_COL32(220, 140, 40, 230));
            dl->AddCircle(s, 4.f * S, IM_COL32(40, 50, 70, 220), 0, 1.2f * S);
            int fi = doc_.featureIndex(fid);
            if (fi >= 0) dl->AddText(ImVec2(s.x + 6, s.y - 14), IM_COL32(180, 100, 20, 230), doc_.timeline[fi].name.c_str());
        }
    }
    if (doc_.constructPlanes.empty()) return;
    double half = std::max(20.0, sceneBounds_.valid() ? sceneBounds_.diag() * 0.3 : 60.0);
    for (auto& [fid, pl] : doc_.constructPlanes) {
        if (!doc_.isFeatureActive(fid)) continue;
        if (doc_.hiddenPlanes.count(fid) && !sk_.choosingPlane) continue;
        glm::dvec3 c[4] = {pl.toWorld({-half, -half}), pl.toWorld({half, -half}), pl.toWorld({half, half}), pl.toWorld({-half, half})};
        ImVec2 pts[4];
        bool ok = true;
        for (int i = 0; i < 4; ++i)
            if (!worldToScreen(c[i], pts[i])) ok = false;
        if (!ok) continue;
        dl->AddConvexPolyFilled(pts, 4, IM_COL32(230, 160, 60, sk_.choosingPlane ? 50 : 26));
        dl->AddPolyline(pts, 4, IM_COL32(220, 140, 40, sk_.choosingPlane ? 220 : 140), ImDrawFlags_Closed, 1.5f);
        int fi = doc_.featureIndex(fid);
        const char* nm = fi >= 0 ? doc_.timeline[fi].name.c_str() : pl.name.c_str();
        dl->AddText(ImVec2(pts[0].x + 4, pts[0].y - 16), IM_COL32(200, 120, 30, 230), nm);
    }
}

void App::exportStepDialog() {
    auto items = exportItems();
    if (items.empty()) {
        toast("Nothing visible to export", ToastKind::Warning);
        return;
    }
    std::string p = platform::saveFileDialog("Export STEP", "STEP files (*.step;*.stp)", "*.step", "step", doc_.name + ".step");
    if (p.empty()) return;
    auto cancel = std::make_shared<CancelToken>();
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Exporting STEP");
    jobs_->submit("export step", [this, jobs, items, p, cancel] {
        kernel::exportStep(items, p, *cancel);
        jobs->post([this, jobs, p] { jobs->setStatus(""); toast("Saved " + p, ToastKind::Success); });
    }, [this, jobs](const std::string& err) { jobs->setStatus(""); toast("Export failed: " + err, ToastKind::Error); });
}

void App::export3mfDialog() {
    auto items = exportItems();
    if (items.empty()) {
        toast("Nothing visible to export", ToastKind::Warning);
        return;
    }
    std::string p = platform::saveFileDialog("Export 3MF", "3MF files (*.3mf)", "*.3mf", "3mf", doc_.name + ".3mf");
    if (p.empty()) return;
    auto cancel = std::make_shared<CancelToken>();
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Exporting 3MF");
    jobs_->submit("export 3mf", [this, jobs, items, p, cancel] {
        kernel::export3mf(items, p, *cancel);
        jobs->post([this, jobs, p] { jobs->setStatus(""); toast("Saved " + p, ToastKind::Success); });
    }, [this, jobs](const std::string& err) { jobs->setStatus(""); toast("Export failed: " + err, ToastKind::Error); });
}

// ===================================================================== dialog <-> params

void App::fillToolDlgFromParams(const Feature& f) {
    if (f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer || f.type == FeatureType::Shell ||
        f.type == FeatureType::PressPull || f.type == FeatureType::Draft || f.type == FeatureType::ReplaceFace ||
        f.type == FeatureType::Thicken) {
        if (!f.modify) return;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.modify->valueExpr.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.modify->endExpr.c_str());
        tdlg_.targets = f.modify->targets;
        tdlg_.edges = f.modify->edges;
        tdlg_.faces = f.modify->faces;
        tdlg_.edgeTotal = f.modify->edgeTotal;
        tdlg_.faceTotal = f.modify->faceTotal;
        tdlg_.option = f.modify->axis;
        if (f.type == FeatureType::Shell || f.type == FeatureType::Thicken) tdlg_.option2 = f.modify->side;
    } else if ((f.type == FeatureType::Rib && f.rib) || (f.type == FeatureType::Web && f.web)) {
        const RibParams& r = f.type == FeatureType::Web ? *f.web : *f.rib;
        tdlg_.selA = (int)r.sketchId;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", r.thickExpr.c_str());
        tdlg_.flag = r.flip;
        tdlg_.option = r.side;
        tdlg_.targets = r.targets;
    } else if (f.type == FeatureType::Emboss && f.emboss) {
        const EmbossParams& e = *f.emboss;
        tdlg_.selA = (int)e.sketchId;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", e.depthExpr.c_str());
        tdlg_.flag = e.flip;
        tdlg_.option = e.deboss ? 1 : 0;
        tdlg_.targets = e.targets;
    } else if (f.type == FeatureType::Boss && f.boss) {
        const BossParams& b = *f.boss;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", b.xExpr.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", b.yExpr.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", b.zExpr.c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", b.diaExpr.c_str());
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", b.heightExpr.c_str());
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", b.holeExpr.c_str());
        tdlg_.option = b.axis;
        tdlg_.targets = b.targets;
    } else if (f.type == FeatureType::Snap && f.snap) {
        const SnapParams& s = *f.snap;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", s.xExpr.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", s.yExpr.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", s.zExpr.c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", s.lengthExpr.c_str());
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", s.widthExpr.c_str());
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", s.thickExpr.c_str());
        std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%s", s.hookExpr.c_str());
        std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%s", s.hookLenExpr.c_str());
        tdlg_.option = s.axis;
        tdlg_.targets = s.targets;
    } else if (f.type == FeatureType::Rest && f.rest) {
        const RestParams& r = *f.rest;
        tdlg_.selA = (int)r.sketchId;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", r.thickExpr.c_str());
        tdlg_.flag = r.flip;
        tdlg_.targets = r.targets;
    } else if (f.type == FeatureType::Lip && f.lip) {
        const LipParams& lip = *f.lip;
        tdlg_.selA = (int)lip.sketchId;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", lip.widthExpr.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", lip.heightExpr.c_str());
        tdlg_.flag = lip.flip;
        tdlg_.option = lip.groove ? 1 : 0;
        tdlg_.targets = lip.targets;
    } else if (f.type == FeatureType::Silhouette && f.silhouette) {
        tdlg_.option = f.silhouette->axis;
        tdlg_.targets = f.silhouette->targets;
    } else if ((f.type == FeatureType::Joint && f.joint) || (f.type == FeatureType::AsBuilt && f.asbuilt)) {
        const JointParams& j = f.type == FeatureType::AsBuilt ? *f.asbuilt : *f.joint;
        if (j.ground.first) tdlg_.targets = {j.ground};
        if (j.move.first) tdlg_.tools = {j.move};
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", j.gx.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", j.gy.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", j.gz.c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", j.mx.c_str());
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", j.my.c_str());
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", j.mz.c_str());
        std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%s", j.angleExpr.c_str());
        std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%s", j.slideExpr.c_str());
        tdlg_.option = std::clamp(j.kind, 0, 2);
        tdlg_.option2 = std::clamp(j.axis, 0, 2);
    } else if (f.type == FeatureType::Rigid && f.rigid) {
        tdlg_.targets = f.rigid->members;
    } else if ((f.type == FeatureType::Coil || f.type == FeatureType::Pipe) && f.pathSolid) {
        const SweepSolidParams& p = *f.pathSolid;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", p.diaExpr.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", p.heightExpr.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", (f.type == FeatureType::Coil ? p.turnsExpr : p.innerExpr).c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", p.sectionExpr.c_str());
        tdlg_.option = p.axis;
        tdlg_.option2 = p.leftHand ? 1 : 0;
        tdlg_.selA = (int)p.pathSketch;
    } else if (f.type == FeatureType::Pattern && f.pattern) {
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.pattern->countExpr.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", f.pattern->countYExpr.c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", f.pattern->spacingYExpr.c_str());
        tdlg_.axisO = f.pattern->axisO;
        tdlg_.axisD = f.pattern->axisD;
        tdlg_.targets = f.pattern->targets;
        tdlg_.patFaces = !f.pattern->faces.empty();
        tdlg_.patFaceSel.clear();
        for (auto& [ti, fc] : f.pattern->faces) {
            if (ti >= f.pattern->faceTargets.size()) continue;
            const TargetRef& r = f.pattern->faceTargets[ti];
            std::vector<uint32_t> ids = doc_.featureBodies(r.first);
            if (r.second < ids.size()) tdlg_.patFaceSel.insert({ids[r.second], fc});
        }
        if (tdlg_.patFaces) selFaces_ = tdlg_.patFaceSel;
        if (f.pattern->onPath) {
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.pattern->spacingExpr.c_str());
            tdlg_.option = f.pattern->distType;
            tdlg_.option2 = 2;
            tdlg_.option3 = f.pattern->orient;
            tdlg_.flag = f.pattern->flip;
            tdlg_.selA = (int)f.pattern->pathSketch;
            tdlg_.selB = (int)f.pattern->pathEnt;
            tdlg_.pathBody = f.pattern->pathBody;
            tdlg_.pathEdge = f.pattern->pathEdge;
            tdlg_.axisName = f.pattern->pathSketch ? "Sketch curve" : f.pattern->pathBody.first ? "Edge" : "";
        } else {
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.pattern->circular ? f.pattern->angleExpr.c_str() : f.pattern->spacingExpr.c_str());
            tdlg_.option = f.pattern->axis;
            if (f.pattern->axis == 3) tdlg_.axisName = "Picked edge";
            tdlg_.option2 = f.pattern->circular ? 1 : 0;
        }
    } else if (f.type == FeatureType::Mirror && f.mirror) {
        tdlg_.option = f.mirror->plane;
        tdlg_.targets = f.mirror->targets;
        tdlg_.axisO = f.mirror->planeO;
        tdlg_.axisD = f.mirror->planeN;
        tdlg_.axisName = f.mirror->planeName;
    } else if (f.type == FeatureType::Combine && f.combine) {
        tdlg_.option = f.combine->op;
        tdlg_.targets = f.combine->targets;
        tdlg_.tools = f.combine->tools;
        tdlg_.option2 = f.combine->keepTools ? 1 : 0;
    } else if (f.type == FeatureType::Split && f.split) {
        tdlg_.option = f.split->plane;
        tdlg_.targets = f.split->targets;
        tdlg_.axisO = f.split->toolO;
        tdlg_.axisD = f.split->toolN;
        tdlg_.axisR = f.split->toolR;
        tdlg_.axisName = f.split->toolName;
        tdlg_.selA = (int)f.split->sketchId;
        tdlg_.selB = (int)f.split->sketchEnt;
    } else if (f.type == FeatureType::SplitFace && f.splitFace) {
        tdlg_.option = f.splitFace->plane;
        tdlg_.targets = f.splitFace->targets;
        tdlg_.selA = (int)f.splitFace->sketchId;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.splitFace->offsetExpr.c_str());
    } else if (f.type == FeatureType::Align && f.align) {
        tdlg_.align = *f.align;
        tdlg_.targets = f.align->targets;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.align->angleExpr.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.align->offsetExpr.c_str());
    } else if (f.type == FeatureType::Hole && f.hole) {
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.hole->diaExpr.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.hole->depthExpr.c_str());
        tdlg_.option = f.hole->kind;
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", (f.hole->kind == 2 ? f.hole->csDiaExpr : f.hole->cbDiaExpr).c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", (f.hole->kind == 2 ? f.hole->csAngleExpr : f.hole->cbDepthExpr).c_str());
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", f.hole->spotDiaExpr.c_str());
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", f.hole->spotDepthExpr.c_str());
        std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%s", f.hole->drillExpr.c_str());
        std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%s", f.hole->pitchExpr.c_str());
        tdlg_.flag = f.hole->tapped;
        tdlg_.targets = f.hole->targets;
    } else if ((f.type == FeatureType::Sweep || f.type == FeatureType::Loft) && f.multi) {
        tdlg_.sketchIds = f.multi->sketchIds;
        tdlg_.railIds = f.multi->railIds;
        tdlg_.option = f.multi->op;
        if (f.type == FeatureType::Sweep) {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.multi->twistExpr.empty() ? "0" : f.multi->twistExpr.c_str());
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.multi->thinExpr.c_str());
            tdlg_.option2 = f.multi->thinSide;
        }
        if (f.type == FeatureType::Loft) {
            tdlg_.loftPicks = f.multi->picks;
            tdlg_.loftSel.clear();   // rebuilt below with the canvas selection
            tdlg_.option2 = f.multi->pointEnd;
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.multi->pointX.c_str());
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.multi->pointY.c_str());
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", f.multi->pointZ.c_str());
            tdlg_.selA = (int)f.multi->centerId;
        }
    } else if (f.type == FeatureType::Move && f.move) {
        tdlg_.option = f.move->op;
        if (f.move->op == 1) {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.move->sxExpr.c_str());
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.move->syExpr.c_str());
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", f.move->szExpr.c_str());
        } else {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.move->xExpr.c_str());
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.move->yExpr.c_str());
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", f.move->zExpr.c_str());
            std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", f.move->rxExpr.c_str());
            std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", f.move->ryExpr.c_str());
            std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", f.move->rzExpr.c_str());
        }
        tdlg_.option2 = f.move->copy ? 1 : 0;
        tdlg_.targets = f.move->targets;
    } else if (f.type == FeatureType::ConstructPlane && f.cplane) {
        tdlg_.option = f.cplane->base;
        tdlg_.option2 = f.cplane->axis;
        tdlg_.selA = (int)f.cplane->refPlane;
        if (f.cplane->kind == 3) {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.cplane->x1.c_str());
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.cplane->y1.c_str());
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", f.cplane->z1.c_str());
            std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", f.cplane->x2.c_str());
            std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", f.cplane->y2.c_str());
            std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", f.cplane->z2.c_str());
            std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%s", f.cplane->x3.c_str());
            std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%s", f.cplane->y3.c_str());
            std::snprintf(tdlg_.v9, sizeof tdlg_.v9, "%s", f.cplane->z3.c_str());
        } else if (f.cplane->kind == 4) {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.cplane->angleExpr.c_str());
            if (f.cplane->faceRef.first) {
                tdlg_.targets = {f.cplane->faceRef};
                tdlg_.selA = (int)f.cplane->faceSub;
                tdlg_.faceTotal = f.cplane->faceTotal;
            }
        } else {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.cplane->offsetExpr.c_str());
        }
    } else if (f.type == FeatureType::ConstructAxis && f.caxis) {
        tdlg_.option = f.caxis->kind;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.caxis->ax.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.caxis->ay.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", f.caxis->az.c_str());
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", f.caxis->bx.c_str());
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%s", f.caxis->by.c_str());
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%s", f.caxis->bz.c_str());
        if (f.caxis->kind != 0 && f.caxis->ref.first) {
            tdlg_.targets = {f.caxis->ref};
            tdlg_.selA = (int)f.caxis->sub;
            if (f.caxis->kind == 1) tdlg_.edgeTotal = f.caxis->subTotal;
            else tdlg_.faceTotal = f.caxis->subTotal;
        }
    } else if (f.type == FeatureType::ConstructPoint && f.cpoint) {
        tdlg_.option = f.cpoint->kind;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", f.cpoint->x.c_str());
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", f.cpoint->y.c_str());
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", f.cpoint->z.c_str());
        if (f.cpoint->kind != 0 && f.cpoint->ref.first) {
            tdlg_.targets = {f.cpoint->ref};
            tdlg_.selA = (int)f.cpoint->sub;
            if (f.cpoint->kind == 2) tdlg_.faceTotal = f.cpoint->subTotal;
            else tdlg_.edgeTotal = f.cpoint->subTotal;
        }
    } else if (f.type == FeatureType::Remove && f.remove) {
        tdlg_.targets = f.remove->targets;
    } else if (f.type == FeatureType::Gear && f.gear) {
        tdlg_.gear = *f.gear;
    } else if (f.type == FeatureType::Thread && f.thread) {
        tdlg_.thread = *f.thread;
    } else if (f.type == FeatureType::Wing && f.wing) {
        tdlg_.wing = *f.wing;
    } else if (f.type == FeatureType::Dovetail && f.dovetail) {
        tdlg_.dovetail = *f.dovetail;
        tdlg_.targets = f.dovetail->targets;
    } else if (f.type == FeatureType::Blade && f.blade) {
        tdlg_.blade = *f.blade;
    }
}

// Called by editFeature (undo already pushed, timeline already rolled to the feature).
void App::editToolFeature(Feature& f) {
    tdlg_ = ToolDlg{};
    tdlg_.open = true;
    tdlg_.editing = true;
    tdlg_.featureId = f.id;
    tdlg_.type = f.type;
    tdlg_.focus = false;
    fillToolDlgFromParams(f);
    int kind = kindOfType(f.type);
    activePickFilter_ = kind == 0 || kind == 1 ? (PickEdge | PickFace) : kind >= 2 ? PickFace : bodyPickTool(f.type) ? PickBody : 0;
    if (f.type == FeatureType::Pattern && f.pattern && (f.pattern->circular || f.pattern->onPath))
        activePickFilter_ = PickBody | PickEdge;
    if (f.type == FeatureType::Rib || f.type == FeatureType::Web || f.type == FeatureType::Emboss || f.type == FeatureType::Boss || f.type == FeatureType::Snap || f.type == FeatureType::Rest || f.type == FeatureType::Lip || f.type == FeatureType::Silhouette) activePickFilter_ = PickBody;
    if (f.type == FeatureType::Thread) activePickFilter_ = PickFace;

    // Show the feature's picks as the canvas selection, on the bodies it started from.
    clearSelection();
    auto bodyOf = [&](const TargetRef& r) -> uint32_t {
        std::vector<uint32_t> ids = doc_.featureBodies(r.first);
        return r.second < ids.size() ? ids[r.second] : 0;
    };
    if (kind >= 0) {
        for (auto& e : tdlg_.edges)
            if (e.first < tdlg_.targets.size()) {
                uint32_t b = bodyOf(tdlg_.targets[e.first]);
                if (b) selEdges_.insert({b, e.second});
            }
        for (auto& fc : tdlg_.faces)
            if (fc.first < tdlg_.targets.size()) {
                uint32_t b = bodyOf(tdlg_.targets[fc.first]);
                if (b) selFaces_.insert({b, fc.second});
            }
        if (f.type == FeatureType::Shell && tdlg_.faces.empty() && !tdlg_.targets.empty()) {
            uint32_t b = bodyOf(tdlg_.targets[0]);
            if (b) selection_.insert(b);
        }
    } else if (bodyPickTool(f.type)) {
        for (auto& r : tdlg_.targets)
            if (uint32_t b = bodyOf(r)) { selection_.insert(b); selOrder_.push_back(b); }
        for (auto& r : tdlg_.tools)
            if (uint32_t b = bodyOf(r)) { selection_.insert(b); selOrder_.push_back(b); }
    } else if (f.type == FeatureType::Thread && tdlg_.thread.face.feature) {
        if (uint32_t b = bodyOf({tdlg_.thread.face.feature, tdlg_.thread.face.slot})) selFaces_.insert({b, tdlg_.thread.face.face});
    } else if (f.type == FeatureType::Loft) {
        // The clicked sections again, in their order (a pick that no longer resolves is dropped).
        activePickFilter_ = PickProfile | PickFace;
        std::vector<LoftPick> kept;
        tdlg_.loftSel.clear();
        for (const LoftPick& lp : tdlg_.loftPicks) {
            std::pair<int, SubRef> sel{-1, {0, 0}};
            if (lp.sketchId) {
                auto sit = doc_.sketches.find(lp.sketchId);
                if (sit != doc_.sketches.end()) {
                    const auto& profs = sketchProfiles(sit->second);
                    for (size_t i = 0; i < profs.size(); ++i)
                        if (sk::pointInProfile(profs[i], lp.pt)) { sel = {0, {lp.sketchId, (uint32_t)i}}; break; }
                }
                if (sel.first == 0) selProfiles_.insert(sel.second);
            } else if (uint32_t b = bodyOf({lp.face.feature, lp.face.slot})) {
                sel = {1, {b, lp.face.face}};
                selFaces_.insert(sel.second);
            }
            if (sel.first < 0) continue;
            kept.push_back(lp);
            tdlg_.loftSel.push_back(sel);
        }
        if (!tdlg_.loftPicks.empty()) tdlg_.loftPicks = kept;
    }
    markSceneDirty();
    if (f.type == FeatureType::PressPull) syncToolFromSelection();
}

void App::applyToolDialog() {
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (fi < 0) return;
    // A typed value moves the arrow too.
    if (manip_.active && manip_.target == 2 && !manip_.dragging) {
        try { manip_.value = std::max(peval::eval(tdlg_.v1, doc_.params), manip_.minValue); } catch (const std::runtime_error&) {}
    }
    Feature& f = doc_.timeline[fi];
    if (f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer || f.type == FeatureType::Shell ||
        f.type == FeatureType::PressPull || f.type == FeatureType::Draft || f.type == FeatureType::ReplaceFace ||
        f.type == FeatureType::Thicken) {
        if (!f.modify) return;
        auto m = std::make_shared<ModifyParams>(*f.modify);
        m->valueExpr = tdlg_.v1;
        if (f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer) m->endExpr = tdlg_.v2;
        m->targets = tdlg_.targets;
        m->edges = tdlg_.edges;
        m->faces = tdlg_.faces;
        m->edgeTotal = tdlg_.edgeTotal;
        m->faceTotal = tdlg_.faceTotal;
        if (f.type == FeatureType::Draft || f.type == FeatureType::ReplaceFace) m->axis = tdlg_.option;
        if (f.type == FeatureType::Shell || f.type == FeatureType::Thicken) m->side = std::clamp(tdlg_.option2, 0, 2);
        if (f.type == FeatureType::Fillet) m->side = tdlg_.option2 == 1 ? 1 : 0;
        f.modify = m;
        if (manip_.active && manip_.target == 2 && !manip_.dragging) {
            try { manip_.value = peval::eval(tdlg_.v1, doc_.params); } catch (const std::runtime_error&) {}
        }
    } else if ((f.type == FeatureType::Rib && f.rib) || (f.type == FeatureType::Web && f.web)) {
        auto r = std::make_shared<RibParams>(f.type == FeatureType::Web ? *f.web : *f.rib);
        r->sketchId = (uint32_t)std::max(tdlg_.selA, 0);
        r->thickExpr = tdlg_.v1;
        r->flip = tdlg_.flag;
        r->side = tdlg_.option == 1 ? 1 : 0;
        r->targets = tdlg_.targets;
        if (f.type == FeatureType::Web) f.web = r;
        else f.rib = r;
    } else if (f.type == FeatureType::Emboss && f.emboss) {
        auto e = std::make_shared<EmbossParams>(*f.emboss);
        e->sketchId = (uint32_t)std::max(tdlg_.selA, 0);
        e->depthExpr = tdlg_.v1;
        e->flip = tdlg_.flag;
        e->deboss = tdlg_.option == 1;
        e->targets = tdlg_.targets;
        f.emboss = e;
    } else if (f.type == FeatureType::Boss && f.boss) {
        auto b = std::make_shared<BossParams>(*f.boss);
        b->xExpr = tdlg_.v1;
        b->yExpr = tdlg_.v2;
        b->zExpr = tdlg_.v3;
        b->diaExpr = tdlg_.v4;
        b->heightExpr = tdlg_.v5;
        b->holeExpr = tdlg_.v6;
        b->axis = std::clamp(tdlg_.option, 0, 2);
        b->targets = tdlg_.targets;
        f.boss = b;
    } else if (f.type == FeatureType::Snap && f.snap) {
        auto s = std::make_shared<SnapParams>(*f.snap);
        s->xExpr = tdlg_.v1;
        s->yExpr = tdlg_.v2;
        s->zExpr = tdlg_.v3;
        s->lengthExpr = tdlg_.v4;
        s->widthExpr = tdlg_.v5;
        s->thickExpr = tdlg_.v6;
        s->hookExpr = tdlg_.v7;
        s->hookLenExpr = tdlg_.v8;
        s->axis = std::clamp(tdlg_.option, 0, 2);
        s->targets = tdlg_.targets;
        f.snap = s;
    } else if (f.type == FeatureType::Rest && f.rest) {
        auto r = std::make_shared<RestParams>(*f.rest);
        r->sketchId = (uint32_t)std::max(tdlg_.selA, 0);
        r->thickExpr = tdlg_.v1;
        r->flip = tdlg_.flag;
        r->targets = tdlg_.targets;
        f.rest = r;
    } else if (f.type == FeatureType::Lip && f.lip) {
        auto lip = std::make_shared<LipParams>(*f.lip);
        lip->sketchId = (uint32_t)std::max(tdlg_.selA, 0);
        lip->widthExpr = tdlg_.v1;
        lip->heightExpr = tdlg_.v2;
        lip->flip = tdlg_.flag;
        lip->groove = tdlg_.option == 1;
        lip->targets = tdlg_.targets;
        f.lip = lip;
    } else if (f.type == FeatureType::Silhouette && f.silhouette) {
        auto s = std::make_shared<SilhouetteParams>(*f.silhouette);
        s->axis = std::clamp(tdlg_.option, 0, 2);
        s->targets = tdlg_.targets;
        f.silhouette = s;
    } else if ((f.type == FeatureType::Joint && f.joint) || (f.type == FeatureType::AsBuilt && f.asbuilt)) {
        auto j = std::make_shared<JointParams>(f.type == FeatureType::AsBuilt ? *f.asbuilt : *f.joint);
        j->kind = f.type == FeatureType::AsBuilt ? 0 : std::clamp(tdlg_.option, 0, 2);
        j->ground = tdlg_.targets.empty() ? TargetRef{0, 0} : tdlg_.targets[0];
        j->move = tdlg_.tools.empty() ? TargetRef{0, 0} : tdlg_.tools[0];
        j->gx = tdlg_.v1; j->gy = tdlg_.v2; j->gz = tdlg_.v3;
        j->mx = tdlg_.v4; j->my = tdlg_.v5; j->mz = tdlg_.v6;
        j->angleExpr = tdlg_.v7[0] ? tdlg_.v7 : "0";
        j->axis = std::clamp(tdlg_.option2, 0, 2);
        j->slideExpr = tdlg_.v8[0] ? tdlg_.v8 : "0";
        if (f.type == FeatureType::AsBuilt) f.asbuilt = j;
        else f.joint = j;
    } else if (f.type == FeatureType::Rigid && f.rigid) {
        auto r = std::make_shared<RigidParams>(*f.rigid);
        r->members.clear();
        for (auto& ref : tdlg_.targets)
            if (ref.first && std::find(r->members.begin(), r->members.end(), ref) == r->members.end())
                r->members.push_back(ref);
        f.rigid = r;
    } else if ((f.type == FeatureType::Coil || f.type == FeatureType::Pipe) && f.pathSolid) {
        auto p = std::make_shared<SweepSolidParams>(*f.pathSolid);
        p->diaExpr = tdlg_.v1;
        p->heightExpr = tdlg_.v2;
        if (f.type == FeatureType::Coil) { p->turnsExpr = tdlg_.v3; p->sectionExpr = tdlg_.v4; }
        else p->innerExpr = tdlg_.v3;
        p->axis = tdlg_.option;
        p->leftHand = tdlg_.option2 != 0;
        p->pathSketch = (uint32_t)std::max(tdlg_.selA, 0);
        f.pathSolid = p;
    } else if (f.type == FeatureType::Pattern && f.pattern) {
        auto p = std::make_shared<PatternParams>(*f.pattern);
        p->countExpr = tdlg_.v1;
        p->onPath = tdlg_.option2 == 2;
        p->circular = tdlg_.option2 == 1;
        p->countYExpr = tdlg_.v3;
        p->spacingYExpr = tdlg_.v4;
        p->targets = tdlg_.patFaces ? std::vector<TargetRef>{} : tdlg_.targets;
        p->faceTargets.clear();
        p->faces.clear();
        if (tdlg_.patFaces)
            for (const SubRef& s : tdlg_.patFaceSel) {
                TargetRef r = refOfBody(s.first);
                if (!r.first || r.first == tdlg_.featureId) continue;
                auto it = std::find(p->faceTargets.begin(), p->faceTargets.end(), r);
                uint32_t idx = (uint32_t)(it - p->faceTargets.begin());
                if (it == p->faceTargets.end()) p->faceTargets.push_back(r);
                p->faces.push_back({idx, s.second});
            }
        if (p->onPath) {
            p->spacingExpr = tdlg_.v2;
            p->distType = tdlg_.option == 1 ? 1 : 0;
            p->orient = tdlg_.option3 == 1 ? 1 : 0;
            p->flip = tdlg_.flag;
            p->pathSketch = (uint32_t)std::max(tdlg_.selA, 0);
            p->pathEnt = (uint32_t)std::max(tdlg_.selB, 0);
            p->pathBody = tdlg_.pathBody;
            p->pathEdge = tdlg_.pathEdge;
        } else if (p->circular) {
            p->angleExpr = tdlg_.v2;
            p->axis = tdlg_.option;
            p->axisO = tdlg_.axisO;
            p->axisD = tdlg_.axisD;
        } else {
            p->spacingExpr = tdlg_.v2;
            p->axis = tdlg_.option;
            p->axisO = tdlg_.axisO;
            p->axisD = tdlg_.axisD;
        }
        f.pattern = p;
    } else if (f.type == FeatureType::Mirror && f.mirror) {
        auto mp = std::make_shared<MirrorParams>(*f.mirror);
        mp->plane = tdlg_.option;
        mp->targets = tdlg_.mirrorWaitPlane ? std::vector<TargetRef>{} : tdlg_.targets;
        mp->planeO = tdlg_.axisO;
        mp->planeN = tdlg_.axisD;
        mp->planeName = tdlg_.axisName;
        f.mirror = mp;
    } else if (f.type == FeatureType::Combine && f.combine) {
        auto cp = std::make_shared<CombineParams>(*f.combine);
        cp->op = std::clamp(tdlg_.option, 1, 3);
        cp->targets = tdlg_.targets;
        // Nothing is combined while you are still picking: it happens on OK (or Enter).
        cp->tools = combineFinal_ ? tdlg_.tools : std::vector<TargetRef>{};
        cp->tool = {0, 0};
        cp->keepTools = tdlg_.option2 != 0;
        f.combine = cp;
    } else if (f.type == FeatureType::Split && f.split) {
        auto sp = std::make_shared<SplitParams>(*f.split);
        sp->plane = tdlg_.option;
        sp->targets = tdlg_.targets;
        sp->toolO = tdlg_.axisO;
        sp->toolN = tdlg_.axisD;
        sp->toolR = tdlg_.option == 3 ? tdlg_.axisR : 0.0;
        sp->toolName = tdlg_.axisName;
        sp->sketchId = tdlg_.option == 3 ? (uint32_t)std::max(0, tdlg_.selA) : 0;
        sp->sketchEnt = tdlg_.option == 3 ? (uint32_t)std::max(0, tdlg_.selB) : 0;
        f.split = sp;
    } else if (f.type == FeatureType::SplitFace && f.splitFace) {
        auto m = std::make_shared<SplitFaceParams>(*f.splitFace);
        m->plane = std::clamp(tdlg_.option, 0, 2);
        m->offsetExpr = tdlg_.v1;
        m->targets = tdlg_.targets;
        m->sketchId = (uint32_t)std::max(tdlg_.selA, 0);
        f.splitFace = m;
    } else if (f.type == FeatureType::Align && f.align) {
        auto a = std::make_shared<AlignParams>(tdlg_.align);
        if (a->targets.empty()) a->targets = tdlg_.targets;
        a->angleExpr = tdlg_.v1;
        a->offsetExpr = tdlg_.v2;
        tdlg_.align.angleExpr = a->angleExpr;
        tdlg_.align.offsetExpr = a->offsetExpr;
        f.align = a;
    } else if (f.type == FeatureType::Hole && f.hole) {
        auto hp = std::make_shared<HoleParams>(*f.hole);
        hp->diaExpr = tdlg_.v1;
        hp->depthExpr = tdlg_.v2;
        hp->kind = tdlg_.option;
        if (hp->kind == 1) { hp->cbDiaExpr = tdlg_.v3; hp->cbDepthExpr = tdlg_.v4; }
        if (hp->kind == 2) { hp->csDiaExpr = tdlg_.v3; hp->csAngleExpr = tdlg_.v4; }
        hp->spotDiaExpr = tdlg_.v5;
        hp->spotDepthExpr = tdlg_.v6;
        hp->drillExpr = tdlg_.v7;
        hp->tapped = tdlg_.flag;
        hp->pitchExpr = tdlg_.v8;
        f.hole = hp;
    } else if ((f.type == FeatureType::Sweep || f.type == FeatureType::Loft) && f.multi) {
        auto m = std::make_shared<MultiSketchParams>(*f.multi);
        if (f.type == FeatureType::Sweep && tdlg_.sketchIds.size() >= 2)
            m->sketchIds = {tdlg_.sketchIds[0], tdlg_.sketchIds[1]};
        else
            m->sketchIds = tdlg_.sketchIds;
        m->op = tdlg_.option;
        if (f.type == FeatureType::Sweep) {
            m->twistExpr = tdlg_.v1;
            m->thinExpr = tdlg_.v2;
            m->thinSide = tdlg_.option2;
        }
        if (f.type == FeatureType::Loft || f.type == FeatureType::Sweep) {
            m->railIds.clear();
            for (uint32_t id : tdlg_.railIds)
                if (std::find(m->sketchIds.begin(), m->sketchIds.end(), id) == m->sketchIds.end())
                    m->railIds.push_back(id);
        }
        if (f.type == FeatureType::Loft) {
            m->picks = tdlg_.loftPicks;
            m->pointEnd = tdlg_.option2;
            m->pointX = tdlg_.v1;
            m->pointY = tdlg_.v2;
            m->pointZ = tdlg_.v3;
            m->centerId = tdlg_.selA > 0 ? (uint32_t)tdlg_.selA : 0;
            if (m->pointEnd != 0) { m->railIds.clear(); m->centerId = 0; }
            if (m->centerId) m->railIds.clear();
        }
        f.multi = m;
    } else if (f.type == FeatureType::Move && f.move) {
        auto m = std::make_shared<MoveParams>(*f.move);
        if (tdlg_.option == 1) {
            m->op = 1;
            m->sxExpr = tdlg_.v1;
            m->syExpr = tdlg_.v2;
            m->szExpr = tdlg_.v3;
        } else {
            m->op = 0;
            m->xExpr = tdlg_.v1;
            m->yExpr = tdlg_.v2;
            m->zExpr = tdlg_.v3;
            m->rxExpr = tdlg_.v4[0] ? tdlg_.v4 : "0";
            m->ryExpr = tdlg_.v5[0] ? tdlg_.v5 : "0";
            m->rzExpr = tdlg_.v6[0] ? tdlg_.v6 : "0";
        }
        m->copy = tdlg_.option2 != 0;
        m->targets = tdlg_.targets;
        f.move = m;
    } else if (f.type == FeatureType::ConstructPlane && f.cplane) {
        auto m = std::make_shared<ConstructPlaneParams>(*f.cplane);
        m->base = tdlg_.option;
        m->axis = tdlg_.option2;
        m->refPlane = (uint32_t)std::max(tdlg_.selA, 0);
        if (m->kind == 3) {
            m->x1 = tdlg_.v1; m->y1 = tdlg_.v2; m->z1 = tdlg_.v3;
            m->x2 = tdlg_.v4; m->y2 = tdlg_.v5; m->z2 = tdlg_.v6;
            m->x3 = tdlg_.v7; m->y3 = tdlg_.v8; m->z3 = tdlg_.v9;
        } else if (m->kind == 4) {
            m->angleExpr = tdlg_.v1[0] ? tdlg_.v1 : "0";
            if (!tdlg_.targets.empty()) {
                m->faceRef = tdlg_.targets[0];
                m->faceSub = (uint32_t)std::max(tdlg_.selA, 0);
                m->faceTotal = tdlg_.faceTotal;
            }
        } else {
            m->offsetExpr = tdlg_.v1;
        }
        f.cplane = m;
        updateConstructPlane(f);
    } else if (f.type == FeatureType::ConstructAxis && f.caxis) {
        auto m = std::make_shared<ConstructAxisParams>(*f.caxis);
        m->kind = tdlg_.option;
        m->ax = tdlg_.v1; m->ay = tdlg_.v2; m->az = tdlg_.v3;
        m->bx = tdlg_.v4; m->by = tdlg_.v5; m->bz = tdlg_.v6;
        if (m->kind != 0 && !tdlg_.targets.empty()) {
            m->ref = tdlg_.targets[0];
            m->sub = (uint32_t)std::max(tdlg_.selA, 0);
            m->subTotal = m->kind == 1 ? tdlg_.edgeTotal : tdlg_.faceTotal;
        }
        f.caxis = m;
        updateConstructAxis(f);
    } else if (f.type == FeatureType::ConstructPoint && f.cpoint) {
        auto m = std::make_shared<ConstructPointParams>(*f.cpoint);
        m->kind = tdlg_.option;
        m->x = tdlg_.v1; m->y = tdlg_.v2; m->z = tdlg_.v3;
        if (m->kind != 0 && !tdlg_.targets.empty()) {
            m->ref = tdlg_.targets[0];
            m->sub = (uint32_t)std::max(tdlg_.selA, 0);
            m->subTotal = m->kind == 2 ? tdlg_.faceTotal : tdlg_.edgeTotal;
        }
        f.cpoint = m;
        updateConstructPoint(f);
    } else if (f.type == FeatureType::Remove && f.remove) {
        auto m = std::make_shared<RemoveParams>(*f.remove);
        m->targets = tdlg_.targets;
        f.remove = m;
    } else if (f.type == FeatureType::Gear && f.gear) {
        f.gear = std::make_shared<GearParams>(tdlg_.gear);
    } else if (f.type == FeatureType::Thread && f.thread) {
        f.thread = std::make_shared<ThreadParams>(tdlg_.thread);
    } else if (f.type == FeatureType::Wing && f.wing) {
        f.wing = std::make_shared<WingParams>(tdlg_.wing);
    } else if (f.type == FeatureType::Dovetail && f.dovetail) {
        auto d = std::make_shared<DovetailParams>(tdlg_.dovetail);
        d->targets = tdlg_.targets;
        f.dovetail = d;
    } else if (f.type == FeatureType::Blade && f.blade) {
        f.blade = std::make_shared<BladeParams>(tdlg_.blade);
    } else {
        return;
    }
    doc_.touch();
    dirty_ = true;
    scheduleRecompute(fi);
}

void App::closeToolDialog(bool ok) {
    if (!tdlg_.open) return;
    if (ok && tdlg_.type == FeatureType::Mirror && tdlg_.mirrorWaitPlane) {
        tdlg_.mirrorWaitPlane = false;   // OK without a face: the plane shown in the box
        applyToolDialog();
    }
    if (ok && tdlg_.type == FeatureType::Combine) {
        combineFinal_ = true;
        applyToolDialog();
        combineFinal_ = false;
    }
    tdlg_.open = false;
    holePlace_.active = false;
    manipShiftFid_ = UINT32_MAX;
    holePlace_.valid = false;
    manip_ = Manip{};
    activePickFilter_ = PickBody | PickFace | PickEdge | PickProfile;
    selOrder_.clear();
    if (!ok) {
        savedMarker_ = -1;
        clearSelection();
        undo();   // back to before the command
        return;
    }
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (fi >= 0) {
        Feature& f = doc_.timeline[fi];
        bool empty = false;
        int kind = kindOfType(f.type);
        if (f.type == FeatureType::Fillet && tdlg_.option2 == 1) {
            if (tdlg_.faces.empty() && !tdlg_.editing) {
                savedMarker_ = -1;
                clearSelection();
                undo();
                toast("Click the face to replace");
                return;
            }
            empty = false;
        } else if (kind == 0 || kind == 1) empty = tdlg_.edges.empty();
        else if (kind == 3 || kind == 4 || kind == 5 || kind == 6) empty = tdlg_.faces.empty();
        else if (kind == 2) empty = tdlg_.targets.empty();
        else if (f.type == FeatureType::Dovetail) empty = false;   // makes its own bodies without targets
        else if (f.type == FeatureType::Align) empty = tdlg_.align.from.ref.first == 0 || tdlg_.align.to.ref.first == 0;
        else if (f.type == FeatureType::Pattern && tdlg_.patFaces) empty = tdlg_.patFaceSel.empty();
        else if (bodyPickTool(f.type)) empty = tdlg_.targets.empty();
        else if (f.type == FeatureType::Thread) empty = tdlg_.thread.face.feature == 0;
        else if (f.type == FeatureType::SplitFace && !tdlg_.editing && f.splitFace && f.splitFace->offsetExpr.empty() && f.splitFace->sketchId == 0) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Set where the plane sits");
            return;
        }
        else if (f.type == FeatureType::ConstructAxis && f.caxis) {
            if (f.caxis->kind == 0) {
                auto ev = [&](const std::string& e) {
                    try { return peval::eval(e, doc_.params); }
                    catch (const std::runtime_error&) { return 0.0; }
                };
                glm::dvec3 a(ev(f.caxis->ax), ev(f.caxis->ay), ev(f.caxis->az));
                glm::dvec3 b(ev(f.caxis->bx), ev(f.caxis->by), ev(f.caxis->bz));
                if (glm::length(b - a) < 1e-4) {
                    savedMarker_ = -1;
                    clearSelection();
                    undo();
                    toast("The two points are the same, so no axis was created");
                    return;
                }
            } else if (f.caxis->ref.first == 0) {
                savedMarker_ = -1;
                clearSelection();
                undo();
                toast(f.caxis->kind == 1 ? "Pick a straight edge" : "Pick a round face");
                return;
            }
        }
        else if (f.type == FeatureType::ConstructPoint && f.cpoint && f.cpoint->kind != 0 && f.cpoint->ref.first == 0) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast(f.cpoint->kind == 1 ? "Pick an edge" : f.cpoint->kind == 3 ? "Pick a corner" : "Pick a flat face");
            return;
        }
        else if (f.type == FeatureType::ConstructPlane && f.cplane && f.cplane->kind == 4 && f.cplane->faceRef.first == 0 && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Pick a round face");
            return;
        }
        if (empty && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Nothing was picked, so nothing was created");
            return;
        }
        if (((f.type == FeatureType::Rib && f.rib && !f.rib->sketchId) ||
             (f.type == FeatureType::Web && f.web && !f.web->sketchId) ||
             (f.type == FeatureType::Emboss && f.emboss && !f.emboss->sketchId) ||
             (f.type == FeatureType::Rest && f.rest && !f.rest->sketchId) ||
             (f.type == FeatureType::Lip && f.lip && !f.lip->sketchId)) && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast(f.type == FeatureType::Rib || f.type == FeatureType::Web ? "Pick an open sketch" : "Pick a closed sketch");
            return;
        }
        if (f.type == FeatureType::Boss && f.boss && f.boss->targets.empty() && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Select the body the boss stands on");
            return;
        }
        if (f.type == FeatureType::Snap && f.snap && f.snap->targets.empty() && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Select the body the snap stands on");
            return;
        }
        if (f.type == FeatureType::Rest && f.rest && f.rest->targets.empty() && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Select the body the rest stands on");
            return;
        }
        if (f.type == FeatureType::Lip && f.lip && f.lip->targets.empty() && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Select the body the lip stands on");
            return;
        }
        if (f.type == FeatureType::Silhouette && f.silhouette && f.silhouette->targets.empty() && !tdlg_.editing) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Select the body to split");
            return;
        }
        if (f.type == FeatureType::Joint && f.joint && !tdlg_.editing && (!f.joint->ground.first || !f.joint->move.first)) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Click the body that stays, then the body that moves");
            return;
        }
        if (f.type == FeatureType::AsBuilt && f.asbuilt && !tdlg_.editing && (!f.asbuilt->ground.first || !f.asbuilt->move.first)) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Click the two bodies that are already in place");
            return;
        }
        if (f.type == FeatureType::Rigid && f.rigid && !tdlg_.editing && f.rigid->members.size() < 2) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Pick at least two bodies");
            return;
        }
        if (f.type == FeatureType::Pattern && f.pattern && f.pattern->onPath && !tdlg_.editing &&
            !f.pattern->pathSketch && !f.pattern->pathBody.first) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Pick a sketch curve or an edge to follow");
            return;
        }
        if ((kind == 0 || kind == 1 || kind == 3) && !tdlg_.editing && f.modify) {
            double v = 1;
            try { v = peval::eval(f.modify->valueExpr, doc_.params); } catch (const std::runtime_error&) {}
            if (std::abs(v) < 1e-9) {
                savedMarker_ = -1;
                clearSelection();
                undo();
                toast("No size was set, so nothing was created");
                return;
            }
        }
        if (f.failed && !f.error.empty()) toast(f.name + ": " + f.error, ToastKind::Warning);
    }
    clearSelection();
    restoreMarkerAfterEdit();
}

// ===================================================================== the dialog

void App::drawToolDialog() {
    if (!tdlg_.open) return;
    int fi = doc_.featureIndex(tdlg_.featureId);
    if (fi < 0 || fi >= doc_.markerPos) {
        tdlg_.open = false;
        manip_ = Manip{};
        activePickFilter_ = PickBody | PickFace | PickEdge | PickProfile;
        return;
    }
    Feature& f = doc_.timeline[fi];
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 150 * S, vp->Pos.y + 118 * S), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(320 * S, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 10 * S));
    bool open = true;
    const char* dlgTitle = featureTitle(f.type);
    if (f.type == FeatureType::ConstructPlane && f.cplane)
        dlgTitle = f.cplane->kind == 1 ? "PLANE AT ANGLE###tooldlg" : f.cplane->kind == 2 ? "MIDPLANE###tooldlg" : f.cplane->kind == 3 ? "PLANE THROUGH 3 POINTS###tooldlg" : f.cplane->kind == 4 ? "TANGENT PLANE###tooldlg" : dlgTitle;
    if (f.type == FeatureType::Move && tdlg_.option == 1) dlgTitle = "SCALE###tooldlg";
    ImGui::Begin(dlgTitle, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    auto commit = [this]() { applyToolDialog(); };
    bool ok = false, cancel = false;
    auto enterKey = [] { return ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter); };
    auto label = [&](const char* l) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(l);
        ImGui::SameLine(120 * S);
    };
    auto exprField = [&](const char* l, char* buf, const char* unit, bool live = true) {
        ImGui::PushID(l);
        label(l);
        ImGui::SetNextItemWidth(-40 * S);
        // The Move arrow or ring just clicked puts the cursor in its own field, its value selected,
        // so typing a number goes straight to that axis.
        if (tdlg_.focus || (focusFieldBuf_ && focusFieldBuf_ == buf)) {
            ImGui::SetKeyboardFocusHere();
            tdlg_.focus = false;
            focusFieldBuf_ = nullptr;
        }
        ImGui::InputText("##v", buf, 64, ImGuiInputTextFlags_AutoSelectAll);
        if (live && ImGui::IsItemEdited()) {
            try { peval::eval(buf, doc_.params); commit(); } catch (const std::runtime_error&) {}
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
        if (ImGui::IsItemDeactivated() && enterKey()) ok = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", unit);
        if (buf[0]) {
            try {
                double v = peval::eval(buf, doc_.params);
                if (!peval::plainNumber(buf)) ImGui::TextDisabled("%*s= %s", 28, "", fmtT(v).c_str());
            } catch (const std::runtime_error&) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
                ImGui::TextUnformatted("Not a valid value");
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopID();
    };
    auto combo = [&](const char* l, const char* id, int& v, const char* const* items, int n, int base = 0) {
        label(l);
        ImGui::SetNextItemWidth(-1);
        int cur = std::clamp(v - base, 0, n - 1);
        if (ImGui::Combo(id, &cur, items, n)) { v = cur + base; commit(); }
    };
    auto picks = [&](const char* l, size_t n, const char* noun, const char* prompt) {
        label(l);
        if (n == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 95, 40, 255));
            ImGui::TextUnformatted(prompt);
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%d %s%s", (int)n, noun, n == 1 ? "" : "s");
            ImGui::SameLine();
            ImGui::PushID(l);
            if (ImGui::SmallButton("x")) clearSelection();
            ImGui::PopID();
            ImGui::SetItemTooltip("Clear the picks");
        }
    };

    if (f.failed && !f.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
        ImGui::TextWrapped("%s", f.error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    int kind = kindOfType(f.type);
    if (kind == 0 || kind == 1) {
        if (kind == 0) {
            const char* types[] = {"Constant", "Full round"};
            int before = tdlg_.option2;
            combo("Type", "##filtype", tdlg_.option2, types, 2);
            if (tdlg_.option2 != before) {
                tdlg_.edges.clear();
                tdlg_.faces.clear();
                tdlg_.targets.clear();
                selEdges_.clear();
                selFaces_.clear();
                commit();
            }
            if (tdlg_.option2 == 1) activePickFilter_ = PickFace;
        }
        if (kind == 0 && tdlg_.option2 == 1) {
            picks("Face", tdlg_.faces.size(), "face", "Click the face to replace");
            ImGui::TextDisabled("The round replaces that face and meets\nthe two parallel faces beside it.");
        } else {
        picks("Edges", tdlg_.edges.size(), "edge", "Select edges or faces");
        exprField(kind == 0 ? "Radius" : "Distance", tdlg_.v1, "mm");
        if (kind == 0) {
            exprField("End radius", tdlg_.v2, "mm");
            ImGui::TextDisabled("Leave the end empty for one radius the whole way.");
        } else {
            exprField("Distance 2", tdlg_.v2, "mm");
            ImGui::TextDisabled("Leave this empty for the same distance on both faces.\nSwap the two numbers to swap the faces.");
        }
        label("Tangent Chain");
        ImGui::Checkbox("##tchain", &tangentChain_);
        ImGui::SetItemTooltip("Clicking an edge also picks the edges that flow smoothly from it");
        ImGui::Checkbox("Snap to faces", &settings_.manipSnap);
        ImGui::SetItemTooltip("Dragging the arrow near another flat face stops it on that face's plane.\nHold Alt while dragging to skip snapping.");
        if (kind == 0) ImGui::TextDisabled("Never crashes: if a radius is too big, Kestral\nrounds what fits and tells you the max.");
        }
    } else if (kind == 2) {
        picks("Faces", tdlg_.faces.size(), "face", "Select faces to remove");
        exprField("Thickness", tdlg_.v1, "mm");
        const char* sides[] = {"Inside", "Outside", "Both"};
        combo("Direction", "##shside", tdlg_.option2, sides, 3);
        if (tdlg_.faces.empty() && !tdlg_.targets.empty()) ImGui::TextDisabled("No faces: closed hollow body");
    } else if (kind == 6) {
        picks("Faces", tdlg_.faces.size(), "face", "Select faces to thicken");
        exprField("Thickness", tdlg_.v1, "mm");
        const char* sides[] = {"One Side", "Other Side", "Both"};
        combo("Direction", "##thside", tdlg_.option2, sides, 3);
        ImGui::TextDisabled("A new solid. The body you picked stays.\nOne Side follows the outward normal.");
    } else if (kind == 3) {
        picks("Faces", tdlg_.faces.size(), "face", "Select faces");
        exprField("Distance", tdlg_.v1, "mm");
        ImGui::Checkbox("Snap to faces", &settings_.manipSnap);
        ImGui::SetItemTooltip("Dragging the arrow near another flat face stops it on that face's plane.\nHold Alt while dragging to skip snapping.");
        ImGui::TextDisabled("Drag the arrow or type. Negative goes in.");
    } else if (f.type == FeatureType::Pattern) {
        {
            // Fusion's Pattern Type: whole bodies, or faces (a hole, a pocket, a boss) copied within their body.
            const char* objs[] = {"Bodies", "Faces"};
            int ok = tdlg_.patFaces ? 1 : 0;
            label("Objects");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##patobj", &ok, objs, 2) && (ok == 1) != tdlg_.patFaces) {
                tdlg_.patFaces = ok == 1;
                tdlg_.pickWait = false;
                tdlg_.patFaceSel.clear();
                selFaces_.clear();
                selection_.clear();
                selOrder_.clear();
                tdlg_.targets.clear();
                activePickFilter_ = patternObjectFilter(tdlg_.patFaces, tdlg_.option2);
                commit();
            }
            ImGui::SetItemTooltip("Faces: click the faces of a hole, pocket or boss (a plain hole: its wall; a counterbore: both walls).\nThe copies are cut into, or added onto, that same body.");
        }
        if (tdlg_.patFaces) picks("Faces", tdlg_.patFaceSel.size(), "face", "Click the faces to copy");
        else picks("Objects", tdlg_.targets.size(), "body", "Select bodies");
        const char* kinds[] = {"Rectangular", "Circular", "Along Path"};
        int k = tdlg_.option2;
        label("Type");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##patkind", &k, kinds, 3) && k != tdlg_.option2) {
            tdlg_.option2 = k;
            tdlg_.pickWait = false;
            if (k == 1) {
                std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "6");
                std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "360");
                tdlg_.option = 2;
                tdlg_.pickWait = !tdlg_.targets.empty() || !tdlg_.patFaceSel.empty();
                activePickFilter_ = tdlg_.pickWait ? (PickFace | PickEdge) : patternObjectFilter(tdlg_.patFaces, 1);
            } else if (k == 2) {
                std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "4");
                std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "20");
                tdlg_.option = 0;
                tdlg_.option3 = 0;
                tdlg_.flag = false;
                tdlg_.axisName.clear();
                tdlg_.pickWait = !tdlg_.targets.empty() || !tdlg_.patFaceSel.empty();
                activePickFilter_ = tdlg_.pickWait ? PickEdge : patternObjectFilter(tdlg_.patFaces, 2);
            } else {
                activePickFilter_ = patternObjectFilter(tdlg_.patFaces, 0);
                std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "3");
                std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "20");
            }
            commit();
        }
        if (tdlg_.option2 == 0) {
            exprField("Quantity X", tdlg_.v1, "");
            exprField("Spacing X", tdlg_.v2, "mm");
            exprField("Quantity Y", tdlg_.v3, "");
            exprField("Spacing Y", tdlg_.v4, "mm");
        } else if (tdlg_.option2 == 2) {
            bool picking = tdlg_.pickWait;
            if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
            if (ImGui::Button(picking ? "Click a curve or an edge..." : "Pick path", ImVec2(-1, 0))) {
                tdlg_.pickWait = !tdlg_.pickWait;
                activePickFilter_ = tdlg_.pickWait ? PickEdge : patternObjectFilter(tdlg_.patFaces, 2);
            }
            if (picking) ImGui::PopStyleColor();
            if (!tdlg_.axisName.empty()) ImGui::TextDisabled("%s", tdlg_.axisName.c_str());
            exprField("Quantity", tdlg_.v1, "");
            const char* how[] = {"Spacing", "Extent"};
            combo("Distance Type", "##patdist", tdlg_.option, how, 2);
            exprField("Distance", tdlg_.v2, "mm");
            const char* ori[] = {"Identical", "Along Path"};
            combo("Orientation", "##patori", tdlg_.option3, ori, 2);
            label("Flip");
            if (ImGui::Checkbox("##pflip", &tdlg_.flag)) commit();
        } else {
            // Fusion's two boxes: Objects (the bodies), then Axis (a face or edge to turn around).
            bool picking = tdlg_.pickWait;
            if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
            if (ImGui::Button(picking ? "Click a face or edge to go around..." : "Pick axis (face or edge)", ImVec2(-1, 0))) {
                tdlg_.pickWait = !tdlg_.pickWait;
                activePickFilter_ = tdlg_.pickWait ? (PickFace | PickEdge) : patternObjectFilter(tdlg_.patFaces, 1);
            }
            if (picking) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Pick the bodies (or faces) first, then this, then click a round face (a cylinder, a hole),\na flat face (turns around its normal) or an edge.");
            if (!picking && (!tdlg_.targets.empty() || !tdlg_.patFaceSel.empty()) && tdlg_.option != 3)
                ImGui::TextDisabled(tdlg_.patFaces ? "Faces picked. Now Pick axis." : "Bodies picked. Now the axis.");
            const char* axes[] = {"X Axis", "Y Axis", "Z Axis", "Picked"};
            combo("Axis", "##pataxis", tdlg_.option, axes, tdlg_.option == 3 ? 4 : 3);
            if (tdlg_.option == 3 && !tdlg_.axisName.empty()) ImGui::TextDisabled("%s", tdlg_.axisName.c_str());
            exprField("Quantity", tdlg_.v1, "");
            exprField("Total Angle", tdlg_.v2, "deg");
        }
    } else if (f.type == FeatureType::Mirror) {
        picks("Objects", tdlg_.targets.size(), "body", "Select bodies");
        // The first body picked moves straight on to the plane: the next click is the face.
        if (tdlg_.mirrorWaitPlane && !tdlg_.targets.empty() && tdlg_.option2 == 0 && !tdlg_.mirrorAsked) {
            tdlg_.mirrorAsked = true;
            tdlg_.option2 = 1;
            activePickFilter_ = PickFace;
        }
        const char* planes[] = {"XY Plane", "XZ Plane", "YZ Plane", "Picked face"};
        const char* planesWait[] = {"XY Plane", "XZ Plane", "YZ Plane", "Click a face..."};
        int before = tdlg_.option;
        if (tdlg_.mirrorWaitPlane) {
            int pick = -1;
            label("Mirror Plane");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##planew", "Click a face, or choose...")) {
                for (int i = 0; i < 3; ++i)
                    if (ImGui::Selectable(planesWait[i])) pick = i;
                ImGui::EndCombo();
            }
            if (pick >= 0) {
                tdlg_.option = pick;
                tdlg_.mirrorWaitPlane = false;
                tdlg_.option2 = 0;
                activePickFilter_ = PickBody;
                commit();
            }
        } else {
            combo("Mirror Plane", "##plane", tdlg_.option, planes, tdlg_.option == 3 ? 4 : 3);
        }
        (void)before;
        if (tdlg_.option == 3) ImGui::TextDisabled("%*s%s", 26, "", tdlg_.axisName.c_str());
        label("");
        bool picking = tdlg_.option2 == 1;
        if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
        if (ImGui::Button(picking ? "Click a flat face..." : "Pick a face", ImVec2(-1, 0))) {
            tdlg_.option2 = picking ? 0 : 1;
            activePickFilter_ = tdlg_.option2 ? PickFace : PickBody;
        }
        if (picking) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Mirror across any flat face of a body");
    } else if (f.type == FeatureType::Combine) {
        picks("Target Body", tdlg_.targets.size(), "body", "Select the target body");
        picks("Tool Bodies", tdlg_.tools.size(), "body", "Select tool bodies");
        const char* ops[] = {"Join", "Cut", "Intersect"};
        combo("Operation", "##op", tdlg_.option, ops, 3, 1);
        bool keep = tdlg_.option2 != 0;
        label("Keep Tools");
        if (ImGui::Checkbox("##keep", &keep)) { tdlg_.option2 = keep ? 1 : 0; commit(); }
    } else if (f.type == FeatureType::Split) {
        picks("Body to Split", tdlg_.targets.size(), "body", "Select bodies");
        // Like Fusion: once a body is picked, the next click is the splitting tool.
        if (!tdlg_.targets.empty() && tdlg_.option != 3 && tdlg_.option2 == 0 && !tdlg_.splitManual) {
            tdlg_.option2 = 1;
            activePickFilter_ = PickFace | PickEdge;
        }
        const char* planes[] = {"XY Plane", "XZ Plane", "YZ Plane", "Picked tool"};
        int plane = std::clamp(tdlg_.option, 0, 3);
        label("Splitting tool");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##plane", &plane, planes, tdlg_.option == 3 ? 4 : 3) && plane != tdlg_.option && plane < 3) {
            tdlg_.option = plane;
            tdlg_.selA = tdlg_.selB = 0;
            tdlg_.splitManual = true;
            tdlg_.option2 = 0;
            activePickFilter_ = PickBody;
            commit();
        }
        if (tdlg_.option == 3 && !tdlg_.axisName.empty()) ImGui::TextDisabled("%s", tdlg_.axisName.c_str());
        bool picking = tdlg_.option2 == 1;
        if (picking && viewportHovered_) {
            // Show which sketch line a click takes (it wins over the face under it).
            ImVec2 m = ImGui::GetIO().MousePos;
            float best = 9.f * uiScale_;
            ImVec2 ba, bb;
            bool any = false;
            for (auto& [sid, s] : doc_.sketches) {
                if (!sketchShown(s)) continue;
                for (auto& [eid, e] : s.ents) {
                    if (e.type != sk::EntType::Line) continue;
                    ImVec2 sa, sb;
                    if (!worldToScreen(s.plane.toWorld(s.pt(e.a)), sa) || !worldToScreen(s.plane.toWorld(s.pt(e.b)), sb)) continue;
                    float dx = sb.x - sa.x, dy = sb.y - sa.y, l2 = dx * dx + dy * dy;
                    float u = l2 > 1e-6f ? std::clamp(((m.x - sa.x) * dx + (m.y - sa.y) * dy) / l2, 0.f, 1.f) : 0.f;
                    float d = std::hypot(m.x - (sa.x + dx * u), m.y - (sa.y + dy * u));
                    if (d < best) { best = d; ba = sa; bb = sb; any = true; }
                }
            }
            if (any) {
                ImGui::GetForegroundDrawList()->AddLine(ba, bb, IM_COL32(255, 160, 40, 255), 3.f * uiScale_);
                ImGui::SetTooltip("Sketch line: split along it");
            }
        }
        if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
        if (ImGui::Button(picking ? "Click a line, edge or face..." : "Pick a line, edge or face", ImVec2(-1, 0))) {
            tdlg_.option2 = picking ? 0 : 1;
            tdlg_.splitManual = true;
            activePickFilter_ = tdlg_.option2 ? (PickFace | PickEdge) : PickBody;
        }
        if (picking) ImGui::PopStyleColor();
        {   // Clear feedback: what the split does right now.
            int fi2 = doc_.featureIndex(tdlg_.featureId);
            size_t n = doc_.featureBodies(tdlg_.featureId).size();
            bool failed = fi2 >= 0 && doc_.timeline[fi2].failed;
            if (tdlg_.targets.empty()) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.f), "1. Click the body to split");
            else if (tdlg_.option2 == 1) ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.f, 1.f), "2. Click a sketch line, edge or flat face");
            else if (failed) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", doc_.timeline[fi2].error.c_str());
            else if (recomputeRunning_) ImGui::TextDisabled("Splitting...");
            else if (n <= tdlg_.targets.size()) ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "The tool does not cut the body: nothing split");
            else ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.f), "Splits into %d bodies", (int)n);
        }
        ImGui::SetItemTooltip("A sketch line cuts straight through the body along that line.\nAn edge cuts along the edge. A flat face cuts on the face's plane.");
    } else if (f.type == FeatureType::SplitFace) {
        picks("Body", tdlg_.targets.size(), "body", "Select bodies");
        const char* planes[] = {"XY Plane", "XZ Plane", "YZ Plane"};
        combo("Plane", "##sfpl", tdlg_.option, planes, 3);
        exprField("Offset", tdlg_.v1, "mm");
        auto names = sketchesBeforeMarker();
        label("Sketch curve");
        ImGui::SetNextItemWidth(-1);
        std::string cur = "None";
        for (auto& [id, nm] : names)
            if ((int)id == tdlg_.selA) cur = nm;
        if (ImGui::BeginCombo("##sfsk", cur.c_str())) {
            if (ImGui::Selectable("None", tdlg_.selA == 0)) { tdlg_.selA = 0; commit(); }
            for (auto& [id, nm] : names)
                if (ImGui::Selectable(nm.c_str(), (int)id == tdlg_.selA)) { tdlg_.selA = (int)id; commit(); }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("A sketch curve is projected along the sketch normal.\nEvery face it crosses is divided. The body stays one piece.\nWith no sketch, the plane is used.");
    } else if (f.type == FeatureType::Align) {
        auto row = [&](const char* l, const std::string& v) {
            label(l);
            if (v.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 95, 40, 255));
                ImGui::TextUnformatted("Click the model");
                ImGui::PopStyleColor();
            } else ImGui::TextUnformatted(v.c_str());
        };
        row("From", tdlg_.align.fromName);
        row("To", tdlg_.align.toName);
        label("Flip");
        if (ImGui::Checkbox("##aflip", &tdlg_.align.flip)) commit();
        ImGui::SetItemTooltip("Flat faces point the same way instead of mating.\nCircles and edges turn the other way.");
        exprField("Angle", tdlg_.v1, "deg");
        ImGui::SetItemTooltip("Turns the part about the destination's axis after it lands\n(the face normal, the circle's axis or the edge).");
        exprField("Offset", tdlg_.v2, "mm");
        ImGui::SetItemTooltip("Leaves a gap along the destination's direction.");
        if (ImGui::Button("Pick again", ImVec2(-1, 0))) {
            alignFromShown_ = false;
            tdlg_.align.from = {};
            tdlg_.align.to = {};
            tdlg_.align.fromName.clear();
            tdlg_.align.toName.clear();
            commit();
        }
        ImGui::TextDisabled("Hover to see the snaps: face center or axis, circle\ncenter, edge middle and corners. The orange one is\nwhat a click takes. Flat faces mate, circles share\nan axis. Flip, Angle and Offset fine tune it.");
    } else if (f.type == FeatureType::Remove) {
        picks("Bodies", tdlg_.targets.size(), "body", "Select bodies to remove");
    } else if (f.type == FeatureType::Rib || f.type == FeatureType::Web) {
        bool web = f.type == FeatureType::Web;
        auto names = sketchesBeforeMarker();
        label("Profile");
        ImGui::SetNextItemWidth(-1);
        const char* cur = "pick an open sketch";
        for (auto& [id, nm] : names)
            if ((int)id == tdlg_.selA) cur = nm.c_str();
        if (ImGui::BeginCombo("##ribsk", cur)) {
            for (auto& [id, nm] : names)
                if (ImGui::Selectable(nm.c_str(), (int)id == tdlg_.selA)) { tdlg_.selA = (int)id; commit(); }
            ImGui::EndCombo();
        }
        if (names.empty()) ImGui::TextDisabled("Draw an open sketch first.");
        exprField("Thickness", tdlg_.v1, "mm");
        const char* sides[] = {"Centered", "One side"};
        combo("Thickness side", "##ribside", tdlg_.option, sides, 2);
        label("Flip direction");
        if (ImGui::Checkbox("##ribflip", &tdlg_.flag)) commit();
        ImGui::TextDisabled(web
            ? "The sketch is the side of the wall. Thickness sticks out of\nthe plane, and the wall grows across the plane."
            : "The wall grows along the sketch normal until it meets a body.");
        picks("Body", tdlg_.targets.size(), "body", "Optional. Blank uses the body it hits.");
    } else if (f.type == FeatureType::Emboss) {
        auto names = sketchesBeforeMarker();
        label("Profile");
        ImGui::SetNextItemWidth(-1);
        const char* cur = "pick a closed sketch";
        for (auto& [id, nm] : names)
            if ((int)id == tdlg_.selA) cur = nm.c_str();
        if (ImGui::BeginCombo("##embsk", cur)) {
            for (auto& [id, nm] : names)
                if (ImGui::Selectable(nm.c_str(), (int)id == tdlg_.selA)) { tdlg_.selA = (int)id; commit(); }
            ImGui::EndCombo();
        }
        exprField("Depth", tdlg_.v1, "mm");
        const char* kinds[] = {"Emboss", "Deboss"};
        combo("Effect", "##embkind", tdlg_.option, kinds, 2);
        label("Flip direction");
        if (ImGui::Checkbox("##embflip", &tdlg_.flag)) commit();
        ImGui::TextDisabled("Projects the profile onto a body. Emboss raises it.\nDeboss cuts it in.");
        picks("Body", tdlg_.targets.size(), "body", "Optional. Blank uses the body it hits.");
    } else if (f.type == FeatureType::Boss) {
        exprField("X", tdlg_.v1, "mm");
        exprField("Y", tdlg_.v2, "mm");
        exprField("Z", tdlg_.v3, "mm");
        exprField("Diameter", tdlg_.v4, "mm");
        exprField("Height", tdlg_.v5, "mm");
        exprField("Hole", tdlg_.v6, "mm");
        ImGui::TextDisabled("%*s0 = solid", 26, "");
        const char* axes[] = {"X", "Y", "Z"};
        combo("Direction", "##bossax", tdlg_.option, axes, 3);
        picks("Body", tdlg_.targets.size(), "body", "Select the body it stands on");
    } else if (f.type == FeatureType::Snap) {
        exprField("X", tdlg_.v1, "mm");
        exprField("Y", tdlg_.v2, "mm");
        exprField("Z", tdlg_.v3, "mm");
        exprField("Length", tdlg_.v4, "mm");
        exprField("Width", tdlg_.v5, "mm");
        exprField("Thickness", tdlg_.v6, "mm");
        exprField("Catch", tdlg_.v7, "mm");
        ImGui::TextDisabled("%*s0 = no catch", 26, "");
        exprField("Catch length", tdlg_.v8, "mm");
        const char* axes[] = {"X", "Y", "Z"};
        combo("Direction", "##snapax", tdlg_.option, axes, 3);
        ImGui::TextDisabled("A beam with a lip at the tip. On Z the lip goes toward +X.");
        picks("Body", tdlg_.targets.size(), "body", "Select the body it stands on");
    } else if (f.type == FeatureType::Rest) {
        auto names = sketchesBeforeMarker();
        label("Profile");
        ImGui::SetNextItemWidth(-1);
        const char* cur = "pick a closed sketch";
        for (auto& [id, nm] : names)
            if ((int)id == tdlg_.selA) cur = nm.c_str();
        if (ImGui::BeginCombo("##restsk", cur)) {
            for (auto& [id, nm] : names)
                if (ImGui::Selectable(nm.c_str(), (int)id == tdlg_.selA)) { tdlg_.selA = (int)id; commit(); }
            ImGui::EndCombo();
        }
        if (names.empty()) ImGui::TextDisabled("Draw a closed sketch first.");
        exprField("Thickness", tdlg_.v1, "mm");
        label("Flip direction");
        if (ImGui::Checkbox("##restflip", &tdlg_.flag)) commit();
        ImGui::TextDisabled("The sketch is one face of the pad. Thickness grows\nalong the sketch normal and joins the body.");
        picks("Body", tdlg_.targets.size(), "body", "Select the body it stands on");
    } else if (f.type == FeatureType::Lip) {
        auto names = sketchesBeforeMarker();
        label("Profile");
        ImGui::SetNextItemWidth(-1);
        const char* cur = "pick a closed sketch";
        for (auto& [id, nm] : names)
            if ((int)id == tdlg_.selA) cur = nm.c_str();
        if (ImGui::BeginCombo("##lipsk", cur)) {
            for (auto& [id, nm] : names)
                if (ImGui::Selectable(nm.c_str(), (int)id == tdlg_.selA)) { tdlg_.selA = (int)id; commit(); }
            ImGui::EndCombo();
        }
        if (names.empty()) ImGui::TextDisabled("Draw a closed sketch first.");
        exprField("Width", tdlg_.v1, "mm");
        exprField("Height", tdlg_.v2, "mm");
        const char* kinds[] = {"Lip", "Groove"};
        combo("Effect", "##lipkind", tdlg_.option, kinds, 2);
        label("Flip direction");
        if (ImGui::Checkbox("##lipflip", &tdlg_.flag)) commit();
        ImGui::TextDisabled("The sketch is the outside of the ridge. Width comes in\nfrom that outline. Groove cuts the ridge into the body.");
        picks("Body", tdlg_.targets.size(), "body", "Select the body it stands on");
    } else if (f.type == FeatureType::Silhouette) {
        const char* axes[] = {"X", "Y", "Z"};
        combo("View direction", "##silax", tdlg_.option, axes, 3);
        ImGui::TextDisabled("Cuts the body on the outline seen from that direction.");
        picks("Body", tdlg_.targets.size(), "body", "Select the body to split");
    } else if (f.type == FeatureType::Joint || f.type == FeatureType::AsBuilt) {
        bool built = f.type == FeatureType::AsBuilt;
        picks(built ? "Body 1" : "Stays", tdlg_.targets.size(), "body", built ? "Click the first body" : "Click the body that stays");
        picks(built ? "Body 2" : "Moves", tdlg_.tools.size(), "body", built ? "Click the second body" : "Click the body that moves");
        exprField(built ? "Origin X" : "Stay X", tdlg_.v1, "mm");
        exprField(built ? "Origin Y" : "Stay Y", tdlg_.v2, "mm");
        exprField(built ? "Origin Z" : "Stay Z", tdlg_.v3, "mm");
        exprField(built ? "On body 2 X" : "Move X", tdlg_.v4, "mm");
        exprField(built ? "On body 2 Y" : "Move Y", tdlg_.v5, "mm");
        exprField(built ? "On body 2 Z" : "Move Z", tdlg_.v6, "mm");
        if (!built) {
            const char* kinds[] = {"Rigid", "Revolute", "Slider"};
            combo("Type", "##jntkind", tdlg_.option, kinds, 3);
        }
        if (!built && tdlg_.option == 1) {
            exprField("Angle", tdlg_.v7, "deg");
            const char* axes[] = {"X", "Y", "Z"};
            combo("Axis", "##jntax", tdlg_.option2, axes, 3);
            ImGui::TextDisabled("The moving body lands on the stay point, then turns\nabout that point. Blank points use each center.");
        } else if (!built && tdlg_.option == 2) {
            exprField("Distance", tdlg_.v8, "mm");
            const char* axes[] = {"X", "Y", "Z"};
            combo("Axis", "##jntslide", tdlg_.option2, axes, 3);
            ImGui::TextDisabled("The moving body lands on the stay point, then slides\nalong the axis. Blank points use each center.");
        } else if (built)
            ImGui::TextDisabled("Both bodies stay where they are. The joint is recorded\nat their current positions. Blank points use each center.");
        else
            ImGui::TextDisabled("Blank points use each body's center. The moving body\nshifts so its point lands on the one that stays.");
    } else if (f.type == FeatureType::Rigid) {
        picks("Bodies", tdlg_.targets.size(), "body", "Click the bodies to lock together");
        ImGui::TextDisabled("They stay where they are. Moving one later moves the rest.\nScale does not.");
    } else if (f.type == FeatureType::Hole) {
        bool placed = f.hole && f.hole->sketchId == 0;
        if (placed) drawHoleDialogPlacement();
        else ImGui::TextDisabled("One hole at each circle of the sketch.");
        static const char* kinds[] = {"Simple", "Counterbore", "Countersink"};
        int before = tdlg_.option;
        combo("Type", "##holekind", tdlg_.option, kinds, 3);
        if (tdlg_.option != before) {
            // Sensible sizes for the new type, from the hole's own diameter.
            double d = 5;
            try { if (tdlg_.v1[0]) d = peval::eval(tdlg_.v1, doc_.params); } catch (const std::runtime_error&) {}
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", fmtT(std::round(d * 1.8 * 10) / 10).c_str());
            std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%s", tdlg_.option == 2 ? "90" : fmtT(std::round(d * 0.6 * 10) / 10).c_str());
            commit();
        }
        exprField("Diameter", tdlg_.v1, "mm");
        if (!placed) ImGui::TextDisabled("%*sblank = each circle's own size", 26, "");
        exprField("Depth", tdlg_.v2, "mm");
        ImGui::TextDisabled("%*sblank = all the way through", 26, "");
        if (tdlg_.option == 1) {
            exprField("Counterbore dia.", tdlg_.v3, "mm");
            exprField("Counterbore depth", tdlg_.v4, "mm");
        } else if (tdlg_.option == 2) {
            exprField("Countersink dia.", tdlg_.v3, "mm");
            exprField("Countersink angle", tdlg_.v4, "deg");
        }
        exprField("Spotface dia.", tdlg_.v5, "mm");
        exprField("Spotface depth", tdlg_.v6, "mm");
        ImGui::TextDisabled("%*sblank = no spotface", 26, "");
        exprField("Drill point", tdlg_.v7, "deg");
        ImGui::TextDisabled("%*sblank = flat bottom. Needs a depth.", 26, "");
        bool tap = tdlg_.flag;
        if (ImGui::Checkbox("Tapped", &tap)) {
            tdlg_.flag = tap;
            if (tap && !tdlg_.v8[0]) std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "1.5");
            commit();
        }
        if (tdlg_.flag) {
            exprField("Pitch", tdlg_.v8, "mm");
            ImGui::TextDisabled("ISO metric form, cut into the hole wall.");
        }
    } else if (f.type == FeatureType::Sweep) {
        auto names = sketchesBeforeMarker();
        auto sketchCombo = [&](const char* l, size_t slot) {
            label(l);
            ImGui::SetNextItemWidth(-1);
            const char* cur = "pick a sketch";
            for (auto& [id, nm] : names)
                if (slot < tdlg_.sketchIds.size() && id == tdlg_.sketchIds[slot]) cur = nm.c_str();
            ImGui::PushID(l);
            if (ImGui::BeginCombo("##sk", cur)) {
                for (auto& [id, nm] : names)
                    if (ImGui::Selectable(nm.c_str(), slot < tdlg_.sketchIds.size() && id == tdlg_.sketchIds[slot])) {
                        if (slot >= tdlg_.sketchIds.size()) tdlg_.sketchIds.resize(slot + 1, 0);
                        tdlg_.sketchIds[slot] = id;
                        commit();
                    }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        };
        sketchCombo("Profile", 0);
        sketchCombo("Path", 1);
        exprField("Twist", tdlg_.v1, "deg");
        exprField("Wall", tdlg_.v2, "mm");
        ImGui::TextDisabled("%*sblank = solid from a closed profile", 26, "");
        if (tdlg_.v2[0]) {
            const char* sides[] = {"Center", "One Side", "Other Side"};
            combo("Wall Side", "##thinside", tdlg_.option2, sides, 3);
        }
        label("Guide");
        ImGui::SetNextItemWidth(-1);
        uint32_t guide = tdlg_.railIds.empty() ? 0 : tdlg_.railIds[0];
        std::string guideName = "None";
        for (auto& [id, nm] : names)
            if (id == guide) guideName = nm;
        if (ImGui::BeginCombo("##swguide", guideName.c_str())) {
            if (ImGui::Selectable("None", guide == 0)) { tdlg_.railIds.clear(); commit(); }
            for (auto& [id, nm] : names) {
                bool used = tdlg_.sketchIds.size() > 0 && id == tdlg_.sketchIds[0];
                if (tdlg_.sketchIds.size() > 1 && id == tdlg_.sketchIds[1]) used = true;
                if (used) continue;
                if (ImGui::Selectable(nm.c_str(), id == guide)) { tdlg_.railIds = {id}; commit(); }
            }
            ImGui::EndCombo();
        }
        const char* ops[] = {"New Body", "Join", "Cut", "Intersect"};
        combo("Operation", "##op", tdlg_.option, ops, 4);
        ImGui::TextDisabled("Profile: a closed sketch, or an open curve with a wall.\nPath: one connected chain.\nA guide scales the profile so it meets that curve.\nTwist turns the profile when no guide is set.");
    } else if (f.type == FeatureType::Loft) {
        auto names = sketchesBeforeMarker();
        ImGui::TextUnformatted("Profiles (in order)");
        // Fusion's way: click sketch profiles or flat faces in the canvas, one after another.
        int removeAt = -1;
        for (size_t i = 0; i < tdlg_.loftPicks.size(); ++i) {
            const LoftPick& lp = tdlg_.loftPicks[i];
            std::string what;
            if (lp.sketchId) {
                auto sit = doc_.sketches.find(lp.sketchId);
                what = "Profile in " + (sit != doc_.sketches.end() ? sit->second.name : std::string("a sketch"));
            } else {
                std::vector<uint32_t> ids = doc_.featureBodies(lp.face.feature);
                uint32_t b = lp.face.slot < ids.size() ? ids[lp.face.slot] : 0;
                auto bit = doc_.bodies.find(b);
                what = "Face of " + (bit != doc_.bodies.end() ? bit->second.name : std::string("a body"));
            }
            ImGui::PushID((int)i + 300000);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d. %s", (int)i + 1, what.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 22 * S);
            if (ImGui::SmallButton("x")) removeAt = (int)i;
            ImGui::SetItemTooltip("Take this section out");
            ImGui::PopID();
        }
        if (removeAt >= 0) {
            if ((size_t)removeAt < tdlg_.loftSel.size()) {
                auto it = tdlg_.loftSel[(size_t)removeAt];
                if (it.first == 0) selProfiles_.erase(it.second);
                else selFaces_.erase(it.second);
                tdlg_.loftSel.erase(tdlg_.loftSel.begin() + removeAt);
            }
            tdlg_.loftPicks.erase(tdlg_.loftPicks.begin() + removeAt);
            markSceneDirty();
            commit();
        }
        if (tdlg_.loftPicks.empty() && tdlg_.sketchIds.empty())
            ImGui::TextDisabled("Click sketch profiles or flat faces of bodies,\nin the order the loft goes through them.\n(A sketch must be visible to click its profile.)");
        if (tdlg_.loftPicks.empty() && !tdlg_.sketchIds.empty()) {
            // A loft made before picking: whole sketches, ticked here.
            for (auto& [sid, name] : names) {
                bool on = std::find(tdlg_.sketchIds.begin(), tdlg_.sketchIds.end(), sid) != tdlg_.sketchIds.end();
                ImGui::PushID((int)sid);
                if (ImGui::Checkbox(name.c_str(), &on)) {
                    if (!on) tdlg_.sketchIds.erase(std::remove(tdlg_.sketchIds.begin(), tdlg_.sketchIds.end(), sid), tdlg_.sketchIds.end());
                    else tdlg_.sketchIds.push_back(sid);
                    commit();
                }
                ImGui::PopID();
            }
        }
        size_t nSec = tdlg_.loftPicks.empty() ? tdlg_.sketchIds.size() : tdlg_.loftPicks.size();
        bool pointed = tdlg_.option2 == 1 || tdlg_.option2 == 2;
        if (pointed && nSec == 0) ImGui::TextDisabled("Pick a profile.");
        else if (!pointed && nSec == 1) ImGui::TextDisabled("Now click the next one.");
        const char* ends[] = {"None", "At start", "At end"};
        label("Point");
        ImGui::SetNextItemWidth(-1);
        int pend = std::clamp(tdlg_.option2, 0, 2);
        if (ImGui::Combo("##loftpt", &pend, ends, 3)) {
            tdlg_.option2 = pend;
            if (pend != 0) { tdlg_.railIds.clear(); tdlg_.selA = 0; }
            commit();
        }
        if (pointed) {
            exprField("Point X", tdlg_.v1, "mm");
            exprField("Point Y", tdlg_.v2, "mm");
            exprField("Point Z", tdlg_.v3, "mm");
            ImGui::TextDisabled("The loft comes to this point. One profile is enough.");
        }
        if (!pointed) {
            label("Centerline");
            ImGui::SetNextItemWidth(-1);
            uint32_t center = tdlg_.selA > 0 ? (uint32_t)tdlg_.selA : 0;
            std::string centerName = "None";
            for (auto& [id, nm] : names)
                if (id == center) centerName = nm;
            if (ImGui::BeginCombo("##loftcenter", centerName.c_str())) {
                if (ImGui::Selectable("None", center == 0)) { tdlg_.selA = 0; commit(); }
                for (auto& [id, nm] : names) {
                    if (std::find(tdlg_.sketchIds.begin(), tdlg_.sketchIds.end(), id) != tdlg_.sketchIds.end()) continue;
                    if (ImGui::Selectable(nm.c_str(), id == center)) { tdlg_.selA = (int)id; tdlg_.railIds.clear(); commit(); }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("The middle of each profile follows this curve. The profiles keep their size.");
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Rails");
        bool anyRail = false;
        if (!pointed) {
        for (auto& [sid, name] : names) {
            if (std::find(tdlg_.sketchIds.begin(), tdlg_.sketchIds.end(), sid) != tdlg_.sketchIds.end()) continue;
            bool usedByPick = false;
            for (auto& lp : tdlg_.loftPicks) usedByPick = usedByPick || lp.sketchId == sid;
            if (usedByPick) continue;
            anyRail = true;
            bool on = std::find(tdlg_.railIds.begin(), tdlg_.railIds.end(), sid) != tdlg_.railIds.end();
            ImGui::PushID((int)sid + 100000);
            if (ImGui::Checkbox(name.c_str(), &on)) {
                if (!on) tdlg_.railIds.erase(std::remove(tdlg_.railIds.begin(), tdlg_.railIds.end(), sid), tdlg_.railIds.end());
                else { tdlg_.railIds.push_back(sid); tdlg_.selA = 0; }
                commit();
            }
            ImGui::PopID();
        }
        }
        if (pointed) ImGui::TextDisabled("A point end does not take rails.");
        else if (!anyRail) ImGui::TextDisabled("A rail is an open sketch the loft follows.");
        const char* ops[] = {"New Body", "Join", "Cut", "Intersect"};
        combo("Operation", "##op", tdlg_.option, ops, 4);
    } else if (f.type == FeatureType::Draft) {
        picks("Faces", tdlg_.faces.size(), "face", "Select the side faces");
        exprField("Angle", tdlg_.v1, "deg");
        const char* dirs[] = {"Pull +X", "Pull +Y", "Pull +Z"};
        combo("Direction", "##draftdir", tdlg_.option, dirs, 3);
        ImGui::TextDisabled("The base opposite the pull stays put. The sides lean in.");
    } else if (f.type == FeatureType::ReplaceFace) {
        picks("Faces", tdlg_.faces.size(), "face", "Select flat faces");
        const char* planes[] = {"XY Plane", "XZ Plane", "YZ Plane"};
        combo("Plane", "##repl", tdlg_.option, planes, 3);
        exprField("Offset", tdlg_.v1, "mm");
        ImGui::TextDisabled("The faces move onto that plane. Offset is along the plane normal.");
    } else if (f.type == FeatureType::Coil) {
        exprField("Diameter", tdlg_.v1, "mm");
        exprField("Height", tdlg_.v2, "mm");
        exprField("Revolutions", tdlg_.v3, "");
        exprField("Section", tdlg_.v4, "mm");
        const char* axes[] = {"X Axis", "Y Axis", "Z Axis"};
        combo("Axis", "##coilax", tdlg_.option, axes, 3);
        bool left = tdlg_.option2 != 0;
        label("Left Hand");
        if (ImGui::Checkbox("##coilhand", &left)) { tdlg_.option2 = left ? 1 : 0; commit(); }
        ImGui::TextDisabled("Diameter is the centerline. Section is the wire.");
    } else if (f.type == FeatureType::Pipe) {
        exprField("Outside", tdlg_.v1, "mm");
        exprField("Inside", tdlg_.v3, "mm");
        exprField("Length", tdlg_.v2, "mm");
        const char* axes[] = {"X Axis", "Y Axis", "Z Axis"};
        combo("Axis", "##pipeax", tdlg_.option, axes, 3);
        auto names = sketchesBeforeMarker();
        label("Path");
        ImGui::SetNextItemWidth(-1);
        std::string cur = "Straight line";
        for (auto& [sid, name] : names)
            if ((int)sid == tdlg_.selA) cur = name;
        if (ImGui::BeginCombo("##pipepath", cur.c_str())) {
            if (ImGui::Selectable("Straight line", tdlg_.selA == 0)) { tdlg_.selA = 0; commit(); }
            for (auto& [sid, name] : names)
                if (ImGui::Selectable(name.c_str(), (int)sid == tdlg_.selA)) { tdlg_.selA = (int)sid; commit(); }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Inside 0 is solid. A path sketch replaces the length.");
    } else if (f.type == FeatureType::Move) {
        picks("Objects", tdlg_.targets.size(), "body", "Select bodies");
        if (tdlg_.option == 1) {
            exprField("Scale X", tdlg_.v1, "");
            exprField("Scale Y", tdlg_.v2, "");
            exprField("Scale Z", tdlg_.v3, "");
            ImGui::TextDisabled("1 leaves that axis as it is. The pivot is the body center.");
        } else {
            exprField("X Distance", tdlg_.v1, "mm");
            exprField("Y Distance", tdlg_.v2, "mm");
            exprField("Z Distance", tdlg_.v3, "mm");
            bool axisTurn = f.move && f.move->angExpr != "0" && !f.move->angExpr.empty() &&
                            (f.move->axExpr != "0" || f.move->ayExpr != "0" || f.move->azExpr != "0");
            if (axisTurn) {
                ImGui::TextDisabled("Turned %s deg about (%s, %s, %s).", f.move->angExpr.c_str(), f.move->axExpr.c_str(),
                                    f.move->ayExpr.c_str(), f.move->azExpr.c_str());
            } else {
                exprField("X Angle", tdlg_.v4, "deg");
                exprField("Y Angle", tdlg_.v5, "deg");
                exprField("Z Angle", tdlg_.v6, "deg");
                ImGui::TextDisabled(f.move && f.move->together
                    ? "Drag the rings to turn. All picked bodies turn as one,\nabout the center of all of them."
                    : "Turns about the body center, X first, then Y, then Z.");
            }
        }
        bool copy = tdlg_.option2 != 0;
        label("Create Copy");
        if (ImGui::Checkbox("##copy", &copy)) { tdlg_.option2 = copy ? 1 : 0; commit(); }
    } else if (f.type == FeatureType::ConstructPlane) {
        int kind = f.cplane ? f.cplane->kind : 0;
        if (kind == 3) {
            exprField("Point 1 X", tdlg_.v1, "mm");
            exprField("Point 1 Y", tdlg_.v2, "mm");
            exprField("Point 1 Z", tdlg_.v3, "mm");
            exprField("Point 2 X", tdlg_.v4, "mm");
            exprField("Point 2 Y", tdlg_.v5, "mm");
            exprField("Point 2 Z", tdlg_.v6, "mm");
            exprField("Point 3 X", tdlg_.v7, "mm");
            exprField("Point 3 Y", tdlg_.v8, "mm");
            exprField("Point 3 Z", tdlg_.v9, "mm");
            ImGui::TextDisabled("The first point is the origin. The three points\nmust not lie on one line.");
        } else if (kind == 4) {
            activePickFilter_ = PickFace;
            exprField("Angle", tdlg_.v1, "deg");
            ImGui::TextDisabled(tdlg_.targets.empty()
                ? "Click a cylinder face. The plane sits against it."
                : "The plane is tangent to that cylinder. Angle picks the side.");
        } else {
        const char* bases[] = {"XY Plane", "XZ Plane", "YZ Plane"};
        combo("Plane", "##cplbase", tdlg_.option, bases, 3);
        if (kind == 1) {
            const char* axes[] = {"Along X", "Along Y"};
            combo("Axis", "##cplax", tdlg_.option2, axes, 2);
            exprField("Angle", tdlg_.v1, "deg");
        } else if (kind == 2) {
            label("Other plane");
            ImGui::SetNextItemWidth(-1);
            std::string cur = "Pick a plane";
            std::vector<std::pair<uint32_t, std::string>> planes;
            for (auto& ft : doc_.timeline) {
                if (ft.id == f.id) break;
                if (ft.type == FeatureType::ConstructPlane) planes.push_back({ft.id, ft.name});
            }
            for (auto& [id, name] : planes)
                if ((int)id == tdlg_.selA) cur = name;
            if (ImGui::BeginCombo("##midother", cur.c_str())) {
                for (auto& [id, name] : planes)
                    if (ImGui::Selectable(name.c_str(), (int)id == tdlg_.selA)) { tdlg_.selA = (int)id; commit(); }
                ImGui::EndCombo();
            }
            if (planes.empty()) ImGui::TextDisabled("Create the other plane first.");
        } else {
            exprField("Distance", tdlg_.v1, "mm");
        }
        }
    } else if (f.type == FeatureType::ConstructAxis) {
        const char* kinds[] = {"Two Points", "Edge", "Cylinder"};
        int before = tdlg_.option;
        combo("Type", "##axkind", tdlg_.option, kinds, 3);
        if (tdlg_.option != before) {
            tdlg_.targets.clear();
            tdlg_.selA = 0;
            selEdges_.clear();
            selFaces_.clear();
            activePickFilter_ = tdlg_.option == 1 ? PickEdge : tdlg_.option == 2 ? PickFace : 0;
            commit();
        }
        if (tdlg_.option == 0) {
            exprField("Point 1 X", tdlg_.v1, "mm");
            exprField("Point 1 Y", tdlg_.v2, "mm");
            exprField("Point 1 Z", tdlg_.v3, "mm");
            exprField("Point 2 X", tdlg_.v4, "mm");
            exprField("Point 2 Y", tdlg_.v5, "mm");
            exprField("Point 2 Z", tdlg_.v6, "mm");
            ImGui::TextDisabled("The axis runs through both points.");
        } else if (tdlg_.option == 1) {
            ImGui::TextDisabled(tdlg_.targets.empty() ? "Click a straight edge." : "The axis runs along that edge.");
        } else {
            ImGui::TextDisabled(tdlg_.targets.empty() ? "Click a round face." : "The axis runs through the center of that face.");
        }
    } else if (f.type == FeatureType::ConstructPoint) {
        const char* kinds[] = {"Coordinates", "Edge", "Face", "Corner"};
        int before = tdlg_.option;
        combo("Type", "##ptkind", tdlg_.option, kinds, 4);
        if (tdlg_.option != before) {
            tdlg_.targets.clear();
            tdlg_.selA = 0;
            selEdges_.clear();
            selFaces_.clear();
            commit();
        }
        activePickFilter_ = tdlg_.option == 1 ? PickEdge : tdlg_.option == 2 ? PickFace : tdlg_.option == 3 ? PickVertex : 0;
        if (tdlg_.option == 0) {
            exprField("X", tdlg_.v1, "mm");
            exprField("Y", tdlg_.v2, "mm");
            exprField("Z", tdlg_.v3, "mm");
        } else if (tdlg_.option == 1) {
            ImGui::TextDisabled(tdlg_.targets.empty() ? "Click an edge." : "The point sits at the middle of that edge.");
        } else if (tdlg_.option == 2) {
            ImGui::TextDisabled(tdlg_.targets.empty() ? "Click a flat face." : "The point sits at the center of that face.");
        } else {
            ImGui::TextDisabled(tdlg_.targets.empty() ? "Click a corner." : "The point sits on that corner.");
        }
    } else if (f.type == FeatureType::Gear) {
        drawGearDialog(ok);
    } else if (f.type == FeatureType::Thread) {
        drawThreadDialog(ok);
    } else if (f.type == FeatureType::Wing) {
        drawWingDialog(ok);
    } else if (f.type == FeatureType::Dovetail) {
        drawDovetailDialog(ok);
    } else if (f.type == FeatureType::Blade) {
        drawBladeDialog(ok);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    float bw = (ImGui::GetContentRegionAvail().x - 8 * S) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.55f, 0.96f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("OK", ImVec2(bw, 28 * S))) ok = true;
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 8 * S);
    if (ImGui::Button("Cancel", ImVec2(bw, 28 * S))) cancel = true;
    ImGui::End();
    ImGui::PopStyleVar();
    if (!open) cancel = true;
    if (ok) closeToolDialog(true);
    else if (cancel) closeToolDialog(false);
}

} // namespace kst

namespace kst {

// The snaps on what the mouse is over, and the one a click takes: the nearest snap on screen when
// the mouse is close to it, else the face's center (or axis) or the edge's middle (or circle center).
bool App::alignSnapAt(AlignSnap& out, std::vector<AlignSnap>* cands) {
    if (!(hov_.kind == HoverKind::Face || hov_.kind == HoverKind::Edge)) return false;
    uint32_t body = pickBodyFor(hov_.body);   // a preview body stands for the one it copies
    auto bit = doc_.bodies.find(body);
    if (bit == doc_.bodies.end()) return false;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return false;
    TargetRef ref = refOfBody(body);
    if (!ref.first || ref.first == tdlg_.featureId) return false;
    const ShapePtr& shape = pit->second.shape;
    const glm::dmat4 xf(bit->second.xform);
    const std::string& bn = bit->second.name;
    std::vector<AlignSnap> all;
    AlignSnap fallback;
    bool haveFallback = false;
    uint32_t eTotal = kernel::edgeCount(shape);
    auto addEdge = [&](uint32_t e, bool asDefault) {
        kernel::EdgeGeom g;
        if (!kernel::edgeGeom(shape, bit->second.xform, e, g)) return;
        AlignSnap s;
        s.pick.ref = ref;
        s.pick.sub = e;
        s.pick.total = eTotal;
        if (g.type == 1 || g.type == 2) {
            s.pick.kind = 1;
            s.label = (g.type == 1 ? "Center of circle on " : "Center of arc on ") + bn;
            s.world = g.center;
            all.push_back(s);
            if (asDefault && !haveFallback) { fallback = s; haveFallback = true; }
        } else {
            s.pick.kind = 1;
            s.label = "Middle of edge on " + bn;
            s.world = g.type == 0 ? (g.a + g.b) * 0.5 : (!g.pts.empty() ? g.pts[g.pts.size() / 2] : (g.a + g.b) * 0.5);
            all.push_back(s);
            if (asDefault && !haveFallback) { fallback = s; haveFallback = true; }
        }
        if (g.type != 1) {
            for (int end = 1; end <= 2; ++end) {
                AlignSnap c;
                c.pick.ref = ref;
                c.pick.kind = 2;
                c.pick.sub = e;
                c.pick.total = eTotal;
                c.pick.end = end;
                c.label = "Corner of " + bn;
                c.world = end == 1 ? g.a : g.b;
                all.push_back(c);
            }
        }
    };
    if (hov_.kind == HoverKind::Face) {
        glm::dvec3 c, n, axO, axD;
        bool planar = false;
        AlignSnap s;
        s.pick.ref = ref;
        s.pick.kind = 0;
        s.pick.sub = hov_.index;
        s.pick.total = kernel::faceCount(shape);
        if (kernel::faceRoundAxis(shape, hov_.index, axO, axD)) {
            s.label = "Axis of " + bn;
            s.world = glm::dvec3(xf * glm::dvec4(axO, 1.0));
        } else if (kernel::faceInfo(shape, hov_.index, c, n, planar)) {
            s.label = std::string(planar ? "Center of face on " : "Center of curved face on ") + bn;
            s.world = glm::dvec3(xf * glm::dvec4(c, 1.0));
        } else {
            return false;
        }
        all.push_back(s);
        fallback = s;
        haveFallback = true;
        std::vector<uint32_t> fe = kernel::faceEdges(shape, hov_.index);
        for (size_t i = 0; i < fe.size() && i < 200; ++i) addEdge(fe[i], false);
    } else {
        addEdge(hov_.index, true);
    }
    if (!haveFallback) return false;
    // The nearest snap on screen wins when the mouse is right on it.
    ImVec2 m = ImGui::GetIO().MousePos;
    float best = 14.f * uiScale_;
    const AlignSnap* pick = nullptr;
    for (auto& s : all) {
        ImVec2 q;
        if (!worldToScreen(s.world, q)) continue;
        float d = std::hypot(m.x - q.x, m.y - q.y);
        if (d < best) { best = d; pick = &s; }
    }
    out = pick ? *pick : fallback;
    if (cands) *cands = std::move(all);
    return true;
}

// Fusion style snap glyphs while Align waits for a pick: every snap on the hovered face or edge,
// the one a click takes in orange with its name, and the "from" point once it is chosen.
void App::drawAlignSnaps() {
    if (!tdlg_.open || tdlg_.type != FeatureType::Align) { alignFromShown_ = false; return; }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float S = uiScale_;
    if (alignFromShown_ && tdlg_.align.from.ref.first && !tdlg_.align.to.ref.first) {
        ImVec2 q;
        if (worldToScreen(alignFromWorld_, q)) {
            dl->AddCircleFilled(q, 5.5f * S, IM_COL32(40, 160, 255, 240));
            dl->AddCircle(q, 5.5f * S, IM_COL32(255, 255, 255, 240), 0, 1.5f * S);
        }
    }
    if (!viewportHovered_ || (tdlg_.align.from.ref.first && tdlg_.align.to.ref.first)) return;
    AlignSnap cur;
    std::vector<AlignSnap> cands;
    if (!alignSnapAt(cur, &cands)) return;
    for (auto& s : cands) {
        ImVec2 q;
        if (!worldToScreen(s.world, q)) continue;
        dl->AddCircleFilled(q, 3.f * S, IM_COL32(255, 255, 255, 200));
        dl->AddCircle(q, 3.f * S, IM_COL32(40, 40, 40, 200), 0, 1.f * S);
    }
    ImVec2 q;
    if (worldToScreen(cur.world, q)) {
        dl->AddCircleFilled(q, 6.f * S, IM_COL32(255, 107, 53, 245));
        dl->AddCircle(q, 6.f * S, IM_COL32(255, 255, 255, 245), 0, 1.5f * S);
        ImVec2 tp(q.x + 10 * S, q.y - 22 * S);
        ImVec2 ts = ImGui::CalcTextSize(cur.label.c_str());
        dl->AddRectFilled(ImVec2(tp.x - 4 * S, tp.y - 2 * S), ImVec2(tp.x + ts.x + 4 * S, tp.y + ts.y + 2 * S), IM_COL32(25, 25, 28, 220), 3 * S);
        dl->AddText(tp, IM_COL32(255, 255, 255, 255), cur.label.c_str());
    }
}

} // namespace kst
