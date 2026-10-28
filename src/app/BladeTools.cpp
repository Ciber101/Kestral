// Blade generator: propellers, helicopter rotors, ducted fans, wind turbines and boat propellers.
// Each kind turns a few design values into airfoil sections from root to tip.
#include "app/App.h"
#include "app/MechTools.h"
#include "core/Airfoils.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kst {

namespace {

const double kPiB = 3.14159265358979323846;
const char* kBladeKinds[] = {"Propeller (plane / drone)", "Helicopter rotor", "Ducted fan (EDF)", "Wind turbine", "Boat propeller"};
enum Kind { Prop, Heli, Edf, Wind, Marine, KindCount };

double evB(const std::string& e, const ParamMap& p, const char* what) {
    try {
        return peval::eval(e, p);
    } catch (const std::runtime_error&) {
        throw std::runtime_error(std::string(what) + ": \"" + e + "\" is not a valid value");
    }
}

std::string fmtB(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Piecewise linear through (x, y) points, x rising.
double curve(const std::vector<glm::dvec2>& pts, double x) {
    if (x <= pts.front().x) return pts.front().y;
    for (size_t i = 1; i < pts.size(); ++i)
        if (x <= pts[i].x) {
            double t = (x - pts[i - 1].x) / std::max(1e-9, pts[i].x - pts[i - 1].x);
            return pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t;
        }
    return pts.back().y;
}

} // namespace

const char* bladeKindName(int k) { return k >= 0 && k < KindCount ? kBladeKinds[k] : "Blades"; }

BladeParams defaultBlade() {
    BladeParams b;
    bladeKindDefaults(b, Prop);
    return b;
}

void bladeKindDefaults(BladeParams& b, int k) {
    std::map<std::string, std::string> keep;
    for (const char* key : {"x", "y", "z"}) keep[key] = b.val(key, "0");
    b.vals = keep;
    auto set = [&](std::initializer_list<std::pair<const char*, const char*>> kv) {
        for (auto& [key, v] : kv) b.vals[key] = v;
    };
    switch (k) {
    case Prop:
        b.airfoil = "Clark Y";
        set({{"dia", "254"}, {"pitch", "127"}, {"blades", "2"}, {"hubDia", "18"}, {"hubLen", "10"}, {"bore", "5"}, {"chord", "24"},
             {"thickRoot", "1.8"}, {"thickTip", "0.8"}});
        break;
    case Heli:
        b.airfoil = "NACA 0012";
        set({{"dia", "700"}, {"blades", "2"}, {"rootCut", "70"}, {"chord", "40"}, {"collective", "6"}, {"twist", "-8"},
             {"gripW", "30"}, {"hubLen", "8"}, {"bore", "5"}, {"boltDia", "3"}});
        break;
    case Edf:
        b.airfoil = "NACA 2409";
        set({{"dia", "70"}, {"pitch", "130"}, {"blades", "12"}, {"hubDia", "32"}, {"hubLen", "18"}, {"bore", "3"}, {"chord", "13"},
             {"thickRoot", "1.2"}, {"thickTip", "0.8"}, {"ductGap", "0.5"}, {"ductThick", "1.6"}, {"ductLen", "36"}});
        break;
    case Wind:
        b.airfoil = "NACA 4412";
        set({{"dia", "1000"}, {"blades", "3"}, {"tsr", "6"}, {"aoa", "5"}, {"cl", "1.0"}, {"chord", "110"}, {"hubDia", "80"},
             {"hubLen", "50"}, {"bore", "8"}, {"thickRoot", "1.8"}, {"thickTip", "1"}});
        break;
    case Marine:
        b.airfoil = "NACA 2410";
        set({{"dia", "200"}, {"pitch", "200"}, {"blades", "3"}, {"ear", "0.55"}, {"skew", "20"}, {"rake", "8"}, {"hubDia", "36"},
             {"hubLen", "40"}, {"bore", "8"}, {"thickRoot", "1.6"}, {"thickTip", "0.6"}});
        break;
    default: break;
    }
    b.kind = k;
}

kernel::BladeSpec bladeSpecOf(const BladeParams& b, const ParamMap& p) {
    auto v = [&](const char* k, const char* def, const char* what) { return evB(b.val(k, def), p, what); };
    const int kind = std::clamp(b.kind, 0, KindCount - 1);
    kernel::BladeSpec s;
    const double D = v("dia", "254", "Diameter"), R = D / 2;
    if (D <= 0) throw std::runtime_error("The diameter must be above 0");
    s.blades = (int)std::lround(v("blades", "2", "Blades"));
    s.leftHand = b.leftHand;
    s.bore = v("bore", "0", "Bore");
    s.hubLen = v("hubLen", "10", "Hub length");
    double r0;
    if (kind == Heli) {
        s.hubDia = 0;
        r0 = v("rootCut", "70", "Root cutout");
        double chord = v("chord", "40", "Chord");
        s.gripTo = r0 + 0.25 * chord;
        s.gripW = v("gripW", "30", "Grip width");
        s.gripT = s.hubLen;
        s.boltDia = v("boltDia", "3", "Bolt hole");
        s.boltR = r0 * 0.4;
        s.pivot = 0.25;
    } else {
        s.hubDia = v("hubDia", "18", "Hub diameter");
        r0 = s.hubDia / 2 * 0.8;
    }
    if (r0 <= 0 || r0 >= R * 0.9) throw std::runtime_error("The hub (or root cutout) must be smaller than the rotor");
    if (s.bore >= (kind == Heli ? s.gripW : s.hubDia)) throw std::runtime_error("The bore is bigger than the hub");
    std::vector<glm::dvec2> foil = airfoils::outline(b.airfoil, airfoilFolder(), 50);
    const double thR = kind == Heli ? 1.0 : v("thickRoot", "1.5", "Root thickness"), thT = kind == Heli ? 1.0 : v("thickTip", "0.8", "Tip thickness");
    const int N = 14;
    const double xh = std::min(r0 / R, 0.29);
    double cMax = 0;
    if (kind == Marine) {
        double ear = v("ear", "0.55", "Blade area ratio");
        if (ear <= 0 || ear > 1.5) throw std::runtime_error("The blade area ratio must be 0 to 1.5 (0.4 to 0.8 is usual)");
        cMax = ear * kPiB * R * R / (std::max(1, s.blades) * 0.78 * (R - r0));
    }
    double pitch = (kind == Prop || kind == Edf || kind == Marine) ? v("pitch", "127", "Pitch") : 0;
    if ((kind == Prop || kind == Edf || kind == Marine) && pitch <= 0) throw std::runtime_error("The pitch must be above 0");
    for (int i = 0; i < N; ++i) {
        double f = std::sin(kPiB / 2 * i / (N - 1));
        double r = r0 + (R - r0) * f, x = r / R;
        kernel::BladeSection sec;
        sec.r = r;
        sec.foil = foil;
        sec.thick = thR + (thT - thR) * f;
        switch (kind) {
        case Prop:
            sec.betaDeg = std::atan(pitch / (2 * kPiB * r)) * 180 / kPiB;
            sec.chord = v("chord", "24", "Max chord") * curve({{xh, 0.85}, {0.3, 1.0}, {0.6, 0.92}, {0.9, 0.62}, {1.0, 0.35}}, x);
            break;
        case Heli:
            sec.betaDeg = v("collective", "6", "Collective") + v("twist", "-8", "Twist") * (x - 0.75);
            sec.chord = v("chord", "40", "Chord");
            break;
        case Edf:
            sec.betaDeg = std::atan(pitch / (2 * kPiB * r)) * 180 / kPiB;
            sec.chord = v("chord", "13", "Chord") * (1.0 - 0.1 * f);
            break;
        case Wind: {
            double tsr = v("tsr", "6", "Tip speed ratio"), aoa = v("aoa", "5", "Design angle of attack"), cl = v("cl", "1.0", "Design lift");
            if (tsr <= 0.5 || cl <= 0.1) throw std::runtime_error("Tip speed ratio and lift coefficient must be positive (TSR 4 to 8, Cl about 1)");
            double lr = tsr * x;
            double phi = 2.0 / 3.0 * std::atan(1.0 / lr);
            sec.betaDeg = phi * 180 / kPiB - aoa;
            double c = 16 * kPiB * r / (std::max(1, s.blades) * cl) * std::pow(std::sin(phi / 2), 2);
            sec.chord = std::min(c, v("chord", "110", "Max chord"));
            break;
        }
        case Marine: {
            sec.betaDeg = std::atan(pitch / (2 * kPiB * r)) * 180 / kPiB;
            sec.chord = cMax * curve({{xh, 0.7}, {0.4, 0.95}, {0.6, 1.0}, {0.8, 0.92}, {0.95, 0.62}, {1.0, 0.3}}, x);
            sec.skewDeg = v("skew", "20", "Skew") * f * f;
            sec.rake = -(r - r0) * std::tan(v("rake", "8", "Rake") * kPiB / 180);
            break;
        }
        default: break;
        }
        if (sec.chord <= 0.2) throw std::runtime_error("A chord came out below 0.2 mm: make the blade wider");
        s.sections.push_back(sec);
    }
    if (kind == Edf) {
        s.ductGap = v("ductGap", "0.5", "Tip gap");
        s.ductThick = v("ductThick", "1.6", "Duct wall");
        s.ductLen = v("ductLen", "36", "Duct length");
    }
    return s;
}

glm::mat4 bladePlacement(const BladeParams& b, const ParamMap& p) {
    double x = evB(b.val("x", "0"), p, "X"), y = evB(b.val("y", "0"), p, "Y"), z = evB(b.val("z", "0"), p, "Z");
    glm::dvec3 ax{1, 0, 0}, ay{0, 1, 0};
    if (b.plane == 1) { ax = {1, 0, 0}; ay = {0, 0, 1}; }
    else if (b.plane == 2) { ax = {0, 1, 0}; ay = {0, 0, 1}; }
    glm::dvec3 az = glm::cross(ax, ay), o = ax * x + ay * y + az * z;
    glm::mat4 m(1.f);
    m[0] = glm::vec4(glm::vec3(ax), 0.f);
    m[1] = glm::vec4(glm::vec3(ay), 0.f);
    m[2] = glm::vec4(glm::vec3(az), 0.f);
    m[3] = glm::vec4(glm::vec3(o), 1.f);
    return m;
}

std::string bladeInfo(const kernel::BladeSpec& s) {
    const auto& a = s.sections.front();
    const auto& t = s.sections.back();
    const kernel::BladeSection* m = &a;
    for (auto& sec : s.sections)
        if (std::abs(sec.r - 0.75 * t.r) < std::abs(m->r - 0.75 * t.r)) m = &sec;
    double area = 0;
    for (size_t i = 1; i < s.sections.size(); ++i)
        area += (s.sections[i].r - s.sections[i - 1].r) * (s.sections[i].chord + s.sections[i - 1].chord) / 2;
    double solidity = s.blades * area / (kPiB * t.r * t.r);
    char buf[200];
    std::snprintf(buf, sizeof buf, "Blade angle: root %.1f, 75%% %.1f, tip %.1f deg\nChord: root %.1f, tip %.1f mm  Solidity %.2f",
                  a.betaDeg, m->betaDeg, t.betaDeg, a.chord, t.chord, solidity);
    return buf;
}

// ===================================================================== command and dialog

void App::runBlade() { startToolFeature(FeatureType::Blade); }

void App::drawBladeDialog(bool& ok) {
    const float S = uiScale_;
    BladeParams& b = tdlg_.blade;
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
        std::string& v = b.vals[key];
        if (v.empty()) v = def;
        ImGui::InputText("##v", &v, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit()) apply();
        if (ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ok = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", unit);
        try {
            double x = peval::eval(v, doc_.params);
            if (!peval::plainNumber(v)) ImGui::TextDisabled("%*s= %s", 30, "", fmtB(x).c_str());
        } catch (const std::runtime_error&) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
            ImGui::TextUnformatted("Not a valid value");
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    };

    label("Kind");
    ImGui::SetNextItemWidth(-1);
    int kind = std::clamp(b.kind, 0, KindCount - 1);
    if (ImGui::Combo("##bkind", &kind, kBladeKinds, KindCount) && kind != b.kind) {
        bladeKindDefaults(b, kind);
        apply();
    }
    const char* planes[] = {"XY Plane (axis Z)", "XZ Plane (axis Y)", "YZ Plane (axis X)"};
    label("Plane");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##bplane", &b.plane, planes, 3)) apply();
    field("Center X", "x", "mm", "0");
    field("Center Y", "y", "mm", "0");
    field("Z (along axis)", "z", "mm", "0");
    ImGui::Separator();
    field("Diameter", "dia", "mm", "254");
    field("Blades", "blades", "", "2");
    switch (b.kind) {
    case Prop:
        field("Pitch", "pitch", "mm", "127");
        ImGui::SetItemTooltip("Distance moved per turn. A 10x5 prop is 254 mm x 127 mm.");
        field("Max chord", "chord", "mm", "24");
        break;
    case Heli:
        field("Root cutout", "rootCut", "mm", "70");
        field("Chord", "chord", "mm", "40");
        field("Collective", "collective", "deg", "6");
        ImGui::SetItemTooltip("Blade angle at 75% radius");
        field("Twist", "twist", "deg", "-8");
        ImGui::SetItemTooltip("Root to tip, negative = washout");
        field("Grip width", "gripW", "mm", "30");
        field("Grip thickness", "hubLen", "mm", "8");
        field("Bolt hole", "boltDia", "mm", "3");
        break;
    case Edf:
        field("Pitch", "pitch", "mm", "130");
        field("Chord", "chord", "mm", "13");
        field("Tip gap", "ductGap", "mm", "0.5");
        field("Duct wall", "ductThick", "mm", "1.6");
        field("Duct length", "ductLen", "mm", "36");
        ImGui::TextDisabled("Duct wall 0 = no duct");
        break;
    case Wind:
        field("Tip speed ratio", "tsr", "", "6");
        field("Design AoA", "aoa", "deg", "5");
        field("Design lift (Cl)", "cl", "", "1.0");
        field("Max chord", "chord", "mm", "110");
        ImGui::TextDisabled("Chord and twist follow the Betz / Schmitz optimum.");
        break;
    case Marine:
        field("Pitch", "pitch", "mm", "200");
        field("Blade area ratio", "ear", "", "0.55");
        field("Skew", "skew", "deg", "20");
        field("Rake", "rake", "deg", "8");
        break;
    default: break;
    }
    if (b.kind != Heli) {
        field("Hub diameter", "hubDia", "mm", "18");
        field("Hub length", "hubLen", "mm", "10");
        field("Root thickness", "thickRoot", "x foil", "1.5");
        field("Tip thickness", "thickTip", "x foil", "0.8");
    }
    field("Bore", "bore", "mm", "5");
    label("Airfoil");
    if (drawAirfoilPicker(b.airfoil, 130 * S)) apply();
    label("Left hand");
    if (ImGui::Checkbox("##bleft", &b.leftHand)) apply();
    ImGui::SetItemTooltip("Turns the other way (clockwise seen from the thrust side)");
    ImGui::Spacing();
    try {
        ImGui::TextDisabled("%s", bladeInfo(bladeSpecOf(b, doc_.params)).c_str());
    } catch (const std::runtime_error& e) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
        ImGui::TextWrapped("%s", e.what());
        ImGui::PopStyleColor();
    }
}

} // namespace kst
