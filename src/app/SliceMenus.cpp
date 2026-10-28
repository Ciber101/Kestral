// Kestral's dropdown lists (see SliceStyle.h): the same look everywhere in the Slice tab.
#include "app/SliceStyle.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace kst::sl {

using namespace ui;

namespace {
int gPushedVars = 0, gPushedCols = 0;
}

bool beginMenuCombo(const char* id, const char* preview, float popupWidth, bool frameless) {
    const float S = ImGui::GetFontSize() / 15.f;
    // The button: the chosen value, a chevron at the right.
    float w = ImGui::CalcItemWidth();
    float h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##btn", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImGuiID popupId = ImGui::GetID("##list");
    bool open = ImGui::IsPopupOpen(popupId, 0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!frameless || hov || open) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov || open ? col::Hover : ImGui::GetColorU32(ImGuiCol_FrameBg), 5.f * S);
        if (open) dl->AddRect(p, ImVec2(p.x + w, p.y + h), orangeU(), 5.f * S, 0, 1.2f * S);
    }
    float chev = 8.f * S;
    ImVec2 cc(p.x + w - 12.f * S, p.y + h * 0.5f);
    ImU32 tc = ImGui::GetColorU32(ImGuiCol_Text);
    if (open) { dl->AddLine(ImVec2(cc.x - chev * 0.5f, cc.y + chev * 0.25f), ImVec2(cc.x, cc.y - chev * 0.25f), tc, 1.4f * S); dl->AddLine(ImVec2(cc.x, cc.y - chev * 0.25f), ImVec2(cc.x + chev * 0.5f, cc.y + chev * 0.25f), tc, 1.4f * S); }
    else { dl->AddLine(ImVec2(cc.x - chev * 0.5f, cc.y - chev * 0.25f), ImVec2(cc.x, cc.y + chev * 0.25f), tc, 1.4f * S); dl->AddLine(ImVec2(cc.x, cc.y + chev * 0.25f), ImVec2(cc.x + chev * 0.5f, cc.y - chev * 0.25f), tc, 1.4f * S); }
    ImVec4 clip(p.x + 6 * S, p.y, p.x + w - 24 * S, p.y + h);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 8 * S, p.y + (h - ImGui::GetFontSize()) * 0.5f), tc, preview, nullptr, 0.f, &clip);
    if (clicked) ImGui::OpenPopup(popupId);
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + h + 3 * S));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(popupWidth, w), 0), ImVec2(std::max(popupWidth, w), 460.f * S));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 9.f * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 6 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * S, 1 * S));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(col::Card));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::PanelBorder));
    bool shown = ImGui::BeginPopupEx(popupId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    if (!shown) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        ImGui::PopID();
        return false;
    }
    gPushedVars = 3;
    gPushedCols = 2;
    return true;
}

void endMenuCombo() {
    ImGui::EndPopup();
    ImGui::PopStyleColor(gPushedCols);
    ImGui::PopStyleVar(gPushedVars);
    ImGui::PopID();
}

void menuHeader(const char* text) {
    const float S = ImGui::GetFontSize() / 15.f;
    ImGui::Dummy(ImVec2(0, 3 * S));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetWindowFontScale(0.86f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 8 * S, p.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
    float th = ImGui::GetFontSize();
    ImGui::SetWindowFontScale(1.f);
    ImGui::Dummy(ImVec2(0, th + 3 * S));
}

bool menuRow(const char* label, bool selected, const char* hint, ImU32 swatch) {
    const float S = ImGui::GetFontSize() / 15.f;
    float w = ImGui::GetContentRegionAvail().x;
    float h = 28.f * S;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), orangeU(50), 6.f * S);
    else if (selected) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), orangeU(22), 6.f * S);
    float x = p.x + 8 * S;
    // A check mark for the chosen one.
    if (selected) {
        ImVec2 c(x + 6 * S, p.y + h * 0.5f);
        dl->AddLine(ImVec2(c.x - 5 * S, c.y), ImVec2(c.x - 1.5f * S, c.y + 4 * S), orangeU(), 2.f * S);
        dl->AddLine(ImVec2(c.x - 1.5f * S, c.y + 4 * S), ImVec2(c.x + 6 * S, c.y - 4.5f * S), orangeU(), 2.f * S);
    }
    x += 20 * S;
    if (swatch) {
        dl->AddRectFilled(ImVec2(x, p.y + h * 0.5f - 7 * S), ImVec2(x + 14 * S, p.y + h * 0.5f + 7 * S), swatch, 3 * S);
        x += 22 * S;
    }
    float hintW = hint && *hint ? ImGui::CalcTextSize(hint).x + 12 * S : 0.f;
    ImVec4 clip(x, p.y, p.x + w - hintW - 4 * S, p.y + h);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, p.y + (h - ImGui::GetFontSize()) * 0.5f),
                selected ? orangeU() : ImGui::GetColorU32(ImGuiCol_Text), label, nullptr, 0.f, &clip);
    if (hintW > 0) dl->AddText(ImVec2(p.x + w - hintW, p.y + (h - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    if (hov && ImGui::CalcTextSize(label).x > clip.z - clip.x) ImGui::SetTooltip("%s", label);
    if (clicked) ImGui::CloseCurrentPopup();
    return clicked;
}

bool menuSearch(std::string& buf, const char* hint) {
    const float S = ImGui::GetFontSize() / 15.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28 * S, 6 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f * S);
    ImGui::SetNextItemWidth(w);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool ch = ImGui::InputTextWithHint("##menusearch", hint, &buf);
    ImGui::PopStyleVar(2);
    float h = ImGui::GetItemRectSize().y;
    drawOIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + 14 * S, p.y + h * 0.5f), 13 * S, OIcon::Search, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::Dummy(ImVec2(0, 3 * S));
    return ch;
}

} // namespace kst::sl
