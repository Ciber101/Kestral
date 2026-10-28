// The Slice tab's side panel, laid out like OrcaSlicer's: Printer (printer, nozzle, plate type),
// Filament (the numbered filaments with their profiles and colors, flushing volumes), Process
// (Global: the profile and its pages; Objects: the parts, the filament each prints with and their
// own settings).
#include "app/App.h"
#include "app/SliceStyle.h"
#include "print/OrcaOptions.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>

namespace kst {

using namespace ui;
using sl::OIcon;

namespace {

const char* kBeds[] = {"Cool Plate", "Engineering Plate", "High Temp Plate", "Textured PEI Plate", "Textured Cool Plate", "Supertack Plate"};

std::string nozzleOf(const std::string& printerName) {
    std::smatch m;
    static const std::regex re("([0-9]+\\.[0-9]+) nozzle");
    if (std::regex_search(printerName, m, re)) return m[1];
    return {};
}

glm::vec3 hexColor(const std::string& s, glm::vec3 d) {
    if (s.size() >= 7 && s[0] == '#') {
        unsigned v = (unsigned)std::strtoul(s.substr(1, 6).c_str(), nullptr, 16);
        return glm::vec3(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f);
    }
    return d;
}

} // namespace

// A preset list with search, user presets first (shared by the panel and the filament grid).
bool App::slicePresetCombo(const char* id, std::string& cur, const std::vector<const print::Preset*>& list, const char* none, float width, bool frameless) {
    const float S = uiScale_;
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    static std::string filter;
    if (!flashKey_.empty() && flashKey_ == std::string("combo:") + id) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        flashRect(ImGui::GetForegroundDrawList(), p, ImVec2(p.x + width, p.y + ImGui::GetFrameHeight()), flashAmount(flashKey_), 5 * S);
    }
    if (sl::beginMenuCombo(id, cur.empty() ? none : cur.c_str(), std::max(width, 360 * S), frameless)) {
        if (ImGui::IsWindowAppearing()) filter.clear();
        sl::menuSearch(filter, "Search presets");
        std::string lf = toLower(filter);
        ImGui::BeginChild("##rows", ImVec2(0, std::min(380 * S, (float)std::max<size_t>(list.size(), 1) * 30 * S + 60 * S)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        bool userHeader = false, sysHeader = false;
        int shown = 0;
        for (auto* p : list) {
            if (!lf.empty() && toLower(p->name).find(lf) == std::string::npos) continue;
            if (++shown > 400) break;
            if (p->user && !userHeader) { userHeader = true; sl::menuHeader("YOUR PRESETS"); }
            if (!p->user && !sysHeader) { sysHeader = true; sl::menuHeader("SYSTEM PRESETS"); }
            ImU32 sw = 0;
            if (p->type == "filament") { glm::vec3 c = presetColor(p->name); sw = sl::u32(c.r, c.g, c.b); }
            if (sl::menuRow(p->name.c_str(), p->name == cur, nullptr, sw)) { cur = p->name; changed = true; }
            if (p->name == cur && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.3f);
        }
        if (!shown) ImGui::TextDisabled("  Nothing matches \"%s\"", filter.c_str());
        ImGui::EndChild();
        sl::endMenuCombo();
    }
    return changed;
}

void App::drawSlicePanel() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top = sliceTop();
    float w = sliceSideW();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(w, vp->Size.y - top));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Panel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##slicepanel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hh = 34 * S;
    float inner = w - 24 * S;
    // Orca's section header: a strip with the section icon and title; buttons sit at its right.
    float hdrY = 0;
    auto header = [&](const char* title, OIcon ic) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        hdrY = p.y;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + hh), col::Card);
        dl->AddLine(ImVec2(p.x, p.y + hh), ImVec2(p.x + w, p.y + hh), col::PanelBorder);
        sl::drawOIcon(dl, ImVec2(p.x + 18 * S, p.y + hh * 0.5f), 16 * S, ic, sl::orangeU());
        ImGui::PushFont(fonts_.bold, 0.f);
        dl->AddText(ImVec2(p.x + 34 * S, p.y + (hh - ImGui::GetFontSize()) * 0.5f), col::Text, title);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hh + 8 * S));
    };
    float hdrRight = 0;   // buttons fill the header from the right
    auto hdrBegin = [&] { hdrRight = ImGui::GetWindowPos().x + w - 8 * S; };
    auto hdrBtn = [&](const char* id, OIcon ic, const char* tip, bool enabled = true) {
        float bs = 24 * S;
        hdrRight -= bs;
        ImVec2 p(hdrRight, hdrY + (hh - bs) * 0.5f);
        ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(p);
        bool c = ImGui::InvisibleButton(id, ImVec2(bs, bs)) && enabled;
        bool hv = ImGui::IsItemHovered();
        if (hv && enabled) dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + bs), col::Hover, 4 * S);
        sl::drawOIcon(dl, ImVec2(p.x + bs * 0.5f, p.y + bs * 0.5f), 15 * S, ic, enabled ? col::Text : col::TextDim);
        if (hv) ImGui::SetTooltip("%s", tip);
        ImGui::SetCursorScreenPos(keep);
        hdrRight -= 4 * S;
        return c;
    };
    auto hdrPill = [&](const char* label, const char* tip, bool enabled) {
        float pw = ImGui::CalcTextSize(label).x + 18 * S;
        hdrRight -= pw;
        ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(hdrRight, hdrY + (hh - ImGui::GetFrameHeight() + 4 * S) * 0.5f));
        bool c = sl::pillButton(label, enabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
        ImGui::SetCursorScreenPos(keep);
        hdrRight -= 6 * S;
        return c;
    };
    auto indent = [&] { ImGui::SetCursorPosX(12 * S); };

    std::string exe = orcaPathFind();
    if (exe.empty()) {
        header("Slicer engine", OIcon::Gear);
        indent();
        ImGui::PushTextWrapPos(w - 12 * S);
        ImGui::TextWrapped("Kestral slices with OrcaSlicer's engine (free and open source). It runs hidden inside Kestral: "
                           "no OrcaSlicer window. Install it once, about 100 MB.");
        ImGui::PopTextWrapPos();
        indent();
        if (!slice_.installing && sl::orangeButton("Install slicer engine", ImVec2(inner, 0))) engineCheck(true);
        if (slice_.installing) {
            std::lock_guard lk(slice_.m);
            indent();
            ImGui::ProgressBar(slice_.installFrac < 0 ? (float)-ImGui::GetTime() : (float)slice_.installFrac, ImVec2(inner, 0), slice_.installStage.c_str());
            activity();
        }
        if (!slice_.installError.empty()) { indent(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Error), "%s", slice_.installError.c_str()); }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }

    // ================================================================ Printer
    header("Printer", OIcon::Printer);
    hdrBegin();
    if (hdrBtn("##printerset", OIcon::Gear, "Printer settings")) { slice_.editWin = 1; slice_.editPage = 0; }
    if (hdrBtn("##printersync", OIcon::Sync, "Read the nozzle and AMS from the printer")) {
        devicePoll(true);
        slice_.syncFromPrinter = true;
    }
    if (!slice_.setup) {
        indent();
        ImGui::TextDisabled(slice_.setupLoading ? "Reading profiles..." : "No profiles");
    } else {
        // A card: the printer and its profile; the nozzle; the plate type.
        float cardH = 58 * S;
        ImVec2 c0(ImGui::GetCursorScreenPos().x + 10 * S, ImGui::GetCursorScreenPos().y);
        float nozW = 88 * S, bedW = 46 * S;
        float mainW = inner - nozW - bedW - 8 * S;
        dl->AddRectFilled(c0, ImVec2(c0.x + mainW, c0.y + cardH), col::Card, 6 * S);
        dl->AddRect(c0, ImVec2(c0.x + mainW, c0.y + cardH), col::PanelBorder, 6 * S);
        sl::drawOIcon(dl, ImVec2(c0.x + 24 * S, c0.y + cardH * 0.5f), 30 * S, OIcon::Printer, col::Text);
        ImGui::SetCursorScreenPos(ImVec2(c0.x + 44 * S, c0.y + (cardH - ImGui::GetFrameHeight()) * 0.5f));
        if (slicePresetCombo("##printer", slice_.machine, slice_.printers, "Select a printer", mainW - 50 * S, true)) {
            slice_.overrides.clear();
            sliceLoadSettings();
        }
        ImGui::SetItemTooltip("%s", slice_.machine.c_str());
        // Nozzle: the same printer with another nozzle size; and the nozzle's material.
        ImVec2 n0(c0.x + mainW + 4 * S, c0.y);
        dl->AddRectFilled(n0, ImVec2(n0.x + nozW, n0.y + cardH), col::Card, 6 * S);
        dl->AddRect(n0, ImVec2(n0.x + nozW, n0.y + cardH), col::PanelBorder, 6 * S);
        {
            ImVec2 ts = ImGui::CalcTextSize("Nozzle");
            dl->AddText(ImVec2(n0.x + (nozW - ts.x) * 0.5f, n0.y + 3 * S), col::TextDim, "Nozzle");
            std::string cur = nozzleOf(slice_.machine);
            const print::Preset* mp = print::findPreset(*slice_.setup, "machine", slice_.machine);
            const print::Preset* base = mp ? print::systemBase(*slice_.setup, *mp) : nullptr;
            std::string model = mp && !mp->printerModel.empty() ? mp->printerModel : base ? base->printerModel : std::string();
            ImGui::SetCursorScreenPos(ImVec2(n0.x + 6 * S, n0.y + 18 * S));
            ImGui::SetNextItemWidth(nozW - 12 * S);
            if (cur.empty() && base) cur = nozzleOf(base->name);
            if (sl::beginMenuCombo("##nozzle", cur.empty() ? "-" : (cur + " mm").c_str(), 220 * S)) {
                // Every printer profile of this printer model, one row per nozzle size (built in first,
                // then yours, the way Orca's nozzle list reads).
                std::map<std::string, std::vector<const print::Preset*>> bySize;
                for (auto& p : slice_.setup->presets) {
                    if (p.type != "machine") continue;
                    const print::Preset* pb = p.printerModel.empty() ? print::systemBase(*slice_.setup, p) : &p;
                    std::string pm = pb ? pb->printerModel : std::string();
                    if (model.empty() || pm != model) continue;
                    std::string d = nozzleOf(p.name);
                    if (d.empty() && pb) d = nozzleOf(pb->name);
                    if (d.empty()) continue;
                    bySize[d].push_back(&p);
                }
                if (bySize.empty()) ImGui::TextDisabled("No other nozzle sizes for this printer");
                for (auto& [d, list] : bySize) {
                    sl::menuHeader((d + " mm nozzle").c_str());
                    std::stable_sort(list.begin(), list.end(), [](auto* a, auto* b) { return !a->user && b->user; });
                    for (auto* p : list)
                        if (sl::menuRow(p->name.c_str(), p->name == slice_.machine, p->user ? "yours" : "built in")) {
                            slice_.machine = p->name;
                            slice_.overrides.clear();
                            sliceLoadSettings();
                        }
                }
                sl::endMenuCombo();
            }
            ImGui::SetItemTooltip("Nozzle diameter: picks the printer profile for that nozzle");
            std::string nt = sliceValue("nozzle_type");
            std::string ntl = nt == "hardened_steel" ? "Hardened" : nt == "stainless_steel" ? "Stainless" : nt == "brass" ? "Brass" : nt;
            ImVec2 tts = ImGui::CalcTextSize(ntl.c_str());
            ImGui::SetCursorScreenPos(ImVec2(n0.x + (nozW - tts.x) * 0.5f - 4 * S, n0.y + cardH - tts.y - 4 * S));
            if (ImGui::InvisibleButton("##noztype", ImVec2(tts.x + 8 * S, tts.y + 2 * S))) ImGui::OpenPopup("##noztypes");
            dl->AddText(ImVec2(n0.x + (nozW - tts.x) * 0.5f, n0.y + cardH - tts.y - 4 * S), col::TextDim, ntl.c_str());
            ImGui::SetItemTooltip("Nozzle material");
            ImGui::SetNextWindowSizeConstraints(ImVec2(220 * S, 0), ImVec2(400 * S, 600 * S));
            if (ImGui::BeginPopup("##noztypes")) {
                const char* vals[] = {"stainless_steel", "hardened_steel", "brass", "undefine"};
                const char* labs[] = {"Stainless steel", "Hardened steel", "Brass", "Undefined"};
                sl::menuHeader("NOZZLE MATERIAL");
                for (int i = 0; i < 4; ++i)
                    if (sl::menuRow(labs[i], nt == vals[i])) sliceSetValue("nozzle_type", vals[i]);
                ImGui::EndPopup();
            }
        }
        // Plate type: Orca's plate button.
        ImVec2 b0(n0.x + nozW + 4 * S, c0.y);
        ImGui::SetCursorScreenPos(b0);
        if (ImGui::InvisibleButton("##bedbtn", ImVec2(bedW, cardH))) ImGui::OpenPopup("##beds");
        bool bh = ImGui::IsItemHovered();
        dl->AddRectFilled(b0, ImVec2(b0.x + bedW, b0.y + cardH), bh ? col::Hover : col::Card, 6 * S);
        dl->AddRect(b0, ImVec2(b0.x + bedW, b0.y + cardH), col::PanelBorder, 6 * S);
        sl::drawOIcon(dl, ImVec2(b0.x + bedW * 0.5f, b0.y + cardH * 0.5f), 22 * S, OIcon::Bed, col::Text);
        if (bh) ImGui::SetTooltip("Plate type: %s", slice_.bedType.empty() ? "printer's default" : slice_.bedType.c_str());
        ImGui::SetNextWindowSizeConstraints(ImVec2(240 * S, 0), ImVec2(400 * S, 600 * S));
        if (ImGui::BeginPopup("##beds")) {
            sl::menuHeader("PLATE TYPE");
            for (auto* b : kBeds)
                if (sl::menuRow(b, slice_.bedType == b)) { slice_.bedType = b; sceneDirty_ = true; }
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(c0.x - 10 * S, c0.y + cardH + 10 * S));
    }

    // ================================================================ Filament
    sliceFilsEnsure();
    auto& fils = slice_.ex.fils;
    if (!fils.empty() && fils[0].preset.empty() && !slice_.filament.empty() && slice_.filamentJson.isObject() && !slice_.fil1ColorSet) {
        fils[0].color = sliceFilamentColor(0);   // filament 1 shows its profile's color until changed
    }
    header("Filament", OIcon::Filament);
    hdrBegin();
    if (hdrBtn("##filset", OIcon::Gear, "Filament settings")) { slice_.editWin = 2; slice_.editPage = 0; }
    if (hdrBtn("##filsync", OIcon::Sync, "Take the filaments from the printer's AMS (types and colors)")) {
        slice_.syncAmsFils = true;
        devicePoll(true);
    }
    if (hdrBtn("##filminus", OIcon::Minus, "Remove the last filament", fils.size() > 1)) sliceRemoveFil((int)fils.size() - 1);
    if (hdrBtn("##filplus", OIcon::Plus, "Add a filament", fils.size() < 16)) sliceAddFil(slice_.filament, slicePaletteColor(sliceNewFilColor()), false);
    if (hdrPill("Flushing volumes", "How much is purged when the printer changes from one filament to another", fils.size() > 1)) flushOpen_ = true;
    if (slice_.setup) {
        float cellW = (inner - 8 * S) * 0.5f, chip = ImGui::GetFrameHeight();
        for (int i = 0; i < (int)fils.size(); ++i) {
            ImGui::PushID(i + 500);
            float x = 12 * S + (i % 2) * (cellW + 8 * S);
            if (i % 2) ImGui::SameLine(x);
            else ImGui::SetCursorPosX(x);
            ImVec2 p = ImGui::GetCursorScreenPos();
            glm::vec3 fc = fils[(size_t)i].color;
            if (ImGui::InvisibleButton("##chip", ImVec2(chip, chip))) ImGui::OpenPopup("##filcolor");
            sl::filChip(dl, p, chip, fc.r, fc.g, fc.b, i + 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Filament %d: click for its color%s", i + 1, fils[(size_t)i].autoMade ? "\n(made for a part's color)" : "");
            if (ImGui::BeginPopup("##filcolor")) {
                float c3[3] = {fc.r, fc.g, fc.b};
                ImGui::TextDisabled("Filament %d", i + 1);
                if (ImGui::ColorPicker3("##pick", c3, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_PickerHueWheel)) {
                    fils[(size_t)i].color = glm::vec3(c3[0], c3[1], c3[2]);
                    fils[(size_t)i].autoMade = false;
                    if (i == 0) slice_.fil1ColorSet = true;
                    sceneDirty_ = true;
                }
                // The printer's AMS: take a slot's filament.
                const auto& st = slice_.devStatus;
                if (st.ok && (!st.trays.empty() || st.hasExt)) {
                    ImGui::SeparatorText("From the AMS");
                    auto slotBtn = [&](const print::PrintStatus::AmsTray& t, int tid) {
                        if (!t.loaded) return;
                        glm::vec3 tc = hexColor(t.color, glm::vec3(0.5f));
                        ImVec2 q = ImGui::GetCursorScreenPos();
                        std::string lbl = (tid == 254 ? std::string("Ext") : "A" + std::to_string(t.ams + 1) + "-" + std::to_string(t.slot + 1)) + "##" + std::to_string(tid);
                        if (ImGui::InvisibleButton(lbl.c_str(), ImVec2(chip * 1.6f, chip))) {
                            fils[(size_t)i].color = tc;
                            fils[(size_t)i].autoMade = false;
                            auto found = print::searchPresets(*slice_.setup, "filament", (t.name.empty() ? t.type : t.name) + " " + slice_.machine, 1);
                            if (!found.empty() && i > 0) fils[(size_t)i].preset = found[0]->name;
                            else if (!found.empty() && i == 0 && found[0]->name != slice_.filament) { slice_.filament = found[0]->name; sliceLoadSettings(); }
                            if (i == 0) slice_.fil1ColorSet = true;
                            sceneDirty_ = true;
                        }
                        ImGui::GetWindowDrawList()->AddRectFilled(q, ImVec2(q.x + chip * 1.6f, q.y + chip), sl::u32(tc.r, tc.g, tc.b), 4 * S);
                        std::string tt = t.type.empty() ? "?" : t.type;
                        ImGui::GetWindowDrawList()->AddText(ImVec2(q.x + 3 * S, q.y + 2 * S), sl::isDark(tc.r, tc.g, tc.b) ? IM_COL32_WHITE : IM_COL32_BLACK, tt.c_str());
                        ImGui::SetItemTooltip("%s %s", lbl.substr(0, lbl.find('#')).c_str(), t.name.empty() ? t.type.c_str() : t.name.c_str());
                        ImGui::SameLine();
                    };
                    for (auto& t : st.trays) slotBtn(t, t.ams * 4 + t.slot);
                    if (st.hasExt) slotBtn(st.ext, 254);
                    ImGui::NewLine();
                }
                if (i > 0 && ImGui::Button("Remove this filament")) { sliceRemoveFil(i); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 4 * S);
            std::string cur = i == 0 ? slice_.filament : (fils[(size_t)i].preset.empty() ? slice_.filament : fils[(size_t)i].preset);
            float comboW = cellW - chip - 4 * S - chip - 2 * S;
            if (slicePresetCombo("##fp", cur, slice_.filaments, "Select a filament", comboW, false)) {
                if (i == 0) { slice_.filament = cur; slice_.fil1ColorSet = false; sliceLoadSettings(); }
                else {
                    fils[(size_t)i].preset = cur;
                    if (fils[(size_t)i].autoMade == false) fils[(size_t)i].color = presetColor(cur);
                }
                sceneDirty_ = true;
            }
            ImGui::SetItemTooltip("%s", cur.c_str());
            ImGui::SameLine(0, 2 * S);
            ImVec2 ep = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##edit", ImVec2(chip, chip))) { slice_.editWin = 2; slice_.editPage = 0; }
            bool eh = ImGui::IsItemHovered();
            if (eh) { dl->AddCircleFilled(ImVec2(ep.x + chip * 0.5f, ep.y + chip * 0.5f), chip * 0.42f, col::Hover, 16); ImGui::SetTooltip("Edit the filament's settings"); }
            sl::drawOIcon(dl, ImVec2(ep.x + chip * 0.5f, ep.y + chip * 0.5f), chip * 0.6f, OIcon::Edit, col::TextDim);
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 2 * S));
    }
    ImGui::Dummy(ImVec2(0, 4 * S));

    // ================================================================ Process
    header("Process", OIcon::Process);
    {
        // Global | Objects, and Orca's Advanced switch, inside the header.
        ImVec2 keep = ImGui::GetCursorScreenPos();
        float segW = 64 * S, segH = 22 * S;
        ImVec2 s0(ImGui::GetWindowPos().x + 112 * S, hdrY + (hh - segH) * 0.5f);
        dl->AddRectFilled(s0, ImVec2(s0.x + segW * 2, s0.y + segH), col::Chip, segH * 0.5f);
        for (int k = 0; k < 2; ++k) {
            ImVec2 a(s0.x + segW * k, s0.y);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(k + 900);
            if (ImGui::InvisibleButton("##seg", ImVec2(segW, segH))) slice_.objectsView = k == 1;
            ImGui::PopID();
            bool on = slice_.objectsView == (k == 1);
            if (on) dl->AddRectFilled(a, ImVec2(a.x + segW, a.y + segH), sl::orangeU(), segH * 0.5f);
            const char* t = k == 0 ? "Global" : "Objects";
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(a.x + (segW - ts.x) * 0.5f, a.y + (segH - ts.y) * 0.5f), on ? IM_COL32_WHITE : col::TextDim, t);
        }
        // Advanced: a small switch at the right.
        float swW = 34 * S, swH = 18 * S;
        ImVec2 w0(ImGui::GetWindowPos().x + w - swW - 12 * S, hdrY + (hh - swH) * 0.5f);
        ImVec2 lsz = ImGui::CalcTextSize("Advanced");
        dl->AddText(ImVec2(w0.x - lsz.x - 6 * S, hdrY + (hh - lsz.y) * 0.5f), col::TextDim, "Advanced");
        ImGui::SetCursorScreenPos(w0);
        if (ImGui::InvisibleButton("##adv", ImVec2(swW, swH))) slice_.advanced = !slice_.advanced;
        ImGui::SetItemTooltip("Show every setting (off: the common ones, like Orca's simple mode)");
        dl->AddRectFilled(w0, ImVec2(w0.x + swW, w0.y + swH), slice_.advanced ? sl::orangeU() : col::Chip, swH * 0.5f);
        float kx = slice_.advanced ? w0.x + swW - swH * 0.5f : w0.x + swH * 0.5f;
        dl->AddCircleFilled(ImVec2(kx, w0.y + swH * 0.5f), swH * 0.5f - 3 * S, IM_COL32_WHITE, 16);
        ImGui::SetCursorScreenPos(keep);
    }
    print::SliceResult last;
    {
        std::lock_guard lock(print_.m);
        last = print_.last;
    }
    float footer = slice_.overrides.empty() ? 0.f : ImGui::GetFrameHeightWithSpacing() + 6 * S;
    if (!slice_.error.empty() || (last.ok && !last.warnings.empty())) footer += 3 * ImGui::GetTextLineHeightWithSpacing();
    if (!slice_.objectsView) {
        if (slice_.setup) {
            indent();
            float ib = ImGui::GetFrameHeight();
            if (slicePresetCombo("##process", slice_.process, slice_.processes, "Select a process", inner - 3 * (ib + 2 * S), false)) sliceLoadSettings();
            ImGui::SetItemTooltip("%s", slice_.process.c_str());
            auto smallIcon = [&](const char* id, OIcon ic, const char* tip, bool on) {
                ImGui::SameLine(0, 2 * S);
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool c = ImGui::InvisibleButton(id, ImVec2(ib, ib));
                bool hv = ImGui::IsItemHovered();
                if (hv || on) dl->AddRectFilled(p, ImVec2(p.x + ib, p.y + ib), on ? sl::orangeU(60) : col::Hover, 4 * S);
                sl::drawOIcon(dl, ImVec2(p.x + ib * 0.5f, p.y + ib * 0.5f), ib * 0.62f, ic, col::Text);
                if (hv) ImGui::SetTooltip("%s", tip);
                return c;
            };
            if (smallIcon("##psave", OIcon::Save, "Save these settings as your own process profile (Orca's Save preset)", false)) slice_.savePresetOpen = true;
            if (smallIcon("##pcmp", OIcon::Compare, "What is changed from the profile", slice_.compareOpen)) slice_.compareOpen = !slice_.compareOpen;
            if (smallIcon("##psearch", OIcon::Search, "Search the settings", slice_.searchOpen)) slice_.searchOpen = !slice_.searchOpen;
        }
        if (slice_.searchOpen || !slice_.search.empty()) {
            indent();
            ImGui::SetNextItemWidth(inner);
            if (slice_.searchOpen && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint("##setsearch", "Search settings", &slice_.search);
        }
        // Orca's page tabs: plain words, the one in front orange and underlined.
        auto pages = print::orcaPages("process");
        if (slice_.page >= (int)pages.size()) slice_.page = 0;
        if (slice_.search.empty()) {
            ImGui::Dummy(ImVec2(0, 2 * S));
            indent();
            float x0 = ImGui::GetCursorPosX();
            for (int i = 0; i < (int)pages.size(); ++i) {
                ImVec2 ts = ImGui::CalcTextSize(pages[i].c_str());
                float bw = ts.x + 12 * S;
                if (i > 0) {
                    ImGui::SameLine(0, 2 * S);
                    if (ImGui::GetCursorPosX() + bw > w - 8 * S) { ImGui::NewLine(); ImGui::SetCursorPosX(x0); }
                }
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(i + 1200);
                if (ImGui::InvisibleButton("##page", ImVec2(bw, ts.y + 8 * S))) slice_.page = i;
                bool hv = ImGui::IsItemHovered();
                ImGui::PopID();
                bool on = i == slice_.page;
                if (on) ImGui::PushFont(fonts_.bold, 0.f);
                dl->AddText(ImVec2(p.x + 6 * S, p.y + 3 * S), on ? sl::orangeU() : hv ? col::Text : col::TextDim, pages[i].c_str());
                if (on) ImGui::PopFont();
                if (on) dl->AddRectFilled(ImVec2(p.x + 6 * S, p.y + ts.y + 5 * S), ImVec2(p.x + bw - 6 * S, p.y + ts.y + 7 * S), sl::orangeU(), 1 * S);
            }
            ImVec2 lp = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(ImGui::GetWindowPos().x + 8 * S, lp.y), ImVec2(ImGui::GetWindowPos().x + w - 8 * S, lp.y), col::Divider);
        }
        ImGui::Dummy(ImVec2(0, 2 * S));
        ImGui::SetCursorPosX(0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 4 * S));
        ImGui::BeginChild("##processpage", ImVec2(w, std::max(80 * S, ImGui::GetContentRegionAvail().y - footer)), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (slice_.compareOpen) {
            ImGui::PushFont(fonts_.bold, 0.f);
            ImGui::TextUnformatted("Changed from the profiles");
            ImGui::PopFont();
            if (slice_.overrides.empty()) ImGui::TextDisabled("Nothing: every setting is the profile's.");
            for (auto& [k, v] : slice_.overrides) {
                const print::OrcaOptionDef* d = print::orcaOption(k);
                std::string lbl = d && *d->label ? d->label : k;
                ImGui::TextColored(sl::changed(), "%s", lbl.c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
                std::string bv = sliceBaseValue(k);
                ImGui::TextDisabled("%s  >  %s", bv.size() > 14 ? (bv.substr(0, 12) + "..").c_str() : bv.c_str(), v.size() > 14 ? (v.substr(0, 12) + "..").c_str() : v.c_str());
            }
            ImGui::Separator();
        }
        if (slice_.processJson.isObject()) drawOrcaPage("process", pages.empty() ? "" : pages[(size_t)slice_.page], slice_.search);
        else ImGui::TextDisabled(slice_.settingsLoading ? "Reading settings..." : "Pick a printer and a process.");
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
        drawSliceObjects(w, std::max(120 * S, ImGui::GetContentRegionAvail().y - footer));
    }
    if (!slice_.overrides.empty()) {
        indent();
        ImGui::TextColored(sl::changed(), "%d changed from the profiles", (int)slice_.overrides.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset all")) slice_.overrides.clear();
    }
    ImGui::PushTextWrapPos(w - 12 * S);
    if (!slice_.error.empty()) { indent(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Error), "%s", slice_.error.c_str()); }
    else if (last.ok)
        for (size_t i = 0; i < last.warnings.size() && i < 2; ++i) {
            indent();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Warning), "%s", sliceWarningText(last.warnings[i]).c_str());
        }
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    drawSavePresetDialog();
}

// ===================================================================== Objects (Process > Objects)

void App::drawSliceObjects(float w, float height) {
    const float S = uiScale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    sliceFilsEnsure();
    // A selection came in from the design tab: say so, and offer everything again with one click.
    if (!slice_.plateOnly.empty()) {
        int shown = 0;
        for (auto& [id, o] : slice_.plate)
            if (plateInFilter(id) && o.onPlate) ++shown;
        ImGui::SetCursorPosX(12 * S);
        ImGui::TextDisabled("%d selected bod%s from the design", shown, shown == 1 ? "y" : "ies");
        ImGui::SameLine();
        if (ImGui::SmallButton("Show all bodies")) { slice_.plateOnly.clear(); sceneDirty_ = true; }
    }
    ImGui::SetCursorPosX(0);
    float listH = std::min(height * 0.45f, std::max(3.f, (float)slice_.plate.size() + 1.f) * ImGui::GetFrameHeightWithSpacing() + 8 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * S, 4 * S));
    ImGui::BeginChild("##objlist", ImVec2(w, listH), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (ImGui::BeginTable("##objs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
        float fh = ImGui::GetFrameHeight();
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fh);          // filament
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fh);          // settings
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fh);          // on the plate
        ImGui::TableHeadersRow();
        std::set<uint32_t> groupsShown;
        for (auto& [id, o] : slice_.plate) {
            if (!plateInFilter(id)) continue;
            ImGui::PushID((int)id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool sel = false;
            for (int k = 0; k < (int)o.pos.size(); ++k) sel |= plateIsSel(id, k);
            bool out = false;
            for (int k = 0; k < (int)o.pos.size(); ++k) out |= o.onPlate && !plateInstanceOnBed(id, k);
            std::string label = (o.group ? "   " : "") + (o.name.empty() ? std::string("Part") : o.name);
            if (o.pos.size() > 1) label += "  x" + std::to_string(o.pos.size());
            if (o.src != sliceDocKey() && o.src.rfind("file:", 0) != 0) label += "  (" + pathToU8(u8path(o.src.substr(0, o.src.find('#'))).stem()) + ")";
            if (slice_.plates > 1) label += "  [plate " + std::to_string(plateOfCopy(o, 0) + 1) + "]";
            if (!o.onPlate) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::TextDim));
            else if (out) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Error));
            if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                bool add = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
                plateSelect(id, 0, add);
                if (o.group) for (auto& [j, p] : slice_.plate) if (j != id && p.group == o.group) plateSelect(j, 0, true);
            }
            if (!o.onPlate || out) ImGui::PopStyleColor();
            if (out) ImGui::SetItemTooltip("Part of it is off the plate");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (!sel) plateSelect(id, 0, false);
                slice_.ctxOpen = true;
            }
            // Filament: the numbered chip; click to pick another (Orca's filament column).
            ImGui::TableNextColumn();
            ImVec2 p = ImGui::GetCursorScreenPos();
            int fi = std::clamp(o.fil, 0, (int)slice_.ex.fils.size() - 1);
            glm::vec3 fc = sliceFilColor(fi);
            if (ImGui::InvisibleButton("##fil", ImVec2(fh, fh))) ImGui::OpenPopup("##pickfil");
            sl::filChip(dl, ImVec2(p.x + 2 * S, p.y + 2 * S), fh - 4 * S, fc.r, fc.g, fc.b, fi + 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Prints with filament %d%s", fi + 1, o.autoFil ? " (picked from its color)" : "");
            if (ImGui::BeginPopup("##pickfil")) {
                ImGui::TextDisabled("Filament for %s", o.name.c_str());
                if (ImGui::Selectable("By its color (automatic)", o.autoFil)) {
                    for (auto [sid, sk] : sel ? plateSelection() : std::vector<std::pair<uint32_t, int>>{{id, 0}}) { slice_.plate[sid].autoFil = true; slice_.plate[sid].useColor = true; }
                    sliceAutoFils();
                    sceneDirty_ = true;
                }
                for (int f = 0; f < (int)slice_.ex.fils.size(); ++f) {
                    glm::vec3 c = sliceFilColor(f);
                    ImVec2 q = ImGui::GetCursorScreenPos();
                    std::string lb = "       " + std::to_string(f + 1) + "   " + sliceFilPreset(f) + "##f" + std::to_string(f);
                    if (ImGui::Selectable(lb.c_str(), !o.autoFil && f == fi)) {
                        for (auto [sid, sk] : sel ? plateSelection() : std::vector<std::pair<uint32_t, int>>{{id, 0}}) { slice_.plate[sid].autoFil = false; slice_.plate[sid].fil = f; }
                        sliceAutoFils();
                        sceneDirty_ = true;
                    }
                    float hgt = ImGui::GetTextLineHeight();
                    sl::filChip(ImGui::GetWindowDrawList(), ImVec2(q.x + 2 * S, q.y), hgt, c.r, c.g, c.b, f + 1, 2 * S);
                }
                ImGui::EndPopup();
            }
            // Its own settings (a badge when it has some).
            ImGui::TableNextColumn();
            p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##set", ImVec2(fh, fh))) { plateSelect(id, 0, false); slice_.objAddOpen = true; }
            sl::drawOIcon(dl, ImVec2(p.x + fh * 0.5f, p.y + fh * 0.5f), fh * 0.55f, OIcon::Gear, o.settings.empty() ? col::TextDim : sl::orangeU());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(o.settings.empty() ? "Add settings just for this object" : "%d own settings", (int)o.settings.size());
            // On the plate.
            ImGui::TableNextColumn();
            p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##on", ImVec2(fh, fh))) { o.onPlate = !o.onPlate; sceneDirty_ = true; }
            sl::drawOIcon(dl, ImVec2(p.x + fh * 0.5f, p.y + fh * 0.5f), fh * 0.6f, o.onPlate ? OIcon::Eye : OIcon::EyeOff, o.onPlate ? col::Text : col::TextDim);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(o.onPlate ? "On the plate (click: leave it out of the print)" : "Not on the plate (click: put it back)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (slice_.plate.empty()) ImGui::TextDisabled("Nothing on the plate yet. Add a model, or open a design.");
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // The selected object: placing (Orca's object manipulation) and its own settings.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 6 * S));
    ImGui::BeginChild("##objprops", ImVec2(w, std::max(60 * S, height - listH - 6 * S)), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (!slice_.plateSel || !slice_.plate.count(slice_.plateSel)) {
        ImGui::TextDisabled("Click an object (here or on the plate).\nShift drag on the plate picks many.\nM move, R rotate, S scale, F lay on face, C cut.");
    } else {
        auto& o = slice_.plate[slice_.plateSel];
        int k = std::clamp(slice_.plateSelCopy, 0, std::max(0, (int)o.pos.size() - 1));
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(o.name.c_str());
        ImGui::PopFont();
        // X / Y / Z columns like Orca's manipulation panel.
        float col0 = 80 * S, cw = (ImGui::GetContentRegionAvail().x - col0 - 30 * S) / 3.f;
        auto row = [&](const char* label, float* v, int n, const char* unit, float step, const char* id) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            bool ch = false;
            for (int i = 0; i < n; ++i) {
                ImGui::SameLine(col0 + i * (cw + 4 * S));
                ImGui::SetNextItemWidth(cw);
                char nm[32];
                std::snprintf(nm, sizeof nm, "##%s%d", id, i);
                ch |= ImGui::DragFloat(nm, &v[i], step, 0, 0, "%.2f");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", unit);
            return ch;
        };
        ImGui::TextDisabled("%*s X%*s Y%*s Z", (int)(col0 / ImGui::CalcTextSize(" ").x), "", (int)(cw / ImGui::CalcTextSize(" ").x - 1), "",
                            (int)(cw / ImGui::CalcTextSize(" ").x - 1), "");
        if (!o.pos.empty()) {
            const Aabb& cb = plateCore(slice_.plateSel);
            float pv[3] = {o.pos[(size_t)k].x, o.pos[(size_t)k].y, cb.valid() ? 0.f : 0.f};
            if (row("Position", pv, 2, "mm", 0.5f, "pos")) { o.pos[(size_t)k] = glm::vec2(pv[0], pv[1]); sceneDirty_ = true; }
        }
        float rv[1] = {o.rotZ};
        if (row("Rotation", rv, 1, "deg  (Z)", 1.f, "rot")) { o.rotZ = rv[0]; o.cacheValid = false; sceneDirty_ = true; }
        float sv[3] = {o.scale.x * 100.f, o.scale.y * 100.f, o.scale.z * 100.f};
        if (row("Scale", sv, 3, "%", 1.f, "scl")) {
            bool uni = slice_.uniformScale;
            glm::vec3 ns(sv[0], sv[1], sv[2]);
            if (uni) {
                float f = ns.x != o.scale.x * 100.f ? ns.x : ns.y != o.scale.y * 100.f ? ns.y : ns.z;
                ns = glm::vec3(f);
            }
            if (ns.x > 1 && ns.y > 1 && ns.z > 1) { o.scale = ns / 100.f; o.cacheValid = false; sceneDirty_ = true; }
        }
        {
            const Aabb& cb = plateCore(slice_.plateSel);
            if (cb.valid()) {
                float sz[3] = {cb.max.x - cb.min.x, cb.max.y - cb.min.y, cb.max.z - cb.min.z};
                float s0[3] = {sz[0], sz[1], sz[2]};
                if (row("Size", sz, 3, "mm", 0.5f, "size")) {
                    for (int i = 0; i < 3; ++i)
                        if (sz[i] != s0[i] && sz[i] > 0.1f) {
                            float f = sz[i] / s0[i];
                            if (slice_.uniformScale) o.scale *= f;
                            else o.scale[i] *= f;
                        }
                    o.cacheValid = false;
                    sceneDirty_ = true;
                }
            }
        }
        ImGui::Checkbox("Uniform scale", &slice_.uniformScale);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("   Copies");
        ImGui::SameLine();
        if (ImGui::SmallButton("-##cp") && o.pos.size() > 1) {
            o.pos.pop_back();
            if (o.plateOf.size() > o.pos.size()) o.plateOf.resize(o.pos.size());
            slice_.plateSelCopy = 0;
            sceneDirty_ = true;
        }
        ImGui::SameLine();
        ImGui::Text("%d", (int)o.pos.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("+##cp")) plateAddCopies(1);
        if (ImGui::Button("Lay flat")) plateLayFlat(slice_.plateSel, -1);
        ImGui::SameLine();
        if (ImGui::Button("Auto orient")) { plateAutoOrient(slice_.plateSel); plateArrangeOne(slice_.activePlate); }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            o.rotZ = 0;
            o.lay = glm::mat4(1.f);
            o.scale = glm::vec3(1.f);
            o.cacheValid = false;
            sceneDirty_ = true;
        }
        // Per object settings (Orca: right-click > Add settings): any process setting, just for this object.
        ImGui::Dummy(ImVec2(0, 4 * S));
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted("Object settings");
        ImGui::PopFont();
        ImGui::SameLine();
        if (sl::pillButton("+ Add setting")) slice_.objAddOpen = true;
        std::string drop;
        for (auto& [key, val] : o.settings) {
            ImGui::PushID(key.c_str());
            const print::OrcaOptionDef* d = print::orcaOption(key);
            std::string lbl = d && *d->label ? d->label : key;
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(sl::changed(), "%s", lbl.c_str());
            if (d && *d->tooltip) ImGui::SetItemTooltip("%s", d->tooltip);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
            float vw = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 4 * S;
            std::string type = d ? d->type : "";
            if (d && (type == "coBool" || type == "coBools")) {
                bool on = val == "1" || val == "true";
                if (ImGui::Checkbox("##v", &on)) val = on ? "1" : "0";
            } else if (d && (type == "coEnum" || type == "coEnums") && *d->values) {
                auto vals = print::splitChoices(d->values), labs = print::splitChoices(d->valueLabels);
                std::string shown = val;
                for (size_t i = 0; i < vals.size(); ++i) if (vals[i] == val) shown = i < labs.size() && !labs[i].empty() ? labs[i] : vals[i];
                ImGui::SetNextItemWidth(vw);
                if (ImGui::BeginCombo("##v", shown.c_str())) {
                    for (size_t i = 0; i < vals.size(); ++i)
                        if (ImGui::Selectable((i < labs.size() && !labs[i].empty() ? labs[i] : vals[i]).c_str(), vals[i] == val)) val = vals[i];
                    ImGui::EndCombo();
                }
            } else {
                ImGui::SetNextItemWidth(vw);
                ImGui::InputText("##v", &val);
            }
            ImGui::SameLine(0, 4 * S);
            if (ImGui::SmallButton("x")) drop = key;
            ImGui::SetItemTooltip("Remove: the object uses the plate's setting again");
            ImGui::PopID();
        }
        if (!drop.empty()) o.settings.erase(drop);
        if (o.settings.empty()) ImGui::TextDisabled("None: this object prints with the process settings.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // Add a setting: search every process setting.
    if (slice_.objAddOpen) { ImGui::OpenPopup("Add a setting for this object"); slice_.objAddOpen = false; slice_.objSearch.clear(); }
    ImGui::SetNextWindowSize(ImVec2(420 * S, 460 * S), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add a setting for this object", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##q", "Search (wall loops, infill, brim...)", &slice_.objSearch);
        ImGui::BeginChild("##opts", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4 * S), ImGuiChildFlags_Borders);
        std::string q = toLower(slice_.objSearch);
        std::string lastPage;
        auto it = slice_.plate.find(slice_.plateSel);
        for (const print::OrcaLayoutRow* r : print::orcaLayout("process")) {
            const print::OrcaOptionDef* d = print::orcaOption(r->key);
            if (!d || std::string(d->mode) == "develop") continue;
            std::string hay = toLower(std::string(d->label) + " " + r->key + " " + r->group + " " + r->page);
            if (!q.empty() && hay.find(q) == std::string::npos) continue;
            if (lastPage != r->page) { lastPage = r->page; ImGui::SeparatorText(r->page); }
            bool has = it != slice_.plate.end() && it->second.settings.count(r->key);
            std::string lb = std::string(*d->label ? d->label : r->key) + "   (" + r->group + ")##" + r->key;
            if (ImGui::Selectable(lb.c_str(), has) && it != slice_.plate.end()) {
                if (!has) it->second.settings[r->key] = sliceValue(r->key);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
        if (ImGui::Button("Close", ImVec2(110 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ===================================================================== flushing volumes

void App::drawFlushDialog() {
    if (flushOpen_) { ImGui::OpenPopup("Flushing volumes"); flushOpen_ = false; }
    const float S = uiScale_;
    if (!ImGui::BeginPopupModal("Flushing volumes", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    sliceFilsEnsure();
    auto& f = slice_.ex.fils;
    size_t n = f.size();
    ImGui::TextUnformatted("Flushing volume (mm3) when changing from a filament (row) to another (column).");
    ImGui::TextDisabled("Going from dark to light needs more. Kestral starts from Orca's rule of thumb.");
    if (ImGui::BeginTable("##flush", (int)n + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        float fh = ImGui::GetFrameHeight();
        ImGui::TableSetupColumn("From \\ To", ImGuiTableColumnFlags_WidthFixed, 74 * S);
        for (size_t c = 0; c < n; ++c) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 64 * S);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("from \\ to");
        for (size_t c = 0; c < n; ++c) {
            ImGui::TableNextColumn();
            ImVec2 p = ImGui::GetCursorScreenPos();
            sl::filChip(ImGui::GetWindowDrawList(), p, fh, f[c].color.r, f[c].color.g, f[c].color.b, (int)c + 1);
            ImGui::Dummy(ImVec2(fh, fh));
        }
        for (size_t r = 0; r < n; ++r) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImVec2 p = ImGui::GetCursorScreenPos();
            sl::filChip(ImGui::GetWindowDrawList(), p, fh, f[r].color.r, f[r].color.g, f[r].color.b, (int)r + 1);
            ImGui::Dummy(ImVec2(fh, fh));
            for (size_t c = 0; c < n; ++c) {
                ImGui::TableNextColumn();
                if (r == c) { ImGui::TextDisabled("  -"); continue; }
                ImGui::PushID((int)(r * 16 + c));
                ImGui::SetNextItemWidth(60 * S);
                float& v = slice_.ex.flush[r * n + c];
                if (ImGui::DragFloat("##v", &v, 1.f, 0.f, 1500.f, "%.0f")) slice_.ex.flushAuto = false;
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(120 * S);
    ImGui::DragFloat("Multiplier", &slice_.ex.flushMul, 0.01f, 0.f, 3.f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("Auto-calculate")) { slice_.ex.flushAuto = true; sliceFlushDefaults(); }
    ImGui::Spacing();
    if (sl::orangeButton("OK", ImVec2(110 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ===================================================================== save preset

void App::drawSavePresetDialog() {
    const float S = uiScale_;
    if (slice_.savePresetOpen) {
        ImGui::OpenPopup("Save process preset");
        slice_.savePresetOpen = false;
        slice_.savePresetName = slice_.process + " (Kestral)";
    }
    if (!ImGui::BeginPopupModal("Save process preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextUnformatted("Your own process profile: the current one with the changes you made.");
    ImGui::TextDisabled("It lands in OrcaSlicer's user presets, so OrcaSlicer sees it too.");
    ImGui::SetNextItemWidth(380 * S);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputText("Name", &slice_.savePresetName);
    if (sl::orangeButton("Save", ImVec2(110 * S, 0))) {
        std::string err = sliceSaveProcessPreset(slice_.savePresetName);
        if (err.empty()) { toast("Saved " + slice_.savePresetName, ToastKind::Success); ImGui::CloseCurrentPopup(); }
        else toast(err, ToastKind::Error);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace kst
