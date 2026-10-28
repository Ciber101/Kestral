#pragma once
// Look of the Slice tab: OrcaSlicer's layout in Kestral's colors (orange where Orca uses teal).
#include "ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace kst::sl {

inline ImVec4 orange() { return ImVec4(1.f, 0.42f, 0.21f, 1.f); }
inline ImVec4 orangeHover() { return ImVec4(1.f, 0.52f, 0.3f, 1.f); }
inline ImVec4 orangeDark() { return ImVec4(0.84f, 0.33f, 0.14f, 1.f); }
inline ImU32 orangeU(int a = 255) { return IM_COL32(255, 107, 53, a); }
inline ImU32 orangeDarkU(int a = 255) { return IM_COL32(214, 84, 36, a); }
inline ImVec4 changed() { return ImVec4(0.98f, 0.55f, 0.2f, 1.f); }

inline bool orangeButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, orange());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, orangeHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, orangeDark());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// A pill shaped orange button (Orca's teal "Flushing volumes").
inline bool pillButton(const char* label, bool enabled = true) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.f, 2.f));
    ImGui::BeginDisabled(!enabled);
    bool r = orangeButton(label);
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    return r;
}

// Packs a 0..1 color as RGBA8 for ImGui.
inline ImU32 u32(float r, float g, float b, float a = 1.f) {
    return IM_COL32((int)std::lround(std::clamp(r, 0.f, 1.f) * 255), (int)std::lround(std::clamp(g, 0.f, 1.f) * 255),
                    (int)std::lround(std::clamp(b, 0.f, 1.f) * 255), (int)std::lround(std::clamp(a, 0.f, 1.f) * 255));
}
inline bool isDark(float r, float g, float b) { return r * 0.3f + g * 0.59f + b * 0.11f < 0.55f; }

// A numbered filament chip (Orca: the colored square with the filament number).
inline void filChip(ImDrawList* dl, ImVec2 p, float sz, float r, float g, float b, int number, float rounding = 3.f) {
    dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), u32(r, g, b), rounding);
    dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(0, 0, 0, 60), rounding);
    char t[8];
    std::snprintf(t, sizeof t, "%d", number);
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(p.x + (sz - ts.x) * 0.5f, p.y + (sz - ts.y) * 0.5f), isDark(r, g, b) ? IM_COL32_WHITE : IM_COL32(20, 20, 20, 255), t);
}

// Kestral's dropdowns: a framed button with a chevron, a rounded list with headers, rows with a
// check mark for the chosen one and a dim hint at the right, and a search box.
bool beginMenuCombo(const char* id, const char* preview, float popupWidth, bool frameless = false);
void endMenuCombo();
void menuHeader(const char* text);
bool menuRow(const char* label, bool selected, const char* hint = nullptr, ImU32 swatch = 0);
bool menuSearch(std::string& buf, const char* hint = "Search");

// Orca's 3D toolbar and side icons, drawn as thin line art.
enum class OIcon {
    AddObj, AddPlate, Orient, Arrange, SplitObj, SplitPart, VarLayer, Move, Rotate, Scale, LayFace, Cut, Support, Seam,
    Paint, Measure, Close, Lock, Unlock, PlateSet, ToFront, Printer, Filament, Process, Plus, Minus, Sync, Gear, Search,
    Save, Compare, Eye, EyeOff, Menu, Zoom, Home, Prepare, Preview, Device, Project, Calibrate, Bed, Pencil, Fan, Lamp,
    Nozzle, BedTemp, Chamber, Camera, Folder, Play, Pause, Stop, Wifi, Ams, Edit, Text, Layers, Fuzzy
};
void drawOIcon(ImDrawList* dl, ImVec2 center, float size, OIcon icon, ImU32 col);

} // namespace kst::sl
