// Start page: what Kestral shows when it opens without a design. New, Open, Recover and the recent
// designs with their previews, on the dark studio background.
#include "app/App.h"
#include "platform/Platform.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>

namespace kst {

using namespace ui;

void App::drawStartPage() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top = 34.f * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##start", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 w0 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImU32 bgTop = darkness() > 0 ? IM_COL32(38, 40, 45, 255) : IM_COL32(244, 246, 249, 255);
    ImU32 bgBot = darkness() > 0 ? IM_COL32(22, 23, 26, 255) : IM_COL32(224, 229, 236, 255);
    dl->AddRectFilledMultiColor(w0, ImVec2(w0.x + ws.x, w0.y + ws.y), bgTop, bgTop, bgBot, bgBot);

    // Left column: brand and actions.
    float colW = 300 * S, pad = 48 * S;
    ImVec2 L(w0.x + pad, w0.y + pad);
    drawLogo(dl, L, 44 * S);
    ImGui::PushFont(fonts_.bold, 30.f * S);
    dl->AddText(ImVec2(L.x + 58 * S, L.y + 4 * S), col::Text, "Kestral");
    ImGui::PopFont();
    dl->AddText(ImVec2(L.x + 60 * S, L.y + 40 * S), col::TextDim, "Design, simulate, print.");

    bool hasRecovery = !recoveryFile_.empty();
    struct Act { const char* label; const char* sub; Icon icon; int id; };
    Act acts[] = {
        {"New Design", "Start from an empty model", Icon::Extrude, 0},
        {"Open...", "A Kestral design, STEP, STL or OBJ", Icon::Folder, 1},
        {"Recover", hasRecovery ? "Autosaved work is waiting" : "No autosaved work", Icon::Undo, 2},
        {"Move from Fusion", "Bring every Fusion design over", Icon::ImportFeature, 3},
    };
    float y = L.y + 96 * S;
    for (auto& a : acts) {
        bool enabled = a.id != 2 || hasRecovery;
        ImGui::SetCursorScreenPos(ImVec2(L.x, y));
        ImGui::PushID(a.id);
        bool clicked = ImGui::InvisibleButton("##act", ImVec2(colW, 54 * S)) && enabled;
        bool hov = ImGui::IsItemHovered() && enabled;
        ImGui::PopID();
        ImVec2 p0(L.x, y), p1(L.x + colW, y + 54 * S);
        dl->AddRectFilled(p0, p1, hov ? col::Selected : col::Card, 8 * S);
        if (a.id == 0) dl->AddRectFilled(p0, ImVec2(p0.x + 4 * S, p1.y), col::Accent, 8 * S, ImDrawFlags_RoundCornersLeft);
        drawIcon(dl, ImVec2(p0.x + 30 * S, (p0.y + p1.y) * 0.5f), 26 * S, a.icon, enabled);
        ImGui::PushFont(fonts_.bold, 0.f);
        dl->AddText(ImVec2(p0.x + 58 * S, p0.y + 9 * S), enabled ? col::Text : col::TextDim, a.label);
        ImGui::PopFont();
        dl->AddText(ImVec2(p0.x + 58 * S, p0.y + 29 * S), a.id == 2 && hasRecovery ? col::Warning : col::TextDim, a.sub);
        if (clicked) {
            if (a.id == 0) { newDocument(); startOpen_ = false; }
            if (a.id == 1) { openDocumentDialog(); if (!doc_.timeline.empty() || !doc_.bodies.empty()) startOpen_ = false; }
            if (a.id == 2) startOpen_ = false;   // the Recovered work prompt takes it from here
            if (a.id == 3) { fusionMoveOpen_ = true; }
        }
        y += 62 * S;
    }
    ImGui::SetCursorScreenPos(ImVec2(L.x, y + 10 * S));
    if (ImGui::InvisibleButton("##prefs_sp", ImVec2(colW, 22 * S))) settingsOpen_ = true;
    dl->AddText(ImVec2(L.x + 4 * S, y + 12 * S), ImGui::IsItemHovered() ? col::Accent : col::TextDim, "Preferences");

    // Right: recent designs as preview cards.
    float rx = L.x + colW + 56 * S, ry = w0.y + pad + 8 * S;
    ImGui::PushFont(fonts_.bold, 0.f);
    dl->AddText(ImVec2(rx, ry), col::Text, "RECENT");
    ImGui::PopFont();
    ry += 30 * S;
    float cw = 190 * S, ch = 176 * S, gap = 16 * S;
    int cols = std::max(1, (int)((w0.x + ws.x - pad - rx + gap) / (cw + gap)));
    std::vector<std::string> recent;
    std::error_code ec;
    for (auto& r : settings_.recentFiles)
        if (std::filesystem::exists(u8path(r), ec)) recent.push_back(r);
    if (recent.empty()) dl->AddText(ImVec2(rx, ry), col::TextDim, "Designs you open show up here.");
    int maxCards = cols * std::max(1, (int)((w0.y + ws.y - ry - pad) / (ch + gap)));
    for (int i = 0; i < (int)recent.size() && i < maxCards; ++i) {
        const std::string& path = recent[i];
        ImVec2 p0(rx + (i % cols) * (cw + gap), ry + (i / cols) * (ch + gap)), p1(p0.x + cw, p0.y + ch);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(i + 100);
        bool clicked = ImGui::InvisibleButton("##recent", ImVec2(cw, ch));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) ImGui::SetTooltip("%s", path.c_str());
        dl->AddRectFilled(p0, p1, col::Card, 8 * S);
        dl->AddRect(p0, p1, hov ? col::Accent : col::PanelBorder, 8 * S, 0, hov ? 2.f * S : 1.f);
        ImVec2 t0(p0.x + 8 * S, p0.y + 8 * S), t1(p1.x - 8 * S, p1.y - 44 * S);
        dl->AddRectFilled(t0, t1, darkness() > 0 ? IM_COL32(44, 47, 53, 255) : IM_COL32(236, 239, 243, 255), 6 * S);
        if (ImTextureData* tex = thumbFor(path)) {
            float side = std::min(t1.x - t0.x, t1.y - t0.y);
            ImVec2 c((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f);
            dl->AddImage(tex->GetTexRef(), ImVec2(c.x - side * 0.5f, c.y - side * 0.5f), ImVec2(c.x + side * 0.5f, c.y + side * 0.5f));
        } else {
            drawIcon(dl, ImVec2((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f), 40 * S, Icon::Extrude, false);
        }
        std::string name = pathToU8(u8path(path).stem());
        std::string ext = pathToU8(u8path(path).extension());
        ImGui::PushClipRect(ImVec2(p0.x + 8 * S, t1.y), ImVec2(p1.x - 8 * S, p1.y), true);
        ImGui::PushFont(fonts_.bold, 0.f);
        dl->AddText(ImVec2(p0.x + 10 * S, t1.y + 6 * S), col::Text, name.c_str());
        ImGui::PopFont();
        dl->AddText(ImVec2(p0.x + 10 * S, t1.y + 24 * S), col::TextDim, ext == ".kst" ? "Kestral design" : ext.c_str());
        ImGui::PopClipRect();
        if (clicked) { importPath(path); startOpen_ = false; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !settingsOpen_) { startOpen_ = false; }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

} // namespace kst
