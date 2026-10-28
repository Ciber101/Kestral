#pragma once
#include "sketch/Sketch.h"

namespace kst::sk {

struct DragPoint { uint32_t point = 0; Vec2 target{0, 0}; };

struct SolveOptions {
    std::vector<DragPoint> drags;   // soft pulls toward targets (interactive dragging)
    double dragWeight = 0.05;
    int maxIter = 80;
    bool computeDof = false;
};

struct SolveResult {
    bool ok = true;              // false: constraints conflict or the solver did not converge (sketch left untouched)
    int iterations = 0;
    double maxResidual = 0;
    std::vector<uint32_t> bad;   // constraints with large residual when ok == false
};

// Newton / Levenberg Marquardt solver for 2D sketch constraints. Never throws and never leaves the
// sketch with NaN values: on failure the geometry is restored.
SolveResult solve(Sketch& sk, const SolveOptions& opt = {});

// Recomputes sk.dof and sk.defined from the current geometry.
void computeDof(Sketch& sk);

} // namespace kst::sk
