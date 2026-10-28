#include "app/Keys.h"
#include <imgui_internal.h>
#include <cctype>
#include <vector>

namespace kst {

namespace {

bool isModifier(ImGuiKey k) {
    return k == ImGuiKey_LeftCtrl || k == ImGuiKey_RightCtrl || k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift ||
           k == ImGuiKey_LeftAlt || k == ImGuiKey_RightAlt || k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper ||
           k == ImGuiKey_ReservedForModCtrl || k == ImGuiKey_ReservedForModShift || k == ImGuiKey_ReservedForModAlt ||
           k == ImGuiKey_ReservedForModSuper;
}

bool isKeyboardKey(ImGuiKey k) {
    if (isModifier(k)) return false;
    if (k >= ImGuiKey_Keyboard_BEGIN && k < ImGuiKey_Keyboard_END) return true;
    return false;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

KeyCombo parseCombo(const std::string& text) {
    KeyCombo c;
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : text) {
        if (ch == '+' && !cur.empty()) { parts.push_back(trim(cur)); cur.clear(); }
        else cur += ch;
    }
    if (!trim(cur).empty()) parts.push_back(trim(cur));
    if (parts.empty()) return c;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string p = lower(parts[i]);
        if (p == "ctrl" || p == "control") c.ctrl = true;
        else if (p == "shift") c.shift = true;
        else if (p == "alt") c.alt = true;
        else return KeyCombo{};
    }
    std::string k = lower(parts.back());
    if (k == "del") k = "delete";
    if (k == "esc") k = "escape";
    for (int i = ImGuiKey_Keyboard_BEGIN; i < ImGuiKey_Keyboard_END; ++i) {
        ImGuiKey key = (ImGuiKey)i;
        if (isModifier(key)) continue;
        const char* nm = ImGui::GetKeyName(key);
        if (nm && *nm && lower(nm) == k) { c.key = key; return c; }
    }
    return KeyCombo{};
}

std::string formatCombo(const KeyCombo& c) {
    if (!c.valid()) return {};
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.alt) s += "Alt+";
    if (c.shift) s += "Shift+";
    const char* nm = ImGui::GetKeyName(c.key);
    s += nm ? nm : "?";
    return s;
}

KeyCombo pressedCombo() {
    ImGuiIO& io = ImGui::GetIO();
    for (int i = ImGuiKey_Keyboard_BEGIN; i < ImGuiKey_Keyboard_END; ++i) {
        ImGuiKey key = (ImGuiKey)i;
        if (!isKeyboardKey(key)) continue;
        if (ImGui::IsKeyPressed(key, false)) {
            KeyCombo c;
            c.key = key;
            c.ctrl = io.KeyCtrl;
            c.shift = io.KeyShift;
            c.alt = io.KeyAlt;
            return c;
        }
    }
    return KeyCombo{};
}

} // namespace kst
