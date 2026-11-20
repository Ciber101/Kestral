// Display meshes follow the zoom. A body's triangles are made with a chord error that looks exact at
// normal zoom; zoomed in close, its curved faces would show their facets beside the exact edges and
// sketch lines. When the view has settled, the bodies on screen whose mesh is too coarse for it get a
// finer one, made on a worker from a copy of the shape. Zoomed back out, they return to the normal
// mesh so memory stays small.
#include "app/App.h"
#include "core/Log.h"
#include "kernel/Kernel.h"

#include <imgui.h>

#include <cmath>

namespace kst {

void App::refineMeshesForView() {
    if (viewMeshBusy_ || recomputeRunning_ || sliceWs_ || startOpen_) return;
    // Only once the view has stopped moving for a moment (not on every frame of a zoom).
    glm::dmat4 vp = viewProj();
    if (vp != viewMeshVp_) {
        viewMeshVp_ = vp;
        viewMeshStill_ = time_;
        return;
    }
    if (time_ - viewMeshStill_ < 0.3) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x < 2 || ds.y < 2) return;
    double hh = cam_.halfHeightAtTarget();
    if (!(hh > 0)) return;
    const double ppu = ds.y / (2.0 * hh);   // pixels per mm where the view is aimed
    const double want = 0.4 / ppu;          // chord error that stays under half a pixel there

    // The part whose mesh is furthest from what this view needs.
    uint32_t bestPart = 0;
    double bestRatio = 1.0, bestLin = 0;
    for (auto& [bid, b] : doc_.bodies) {
        if (!bodyShown(b)) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit == doc_.parts.end() || !pit->second.shape || !pit->second.mesh) continue;
        const MeshData& m = *pit->second.mesh;
        if (!m.bounds.valid()) continue;
        // On screen at all? (a corner behind the eye counts as maybe)
        Aabb wb = m.bounds.transformed(b.xform);
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        bool behind = false;
        for (int c = 0; c < 8; ++c) {
            glm::dvec4 q = vp * glm::dvec4((c & 1) ? wb.max.x : wb.min.x, (c & 2) ? wb.max.y : wb.min.y, (c & 4) ? wb.max.z : wb.min.z, 1.0);
            if (q.w <= 1e-9) { behind = true; break; }
            float sx = (float)((q.x / q.w * 0.5 + 0.5) * ds.x), sy = (float)((q.y / q.w * 0.5 + 0.5) * ds.y);
            x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        }
        bool onScreen = behind || !(x1 < 0 || y1 < 0 || x0 > ds.x || y0 > ds.y);
        double diag = std::max((double)m.bounds.diag(), 1e-6);
        double base = std::clamp(diag * 0.0005, 0.001, 2.0);           // the normal mesh
        double floor = std::max(diag * 2e-6, 2e-5);                      // finest we ever go
        double target = onScreen ? std::clamp(want, floor, base) : base;
        double have = m.lin > 0 ? m.lin : base;
        double ratio = have > target ? have / target : target / have;   // either too coarse or too fine
        bool tooCoarse = have > target * 1.6, tooFine = have < target / 8.0;
        if (!tooCoarse && !tooFine) continue;
        if (viewMeshFailed_.count(pit->second.shape.get()) && tooCoarse) continue;   // it got too big once
        if (ratio > bestRatio) { bestRatio = ratio; bestPart = pit->first; bestLin = target; }
    }
    if (!bestPart) return;
    auto pit = doc_.parts.find(bestPart);
    ShapePtr shape = pit->second.shape;
    MeshPtr old = pit->second.mesh;
    double lin = bestLin;
    double base = std::clamp((double)old->bounds.diag() * 0.0005, 0.001, 2.0);
    viewMeshBusy_ = true;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("view mesh", [this, jobs, shape, old, lin, base, bestPart] {
        MeshPtr m = lin >= base * 0.999 ? kernel::tessellate(*shape) : kernel::tessellateForView(shape, lin);
        if (m && !old->faceColors.empty() && old->faceColors.size() == m->faceCount)
            std::const_pointer_cast<MeshData>(m)->faceColors = old->faceColors;
        jobs->post([this, m, shape, bestPart] {
            viewMeshBusy_ = false;
            auto it = doc_.parts.find(bestPart);
            if (it == doc_.parts.end() || it->second.shape != shape) return;   // rebuilt meanwhile
            if (!m) {
                viewMeshFailed_.insert(shape.get());
                return;
            }
            it->second.mesh = m;
            markSceneDirty();
            activity();
        });
    }, [this](const std::string& err) {
        viewMeshBusy_ = false;
        log::warn("View mesh failed: " + err);
    });
}

} // namespace kst
