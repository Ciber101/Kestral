#include "render/Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace kst {

static const glm::vec3 kHomeDir = glm::normalize(glm::vec3(1.f, -1.f, 0.8f));  // front right top

Camera::Camera() { rot = lookRotation(kHomeDir, {0, 0, 1}); }

glm::quat Camera::lookRotation(const glm::vec3& backDir, const glm::vec3& upHint) {
    glm::vec3 z = glm::normalize(backDir);
    glm::vec3 u = upHint;
    if (std::abs(glm::dot(z, glm::normalize(u))) > 0.999f) u = std::abs(z.z) > 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    glm::vec3 x = glm::normalize(glm::cross(u, z));
    glm::vec3 y = glm::cross(z, x);
    glm::mat3 m(x, y, z);
    return glm::normalize(glm::quat_cast(m));
}

glm::mat4 Camera::view() const {
    glm::mat4 r = glm::mat4_cast(glm::conjugate(rot));
    return r * glm::translate(glm::mat4(1.f), -eye());
}

glm::mat4 Camera::proj(float aspect, const Aabb& scene) const {
    glm::mat4 p(0.f);
    if (!ortho) {
        float f = 1.f / std::tan(fovY * 0.5f);
        float nearZ = std::max(distance * 0.002f, 0.0002f);
        p[0][0] = f / aspect;
        p[1][1] = -f;               // Vulkan y down
        p[2][2] = 0.f;
        p[2][3] = -1.f;
        p[3][2] = nearZ;            // reverse Z, infinite far
        return p;
    }
    float hh = halfHeightAtTarget();
    float hw = hh * aspect;
    float R = std::max(scene.valid() ? scene.diag() : 100.f, 10.f);
    float dc = scene.valid() ? glm::length(eye() - scene.center()) : distance;
    float nearZ = dc - R * 1.5f - 1.f;
    float farZ = dc + R * 1.5f + 1.f;
    p[0][0] = 1.f / hw;
    p[1][1] = -1.f / hh;
    p[2][2] = 1.f / (farZ - nearZ);
    p[3][2] = farZ / (farZ - nearZ);
    p[3][3] = 1.f;
    return p;
}

void Camera::orbit(float dx, float dy, bool constrained) {
    anim_.active = false;
    const float k = 0.0065f;
    if (constrained) {
        glm::quat yaw = glm::angleAxis(-dx * k, glm::vec3(0, 0, 1));
        glm::quat pitch = glm::angleAxis(-dy * k, right());
        glm::quat nr = glm::normalize(yaw * pitch * rot);
        // Keep the camera from flipping over the pole.
        glm::vec3 back = nr * glm::vec3(0, 0, 1);
        if (std::abs(back.z) < 0.995f) rot = nr;
        else rot = glm::normalize(yaw * rot);
    } else {
        glm::quat yaw = glm::angleAxis(-dx * k, up());
        glm::quat pitch = glm::angleAxis(-dy * k, right());
        rot = glm::normalize(yaw * pitch * rot);
    }
}

void Camera::orbitAround(const glm::vec3& pivot, float dx, float dy, bool constrained) {
    glm::quat before = rot;
    orbit(dx, dy, constrained);
    glm::quat delta = rot * glm::conjugate(before);
    // Rotate the eye about the pivot; keep the target on the view axis at the same distance.
    glm::vec3 eyeOld = before * glm::vec3(0, 0, distance) + target;
    glm::vec3 eyeNew = pivot + delta * (eyeOld - pivot);
    target = eyeNew - rot * glm::vec3(0, 0, distance);
}

void Camera::pan(float dx, float dy, float viewportH) {
    anim_.active = false;
    float wpp = 2.f * halfHeightAtTarget() / std::max(viewportH, 1.f);
    target -= right() * dx * wpp;
    target += up() * dy * wpp;
}

void Camera::zoomAt(float steps, glm::vec2 ndc, float aspect) {
    anim_.active = false;
    float factor = std::pow(0.87f, steps);
    float hh = halfHeightAtTarget();
    glm::vec3 p = target + right() * (ndc.x * hh * aspect) + up() * (ndc.y * hh);
    float nd = std::clamp(distance * factor, 0.0005f, 1e7f);
    float realFactor = nd / distance;
    target = p + (target - p) * realFactor;
    distance = nd;
}

void Camera::animateTo(const glm::vec3& t, float d, const glm::quat& r, float seconds) {
    anim_.active = true;
    anim_.t = 0;
    anim_.dur = seconds;
    anim_.t0 = target; anim_.t1 = t;
    anim_.d0 = distance; anim_.d1 = d;
    anim_.r0 = rot; anim_.r1 = r;
    if (glm::dot(anim_.r0, anim_.r1) < 0) anim_.r1 = -anim_.r1;
}

void Camera::lookFrom(const glm::vec3& dir, float seconds) {
    glm::vec3 d = glm::normalize(dir);
    glm::vec3 upHint(0, 0, 1);
    if (std::abs(d.z) > 0.99f) {
        // Top or bottom: keep the current screen orientation as much as possible.
        glm::vec3 u = up();
        u.z = 0;
        upHint = glm::length(u) > 1e-3f ? glm::normalize(u) : glm::vec3(0, 1, 0);
        if (d.z < 0) upHint = glm::length(u) > 1e-3f ? glm::normalize(u) : glm::vec3(0, -1, 0);
    }
    animateTo(target, distance, lookRotation(d, upHint), seconds);
}

float Camera::fitDistance(const Aabb& box, const glm::quat& r, float aspect) const {
    glm::vec3 c = box.center();
    glm::vec3 rx = r * glm::vec3(1, 0, 0), ry = r * glm::vec3(0, 1, 0), rz = r * glm::vec3(0, 0, 1);
    float hx = 0, hy = 0, hz = 0;
    for (int i = 0; i < 8; ++i) {
        glm::vec3 p((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z);
        glm::vec3 d = p - c;
        hx = std::max(hx, std::abs(glm::dot(d, rx)));
        hy = std::max(hy, std::abs(glm::dot(d, ry)));
        hz = std::max(hz, std::abs(glm::dot(d, rz)));
    }
    float t = std::tan(fovY * 0.5f);
    float need = std::max(hy, hx / std::max(aspect, 0.1f));
    need = std::max(need, 0.5f);
    float d = need / t * 1.12f;
    if (!ortho) d += hz;   // keep the near side of the box inside the frustum
    return d;
}

void Camera::fit(const Aabb& box, float aspect, bool animate) {
    if (!box.valid()) return;
    float d = fitDistance(box, rot, aspect);
    if (animate) animateTo(box.center(), d, rot);
    else { target = box.center(); distance = d; }
}

void Camera::home(const Aabb& box, float aspect) {
    glm::quat r = lookRotation(kHomeDir, {0, 0, 1});
    if (!box.valid()) { animateTo({0, 0, 0}, 300.f, r); return; }
    animateTo(box.center(), fitDistance(box, r, aspect), r);
}

bool Camera::update(float dt) {
    if (!anim_.active) return false;
    anim_.t += dt;
    float u = std::clamp(anim_.t / anim_.dur, 0.f, 1.f);
    float e = u * u * (3.f - 2.f * u);   // smoothstep
    target = glm::mix(anim_.t0, anim_.t1, e);
    distance = std::exp(glm::mix(std::log(anim_.d0), std::log(anim_.d1), e));
    rot = glm::normalize(glm::slerp(anim_.r0, anim_.r1, e));
    if (u >= 1.f) anim_.active = false;
    return true;
}

} // namespace kst
