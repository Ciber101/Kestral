#pragma once
#include "sketch/Sketch.h"

namespace kst::sk {

// One piece of a boundary: a stretch of a sketch curve. Parameters run 0..1 along the curve's natural
// direction (line a to b, arc counter clockwise). ta to tb is the direction of travel and may exceed 1
// for a circle that wraps past its seam.
struct ProfEdge {
    uint32_t ent = 0;
    double ta = 0, tb = 1;
    bool chord = false;          // a sampled piece of a spline; ca to cb is the piece
    Vec2 ca{0}, cb{0};
};

struct Loop {
    std::vector<ProfEdge> edges;
    std::vector<Vec2> poly;      // tessellated boundary (for display, picking and area)
    double area = 0;             // signed, counter clockwise positive
};

// A closed region of the sketch: an outer boundary and zero or more holes.
struct Profile {
    Loop outer;
    std::vector<Loop> holes;
    double area = 0;                 // outer minus holes
    std::vector<Vec2> tris;          // triangle list for display (3 points per triangle)
    Vec2 lo{0}, hi{0};
};

// Finds every closed region formed by the visible (non construction) curves, splitting curves where they
// cross. Open chains are ignored. Works on a snapshot and never throws.
std::vector<Profile> findProfiles(const Sketch& sk);

bool pointInProfile(const Profile& p, Vec2 q);

// Parameters (0..1 along the curve's natural direction) where `ent` is cut by other curves, plus its
// endpoints when it is open. Sorted and unique. Used by Trim.
std::vector<double> curveSplitParams(const Sketch& sk, uint32_t ent);

// The crossing of two lines, arcs, or circles closest to q, within maxDist. Parallel curves are not
// a crossing. False when nothing is close enough.
bool nearestCrossing(const Sketch& sk, Vec2 q, double maxDist, uint32_t& entA, uint32_t& entB, Vec2& at);
// Every place the two curves cross. Empty when they miss, are parallel, or are not lines, arcs, or circles.
void crossingsOf(const Sketch& sk, uint32_t a, uint32_t b, std::vector<Vec2>& out);
// Parameter of the point on `ent` closest to q.
double curveParamAt(const Sketch& sk, uint32_t ent, Vec2 q);

} // namespace kst::sk
