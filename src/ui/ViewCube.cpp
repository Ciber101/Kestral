#include "ui/ViewCube.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include <cmath>
#include <cstring>

namespace kst::ui {

namespace {

struct FaceDef { glm::vec3 n, u, v; const char* label; };
const FaceDef kFaces[6] = {
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, "RIGHT"},
    {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}, "LEFT"},
    {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, "BACK"},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, "FRONT"},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, "TOP"},
    {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}, "BOTTOM"},
};

bool pointInQuad(const ImVec2 q[4], ImVec2 p) {
    float sign = 0;
    for (int i = 0; i < 4; ++i) {
        ImVec2 a = q[i], b = q[(i + 1) % 4];
        float c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::abs(c) < 1e-6f) continue;
        if (sign == 0) sign = c;
        else if ((c > 0) != (sign > 0)) return false;
    }
    return true;
}

} // namespace

ViewCube::Result ViewCube::draw(const glm::quat& camRot, float size) {
    Result res;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##viewcube", ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    res.hovered = hovered || active;
    ImVec2 mouse = ImGui::GetIO().MousePos;

    ImVec2 c(p0.x + size * 0.55f, p0.y + size * 0.52f);
    float s = size * 0.26f;
    glm::quat R = glm::conjugate(camRot);
    auto proj = [&](const glm::vec3& w) {
        glm::vec3 v = R * w;
        return ImVec2(c.x + v.x * s, c.y - v.y * s);
    };

    // Home button (top left)
    ImVec2 hc(p0.x + size * 0.12f, p0.y + size * 0.12f);
    float hs = size * 0.13f;
    bool homeHover = hovered && std::abs(mouse.x - hc.x) < hs * 0.6f && std::abs(mouse.y - hc.y) < hs * 0.6f;
    if (homeHover) dl->AddCircleFilled(hc, hs * 0.75f, IM_COL32(255, 107, 53, 50));
    drawIcon(dl, hc, hs, Icon::Home, true);

    // Axis triad along the cube edges from the (-1,-1,-1) corner.
    glm::vec3 o(-1, -1, -1);
    struct Ax { glm::vec3 d; ImU32 col; const char* t; } axes[3] = {
        {{1, 0, 0}, IM_COL32(215, 60, 50, 255), "X"},
        {{0, 1, 0}, IM_COL32(60, 165, 60, 255), "Y"},
        {{0, 0, 1}, IM_COL32(50, 100, 220, 255), "Z"},
    };
    for (auto& a : axes) {
        ImVec2 e = proj(o + a.d * 2.55f);
        dl->AddLine(proj(o), e, a.col, 2.2f);
        dl->AddText(ImVec2(e.x - 3, e.y - 7), a.col, a.t);
    }

    // Hit test sub regions of visible faces.
    const float k = 0.58f;
    const float cuts[4] = {-1.f, -k, k, 1.f};
    int hitFace = -1, hitI = -1, hitJ = -1;
    glm::vec3 hitDir(0);
    for (int f = 0; f < 6; ++f) {
        const FaceDef& fd = kFaces[f];
        if ((R * fd.n).z <= 0.02f) continue;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                ImVec2 q[4] = {
                    proj(fd.n + fd.u * cuts[i] + fd.v * cuts[j]),
                    proj(fd.n + fd.u * cuts[i + 1] + fd.v * cuts[j]),
                    proj(fd.n + fd.u * cuts[i + 1] + fd.v * cuts[j + 1]),
                    proj(fd.n + fd.u * cuts[i] + fd.v * cuts[j + 1]),
                };
                if (hovered && !homeHover && !dragging_ && pointInQuad(q, mouse)) {
                    hitFace = f; hitI = i; hitJ = j;
                    hitDir = fd.n + fd.u * (float)(i - 1) + fd.v * (float)(j - 1);
                }
            }
    }

    // Draw faces
    for (int f = 0; f < 6; ++f) {
        const FaceDef& fd = kFaces[f];
        float facing = (R * fd.n).z;
        if (facing <= 0.02f) continue;
        ImVec2 q[4] = {proj(fd.n - fd.u - fd.v), proj(fd.n + fd.u - fd.v), proj(fd.n + fd.u + fd.v), proj(fd.n - fd.u + fd.v)};
        int shade = (int)(232 + 18 * facing);
        dl->AddConvexPolyFilled(q, 4, th(IM_COL32(shade, shade, std::min(255, shade + 4), 245)));
        if (hitFace >= 0) {
            // Highlight every visible face sub quad that maps to the same direction (edges and corners span faces).
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    glm::vec3 d = fd.n + fd.u * (float)(i - 1) + fd.v * (float)(j - 1);
                    if (glm::length(d - hitDir) > 1e-3f) continue;
                    ImVec2 h[4] = {
                        proj(fd.n + fd.u * cuts[i] + fd.v * cuts[j]),
                        proj(fd.n + fd.u * cuts[i + 1] + fd.v * cuts[j]),
                        proj(fd.n + fd.u * cuts[i + 1] + fd.v * cuts[j + 1]),
                        proj(fd.n + fd.u * cuts[i] + fd.v * cuts[j + 1]),
                    };
                    dl->AddConvexPolyFilled(h, 4, IM_COL32(90, 170, 255, 170));
                }
        }
        dl->AddPolyline(q, 4, th(IM_COL32(120, 128, 140, 255)), ImDrawFlags_Closed, 1.2f);

        // Label drawn as text glyphs mapped onto the face plane.
        ImFont* font = ImGui::GetFont();
        float fsz = s * 0.42f;
        ImVec2 tsz = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, fd.label);
        ImVec2 fc = proj(fd.n);
        ImVec2 o0 = proj(glm::vec3(0));
        ImVec2 U(proj(fd.u).x - o0.x, proj(fd.u).y - o0.y);
        ImVec2 V(proj(-fd.v).x - o0.x, proj(-fd.v).y - o0.y);
        float scale = 0.78f / std::max(tsz.x * 0.5f, 1.f);
        int v0 = dl->VtxBuffer.Size;
        dl->PushClipRect(ImVec2(-1e5f, -1e5f), ImVec2(1e5f, 1e5f), false);
        dl->AddText(font, fsz, ImVec2(0, 0), th(IM_COL32(70, 78, 92, (int)(255 * std::min(1.f, facing * 2.f)))), fd.label);
        dl->PopClipRect();
        for (int vi = v0; vi < dl->VtxBuffer.Size; ++vi) {
            ImDrawVert& vt = dl->VtxBuffer[vi];
            float lx = (vt.pos.x - tsz.x * 0.5f) * scale;
            float ly = (vt.pos.y - tsz.y * 0.5f) * scale;
            vt.pos = ImVec2(fc.x + U.x * lx + V.x * ly, fc.y + U.y * lx + V.y * ly);
        }
    }

    // Interaction
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.f)) dragging_ = true;
    if (dragging_ && active) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        res.drag = {d.x, d.y};
    }
    if (ImGui::IsItemDeactivated()) {
        if (!dragging_) {
            if (homeHover) res.home = true;
            else if (hitFace >= 0) { res.clicked = true; res.dir = glm::normalize(hitDir); }
        }
        dragging_ = false;
    }
    if (!active) dragging_ = false;
    (void)hitI; (void)hitJ;
    return res;
}

} // namespace kst::ui
