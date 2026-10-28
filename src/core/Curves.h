#pragma once
#include "sketch/Profiles.h"
#include "sketch/Sketch.h"
#include <glm/glm.hpp>
#include <vector>

namespace kst {

// One piece of a profile boundary, in world millimetres, exact (no tessellation).
// Arcs are counter clockwise around `normal` when going a -> b, unless the loop reverses them
// (the consumer honours loop order; a reversed traversal just swaps a and b).
struct CurveSeg {
    enum class Kind : uint8_t { Line, Arc, Circle, Ellipse, EllipArc };
    Kind kind = Kind::Line;
    glm::dvec3 a{0}, b{0};       // line: endpoints. arc: start/end. circle: unused
    glm::dvec3 center{0};        // arc / circle / ellipse center
    glm::dvec3 normal{1, 0, 0};  // plane the arc/circle lies in
    glm::dvec3 majorDir{1, 0, 0}; // ellipse: direction of the long axis
    double radius = 0;           // circle radius, or the ellipse long radius
    double minor = 0;            // ellipse short radius
    double ang0 = 0, sweep = 0;  // elliptical arc, eccentric angles in radians
    bool back = false;           // loop travels b -> a (edge added reversed to the wire)
};

// A closed region: loops[0] is the outer boundary, the rest are holes.
using RegionLoops = std::vector<std::vector<CurveSeg>>;

// All closed regions of the sketch, world space, ready for the kernel to build faces.
// Uses the profile finder, so crossing curves are split correctly. Returns empty if the
// sketch has no closed profile.
std::vector<RegionLoops> sketchRegions(const sk::Sketch& s);
// Only the regions that contain one of the given sketch-space points (picked profiles).
// Each point selects the smallest region around it. Empty `pts` = every region.
std::vector<RegionLoops> sketchRegionsAt(const sk::Sketch& s, const std::vector<sk::Vec2>& pts);
// A point safely inside each profile (for remembering which profiles were picked).
sk::Vec2 profileSamplePoint(const sk::Profile& p);

// All visible curves of the sketch chained end to end into one open path (sweep spine).
// Throws std::runtime_error when the geometry does not form one connected chain.
std::vector<CurveSeg> chainedPath(const sk::Sketch& s);

} // namespace kst
