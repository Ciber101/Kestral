// MCP tools that drive the whole program the way a person does: a picture of the window, mouse
// and keyboard input, the camera, undo, design tabs, workspaces, preferences, Kestral's own
// assistant, the log. Together with the assistant's modeling tools they reach everything in
// Kestral. Input goes in through Dear ImGui's event queue, so every button, menu, dialog and the
// viewport react exactly as to a real mouse and keyboard.
#include "app/App.h"
#include "app/Mcp.h"
#include "core/Json.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "ui/UiItems.h"
#include "third_party/stb_image.h"

#include <GLFW/glfw3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>
#include <tuple>

namespace kst {

using json::Value;

namespace {

struct UiToolDef { const char* name; const char* desc; const char* schema; };

const UiToolDef kUiTools[] = {
    {"screenshot",
     "A picture of the whole Kestral window exactly as the user sees it: menus, toolbar, browser, timeline, dialogs, the Slice "
     "tab, everything. Use it to find where things are before ui_input. Coordinates in the picture can be passed to ui_input as "
     "they are (it scales them to the window).",
     R"({"type":"object","properties":{}})"},
    {"ui_input",
     "Use the mouse and keyboard in Kestral like a person: every button, menu, dialog, field, the browser tree, the timeline and the "
     "3D view respond. Give a list of actions, done in order. x and y are pixels in the last screenshot (or window pixels with "
     "space=\"window\"), or give target (a control's name, as in ui_click, with window and index) instead of x and y. Actions: {\"action\":\"move\",\"x\",\"y\"}, {\"action\":\"click\",\"x\",\"y\",\"button\":\"left|right|middle\","
     "\"double\":false}, {\"action\":\"drag\",\"x\",\"y\",\"to_x\",\"to_y\",\"button\",\"keys\":\"shift\"} (keys held during the drag, e.g. "
     "shift+middle drag orbits), {\"action\":\"scroll\",\"x\",\"y\",\"amount\"} (positive scrolls up / zooms in), "
     "{\"action\":\"type\",\"text\"} (into the focused field; numbers typed during a command go into its value box), "
     "{\"action\":\"key\",\"key\":\"ctrl+z\"} (names: a-z, 0-9, f1-f12, enter, escape, tab, backspace, delete, space, up, down, left, "
     "right, home, end, pageup, pagedown, with ctrl+, shift+, alt+), {\"action\":\"wait\",\"ms\"}. Returns a screenshot afterwards "
     "unless screenshot=false.",
     R"J({"type":"object","properties":{"actions":{"type":"array","items":{"type":"object"}},"space":{"type":"string","enum":["screenshot","window"]},"screenshot":{"type":"boolean"},"smooth":{"type":"boolean","description":"glide the mouse (default true; false jumps)"}},"required":["actions"]})J"},
    {"get_ui_state",
     "What is on screen right now: workspace (design or slice), open design tabs, the open dialog or command, sketch mode, the "
     "selection, which panels are open, recent messages Kestral showed, the window size and every visible window or popup with its "
     "rectangle (in screenshot pixels).",
     R"({"type":"object","properties":{}})"},
    {"list_commands",
     "Every Kestral command (toolbar, menus, utilities) with its id for run_command, its label, keyboard shortcut and what it does.",
     R"({"type":"object","properties":{"query":{"type":"string","description":"only commands whose id, label or tip contain this"}}})"},
    {"view",
     "Move the camera: view=home, fit, fit_selection, iso, front, back, top, bottom, left or right; ortho=true/false switches "
     "orthographic and perspective; orbit_x / orbit_y turn the view (in screen pixels of mouse movement, 100 is about a quarter turn); "
     "zoom (positive zooms in, in wheel steps).",
     R"({"type":"object","properties":{"view":{"type":"string"},"ortho":{"type":"boolean"},"orbit_x":{"type":"number"},"orbit_y":{"type":"number"},"zoom":{"type":"number"}}})"},
    {"undo", "Undo the last steps (like Ctrl+Z). steps: how many (default 1).",
     R"({"type":"object","properties":{"steps":{"type":"number"}}})"},
    {"redo", "Redo steps that were undone (like Ctrl+Y). steps: how many (default 1).",
     R"({"type":"object","properties":{"steps":{"type":"number"}}})"},
    {"design_tabs",
     "The open design tabs: action=list (default), switch (index), new (an empty design in a new tab) or close (index; unsaved work "
     "goes to the autosave).",
     R"({"type":"object","properties":{"action":{"type":"string","enum":["list","switch","new","close"]},"index":{"type":"number"}}})"},
    {"workspace",
     "Switch between the Design workspace and the Slice tab (to=design or slice). In the Slice tab, page picks Prepare, Preview, "
     "Device, Project or Calibration.",
     R"({"type":"object","properties":{"to":{"type":"string","enum":["design","slice"]},"page":{"type":"string","enum":["prepare","preview","device","project","calibration"]}},"required":["to"]})"},
    {"get_settings",
     "Kestral's preferences as key = value (API keys, passwords and other secrets are left out). query narrows the list.",
     R"({"type":"object","properties":{"query":{"type":"string"}}})"},
    {"set_setting",
     "Change one preference by its key from get_settings (for example darkMode, uiScale, showFps, perspective, showEdges). Secrets, the "
     "MCP server's own settings and whether prints may start without asking cannot be changed this way.",
     R"({"type":"object","properties":{"key":{"type":"string"},"value":{"type":"string"}},"required":["key","value"]})"},
    {"assistant",
     "Kestral's own built in assistant: action=read (its chat, last lines), send (a message to it, it then works on its own), stop, "
     "open or close (the panel). With send, images (base64 PNG or JPG) go along as pictures, like pasting them into its chat.",
     R"({"type":"object","properties":{"action":{"type":"string","enum":["read","send","stop","open","close"]},"text":{"type":"string"},"last":{"type":"number"},"images":{"type":"array","items":{"type":"string"}}},"required":["action"]})"},
    {"get_log", "The last lines of Kestral's log (errors, warnings, what it did). lines: how many (default 80).",
     R"({"type":"object","properties":{"lines":{"type":"number"}}})"},
    {"window",
     "Kestral's main window: action=focus (bring it to the front), maximize, restore, size (width and height in pixels), show or hide "
     "it, or close (quits Kestral; refuses when there are unsaved changes unless discard_unsaved=true).",
     R"({"type":"object","properties":{"action":{"type":"string","enum":["focus","maximize","restore","size","show","hide","close"]},"width":{"type":"number"},"height":{"type":"number"},"discard_unsaved":{"type":"boolean"}},"required":["action"]})"},
    {"wait_for",
     "Wait until something happens instead of asking again and again: until=idle (the design finished rebuilding and no background "
     "job runs), design_changed (anything in the design changed, also by the user), assistant_done (Kestral's own assistant finished), "
     "printer_changed (the printer's state changed, e.g. a print finished). timeout_s: how long to wait at most (default 120, up to 3600).",
     R"({"type":"object","properties":{"until":{"type":"string","enum":["idle","design_changed","assistant_done","printer_changed"]},"timeout_s":{"type":"number"}},"required":["until"]})"},
    {"ui_find",
     "Find buttons, menus, menu items, checkboxes, fields, tabs and other controls on screen by their name (what they say, or the "
     "label next to them). Leave text empty to list everything with a name, or give window to look in one window or popup. Each "
     "match comes with its window, kind, state (checked, open, disabled) and its center in screenshot pixels.",
     R"({"type":"object","properties":{"text":{"type":"string"},"window":{"type":"string"},"limit":{"type":"number"}}})"},
    {"ui_click",
     "Click a control by its name, like a person would: a button, menu, menu item, tab, checkbox, list row, toolbar command. text is "
     "what it says (or the label next to it); window narrows it to one window or popup; index picks among several matches (0 first). "
     "Menus: click the menu, then the item (two calls, or one ui_input with two click actions using target).",
     R"({"type":"object","properties":{"text":{"type":"string"},"window":{"type":"string"},"index":{"type":"number"},"button":{"type":"string","enum":["left","right","middle"]},"double":{"type":"boolean"},"screenshot":{"type":"boolean"}},"required":["text"]})"},
    {"ui_set",
     "Set a control by its name: a field or slider gets the value typed in (then Enter), a checkbox is turned on or off "
     "(value true or false), a drop down list picks the entry named value.",
     R"({"type":"object","properties":{"text":{"type":"string"},"value":{"type":"string"},"window":{"type":"string"},"index":{"type":"number"},"screenshot":{"type":"boolean"}},"required":["text","value"]})"},
};

std::string lowerS(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool secretKey(const std::string& k) {
    std::string l = lowerS(k);
    for (const char* w : {"key", "token", "secret", "password", "pass", "code", "cookie", "auth"})
        if (l.find(w) != std::string::npos) return true;
    return false;
}

// "ctrl+shift+s" -> modifiers and a key. False when the name is unknown.
bool parseKey(const std::string& spec, std::vector<ImGuiKey>& mods, ImGuiKey& key) {
    std::string s = lowerS(spec);
    key = ImGuiKey_None;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find('+', p);
        if (e == p && e != std::string::npos) { p++; continue; }
        std::string part = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (e == std::string::npos && part.empty() && !s.empty() && s.back() == '+') part = "+";
        p = e == std::string::npos ? s.size() + 1 : e + 1;
        if (part == "ctrl" || part == "control" || part == "cmd") { mods.push_back(ImGuiMod_Ctrl); continue; }
        if (part == "shift") { mods.push_back(ImGuiMod_Shift); continue; }
        if (part == "alt" || part == "option") { mods.push_back(ImGuiMod_Alt); continue; }
        if (part == "super" || part == "win" || part == "meta") { mods.push_back(ImGuiMod_Super); continue; }
        if (part.size() == 1 && part[0] >= 'a' && part[0] <= 'z') { key = (ImGuiKey)(ImGuiKey_A + (part[0] - 'a')); continue; }
        if (part.size() == 1 && part[0] >= '0' && part[0] <= '9') { key = (ImGuiKey)(ImGuiKey_0 + (part[0] - '0')); continue; }
        if (part.size() >= 2 && part[0] == 'f' && std::isdigit((unsigned char)part[1])) {
            int n = std::atoi(part.c_str() + 1);
            if (n >= 1 && n <= 12) { key = (ImGuiKey)(ImGuiKey_F1 + (n - 1)); continue; }
        }
        static const std::pair<const char*, ImGuiKey> named[] = {
            {"enter", ImGuiKey_Enter}, {"return", ImGuiKey_Enter}, {"escape", ImGuiKey_Escape}, {"esc", ImGuiKey_Escape},
            {"tab", ImGuiKey_Tab}, {"backspace", ImGuiKey_Backspace}, {"delete", ImGuiKey_Delete}, {"del", ImGuiKey_Delete},
            {"space", ImGuiKey_Space}, {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
            {"right", ImGuiKey_RightArrow}, {"home", ImGuiKey_Home}, {"end", ImGuiKey_End}, {"pageup", ImGuiKey_PageUp},
            {"pagedown", ImGuiKey_PageDown}, {"insert", ImGuiKey_Insert}, {"minus", ImGuiKey_Minus}, {"-", ImGuiKey_Minus},
            {"equal", ImGuiKey_Equal}, {"=", ImGuiKey_Equal}, {"comma", ImGuiKey_Comma}, {",", ImGuiKey_Comma},
            {"period", ImGuiKey_Period}, {".", ImGuiKey_Period}, {"slash", ImGuiKey_Slash}, {"/", ImGuiKey_Slash},
        };
        bool found = false;
        for (auto& [n, k] : named) if (part == n) { key = k; found = true; break; }
        if (!found) return false;
    }
    return key != ImGuiKey_None || !mods.empty();
}

std::string base64Decode(const std::string& in) {
    static int8_t map[256];
    static bool init = [] {
        for (auto& m : map) m = -1;
        const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) map[(unsigned char)t[i]] = (int8_t)i;
        map[(unsigned char)'-'] = 62;
        map[(unsigned char)'_'] = 63;
        return true;
    }();
    (void)init;
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (map[c] < 0) continue;   // padding, spaces, newlines
        acc = (acc << 6) | (uint32_t)map[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((acc >> bits) & 0xFF));
        }
    }
    return out;
}

bool decodeImage(const std::string& bin, std::vector<uint8_t>& rgba, int& w, int& h) {
    int n = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)bin.data(), (int)bin.size(), &w, &h, &n, 4);
    if (!px) return false;
    rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

ImGuiKey modKey(ImGuiKey m) {   // the left hand key that goes with a modifier
    return m == ImGuiMod_Ctrl ? ImGuiKey_LeftCtrl : m == ImGuiMod_Shift ? ImGuiKey_LeftShift : m == ImGuiMod_Alt ? ImGuiKey_LeftAlt : ImGuiKey_LeftSuper;
}

std::string stripId(const char* name) {
    std::string n = name ? name : "";
    size_t h = n.find("##");
    if (h == 0) {   // a window with only an id: its id, readable
        n = n.substr(n.find_first_not_of('#'));
        for (auto& c : n) if (c == '_') c = ' ';
        return n;
    }
    if (h != std::string::npos) n.erase(h);
    return n;
}

} // namespace

json::Value App::mcpUiToolList() const {
    Value arr = Value::array();
    for (auto& t : kUiTools) {
        Value o = Value::object();
        o.set("name", t.name);
        o.set("description", t.desc);
        o.set("inputSchema", Value::parse(t.schema));
        arr.push(o);
    }
    return arr;
}

bool App::mcpIsUiTool(const std::string& name) {
    for (auto& t : kUiTools) if (name == t.name) return true;
    return false;
}

// ---------------------------------------------------------------- input, every frame (main thread)

void App::mcpFeedInput() {
    ImGuiIO& io = ImGui::GetIO();
    double now = glfwGetTime();
    float fs = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.f;
    // The backend reads the real cursor when it is outside the window; while MCP is using the
    // mouse its position wins.
    if (now < mcpHoldUntil_) io.AddMousePosEvent(mcpCurX_ / fs, mcpCurY_ / fs);
    if (mcpInput_.empty()) return;
    redrawFrames_ = std::max(redrawFrames_, 3);
    while (!mcpInput_.empty()) {
        McpInput& e = mcpInput_.front();
        if (e.kind == 5) {   // wait frames
            if (e.frames-- > 0) return;
            mcpInput_.pop_front();
            continue;
        }
        if (e.kind == 6) {   // wait: until holds the length until it is reached
            if (e.frames == 0) { e.until += now; e.frames = 1; }
            if (now < e.until) return;
            mcpInput_.pop_front();
            continue;
        }
        switch (e.kind) {
        case 0:
            mcpCurX_ = e.x;
            mcpCurY_ = e.y;
            io.AddMousePosEvent(e.x / fs, e.y / fs);
            break;
        case 1: io.AddMouseButtonEvent(e.button, e.down); break;
        case 2: io.AddMouseWheelEvent(0.f, e.wheel); break;
        case 3: io.AddInputCharactersUTF8(e.text.c_str()); break;
        case 4: io.AddKeyEvent((ImGuiKey)e.key, e.down); break;
        default: break;
        }
        mcpCursorT_ = now;
        mcpHoldUntil_ = now + 3.0;
        mcpInput_.pop_front();
    }
}

void App::mcpDrawCursor() {
    // A visible pointer where MCP's mouse is, so the user (and a screen recording) can follow it.
    double now = glfwGetTime();
    if (now - mcpCursorT_ > 4.0 || mcpCurX_ < 0) return;
    ImGuiIO& io = ImGui::GetIO();
    float fs = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.f;
    float a = (float)std::clamp(1.0 - (now - mcpCursorT_ - 3.0), 0.0, 1.0);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 p(mcpCurX_ / fs, mcpCurY_ / fs);
    float s = 1.15f * uiScale_;
    ImVec2 pts[] = {{p.x, p.y}, {p.x, p.y + 18 * s}, {p.x + 4.5f * s, p.y + 14 * s}, {p.x + 8 * s, p.y + 21 * s},
                    {p.x + 10.5f * s, p.y + 20 * s}, {p.x + 7.2f * s, p.y + 13 * s}, {p.x + 13 * s, p.y + 13 * s}};
    ImU32 fill = IM_COL32(242, 110, 46, (int)(255 * a)), edge = IM_COL32(255, 255, 255, (int)(255 * a));
    dl->AddConcavePolyFilled(pts, 7, fill);
    dl->AddPolyline(pts, 7, edge, ImDrawFlags_Closed, 1.4f * uiScale_);
    if (ImGui::GetIO().MouseDown[0] || ImGui::GetIO().MouseDown[1] || ImGui::GetIO().MouseDown[2])
        dl->AddCircle(p, 11 * s, IM_COL32(242, 110, 46, (int)(200 * a)), 24, 2.f * uiScale_);
    if (mcpInput_.empty() && now - mcpCursorT_ < 4.0) redrawFrames_ = std::max(redrawFrames_, 2);   // let it fade
}

// ---------------------------------------------------------------- the tools (MCP connection thread)

App::McpCall App::mcpRunUiTool(const std::string& client, const std::string& name, const Value& a,
                               const std::shared_ptr<std::atomic<bool>>& cancel) {
    (void)client;
    PostFn post = [this](std::function<void()> fn) {
        jobs_->post(std::move(fn));
        glfwPostEmptyEvent();
    };
    // Runs fn on the main thread and waits for its answer.
    auto onMain = [&](auto fn) -> decltype(fn()) {
        using R = decltype(fn());
        auto p = std::make_shared<std::promise<R>>();
        auto f = p->get_future();
        post([p, fn]() mutable {
            try { p->set_value(fn()); } catch (...) { p->set_exception(std::current_exception()); }
        });
        while (!cancel->load())
            if (f.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready) return f.get();
        return R{};
    };
    auto waitInput = [&] {
        for (int i = 0; i < 3000 && !cancel->load(); ++i) {   // up to a minute
            if (onMain([this] { return mcpInput_.empty(); })) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        // A few more frames so what the input did is drawn.
        onMain([this] { mcpInput_.push_back({5, 0, 0, 0, false, 0, "", 0, 0, 3}); return true; });
        for (int i = 0; i < 400 && !cancel->load(); ++i) {
            if (onMain([this] { return mcpInput_.empty(); })) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    };
    McpCall out;
    auto shot = [&]() -> bool {
        bool ok = onMain([this] {
            if (!renderer_.canCapture()) return false;
            renderer_.requestCapture();
            redrawFrames_ = std::max(redrawFrames_, 3);
            return true;
        });
        if (!ok) { out.text += "This graphics driver cannot copy the window, so there is no screenshot."; return false; }
        struct Px { std::vector<uint8_t> rgba; int w = 0, h = 0; };
        Px px;
        for (int i = 0; i < 300 && !cancel->load(); ++i) {   // a few seconds at most
            px = onMain([this] {
                Px r;
                if (!renderer_.takeCapture(r.rgba, r.w, r.h)) { renderer_.requestCapture(); redrawFrames_ = std::max(redrawFrames_, 2); }
                return r;
            });
            if (!px.rgba.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        if (px.rgba.empty()) { out.text += "The window did not draw a frame for the screenshot (is it minimized?)."; return false; }
        AiImageBuilt b = aiBuildImage(px.rgba.data(), px.w, px.h);
        float scale = b.img->w > 0 ? (float)px.w / b.img->w : 1.f;
        mcpShotScale_.store(scale);
        char buf[200];
        std::snprintf(buf, sizeof buf, "Screenshot of the Kestral window (%d x %d pixels, shown at %d x %d). ", px.w, px.h, b.img->w, b.img->h);
        out.text += buf;
        out.images.push_back(b.img);
        return true;
    };

    if (name == "screenshot") {
        shot();
        out.error = out.images.empty();
        return out;
    }

    // Controls by name, from the last frame Kestral drew. Window pixels.
    struct Found { bool ok = false, scrolled = false; float x = 0, y = 0; uiitems::Item item; int matches = 0; std::string why; };
    auto findOnce = [&](const std::string& text, const std::string& win, int index) {
        return onMain([this, text, win, index] {
            Found f;
            std::string t = lowerS(text), w = lowerS(win);
            struct C { int score; size_t order; const uiitems::Item* it; };
            std::vector<C> cands;
            const auto& items = uiitems::last();
            for (size_t i = 0; i < items.size(); ++i) {
                const auto& it = items[i];
                if (!it.visible || it.label.empty()) continue;
                if (!w.empty() && lowerS(it.window).find(w) == std::string::npos) continue;
                std::string l = lowerS(it.label);
                int sc = l == t ? 3 : l.rfind(t, 0) == 0 ? 2 : l.find(t) != std::string::npos ? 1 : 0;
                if (sc == 0) continue;
                cands.push_back({sc, i, &it});
            }
            if (cands.empty()) {
                // Out of view in a scrolling page? Scroll it in and look again.
                for (size_t i = items.size(); i-- > 0;) {
                    const auto& it = items[i];
                    if (it.visible || it.label.empty()) continue;
                    if (!w.empty() && lowerS(it.window).find(w) == std::string::npos) continue;
                    std::string l = lowerS(it.label);
                    if (l != t && l.find(t) == std::string::npos) continue;
                    if (uiitems::scrollTo(it)) { f.scrolled = true; redrawFrames_ = std::max(redrawFrames_, 4); return f; }
                }
                f.why = "Nothing on screen is called \"" + text + "\"" + (win.empty() ? "" : " in " + win) + ". ui_find lists what there is (or take a screenshot).";
                return f;
            }
            int best = 0;
            for (auto& c : cands) best = std::max(best, c.score);
            std::vector<C> top;
            for (auto& c : cands) if (c.score == best) top.push_back(c);
            // Drawn later is in front (popups and dialogs come after the windows below them).
            std::sort(top.begin(), top.end(), [](const C& a, const C& b) { return a.order > b.order; });
            f.matches = (int)top.size();
            int k = index >= 0 ? index : 0;
            if (k >= (int)top.size()) { f.why = "Only " + std::to_string(top.size()) + " controls match \"" + text + "\"."; return f; }
            f.item = *top[(size_t)k].it;
            float fs = ImGui::GetIO().DisplayFramebufferScale.x > 0 ? ImGui::GetIO().DisplayFramebufferScale.x : 1.f;
            f.x = (f.item.min.x + f.item.max.x) * 0.5f * fs;
            f.y = (f.item.min.y + f.item.max.y) * 0.5f * fs;
            f.ok = true;
            return f;
        });
    };
    auto findTarget = [&](const std::string& text, const std::string& win, int index) {
        Found f = findOnce(text, win, index);
        for (int tries = 0; f.scrolled && tries < 3 && !cancel->load(); ++tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));   // a few frames to draw it in view
            onMain([this] { redrawFrames_ = std::max(redrawFrames_, 3); return true; });
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            f = findOnce(text, win, index);
        }
        if (f.scrolled && !f.ok) f.why = "Found \"" + text + "\" but could not scroll it into view.";
        return f;
    };
    auto describe = [&](const uiitems::Item& it, float sc) {
        float fs = 1.f;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.0f, %.0f", (it.min.x + it.max.x) * 0.5f * fs / sc, (it.min.y + it.max.y) * 0.5f * fs / sc);
        std::string kind = it.input ? "field" : it.menuItem && !it.openable ? "menu item" : it.openable ? "menu or tree" : it.checkable ? "checkbox" : "button";
        std::string st;
        if (it.checkable && (!it.menuItem || it.checked)) st += it.checked ? ", on" : ", off";
        if (it.openable) st += it.opened ? ", open" : ", closed";
        if (it.disabled) st += ", disabled";
        return "\"" + it.label + "\" (" + kind + st + ") in " + (it.window.empty() ? std::string("the main window") : it.window) + " at " + buf;
    };

    if (name == "ui_find") {
        std::string t = lowerS(a["text"].asString()), w = lowerS(a["window"].asString());
        int limit = std::clamp((int)a["limit"].asNumber(150), 1, 1000);
        float sc = mcpShotScale_.load();
        out.text = onMain([this, t, w, limit, sc, &describe] {
            std::string r;
            int n = 0, total = 0;
            float fs = ImGui::GetIO().DisplayFramebufferScale.x > 0 ? ImGui::GetIO().DisplayFramebufferScale.x : 1.f;
            const auto& items = uiitems::last();
            for (size_t i = items.size(); i-- > 0;) {   // front most first
                uiitems::Item it = items[i];
                if (!it.visible || it.label.empty()) continue;
                if (!w.empty() && lowerS(it.window).find(w) == std::string::npos) continue;
                if (!t.empty() && lowerS(it.label).find(t) == std::string::npos) continue;
                ++total;
                if (n >= limit) continue;
                it.min.x *= fs; it.min.y *= fs; it.max.x *= fs; it.max.y *= fs;
                r += describe(it, sc) + "\n";
                ++n;
            }
            if (total == 0) return std::string("No control on screen matches. Take a screenshot to see what is there.");
            if (total > n) r += "(" + std::to_string(total - n) + " more; narrow it with text or window)\n";
            return "Front most first, centers in screenshot pixels:\n" + r;
        });
        return out;
    }

    Value synth;
    if (name == "ui_click" || name == "ui_set") {
        std::string text = a["text"].asString(), win = a["window"].asString();
        int index = (int)a["index"].asNumber(-1);
        Found f = findTarget(text, win, index);
        if (!f.ok) { out.text = "Error: " + f.why; out.error = true; return out; }
        Value acts = Value::array();
        auto click = [&](const std::string& target, const std::string& w, int idx, bool ctrl) {
            Value c = Value::object();
            c.set("action", "click");
            c.set("target", target);
            if (!w.empty()) c.set("window", w);
            if (idx >= 0) c.set("index", idx);
            if (ctrl) c.set("keys", "ctrl");
            if (a["button"].isString()) c.set("button", a["button"]);
            if (a["double"].isBool()) c.set("double", a["double"]);
            acts.push(c);
        };
        std::string note;
        if (name == "ui_click") {
            click(text, win, index, false);
            note = "Clicked \"" + f.item.label + "\"" + (f.item.window.empty() ? std::string() : " in " + f.item.window) + ".";
        } else {
            std::string v = a["value"].asString(), lv = lowerS(v);
            bool want = lv == "true" || lv == "on" || lv == "1" || lv == "yes";
            if (f.item.checkable) {
                if (f.item.checked != want) click(text, win, index, false);
                note = "\"" + f.item.label + "\" is now " + (want ? "on." : "off.");
            } else if (f.item.input) {
                click(text, win, index, true);   // ctrl+click: sliders and drags take typed text too
                Value k = Value::object(); k.set("action", "key"); k.set("key", "ctrl+a"); acts.push(k);
                Value ty = Value::object(); ty.set("action", "type"); ty.set("text", v); acts.push(ty);
                Value en = Value::object(); en.set("action", "key"); en.set("key", "enter"); acts.push(en);
                note = "Typed " + v + " into \"" + f.item.label + "\".";
            } else {   // a drop down list: open it, pick the entry
                click(text, win, index, false);
                Value w2 = Value::object(); w2.set("action", "wait"); w2.set("ms", 250); acts.push(w2);
                click(v, "", -1, false);
                note = "Picked " + v + " in \"" + f.item.label + "\".";
            }
        }
        synth = Value::object();
        synth.set("actions", acts);
        synth.set("screenshot", a["screenshot"].asBool(false));
        McpCall r = mcpRunUiTool(client, "ui_input", synth, cancel);
        r.text = note + (f.matches > 1 && index < 0 ? " (" + std::to_string(f.matches) + " controls had that name: the front most was used; index picks another.)" : std::string()) +
                 " " + r.text;
        return r;
    }

    if (name == "ui_input") {
        bool winSpace = a["space"].asString() == "window";
        float sc = winSpace ? 1.f : mcpShotScale_.load();
        bool smooth = a["smooth"].asBool(true);
        std::vector<McpInput> steps;
        std::string bad;
        float cx = -1, cy = -1;
        auto frame = [&](int n = 1) { McpInput w{}; w.kind = 5; w.frames = n; steps.push_back(w); };
        auto moveTo = [&](float x, float y, int glide) {
            if (cx < 0 || !smooth) glide = 0;
            for (int i = 1; i <= glide; ++i) {
                float t = (float)i / (glide + 1);
                t = t * t * (3 - 2 * t);
                McpInput m{};
                m.kind = 0;
                m.x = cx + (x - cx) * t;
                m.y = cy + (y - cy) * t;
                steps.push_back(m);
                frame();
            }
            McpInput m{};
            m.kind = 0;
            m.x = x;
            m.y = y;
            steps.push_back(m);
            frame();
            cx = x;
            cy = y;
        };
        float startX = -1, startY = -1;
        onMain([this, &startX, &startY] {
            startX = mcpCurX_;
            startY = mcpCurY_;
            return true;
        });
        cx = startX;
        cy = startY;
        auto btnOf = [](const std::string& b) { return b == "right" ? 1 : b == "middle" ? 2 : 0; };
        auto keyStep = [&](ImGuiKey k, bool down) { McpInput e{}; e.kind = 4; e.key = (int)k; e.down = down; steps.push_back(e); };
        auto flush = [&] {
            if (steps.empty()) return;
            onMain([this, steps]() mutable {
                for (auto& s : steps) mcpInput_.push_back(s);
                redrawFrames_ = std::max(redrawFrames_, 3);
                return true;
            });
            steps.clear();
            waitInput();
        };
        for (auto& act : a["actions"].items()) {
            std::string kind = lowerS(act["action"].asString());
            float x = (float)act["x"].asNumber(-1) * sc, y = (float)act["y"].asNumber(-1) * sc;
            if (act["target"].isString()) {
                // Earlier actions first (a menu has to be open before its item can be found).
                flush();
                Found f = findTarget(act["target"].asString(), act["window"].asString(), (int)act["index"].asNumber(-1));
                if (!f.ok) { bad += " " + f.why; break; }
                x = f.x;
                y = f.y;
            }
            if (kind == "move") {
                moveTo(x, y, 10);
            } else if (kind == "click") {
                moveTo(x, y, 10);
                std::vector<ImGuiKey> held;
                ImGuiKey none;
                if (!act["keys"].asString().empty()) parseKey(act["keys"].asString(), held, none);
                for (auto m : held) { keyStep(m, true); keyStep(modKey(m), true); }
                if (!held.empty()) frame();
                int b = btnOf(lowerS(act["button"].asString()));
                int times = act["double"].asBool(false) ? 2 : 1;
                for (int i = 0; i < times; ++i) {
                    McpInput d{}; d.kind = 1; d.button = b; d.down = true; steps.push_back(d); frame();
                    McpInput u{}; u.kind = 1; u.button = b; u.down = false; steps.push_back(u); frame();
                }
                for (auto m : held) { keyStep(modKey(m), false); keyStep(m, false); }
                frame(2);
            } else if (kind == "drag") {
                float tx = (float)act["to_x"].asNumber(-1) * sc, ty = (float)act["to_y"].asNumber(-1) * sc;
                int b = btnOf(lowerS(act["button"].asString()));
                std::vector<ImGuiKey> mods;
                ImGuiKey dummy;
                if (!act["keys"].asString().empty()) parseKey(act["keys"].asString(), mods, dummy);
                moveTo(x, y, 10);
                for (auto m : mods) { keyStep(m, true); keyStep(modKey(m), true); }
                frame();
                McpInput d{}; d.kind = 1; d.button = b; d.down = true; steps.push_back(d); frame(2);
                int n = smooth ? 24 : 4;
                for (int i = 1; i <= n; ++i) {
                    float t = (float)i / n;
                    McpInput m{};
                    m.kind = 0;
                    m.x = x + (tx - x) * t;
                    m.y = y + (ty - y) * t;
                    steps.push_back(m);
                    frame();
                }
                cx = tx;
                cy = ty;
                McpInput u{}; u.kind = 1; u.button = b; u.down = false; steps.push_back(u); frame();
                for (auto m : mods) { keyStep(modKey(m), false); keyStep(m, false); }
                frame(2);
            } else if (kind == "scroll") {
                if (x >= 0 && y >= 0) moveTo(x, y, 6);
                McpInput w{}; w.kind = 2; w.wheel = (float)act["amount"].asNumber(1); steps.push_back(w); frame(2);
            } else if (kind == "type") {
                McpInput t{}; t.kind = 3; t.text = act["text"].asString(); steps.push_back(t); frame(2);
            } else if (kind == "key") {
                std::vector<ImGuiKey> mods;
                ImGuiKey k;
                if (!parseKey(act["key"].asString(), mods, k)) { bad += " unknown key \"" + act["key"].asString() + "\";"; continue; }
                for (auto m : mods) { keyStep(m, true); keyStep(modKey(m), true); }
                if (k != ImGuiKey_None) { keyStep(k, true); frame(); keyStep(k, false); }
                for (auto m : mods) { keyStep(modKey(m), false); keyStep(m, false); }
                frame(2);
            } else if (kind == "wait") {
                McpInput w{}; w.kind = 6; w.until = std::clamp(act["ms"].asNumber(500), 0.0, 30000.0) / 1000.0;   // made absolute when reached
                steps.push_back(w);
            } else {
                bad += " unknown action \"" + kind + "\";";
            }
        }
        flush();
        out.text = "Done (" + std::to_string(a["actions"].size()) + " actions)." + (bad.empty() ? std::string() : " Skipped:" + bad) + " ";
        if (a["screenshot"].asBool(true)) shot();
        return out;
    }

    if (name == "get_ui_state") {
        float sc = mcpShotScale_.load();
        out.text = onMain([this, sc] {
            Value s = Value::object();
            s.set("workspace", sliceWs_ ? "slice" : "design");
            Value tabs = Value::array();
            for (size_t i = 0; i < designTabs_.size(); ++i) {
                Value t = Value::object();
                t.set("index", (int)i);
                bool act = (int)i == designActive_;
                t.set("name", act ? doc_.name : designTabs_[i].doc.name);
                t.set("active", act);
                t.set("unsaved", act ? dirty_ : designTabs_[i].dirty);
                tabs.push(t);
            }
            s.set("design_tabs", tabs);
            s.set("design_file", doc_.path);
            s.set("sketch_mode", sk_.active);
            s.set("feature_dialog_open", dlg_.open);
            s.set("tool_dialog_open", tdlg_.open);
            s.set("choosing_sketch_plane", sk_.choosingPlane);
            s.set("selected_bodies", (int)selection_.size());
            s.set("preferences_open", settingsOpen_);
            s.set("data_panel_open", dataOpen_);
            s.set("assistant_open", ai_.open);
            s.set("assistant_busy", ai_.busy);
            s.set("timeline_steps", (int)doc_.timeline.size());
            Value msgs = Value::array();
            for (auto& t : toasts_) msgs.push(t.msg);
            s.set("messages_on_screen", msgs);
            int fw = 0, fh = 0;
            glfwGetFramebufferSize(window_, &fw, &fh);
            s.set("window_pixels", std::to_string(fw) + " x " + std::to_string(fh));
            s.set("screenshot_scale", sc);
            s.set("focused", glfwGetWindowAttrib(window_, GLFW_FOCUSED) != 0);
            // Every window and popup on screen (Dear ImGui's), front most last, in screenshot pixels.
            Value wins = Value::array();
            ImGuiContext& g = *GImGui;
            float fs = ImGui::GetIO().DisplayFramebufferScale.x > 0 ? ImGui::GetIO().DisplayFramebufferScale.x : 1.f;
            for (ImGuiWindow* w : g.Windows) {
                if (!w || !(w->Active || w->WasActive) || w->Hidden || (w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
                std::string n = stripId(w->Name);
                if (n.empty() || n.rfind("Debug", 0) == 0 || n.rfind("Fallback", 0) == 0) continue;
                Value o = Value::object();
                o.set("title", n);
                if (w->Flags & ImGuiWindowFlags_Popup) o.set("popup", true);
                char r[96];
                std::snprintf(r, sizeof r, "x %.0f, y %.0f, w %.0f, h %.0f", w->Pos.x * fs / sc, w->Pos.y * fs / sc, w->Size.x * fs / sc, w->Size.y * fs / sc);
                o.set("rect", r);
                wins.push(o);
            }
            s.set("windows", wins);
            return s.dump();
        });
        return out;
    }

    if (name == "list_commands") {
        std::string q = lowerS(a["query"].asString());
        out.text = onMain([this, q] {
            Value arr = Value::array();
            for (auto& t : tools_) {
                if (!q.empty() && lowerS(t.id + " " + t.label + " " + t.tip).find(q) == std::string::npos) continue;
                Value o = Value::object();
                o.set("id", t.id);
                o.set("label", t.label);
                if (!t.shortcut.empty()) o.set("shortcut", t.shortcut);
                if (!t.tip.empty()) o.set("tip", t.tip);
                if (!t.available()) o.set("available", false);
                arr.push(o);
            }
            return arr.dump();
        });
        return out;
    }

    if (name == "view") {
        out.text = onMain([this, a] {
            std::string v = lowerS(a["view"].asString());
            float asp = vk_.extent.height ? (float)vk_.extent.width / vk_.extent.height : 1.f;
            std::string did;
            if (v == "home") { cam_.home(sceneBounds_, asp); did += "home view; "; }
            else if (v == "fit") { fitView(false); did += "fit; "; }
            else if (v == "fit_selection") { fitView(true); did += "fit to the selection; "; }
            else if (!v.empty()) {
                glm::vec3 d(0.f);
                if (v == "front") d = {0, -1, 0};
                else if (v == "back") d = {0, 1, 0};
                else if (v == "right") d = {1, 0, 0};
                else if (v == "left") d = {-1, 0, 0};
                else if (v == "top") d = {0, 0, 1};
                else if (v == "bottom") d = {0, 0, -1};
                else if (v == "iso") d = glm::normalize(glm::vec3(1, -1, 0.8f));
                if (glm::length(d) > 0) { cam_.lookFrom(d); did += v + " view; "; }
                else did += "unknown view " + v + "; ";
            }
            if (a["ortho"].isBool()) {
                cam_.ortho = a["ortho"].asBool();
                settings_.perspective = !cam_.ortho;
                settings_.save(settingsPath_);
                did += cam_.ortho ? "orthographic; " : "perspective; ";
            }
            float ox = (float)a["orbit_x"].asNumber(0), oy = (float)a["orbit_y"].asNumber(0);
            if (ox != 0 || oy != 0) { cam_.orbit(ox, oy, settings_.constrainedOrbit); did += "orbited; "; }
            float z = (float)a["zoom"].asNumber(0);
            if (z != 0) { cam_.zoomAt(z, glm::vec2(0.f), asp); did += "zoomed; "; }
            activity();
            return did.empty() ? std::string("Nothing to do: give view, ortho, orbit_x / orbit_y or zoom.") : "Camera: " + did;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(350));   // the camera glides there
        return out;
    }

    if (name == "undo" || name == "redo") {
        int n = std::clamp((int)a["steps"].asNumber(1), 1, 200);
        bool u = name == "undo";
        out.text = onMain([this, n, u] {
            for (int i = 0; i < n; ++i) u ? undo() : redo();
            activity();
            return std::string(u ? "Undid " : "Redid ") + std::to_string(n) + (n == 1 ? " step." : " steps.") +
                   " Timeline now has " + std::to_string(doc_.timeline.size()) + " steps.";
        });
        return out;
    }

    if (name == "design_tabs") {
        std::string act = lowerS(a["action"].asString());
        int idx = (int)a["index"].asNumber(-1);
        out.text = onMain([this, act, idx] {
            if (act == "switch") {
                if (idx < 0 || idx >= (int)designTabs_.size()) return std::string("Error: no tab ") + std::to_string(idx) + ".";
                switchDesignTab(idx);
            } else if (act == "new") {
                newDesignTab();
            } else if (act == "close") {
                if (idx < 0 || idx >= (int)designTabs_.size()) return std::string("Error: no tab ") + std::to_string(idx) + ".";
                closeDesignTab(idx);
            }
            activity();
            Value tabs = Value::array();
            for (size_t i = 0; i < designTabs_.size(); ++i) {
                bool on = (int)i == designActive_;
                Value t = Value::object();
                t.set("index", (int)i);
                t.set("name", on ? doc_.name : designTabs_[i].doc.name);
                t.set("active", on);
                tabs.push(t);
            }
            return tabs.dump();
        });
        if (act == "switch" || act == "new") std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return out;
    }

    if (name == "workspace") {
        std::string to = lowerS(a["to"].asString()), page = lowerS(a["page"].asString());
        out.text = onMain([this, to, page] {
            if (to == "design") {
                if (sliceWs_) leaveSliceWorkspace();
                activity();
                return std::string("In the Design workspace.");
            }
            enterSliceWorkspace();
            int p = page == "prepare" ? 1 : page == "preview" ? 2 : page == "device" ? 3 : page == "project" ? 4 : page == "calibration" ? 5 : -1;
            if (p > 0) setSliceTop(p);
            activity();
            return std::string("In the Slice tab") + (page.empty() ? "." : " (" + page + ").");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return out;
    }

    if (name == "get_settings" || name == "set_setting") {
        std::string key = a["key"].asString(), val = a["value"].asString(), q = lowerS(a["query"].asString());
        bool set = name == "set_setting";
        out.text = onMain([this, set, key, val, q] {
            std::string tmp = platform::appDataDir() + "/mcp_settings.tmp";
            if (set) {
                std::string lk = lowerS(key);
                if (key.empty() || key.find('=') != std::string::npos || key.find('\n') != std::string::npos || val.find('\n') != std::string::npos)
                    return std::string("Error: give one key and one value.");
                if (secretKey(key) || lk.rfind("mcp", 0) == 0 || lk == "aiautoprint" || lk == "recent")
                    return "Error: " + key + " can only be changed by the user in Preferences.";
                // Only keys that exist.
                settings_.save(tmp);
                std::ifstream f(u8path(tmp));
                std::string line;
                bool known = false;
                while (std::getline(f, line)) if (line.rfind(key + "=", 0) == 0) known = true;
                f.close();
                if (!known) { std::remove(tmp.c_str()); return "Error: no setting called " + key + ". get_settings lists them."; }
                {
                    std::ofstream o(u8path(tmp), std::ios::trunc);
                    o << key << "=" << val << "\n";
                }
                settings_.load(tmp);
                std::remove(tmp.c_str());
                settings_.save(settingsPath_);
                cam_.ortho = !settings_.perspective;
                ui::applyTheme(uiScale_, settings_.darkMode ? settings_.darkness : 0.f);
                if (lk == "uiscale" && settings_.uiScale > 0) applyUiScale(std::clamp(settings_.uiScale, 0.75f, 3.f));
                activity();
                return "Set " + key + " = " + val + ". Some settings show after reopening Preferences or restarting Kestral.";
            }
            settings_.save(tmp);
            std::ifstream f(u8path(tmp));
            std::string line, outS;
            while (std::getline(f, line)) {
                size_t e = line.find('=');
                if (e == std::string::npos) continue;
                std::string k = line.substr(0, e);
                if (secretKey(k) || lowerS(k) == "recent") continue;
                if (!q.empty() && lowerS(line).find(q) == std::string::npos) continue;
                outS += line + "\n";
            }
            f.close();
            std::remove(tmp.c_str());
            return outS.empty() ? std::string("No settings match.") : outS;
        });
        return out;
    }

    if (name == "assistant") {
        std::string act = lowerS(a["action"].asString()), text = a["text"].asString();
        int last = std::clamp((int)a["last"].asNumber(20), 1, 400);
        // Pictures come as base64: decoded here, attached on the main thread.
        struct Pic { std::vector<uint8_t> rgba; int w = 0, h = 0; };
        std::vector<Pic> pics;
        std::string picErr;
        for (auto& im : a["images"].items()) {
            std::string b64 = im.asString();
            size_t comma = b64.find(',');
            if (b64.rfind("data:", 0) == 0 && comma != std::string::npos) b64 = b64.substr(comma + 1);
            std::string bin = base64Decode(b64);
            Pic p;
            if (!decodeImage(bin, p.rgba, p.w, p.h)) { picErr += " a picture could not be read;"; continue; }
            pics.push_back(std::move(p));
        }
        out.text = onMain([this, act, text, last, pics] {
            if (act == "send") {
                if (text.empty() && pics.empty()) return std::string("Error: give text.");
                for (auto& p : pics) aiAttachImage(p.rgba.data(), p.w, p.h);
                ai_.open = true;
                if (ai_.busy) {
                    std::lock_guard<std::mutex> lk(aiSteerMx_);
                    aiSteer_.push_back({text, {}});
                    ai_.lines.push_back({0, text});
                    return std::string("The assistant is busy: added your message to what it is doing.");
                }
                aiSend(text);
                return std::string("Sent. Read its answer with action=read (assistant_busy in get_ui_state says when it is done).");
            }
            if (act == "stop") {
                if (ai_.cancel) ai_.cancel->store(true);
                return std::string("Stopped the assistant.");
            }
            if (act == "open" || act == "close") {
                ai_.open = act == "open";
                activity();
                return std::string(ai_.open ? "Opened" : "Closed") + " the assistant panel.";
            }
            static const char* kinds[] = {"you", "assistant", "action", "error", "thinking", "picture description", "picture"};
            std::string s;
            size_t from = ai_.lines.size() > (size_t)last ? ai_.lines.size() - (size_t)last : 0;
            for (size_t i = from; i < ai_.lines.size(); ++i) {
                auto& l = ai_.lines[i];
                s += std::string("[") + kinds[std::clamp(l.kind, 0, 6)] + "] " + l.text.substr(0, 4000) +
                     (l.images.empty() ? std::string() : " (" + std::to_string(l.images.size()) + " picture" + (l.images.size() > 1 ? "s)" : ")")) + "\n";
            }
            s += ai_.busy ? "(the assistant is still working)" : "(the assistant is idle)";
            return s;
        });
        if (!picErr.empty()) out.text += " (Skipped:" + picErr + ")";
        return out;
    }

    if (name == "wait_for") {
        std::string until = lowerS(a["until"].asString());
        double limit = std::clamp(a["timeout_s"].asNumber(120), 1.0, 3600.0);
        auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        auto idleNow = [this] {
            return recomputeQueuedFrom_ < 0 && !recomputeRunning_ && !recomputeResults_ && jobs_->active() == 0 && mcpInput_.empty();
        };
        if (until == "idle") {
            while (!cancel->load() && elapsed() < limit) {
                if (onMain(idleNow)) { out.text = "Idle: the design is rebuilt and nothing runs in the background."; return out; }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        } else if (until == "design_changed") {
            uint64_t rev0 = onMain([this] { return doc_.revision; });
            while (!cancel->load() && elapsed() < limit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                auto now = onMain([this] { return std::make_tuple(doc_.revision, doc_.timeline.size(), doc_.bodies.size()); });
                if (std::get<0>(now) != rev0) {
                    out.text = "The design changed. Timeline: " + std::to_string(std::get<1>(now)) + " steps, " + std::to_string(std::get<2>(now)) +
                               " bodies. get_design shows what is there now.";
                    return out;
                }
            }
        } else if (until == "assistant_done") {
            while (!cancel->load() && elapsed() < limit) {
                if (!onMain([this] { return ai_.busy; })) { out.text = "Kestral's assistant is idle. assistant with action=read shows what it said."; return out; }
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        } else if (until == "printer_changed") {
            PostFn post2 = [this](std::function<void()> fn) { jobs_->post(std::move(fn)); glfwPostEmptyEvent(); };
            std::string first = aiPrintTool("printer_status", "{}", post2, cancel);
            while (!cancel->load() && elapsed() < limit) {
                for (int i = 0; i < 50 && !cancel->load() && elapsed() < limit; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::string now = aiPrintTool("printer_status", "{}", post2, cancel);
                if (now != first) { out.text = "The printer changed. Before: " + first + "\nNow: " + now; return out; }
            }
            if (!cancel->load()) { out.text = "No change in " + std::to_string((int)limit) + " s. The printer: " + first; return out; }
        } else {
            out.text = "Error: until must be idle, design_changed, assistant_done or printer_changed.";
            out.error = true;
            return out;
        }
        out.text = cancel->load() ? "Stopped." : "Still waiting after " + std::to_string((int)limit) + " s (" + until + "): nothing happened yet.";
        return out;
    }

    if (name == "get_log") {
        int n = std::clamp((int)a["lines"].asNumber(80), 1, 2000);
        std::ifstream f(u8path(platform::appDataDir() + "/kestral.log"), std::ios::binary);
        std::deque<std::string> q;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            q.push_back(line);
            if ((int)q.size() > n) q.pop_front();
        }
        for (auto& l : q) out.text += l + "\n";
        if (out.text.empty()) out.text = "The log is empty.";
        return out;
    }

    if (name == "window") {
        std::string act = lowerS(a["action"].asString());
        int w = (int)a["width"].asNumber(0), h = (int)a["height"].asNumber(0);
        bool discard = a["discard_unsaved"].asBool(false);
        out.text = onMain([this, act, w, h, discard] {
            if (act == "focus") { glfwShowWindow(window_); glfwRestoreWindow(window_); glfwFocusWindow(window_); }
            else if (act == "maximize") glfwMaximizeWindow(window_);
            else if (act == "restore") glfwRestoreWindow(window_);
            else if (act == "show") { glfwShowWindow(window_); hiddenWindow_ = false; }
            else if (act == "hide") { glfwHideWindow(window_); hiddenWindow_ = true; }
            else if (act == "close") {
                if (dirty_ && !discard) return std::string("Error: the design has unsaved changes. Save it (save_design) or pass discard_unsaved=true.");
                if (discard) dirty_ = false;
                for (auto& t : designTabs_) if (discard) t.dirty = false;
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
                return std::string("Kestral is closing.");
            }
            else if (act == "size") {
                if (w < 640 || h < 400) return std::string("Error: give width and height (at least 640 x 400).");
                glfwRestoreWindow(window_);
                glfwSetWindowSize(window_, w, h);
            }
            activity();
            int fw = 0, fh = 0;
            glfwGetWindowSize(window_, &fw, &fh);
            return "Window is " + std::to_string(fw) + " x " + std::to_string(fh) + ".";
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return out;
    }

    out.text = "Unknown tool: " + name;
    out.error = true;
    return out;
}

} // namespace kst
