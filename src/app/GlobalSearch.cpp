// The search box at the top right: finds any command, slice setting, page, preset or preference,
// jumps there and makes the spot blink so the eye finds it.
#include "app/App.h"
#include "app/SliceStyle.h"
#include "print/OrcaOptions.h"
#include "ui/Icons.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace kst {

using namespace ui;

namespace {

struct PrefEntry { int sec; const char* label; const char* section; const char* words; };
const PrefEntry kPrefs[] = {
    {0, "Keyboard Shortcuts...", "General", ""},
    {0, "Saving", "General", ""},
    {0, "Autosave every (seconds)", "General", "auto save backup"},
    {0, "Autosave saved designs straight into their file", "General", ""},
    {0, "Interface size", "General", "ui scale zoom dpi"},
    {0, "Size the interface for my screen automatically", "General", ""},
    {1, "Theme", "Appearance", ""},
    {1, "Dark theme", "Appearance", "dark mode light mode night colors"},
    {1, "Viewport", "Appearance", ""},
    {1, "Show edges", "Appearance", ""},
    {1, "Ground grid outside sketches", "Appearance", ""},
    {1, "Show origin axes", "Appearance", ""},
    {1, "Show FPS", "Appearance", "frames per second performance"},
    {2, "Reverse zoom direction", "Navigation", "scroll wheel invert mouse"},
    {2, "Constrained orbit (keep Z up)", "Navigation", ""},
    {2, "Perspective camera", "Navigation", ""},
    {3, "Body selection priority (otherwise faces, edges and profiles, like Fusion)", "Selection", ""},
    {3, "Fillet and chamfer", "Selection", ""},
    {3, "Tangent chain (a click also picks the smooth edges that flow on)", "Selection", ""},
    {4, "Browse for slicer .exe...", "3D Printing", ""},
    {5, "Notifications", "Assistant", ""},
    {5, "Notify me when the assistant finishes a reply", "Assistant", "notification sound alert"},
    {5, "Play a sound too", "Assistant", ""},
    {5, "Only while I'm in another window", "Assistant", ""},
    {5, "Model", "Assistant", ""},
    {5, "Provider", "Assistant", "ollama openai anthropic claude lm studio llm ai model"},
    {5, "Address", "Assistant", ""},
    {5, "Speed and memory", "Assistant", ""},
    {5, "Context window", "Assistant", "tokens memory ctx num_ctx"},
    {5, "Tools sent", "Assistant", "tool mode smart find_tools prompt size"},
    {5, "Thinking", "Assistant", "reasoning think effort"},
    {5, "Keep the model loaded", "Assistant", "keep_alive unload memory"},
    {5, "Check the graphics card before sending (other models or programs using it)", "Assistant", ""},
    {5, "Temperature", "Assistant", "creativity randomness"},
    {5, "Long chats", "Assistant", ""},
    {5, "When the window fills", "Assistant", "compaction compact summarize history"},
    {5, "Start at", "Assistant", ""},
    {5, "Step limit", "Assistant", "max steps iterations"},
    {5, "Behaviour", "Assistant", ""},
    {5, "Tell it which page I'm on and what is selected", "Assistant", ""},
    {5, "Show its thinking", "Assistant", ""},
    {5, "Let it start prints without asking", "Assistant", ""},
    {2, "Mouse controls", "Navigation", "navigation style orbit pan fusion solidworks onshape blender tinkercad maya"},
    {4, "Printers", "3D Printing", "bambu printer access code ip lan add printer"},
    {5, "Your own instructions (sent with every chat)", "Assistant", ""},
    {5, "API key", "Assistant", ""},
};

struct S3dEntry { const char* id; const char* label; const char* words; };
const S3dEntry kS3d[] = {
    {"add", "Add a model to the plate", "import stl step obj 3mf file"},
    {"addplate", "Add plate", "new plate bed"},
    {"orient", "Auto orient", "rotate best side"},
    {"arrange", "Arrange all objects", "auto arrange pack layout"},
    {"splitobj", "Split to objects", "separate"},
    {"splitpart", "Split to parts", "separate multi color"},
    {"varlayer", "Variable layer height", "adaptive layers height ranges"},
    {"move", "Move parts on the plate", "translate position gizmo"},
    {"rotate", "Rotate parts on the plate", "turn gizmo"},
    {"scale", "Scale parts", "resize size gizmo"},
    {"layface", "Lay on face", "flat face down place"},
    {"cut", "Cut a part", "split height"},
    {"support", "Support painting", "paint supports enforcer blocker"},
    {"seam", "Seam painting", "paint seam"},
    {"paint", "Color painting", "paint color multi color filament mmu ams"},
    {"measure", "Measure", "distance"},
};

const char* kSliceTops[] = {"Home", "Prepare", "Preview", "Device", "Project", "Calibration"};
const char* kSliceTopWords[] = {"recent print projects", "plate printer filament process settings", "layers sliced gcode preview",
                                "printer camera ams status controls", "project name author notes", "calibration flow pressure advance temperature tower retraction"};

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Every word of the query must be found; the label scores more than the extra words.
int scoreOf(const std::vector<std::string>& q, const std::string& label, const std::string& extra) {
    std::string l = lower(label), e = lower(extra);
    int score = 0;
    for (auto& w : q) {
        size_t p = l.find(w);
        if (p == std::string::npos) {
            if (e.find(w) == std::string::npos) return 0;
            score += 10;
            continue;
        }
        if (p == 0) score += 60;
        else if (!std::isalnum((unsigned char)l[p - 1])) score += 40;
        else score += 20;
    }
    std::string whole;
    for (auto& w : q) whole += (whole.empty() ? "" : " ") + w;
    if (l == whole) score += 100;
    score -= (int)std::min<size_t>(l.size(), 60) / 6;   // shorter labels first
    return std::max(score, 1);
}

} // namespace

// ---------------------------------------------------------------- blinking

float App::flashAmount(const std::string& key) {
    if (flashKey_.empty() || key != flashKey_) return 0.f;
    double t = glfwGetTime() - flashT0_;
    if (t > 2.6) { flashKey_.clear(); return 0.f; }
    activity();
    return 0.5f - 0.5f * std::cos((float)t * 2.f * 3.14159f / 0.85f);   // three soft blinks
}

void App::flashRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float amount, float rounding) {
    if (amount <= 0.f) return;
    const float S = uiScale_;
    dl->AddRectFilled(ImVec2(a.x - 3 * S, a.y - 2 * S), ImVec2(b.x + 3 * S, b.y + 2 * S), sl::orangeU((int)(80 * amount)), rounding);
    dl->AddRect(ImVec2(a.x - 3 * S, a.y - 2 * S), ImVec2(b.x + 3 * S, b.y + 2 * S), sl::orangeU((int)(255 * amount)), rounding, 0, 2.f * S);
}

void App::flashItem(const std::string& key) {
    if (flashKey_.empty() || key != flashKey_) return;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    b.x = std::max(b.x, right - 4 * uiScale_);
    if (flashScroll_) { ImGui::SetScrollHereY(0.35f); flashScroll_ = false; }
    flashRect(ImGui::GetForegroundDrawList(), a, b, flashAmount(key), 5 * uiScale_);
}

void App::flashStart(const std::string& key) {
    flashKey_ = key;
    flashT0_ = glfwGetTime() + 0.15;   // a moment for the page to open first
    flashScroll_ = true;
    flashOpenGroup_ = true;
    activity();
}

// ---------------------------------------------------------------- results

std::vector<App::SearchHit> App::globalSearch(const std::string& query) {
    std::vector<SearchHit> hits;
    std::vector<std::string> q;
    {
        std::string w;
        for (char c : lower(query) + " ") {
            if (std::isspace((unsigned char)c)) { if (!w.empty()) q.push_back(w); w.clear(); }
            else w += c;
        }
    }
    if (q.empty()) return hits;
    auto add = [&](int kind, std::string label, std::string path, std::string key, int a, int b, const std::string& extra) {
        int sc = scoreOf(q, label, path + " " + key + " " + extra);
        if (sc > 0 && kind == 4) {
            // A setting whose own group matches the words too is the likelier one.
            std::string grp = lower(path.substr(path.rfind('>') == std::string::npos ? 0 : path.rfind('>') + 1));
            for (auto& w : q) if (grp.find(w) != std::string::npos) sc += 15;
        }
        if (sc > 0) hits.push_back({kind, std::move(label), std::move(path), std::move(key), a, b, sc});
    };
    // Commands, with the toolbar group they live in.
    auto groupOf = [&](int ti, std::string& where) {
        for (auto& g : solidGroups_) for (int t : g.tools) if (t == ti) { where = "Design > Solid > " + g.name; return; }
        for (auto& g : utilGroups_) for (int t : g.tools) if (t == ti) { where = "Design > Utilities > " + g.name; return; }
        for (auto& g : sketchGroups_) for (int t : g.tools) if (t == ti) { where = "Sketch > " + g.name; return; }
        where = "Command";
    };
    for (int i = 0; i < (int)tools_.size(); ++i) {
        const Tool& t = tools_[(size_t)i];
        if (!t.available()) continue;
        std::string where;
        groupOf(i, where);
        if (!t.shortcut.empty()) where += "   " + t.shortcut;
        add(0, t.label, where, t.id, i, 0, t.tip);
    }
    for (auto& s : kS3d) add(1, s.label, "Slice > Prepare toolbar", s.id, 0, 0, s.words);
    for (int i = 0; i < 6; ++i) add(2, std::string(kSliceTops[i]) + " page", "Slice", "", i, 0, kSliceTopWords[i]);
    add(3, "Slice workspace", "Workspaces", "slice", 0, 0, "orca slicer print plate");
    add(3, "Move from Fusion", "Workspaces", "fusion", 1, 0, "import fusion 360 designs migrate");
    // Every OrcaSlicer setting, by its page.
    static const char* tabs[] = {"process", "filament", "printer"};
    static const char* tabNames[] = {"Process", "Filament settings", "Printer settings"};
    for (int ti = 0; ti < 3; ++ti) {
        auto pages = print::orcaPages(tabs[ti]);
        for (const print::OrcaLayoutRow* r : print::orcaLayout(tabs[ti])) {
            const print::OrcaOptionDef* d = print::orcaOption(r->key);
            if (!d || std::string(d->mode) == "develop") continue;
            std::string label = *d->label ? d->label : r->key;
            if (*r->line && lower(label) != lower(r->line)) label = std::string(r->line) + " (" + label + ")";
            // Short names used in several groups ("Infill" under Speed, Jerk...): say which one.
            std::string g = r->group;
            if (!g.empty() && lower(label).find(lower(g)) == std::string::npos && lower(g).find(lower(label)) == std::string::npos &&
                label.find(' ') == std::string::npos)
                label = g + ": " + label;
            int page = 0;
            for (int p = 0; p < (int)pages.size(); ++p) if (pages[(size_t)p] == r->page) page = p;
            std::string path = std::string("Slice > ") + tabNames[ti] + " > " + r->page + (*r->group ? std::string(" > ") + r->group : "");
            add(4, label, path, r->key, ti, page, d->tooltip);
        }
    }
    // Profiles.
    if (slice_.setup) {
        auto presets = [&](const std::vector<const print::Preset*>& list, const char* what, int which) {
            for (auto* p : list) add(5, p->name, std::string("Slice > ") + what + (p->user ? " (yours)" : ""), "", which, 0, "preset profile");
        };
        presets(slice_.printers, "Printer presets", 0);
        presets(slice_.processes, "Process presets", 1);
        presets(slice_.filaments, "Filament presets", 2);
    }
    for (auto& p : kPrefs) add(6, p.label, std::string("Preferences > ") + p.section, "", p.sec, 0, p.words);
    for (int i = 0; i < 7; ++i) {
        static const char* secs[] = {"General", "Appearance", "Navigation", "Selection", "3D Printing", "Assistant", "About"};
        add(6, std::string(secs[i]) + " preferences", "Preferences", "", i, 0, "settings options");
    }
    std::stable_sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    if (hits.size() > 60) hits.resize(60);
    return hits;
}

void App::globalSearchGo(const SearchHit& h) {
    gsearch_.open = false;
    gsearch_.text.clear();
    gsearch_.last.clear();
    ImGui::ClearActiveID();   // the box lets go of its text (Enter keeps it active otherwise)
    switch (h.kind) {
    case 0: {   // a command: show it on the toolbar, open its menu when it is not a big icon
        int ti = h.a;
        auto findIn = [&](const std::vector<ToolGroup>& gs) {
            for (auto& g : gs) for (int t : g.tools) if (t == ti) return true;
            return false;
        };
        bool solid = findIn(solidGroups_), util = findIn(utilGroups_);
        if (!solid && !util) {
            if (findIn(sketchGroups_) && sk_.active) { flashStart("tool:" + tools_[(size_t)ti].id); return; }
            runTool(tools_[(size_t)ti].id);   // not on a toolbar: just do it
            return;
        }
        if (sliceWs_) leaveSliceWorkspace();
        if (startOpen_) { newDocument(); startOpen_ = false; }
        if (sk_.active) finishSketch();
        activeTab_ = solid ? 0 : 1;
        flashStart("tool:" + tools_[(size_t)ti].id);
        return;
    }
    case 1:
        enterSliceWorkspace();
        if (sliceCurTop() != 1) setSliceTop(1);
        flashStart("s3d:" + h.key);
        return;
    case 2:
        enterSliceWorkspace();
        setSliceTop(h.a);
        flashStart("stop:" + std::to_string(h.a));
        return;
    case 3:
        if (h.a == 0) { enterSliceWorkspace(); flashStart("stop:" + std::to_string(sliceCurTop())); }
        else fusionMoveOpen_ = true;
        return;
    case 4: {   // a slice setting
        enterSliceWorkspace();
        const print::OrcaOptionDef* d = print::orcaOption(h.key);
        if (d && std::string(d->mode) == "advanced") slice_.advanced = true;
        if (h.a == 0) {
            if (sliceCurTop() != 1 && sliceCurTop() != 2) setSliceTop(1);
            slice_.objectsView = false;
            slice_.search.clear();
            slice_.searchOpen = false;
            slice_.page = h.b;
        } else {
            slice_.editWin = h.a == 2 ? 1 : 2;
            slice_.editPage = h.b;
        }
        flashStart("opt:" + h.key);
        return;
    }
    case 5:
        enterSliceWorkspace();
        if (sliceCurTop() != 1 && sliceCurTop() != 2) setSliceTop(1);
        if (h.a == 1) slice_.objectsView = false;
        flashStart(h.a == 0 ? "combo:##printer" : h.a == 1 ? "combo:##process" : "combo:##fp");
        return;
    case 6:
        settingsOpen_ = true;
        prefSection_ = h.a;
        flashStart("pref:" + h.label);
        if (h.label.find(" preferences") != std::string::npos) flashKey_.clear();
        return;
    }
}

// ---------------------------------------------------------------- the box

void App::drawGlobalSearch(ImDrawList* dl, float right, float y, float h) {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = std::clamp(vp->Size.x * 0.17f, 150 * S, 290 * S);
    if (gsearch_.open) w = std::max(w, std::min(360 * S, vp->Size.x * 0.3f));
    float bh = 26 * S;
    ImVec2 a(right - w, y + (h - bh) * 0.5f), b(right, a.y + bh);
    bool active = gsearch_.open;
    dl->AddRectFilled(a, b, active ? col::Card : col::Chip, bh * 0.5f);
    dl->AddRect(a, b, active ? sl::orangeU() : col::PanelBorder, bh * 0.5f, 0, active ? 1.5f * S : 1.f * S);
    sl::drawOIcon(dl, ImVec2(a.x + 15 * S, a.y + bh * 0.5f), 13 * S, sl::OIcon::Search, col::TextDim);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 28 * S, a.y + (bh - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::SetNextItemWidth(w - 40 * S);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    if (gsearch_.focus) { ImGui::SetKeyboardFocusHere(); gsearch_.focus = false; }
    bool enter = ImGui::InputTextWithHint("##gsearch", "Search Kestral   Ctrl+F", &gsearch_.text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool inputActive = ImGui::IsItemActive();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (inputActive) gsearch_.open = true;
    if (gsearchTest_) {   // developer check: keep the list open, or jump once the profiles are read
        activity();
        if (gsearchTestGo_ && glfwGetTime() > (std::getenv("KESTRAL_SLICE_TEST") ? 20.0 : 8.0)) {
            gsearchTest_ = false;
            gsearch_.hits = globalSearch(gsearch_.text);
            if (!gsearch_.hits.empty()) { SearchHit h0 = gsearch_.hits[0]; globalSearchGo(h0); }
            return;
        }
        gsearch_.open = true;
        inputActive = true;
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::GetIO().WantTextInput) { gsearch_.focus = true; gsearch_.open = true; }
    if (gsearch_.text != gsearch_.last) {
        gsearch_.last = gsearch_.text;
        gsearch_.hits = globalSearch(gsearch_.text);
        gsearch_.sel = 0;
        gsearch_.scrollTo = true;
    }
    if (!gsearch_.open || gsearch_.text.empty()) {
        if (!inputActive) gsearch_.open = false;
        return;
    }
    // Keys while typing: arrows move, Enter goes, Escape closes.
    if (inputActive) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { gsearch_.sel = std::min(gsearch_.sel + 1, (int)gsearch_.hits.size() - 1); gsearch_.scrollTo = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { gsearch_.sel = std::max(gsearch_.sel - 1, 0); gsearch_.scrollTo = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { gsearch_.open = false; ImGui::ClearActiveID(); return; }
    }
    // The results: a card under the box, grouped by kind.
    float rw = std::max(w, 440 * S);
    ImGui::SetNextWindowPos(ImVec2(std::min(right - rw, right - w), b.y + 6 * S));
    ImGui::SetNextWindowSize(ImVec2(rw, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(rw, 0), ImVec2(rw, std::min(520 * S, vp->Size.y * 0.7f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 6 * S));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Card));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::PanelBorder));
    ImGui::Begin("##gsearchresults", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImDrawList* pl = ImGui::GetWindowDrawList();
    static const char* kindNames[] = {"COMMANDS", "SLICE TOOLS", "SLICE PAGES", "WORKSPACES", "SETTINGS", "PRESETS", "PREFERENCES"};
    if (gsearch_.hits.empty()) {
        ImGui::Dummy(ImVec2(rw - 12 * S, 4 * S));
        ImGui::TextDisabled("  Nothing matches \"%s\"", gsearch_.text.c_str());
        ImGui::Dummy(ImVec2(0, 4 * S));
    }
    // Best kinds first: the order of each kind's best hit.
    std::vector<int> order;
    for (auto& h : gsearch_.hits) if (std::find(order.begin(), order.end(), h.kind) == order.end()) order.push_back(h.kind);
    std::vector<int> shownIdx;
    float listH = 0;
    for (int k : order) {
        int n = 0;
        for (auto& h : gsearch_.hits) if (h.kind == k && n < 8) ++n;
        listH += 24 * S + n * 40 * S;
    }
    ImGui::BeginChild("##gsl", ImVec2(rw - 12 * S, std::min(listH, std::min(500 * S, vp->Size.y * 0.66f))), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList* cl = ImGui::GetWindowDrawList();
    float cw = ImGui::GetContentRegionAvail().x;
    int clicked = -1;
    // Rows in drawing order, so the arrow keys follow what is on screen.
    std::vector<int> drawOrder;
    for (int k : order) {
        int n = 0;
        for (int i = 0; i < (int)gsearch_.hits.size(); ++i)
            if (gsearch_.hits[(size_t)i].kind == k && n++ < 8) drawOrder.push_back(i);
    }
    int selRow = std::clamp(gsearch_.sel, 0, std::max(0, (int)drawOrder.size() - 1));
    int row = 0;
    for (int k : order) {
        ImVec2 hp = ImGui::GetCursorScreenPos();
        cl->AddText(fonts_.bold, ImGui::GetFontSize() * 0.82f, ImVec2(hp.x + 8 * S, hp.y + 6 * S), col::TextDim, kindNames[k]);
        ImGui::Dummy(ImVec2(cw, 24 * S));
        int n = 0;
        for (int i = 0; i < (int)gsearch_.hits.size(); ++i) {
            const SearchHit& h = gsearch_.hits[(size_t)i];
            if (h.kind != k || n++ >= 8) continue;
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##hit", ImVec2(cw, 38 * S))) clicked = i;
            bool hv = ImGui::IsItemHovered();
            ImGui::PopID();
            bool sel = row == selRow;
            if (hv) gsearch_.sel = row;
            if (sel && gsearch_.scrollTo) { ImGui::SetScrollHereY(0.5f); gsearch_.scrollTo = false; }
            if (sel || hv) cl->AddRectFilled(p, ImVec2(p.x + cw, p.y + 38 * S), hv ? col::Hover : col::Selected, 6 * S);
            if (sel) cl->AddRectFilled(ImVec2(p.x, p.y + 8 * S), ImVec2(p.x + 3 * S, p.y + 30 * S), sl::orangeU(), 1.5f * S);
            // The kind's icon in a soft square.
            ImVec2 ic(p.x + 22 * S, p.y + 19 * S);
            cl->AddRectFilled(ImVec2(ic.x - 13 * S, ic.y - 13 * S), ImVec2(ic.x + 13 * S, ic.y + 13 * S), col::Chip, 6 * S);
            if (h.kind == 0) drawIcon(cl, ic, 18 * S, tools_[(size_t)h.a].icon, true);
            else {
                sl::OIcon oi = h.kind == 1 ? sl::OIcon::Arrange : h.kind == 2 ? sl::OIcon::Prepare : h.kind == 3 ? sl::OIcon::Layers
                             : h.kind == 4 ? (h.a == 0 ? sl::OIcon::Process : h.a == 1 ? sl::OIcon::Filament : sl::OIcon::Printer)
                             : h.kind == 5 ? (h.a == 0 ? sl::OIcon::Printer : h.a == 1 ? sl::OIcon::Process : sl::OIcon::Filament)
                                           : sl::OIcon::Gear;
                sl::drawOIcon(cl, ic, 15 * S, oi, col::Text);
            }
            std::string label = h.label, path = h.path;
            float tx = p.x + 44 * S, maxW = cw - 52 * S;
            while (label.size() > 4 && ImGui::CalcTextSize(label.c_str()).x > maxW) label = label.substr(0, label.size() - 4) + "..";
            while (path.size() > 4 && ImGui::CalcTextSize(path.c_str()).x * 0.86f > maxW) path = path.substr(0, path.size() - 4) + "..";
            cl->AddText(ImVec2(tx, p.y + 3 * S), col::Text, label.c_str());
            cl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.86f, ImVec2(tx, p.y + 20 * S), col::TextDim, path.c_str());
            ++row;
        }
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        const char* hint = "Up and down to choose, Enter to go there";
        ImGui::SetCursorPosX((rw - ImGui::CalcTextSize(hint).x * 0.86f) * 0.5f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        pl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.86f, p, col::TextDim, hint);
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    if (enter && !drawOrder.empty()) clicked = drawOrder[(size_t)selRow];
    if (gsearch_.sel != selRow) gsearch_.sel = selRow;
    if (clicked >= 0) { SearchHit h = gsearch_.hits[(size_t)clicked]; globalSearchGo(h); return; }
    // A click anywhere else closes it.
    if (!inputActive && !hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gsearch_.open = false;
    if (!inputActive && !hovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
        // Focus left (Tab, another window): close unless the mouse is on the list.
        if (ImGui::GetIO().WantTextInput) gsearch_.open = false;
    }
}

} // namespace kst
