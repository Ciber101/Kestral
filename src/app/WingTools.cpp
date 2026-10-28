// Wing generator: panels and airfoils -> kernel spec, the dialog and airfoil import.
#include "app/App.h"
#include "app/MechTools.h"
#include "core/Airfoils.h"
#include "platform/Platform.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace kst {

namespace {

const double kPi = 3.14159265358979323846;

std::string fmtW(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

double evW(const std::string& e, const ParamMap& p, const std::string& what) {
    try {
        return peval::eval(e, p);
    } catch (const std::runtime_error&) {
        throw std::runtime_error(what + ": \"" + e + "\" is not a valid value");
    }
}

double rad(double d) { return d * kPi / 180.0; }

} // namespace

std::string airfoilFolder() { return platform::appDataDir() + "/airfoils"; }

WingParams defaultWing() {
    WingParams w;
    w.panels.push_back({});
    w.vals = {{"rootChord", "200"}, {"x", "0"}, {"y", "0"}, {"z", "0"}, {"skin", "1.2"}, {"spar1", "25"}, {"spar1Dia", "6"},
              {"spar2", "65"}, {"spar2Dia", "0"}, {"ribSpacing", "50"}, {"ribThick", "1.2"}};
    return w;
}

kernel::WingSpec wingSpecOf(const WingParams& w, const ParamMap& p) {
    if (w.panels.empty()) throw std::runtime_error("Add at least one panel");
    if (w.panels.size() > 20) throw std::runtime_error("Over 20 panels: use fewer");
    const std::string folder = airfoilFolder();
    kernel::WingSpec s;
    kernel::WingSection root;
    root.le = {evW(w.val("x", "0"), p, "Position X"), evW(w.val("y", "0"), p, "Position Y"), evW(w.val("z", "0"), p, "Position Z")};
    root.chord = evW(w.val("rootChord", "200"), p, "Root chord");
    root.foil = airfoils::outline(w.rootAirfoil, folder);
    s.sections.push_back(root);
    for (size_t i = 0; i < w.panels.size(); ++i) {
        const auto& pn = w.panels[i];
        std::string tag = "Panel " + std::to_string(i + 1) + " ";
        double span = evW(pn.span, p, tag + "span");
        if (span <= 0) throw std::runtime_error(tag + "span must be above 0");
        double sweep = evW(pn.sweep, p, tag + "sweep"), dih = evW(pn.dihedral, p, tag + "dihedral");
        if (std::abs(sweep) >= 80 || std::abs(dih) >= 80) throw std::runtime_error(tag + "sweep and dihedral must be under 80 degrees");
        kernel::WingSection sec;
        const auto& prev = s.sections.back();
        sec.le = prev.le + glm::dvec3(span * std::tan(rad(sweep)), span, span * std::tan(rad(dih)));
        sec.chord = evW(pn.tipChord, p, tag + "tip chord");
        if (sec.chord <= 0) throw std::runtime_error(tag + "tip chord must be above 0");
        sec.twistDeg = evW(pn.twist, p, tag + "twist");
        sec.foil = airfoils::outline(pn.airfoil, folder);
        s.sections.push_back(sec);
    }
    s.mirror = w.mirror;
    s.hollow = w.hollow;
    s.skin = evW(w.val("skin", "1.2"), p, "Skin");
    for (const char* k : {"spar1", "spar2"}) {
        double dia = evW(w.val((std::string(k) + "Dia").c_str(), "0"), p, "Spar diameter");
        if (dia <= 0) continue;
        double pos = evW(w.val(k, "25"), p, "Spar position");
        s.spars.push_back({std::clamp(pos, 2.0, 95.0) / 100.0, dia});
    }
    if (w.hollow) {
        s.ribSpacing = evW(w.val("ribSpacing", "0"), p, "Rib spacing");
        s.ribThick = evW(w.val("ribThick", "1.2"), p, "Rib thickness");
    }
    return s;
}

std::string wingInfo(const kernel::WingSpec& s) {
    double area = 0, mac = 0, span = 0;
    for (size_t i = 1; i < s.sections.size(); ++i) {
        double b = s.sections[i].le.y - s.sections[i - 1].le.y, c1 = s.sections[i - 1].chord, c2 = s.sections[i].chord;
        area += b * (c1 + c2) / 2;
        mac += b * (c1 * c1 + c1 * c2 + c2 * c2) / 3;
        span += b;
    }
    if (area <= 0) return "";
    mac /= area;
    if (s.mirror) { area *= 2; span *= 2; }
    char b[160];
    std::snprintf(b, sizeof b, "Span %.0f mm  Area %.1f dm\xC2\xB2\nAspect ratio %.2f  MAC %.1f mm%s", span, area / 1e4, span * span / area, mac,
                  s.mirror ? "" : "\n(one half: tick Both halves for the full wing)");
    return b;
}

// ===================================================================== command and dialog

void App::runWing() { startToolFeature(FeatureType::Wing); }

bool App::drawAirfoilPicker(std::string& name, float indent) {
    const float S = uiScale_;
    bool changed = false;
    ImGui::PushID(&name);
    ImGui::SetNextItemWidth(-30 * S);
    if (ImGui::BeginCombo("##foil", name.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (auto& n : airfoils::library(airfoilFolder()))
            if (ImGui::Selectable(n.c_str(), n == name) && n != name) {
                name = n;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Any NACA 4 or 5 digit code also works: type it below.");
    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(-1, 0))) {
        std::string path = platform::openFileDialog("Import airfoil", "Airfoil coordinates (*.dat;*.txt)", "*.dat;*.DAT;*.txt;*.TXT");
        if (!path.empty()) {
            try {
                namespace fs = std::filesystem;
                airfoils::readDat(path);
                fs::path src = fs::u8path(path), dir = fs::u8path(airfoilFolder());
                fs::create_directories(dir);
                auto stem = src.stem().u8string();
                std::string stemU8(stem.begin(), stem.end());
                fs::path dst = dir / fs::u8path(stemU8 + ".dat");
                if (fs::absolute(src) != fs::absolute(dst)) fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
                name = stemU8;
                changed = true;
                toast("Airfoil " + name + " added to the library");
            } catch (const std::exception& e) {
                toast(std::string("Could not import the airfoil: ") + e.what(), ToastKind::Warning);
            }
        }
    }
    ImGui::SetItemTooltip("Import a .dat airfoil (Selig or Lednicer format, e.g. from the UIUC database)");
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::SameLine(indent);
    ImGui::SetNextItemWidth(-30 * S);
    std::string typed = name;
    if (ImGui::InputText("##ftype", &typed, ImGuiInputTextFlags_EnterReturnsTrue) && typed != name) {
        name = typed;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

void App::drawWingDialog(bool& ok) {
    const float S = uiScale_;
    WingParams& w = tdlg_.wing;
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
        try {
            double x = peval::eval(v, doc_.params);
            if (!peval::plainNumber(v)) ImGui::TextDisabled("%*s= %s", 30, "", fmtW(x).c_str());
        } catch (const std::runtime_error&) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
            ImGui::TextUnformatted("Not a valid value");
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    };
    auto val = [&](const char* l, const char* key, const char* unit, const char* def) {
        std::string& v = w.vals[key];
        if (v.empty()) v = def;
        field(l, v, unit);
    };
    auto airfoil = [&](const char* l, std::string& name) {
        label(l);
        if (drawAirfoilPicker(name, 120 * S)) apply();
    };
    auto check = [&](const char* l, const char* id, bool& v) {
        label(l);
        if (ImGui::Checkbox(id, &v)) apply();
    };

    val("Position X", "x", "mm", "0");
    val("Position Y", "y", "mm", "0");
    val("Position Z", "z", "mm", "0");
    ImGui::Separator();
    val("Root chord", "rootChord", "mm", "200");
    airfoil("Root airfoil", w.rootAirfoil);
    int remove = -1;
    for (size_t i = 0; i < w.panels.size(); ++i) {
        auto& pn = w.panels[i];
        ImGui::PushID((int)i);
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Panel %d", (int)i + 1);
        if (w.panels.size() > 1) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50 * S);
            if (ImGui::SmallButton("Remove")) remove = (int)i;
        }
        field("Span", pn.span, "mm");
        field("Tip chord", pn.tipChord, "mm");
        field("Sweep (LE)", pn.sweep, "deg");
        field("Dihedral", pn.dihedral, "deg");
        field("Tip twist", pn.twist, "deg");
        ImGui::SetItemTooltip("Twist at the end of this panel, about the quarter chord.\nNegative = washout (tip nose down).");
        airfoil("Tip airfoil", pn.airfoil);
        ImGui::PopID();
    }
    if (remove >= 0) {
        w.panels.erase(w.panels.begin() + remove);
        apply();
    }
    if (w.panels.size() < 20 && ImGui::Button("+ Add panel")) {
        WingParams::Panel pn;
        if (!w.panels.empty()) {
            pn = w.panels.back();
            try {
                pn.tipChord = fmtW(std::round(peval::eval(pn.tipChord, doc_.params) * 0.6));
            } catch (const std::runtime_error&) {
            }
            pn.span = "200";
        }
        w.panels.push_back(pn);
        apply();
    }
    ImGui::Separator();
    check("Both halves", "##wmirror", w.mirror);
    ImGui::SetItemTooltip("Mirror the panel set across the root for a full wing.\nLeave off to print each half separately.");
    check("Hollow (print)", "##whollow", w.hollow);
    ImGui::SetItemTooltip("A printable shell with ribs and spar tubes inside.");
    if (w.hollow) {
        val("Skin", "skin", "mm", "1.2");
        val("Rib spacing", "ribSpacing", "mm", "50");
        val("Rib thickness", "ribThick", "mm", "1.2");
    }
    val("Spar 1 at", "spar1", "% c", "25");
    val("Spar 1 dia.", "spar1Dia", "mm", "6");
    val("Spar 2 at", "spar2", "% c", "65");
    val("Spar 2 dia.", "spar2Dia", "mm", "0");
    ImGui::TextDisabled("Spar holes run from the root to just short of the tip.\nDiameter 0 = no spar.");
    ImGui::Spacing();
    try {
        ImGui::TextDisabled("%s", wingInfo(wingSpecOf(w, doc_.params)).c_str());
    } catch (const std::runtime_error& e) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
        ImGui::TextWrapped("%s", e.what());
        ImGui::PopStyleColor();
    }
}

} // namespace kst
