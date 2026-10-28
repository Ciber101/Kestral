#pragma once
// Thread standards: families, their sizes and their tooth forms. Pure math, no kernel types, so the
// UI can list sizes and the recompute can build the groove profile that the kernel sweeps.
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace kst::threads {

struct Size {
    std::string name;
    double major = 0;       // mm, nominal (major) diameter
    double pitch = 0;       // mm
    int starts = 1;
};

struct Family {
    std::string name;
    std::string note;       // one line shown under the size list
    double angleL = 30, angleR = 30;   // flank angles from the radial line, degrees
    double hExt = 0, flatExt = 0;      // external: groove depth and crest flat at the major diameter (x pitch)
    double hInt = 0, flatInt = 0;      // internal: tooth depth and crest flat at the minor diameter (x pitch)
    bool trapAllowance = false;        // ISO 2904: extra root depth that grows with the pitch
    bool round = false;                // knuckle: rounded crests and roots
    double taperDeg = 0;               // half angle on the radius, 0 = parallel
    bool inch = false;                 // sizes are named in inches
    std::vector<Size> sizes;
};

const std::vector<Family>& families();

// One groove of the thread as a closed polygon of (radius, axial offset) points, centered on axial
// offset 0. External grooves run from above max(major, face) down into the shaft; internal ones
// from below min(minor, face) out to the major diameter.
std::vector<glm::dvec2> groove(const Family& f, double major, double pitch, bool internal, double faceRadius);

// Minor diameter of the internal thread (the tap drill size).
double minorDia(const Family& f, double major, double pitch);

// Size whose major (external) or minor (internal) diameter is closest to a face diameter.
int closestSize(const Family& f, double faceDia, bool internal);

} // namespace kst::threads
