// Keyboard shortcuts: Fusion's defaults, rebindable from Preferences > Keyboard Shortcuts or by
// right clicking any toolbar command ("Change Keyboard Shortcut"), plus copy / paste / select all.
#include "app/App.h"
#include "platform/Platform.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace kst {

using namespace ui;

namespace {

// Sketch tools that start a new sketch when pressed outside one (Fusion: L asks for a plane, then
// draws lines).
bool autoStartsSketch(const std::string& id) {
    static const std::set<std::string> ids = {"line", "rectangle", "rect_center", "rect_3pt", "circle", "circle_2pt", "circle_3pt",
                                              "arc", "arc_center", "polygon", "polygon_in", "slot", "slot_overall", "slot_center", "slot_arc", "sk_point", "cross_point", "project"};
    return ids.count(id) > 0;
}

bool containsCI(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string h = hay, n = needle;
    for (auto& c : h) c = (char)std::tolower((unsigned char)c);
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

} // namespace

void App::applyKeyBindings() {
    toolIsSketch_.assign(tools_.size(), false);
    for (auto& g : sketchGroups_)
        for (int ti : g.tools)
            if (ti >= 0 && ti < (int)tools_.size()) toolIsSketch_[ti] = true;
    toolCombo_.assign(tools_.size(), KeyCombo{});
    for (size_t i = 0; i < tools_.size(); ++i) {
        Tool& t = tools_[i];
        auto it = settings_.keys.find(t.id);
        t.shortcut = it != settings_.keys.end() ? it->second : t.defShortcut;
        toolCombo_[i] = parseCombo(t.shortcut);
        if (toolCombo_[i].valid()) t.shortcut = formatCombo(toolCombo_[i]);   // normalized spelling
    }
}

int App::toolForCombo(const KeyCombo& c) const {
    int best = -1, bestScore = 0;
    for (int i = 0; i < (int)tools_.size() && i < (int)toolCombo_.size(); ++i) {
        if (toolCombo_[i] != c) continue;
        const Tool& t = tools_[i];
        bool isSk = toolIsSketch_[i];
        int score;
        if (t.global) score = 2;
        else if (sk_.active) score = isSk ? 3 : 1;                  // a model command finishes the sketch
        else score = isSk ? (autoStartsSketch(t.id) ? 1 : 0) : 3;   // L, R, C start a sketch
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best;
}

void App::runShortcut(int ti) {
    const Tool& t = tools_[ti];
    std::string id = t.id;
    if (sk_.active && !toolIsSketch_[ti] && !t.global) {
        // Extrude and Hole take the sketch with them; anything else closes the sketch first.
        if (id != "extrude" && id != "hole" && id != "revolve") finishSketch();
    }
    runTool(id);
}

static bool shortcutsClash(bool aSk, bool aGlobal, bool bSk, bool bGlobal) {
    if (aGlobal || bGlobal) return true;
    return aSk == bSk;
}

void App::setToolShortcut(int ti, const std::string& combo) {
    if (ti < 0 || ti >= (int)tools_.size()) return;
    KeyCombo c = parseCombo(combo);
    std::string norm = c.valid() ? formatCombo(c) : std::string();
    // Whoever had these keys in the same workspace loses them (Fusion asks; this just tells you).
    if (c.valid()) {
        for (int j = 0; j < (int)tools_.size(); ++j) {
            if (j == ti || toolCombo_[j] != c) continue;
            if (!shortcutsClash(toolIsSketch_[ti], tools_[ti].global, toolIsSketch_[j], tools_[j].global)) continue;
            settings_.keys[tools_[j].id] = "";
            if (tools_[j].defShortcut.empty()) settings_.keys.erase(tools_[j].id);
            toast(norm + " was " + tools_[j].label + "'s shortcut; that one now has none");
        }
    }
    const Tool& t = tools_[ti];
    if (norm == formatCombo(parseCombo(t.defShortcut))) settings_.keys.erase(t.id);
    else settings_.keys[t.id] = norm;
    applyKeyBindings();
    settings_.save(settingsPath_);
}

void App::beginKeyEdit(int ti) {
    if (ti < 0 || ti >= (int)tools_.size()) return;
    keysOpen_ = true;
    std::snprintf(keyFilter_, sizeof keyFilter_, "%s", tools_[ti].label.c_str());
    keyRec_ = ti;
}

void App::drawKeysDialog() {
    if (!keysOpen_) { keyRec_ = -1; return; }
    const float S = uiScale_;

    // Recording: the next key press (with Ctrl / Shift / Alt) becomes the shortcut.
    if (keyRec_ >= 0) {
        KeyCombo c = pressedCombo();
        bool plain = c.valid() && !c.ctrl && !c.shift && !c.alt;
        if (c.valid()) {
            if (plain && c.key == ImGuiKey_Escape) keyRec_ = -1;
            else if (plain && (c.key == ImGuiKey_Backspace || c.key == ImGuiKey_Delete)) { setToolShortcut(keyRec_, ""); keyRec_ = -1; }
            else if (plain && (c.key == ImGuiKey_Enter || c.key == ImGuiKey_KeypadEnter || c.key == ImGuiKey_Tab)) {
                toast("Enter and Tab are kept for OK and value fields", ToastKind::Warning);
            } else { setToolShortcut(keyRec_, formatCombo(c)); keyRec_ = -1; }
        }
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560 * S, 600 * S), ImGuiCond_Appearing);
    if (!ImGui::Begin("Keyboard Shortcuts", &keysOpen_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Click a shortcut, then press the new keys. Backspace clears it, Esc cancels.");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##kf", "Search commands", keyFilter_, sizeof keyFilter_)) keyRec_ = -1;

    ImGui::BeginChild("##keys", ImVec2(0, -40 * S), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##kt", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 52 * S);
        ImGui::TableHeadersRow();
        std::set<std::string> seen;
        for (int i = 0; i < (int)tools_.size(); ++i) {
            const Tool& t = tools_[i];
            if (!t.available() || !seen.insert(t.id).second) continue;
            std::string where = t.global ? "Anywhere" : toolIsSketch_[i] ? "Sketch" : "Model";
            if (!containsCI(t.label, keyFilter_) && !containsCI(t.shortcut, keyFilter_) && !containsCI(where, keyFilter_)) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImVec2 p = ImGui::GetCursorScreenPos();
            drawIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + 9 * S, p.y + ImGui::GetTextLineHeight() * 0.55f), 16 * S, t.icon, true);
            ImGui::SetCursorScreenPos(ImVec2(p.x + 22 * S, p.y));
            ImGui::TextUnformatted(t.label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", where.c_str());
            ImGui::TableNextColumn();
            bool rec = keyRec_ == i;
            std::string lbl = rec ? "Press keys..." : (t.shortcut.empty() ? "(none)" : t.shortcut);
            if (rec) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
            if (rec) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            if (ImGui::Button((lbl + "###b").c_str(), ImVec2(-1, 0))) keyRec_ = rec ? -1 : i;
            if (rec) ImGui::PopStyleColor(2);
            if (rec && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            ImGui::TableNextColumn();
            bool custom = formatCombo(parseCombo(t.shortcut)) != formatCombo(parseCombo(t.defShortcut));
            if (custom && ImGui::SmallButton("Reset")) setToolShortcut(i, t.defShortcut);
            if (custom && ImGui::IsItemHovered())
                ImGui::SetTooltip("Fusion default: %s", t.defShortcut.empty() ? "none" : t.defShortcut.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (ImGui::Button("Reset All to Fusion Defaults")) {
        settings_.keys.clear();
        applyKeyBindings();
        settings_.save(settingsPath_);
        keyRec_ = -1;
        toast("All shortcuts are back to Fusion's defaults");
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 90 * S);
    if (ImGui::Button("Close", ImVec2(80 * S, 0))) keysOpen_ = false;
    ImGui::End();
    if (!keysOpen_) keyRec_ = -1;
}

// ===================================================================== copy / paste / select all

void App::copySelection() {
    if (sk_.active) {
        toast("Copy in a sketch is not built yet; use Mirror or Offset", ToastKind::Info);
        return;
    }
    std::vector<uint32_t> ids = selectedBodies();
    if (ids.empty()) {
        toast("Select bodies to copy");
        return;
    }
    clipboard_ = ids;
    toast(ids.size() == 1 ? "Copied 1 body" : "Copied " + std::to_string(ids.size()) + " bodies");
}

void App::pasteClipboard() {
    if (sk_.active) return;
    std::vector<uint32_t> ids;
    for (uint32_t id : clipboard_) {
        auto it = doc_.bodies.find(id);
        if (it != doc_.bodies.end() && doc_.isBodyAlive(it->second)) ids.push_back(id);
    }
    if (ids.empty()) {
        toast("Nothing to paste: copy bodies with Ctrl+C first");
        return;
    }
    // Fusion: paste opens Move with the copy, so you drop it where you want.
    clearSelection();
    selectBodies(ids, false);
    startToolFeature(FeatureType::Move);
    if (tdlg_.open && tdlg_.type == FeatureType::Move) {
        tdlg_.option2 = 1;
        applyToolDialog();
    }
}

void App::selectAll() {
    if (sk_.active) {
        if (auto* s = curSketch()) {
            for (auto& [id, e] : s->ents)
                if (!e.locked) sk_.selEnt.insert(id);
        }
        return;
    }
    std::vector<uint32_t> all;
    for (auto& [id, b] : doc_.bodies)
        if (bodyShown(b)) all.push_back(id);
    selectBodies(all, false);
}

} // namespace kst
