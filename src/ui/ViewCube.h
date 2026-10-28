#pragma once
#include "core/Types.h"
#include <imgui.h>

namespace kst::ui {

// Navigation cube (top right). Click faces, edges and corners for 26 standard views,
// drag to orbit, house icon for the home view.
class ViewCube {
public:
    struct Result {
        bool clicked = false;
        glm::vec3 dir{0.f};      // direction from target to camera
        bool home = false;
        glm::vec2 drag{0.f};     // pixel delta while dragging
        bool hovered = false;
    };
    // Call inside an ImGui window. Occupies a size x size square at the cursor.
    Result draw(const glm::quat& camRot, float size);

private:
    bool dragging_ = false;
};

} // namespace kst::ui
