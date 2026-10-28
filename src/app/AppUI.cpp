// Fusion style layout: app bar, toolbar with tabs and groups, floating browser on the left,
// view cube top right, navigation bar bottom center, timeline along the bottom.
#include "app/App.h"
#include "app/Materials.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "kernel/Features.h"
#include "platform/Platform.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace kst {

using namespace ui;

namespace {

constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

// Square icon button with hover background. Returns true on click (only when enabled).
bool iconButton(const char* id, Icon icon, float size, bool enabled, const char* tip, bool active = false) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col::Active, 4.f);
    else if (hov && enabled) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col::Hover, 4.f);
    drawIcon(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.68f, icon, enabled);
    if (hov && tip && *tip) ImGui::SetTooltip("%s", tip);
    return ImGui::IsItemClicked(ImGuiMouseButton_Left) && enabled;
}

bool containsI(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

} // namespace

void App::toast(const std::string& msg, ToastKind kind) {
    if (kind == ToastKind::Error) log::error(msg);
    else if (kind == ToastKind::Warning) log::warn(msg);
    else log::info(msg);
    toasts_.push_back({msg, kind, time_});
    if (aiCapture_ && aiCaptured_.size() < 12) aiCaptured_.push_back(msg);
    while (toasts_.size() > 5) toasts_.pop_front();
    activity();
}

// ===================================================================== root

void App::drawUI() {
    if (themeDirty_) { themeDirty_ = false; ui::applyTheme(uiScale_, settings_.darkMode ? 1.f : 0.f); }
    if (playing_) {
        playT_ += ImGui::GetIO().DeltaTime;
        if (playT_ > 0.45) {
            playT_ = 0;
            if (doc_.markerPos >= (int)doc_.timeline.size()) playing_ = false;
            else setMarker(doc_.markerPos + 1);
        }
        activity();
    }
    flushTextureGraveyard();
    // Read the print profiles, settings and bed soon after start, so the Slice tab opens ready.
    if (!slicePrewarmed_ && ImGui::GetFrameCount() > 30) {
        slicePrewarmed_ = true;
        if (!slice_.choicesLoaded) sliceLoadChoices();
    }
    if (slicePrewarmed_ && !sliceWs_ && slice_.choicesLoaded) sliceLoadBed();
    drawCloseAsk();
    drawAppBar();
    if (sliceWs_) {
        // Kestral's slicer screen; the design UI rests.
        drawSliceBar();
        drawSliceWorkspace();
        drawToasts();
        drawSettings();
        drawFusionMove();
        drawAssistant();   // the assistant works here too
        keepWindowsOnScreen();
        return;
    }
    if (startOpen_ && (!doc_.timeline.empty() || !doc_.bodies.empty())) startOpen_ = false;   // a design opened
    if (startOpen_) {
        updateThumbnails();
        drawStartPage();
        drawToasts();
        drawSettings();
        drawFusionMove();
        keepWindowsOnScreen();
        return;
    }
    drawToolbar();
    drawTimeline();
    drawBrowser();
    updateThumbnails();
    drawDataPanel();
    drawNavBar();
    drawViewCube();
    drawIsolateBanner();
    drawStatus();
    drawToasts();
    drawDonateNote();
    drawPalette();
    drawSettings();
    drawKeysDialog();
    drawFusionMove();
    drawRobotWindow();
    drawAssistant();
    drawAppearance();
    drawSketchPalette();
    drawDimensionEditor();
    drawFeatureDialog();
    drawSectionDialog();
    drawToolDialog();
    drawParamsDialog();
    drawMeasurePanel();
    drawConstructPlanes();
    drawRecoveryPrompt();
    drawBreakCheck();
    drawMarkingMenu();
    keepWindowsOnScreen();
}

// Windows (dialogs, assistant, appearance...) never end up off screen after a resize or a
// move to a smaller monitor.
void App::keepWindowsOnScreen() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float S = uiScale_;
    // Minimized or mid Alt+Tab the window can report a tiny or zero size for a frame. Clamping to
    // that would throw every window into the top left corner for good.
    if (vp->Size.x < 300 * S || vp->Size.y < 200 * S) return;
    for (ImGuiWindow* w : g.Windows) {
        if (!w->WasActive || w->Hidden) continue;
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) continue;
        ImVec2 p = w->Pos;
        float minX = vp->Pos.x - w->Size.x + 80 * S, maxX = vp->Pos.x + vp->Size.x - 80 * S;
        float minY = vp->Pos.y, maxY = vp->Pos.y + vp->Size.y - 40 * S;
        ImVec2 q(std::clamp(p.x, minX, std::max(minX, maxX)), std::clamp(p.y, minY, std::max(minY, maxY)));
        if (q.x != p.x || q.y != p.y) ImGui::SetWindowPos(w->Name, q);
    }
}

// ===================================================================== app bar

void App::drawAppBar() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float h = 34.f * S;
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::AppBar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 3 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##appbar", nullptr, kPanel);
    float bs = 28.f * S;

    {   // Brand on the left, like pro CAD apps: logo mark and wordmark, then the file tools.
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        drawLogo(bdl, ImVec2(p0.x + 4 * S, vp->Pos.y + (h - 20 * S) * 0.5f), 20 * S);
        ImGui::PushFont(fonts_.bold, 0.f);
        const char* wm = "KESTRAL";
        ImVec2 wsz = ImGui::CalcTextSize(wm);
        bdl->AddText(ImVec2(p0.x + 30 * S, vp->Pos.y + (h - wsz.y) * 0.5f), col::Text, wm);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 38 * S + wsz.x, p0.y));
        bdl->AddLine(ImVec2(p0.x + 34 * S + wsz.x, vp->Pos.y + 9 * S), ImVec2(p0.x + 34 * S + wsz.x, vp->Pos.y + h - 9 * S), col::Divider);
    }
    if (iconButton("##file", Icon::File, bs, true, "File")) ImGui::OpenPopup("##filemenu");
    if (ImGui::BeginPopup("##filemenu")) {
        if (ImGui::MenuItem("Start Page")) startOpen_ = true;
        if (ImGui::MenuItem("New Design")) {
            if (doc_.bodies.empty() && doc_.timeline.empty()) newDocument();
            else confirmNew_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open Design...", "Ctrl+O")) openDocumentDialog();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, true)) saveDocumentNow();
        if (ImGui::MenuItem("Save As...")) saveDocumentAs();
        if (ImGui::MenuItem("Insert...", "Ctrl+I")) importDialog();
        if (ImGui::MenuItem("Move from Fusion (all designs)...")) fusionMoveOpen_ = true;
        if (ImGui::BeginMenu("Recent", !settings_.recentFiles.empty())) {
            for (auto& r : settings_.recentFiles)
                if (ImGui::MenuItem(r.c_str())) importPath(r);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export STL...")) exportStlDialog();
        if (ImGui::MenuItem("Export STEP...")) exportStepDialog();
        if (ImGui::MenuItem("Export 3MF...")) export3mfDialog();
        if (ImGui::MenuItem("Export Robot (URDF / USD)...")) robotOpen_ = true;
        if (ImGui::MenuItem("Send to Slicer", "Ctrl+P")) sendToSlicer();
        if (ImGui::MenuItem("Slice in OrcaSlicer (inside Kestral)")) enterSliceWorkspace();
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences...")) settingsOpen_ = true;
        if (ImGui::MenuItem("Open Log Folder")) platform::revealInExplorer(platform::appDataDir() + "/kestral.log");
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) requestQuit();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##data", Icon::Folder, bs, true, "Data panel: all your designs", dataOpen_)) {
        if (dataOpen_) dataOpen_ = false;
        else openDataPanel(dataCur_);
    }
    ImGui::SameLine(0, 2 * S);
    iconButton("##save", Icon::Save, bs, true, "Save (Ctrl+S)");
    if (ImGui::IsItemClicked()) saveDocumentNow();
    ImGui::SameLine(0, 2 * S);
    std::string ut = undo_.canUndo() ? "Undo " + undo_.undoLabel() + " (Ctrl+Z)" : "Undo (Ctrl+Z)";
    if (iconButton("##undo", Icon::Undo, bs, undo_.canUndo(), ut.c_str())) undo();
    ImGui::SameLine(0, 2 * S);
    std::string rt = undo_.canRedo() ? "Redo " + undo_.redoLabel() + " (Ctrl+Y)" : "Redo (Ctrl+Y)";
    if (iconButton("##redo", Icon::Redo, bs, undo_.canRedo(), rt.c_str())) redo();

    // Design tabs (center), like Fusion's document tabs, then the slice tabs after them.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int nD = startOpen_ ? 1 : (int)designTabs_.size();
    std::vector<std::string> titles;
    std::vector<float> widths;
    float total = 0;
    for (int i = 0; i < nD; ++i) {
        titles.push_back(startOpen_ ? std::string("Home") : designTabTitle(i));
        float w = std::max(ImGui::CalcTextSize(titles.back().c_str()).x + (startOpen_ ? 40 : 58) * S, 150 * S);
        widths.push_back(w);
        total += w + 3 * S;
    }
    float tx = std::max(260 * S, (vp->Size.x - total - 30 * S - 150 * S * std::max<size_t>(1, sliceDocs_.size())) * 0.5f);
    ImVec2 t0(vp->Pos.x + tx, vp->Pos.y + 4 * S), t1(t0.x + 10 * S, vp->Pos.y + h);
    {
        float x = t0.x;
        int closeD = -1;
        ImVec2 a0 = t0, a1 = t1;   // the tab in front (the versions menu opens under it)
        for (int i = 0; i < nD; ++i) {
            bool act = i == designActive_;
            ImVec2 s0(x, t0.y), s1(x + widths[(size_t)i], t1.y);
            ImGui::PushID(i + 9000);
            ImGui::SetCursorScreenPos(s0);
            bool clicked = ImGui::InvisibleButton("##designtab", ImVec2(widths[(size_t)i] - (startOpen_ ? 0 : 22 * S), h - 4 * S));
            bool dh = ImGui::IsItemHovered();
            if (dh && act && !sliceWs_ && !doc_.path.empty()) ImGui::SetTooltip("Versions of this design");
            else if (dh && !act) ImGui::SetTooltip("%s", designTabs_[(size_t)i].doc.path.empty() ? titles[(size_t)i].c_str() : designTabs_[(size_t)i].doc.path.c_str());
            bool xh = false;
            if (!startOpen_) {
                ImGui::SetCursorScreenPos(ImVec2(s1.x - 22 * S, s0.y));
                if (ImGui::InvisibleButton("##designx", ImVec2(20 * S, h - 4 * S))) closeD = i;
                xh = ImGui::IsItemHovered();
                if (xh) ImGui::SetTooltip("Close this design (unsaved work goes to the autosave)");
            }
            ImGui::PopID();
            if (clicked) {
                if (sliceWs_) { leaveSliceWorkspace(); if (!act) switchDesignTab(i); }
                else if (!act) switchDesignTab(i);
                else if (!startOpen_ && !doc_.path.empty()) ImGui::OpenPopup("##vermenu");   // the versions dropdown
            }
            bool front = act && !sliceWs_;
            dl->AddRectFilled(s0, s1, front ? col::Card : (dh || xh ? col::Hover : col::AppBar), 5 * S, ImDrawFlags_RoundCornersTop);
            if (front) dl->AddRectFilled(ImVec2(s0.x + 8 * S, s1.y - 2.5f * S), ImVec2(s1.x - 8 * S, s1.y), col::Accent, 1.5f * S);
            ImVec2 ts = ImGui::CalcTextSize(titles[(size_t)i].c_str());
            float textW = widths[(size_t)i] - (startOpen_ ? 0 : 22 * S);
            dl->AddText(ImVec2(s0.x + (textW - ts.x) * 0.5f, s0.y + (h - 4 * S - ts.y) * 0.5f), front ? col::Text : col::TextDim, titles[(size_t)i].c_str());
            if (!startOpen_) {
                ImVec2 xc(s1.x - 12 * S, (s0.y + s1.y) * 0.5f);
                if (xh) dl->AddCircleFilled(xc, 7 * S, col::Hover, 16);
                float r = 3.5f * S;
                dl->AddLine(ImVec2(xc.x - r, xc.y - r), ImVec2(xc.x + r, xc.y + r), xh ? col::Text : col::TextDim, 1.4f * S);
                dl->AddLine(ImVec2(xc.x - r, xc.y + r), ImVec2(xc.x + r, xc.y - r), xh ? col::Text : col::TextDim, 1.4f * S);
            }
            if (act) { a0 = s0; a1 = s1; }
            x = s1.x + 3 * S;
        }
        // "+": a new design in its own tab.
        if (!startOpen_) {
            ImVec2 p0(x, t0.y), p1(x + 26 * S, t1.y);
            ImGui::SetCursorScreenPos(p0);
            if (ImGui::InvisibleButton("##newdesigntab", ImVec2(26 * S, h - 4 * S))) { if (sliceWs_) leaveSliceWorkspace(); newDesignTab(); }
            bool ph = ImGui::IsItemHovered();
            if (ph) ImGui::SetTooltip("New design (its own tab)");
            if (ph) dl->AddRectFilled(p0, p1, col::Hover, 5 * S, ImDrawFlags_RoundCornersTop);
            ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            dl->AddLine(ImVec2(c.x - 5 * S, c.y), ImVec2(c.x + 5 * S, c.y), ph ? col::Text : col::TextDim, 1.5f * S);
            dl->AddLine(ImVec2(c.x, c.y - 5 * S), ImVec2(c.x, c.y + 5 * S), ph ? col::Text : col::TextDim, 1.5f * S);
            x = p1.x + 6 * S;
        }
        if (closeD >= 0) {
            bool dirtyT = closeD == designActive_ ? dirty_ : designTabs_[(size_t)closeD].dirty;
            if (dirtyT) { closeAskTab_ = closeD; ImGui::OpenPopup("Close design?"); }
            else closeDesignTab(closeD);
        }
        if (ImGui::BeginPopupModal("Close design?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            int ct = closeAskTab_;
            std::string nm = ct >= 0 && ct < (int)designTabs_.size() ? (ct == designActive_ ? doc_.name : designTabs_[(size_t)ct].doc.name) : std::string();
            ImGui::Text("%s has changes that are not saved.", nm.c_str());
            if (ImGui::Button("Save", ImVec2(110 * S, 0))) {
                if (ct != designActive_) switchDesignTab(ct);
                saveDocumentNow();
                if (!dirty_) closeDesignTab(designActive_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't save", ImVec2(110 * S, 0))) {
                if (ct == designActive_) dirty_ = false;
                else if (ct >= 0 && ct < (int)designTabs_.size()) designTabs_[(size_t)ct].dirty = false;
                closeDesignTab(ct);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        t1.x = x - 4 * S;
        ImGui::SetNextWindowPos(ImVec2(a0.x, a1.y + 2 * S));
        if (ImGui::BeginPopup("##vermenu")) {
            auto vers = designVersions(doc_.path);
            ImGui::TextDisabled("Versions of %s", doc_.name.c_str());
            ImGui::Separator();
            if (vers.empty()) ImGui::TextDisabled("Ctrl+S saves v1");
            for (int v : vers) {
                std::string lbl = "v" + std::to_string(v) + (v == doc_.version ? "   (open)" : "");
                if (ImGui::MenuItem(lbl.c_str(), nullptr, v == doc_.version)) {
                    if (v != doc_.version) openDesignVersion(doc_.path, v);
                }
            }
            ImGui::EndPopup();
        }
        // One Slice tab per design you slice (each with its own plate and result), then "Slice" for
        // the open design when it has none yet.
        x = t1.x + 4 * S;
        int closeTab = -1, mergeFrom = -1, mergeInto = -1;
        bool haveOwn = sliceTabFor(sliceDocKey()) >= 0;
        std::vector<std::pair<ImVec2, ImVec2>> tabRects;
        for (int i = 0; i <= (int)sliceDocs_.size(); ++i) {
            bool isNew = i == (int)sliceDocs_.size();
            if (isNew && haveOwn) break;
            std::string lbl = isNew ? std::string("Slice") : sliceDocs_[(size_t)i].name;
            if (lbl.size() > 22) lbl = lbl.substr(0, 21) + "...";
            ImVec2 ss = ImGui::CalcTextSize(lbl.c_str());
            float sw = ss.x + (isNew ? 44 : 62) * S;
            ImVec2 s0(x, t0.y), s1(x + sw, t1.y);
            bool on = sliceWs_ && i == sliceActive_;
            ImGui::PushID(i + 7000);
            ImGui::SetCursorScreenPos(s0);
            bool clicked = ImGui::InvisibleButton("##slicetab", ImVec2(sw - (isNew ? 0 : 20 * S), h - 4 * S));
            bool sh = ImGui::IsItemHovered();
            // Drag a slice tab onto another: its parts join that tab's plate.
            if (!isNew && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8 * S)) sliceTabDrag_ = i;
            if (sliceTabDrag_ >= 0) clicked = false;
            if (!isNew && ImGui::BeginPopupContextItem("##tabctx")) {
                ImGui::TextDisabled("%s", sliceDocs_[(size_t)i].name.c_str());
                ImGui::Separator();
                if (ImGui::BeginMenu("Combine into", sliceDocs_.size() > 1)) {
                    for (int j = 0; j < (int)sliceDocs_.size(); ++j)
                        if (j != i && ImGui::MenuItem(sliceDocs_[(size_t)j].name.c_str())) { mergeFrom = i; mergeInto = j; }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Close")) closeTab = i;
                ImGui::EndPopup();
            }
            tabRects.push_back({s0, s1});
            if (sh) ImGui::SetTooltip(isNew ? "Slice this design (OrcaSlicer, inside Kestral)" : "%s", isNew ? "" : sliceDocs_[(size_t)i].key.c_str());
            bool xh = false;
            if (!isNew) {
                ImGui::SetCursorScreenPos(ImVec2(s1.x - 20 * S, s0.y));
                if (ImGui::InvisibleButton("##slicex", ImVec2(18 * S, h - 4 * S))) closeTab = i;
                xh = ImGui::IsItemHovered();
                if (xh) ImGui::SetTooltip("Close this slice tab");
            }
            ImGui::PopID();
            if (clicked) {
                if (isNew) enterSliceWorkspace();
                else enterSliceTab(i);
            }
            dl->AddRectFilled(s0, s1, on ? col::Card : (sh || xh ? col::Hover : col::AppBar), 5 * S, ImDrawFlags_RoundCornersTop);
            drawIcon(dl, ImVec2(s0.x + 14 * S, (s0.y + s1.y) * 0.5f), 16 * S, Icon::Slicer, true);
            dl->AddText(ImVec2(s0.x + 26 * S, s0.y + (h - 4 * S - ss.y) * 0.5f), on ? col::Print : col::TextDim, lbl.c_str());
            if (!isNew) {
                ImVec2 xc(s1.x - 11 * S, (s0.y + s1.y) * 0.5f);
                if (xh) dl->AddCircleFilled(xc, 7 * S, col::Hover, 16);
                float r = 3.5f * S;
                dl->AddLine(ImVec2(xc.x - r, xc.y - r), ImVec2(xc.x + r, xc.y + r), xh ? col::Text : col::TextDim, 1.4f * S);
                dl->AddLine(ImVec2(xc.x - r, xc.y + r), ImVec2(xc.x + r, xc.y - r), xh ? col::Text : col::TextDim, 1.4f * S);
            }
            x = s1.x + 3 * S;
        }
        if (sliceTabDrag_ >= 0 && sliceTabDrag_ < (int)sliceDocs_.size()) {
            ImVec2 m = ImGui::GetIO().MousePos;
            int over = -1;
            for (int i = 0; i < (int)sliceDocs_.size() && i < (int)tabRects.size(); ++i)
                if (i != sliceTabDrag_ && m.x >= tabRects[(size_t)i].first.x && m.x <= tabRects[(size_t)i].second.x && m.y <= tabRects[(size_t)i].second.y + 30 * S)
                    over = i;
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            if (over >= 0) fg->AddRect(tabRects[(size_t)over].first, tabRects[(size_t)over].second, col::Print, 5 * S, 0, 2.f * S);
            std::string tip = over >= 0 ? "Put " + sliceDocs_[(size_t)sliceTabDrag_].name + " on " + sliceDocs_[(size_t)over].name + "'s plate"
                                        : "Drop it on another slice tab to combine them";
            ImVec2 ts = ImGui::CalcTextSize(tip.c_str());
            fg->AddRectFilled(ImVec2(m.x + 12 * S, m.y + 14 * S), ImVec2(m.x + 24 * S + ts.x, m.y + 22 * S + ts.y), IM_COL32(20, 21, 24, 230), 6 * S);
            fg->AddText(ImVec2(m.x + 18 * S, m.y + 18 * S), IM_COL32(240, 240, 240, 255), tip.c_str());
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (over >= 0) { mergeFrom = sliceTabDrag_; mergeInto = over; }
                sliceTabDrag_ = -1;
            }
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            sliceTabDrag_ = -1;
        }
        if (mergeFrom >= 0 && mergeInto >= 0) sliceMergeTabs(mergeFrom, mergeInto);
        else if (closeTab >= 0) closeSliceTab(closeTab);
    }

    float brandX = 0;
    // Dark mode: a switch, and a slider for how dark (shown while it is on).
    {
        float sw = 34 * S, sh = 18 * S, sliderW = 96 * S;
        float x = vp->Pos.x + vp->Size.x - 56 * S - sw;
        (void)sliderW; (void)brandX;
        float y = vp->Pos.y + (h - sh) * 0.5f;
        // Moon / sun glyph in front of the switch.
        ImVec2 gc(x - 12 * S, y + sh * 0.5f);
        if (settings_.darkMode) {
            dl->AddCircleFilled(gc, 6 * S, col::TextDim, 20);
            dl->AddCircleFilled(ImVec2(gc.x + 3.2f * S, gc.y - 2.4f * S), 5.2f * S, col::AppBar, 20);
        } else {
            dl->AddCircleFilled(gc, 3.6f * S, col::TextDim, 16);
            for (int k = 0; k < 8; ++k) {
                float a = k * 0.785398f;
                dl->AddLine(ImVec2(gc.x + std::cos(a) * 5.4f * S, gc.y + std::sin(a) * 5.4f * S),
                            ImVec2(gc.x + std::cos(a) * 7.4f * S, gc.y + std::sin(a) * 7.4f * S), col::TextDim, 1.3f * S);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(x - 22 * S, y));
        ImGui::InvisibleButton("##darkmode", ImVec2(sw + 22 * S, sh));
        bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip(settings_.darkMode ? "Dark mode on. Click for light mode." : "Dark mode");
        if (ImGui::IsItemClicked()) {
            settings_.darkMode = !settings_.darkMode;
            themeDirty_ = true;   // applied at the start of the next frame (style pushes here would undo it)
            settings_.save(settingsPath_);
        }
        ImU32 track = settings_.darkMode ? col::Accent : (hov ? col::PanelBorder : col::Chip);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sw, y + sh), track, sh * 0.5f);
        float kx = settings_.darkMode ? x + sw - sh * 0.5f : x + sh * 0.5f;
        dl->AddCircleFilled(ImVec2(kx, y + sh * 0.5f), sh * 0.5f - 2.5f * S, IM_COL32(255, 255, 255, 255), 20);
        // Search anything, left of the switch.
        drawGlobalSearch(dl, x - 34 * S, vp->Pos.y, h);
    }
    ImGui::SameLine(vp->Size.x - 36 * S);
    if (iconButton("##prefs", Icon::Gear, bs, true, "Preferences", settingsOpen_)) settingsOpen_ = !settingsOpen_;

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    if (confirmNew_) { ImGui::OpenPopup("New design?"); confirmNew_ = false; }
    if (ImGui::BeginPopupModal("New design?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Start a new design? Unsaved changes are kept\nin the autosave and offered on the next start.");
        if (ImGui::Button("New Design", ImVec2(120 * S, 0))) {
            if (dirty_ && !doc_.timeline.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(u8path(autosavePath()).parent_path(), ec);
                saveDocument(doc_, autosavePath());
            }
            newDocument();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120 * S, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ===================================================================== toolbar

bool App::toolButton(int ti, float size, bool) {
    const Tool& t = tools_[ti];
    std::string id = "##tb_" + t.id;
    std::string tip = t.label + (t.shortcut.empty() ? "" : "  (" + t.shortcut + ")");
    if (!t.tip.empty()) tip += "\n" + t.tip;
    if (!t.available()) tip += "\nComing in milestone M" + std::to_string(t.milestone);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id.c_str(), ImVec2(size, size));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool active = sk_.active && !sk_.tool.empty() && (sk_.tool == t.id || (t.id == "sk_point" && sk_.tool == "point"));
    if (active) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col::Active, 4.f);
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col::Hover, 4.f);
    drawIcon(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.74f, t.icon, t.available());
    flashRect(ImGui::GetForegroundDrawList(), p, ImVec2(p.x + size, p.y + size), flashAmount("tool:" + t.id), 6.f);
    if (hov) ImGui::SetTooltip("%s", tip.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { tbCtxTool_ = ti; ImGui::OpenPopup("##tbctx"); }
    if (ImGui::IsItemClicked()) { runTool(t.id); return true; }
    return false;
}

void App::toolMenuItems(const ToolGroup& g) {
    const float S = uiScale_;
    for (int ti : g.tools) {
        const Tool& t = tools_[ti];
        ImVec2 p = ImGui::GetCursorScreenPos();
        float rowH = 24 * S;
        std::string label = "      " + t.label;
        std::string sc = t.available() ? t.shortcut : (t.shortcut.empty() ? "" : t.shortcut + "  ") + "M" + std::to_string(t.milestone);
        if (ImGui::MenuItem(label.c_str(), sc.empty() ? nullptr : sc.c_str(), false, true)) runTool(t.id);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && t.available()) {
            beginKeyEdit(ti);   // Fusion: Change Keyboard Shortcut
            ImGui::CloseCurrentPopup();
        }
        drawIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + 10 * S, p.y + rowH * 0.42f), 18 * S, t.icon, t.available());
        flashItem("tool:" + t.id);
    }
}

void App::drawToolbar() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top = 34.f * S, h = 76.f * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Card));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * S, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##toolbar", nullptr, kPanel);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x, wp.y + h - 1), ImVec2(wp.x + vp->Size.x, wp.y + h - 1), col::PanelBorder);

    // Workspace selector box
    float wsW = 112 * S;
    ImVec2 ws0(wp.x + 8 * S, wp.y + 8 * S), ws1(ws0.x + wsW, wp.y + h - 8 * S);
    ImGui::SetCursorScreenPos(ws0);
    ImGui::InvisibleButton("##workspace", ImVec2(wsW, ws1.y - ws0.y));
    bool wsHov = ImGui::IsItemHovered();
    dl->AddRectFilled(ws0, ws1, wsHov ? col::Selected : col::Chip, 6 * S);
    dl->AddRect(ws0, ws1, wsHov ? col::Accent : col::PanelBorder, 6 * S, 0, 1.f);
    ImGui::PushFont(fonts_.bold, 0.f);
    ImVec2 dsz = ImGui::CalcTextSize("DESIGN");
    dl->AddText(ImVec2(ws0.x + 10 * S, (ws0.y + ws1.y - dsz.y) * 0.5f), col::Text, "DESIGN");
    ImGui::PopFont();
    drawIcon(dl, ImVec2(ws1.x - 14 * S, (ws0.y + ws1.y) * 0.5f), 12 * S, Icon::Chevron, true);
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##wsmenu");
    // Workspace list: a card under the button, one roomy row per workspace (Fusion style).
    ImGui::SetNextWindowPos(ImVec2(ws0.x, ws1.y + 4 * S));
    ImGui::SetNextWindowSize(ImVec2(std::max(360 * S, ImGui::CalcTextSize("OrcaSlicer inside Kestral, your model already loaded.").x + 110 * S), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 6 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8 * S);
    if (ImGui::BeginPopup("##wsmenu")) {
        struct Ws { const char* title; const char* desc; Icon icon; int state; };   // state 0 here, 1 open, 2 later
        const Ws list[] = {
            {"DESIGN", "Sketch, model, assemble. Everything you make.", Icon::Extrude, 0},
            {"SLICE", "OrcaSlicer inside Kestral, your model already loaded.", Icon::Slicer, 1},
            {"MOVE FROM FUSION", "Bring every Fusion design over in one go.", Icon::ImportFeature, 1},
            {"RENDER", "Photo quality pictures of your designs.", Icon::Appearance, 2},
            {"DRAWING", "2D drawings with dimensions, for the shop.", Icon::Dimension, 2},
        };
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 5; ++i) {
            const Ws& w = list[i];
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float rw = ImGui::GetContentRegionAvail().x, rh = 52 * S;
            bool clicked = ImGui::InvisibleButton("##ws", ImVec2(rw, rh));
            bool hov = ImGui::IsItemHovered() && w.state != 2;
            if (w.state == 0) pdl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), col::Selected, 6 * S);
            else if (hov) pdl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), col::Hover, 6 * S);
            drawIcon(pdl, ImVec2(p.x + 26 * S, p.y + rh * 0.5f), 30 * S, w.icon, w.state != 2);
            ImGui::PushFont(fonts_.bold, 0.f);
            pdl->AddText(ImVec2(p.x + 52 * S, p.y + 8 * S), w.state == 2 ? col::TextDim : (i == 1 ? col::Print : col::Text), w.title);
            ImGui::PopFont();
            pdl->AddText(ImVec2(p.x + 52 * S, p.y + 28 * S), col::TextDim, w.desc);
            if (w.state == 2) {
                const char* later = "coming later";
                ImVec2 ls = ImGui::CalcTextSize(later);
                pdl->AddText(ImVec2(p.x + rw - ls.x - 10 * S, p.y + 8 * S), col::TextDim, later);
            }
            if (w.state == 0) drawIcon(pdl, ImVec2(p.x + rw - 16 * S, p.y + rh * 0.5f), 12 * S, Icon::FinishSketch, true);
            if (clicked && w.state != 2) {
                if (i == 0 && sliceWs_) leaveSliceWorkspace();
                if (i == 1) enterSliceWorkspace();
                if (i == 2) fusionMoveOpen_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    // Tabs
    float x0 = ws1.x + 16 * S;
    const char* tabs[] = {"SOLID", "UTILITIES"};
    const char* skTabs[] = {"SKETCH"};
    float tx = x0;
    const bool inSk = sk_.active;
    for (int i = 0; i < (inSk ? 1 : 2); ++i) {
        const char* tabName = inSk ? skTabs[i] : tabs[i];
        ImVec2 tsz = ImGui::CalcTextSize(tabName);
        ImGui::SetCursorScreenPos(ImVec2(tx, wp.y + 2 * S));
        ImGui::InvisibleButton(tabName, ImVec2(tsz.x + 16 * S, 20 * S));
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && !inSk) activeTab_ = i;
        bool tabOn = inSk || activeTab_ == i;
        ImU32 tc = tabOn ? col::Text : (hov ? col::Text : col::TextDim);
        if (tabOn) ImGui::PushFont(fonts_.bold, 0.f);
        dl->AddText(ImVec2(tx + 8 * S, wp.y + 4 * S), tc, tabName);
        if (tabOn) ImGui::PopFont();
        if (tabOn) dl->AddRectFilled(ImVec2(tx + 6 * S, wp.y + 20 * S), ImVec2(tx + tsz.x + 10 * S, wp.y + 22.5f * S), col::Accent, 1.5f * S);
        tx += tsz.x + 20 * S;
    }

    // Groups
    const auto& groups = inSk ? sketchGroups_ : (activeTab_ == 0 ? solidGroups_ : utilGroups_);
    float gx = x0;
    float iconSize = 34 * S;
    float gy = wp.y + 24 * S;
    // Narrow windows: fewer big icons per group, then labels only (everything stays in the menus).
    int maxPrimary = 99;
    {
        float avail = wp.x + vp->Size.x - x0 - 12 * S;
        auto need = [&](int cap) {
            float w = 0;
            for (auto& g : groups) {
                int np = std::min({g.primary, (int)g.tools.size(), cap});
                float lw = ImGui::CalcTextSize(g.name.c_str()).x;
                w += std::max(np * (iconSize + 2 * S), lw + 18 * S) + 14 * S;
            }
            return w;
        };
        if (need(99) > avail) maxPrimary = need(1) <= avail ? 1 : 0;
    }
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const ToolGroup& g = groups[gi];
        int np = std::min({g.primary, (int)g.tools.size(), maxPrimary});
        std::string glabel = g.name;
        ImVec2 lsz = ImGui::CalcTextSize(glabel.c_str());
        float gw = std::max(np * (iconSize + 2 * S), lsz.x + 18 * S) + 10 * S;
        float ix = gx + (gw - np * (iconSize + 2 * S)) * 0.5f;
        for (int k = 0; k < np; ++k) {
            ImGui::SetCursorScreenPos(ImVec2(ix + k * (iconSize + 2 * S), gy));
            toolButton(g.tools[k], iconSize, true);
        }
        // Group label with dropdown
        ImVec2 l0(gx + (gw - lsz.x - 12 * S) * 0.5f, gy + iconSize + 1 * S);
        ImGui::SetCursorScreenPos(ImVec2(gx + 2 * S, l0.y - 1 * S));
        std::string gid = "##grp" + std::to_string(inSk ? 9 : activeTab_) + "_" + std::to_string(gi);
        ImGui::InvisibleButton(gid.c_str(), ImVec2(gw - 4 * S, lsz.y + 3 * S));
        bool lh = ImGui::IsItemHovered();
        if (lh) dl->AddRectFilled(ImVec2(gx + 2 * S, l0.y - 1 * S), ImVec2(gx + gw - 2 * S, l0.y + lsz.y + 2 * S), col::Hover, 3 * S);
        ImU32 lc = g.name == "3D PRINT" ? col::Print : (lh ? col::Text : col::TextDim);
        dl->AddText(l0, lc, glabel.c_str());
        drawIcon(dl, ImVec2(l0.x + lsz.x + 7 * S, l0.y + lsz.y * 0.55f), 9 * S, Icon::Chevron, true);
        std::string pid = "##grppop" + gid;
        if (ImGui::IsItemClicked()) ImGui::OpenPopup(pid.c_str());
        if (flashOpenGroup_ && flashKey_.rfind("tool:", 0) == 0) {
            // A search result inside this group's menu: open the menu so it can blink there.
            for (size_t k = 0; k < g.tools.size(); ++k)
                if ("tool:" + tools_[(size_t)g.tools[k]].id == flashKey_) {
                    flashOpenGroup_ = false;
                    if ((int)k >= np) ImGui::OpenPopup(pid.c_str());
                    flashRect(ImGui::GetForegroundDrawList(), ImVec2(gx + 2 * S, l0.y - 1 * S), ImVec2(gx + gw - 2 * S, l0.y + lsz.y + 2 * S), 1.f, 3 * S);
                }
        }
        ImGui::SetNextWindowPos(ImVec2(gx, gy + iconSize + lsz.y + 6 * S));
        if (ImGui::BeginPopup(pid.c_str())) {
            toolMenuItems(g);
            ImGui::EndPopup();
        }
        gx += gw;
        if (gi + 1 < groups.size())
            dl->AddLine(ImVec2(gx, gy + 4 * S), ImVec2(gx, wp.y + h - 6 * S), col::Divider);
        gx += 4 * S;
    }
    if (ImGui::BeginPopup("##tbctx")) {
        if (tbCtxTool_ >= 0 && tbCtxTool_ < (int)tools_.size()) {
            const Tool& t = tools_[tbCtxTool_];
            ImGui::TextDisabled("%s", t.label.c_str());
            ImGui::Separator();
            std::string cur = "Change Keyboard Shortcut...";
            if (ImGui::MenuItem(cur.c_str(), t.shortcut.empty() ? nullptr : t.shortcut.c_str(), false, t.available())) beginKeyEdit(tbCtxTool_);
            if (ImGui::MenuItem("All Keyboard Shortcuts...")) { keysOpen_ = true; keyFilter_[0] = 0; }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// ===================================================================== browser

void App::buildBrowserRows() {
    // Reveal a clicked body: open every folder above it (the row key below then rebuilds).
    if (revealBody_) {
        auto bit = doc_.bodies.find(revealBody_);
        if (bit != doc_.bodies.end()) {
            uint32_t c = bit->second.componentId;
            collapsed_.erase(c | 0x80000000u);   // its Bodies folder
            for (int guard = 0; c && guard < 64; ++guard) {
                collapsed_.erase(c);
                auto cit = doc_.components.find(c);
                if (cit == doc_.components.end() || c == doc_.rootId) break;
                c = cit->second.parentId;
            }
            collapsed_.erase(doc_.rootId);
        }
    }
    std::string key = std::to_string(docGen_) + ":" + std::to_string(doc_.revision) + "|" + browserSearch_ + "|" + std::to_string(browserFilter_) + "|" +
                      std::to_string(collapsed_.size()) + "|" + std::to_string(browserFilter_ == 4 ? selection_.size() : 0) +
                      "|" + std::to_string(isolated_.size());
    for (uint32_t c : collapsed_) key += "," + std::to_string(c);
    if (key == rowsKey_) return;
    rowsKey_ = key;
    rows_.clear();
    bool flat = browserSearch_[0] != 0 || browserFilter_ != 0;
    if (!flat) {
        int alive = 0;
        for (auto& [id, b] : doc_.bodies) if (doc_.isBodyAlive(b)) ++alive;
        rows_.push_back({0, doc_.rootId, 0, doc_.name, alive});
        if (collapsed_.count(doc_.rootId)) return;
        rows_.push_back({4, 0, 1, "Origin", 0});
        if (!doc_.sketches.empty()) {
            rows_.push_back({5, 0, 1, "Sketches", (int)doc_.sketches.size()});
            if (!collapsed_.count(0x7ffffffeu))
                for (auto& [sid, sk] : doc_.sketches) rows_.push_back({6, sid, 2, sk.name, 0});
        }
        std::vector<uint32_t> planes;
        for (auto& [fid, pl] : doc_.constructPlanes) if (doc_.featureIndex(fid) >= 0) planes.push_back(fid);
        if (!planes.empty()) {
            rows_.push_back({7, 0, 1, "Construction", (int)planes.size()});
            if (!collapsed_.count(0x7ffffffdu))
                for (uint32_t fid : planes) rows_.push_back({8, fid, 2, doc_.timeline[doc_.featureIndex(fid)].name, 0});
        }
        std::function<void(uint32_t, int)> addComp = [&](uint32_t cid, int depth) {
            auto bodies = doc_.bodiesIn(cid);
            bodies.erase(std::remove_if(bodies.begin(), bodies.end(), [&](uint32_t b) { return !doc_.isBodyAlive(doc_.bodies[b]); }), bodies.end());
            if (!bodies.empty()) {
                uint32_t fk = cid | 0x80000000u;
                rows_.push_back({1, cid, depth, "Bodies", (int)bodies.size()});
                if (!collapsed_.count(fk))
                    for (uint32_t b : bodies) rows_.push_back({3, b, depth + 1, doc_.bodies[b].name, 0});
            }
            for (uint32_t c : doc_.childComponents(cid)) {
                rows_.push_back({2, c, depth, doc_.components[c].name, doc_.countBodiesDeep(c)});
                if (!collapsed_.count(c)) addComp(c, depth + 1);
            }
        };
        addComp(doc_.rootId, 1);
        return;
    }
    // Flat filtered list (instant on thousands of bodies).
    if (browserFilter_ == 0 || browserFilter_ == 2)
        for (auto& [id, c] : doc_.components)
            if (id != doc_.rootId && containsI(c.name, browserSearch_)) rows_.push_back({2, id, 0, c.name, doc_.countBodiesDeep(id)});
    if (browserFilter_ != 2)
        for (auto& [id, b] : doc_.bodies) {
            if (!doc_.isBodyAlive(b)) continue;
            if (!containsI(b.name, browserSearch_)) continue;
            if (browserFilter_ == 3 && bodyShown(b)) continue;
            if (browserFilter_ == 4 && !selection_.count(id)) continue;
            rows_.push_back({3, id, 0, b.name, 0});
        }
}

void App::drawBrowser() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top = (34.f + 76.f + 8.f) * S;
    float x = vp->Pos.x + 8 * S;
    float maxH = vp->Size.y - top - (38.f + 60.f) * S;
    if (!browserOpen_) {
        ImGui::SetNextWindowPos(ImVec2(x, vp->Pos.y + top));
        ImGui::SetNextWindowSize(ImVec2(110 * S, 30 * S));
        ImGui::Begin("##browser_closed", nullptr, kPanel & ~ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushFont(fonts_.bold, 0.f);
        if (ImGui::Selectable("BROWSER  >>")) browserOpen_ = true;
        ImGui::PopFont();
        ImGui::End();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(x, vp->Pos.y + top));
    ImGui::SetNextWindowSizeConstraints(ImVec2(200 * S, 120 * S), ImVec2(600 * S, maxH));
    ImGui::SetNextWindowSize(ImVec2(browserWidth_, maxH), ImGuiCond_Once);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 6 * S));
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##browser", nullptr, fl);
    browserWidth_ = ImGui::GetWindowWidth();

    // Header
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("BROWSER");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetWindowWidth() - 30 * S);
    if (ImGui::SmallButton("<<")) browserOpen_ = false;
    ImGui::SetItemTooltip("Collapse browser");

    // Search
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##bsearch", "Search bodies and components", browserSearch_, sizeof browserSearch_);

    // Filter chips
    const char* chips[] = {"All", "Bodies", "Comps", "Hidden", "Picked"};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, 3 * S);
        bool on = browserFilter_ == i;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(1.f, 0.42f, 0.21f, 1.f) : th(ImVec4(0.93f, 0.94f, 0.95f, 1.f)));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : th(ImVec4(0.3f, 0.33f, 0.37f, 1.f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5 * S, 1 * S));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9 * S);
        if (ImGui::Button(chips[i])) browserFilter_ = on ? 0 : i;
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
    ImGui::Separator();

    buildBrowserRows();

    ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    // Rows sit exactly rowH apart (no item spacing), so the list clipper and the scroll to a picked
    // body land on the right row.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rowH = 26 * S;
    // Bodies related to what is picked in the viewport (a picked face or edge marks its body),
    // and the folders that hold them: marked with an underline like Fusion.
    std::set<uint32_t> related(selection_.begin(), selection_.end());
    for (auto& f : selFaces_) related.insert(f.first);
    for (auto& e : selEdges_) related.insert(e.first);
    std::set<uint32_t> relatedComps;
    for (uint32_t b : related) {
        auto bit = doc_.bodies.find(b);
        if (bit == doc_.bodies.end()) continue;
        uint32_t c = bit->second.componentId;
        relatedComps.insert(c | 0x80000000u);
        for (int guard = 0; c && guard < 64; ++guard) {
            relatedComps.insert(c);
            auto cit = doc_.components.find(c);
            if (cit == doc_.components.end() || c == doc_.rootId) break;
            c = cit->second.parentId;
        }
    }
    if (revealBody_) {
        // Folders above it may have just opened: the list is only as tall as last frame's, so the
        // scroll is clamped. Keep trying for a few frames until the row can be reached.
        bool done = true;
        for (int ri = 0; ri < (int)rows_.size(); ++ri)
            if (rows_[ri].kind == 3 && rows_[ri].id == revealBody_) {
                float y = ri * rowH, top = ImGui::GetScrollY(), h = ImGui::GetWindowHeight();
                if (y < top || y + rowH > top + h) {
                    float want = std::max(0.f, y - h * 0.4f);
                    ImGui::SetScrollY(want);
                    if (want > ImGui::GetScrollMaxY() + 1.f) done = false;
                }
                break;
            }
        if (done || ++revealTries_ > 6) { revealBody_ = 0; revealTries_ = 0; }
    }
    std::set<uint32_t> newHover;
    bool anyRowHovered = false;
    // Popup ids taken here, outside the per row PushID, so BeginPopup below finds them.
    const ImGuiID plCtx = ImGui::GetID("##plctx"), skCtx = ImGui::GetID("##skctx"), rowCtx = ImGui::GetID("##rowctx"),
                  folderCtx = ImGui::GetID("##folderctx");
    ImGuiListClipper clip;
    clip.Begin((int)rows_.size(), rowH);
    while (clip.Step()) {
        for (int ri = clip.DisplayStart; ri < clip.DisplayEnd; ++ri) {
            const Row& r = rows_[ri];
            ImGui::PushID(ri);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("##row", ImVec2(w, rowH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            bool hov = ImGui::IsItemHovered();
            ImVec2 m = ImGui::GetIO().MousePos;

            // Row state
            bool selected = false, visible = true;
            std::vector<uint32_t> rowBodies;
            if (r.kind == 3) {
                rowBodies.push_back(r.id);
                selected = selection_.count(r.id) > 0;
                auto it = doc_.bodies.find(r.id);
                visible = it != doc_.bodies.end() && it->second.visible;
            } else if (r.kind == 2 || r.kind == 0) {
                doc_.collectBodies(r.id, rowBodies);
                auto it = doc_.components.find(r.id);
                visible = it != doc_.components.end() && it->second.visible;
                selected = !rowBodies.empty() && std::all_of(rowBodies.begin(), rowBodies.end(), [&](uint32_t b) { return selection_.count(b) > 0; });
            } else if (r.kind == 1) {
                rowBodies = doc_.bodiesIn(r.id);
                visible = std::any_of(rowBodies.begin(), rowBodies.end(), [&](uint32_t b) { return doc_.bodies[b].visible; });
            } else if (r.kind == 4) {
                visible = settings_.showOrigin;
            } else if (r.kind == 5) {
                visible = std::any_of(doc_.sketches.begin(), doc_.sketches.end(), [](auto& kv) { return kv.second.visible; });
            } else if (r.kind == 6) {
                auto it = doc_.sketches.find(r.id);
                visible = it != doc_.sketches.end() && it->second.visible;
                selected = selSketches_.count(r.id) > 0;
            } else if (r.kind == 7) {
                visible = doc_.hiddenPlanes.size() < doc_.constructPlanes.size();
            } else if (r.kind == 8) {
                visible = !doc_.hiddenPlanes.count(r.id);
                selected = selPlanes_.count(r.id) > 0;
            }
            if (hov && r.kind <= 3) { anyRowHovered = true; for (uint32_t b : rowBodies) newHover.insert(b); }

            if (selected) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), col::Selected, 3 * S);
            else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), col::Hover, 3 * S);
            {
                bool rel = (r.kind == 3 && related.count(r.id)) ||
                           ((r.kind == 2 || r.kind == 0) && relatedComps.count(r.id)) ||
                           (r.kind == 1 && relatedComps.count(r.id | 0x80000000u));
                if (rel) dl->AddLine(ImVec2(p.x + (r.kind == 3 ? 40 : 20) * S + r.depth * 14 * S, p.y + rowH - 2 * S),
                                     ImVec2(p.x + w - 6 * S, p.y + rowH - 2 * S), IM_COL32(255, 107, 53, 255), 2.f * S);
            }
            bool flat = browserSearch_[0] != 0 || browserFilter_ != 0;
            float ind = (flat ? 0 : r.depth) * 14 * S;
            float ax = p.x + ind, cy = p.y + rowH * 0.5f;
            bool expandable = !flat && (r.kind == 0 || r.kind == 1 || r.kind == 2 || r.kind == 5 || r.kind == 7);
            uint32_t ckey = r.kind == 1 ? (r.id | 0x80000000u) : r.kind == 5 ? 0x7ffffffeu : r.kind == 7 ? 0x7ffffffdu : r.id;
            if (expandable) {
                bool open = !collapsed_.count(ckey);
                ImVec2 c(ax + 6 * S, cy);
                if (open) dl->AddTriangleFilled(ImVec2(c.x - 4 * S, c.y - 2 * S), ImVec2(c.x + 4 * S, c.y - 2 * S), ImVec2(c.x, c.y + 3 * S), col::TextDim);
                else dl->AddTriangleFilled(ImVec2(c.x - 2 * S, c.y - 4 * S), ImVec2(c.x + 3 * S, c.y), ImVec2(c.x - 2 * S, c.y + 4 * S), col::TextDim);
            }
            float ex = ax + 14 * S;
            drawIcon(dl, ImVec2(ex + 8 * S, cy), 15 * S, visible ? Icon::Eye : Icon::EyeOff, true);
            float ix = ex + 20 * S;
            Icon ic = r.kind == 0 ? Icon::Component : r.kind == 1 ? Icon::Folder : r.kind == 2 ? Icon::Component : r.kind == 3 ? Icon::Body :
                      r.kind == 5 || r.kind == 7 ? Icon::Folder : r.kind == 6 ? Icon::Sketch : r.kind == 8 ? Icon::OffsetPlane : Icon::Origin;
            if (r.kind == 3) {
                auto it = doc_.bodies.find(r.id);
                if (it != doc_.bodies.end()) {
                    glm::vec3 c = it->second.color;
                    dl->AddRectFilled(ImVec2(ix + 1 * S, cy - 6 * S), ImVec2(ix + 15 * S, cy + 6 * S),
                                      IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 255), 2 * S);
                    dl->AddRect(ImVec2(ix + 1 * S, cy - 6 * S), ImVec2(ix + 15 * S, cy + 6 * S), col::Status, 2 * S);
                }
            } else {
                drawIcon(dl, ImVec2(ix + 8 * S, cy), 16 * S, ic, true);
            }
            float lx = ix + 22 * S;
            ImU32 tc = visible ? col::Text : col::TextDim;
            if (renameId_ != 0 && renameId_ == r.id && (r.kind == 6 ? renameIsSketch_ : (!renameIsSketch_ && renameIsComp_ == (r.kind == 2 || r.kind == 0)))) {
                ImGui::SetCursorScreenPos(ImVec2(lx, p.y + 1 * S));
                ImGui::SetNextItemWidth(w - (lx - p.x) - 4 * S);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    if (renameBuf_[0]) {
                        pushUndo("Rename");
                        if (renameIsSketch_) {
                            auto sit = doc_.sketches.find(r.id);
                            if (sit != doc_.sketches.end()) {
                                sit->second.name = renameBuf_;
                                for (auto& f : doc_.timeline) if (f.sketchId == r.id) f.name = renameBuf_;
                            }
                        } else if (renameIsComp_) {
                            if (r.id == doc_.rootId) doc_.name = renameBuf_;
                            doc_.components[r.id].name = renameBuf_;
                            rememberComponentName(r.id);
                        } else {
                            doc_.bodies[r.id].name = renameBuf_;
                            syncBodyOverride(r.id);
                        }
                        doc_.touch();
                    }
                    renameId_ = 0;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered())) renameId_ = 0;
            } else {
                std::string label = r.label;
                if (r.kind == 2 && !flat) label += ":1";
                if ((r.kind == 0 || r.kind == 2) && r.id == doc_.activeComponent) label += "  active";
                if (r.kind == 2) {
                    auto git = doc_.components.find(r.id);
                    if (git != doc_.components.end() && git->second.grounded) label += "  grounded";
                }
                if (flat && r.kind == 3) {
                    auto it = doc_.bodies.find(r.id);
                    if (it != doc_.bodies.end() && it->second.componentId != doc_.rootId) {
                        auto cit = doc_.components.find(it->second.componentId);
                        if (cit != doc_.components.end()) label += "   (" + cit->second.name + ")";
                    }
                }
                dl->PushClipRect(ImVec2(lx, p.y), ImVec2(p.x + w - 40 * S, p.y + rowH), true);
                dl->AddText(ImVec2(lx, cy - ImGui::GetTextLineHeight() * 0.5f), tc, label.c_str());
                dl->PopClipRect();
            }
            if (r.count > 0 && r.kind != 3) {
                std::string cnt = std::to_string(r.count);
                ImVec2 cs = ImGui::CalcTextSize(cnt.c_str());
                ImVec2 b0(p.x + w - cs.x - 14 * S, cy - cs.y * 0.5f - 1 * S), b1(p.x + w - 4 * S, cy + cs.y * 0.5f + 1 * S);
                dl->AddRectFilled(b0, b1, col::Chip, 8 * S);
                dl->AddText(ImVec2(b0.x + 5 * S, b0.y + 1 * S), col::TextDim, cnt.c_str());
            }

            // Interaction
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                bool ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
                if (expandable && m.x < ex) {
                    if (collapsed_.count(ckey)) collapsed_.erase(ckey);
                    else collapsed_.insert(ckey);
                } else if (m.x >= ex && m.x < ix) {
                    if (ImGui::GetIO().KeyAlt) {
                        isolate(rowBodies);     // Alt + eye = solo
                    } else if (r.kind == 3 || r.kind == 1) {
                        toggleVisibility(rowBodies);
                    } else if (r.kind == 2 || r.kind == 0) {
                        setComponentVisible(r.id, !visible);
                    } else if (r.kind == 4) {
                        settings_.showOrigin = !settings_.showOrigin;
                    } else if (r.kind == 5) {
                        pushUndo(visible ? "Hide sketches" : "Show sketches"); for (auto& kv : doc_.sketches) kv.second.visible = !visible;
                        doc_.touch();
                    } else if (r.kind == 6) {
                        auto sit = doc_.sketches.find(r.id);
                        if (sit != doc_.sketches.end()) { pushUndo(sit->second.visible ? "Hide" : "Show"); sit->second.visible = !sit->second.visible; doc_.touch(); }
                    } else if (r.kind == 7) {
                        pushUndo(visible ? "Hide planes" : "Show planes");
                        if (visible) for (auto& kv : doc_.constructPlanes) doc_.hiddenPlanes.insert(kv.first);
                        else doc_.hiddenPlanes.clear();
                    } else if (r.kind == 8) {
                        pushUndo("Toggle plane"); if (!doc_.hiddenPlanes.erase(r.id)) doc_.hiddenPlanes.insert(r.id);
                    }
                } else if (r.kind <= 3 || r.kind == 6 || r.kind == 8) {
                    // Plain click: just this row. Ctrl: add or remove this row. Shift: every body,
                    // sketch and plane row from the last click to this one.
                    bool ctrlKey = ImGui::GetIO().KeyCtrl, shiftKey = ImGui::GetIO().KeyShift;
                    (void)ctrl;
                    auto pickRow = [&](const Row& rr, bool on) {
                        if (rr.kind == 6) { if (on) selSketches_.insert(rr.id); else selSketches_.erase(rr.id); return; }
                        if (rr.kind == 8) { if (on) selPlanes_.insert(rr.id); else selPlanes_.erase(rr.id); return; }
                        if (rr.kind > 3) return;
                        std::vector<uint32_t> bs;
                        if (rr.kind == 3) bs.push_back(rr.id);
                        else if (rr.kind == 1) bs = doc_.bodiesIn(rr.id);
                        else doc_.collectBodies(rr.id, bs);
                        for (uint32_t b : bs) {
                            if (on) { if (!selection_.count(b)) selOrder_.push_back(b); selection_.insert(b); }
                            else selection_.erase(b);
                        }
                    };
                    if (shiftKey && browserAnchor_ >= 0 && browserAnchor_ < (int)rows_.size()) {
                        int a = std::min(browserAnchor_, ri), b = std::max(browserAnchor_, ri);
                        if (!ctrlKey) { selection_.clear(); selOrder_.clear(); selSketches_.clear(); selPlanes_.clear(); }
                        for (int k = a; k <= b; ++k)
                            if (rows_[k].kind == 3 || rows_[k].kind == 6 || rows_[k].kind == 8) pickRow(rows_[k], true);
                    } else if (ctrlKey) {
                        pickRow(r, !selected);
                        browserAnchor_ = ri;
                    } else {
                        selSketches_.clear();
                        selPlanes_.clear();
                        if (r.kind <= 3) selectBodies(rowBodies, false);
                        else { clearSelection(); pickRow(r, true); }
                        browserAnchor_ = ri;
                    }
                    markSceneDirty();
                }
            }
            if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m.x >= ix && r.kind == 6) {
                editSketch(r.id);
            } else if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m.x >= ix && r.kind == 8) {
                editFeature(r.id);
            } else if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m.x >= ix && (r.kind == 3 || r.kind == 2 || r.kind == 0)) {
                renameIsSketch_ = false;
                renameId_ = r.id;
                renameIsComp_ = r.kind != 3;
                std::string cur = r.kind == 0 ? doc_.name : r.label;
                std::snprintf(renameBuf_, sizeof renameBuf_, "%s", cur.c_str());
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && r.kind == 8) {
                ctxRow_ = r;
                if (!selected) { clearSelection(); selSketches_.clear(); selPlanes_ = {r.id}; }
                ImGui::OpenPopup(plCtx);
            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && r.kind == 6) {
                ctxRow_ = r;
                if (!selected) { clearSelection(); selPlanes_.clear(); selSketches_ = {r.id}; }
                ImGui::OpenPopup(skCtx);
            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && r.kind <= 3) {
                ctxRow_ = r;
                if (!selected) { selSketches_.clear(); selPlanes_.clear(); selectBodies(rowBodies, false); }
                ImGui::OpenPopup(rowCtx);
            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && (r.kind == 4 || r.kind == 5 || r.kind == 7)) {
                ctxRow_ = r;
                ImGui::OpenPopup(folderCtx);
            }
            ImGui::PopID();
        }
    }
    clip.End();

    if (ImGui::BeginPopup("##plctx")) {
        int fi = doc_.featureIndex(ctxRow_.id);
        if (fi >= 0) {
            if (fi < doc_.markerPos && ImGui::MenuItem("Edit Feature")) editFeature(ctxRow_.id);
            bool hid = doc_.hiddenPlanes.count(ctxRow_.id) > 0;
            if (ImGui::MenuItem(hid ? "Show" : "Hide")) { pushUndo(hid ? "Show" : "Hide"); if (hid) doc_.hiddenPlanes.erase(ctxRow_.id); else doc_.hiddenPlanes.insert(ctxRow_.id); }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) {
                if (!(selPlanes_.count(ctxRow_.id) && selSketches_.size() + selPlanes_.size() + selection_.size() > 1)) {
                    selPlanes_ = {ctxRow_.id};
                    selSketches_.clear();
                    selection_.clear();
                }
                deleteSelection();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##folderctx")) {
        if (ctxRow_.kind == 4) {
            if (ImGui::MenuItem(settings_.showOrigin ? "Hide Origin" : "Show Origin")) settings_.showOrigin = !settings_.showOrigin;
        } else if (ctxRow_.kind == 5) {
            if (ImGui::MenuItem("Show All Sketches")) { pushUndo("Show sketches"); for (auto& kv : doc_.sketches) kv.second.visible = true; doc_.touch(); }
            if (ImGui::MenuItem("Hide All Sketches")) { pushUndo("Hide sketches"); for (auto& kv : doc_.sketches) kv.second.visible = false; doc_.touch(); }
            if (ImGui::MenuItem("Create Sketch")) beginCreateSketch();
        } else if (ctxRow_.kind == 7) {
            if (ImGui::MenuItem("Show All Planes")) { pushUndo("Show planes"); doc_.hiddenPlanes.clear(); }
            if (ImGui::MenuItem("Hide All Planes")) { pushUndo("Hide planes"); for (auto& kv : doc_.constructPlanes) doc_.hiddenPlanes.insert(kv.first); }
            if (ImGui::MenuItem("New Offset Plane")) runOffsetPlane();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##skctx")) {
        auto sit = doc_.sketches.find(ctxRow_.id);
        if (sit != doc_.sketches.end()) {
            if (ImGui::MenuItem("Edit Sketch")) editSketch(ctxRow_.id);
            if (ImGui::MenuItem(sit->second.visible ? "Hide" : "Show")) { pushUndo("Toggle sketch"); sit->second.visible = !sit->second.visible; doc_.touch(); }
            if (ImGui::MenuItem("Rename")) {
                renameIsSketch_ = true;
                renameId_ = ctxRow_.id;
                std::snprintf(renameBuf_, sizeof renameBuf_, "%s", ctxRow_.label.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) {
                if (selSketches_.count(ctxRow_.id) && selSketches_.size() + selPlanes_.size() + selection_.size() > 1) {
                    deleteSelection();   // everything picked goes, in one undo step
                } else {
                    selSketches_ = {ctxRow_.id};
                    selPlanes_.clear();
                    selection_.clear();
                    deleteSelection();
                }
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##rowctx")) {
        std::vector<uint32_t> bodies;
        if (ctxRow_.kind == 3) bodies.push_back(ctxRow_.id);
        else if (ctxRow_.kind == 1) bodies = doc_.bodiesIn(ctxRow_.id);
        else doc_.collectBodies(ctxRow_.id, bodies);
        if (ctxRow_.kind != 1 && ImGui::MenuItem("Rename")) {
            renameIsSketch_ = false;
            renameId_ = ctxRow_.id;
            renameIsComp_ = ctxRow_.kind != 3;
            std::string cur = ctxRow_.kind == 0 ? doc_.name : ctxRow_.label;
            std::snprintf(renameBuf_, sizeof renameBuf_, "%s", cur.c_str());
        }
        if (ImGui::MenuItem("Isolate")) isolate(bodies);
        if (!isolated_.empty() && ImGui::MenuItem("Exit Isolate")) exitIsolate();
        if (ImGui::MenuItem("Show / Hide", "V")) {
            if (ctxRow_.kind == 2) setComponentVisible(ctxRow_.id, !doc_.components[ctxRow_.id].visible);
            else toggleVisibility(bodies);
        }
        if (ImGui::MenuItem("Show All")) showAll();
        if (ImGui::MenuItem("Select Bodies")) selectBodies(bodies, false);
        if (ImGui::MenuItem("Find in Window")) { selectBodies(bodies, false); fitView(true); }
        if (ctxRow_.kind == 0 || ctxRow_.kind == 2) {
            if (ImGui::MenuItem("Activate")) activateComponent(ctxRow_.id);
        }
        if (ctxRow_.kind == 2) {
            bool grounded = doc_.components.count(ctxRow_.id) && doc_.components[ctxRow_.id].grounded;
            if (ImGui::MenuItem(grounded ? "Unground" : "Ground")) groundComponent(ctxRow_.id, !grounded);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Move/Copy", "M")) { selectBodies(bodies, false); runMove(); }
        if (ImGui::MenuItem("Copy", "Ctrl+C")) { selectBodies(bodies, false); copySelection(); }
        if (!clipboard_.empty() && ImGui::MenuItem("Paste", "Ctrl+V")) pasteClipboard();
        ImGui::Separator();
        if (ImGui::MenuItem("Appearance...", "A")) { selectBodies(bodies, false); appearanceOpen_ = true; appearanceUndoPushed_ = false; matApplyFaces_ = 0; }
        if (ImGui::BeginMenu("Quick Appearance")) {
            static const char* quick[] = {"Plastic - Glossy White", "Plastic - Matte Black", "Plastic - Red", "Plastic - Blue",
                                          "Aluminum - Brushed", "Steel - Stainless", "Chrome", "Glass - Clear", "Wood - Oak", "Carbon Fiber"};
            for (const char* qn : quick)
                if (const LibMaterial* lm = findLibMaterial(qn); lm && ImGui::MenuItem(qn)) {
                    pushUndo("Appearance");
                    MatProps m = lm->mat;
                    m.fileColors = false;
                    applyLook(lm->color, m, false, bodies, {});
                }
            ImGui::EndMenu();
        }
        if (ctxRow_.kind == 3) {
            auto bit = doc_.bodies.find(ctxRow_.id);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit != doc_.parts.end() && !pit->second.shape && pit->second.isMesh &&
                ImGui::MenuItem("Convert to Solid Body"))
                convertToSolid(ctxRow_.id);
        }
        if (ImGui::MenuItem("Send to Slicer")) { selectBodies(bodies, false); sendToSlicer(); }
        if (ImGui::MenuItem("Export STL...")) { selectBodies(bodies, false); exportStlDialog(); }
        ImGui::Separator();
        if (ctxRow_.kind == 2) {
            if (ImGui::MenuItem("Delete Component")) {
                pushUndo("Delete Component");
                doc_.removeComponent(ctxRow_.id);
                selection_.clear();
            }
        } else if (ctxRow_.kind == 3 || ctxRow_.kind == 1) {
            if (ImGui::MenuItem("Delete", "Del")) {
                bool inSel = std::all_of(bodies.begin(), bodies.end(), [&](uint32_t b) { return selection_.count(b) > 0; });
                if (!inSel) { selSketches_.clear(); selPlanes_.clear(); selectBodies(bodies, false); }
                deleteSelection();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();

    if (!anyRowHovered) newHover.clear();
    if (newHover != browserHover_) {
        browserHover_ = std::move(newHover);
        markSceneDirty();
    }
}

// ===================================================================== timeline

namespace {

ui::Icon featureIcon(FeatureType t) {
    switch (t) {
    case FeatureType::Sketch: return ui::Icon::Sketch;
    case FeatureType::Extrude: return ui::Icon::Extrude;
    case FeatureType::Revolve: return ui::Icon::Revolve;
    case FeatureType::Fillet: return ui::Icon::Fillet;
    case FeatureType::Chamfer: return ui::Icon::Chamfer;
    case FeatureType::Shell: return ui::Icon::Shell;
    case FeatureType::PressPull: return ui::Icon::PressPull;
    case FeatureType::DeleteFace: return ui::Icon::OffsetFace;
    case FeatureType::Pattern: return ui::Icon::RectPattern;
    case FeatureType::Mirror: return ui::Icon::Mirror;
    case FeatureType::Combine: return ui::Icon::Combine;
    case FeatureType::Split: return ui::Icon::SplitBody;
    case FeatureType::SplitFace: return ui::Icon::SplitBody;
    case FeatureType::Hole: return ui::Icon::Hole;
    case FeatureType::Sweep: return ui::Icon::Sweep;
    case FeatureType::Loft: return ui::Icon::Loft;
    case FeatureType::Move: return ui::Icon::MoveCopy;
    case FeatureType::ConstructPlane: return ui::Icon::OffsetPlane;
    case FeatureType::Gear: return ui::Icon::Gear;
    case FeatureType::Thread: return ui::Icon::Thread;
    case FeatureType::Wing: return ui::Icon::Loft;
    case FeatureType::Dovetail: return ui::Icon::Combine;
    case FeatureType::Blade: return ui::Icon::CircPattern;
    case FeatureType::Align: return ui::Icon::Align;
    case FeatureType::Draft: return ui::Icon::Draft;
    case FeatureType::Coil: return ui::Icon::Revolve;
    case FeatureType::Pipe: return ui::Icon::Sweep;
    case FeatureType::ReplaceFace: return ui::Icon::OffsetFace;
    case FeatureType::Thicken: return ui::Icon::Shell;
    case FeatureType::ConstructAxis: return ui::Icon::Axis;
    case FeatureType::ConstructPoint: return ui::Icon::Point;
    case FeatureType::Rib: return ui::Icon::Rib;
    case FeatureType::Web: return ui::Icon::Extrude;
    case FeatureType::Emboss: return ui::Icon::PressPull;
    case FeatureType::Boss: return ui::Icon::Cylinder;
    case FeatureType::Snap: return ui::Icon::Joint;
    case FeatureType::Rest: return ui::Icon::Box;
    case FeatureType::Lip: return ui::Icon::Offset;
    case FeatureType::Silhouette: return ui::Icon::SplitBody;
    case FeatureType::Joint: return ui::Icon::Joint;
    case FeatureType::AsBuilt: return ui::Icon::AsBuiltJoint;
    case FeatureType::Rigid: return ui::Icon::RigidGroup;
    default: return ui::Icon::ImportFeature;
    }
}

} // namespace

void App::drawTimeline() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float h = 38.f * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Timeline));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * S, 5 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##timeline", nullptr, kPanel);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, ImVec2(wp.x + vp->Size.x, wp.y), col::PanelBorder);
    float bs = 26 * S;
    int n = (int)doc_.timeline.size();
    if (iconButton("##tl0", Icon::ToStart, bs, n > 0, "Go to start")) { playing_ = false; setMarker(0); }
    ImGui::SameLine(0, 1 * S);
    if (iconButton("##tl1", Icon::StepBack, bs, doc_.markerPos > 0, "Step back")) { playing_ = false; setMarker(doc_.markerPos - 1); }
    ImGui::SameLine(0, 1 * S);
    if (iconButton("##tl2", Icon::Play, bs, n > 0, "Play", playing_)) {
        playing_ = !playing_;
        if (playing_ && doc_.markerPos >= n) setMarker(0);
        playT_ = 0;
    }
    ImGui::SameLine(0, 1 * S);
    if (iconButton("##tl3", Icon::StepFwd, bs, doc_.markerPos < n, "Step forward")) { playing_ = false; setMarker(doc_.markerPos + 1); }
    ImGui::SameLine(0, 1 * S);
    if (iconButton("##tl4", Icon::ToEnd, bs, doc_.markerPos < n, "Go to end")) { playing_ = false; setMarker(n); }
    ImGui::SameLine(0, 12 * S);

    // Feature cells
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    float cell = 26 * S, gap = 3 * S;
    timelineHover_ = -1;
    for (int i = 0; i < n; ++i) {
        const Feature& f = doc_.timeline[i];
        ImVec2 p(c0.x + i * (cell + gap) + (i >= doc_.markerPos ? 10 * S : 0), c0.y);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        ImGui::InvisibleButton("##feat", ImVec2(cell, cell), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hov = ImGui::IsItemHovered();
        bool active = i < doc_.markerPos && !f.suppressed;
        if (hov) { timelineHover_ = i; dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), col::Hover, 3 * S); }
        drawIcon(dl, ImVec2(p.x + cell * 0.5f, p.y + cell * 0.5f), cell * 0.75f, featureIcon(f.type), active);
        if (f.failed) dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), col::Error, 3 * S, 0, 2 * S);
        if (f.suppressed) dl->AddLine(ImVec2(p.x + 3 * S, p.y + cell - 3 * S), ImVec2(p.x + cell - 3 * S, p.y + 3 * S), col::Error, 1.5f * S);
        if (hov) {
            std::string tip = f.name;
            if (f.suppressed) tip += "  (suppressed)";
            if (f.lastMs > 0.0) {
                char ms[32];
                std::snprintf(ms, sizeof ms, f.lastMs >= 100.0 ? "\n%.0f ms" : "\n%.1f ms", f.lastMs);
                tip += ms;
            }
            if (f.failed && !f.error.empty()) tip += "\n" + f.error;
            ImGui::SetTooltip("%s", tip.c_str());
        }
        bool editable = (f.type == FeatureType::Extrude || f.type == FeatureType::Revolve ||
                         f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer ||
                         f.type == FeatureType::Shell || f.type == FeatureType::PressPull ||
                         f.type == FeatureType::Pattern || f.type == FeatureType::Mirror ||
                         f.type == FeatureType::Combine || f.type == FeatureType::Split ||
                         f.type == FeatureType::SplitFace ||
                         f.type == FeatureType::Hole || f.type == FeatureType::Sweep ||
                         f.type == FeatureType::Loft || f.type == FeatureType::Move ||
                         f.type == FeatureType::ConstructPlane || f.type == FeatureType::Gear ||
                         f.type == FeatureType::Thread || f.type == FeatureType::Wing ||
                         f.type == FeatureType::Dovetail || f.type == FeatureType::Blade ||
                         f.type == FeatureType::Align || f.type == FeatureType::Draft ||
                         f.type == FeatureType::Coil || f.type == FeatureType::Pipe ||
                         f.type == FeatureType::ReplaceFace ||
                         f.type == FeatureType::Thicken ||
                         f.type == FeatureType::ConstructAxis || f.type == FeatureType::ConstructPoint ||
                         f.type == FeatureType::Rib || f.type == FeatureType::Web ||
                         f.type == FeatureType::Emboss || f.type == FeatureType::Boss ||
                         f.type == FeatureType::Snap || f.type == FeatureType::Rest ||
                         f.type == FeatureType::Lip || f.type == FeatureType::Silhouette ||
                         f.type == FeatureType::Joint || f.type == FeatureType::AsBuilt ||
                         f.type == FeatureType::Rigid) && i < doc_.markerPos;
        if (hov && editable && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            editFeature(f.id);
        } else if (hov && f.type == FeatureType::Sketch && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && i < doc_.markerPos) {
            editSketch(f.sketchId);
        } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            std::vector<uint32_t> ids;
            for (auto& [id, b] : doc_.bodies) if (b.featureId == f.id) ids.push_back(id);
            selectBodies(ids, ImGui::GetIO().KeyCtrl);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##featctx");
        if (ImGui::BeginPopup("##featctx")) {
            ImGui::TextDisabled("%s", f.name.c_str());
            ImGui::Separator();
            if (editable && ImGui::MenuItem("Edit Feature")) editFeature(f.id);
            if (f.type == FeatureType::Sketch && ImGui::MenuItem("Edit Sketch")) editSketch(f.sketchId);
            if (ImGui::MenuItem(f.suppressed ? "Unsuppress Feature" : "Suppress Feature")) toggleSuppress(i);
            if (ImGui::MenuItem("Roll History Marker Here")) setMarker(i);
            if (ImGui::MenuItem("Delete")) deleteFeature(i);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    // History marker (draggable)
    float mx = c0.x + doc_.markerPos * (cell + gap) + 2 * S;
    ImVec2 m0(mx, c0.y - 2 * S), m1(mx + 6 * S, c0.y + cell + 2 * S);
    ImGui::SetCursorScreenPos(ImVec2(m0.x - 3 * S, m0.y));
    ImGui::InvisibleButton("##marker", ImVec2(12 * S, m1.y - m0.y));
    bool mh = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (mh) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        float rel = ImGui::GetIO().MousePos.x - c0.x;
        int pos = (int)std::round(rel / (cell + gap));
        playing_ = false;
        setMarker(pos);
    }
    dl->AddRectFilled(m0, m1, mh ? IM_COL32(30, 100, 210, 255) : col::Accent, 2 * S);
    if (n == 0) dl->AddText(ImVec2(c0.x + 20 * S, c0.y + 5 * S), col::TextDim, "Timeline: features appear here as you model");

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ===================================================================== navigation bar

void App::drawNavBar() {
    if (!showNavBar_) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float bs = 28 * S;
    float w = 9 * (bs + 2 * S) + 30 * S, h = 36 * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + vp->Size.y - 38 * S - h - 8 * S));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 4 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * S);
    ImGui::Begin("##navbar", nullptr, kPanel);
    if (iconButton("##orbit", Icon::Orbit, bs, true, "Orbit (Shift + middle drag)", navMode_ == NavMode::Orbit))
        navMode_ = navMode_ == NavMode::Orbit ? NavMode::None : NavMode::Orbit;
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##lookat", Icon::LookAt, bs, !selection_.empty(), "Look At selection")) fitView(true);
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##pan", Icon::Pan, bs, true, "Pan (middle drag)", navMode_ == NavMode::Pan))
        navMode_ = navMode_ == NavMode::Pan ? NavMode::None : NavMode::Pan;
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##zoom", Icon::Zoom, bs, true, "Zoom (mouse wheel)", navMode_ == NavMode::Zoom))
        navMode_ = navMode_ == NavMode::Zoom ? NavMode::None : NavMode::Zoom;
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##fit", Icon::Fit, bs, true, "Fit (F6, or double click middle button)")) fitView(false);
    ImGui::SameLine(0, 2 * S);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 sp = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(sp.x + 2 * S, sp.y + 4 * S), ImVec2(sp.x + 2 * S, sp.y + bs - 4 * S), col::PanelBorder);
    ImGui::Dummy(ImVec2(6 * S, bs));
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##display", Icon::Display, bs, true, "Display Settings")) ImGui::OpenPopup("##dispmenu");
    if (ImGui::BeginPopup("##dispmenu")) {
        ImGui::TextDisabled("Visual Style");
        if (ImGui::MenuItem("Shaded with Visible Edges", nullptr, settings_.showEdges)) settings_.showEdges = true;
        if (ImGui::MenuItem("Shaded", nullptr, !settings_.showEdges)) settings_.showEdges = false;
        ImGui::Separator();
        ImGui::TextDisabled("Camera");
        if (ImGui::MenuItem("Perspective", nullptr, settings_.perspective)) settings_.perspective = true;
        if (ImGui::MenuItem("Orthographic", nullptr, !settings_.perspective)) settings_.perspective = false;
        ImGui::Separator();
        ImGui::TextDisabled("Orbit");
        if (ImGui::MenuItem("Free Orbit", nullptr, !settings_.constrainedOrbit)) settings_.constrainedOrbit = false;
        if (ImGui::MenuItem("Constrained Orbit (Z stays up)", nullptr, settings_.constrainedOrbit)) settings_.constrainedOrbit = true;
        ImGui::MenuItem("Reverse Zoom Direction", nullptr, &settings_.reverseZoom);
        ImGui::Separator();
        ImGui::TextDisabled("Mouse Controls");
        static const char* styles[] = {"Fusion 360 / Inventor", "SolidWorks", "Onshape", "Blender", "Tinkercad", "Maya / Alias"};
        for (int i = 0; i < 6; ++i)
            if (ImGui::MenuItem(styles[i], nullptr, settings_.navStyle == i)) { settings_.navStyle = i; settings_.save(settingsPath_); }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##grid", Icon::Grid, bs, true, "Grid and Origin")) ImGui::OpenPopup("##gridmenu");
    if (ImGui::BeginPopup("##gridmenu")) {
        ImGui::MenuItem("Layout Grid", nullptr, &settings_.showGrid);
        ImGui::MenuItem("Origin Axes", nullptr, &settings_.showOrigin);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2 * S);
    if (iconButton("##palette", Icon::Palette, bs, true, "Command search (S)")) { paletteOpen_ = true; paletteBuf_[0] = 0; }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ===================================================================== view cube

void App::drawViewCube() {
    if (!showViewCube_) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    {   // Axis triad in the bottom left corner: which way X, Y and Z point right now.
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        ImVec2 c(vp->Pos.x + 52 * S, vp->Pos.y + vp->Size.y - (38 + 60 + 52) * S);
        float L = 30 * S;
        glm::vec3 r = cam_.right(), u = cam_.up(), f = cam_.forward();
        struct Ax { glm::vec3 d; ImU32 col; const char* n; float depth; };
        Ax ax[3] = {{{1, 0, 0}, IM_COL32(232, 72, 72, 255), "X", 0}, {{0, 1, 0}, IM_COL32(96, 200, 90, 255), "Y", 0},
                    {{0, 0, 1}, IM_COL32(72, 140, 255, 255), "Z", 0}};
        for (auto& a : ax) a.depth = glm::dot(a.d, f);
        std::sort(std::begin(ax), std::end(ax), [](const Ax& a, const Ax& b) { return a.depth > b.depth; });   // far first
        bg->AddCircleFilled(c, 4 * S, col::TextDim, 16);
        for (auto& a : ax) {
            ImVec2 e(c.x + glm::dot(a.d, r) * L, c.y - glm::dot(a.d, u) * L);
            ImU32 cc = a.depth > 0.3f ? ((a.col & 0x00FFFFFF) | 0x90000000u) : a.col;
            bg->AddLine(c, e, cc, 2.4f * S);
            bg->AddCircleFilled(e, 8 * S, cc, 20);
            ImVec2 ts = ImGui::CalcTextSize(a.n);
            bg->AddText(ImVec2(e.x - ts.x * 0.5f, e.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), a.n);
        }
    }
    // The cube sits in the top right corner; the D-pad sits just left of it, level with it.
    float size = 128 * S, cell = 19 * S, dpadW = cell * 3 + 10 * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - size - dpadW - 2 * S, vp->Pos.y + (34 + 76 + 2) * S));
    ImGui::SetNextWindowSize(ImVec2(size + dpadW, size));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##viewcube", nullptr, kPanel | ImGuiWindowFlags_NoBackground);
    {
        // A D-pad above the cube turns the view 90 degrees up, down, left or right; the two round
        // "shoulder" buttons roll it a quarter turn.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 w0 = ImGui::GetWindowPos();
        const char* tips[6] = {"Rotate view left 90", "Rotate view up 90", "Rotate view down 90", "Rotate view right 90",
                               "Roll view counterclockwise 90", "Roll view clockwise 90"};
        // Cell of each button on a 3 x 3 grid (column, row) centered over the cube.
        const int gx[6] = {0, 1, 1, 2, 0, 2}, gy[6] = {1, 0, 2, 1, 0, 0};
        float cxm = w0.x + dpadW * 0.5f, x0 = cxm - cell * 1.5f, y0 = w0.y + size * 0.52f - cell * 1.5f;
        const float hp = glm::half_pi<float>();
        // D-pad body: a rounded cross.
        {
            float t = cell * 0.5f, rr = 4 * S;
            ImVec2 c(cxm, y0 + cell * 1.5f);
            ImU32 bg = (col::Card & 0x00FFFFFF) | 0xE6000000u, edge = col::PanelBorder;
            dl->AddRectFilled(ImVec2(c.x - t, c.y - cell * 1.5f), ImVec2(c.x + t, c.y + cell * 1.5f), bg, rr);
            dl->AddRectFilled(ImVec2(c.x - cell * 1.5f, c.y - t), ImVec2(c.x + cell * 1.5f, c.y + t), bg, rr);
            dl->AddRect(ImVec2(c.x - t, c.y - cell * 1.5f), ImVec2(c.x + t, c.y + cell * 1.5f), edge, rr);
            dl->AddRect(ImVec2(c.x - cell * 1.5f, c.y - t), ImVec2(c.x + cell * 1.5f, c.y + t), edge, rr);
            dl->AddRectFilled(ImVec2(c.x - t + 1, c.y - t + 1), ImVec2(c.x + t - 1, c.y + t - 1), bg);   // clean center
            dl->AddCircleFilled(c, 2.5f * S, col::PanelBorder, 12);
        }
        for (int i = 0; i < 6; ++i) {
            ImVec2 p(x0 + gx[i] * cell, y0 + gy[i] * cell);
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(i);
            bool clicked = ImGui::InvisibleButton("##vca", ImVec2(cell, cell));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            ImVec2 cc(p.x + cell * 0.5f, p.y + cell * 0.5f);
            if (i >= 4) {
                // Shoulder buttons: round, with a curved arrow.
                ImU32 bg = hov ? col::Hover : ((col::Card & 0x00FFFFFF) | 0xE6000000u);
                dl->AddCircleFilled(cc, cell * 0.46f, bg, 24);
                dl->AddCircle(cc, cell * 0.46f, col::PanelBorder, 24);
            } else if (hov) {
                dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + cell - 1, p.y + cell - 1), col::Hover, 3 * S);
            }
            if (hov) ImGui::SetTooltip("%s", tips[i]);
            ImU32 c = hov ? col::Accent : col::TextDim;
            float h = 4.5f * S;
            if (i == 0) dl->AddTriangleFilled(ImVec2(cc.x - h, cc.y), ImVec2(cc.x + h * 0.7f, cc.y - h), ImVec2(cc.x + h * 0.7f, cc.y + h), c);
            if (i == 3) dl->AddTriangleFilled(ImVec2(cc.x + h, cc.y), ImVec2(cc.x - h * 0.7f, cc.y - h), ImVec2(cc.x - h * 0.7f, cc.y + h), c);
            if (i == 1) dl->AddTriangleFilled(ImVec2(cc.x, cc.y - h), ImVec2(cc.x - h, cc.y + h * 0.7f), ImVec2(cc.x + h, cc.y + h * 0.7f), c);
            if (i == 2) dl->AddTriangleFilled(ImVec2(cc.x, cc.y + h), ImVec2(cc.x - h, cc.y - h * 0.7f), ImVec2(cc.x + h, cc.y - h * 0.7f), c);
            if (i >= 4) {
                // Three quarter circle with the arrow head at its end, fully inside the button.
                float rr = cell * 0.24f;
                bool cw = i == 5;
                float s0 = cw ? -2.6f : -0.55f, s1 = cw ? 1.0f : 3.69f;
                dl->PathArcTo(cc, rr, s0, s1, 16);
                dl->PathStroke(c, 0, 1.6f * S);
                float ea = cw ? s1 : s0;
                ImVec2 e(cc.x + std::cos(ea) * rr, cc.y + std::sin(ea) * rr);
                ImVec2 t(-std::sin(ea), std::cos(ea));   // direction of travel at the end
                if (!cw) { t.x = -t.x; t.y = -t.y; }
                ImVec2 n(std::cos(ea), std::sin(ea));
                float ah = 3.2f * S;
                dl->AddTriangleFilled(ImVec2(e.x + t.x * ah, e.y + t.y * ah), ImVec2(e.x + n.x * ah * 0.85f, e.y + n.y * ah * 0.85f),
                                      ImVec2(e.x - n.x * ah * 0.85f, e.y - n.y * ah * 0.85f), c);
            }
            if (clicked) {
                glm::vec3 axis;
                float ang;
                switch (i) {
                case 0: axis = cam_.up(); ang = -hp; break;
                case 3: axis = cam_.up(); ang = hp; break;
                case 1: axis = cam_.right(); ang = -hp; break;
                case 2: axis = cam_.right(); ang = hp; break;
                case 4: axis = -cam_.forward(); ang = -hp; break;
                default: axis = -cam_.forward(); ang = hp; break;
                }
                cam_.animateTo(cam_.target, cam_.distance, glm::normalize(glm::angleAxis(ang, glm::normalize(axis)) * cam_.rot), 0.3f);
                activity();
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(w0.x + dpadW, w0.y));
    }
    auto r = viewCube_.draw(cam_.rot, size);
    if (r.clicked) cam_.lookFrom(r.dir);
    if (r.home) {
        float a = vk_.extent.height ? (float)vk_.extent.width / vk_.extent.height : 1.f;
        cam_.home(sceneBounds_, a);
    }
    if (r.drag.x != 0.f || r.drag.y != 0.f) cam_.orbit(r.drag.x, r.drag.y, settings_.constrainedOrbit);
    ImGui::End();
    ImGui::PopStyleVar();
}

// ===================================================================== overlays

void App::drawIsolateBanner() {
    if (isolated_.empty()) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + (34 + 76 + 10) * S), 0, ImVec2(0.5f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.f, 0.42f, 0.21f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::Begin("##isolate", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Isolated: %d %s", (int)isolated_.size(), isolated_.size() == 1 ? "body" : "bodies");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.2f));
    if (ImGui::SmallButton("Exit Isolate")) exitIsolate();
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleColor(2);
}

void App::drawSectionDialog() {
    if (!sectionOpen_) return;
    const float S = uiScale_;
    ImGui::SetNextWindowSize(ImVec2(280 * S, 0), ImGuiCond_Always);
    if (!ImGui::Begin("Section Analysis###sectiondlg", &sectionOpen_, ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }
    const char* planes[] = {"YZ  (along X)", "XZ  (along Y)", "XY  (along Z)"};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##secplane", &sectionAxis_, planes, 3);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##secoff", sectionOff_, sizeof sectionOff_);
    ImGui::TextDisabled("Plane position, mm. The half past it is hidden.");
    ImGui::Checkbox("Flip side", &sectionFlip_);
    ImGui::Spacing();
    float bw = (ImGui::GetContentRegionAvail().x - 8 * S) * 0.5f;
    if (ImGui::Button("OK", ImVec2(bw, 28 * S))) {
        std::string err = applySection(true);
        if (err.empty()) sectionOpen_ = false;
        else toast(err, ToastKind::Warning);
    }
    ImGui::SameLine(0, 8 * S);
    if (ImGui::Button("Turn off", ImVec2(bw, 28 * S))) {
        applySection(false);
        sectionOpen_ = false;
    }
    ImGui::End();
}

void App::drawStatus() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    std::string s;
    std::string js = jobs_->status();
    if (!js.empty()) s += js + "   |   ";
    if (!selection_.empty()) s += std::to_string(selection_.size()) + " selected   |   ";
    s += std::to_string(renderer_.stats.instances) + " bodies, " + std::to_string(renderer_.stats.meshes) + " unique";
    double mt = renderer_.stats.triangles / 1e6;
    char buf[64];
    std::snprintf(buf, sizeof buf, "   |   %.2fM tris", mt);
    s += buf;
    if (settings_.showFps) { std::snprintf(buf, sizeof buf, "   |   %.0f fps", fps_); s += buf; }
    if (doc_.section.on) {
        const char* plane = doc_.section.axis == 0 ? "YZ" : doc_.section.axis == 1 ? "XZ" : "XY";
        std::snprintf(buf, sizeof buf, "   |   Section %s %g%s", plane, doc_.section.offset, doc_.section.flip ? " flipped" : "");
        s += buf;
    }
    ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 p(vp->Pos.x + vp->Size.x - ts.x - 14 * S, vp->Pos.y + vp->Size.y - 38 * S - ts.y - 10 * S);
    dl->AddText(p, col::Status, s.c_str());
    if (!js.empty()) {
        float a = (float)time_ * 6.f;
        ImVec2 c(p.x - 12 * S, p.y + ts.y * 0.5f);
        dl->PathArcTo(c, 5 * S, a, a + 4.2f, 12);
        dl->PathStroke(col::Accent, 0, 2 * S);
        activity();
    }
}

void App::drawToasts() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->Pos.y + vp->Size.y - (38 + 36 + 28) * S;
    for (int i = (int)toasts_.size() - 1; i >= 0; --i) {
        Toast& t = toasts_[i];
        double life = t.kind == ToastKind::Error ? 9.0 : t.kind == ToastKind::Warning ? 6.0 : 3.5;
        double age = time_ - t.t0;
        if (age > life) { toasts_.erase(toasts_.begin() + i); continue; }
        float alpha = (float)std::min(1.0, (life - age) * 3.0);
        ImU32 bar = t.kind == ToastKind::Error ? col::Error : t.kind == ToastKind::Warning ? col::Warning :
                    t.kind == ToastKind::Success ? col::Success : col::Accent;
        ImVec2 ts = ImGui::CalcTextSize(t.msg.c_str(), nullptr, false, 380 * S);
        ImVec2 sz(ts.x + 26 * S, ts.y + 16 * S);
        ImVec2 p0(vp->Pos.x + vp->Size.x - sz.x - 14 * S, y - sz.y);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 3), ImVec2(p0.x + sz.x + 2, p0.y + sz.y + 3), IM_COL32(0, 0, 0, (int)(25 * alpha)), 6 * S);
        dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), (col::Card & 0x00FFFFFF) | ((ImU32)(250 * alpha) << 24), 6 * S);
        dl->AddRectFilled(p0, ImVec2(p0.x + 4 * S, p0.y + sz.y), (bar & 0x00FFFFFF) | ((ImU32)(255 * alpha) << 24), 6 * S, ImDrawFlags_RoundCornersLeft);
        dl->AddText(nullptr, 0.f, ImVec2(p0.x + 14 * S, p0.y + 8 * S), (col::Text & 0x00FFFFFF) | ((ImU32)(255 * alpha) << 24), t.msg.c_str(), nullptr, 380 * S);
        ImVec2 m = ImGui::GetIO().MousePos;
        if (ImGui::IsMouseClicked(0) && m.x >= p0.x && m.x <= p0.x + sz.x && m.y >= p0.y && m.y <= p0.y + sz.y) t.t0 = time_ - life;
        y -= sz.y + 6 * S;
    }
}

// ===================================================================== closing

bool App::allowClose() {
    if (quitting_) return true;
    bool unsaved = dirty_ && (!doc_.timeline.empty() || !doc_.bodies.empty());
    if (!unsaved) {
        // Another tab with unsaved work: bring it in front and ask about it.
        for (int i = 0; i < (int)designTabs_.size(); ++i) {
            if (i == designActive_) continue;
            const DesignTab& t = designTabs_[(size_t)i];
            if (t.dirty && (!t.doc.timeline.empty() || !t.doc.bodies.empty())) { switchDesignTab(i); unsaved = true; break; }
        }
    }
    if (!unsaved) return true;
    closeAsk_ = true;
    activity();
    return false;
}

void App::requestQuit() {
    if (allowClose()) glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void App::drawCloseAsk() {
    if (closeAsk_) { ImGui::OpenPopup("Save before closing?"); closeAsk_ = false; }
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Save before closing?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::Text("Save the changes to \"%s\"?", doc_.name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("Don't Save closes without them.");
    ImGui::Spacing();
    auto quit = [&] {
        ImGui::CloseCurrentPopup();
        dirty_ = false;   // this one is handled; the next tab with changes asks too
        if (allowClose()) {
            quitting_ = true;
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }
    };
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 0.52f, 0.30f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool save = ImGui::Button("Save", ImVec2(120 * S, 0));
    ImGui::PopStyleColor(3);
    if (save) {
        if (doc_.path.empty()) saveDocumentAs();
        else saveDocumentNow();
        if (!dirty_) quit();   // a cancelled Save As keeps Kestral open
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save", ImVec2(120 * S, 0))) {
        quit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ===================================================================== support the project

namespace {
const char* kDonateUrl = "https://ko-fi.com/ciberai";
long long unixNow() { return (long long)std::time(nullptr); }
}

void App::drawDonateBanner() {
    if (settings_.donateNever || donateSnoozed_) return;
    const float S = uiScale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 64 * S;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), th(IM_COL32(255, 244, 236, 255)), 6 * S);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 94, 91, 160), 6 * S);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * S, p.y + 7 * S));
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("Enjoying Kestral?");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * S, p.y + 26 * S));
    ImGui::TextUnformatted("It's free and made by one person. A coffee keeps it growing.");
    ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * S, p.y + h + 6 * S));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.37f, 0.36f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 0.47f, 0.45f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("Support on Ko-fi")) platform::openUrl(kDonateUrl);
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button("Remove for now")) donateSnoozed_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Never show again")) { settings_.donateNever = true; settings_.save(settingsPath_); }
    ImGui::Separator();
}

void App::drawDonateNote() {
    // A small note, at most every few days, and only after Kestral has been in use for a while.
    static bool showing = false;
    if (settings_.donateNever || donateSnoozed_) { showing = false; return; }
    long long now = unixNow();
    if (!showing) {
        if (time_ < 600.0) return;
        if (settings_.donateLastShown != 0 && now - settings_.donateLastShown < 4 * 24 * 3600) return;
        if (dlg_.open || tdlg_.open || sk_.active) return;   // never in the middle of work
        showing = true;
        settings_.donateLastShown = now;
        settings_.save(settingsPath_);
    }
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 14 * S, vp->Pos.y + vp->Size.y - (38 + 36 + 28) * S), 0, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(330 * S, 0));
    ImGui::Begin("##donate", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("Enjoying Kestral?");
    ImGui::PopFont();
    ImGui::TextWrapped("Kestral is free and made by one person. If it saves you time, you can buy the creator a coffee at ko-fi.com/ciberai.");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.37f, 0.36f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 0.47f, 0.45f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool done = false;
    if (ImGui::Button("Donate")) { platform::openUrl(kDonateUrl); done = true; }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button("Remove for now")) { donateSnoozed_ = true; done = true; }
    ImGui::SameLine();
    if (ImGui::Button("Don't show again")) { settings_.donateNever = true; done = true; }
    ImGui::End();
    if (done) {
        settings_.donateLastShown = now;
        donateSnoozed_ = true;
        settings_.save(settingsPath_);
    }
}

void App::drawPalette() {
    const float S = uiScale_;
    if (paletteOpen_) { ImGui::OpenPopup("##palette"); paletteOpen_ = false; paletteSel_ = 0; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.25f), 0, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(440 * S, 0));
    if (!ImGui::BeginPopup("##palette")) return;
    ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputTextWithHint("##pq", "Type a command (e.g. fillet, slicer, isolate)", paletteBuf_, sizeof paletteBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    std::vector<int> hits;
    for (int i = 0; i < (int)tools_.size(); ++i)
        if (containsI(tools_[i].label, paletteBuf_) || containsI(tools_[i].id, paletteBuf_)) hits.push_back(i);
    // Sketch mode: sketch tools first. Then available before not yet built.
    std::set<int> skTools;
    if (sk_.active)
        for (auto& g : sketchGroups_) for (int ti : g.tools) skTools.insert(ti);
    auto rank = [&](int i) { return (tools_[i].available() ? 0 : 2) + (skTools.count(i) || !sk_.active ? 0 : 1); };
    std::stable_sort(hits.begin(), hits.end(), [&](int a, int b) { return rank(a) < rank(b); });
    if (ImGui::IsItemEdited()) paletteSel_ = 0;
    if (hits.size() > 12) hits.resize(12);
    // One highlight: the arrows and the mouse both move paletteSel_ (ImGui's own nav highlight is off).
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) paletteSel_ = std::min(paletteSel_ + 1, (int)hits.size() - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) paletteSel_ = std::max(paletteSel_ - 1, 0);
    paletteSel_ = std::clamp(paletteSel_, 0, std::max(0, (int)hits.size() - 1));
    bool mouseMoved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int k = 0; k < (int)hits.size(); ++k) {
        const Tool& t = tools_[hits[k]];
        ImVec2 p = ImGui::GetCursorScreenPos();
        std::string label = "      " + t.label + (t.available() ? "" : "   (M" + std::to_string(t.milestone) + ")");
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(k == paletteSel_ ? ImGuiCol_Header : ImGuiCol_PopupBg));
        bool clicked = ImGui::Selectable(label.c_str(), k == paletteSel_);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && mouseMoved) paletteSel_ = k;
        if (clicked) { runTool(t.id); ImGui::CloseCurrentPopup(); }
        drawIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + 10 * S, p.y + ImGui::GetTextLineHeight() * 0.5f), 17 * S, t.icon, t.available());
        if (!t.shortcut.empty()) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 70 * S);
            ImGui::TextDisabled("%s", t.shortcut.c_str());
        }
    }
    ImGui::PopItemFlag();
    if (enter && !hits.empty()) { runTool(tools_[hits[paletteSel_]].id); ImGui::CloseCurrentPopup(); }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::drawSettings() {
    if (!settingsOpen_) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(860 * S, 640 * S), ImGuiCond_Appearing);
    if (!ImGui::Begin("Preferences", &settingsOpen_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    static std::vector<platform::SlicerInfo> detected;
    static bool scanned = false;
    if (!scanned || ImGui::IsWindowAppearing()) { detected = platform::detectSlicers(); scanned = true; }
    int& sec = prefSection_;
    sec = std::clamp(sec, 0, 6);
    const char* secs[] = {"General", "Appearance", "Navigation", "Selection", "3D Printing", "Assistant", "About"};
    const int nSec = 7;
    // Left: sections. Right: the section's settings.
    ImGui::BeginChild("##prefnav", ImVec2(170 * S, -36 * S), ImGuiChildFlags_Borders);
    for (int i = 0; i < nSec; ++i) {
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
        if (ImGui::Selectable(secs[i], sec == i, 0, ImVec2(0, 28 * S))) sec = i;
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##prefbody", ImVec2(0, -36 * S), ImGuiChildFlags_None);
    ImGui::PushFont(fonts_.bold, 20.f * S);
    ImGui::TextUnformatted(secs[sec]);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
    auto head = [&](const char* t) {
        ImGui::Spacing();
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        flashItem(std::string("pref:") + t);
    };
    if (sec == 0) {
        if (ImGui::Button("Keyboard Shortcuts...")) { keysOpen_ = true; keyFilter_[0] = 0; } flashItem("pref:Keyboard Shortcuts...");
        ImGui::SameLine();
        ImGui::TextDisabled("Fusion defaults, all changeable");
        head("Saving");
    float as = settings_.autosaveSec;
    ImGui::SetNextItemWidth(160 * S);
    if (ImGui::SliderFloat("Autosave every (seconds)", &as, 10.f, 600.f, as <= 0 ? "off" : "%.0f")) settings_.autosaveSec = as; flashItem("pref:Autosave every (seconds)");
    ImGui::SameLine(0, 6 * S);
    if (ImGui::SmallButton(settings_.autosaveSec > 0 ? "Turn off" : "Turn on")) settings_.autosaveSec = settings_.autosaveSec > 0 ? 0.f : 30.f;
    ImGui::Checkbox("Autosave saved designs straight into their file", &settings_.autosaveToFile); flashItem("pref:Autosave saved designs straight into their file");
        head("Interface size");
    {
        bool autoSc = settings_.uiScale <= 0.f;
        if (ImGui::Checkbox("Size the interface for my screen automatically", &autoSc)) settings_.uiScale = autoSc ? 0.f : uiScale_; flashItem("pref:Size the interface for my screen automatically");
        ImGui::BeginDisabled(autoSc);
        float sc = autoSc ? uiScale_ : settings_.uiScale;
        ImGui::SetNextItemWidth(160 * S);
        if (ImGui::SliderFloat("Interface size", &sc, 0.75f, 3.f, "%.2fx")) settings_.uiScale = sc; flashItem("pref:Interface size");
        ImGui::EndDisabled();
        if (autoSc) { ImGui::SameLine(); ImGui::TextDisabled("now %.2fx", uiScale_); }
    }
    } else if (sec == 1) {
        head("Theme");
        bool flc1_ = ImGui::Checkbox("Dark theme", &settings_.darkMode);
        flashItem("pref:Dark theme");
        if (flc1_) {
            themeDirty_ = true;
            settings_.save(settingsPath_);
        }
        head("Viewport");
    ImGui::Checkbox("Show edges", &settings_.showEdges); flashItem("pref:Show edges");
    ImGui::Checkbox("Ground grid outside sketches", &settings_.showGrid); flashItem("pref:Ground grid outside sketches");
    ImGui::Checkbox("Show origin axes", &settings_.showOrigin); flashItem("pref:Show origin axes");
    ImGui::Checkbox("Show FPS", &settings_.showFps); flashItem("pref:Show FPS");
    } else if (sec == 2) {
    ImGui::Checkbox("Reverse zoom direction", &settings_.reverseZoom); flashItem("pref:Reverse zoom direction");
    ImGui::Checkbox("Constrained orbit (keep Z up)", &settings_.constrainedOrbit); flashItem("pref:Constrained orbit (keep Z up)");
    {
        static const char* styles[] = {"Fusion 360 / Inventor  (middle pan, Shift+middle orbit)",
                                       "SolidWorks  (middle orbit, Ctrl+middle pan, Shift+middle zoom)",
                                       "Onshape  (right orbit, middle pan)",
                                       "Blender  (middle orbit, Shift+middle pan, Ctrl+middle zoom)",
                                       "Tinkercad  (right orbit, middle pan)",
                                       "Maya / Alias  (Alt+left orbit, Alt+middle pan, Alt+right zoom)"};
        ImGui::SetNextItemWidth(-1);
        int ns = std::clamp(settings_.navStyle, 0, 5);
        if (ImGui::Combo("##navstyle", &ns, styles, 6)) settings_.navStyle = ns;
        flashItem("pref:Mouse controls");
        ImGui::TextDisabled("Mouse controls above. Wheel zooms in every style.");
    }
    ImGui::Checkbox("Perspective camera", &settings_.perspective); flashItem("pref:Perspective camera");
    } else if (sec == 3) {
        ImGui::Checkbox("Body selection priority (otherwise faces, edges and profiles, like Fusion)", &settings_.bodyPriority); flashItem("pref:Body selection priority (otherwise faces, edges and profiles, like Fusion)");
        ImGui::TextDisabled("Hold Ctrl to flip it for one pick: with faces first, Ctrl highlights and\npicks the whole body. Shift adds to the selection (Ctrl+Shift adds bodies).");
        head("Fillet and chamfer");
        ImGui::Checkbox("Tangent chain (a click also picks the smooth edges that flow on)", &tangentChain_); flashItem("pref:Tangent chain (a click also picks the smooth edges that flow on)");
    } else if (sec == 4) {
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("Send to Slicer");
    ImGui::PopFont();
    ImGui::TextWrapped("Current: %s", settings_.slicerPath.empty() ? "(none chosen)" : (settings_.slicerName + "  -  " + settings_.slicerPath).c_str());
    for (auto& d : detected) {
        std::string b = "Use " + d.name;
        if (ImGui::Button(b.c_str())) { settings_.slicerName = d.name; settings_.slicerPath = d.path; settings_.save(settingsPath_); }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", d.path.c_str());
    }
    if (detected.empty()) ImGui::TextDisabled("No slicer found in Program Files. Use Browse to pick its .exe.");
    bool flc2_ = ImGui::Button("Browse for slicer .exe...");
    flashItem("pref:Browse for slicer .exe...");
    if (flc2_) {
        std::string p = platform::pickExeDialog("Choose your slicer program");
        if (!p.empty()) {
            settings_.slicerPath = p;
            settings_.slicerName = pathToU8(u8path(p).stem());
            settings_.save(settingsPath_);
        }
    }
    ImGui::Spacing();

    drawPrinterPrefs();
    if (pendingSendAfterSetup_ && !settings_.slicerPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Send now")) { pendingSendAfterSetup_ = false; settingsOpen_ = false; sendToSlicer(); } flashItem("pref:Send now");
    }
    } else if (sec == 5) {
        drawAssistantPrefs();
        head("Notifications");
    {
        bool ch = false;
        ch |= ImGui::Checkbox("Notify me when the assistant finishes a reply", &settings_.aiNotify); flashItem("pref:Notify me when the assistant finishes a reply");
        ch |= ImGui::Checkbox("Play a sound too", &settings_.aiNotifySound); flashItem("pref:Play a sound too");
        if (settings_.aiNotifySound) {
            ImGui::SameLine();
            const char* kinds[] = {"Chime", "Pop", "Windows sound"};
            ImGui::SetNextItemWidth(150 * uiScale_);
            int k = std::clamp(settings_.aiNotifySoundKind, 0, 2);
            if (ImGui::Combo("##sndkind", &k, kinds, 3)) {
                settings_.aiNotifySoundKind = k;
                ch = true;
                platform::notifyUser(nullptr, "", "", false, k);   // hear it right away
            }
        }
        ImGui::BeginDisabled(!settings_.aiNotify && !settings_.aiNotifySound);
        ch |= ImGui::Checkbox("Only while I'm in another window", &settings_.aiNotifyAway); flashItem("pref:Only while I'm in another window");
        ImGui::EndDisabled();
        if (ImGui::SmallButton("Try it")) {
            bool away = settings_.aiNotifyAway;
            settings_.aiNotifyAway = false;
            notifyAssistantDone("This is how Kestral lets you know the assistant is done.");
            settings_.aiNotifyAway = away;
        }
        if (ch) settings_.save(settingsPath_);
    }
    } else {
        drawDonateBanner();
        ImGui::TextDisabled("Kestral 0.4.0   GPU: %s   OpenCASCADE %s", vk_.gpuName.c_str(), kernel::kernelVersion().c_str());
        // The license notice (GNU AGPL v3) and where the source is.
        ImGui::Spacing();
        ImGui::TextWrapped("Kestral is free software: you can share and change it under the GNU Affero General Public License v3 "
                           "or later. It comes with no warranty. Its source code is on GitHub:");
        if (ImGui::SmallButton("github.com/Ciber101/Kestral")) platform::openUrl("https://github.com/Ciber101/Kestral");
        ImGui::SameLine();
        if (ImGui::SmallButton("License")) platform::openUrl("https://www.gnu.org/licenses/agpl-3.0.html");
        ImGui::SameLine();
        if (ImGui::SmallButton("Third party licenses")) platform::openUrl("https://github.com/Ciber101/Kestral/blob/main/THIRD_PARTY.md");
        ImGui::TextDisabled("Settings and log: %s", platform::appDataDir().c_str());
    }
    ImGui::EndChild();
    float bw = 100 * S;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button("Done", ImVec2(bw, 0))) settingsOpen_ = false; flashItem("pref:Done");
    ImGui::End();
    if (!settingsOpen_) settings_.save(settingsPath_);
}

// ===================================================================== input

void App::handleViewportInput() {
    ImGuiIO& io = ImGui::GetIO();
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top = vp->Pos.y + (34 + 76) * S, bottom = vp->Pos.y + vp->Size.y - 38 * S;
    ImVec2 m = io.MousePos;
    bool inRect = m.x >= vp->Pos.x && m.x < vp->Pos.x + vp->Size.x && m.y >= top && m.y < bottom;
    bool overUi = io.WantCaptureMouse && drag_ == Drag::None && !winSel_;
    viewportHovered_ = inRect && !overUi && glfwGetWindowAttrib(window_, GLFW_HOVERED);
    float W = std::max(io.DisplaySize.x, 1.f), H = std::max(io.DisplaySize.y, 1.f);

    // The marking menu owns the mouse while it is up.
    if (mm_.open) {
        drawMarkingMenu();
        return;
    }

    const bool inSketch = sk_.active || sk_.choosingPlane;
    if (inSketch) {
        sketchUpdate();
        if (sk_.active && sk_.tool == "project") updateHover();   // body edges / faces to project
        else hov_ = HoverInfo{};
        if (viewportHovered_ && drag_ == Drag::None && !sk_.dimEdit && navMode_ == NavMode::None) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) sketchMouseDown();
        }
        sketchMouseMove();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) sketchMouseUp();
    } else {
        updateHover();
        if (holeMode()) updateHolePlacement();
    }

    if (drag_ == Drag::None && viewportHovered_) {
        // View drags follow the chosen navigation style (Preferences): Fusion by default.
        auto startNav = [&](int btn, Drag d) {
            drag_ = d;
            dragBtn_ = btn;
            dragStart_ = m;
            dragDist_ = 0;
            if (drag_ == Drag::Orbit) beginOrbit(m);
        };
        bool handled = false;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            Drag d = (Drag)navFor(1);
            if (d == Drag::None) {
                // Fusion: right click (or right drag gesture) opens the radial marking menu.
                openMarkingMenu(m);
                drawMarkingMenu();
                return;
            }
            startNav(1, d);   // a right click without a drag still opens the menu on release
            handled = true;
        }
        if (!handled && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            if (time_ - lastMiddleClick_ < 0.35) { fitView(false); lastMiddleClick_ = -10; }
            else lastMiddleClick_ = time_;
            Drag d = (Drag)navFor(2);
            startNav(2, d == Drag::None ? Drag::Pan : d);
            handled = true;
        }
        if (!handled && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (Drag)navFor(0) != Drag::None) {
            startNav(0, (Drag)navFor(0));   // Maya / Alias style Alt + left
            handled = true;
        }
        if (!handled && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !manipHovered_ && !manip_.dragging && !inSketch) {
            if (navMode_ != NavMode::None) {
                startNav(0, navMode_ == NavMode::Orbit ? Drag::Orbit : navMode_ == NavMode::Pan ? Drag::Pan : Drag::Zoom);
            } else {
                drag_ = Drag::Select;
                dragBtn_ = 0;
                selPressPos_ = m;
                selPressDouble_ = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                dragDist_ = 0;
            }
        } else if (!handled && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inSketch && navMode_ != NavMode::None) {
            startNav(0, navMode_ == NavMode::Orbit ? Drag::Orbit : navMode_ == NavMode::Pan ? Drag::Pan : Drag::Zoom);
        }
        if (io.MouseWheel != 0.f) {
            float steps = io.MouseWheel * (settings_.reverseZoom ? -1.f : 1.f);
            glm::vec2 ndc(m.x / W * 2.f - 1.f, 1.f - m.y / H * 2.f);
            // Zooming in over a part: the pivot moves onto the surface under the cursor first, so
            // the view keeps closing in on it as far as you like (it would stall at the old pivot).
            glm::dvec3 hp;
            if (steps > 0 && !cam_.ortho && hoverBody_ && doc_.bodies.count(hoverBody_) && hitPoint(hoverBody_, hoverFace_, m, hp)) {
                glm::vec3 eye = cam_.eye(), fw = cam_.forward();
                float d = glm::dot(glm::vec3(hp) - eye, fw);
                if (d > 1e-4f && d < cam_.distance * 0.98f) {
                    cam_.target = eye + fw * d;
                    cam_.distance = d;
                }
            }
            cam_.zoomAt(steps, ndc, W / H);
            activity();
        }
    }
    if (drag_ != Drag::None) {
        ImVec2 d = io.MouseDelta;
        dragDist_ += std::abs(d.x) + std::abs(d.y);
        int btn = dragBtn_ < 0 ? 2 : dragBtn_;
        bool held = ImGui::IsMouseDown((ImGuiMouseButton)btn);
        if (drag_ != Drag::Select && navMode_ == NavMode::None && held) {
            // Modifiers switch the action mid drag (Fusion: Shift turns pan into orbit).
            Drag want = (Drag)navFor(btn);
            if (want == Drag::None && btn == 2) want = Drag::Pan;
            if (want != Drag::None && want != drag_) {
                drag_ = want;
                if (drag_ == Drag::Orbit) beginOrbit(m);
            }
        }
        if (drag_ == Drag::Orbit && (d.x != 0 || d.y != 0)) {
            if (orbitPivotValid_) cam_.orbitAround(glm::vec3(orbitPivot_), d.x, d.y, settings_.constrainedOrbit);
            else cam_.orbit(d.x, d.y, settings_.constrainedOrbit);
        }
        if (drag_ == Drag::Pan && (d.x != 0 || d.y != 0)) cam_.pan(d.x, d.y, H);
        if (drag_ == Drag::Zoom && d.y != 0) {
            ImVec2 c0 = dragStart_;
            cam_.zoomAt(-d.y * 0.03f, glm::vec2(c0.x / W * 2.f - 1.f, 1.f - c0.y / H * 2.f), W / H);
        }
        if (drag_ == Drag::Select && !winSel_) {
            float mx = m.x - selPressPos_.x, my = m.y - selPressPos_.y;
            if (mx * mx + my * my > 36.f * S * S && !measureMode_) {
                winSel_ = true;
                winA_ = selPressPos_;
            }
        }
        if (!held) {
            // Shift + middle click (no drag): the clicked point becomes the view center, so every
            // orbit after this turns around it.
            bool rightClick = btn == 1 && dragDist_ < 4.f * S && drag_ != Drag::Select;
            if (drag_ == Drag::Orbit && btn == 2 && navMode_ == NavMode::None && dragDist_ < 4.f * S && orbitPivotValid_) {
                glm::vec3 p(orbitPivot_);
                float dd = glm::dot(p - cam_.eye(), cam_.forward());
                if (dd > 1e-3f) cam_.animateTo(p, dd, cam_.rot, 0.22f);
            }
            // Shift adds. Ctrl alone picks a whole body in place of the selection.
            bool add = io.KeyShift || (io.KeyCtrl && (tdlg_.open || dlg_.open));
            if (drag_ == Drag::Select && winSel_) {
                add = io.KeyShift || io.KeyCtrl;
                windowSelect(winA_, m, add);
            } else if (drag_ == Drag::Select) {
                if (measureMode_ && hoverBody_ != 0) {
                    measureClick();
                } else {
                    viewportClick(add, selPressDouble_);
                }
            }
            winSel_ = false;
            orbitPivotValid_ = false;
            drag_ = Drag::None;
            dragBtn_ = -1;
            // Styles that orbit with the right button: a right click without a drag is still
            // the marking menu.
            if (rightClick) {
                openMarkingMenu(m);
                mm_.clickMode = true;
            }
        }
        activity();
    }
    if (viewportHovered_ && drag_ == Drag::None) {
        ImGui::SetMouseCursor(navMode_ == NavMode::Pan ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
    }
}

int App::navFor(int b) const {
    const ImGuiIO& io = ImGui::GetIO();
    bool c = io.KeyCtrl, s = io.KeyShift, a = io.KeyAlt;
    Drag none = Drag::None, orbit = Drag::Orbit, pan = Drag::Pan, zoom = Drag::Zoom;
    Drag r = none;
    switch (settings_.navStyle) {
    default:
    case 0:   // Fusion 360 (also Inventor): middle pans, Shift + middle orbits, right = marking menu
        if (b == 2) r = s ? orbit : pan;
        break;
    case 1:   // SolidWorks: middle orbits, Ctrl + middle pans, Shift + middle zooms
        if (b == 2) r = c ? pan : s ? zoom : orbit;
        break;
    case 2:   // Onshape: right orbits, Ctrl + right or middle pans, Shift + right zooms
        if (b == 1) r = c ? pan : s ? zoom : orbit;
        if (b == 2) r = pan;
        break;
    case 3:   // Blender: middle orbits, Shift + middle pans, Ctrl + middle zooms
        if (b == 2) r = s ? pan : c ? zoom : orbit;
        break;
    case 4:   // Tinkercad: right orbits, Shift + right or middle pans
        if (b == 1) r = s ? pan : orbit;
        if (b == 2) r = pan;
        break;
    case 5:   // Maya / Alias: Alt + left orbits, Alt + middle pans, Alt + right zooms
        if (a && b == 0) r = orbit;
        else if (a && b == 1) r = zoom;
        else if (b == 2) r = pan;
        break;
    }
    return (int)r;
}

void App::beginOrbit(ImVec2 m) {
    // Fusion: orbit around exactly the point you pressed on (green pivot dot). The body the GPU
    // says is under the cursor is tried first, then every visible body the ray's box test hits,
    // then the active sketch plane. Empty space orbits around the view center.
    orbitPivotValid_ = false;
    glm::dvec3 o, d;
    if (!rayAt(m, o, d)) return;
    double best = 1e300;
    glm::dvec3 bestP(0);
    auto tryBody = [&](uint32_t id) {
        glm::dvec3 p;
        if (!hitPoint(id, 0, m, p)) return;
        double t = glm::dot(p - o, d);
        if (t > 0 && t < best) { best = t; bestP = p; }
    };
    if (hoverBody_ && doc_.bodies.count(hoverBody_)) tryBody(hoverBody_);
    if (best >= 1e300) {
        size_t tris = 0;
        for (auto& [id, b] : doc_.bodies) {
            if (!bodyShown(b)) continue;
            auto pit = doc_.parts.find(b.partId);
            if (pit == doc_.parts.end() || !pit->second.mesh) continue;
            Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
            if (!bb.valid()) continue;
            // Slab test: skip bodies the ray misses.
            double t0 = -1e300, t1 = 1e300;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                double lo = bb.min[a], hi = bb.max[a];
                if (std::abs(d[a]) < 1e-15) { if (o[a] < lo || o[a] > hi) miss = true; continue; }
                double ta = (lo - o[a]) / d[a], tb = (hi - o[a]) / d[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                if (t0 > t1) miss = true;
            }
            if (miss || t1 < 0) continue;
            tris += pit->second.mesh->indices.size() / 3;
            if (tris > 4000000) break;   // giant scenes: keep the press instant
            tryBody(id);
        }
    }
    if (best >= 1e300 && sk_.active) {
        if (auto* s = curSketch()) {
            sk::Vec2 q;
            if (s->plane.intersect(o, d, q)) {
                glm::dvec3 p = s->plane.toWorld(q);
                if (glm::dot(p - o, d) > 0) { best = 0; bestP = p; }
            }
        }
    }
    if (best < 1e300) {
        orbitPivot_ = bestP;
        orbitPivotValid_ = true;
    }
}

void App::measureClick() {
    bool onEdge = hov_.kind == HoverKind::Edge && doc_.bodies.count(hov_.body);
    uint32_t bodyId = onEdge ? hov_.body : hoverBody_;
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return;
    int kind = std::clamp(measureKind_, 0, 4);
    if (kind >= 1 && !onEdge && !hoverFace_) {
        toast(kind == 1 ? "Click an edge or a flat face" : "Click a face or an edge", ToastKind::Warning);
        return;
    }
    int need = kind <= 1 ? 2 : 1;
    if ((int)measurePicks_.size() >= need) {
        measurePicks_.clear();
        measureDist_ = -1;
    }
    MeasurePick mp;
    mp.body = bodyId;
    mp.face = onEdge ? 0 : hoverFace_;
    mp.edge = onEdge ? hov_.index + 1 : 0;
    mp.name = bit->second.name + (onEdge ? " (edge)" : hoverFace_ ? " (face)" : "");
    measurePicks_.push_back(mp);
    activity();
    if ((int)measurePicks_.size() != need) return;
    std::vector<ShapePtr> shapes;
    std::vector<glm::mat4> xfs;
    for (auto& pk : measurePicks_) {
        auto b2 = doc_.bodies.find(pk.body);
        if (b2 == doc_.bodies.end()) return;
        auto p2 = doc_.parts.find(b2->second.partId);
        if (p2 == doc_.parts.end() || !p2->second.shape) {
            toast("Measure needs solid bodies (mesh bodies have no exact faces)", ToastKind::Warning);
            measurePicks_.clear();
            return;
        }
        shapes.push_back(p2->second.shape);
        xfs.push_back(b2->second.xform);
    }
    measureBusy_ = true;
    measureDist_ = -1;
    uint32_t f0 = measurePicks_[0].face, e0 = measurePicks_[0].edge;
    uint32_t f1 = need > 1 ? measurePicks_[1].face : 0, e1 = need > 1 ? measurePicks_[1].edge : 0;
    auto none = [](uint32_t v) { return v == 0 ? 0xffffffffu : v - 1; };
    JobSystem* jobs = jobs_.get();
    jobs_->submit("measure", [this, jobs, shapes, xfs, f0, f1, e0, e1, kind, none] {
        kernel::CurveMeasure r;
        if (kind == 0) {
            auto d = kernel::measureShapes(shapes[0], xfs[0], shapes[1], xfs[1], none(f0), none(f1), none(e0), none(e1));
            r.ok = d.ok;
            r.value = d.distance;
        } else if (kind == 1) {
            r = kernel::measureAngle(shapes[0], xfs[0], none(f0), none(e0), shapes[1], xfs[1], none(f1), none(e1));
        } else if (kind == 2) {
            r = kernel::measureRadius(shapes[0], xfs[0], none(f0), none(e0));
        } else if (kind == 3) {
            r = kernel::measureArea(shapes[0], xfs[0], none(f0));
        } else {
            r = kernel::measureLoop(shapes[0], xfs[0], none(f0), none(e0));
        }
        jobs->post([this, r, kind] {
            measureBusy_ = false;
            measureDist_ = r.ok ? r.value : -1;
            if (!r.ok) {
                const char* why[] = {
                    "Could not measure that distance",
                    "Click two edges, or two flat faces",
                    "That is not a circle, a cylinder, a sphere, or a torus",
                    "Click a face",
                    "That pick has no loop"};
                toast(why[std::clamp(kind, 0, 4)], ToastKind::Warning);
            }
            activity();
        });
    }, [this](const std::string& err) {
        measureBusy_ = false;
        toast("Measure failed: " + err, ToastKind::Warning);
    });
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (mm_.open || keyRec_ >= 0) return;
    if (ImGui::IsPopupOpen("##palette")) return;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    bool enter = pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter);
    bool inCmd = dlg_.open || tdlg_.open;

    // Type-to-focus, part 2: the value field is live now, hand it the digits typed a frame ago.
    if (!typeAhead_.empty()) {
        // Keys typed while the field was coming alive join the queue, in order.
        for (ImWchar ch : io.InputQueueCharacters)
            if (ch >= 32 && ch < 127) typeAhead_ += ch == ',' ? '.' : (char)ch;
        io.InputQueueCharacters.resize(0);
        if (io.WantTextInput) {
            for (char ch : typeAhead_) io.AddInputCharacter((unsigned)ch);
            typeAhead_.clear();
            return;
        }
        if (++typeAheadFrames_ > 6 || !inCmd) typeAhead_.clear();
        return;
    }

    // Commands: Enter = OK and Esc = Cancel, even while a value field has focus.
    if (inCmd && pressed(ImGuiKey_Escape)) { commandCancel(); return; }
    // Esc closes an open window (the one in front first). The assistant keeps working while closed
    // and opens again right where it was.
    if (pressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow;
        std::string front = nav ? (nav->RootWindow ? nav->RootWindow->Name : nav->Name) : "";
        struct W { const char* name; bool* open; };
        W wins[] = {{"Assistant", &ai_.open}, {"Appearance", &appearanceOpen_}, {"Preferences", &settingsOpen_},
                    {"##datapanel", &dataOpen_}, {"Parameters", &paramsOpen_}, {"Robot", &robotOpen_}, {"Keyboard Shortcuts", &keysOpen_}};
        bool closed = false;
        for (auto& w : wins)
            if (*w.open && front.rfind(w.name, 0) == 0) { *w.open = false; closed = true; break; }
        if (!closed)
            for (auto& w : wins)
                if (*w.open) { *w.open = false; closed = true; break; }
        if (closed) { ImGui::SetWindowFocus(nullptr); return; }
    }
    if (io.WantTextInput) return;   // typing into a field
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    if (inCmd && enter) { commandOk(); return; }
    // Fusion: during a command, just type a number and it goes into the value box. Letters stay
    // shortcuts (pressing E during Fillet starts Extrude), so the box is not focused until then.
    if (inCmd && !io.KeyCtrl && !io.KeyAlt && typeAhead_.empty()) {
        std::string typed;
        for (ImWchar ch : io.InputQueueCharacters)
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == ',') typed += ch == ',' ? '.' : (char)ch;
        if (!typed.empty()) {
            typeAhead_ = typed;
            typeAheadFrames_ = 0;
            if (tdlg_.open) tdlg_.focus = true;
            if (dlg_.open) dlg_.focus = true;
            return;
        }
    }
    if (sketchKeys()) return;

    bool mods = io.KeyCtrl || io.KeyShift || io.KeyAlt;
    if (!mods && !sk_.active && (pressed(ImGuiKey_Delete) || pressed(ImGuiKey_Backspace))) { deleteSelection(); return; }
    if (!mods && pressed(ImGuiKey_Escape)) {
        if (measureMode_) { runMeasure(); return; }
        if (sk_.active) { sketchCancel(); return; }
        if (navMode_ != NavMode::None) navMode_ = NavMode::None;
        else if (anySelection()) clearSelection();
        else if (!isolated_.empty()) exitIsolate();
        return;
    }
    // Everything else is a rebindable shortcut (Preferences > Keyboard Shortcuts).
    KeyCombo c = pressedCombo();
    if (!c.valid()) return;
    int ti = toolForCombo(c);
    if (ti >= 0) { runShortcut(ti); return; }
    if (c.ctrl && c.shift && !c.alt && c.key == ImGuiKey_Z && !inCmd) redo();   // second redo key
}

} // namespace kst
