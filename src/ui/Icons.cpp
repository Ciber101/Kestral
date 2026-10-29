#include "ui/Icons.h"
#include "ui/Theme.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace kst::ui {

namespace {

struct Pen {
    ImDrawList* dl;
    ImVec2 c;
    float h;
    float a;   // alpha multiplier (disabled icons are faded)
    ImVec2 P(float x, float y) const { return ImVec2(c.x + x * h, c.y + y * h); }
    ImU32 col(int r, int g, int b, int al = 255) const { return thIcon(IM_COL32(r, g, b, (int)(al * a))); }
    float w(float t) const { return std::max(1.f, t * h); }

    void poly(std::initializer_list<ImVec2> pts, ImU32 fill, ImU32 stroke = 0, float th = 0.07f) const {
        ImVec2 buf[16];
        int n = 0;
        for (auto& p : pts) if (n < 16) buf[n++] = P(p.x, p.y);
        if (fill) dl->AddConvexPolyFilled(buf, n, fill);
        if (stroke) dl->AddPolyline(buf, n, stroke, ImDrawFlags_Closed, w(th));
    }
    void line(float x0, float y0, float x1, float y1, ImU32 cl, float th = 0.08f) const { dl->AddLine(P(x0, y0), P(x1, y1), cl, w(th)); }
    void circle(float x, float y, float r, ImU32 cl, bool fill, float th = 0.08f, int seg = 0) const {
        if (fill) dl->AddCircleFilled(P(x, y), r * h, cl, seg);
        else dl->AddCircle(P(x, y), r * h, cl, seg, w(th));
    }
    void rect(float x0, float y0, float x1, float y1, ImU32 cl, bool fill, float th = 0.08f, float round = 0.f) const {
        if (fill) dl->AddRectFilled(P(x0, y0), P(x1, y1), cl, round * h);
        else dl->AddRect(P(x0, y0), P(x1, y1), cl, round * h, 0, w(th));
    }
    void arrowUp(float x, float y0, float y1, ImU32 cl) const {
        line(x, y0, x, y1 + 0.12f, cl, 0.11f);
        poly({{x - 0.18f, y1 + 0.2f}, {x + 0.18f, y1 + 0.2f}, {x, y1}}, cl);
    }
    // Isometric block. y0 = top of block, height = vertical size.
    void box(float cx, float cy, float s, float hgt, ImU32 top, ImU32 left, ImU32 right, ImU32 edge) const {
        float dx = 0.62f * s, dy = 0.32f * s;
        ImVec2 t0{cx, cy - dy}, t1{cx + dx, cy}, t2{cx, cy + dy}, t3{cx - dx, cy};
        poly({t3, t2, {t2.x, t2.y + hgt}, {t3.x, t3.y + hgt}}, left, edge, 0.05f);
        poly({t2, t1, {t1.x, t1.y + hgt}, {t2.x, t2.y + hgt}}, right, edge, 0.05f);
        poly({t0, t1, t2, t3}, top, edge, 0.05f);
    }
    void stdBox(float cx = 0, float cy = 0.05f, float s = 1.f, float hgt = 0.55f) const {
        box(cx, cy, s, hgt, col(214, 222, 232), col(150, 164, 182), col(118, 134, 156), col(70, 84, 104));
    }
};

const int kBlue[3] = {43, 125, 233};
const int kSketch[3] = {18, 163, 127};
const int kAmber[3] = {232, 163, 61};
const int kPurple[3] = {142, 107, 216};
const int kOrange[3] = {240, 138, 36};
const int kSlate[3] = {88, 102, 122};
const int kPrint[3] = {255, 107, 53};

} // namespace

void drawIcon(ImDrawList* dl, ImVec2 center, float size, Icon icon, bool enabled) {
    Pen p{dl, center, size * 0.5f, enabled ? 1.f : 0.35f};
    ImU32 blue = p.col(kBlue[0], kBlue[1], kBlue[2]);
    ImU32 sketch = p.col(kSketch[0], kSketch[1], kSketch[2]);
    ImU32 amber = p.col(kAmber[0], kAmber[1], kAmber[2]);
    ImU32 purple = p.col(kPurple[0], kPurple[1], kPurple[2]);
    ImU32 orange = p.col(kOrange[0], kOrange[1], kOrange[2]);
    ImU32 slate = p.col(kSlate[0], kSlate[1], kSlate[2]);
    ImU32 print = p.col(kPrint[0], kPrint[1], kPrint[2]);
    ImU32 dark = p.col(60, 66, 76);
    ImU32 light = p.col(214, 222, 232);
    ImU32 white = p.col(255, 255, 255);

    switch (icon) {
    case Icon::None: break;
    case Icon::NewComponent:
        p.stdBox(0, 0.05f, 0.95f, 0.5f);
        p.circle(0.55f, -0.55f, 0.32f, purple, true);
        p.line(0.55f, -0.75f, 0.55f, -0.35f, white, 0.09f);
        p.line(0.35f, -0.55f, 0.75f, -0.55f, white, 0.09f);
        break;
    case Icon::Sketch:
        p.rect(-0.75f, -0.55f, 0.55f, 0.6f, p.col(232, 248, 242), true);
        p.rect(-0.75f, -0.55f, 0.55f, 0.6f, sketch, false, 0.09f);
        p.line(-0.2f, 0.45f, 0.75f, -0.5f, dark, 0.16f);
        p.poly({{-0.3f, 0.55f}, {-0.2f, 0.32f}, {-0.07f, 0.45f}}, orange);
        break;
    case Icon::Extrude:
        p.stdBox(0, 0.25f, 1.f, 0.35f);
        p.box(0, -0.15f, 0.6f, 0.4f, p.col(196, 220, 250), p.col(110, 160, 230), blue, p.col(30, 80, 160));
        p.arrowUp(0.62f, 0.2f, -0.85f, blue);
        break;
    case Icon::Revolve:
        p.rect(-0.35f, -0.6f, 0.15f, 0.6f, p.col(196, 220, 250), true);
        p.rect(-0.35f, -0.6f, 0.15f, 0.6f, blue, false);
        p.line(0.35f, -0.85f, 0.35f, 0.85f, dark, 0.06f);
        p.dl->PathArcTo(p.P(0.35f, 0.0f), 0.55f * p.h, 3.6f, 5.9f, 16);
        p.dl->PathStroke(blue, 0, p.w(0.11f));
        break;
    case Icon::Sweep:
        p.dl->AddBezierCubic(p.P(-0.8f, 0.6f), p.P(-0.3f, -0.6f), p.P(0.2f, 0.8f), p.P(0.8f, -0.4f), sketch, p.w(0.07f));
        p.circle(-0.8f, 0.6f, 0.25f, p.col(196, 220, 250), true);
        p.circle(-0.8f, 0.6f, 0.25f, blue, false);
        p.circle(0.8f, -0.4f, 0.25f, blue, true);
        break;
    case Icon::Loft:
        p.poly({{-0.7f, 0.5f}, {0.2f, 0.5f}, {0.7f, 0.2f}, {-0.2f, 0.2f}}, p.col(196, 220, 250), blue);
        p.circle(0.f, -0.45f, 0.3f, blue, false);
        p.line(-0.45f, 0.35f, -0.28f, -0.45f, slate, 0.05f);
        p.line(0.45f, 0.35f, 0.28f, -0.45f, slate, 0.05f);
        break;
    case Icon::Rib:
        p.poly({{-0.8f, 0.6f}, {0.8f, 0.6f}, {0.8f, 0.4f}, {-0.6f, 0.4f}, {-0.6f, -0.8f}, {-0.8f, -0.8f}}, light, dark, 0.05f);
        p.poly({{-0.6f, 0.4f}, {0.4f, 0.4f}, {-0.6f, -0.5f}}, p.col(196, 220, 250), blue);
        break;
    case Icon::Hole:
        p.stdBox(0, 0.f, 1.1f, 0.5f);
        p.dl->AddEllipseFilled(p.P(0, 0), ImVec2(0.32f * p.h, 0.16f * p.h), p.col(40, 48, 60));
        p.line(0, -0.75f, 0, -0.25f, blue, 0.09f);
        p.poly({{-0.13f, -0.35f}, {0.13f, -0.35f}, {0, -0.18f}}, blue);
        break;
    case Icon::Thread:
        p.rect(-0.3f, -0.8f, 0.3f, 0.8f, light, true);
        for (int i = 0; i < 6; ++i) {
            float y = -0.7f + i * 0.27f;
            p.line(-0.38f, y + 0.08f, 0.38f, y - 0.08f, dark, 0.08f);
        }
        break;
    case Icon::Box: p.stdBox(0, -0.05f, 1.15f, 0.6f); break;
    case Icon::Cylinder:
        p.rect(-0.55f, -0.45f, 0.55f, 0.45f, p.col(150, 164, 182), true);
        p.dl->AddEllipseFilled(p.P(0, 0.45f), ImVec2(0.55f * p.h, 0.2f * p.h), p.col(150, 164, 182));
        p.dl->AddEllipseFilled(p.P(0, -0.45f), ImVec2(0.55f * p.h, 0.2f * p.h), light);
        p.dl->AddEllipse(p.P(0, -0.45f), ImVec2(0.55f * p.h, 0.2f * p.h), dark, 0, 0, p.w(0.05f));
        break;
    case Icon::RectPattern:
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                p.rect(-0.8f + i * 0.58f, -0.8f + j * 0.58f, -0.38f + i * 0.58f, -0.38f + j * 0.58f,
                       (i == 0 && j == 2) ? blue : p.col(150, 164, 182), true, 0, 0.06f);
        break;
    case Icon::CircPattern:
        for (int i = 0; i < 6; ++i) {
            float a = i * 6.2831853f / 6.f;
            p.circle(std::cos(a) * 0.6f, std::sin(a) * 0.6f, 0.2f, i == 0 ? blue : p.col(150, 164, 182), true);
        }
        p.circle(0, 0, 0.08f, dark, true);
        break;
    case Icon::Mirror:
        p.poly({{-0.85f, 0.55f}, {-0.15f, 0.55f}, {-0.15f, -0.55f}}, blue);
        p.poly({{0.85f, 0.55f}, {0.15f, 0.55f}, {0.15f, -0.55f}}, p.col(150, 164, 182));
        p.line(0, -0.85f, 0, 0.85f, dark, 0.05f);
        break;
    case Icon::PressPull:
        p.stdBox(0, 0.15f, 1.1f, 0.45f);
        p.poly({{0.f, -0.17f}, {0.68f, 0.15f}, {0.f, 0.47f}, {-0.68f, 0.15f}}, p.col(255, 220, 160), amber, 0.05f);
        p.arrowUp(0, 0.15f, -0.9f, amber);
        break;
    case Icon::Fillet:
        p.dl->PathLineTo(p.P(-0.75f, 0.75f));
        p.dl->PathLineTo(p.P(-0.75f, -0.75f));
        p.dl->PathLineTo(p.P(-0.05f, -0.75f));
        p.dl->PathArcTo(p.P(-0.05f, 0.05f), 0.8f * p.h, -1.5708f, 0.f, 12);
        p.dl->PathLineTo(p.P(0.75f, 0.75f));
        p.dl->PathFillConcave(light);
        p.dl->PathArcTo(p.P(-0.05f, 0.05f), 0.8f * p.h, -1.5708f, 0.f, 12);
        p.dl->PathStroke(amber, 0, p.w(0.14f));
        p.line(-0.75f, 0.75f, 0.75f, 0.75f, dark, 0.05f);
        p.line(-0.75f, -0.75f, -0.75f, 0.75f, dark, 0.05f);
        break;
    case Icon::Chamfer:
        p.poly({{-0.75f, 0.75f}, {-0.75f, -0.75f}, {0.05f, -0.75f}, {0.75f, -0.05f}, {0.75f, 0.75f}}, light);
        p.line(0.05f, -0.75f, 0.75f, -0.05f, amber, 0.14f);
        p.line(-0.75f, 0.75f, 0.75f, 0.75f, dark, 0.05f);
        p.line(-0.75f, -0.75f, -0.75f, 0.75f, dark, 0.05f);
        break;
    case Icon::Shell:
        p.stdBox(0, 0.f, 1.15f, 0.55f);
        p.poly({{0.f, -0.2f}, {0.5f, 0.f}, {0.f, 0.2f}, {-0.5f, 0.f}}, p.col(90, 100, 116), amber, 0.05f);
        break;
    case Icon::Draft:
        p.poly({{-0.5f, -0.6f}, {0.5f, -0.6f}, {0.7f, 0.6f}, {-0.7f, 0.6f}}, light, dark, 0.05f);
        p.line(0.5f, -0.6f, 0.7f, 0.6f, amber, 0.12f);
        break;
    case Icon::Scale:
        p.rect(-0.8f, -0.1f, 0.1f, 0.8f, light, true);
        p.rect(-0.8f, -0.8f, 0.8f, 0.8f, amber, false, 0.07f);
        p.line(-0.1f, 0.1f, 0.6f, -0.6f, dark, 0.08f);
        break;
    case Icon::Combine:
        p.circle(-0.25f, 0.f, 0.5f, p.col(150, 164, 182), true);
        p.circle(0.25f, 0.f, 0.5f, p.col(150, 164, 182, 200), true);
        p.circle(-0.25f, 0.f, 0.5f, amber, false, 0.07f);
        p.circle(0.25f, 0.f, 0.5f, amber, false, 0.07f);
        break;
    case Icon::SplitBody:
        p.stdBox(-0.15f, 0.1f, 0.8f, 0.45f);
        p.stdBox(0.3f, -0.2f, 0.8f, 0.45f);
        p.line(-0.8f, 0.6f, 0.8f, -0.6f, amber, 0.08f);
        break;
    case Icon::MoveCopy:
        p.stdBox(0, 0.05f, 0.75f, 0.4f);
        p.line(-0.9f, 0.f, 0.9f, 0.f, amber, 0.08f);
        p.line(0.f, -0.9f, 0.f, 0.9f, amber, 0.08f);
        p.poly({{0.9f, 0.f}, {0.7f, -0.13f}, {0.7f, 0.13f}}, amber);
        p.poly({{0.f, -0.9f}, {-0.13f, -0.7f}, {0.13f, -0.7f}}, amber);
        break;
    case Icon::Align:
        p.rect(-0.8f, -0.6f, -0.1f, 0.1f, light, true);
        p.rect(0.1f, -0.1f, 0.8f, 0.6f, p.col(150, 164, 182), true);
        p.line(-0.9f, 0.1f, 0.9f, 0.1f, amber, 0.07f);
        break;
    case Icon::OffsetFace:
        p.stdBox(0, 0.1f, 1.f, 0.5f);
        p.poly({{0.f, -0.42f}, {0.62f, -0.1f}, {0.f, 0.22f}, {-0.62f, -0.1f}}, p.col(255, 220, 160, 200), amber, 0.05f);
        break;
    case Icon::Parameters:
        p.rect(-0.75f, -0.75f, 0.75f, 0.75f, p.col(245, 247, 250), true, 0, 0.1f);
        p.rect(-0.75f, -0.75f, 0.75f, 0.75f, slate, false, 0.06f, 0.1f);
        p.dl->AddText(nullptr, p.h * 1.0f, p.P(-0.55f, -0.62f), amber, "fx");
        break;
    case Icon::Joint:
        p.stdBox(-0.3f, 0.3f, 0.75f, 0.35f);
        p.stdBox(0.3f, -0.35f, 0.75f, 0.35f);
        p.circle(0.f, 0.f, 0.2f, purple, true);
        p.circle(0.f, 0.f, 0.2f, white, false, 0.05f);
        break;
    case Icon::AsBuiltJoint:
        p.stdBox(-0.2f, 0.2f, 0.9f, 0.4f);
        p.circle(0.35f, -0.4f, 0.22f, purple, true);
        p.line(0.35f, -0.4f, 0.75f, -0.8f, purple, 0.07f);
        break;
    case Icon::RigidGroup:
        p.rect(-0.8f, -0.4f, -0.05f, 0.35f, light, true);
        p.rect(0.05f, -0.4f, 0.8f, 0.35f, light, true);
        p.rect(-0.85f, -0.5f, 0.85f, 0.45f, purple, false, 0.07f, 0.12f);
        break;
    case Icon::OffsetPlane:
        p.poly({{-0.8f, 0.55f}, {0.3f, 0.55f}, {0.8f, 0.15f}, {-0.3f, 0.15f}}, p.col(255, 230, 200), orange, 0.05f);
        p.poly({{-0.8f, -0.05f}, {0.3f, -0.05f}, {0.8f, -0.45f}, {-0.3f, -0.45f}}, p.col(255, 200, 150), orange, 0.05f);
        break;
    case Icon::AnglePlane:
        p.line(-0.8f, 0.6f, 0.8f, 0.6f, dark, 0.06f);
        p.poly({{-0.6f, 0.6f}, {0.0f, 0.6f}, {0.7f, -0.5f}, {0.1f, -0.5f}}, p.col(255, 220, 180), orange, 0.05f);
        break;
    case Icon::MidPlane:
        p.rect(-0.8f, -0.7f, -0.45f, 0.7f, light, true);
        p.rect(0.45f, -0.7f, 0.8f, 0.7f, light, true);
        p.poly({{-0.15f, -0.8f}, {0.15f, -0.6f}, {0.15f, 0.8f}, {-0.15f, 0.6f}}, p.col(255, 220, 180), orange, 0.05f);
        break;
    case Icon::Axis:
        p.line(-0.7f, 0.7f, 0.7f, -0.7f, orange, 0.1f);
        p.circle(-0.7f, 0.7f, 0.1f, orange, true);
        p.circle(0.7f, -0.7f, 0.1f, orange, true);
        break;
    case Icon::Point:
        p.circle(0, 0, 0.28f, orange, true);
        p.circle(0, 0, 0.5f, orange, false, 0.06f);
        break;
    case Icon::Measure:
        p.poly({{-0.85f, 0.25f}, {0.6f, -0.85f}, {0.85f, -0.5f}, {-0.6f, 0.6f}}, p.col(255, 226, 120), slate, 0.05f);
        for (int i = 0; i < 5; ++i) {
            float t = 0.15f + i * 0.17f;
            float x = -0.85f + t * 1.45f, y = 0.25f - t * 1.1f;
            p.line(x, y, x + 0.12f, y + 0.15f, slate, 0.05f);
        }
        break;
    case Icon::Interference:
        p.stdBox(-0.25f, 0.15f, 0.8f, 0.4f);
        p.stdBox(0.25f, -0.15f, 0.8f, 0.4f);
        p.circle(0.f, 0.f, 0.22f, p.col(230, 60, 60), true);
        break;
    case Icon::Illustration:
        // A framed picture of a part.
        p.rect(-0.85f, -0.75f, 0.85f, 0.75f, p.col(250, 250, 252), true);
        p.rect(-0.85f, -0.75f, 0.85f, 0.75f, slate, false, 0.08f);
        p.stdBox(0, 0.12f, 0.8f, 0.36f);
        p.circle(0.55f, -0.48f, 0.13f, orange, true);
        break;
    case Icon::Section:
        p.stdBox(0, 0.05f, 1.1f, 0.5f);
        p.line(-0.9f, 0.5f, 0.9f, -0.4f, p.col(230, 60, 60), 0.08f);
        break;
    case Icon::InsertMesh:
        p.poly({{0.f, -0.8f}, {0.75f, 0.55f}, {-0.75f, 0.55f}}, p.col(205, 236, 214), p.col(46, 160, 90), 0.06f);
        p.line(0.f, -0.8f, 0.f, 0.55f, p.col(46, 160, 90), 0.05f);
        p.line(-0.37f, -0.12f, 0.37f, -0.12f, p.col(46, 160, 90), 0.05f);
        break;
    case Icon::InsertStep:
        p.stdBox(0, 0.15f, 1.f, 0.45f);
        p.arrowUp(0.f, -0.2f, -0.9f, p.col(46, 160, 90));
        break;
    case Icon::ExportStl:
        p.poly({{0.f, -0.5f}, {0.7f, 0.7f}, {-0.7f, 0.7f}}, light, slate, 0.05f);
        p.line(0.3f, -0.15f, 0.85f, -0.7f, p.col(46, 160, 90), 0.1f);
        p.poly({{0.9f, -0.85f}, {0.55f, -0.8f}, {0.85f, -0.5f}}, p.col(46, 160, 90));
        break;
    case Icon::Slicer:
        // Printer nozzle laying down layers.
        p.rect(-0.8f, 0.35f, 0.8f, 0.5f, print, true);
        p.rect(-0.65f, 0.15f, 0.65f, 0.3f, p.col(255, 150, 110), true);
        p.rect(-0.5f, -0.05f, 0.5f, 0.1f, p.col(255, 190, 160), true);
        p.poly({{-0.25f, -0.85f}, {0.25f, -0.85f}, {0.25f, -0.45f}, {0.08f, -0.2f}, {-0.08f, -0.2f}, {-0.25f, -0.45f}}, dark);
        break;
    case Icon::Select:
        p.poly({{-0.5f, -0.8f}, {0.5f, 0.15f}, {0.05f, 0.18f}, {-0.5f, 0.55f}}, dark);
        p.line(0.0f, 0.1f, 0.35f, 0.7f, dark, 0.14f);
        break;
    case Icon::WindowSelect:
        p.rect(-0.8f, -0.8f, 0.8f, 0.8f, blue, false, 0.06f);
        p.rect(-0.8f, -0.8f, 0.8f, 0.8f, p.col(43, 125, 233, 50), true);
        break;
    case Icon::Line: p.line(-0.75f, 0.6f, 0.75f, -0.6f, sketch, 0.1f); p.circle(-0.75f, 0.6f, 0.12f, dark, true); p.circle(0.75f, -0.6f, 0.12f, dark, true); break;
    case Icon::Rectangle: p.rect(-0.75f, -0.55f, 0.75f, 0.55f, sketch, false, 0.1f); break;
    case Icon::Circle: p.circle(0, 0, 0.7f, sketch, false, 0.1f); p.circle(0, 0, 0.08f, dark, true); break;
    case Icon::Arc:
        p.dl->PathArcTo(p.P(0, 0.4f), 0.75f * p.h, 3.4f, 6.0f, 16);
        p.dl->PathStroke(sketch, 0, p.w(0.1f));
        break;
    case Icon::Polygon: {
        ImVec2 pts[6];
        for (int i = 0; i < 6; ++i) pts[i] = p.P(std::cos(i * 1.0472f) * 0.72f, std::sin(i * 1.0472f) * 0.72f);
        p.dl->AddPolyline(pts, 6, sketch, ImDrawFlags_Closed, p.w(0.1f));
        break;
    }
    case Icon::Slot: p.rect(-0.8f, -0.35f, 0.8f, 0.35f, sketch, false, 0.1f, 0.35f); break;
    case Icon::Spline: p.dl->AddBezierCubic(p.P(-0.8f, 0.5f), p.P(-0.3f, -0.9f), p.P(0.3f, 0.9f), p.P(0.8f, -0.5f), sketch, p.w(0.1f)); break;
    case Icon::Dimension:
        p.line(-0.8f, -0.3f, -0.8f, 0.6f, dark, 0.05f);
        p.line(0.8f, -0.3f, 0.8f, 0.6f, dark, 0.05f);
        p.line(-0.8f, 0.2f, 0.8f, 0.2f, sketch, 0.07f);
        p.dl->AddText(nullptr, p.h * 0.8f, p.P(-0.35f, -0.65f), dark, "10");
        break;
    case Icon::Appearance:
        p.circle(0, 0, 0.75f, p.col(240, 240, 240), true);
        p.circle(-0.3f, -0.25f, 0.18f, p.col(230, 70, 70), true);
        p.circle(0.2f, -0.35f, 0.18f, p.col(60, 160, 230), true);
        p.circle(0.35f, 0.15f, 0.18f, p.col(80, 190, 90), true);
        p.circle(0, 0, 0.75f, slate, false, 0.06f);
        break;
    case Icon::Settings:
    case Icon::Gear: {
        for (int i = 0; i < 8; ++i) {
            float a = i * 0.7854f;
            p.line(std::cos(a) * 0.45f, std::sin(a) * 0.45f, std::cos(a) * 0.8f, std::sin(a) * 0.8f, slate, 0.2f);
        }
        p.circle(0, 0, 0.55f, slate, true);
        p.circle(0, 0, 0.22f, white, true);
        break;
    }
    case Icon::Save:
        p.rect(-0.75f, -0.75f, 0.75f, 0.75f, slate, true, 0, 0.08f);
        p.rect(-0.45f, -0.75f, 0.45f, -0.25f, white, true);
        p.rect(-0.5f, 0.15f, 0.5f, 0.75f, p.col(220, 226, 234), true);
        break;
    case Icon::Undo:
    case Icon::Redo: {
        float s = icon == Icon::Undo ? 1.f : -1.f;
        p.dl->PathArcTo(p.P(0.f, 0.15f), 0.55f * p.h, icon == Icon::Undo ? 3.6f : -0.46f, icon == Icon::Undo ? 6.6f : -3.46f, 14);
        p.dl->PathStroke(slate, 0, p.w(0.13f));
        p.poly({{-0.75f * s, -0.35f}, {-0.2f * s, -0.45f}, {-0.6f * s, 0.1f}}, slate);
        break;
    }
    case Icon::File:
        p.rect(-0.8f, -0.6f, 0.8f, -0.35f, slate, true, 0, 0.05f);
        p.rect(-0.8f, -0.12f, 0.8f, 0.12f, slate, true, 0, 0.05f);
        p.rect(-0.8f, 0.35f, 0.8f, 0.6f, slate, true, 0, 0.05f);
        break;
    case Icon::Home:
        p.poly({{0.f, -0.8f}, {0.8f, -0.05f}, {-0.8f, -0.05f}}, slate);
        p.rect(-0.55f, -0.1f, 0.55f, 0.7f, slate, true);
        p.rect(-0.15f, 0.25f, 0.15f, 0.7f, white, true);
        break;
    case Icon::Eye:
    case Icon::EyeOff:
        p.dl->AddEllipse(p.P(0, 0), ImVec2(0.8f * p.h, 0.45f * p.h), icon == Icon::Eye ? dark : p.col(150, 156, 164), 0, 0, p.w(0.1f));
        if (icon == Icon::Eye) p.circle(0, 0, 0.25f, dark, true);
        else p.line(-0.75f, 0.6f, 0.75f, -0.6f, p.col(150, 156, 164), 0.1f);
        break;
    case Icon::Component:
        p.box(0, -0.15f, 1.1f, 0.5f, p.col(232, 222, 250), p.col(180, 160, 230), purple, p.col(80, 60, 140));
        break;
    case Icon::Body:
        p.box(0, -0.15f, 1.1f, 0.5f, p.col(225, 230, 238), p.col(165, 176, 192), p.col(130, 144, 164), p.col(80, 92, 110));
        break;
    case Icon::Folder:
        p.poly({{-0.85f, -0.55f}, {-0.3f, -0.55f}, {-0.15f, -0.4f}, {0.85f, -0.4f}, {0.85f, 0.65f}, {-0.85f, 0.65f}}, p.col(240, 200, 110), p.col(190, 145, 50), 0.05f);
        break;
    case Icon::Origin:
        p.line(0, 0, 0.8f, 0.3f, p.col(220, 60, 50), 0.09f);
        p.line(0, 0, -0.6f, 0.45f, p.col(60, 170, 60), 0.09f);
        p.line(0, 0, 0, -0.8f, p.col(50, 100, 220), 0.09f);
        p.circle(0, 0, 0.13f, orange, true);
        break;
    case Icon::Orbit:
        p.dl->AddEllipse(p.P(0, 0), ImVec2(0.8f * p.h, 0.35f * p.h), slate, 0.3f, 0, p.w(0.08f));
        p.circle(0, 0, 0.3f, slate, true);
        break;
    case Icon::Pan:
        p.line(-0.8f, 0, 0.8f, 0, slate, 0.08f);
        p.line(0, -0.8f, 0, 0.8f, slate, 0.08f);
        p.poly({{0.85f, 0}, {0.6f, -0.18f}, {0.6f, 0.18f}}, slate);
        p.poly({{-0.85f, 0}, {-0.6f, -0.18f}, {-0.6f, 0.18f}}, slate);
        p.poly({{0, 0.85f}, {-0.18f, 0.6f}, {0.18f, 0.6f}}, slate);
        p.poly({{0, -0.85f}, {-0.18f, -0.6f}, {0.18f, -0.6f}}, slate);
        break;
    case Icon::Zoom:
    case Icon::Search:
        p.circle(-0.15f, -0.15f, 0.48f, slate, false, 0.12f);
        p.line(0.2f, 0.2f, 0.75f, 0.75f, slate, 0.16f);
        break;
    case Icon::Fit:
        p.rect(-0.35f, -0.35f, 0.35f, 0.35f, slate, true);
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                p.line(sx * 0.85f, sy * 0.85f, sx * 0.5f, sy * 0.85f, slate, 0.08f);
                p.line(sx * 0.85f, sy * 0.85f, sx * 0.85f, sy * 0.5f, slate, 0.08f);
            }
        break;
    case Icon::LookAt:
        p.rect(-0.6f, -0.6f, 0.6f, 0.6f, slate, false, 0.08f);
        p.circle(0, 0, 0.2f, blue, true);
        break;
    case Icon::Display:
        p.rect(-0.8f, -0.6f, 0.8f, 0.45f, slate, false, 0.09f, 0.08f);
        p.line(-0.35f, 0.75f, 0.35f, 0.75f, slate, 0.09f);
        p.stdBox(0, -0.15f, 0.55f, 0.25f);
        break;
    case Icon::Grid:
        for (int i = -1; i <= 1; ++i) {
            p.line(i * 0.45f, -0.8f, i * 0.45f, 0.8f, slate, 0.06f);
            p.line(-0.8f, i * 0.45f, 0.8f, i * 0.45f, slate, 0.06f);
        }
        break;
    case Icon::Play: p.poly({{-0.4f, -0.6f}, {0.6f, 0.f}, {-0.4f, 0.6f}}, slate); break;
    case Icon::StepBack: p.poly({{0.4f, -0.6f}, {-0.6f, 0.f}, {0.4f, 0.6f}}, slate); break;
    case Icon::StepFwd: p.poly({{-0.4f, -0.6f}, {0.6f, 0.f}, {-0.4f, 0.6f}}, slate); break;
    case Icon::ToStart: p.rect(-0.7f, -0.6f, -0.5f, 0.6f, slate, true); p.poly({{0.6f, -0.6f}, {-0.4f, 0.f}, {0.6f, 0.6f}}, slate); break;
    case Icon::ToEnd: p.rect(0.5f, -0.6f, 0.7f, 0.6f, slate, true); p.poly({{-0.6f, -0.6f}, {0.4f, 0.f}, {-0.6f, 0.6f}}, slate); break;
    case Icon::ImportFeature:
        p.rect(-0.7f, -0.7f, 0.7f, 0.7f, p.col(205, 236, 214), true, 0, 0.12f);
        p.rect(-0.7f, -0.7f, 0.7f, 0.7f, p.col(46, 160, 90), false, 0.07f, 0.12f);
        p.arrowUp(0.f, 0.45f, -0.5f, p.col(46, 160, 90));
        break;
    case Icon::Close:
        p.line(-0.6f, -0.6f, 0.6f, 0.6f, slate, 0.12f);
        p.line(0.6f, -0.6f, -0.6f, 0.6f, slate, 0.12f);
        break;
    case Icon::Palette:
        p.rect(-0.8f, -0.5f, 0.8f, 0.5f, slate, false, 0.08f, 0.15f);
        p.dl->AddText(nullptr, p.h * 0.9f, p.P(-0.3f, -0.42f), slate, "S");
        break;
    case Icon::Trim:
        p.line(-0.85f, 0.25f, 0.85f, 0.25f, dark, 0.09f);
        p.line(0.f, -0.85f, 0.f, 0.25f, sketch, 0.11f);
        p.line(0.f, 0.25f, 0.f, 0.85f, p.col(200, 205, 212), 0.07f);
        p.line(-0.3f, 0.55f, 0.3f, 0.95f, p.col(230, 70, 70), 0.08f);
        p.line(0.3f, 0.55f, -0.3f, 0.95f, p.col(230, 70, 70), 0.08f);
        break;
    case Icon::Break:
        p.line(-0.85f, 0.f, -0.15f, 0.f, sketch, 0.11f);
        p.line(0.15f, 0.f, 0.85f, 0.f, sketch, 0.11f);
        p.line(0.f, -0.75f, 0.f, 0.75f, dark, 0.08f);
        break;
    case Icon::Intersect:
        p.rect(-0.5f, -0.75f, 0.5f, 0.2f, dark, false, 0.08f);
        p.line(-0.85f, 0.05f, 0.85f, 0.05f, sketch, 0.12f);
        break;
    case Icon::ProjectFace:
        p.rect(-0.7f, -0.85f, 0.7f, -0.2f, dark, false, 0.08f);
        p.line(-0.55f, 0.75f, 0.55f, 0.75f, sketch, 0.11f);
        p.line(-0.55f, 0.75f, -0.35f, -0.2f, p.col(120, 130, 145), 0.07f);
        p.line(0.55f, 0.75f, 0.35f, -0.2f, p.col(120, 130, 145), 0.07f);
        break;
    case Icon::CrossPoint:
        p.line(-0.7f, -0.7f, 0.7f, 0.7f, sketch, 0.09f);
        p.line(-0.7f, 0.7f, 0.7f, -0.7f, sketch, 0.09f);
        p.circle(0, 0, 0.22f, orange, true);
        break;
    case Icon::Sphere:
        p.circle(0, 0, 0.72f, light, true);
        p.circle(0, 0, 0.72f, dark, false, 0.06f);
        p.circle(-0.22f, -0.18f, 0.14f, p.col(255, 255, 255), true);
        break;
    case Icon::Cone:
        p.poly({{-0.62f, -0.55f}, {0.62f, -0.55f}, {0, 0.72f}}, light, dark, 0.06f);
        break;
    case Icon::Torus:
        p.circle(0, 0, 0.72f, light, false, 0.18f);
        break;
    case Icon::Wedge:
        p.poly({{-0.7f, 0.55f}, {0.7f, 0.55f}, {-0.7f, -0.65f}}, light, dark, 0.06f);
        break;
    case Icon::Tube:
        p.circle(0, 0, 0.7f, light, true);
        p.circle(0, 0, 0.32f, dark, true);
        p.circle(0, 0, 0.7f, dark, false, 0.06f);
        break;
    case Icon::Offset:
        p.rect(-0.85f, -0.65f, 0.85f, 0.65f, sketch, false, 0.1f, 0.12f);
        p.rect(-0.45f, -0.3f, 0.45f, 0.3f, p.col(120, 130, 145), false, 0.07f, 0.06f);
        break;
    case Icon::Project:
        p.rect(-0.55f, -0.85f, 0.55f, -0.35f, dark, false, 0.07f);
        p.arrowUp(0.f, -0.2f, 0.4f, p.col(120, 130, 145));
        p.line(-0.9f, 0.8f, 0.9f, 0.8f, sketch, 0.11f);
        break;
    case Icon::Construction:
        for (int i = 0; i < 4; ++i) p.line(-0.9f + i * 0.5f, 0.55f - i * 0.37f, -0.62f + i * 0.5f, 0.35f - i * 0.37f, sketch, 0.11f);
        break;
    case Icon::FinishSketch:
        p.rect(-0.85f, -0.85f, 0.85f, 0.85f, p.col(46, 160, 90), true, 0, 0.2f);
    {
        ImVec2 chk[3] = {p.P(-0.45f, 0.05f), p.P(-0.1f, 0.4f), p.P(0.5f, -0.35f)};
        p.dl->AddPolyline(chk, 3, white, 0, p.w(0.16f));
        break;
    }
    case Icon::CenterRect:
        p.rect(-0.75f, -0.55f, 0.75f, 0.55f, sketch, false, 0.1f);
        p.circle(0, 0, 0.1f, dark, true);
        break;
    case Icon::ConCoincident:
        p.line(-0.8f, 0.7f, 0.f, 0.f, dark, 0.09f);
        p.line(0.f, 0.f, 0.8f, 0.7f, dark, 0.09f);
        p.circle(0, 0, 0.2f, p.col(230, 70, 70), true);
        break;
    case Icon::ConCollinear:
        p.line(-0.9f, 0.45f, -0.15f, 0.08f, dark, 0.1f);
        p.line(0.15f, -0.08f, 0.9f, -0.45f, dark, 0.1f);
        p.line(-0.9f, 0.45f, 0.9f, -0.45f, p.col(150, 160, 175), 0.04f);
        break;
    case Icon::ConConcentric:
        p.circle(0, 0, 0.8f, dark, false, 0.09f);
        p.circle(0, 0, 0.4f, dark, false, 0.09f);
        p.circle(0, 0, 0.1f, p.col(230, 70, 70), true);
        break;
    case Icon::ConMidpoint:
        p.line(-0.9f, 0.f, 0.9f, 0.f, dark, 0.09f);
        p.poly({{0.f, -0.1f}, {-0.3f, -0.7f}, {0.3f, -0.7f}}, p.col(230, 70, 70));
        break;
    case Icon::ConFix:
        p.rect(-0.55f, -0.05f, 0.55f, 0.8f, dark, true, 0, 0.1f);
        p.dl->PathArcTo(p.P(0, -0.05f), 0.32f * p.h, 3.14159f, 6.28318f, 12);
        p.dl->PathStroke(dark, 0, p.w(0.14f));
        break;
    case Icon::ConParallel:
        p.line(-0.55f, 0.8f, 0.05f, -0.8f, dark, 0.11f);
        p.line(0.05f, 0.8f, 0.65f, -0.8f, dark, 0.11f);
        break;
    case Icon::ConPerpendicular:
        p.line(0.f, -0.8f, 0.f, 0.7f, dark, 0.11f);
        p.line(-0.8f, 0.7f, 0.8f, 0.7f, dark, 0.11f);
        p.rect(0.f, 0.35f, 0.35f, 0.7f, p.col(230, 70, 70), false, 0.06f);
        break;
    case Icon::ConHorizontal:
        p.line(-0.85f, 0.f, 0.85f, 0.f, dark, 0.12f);
        p.line(-0.85f, -0.35f, -0.85f, 0.35f, dark, 0.07f);
        p.line(0.85f, -0.35f, 0.85f, 0.35f, dark, 0.07f);
        break;
    case Icon::ConVertical:
        p.line(0.f, -0.85f, 0.f, 0.85f, dark, 0.12f);
        p.line(-0.35f, -0.85f, 0.35f, -0.85f, dark, 0.07f);
        p.line(-0.35f, 0.85f, 0.35f, 0.85f, dark, 0.07f);
        break;
    case Icon::ConTangent:
        p.circle(0.f, 0.15f, 0.55f, dark, false, 0.1f);
        p.line(-0.9f, -0.4f, 0.9f, -0.4f, p.col(230, 70, 70), 0.1f);
        break;
    case Icon::ConEqual:
        p.line(-0.7f, -0.28f, 0.7f, -0.28f, dark, 0.14f);
        p.line(-0.7f, 0.28f, 0.7f, 0.28f, dark, 0.14f);
        break;
    case Icon::ConSymmetric:
        p.poly({{-0.85f, -0.5f}, {-0.35f, 0.f}, {-0.85f, 0.5f}}, dark);
        p.poly({{0.85f, -0.5f}, {0.35f, 0.f}, {0.85f, 0.5f}}, dark);
        for (int i = 0; i < 4; ++i) p.line(0.f, -0.85f + i * 0.45f, 0.f, -0.6f + i * 0.45f, p.col(230, 70, 70), 0.09f);
        break;
    case Icon::SketchOptions:
        p.rect(-0.7f, -0.7f, 0.7f, 0.7f, slate, false, 0.08f, 0.1f);
        p.line(-0.4f, -0.25f, 0.4f, -0.25f, slate, 0.08f);
        p.line(-0.4f, 0.15f, 0.4f, 0.15f, slate, 0.08f);
        p.line(-0.4f, 0.5f, 0.1f, 0.5f, slate, 0.08f);
        break;
    case Icon::Chevron: p.poly({{-0.5f, -0.25f}, {0.5f, -0.25f}, {0.f, 0.35f}}, slate); break;
    }
}

void drawLogo(ImDrawList* dl, ImVec2 o, float size) {
    // Same drawing as assets/kestral_icon.svg (a 256 unit square).
    const float k = size / 256.f;
    auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    dl->AddRectFilled(P(8, 8), P(248, 248), IM_COL32(24, 33, 50, 255), 54 * k);
    dl->AddRectFilled(P(62, 50), P(98, 206), IM_COL32(242, 244, 247, 255), 9 * k);
    ImVec2 leg[4] = {P(98, 150), P(126, 128), P(204, 206), P(160, 206)};
    dl->AddConvexPolyFilled(leg, 4, IM_COL32(214, 221, 231, 255));
    dl->PathLineTo(P(98, 116));
    dl->PathBezierCubicCurveTo(P(132, 98), P(170, 70), P(216, 38), 10);
    for (auto q : {P(203, 68), P(213, 66), P(194, 93), P(204, 92), P(180, 117), P(190, 118)}) dl->PathLineTo(q);
    dl->PathBezierCubicCurveTo(P(166, 138), P(134, 156), P(98, 172), 10);
    dl->PathFillConcave(IM_COL32(245, 112, 46, 255));
}

} // namespace kst::ui
