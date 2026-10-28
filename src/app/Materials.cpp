// Appearances: the material library and the Appearance panel (Fusion's A). Pick bodies or faces
// and click a swatch, or drag a swatch straight onto the model. Every look can be tuned: color,
// see through, metal, roughness, glow and a procedural texture (wood, carbon fiber, brushed,
// knurl, marble, speckle, hex, tread plate, checker). All changes are undoable and saved.
#include "app/Materials.h"
#include "app/App.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>

namespace kst {

using namespace ui;

namespace {

MatProps mp(float rough, float metal = 0.f, float opacity = 1.f, MatPattern pat = MatPattern::None,
            glm::vec3 c2 = {0.25f, 0.25f, 0.27f}, float scale = 10.f, float glow = 0.f) {
    MatProps m;
    m.rough = rough;
    m.metal = metal;
    m.opacity = opacity;
    m.pattern = pat;
    m.color2 = c2;
    m.scale = scale;
    m.glow = glow;
    return m;
}

std::vector<LibMaterial> buildLibrary() {
    using P = MatPattern;
    std::vector<LibMaterial> v = {
        // Plastics and 3D printing
        {"Plastic", "Plastic - Glossy White", {0.92f, 0.92f, 0.91f}, mp(0.22f)},
        {"Plastic", "Plastic - Glossy Black", {0.035f, 0.035f, 0.04f}, mp(0.2f)},
        {"Plastic", "Plastic - Matte Black", {0.06f, 0.06f, 0.065f}, mp(0.78f)},
        {"Plastic", "Plastic - Matte Gray", {0.5f, 0.51f, 0.53f}, mp(0.7f)},
        {"Plastic", "Plastic - Matte White", {0.86f, 0.86f, 0.85f}, mp(0.75f)},
        {"Plastic", "Plastic - Red", {0.78f, 0.1f, 0.08f}, mp(0.35f)},
        {"Plastic", "Plastic - Orange", {0.96f, 0.42f, 0.07f}, mp(0.35f)},
        {"Plastic", "Plastic - Yellow", {0.96f, 0.78f, 0.1f}, mp(0.35f)},
        {"Plastic", "Plastic - Green", {0.14f, 0.58f, 0.24f}, mp(0.35f)},
        {"Plastic", "Plastic - Blue", {0.1f, 0.33f, 0.8f}, mp(0.35f)},
        {"Plastic", "Plastic - Purple", {0.45f, 0.2f, 0.7f}, mp(0.35f)},
        {"Plastic", "Plastic - Textured Black", {0.07f, 0.07f, 0.075f}, mp(0.85f, 0.f, 1.f, P::Speckle, {0.12f, 0.12f, 0.13f}, 1.5f)},
        {"Plastic", "Plastic - Textured Gray", {0.45f, 0.46f, 0.48f}, mp(0.85f, 0.f, 1.f, P::Speckle, {0.38f, 0.39f, 0.4f}, 1.5f)},
        {"Plastic", "PLA - Silk Silver", {0.78f, 0.79f, 0.8f}, mp(0.25f, 0.55f)},
        {"Plastic", "PLA - Silk Gold", {0.9f, 0.7f, 0.3f}, mp(0.25f, 0.55f)},
        {"Plastic", "PETG - Clear", {0.9f, 0.95f, 0.97f}, mp(0.08f, 0.f, 0.3f)},
        {"Plastic", "PETG - Translucent Blue", {0.2f, 0.45f, 0.9f}, mp(0.12f, 0.f, 0.5f)},
        {"Plastic", "PETG - Translucent Orange", {0.98f, 0.45f, 0.08f}, mp(0.12f, 0.f, 0.55f)},
        {"Plastic", "TPU - Black", {0.05f, 0.05f, 0.05f}, mp(0.9f)},
        {"Plastic", "Resin - Gray", {0.56f, 0.57f, 0.59f}, mp(0.4f)},
        // Metals
        {"Metal", "Aluminum - Polished", {0.91f, 0.92f, 0.92f}, mp(0.12f, 1.f)},
        {"Metal", "Aluminum - Satin", {0.86f, 0.87f, 0.88f}, mp(0.42f, 1.f)},
        {"Metal", "Aluminum - Brushed", {0.88f, 0.89f, 0.9f}, mp(0.3f, 1.f, 1.f, P::Brushed)},
        {"Metal", "Aluminum - Anodized Black", {0.07f, 0.07f, 0.08f}, mp(0.35f, 0.9f)},
        {"Metal", "Aluminum - Anodized Gray", {0.42f, 0.43f, 0.45f}, mp(0.35f, 0.9f)},
        {"Metal", "Aluminum - Anodized Red", {0.62f, 0.05f, 0.05f}, mp(0.32f, 0.9f)},
        {"Metal", "Aluminum - Anodized Blue", {0.08f, 0.22f, 0.62f}, mp(0.32f, 0.9f)},
        {"Metal", "Steel - Polished", {0.77f, 0.78f, 0.78f}, mp(0.14f, 1.f)},
        {"Metal", "Steel - Brushed", {0.72f, 0.73f, 0.74f}, mp(0.3f, 1.f, 1.f, P::Brushed)},
        {"Metal", "Steel - Stainless", {0.68f, 0.68f, 0.7f}, mp(0.24f, 1.f)},
        {"Metal", "Steel - Galvanized", {0.7f, 0.72f, 0.73f}, mp(0.5f, 0.9f, 1.f, P::Speckle, {0.82f, 0.83f, 0.84f}, 4.f)},
        {"Metal", "Steel - Black Oxide", {0.12f, 0.12f, 0.13f}, mp(0.5f, 0.8f)},
        {"Metal", "Iron - Cast", {0.34f, 0.34f, 0.35f}, mp(0.7f, 0.8f, 1.f, P::Speckle, {0.25f, 0.25f, 0.26f}, 1.f)},
        {"Metal", "Chrome", {0.96f, 0.96f, 0.97f}, mp(0.04f, 1.f)},
        {"Metal", "Gold", {1.f, 0.78f, 0.34f}, mp(0.18f, 1.f)},
        {"Metal", "Copper", {0.95f, 0.64f, 0.54f}, mp(0.25f, 1.f)},
        {"Metal", "Brass", {0.91f, 0.78f, 0.42f}, mp(0.25f, 1.f)},
        {"Metal", "Bronze", {0.8f, 0.5f, 0.25f}, mp(0.35f, 1.f)},
        {"Metal", "Titanium", {0.62f, 0.6f, 0.57f}, mp(0.35f, 1.f)},
        {"Metal", "Tread Plate", {0.85f, 0.86f, 0.87f}, mp(0.3f, 1.f, 1.f, P::DiamondPlate, {0.6f, 0.6f, 0.6f}, 20.f)},
        {"Metal", "Knurled Steel", {0.74f, 0.75f, 0.76f}, mp(0.3f, 1.f, 1.f, P::Knurl, {0.35f, 0.35f, 0.36f}, 1.2f)},
        {"Metal", "Hex Mesh", {0.2f, 0.2f, 0.22f}, mp(0.4f, 0.9f, 1.f, P::Hex, {0.75f, 0.76f, 0.78f}, 5.f)},
        // Glass and clear parts
        {"Glass", "Glass - Clear", {0.95f, 0.97f, 0.98f}, mp(0.03f, 0.f, 0.14f)},
        {"Glass", "Glass - Blue Tint", {0.4f, 0.6f, 0.9f}, mp(0.03f, 0.f, 0.3f)},
        {"Glass", "Glass - Green Tint", {0.45f, 0.75f, 0.55f}, mp(0.03f, 0.f, 0.3f)},
        {"Glass", "Glass - Smoked", {0.12f, 0.13f, 0.15f}, mp(0.04f, 0.f, 0.5f)},
        {"Glass", "Glass - Frosted", {0.9f, 0.92f, 0.94f}, mp(0.6f, 0.f, 0.55f)},
        {"Glass", "Acrylic - Clear", {0.97f, 0.97f, 0.98f}, mp(0.06f, 0.f, 0.2f)},
        {"Glass", "Acrylic - Red", {0.9f, 0.1f, 0.08f}, mp(0.08f, 0.f, 0.55f)},
        {"Glass", "Acrylic - Smoke", {0.2f, 0.2f, 0.22f}, mp(0.08f, 0.f, 0.6f)},
        {"Glass", "Screen - Black Glass", {0.02f, 0.02f, 0.03f}, mp(0.04f)},
        // Wood
        {"Wood", "Wood - Oak", {0.72f, 0.53f, 0.32f}, mp(0.6f, 0.f, 1.f, P::Wood, {0.52f, 0.35f, 0.19f}, 6.f)},
        {"Wood", "Wood - Walnut", {0.42f, 0.27f, 0.16f}, mp(0.55f, 0.f, 1.f, P::Wood, {0.24f, 0.14f, 0.08f}, 6.f)},
        {"Wood", "Wood - Pine", {0.87f, 0.71f, 0.48f}, mp(0.6f, 0.f, 1.f, P::Wood, {0.72f, 0.51f, 0.29f}, 8.f)},
        {"Wood", "Wood - Cherry", {0.62f, 0.32f, 0.2f}, mp(0.45f, 0.f, 1.f, P::Wood, {0.45f, 0.2f, 0.12f}, 6.f)},
        {"Wood", "Wood - Maple", {0.9f, 0.78f, 0.6f}, mp(0.5f, 0.f, 1.f, P::Wood, {0.8f, 0.64f, 0.45f}, 9.f)},
        {"Wood", "Wood - Ebony", {0.12f, 0.09f, 0.07f}, mp(0.35f, 0.f, 1.f, P::Wood, {0.05f, 0.04f, 0.03f}, 5.f)},
        // Everything else
        {"Other", "Carbon Fiber", {0.14f, 0.14f, 0.15f}, mp(0.25f, 0.1f, 1.f, P::CarbonFiber, {0.02f, 0.02f, 0.025f}, 2.5f)},
        {"Other", "Rubber - Black", {0.055f, 0.055f, 0.055f}, mp(0.95f)},
        {"Other", "Rubber - Tire", {0.07f, 0.07f, 0.07f}, mp(0.9f, 0.f, 1.f, P::Speckle, {0.1f, 0.1f, 0.1f}, 0.8f)},
        {"Other", "Paint - Glossy Red", {0.75f, 0.04f, 0.04f}, mp(0.12f)},
        {"Other", "Paint - Glossy Blue", {0.05f, 0.18f, 0.6f}, mp(0.12f)},
        {"Other", "Paint - Matte Olive", {0.33f, 0.36f, 0.2f}, mp(0.8f)},
        {"Other", "Paint - Safety Yellow", {0.98f, 0.8f, 0.05f}, mp(0.3f)},
        {"Other", "Concrete", {0.6f, 0.6f, 0.58f}, mp(0.9f, 0.f, 1.f, P::Speckle, {0.45f, 0.45f, 0.43f}, 3.f)},
        {"Other", "Marble - White", {0.93f, 0.93f, 0.92f}, mp(0.2f, 0.f, 1.f, P::Marble, {0.5f, 0.52f, 0.55f}, 30.f)},
        {"Other", "Marble - Black", {0.08f, 0.08f, 0.09f}, mp(0.2f, 0.f, 1.f, P::Marble, {0.75f, 0.75f, 0.75f}, 30.f)},
        {"Other", "Granite", {0.35f, 0.35f, 0.36f}, mp(0.4f, 0.f, 1.f, P::Speckle, {0.8f, 0.8f, 0.8f}, 1.5f)},
        {"Other", "Ceramic - White", {0.95f, 0.95f, 0.94f}, mp(0.1f)},
        {"Other", "LED - White", {1.f, 1.f, 1.f}, mp(0.3f, 0.f, 1.f, P::None, {0.25f, 0.25f, 0.27f}, 10.f, 0.9f)},
        {"Other", "LED - Red", {1.f, 0.12f, 0.08f}, mp(0.3f, 0.f, 1.f, P::None, {0.25f, 0.25f, 0.27f}, 10.f, 0.9f)},
        {"Other", "LED - Green", {0.15f, 1.f, 0.3f}, mp(0.3f, 0.f, 1.f, P::None, {0.25f, 0.25f, 0.27f}, 10.f, 0.9f)},
        {"Other", "LED - Blue", {0.15f, 0.45f, 1.f}, mp(0.3f, 0.f, 1.f, P::None, {0.25f, 0.25f, 0.27f}, 10.f, 0.9f)},
        {"Other", "Checker (shape check)", {0.92f, 0.92f, 0.92f}, mp(0.4f, 0.f, 1.f, P::Checker, {0.12f, 0.12f, 0.13f}, 5.f)},
        {"Other", "Default (Kestral gray)", {0.72f, 0.74f, 0.77f}, MatProps{}},
    };
    for (auto& m : v) m.mat.name = m.name;
    return v;
}

ImU32 u32(const glm::vec3& c, float a = 1.f) {
    return IM_COL32((int)std::lround(std::clamp(c.r, 0.f, 1.f) * 255), (int)std::lround(std::clamp(c.g, 0.f, 1.f) * 255),
                    (int)std::lround(std::clamp(c.b, 0.f, 1.f) * 255), (int)std::lround(std::clamp(a, 0.f, 1.f) * 255));
}

// A little shaded ball that previews a look: highlight size from roughness, contrast from metal,
// a checkerboard behind glass, a hint of the texture.
void drawSwatch(ImDrawList* dl, ImVec2 c, float r, const glm::vec3& color, const MatProps& m, bool hot) {
    if (m.opacity < 0.999f) {
        float q = r * 0.5f;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                dl->AddRectFilled(ImVec2(c.x - r + i * q, c.y - r + j * q), ImVec2(c.x - r + (i + 1) * q, c.y - r + (j + 1) * q),
                                  ((i + j) & 1) ? IM_COL32(200, 200, 200, 255) : IM_COL32(255, 255, 255, 255));
    }
    if (m.glow > 0.f) dl->AddCircleFilled(c, r * 1.18f, u32(color, 0.35f * std::min(1.f, m.glow)), 32);
    float a = std::clamp(m.opacity, 0.15f, 1.f);
    glm::vec3 dark = color * (m.metal > 0.5f ? 0.35f : 0.6f);
    dl->AddCircleFilled(c, r, u32(dark, a), 32);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.12f), r * 0.85f, u32(color, a), 32);
    if (m.metal > 0.5f) {
        // A horizon band: what makes metal look like metal.
        dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y + r * 0.05f), ImVec2(c.x + r * 0.8f, c.y + r * 0.22f), u32(color * 0.45f, a * 0.8f));
    }
    if (m.pattern != MatPattern::None) {
        dl->PushClipRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), true);
        ImU32 pc = u32(m.color2, 0.7f * a);
        switch (m.pattern) {
        case MatPattern::Wood:
            for (int i = 1; i < 6; ++i) dl->AddCircle(ImVec2(c.x - r * 0.9f, c.y + r * 0.9f), r * 0.32f * i, pc, 24, 1.2f);
            break;
        case MatPattern::CarbonFiber:
            for (int i = 0; i < 6; ++i)
                for (int j = 0; j < 6; ++j)
                    if ((i + j) % 2) dl->AddRectFilled(ImVec2(c.x - r + i * r / 3, c.y - r + j * r / 3), ImVec2(c.x - r + (i + 1) * r / 3, c.y - r + (j + 1) * r / 3), pc);
            break;
        case MatPattern::Brushed:
            for (int i = 0; i < 10; ++i) dl->AddLine(ImVec2(c.x - r, c.y - r + i * r * 0.2f), ImVec2(c.x + r, c.y - r + i * r * 0.2f), u32(color * 0.8f, 0.5f), 1.f);
            break;
        case MatPattern::Knurl:
        case MatPattern::DiamondPlate:
            for (int i = -4; i <= 4; ++i) {
                dl->AddLine(ImVec2(c.x + i * r * 0.4f - r, c.y - r), ImVec2(c.x + i * r * 0.4f + r, c.y + r), pc, 1.f);
                dl->AddLine(ImVec2(c.x + i * r * 0.4f + r, c.y - r), ImVec2(c.x + i * r * 0.4f - r, c.y + r), pc, 1.f);
            }
            break;
        case MatPattern::Checker:
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    if ((i + j) % 2) dl->AddRectFilled(ImVec2(c.x - r + i * r / 2, c.y - r + j * r / 2), ImVec2(c.x - r + (i + 1) * r / 2, c.y - r + (j + 1) * r / 2), pc);
            break;
        case MatPattern::Marble:
            dl->AddBezierCubic(ImVec2(c.x - r, c.y - r * 0.2f), ImVec2(c.x - r * 0.2f, c.y - r * 0.8f), ImVec2(c.x + r * 0.1f, c.y + r * 0.6f), ImVec2(c.x + r, c.y), pc, 1.4f);
            dl->AddBezierCubic(ImVec2(c.x - r, c.y + r * 0.5f), ImVec2(c.x - r * 0.3f, c.y + r * 0.1f), ImVec2(c.x + r * 0.3f, c.y + r * 0.9f), ImVec2(c.x + r, c.y + r * 0.4f), pc, 1.f);
            break;
        case MatPattern::Speckle:
            for (int i = 0; i < 18; ++i) {
                float x = std::fmod(i * 0.618f, 1.f) * 2.f - 1.f, y = std::fmod(i * 0.371f + 0.2f, 1.f) * 2.f - 1.f;
                dl->AddCircleFilled(ImVec2(c.x + x * r * 0.8f, c.y + y * r * 0.8f), r * 0.06f, pc, 6);
            }
            break;
        case MatPattern::Hex:
            for (int i = -2; i <= 2; ++i)
                for (int j = -2; j <= 2; ++j)
                    dl->AddNgon(ImVec2(c.x + (i + (j & 1) * 0.5f) * r * 0.5f, c.y + j * r * 0.43f), r * 0.27f, pc, 6, 1.f);
            break;
        default: break;
        }
        dl->PopClipRect();
    }
    // Highlight: small and sharp when glossy, broad and dim when rough.
    float hs = r * (0.12f + m.rough * 0.35f);
    float ha = (1.f - m.rough * 0.75f) * (m.metal > 0.5f ? 0.95f : 0.75f);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.4f), hs, IM_COL32(255, 255, 255, (int)(255 * ha)), 20);
    dl->AddCircle(c, r, hot ? col::Accent : th(IM_COL32(170, 176, 186, 255)), 32, hot ? 2.f : 1.f);
}

} // namespace

const std::vector<LibMaterial>& materialLibrary() {
    static const std::vector<LibMaterial> lib = buildLibrary();
    return lib;
}

const LibMaterial* findLibMaterial(const std::string& name) {
    std::string n = toLower(name);
    const LibMaterial* partial = nullptr;
    int partials = 0;
    for (auto& m : materialLibrary()) {
        std::string ln = toLower(m.name);
        if (ln == n) return &m;
        if (!n.empty() && ln.find(n) != std::string::npos) { partial = &m; ++partials; }
    }
    return partials == 1 ? partial : (partials > 1 ? partial : nullptr);
}

const char* patternName(MatPattern p) {
    static const char* names[] = {"None", "Wood", "Carbon fiber", "Brushed", "Knurl", "Checker", "Marble", "Speckle", "Hex mesh", "Tread plate"};
    int i = (int)p;
    return i >= 0 && i < (int)MatPattern::Count ? names[i] : "None";
}

// ===================================================================== applying

void App::applyLook(const glm::vec3& color, const MatProps& m, bool toFaces, const std::vector<uint32_t>& bodies,
                    const std::set<std::pair<uint32_t, uint32_t>>& faces) {
    if (toFaces) {
        for (auto& [bid, face] : faces) {
            auto it = doc_.bodies.find(bid);
            if (it == doc_.bodies.end()) continue;
            it->second.faceLooks[face] = FaceLook{color, m};
            syncBodyOverride(bid);
        }
    } else {
        for (uint32_t id : bodies) {
            auto it = doc_.bodies.find(id);
            if (it == doc_.bodies.end()) continue;
            it->second.color = color;
            it->second.mat = m;
            syncBodyOverride(id);
        }
    }
    doc_.touch();
    dirty_ = true;
    markSceneDirty();
}

// ===================================================================== panel

void App::drawAppearance() {
    // A swatch dropped on the model applies to what is under the cursor.
    ImGuiIO& io = ImGui::GetIO();
    matDragOverView_ = false;
    if (matDrag_ >= 0) {
        bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const float S0 = uiScale_;
        ImGuiViewport* vp0 = ImGui::GetMainViewport();
        bool inView = io.MousePos.y > vp0->Pos.y + (34 + 76) * S0 && io.MousePos.y < vp0->Pos.y + vp0->Size.y - 38 * S0;
        matDragOverView_ = !overUi && inView;
        activity();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto& lib = materialLibrary();
            if (matDragOverView_ && hoverBody_ && matDrag_ < (int)lib.size() && doc_.bodies.count(hoverBody_)) {
                const LibMaterial& lm = lib[matDrag_];
                MatProps m = lm.mat;
                bool faces = !io.KeyCtrl && hoverFace_ > 0;   // a drop colors the face under it; Ctrl: the whole body
                if (!faces) m.fileColors = matKeepFileColors_;
                pushUndo("Appearance");
                applyLook(lm.color, m, faces, {hoverBody_}, {{hoverBody_, hoverFace_ - 1}});
                toast(std::string(lm.name) + (faces ? " on a face of " : " on ") + doc_.bodies[hoverBody_].name, ToastKind::Success);
            }
            matDrag_ = -1;
        }
    }
    if (matDrag_ >= 0 && matDrag_ < (int)materialLibrary().size()) {
        const LibMaterial& lm = materialLibrary()[matDrag_];
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float r = 14 * uiScale_;
        drawSwatch(fg, ImVec2(io.MousePos.x + r + 6, io.MousePos.y + r + 6), r, lm.color, lm.mat, true);
        if (matDragOverView_ && hoverBody_ && doc_.bodies.count(hoverBody_))
            ImGui::SetTooltip(!io.KeyCtrl && hoverFace_ > 0 ? "%s on a face of %s (Ctrl: whole body)" : "%s on %s",
                              lm.name, doc_.bodies[hoverBody_].name.c_str());
    }
    if (!appearanceOpen_) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 380 * S, vp->Pos.y + 270 * S), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(360 * S, 600 * S), ImGuiCond_Appearing);
    if (!ImGui::Begin("Appearance", &appearanceOpen_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    std::vector<uint32_t> bodies = matApplyFaces_ == 1 ? std::vector<uint32_t>{} : selectedBodies();
    bool haveTarget = matApplyFaces_ == 1 ? !selFaces_.empty() : !bodies.empty();

    // Apply to
    ImGui::TextUnformatted("Apply to:");
    ImGui::SameLine();
    ImGui::RadioButton("Bodies / components", &matApplyFaces_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Faces", &matApplyFaces_, 1);
    if (matApplyFaces_ == 0) {
        ImGui::Checkbox("Keep an imported file's face colors", &matKeepFileColors_);
        ImGui::SetItemTooltip("Off: the whole body takes the new look.\nOn: faces the file colored keep their color.");
    }
    if (!haveTarget)
        ImGui::TextDisabled(matApplyFaces_ == 1 ? "Select faces, or drag a swatch onto a face." : "Select bodies, or drag a swatch onto a face (Ctrl: whole body).");
    else
        ImGui::TextDisabled("%d %s selected", matApplyFaces_ == 1 ? (int)selFaces_.size() : (int)bodies.size(),
                            matApplyFaces_ == 1 ? "faces" : "bodies");

    auto applyLib = [&](const LibMaterial& lm) {
        if (!haveTarget) { toast("Select bodies or faces first, or drag the swatch onto the model"); return; }
        MatProps m = lm.mat;
        if (matApplyFaces_ == 0) m.fileColors = matKeepFileColors_;
        pushUndo("Appearance");
        applyLook(lm.color, m, matApplyFaces_ == 1, bodies, selFaces_);
        appearanceUndoPushed_ = false;
    };

    // In this design: every distinct look already used.
    ImGui::SeparatorText("In this design");
    {
        std::vector<std::pair<glm::vec3, MatProps>> used;
        for (auto& [id, b] : doc_.bodies) {
            if (!doc_.isBodyAlive(b)) continue;
            auto add = [&](const glm::vec3& c, const MatProps& m) {
                for (auto& u : used) if (glm::all(glm::equal(u.first, c)) && u.second == m) return;
                if (used.size() < 24) used.push_back({c, m});
            };
            add(b.color, b.mat);
            for (auto& [f, fl] : b.faceLooks) add(fl.color, fl.mat);
        }
        float r = 13 * S;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float avail = ImGui::GetContentRegionAvail().x;
        int perRow = std::max(1, (int)(avail / (2 * r + 6 * S)));
        for (size_t i = 0; i < used.size(); ++i) {
            if (i % perRow) ImGui::SameLine(0, 6 * S);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID((int)i + 5000);
            bool click = ImGui::InvisibleButton("##used", ImVec2(2 * r, 2 * r));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            drawSwatch(dl, ImVec2(p.x + r, p.y + r), r, used[i].first, used[i].second, hov);
            if (hov) ImGui::SetTooltip("%s", used[i].second.name.empty() ? "Custom color" : used[i].second.name.c_str());
            if (click && haveTarget) {
                pushUndo("Appearance");
                MatProps m = used[i].second;
                if (matApplyFaces_ == 0) m.fileColors = matKeepFileColors_;
                applyLook(used[i].first, m, matApplyFaces_ == 1, bodies, selFaces_);
            }
        }
        if (used.empty()) ImGui::TextDisabled("Nothing yet");
    }

    // Library
    ImGui::SeparatorText("Library");
    static const char* cats[] = {"All", "Plastic", "Metal", "Glass", "Wood", "Other"};
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine(0, 3 * S);
        bool on = matCategory_ == i;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(1.f, 0.42f, 0.21f, 1.f) : th(ImVec4(0.93f, 0.94f, 0.95f, 1.f)));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : th(ImVec4(0.3f, 0.33f, 0.37f, 1.f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9 * S);
        if (ImGui::SmallButton(cats[i])) matCategory_ = i;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##matsearch", "Search appearances", matSearch_, sizeof matSearch_);
    float libH = std::max(120 * S, ImGui::GetContentRegionAvail().y * 0.48f);
    ImGui::BeginChild("##lib", ImVec2(0, libH), ImGuiChildFlags_Borders);
    {
        const auto& lib = materialLibrary();
        float r = 17 * S, cell = 2 * r + 10 * S;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        int perRow = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cell));
        int n = 0;
        std::string q = toLower(matSearch_);
        for (int i = 0; i < (int)lib.size(); ++i) {
            const LibMaterial& lm = lib[i];
            if (matCategory_ > 0 && std::string(lm.category) != cats[matCategory_]) continue;
            if (!q.empty() && toLower(lm.name).find(q) == std::string::npos) continue;
            if (n % perRow) ImGui::SameLine(0, 0);
            ++n;
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            bool click = ImGui::InvisibleButton("##lm", ImVec2(cell, cell));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6 * S)) matDrag_ = i;
            ImGui::PopID();
            drawSwatch(dl, ImVec2(p.x + cell * 0.5f, p.y + cell * 0.5f), r, lm.color, lm.mat, hov);
            if (hov && matDrag_ < 0) ImGui::SetTooltip("%s\nClick: apply to the selection. Drag: drop it on the model.", lm.name);
            if (click && matDrag_ < 0) applyLib(lm);
        }
        if (n == 0) ImGui::TextDisabled("No appearance matches.");
    }
    ImGui::EndChild();

    // Editor: tune the look of the selection (first selected thing shows its values).
    ImGui::SeparatorText("Edit");
    const Body* first = nullptr;
    FaceLook look;
    bool haveLook = false;
    if (matApplyFaces_ == 1 && !selFaces_.empty()) {
        auto [bid, face] = *selFaces_.begin();
        auto it = doc_.bodies.find(bid);
        if (it != doc_.bodies.end()) {
            auto fl = it->second.faceLooks.find(face);
            look = fl != it->second.faceLooks.end() ? fl->second : FaceLook{it->second.color, it->second.mat};
            haveLook = true;
        }
    } else if (!bodies.empty()) {
        auto it = doc_.bodies.find(bodies[0]);
        if (it != doc_.bodies.end()) {
            first = &it->second;
            look = FaceLook{first->color, first->mat};
            haveLook = true;
        }
    }
    if (!haveLook) {
        ImGui::TextDisabled("Select something to edit its look.");
    } else {
        bool changed = false, started = false;
        auto track = [&] {
            if (ImGui::IsItemActivated()) started = true;
            if (ImGui::IsItemEdited()) changed = true;
        };
        float c[3] = {look.color.r, look.color.g, look.color.b};
        ImGui::SetNextItemWidth(-90 * S);
        ImGui::ColorEdit3("Color", c, ImGuiColorEditFlags_DisplayHex);
        track();
        look.color = {c[0], c[1], c[2]};
        MatProps& m = look.mat;
        float see = 1.f - m.opacity;
        ImGui::SetNextItemWidth(-90 * S);
        ImGui::SliderFloat("See through", &see, 0.f, 0.95f, "%.2f");
        track();
        m.opacity = 1.f - see;
        ImGui::SetNextItemWidth(-90 * S);
        ImGui::SliderFloat("Metal", &m.metal, 0.f, 1.f, "%.2f");
        track();
        ImGui::SetNextItemWidth(-90 * S);
        ImGui::SliderFloat("Roughness", &m.rough, 0.f, 1.f, "%.2f");
        track();
        ImGui::SetNextItemWidth(-90 * S);
        ImGui::SliderFloat("Glow", &m.glow, 0.f, 1.5f, "%.2f");
        track();
        int pat = (int)m.pattern;
        ImGui::SetNextItemWidth(-90 * S);
        if (ImGui::BeginCombo("Texture", patternName(m.pattern))) {
            for (int i = 0; i < (int)MatPattern::Count; ++i)
                if (ImGui::Selectable(patternName((MatPattern)i), i == pat)) { m.pattern = (MatPattern)i; changed = started = true; }
            ImGui::EndCombo();
        }
        if (m.pattern != MatPattern::None) {
            float c2[3] = {m.color2.r, m.color2.g, m.color2.b};
            ImGui::SetNextItemWidth(-90 * S);
            ImGui::ColorEdit3("Color 2", c2, ImGuiColorEditFlags_DisplayHex);
            track();
            m.color2 = {c2[0], c2[1], c2[2]};
            ImGui::SetNextItemWidth(-90 * S);
            ImGui::SliderFloat("Texture size", &m.scale, 0.2f, 100.f, "%.1f mm", ImGuiSliderFlags_Logarithmic);
            track();
        }
        if (changed) {
            if (started || !appearanceUndoPushed_) { pushUndo("Appearance"); appearanceUndoPushed_ = true; }
            m.name.clear();   // tuned: now a custom look
            if (matApplyFaces_ == 0) m.fileColors = matKeepFileColors_;
            applyLook(look.color, m, matApplyFaces_ == 1, bodies, selFaces_);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) appearanceUndoPushed_ = false;
        if (ImGui::Button("Reset to default")) {
            pushUndo("Appearance");
            if (matApplyFaces_ == 1) {
                for (auto& [bid, face] : selFaces_) {
                    auto it = doc_.bodies.find(bid);
                    if (it != doc_.bodies.end()) { it->second.faceLooks.erase(face); syncBodyOverride(bid); }
                }
                doc_.touch();
                markSceneDirty();
            } else {
                applyLook(glm::vec3(0.72f, 0.74f, 0.77f), MatProps{}, false, bodies, {});
            }
        }
        if (matApplyFaces_ == 0 && first && !first->faceLooks.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear face looks")) {
                pushUndo("Appearance");
                for (uint32_t id : bodies) {
                    auto it = doc_.bodies.find(id);
                    if (it != doc_.bodies.end()) { it->second.faceLooks.clear(); syncBodyOverride(id); }
                }
                doc_.touch();
                markSceneDirty();
            }
        }
    }
    ImGui::End();
}

} // namespace kst
