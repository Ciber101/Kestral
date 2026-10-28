// The Slice tab's frame, laid out like OrcaSlicer: the tab row (Home, Prepare, Preview, Device,
// Project, Calibration) with Slice plate and Print plate at the right; the 3D toolbar over the
// plate; each build plate's name, number and side buttons; the view buttons at the bottom left.
#include "app/App.h"
#include "app/SliceStyle.h"
#include "core/Log.h"
#include "kernel/Features.h"
#include "platform/Platform.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>

namespace kst {

using namespace ui;
using sl::OIcon;

// ===================================================================== tabs

int App::sliceCurTop() const {
    int t = slice_.top;
    if (t == 0 || t == 4 || t == 5) return t;
    if (slice_.device) return 3;
    if (slice_.preview && slice_.paths) return 2;
    return 1;
}

void App::setSliceTop(int t) {
    int was = sliceCurTop();
    slice_.top = t;
    slice_.device = t == 3;
    if (t == 2) {
        slice_.preview = true;
        bool sliced = false;
        {
            std::lock_guard lock(print_.m);
            sliced = print_.last.ok;
        }
        if (!slice_.paths || !sliced || slice_.slicedRev != doc_.revision) {
            if (!slice_.slicing && (!slice_.paths || !sliced)) startSliceJob();   // Orca slices when Preview opens
        }
    } else if (t == 1) {
        slice_.preview = false;
    }
    if (t == 3) devicePoll(true);
    (void)was;   // the view stays where it is (Orca keeps it too)
    sceneDirty_ = true;
    activity();
}

void App::drawSliceBar() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = 34.f * S, h = 40.f * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::AppBar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##slicebar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x, wp.y + h - 1), ImVec2(wp.x + vp->Size.x, wp.y + h - 1), col::PanelBorder);
    int cur = sliceCurTop();

    // Home, then the named tabs; the one in front is filled (Orca: teal, Kestral: orange).
    struct T { const char* name; OIcon icon; const char* tip; };
    const T tabs[6] = {{"", OIcon::Home, "Home: recent print projects"},
                       {"Prepare", OIcon::Prepare, "Prepare: place the parts, pick printer, filaments and settings"},
                       {"Preview", OIcon::Preview, "Preview: the sliced print, layer by layer"},
                       {"Device", OIcon::Device, "Device: your printer, its camera and controls"},
                       {"Project", OIcon::Project, "Project: name, author and notes of this print project"},
                       {"Calibration", OIcon::Calibrate, "Calibration: flow dynamics, flow ratio and other tests"}};
    float x = wp.x;
    for (int i = 0; i < 6; ++i) {
        float tw = i == 0 ? 40 * S : std::max(120 * S, ImGui::CalcTextSize(tabs[i].name).x + 56 * S);
        ImVec2 a(x, wp.y), b(x + tw, wp.y + h - 1);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##tab", ImVec2(tw, h - 1));
        bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("%s", tabs[i].tip);
        ImGui::PopID();
        bool on = i == cur;
        if (on) dl->AddRectFilled(a, b, sl::orangeU());
        else if (hov) dl->AddRectFilled(a, b, col::Hover);
        ImU32 tc = on ? IM_COL32_WHITE : col::Text;
        float ix = i == 0 ? x + tw * 0.5f : x + 22 * S;
        sl::drawOIcon(dl, ImVec2(ix, wp.y + h * 0.5f), 17 * S, tabs[i].icon, tc);
        if (i > 0) dl->AddText(fonts_.bold, ImGui::GetFontSize() * 1.06f, ImVec2(x + 40 * S, wp.y + (h - ImGui::GetFontSize() * 1.06f) * 0.5f), tc, tabs[i].name);
        if (clicked && i != cur) setSliceTop(i);
        flashRect(ImGui::GetForegroundDrawList(), a, b, flashAmount("stop:" + std::to_string(i)), 4 * S);
        x += tw;
    }

    // Right: the engine and the assistant (Kestral's own), then Slice plate and Print plate (Orca's).
    float bh = 28 * S, arrowW = 24 * S;
    float sliceW = 118 * S, printW = 118 * S;
    float rx = wp.x + vp->Size.x - 12 * S - (arrowW + sliceW) - 10 * S - (arrowW + printW);
    float ry = wp.y + (h - bh) * 0.5f;
    auto iconAt = [&](const char* id, float px, OIcon ic, const char* tip, bool attention) {
        ImGui::SetCursorScreenPos(ImVec2(px, ry));
        bool c = ImGui::InvisibleButton(id, ImVec2(bh, bh));
        bool hv = ImGui::IsItemHovered();
        if (hv) { dl->AddRectFilled(ImVec2(px, ry), ImVec2(px + bh, ry + bh), col::Hover, 5 * S); ImGui::SetTooltip("%s", tip); }
        sl::drawOIcon(dl, ImVec2(px + bh * 0.5f, ry + bh * 0.5f), 16 * S, ic, attention ? sl::orangeU() : col::Text);
        return c;
    };
    {
        bool attention = orcaPathFind().empty() || !slice_.pending.empty() ||
                         (slice_.latest.error.empty() && !slice_.latest.version.empty() && !slice_.engineVersion.empty() &&
                          print::compareVersions(slice_.latest.version, slice_.engineVersion) > 0);
        if (iconAt("##engine", rx - 2 * (bh + 6 * S), OIcon::Gear, ("Slicer engine\n" + engineStatusText()).c_str(), attention)) ImGui::OpenPopup("##engine");
        if (iconAt("##ai", rx - (bh + 6 * S), OIcon::Edit, "Assistant: it can do anything in this tab (settings, plates, filaments, printing)", ai_.open)) {
            ai_.open = !ai_.open;
            ai_.focus = ai_.open;
        }
        if (ImGui::BeginPopup("##engine")) {
            std::string exe = orcaPathFind();
            ImGui::TextUnformatted(engineStatusText().c_str());
            bool newer = slice_.latest.error.empty() && !slice_.latest.version.empty() && !slice_.engineVersion.empty() &&
                         print::compareVersions(slice_.latest.version, slice_.engineVersion) > 0 && slice_.pending.empty();
            if (slice_.installing) {
                std::lock_guard lk(slice_.m);
                ImGui::ProgressBar(slice_.installFrac < 0 ? (float)-ImGui::GetTime() : (float)slice_.installFrac, ImVec2(300 * S, 0),
                                   slice_.installStage.c_str());
                if (ImGui::Button("Stop") && slice_.engineCancel) slice_.engineCancel->store(true);
                activity();
            } else if (!slice_.pending.empty()) {
                ImGui::Text("OrcaSlicer %s is installed.", slice_.pending.c_str());
                if (sl::orangeButton("Restart Kestral to finish")) restartKestral();
            } else if (slice_.checking) {
                ImGui::TextDisabled("Checking for updates...");
                activity();
            } else if (exe.empty()) {
                if (sl::orangeButton("Install slicer engine")) engineCheck(true);
            } else if (newer) {
                if (sl::orangeButton(("Update to OrcaSlicer " + slice_.latest.version).c_str())) engineCheck(true);
            } else {
                if (slice_.checked && slice_.latest.error.empty()) ImGui::TextDisabled("Up to date (newest: OrcaSlicer %s)", slice_.latest.version.c_str());
                if (ImGui::Button("Check for updates")) engineCheck(false);
            }
            if (!slice_.installError.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Error), "%s", slice_.installError.c_str());
            ImGui::EndPopup();
        }
    }
    bool sliced = false;
    {
        std::lock_guard lock(print_.m);
        sliced = print_.last.ok;
    }
    // A split button: the arrow at the left opens the menu, like Orca's.
    auto split = [&](const char* id, float px, float w, const char* label, bool enabled, bool& menu) {
        ImVec2 a(px, ry), m(px + arrowW, ry), b(px + arrowW + w, ry + bh);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(id);
        bool openMenu = ImGui::InvisibleButton("##arrow", ImVec2(arrowW, bh)) && enabled;
        bool ah = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(m);
        bool press = ImGui::InvisibleButton("##main", ImVec2(w, bh)) && enabled;
        bool mh = ImGui::IsItemHovered();
        ImGui::PopID();
        ImU32 base = enabled ? sl::orangeU() : col::Chip, dark = enabled ? sl::orangeDarkU() : col::Chip;
        dl->AddRectFilled(a, b, base, bh * 0.5f);
        dl->AddRectFilled(a, ImVec2(m.x, b.y), ah && enabled ? sl::orangeU() : dark, bh * 0.5f, ImDrawFlags_RoundCornersLeft);
        if (mh && enabled) dl->AddRectFilled(m, b, IM_COL32(255, 255, 255, 28), bh * 0.5f, ImDrawFlags_RoundCornersRight);
        ImU32 tc = enabled ? IM_COL32_WHITE : col::TextDim;
        // The arrow: a small chevron.
        ImVec2 cc(a.x + arrowW * 0.55f, a.y + bh * 0.5f);
        dl->AddLine(ImVec2(cc.x - 4 * S, cc.y - 2 * S), ImVec2(cc.x, cc.y + 2 * S), tc, 1.6f * S);
        dl->AddLine(ImVec2(cc.x, cc.y + 2 * S), ImVec2(cc.x + 4 * S, cc.y - 2 * S), tc, 1.6f * S);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(m.x + (w - ts.x) * 0.5f, a.y + (bh - ts.y) * 0.5f), tc, label);
        menu = openMenu;
        return press;
    };
    bool sliceMenu = false, printMenu = false;
    if (slice_.slicing) {
        ImGui::SetCursorScreenPos(ImVec2(rx, ry));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, sl::orange());
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
        ImGui::ProgressBar(-(float)ImGui::GetTime(), ImVec2(arrowW + sliceW, bh), "Slicing... (stop)");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && slice_.cancel) { slice_.cancel->store(true); slice_.slicing = false; jobs_->setStatus(""); }
        activity();
    } else if (split("slice", rx, sliceW, "Slice plate", !orcaPathFind().empty(), sliceMenu)) {
        startSliceJob();
        if (sliceCurTop() == 1) slice_.top = 2;   // Orca shows Preview once sliced
    }
    if (sliceMenu) ImGui::OpenPopup("##slicemenu");
    if (split("print", rx + arrowW + sliceW + 10 * S, printW, "Print plate", sliced, printMenu)) slice_.sendOpen = true;
    if (printMenu) ImGui::OpenPopup("##printmenu");
    if (ImGui::BeginPopup("##slicemenu")) {
        if (ImGui::MenuItem("Slice plate")) startSliceJob();
        if (ImGui::MenuItem("Slice all", nullptr, false, slice_.plates > 1)) sliceAllPlates();
        ImGui::Separator();
        if (ImGui::MenuItem("Everything on the plate", nullptr, !slice_.onlySelected)) { slice_.onlySelected = false; sceneDirty_ = true; }
        if (ImGui::MenuItem("Selected parts only", nullptr, slice_.onlySelected)) { slice_.onlySelected = true; sceneDirty_ = true; }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##printmenu")) {
        if (ImGui::MenuItem("Print plate")) slice_.sendOpen = true;
        if (ImGui::MenuItem("Export plate sliced file")) sliceSaveFile();
        if (ImGui::MenuItem("Export G-code file")) sliceSaveGcode();
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    // Orca's keys in Prepare (not while typing).
    if (cur == 1 && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) plateArrange();
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            auto sel = plateSelection();
            if (sel.empty()) plateOrientPlate(slice_.activePlate);
            for (auto [oid, k] : sel) plateAutoOrient(oid);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false) && slice_.plateSel) { slice_.cutMode = true; slice_.cutZ = std::max(1.f, plateCore(slice_.plateSel).max.z * 0.5f - plateCore(slice_.plateSel).min.z * 0.5f); }
        if (ImGui::IsKeyPressed(ImGuiKey_L, false)) slice_.paintMode = slice_.paintMode == 2 ? 0 : 2;
        if (ImGui::IsKeyPressed(ImGuiKey_P, false)) slice_.paintMode = slice_.paintMode == 4 ? 0 : 4;
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) slice_.paintMode = slice_.paintMode == 1 ? 0 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_U, false)) { slice_.measure = !slice_.measure; slice_.measurePts.clear(); }
    }
    if (cur == 1 && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_I, false)) plateAddFromFile();
    // Ctrl+Tab steps through the tabs like Orca.
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) setSliceTop(cur % 5 + 1);
}

// ===================================================================== 3D toolbar

void App::drawSlice3DToolbar() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float left = sliceSideW();
    float cx = vp->Pos.x + left + (vp->Size.x - left) * 0.5f;
    const float bs = 36 * S;
    ImGui::SetNextWindowPos(ImVec2(cx, vp->Pos.y + sliceTop() + 6 * S), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 4 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2 * S, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Card));
    ImGui::Begin("##orcabar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool sel = !plateSelection().empty();
    auto btn = [&](const char* id, OIcon ic, const char* tip, bool enabled, bool active) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool c = ImGui::InvisibleButton(id, ImVec2(bs, bs)) && enabled;
        bool hv = ImGui::IsItemHovered();
        if (active) dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + bs), sl::orangeU(70), 5 * S);
        else if (hv && enabled) dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + bs), col::Hover, 5 * S);
        if (active) dl->AddRect(p, ImVec2(p.x + bs, p.y + bs), sl::orangeU(), 5 * S, 0, 1.2f * S);
        ImU32 ic2 = !enabled ? col::TextDim : active ? sl::orangeU() : col::Text;
        sl::drawOIcon(dl, ImVec2(p.x + bs * 0.5f, p.y + bs * 0.5f), bs * 0.56f, ic, ic2);
        flashRect(ImGui::GetForegroundDrawList(), p, ImVec2(p.x + bs, p.y + bs), flashAmount(std::string("s3d:") + id), 5 * S);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return c;
    };
    auto sep = [&] {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x + 4 * S, p.y + 6 * S), ImVec2(p.x + 4 * S, p.y + bs - 6 * S), col::Divider, 1.f * S);
        ImGui::Dummy(ImVec2(8 * S, bs));
        ImGui::SameLine();
    };
    if (btn("add", OIcon::AddObj, "Add  [Ctrl+I]\nA model file (STL, STEP, OBJ, 3MF) onto this plate", true, false)) plateAddFromFile();
    if (btn("addplate", OIcon::AddPlate, "Add plate", true, false)) plateNewPlate();
    if (btn("orient", OIcon::Orient, "Auto orient  [Q]\nTurns the selected parts (or the whole plate) to their best printing side", true, false)) {
        auto s = plateSelection();
        if (s.empty()) plateOrientPlate(slice_.activePlate);
        for (auto [oid, k] : s) plateAutoOrient(oid);
    }
    if (btn("arrange", OIcon::Arrange, "Arrange all objects  [A]\nShift+A: this plate only", true, false)) {
        if (ImGui::GetIO().KeyShift) plateArrangeOne(slice_.activePlate);
        else plateArrange();
    }
    sep();
    if (btn("splitobj", OIcon::SplitObj, "Split to objects\nEach separate solid becomes its own object", sel, false)) plateSplit(slice_.plateSel, false);
    if (btn("splitpart", OIcon::SplitPart, "Split to parts\nEach separate solid becomes a part of the same object (each part can have its own filament)", sel, false))
        plateSplit(slice_.plateSel, true);
    if (btn("varlayer", OIcon::VarLayer, "Variable layer height\nFiner layers where the selected part curves (height ranges)", sel, slice_.varLayer)) slice_.varLayer = !slice_.varLayer;
    sep();
    if (btn("move", OIcon::Move, "Move  [M]", sel, slice_.gizmo == 1)) slice_.gizmo = slice_.gizmo == 1 ? 0 : 1;
    if (btn("rotate", OIcon::Rotate, "Rotate  [R]", sel, slice_.gizmo == 2)) slice_.gizmo = slice_.gizmo == 2 ? 0 : 2;
    if (btn("scale", OIcon::Scale, "Scale  [S]", sel, slice_.gizmo == 3)) slice_.gizmo = slice_.gizmo == 3 ? 0 : 3;
    if (btn("layface", OIcon::LayFace, "Lay on face  [F]\nThen click the face that should sit on the plate", sel, slice_.layMode)) slice_.layMode = !slice_.layMode;
    if (btn("cut", OIcon::Cut, "Cut  [C]\nCuts the selected part at a height", sel, slice_.cutMode)) {
        slice_.cutMode = !slice_.cutMode;
        if (slice_.cutMode && slice_.plateSel) slice_.cutZ = std::max(0.5f, (plateCore(slice_.plateSel).max.z - plateCore(slice_.plateSel).min.z) * 0.5f);
    }
    sep();
    if (btn("support", OIcon::Support, "Support painting  [L]\nClick faces: supports always there (left) or never (right)", true, slice_.paintMode == 2))
        slice_.paintMode = slice_.paintMode == 2 ? 0 : 2;
    if (btn("seam", OIcon::Seam, "Seam painting  [P]\nClick faces where the seam should go (left) or not go (right)", true, slice_.paintMode == 4))
        slice_.paintMode = slice_.paintMode == 4 ? 0 : 4;
    if (btn("paint", OIcon::Paint, "Color painting  [N]\nClick faces to print them with another filament", true, slice_.paintMode == 1))
        slice_.paintMode = slice_.paintMode == 1 ? 0 : 1;
    if (btn("measure", OIcon::Measure, "Measure  [U]\nClick two points on the parts", true, slice_.measure)) { slice_.measure = !slice_.measure; slice_.measurePts.clear(); }
    ImGui::NewLine();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ===================================================================== plates

std::string App::plateName(int plate) const {
    if (plate >= 0 && plate < (int)slice_.ex.plateNames.size() && !slice_.ex.plateNames[(size_t)plate].empty()) return slice_.ex.plateNames[(size_t)plate];
    return "Untitled";
}

void App::plateNewPlate(bool makeActive) {
    if (slice_.plates >= 36) return;
    slice_.plates += 1;
    if (makeActive) slice_.activePlate = slice_.plates - 1;
    slice_.ex.plateNames.resize((size_t)slice_.plates);
    slice_.ex.plateLocked.resize((size_t)slice_.plates, 0);
    slice_.ex.plateBeds.resize((size_t)slice_.plates);
    sceneDirty_ = true;   // the view stays where it is
}

void App::plateDeletePlate(int gone) {
    if (slice_.plates <= 1 || gone < 0 || gone >= slice_.plates) return;
    // Its parts stay: they move to the plate before it (plate 1 when it was the first) and find free spots there.
    int to = gone > 0 ? gone - 1 : 0;
    std::vector<std::pair<uint32_t, int>> moved;
    for (auto& [id, o] : slice_.plate) {
        if (o.plateOf.size() < o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
        for (int k = 0; k < (int)o.plateOf.size(); ++k) {
            int& pl = o.plateOf[(size_t)k];
            if (pl == gone) { pl = to > gone ? to - 1 : to; moved.push_back({id, k}); }
            else if (pl > gone) --pl;
        }
    }
    auto drop = [&](auto& v) { if (gone < (int)v.size()) v.erase(v.begin() + gone); };
    drop(slice_.ex.plateNames);
    drop(slice_.ex.plateLocked);
    drop(slice_.ex.plateBeds);
    if (gone < (int)slice_.ex.plateResults.size()) slice_.ex.plateResults.erase(slice_.ex.plateResults.begin() + gone);
    slice_.plates = std::max(1, slice_.plates - 1);
    slice_.activePlate = std::clamp(gone > 0 ? gone - 1 : 0, 0, slice_.plates - 1);
    if (!moved.empty()) platePlaceNew(moved);
    sceneDirty_ = true;   // no camera move
}

void App::plateToFront(int p) {
    // Orca's "Move plate to the front": it becomes plate 1.
    if (p <= 0 || p >= slice_.plates) return;
    for (auto& [id, o] : slice_.plate) {
        if (o.plateOf.size() < o.pos.size()) o.plateOf.resize(o.pos.size(), 0);
        for (auto& pl : o.plateOf) pl = pl == p ? 0 : pl < p ? pl + 1 : pl;
    }
    auto move = [&](auto& v, auto fill) {
        v.resize((size_t)slice_.plates, fill);
        auto x = v[(size_t)p];
        v.erase(v.begin() + p);
        v.insert(v.begin(), x);
    };
    move(slice_.ex.plateNames, std::string());
    move(slice_.ex.plateLocked, (char)0);
    move(slice_.ex.plateBeds, std::string());
    slice_.activePlate = 0;
    sceneDirty_ = true;
}

void App::drawSlicePlateOverlays() {
    if (!slice_.bed || !slice_.bed->bounds.valid()) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const Aabb& b = slice_.bed->bounds;
    bool prev = sliceCurTop() == 2;
    int n = prev ? 1 : std::max(1, slice_.plates);
    float sideX = vp->Pos.x + sliceSideW();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    slice_.ex.plateNames.resize((size_t)std::max(slice_.plates, 1));
    slice_.ex.plateLocked.resize((size_t)std::max(slice_.plates, 1), 0);
    slice_.ex.plateBeds.resize((size_t)std::max(slice_.plates, 1));
    int renamePlate = -1, settingsPlate = -1;
    for (int pl = 0; pl < n; ++pl) {
        int which = prev ? slice_.activePlate : pl;
        glm::vec2 off = prev ? glm::vec2(0) : plateOffset(which);
        ImVec2 tl, tr, br;
        if (!worldToScreen(glm::dvec3(b.min.x + off.x, b.max.y + off.y, 0.0), tl)) continue;
        if (!worldToScreen(glm::dvec3(b.max.x + off.x, b.max.y + off.y, 0.0), tr)) continue;
        if (!worldToScreen(glm::dvec3(b.max.x + off.x, b.min.y + off.y, 0.0), br)) continue;
        bool act = which == slice_.activePlate;
        // The name above the plate (click: this plate in front; the pencil renames it).
        std::string nm = plateName(which);
        ImVec2 ns = ImGui::CalcTextSize(nm.c_str());
        ImVec2 n0(tl.x, tl.y - ns.y - 12 * S);
        if (n0.x > sideX) {
            ImU32 tc = act ? sl::orangeU() : col::TextDim;
            bg->AddText(fonts_.bold, ImGui::GetFontSize(), ImVec2(n0.x + 4 * S, n0.y + 3 * S), tc, nm.c_str());
            ImGui::SetNextWindowPos(n0);
            ImGui::SetNextWindowSize(ImVec2(ns.x + 34 * S, ns.y + 8 * S));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            std::string wid = "##platename" + std::to_string(which);
            ImGui::Begin(wid.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
            if (ImGui::InvisibleButton("##n", ImVec2(ns.x + 8 * S, ns.y + 8 * S))) { slice_.activePlate = which; sceneDirty_ = true; }
            ImGui::SetItemTooltip("Plate %d: click to make it the plate Slice plate slices", which + 1);
            ImGui::SameLine(0, 2 * S);
            ImVec2 pp = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##pen", ImVec2(20 * S, ns.y + 8 * S))) renamePlate = which;
            ImGui::SetItemTooltip("Rename this plate");
            sl::drawOIcon(ImGui::GetWindowDrawList(), ImVec2(pp.x + 10 * S, pp.y + (ns.y + 8 * S) * 0.5f), 12 * S, OIcon::Pencil,
                          ImGui::IsItemHovered() ? col::Text : col::TextDim);
            ImGui::End();
            ImGui::PopStyleVar();
        }
        // The plate number at the bottom right, Orca style ("01").
        char num[8];
        std::snprintf(num, sizeof num, "%02d", which + 1);
        if (br.x > sideX) bg->AddText(fonts_.bold, ImGui::GetFontSize() * 1.5f, ImVec2(br.x + 10 * S, br.y - ImGui::GetFontSize() * 1.6f), act ? sl::orangeU() : col::TextDim, num);
        if (prev) continue;
        // Side buttons at the top right: remove, orient, arrange, lock, settings, to the front.
        float bsz = 26 * S;
        ImVec2 s0(tr.x + 10 * S, tr.y);
        if (s0.x < sideX || s0.x > vp->Pos.x + vp->Size.x - bsz) continue;
        ImGui::SetNextWindowPos(s0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 5 * S));
        std::string wid = "##platebtns" + std::to_string(which);
        ImGui::Begin(wid.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImDrawList* wd = ImGui::GetWindowDrawList();
        auto sb = [&](const char* id, OIcon ic, const char* tip, bool enabled, bool on) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool c = ImGui::InvisibleButton(id, ImVec2(bsz, bsz)) && enabled;
            bool hv = ImGui::IsItemHovered();
            wd->AddRectFilled(p, ImVec2(p.x + bsz, p.y + bsz), hv && enabled ? col::Hover : col::Card, 4 * S);
            wd->AddRect(p, ImVec2(p.x + bsz, p.y + bsz), on ? sl::orangeU() : col::PanelBorder, 4 * S, 0, 1.f * S);
            sl::drawOIcon(wd, ImVec2(p.x + bsz * 0.5f, p.y + bsz * 0.5f), bsz * 0.55f, ic, !enabled ? col::TextDim : on ? sl::orangeU() : col::Text);
            if (hv) ImGui::SetTooltip("%s", tip);
            return c;
        };
        bool locked = slice_.ex.plateLocked[(size_t)which] != 0;
        if (sb("del", OIcon::Close, "Remove this plate", slice_.plates > 1 && !locked, false)) plateDeletePlate(which);
        if (sb("ori", OIcon::Orient, "Auto orient the objects on this plate", !locked, false)) plateOrientPlate(which);
        if (sb("arr", OIcon::Arrange, "Arrange the objects on this plate", !locked, false)) plateArrangeOne(which);
        if (sb("lock", locked ? OIcon::Lock : OIcon::Unlock, locked ? "Unlock the plate" : "Lock the plate (Arrange and Orient leave it alone)", true, locked))
            slice_.ex.plateLocked[(size_t)which] = locked ? 0 : 1;
        if (sb("set", OIcon::PlateSet, "Plate settings: name, plate type", true, false)) settingsPlate = which;
        if (sb("front", OIcon::ToFront, "Move the plate to the front (it becomes plate 1)", which > 0, false)) plateToFront(which);
        ImGui::End();
        ImGui::PopStyleVar(2);
        if (n == 1) (void)0;
    }
    // Rename and plate settings popups (one at a time).
    static int editPlate = -1;
    static std::string nameBuf;
    if (renamePlate >= 0 || settingsPlate >= 0) {
        editPlate = renamePlate >= 0 ? renamePlate : settingsPlate;
        nameBuf = editPlate < (int)slice_.ex.plateNames.size() ? slice_.ex.plateNames[(size_t)editPlate] : std::string();
        ImGui::OpenPopup("Plate settings");
    }
    ImGui::SetNextWindowSize(ImVec2(360 * S, 0));
    if (ImGui::BeginPopupModal("Plate settings", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        if (editPlate >= 0 && editPlate < slice_.plates) {
            ImGui::Text("Plate %d", editPlate + 1);
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Plate name");
            ImGui::SameLine(110 * S);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint("##pname", "Untitled", &nameBuf);
            const char* beds[] = {"Same as the project", "Cool Plate", "Engineering Plate", "High Temp Plate", "Textured PEI Plate", "Textured Cool Plate", "Supertack Plate"};
            std::string& bt = slice_.ex.plateBeds[(size_t)editPlate];
            int bi = 0;
            for (int i = 1; i < 7; ++i) if (bt == beds[i]) bi = i;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Plate type");
            ImGui::SameLine(110 * S);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##pbed", &bi, beds, 7)) bt = bi == 0 ? std::string() : beds[bi];
            std::string seq = sliceValue("print_sequence");
            int si = seq == "by object" ? 1 : 0;
            const char* seqs[] = {"By layer", "By object"};
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Print sequence");
            ImGui::SameLine(110 * S);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##pseq", &si, seqs, 2)) sliceSetValue("print_sequence", si == 1 ? "by object" : "by layer");
            bool vase = sliceValue("spiral_mode") == "1";
            if (ImGui::Checkbox("Spiral vase", &vase)) sliceSetValue("spiral_mode", vase ? "1" : "0");
        }
        ImGui::Spacing();
        if (sl::orangeButton("OK", ImVec2(100 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (editPlate >= 0 && editPlate < (int)slice_.ex.plateNames.size()) slice_.ex.plateNames[(size_t)editPlate] = nameBuf;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ===================================================================== bottom left: views

void App::drawSliceViewButtons() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float bsz = 30 * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + sliceSideW() + 14 * S, vp->Pos.y + vp->Size.y - 14 * S - bsz));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * S, 0));
    ImGui::Begin("##sliceviewbtns", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* wd = ImGui::GetWindowDrawList();
    auto rb = [&](const char* id, OIcon ic, const char* tip) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool c = ImGui::InvisibleButton(id, ImVec2(bsz, bsz));
        bool hv = ImGui::IsItemHovered();
        wd->AddCircleFilled(ImVec2(p.x + bsz * 0.5f, p.y + bsz * 0.5f), bsz * 0.5f, hv ? col::Hover : col::Card, 24);
        sl::drawOIcon(wd, ImVec2(p.x + bsz * 0.5f, p.y + bsz * 0.5f), bsz * 0.5f, ic, col::Text);
        if (hv) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return c;
    };
    if (rb("views", OIcon::Menu, "Views")) ImGui::OpenPopup("##sliceviews");
    if (rb("fit", OIcon::Zoom, "Zoom to the plate  [Z]")) slice_.framed = false;
    ImGui::NewLine();
    if (ImGui::BeginPopup("##sliceviews")) {
        struct V { const char* name; const char* key; glm::vec3 dir; };
        const V views[] = {{"Default (iso)", "0", glm::vec3(1, -1, 1)}, {"Top", "1", glm::vec3(0, 0, 1)}, {"Bottom", "2", glm::vec3(0, 0, -1)},
                           {"Front", "3", glm::vec3(0, -1, 0)}, {"Rear", "4", glm::vec3(0, 1, 0)}, {"Left", "5", glm::vec3(-1, 0, 0)},
                           {"Right", "6", glm::vec3(1, 0, 0)}};
        for (auto& v : views)
            if (ImGui::MenuItem(v.name, v.key)) sliceLookFrom(v.dir);
        ImGui::Separator();
        ImGui::MenuItem("Show labels", "E", &slice_.showLabels);
        ImGui::MenuItem("Show overhangs", nullptr, &slice_.showOverhang);
        if (ImGui::MenuItem("Perspective camera", "K", settings_.perspective)) { settings_.perspective = !settings_.perspective; settings_.save(settingsPath_); }
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    // Orca's view keys.
    if (!ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && sliceCurTop() <= 2) {
        const glm::vec3 dirs[7] = {glm::vec3(1, -1, 1), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0), glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0), glm::vec3(1, 0, 0)};
        for (int i = 0; i < 7; ++i)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + i), false)) sliceLookFrom(dirs[i]);
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) slice_.framed = false;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) slice_.showLabels = !slice_.showLabels;
    }
}

void App::sliceLookFrom(glm::vec3 dir) {
    Aabb b = sceneBounds_.valid() ? sceneBounds_ : plateBedRect();
    ImGuiIO& io = ImGui::GetIO();
    cam_.home(b, io.DisplaySize.y > 0 ? io.DisplaySize.x / io.DisplaySize.y : 1.f);
    cam_.lookFrom(glm::normalize(dir));
    sceneDirty_ = true;
    activity();
}

// ===================================================================== plate operations

uint32_t App::plateAddPart(const std::string& src, MeshPtr mesh, ShapePtr shape, const glm::mat4& bodyXform, const std::string& name, glm::vec3 color) {
    if (!mesh || mesh->indices.empty()) return 0;
    uint32_t oid = slice_.nextObj++;
    auto& o = slice_.plate[oid];
    o.src = src;
    o.stable = 0;
    o.mesh = mesh;
    o.shape = shape;
    o.bodyXform = bodyXform;
    o.name = name;
    o.color = color;
    o.pos.push_back(glm::vec2(plateBedRect().center()));
    o.plateOf.push_back(slice_.activePlate);
    o.cacheValid = false;
    sceneDirty_ = true;
    return oid;
}

void App::plateAddFromFile() {
    std::string f = platform::openFileDialog("Add a model to the plate", "Models (*.stl;*.step;*.stp;*.obj;*.3mf)", "*.stl;*.step;*.stp;*.obj;*.3mf");
    if (f.empty()) return;
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Reading " + pathToU8(u8path(f).filename()));
    jobs_->submit("add model to plate", [this, jobs, f] {
        CancelToken ct;
        auto r = std::make_shared<kernel::ImportResult>(kernel::importFile(f, ct, nullptr));
        jobs->post([this, jobs, r, f] {
            jobs->setStatus("");
            int added = 0;
            std::string stem = pathToU8(u8path(f).stem());
            for (auto& b : r->bodies) {
                if (b.part < 0 || b.part >= (int)r->parts.size()) continue;
                auto& p = r->parts[(size_t)b.part];
                MeshPtr m = p.mesh;
                if (!m && p.shape) m = kernel::tessellate(*p.shape);
                std::string nm = !b.name.empty() ? b.name : !p.name.empty() ? p.name : stem;
                uint32_t oid = plateAddPart("file:" + f, m, p.shape, b.xform, nm, b.hasColor ? b.color : glm::vec3(0.72f, 0.74f, 0.77f));
                if (oid) { ++added; plateSelect(oid, 0, added > 1); }
            }
            sliceAutoFils();
            plateArrangeOne(slice_.activePlate);
            toast(added ? "Added " + std::to_string(added) + " part" + (added == 1 ? "" : "s") + " from " + stem : "Nothing to print in " + stem,
                  added ? ToastKind::Success : ToastKind::Warning);
        });
    }, [this, jobs](const std::string& e) {
        jobs->setStatus("");
        toast("Could not read the model: " + e, ToastKind::Error);
    });
}

namespace {

glm::mat4 rotationBetweenV(glm::vec3 a, glm::vec3 b) {
    a = glm::normalize(a);
    b = glm::normalize(b);
    float c = glm::dot(a, b);
    if (c > 0.99999f) return glm::mat4(1.f);
    if (c < -0.99999f) {
        glm::vec3 ax = std::abs(a.x) < 0.9f ? glm::cross(a, glm::vec3(1, 0, 0)) : glm::cross(a, glm::vec3(0, 1, 0));
        return glm::rotate(glm::mat4(1.f), 3.14159265f, glm::normalize(ax));
    }
    return glm::rotate(glm::mat4(1.f), std::acos(std::clamp(c, -1.f, 1.f)), glm::normalize(glm::cross(a, b)));
}

} // namespace

void App::plateAutoOrient(uint32_t oid) {
    // Orca's auto orient, simply: try the part's big flat sides (and how it is now) as the bottom;
    // keep the one with the least overhang to hold up and the most contact with the plate.
    auto it = slice_.plate.find(oid);
    if (it == slice_.plate.end() || !it->second.mesh) return;
    auto& o = it->second;
    if (plateOfCopy(o, 0) < (int)slice_.ex.plateLocked.size() && slice_.ex.plateLocked[(size_t)plateOfCopy(o, 0)]) return;
    const MeshData& m = *o.mesh;
    glm::mat4 base = glm::scale(glm::mat4(1.f), o.scale) * o.bodyXform;
    size_t nt = m.indices.size() / 3;
    std::vector<glm::vec3> cr(nt), cen(nt);
    std::map<std::tuple<int, int, int>, std::pair<float, glm::vec3>> groups;
    for (size_t t = 0; t < nt; ++t) {
        auto v = [&](uint32_t i) { return glm::vec3(base * glm::vec4(m.verts[i * 6], m.verts[i * 6 + 1], m.verts[i * 6 + 2], 1.f)); };
        glm::vec3 a = v(m.indices[t * 3]), b = v(m.indices[t * 3 + 1]), c = v(m.indices[t * 3 + 2]);
        cr[t] = glm::cross(b - a, c - a);
        cen[t] = (a + b + c) / 3.f;
        float area = glm::length(cr[t]);
        if (area < 1e-9f) continue;
        glm::vec3 u = cr[t] / area;
        auto key = std::make_tuple((int)std::lround(u.x * 30), (int)std::lround(u.y * 30), (int)std::lround(u.z * 30));
        auto& g = groups[key];
        g.first += area;
        g.second += cr[t];
    }
    std::vector<std::pair<float, glm::vec3>> cands(groups.size());
    std::transform(groups.begin(), groups.end(), cands.begin(), [](auto& kv) { return kv.second; });
    std::sort(cands.begin(), cands.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (cands.size() > 14) cands.resize(14);
    std::vector<glm::mat4> rots;
    rots.push_back(o.lay);
    for (auto& [area, n] : cands) rots.push_back(rotationBetweenV(n, glm::vec3(0, 0, -1)));
    float bestScore = 1e30f;
    glm::mat4 best = o.lay;
    for (auto& R : rots) {
        glm::mat3 r3(R);
        float minZ = 1e30f, maxZ = -1e30f;
        for (size_t t = 0; t < nt; ++t) { float z = (r3 * cen[t]).z; minZ = std::min(minZ, z); maxZ = std::max(maxZ, z); }
        float over = 0, contact = 0;
        for (size_t t = 0; t < nt; ++t) {
            glm::vec3 n = r3 * cr[t];
            float area = glm::length(n);
            if (area < 1e-9f) continue;
            float nz = n.z / area, z = (r3 * cen[t]).z;
            if (z - minZ < 0.2f && nz < -0.99f) contact += area * 0.5f;
            else if (nz < -0.707f) over += area * 0.5f * (-nz);
        }
        float score = over - 1.5f * contact + 0.02f * (maxZ - minZ);
        if (score < bestScore - 1e-3f) { bestScore = score; best = R; }
    }
    o.lay = best;
    o.rotZ = 0;
    o.cacheValid = false;
    sceneDirty_ = true;
}

void App::plateOrientPlate(int plate) {
    if (plate < (int)slice_.ex.plateLocked.size() && slice_.ex.plateLocked[(size_t)plate]) return;
    for (auto& [oid, o] : slice_.plate)
        if (o.onPlate && plateOfCopy(o, 0) == plate && plateInFilter(oid)) plateAutoOrient(oid);
    plateArrangeOne(plate);
}

void App::plateSplit(uint32_t oid, bool parts) {
    auto it = slice_.plate.find(oid);
    if (it == slice_.plate.end()) return;
    SliceTab::PlateObj src = it->second;
    std::vector<std::pair<MeshPtr, ShapePtr>> pieces;
    if (src.shape) {
        for (auto& s : kernel::solidsOf(src.shape)) pieces.push_back({kernel::tessellate(*s), s});
    } else if (src.mesh) {
        // A mesh: its connected pieces (triangles that share corners).
        const MeshData& m = *src.mesh;
        size_t nv = m.vertexCount(), nt = m.indices.size() / 3;
        std::vector<uint32_t> parent(nv);
        std::iota(parent.begin(), parent.end(), 0u);
        std::function<uint32_t(uint32_t)> find = [&](uint32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
        // Corners at the same spot count as one (meshes often repeat them per face).
        std::map<std::tuple<long, long, long>, uint32_t> at;
        for (uint32_t v = 0; v < nv; ++v) {
            auto key = std::make_tuple(std::lround(m.verts[v * 6] * 1000), std::lround(m.verts[v * 6 + 1] * 1000), std::lround(m.verts[v * 6 + 2] * 1000));
            auto f = at.find(key);
            if (f == at.end()) at[key] = v;
            else parent[find(v)] = find(f->second);
        }
        for (size_t t = 0; t < nt; ++t) {
            uint32_t a = find(m.indices[t * 3]);
            parent[find(m.indices[t * 3 + 1])] = a;
            parent[find(m.indices[t * 3 + 2])] = a;
        }
        std::map<uint32_t, std::shared_ptr<MeshData>> out;
        std::map<uint32_t, std::map<uint32_t, uint32_t>> remap;
        for (size_t t = 0; t < nt; ++t) {
            uint32_t r = find(m.indices[t * 3]);
            auto& md = out[r];
            if (!md) md = std::make_shared<MeshData>();
            auto& rm = remap[r];
            for (int k = 0; k < 3; ++k) {
                uint32_t v = m.indices[t * 3 + k];
                auto f = rm.find(v);
                if (f == rm.end()) {
                    f = rm.emplace(v, (uint32_t)md->vertexCount()).first;
                    md->verts.insert(md->verts.end(), m.verts.begin() + v * 6, m.verts.begin() + v * 6 + 6);
                    md->bounds.add(glm::vec3(m.verts[v * 6], m.verts[v * 6 + 1], m.verts[v * 6 + 2]));
                }
                md->indices.push_back(f->second);
            }
            md->triFace.push_back(0);
        }
        for (auto& [r, md] : out) { md->faceCount = 1; pieces.push_back({md, nullptr}); }
    }
    if (pieces.size() < 2) { toast("This part is one piece already", ToastKind::Info); return; }
    uint32_t group = parts ? slice_.nextObj + 100000 : 0;
    int k = 0;
    for (auto& [mesh, shape] : pieces) {
        if (!mesh || mesh->indices.empty()) continue;
        uint32_t nid = slice_.nextObj++;
        SliceTab::PlateObj c = src;
        c.mesh = mesh;
        c.shape = shape;
        c.src = src.src + "#split";
        c.stable = 0;
        c.bodyId = 0;
        c.group = group;
        c.name = src.name + (parts ? " part " : " ") + std::to_string(++k);
        c.paintColor.clear();
        c.paintSupport.clear();
        c.paintSeam.clear();
        c.cacheValid = false;
        if (parts) {
            // Parts stay where they were in the object: same center, so they sit together.
            glm::vec3 whole = src.mesh->bounds.transformed(src.bodyXform).center();
            glm::vec3 mine = mesh->bounds.transformed(src.bodyXform).center();
            glm::vec3 d = glm::mat3(glm::rotate(glm::mat4(1.f), glm::radians(src.rotZ), glm::vec3(0, 0, 1)) * src.lay * glm::scale(glm::mat4(1.f), src.scale)) * (mine - whole);
            for (auto& p : c.pos) p += glm::vec2(d.x, d.y);
        }
        slice_.plate[nid] = std::move(c);
    }
    if (plateOwns(src.src) && src.src == sliceDocKey()) slice_.plate[oid].onPlate = false;   // the design's part: off the plate, can come back
    else slice_.plate.erase(oid);
    plateSelect(0, 0, false);
    sliceAutoFils();
    if (!parts) plateArrangeOne(slice_.activePlate);
    sceneDirty_ = true;
    toast("Split into " + std::to_string(k) + (parts ? " parts" : " objects"), ToastKind::Success);
}

void App::plateCutSelected() {
    uint32_t oid = slice_.plateSel;
    auto it = slice_.plate.find(oid);
    if (it == slice_.plate.end() || !it->second.mesh) return;
    SliceTab::PlateObj src = it->second;
    if (!src.shape) { toast("Cutting needs a solid (a mesh from a file can not be cut here yet)", ToastKind::Warning); return; }
    int k = slice_.plateSelCopy;
    glm::mat4 X = plateXform(oid, k);
    // The cut is level, at cutZ above the plate; in the part's own coordinates for the kernel.
    // X maps the part's own coordinates (the shape's) onto the plate: the plane goes back through it.
    glm::vec4 ps = glm::inverse(X) * glm::vec4(0.f, 0.f, slice_.cutZ, 1.f);
    glm::vec3 nl = glm::normalize(glm::transpose(glm::mat3(X)) * glm::vec3(0, 0, 1));
    std::vector<ShapePtr> out;
    try {
        out = kernel::splitByPlane(src.shape, glm::dvec3(ps), glm::dvec3(nl));
    } catch (const std::exception& e) {
        toast(std::string("Cut failed: ") + e.what(), ToastKind::Error);
        return;
    }
    if (out.size() < 2) { toast("The cut height misses the part", ToastKind::Warning); return; }
    int made = 0;
    for (auto& s : out) {
        MeshPtr mm = kernel::tessellate(*s);
        if (!mm || mm->indices.empty()) continue;
        // Which side: compare its center with the cut plane.
        glm::vec3 c = glm::vec3(X * glm::vec4(mm->bounds.center(), 1.f));
        bool upper = c.z > slice_.cutZ;
        if ((slice_.cutKeep == 1 && !upper) || (slice_.cutKeep == 2 && upper)) continue;
        uint32_t nid = slice_.nextObj++;
        SliceTab::PlateObj p = src;
        p.mesh = mm;
        p.shape = s;
        p.src = src.src + "#cut";
        p.stable = 0;
        p.bodyId = 0;
        p.name = src.name + (upper ? " (top)" : " (bottom)");
        p.paintColor.clear();
        p.paintSupport.clear();
        p.paintSeam.clear();
        p.cacheValid = false;
        p.pos = {src.pos.empty() ? glm::vec2(plateBedRect().center()) : src.pos[(size_t)std::clamp(k, 0, (int)src.pos.size() - 1)]};
        p.plateOf = {plateOfCopy(src, k)};
        // The top piece turns over so its cut face sits on the plate (Orca's default).
        if (upper) p.lay = glm::rotate(glm::mat4(1.f), 3.14159265f, glm::vec3(1, 0, 0)) * p.lay;
        slice_.plate[nid] = std::move(p);
        ++made;
    }
    if (plateOwns(src.src) && src.src == sliceDocKey()) slice_.plate[oid].onPlate = false;
    else slice_.plate.erase(oid);
    plateSelect(0, 0, false);
    slice_.cutMode = false;
    sliceAutoFils();
    plateArrangeOne(slice_.activePlate);
    toast("Cut into " + std::to_string(made) + " pieces", ToastKind::Success);
}

} // namespace kst
