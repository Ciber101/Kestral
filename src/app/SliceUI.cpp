// The Slice tab's screen, laid out like OrcaSlicer: Prepare / Preview at the top with Slice plate and
// Print plate at the right; the sidebar with Printer, Filament and Process (OrcaSlicer's own pages,
// groups, labels and tooltips, Advanced switch, search); printer and filament settings windows; in
// Preview the legend at the top left, the layer slider at the right and the move slider at the bottom.
#include "app/App.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "print/OrcaOptions.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace kst {

using namespace ui;
using json::Value;

namespace {

// Kestral's own colors: the blue accent for choices, the 3D print orange for Slice and Print.
const ImVec4 kTeal(255 / 255.f, 107 / 255.f, 53 / 255.f, 1.f);
const ImVec4 kTealHover(255 / 255.f, 130 / 255.f, 80 / 255.f, 1.f);
const ImVec4 kOrange(1.f, 0.42f, 0.21f, 1.f);
const ImVec4 kOrangeHover(1.f, 0.52f, 0.3f, 1.f);
const ImVec4 kChanged(0.98f, 0.55f, 0.2f, 1.f);       // a value changed from the profile (Orca: orange)

std::string settingText(const Value& v) {
    if (v.isArray()) return v.size() ? settingText(v[(size_t)0]) : std::string();
    if (v.isString()) return v.asString();
    if (v.isNumber()) {
        std::ostringstream o;
        o << v.asNumber();
        return o.str();
    }
    if (v.isBool()) return v.asBool() ? "1" : "0";
    return {};
}

bool tealButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, kTeal);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTealHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kTeal);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// Orca's warning keys in slice_info, as sentences.
std::string warningText(const std::string& w) {
    if (w == "bed_temperature_too_high_than_filament")
        return "The bed temperature is higher than this filament's softening temperature. The part may warp or clog the nozzle.";
    if (w.find('_') != std::string::npos && w.find(' ') == std::string::npos) {
        std::string s = w;
        for (auto& c : s) if (c == '_') c = ' ';
        s[0] = (char)std::toupper((unsigned char)s[0]);
        return s;
    }
    return w;
}

bool isMultiline(const print::OrcaOptionDef& d) {
    std::string k = d.key;
    return k.find("gcode") != std::string::npos || k == "post_process" || k.find("notes") != std::string::npos;
}

} // namespace

// ===================================================================== values

std::string App::sliceBaseValue(const std::string& k) const {
    for (const Value* j : {&slice_.processJson, &slice_.filamentJson, &slice_.machineJson, &slice_.allJson})
        if (j->isObject() && j->has(k)) return settingText((*j)[k]);
    return {};
}

std::string App::sliceValue(const std::string& k) const {
    auto it = slice_.overrides.find(k);
    return it != slice_.overrides.end() ? it->second : sliceBaseValue(k);
}

void App::sliceSetValue(const std::string& k, const std::string& v) {
    if (v == sliceBaseValue(k)) slice_.overrides.erase(k);
    else slice_.overrides[k] = v;
}

// ===================================================================== one setting

void App::drawOrcaOption(const std::string& key, const std::string& line) {
    const print::OrcaOptionDef* d = print::orcaOption(key);
    if (!d) return;
    const float S = uiScale_;
    std::string type = d->type, cur = sliceValue(key);
    bool changed = slice_.overrides.count(key) > 0;
    ImGui::PushID(key.c_str());
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    float labelW = std::floor(avail * 0.52f);
    ImGui::AlignTextToFramePadding();
    std::string label = *d->label ? d->label : key;
    if (!line.empty()) {
        // Orca shows these side by side on a named row ("Nozzle": First layer, Other layers).
        std::string l = label;
        if (!l.empty()) l[0] = (char)std::tolower((unsigned char)l[0]);
        label = toLower(l) == toLower(line) ? line : line + " (" + l + ")";
    }
    if (changed) ImGui::TextColored(kChanged, "%s", label.c_str());
    else ImGui::TextUnformatted(label.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        if (*d->tooltip) ImGui::TextUnformatted(d->tooltip);
        ImGui::TextDisabled("%s", key.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SameLine(labelW);
    float revertW = changed ? ImGui::GetFrameHeight() : 0.f;
    float w = std::max(40 * S, ImGui::GetContentRegionAvail().x - revertW - 4 * S);
    bool unitInside = false;
    if (type == "coBool" || type == "coBools") {
        bool on = cur == "1" || cur == "true";
        if (ImGui::Checkbox("##v", &on)) sliceSetValue(key, on ? "1" : "0");
    } else if ((type == "coEnum" || type == "coEnums") && *d->values) {
        auto vals = print::splitChoices(d->values), labs = print::splitChoices(d->valueLabels);
        std::string shown = cur;
        for (size_t i = 0; i < vals.size(); ++i)
            if (vals[i] == cur) shown = i < labs.size() && !labs[i].empty() ? labs[i] : vals[i];
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##v", shown.c_str())) {
            for (size_t i = 0; i < vals.size(); ++i) {
                std::string l = i < labs.size() && !labs[i].empty() ? labs[i] : vals[i];
                if (ImGui::Selectable(l.c_str(), vals[i] == cur)) sliceSetValue(key, vals[i]);
            }
            ImGui::EndCombo();
        }
    } else if (isMultiline(*d)) {
        if (ImGui::Button("Edit...", ImVec2(w, 0))) {
            slice_.gcodeKey = key;
            slice_.gcodeBuf = cur;
            ImGui::OpenPopup("##gcodeedit");
        }
        if (ImGui::BeginPopup("##gcodeedit")) {
            ImGui::TextUnformatted(label.c_str());
            ImGui::InputTextMultiline("##g", &slice_.gcodeBuf, ImVec2(560 * S, 320 * S));
            if (tealButton("OK")) { sliceSetValue(slice_.gcodeKey, slice_.gcodeBuf); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    } else {
        // Numbers, percentages, text: typed, kept when the box is left (Orca does the same).
        std::string tmp = cur;
        std::string& buf = slice_.editKey == key ? slice_.editBuf : tmp;
        ImGui::SetNextItemWidth(w);
        ImGui::InputText("##v", &buf);
        if (*d->unit) {
            // Orca shows the unit inside the box, at its right.
            ImVec2 r1 = ImGui::GetItemRectMax(), us = ImGui::CalcTextSize(d->unit);
            ImGui::GetWindowDrawList()->AddText(ImVec2(r1.x - us.x - 6 * S, r1.y - ImGui::GetFrameHeight() * 0.5f - us.y * 0.5f),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), d->unit);
            unitInside = true;
        }
        if (ImGui::IsItemActivated()) { slice_.editKey = key; slice_.editBuf = cur; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { sliceSetValue(key, slice_.editKey == key ? slice_.editBuf : buf); slice_.editKey.clear(); }
        else if (ImGui::IsItemDeactivated() && slice_.editKey == key) slice_.editKey.clear();
    }
    if (*d->unit && !unitInside) {
        ImGui::SameLine(0, 4 * S);
        ImGui::TextDisabled("%s", d->unit);
    }
    if (changed) {
        ImGui::SameLine(0, 4 * S);
        ImGui::PushStyleColor(ImGuiCol_Text, kChanged);
        if (ImGui::SmallButton("<")) slice_.overrides.erase(key);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Back to the profile's value: %s", sliceBaseValue(key).c_str());
    }
    if (!flashKey_.empty() && flashKey_ == "opt:" + key) {
        // A search jump: scroll it into view and blink the whole row.
        if (flashScroll_) { ImGui::SetScrollHereY(0.35f); flashScroll_ = false; }
        flashRect(ImGui::GetForegroundDrawList(), rowStart, ImVec2(rowStart.x + avail, ImGui::GetItemRectMax().y), flashAmount(flashKey_), 4 * S);
    }
    ImGui::PopID();
}

void App::drawOrcaPage(const std::string& tab, const std::string& page, const std::string& search) {
    const float S = uiScale_;
    std::string q = toLower(search);
    std::string lastGroup = "\x01", lastPage = "\x01";
    int shown = 0;
    for (const print::OrcaLayoutRow* r : print::orcaLayout(tab)) {
        const print::OrcaOptionDef* d = print::orcaOption(r->key);
        if (!d) continue;
        std::string mode = d->mode;
        if (mode == "develop") continue;
        if (q.empty()) {
            if (page != r->page) continue;
            if (mode == "advanced" && !slice_.advanced) continue;
        } else {
            std::string hay = toLower(std::string(d->label) + " " + r->key + " " + r->group);
            if (hay.find(q) == std::string::npos) continue;
        }
        // Settings the engine does not use for this printer stay hidden (Orca hides them too).
        if (slice_.allJson.isObject() && !slice_.allJson.has(r->key) && !slice_.overrides.count(r->key)) continue;
        if (!q.empty() && lastPage != r->page) {
            lastPage = r->page;
            lastGroup = "\x01";
        }
        if (lastGroup != r->group) {
            lastGroup = r->group;
            ImGui::Dummy(ImVec2(0, 4 * S));
            {
                ImVec2 gp = ImGui::GetCursorScreenPos();
                float th = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddRect(ImVec2(gp.x, gp.y + th * 0.15f), ImVec2(gp.x + th * 0.7f, gp.y + th * 0.85f), IM_COL32(255, 107, 53, 255), 2 * S, 0, 1.2f * S);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(gp.x + th * 0.18f, gp.y + th * 0.5f), ImVec2(gp.x + th * 0.52f, gp.y + th * 0.5f), IM_COL32(255, 107, 53, 255), 1.2f * S);
                ImGui::SetCursorScreenPos(ImVec2(gp.x + th, gp.y));
            }
            ImGui::PushFont(fonts_.bold, 0.f);
            if (q.empty()) ImGui::TextUnformatted(*r->group ? r->group : r->page);
            else ImGui::Text("%s  >  %s", r->page, r->group);
            ImGui::PopFont();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
                                                ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::Dummy(ImVec2(0, 2 * S));
        }
        drawOrcaOption(r->key, r->line);
        ++shown;
    }
    if (!shown) ImGui::TextDisabled(slice_.settingsLoading ? "Reading settings..." : q.empty() ? "Nothing on this page for this printer." : "Nothing matches.");
}

// ===================================================================== the workspace

std::string App::sliceWarningText(const std::string& w) const { return warningText(w); }

void App::drawSliceWorkspace() {
    {   // The open design can change under the tab (opened by the assistant or the Data panel).
        bool liveNow = sliceActive_ < 0 || sliceTabOwns(sliceActive_, sliceDocKey());
        if (liveNow != slice_.live) { slice_.live = liveNow; sceneDirty_ = true; }
    }
    sliceLoadBed();
    sliceAfterStatus();
    int cur = sliceCurTop();
    if (cur != 3 && camera_.running()) camera_.stop();
    if ((cur == 1 || cur == 2) && slice_.live && (recomputeQueuedFrom_ >= 0 || recomputeRunning_)) {
        // The design is still being built: say so, parts appear on the plate as they finish.
        const float S = uiScale_;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const char* msg = "The design is still rebuilding. Its parts appear on the plate as they finish.";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        float bw = ts.x + 56 * S, bh = ts.y + 16 * S;
        ImVec2 a(vp->Pos.x + (vp->Size.x - bw) * 0.5f, vp->Pos.y + 118 * S);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), col::Card, bh * 0.5f);
        fg->AddRect(a, ImVec2(a.x + bw, a.y + bh), IM_COL32(255, 107, 53, 255), bh * 0.5f, 0, 1.2f * S);
        ImVec2 c(a.x + 20 * S, a.y + bh * 0.5f);
        float ang = (float)ImGui::GetTime() * 5.f;
        fg->PathArcTo(c, 6 * S, ang, ang + 4.2f, 18);
        fg->PathStroke(IM_COL32(255, 107, 53, 255), 0, 2.f * S);
        fg->AddText(ImVec2(a.x + 36 * S, a.y + 8 * S), col::Text, msg);
        activity();
    }   // the camera only streams while the Device page shows it
    switch (cur) {
    case 0: drawSliceHome(); break;
    case 3: drawSliceDevice(); break;
    case 4: drawSliceProject(); break;
    case 5: drawSliceCalibration(); break;
    default:
        drawSlicePanel();
        drawSlicePlateOverlays();
        drawSliceViewButtons();
        if (cur == 1) {
            drawSlice3DToolbar();
            drawPlateGizmo();
            drawPlateContextMenu();
            drawSliceToolPanels();
            if (slice_.layMode) ImGui::SetTooltip("Click the face that should sit on the plate (Esc: stop)");
        } else {
            drawSlicePlateThumbs();
            if (slice_.paths) {
                drawSliceLegend();
                drawSliceLayerSlider();
                drawSliceMoveSlider();
                drawSliceGcodeView();
            } else {
                drawSliceNoPreview();
            }
        }
        break;
    }
    drawSliceSettingsWindow();
    drawSliceSendDialog();
    drawFlushDialog();
}

// ===================================================================== printer and filament settings

void App::drawSliceSettingsWindow() {
    if (!slice_.editWin) return;
    const float S = uiScale_;
    bool printer = slice_.editWin == 1;
    std::string tab = printer ? "printer" : "filament";
    auto pages = print::orcaPages(tab);
    if (slice_.editPage >= (int)pages.size()) slice_.editPage = 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(760 * S, 620 * S), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    std::string title = std::string(printer ? "Printer settings: " + slice_.machine : "Filament settings: " + slice_.filament) + "###slicesettings";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::BeginChild("##pages", ImVec2(170 * S, 0), ImGuiChildFlags_Borders);
        for (int i = 0; i < (int)pages.size(); ++i)
            if (ImGui::Selectable(pages[i].c_str(), i == slice_.editPage)) slice_.editPage = i;
        ImGui::Dummy(ImVec2(0, 10 * S));
        ImGui::Checkbox("Advanced", &slice_.advanced);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##opts", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (!pages.empty()) drawOrcaPage(tab, pages[(size_t)slice_.editPage], "");
        ImGui::Dummy(ImVec2(0, 8 * S));
        ImGui::TextDisabled("Changes apply to the next slice. The profile itself is not changed.");
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open) slice_.editWin = 0;
}

} // namespace kst
