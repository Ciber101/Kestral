// Dovetail generator: kinds, values -> kernel spec, placement and the dialog.
#include "app/App.h"
#include "app/MechTools.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kst {

namespace {

const char* kDovetailTypes[] = {"Key (bowtie)",        "Tab and socket",           "Sliding rail and slot", "Puzzle tab",
                                "Through dovetail joint", "Half blind dovetail joint", "Secret mitered joint",  "Machine slide (ways + gib)"};

double evD(const std::string& e, const ParamMap& p, const char* what) {
    try {
        return peval::eval(e, p);
    } catch (const std::runtime_error&) {
        throw std::runtime_error(std::string(what) + ": \"" + e + "\" is not a valid value");
    }
}

std::string fmtD(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

} // namespace

const char* dovetailTypeName(int t) { return t >= 0 && t < (int)kernel::DovetailType::Count ? kDovetailTypes[t] : "Dovetail"; }

DovetailParams defaultDovetail() {
    DovetailParams d;
    d.vals = {{"x", "0"}, {"y", "0"}, {"z", "0"}, {"rot", "0"}, {"clearance", "0.15"}, {"count", "1"}, {"spacing", "30"},
              {"boardW", "100"}, {"tailT", "18"}, {"pinT", "18"}, {"tailL", "150"}, {"pinL", "150"}, {"tails", "3"}, {"pinW", "8"},
              {"lap", "4"}, {"baseW", "60"}, {"baseH", "20"}, {"chanW", "30"}, {"chanD", "10"}, {"flank", "60"}, {"slideW", "60"},
              {"slideH", "15"}, {"gib", "3"}, {"slideLen", "100"}};
    dovetailTypeDefaults(d, 1);
    return d;
}

void dovetailTypeDefaults(DovetailParams& d, int t) {
    using T = kernel::DovetailType;
    auto set = [&](const char* k, const char* v) { d.vals[k] = v; };
    switch ((T)t) {
    case T::Key: set("length", "30"); set("width", "8"); set("depth", "5"); set("angle", "15"); break;
    case T::Tab: set("length", "12"); set("width", "10"); set("depth", "10"); set("angle", "10"); break;
    case T::Sliding: set("length", "6"); set("width", "8"); set("depth", "50"); set("angle", "15"); break;
    case T::Puzzle: set("length", "14"); set("width", "6"); set("head", "10"); set("depth", "10"); break;
    case T::Through:
    case T::HalfBlind:
    case T::SecretMitered: set("angle", "9.46"); set("clearance", "0.1"); break;
    case T::MachineSlide: set("clearance", "0.1"); break;
    default: break;
    }
    if (kernel::dovetailIsConnector((T)t) && d.val("clearance", "0.15") == "0.1") set("clearance", "0.15");
    d.type = t;
}

kernel::DovetailSpec dovetailSpecOf(const DovetailParams& d, const ParamMap& p) {
    kernel::DovetailSpec s;
    s.type = (kernel::DovetailType)std::clamp(d.type, 0, (int)kernel::DovetailType::Count - 1);
    auto v = [&](const char* k, const char* def, const char* what) { return evD(d.val(k, def), p, what); };
    s.clearance = v("clearance", "0.15", "Clearance");
    s.angleDeg = v("angle", "10", "Angle");
    if (kernel::dovetailIsConnector(s.type)) {
        s.length = v("length", "12", "Length");
        s.width = v("width", "10", "Width");
        s.depth = v("depth", "10", "Depth");
        s.head = v("head", "10", "Head");
        s.count = (int)std::lround(v("count", "1", "Count"));
        s.spacing = v("spacing", "30", "Spacing");
    } else if (s.type == kernel::DovetailType::MachineSlide) {
        s.baseW = v("baseW", "60", "Base width");
        s.baseH = v("baseH", "20", "Base height");
        s.chanW = v("chanW", "30", "Channel opening");
        s.chanD = v("chanD", "10", "Channel depth");
        s.flankDeg = v("flank", "60", "Flank angle");
        s.slideW = v("slideW", "60", "Slide width");
        s.slideH = v("slideH", "15", "Slide height");
        s.gib = v("gib", "3", "Gib");
        s.slideLen = v("slideLen", "100", "Length");
    } else {
        s.boardW = v("boardW", "100", "Board width");
        s.tailT = v("tailT", "18", "Tail board thickness");
        s.pinT = v("pinT", "18", "Pin board thickness");
        s.tailL = v("tailL", "150", "Tail board length");
        s.pinL = v("pinL", "150", "Pin board length");
        s.tails = (int)std::lround(v("tails", "3", "Tails"));
        s.pinW = v("pinW", "8", "Pin width");
        s.lap = v("lap", "4", "Lap");
    }
    return s;
}

glm::mat4 dovetailPlacement(const DovetailParams& d, const ParamMap& p) {
    double x = evD(d.val("x", "0"), p, "X"), y = evD(d.val("y", "0"), p, "Y"), z = evD(d.val("z", "0"), p, "Z");
    double rot = evD(d.val("rot", "0"), p, "Rotation") * 3.14159265358979323846 / 180.0;
    glm::dvec3 ax{1, 0, 0}, ay{0, 1, 0};
    if (d.plane == 1) { ax = {1, 0, 0}; ay = {0, 0, 1}; }
    else if (d.plane == 2) { ax = {0, 1, 0}; ay = {0, 0, 1}; }
    glm::dvec3 az = glm::cross(ax, ay), o = ax * x + ay * y + az * z;
    glm::dvec3 rx = ax * std::cos(rot) + ay * std::sin(rot), ry = -ax * std::sin(rot) + ay * std::cos(rot);
    glm::mat4 m(1.f);
    m[0] = glm::vec4(glm::vec3(rx), 0.f);
    m[1] = glm::vec4(glm::vec3(ry), 0.f);
    m[2] = glm::vec4(glm::vec3(az), 0.f);
    m[3] = glm::vec4(glm::vec3(o), 1.f);
    return m;
}

// ===================================================================== command and dialog

void App::runDovetail() { startToolFeature(FeatureType::Dovetail); }

void App::drawDovetailDialog(bool& ok) {
    const float S = uiScale_;
    DovetailParams& d = tdlg_.dovetail;
    using T = kernel::DovetailType;
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
        std::string& v = d.vals[key];
        if (v.empty()) v = def;
        ImGui::InputText("##v", &v, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit()) apply();
        if (ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ok = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", unit);
        try {
            double x = peval::eval(v, doc_.params);
            if (!peval::plainNumber(v)) ImGui::TextDisabled("%*s= %s", 30, "", fmtD(x).c_str());
        } catch (const std::runtime_error&) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
            ImGui::TextUnformatted("Not a valid value");
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    };

    label("Kind");
    ImGui::SetNextItemWidth(-1);
    int type = std::clamp(d.type, 0, (int)T::Count - 1);
    if (ImGui::Combo("##dvtype", &type, kDovetailTypes, (int)T::Count) && type != d.type) {
        dovetailTypeDefaults(d, type);
        apply();
    }
    T t = (T)d.type;
    bool conn = kernel::dovetailIsConnector(t);
    const char* planes[] = {"XY Plane", "XZ Plane", "YZ Plane"};
    label("Plane");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##dvplane", &d.plane, planes, 3)) apply();
    field("X", "x", "mm", "0");
    field("Y", "y", "mm", "0");
    field("Z (off plane)", "z", "mm", "0");
    field("Rotation", "rot", "deg", "0");
    ImGui::Separator();
    if (conn) {
        label("Bodies");
        if (tdlg_.targets.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 95, 40, 255));
            ImGui::TextUnformatted(t == T::Key ? "Optional: bodies to cut sockets in" : "Optional: tab body first, then the others");
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%d picked", (int)tdlg_.targets.size());
        }
        if (t == T::Key) {
            field("Length (across)", "length", "mm", "30");
            field("Waist width", "width", "mm", "8");
            field("Depth", "depth", "mm", "5");
            field("Flare angle", "angle", "deg", "15");
        } else if (t == T::Puzzle) {
            field("Tab length", "length", "mm", "14");
            field("Neck width", "width", "mm", "6");
            field("Head diameter", "head", "mm", "10");
            field("Thickness", "depth", "mm", "10");
        } else if (t == T::Sliding) {
            field("Rail height", "length", "mm", "6");
            field("Neck width", "width", "mm", "8");
            field("Rail length", "depth", "mm", "50");
            field("Flare angle", "angle", "deg", "15");
        } else {
            field("Tab length", "length", "mm", "12");
            field("Neck width", "width", "mm", "10");
            field("Thickness", "depth", "mm", "10");
            field("Flare angle", "angle", "deg", "10");
        }
        field("Clearance", "clearance", "mm", "0.15");
        field("Count", "count", "", "1");
        if (d.val("count", "1") != "1") field("Spacing", "spacing", "mm", "30");
        ImGui::Spacing();
        ImGui::TextDisabled(t == T::Key ? "The key spans the seam on the plane (X across it).\nSockets get the clearance; the key is a new body."
                                        : "The seam is the plane's Y axis: the tab grows along +X.\nThe first body picked gets the tab, the rest get\nthe socket with clearance. Negative depth flips it.");
    } else if (t == T::MachineSlide) {
        field("Base width", "baseW", "mm", "60");
        field("Base height", "baseH", "mm", "20");
        field("Channel opening", "chanW", "mm", "30");
        field("Channel depth", "chanD", "mm", "10");
        field("Flank angle", "flank", "deg", "60");
        field("Slide width", "slideW", "mm", "60");
        field("Slide height", "slideH", "mm", "15");
        field("Gib thickness", "gib", "mm", "3");
        field("Length", "slideLen", "mm", "100");
        field("Clearance", "clearance", "mm", "0.1");
        ImGui::TextDisabled("Makes the base, the slide and a gib strip on the +X\nflank (0 = no gib). 60 deg flanks are the usual ways.");
    } else {
        field("Board width", "boardW", "mm", "100");
        field("Tail board thick", "tailT", "mm", "18");
        field("Pin board thick", "pinT", "mm", "18");
        field("Tail board length", "tailL", "mm", "150");
        field("Pin board length", "pinL", "mm", "150");
        field("Tails", "tails", "", "3");
        field("Pin width", "pinW", "mm", "8");
        field("Angle", "angle", "deg", "9.46");
        ImGui::SetItemTooltip("1:6 = 9.46 deg (softwood), 1:8 = 7.13 deg (hardwood)");
        if (t != T::Through) field("Lap", "lap", "mm", "4");
        field("Clearance", "clearance", "mm", "0.1");
        ImGui::TextDisabled("Makes both boards: the tail board lies flat along +Y,\nthe pin board stands up at the corner.");
    }
}

} // namespace kst
