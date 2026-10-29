#include "ui/UiItems.h"
#include "core/Log.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace kst::uiitems {

namespace {

std::vector<Item> g_cur, g_last;
std::unordered_map<ImGuiID, size_t> g_index;   // id -> place in g_cur (latest)
std::string g_next;                            // name waiting for the next item
ImGuiWindow* g_nextWin = nullptr;

std::string readable(const char* label) {
    if (!label) return {};
    std::string s = label;
    size_t h = s.find("##");
    if (h != std::string::npos) s.erase(h);
    // Trim, and drop icon glyphs from private use code points (shown as icons, not text).
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xEE || c == 0xEF) {   // U+E000..U+FFFF: icon fonts
            i += 3;
            continue;
        }
        out += s[i++];
    }
    size_t a = out.find_first_not_of(" \t"), b = out.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

std::string windowTitle(ImGuiWindow* w) {
    while (w && w->ParentWindow && (w->Flags & ImGuiWindowFlags_ChildWindow)) w = w->ParentWindow;
    if (!w) return {};
    std::string n = w->Name;
    size_t h = n.find("##");
    if (h == 0) {
        n = n.substr(n.find_first_not_of('#'));
        for (auto& c : n) if (c == '_') c = ' ';
        return n;
    }
    if (h != std::string::npos) n.erase(h);
    return n;
}

Item* find(ImGuiID id) {
    auto it = g_index.find(id);
    return it == g_index.end() ? nullptr : &g_cur[it->second];
}

} // namespace

void install() {
    if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = true;
}

void beginFrame() {
    static int dbg = std::getenv("KESTRAL_UIITEMS_DEBUG") ? 1 : 0, n = 0;
    if (dbg && ++n % 5 == 0) {
        int named = 0;
        for (auto& i : g_cur) named += !i.label.empty();
        kst::log::info("uiitems: " + std::to_string(g_cur.size()) + " items, " + std::to_string(named) + " named");
    }
    g_last.swap(g_cur);
    g_cur.clear();
    g_index.clear();
    g_next.clear();
    g_nextWin = nullptr;
    if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = true;
}

const std::vector<Item>& last() { return g_last; }

void nameLast(const std::string& name) {
    ImGuiContext& g = *GImGui;
    if (g.LastItemData.ID == 0) {   // a plain text label: it names the next item instead
        nameNext(name);
        return;
    }
    if (Item* it = find(g.LastItemData.ID)) {
        if (it->label.empty()) it->label = name;
    }
}

void nameRect(const std::string& name, ImVec2 a, ImVec2 b) {
    ImGuiContext& g = *GImGui;
    Item it;
    it.min = a;
    it.max = b;
    it.label = name;
    it.window = windowTitle(g.CurrentWindow);
    if (g.CurrentWindow) it.visible = g.CurrentWindow->ClipRect.Overlaps(ImRect(a, b));
    g_cur.push_back(it);
}

bool scrollTo(const Item& it) {
    ImGuiWindow* w = ImGui::FindWindowByID(it.windowId);
    // The nearest window that scrolls (the item may sit in a child without its own scrollbar).
    while (w && w->ScrollMax.y <= 0 && w->ParentWindow && (w->Flags & ImGuiWindowFlags_ChildWindow)) w = w->ParentWindow;
    if (!w || w->ScrollMax.y <= 0) return false;
    float mid = (w->InnerClipRect.Min.y + w->InnerClipRect.Max.y) * 0.5f;
    float cy = (it.min.y + it.max.y) * 0.5f;
    ImGui::SetScrollY(w, std::clamp(w->Scroll.y + (cy - mid), 0.f, w->ScrollMax.y));
    return true;
}

void nameNext(const std::string& name) {
    g_next = name;
    g_nextWin = GImGui->CurrentWindow;
}

} // namespace kst::uiitems

// ---------------------------------------------------------------- Dear ImGui's hooks

using namespace kst::uiitems;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
    if (id == 0 || bb.GetWidth() <= 0 || bb.GetHeight() <= 0) return;
    ImGuiContext& g = *ctx;
    Item it;
    it.id = id;
    it.min = bb.Min;
    it.max = bb.Max;
    it.window = windowTitle(g.CurrentWindow);
    if (g.CurrentWindow) {
        it.visible = g.CurrentWindow->ClipRect.Overlaps(bb);
        it.windowId = g.CurrentWindow->ID;
        it.menuItem = (g.CurrentWindow->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu)) != 0;
    }
    if (data) it.disabled = (data->ItemFlags & ImGuiItemFlags_Disabled) != 0;
    if (!g_next.empty() && g_nextWin == g.CurrentWindow) {
        it.label = g_next;
        g_next.clear();
    }
    if (Item* old = find(id)) {   // the same item again this frame (a window's own id): keep the first
        if (old->label.empty()) old->label = it.label;
        return;
    }
    g_index[id] = g_cur.size();
    g_cur.push_back(std::move(it));
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    Item* it = find(id);
    if (!it) return;
    std::string r = readable(label);
    if (!r.empty()) it->label = r;   // Dear ImGui's own label wins over a guessed one
    it->checkable = (flags & ImGuiItemStatusFlags_Checkable) != 0;
    it->checked = (flags & ImGuiItemStatusFlags_Checked) != 0;
    it->openable = (flags & ImGuiItemStatusFlags_Openable) != 0;
    it->opened = (flags & ImGuiItemStatusFlags_Opened) != 0;
    it->input = (flags & ImGuiItemStatusFlags_Inputable) != 0;
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
    Item* it = find(id);
    return it && !it->label.empty() ? it->label.c_str() : nullptr;
}
