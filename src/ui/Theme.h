#pragma once
#include <imgui.h>

namespace kst::ui {

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
};

// Clean CAD style theme. darkness 0 is the light theme (white panels, gray text, blue accent);
// anything above blends every neutral color toward a dark theme (1 = near black). Colored
// accents (blue, orange, axis colors) stay as they are. Safe to call again at any time.
void applyTheme(float scale, float darkness = 0.f);
Fonts loadFonts(float scale);
float darkness();

// A light theme color as it should look at the current darkness (neutrals flip, accents stay).
ImU32 th(ImU32 lightColor);
ImVec4 th(const ImVec4& lightColor);
// Same for icon ink: only grays flip, colored parts keep their color.
ImU32 thIcon(ImU32 lightColor);

namespace col {
// Set by applyTheme from the light palette.
inline ImU32 Accent = IM_COL32(43, 125, 233, 255);
inline ImU32 AccentSoft = IM_COL32(43, 125, 233, 40);
inline ImU32 Panel = IM_COL32(250, 251, 252, 255);
inline ImU32 PanelBorder = IM_COL32(210, 214, 220, 255);
inline ImU32 Text = IM_COL32(52, 58, 66, 255);
inline ImU32 TextDim = IM_COL32(120, 128, 138, 255);
inline ImU32 Hover = IM_COL32(225, 236, 250, 255);
inline ImU32 Error = IM_COL32(214, 64, 64, 255);
inline ImU32 AppBar = IM_COL32(236, 238, 241, 255);
inline ImU32 Print = IM_COL32(255, 107, 53, 255);
inline ImU32 Card = IM_COL32(255, 255, 255, 255);          // toolbar, document tab, toasts
inline ImU32 Selected = IM_COL32(214, 230, 252, 255);      // selected browser row
inline ImU32 Active = IM_COL32(200, 222, 250, 255);        // pressed / active tool button
inline ImU32 Divider = IM_COL32(226, 229, 234, 255);
inline ImU32 Chip = IM_COL32(232, 235, 240, 255);
inline ImU32 Timeline = IM_COL32(246, 247, 249, 255);
inline ImU32 Status = IM_COL32(90, 98, 110, 255);
inline ImU32 Success = IM_COL32(46, 160, 90, 255);
inline ImU32 Warning = IM_COL32(232, 163, 61, 255);
}

// Viewport background gradient and grid ink (0..1 RGB) for the current darkness.
void viewportColors(float top[3], float bottom[3], float grid[3]);

} // namespace kst::ui
