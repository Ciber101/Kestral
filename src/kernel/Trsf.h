#pragma once
// A body's placement (a glm matrix) as an OpenCASCADE transform.
#include <gp_Trsf.hxx>
#include <glm/glm.hpp>

#include <cmath>

namespace kst::kernel {

// glm keeps placements in floats, so a turned body's matrix is a hair off a pure rotation (its scale
// comes out 0.99999999 or 1.00000001). OpenCASCADE refuses to move a shape by a scaled transform
// ("Moving with scaling transformation is forbidden"), which made measuring, joining or embossing a
// turned body (a circular pattern's copies) fail. A placement that is a rotation is squared up in
// double precision first; anything else (a real scale or a mirror) goes through as it is.
inline gp_Trsf trsfOfMat(const glm::mat4& m) {
    glm::dvec3 c0(m[0]), c1(m[1]), c2(m[2]), t(m[3]);
    gp_Trsf tr;
    double l0 = glm::length(c0), l1 = glm::length(c1), l2 = glm::length(c2);
    if (std::abs(l0 - 1.0) < 1e-4 && std::abs(l1 - 1.0) < 1e-4 && std::abs(l2 - 1.0) < 1e-4) {
        glm::dvec3 x = glm::normalize(c0);
        glm::dvec3 y = c1 - x * glm::dot(x, c1);
        if (glm::length(y) > 1e-9) {
            y = glm::normalize(y);
            glm::dvec3 z = glm::cross(x, y);
            if (glm::dot(z, c2) > 0) {
                tr.SetValues(x.x, y.x, z.x, t.x,
                             x.y, y.y, z.y, t.y,
                             x.z, y.z, z.z, t.z);
                return tr;
            }
        }
    }
    tr.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                 m[0][1], m[1][1], m[2][1], m[3][1],
                 m[0][2], m[1][2], m[2][2], m[3][2]);
    return tr;
}

} // namespace kst::kernel
