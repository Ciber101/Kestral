// Gear generator and thread tool: feature values, dialogs, face picking and cosmetic thread display.
#include "app/App.h"
#include "app/MechTools.h"
#include "kernel/Features.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kst {

namespace {

const double kPi = 3.14159265358979323846;

const char* kGearTypes[] = {"Spur", "Helical", "Herringbone", "Internal (ring)", "Rack", "Bevel", "Worm", "Worm wheel"};

std::string fmtV(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

double ev(const std::string& e, const ParamMap& p, const char* what) {
    try {
        return peval::eval(e, p);
    } catch (const std::runtime_error&) {
        throw std::runtime_error(std::string(what) + ": \"" + e + "\" is not a valid value");
    }
}

bool usesHelix(int t) {
    auto g = (kernel::GearType)t;
    return g == kernel::GearType::Helical || g == kernel::GearType::Herringbone || g == kernel::GearType::Internal ||
           g == kernel::GearType::Rack;
}
bool usesHand(int t) { return usesHelix(t) || t == (int)kernel::GearType::Worm || t == (int)kernel::GearType::WormWheel; }

} // namespace

// ===================================================================== values -> specs

const char* gearTypeName(int type) { return type >= 0 && type < (int)kernel::GearType::Count ? kGearTypes[type] : "Gear"; }

GearParams defaultGear() {
    GearParams g;
    g.vals = {{"module", "2"}, {"dp", "12"}, {"teeth", "24"}, {"pressure", "20"}, {"width", "10"}, {"bore", "5"},
              {"helix", "20"}, {"backlash", "0"}, {"shift", "0"}, {"rim", "5"}, {"mate", "24"}, {"starts", "1"},
              {"wormDia", "20"}, {"length", "40"}, {"x", "0"}, {"y", "0"}};
    return g;
}

void gearTypeDefaults(GearParams& g, int t) {
    auto type = (kernel::GearType)t;
    if (type == kernel::GearType::Helical || type == kernel::GearType::Herringbone) {
        if (g.val("helix", "0") == "0") g.vals["helix"] = "20";
    } else if (type == kernel::GearType::Internal || type == kernel::GearType::Rack) {
        g.vals["helix"] = "0";
    }
    if (type == kernel::GearType::Rack && g.val("teeth", "24") == "24") g.vals["teeth"] = "12";
    if (type == kernel::GearType::WormWheel && g.val("teeth", "24") == "24") g.vals["teeth"] = "30";
    if (type == kernel::GearType::Internal && g.val("teeth", "24") == "24") g.vals["teeth"] = "48";
    g.type = t;
}

kernel::GearSpec gearSpecOf(const GearParams& g, const ParamMap& p) {
    kernel::GearSpec s;
    s.type = (kernel::GearType)std::clamp(g.type, 0, (int)kernel::GearType::Count - 1);
    if (g.sizeBy == 1) {
        double dp = ev(g.val("dp", "12"), p, "Diametral pitch");
        if (dp <= 0) throw std::runtime_error("The diametral pitch must be above 0");
        s.module = 25.4 / dp;
    } else {
        s.module = ev(g.val("module", "2"), p, "Module");
    }
    s.teeth = (int)std::lround(ev(g.val("teeth", "24"), p, "Teeth"));
    s.pressureDeg = ev(g.val("pressure", "20"), p, "Pressure angle");
    s.width = ev(g.val("width", "10"), p, "Face width");
    s.bore = ev(g.val("bore", "0"), p, "Bore");
    s.helixDeg = ev(g.val("helix", "20"), p, "Helix angle");
    s.backlash = ev(g.val("backlash", "0"), p, "Backlash");
    s.shift = ev(g.val("shift", "0"), p, "Profile shift");
    s.rim = ev(g.val("rim", "5"), p, "Rim");
    s.mateTeeth = (int)std::lround(ev(g.val("mate", "24"), p, "Mating teeth"));
    s.starts = (int)std::lround(ev(g.val("starts", "1"), p, "Starts"));
    s.wormDia = ev(g.val("wormDia", "20"), p, "Worm pitch diameter");
    s.length = ev(g.val("length", "40"), p, "Length");
    s.leftHand = g.leftHand;
    if (s.teeth > 1000) throw std::runtime_error("Over 1000 teeth: use fewer");
    return s;
}

glm::mat4 gearPlacement(const GearParams& g, const ParamMap& p) {
    double x = ev(g.val("x", "0"), p, "Center X"), y = ev(g.val("y", "0"), p, "Center Y");
    glm::dvec3 ax{1, 0, 0}, ay{0, 1, 0};
    if (g.plane == 1) { ax = {1, 0, 0}; ay = {0, 0, 1}; }
    else if (g.plane == 2) { ax = {0, 1, 0}; ay = {0, 0, 1}; }
    glm::dvec3 az = glm::cross(ax, ay), o = ax * x + ay * y;
    glm::mat4 m(1.f);
    m[0] = glm::vec4(glm::vec3(ax), 0.f);
    m[1] = glm::vec4(glm::vec3(ay), 0.f);
    m[2] = glm::vec4(glm::vec3(az), 0.f);
    m[3] = glm::vec4(glm::vec3(o), 1.f);
    return m;
}

ThreadSizeInfo threadSize(const ThreadParams& t, const ParamMap& p) {
    const auto& fams = threads::families();
    ThreadSizeInfo out;
    out.family = &fams[std::clamp(t.family, 0, (int)fams.size() - 1)];
    if (t.size >= 0 && t.size < (int)out.family->sizes.size()) {
        const threads::Size& s = out.family->sizes[t.size];
        out.name = s.name;
        out.major = s.major;
        out.pitch = s.pitch;
        out.starts = s.starts;
    } else {
        out.name = "Custom";
        out.major = ev(t.diaExpr, p, "Diameter");
        out.pitch = ev(t.pitchExpr, p, "Pitch");
    }
    if (!t.startsExpr.empty()) out.starts = (int)std::lround(ev(t.startsExpr, p, "Starts"));
    out.starts = std::clamp(out.starts, 1, 12);
    return out;
}

bool threadInternal(const ThreadParams& t) { return t.side == 0 ? t.hole : t.side == 2; }

kernel::ThreadCut threadCutOf(const ThreadParams& t, const kernel::RoundFace& face, const ParamMap& p) {
    ThreadSizeInfo sz = threadSize(t, p);
    if (sz.major <= 0) throw std::runtime_error("The thread diameter must be above 0");
    if (sz.pitch <= 0) throw std::runtime_error("The pitch must be above 0");
    bool internal = threadInternal(t);
    double len = t.fullLength ? face.length : ev(t.lengthExpr, p, "Length");
    double off = t.fullLength ? 0.0 : ev(t.offsetExpr, p, "Offset");
    if (len <= 0) throw std::runtime_error("The thread length must be above 0");
    len = std::min(len, std::max(0.0, face.length - off));
    if (len <= 1e-6) throw std::runtime_error("The offset is past the end of the face");
    kernel::ThreadCut c;
    c.dir = t.flip ? -face.dir : face.dir;
    glm::dvec3 start = t.flip ? face.origin + face.dir * face.length : face.origin;
    c.origin = start + c.dir * off;
    c.length = len;
    c.pitch = sz.pitch;
    c.starts = sz.starts;
    c.leftHand = t.leftHand;
    c.taperDeg = sz.family->taperDeg;
    c.groove = threads::groove(*sz.family, sz.major, sz.pitch, internal, face.radius);
    return c;
}

// ===================================================================== commands

void App::runGear() { startToolFeature(FeatureType::Gear); }

void App::runThread() {
    startToolFeature(FeatureType::Thread);
    if (tdlg_.open && tdlg_.type == FeatureType::Thread && !tdlg_.thread.face.feature)
        toast("Click a round face: the side of a shaft or the inside of a hole");
}

void App::syncThreadFromSelection() {
    ThreadParams& t = tdlg_.thread;
    if (selFaces_.empty()) return;
    // One face: the newest click wins.
    SubRef pick = *selFaces_.begin();
    if (selFaces_.size() > 1) {
        std::vector<uint32_t> cur = doc_.featureBodies(t.face.feature);
        uint32_t curBody = t.face.slot < cur.size() ? cur[t.face.slot] : 0;
        for (auto& s : selFaces_)
            if (!(s.first == curBody && s.second == t.face.face)) pick = s;
        selFaces_ = {pick};
    }
    TargetRef r = refOfBody(pick.first);
    auto bit = doc_.bodies.find(pick.first);
    auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
    if (!r.first || r.first == tdlg_.featureId || pit == doc_.parts.end() || !pit->second.shape) return;
    kernel::RoundFace rf;
    if (!kernel::roundFace(pit->second.shape, bit->second.xform, pick.second, rf)) {
        toast("That face is not round. Pick the side of a shaft or the inside of a hole.", ToastKind::Warning);
        selFaces_.clear();
        return;
    }
    bool changed = t.face.feature != r.first || t.face.slot != r.second || t.face.face != pick.second;
    t.face = {r.first, r.second, pick.second, kernel::faceCount(pit->second.shape)};
    t.axisO = rf.origin;
    t.axisD = rf.dir;
    t.radius = rf.radius;
    t.faceLen = rf.length;
    t.hole = rf.hole;
    if (changed) {
        const auto& fams = threads::families();
        const threads::Family& fam = fams[std::clamp(t.family, 0, (int)fams.size() - 1)];
        int idx = threads::closestSize(fam, rf.radius * 2, threadInternal(t));
        if (idx >= 0 && t.size >= 0) t.size = idx;
        t.lengthExpr = fmtV(std::round(rf.length * 100) / 100);
    }
}

// ===================================================================== dialogs

void App::drawGearDialog(bool& ok) {
    const float S = uiScale_;
    GearParams& g = tdlg_.gear;
    auto apply = [this] { applyToolDialog(); };
    auto label = [&](const char* l) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(l);
        ImGui::SameLine(130 * S);
    };
    auto field = [&](const char* l, const char* key, const char* unit, const char* def) {
        ImGui::PushID(key);
        label(l);
        ImGui::SetNextItemWidth(-40 * S);
        std::string& v = g.vals[key];
        if (v.empty()) v = def;
        ImGui::InputText("##v", &v, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit()) apply();
        if (ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ok = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", unit);
        try {
            double x = peval::eval(v, doc_.params);
            if (!peval::plainNumber(v)) ImGui::TextDisabled("%*s= %s", 30, "", fmtV(x).c_str());
        } catch (const std::runtime_error&) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
            ImGui::TextUnformatted("Not a valid value");
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    };
    auto combo = [&](const char* l, const char* id, int& v, const char* const* items, int n) {
        label(l);
        ImGui::SetNextItemWidth(-1);
        int cur = std::clamp(v, 0, n - 1);
        if (ImGui::Combo(id, &cur, items, n) && cur != v) {
            v = cur;
            return true;
        }
        return false;
    };

    int type = g.type;
    if (combo("Type", "##gtype", type, kGearTypes, (int)kernel::GearType::Count)) {
        gearTypeDefaults(g, type);
        apply();
    }
    auto T = (kernel::GearType)g.type;
    const char* planes[] = {"XY Plane", "XZ Plane", "YZ Plane"};
    if (combo("Plane", "##gplane", g.plane, planes, 3)) apply();
    field("Center X", "x", "mm", "0");
    field("Center Y", "y", "mm", "0");
    ImGui::Separator();
    const char* by[] = {"Module (mm)", "Diametral pitch (1/in)"};
    if (combo("Size by", "##gsize", g.sizeBy, by, 2)) apply();
    if (g.sizeBy == 1) field("Diametral pitch", "dp", "/in", "12");
    else field(T == kernel::GearType::Worm || T == kernel::GearType::WormWheel ? "Axial module" : "Module", "module", "mm", "2");
    if (T != kernel::GearType::Worm) field(T == kernel::GearType::Rack ? "Teeth (length)" : "Teeth", "teeth", "", "24");
    field("Pressure angle", "pressure", "deg", "20");
    if (T == kernel::GearType::Worm) field("Length", "length", "mm", "40");
    else field(T == kernel::GearType::Bevel ? "Face width (cone)" : "Face width", "width", "mm", "10");
    if (usesHelix(g.type)) field("Helix angle", "helix", "deg", "20");
    if (T == kernel::GearType::Internal) field("Ring wall", "rim", "mm", "5");
    if (T == kernel::GearType::Rack) field("Base height", "rim", "mm", "5");
    if (T == kernel::GearType::Bevel) field("Mating teeth", "mate", "", "24");
    if (T == kernel::GearType::Worm || T == kernel::GearType::WormWheel) {
        field("Worm starts", "starts", "", "1");
        field("Worm pitch dia.", "wormDia", "mm", "20");
    }
    if (T != kernel::GearType::Rack && T != kernel::GearType::Internal) field("Bore", "bore", "mm", "0");
    field("Backlash", "backlash", "mm", "0");
    if (T == kernel::GearType::Spur || T == kernel::GearType::Helical || T == kernel::GearType::Herringbone || T == kernel::GearType::Bevel)
        field("Profile shift", "shift", "x m", "0");
    if (usesHand(g.type)) {
        label("Left hand");
        if (ImGui::Checkbox("##ghand", &g.leftHand)) apply();
    }
    ImGui::Spacing();
    try {
        ImGui::TextDisabled("%s", kernel::gearInfo(gearSpecOf(g, doc_.params)).c_str());
    } catch (const std::runtime_error&) {
    }
    if (T == kernel::GearType::Worm || T == kernel::GearType::WormWheel)
        ImGui::TextDisabled("Make the worm and the wheel with the same\nmodule, starts and worm pitch diameter.");
    if (T == kernel::GearType::Bevel) ImGui::TextDisabled("For a pair, swap Teeth and Mating teeth.");
}

void App::drawThreadDialog(bool& ok) {
    const float S = uiScale_;
    ThreadParams& t = tdlg_.thread;
    auto apply = [this] { applyToolDialog(); };
    auto label = [&](const char* l) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(l);
        ImGui::SameLine(120 * S);
    };
    auto field = [&](const char* l, std::string& v, const char* unit) {
        ImGui::PushID(l);
        label(l);
        ImGui::SetNextItemWidth(-40 * S);
        ImGui::InputText("##v", &v, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit()) apply();
        if (ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ok = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", unit);
        ImGui::PopID();
    };
    auto check = [&](const char* l, const char* id, bool& v) {
        label(l);
        if (ImGui::Checkbox(id, &v)) apply();
    };

    label("Face");
    if (!t.face.feature) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 95, 40, 255));
        ImGui::TextUnformatted("Select a round face");
        ImGui::PopStyleColor();
    } else {
        ImGui::Text("%s  %s mm", t.hole ? "Hole" : "Shaft", fmtV(std::round(t.radius * 2000) / 1000).c_str());
    }
    const char* sides[] = {"From the face", "External (bolt)", "Internal (nut)"};
    label("Side");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##tside", &t.side, sides, 3)) apply();
    check("Modeled", "##tmodel", t.modeled);
    ImGui::SetItemTooltip("On: cut the real thread into the body (slower, prints).\nOff: cosmetic, drawn only.");

    const auto& fams = threads::families();
    t.family = std::clamp(t.family, 0, (int)fams.size() - 1);
    const threads::Family& fam = fams[t.family];
    label("Standard");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##tfam", fam.name.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < (int)fams.size(); ++i)
            if (ImGui::Selectable(fams[i].name.c_str(), i == t.family) && i != t.family) {
                t.family = i;
                if (t.size >= 0) {
                    int idx = t.radius > 0 ? threads::closestSize(fams[i], t.radius * 2, threadInternal(t)) : 0;
                    t.size = std::max(0, idx);
                }
                apply();
            }
        ImGui::EndCombo();
    }
    const threads::Family& f2 = fams[t.family];
    label("Size");
    ImGui::SetNextItemWidth(-1);
    std::string cur = t.size >= 0 && t.size < (int)f2.sizes.size() ? f2.sizes[t.size].name : "Custom";
    if (ImGui::BeginCombo("##tsize", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < (int)f2.sizes.size(); ++i)
            if (ImGui::Selectable(f2.sizes[i].name.c_str(), i == t.size)) { t.size = i; apply(); }
        if (ImGui::Selectable("Custom", t.size < 0)) {
            if (t.size >= 0 && t.size < (int)f2.sizes.size()) {
                t.diaExpr = fmtV(f2.sizes[t.size].major);
                t.pitchExpr = fmtV(f2.sizes[t.size].pitch);
            }
            t.size = -1;
            apply();
        }
        ImGui::EndCombo();
    }
    if (t.size < 0) {
        field("Diameter", t.diaExpr, "mm");
        field("Pitch", t.pitchExpr, "mm");
    }
    ImGui::TextDisabled("%s", f2.note.c_str());
    try {
        ThreadSizeInfo sz = threadSize(t, doc_.params);
        double minor = threads::minorDia(*sz.family, sz.major, sz.pitch);
        if (f2.inch) ImGui::TextDisabled("Major %.3f mm  %.4g TPI  Tap drill %.2f", sz.major, 25.4 / sz.pitch, minor);
        else ImGui::TextDisabled("Major %.3f mm  Pitch %.4g  Tap drill %.2f", sz.major, sz.pitch, minor);
        if (t.face.feature && t.radius > 0) {
            bool internal = threadInternal(t);
            double want = internal ? minor : sz.major, have = t.radius * 2;
            if (std::abs(want - have) > std::max(0.05, want * 0.03)) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 130, 30, 255));
                ImGui::TextWrapped("The face is %.2f mm but this size wants %.2f mm %s.", have, want, internal ? "(tap drill)" : "");
                ImGui::PopStyleColor();
            }
        }
    } catch (const std::runtime_error& e) {
        ImGui::TextDisabled("%s", e.what());
    }
    ImGui::Separator();
    check("Full length", "##tfull", t.fullLength);
    if (!t.fullLength) {
        field("Length", t.lengthExpr, "mm");
        field("Offset", t.offsetExpr, "mm");
    }
    check("Flip start", "##tflip", t.flip);
    check("Left hand", "##tleft", t.leftHand);
    field("Starts", t.startsExpr, "");
    ImGui::TextDisabled("%*sblank = the size's own (1 for most)", 22, "");
}

// ===================================================================== cosmetic threads

void App::drawCosmeticThreads() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float S = uiScale_;
    for (int i = 0; i < doc_.markerPos && i < (int)doc_.timeline.size(); ++i) {
        const Feature& f = doc_.timeline[i];
        if (f.type != FeatureType::Thread || f.suppressed || !f.thread) continue;
        const ThreadParams& t = *f.thread;
        if (t.modeled || !t.face.feature || t.radius <= 0) continue;
        ThreadSizeInfo sz;
        double len = t.faceLen, off = 0;
        try {
            sz = threadSize(t, doc_.params);
            if (!t.fullLength) {
                len = peval::eval(t.lengthExpr, doc_.params);
                off = peval::eval(t.offsetExpr, doc_.params);
            }
        } catch (const std::runtime_error&) {
            continue;
        }
        if (sz.pitch <= 0) continue;
        len = std::min(len, t.faceLen - off);
        if (len <= 0) continue;
        glm::dvec3 d = t.flip ? -t.axisD : t.axisD;
        glm::dvec3 o = (t.flip ? t.axisO + t.axisD * t.faceLen : t.axisO) + d * off;
        glm::dvec3 u = glm::abs(d.z) < 0.9 ? glm::normalize(glm::cross(d, glm::dvec3(0, 0, 1))) : glm::normalize(glm::cross(d, glm::dvec3(1, 0, 0)));
        glm::dvec3 v = glm::cross(d, u);
        double r = t.radius * (t.hole ? 0.995 : 1.005);
        int rings = (int)std::floor(len / sz.pitch) + 1;
        int step = std::max(1, rings / 120);
        ImU32 c = ui::th(IM_COL32(60, 70, 90, 150));
        for (int k = 0; k < rings; k += step) {
            glm::dvec3 cc = o + d * (k * sz.pitch);
            ImVec2 pts[41];
            bool okAll = true;
            for (int s = 0; s <= 40; ++s) {
                double a = 2 * kPi * s / 40 * (t.leftHand ? -1 : 1);
                double rise = std::min(sz.pitch * s / 40.0, len - k * sz.pitch);
                glm::dvec3 p = cc + u * (r * std::cos(a)) + v * (r * std::sin(a)) + d * rise;
                if (!worldToScreen(p, pts[s])) { okAll = false; break; }
            }
            if (okAll) dl->AddPolyline(pts, 41, c, ImDrawFlags_None, 1.0f * S);
        }
    }
}

} // namespace kst
