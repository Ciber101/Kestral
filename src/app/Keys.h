#pragma once
// Keyboard shortcuts as data, so every command can be rebound (Fusion's "Change Keyboard Shortcut").
#include <imgui.h>
#include <string>

namespace kst {

struct KeyCombo {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false, alt = false;
    bool valid() const { return key != ImGuiKey_None; }
    bool operator==(const KeyCombo& o) const { return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt; }
    bool operator!=(const KeyCombo& o) const { return !(*this == o); }
};

// "Ctrl+Shift+S", "F6", "Delete", "1". Unknown text gives an invalid combo.
KeyCombo parseCombo(const std::string& s);
std::string formatCombo(const KeyCombo& c);
// The first ordinary key (not a modifier, not a mouse button) pressed this frame, with the
// modifiers held right now. Invalid when nothing was pressed.
KeyCombo pressedCombo();

} // namespace kst
