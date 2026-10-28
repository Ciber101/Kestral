#pragma once
#include "core/Types.h"

namespace kst {

// Orbit camera. Z is up. Reverse Z depth (1 = near, 0 = far) for precision at any zoom.
class Camera {
public:
    glm::vec3 target{0.f};
    float distance = 300.f;
    glm::quat rot{1, 0, 0, 0};     // camera to world rotation; camera looks down its -Z
    bool ortho = false;
    float fovY = glm::radians(30.f);

    Camera();

    glm::vec3 eye() const { return target + rot * glm::vec3(0, 0, distance); }
    glm::vec3 forward() const { return rot * glm::vec3(0, 0, -1); }
    glm::vec3 right() const { return rot * glm::vec3(1, 0, 0); }
    glm::vec3 up() const { return rot * glm::vec3(0, 1, 0); }

    glm::mat4 view() const;
    glm::mat4 proj(float aspect, const Aabb& scene) const;
    float halfHeightAtTarget() const { return distance * std::tan(fovY * 0.5f); }

    // Interaction (pixel deltas, viewport height in pixels)
    void orbit(float dxPx, float dyPx, bool constrained);
    // Orbit keeping `pivot` fixed on screen (Fusion's orbit around the point under the cursor).
    void orbitAround(const glm::vec3& pivot, float dxPx, float dyPx, bool constrained);
    void pan(float dxPx, float dyPx, float viewportH);
    // steps > 0 zooms in. ndc is cursor position in [-1,1] (y up).
    void zoomAt(float steps, glm::vec2 ndc, float aspect);

    // Animated moves
    void animateTo(const glm::vec3& target, float distance, const glm::quat& rot, float seconds = 0.28f);
    void lookFrom(const glm::vec3& dirFromTarget, float seconds = 0.28f);  // view cube clicks
    void fit(const Aabb& box, float aspect, bool animate = true);
    void home(const Aabb& box, float aspect);
    bool update(float dt);   // returns true while animating
    bool animating() const { return anim_.active; }

    static glm::quat lookRotation(const glm::vec3& backDir, const glm::vec3& upHint);
    // Distance that frames the box tightly for a given view rotation.
    float fitDistance(const Aabb& box, const glm::quat& r, float aspect) const;

private:
    struct Anim {
        bool active = false;
        float t = 0, dur = 0.28f;
        glm::vec3 t0, t1;
        float d0 = 0, d1 = 0;
        glm::quat r0, r1;
    } anim_;
};

} // namespace kst
