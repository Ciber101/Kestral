// Preview, laid out like OrcaSlicer's: plate pictures at the top left, the legend at the top right
// (view type, line types with time, share and filament used, the totals), the G-code under it, the
// layer slider at the right edge and the move slider along the bottom.
#include "app/App.h"
#include "app/SliceStyle.h"
#include "app/Thumbnails.h"
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kst {

using namespace ui;
using sl::OIcon;

namespace {

std::string shortT(double sec) {
    int s = (int)std::lround(sec);
    char b[32];
    if (s < 60) std::snprintf(b, sizeof b, "%ds", s);
    else if (s < 3600) std::snprintf(b, sizeof b, "%dm%02ds", s / 60, s % 60);
    else std::snprintf(b, sizeof b, "%dh%02dm", s / 3600, (s % 3600) / 60);
    return b;
}

ImU32 rgbaToU32(uint32_t rgba) { return rgba; }   // RGBA8 with R low is ImGui's layout too

const char* kViewNames[print::ViewModes] = {"Line Type", "Filament", "Speed", "Layer height", "Line width", "Fan speed", "Temperature", "Volumetric flow rate"};
const char* kViewUnits[print::ViewModes] = {"", "", "mm/s", "mm", "mm", "%", "C", "mm3/s"};

} // namespace

// ===================================================================== textures

ImTextureData* App::makeTexture(const uint8_t* rgba, int w, int h) {
    ImTextureData* t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, w, h);
    std::memcpy(t->GetPixels(), rgba, (size_t)w * h * 4);
    t->UseColors = true;
    ImGui::RegisterUserTexture(t);
    return t;
}

void App::dropTexture(ImTextureData* t) {
    if (!t) return;
    t->SetStatus(ImTextureStatus_WantDestroy);
    thumbGraveyard_.push_back(t);
}

ImTextureData* App::sliceThumbFor(const std::string& gcode3mf) {
    auto it = plateThumbTex_.find(gcode3mf);
    if (it != plateThumbTex_.end()) return it->second;
    ImTextureData* tex = nullptr;
    std::string png = print::readZipEntry(gcode3mf, "Metadata/plate_1.png");
    if (!png.empty()) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(png.data()), (int)png.size(), &w, &h, &n, 4);
        if (px) {
            tex = makeTexture(px, w, h);
            stbi_image_free(px);
        }
    }
    if (!tex) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (thumb::readPng(gcode3mf + ".png", rgba, w, h)) tex = makeTexture(rgba.data(), w, h);
    }
    plateThumbTex_[gcode3mf] = tex;
    return tex;
}

// ===================================================================== plate pictures

void App::drawSlicePlateThumbs() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float sz = 84 * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + sliceSideW() + 10 * S, vp->Pos.y + sliceTop() + 10 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 6 * S));
    ImGui::Begin("##platethumbs", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int p = 0; p < std::max(1, slice_.plates); ++p) {
        ImGui::PushID(p);
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + sz, a.y + sz);
        bool click = ImGui::InvisibleButton("##t", ImVec2(sz, sz));
        bool hv = ImGui::IsItemHovered();
        bool act = p == slice_.activePlate;
        const print::SliceResult* r = p < (int)slice_.ex.plateResults.size() && slice_.ex.plateResults[(size_t)p].ok ? &slice_.ex.plateResults[(size_t)p] : nullptr;
        dl->AddRectFilled(a, b, col::Card, 6 * S);
        if (r) {
            if (ImTextureData* t = sliceThumbFor(r->gcode3mf))
                dl->AddImage(t->GetTexRef(), ImVec2(a.x + 6 * S, a.y + 6 * S), ImVec2(b.x - 6 * S, b.y - 6 * S));
        } else {
            const char* t = "Not sliced";
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(a.x + (sz - ts.x) * 0.5f, a.y + (sz - ts.y) * 0.5f), col::TextDim, t);
        }
        dl->AddRect(a, b, act ? sl::orangeU() : hv ? col::Text : col::PanelBorder, 6 * S, 0, act ? 2.f * S : 1.f * S);
        char num[8];
        std::snprintf(num, sizeof num, "%d", p + 1);
        dl->AddText(ImVec2(a.x + 6 * S, a.y + 4 * S), act ? sl::orangeU() : col::Text, num);
        if (hv) ImGui::SetTooltip("Plate %d (%s)%s", p + 1, plateName(p).c_str(), r ? "" : ": click to slice it");
        if (click) {
            slice_.activePlate = p;
            if (r) {
                {
                    std::lock_guard lock(print_.m);
                    print_.last = *r;
                }
                loadSlicePreview(r->gcode3mf);
            } else if (!slice_.slicing) {
                startSliceJob();
            }
            sceneDirty_ = true;
        }
        ImGui::PopID();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void App::drawSliceNoPreview() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float cx = vp->Pos.x + sliceSideW() + (vp->Size.x - sliceSideW()) * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(cx, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * S, 14 * S));
    ImGui::Begin("##nopreview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    if (slice_.slicing || slice_.loadingPreview) {
        ImGui::TextUnformatted(slice_.slicing ? "Slicing..." : "Reading the toolpaths...");
        activity();
    } else {
        ImGui::TextUnformatted("This plate is not sliced yet.");
        if (sl::orangeButton("Slice plate", ImVec2(160 * S, 0))) startSliceJob();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ===================================================================== legend

void App::drawSliceLegend() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float legendW = 350 * S;
    float right = vp->Pos.x + vp->Size.x - 100 * S;   // room for the layer slider
    ImGui::SetNextWindowPos(ImVec2(right - legendW, vp->Pos.y + sliceTop() + 10 * S));
    ImGui::SetNextWindowSize(ImVec2(legendW, 0));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * S, 8 * S));
    ImGui::Begin("##legend", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize);
    const auto& tp = *slice_.paths;
    print::SliceResult last;
    {
        std::lock_guard lock(print_.m);
        last = print_.last;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Header: fold, the G-code switch, then the view type.
    {
        float ib = ImGui::GetFrameHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##fold", ImVec2(ib, ib))) slice_.legendFolded = !slice_.legendFolded;
        ImGui::SetItemTooltip(slice_.legendFolded ? "Show the legend" : "Fold the legend");
        float cx = p.x + ib * 0.5f, cy = p.y + ib * 0.5f, r = 4 * S;
        if (slice_.legendFolded) { dl->AddLine(ImVec2(cx - r, cy - r * 0.5f), ImVec2(cx, cy + r * 0.5f), col::Text, 1.5f * S); dl->AddLine(ImVec2(cx, cy + r * 0.5f), ImVec2(cx + r, cy - r * 0.5f), col::Text, 1.5f * S); }
        else { dl->AddLine(ImVec2(cx - r, cy + r * 0.5f), ImVec2(cx, cy - r * 0.5f), col::Text, 1.5f * S); dl->AddLine(ImVec2(cx, cy - r * 0.5f), ImVec2(cx + r, cy + r * 0.5f), col::Text, 1.5f * S); }
        ImGui::SameLine(0, 2 * S);
        p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##gcv", ImVec2(ib, ib))) slice_.gcodeView = !slice_.gcodeView;
        ImGui::SetItemTooltip(slice_.gcodeView ? "Hide the G-code" : "Show the G-code");
        const char* code = "</>";
        ImVec2 ts = ImGui::CalcTextSize(code);
        dl->AddText(ImVec2(p.x + (ib - ts.x) * 0.5f, p.y + (ib - ts.y) * 0.5f), slice_.gcodeView ? sl::orangeU() : col::Text, code);
        ImGui::SameLine(0, 8 * S);
        ImGui::SetNextItemWidth(-1);
        int vt = std::clamp(slice_.viewType, 0, print::ViewModes - 1);
        if (ImGui::Combo("##viewtype", &vt, kViewNames, print::ViewModes)) { slice_.viewType = vt; sceneDirty_ = true; }
    }
    if (slice_.legendFolded) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }
    double raw = 0;
    for (int f = 0; f < print::kFeatureCount; ++f) if (!print::featureIsMarker(f)) raw += tp.seconds[f];
    double scale = raw > 0 && last.seconds > 0 ? last.seconds / raw : 1.0;
    // Filament weight: diameter and density from the filament profile.
    double dia = std::atof(sliceValue("filament_diameter").c_str());
    double dens = std::atof(sliceValue("filament_density").c_str());
    double cost = std::atof(sliceValue("filament_cost").c_str());
    if (dia <= 0.1) dia = 1.75;
    if (dens <= 0.1) dens = 1.24;
    const double area = 3.14159265 * dia * dia * 0.25;
    auto grams = [&](double mm) { return mm * area * dens / 1000.0; };
    int mode = std::clamp(slice_.viewType, 0, print::ViewModes - 1);
    float chip = ImGui::GetTextLineHeight();
    auto eye = [&](int f) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(f + 3000);
        if (ImGui::InvisibleButton("##eye", ImVec2(chip * 1.3f, chip))) { slice_.show[f] = !slice_.show[f]; sceneDirty_ = true; }
        ImGui::PopID();
        sl::drawOIcon(dl, ImVec2(p.x + chip * 0.65f, p.y + chip * 0.5f), chip * 0.9f, slice_.show[f] ? OIcon::Eye : OIcon::EyeOff, slice_.show[f] ? sl::orangeU() : col::TextDim);
    };
    if (mode == print::ViewFeature || mode >= print::ViewSpeed) {
        if (ImGui::BeginTable("##lt", 5, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn(kViewNames[0], ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 52 * S);
            ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 36 * S);
            ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed, 82 * S);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, chip * 1.3f);
            ImGui::TableHeadersRow();
            for (int f = 0; f < print::kFeatureCount; ++f) {
                if (!tp.used[f]) continue;
                if (mode >= print::ViewSpeed && !print::featureIsMove(f)) continue;   // the gradient shows the lines
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImVec2 p = ImGui::GetCursorScreenPos();
                uint32_t c = print::featureColor(f);
                dl->AddRectFilled(ImVec2(p.x, p.y + 2 * S), ImVec2(p.x + chip - 4 * S, p.y + chip - 2 * S), slice_.show[f] ? rgbaToU32(c) : (c & 0x00FFFFFF) | 0x50000000, 2 * S);
                ImGui::Dummy(ImVec2(chip - 2 * S, chip));
                ImGui::SameLine();
                if (slice_.show[f]) ImGui::TextUnformatted(print::featureName(f));
                else ImGui::TextDisabled("%s", print::featureName(f));
                ImGui::TableNextColumn();
                if (!print::featureIsMarker(f)) ImGui::TextDisabled("%s", shortT(tp.seconds[f] * scale).c_str());
                ImGui::TableNextColumn();
                if (!print::featureIsMarker(f)) ImGui::TextDisabled("%.1f", raw > 0 ? tp.seconds[f] * 100.0 / raw : 0.0);
                ImGui::TableNextColumn();
                if (print::featureIsMarker(f)) ImGui::TextDisabled("%d", tp.count[f]);
                else if (f == (int)print::Feature::Travel) ImGui::TextDisabled("%.2fm", tp.length[f] / 1000.0);
                else ImGui::TextDisabled("%.2fm %.2fg", tp.filamentMm[f] / 1000.0, grams(tp.filamentMm[f]));
                ImGui::TableNextColumn();
                eye(f);
            }
            ImGui::EndTable();
        }
        if (mode >= print::ViewSpeed) {
            // The gradient: highest at the top, like Orca.
            ImGui::Separator();
            ImGui::TextDisabled("%s (%s)", kViewNames[mode], kViewUnits[mode]);
            float lo = tp.lo[mode], hi = tp.hi[mode];
            for (int b = print::kBins - 1; b >= 0; --b) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(ImVec2(p.x, p.y + 1 * S), ImVec2(p.x + chip * 1.6f, p.y + chip - 1 * S), rgbaToU32(print::binColor(b)), 2 * S);
                ImGui::Dummy(ImVec2(chip * 1.6f, chip));
                ImGui::SameLine();
                float v = lo + (hi - lo) * (b + 0.5f) / print::kBins;
                if (mode == print::ViewHeight || mode == print::ViewWidth) ImGui::Text("%.3f", v);
                else ImGui::Text("%.1f", v);
            }
            if (slice_.scalarMode != mode) { ImGui::TextDisabled("Coloring..."); activity(); }
        }
    } else {
        // Filaments: each one's color, profile and how much it uses.
        if (ImGui::BeginTable("##fils", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Filament", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Used", ImGuiTableColumnFlags_WidthFixed, 110 * S);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 4 * S);
            ImGui::TableHeadersRow();
            for (int t = 0; t < 16; ++t) {
                if (!tp.toolUsed[t]) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                glm::vec3 c = t < (int)slice_.toolColors.size() ? slice_.toolColors[(size_t)t] : sliceFilColor(0);
                int tabFil = t < (int)slice_.ex.slicedFils.size() ? slice_.ex.slicedFils[(size_t)t] : t;
                ImVec2 p = ImGui::GetCursorScreenPos();
                sl::filChip(dl, p, chip + 2 * S, c.r, c.g, c.b, tabFil + 1, 2 * S);
                ImGui::Dummy(ImVec2(chip + 2 * S, chip + 2 * S));
                ImGui::SameLine();
                std::string pn = sliceFilPreset(tabFil);
                if (pn.size() > 26) pn = pn.substr(0, 25) + "..";
                ImGui::TextUnformatted(pn.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.2fm  %.2fg", tp.toolMm[t] / 1000.0, grams(tp.toolMm[t]));
            }
            ImGui::EndTable();
        }
        if (tp.toolChanges > 0) ImGui::TextDisabled("Filament changes: %d", tp.toolChanges);
    }
    // Totals (Orca's Total estimation).
    ImGui::Separator();
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("Total estimation");
    ImGui::PopFont();
    double model = 0;
    for (int f = 0; f < print::kFeatureCount; ++f) if (f != (int)print::Feature::PrimeTower && !print::featureIsMove(f)) model += tp.filamentMm[f];
    double prepare = tp.seconds[(int)print::Feature::Custom] * scale;
    auto kv = [&](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k);
        ImGui::SameLine(150 * S);
        ImGui::TextUnformatted(v.c_str());
    };
    char b[96];
    std::snprintf(b, sizeof b, "%.2f m    %.2f g", last.ok ? last.meters : 0.0, last.ok ? last.grams : 0.0);
    kv("Total filament:", b);
    std::snprintf(b, sizeof b, "%.2f m    %.2f g", model / 1000.0, grams(model));
    kv("Model filament:", b);
    if (cost > 0) { std::snprintf(b, sizeof b, "%.2f", (last.ok ? last.grams : grams(model)) / 1000.0 * cost); kv("Cost:", b); }
    kv("Prepare time:", shortT(prepare));
    kv("Model printing time:", shortT(std::max(0.0, (last.ok ? last.seconds : raw) - prepare)));
    kv("Total time:", print::formatDuration(last.ok ? last.seconds : raw));
    if (slice_.slicedRev != doc_.revision) ImGui::TextColored(sl::changed(), "The design changed since this slice");
    slice_.legendBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
    ImGui::PopStyleVar(2);
    // The nozzle at the move slider's spot.
    if (slice_.nozzleShown) {
        ImVec2 q;
        if (worldToScreen(glm::dvec3(slice_.nozzleAt), q)) {
            ImDrawList* fg = ImGui::GetBackgroundDrawList();
            fg->AddTriangleFilled(ImVec2(q.x, q.y), ImVec2(q.x - 7 * S, q.y - 16 * S), ImVec2(q.x + 7 * S, q.y - 16 * S), sl::orangeU(230));
            fg->AddTriangle(ImVec2(q.x, q.y), ImVec2(q.x - 7 * S, q.y - 16 * S), ImVec2(q.x + 7 * S, q.y - 16 * S), IM_COL32(40, 20, 10, 255), 1.f * S);
        }
    }
}

// ===================================================================== G-code

void App::drawSliceGcodeView() {
    if (!slice_.gcodeView || !slice_.paths || !slice_.paths->gcode) return;
    const auto& tp = *slice_.paths;
    if (slice_.layerHi < 0 || slice_.layerHi >= (int)tp.layers.size()) return;
    const auto& L = tp.layers[(size_t)slice_.layerHi];
    if (L.segs.empty()) return;
    size_t at = slice_.moveHi < 0 ? L.segs.size() : (size_t)std::clamp(slice_.moveHi, 1, (int)L.segs.size());
    uint32_t curLine = L.segs[at - 1].line;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = 350 * S;
    float right = vp->Pos.x + vp->Size.x - 100 * S;
    float y0 = std::max(slice_.legendBottom + 8 * S, vp->Pos.y + sliceTop() + 200 * S);
    int rows = 20;
    ImGui::SetNextWindowPos(ImVec2(right - w, y0));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * S, 6 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * S, 1 * S));
    ImGui::Begin("##gcodeview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_AlwaysAutoResize);
    const std::string& g = *tp.gcode;
    int first = std::max(0, (int)curLine - rows / 2);
    int lastL = std::min((int)tp.lineStart.size() - 1, first + rows - 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = first; i <= lastL; ++i) {
        size_t a = tp.lineStart[(size_t)i];
        size_t e = i + 1 < (int)tp.lineStart.size() ? tp.lineStart[(size_t)i + 1] : g.size();
        std::string line = g.substr(a, e - a);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.size() > 46) line = line.substr(0, 45) + "..";
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool cur = i == (int)curLine;
        float lh = ImGui::GetTextLineHeight();
        if (cur) dl->AddRect(ImVec2(p.x - 3 * S, p.y - 1 * S), ImVec2(p.x + w - 14 * S, p.y + lh + 1 * S), sl::orangeU(), 3 * S, 0, 1.2f * S);
        char num[16];
        std::snprintf(num, sizeof num, "%d", i + 1);
        ImGui::TextColored(sl::orange(), "%s", num);
        ImGui::SameLine(48 * S);
        if (!line.empty() && line[0] == ';') ImGui::TextDisabled("%s", line.c_str());
        else {
            size_t sp = line.find(' ');
            std::string cmd = line.substr(0, sp), rest = sp == std::string::npos ? std::string() : line.substr(sp);
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.35f, 1.f), "%s", cmd.c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(rest.c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ===================================================================== sliders

void App::drawSliceLayerSlider() {
    // Orca's layer slider at the right edge: the top handle shows the layer and its height in a box,
    // the bottom one too. Drag them, or scroll over the slider, or Up / Down.
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    int n = (int)slice_.meshes.size();
    if (n <= 0) return;
    float top = vp->Pos.y + sliceTop() + 60.f * S, bottom = vp->Pos.y + vp->Size.y - 110.f * S;
    float cx = vp->Pos.x + vp->Size.x - 22.f * S;
    ImGui::SetNextWindowPos(ImVec2(cx - 90 * S, top - 40 * S));
    ImGui::SetNextWindowSize(ImVec2(106 * S, bottom - top + 80 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##layerslider", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto yOf = [&](int layer) { return n <= 1 ? bottom : bottom - (bottom - top) * (float)layer / (float)(n - 1); };
    auto layerAt = [&](float y) { return std::clamp((int)std::lround((bottom - y) / (bottom - top) * (n - 1)), 0, n - 1); };
    dl->AddLine(ImVec2(cx, top), ImVec2(cx, bottom), col::Chip, 4 * S);
    dl->AddLine(ImVec2(cx, yOf(slice_.layerHi)), ImVec2(cx, yOf(slice_.layerLo)), sl::orangeU(), 4 * S);
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(ImVec2(cx - 14 * S, top - 10 * S));
    ImGui::InvisibleButton("##track", ImVec2(28 * S, bottom - top + 20 * S));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
        int l = layerAt(io.MousePos.y);
        slice_.dragSlider = std::abs(l - slice_.layerHi) <= std::abs(l - slice_.layerLo) ? 1 : 2;
    }
    int oldHi = slice_.layerHi, oldLo = slice_.layerLo;
    if (ImGui::IsItemActive() && slice_.dragSlider) {
        int l = layerAt(io.MousePos.y);
        if (slice_.dragSlider == 1) slice_.layerHi = std::max(l, slice_.layerLo);
        else slice_.layerLo = std::min(l, slice_.layerHi);
    }
    if (!ImGui::IsItemActive()) slice_.dragSlider = 0;
    if (hovered && io.MouseWheel != 0.f) slice_.layerHi = std::clamp(slice_.layerHi + (io.MouseWheel > 0 ? 1 : -1), slice_.layerLo, n - 1);
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) slice_.layerHi = std::min(slice_.layerHi + 1, n - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) slice_.layerHi = std::max(slice_.layerHi - 1, slice_.layerLo);
    }
    for (int hnd = 0; hnd < 2; ++hnd) {
        int l = hnd == 0 ? slice_.layerHi : slice_.layerLo;
        float y = yOf(l);
        dl->AddCircleFilled(ImVec2(cx, y), 7 * S, col::Panel);
        dl->AddCircle(ImVec2(cx, y), 7 * S, sl::orangeU(), 0, 2.5f * S);
        char t[48];
        const auto& Lr = slice_.paths->layers[(size_t)l];
        std::snprintf(t, sizeof t, "%d\n%.2f", l + 1, Lr.z);
        ImVec2 ts = ImGui::CalcTextSize(t);
        // The top box above its handle, the bottom one below (Orca).
        float bw = std::max(ts.x, 40 * S) + 10 * S;
        ImVec2 a(cx - 16 * S - bw, hnd == 0 ? y - ts.y - 10 * S : y + 2 * S);
        dl->AddRectFilled(a, ImVec2(a.x + bw, a.y + ts.y + 6 * S), col::Card, 4 * S);
        dl->AddRect(a, ImVec2(a.x + bw, a.y + ts.y + 6 * S), hnd == 0 ? sl::orangeU() : col::PanelBorder, 4 * S);
        dl->AddText(ImVec2(a.x + (bw - ts.x) * 0.5f, a.y + 3 * S), col::Text, t);
    }
    if (slice_.layerHi != oldHi || slice_.layerLo != oldLo) {
        slice_.moveHi = -1;   // a new top layer starts fully drawn
        sceneDirty_ = true;
        activity();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void App::drawSliceMoveSlider() {
    // Along the bottom: how far into the top layer the nozzle has got (Orca's horizontal slider).
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!slice_.paths || slice_.layerHi < 0 || slice_.layerHi >= (int)slice_.paths->layers.size()) return;
    int total = (int)slice_.paths->layers[(size_t)slice_.layerHi].segs.size();
    if (total <= 0) return;
    float viewL = vp->Pos.x + sliceSideW(), viewR = vp->Pos.x + vp->Size.x;
    float left = viewL + (viewR - viewL) * 0.22f, right = viewL + (viewR - viewL) * 0.78f;
    float y = vp->Pos.y + vp->Size.y - 26 * S;
    ImGui::SetNextWindowPos(ImVec2(left - 20 * S, y - 20 * S));
    ImGui::SetNextWindowSize(ImVec2(right - left + 110 * S, 40 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##moveslider", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int cur = slice_.moveHi < 0 ? total : std::min(slice_.moveHi, total);
    auto xOf = [&](int v) { return left + (right - left) * (float)v / (float)total; };
    dl->AddLine(ImVec2(left, y), ImVec2(right, y), col::Chip, 4 * S);
    dl->AddLine(ImVec2(left, y), ImVec2(xOf(cur), y), sl::orangeU(), 4 * S);
    ImGui::SetCursorScreenPos(ImVec2(left - 10 * S, y - 12 * S));
    ImGui::InvisibleButton("##mtrack", ImVec2(right - left + 20 * S, 24 * S));
    ImGuiIO& io = ImGui::GetIO();
    int before = cur;
    if (ImGui::IsItemActive()) cur = std::clamp((int)std::lround((io.MousePos.x - left) / (right - left) * total), 0, total);
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) cur = std::max(cur - 1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) cur = std::min(cur + 1, total);
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.f) cur = std::clamp(cur + (io.MouseWheel > 0 ? 1 : -1) * std::max(1, total / 100), 0, total);
    float hx = xOf(cur);
    dl->AddCircleFilled(ImVec2(hx, y), 7 * S, col::Panel);
    dl->AddCircle(ImVec2(hx, y), 7 * S, sl::orangeU(), 0, 2.5f * S);
    char t[32];
    std::snprintf(t, sizeof t, "%d", cur);
    ImVec2 ts = ImGui::CalcTextSize(t);
    ImVec2 a(right + 16 * S, y - ts.y * 0.5f - 3 * S);
    dl->AddRectFilled(a, ImVec2(a.x + ts.x + 12 * S, a.y + ts.y + 6 * S), col::Card, 4 * S);
    dl->AddRect(a, ImVec2(a.x + ts.x + 12 * S, a.y + ts.y + 6 * S), sl::orangeU(), 4 * S);
    dl->AddText(ImVec2(a.x + 6 * S, a.y + 3 * S), col::Text, t);
    if (cur != before) {
        slice_.moveHi = cur >= total ? -1 : cur;
        sceneDirty_ = true;
        activity();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace kst
