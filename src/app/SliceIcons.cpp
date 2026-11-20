// Line art icons for the Slice tab, in the spirit of OrcaSlicer's toolbar (thin strokes, one color).
#include "app/SliceStyle.h"

#include <cmath>

namespace kst::sl {

void drawOIcon(ImDrawList* dl, ImVec2 c, float size, OIcon icon, ImU32 col) {
    const float h = size * 0.5f;
    const float t = std::max(1.2f, size * 0.065f);   // stroke
    auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
    auto L = [&](float x0, float y0, float x1, float y1, float w = 0) { dl->AddLine(P(x0, y0), P(x1, y1), col, w > 0 ? w : t); };
    auto R = [&](float x0, float y0, float x1, float y1, float rr = 0.08f) { dl->AddRect(P(x0, y0), P(x1, y1), col, rr * h, 0, t); };
    auto RF = [&](float x0, float y0, float x1, float y1, float rr = 0.08f) { dl->AddRectFilled(P(x0, y0), P(x1, y1), col, rr * h); };
    auto C = [&](float x, float y, float r) { dl->AddCircle(P(x, y), r * h, col, 0, t); };
    auto CF = [&](float x, float y, float r) { dl->AddCircleFilled(P(x, y), r * h, col); };
    auto Arc = [&](float x, float y, float r, float a0, float a1) {
        dl->PathArcTo(P(x, y), r * h, a0, a1, 24);
        dl->PathStroke(col, 0, t);
    };
    auto Tri = [&](float x0, float y0, float x1, float y1, float x2, float y2) { dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), col); };
    auto Arrow = [&](float x0, float y0, float x1, float y1) {   // line with a head at (x1, y1)
        L(x0, y0, x1, y1);
        float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy);
        if (l < 1e-4f) return;
        dx /= l; dy /= l;
        float s = 0.28f;
        Tri(x1 + dx * 0.05f, y1 + dy * 0.05f, x1 - dx * s - dy * s * 0.6f, y1 - dy * s + dx * s * 0.6f, x1 - dx * s + dy * s * 0.6f, y1 - dy * s - dx * s * 0.6f);
    };
    // An isometric cube outline centered at (x, y), half size s.
    auto Cube = [&](float x, float y, float s) {
        ImVec2 top[4] = {P(x, y - s), P(x + s * 0.87f, y - s * 0.5f), P(x, y), P(x - s * 0.87f, y - s * 0.5f)};
        dl->AddPolyline(top, 4, col, ImDrawFlags_Closed, t);
        L(x - s * 0.87f, y - s * 0.5f, x - s * 0.87f, y + s * 0.5f);
        L(x + s * 0.87f, y - s * 0.5f, x + s * 0.87f, y + s * 0.5f);
        L(x, y, x, y + s);
        L(x - s * 0.87f, y + s * 0.5f, x, y + s);
        L(x + s * 0.87f, y + s * 0.5f, x, y + s);
    };
    auto Plus = [&](float x, float y, float s) { L(x - s, y, x + s, y); L(x, y - s, x, y + s); };
    auto Grid = [&](float x0, float y0, float x1, float y1, int n) {
        R(x0, y0, x1, y1, 0.04f);
        for (int i = 1; i < n; ++i) {
            float u = x0 + (x1 - x0) * i / n, v = y0 + (y1 - y0) * i / n;
            L(u, y0, u, y1, t * 0.7f);
            L(x0, v, x1, v, t * 0.7f);
        }
    };
    switch (icon) {
    case OIcon::AddObj: Cube(-0.15f, -0.1f, 0.62f); CF(0.55f, 0.55f, 0.38f); {
            ImU32 bg = IM_COL32(30, 30, 34, 255);
            dl->AddLine(P(0.35f, 0.55f), P(0.75f, 0.55f), bg, t * 1.2f);
            dl->AddLine(P(0.55f, 0.35f), P(0.55f, 0.75f), bg, t * 1.2f);
        } break;
    case OIcon::AddPlate: Grid(-0.85f, -0.75f, 0.45f, 0.55f, 3); CF(0.55f, 0.55f, 0.38f); {
            ImU32 bg = IM_COL32(30, 30, 34, 255);
            dl->AddLine(P(0.35f, 0.55f), P(0.75f, 0.55f), bg, t * 1.2f);
            dl->AddLine(P(0.55f, 0.35f), P(0.55f, 0.75f), bg, t * 1.2f);
        } break;
    case OIcon::Orient: L(-0.9f, 0.75f, 0.9f, 0.75f); {
            ImVec2 q[4] = {P(-0.45f, 0.5f), P(0.25f, 0.2f), P(0.5f, -0.45f), P(-0.2f, -0.15f)};
            dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t);
        } Arc(0.f, 0.f, 0.85f, -2.6f, -1.2f); Tri(0.32f, -0.86f, 0.05f, -0.98f, 0.12f, -0.66f); break;
    case OIcon::Arrange: R(-0.85f, -0.85f, -0.1f, -0.1f); R(0.1f, -0.85f, 0.85f, -0.1f); R(-0.85f, 0.1f, -0.1f, 0.85f); R(0.1f, 0.1f, 0.85f, 0.85f); break;
    case OIcon::SplitObj: Cube(-0.38f, -0.15f, 0.45f); Cube(0.42f, 0.15f, 0.45f); break;
    case OIcon::SplitPart: Cube(0.f, -0.05f, 0.75f); L(-0.65f, 0.05f, 0.65f, 0.05f); break;
    case OIcon::VarLayer:
        for (int i = 0; i < 5; ++i) { float y = -0.75f + i * 0.37f; float w = 0.85f - std::abs(i - 2) * 0.12f; L(-w, y, w * 0.6f, y); }
        Arc(0.55f, 0.f, 0.75f, -1.2f, 1.2f); break;
    case OIcon::Move: Arrow(0, 0, 0, -0.9f); Arrow(0, 0, 0, 0.9f); Arrow(0, 0, -0.9f, 0); Arrow(0, 0, 0.9f, 0); break;
    case OIcon::Rotate: Arc(0, 0, 0.7f, -0.3f, 4.6f); Tri(0.7f, -0.55f, 0.38f, -0.15f, 0.98f, -0.08f); break;
    case OIcon::Scale: R(-0.85f, -0.15f, 0.15f, 0.85f); Arrow(-0.1f, 0.1f, 0.85f, -0.85f); break;
    case OIcon::LayFace: L(-0.9f, 0.8f, 0.9f, 0.8f); {
            ImVec2 q[4] = {P(-0.55f, 0.6f), P(0.55f, 0.6f), P(0.55f, -0.1f), P(-0.55f, -0.1f)};
            dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t);
        } Arrow(0, -0.95f, 0, -0.25f); break;
    case OIcon::Cut: Cube(0.f, 0.05f, 0.7f); L(-0.95f, -0.15f, 0.95f, -0.15f, t * 1.3f); break;
    case OIcon::Support:
        L(-0.7f, -0.7f, 0.7f, -0.7f); L(-0.45f, -0.7f, -0.45f, 0.85f); L(0.45f, -0.7f, 0.45f, 0.85f);
        L(-0.45f, 0.1f, 0.45f, -0.4f, t * 0.7f); L(-0.45f, -0.4f, 0.45f, 0.1f, t * 0.7f); L(-0.75f, 0.85f, 0.75f, 0.85f); break;
    case OIcon::Seam:
        C(0, 0, 0.75f); L(0.f, -0.75f, 0.f, -0.15f, t * 1.4f); CF(0.f, -0.75f, 0.14f); break;
    case OIcon::Paint:
        R(-0.15f, -0.95f, 0.85f, -0.35f, 0.15f); L(0.35f, -0.35f, 0.35f, 0.05f); L(0.35f, 0.05f, -0.3f, 0.05f); L(-0.3f, 0.05f, -0.3f, 0.3f);
        RF(-0.45f, 0.3f, -0.15f, 0.95f, 0.1f); break;
    case OIcon::Measure: {
            ImVec2 q[4] = {P(-0.95f, 0.35f), P(0.35f, -0.95f), P(0.95f, -0.35f), P(-0.35f, 0.95f)};
            dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t);
            for (int i = 1; i < 5; ++i) { float u = -0.95f + i * 0.26f, v = 0.35f - i * 0.26f; L(u, v, u + 0.18f, v + 0.18f, t * 0.8f); }
        } break;
    case OIcon::Text: L(-0.7f, -0.75f, 0.7f, -0.75f, t * 1.3f); L(0, -0.75f, 0, 0.8f, t * 1.3f); L(-0.3f, 0.8f, 0.3f, 0.8f); break;
    case OIcon::Layers:
        for (int i = 0; i < 3; ++i) { float y = -0.45f + i * 0.45f; ImVec2 q[4] = {P(-0.85f, y), P(0, y - 0.35f), P(0.85f, y), P(0, y + 0.35f)}; dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t); }
        break;
    case OIcon::Fuzzy: for (int i = 0; i < 8; ++i) { float x0 = -0.9f + i * 0.225f; L(x0, (i % 2) ? -0.3f : 0.3f, x0 + 0.225f, (i % 2) ? 0.3f : -0.3f); } break;
    case OIcon::Close: L(-0.6f, -0.6f, 0.6f, 0.6f); L(-0.6f, 0.6f, 0.6f, -0.6f); break;
    case OIcon::Lock: R(-0.6f, -0.05f, 0.6f, 0.85f, 0.12f); Arc(0, -0.05f, 0.4f, 3.14159f, 6.28318f); L(-0.4f, -0.05f, -0.4f, -0.3f); L(0.4f, -0.05f, 0.4f, -0.3f); CF(0, 0.4f, 0.1f); break;
    case OIcon::Unlock: R(-0.6f, -0.05f, 0.6f, 0.85f, 0.12f); Arc(0.3f, -0.35f, 0.4f, 3.14159f, 6.28318f); L(-0.1f, -0.35f, -0.1f, -0.05f); CF(0, 0.4f, 0.1f); break;
    case OIcon::PlateSet:
        L(-0.8f, -0.5f, 0.8f, -0.5f); L(-0.8f, 0.0f, 0.8f, 0.0f); L(-0.8f, 0.5f, 0.8f, 0.5f);
        CF(-0.35f, -0.5f, 0.17f); CF(0.4f, 0.0f, 0.17f); CF(-0.1f, 0.5f, 0.17f); break;
    case OIcon::ToFront: L(-0.75f, -0.8f, 0.75f, -0.8f); Arrow(0, 0.85f, 0, -0.55f); break;
    case OIcon::Printer:
        R(-0.85f, -0.85f, 0.85f, 0.85f, 0.1f); L(-0.85f, -0.45f, 0.85f, -0.45f); RF(-0.2f, -0.45f, 0.2f, -0.15f, 0.02f); L(0, -0.15f, 0, 0.05f);
        L(-0.6f, 0.6f, 0.6f, 0.6f); break;
    case OIcon::Filament: C(0, 0, 0.8f); C(0, 0, 0.3f); L(0.8f, 0, 0.95f, 0.85f); break;
    case OIcon::Process: for (int i = 0; i < 4; ++i) { float y = -0.75f + i * 0.5f; L(-0.85f, y, 0.85f, y); } break;
    case OIcon::Plus: Plus(0, 0, 0.7f); break;
    case OIcon::Minus: L(-0.7f, 0, 0.7f, 0); break;
    case OIcon::Sync: Arc(0, 0, 0.7f, 3.6f, 6.0f); Arc(0, 0, 0.7f, 0.45f, 2.85f); Tri(0.7f, 0.05f, 0.4f, -0.3f, 0.95f, -0.35f); Tri(-0.7f, -0.05f, -0.4f, 0.3f, -0.95f, 0.35f); break;
    case OIcon::Gear: {
            C(0, 0, 0.32f);
            for (int i = 0; i < 8; ++i) {
                float a = i * 0.785398f;
                L(std::cos(a) * 0.55f, std::sin(a) * 0.55f, std::cos(a) * 0.9f, std::sin(a) * 0.9f, t * 1.8f);
            }
            C(0, 0, 0.62f);
        } break;
    case OIcon::Search: C(-0.15f, -0.15f, 0.6f); L(0.3f, 0.3f, 0.85f, 0.85f, t * 1.4f); break;
    case OIcon::Save: R(-0.8f, -0.8f, 0.8f, 0.8f, 0.1f); R(-0.45f, -0.8f, 0.45f, -0.25f, 0.02f); R(-0.5f, 0.15f, 0.5f, 0.8f, 0.02f); break;
    case OIcon::Compare: R(-0.85f, -0.7f, -0.05f, 0.7f, 0.06f); R(0.05f, -0.7f, 0.85f, 0.7f, 0.06f); L(-0.65f, -0.2f, -0.25f, -0.2f); L(0.25f, -0.2f, 0.65f, -0.2f); L(0.25f, 0.2f, 0.65f, 0.2f); break;
    case OIcon::Eye: case OIcon::EyeOff: {
            dl->PathArcTo(P(0, 0.9f), 1.2f * h, -2.2f, -0.94f, 16); dl->PathStroke(col, 0, t);
            dl->PathArcTo(P(0, -0.9f), 1.2f * h, 0.94f, 2.2f, 16); dl->PathStroke(col, 0, t);
            CF(0, 0, 0.22f);
            if (icon == OIcon::EyeOff) L(-0.8f, 0.8f, 0.8f, -0.8f);
        } break;
    case OIcon::Menu: L(-0.7f, -0.5f, 0.7f, -0.5f); L(-0.7f, 0, 0.7f, 0); L(-0.7f, 0.5f, 0.7f, 0.5f); break;
    case OIcon::Zoom: C(-0.15f, -0.15f, 0.6f); L(0.3f, 0.3f, 0.85f, 0.85f, t * 1.4f); break;
    case OIcon::Home: {
            ImVec2 q[5] = {P(-0.8f, -0.05f), P(0, -0.8f), P(0.8f, -0.05f), P(0.8f, 0.0f), P(-0.8f, 0.0f)};
            dl->AddPolyline(q, 3, col, 0, t);
            (void)q;
            R(-0.55f, -0.2f, 0.55f, 0.8f, 0.04f); R(-0.15f, 0.3f, 0.15f, 0.8f, 0.02f);
        } break;
    case OIcon::Prepare: Cube(0, 0, 0.8f); break;
    case OIcon::Preview: for (int i = 0; i < 3; ++i) { float y = -0.45f + i * 0.45f; ImVec2 q[4] = {P(-0.85f, y), P(0, y - 0.35f), P(0.85f, y), P(0, y + 0.35f)}; dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t); } break;
    case OIcon::Device: R(-0.8f, -0.8f, 0.8f, 0.8f, 0.1f); R(-0.45f, -0.45f, 0.45f, 0.25f, 0.04f); L(-0.45f, 0.55f, 0.45f, 0.55f); break;
    case OIcon::Project: R(-0.7f, -0.85f, 0.7f, 0.85f, 0.08f); L(-0.4f, -0.45f, 0.4f, -0.45f); L(-0.4f, -0.05f, 0.4f, -0.05f); L(-0.4f, 0.35f, 0.15f, 0.35f); break;
    case OIcon::Calibrate: R(-0.8f, -0.8f, 0.8f, 0.8f, 0.06f); L(-0.8f, 0, -0.45f, 0); L(0.45f, 0, 0.8f, 0); L(0, -0.8f, 0, -0.45f); L(0, 0.45f, 0, 0.8f); C(0, 0, 0.25f); break;
    case OIcon::Bed: L(-0.9f, 0.35f, 0.9f, 0.35f, t * 1.4f); for (int i = 0; i < 3; ++i) { float x = -0.5f + i * 0.5f; Arc(x, -0.35f, 0.18f, 1.57f, 4.71f); Arc(x, -0.05f, 0.18f, -1.57f, 1.57f); } break;
    case OIcon::Pencil: L(-0.7f, 0.7f, 0.5f, -0.5f, t * 1.6f); Tri(-0.85f, 0.85f, -0.75f, 0.45f, -0.45f, 0.75f); break;
    case OIcon::Fan: C(0, 0, 0.18f); for (int i = 0; i < 3; ++i) { float a = i * 2.094f; Arc(std::cos(a) * 0.45f, std::sin(a) * 0.45f, 0.4f, a + 1.2f, a + 3.6f); } break;
    case OIcon::Lamp: Arc(0, -0.2f, 0.55f, 3.6f, 5.82f); L(-0.45f, 0.1f, -0.3f, 0.5f); L(0.45f, 0.1f, 0.3f, 0.5f); L(-0.3f, 0.5f, 0.3f, 0.5f); L(-0.2f, 0.75f, 0.2f, 0.75f); break;
    case OIcon::Nozzle: R(-0.55f, -0.85f, 0.55f, -0.2f, 0.06f); {
            ImVec2 q[4] = {P(-0.4f, -0.2f), P(0.4f, -0.2f), P(0.12f, 0.5f), P(-0.12f, 0.5f)};
            dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t);
        } L(0, 0.6f, 0, 0.9f); break;
    case OIcon::BedTemp: L(-0.85f, 0.7f, 0.85f, 0.7f, t * 1.4f); for (int i = 0; i < 3; ++i) { float x = -0.5f + i * 0.5f; L(x, 0.4f, x, -0.6f); } break;
    case OIcon::Chamber: R(-0.8f, -0.8f, 0.8f, 0.8f, 0.08f); for (int i = 0; i < 3; ++i) { float x = -0.4f + i * 0.4f; L(x, 0.5f, x, -0.4f); } break;
    case OIcon::Camera: R(-0.85f, -0.5f, 0.45f, 0.5f, 0.1f); Tri(0.5f, 0.f, 0.9f, -0.4f, 0.9f, 0.4f); break;
    case OIcon::Folder: {
            ImVec2 q[6] = {P(-0.85f, -0.6f), P(-0.3f, -0.6f), P(-0.15f, -0.4f), P(0.85f, -0.4f), P(0.85f, 0.7f), P(-0.85f, 0.7f)};
            dl->AddPolyline(q, 6, col, ImDrawFlags_Closed, t);
        } break;
    case OIcon::Play: Tri(-0.45f, -0.7f, 0.7f, 0.f, -0.45f, 0.7f); break;
    case OIcon::Pause: RF(-0.5f, -0.65f, -0.15f, 0.65f, 0.02f); RF(0.15f, -0.65f, 0.5f, 0.65f, 0.02f); break;
    case OIcon::Stop: RF(-0.55f, -0.55f, 0.55f, 0.55f, 0.06f); break;
    case OIcon::Wifi: Arc(0, 0.6f, 1.1f, -2.4f, -0.74f); Arc(0, 0.6f, 0.72f, -2.4f, -0.74f); Arc(0, 0.6f, 0.36f, -2.4f, -0.74f); CF(0, 0.6f, 0.1f); break;
    case OIcon::Ams: for (int i = 0; i < 4; ++i) { float x = -0.75f + i * 0.5f; R(x - 0.18f, -0.7f, x + 0.18f, 0.7f, 0.3f); } break;
    case OIcon::Edit: CF(-0.55f, 0, 0.12f); CF(0, 0, 0.12f); CF(0.55f, 0, 0.12f); break;
    }
}

} // namespace kst::sl
