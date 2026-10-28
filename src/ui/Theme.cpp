#include "ui/Theme.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdio>

namespace kst::ui {

namespace {

float gDark = 0.f;

// Flips lightness toward the dark theme: light panels go dark, dark ink goes light (ink switches
// sides as soon as the panels cross the middle, so text stays readable at every slider value). The color's
// tint is kept as an offset (absolute chroma), not as HSL saturation: light grays with a hint of
// blue would otherwise turn into strong navy when darkened. Neutral grays get a touch of warmth
// removed so the dark theme reads as a clean charcoal (like Fusion's dark theme).
ImVec4 flip(const ImVec4& c, float weight) {
    if (weight <= 0.f) return c;
    float mx = std::max({c.x, c.y, c.z}), mn = std::min({c.x, c.y, c.z});
    float l = (mx + mn) * 0.5f;
    // Target lightness: panels (0.97) -> 0.13, frames (0.93) -> 0.17, ink (0.23) -> 0.85.
    float dl = std::clamp(0.10f + (1.f - l) * 0.98f, 0.f, 1.f);
    float nl;
    if (l >= 0.6f) {
        nl = l + (dl - l) * weight;   // panels, frames, highlights: follow the slider smoothly
    } else {
        // Ink (text, icons, lines) must never meet the panels halfway, or it vanishes. It stays on
        // the far side of the main panel color: dark ink while panels are light, light ink once
        // they turn dark, always spaced from the panel like in the full light or full dark theme.
        float bg = 0.97f + (0.13f - 0.97f) * weight;
        if (bg >= 0.5f) nl = bg * l / 0.97f;
        else nl = bg + (1.f - bg) * std::max(dl - 0.13f, 0.f) / 0.87f;
    }
    float chromaScale = 1.f - 0.45f * weight;   // dark UIs carry less tint
    ImVec4 o;
    o.w = c.w;
    o.x = std::clamp(nl + (c.x - l) * chromaScale, 0.f, 1.f);
    o.y = std::clamp(nl + (c.y - l) * chromaScale, 0.f, 1.f);
    o.z = std::clamp(nl + (c.z - l) * chromaScale, 0.f, 1.f);
    return o;
}

float chroma(const ImVec4& c) { return std::max({c.x, c.y, c.z}) - std::min({c.x, c.y, c.z}); }

} // namespace

float darkness() { return gDark; }

ImVec4 th(const ImVec4& c) { return flip(c, gDark); }
ImU32 th(ImU32 c) {
    if (gDark <= 0.f) return c;
    return ImGui::ColorConvertFloat4ToU32(flip(ImGui::ColorConvertU32ToFloat4(c), gDark));
}
ImU32 thIcon(ImU32 c) {
    if (gDark <= 0.f) return c;
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    float gray = std::clamp(1.f - (chroma(v) - 0.08f) / 0.22f, 0.f, 1.f);
    return ImGui::ColorConvertFloat4ToU32(flip(v, gDark * gray));
}

void viewportColors(float top[3], float bottom[3], float grid[3]) {
    ImVec4 t = th(ImVec4(0.976f, 0.980f, 0.988f, 1.f)), b = th(ImVec4(0.800f, 0.835f, 0.870f, 1.f));
    // Dark: a studio gradient, lifted slate at the top down to deep charcoal (pro CAD look).
    if (gDark > 0.f) { t = ImVec4(0.235f, 0.250f, 0.278f, 1.f); b = ImVec4(0.098f, 0.102f, 0.114f, 1.f); }
    ImVec4 g = th(ImVec4(0.42f, 0.45f, 0.50f, 1.f));
    top[0] = t.x; top[1] = t.y; top[2] = t.z;
    bottom[0] = b.x; bottom[1] = b.y; bottom[2] = b.z;
    grid[0] = g.x; grid[1] = g.y; grid[2] = g.z;
}

void applyTheme(float scale, float dark) {
    // Dark is one designed theme (charcoal panels, like pro CAD tools), not a gray in between.
    gDark = dark <= 0.f ? 0.f : 1.f;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsLight(&s);
    s.WindowRounding = 6.f;
    s.ChildRounding = 4.f;
    s.FrameRounding = 4.f;
    s.PopupRounding = 6.f;
    s.ScrollbarRounding = 6.f;
    s.GrabRounding = 3.f;
    s.TabRounding = 3.f;
    s.WindowBorderSize = 1.f;
    s.PopupBorderSize = 1.f;
    s.FrameBorderSize = 0.f;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(6, 4);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.IndentSpacing = 14.f;
    s.ScrollbarSize = 11.f;
    s.WindowTitleAlign = ImVec2(0.f, 0.5f);

    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    c[ImGuiCol_Text] = rgb(52, 58, 66);
    c[ImGuiCol_TextDisabled] = rgb(150, 156, 164);
    c[ImGuiCol_WindowBg] = rgb(246, 247, 249);
    c[ImGuiCol_ChildBg] = rgb(250, 251, 252, 0.f);
    c[ImGuiCol_PopupBg] = rgb(255, 255, 255);
    c[ImGuiCol_Border] = rgb(210, 214, 220);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.f);
    c[ImGuiCol_FrameBg] = rgb(238, 240, 243);
    c[ImGuiCol_FrameBgHovered] = rgb(250, 236, 228);
    c[ImGuiCol_FrameBgActive] = rgb(255, 222, 205);
    c[ImGuiCol_TitleBg] = rgb(240, 242, 245);
    c[ImGuiCol_TitleBgActive] = rgb(240, 242, 245);
    c[ImGuiCol_TitleBgCollapsed] = rgb(240, 242, 245);
    c[ImGuiCol_MenuBarBg] = rgb(236, 238, 241);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0.f);
    c[ImGuiCol_ScrollbarGrab] = rgb(196, 202, 210);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(170, 178, 188);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(150, 158, 170);
    c[ImGuiCol_CheckMark] = rgb(255, 107, 53);
    c[ImGuiCol_SliderGrab] = rgb(255, 107, 53);
    c[ImGuiCol_SliderGrabActive] = rgb(230, 85, 30);
    c[ImGuiCol_Button] = rgb(238, 240, 243);
    c[ImGuiCol_ButtonHovered] = rgb(252, 232, 222);
    c[ImGuiCol_ButtonActive] = rgb(255, 210, 190);
    c[ImGuiCol_Header] = rgb(255, 224, 208);
    c[ImGuiCol_HeaderHovered] = rgb(253, 236, 226);
    c[ImGuiCol_HeaderActive] = rgb(255, 210, 190);
    c[ImGuiCol_Separator] = rgb(216, 220, 226);
    c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0.f);
    c[ImGuiCol_ResizeGripHovered] = rgb(255, 107, 53, 0.4f);
    c[ImGuiCol_ResizeGripActive] = rgb(255, 107, 53, 0.7f);
    c[ImGuiCol_PlotHistogram] = rgb(255, 107, 53);
    c[ImGuiCol_TextSelectedBg] = rgb(255, 107, 53, 0.3f);
    c[ImGuiCol_NavHighlight] = rgb(255, 107, 53);
    c[ImGuiCol_ModalWindowDimBg] = rgb(30, 34, 40, 0.25f);
    if (gDark > 0.f) {
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            if (i == ImGuiCol_ModalWindowDimBg || i == ImGuiCol_BorderShadow) continue;
            c[i] = th(c[i]);
        }
        c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.45f);
        // The designed dark palette: neutral charcoal, one blue accent.
        c[ImGuiCol_Text] = rgb(226, 228, 232);
        c[ImGuiCol_TextDisabled] = rgb(128, 133, 142);
        c[ImGuiCol_WindowBg] = rgb(35, 36, 40);
        c[ImGuiCol_PopupBg] = rgb(40, 42, 47);
        c[ImGuiCol_Border] = rgb(52, 54, 60);
        c[ImGuiCol_FrameBg] = rgb(46, 48, 54);
        c[ImGuiCol_FrameBgHovered] = rgb(55, 58, 65);
        c[ImGuiCol_FrameBgActive] = rgb(62, 66, 74);
        c[ImGuiCol_TitleBg] = rgb(30, 31, 35);
        c[ImGuiCol_TitleBgActive] = rgb(30, 31, 35);
        c[ImGuiCol_TitleBgCollapsed] = rgb(30, 31, 35);
        c[ImGuiCol_MenuBarBg] = rgb(30, 31, 35);
        c[ImGuiCol_ScrollbarGrab] = rgb(66, 69, 77);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(84, 88, 97);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(100, 105, 115);
        c[ImGuiCol_CheckMark] = rgb(255, 130, 80);
        c[ImGuiCol_SliderGrab] = rgb(255, 107, 53);
        c[ImGuiCol_SliderGrabActive] = rgb(255, 140, 90);
        c[ImGuiCol_Button] = rgb(48, 50, 57);
        c[ImGuiCol_ButtonHovered] = rgb(60, 64, 72);
        c[ImGuiCol_ButtonActive] = rgb(255, 107, 53, 0.55f);
        c[ImGuiCol_Header] = rgb(255, 107, 53, 0.28f);
        c[ImGuiCol_HeaderHovered] = rgb(255, 255, 255, 0.06f);
        c[ImGuiCol_HeaderActive] = rgb(255, 107, 53, 0.40f);
        c[ImGuiCol_Separator] = rgb(52, 54, 60);
        c[ImGuiCol_Tab] = rgb(35, 36, 40);
        c[ImGuiCol_TabHovered] = rgb(55, 58, 65);
        c[ImGuiCol_TabSelected] = rgb(46, 48, 54);
        c[ImGuiCol_TextSelectedBg] = rgb(255, 107, 53, 0.35f);
        c[ImGuiCol_PlotHistogram] = rgb(255, 107, 53);
    }
    s.ScaleAllSizes(scale);
    s.FontSizeBase = 15.f;
    s.FontScaleDpi = scale;   // text drawn at the real pixel size (sharp), see loadFonts

    // Named colors for custom drawing.
    col::Accent = IM_COL32(255, 107, 53, 255);
    col::AccentSoft = IM_COL32(255, 107, 53, 40);
    col::Panel = th(IM_COL32(246, 247, 249, 255));
    col::PanelBorder = th(IM_COL32(210, 214, 220, 255));
    col::Text = th(IM_COL32(52, 58, 66, 255));
    col::TextDim = th(IM_COL32(120, 128, 138, 255));
    col::Hover = th(IM_COL32(252, 236, 226, 255));
    col::Error = IM_COL32(214, 64, 64, 255);
    col::AppBar = th(IM_COL32(236, 238, 241, 255));
    col::Print = IM_COL32(255, 107, 53, 255);
    col::Card = th(IM_COL32(255, 255, 255, 255));
    col::Selected = th(IM_COL32(255, 224, 208, 255));
    col::Active = th(IM_COL32(255, 210, 190, 255));
    col::Divider = th(IM_COL32(226, 229, 234, 255));
    col::Chip = th(IM_COL32(232, 235, 240, 255));
    col::Timeline = th(IM_COL32(246, 247, 249, 255));
    col::Status = th(IM_COL32(90, 98, 110, 255));
    col::Success = IM_COL32(46, 160, 90, 255);
    col::Warning = IM_COL32(232, 163, 61, 255);
    if (gDark > 0.f) {
        col::Accent = IM_COL32(255, 107, 53, 255);
        col::AccentSoft = IM_COL32(255, 107, 53, 48);
        col::Panel = IM_COL32(35, 36, 40, 255);
        col::PanelBorder = IM_COL32(52, 54, 60, 255);
        col::Text = IM_COL32(226, 228, 232, 255);
        col::TextDim = IM_COL32(140, 145, 154, 255);
        col::Hover = IM_COL32(255, 255, 255, 18);
        col::AppBar = IM_COL32(24, 25, 28, 255);
        col::Card = IM_COL32(30, 31, 35, 255);
        col::Selected = IM_COL32(255, 107, 53, 70);
        col::Active = IM_COL32(255, 107, 53, 110);
        col::Divider = IM_COL32(48, 50, 56, 255);
        col::Chip = IM_COL32(48, 50, 57, 255);
        col::Timeline = IM_COL32(30, 31, 35, 255);
        col::Status = IM_COL32(150, 155, 164, 255);
    }
}

namespace {
// Linux and other desktops: ask fontconfig which file the system uses for a family.
std::string fcMatch(const char* pattern) {
#ifdef _WIN32
    (void)pattern;
    return {};
#else
    std::string cmd = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    char buf[1024];
    std::string out;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
#endif
}
} // namespace

Fonts loadFonts(float) {
    // Fonts load once at their base size; ImGui (1.92 dynamic fonts) rasterizes them at the exact
    // pixel size the UI scale asks for, so text stays sharp at any scale and on any monitor.
    ImGuiIO& io = ImGui::GetIO();
    Fonts f;
    const float size = 15.f;
    std::vector<std::string> reg = {"C:\\Windows\\Fonts\\segoeui.ttf"}, bold = {"C:\\Windows\\Fonts\\segoeuib.ttf"};
    std::string fr = fcMatch("sans-serif:style=Regular"), fb = fcMatch("sans-serif:style=Bold");
    if (!fr.empty()) reg.push_back(fr);
    if (!fb.empty()) bold.push_back(fb);
    for (const char* p : {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                          "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf", "/System/Library/Fonts/SFNS.ttf"})
        reg.push_back(p);
    for (const char* p : {"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                          "/usr/share/fonts/google-noto/NotoSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
                          "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf"})
        bold.push_back(p);
    std::error_code ec;
    ImFontConfig cfg;
    cfg.PixelSnapH = true;   // glyphs on whole pixels: crisp edges
    auto ext = [](const std::string& p) {
        auto d = p.find_last_of('.');
        std::string e = d == std::string::npos ? "" : p.substr(d);
        for (auto& c : e) c = (char)std::tolower((unsigned char)c);
        return e == ".ttf" || e == ".otf" || e == ".ttc";
    };
    for (auto& p : reg)
        if (ext(p) && std::filesystem::exists(p, ec)) { f.regular = io.Fonts->AddFontFromFileTTF(p.c_str(), size, &cfg); if (f.regular) break; }
    if (!f.regular) {
        ImFontConfig dc;
        dc.SizePixels = 13.f;
        f.regular = io.Fonts->AddFontDefault(&dc);
    }
    for (auto& p : bold)
        if (ext(p) && std::filesystem::exists(p, ec)) { f.bold = io.Fonts->AddFontFromFileTTF(p.c_str(), size, &cfg); if (f.bold) break; }
    if (!f.bold) f.bold = f.regular;
    io.FontDefault = f.regular;
    return f;
}

} // namespace kst::ui
