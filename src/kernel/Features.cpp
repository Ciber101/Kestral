#include "kernel/Features.h"
#include "kernel/ShapeHandle.h"
#include "kernel/Trsf.h"
#include "core/Log.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <HLRAppli_ReflectLines.hxx>
#include <HLRBRep_TypeOfResultingEdge.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepProj_Projection.hxx>
#include <gp_Circ.hxx>
#include <BRepFeat_SplitShape.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <cstdlib>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <ShapeFix_Solid.hxx>
#include <BRepLib.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_GTrsf.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRepGProp.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <GProp_GProps.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <gp_Pln.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <ElCLib.hxx>
#include <gp_Cone.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <set>
#include <BRepLProp_SLProps.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepAdaptor_CompCurve.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <BRepBndLib.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Builder.hxx>
#include <ShapeFix_Wire.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Shell.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeLine.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>

#include <algorithm>
#include <functional>
#include <map>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <atomic>

namespace kst::kernel {
static constexpr double kPiK = 3.14159265358979323846;
namespace {

const double kTwoPi = 6.283185307179586476925;

[[noreturn]] void rethrowOcct(const Standard_Failure& e, const char* where) {
    const char* msg = e.GetMessageString();
    throw std::runtime_error(std::string(where) + ": " + ((msg && *msg) ? msg : e.DynamicType()->Name()));
}

std::string fmtRadius(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3g", v);
    return b;
}

gp_Pnt pnt(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }
gp_Dir dir(const glm::dvec3& v) {
    glm::dvec3 n = v;
    double l = glm::length(n);
    if (l < 1e-12) return gp_Dir(0, 0, 1);
    n /= l;
    return gp_Dir(n.x, n.y, n.z);
}

double wrap2pi(double a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a;
}

int countSolids(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) ++n;
    return n;
}

// Angle of `v` around `normal`, measured CCW from the plane's reference direction.
double angleAround(const glm::dvec3& v, const glm::dvec3& normal, const glm::dvec3& ref) {
    glm::dvec3 e = v - glm::dot(v, normal) * normal;
    glm::dvec3 r = ref - glm::dot(ref, normal) * normal;
    double le = glm::length(e), lr = glm::length(r);
    if (le < 1e-12 || lr < 1e-12) return 0;
    double cosv = std::clamp(glm::dot(e, r) / (le * lr), -1.0, 1.0);
    double sinv = glm::dot(normal, glm::cross(r, e)) / (le * lr);
    return std::atan2(sinv, cosv);
}

TopoDS_Edge edgeFromSeg(const CurveSeg& c, const glm::dvec3& planeN) {
    if (c.kind == CurveSeg::Kind::Line)
        return BRepBuilderAPI_MakeEdge(pnt(c.a), pnt(c.b)).Edge();
    if (c.kind == CurveSeg::Kind::Circle) {
        gp_Ax2 ax(pnt(c.center), dir(planeN), dir(c.a - c.center));
        gp_Circ circ(ax, c.radius);
        return BRepBuilderAPI_MakeEdge(circ).Edge();
    }
    if (c.kind == CurveSeg::Kind::Ellipse || c.kind == CurveSeg::Kind::EllipArc) {
        glm::dvec3 n = glm::normalize(planeN);
        glm::dvec3 x = c.majorDir - glm::dot(c.majorDir, n) * n;
        if (glm::length(x) < 1e-9) throw std::runtime_error("The ellipse has no long axis");
        x = glm::normalize(x);
        double maj = c.radius, minr = c.minor;
        if (minr > maj) {
            std::swap(maj, minr);
            x = glm::cross(n, x);
        }
        if (!(maj > 1e-9) || !(minr > 1e-9)) throw std::runtime_error("The ellipse radii must be above zero");
        gp_Ax2 ax(pnt(c.center), dir(n), dir(x));
        gp_Elips el(ax, maj, minr);
        if (c.kind == CurveSeg::Kind::Ellipse) return BRepBuilderAPI_MakeEdge(el).Edge();
        double u0 = c.ang0, u1 = c.ang0 + c.sweep;
        if (!(c.sweep > 1e-7) || c.sweep > kTwoPi - 1e-7) return BRepBuilderAPI_MakeEdge(el).Edge();
        return BRepBuilderAPI_MakeEdge(el, u0, u1).Edge();
    }
    // Arc: normalize radius with the endpoints (the solver keeps them equal, be safe anyway).
    glm::dvec3 n = glm::normalize(planeN);
    double r = 0.5 * (glm::length(c.a - c.center) + glm::length(c.b - c.center));
    // The sketch's own radius when the ends agree with it: ends found where curves cross sit a few
    // micrometers off the circle, and an averaged radius made a 17.000001 wall beside the 17 bore
    // that never merged into one smooth face.
    if (c.radius > 1e-9 && std::abs(r - c.radius) < 1e-3) r = c.radius;
    if (r < 1e-9) throw std::runtime_error("Degenerate arc in the profile");
    gp_Ax2 ax(pnt(c.center), dir(n), dir(c.a - c.center));
    gp_Circ circ(ax, r);
    // Both arguments are vectors from the center. Passing the endpoint itself is only
    // correct when the center is the world origin, and an offset arc then sweeps the wrong way.
    double sweep = wrap2pi(angleAround(c.b - c.center, n, c.a - c.center));
    if (sweep < 1e-9 || sweep > kTwoPi - 1e-7) return BRepBuilderAPI_MakeEdge(circ).Edge();
    return BRepBuilderAPI_MakeEdge(circ, 0.0, sweep).Edge();
}

TopoDS_Wire wireFromSegs(const std::vector<CurveSeg>& segsIn, const glm::dvec3& planeN) {
    // Each segment starts exactly where the one before it ended: ends that differ by solver
    // rounding (1e-5 mm) are not joined by OCCT, and the loop silently loses segments.
    std::vector<CurveSeg> segs = segsIn;
    auto startOf = [](CurveSeg& c) -> glm::dvec3& { return c.back ? c.b : c.a; };
    auto endOf = [](CurveSeg& c) -> glm::dvec3& { return c.back ? c.a : c.b; };
    // Arc ends go exactly onto their circle (the sketch radius), and a line that meets an arc moves its
    // end to the arc's instead of the other way round: the arc keeps its true radius, so its face
    // merges with the rest of that cylinder.
    for (auto& c : segs) {
        if (c.kind != CurveSeg::Kind::Arc || !(c.radius > 1e-9)) continue;
        glm::dvec3 n = glm::length(c.normal) > 1e-12 ? glm::normalize(c.normal) : glm::normalize(planeN);
        for (glm::dvec3* e : {&c.a, &c.b}) {
            glm::dvec3 v = *e - c.center;
            v -= n * glm::dot(v, n);
            double l = glm::length(v);
            if (l > 1e-12 && std::abs(l - c.radius) < 1e-3) *e = c.center + v * (c.radius / l);
        }
    }
    for (size_t i = 0; i < segs.size(); ++i) {
        CurveSeg& cur = segs[i];
        if (cur.kind == CurveSeg::Kind::Circle || cur.kind == CurveSeg::Kind::Ellipse) continue;
        CurveSeg& prev = segs[(i + segs.size() - 1) % segs.size()];
        if (prev.kind == CurveSeg::Kind::Circle || prev.kind == CurveSeg::Kind::Ellipse || &prev == &cur) continue;
        glm::dvec3& s0 = startOf(cur);
        glm::dvec3& e0 = endOf(prev);
        double gap = glm::length(s0 - e0);
        if (!(gap > 0 && gap < 1e-3)) continue;
        if (cur.kind != CurveSeg::Kind::Line && prev.kind == CurveSeg::Kind::Line) e0 = s0;
        else s0 = e0;
    }
    BRepBuilderAPI_MakeWire mw;
    std::vector<TopoDS_Edge> edges;
    for (const CurveSeg& c : segs) {
        TopoDS_Edge e = edgeFromSeg(c, planeN);
        if (c.back) e = TopoDS::Edge(e.Reversed());
        edges.push_back(e);
        mw.Add(e);
        if (std::getenv("KESTRAL_FACE_DEBUG")) {
            GProp_GProps ge;
            BRepGProp::LinearProperties(e, ge);
            std::fprintf(stderr, "  add seg kind %d len %g -> error %d\n", (int)c.kind, ge.Mass(), (int)mw.Error());
        }
    }
    if (mw.IsDone()) return mw.Wire();
    // The pieces meet only to a few micrometers: an arc cut where a line crosses it ends a hair from
    // the line's corner (the crossing is solved numerically), and OCCT joins only ends that match to
    // a ten millionth of a millimeter. Join them in order and let ShapeFix close those tiny gaps.
    try {
        BRep_Builder bb;
        TopoDS_Wire w;
        bb.MakeWire(w);
        for (auto& e : edges) bb.Add(w, e);
        Handle(ShapeFix_Wire) fix = new ShapeFix_Wire();
        fix->Load(w);
        fix->ClosedWireMode() = Standard_True;
        fix->SetPrecision(1e-3);
        fix->SetMaxTolerance(1e-3);
        fix->FixReorderMode() = 0;   // keep the loop's own order
        fix->FixConnected(1e-3);
        fix->FixClosed(1e-3);
        TopoDS_Wire out = fix->Wire();
        if (!out.IsNull()) {
            out.Closed(BRep_Tool::IsClosed(out));
            TopoDS_Vertex v0, v1;
            TopExp::Vertices(out, v0, v1);
            if (!v0.IsNull() && !v1.IsNull() && (v0.IsSame(v1) || BRep_Tool::Pnt(v0).Distance(BRep_Tool::Pnt(v1)) < 1e-3)) return out;
        }
    } catch (const Standard_Failure&) {
    }
    throw std::runtime_error("Could not build a closed profile loop");
}

double extent2D(const TopoDS_Shape& s) {
    Bnd_Box b;
    BRepBndLib::Add(s, b);
    if (b.IsVoid()) return 0;
    Standard_Real x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return ((x1 - x0) + (y1 - y0)) * 0.5;   // in-plane growth proxy, plane parallel to an axis pair
}

// Offsets a closed planar wire outward (away from its interior) by `d`. Sign is auto-detected
// because OCCT's convention is opaque: try one side, keep whichever grows the loop.
// The loop moved d outward (d > 0, it grows) or inward (d < 0, it shrinks), in its own plane.
TopoDS_Wire offsetOutward(const TopoDS_Wire& w, double d, const glm::dvec3& planeN) {
    (void)planeN;
    if (std::abs(d) < 1e-12) return w;
    double before = extent2D(w);
    bool grow = d > 0;
    for (int sign : {1, -1}) {
        try {
            // Sharp corners stay sharp (a true draft, like Fusion's): the sides meet where they lean to.
            BRepOffsetAPI_MakeOffset mo(w, GeomAbs_Intersection);
            mo.Perform(sign * std::abs(d));
            TopoDS_Shape res = mo.Shape();
            // Result is a wire (possibly with several edges).
            TopExp_Explorer ex(res, TopAbs_WIRE);
            if (!ex.More()) continue;
            TopoDS_Wire out = TopoDS::Wire(ex.Current());
            double after = extent2D(out);
            if (grow ? after >= before - 1e-9 : (after <= before + 1e-9 && after > 1e-9)) return out;
        } catch (const Standard_Failure&) {
            continue;
        }
    }
    throw std::runtime_error(grow ? "Taper is too large for this profile (offset failed)"
                                  : "The taper leans in so far that the profile closes up before the end. Use a smaller angle or a shorter distance.");
}

TopoDS_Shape prism(const TopoDS_Shape& face, const glm::dvec3& v) {
    BRepPrimAPI_MakePrism m(face, gp_Vec(v.x, v.y, v.z));
    m.Build();
    if (!m.IsDone() || m.Shape().IsNull()) throw std::runtime_error("Extrude failed on this profile");
    return m.Shape();
}

TopoDS_Shape loft(const TopoDS_Wire& a, const TopoDS_Wire& b) {
    BRepOffsetAPI_ThruSections ts(Standard_True /*solid*/, Standard_True /*ruled*/);
    ts.AddWire(a);
    ts.AddWire(b);
    ts.Build();
    if (!ts.IsDone() || ts.Shape().IsNull()) throw std::runtime_error("Tapered extrude failed");
    return ts.Shape();
}

// Merges coplanar faces and collinear edges a boolean leaves behind (seams where a press pull
// or join meets the old body). Never fails: on any problem the input comes back unchanged.
// Steps made from fix1066 on merge on a copy even when the solid is broken: merging in place changed
// edges the inputs share, so another step built later from the same inputs (an undone first try,
// then the real one) got different geometry than a rebuild from the file does. Older steps keep
// the old way so their saved results rebuild as they were.
std::atomic<bool> g_copyBeforeUnify{false};

TopoDS_Shape unify(const TopoDS_Shape& s) {
    try {
        // Merging faces can break a solid that was fine: two halves of a hole wall (the hole through
        // a wall and through a slab pressed onto it) merge into one cylinder whose seam no longer
        // fits, and the hole then draws torn and blocks fillets and chamfers. The merge runs on a
        // copy (it changes shared edges in place), and the unmerged solid is kept when the merged
        // one is broken.
        bool wasValid = BRepCheck_Analyzer(s).IsValid() != 0;
        TopoDS_Shape work = (wasValid || g_copyBeforeUnify.load()) ? BRepBuilderAPI_Copy(s, Standard_True, Standard_False).Shape() : s;
        ShapeUpgrade_UnifySameDomain u(work, Standard_True, Standard_True, Standard_True);
        u.Build();
        TopoDS_Shape r = u.Shape();
        if (r.IsNull() || countSolids(r) != countSolids(s)) return s;
        if (!wasValid || BRepCheck_Analyzer(r).IsValid()) return r;
        // Second try: merge flat faces only. Edges of curved faces are kept as they are.
        TopoDS_Shape work2 = BRepBuilderAPI_Copy(s, Standard_True, Standard_False).Shape();
        TopTools_MapOfShape keep;
        for (TopExp_Explorer fx(work2, TopAbs_FACE); fx.More(); fx.Next()) {
            BRepAdaptor_Surface sa(TopoDS::Face(fx.Current()), Standard_False);
            if (sa.GetType() == GeomAbs_Plane) continue;
            for (TopExp_Explorer ex(fx.Current(), TopAbs_EDGE); ex.More(); ex.Next()) keep.Add(ex.Current());
        }
        ShapeUpgrade_UnifySameDomain u2(work2, Standard_True, Standard_True, Standard_True);
        u2.KeepShapes(keep);
        u2.Build();
        TopoDS_Shape r2 = u2.Shape();
        if (!r2.IsNull() && countSolids(r2) == countSolids(s) && BRepCheck_Analyzer(r2).IsValid()) return r2;
        return s;
    } catch (...) {
        return s;
    }
}

TopoDS_Shape boolOp(bool fuse, const TopoDS_Shape& a, const TopoDS_Shape& b, const char* what) {
    try {
        if (fuse) {
            BRepAlgoAPI_Fuse op(a, b);
            op.Build();
            if (!op.IsDone()) throw std::runtime_error(std::string(what) + " failed");
            return unify(op.Shape());
        }
        BRepAlgoAPI_Cut op(a, b);
        op.Build();
        if (!op.IsDone()) throw std::runtime_error(std::string(what) + " failed");
        return unify(op.Shape());
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, what);
    }
}

// Sides leaning out by taperDeg (in when negative): the outline grows along the extrude and the holes
// shrink with it, so every side face has the same draft.
TopoDS_Shape taperedPrism(const RegionLoops& loops, const glm::dvec3& dirN, double dist, double taperDeg) {
    double tn = std::tan(taperDeg * kPiK / 180.0);
    double grow = dist * tn;
    glm::dvec3 n = loops[0].empty() ? dirN : loops[0][0].normal;
    if (glm::dot(n, dirN) < 0) n = -n;

    TopoDS_Wire outer = wireFromSegs(loops[0], n);
    TopoDS_Wire topOuter = offsetOutward(outer, grow, n);
    // Lift the top wire along the direction.
    gp_Trsf lift;
    lift.SetTranslation(gp_Vec(dirN.x * dist, dirN.y * dist, dirN.z * dist));
    topOuter.Move(TopLoc_Location(lift));
    TopoDS_Shape solid = loft(outer, topOuter);

    // Holes: loft their walls past both ends and cut them out.
    double pad = std::max(dist * 0.05, 1.0);
    for (size_t i = 1; i < loops.size(); ++i) {
        try {
            TopoDS_Wire start = wireFromSegs(loops[i], n);
            // The hole's wall carried past both ends at the same lean: wider below, narrower above
            // when the outline grows.
            TopoDS_Wire base = offsetOutward(start, pad * tn, n);
            TopoDS_Wire top = offsetOutward(start, -(dist + pad) * tn, n);
            gp_Trsf down, up;
            down.SetTranslation(gp_Vec(-dirN.x * pad, -dirN.y * pad, -dirN.z * pad));
            up.SetTranslation(gp_Vec(dirN.x * (dist + pad), dirN.y * (dist + pad), dirN.z * (dist + pad)));
            base.Move(TopLoc_Location(down));
            top.Move(TopLoc_Location(up));
            TopoDS_Shape hole = loft(base, top);
            solid = boolOp(false, solid, hole, "Tapered extrude hole");
        } catch (const Standard_Failure& e) {
            rethrowOcct(e, "tapered extrude hole");
        }
    }
    return solid;
}

} // namespace

ShapePtr buildFace(const RegionLoops& loops) {
    if (loops.empty() || loops[0].empty()) throw std::runtime_error("Empty profile");
    glm::dvec3 n = loops[0][0].normal;
    glm::dvec3 o = loops[0][0].a;
    try {
        BRepBuilderAPI_MakeFace mf(gp_Pln(pnt(o), dir(n)), wireFromSegs(loops[0], n));
        for (size_t i = 1; i < loops.size(); ++i) mf.Add(wireFromSegs(loops[i], n));
        TopoDS_Face f = mf.Face();
        ShapeFix_Face fix(f);
        fix.FixOrientation();
        TopoDS_Face out = fix.Face();
        if (std::getenv("KESTRAL_FACE_DEBUG")) {
            GProp_GProps g0, g1;
            BRepGProp::SurfaceProperties(f, g0);
            BRepGProp::SurfaceProperties(out, g1);
            std::fprintf(stderr, "buildFace: raw area %g, fixed area %g, loops %zu, segs %zu, n (%g,%g,%g)\n", g0.Mass(), g1.Mass(), loops.size(), loops[0].size(), n.x, n.y, n.z);
            for (TopExp_Explorer ex(out, TopAbs_EDGE); ex.More(); ex.Next()) { GProp_GProps ge; BRepGProp::LinearProperties(ex.Current(), ge); std::fprintf(stderr, "  edge len %g\n", ge.Mass()); }
            for (auto& c : loops[0])
                std::fprintf(stderr, "  seg kind %d back %d a(%g,%g,%g) b(%g,%g,%g) c(%g,%g,%g)\n", (int)c.kind, (int)c.back, c.a.x, c.a.y, c.a.z, c.b.x, c.b.y, c.b.z, c.center.x, c.center.y, c.center.z);
        }
        if (out.IsNull()) throw std::runtime_error("Could not build the profile face");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = out;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "profile face");
    }
}

ShapePtr extrudeRegion(const RegionLoops& loops, const glm::dvec3& dir, double dist, bool symmetric, double taperDeg) {
    if (dist <= 1e-9) throw std::runtime_error("Distance must be greater than zero");
    if (glm::length(dir) < 0.5) throw std::runtime_error("Bad extrude direction");
    glm::dvec3 dirN = glm::normalize(dir);
    double half = symmetric ? dist * 0.5 : dist;
    bool tapered = std::abs(taperDeg) > 1e-7;
    try {
        TopoDS_Shape shape;
        if (tapered) {
            shape = taperedPrism(loops, dirN, half, taperDeg);
            if (symmetric) {
                TopoDS_Shape other = taperedPrism(loops, -dirN, half, taperDeg);
                shape = boolOp(true, shape, other, "Symmetric extrude");
            }
        } else {
            ShapePtr face = buildFace(loops);
            shape = prism(face->shape, dirN * half);
            if (symmetric) shape = boolOp(true, shape, prism(face->shape, -dirN * half), "Symmetric extrude");
        }
        if (countSolids(shape) == 0) throw std::runtime_error("The profile did not produce a solid");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = shape;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "extrude");
    }
}

namespace {

std::vector<glm::dvec3> ribPoints(const std::vector<CurveSeg>& chain) {
    std::vector<glm::dvec3> pts;
    auto add = [&](const glm::dvec3& p) {
        if (pts.empty() || glm::length(pts.back() - p) > 1e-6) pts.push_back(p);
    };
    for (auto& c : chain) {
        if (c.kind == CurveSeg::Kind::Circle) throw std::runtime_error("A rib needs an open sketch");
        if (c.kind == CurveSeg::Kind::Line) {
            add(c.a);
            add(c.b);
            continue;
        }
        glm::dvec3 n = glm::normalize(c.normal);
        if (c.back) n = -n;
        glm::dvec3 start = c.back ? c.b : c.a;
        glm::dvec3 end = c.back ? c.a : c.b;
        glm::dvec3 x = start - c.center;
        if (glm::length(x) < 1e-9) throw std::runtime_error("A rib curve is degenerate");
        x = glm::normalize(x);
        glm::dvec3 y = glm::cross(n, x);
        double sweep = angleAround(end - c.center, n, start - c.center);
        if (sweep < 0) sweep += kTwoPi;
        if (sweep < 1e-6) sweep = kTwoPi;
        int steps = std::max(4, (int)std::ceil(sweep / (kTwoPi / 16.0)));
        for (int i = 0; i <= steps; ++i) {
            double a = sweep * (double)i / (double)steps;
            add(c.center + (x * std::cos(a) + y * std::sin(a)) * c.radius);
        }
    }
    if (pts.size() >= 3 && glm::length(pts.front() - pts.back()) < 1e-4)
        throw std::runtime_error("A rib needs an open sketch");
    if (pts.size() < 2) throw std::runtime_error("The sketch has no curve");
    return pts;
}

RegionLoops ribLoop(const std::vector<glm::dvec3>& pts, const glm::dvec3& planeN, double thickness, int side) {
    // side 2 is the other side of the curve. Ribs only pass 0 or 1, so they stay put.
    double halfL = side == 0 ? thickness * 0.5 : side == 2 ? 0.0 : thickness;
    double halfR = side == 0 ? thickness * 0.5 : side == 2 ? thickness : 0.0;
    glm::dvec3 N = glm::normalize(planeN);
    auto tangent = [&](int i) {
        glm::dvec3 d = pts[i + 1] - pts[i];
        double len = glm::length(d);
        if (len < 1e-9) throw std::runtime_error("The sketch has a zero length curve");
        return d / len;
    };
    auto left = [&](const glm::dvec3& t) { return glm::normalize(glm::cross(N, t)); };
    std::vector<glm::dvec3> L, R;
    int n = (int)pts.size();
    for (int i = 0; i < n; ++i) {
        glm::dvec3 off;
        if (i == 0) off = left(tangent(0));
        else if (i == n - 1) off = left(tangent(n - 2));
        else {
            glm::dvec3 n0 = left(tangent(i - 1));
            glm::dvec3 n1 = left(tangent(i));
            glm::dvec3 sum = n0 + n1;
            if (glm::length(sum) < 1e-6) off = n0;
            else {
                off = glm::normalize(sum);
                double d = glm::dot(off, n0);
                if (d < 0.25) d = 0.25;
                off /= d;
            }
        }
        L.push_back(pts[i] + off * halfL);
        R.push_back(pts[i] - off * halfR);
    }
    std::vector<CurveSeg> loop;
    auto push = [&](const glm::dvec3& a, const glm::dvec3& b) {
        if (glm::length(b - a) < 1e-8) return;
        CurveSeg s;
        s.kind = CurveSeg::Kind::Line;
        s.a = a;
        s.b = b;
        s.normal = N;
        loop.push_back(s);
    };
    for (int i = 0; i + 1 < n; ++i) push(L[i], L[i + 1]);
    push(L.back(), R.back());
    for (int i = n - 1; i > 0; --i) push(R[i], R[i - 1]);
    push(R.front(), L.front());
    if (loop.size() < 3) throw std::runtime_error("Thickness must be greater than zero");
    return {loop};
}

RegionLoops webLoop(const std::vector<glm::dvec3>& pts, const glm::dvec3& planeN, const glm::dvec3& extent, double thickness, int side) {
    glm::dvec3 N = glm::normalize(planeN);
    glm::dvec3 E = glm::normalize(extent);
    double halfL = side == 0 ? thickness * 0.5 : thickness;
    double halfR = side == 0 ? thickness * 0.5 : 0;
    int n = (int)pts.size();
    std::vector<glm::dvec3> L(n), R(n);
    for (int i = 0; i < n; ++i) {
        L[i] = pts[i] + N * halfL;
        R[i] = pts[i] - N * halfR;
    }
    std::vector<CurveSeg> loop;
    auto push = [&](const glm::dvec3& a, const glm::dvec3& b) {
        if (glm::length(b - a) < 1e-8) return;
        CurveSeg s;
        s.kind = CurveSeg::Kind::Line;
        s.a = a;
        s.b = b;
        s.normal = E;
        loop.push_back(s);
    };
    for (int i = 0; i + 1 < n; ++i) push(L[i], L[i + 1]);
    push(L.back(), R.back());
    for (int i = n - 1; i > 0; --i) push(R[i], R[i - 1]);
    push(R.front(), L.front());
    if (loop.size() < 3) throw std::runtime_error("Thickness must be greater than zero");
    return {loop};
}

glm::dvec3 webExtent(const std::vector<glm::dvec3>& pts, const glm::dvec3& planeN, bool flip) {
    glm::dvec3 chord = pts.back() - pts.front();
    if (glm::length(chord) < 1e-6) throw std::runtime_error("The sketch has no curve");
    glm::dvec3 t = glm::normalize(chord);
    glm::dvec3 E = glm::cross(glm::normalize(planeN), t);
    if (glm::length(E) < 1e-6) throw std::runtime_error("The sketch must lie in its plane");
    E = glm::normalize(E);
    if (flip) E = -E;
    return E;
}

ShapePtr joinWall(const ShapePtr& slab, const glm::dvec3& planeO, const glm::dvec3& dir, const TopoDS_Shape& body, const char* what) {
    TopoDS_Shape gap = boolOp(false, slab->shape, body, what);
    if (countSolids(gap) == 0) throw std::runtime_error(std::string("The ") + what + " is already inside the body");
    TopoDS_Shape piece;
    double best = 1e300;
    bool any = false;
    for (TopExp_Explorer ex(gap, TopAbs_SOLID); ex.More(); ex.Next()) {
        GProp_GProps g;
        BRepGProp::VolumeProperties(ex.Current(), g);
        if (g.Mass() < 1e-6) continue;
        gp_Pnt c = g.CentreOfMass();
        double u = glm::dot(glm::dvec3(c.X(), c.Y(), c.Z()) - planeO, dir);
        if (u < best) { best = u; piece = ex.Current(); any = true; }
    }
    if (!any) throw std::runtime_error(std::string("The ") + what + " does not meet a body");
    GProp_GProps whole, kept;
    BRepGProp::VolumeProperties(slab->shape, whole);
    BRepGProp::VolumeProperties(piece, kept);
    if (kept.Mass() > whole.Mass() * 0.98) throw std::runtime_error(std::string("The ") + what + " does not meet a body");
    auto h = std::make_shared<ShapeHandle>();
    h->shape = boolOp(true, body, piece, what);
    return h;
}

} // namespace

RegionLoops thinProfile(const std::vector<CurveSeg>& chain, const glm::dvec3& planeN, double thickness, int side) {
    if (!(thickness > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
    if (glm::length(planeN) < 0.5) throw std::runtime_error("Bad wall direction");
    int s = side < 0 ? 0 : side > 2 ? 2 : side;
    try {
        return ribLoop(ribPoints(chain), planeN, thickness, s);
    } catch (const std::runtime_error& e) {
        std::string w = e.what();
        if (w.find("open sketch") != std::string::npos) throw std::runtime_error("Thin needs an open curve");
        throw;
    }
}

ShapePtr ribExtrusion(const std::vector<CurveSeg>& chain, const glm::dvec3& planeN, double thickness, int side, const glm::dvec3& dir, double dist) {
    if (!(thickness > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
    if (!(dist > 1e-6)) throw std::runtime_error("Flip the rib. It does not point at the body.");
    if (glm::length(dir) < 0.5 || glm::length(planeN) < 0.5) throw std::runtime_error("Bad rib direction");
    return extrudeRegion(ribLoop(ribPoints(chain), planeN, thickness, side), glm::normalize(dir), dist, false, 0);
}

ShapePtr ribToNext(const std::vector<CurveSeg>& chain, const glm::dvec3& planeO, const glm::dvec3& planeN,
                   double thickness, int side, const glm::dvec3& dir, const ShapePtr& target, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("The rib does not meet a body");
    glm::dvec3 d = glm::normalize(dir);
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    if (box.IsVoid()) throw std::runtime_error("The rib does not meet a body");
    Standard_Real x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    glm::dvec3 corners[8] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                             {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
    double start = 1e300;
    for (auto& seg : chain) {
        start = std::min(start, glm::dot(seg.a, d));
        start = std::min(start, glm::dot(seg.b, d));
    }
    double reach = 0;
    for (auto& c : corners) reach = std::max(reach, glm::dot(c, d) - start);
    reach += 5.0;
    ShapePtr slab = ribExtrusion(chain, planeN, thickness, side, d, reach);
    return joinWall(slab, planeO, d, body, "rib");
}

ShapePtr webExtrusion(const std::vector<CurveSeg>& chain, const glm::dvec3& planeN, double thickness, int side, bool flip, double dist) {
    if (!(thickness > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
    if (!(dist > 1e-6)) throw std::runtime_error("Flip the web. It does not point at the body.");
    if (glm::length(planeN) < 0.5) throw std::runtime_error("Bad web direction");
    auto pts = ribPoints(chain);
    glm::dvec3 E = webExtent(pts, planeN, flip);
    return extrudeRegion(webLoop(pts, planeN, E, thickness, side), E, dist, false, 0);
}

ShapePtr webToNext(const std::vector<CurveSeg>& chain, const glm::dvec3& planeO, const glm::dvec3& planeN,
                   double thickness, int side, bool flip, const ShapePtr& target, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("The web does not meet a body");
    auto pts = ribPoints(chain);
    glm::dvec3 d = webExtent(pts, planeN, flip);
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    if (box.IsVoid()) throw std::runtime_error("The web does not meet a body");
    Standard_Real x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    glm::dvec3 corners[8] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                             {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
    double start = 1e300;
    for (auto& p : pts) start = std::min(start, glm::dot(p, d));
    double reach = 0;
    for (auto& c : corners) reach = std::max(reach, glm::dot(c, d) - start);
    reach += 5.0;
    ShapePtr slab = webExtrusion(chain, planeN, thickness, side, flip, reach);
    return joinWall(slab, planeO, d, body, "web");
}

ShapePtr embossOnBody(const std::vector<RegionLoops>& regions, const glm::dvec3& planeO, const glm::dvec3& dir,
                      double depth, bool deboss, const ShapePtr& target, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("The emboss does not meet a body");
    if (regions.empty()) throw std::runtime_error("Emboss needs a closed profile");
    if (!(depth > 1e-6)) throw std::runtime_error("Depth must be greater than zero");
    if (glm::length(dir) < 0.5) throw std::runtime_error("Bad emboss direction");
    glm::dvec3 d0 = glm::normalize(dir);
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    if (box.IsVoid()) throw std::runtime_error("The emboss does not meet a body");
    Standard_Real x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    glm::dvec3 corners[8] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                             {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
    // Projects along the sketch normal; when the body is on the other side of the sketch (its
    // normal points away, common for sketches on side planes), projects the other way instead.
    TopoDS_Shape tool;
    bool any = false;
    for (int attempt = 0; attempt < 2 && !any; ++attempt) {
    glm::dvec3 d = attempt == 0 ? d0 : -d0;
    double u0 = glm::dot(planeO, d);
    double reach = 0;
    for (auto& c : corners) reach = std::max(reach, glm::dot(c, d) - u0);
    reach += 5.0;
    for (auto& region : regions) {
        ShapePtr probe = extrudeRegion(region, d, reach, false, 0);
        BRepAlgoAPI_Common common(probe->shape, body);
        common.Build();
        if (!common.IsDone() || countSolids(common.Shape()) == 0) continue;
        Bnd_Box hit;
        BRepBndLib::Add(common.Shape(), hit);
        if (hit.IsVoid()) continue;
        Standard_Real a0, b0, c0, a1, b1, c1;
        hit.Get(a0, b0, c0, a1, b1, c1);
        glm::dvec3 hc[8] = {{a0, b0, c0}, {a1, b0, c0}, {a0, b1, c0}, {a1, b1, c0},
                            {a0, b0, c1}, {a1, b0, c1}, {a0, b1, c1}, {a1, b1, c1}};
        double uHit = 1e300;
        for (auto& c : hc) uHit = std::min(uHit, glm::dot(c, d));
        double shift = uHit - u0;
        gp_Trsf mv;
        mv.SetTranslation(gp_Vec(d.x * shift, d.y * shift, d.z * shift));
        TopoDS_Shape face = buildFace(region)->shape.Moved(TopLoc_Location(mv));
        glm::dvec3 extrude = (deboss ? d : -d) * depth;
        TopoDS_Shape pad = prism(face, extrude);
        tool = any ? boolOp(true, tool, pad, "emboss") : pad;
        any = true;
    }
    }
    if (!any) throw std::runtime_error("The emboss does not meet a body");
    auto h = std::make_shared<ShapeHandle>();
    h->shape = boolOp(!deboss, deboss ? body : tool, deboss ? tool : body, "emboss");
    return h;
}

ShapePtr bossOnBody(const glm::dvec3& origin, const glm::dvec3& axis, double diameter, double height, double holeDia,
                    const ShapePtr& target, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("Select the body the boss stands on");
    if (!(diameter > 1e-6)) throw std::runtime_error("Diameter must be greater than zero");
    if (!(height > 1e-6)) throw std::runtime_error("Height must be greater than zero");
    if (holeDia < -1e-9) throw std::runtime_error("Hole diameter must be zero or more");
    if (holeDia >= diameter - 1e-6) throw std::runtime_error("The hole must be smaller than the boss");
    if (glm::length(axis) < 0.5) throw std::runtime_error("Bad boss direction");
    glm::dvec3 a = glm::normalize(axis);
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    const double bury = 0.05;
    gp_Ax2 ax(pnt(origin - a * bury), dir(a));
    TopoDS_Shape stud = BRepPrimAPI_MakeCylinder(ax, diameter * 0.5, height + bury).Shape();
    TopoDS_Shape joined = boolOp(true, body, stud, "boss");
    if (holeDia > 1e-6) {
        gp_Ax2 hx(pnt(origin - a * bury), dir(a));
        TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(hx, holeDia * 0.5, height + bury + 0.05).Shape();
        joined = boolOp(false, joined, hole, "boss");
    }
    auto h = std::make_shared<ShapeHandle>();
    h->shape = joined;
    return h;
}

ShapePtr snapOnBody(const glm::dvec3& origin, const glm::dvec3& axis, double length, double width, double thick,
                    double hook, double hookLen, const ShapePtr& target, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("Select the body the snap stands on");
    if (!(length > 1e-6)) throw std::runtime_error("Length must be greater than zero");
    if (!(width > 1e-6)) throw std::runtime_error("Width must be greater than zero");
    if (!(thick > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
    if (hook < -1e-9) throw std::runtime_error("The catch must be zero or more");
    if (hookLen < -1e-9) throw std::runtime_error("The catch length must be zero or more");
    if (hook > 1e-6 && !(hookLen > 1e-6)) throw std::runtime_error("The catch needs a length");
    if (hookLen > length + 1e-6) throw std::runtime_error("The catch must be shorter than the beam");
    if (glm::length(axis) < 0.5) throw std::runtime_error("Bad snap direction");
    glm::dvec3 beam = glm::normalize(axis);
    glm::dvec3 ref = std::abs(beam.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
    glm::dvec3 widthDir = glm::normalize(glm::cross(ref, beam));
    glm::dvec3 hookDir = glm::normalize(glm::cross(beam, widthDir));
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    const double bury = 0.05;
    glm::dvec3 corner = origin - widthDir * (width * 0.5) - hookDir * (thick * 0.5) - beam * bury;
    gp_Ax2 ax(pnt(corner), dir(beam), dir(widthDir));
    TopoDS_Shape tool = BRepPrimAPI_MakeBox(ax, width, thick, length + bury).Shape();
    if (hook > 1e-6 && hookLen > 1e-6) {
        const double lap = 0.05;
        glm::dvec3 hc = origin - widthDir * (width * 0.5) + hookDir * (thick * 0.5 - lap) + beam * (length - hookLen);
        gp_Ax2 hx(pnt(hc), dir(beam), dir(widthDir));
        TopoDS_Shape catcher = BRepPrimAPI_MakeBox(hx, width, hook + lap, hookLen).Shape();
        tool = boolOp(true, tool, catcher, "snap");
    }
    TopoDS_Shape joined = boolOp(true, body, tool, "snap");
    if (countSolids(joined) != 1) throw std::runtime_error("The snap does not meet a body");
    auto h = std::make_shared<ShapeHandle>();
    h->shape = joined;
    return h;
}

ShapePtr restOnBody(const std::vector<RegionLoops>& regions, const glm::dvec3& dir, double thickness,
                    const ShapePtr& target, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("Select the body the rest stands on");
    if (regions.empty()) throw std::runtime_error("Rest needs a closed profile");
    if (!(thickness > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
    if (glm::length(dir) < 0.5) throw std::runtime_error("Bad rest direction");
    glm::dvec3 d = glm::normalize(dir);
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    const double bury = 0.05;
    gp_Trsf back;
    back.SetTranslation(gp_Vec(-d.x * bury, -d.y * bury, -d.z * bury));
    TopoDS_Shape tool;
    bool any = false;
    for (auto& region : regions) {
        ShapePtr pad = extrudeRegion(region, d, thickness + bury, false, 0);
        TopoDS_Shape moved = pad->shape.Moved(TopLoc_Location(back));
        tool = any ? boolOp(true, tool, moved, "rest") : moved;
        any = true;
    }
    TopoDS_Shape joined = boolOp(true, body, tool, "rest");
    if (countSolids(joined) != 1) throw std::runtime_error("The rest does not meet a body");
    auto h = std::make_shared<ShapeHandle>();
    h->shape = joined;
    return h;
}

ShapePtr lipOnBody(const std::vector<RegionLoops>& regions, const glm::dvec3& dir, double width, double height,
                   bool groove, const ShapePtr& target, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("Select the body the lip stands on");
    if (regions.empty()) throw std::runtime_error("Lip needs a closed profile");
    if (!(width > 1e-6)) throw std::runtime_error("Width must be greater than zero");
    if (!(height > 1e-6)) throw std::runtime_error("Height must be greater than zero");
    if (glm::length(dir) < 0.5) throw std::runtime_error("Bad lip direction");
    glm::dvec3 d = glm::normalize(dir);
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    auto inward = [](const TopoDS_Wire& w, double dist) {
        double before = extent2D(w);
        for (GeomAbs_JoinType join : {GeomAbs_Intersection, GeomAbs_Arc}) {
            for (int sign : {1, -1}) {
                try {
                    BRepOffsetAPI_MakeOffset mo(w, join);
                    mo.Perform(sign * dist);
                    if (!mo.IsDone()) continue;
                    TopExp_Explorer ex(mo.Shape(), TopAbs_WIRE);
                    if (!ex.More()) continue;
                    TopoDS_Wire out = TopoDS::Wire(ex.Current());
                    if (extent2D(out) < before - 1e-4) return out;
                } catch (const Standard_Failure&) {}
            }
        }
        throw std::runtime_error("The lip is wider than the profile");
    };
    const double bury = 0.05;
    gp_Trsf back;
    back.SetTranslation(gp_Vec(-d.x * bury, -d.y * bury, -d.z * bury));
    gp_Trsf innerShift;
    innerShift.SetTranslation(gp_Vec(-d.x * (bury + 0.1), -d.y * (bury + 0.1), -d.z * (bury + 0.1)));
    TopoDS_Shape tool;
    bool any = false;
    for (auto& region : regions) {
        if (region.empty() || region[0].empty()) continue;
        glm::dvec3 n = region[0][0].normal;
        if (glm::length(n) < 0.5) n = d;
        TopoDS_Wire outer = wireFromSegs(region[0], n);
        TopoDS_Wire hole = inward(outer, width);
        RegionLoops outerOnly;
        outerOnly.push_back(region[0]);
        ShapePtr outerFace = buildFace(outerOnly);
        BRepBuilderAPI_MakeFace mf(hole, Standard_True);
        if (!mf.IsDone()) throw std::runtime_error("The lip is wider than the profile");
        TopoDS_Shape outerSolid = prism(outerFace->shape, d * (height + bury)).Moved(TopLoc_Location(back));
        TopoDS_Shape innerSolid = prism(mf.Face(), d * (height + bury + 0.2)).Moved(TopLoc_Location(innerShift));
        TopoDS_Shape frame = boolOp(false, outerSolid, innerSolid, "lip");
        tool = any ? boolOp(true, tool, frame, "lip") : frame;
        any = true;
    }
    if (!any) throw std::runtime_error("Lip needs a closed profile");
    TopoDS_Shape joined = boolOp(!groove, groove ? body : tool, groove ? tool : body, "lip");
    if (countSolids(joined) != 1) throw std::runtime_error("The lip does not meet a body");
    auto h = std::make_shared<ShapeHandle>();
    h->shape = joined;
    return h;
}

ShapePtr revolveRegion(const RegionLoops& loops, const glm::dvec3& axisO, const glm::dvec3& axisD, double angleDeg) {
    if (glm::length(axisD) < 0.5) throw std::runtime_error("Bad revolve axis");
    if (angleDeg <= 1e-6 || angleDeg > 360.0 + 1e-6) throw std::runtime_error("Angle must be between 0 and 360 degrees");
    ShapePtr face = buildFace(loops);
    try {
        gp_Ax1 ax(pnt(axisO), dir(axisD));
        BRepPrimAPI_MakeRevol m(face->shape, ax, angleDeg * kPiK / 180.0);
        m.Build();
        if (!m.IsDone() || m.Shape().IsNull()) throw std::runtime_error("Revolve failed (does the profile cross the axis?)");
        if (countSolids(m.Shape()) == 0) throw std::runtime_error("The profile did not produce a solid");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = m.Shape();
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "revolve");
    }
}

std::vector<ShapePtr> applyBool(BoolOp op, const ShapePtr& tool, const std::vector<BoolTarget>& targets) {
    std::vector<ShapePtr> out;
    auto push = [&](const TopoDS_Shape& s, const char* what) {
        if (s.IsNull() || countSolids(s) == 0)
            throw std::runtime_error(std::string("The operation produced no solid (") + what + ")");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = what[0] == 'n' ? s : unify(s);
        out.push_back(h);
    };
    if (op == BoolOp::NewBody) {
        push(tool->shape, "new body");
        return out;
    }
    if (targets.empty()) throw std::runtime_error("Select the bodies this feature should modify");
    if (op == BoolOp::Join) {
        try {
            TopoDS_Shape acc = tool->shape;
            for (auto& t : targets) {
                gp_Trsf tr;
                const glm::mat4& m = t.xform;
                tr = trsfOfMat(m);
                TopoDS_Shape other = t.shape->shape.Moved(TopLoc_Location(tr));
                // A join can never lose material: the result must hold at least the bigger input. OCCT
                // sometimes drops one argument (seen with bodies cut by imported STEP parts); then try the
                // other order, then repaired shapes, before giving up.
                auto vol = [](const TopoDS_Shape& sh) {
                    GProp_GProps g;
                    BRepGProp::VolumeProperties(sh, g);
                    return std::abs(g.Mass());
                };
                double need = std::max(vol(acc), vol(other)) * 0.999;
                auto fuse = [&](const TopoDS_Shape& a, const TopoDS_Shape& b, TopoDS_Shape& res) {
                    BRepAlgoAPI_Fuse opf(a, b);
                    opf.Build();
                    if (!opf.IsDone()) return false;
                    res = opf.Shape();
                    return countSolids(res) > 0 && vol(res) >= need;
                };
                TopoDS_Shape res;
                if (!fuse(acc, other, res) && !fuse(other, acc, res)) {
                    auto fixed = [](const TopoDS_Shape& sh) {
                        Handle(ShapeFix_Shape) fx = new ShapeFix_Shape(sh);
                        fx->Perform();
                        return fx->Shape();
                    };
                    TopoDS_Shape fa = fixed(acc), fb = fixed(other);
                    if (!fuse(fa, fb, res) && !fuse(fb, fa, res))
                        throw std::runtime_error("Join lost part of a body (the shapes did not fuse cleanly). Try moving them so they overlap a little more.");
                }
                acc = res;
            }
            push(acc, "join");
        } catch (const Standard_Failure& e) {
            rethrowOcct(e, "join");
        }
        return out;
    }
    for (auto& t : targets) {
        try {
            gp_Trsf tr;
            const glm::mat4& m = t.xform;
            tr = trsfOfMat(m);
            TopoDS_Shape world = t.shape->shape.Moved(TopLoc_Location(tr));
            if (op == BoolOp::Cut) {
                BRepAlgoAPI_Cut opb(world, tool->shape);
                opb.Build();
                if (!opb.IsDone()) throw std::runtime_error("Cut failed");
                push(opb.Shape(), "cut");
            } else {
                BRepAlgoAPI_Common opb(world, tool->shape);
                opb.Build();
                if (!opb.IsDone()) throw std::runtime_error("Intersect failed");
                push(opb.Shape(), "intersect");
            }
        } catch (const Standard_Failure& e) {
            rethrowOcct(e, op == BoolOp::Cut ? "cut" : "intersect");
        }
    }
    return out;
}

// ---------------------------------------------------------------- M4: modification tools

namespace {

TopoDS_Shape copyShape(const TopoDS_Shape& s) {
    // OCCT algorithms can mutate inputs in place on some paths; always hand them a private copy.
    BRepBuilderAPI_Transform t(s, gp_Trsf(), Standard_True);
    return t.Shape();
}

} // namespace

uint32_t edgeCount(const ShapePtr& shape) {
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(shape->shape, TopAbs_EDGE, m);
    return (uint32_t)m.Extent();
}

uint32_t faceCount(const ShapePtr& shape) {
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(shape->shape, TopAbs_FACE, m);
    return (uint32_t)m.Extent();
}

uint32_t vertexCount(const ShapePtr& shape) {
    if (!shape) return 0;
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(shape->shape, TopAbs_VERTEX, m);
    return (uint32_t)m.Extent();
}

void vertexPoints(const ShapePtr& shape, const glm::mat4& xf, std::vector<glm::dvec3>& out) {
    out.clear();
    if (!shape) return;
    try {
        TopTools_IndexedMapOfShape m;
        TopExp::MapShapes(shape->shape, TopAbs_VERTEX, m);
        glm::dmat4 tr(xf);
        out.reserve((size_t)m.Extent());
        for (int i = 1; i <= m.Extent(); ++i) {
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(m(i)));
            out.push_back(glm::dvec3(tr * glm::dvec4(p.X(), p.Y(), p.Z(), 1.0)));
        }
    } catch (...) {
        out.clear();
    }
}

bool vertexPoint(const ShapePtr& shape, const glm::mat4& xf, uint32_t vertex, glm::dvec3& world) {
    std::vector<glm::dvec3> pts;
    vertexPoints(shape, xf, pts);
    if (vertex >= pts.size()) return false;
    world = pts[vertex];
    return true;
}

std::vector<uint32_t> seamEdges(const ShapePtr& shape) {
    std::vector<uint32_t> out;
    try {
        TopTools_IndexedMapOfShape m;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, m);
        for (TopExp_Explorer fx(shape->shape, TopAbs_FACE); fx.More(); fx.Next())
            for (TopExp_Explorer ex(fx.Current(), TopAbs_EDGE); ex.More(); ex.Next())
                if (BRep_Tool::IsClosed(TopoDS::Edge(ex.Current()), TopoDS::Face(fx.Current()))) {
                    int i = m.FindIndex(ex.Current());
                    if (i > 0) out.push_back((uint32_t)i - 1);
                }
    } catch (const Standard_Failure&) {}
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void edgePolylines(const ShapePtr& shape, uint32_t maxEdges, std::vector<std::vector<glm::dvec3>>& out) {
    out.clear();
    try {
        TopTools_IndexedMapOfShape m;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, m);
        Bnd_Box box;
        BRepBndLib::Add(shape->shape, box);
        double defl = 0.05;
        if (!box.IsVoid()) defl = std::max(0.02, std::sqrt(box.SquareExtent()) * 0.0015);
        int n = std::min(m.Extent(), (int)maxEdges);
        out.resize((size_t)n);
        for (int i = 1; i <= n; ++i) {
            try {
                const TopoDS_Edge& e = TopoDS::Edge(m(i));
                if (BRep_Tool::Degenerated(e)) continue;
                BRepAdaptor_Curve c(e);
                double first = c.FirstParameter(), last = c.LastParameter();
                if (!(last > first)) continue;
                auto& pts = out[(size_t)i - 1];
                if (c.GetType() == GeomAbs_Line) {
                    gp_Pnt a = c.Value(first), b = c.Value(last);
                    pts = {{a.X(), a.Y(), a.Z()}, {b.X(), b.Y(), b.Z()}};
                    continue;
                }
                // The angle limit keeps small circles round when zoomed in on a big design (the
                // size-based deflection alone made a small hole a triangle).
                GCPnts_TangentialDeflection disc(c, 0.1, defl, 2);
                if (disc.NbPoints() >= 2) {
                    int np = disc.NbPoints(), stepN = std::max(1, np / 400);   // cap points per edge
                    for (int k = 1; k <= np; k += stepN) { gp_Pnt p = disc.Value(k); pts.push_back({p.X(), p.Y(), p.Z()}); }
                    if ((np - 1) % stepN != 0) { gp_Pnt p = disc.Value(np); pts.push_back({p.X(), p.Y(), p.Z()}); }
                } else {
                    gp_Pnt a = c.Value(first), b = c.Value((first + last) * 0.5), d = c.Value(last);
                    pts = {{a.X(), a.Y(), a.Z()}, {b.X(), b.Y(), b.Z()}, {d.X(), d.Y(), d.Z()}};
                }
            } catch (...) {
            }
        }
    } catch (...) {
    }
}

bool edgePolyline(const ShapePtr& shape, uint32_t edgeIndex, std::vector<glm::dvec3>& out) {
    try {
        TopTools_IndexedMapOfShape m;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, m);
        if ((int)edgeIndex >= m.Extent()) return false;
        const TopoDS_Edge& e = TopoDS::Edge(m((int)edgeIndex + 1));
        if (BRep_Tool::Degenerated(e)) return false;
        BRepAdaptor_Curve c(e);
        double first = c.FirstParameter(), last = c.LastParameter();
        if (!(last > first)) return false;
        GCPnts_TangentialDeflection disc(c, 0.1, 0.05, 2);
        if (disc.NbPoints() >= 2) {
            out.clear();
            out.reserve((size_t)disc.NbPoints());
            for (int i = 1; i <= disc.NbPoints(); ++i) {
                gp_Pnt p = disc.Value(i);
                out.push_back({p.X(), p.Y(), p.Z()});
            }
            return true;
        }
        gp_Pnt a = c.Value(first), b = c.Value((first + last) * 0.5), d = c.Value(last);
        out = {{a.X(), a.Y(), a.Z()}, {b.X(), b.Y(), b.Z()}, {d.X(), d.Y(), d.Z()}};
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

// Geometric fingerprint of an edge: a point in the middle and the tangent there. Edge indices change
// after every fillet, so the fallback path finds "the same edge" again by geometry.
struct EdgeKey { gp_Pnt mid; gp_Vec tan; double len = 0; };

bool edgeKey(const TopoDS_Edge& e, EdgeKey& k) {
    try {
        if (BRep_Tool::Degenerated(e)) return false;
        BRepAdaptor_Curve c(e);
        double f = c.FirstParameter(), l = c.LastParameter();
        if (!(l > f)) return false;
        gp_Vec t;
        c.D1((f + l) * 0.5, k.mid, t);
        if (t.Magnitude() < 1e-12) return false;
        k.tan = t.Normalized();
        GProp_GProps g;
        BRepGProp::LinearProperties(e, g);
        k.len = g.Mass();
        return true;
    } catch (...) {
        return false;
    }
}

// Index (0 based) of the edge in `map` that best matches the key, or -1.
int findEdge(const TopTools_IndexedMapOfShape& map, const EdgeKey& key) {
    int best = -1;
    double bestD = std::max(1e-3, key.len * 0.02);
    BRepBuilderAPI_MakeVertex mv(key.mid);
    for (int i = 1; i <= map.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(map(i));
        if (BRep_Tool::Degenerated(e)) continue;
        try {
            BRepExtrema_DistShapeShape d(mv.Vertex(), e);
            if (!d.IsDone() || d.NbSolution() == 0) continue;
            double dist = d.Value();
            if (dist >= bestD) continue;
            // Tangent must stay parallel (a fillet can create a new edge through the same point).
            EdgeKey k2;
            if (!edgeKey(e, k2)) continue;
            BRepAdaptor_Curve c(e);
            double prm = 0;
            gp_Pnt p;
            gp_Vec t;
            if (d.SupportTypeShape2(1) == BRepExtrema_IsOnEdge) d.ParOnEdgeS2(1, prm);
            else prm = (c.FirstParameter() + c.LastParameter()) * 0.5;
            c.D1(prm, p, t);
            if (t.Magnitude() > 1e-12 && std::abs(t.Normalized().Dot(key.tan)) < 0.98) continue;
            bestD = dist;
            best = i - 1;
        } catch (...) {
        }
    }
    return best;
}

// One fillet attempt on a private copy; false on any failure (never propagates).
// Edge indices refer to the shape's own edge map; the copy keeps the same traversal order.
// OpenCASCADE sometimes "succeeds" with garbage (a solid with a chunk missing or flipped).
// A round or bevel only touches material near its edges, so the volume change is bounded by the
// edge lengths times size squared. Anything else, or an invalid solid, counts as a failure.
bool plausibleRound(const TopoDS_Shape& before, const TopoDS_Shape& after, const TopTools_IndexedMapOfShape& emap,
                    const std::vector<uint32_t>& edges, double size) {
    if (countSolids(after) != countSolids(before)) return false;
    BRepCheck_Analyzer an(after);
    if (!an.IsValid()) return false;
    GProp_GProps g0, g1;
    BRepGProp::VolumeProperties(before, g0);
    BRepGProp::VolumeProperties(after, g1);
    if (g1.Mass() <= 0) return false;
    double len = 0;
    for (uint32_t ei : edges) {
        if ((int)ei >= emap.Extent()) continue;
        GProp_GProps lg;
        BRepGProp::LinearProperties(emap((int)ei + 1), lg);
        len += lg.Mass();
    }
    double bound = len * size * size * 4.0 + 8.0 * size * size * size * (double)edges.size() + std::abs(g0.Mass()) * 1e-6;
    return std::abs(g1.Mass() - g0.Mass()) <= bound;
}

bool tryFillet(const TopoDS_Shape& in, const std::vector<uint32_t>& edges, double radius, TopoDS_Shape& out, double endRadius = 0) {
    try {
        TopoDS_Shape priv = copyShape(in);
        TopTools_IndexedMapOfShape cmap;
        TopExp::MapShapes(priv, TopAbs_EDGE, cmap);
        BRepFilletAPI_MakeFillet mf(priv);
        double r2 = endRadius > 1e-6 ? endRadius : radius;
        bool vary = std::abs(r2 - radius) > 1e-6;
        for (uint32_t ei : edges) {
            if ((int)ei >= cmap.Extent()) return false;
            TopoDS_Edge e = TopoDS::Edge(cmap((int)ei + 1));
            if (vary) mf.Add(radius, r2, e);
            else mf.Add(radius, e);
        }
        mf.Build();
        if (!mf.IsDone() || mf.Shape().IsNull() || countSolids(mf.Shape()) == 0) return false;
        if (!plausibleRound(priv, mf.Shape(), cmap, edges, std::max(radius, r2))) return false;
        out = mf.Shape();
        return true;
    } catch (...) {
        return false;
    }
}

// Largest radius <= upTo that works on these edges (bisection, about 10 kernel calls).
double searchMaxRadius(const TopoDS_Shape& in, const std::vector<uint32_t>& edges, double upTo) {
    double lo = 0, hi = upTo;
    TopoDS_Shape scratch;
    if (tryFillet(in, edges, hi, scratch)) return hi;
    double r = hi;
    for (int i = 0; i < 10; ++i) {
        r *= 0.5;
        if (r < 1e-4) return 0;
        if (tryFillet(in, edges, r, scratch)) {
            lo = r;
            hi = r * 2;
            break;
        }
    }
    if (lo == 0) return 0;
    for (int i = 0; i < 12 && hi - lo > std::max(0.05, lo * 0.02); ++i) {
        double mid = (lo + hi) * 0.5;
        if (tryFillet(in, edges, mid, scratch)) lo = mid;
        else hi = mid;
    }
    return lo;
}

// Groups edges into chains that touch each other (fillets that meet must be built together).
std::vector<std::vector<uint32_t>> touchingGroups(const TopTools_IndexedMapOfShape& emap, const std::vector<uint32_t>& edges) {
    std::vector<int> parent(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) parent[i] = (int)i;
    std::function<int(int)> root = [&](int i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    for (size_t i = 0; i < edges.size(); ++i)
        for (size_t j = i + 1; j < edges.size(); ++j) {
            TopoDS_Vertex a1, a2, b1, b2;
            TopExp::Vertices(TopoDS::Edge(emap((int)edges[i] + 1)), a1, a2);
            TopExp::Vertices(TopoDS::Edge(emap((int)edges[j] + 1)), b1, b2);
            if (a1.IsSame(b1) || a1.IsSame(b2) || a2.IsSame(b1) || a2.IsSame(b2)) parent[root((int)i)] = root((int)j);
        }
    std::map<int, std::vector<uint32_t>> g;
    for (size_t i = 0; i < edges.size(); ++i) g[root((int)i)].push_back(edges[i]);
    std::vector<std::vector<uint32_t>> out;
    for (auto& [k, v] : g) out.push_back(v);
    return out;
}

} // namespace

ShapePtr filletEdges(const ShapePtr& shape, const std::vector<uint32_t>& edges, double radius, FilletReport& report, double endRadius) {
    if (edges.empty()) throw std::runtime_error("Select at least one edge");
    if (!(radius > 1e-6)) throw std::runtime_error("Radius must be greater than zero");
    if (!(endRadius > 1e-6)) endRadius = radius;
    TopTools_IndexedMapOfShape emap;
    TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
    for (uint32_t ei : edges)
        if ((int)ei >= emap.Extent())
            throw std::runtime_error("The body changed and the stored edges no longer match. Edit the feature and reselect them.");

    TopoDS_Shape result;
    if (tryFillet(shape->shape, edges, radius, result, endRadius)) {
        auto h = std::make_shared<ShapeHandle>();
        h->shape = result;
        return h;
    }

    // The whole set failed. Fall back group by group (edges that touch stay together so corners
    // blend), then edge by edge. Edges are found again by geometry after every step because
    // filleting renumbers the body's edges.
    TopoDS_Shape acc = shape->shape;
    int applied = 0;
    auto keysOf = [&](const std::vector<uint32_t>& ids) {
        std::vector<EdgeKey> ks;
        for (uint32_t ei : ids) {
            EdgeKey k;
            if (edgeKey(TopoDS::Edge(emap((int)ei + 1)), k)) ks.push_back(k);
        }
        return ks;
    };
    auto locate = [&](const std::vector<EdgeKey>& ks, std::vector<uint32_t>& out) {
        TopTools_IndexedMapOfShape cur;
        TopExp::MapShapes(acc, TopAbs_EDGE, cur);
        out.clear();
        for (auto& k : ks) {
            int i = findEdge(cur, k);
            if (i < 0) return false;
            out.push_back((uint32_t)i);
        }
        return true;
    };
    std::vector<std::pair<uint32_t, EdgeKey>> singles;
    for (auto& grp : touchingGroups(emap, edges)) {
        std::vector<EdgeKey> ks = keysOf(grp);
        std::vector<uint32_t> idx;
        TopoDS_Shape next;
        if (grp.size() > 1 && ks.size() == grp.size() && locate(ks, idx) && tryFillet(acc, idx, radius, next, endRadius)) {
            acc = next;
            applied += (int)grp.size();
            continue;
        }
        for (size_t i = 0; i < grp.size(); ++i) {
            EdgeKey k;
            if (edgeKey(TopoDS::Edge(emap((int)grp[i] + 1)), k)) singles.push_back({grp[i], k});
            else report.failed.push_back({grp[i], 0});
        }
    }
    for (auto& [orig, key] : singles) {
        std::vector<uint32_t> idx;
        TopoDS_Shape next;
        if (!locate({key}, idx)) {
            report.failed.push_back({orig, 0});
            continue;
        }
        if (tryFillet(acc, idx, radius, next, endRadius)) {
            acc = next;
            ++applied;
        } else {
            report.failed.push_back({orig, searchMaxRadius(acc, idx, radius)});
        }
    }
    if (applied == 0) {
        std::string why = "No edge could take radius " + fmtRadius(radius) + ":";
        for (auto& f : report.failed)
            why += "\n  Edge " + std::to_string(f.edge) + ": max " + fmtRadius(f.maxRadius);
        throw std::runtime_error(why);
    }
    report.partial = !report.failed.empty();
    auto h = std::make_shared<ShapeHandle>();
    h->shape = acc;
    return h;
}

double maxFilletRadius(const ShapePtr& shape, const std::vector<uint32_t>& edges, double upTo) {
    TopTools_IndexedMapOfShape emap;
    TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
    for (uint32_t ei : edges)
        if ((int)ei >= emap.Extent()) return 0;
    return searchMaxRadius(shape->shape, edges, upTo);
}

ShapePtr fullRoundFace(const ShapePtr& shape, uint32_t face) {
    if (!shape) throw std::runtime_error("That body has no solid.");
    glm::dvec3 fc, fn;
    bool planar = false;
    if (!faceInfo(shape, face, fc, fn, planar) || !planar || glm::length(fn) < 1e-9)
        throw std::runtime_error("Full round needs a flat face.");
    fn = glm::normalize(fn);
    std::vector<uint32_t> edges = faceEdges(shape, face);
    if (edges.size() < 2) throw std::runtime_error("That face is not between two parallel faces.");

    TopTools_IndexedMapOfShape emap, fmap;
    TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
    TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shape->shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    struct Side {
        glm::dvec3 a, b, dir, n;
    };
    std::vector<Side> sides;
    glm::mat4 id(1.f);
    for (uint32_t ei : edges) {
        EdgeGeom g;
        if (!edgeGeom(shape, id, ei, g) || g.type != 0) continue;
        glm::dvec3 d = g.b - g.a;
        if (glm::length(d) < 1e-6 || (int)ei >= emap.Extent()) continue;
        int slot = edgeFaces.FindIndex(emap((int)ei + 1));
        if (slot <= 0) continue;
        int other = -1;
        for (auto it = edgeFaces(slot).cbegin(); it != edgeFaces(slot).cend(); ++it) {
            int fi = fmap.FindIndex(*it);
            if (fi > 0 && (uint32_t)(fi - 1) != face) { other = fi - 1; break; }
        }
        if (other < 0) continue;
        glm::dvec3 c, n;
        bool pl = false;
        if (!faceInfo(shape, (uint32_t)other, c, n, pl) || !pl || glm::length(n) < 1e-9) continue;
        sides.push_back({g.a, g.b, glm::normalize(d), glm::normalize(n)});
    }

    int bestI = -1, bestJ = -1;
    double bestD = 1e300;
    for (size_t i = 0; i < sides.size(); ++i) {
        for (size_t j = i + 1; j < sides.size(); ++j) {
            if (std::abs(std::abs(glm::dot(sides[i].n, sides[j].n)) - 1.0) > 1e-4) continue;
            if (std::abs(std::abs(glm::dot(sides[i].dir, sides[j].dir)) - 1.0) > 1e-4) continue;
            double D = glm::length(glm::cross(sides[j].a - sides[i].a, sides[i].dir));
            if (D < 1e-4 || D >= bestD) continue;
            bestD = D;
            bestI = (int)i;
            bestJ = (int)j;
        }
    }
    if (bestI < 0) throw std::runtime_error("That face is not between two parallel faces.");

    const Side& s0 = sides[bestI];
    const Side& s1 = sides[bestJ];
    double R = bestD * 0.5;
    glm::dvec3 axisDir = s0.dir;
    glm::dvec3 gap = s1.a - s0.a;
    gap -= axisDir * glm::dot(gap, axisDir);
    if (glm::length(gap) < 1e-6) throw std::runtime_error("That face is not between two parallel faces.");
    glm::dvec3 mid = s0.a + gap * 0.5;
    mid -= fn * glm::dot(mid - fc, fn);
    glm::dvec3 axisPoint = mid - fn * R;

    auto along = [&](const glm::dvec3& p) { return glm::dot(p - axisPoint, axisDir); };
    double t0 = 1e300, t1 = -1e300;
    for (const Side* s : {&s0, &s1}) {
        t0 = std::min(t0, std::min(along(s->a), along(s->b)));
        t1 = std::max(t1, std::max(along(s->a), along(s->b)));
    }
    if (t1 - t0 < 1e-4) throw std::runtime_error("That face is not between two parallel faces.");

    glm::dvec3 thick = glm::cross(fn, axisDir);
    if (glm::length(thick) < 1e-6) throw std::runtime_error("That face is not between two parallel faces.");
    thick = glm::normalize(thick);
    double pad = 0.5;
    glm::dvec3 boxOrigin = axisPoint + axisDir * t0 - thick * (R + pad);
    gp_Ax2 boxAx(gp_Pnt(boxOrigin.x, boxOrigin.y, boxOrigin.z),
                 gp_Dir(thick.x, thick.y, thick.z),
                 gp_Dir(fn.x, fn.y, fn.z));
    TopoDS_Shape box = BRepPrimAPI_MakeBox(boxAx, R + 1.0, t1 - t0, 2.0 * (R + pad)).Shape();
    glm::dvec3 cyl0 = axisPoint + axisDir * (t0 - 0.05);
    gp_Ax2 cylAx(gp_Pnt(cyl0.x, cyl0.y, cyl0.z), gp_Dir(axisDir.x, axisDir.y, axisDir.z));
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(cylAx, R, (t1 - t0) + 0.1).Shape();
    try {
        TopoDS_Shape tool = boolOp(false, box, cyl, "Full round");
        TopoDS_Shape cut = boolOp(false, copyShape(shape->shape), tool, "Full round");
        if (countSolids(cut) != 1) throw std::runtime_error("Full round did not leave one solid.");
        double before = solidVolume(shape);
        auto h = std::make_shared<ShapeHandle>();
        h->shape = cut;
        double after = solidVolume(h);
        if (std::abs(after - before) < 1.0) throw std::runtime_error("Full round removed no metal");
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "full round");
    }
}

ShapePtr chamferEdges(const ShapePtr& shape, const std::vector<uint32_t>& edges, double dist, double dist2) {
    if (edges.empty()) throw std::runtime_error("Select at least one edge");
    if (!(dist > 1e-6)) throw std::runtime_error("Distance must be greater than zero");
    double d2 = dist2 > 1e-6 ? dist2 : dist;
    bool two = std::abs(d2 - dist) > 1e-9;
    try {
        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
        for (uint32_t ei : edges)
            if ((int)ei >= emap.Extent())
                throw std::runtime_error("The body changed and the stored edges no longer match. Edit the feature and reselect them.");
        TopoDS_Shape priv = copyShape(shape->shape);
        TopTools_IndexedMapOfShape cmap;
        TopExp::MapShapes(priv, TopAbs_EDGE, cmap);
        auto attempt = [&](const TopoDS_Shape& base, const TopTools_IndexedMapOfShape& bmap,
                           const std::vector<TopoDS_Edge>& es, TopoDS_Shape& out) {
            try {
                BRepFilletAPI_MakeChamfer mc(base);
                TopTools_IndexedDataMapOfShapeListOfShape facesOf;
                if (two) TopExp::MapShapesAndAncestors(base, TopAbs_EDGE, TopAbs_FACE, facesOf);
                for (auto& e : es) {
                    if (!two) { mc.Add(dist, e); continue; }
                    int slot = facesOf.FindIndex(e);
                    if (slot <= 0 || facesOf(slot).IsEmpty()) { mc.Add(dist, e); continue; }
                    mc.Add(dist, d2, e, TopoDS::Face(facesOf(slot).First()));
                }
                mc.Build();
                if (!mc.IsDone() || mc.Shape().IsNull() || countSolids(mc.Shape()) == 0) return false;
                std::vector<uint32_t> idx;
                for (auto& e : es) { int k = bmap.FindIndex(e); if (k > 0) idx.push_back((uint32_t)k - 1); }
                if (!plausibleRound(base, mc.Shape(), bmap, idx, std::max(dist, d2))) return false;
                out = mc.Shape();
                return true;
            } catch (...) {
                return false;
            }
        };
        std::vector<TopoDS_Edge> all;
        for (uint32_t ei : edges) all.push_back(TopoDS::Edge(cmap((int)ei + 1)));
        TopoDS_Shape res;
        if (!attempt(priv, cmap, all, res)) {
            // Edge by edge: keep what works (located again on the changed body by geometry).
            TopoDS_Shape acc = priv;
            int done = 0;
            for (auto& e0 : all) {
                TopTools_IndexedMapOfShape amap;
                TopExp::MapShapes(acc, TopAbs_EDGE, amap);
                int k = amap.FindIndex(e0);
                if (k <= 0) {
                    EdgeKey key;
                    if (!edgeKey(e0, key)) continue;
                    int g = findEdge(amap, key);
                    if (g < 0) continue;
                    k = g + 1;
                }
                TopoDS_Shape step;
                if (attempt(acc, amap, {TopoDS::Edge(amap(k))}, step)) { acc = step; ++done; }
            }
            if (done == 0)
                throw std::runtime_error("Chamfer failed on these edges. Try a smaller distance.");
            res = acc;
        }
        auto h = std::make_shared<ShapeHandle>();
        h->shape = res;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "chamfer");
    }
}

ShapePtr shellBody(const ShapePtr& shape, const std::vector<uint32_t>& removeFaces, double thickness, int side) {
    if (!(thickness > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        for (uint32_t fi : removeFaces)
            if ((int)fi >= fmap.Extent())
                throw std::runtime_error("The body changed and the stored faces no longer match. Edit the feature and reselect them.");
        TopoDS_Shape priv = copyShape(shape->shape);
        // No opening: build the inner solid and cut it out of the original. A result whose box is
        // already the same size as the body is the wall itself and is kept as is.
        if (side < 0 || side > 2) side = 0;
        if (removeFaces.empty() && side != 0) {
            // Outside grows the body, then cuts the original out. Both keeps the wall centered.
            double outD = side == 1 ? thickness : thickness * 0.5;
            double inD = side == 1 ? 0.0 : thickness * 0.5;
            auto grow = [&](double dist) -> TopoDS_Shape {
                for (GeomAbs_JoinType join : {GeomAbs_Intersection, GeomAbs_Arc}) {
                    try {
                        BRepOffset_MakeOffset mo;
                        mo.Initialize(priv, dist, 1e-3, BRepOffset_Skin,
                                      join == GeomAbs_Intersection ? Standard_True : Standard_False,
                                      Standard_False, join, Standard_False, Standard_True);
                        mo.MakeOffsetShape();
                        if (mo.IsDone() && !mo.Shape().IsNull() && countSolids(mo.Shape()) > 0)
                            return mo.Shape();
                    } catch (...) {
                    }
                }
                return {};
            };
            TopoDS_Shape outer = grow(outD);
            TopoDS_Shape inner = side == 1 ? copyShape(shape->shape) : grow(-inD);
            if (outer.IsNull() || inner.IsNull())
                throw std::runtime_error("Shell failed on this body. Try a thinner wall or different faces.");
            TopoDS_Shape hollow = boolOp(false, outer, inner, "Shell");
            if (hollow.IsNull() || countSolids(hollow) == 0)
                throw std::runtime_error("Shell failed on this body. Try a thinner wall or different faces.");
            auto h = std::make_shared<ShapeHandle>();
            h->shape = hollow;
            return h;
        }
        if (removeFaces.empty()) {
            auto bounds = [](const TopoDS_Shape& s, Standard_Real& x0, Standard_Real& y0, Standard_Real& z0,
                             Standard_Real& x1, Standard_Real& y1, Standard_Real& z1) {
                Bnd_Box b;
                BRepBndLib::Add(s, b);
                if (b.IsVoid()) return false;
                b.Get(x0, y0, z0, x1, y1, z1);
                return true;
            };
            auto isInner = [&](const TopoDS_Shape& cand) {
                Standard_Real ax0, ay0, az0, ax1, ay1, az1, bx0, by0, bz0, bx1, by1, bz1;
                if (!bounds(shape->shape, ax0, ay0, az0, ax1, ay1, az1)) return false;
                if (!bounds(cand, bx0, by0, bz0, bx1, by1, bz1)) return false;
                double tol = std::max(thickness * 0.25, 1e-3);
                bool shrunk = (ax1 - bx1) > tol || (bx0 - ax0) > tol || (ay1 - by1) > tol || (by0 - ay0) > tol
                           || (az1 - bz1) > tol || (bz0 - az0) > tol;
                bool outside = bx0 < ax0 - tol || by0 < ay0 - tol || bz0 < az0 - tol
                            || bx1 > ax1 + tol || by1 > ay1 + tol || bz1 > az1 + tol;
                return shrunk && !outside;
            };
            auto take = [&](const TopoDS_Shape& cand) -> ShapePtr {
                if (cand.IsNull() || countSolids(cand) == 0) return nullptr;
                TopoDS_Shape hollow = isInner(cand) ? boolOp(false, copyShape(shape->shape), cand, "Shell") : cand;
                if (hollow.IsNull() || countSolids(hollow) == 0) return nullptr;
                auto h = std::make_shared<ShapeHandle>();
                h->shape = hollow;
                return h;
            };
            auto tryOffset = [&](double dist, GeomAbs_JoinType join, bool inter) -> TopoDS_Shape {
                try {
                    BRepOffset_MakeOffset mo;
                    mo.Initialize(priv, -std::abs(dist), 1e-3, BRepOffset_Skin,
                                  inter ? Standard_True : Standard_False, Standard_False, join, Standard_False, Standard_True);
                    mo.MakeOffsetShape();
                    if (!mo.IsDone() || mo.Shape().IsNull() || countSolids(mo.Shape()) == 0) return {};
                    return mo.Shape();
                } catch (...) {
                    return {};
                }
            };
            auto tryWall = [&](GeomAbs_JoinType join) -> TopoDS_Shape {
                try {
                    BRepOffset_MakeOffset mo;
                    mo.Initialize(priv, -std::abs(thickness), 1e-4, BRepOffset_Skin,
                                  Standard_True, Standard_False, join, Standard_True, Standard_True);
                    mo.MakeThickSolid();
                    if (!mo.IsDone() || mo.Shape().IsNull() || countSolids(mo.Shape()) == 0) return {};
                    return mo.Shape();
                } catch (...) {
                    return {};
                }
            };
            if (ShapePtr h = take(tryOffset(thickness, GeomAbs_Arc, false))) return h;
            if (ShapePtr h = take(tryOffset(thickness, GeomAbs_Intersection, true))) return h;
            if (ShapePtr h = take(tryWall(GeomAbs_Intersection))) return h;
            if (ShapePtr h = take(tryWall(GeomAbs_Arc))) return h;
            // A chamfer or fillet the same size as the wall collapses that face and the exact
            // offset is refused. Ease the wall by 0.05mm and try once more.
            double eased = std::max(thickness - 0.05, thickness * 0.5);
            if (eased < thickness - 1e-6) {
                if (ShapePtr h = take(tryOffset(eased, GeomAbs_Arc, false))) return h;
                if (ShapePtr h = take(tryOffset(eased, GeomAbs_Intersection, true))) return h;
            }
            throw std::runtime_error("Shell failed on this body. Try a thinner wall or different faces.");
        }
        TopTools_IndexedMapOfShape cmap;
        TopExp::MapShapes(priv, TopAbs_FACE, cmap);
        TopTools_ListOfShape toRemove;
        for (uint32_t fi : removeFaces) toRemove.Append(cmap((int)fi + 1));
        if (side == 2)
            throw std::runtime_error("Both sides is for a closed shell. Leave the faces unpicked, or choose Inside or Outside.");
        BRepOffsetAPI_MakeThickSolid mk;
        mk.MakeThickSolidByJoin(priv, toRemove, side == 1 ? std::abs(thickness) : -std::abs(thickness), 1e-3);
        mk.Build();
        if (!mk.IsDone() || mk.Shape().IsNull() || countSolids(mk.Shape()) == 0)
            throw std::runtime_error("Shell failed on this body. Try a thinner wall or different faces.");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = mk.Shape();
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "shell");
    }
}

namespace {

// Outward normal of a planar face (respecting orientation). False if not planar.
bool planarNormal(const TopoDS_Face& f, gp_Dir& n, gp_Pnt& p) {
    BRepAdaptor_Surface s(f);
    if (s.GetType() != GeomAbs_Plane) return false;
    gp_Pln pl = s.Plane();
    n = pl.Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
    p = pl.Location();
    return true;
}

} // namespace

namespace {
// Same round surface: one cylinder, cone, sphere or torus (a hole wall is often two half faces).
bool sameRoundSurface(const TopoDS_Face& a, const TopoDS_Face& b) {
    BRepAdaptor_Surface sa(a, Standard_False), sb(b, Standard_False);
    if (sa.GetType() != sb.GetType()) return false;
    const double tol = 1e-6;
    auto sameAxis = [&](const gp_Ax1& p, const gp_Ax1& q) {
        if (!p.Direction().IsParallel(q.Direction(), 1e-6)) return false;
        gp_Vec d(p.Location(), q.Location());
        return d.Crossed(gp_Vec(p.Direction())).Magnitude() < 1e-5;
    };
    switch (sa.GetType()) {
    case GeomAbs_Cylinder:
        return std::abs(sa.Cylinder().Radius() - sb.Cylinder().Radius()) < tol && sameAxis(sa.Cylinder().Axis(), sb.Cylinder().Axis());
    case GeomAbs_Cone:
        return std::abs(sa.Cone().SemiAngle() - sb.Cone().SemiAngle()) < 1e-9 && sa.Cone().Apex().Distance(sb.Cone().Apex()) < 1e-5 &&
               sameAxis(sa.Cone().Axis(), sb.Cone().Axis());
    case GeomAbs_Sphere:
        return std::abs(sa.Sphere().Radius() - sb.Sphere().Radius()) < tol && sa.Sphere().Location().Distance(sb.Sphere().Location()) < 1e-5;
    case GeomAbs_Torus:
        return std::abs(sa.Torus().MajorRadius() - sb.Torus().MajorRadius()) < tol &&
               std::abs(sa.Torus().MinorRadius() - sb.Torus().MinorRadius()) < tol && sameAxis(sa.Torus().Axis(), sb.Torus().Axis());
    default:
        return false;
    }
}

// The picked faces grown to the whole feature they belong to: the rest of the same round wall, then
// every face the set closes in (each of its edges touches the set), like a blind hole's bottom.
std::vector<int> growToFeature(const TopoDS_Shape& shape, const TopTools_IndexedMapOfShape& fmap, const std::vector<uint32_t>& faces) {
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, e2f);
    std::set<int> in;
    for (uint32_t fi : faces) in.insert((int)fi + 1);
    auto neighbours = [&](int fi, auto&& fn) {
        for (TopExp_Explorer ex(fmap(fi), TopAbs_EDGE); ex.More(); ex.Next()) {
            int ei = e2f.FindIndex(ex.Current());
            if (ei == 0) continue;
            for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
                int nj = fmap.FindIndex(it.Value());
                if (nj > 0 && nj != fi) fn(nj);
            }
        }
    };
    for (bool grew = true; grew;) {
        grew = false;
        std::vector<int> cur(in.begin(), in.end());
        for (int fi : cur)
            neighbours(fi, [&](int nj) {
                if (!in.count(nj) && sameRoundSurface(TopoDS::Face(fmap(fi)), TopoDS::Face(fmap(nj)))) { in.insert(nj); grew = true; }
            });
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (int fj = 1; fj <= fmap.Extent(); ++fj) {
            if (in.count(fj)) continue;
            bool closed = true, touches = false;
            for (TopExp_Explorer ex(fmap(fj), TopAbs_EDGE); ex.More() && closed; ex.Next()) {
                const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
                if (BRep_Tool::Degenerated(e)) continue;
                int ei = e2f.FindIndex(e);
                bool shared = false, self = true;
                if (ei)
                    for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
                        int nj = fmap.FindIndex(it.Value());
                        if (nj != fj) self = false;
                        if (in.count(nj)) shared = true;
                    }
                if (self) continue;   // a seam: the face meets only itself there
                if (shared) touches = true;
                else closed = false;
            }
            if (closed && touches) { in.insert(fj); grew = true; }
        }
    }
    std::vector<int> out;
    for (int fi : in) out.push_back(fi);
    return out;
}

// A flat face that cuts across a round face (a flat strip on a bore or on a shaft, left where a
// rib or a box met the cylinder): deleting it means the round face simply continues. The piece of
// the cylinder on the far side of the flat (from the axis) is added (a flat on a shaft) or taken
// away (a rib in a bore), between the flat's ends along the axis, and the round pieces merge into
// one smooth face again. Null when the face is not that case.
TopoDS_Shape fillFlatOnCylinder(const TopoDS_Shape& shape, const TopTools_IndexedMapOfShape& fmap, int fi) {
    const TopoDS_Face& f = TopoDS::Face(fmap(fi));
    gp_Dir n;
    gp_Pnt p0;
    if (!planarNormal(f, n, p0)) return {};
    // The round neighbours: all on one cylinder.
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, e2f);
    bool have = false;
    gp_Cylinder cyl;
    for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next()) {
        int k = e2f.FindIndex(ex.Current());
        if (k <= 0) continue;
        for (TopTools_ListIteratorOfListOfShape it(e2f(k)); it.More(); it.Next()) {
            if (it.Value().IsSame(f)) continue;
            BRepAdaptor_Surface s(TopoDS::Face(it.Value()));
            if (s.GetType() != GeomAbs_Cylinder) continue;
            gp_Cylinder c = s.Cylinder();
            if (!have) { cyl = c; have = true; continue; }
            if (std::abs(c.Radius() - cyl.Radius()) > 1e-6 || !c.Axis().Direction().IsParallel(cyl.Axis().Direction(), 1e-7) ||
                gp_Lin(cyl.Axis()).Distance(c.Location()) > 1e-6)
                return {};
        }
    }
    if (!have) return {};
    gp_Dir ax = cyl.Axis().Direction();
    gp_Lin axis(cyl.Axis());
    double R = cyl.Radius();
    if (std::abs(n.Dot(ax)) > 1e-7) return {};          // the flat must run along the axis
    double dPlane = std::abs(gp_Vec(cyl.Location(), p0).Dot(gp_Vec(n)));
    if (dPlane >= R - 1e-7) return {};                   // it does not cut into the cylinder
    // Along the axis: the flat's own span.
    double t0 = 1e300, t1 = -1e300;
    for (TopExp_Explorer vx(f, TopAbs_VERTEX); vx.More(); vx.Next()) {
        gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
        double t = gp_Vec(cyl.Location(), q).Dot(gp_Vec(ax));
        t0 = std::min(t0, t);
        t1 = std::max(t1, t);
    }
    if (!(t1 - t0 > 1e-6)) return {};
    gp_Pnt base = cyl.Location().Translated(gp_Vec(ax) * t0);
    TopoDS_Shape tube = BRepPrimAPI_MakeCylinder(gp_Ax2(base, ax), R, t1 - t0).Shape();
    // The side of the flat's plane away from the axis.
    gp_Pnt c0 = axis.Location().Translated(gp_Vec(ax) * ((t0 + t1) * 0.5));
    gp_Vec toAxis(p0, c0);
    gp_Dir away = toAxis.Dot(gp_Vec(n)) > 0 ? n.Reversed() : n;
    gp_Pln pl(p0, away);
    TopoDS_Face big = BRepBuilderAPI_MakeFace(pl, -1e5, 1e5, -1e5, 1e5).Face();
    gp_Pnt ref = p0.Translated(gp_Vec(away) * (R + 1.0));
    TopoDS_Shape half = BRepPrimAPI_MakeHalfSpace(big, ref).Solid();
    BRepAlgoAPI_Common cm(tube, half);
    cm.Build();
    if (!cm.IsDone() || cm.Shape().IsNull()) return {};
    TopoDS_Shape wedge = cm.Shape();
    // Material in that sliver now (a rib in a bore: take it away) or not (a flat on a shaft: fill it).
    GProp_GProps g;
    BRepGProp::VolumeProperties(wedge, g);
    gp_Pnt mid = g.CentreOfMass();
    BRepClass3d_SolidClassifier cls(shape, mid, 1e-7);
    bool solidThere = cls.State() == TopAbs_IN;
    TopoDS_Shape res;
    if (solidThere) {
        BRepAlgoAPI_Cut op(shape, wedge);
        op.Build();
        if (!op.IsDone()) return {};
        res = op.Shape();
    } else {
        BRepAlgoAPI_Fuse op(shape, wedge);
        op.Build();
        if (!op.IsDone()) return {};
        res = op.Shape();
    }
    if (res.IsNull() || countSolids(res) == 0) return {};
    res = unify(res);
    // The flat must be gone (its plane no longer carries a face there).
    TopTools_IndexedMapOfShape rm;
    TopExp::MapShapes(res, TopAbs_FACE, rm);
    for (int j = 1; j <= rm.Extent(); ++j) {
        gp_Dir n2;
        gp_Pnt p2;
        if (!planarNormal(TopoDS::Face(rm(j)), n2, p2) || !n2.IsParallel(n, 1e-7)) continue;
        if (std::abs(gp_Vec(p0, p2).Dot(gp_Vec(n))) < 1e-6) {
            GProp_GProps fg;
            BRepGProp::SurfaceProperties(rm(j), fg);
            if (fg.Mass() > 1e-6) return {};
        }
    }
    return res;
}

// The solid with those faces gone and the gap healed, or a null shape.
TopoDS_Shape tryDefeature(const TopoDS_Shape& shape, const TopTools_IndexedMapOfShape& fmap, const std::vector<int>& faces) {
    BRepAlgoAPI_Defeaturing df;
    df.SetShape(shape);
    for (int fi : faces) df.AddFaceToRemove(fmap(fi));
    df.SetRunParallel(false);
    df.SetToFillHistory(false);
    df.Build();
    if (!df.IsDone() || df.HasErrors() || df.Shape().IsNull() || countSolids(df.Shape()) == 0) return {};
    // A removed face that is still there (same place, same size) means nothing was healed: the
    // tool can hand back the part only re-split, with fewer pieces but the same surfaces.
    {
        TopTools_IndexedMapOfShape after;
        TopExp::MapShapes(df.Shape(), TopAbs_FACE, after);
        for (int fi : faces) {
            GProp_GProps g;
            BRepGProp::SurfaceProperties(fmap(fi), g);
            bool round = BRepAdaptor_Surface(TopoDS::Face(fmap(fi))).GetType() != GeomAbs_Plane;
            for (int j = 1; j <= after.Extent(); ++j) {
                GProp_GProps h;
                BRepGProp::SurfaceProperties(after(j), h);
                if (h.CentreOfMass().Distance(g.CentreOfMass()) < 1e-5 && std::abs(h.Mass() - g.Mass()) < 1e-6 * std::max(1.0, g.Mass())) return {};
                // A round face whose surface is still there (its other half merged back in) is not gone either.
                if (round && sameRoundSurface(TopoDS::Face(fmap(fi)), TopoDS::Face(after(j)))) return {};
            }
        }
    }
    // Nothing actually removed means the gap could not be healed.
    TopTools_IndexedMapOfShape rmap;
    TopExp::MapShapes(df.Shape(), TopAbs_FACE, rmap);
    if (rmap.Extent() >= fmap.Extent()) return {};
    return df.Shape();
}
} // namespace

namespace {
// A flat face whose neighbours are all flat (a chamfer, a bevel, a sloped cut): the gap it leaves is
// the space inside every neighbour's plane and past the face, so that wedge is built from half
// spaces and joined on. This closes the cases where growing the neighbours fails.
TopoDS_Shape fillFlatFace(const TopoDS_Shape& body, const TopTools_IndexedMapOfShape& fmap, int fi, std::string* why = nullptr) {
    gp_Dir nF;
    gp_Pnt pF;
    const TopoDS_Face& F = TopoDS::Face(fmap(fi));
    if (!planarNormal(F, nF, pF)) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, e2f);
    std::vector<std::pair<gp_Dir, gp_Pnt>> planes;
    std::set<int> seen;
    for (TopExp_Explorer ex(F, TopAbs_EDGE); ex.More(); ex.Next()) {
        int ei = e2f.FindIndex(ex.Current());
        if (!ei) continue;
        for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
            int nj = fmap.FindIndex(it.Value());
            if (nj <= 0 || nj == fi || seen.count(nj)) continue;
            seen.insert(nj);
            gp_Dir n;
            gp_Pnt p;
            if (!planarNormal(TopoDS::Face(fmap(nj)), n, p)) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
            if (n.IsParallel(nF, 1e-6)) continue;   // a neighbour in the same plane adds no bound
            planes.push_back({n, p});
        }
    }
    if (planes.size() < 2) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    Bnd_Box bb;
    BRepBndLib::Add(body, bb);
    if (bb.IsVoid()) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    double pad = std::sqrt(bb.SquareExtent()) * 0.5 + 1.0;
    TopoDS_Shape wedge = BRepPrimAPI_MakeBox(gp_Pnt(x0 - pad, y0 - pad, z0 - pad), gp_Pnt(x1 + pad, y1 + pad, z1 + pad)).Shape();
    auto keepSide = [&](const gp_Dir& n, const gp_Pnt& p, bool alongNormal) -> bool {
        TopoDS_Face pf = BRepBuilderAPI_MakeFace(gp_Pln(p, n)).Face();
        gp_Pnt ref = p.Translated(gp_Vec(n) * (alongNormal ? 1.0 : -1.0));
        TopoDS_Shape hs = BRepPrimAPI_MakeHalfSpace(pf, ref).Solid();
        BRepAlgoAPI_Common c(wedge, hs);
        if (!c.IsDone() || c.HasErrors() || c.Shape().IsNull()) return false;
        wedge = c.Shape();
        return true;
    };
    if (!keepSide(nF, pF, true)) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    // Each neighbour bounds the gap on the side where the face itself lies: inside the part for an
    // outside corner, outside it for an inside corner (a groove wall), so both kinds close.
    GProp_GProps fg;
    BRepGProp::SurfaceProperties(F, fg);
    gp_Pnt fc = fg.CentreOfMass();
    for (auto& [n, p] : planes)
        if (!keepSide(n, p, gp_Vec(p, fc).Dot(gp_Vec(n)) > 0)) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    auto vol = [](const TopoDS_Shape& x) { GProp_GProps g; BRepGProp::VolumeProperties(x, g); return std::abs(g.Mass()); };
    double vBody = vol(body), vWedge = vol(wedge);
    if (std::getenv("KESTRAL_FACE_DEBUG")) {
        std::fprintf(stderr, "fill: face %d, %zu planes, wedge %.3f, body %.3f\n", fi, planes.size(), vWedge, vBody);
        for (auto& [n, p] : planes) std::fprintf(stderr, "  plane n %.3f %.3f %.3f p %.2f %.2f %.2f\n", n.X(), n.Y(), n.Z(), p.X(), p.Y(), p.Z());
        std::fprintf(stderr, "  face n %.3f %.3f %.3f\n", nF.X(), nF.Y(), nF.Z());
    }
    // The neighbours must close the gap themselves: a fill that reaches the helper box is unbounded.
    {
        Bnd_Box wb;
        BRepBndLib::Add(wedge, wb);
        double tol = pad * 0.98;
        double w0, w1, w2, w3, w4, w5;
        if (wb.IsVoid()) return {};
        wb.Get(w0, w1, w2, w3, w4, w5);
        if (w0 < x0 - tol || w1 < y0 - tol || w2 < z0 - tol || w3 > x1 + tol || w4 > y1 + tol || w5 > z1 + tol) {
            if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: wedge leaves the part\n");
            if (why) *why = "That face is an outer side of the part: the faces around it never meet beyond it (they run side by side), so nothing can grow to close the gap. To move or remove that side, use Press Pull (Q) on it, or Extrude it as a Cut.";
            return {};
        }
    }
    if (!(vWedge > 1e-6) || vWedge > vBody * 0.5) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    BRepAlgoAPI_Fuse fu(body, wedge);
    if (!fu.IsDone() || fu.HasErrors()) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    TopoDS_Shape r = fu.Shape();
    if (countSolids(r) != 1 || !BRepCheck_Analyzer(r).IsValid()) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }
    TopTools_IndexedMapOfShape rmap;
    TopExp::MapShapes(r, TopAbs_FACE, rmap);
    TopoDS_Shape u = r;
    try { u = unify(r); } catch (...) {}
    TopTools_IndexedMapOfShape umap;
    TopExp::MapShapes(u, TopAbs_FACE, umap);
    if (umap.Extent() >= fmap.Extent() + 2) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "fill: gave up at line %d\n", __LINE__); return {}; }   // it made a mess, not a clean fill
    return u;
}
} // namespace

namespace {
// A curved face with flat neighbours that run side by side (the round end of a spoke or fin cut
// from a cylinder): nothing can grow to close it, so it is replaced by a flat face square to the
// face's middle, at its outermost point, so the part keeps its length: a concave end is filled out
// to its rim, a convex end gets its sides carried out to its tip.
TopoDS_Shape flattenCurvedFace(const TopoDS_Shape& body, const TopTools_IndexedMapOfShape& fmap, int fi) {
    const TopoDS_Face& F = TopoDS::Face(fmap(fi));
    BRepAdaptor_Surface s(F);
    if (s.GetType() == GeomAbs_Plane) return {};
    double u0, u1, v0, v1;
    BRepTools::UVBounds(F, u0, u1, v0, v1);
    BRepLProp_SLProps pr(s, (u0 + u1) * 0.5, (v0 + v1) * 0.5, 1, 1e-6);
    if (!pr.IsNormalDefined()) return {};
    gp_Dir n = pr.Normal();
    if (F.Orientation() == TopAbs_REVERSED) n.Reverse();
    // The plane through the face's corners.
    gp_XYZ sum(0, 0, 0);
    int nv = 0;
    for (TopExp_Explorer vx(F, TopAbs_VERTEX); vx.More(); vx.Next()) {
        sum += BRep_Tool::Pnt(TopoDS::Vertex(vx.Current())).XYZ();
        ++nv;
    }
    if (nv < 2) return {};
    gp_Pnt P0(sum / nv);
    // How far the face reaches in front of and behind that plane.
    double dIn = 0, dOut = 0;
    for (int i = 0; i <= 12; ++i)
        for (int j = 0; j <= 12; ++j) {
            gp_Pnt q = s.Value(u0 + (u1 - u0) * i / 12.0, v0 + (v1 - v0) * j / 12.0);
            double d = gp_Vec(P0, q).Dot(gp_Vec(n));
            dOut = std::max(dOut, d);
            dIn = std::max(dIn, -d);
        }
    // Only real dips and bulges: a hair's width (a corner that just touches the plane) is left
    // alone, cutting a slab that thin only leaves slivers behind.
    const double hair = 1e-5;
    if (dIn < hair && dOut < hair) return {};
    GProp_GProps fg;
    BRepGProp::SurfaceProperties(F, fg);
    gp_Pnt fc = fg.CentreOfMass();
    // The neighbours: all flat, each bounding the side the face is on.
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, e2f);
    std::vector<std::pair<gp_Dir, gp_Pnt>> planes;
    std::set<int> seen;
    for (TopExp_Explorer ex(F, TopAbs_EDGE); ex.More(); ex.Next()) {
        int ei = e2f.FindIndex(ex.Current());
        if (!ei) continue;
        for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
            int nj = fmap.FindIndex(it.Value());
            if (nj <= 0 || nj == fi || seen.count(nj)) continue;
            seen.insert(nj);
            gp_Dir nn;
            gp_Pnt pp;
            if (!planarNormal(TopoDS::Face(fmap(nj)), nn, pp)) return {};
            planes.push_back({nn, pp});
        }
    }
    if (planes.size() < 2) return {};
    Bnd_Box bb;
    BRepBndLib::Add(body, bb);
    if (bb.IsVoid()) return {};
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    double pad = std::sqrt(bb.SquareExtent()) * 0.5 + 1.0;
    auto keep = [&](TopoDS_Shape& sh, const gp_Dir& d, const gp_Pnt& p, bool along) {
        TopoDS_Face pf = BRepBuilderAPI_MakeFace(gp_Pln(p, d)).Face();
        TopoDS_Shape hs = BRepPrimAPI_MakeHalfSpace(pf, p.Translated(gp_Vec(d) * (along ? 1.0 : -1.0))).Solid();
        BRepAlgoAPI_Common c(sh, hs);
        if (!c.IsDone() || c.HasErrors() || c.Shape().IsNull()) return false;
        sh = c.Shape();
        return true;
    };
    TopoDS_Shape prism = BRepPrimAPI_MakeBox(gp_Pnt(x0 - pad, y0 - pad, z0 - pad), gp_Pnt(x1 + pad, y1 + pad, z1 + pad)).Shape();
    for (auto& [pn, pp] : planes)
        if (!keep(prism, pn, pp, gp_Vec(pp, fc).Dot(gp_Vec(pn)) > 0)) return {};
    auto bounded = [&](const TopoDS_Shape& sh) {
        Bnd_Box wb;
        BRepBndLib::Add(sh, wb);
        if (wb.IsVoid()) return true;
        double w0, w1, w2, w3, w4, w5, tol = pad * 0.98;
        wb.Get(w0, w1, w2, w3, w4, w5);
        return !(w0 < x0 - tol || w1 < y0 - tol || w2 < z0 - tol || w3 > x1 + tol || w4 > y1 + tol || w5 > z1 + tol);
    };
    // The fill reaches well past the bottom of the dip into the solid, so it overlaps the part by
    // a clear margin (a hair-thin overlap leaves sliver faces that will not merge), but never as
    // far as the part's other end.
    double depth = 0;
    for (TopExp_Explorer vx(body, TopAbs_VERTEX); vx.More(); vx.Next())
        depth = std::max(depth, -gp_Vec(P0, BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()))).Dot(gp_Vec(n)));
    const double over = std::max(1e-4, std::min(0.5, 0.3 * (depth - dIn)));
    TopoDS_Shape res = body;
    {
        // The end comes out flat at its outermost point, so the part keeps its full length: a dip
        // (concave end) is filled out to its rim, a bulge (convex end) gets its sides carried out to
        // its tip. The end is cut off square a little behind the curve, then that new flat end is
        // pushed out to the plane like Press Pull (its sides slide along). The sides grow as the
        // same faces, so nothing is left to merge (a fill joined on left seams on some parts).
        gp_Pnt Q = P0.Translated(gp_Vec(n) * -(dIn + over));
        TopoDS_Shape rem = prism;
        if (!keep(rem, n, Q, true) || !keep(rem, n, P0.Translated(gp_Vec(n) * (dOut + 0.5)), false) || !bounded(rem)) return {};
        BRepAlgoAPI_Cut ct(res, rem);
        if (!ct.IsDone() || ct.HasErrors() || ct.Shape().IsNull() || countSolids(ct.Shape()) != 1) return {};
        TopoDS_Shape cut = unify(ct.Shape());
        TopTools_IndexedMapOfShape cm;
        TopExp::MapShapes(cut, TopAbs_FACE, cm);
        int cap = -1;
        for (int j = 1; j <= cm.Extent() && cap < 0; ++j) {
            gp_Dir cn;
            gp_Pnt cp;
            if (!planarNormal(TopoDS::Face(cm(j)), cn, cp) || cn.Dot(n) < 1 - 1e-7) continue;
            if (std::abs(gp_Vec(Q, cp).Dot(gp_Vec(n))) < 1e-6) cap = j - 1;
        }
        if (cap < 0) return {};
        auto ch = std::make_shared<ShapeHandle>();
        ch->shape = cut;
        ShapePtr pushed;
        try { pushed = offsetFaces(ch, {(uint32_t)cap}, dIn + over + dOut); } catch (...) { return {}; }
        if (!pushed || pushed->shape.IsNull()) return {};
        res = pushed->shape;
    }
    if (res.IsNull() || countSolids(res) != 1 || !BRepCheck_Analyzer(res).IsValid()) return {};
    try { res = unify(res); } catch (...) {}
    // Flat pieces left side by side (the fill's sides on the part's sides): merged with a small
    // tolerance, as after a combine of parts that sit a hair apart.
    try {
        TopoDS_Shape work = BRepBuilderAPI_Copy(res, Standard_True, Standard_False).Shape();
        ShapeUpgrade_UnifySameDomain u(work, Standard_True, Standard_True, Standard_True);
        u.SetLinearTolerance(1e-4);
        u.SetAngularTolerance(1e-6);
        u.Build();
        TopoDS_Shape r = u.Shape();
        auto nfaces = [](const TopoDS_Shape& x) { TopTools_IndexedMapOfShape m; TopExp::MapShapes(x, TopAbs_FACE, m); return m.Extent(); };
        if (!r.IsNull() && countSolids(r) == 1 && nfaces(r) < nfaces(res) && BRepCheck_Analyzer(r).IsValid()) res = r;
        char msg[160];
        std::snprintf(msg, sizeof msg, "Delete Face: curved face made flat (dip %.4g, bulge %.4g mm), %d faces", dIn, dOut, nfaces(res));
        log::info(msg);
    } catch (...) {}
    return res;
}
} // namespace

void setCopyBeforeUnify(bool on) { g_copyBeforeUnify.store(on); }

ShapePtr mergeFlushFaces(const ShapePtr& shape) {
    // Parts placed with float matrices sit up to ~1e-5 mm off, so faces that are really flush
    // (one part inside another) stayed two faces after a combine. Merged here with a small tolerance.
    try {
        TopoDS_Shape s = shape->shape;
        TopoDS_Shape work = BRepBuilderAPI_Copy(s, Standard_True, Standard_False).Shape();
        ShapeUpgrade_UnifySameDomain u(work, Standard_True, Standard_True, Standard_True);
        u.SetLinearTolerance(1e-4);
        u.SetAngularTolerance(1e-6);
        u.Build();
        TopoDS_Shape r = u.Shape();
        if (r.IsNull() || countSolids(r) != countSolids(s) || !BRepCheck_Analyzer(r).IsValid()) return shape;
        auto h = std::make_shared<ShapeHandle>();
        h->shape = r;
        return h;
    } catch (...) {
        return shape;
    }
}

namespace {
// Faces that only split one surface (left where spokes or an extension met a wall, often a hair
// apart in radius so the usual merge passed them by): deleting one means the surface is whole
// again. Every piece of that surface around the picked faces merges into one face; nothing else
// in the part changes. Null when the picked faces have no such neighbours.
TopoDS_Shape dissolveImprints(const TopoDS_Shape& shape, const TopTools_IndexedMapOfShape& fmap, const std::vector<int>& picked) {
    auto same = [](const TopoDS_Face& a, const TopoDS_Face& b) {
        gp_Dir na, nb;
        gp_Pnt pa, pb;
        bool fa = planarNormal(a, na, pa), fb = planarNormal(b, nb, pb);
        if (fa != fb) return false;
        if (fa) return na.IsParallel(nb, 1e-7) && std::abs(gp_Vec(pa, pb).Dot(gp_Vec(na))) < 1e-5;
        return sameRoundSurface(a, b);
    };
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, e2f);
    std::set<int> fam(picked.begin(), picked.end());
    std::vector<int> todo(picked.begin(), picked.end());
    bool any = false;
    while (!todo.empty()) {
        int fi = todo.back();
        todo.pop_back();
        for (TopExp_Explorer ex(fmap(fi), TopAbs_EDGE); ex.More(); ex.Next()) {
            int ei = e2f.FindIndex(ex.Current());
            if (!ei) continue;
            for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
                int nj = fmap.FindIndex(it.Value());
                if (nj <= 0 || nj == fi || fam.count(nj)) continue;
                if (!same(TopoDS::Face(fmap(fi)), TopoDS::Face(fmap(nj)))) continue;
                fam.insert(nj);
                todo.push_back(nj);
                any = true;
            }
        }
    }
    log::info("Delete Face: " + std::to_string(fam.size()) + " piece(s) of the picked surface found");
    if (!any) return {};
    // A hole or shaft wall that is just two halves (split along its length) is one feature, not an
    // imprint: deleting it removes the hole, which the later stages do.
    if (fam.size() <= 2 && BRepAdaptor_Surface(TopoDS::Face(fmap(*fam.begin())), Standard_False).GetType() != GeomAbs_Plane) {
        bool onlyLengthwise = true;
        for (int fi : fam)
            for (TopExp_Explorer ex(fmap(fi), TopAbs_EDGE); ex.More() && onlyLengthwise; ex.Next()) {
                int ei = e2f.FindIndex(ex.Current());
                if (!ei) continue;
                bool shared = false;
                for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
                    int nj = fmap.FindIndex(it.Value());
                    if (nj != fi && fam.count(nj)) shared = true;
                }
                if (shared && BRepAdaptor_Curve(TopoDS::Edge(ex.Current())).GetType() != GeomAbs_Line) onlyLengthwise = false;
            }
        if (onlyLengthwise) { log::info("Delete Face: a two-half hole wall, not an imprint"); return {}; }
    }
    // Every picked face must sit on that shared surface itself (not just touch it).
    for (int fi : picked) {
        bool has = false;
        for (TopExp_Explorer ex(fmap(fi), TopAbs_EDGE); ex.More() && !has; ex.Next()) {
            int ei = e2f.FindIndex(ex.Current());
            if (!ei) continue;
            for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
                int nj = fmap.FindIndex(it.Value());
                if (nj > 0 && nj != fi && fam.count(nj)) { has = true; break; }
            }
        }
        if (!has) { log::info("Delete Face: a picked face is not on the shared surface"); return {}; }
    }
    TopoDS_Shape work = BRepBuilderAPI_Copy(shape, Standard_True, Standard_False).Shape();
    TopTools_IndexedMapOfShape wf;
    TopExp::MapShapes(work, TopAbs_FACE, wf);
    TopTools_IndexedDataMapOfShapeListOfShape we2f;
    TopExp::MapShapesAndAncestors(work, TopAbs_EDGE, TopAbs_FACE, we2f);
    // The copy keeps the face order: only edges between two pieces of the family may go.
    TopTools_MapOfShape keep;
    for (int ei = 1; ei <= we2f.Extent(); ++ei) {
        bool inner = true;
        int n = 0;
        for (TopTools_ListIteratorOfListOfShape it(we2f(ei)); it.More(); it.Next()) {
            ++n;
            if (!fam.count(wf.FindIndex(it.Value()))) inner = false;
        }
        if (!inner || n == 0) keep.Add(we2f.FindKey(ei));
    }
    ShapeUpgrade_UnifySameDomain u(work, Standard_True, Standard_True, Standard_True);
    u.KeepShapes(keep);
    u.SetLinearTolerance(1e-5);
    u.SetAngularTolerance(1e-6);
    u.Build();
    TopoDS_Shape r = u.Shape();
    auto faceCount = [](const TopoDS_Shape& x) { TopTools_IndexedMapOfShape m; TopExp::MapShapes(x, TopAbs_FACE, m); return m.Extent(); };
    if (!r.IsNull() && countSolids(r) == countSolids(shape) && BRepCheck_Analyzer(r).IsValid() && faceCount(r) < fmap.Extent()) return r;
    log::info("Delete Face: merging the pieces did not take (" + std::to_string(r.IsNull() ? -1 : faceCount(r)) + " faces); rebuilding the wall");

    // The merge can refuse pieces of one round wall that were made with opposite axis directions
    // (some built upward, some downward). Then the wall is made new: the empty side of it is filled
    // over the pieces' length and taken out again, which leaves one fresh face. Only when that side
    // really is empty (a bore with nothing inside it, a shaft with nothing around it), so nothing
    // else in the part is touched; holes through the wall stay, they are on the solid side.
    BRepAdaptor_Surface s0(TopoDS::Face(fmap(*fam.begin())), Standard_False);
    if (s0.GetType() != GeomAbs_Cylinder) { log::info("Delete Face: not a cylinder wall"); return {}; }
    gp_Cylinder cyl = s0.Cylinder();
    gp_Ax1 ax = cyl.Axis();
    double R = cyl.Radius();
    double t0 = 1e300, t1 = -1e300;
    for (int fi : fam)
        for (TopExp_Explorer vx(fmap(fi), TopAbs_VERTEX); vx.More(); vx.Next()) {
            double t = gp_Vec(ax.Location(), BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()))).Dot(gp_Vec(ax.Direction()));
            t0 = std::min(t0, t);
            t1 = std::max(t1, t);
        }
    if (!(t1 - t0 > 1e-6)) return {};
    // Which side is empty: the way the wall faces (its outward normal) at a piece's middle.
    const TopoDS_Face& F0 = TopoDS::Face(fmap(*fam.begin()));
    double u0, u1, v0, v1;
    BRepTools::UVBounds(F0, u0, u1, v0, v1);
    BRepLProp_SLProps pr(s0, (u0 + u1) * 0.5, (v0 + v1) * 0.5, 1, 1e-6);
    if (!pr.IsNormalDefined()) return {};
    gp_Dir nrm = pr.Normal();
    if (F0.Orientation() == TopAbs_REVERSED) nrm.Reverse();
    gp_Pnt pm = pr.Value();
    gp_Lin axis(ax);
    gp_Pnt foot = ElCLib::Value(ElCLib::Parameter(axis, pm), axis);
    bool bore = gp_Vec(pm, foot).Dot(gp_Vec(nrm)) > 0;   // faces the axis: the inside is empty
    // The pieces can differ in radius by a hair (17.0000004 beside 16.9999998). The new wall goes
    // to the outermost of a bore (the innermost of a shaft), so no skin a hair thick is left behind.
    for (int fi : fam) {
        BRepAdaptor_Surface sf(TopoDS::Face(fmap(fi)), Standard_False);
        if (sf.GetType() != GeomAbs_Cylinder) continue;
        R = bore ? std::max(R, sf.Cylinder().Radius()) : std::min(R, sf.Cylinder().Radius());
    }
    Bnd_Box bb;
    BRepBndLib::Add(shape, bb);
    double big = std::sqrt(bb.SquareExtent()) + R + 1.0;
    gp_Pnt base = ax.Location().Translated(gp_Vec(ax.Direction()) * t0);
    gp_Ax2 a2(base, ax.Direction());
    auto tube = [&](double ri, double ro, double z0, double z1) -> TopoDS_Shape {
        gp_Ax2 b2(base.Translated(gp_Vec(ax.Direction()) * z0), ax.Direction());
        TopoDS_Shape outer = BRepPrimAPI_MakeCylinder(b2, ro, z1 - z0).Shape();
        if (ri <= 0) return outer;
        return BRepAlgoAPI_Cut(outer, BRepPrimAPI_MakeCylinder(b2, ri, z1 - z0).Shape()).Shape();
    };
    double len = t1 - t0, e = 1e-3;
    TopoDS_Shape V = bore ? tube(0, R, 0, len) : tube(R, R + big, 0, len);
    TopoDS_Shape probe = bore ? tube(0, R - e, e, len - e) : tube(R + e, R + big, e, len - e);
    {
        BRepAlgoAPI_Common c(shape, probe);
        if (!c.IsDone() || c.HasErrors()) return {};
        GProp_GProps g;
        BRepGProp::VolumeProperties(c.Shape(), g);
        if (std::abs(g.Mass()) > 1e-6) {   // something sits on the empty side
            log::info("Delete Face: the " + std::string(bore ? "inside" : "outside") + " of that wall is not empty (" + std::to_string(g.Mass()) + " mm3)");
            return {};
        }
    }
    // Fuzzy: the pieces sit up to a hair off the new wall, which plain booleans read as a gap.
    auto fuzzy = [](auto& op, const TopoDS_Shape& a, const TopoDS_Shape& b) {
        TopTools_ListOfShape args, tools;
        args.Append(a);
        tools.Append(b);
        op.SetArguments(args);
        op.SetTools(tools);
        op.SetFuzzyValue(1e-5);
        op.SetRunParallel(false);
        op.Build();
        return op.IsDone() && !op.HasErrors() && !op.Shape().IsNull();
    };
    BRepAlgoAPI_Fuse fu;
    if (!fuzzy(fu, shape, V)) { log::info("Delete Face: filling the wall's empty side failed"); return {}; }
    BRepAlgoAPI_Cut ct;
    if (!fuzzy(ct, fu.Shape(), V)) { log::info("Delete Face: cutting the new wall failed"); return {}; }
    TopoDS_Shape out = ct.Shape();
    if (!out.IsNull() && countSolids(out) > countSolids(shape)) {
        // Specks a hair thick (where pieces sat a hair apart) are dropped; real pieces are not.
        BRep_Builder bb2;
        TopoDS_Compound kept;
        bb2.MakeCompound(kept);
        int n = 0;
        for (TopExp_Explorer sx(out, TopAbs_SOLID); sx.More(); sx.Next()) {
            GProp_GProps g;
            BRepGProp::VolumeProperties(sx.Current(), g);
            if (std::abs(g.Mass()) < 1e-6) continue;
            bb2.Add(kept, sx.Current());
            ++n;
        }
        if (n == 1) {
            TopExp_Explorer one(kept, TopAbs_SOLID);
            out = one.Current();
        }
    }
    if (out.IsNull() || countSolids(out) != countSolids(shape)) {
        std::string vols;
        if (!out.IsNull())
            for (TopExp_Explorer sx(out, TopAbs_SOLID); sx.More(); sx.Next()) {
                GProp_GProps g;
                BRepGProp::VolumeProperties(sx.Current(), g);
                vols += " " + std::to_string(g.Mass());
            }
        log::info("Delete Face: rebuilding the wall split the part (" + std::to_string(out.IsNull() ? 0 : countSolids(out)) + " solids:" + vols +
                  "; the part had " + std::to_string(countSolids(shape)) + ", radius " + std::to_string(R) + ")");
        return {};
    }
    try { out = unify(out); } catch (...) {}
    GProp_GProps ga, gb;
    BRepGProp::VolumeProperties(shape, ga);
    BRepGProp::VolumeProperties(out, gb);
    log::info("Delete Face: wall rebuilt, " + std::to_string(faceCount(out)) + " faces, valid " + std::to_string((int)BRepCheck_Analyzer(out).IsValid()) +
              ", volume " + std::to_string(ga.Mass()) + " -> " + std::to_string(gb.Mass()));
    if (!BRepCheck_Analyzer(out).IsValid()) return {};
    if (std::abs(ga.Mass() - gb.Mass()) > 1e-4 * std::max(1.0, std::abs(ga.Mass()))) return {};
    if (faceCount(out) >= fmap.Extent()) return {};
    (void)a2;
    return out;
}
} // namespace

ShapePtr deleteFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces) {
    if (faces.empty()) throw std::runtime_error("Select the faces to delete");
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        for (uint32_t fi : faces)
            if ((int)fi >= fmap.Extent())
                throw std::runtime_error("The body changed and the stored faces no longer match. Delete this step and pick the faces again.");
        std::string why;   // a specific reason, when the fill knows one
        std::vector<int> picked;
        for (uint32_t fi : faces) picked.push_back((int)fi + 1);
        TopoDS_Shape res;
        // A face that only splits a surface (an imprint): the surface becomes whole again.
        try { res = dissolveImprints(shape->shape, fmap, picked); } catch (const Standard_Failure& e) {
            log::info(std::string("Delete Face: merging failed: ") + (e.GetMessageString() ? e.GetMessageString() : "")); res.Nullify();
        } catch (...) { log::info("Delete Face: merging failed"); res.Nullify(); }
        if (!res.IsNull()) {
            auto h = std::make_shared<ShapeHandle>();
            h->shape = res;
            return h;
        }
        {
            // Flats across a cylinder first: the round face continues over them (a smooth bore or
            // shaft). Growing the neighbours there tends to close the whole hole instead.
            TopoDS_Shape cur = shape->shape;
            bool all = true;
            std::vector<std::pair<gp_Dir, gp_Pnt>> spots;
            for (int fi : picked) {
                gp_Dir n; gp_Pnt p;
                if (!planarNormal(TopoDS::Face(fmap(fi)), n, p)) { all = false; break; }
                GProp_GProps g;
                BRepGProp::SurfaceProperties(fmap(fi), g);
                spots.push_back({n, g.CentreOfMass()});
            }
            for (size_t k = 0; all && k < spots.size(); ++k) {
                TopTools_IndexedMapOfShape cm;
                TopExp::MapShapes(cur, TopAbs_FACE, cm);
                int hit = 0;
                for (int j = 1; j <= cm.Extent() && !hit; ++j) {
                    gp_Dir n; gp_Pnt p;
                    if (!planarNormal(TopoDS::Face(cm(j)), n, p) || !n.IsParallel(spots[k].first, 1e-6) || n.Dot(spots[k].first) < 0) continue;
                    GProp_GProps g;
                    BRepGProp::SurfaceProperties(cm(j), g);
                    if (g.CentreOfMass().Distance(spots[k].second) < 1e-4) hit = j;
                }
                TopoDS_Shape r = hit ? fillFlatOnCylinder(cur, cm, hit) : TopoDS_Shape();
                if (r.IsNull()) { all = false; break; }
                cur = r;
            }
            if (all) res = cur;
        }
        if (res.IsNull()) {
            // Slivers next to the picked faces (zero width walls left by curves that met a hair
            // apart) go with them: alone they block the heal, together they vanish cleanly.
            TopTools_IndexedDataMapOfShapeListOfShape e2f;
            TopExp::MapShapesAndAncestors(shape->shape, TopAbs_EDGE, TopAbs_FACE, e2f);
            std::set<int> with(picked.begin(), picked.end());
            for (int fi : picked)
                for (TopExp_Explorer ex(fmap(fi), TopAbs_EDGE); ex.More(); ex.Next()) {
                    int k = e2f.FindIndex(ex.Current());
                    if (k <= 0) continue;
                    for (TopTools_ListIteratorOfListOfShape it(e2f(k)); it.More(); it.Next()) {
                        int j = fmap.FindIndex(it.Value());
                        if (j <= 0 || with.count(j)) continue;
                        GProp_GProps g;
                        BRepGProp::SurfaceProperties(it.Value(), g);
                        if (g.Mass() < 1e-3) with.insert(j);
                    }
                }
            if (with.size() > picked.size()) res = tryDefeature(shape->shape, fmap, std::vector<int>(with.begin(), with.end()));
        }
        if (res.IsNull()) res = tryDefeature(shape->shape, fmap, picked);
        if (res.IsNull()) {
            // Fusion heals a hole from one click on its wall: take the rest of the hole along.
            std::vector<int> grown = growToFeature(shape->shape, fmap, faces);
            if (grown.size() > picked.size()) res = tryDefeature(shape->shape, fmap, grown);
        }
        if (res.IsNull()) {
            // Flat faces between flat neighbours: fill the wedge they leave, one face at a time.
            TopoDS_Shape cur = shape->shape;
            bool all = true;
            std::vector<uint32_t> order(faces.begin(), faces.end());
            // Face numbers shift as faces go: fill from the original shape's planes, by position.
            std::vector<std::pair<gp_Dir, gp_Pnt>> want;
            for (uint32_t f : order) {
                gp_Dir n; gp_Pnt p;
                if (!planarNormal(TopoDS::Face(fmap((int)f + 1)), n, p)) { all = false; break; }
                gp_Pnt c = p;
                GProp_GProps g;
                BRepGProp::SurfaceProperties(fmap((int)f + 1), g);
                c = g.CentreOfMass();
                want.push_back({n, c});
            }
            if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "delete: fallback for %zu faces, planar %d\n", faces.size(), (int)all);
            for (size_t k = 0; all && k < want.size(); ++k) {
                TopTools_IndexedMapOfShape cm;
                TopExp::MapShapes(cur, TopAbs_FACE, cm);
                int hit = 0;
                for (int j = 1; j <= cm.Extent() && !hit; ++j) {
                    gp_Dir n; gp_Pnt p;
                    if (!planarNormal(TopoDS::Face(cm(j)), n, p) || !n.IsParallel(want[k].first, 1e-6) || n.Dot(want[k].first) < 0) continue;
                    GProp_GProps g;
                    BRepGProp::SurfaceProperties(cm(j), g);
                    if (g.CentreOfMass().Distance(want[k].second) < 1e-4) hit = j;
                }
                if (!hit) { if (std::getenv("KESTRAL_FACE_DEBUG")) std::fprintf(stderr, "delete: face not found again\n"); all = false; break; }
                TopoDS_Shape r = fillFlatFace(cur, cm, hit, &why);
                if (r.IsNull()) { all = false; break; }
                cur = r;
            }
            if (all) res = cur;
        }
        if (res.IsNull()) {
            // Curved faces nothing can grow over (the round end of a fin): flat through their corners.
            // Merged first, so a round end split in two halves (a seam) goes as one face.
            TopoDS_Shape cur = shape->shape;
            try { cur = unify(copyShape(shape->shape)); } catch (...) { cur = shape->shape; }
            bool all = true;
            std::vector<std::pair<TopoDS_Face, gp_Pnt>> spots;
            for (int fi : picked) {
                GProp_GProps g;
                BRepGProp::SurfaceProperties(fmap(fi), g);
                spots.push_back({TopoDS::Face(fmap(fi)), g.CentreOfMass()});
            }
            for (size_t k = 0; all && k < spots.size(); ++k) {
                TopTools_IndexedMapOfShape cm;
                TopExp::MapShapes(cur, TopAbs_FACE, cm);
                int hit = 0;
                for (int j = 1; j <= cm.Extent() && !hit; ++j) {
                    const TopoDS_Face& cf = TopoDS::Face(cm(j));
                    if (BRepAdaptor_Surface(cf).GetType() == GeomAbs_Plane || !sameRoundSurface(cf, spots[k].first)) continue;
                    Bnd_Box fb;
                    BRepBndLib::Add(cf, fb);
                    fb.Enlarge(1e-4);
                    if (!fb.IsOut(spots[k].second)) hit = j;
                }
                TopoDS_Shape r = hit ? flattenCurvedFace(cur, cm, hit) : TopoDS_Shape();
                if (r.IsNull()) { all = false; break; }
                cur = r;
            }
            if (all) res = cur;
        }
        if (res.IsNull() && !why.empty()) throw std::runtime_error(why);
        if (res.IsNull())
            throw std::runtime_error("Those faces cannot be deleted: the faces around them cannot grow to close the gap. A face whose neighbours meet only beyond the part (or a curved one in a complex spot) has nothing to grow into. Try deleting the whole feature at once, or Press Pull it instead.");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = unify(res);
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "delete face");
    }
}

namespace {
// Press Pull of a flat face whose neighbours share no sliding direction (the side of a spoke
// between two round walls): the volume it gains or loses is the slab between the old and the new
// plane, cut down by each neighbour's surface carried on (a plane as a half space, a cylinder as
// its solid or its outside). Works when the face meets its neighbours on outside corners, which is
// checked on the result: the tool must lie inside the body (pushing in) or outside it (pulling out).
// Null when the case does not fit; Press Pull then falls back to the straight push.
TopoDS_Shape columnTool(const TopoDS_Shape& body, const TopoDS_Face& f, const TopTools_IndexedDataMapOfShapeListOfShape& e2f,
                        const gp_Dir& n, const gp_Pnt& p, double offset) {
    try {
        Bnd_Box bb;
        BRepBndLib::Add(body, bb);
        if (bb.IsVoid()) return {};
        double diag = std::sqrt(bb.SquareExtent());
        double M = std::abs(offset) + diag + 1.0;
        gp_Pnt c = p;
        {
            GProp_GProps g;
            BRepGProp::SurfaceProperties(f, g);
            c = g.CentreOfMass();
        }
        TopoDS_Face sheet = BRepBuilderAPI_MakeFace(gp_Pln(c, n), -M, M, -M, M).Face();
        TopoDS_Shape tool = BRepPrimAPI_MakePrism(sheet, gp_Vec(n) * offset).Shape();
        std::vector<TopoDS_Face> seen;
        for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next()) {
            int k = e2f.FindIndex(ex.Current());
            if (k <= 0) continue;
            for (TopTools_ListIteratorOfListOfShape it(e2f(k)); it.More(); it.Next()) {
                const TopoDS_Face& g = TopoDS::Face(it.Value());
                if (g.IsSame(f)) continue;
                bool dup = false;
                for (auto& o : seen) if (o.IsSame(g)) dup = true;
                if (dup) continue;
                seen.push_back(g);
                BRepAdaptor_Surface gs(g);
                TopoDS_Shape region;
                if (gs.GetType() == GeomAbs_Plane) {
                    gp_Dir gn;
                    gp_Pnt gp0;
                    planarNormal(g, gn, gp0);
                    GProp_GProps gg;
                    BRepGProp::SurfaceProperties(g, gg);
                    gp_Pnt gc = gg.CentreOfMass();
                    TopoDS_Face big = BRepBuilderAPI_MakeFace(gp_Pln(gc, gn), -4 * M, 4 * M, -4 * M, 4 * M).Face();
                    region = BRepPrimAPI_MakeHalfSpace(big, gc.Translated(gp_Vec(gn) * -1.0)).Solid();
                    BRepAlgoAPI_Common cm(tool, region);
                    cm.Build();
                    if (!cm.IsDone()) return {};
                    tool = cm.Shape();
                } else if (gs.GetType() == GeomAbs_Cylinder) {
                    gp_Cylinder cy = gs.Cylinder();
                    double u0, u1, v0, v1;
                    BRepTools::UVBounds(g, u0, u1, v0, v1);
                    gp_Pnt pm;
                    gp_Vec du, dv;
                    gs.D1((u0 + u1) / 2, (v0 + v1) / 2, pm, du, dv);
                    gp_Vec out = du.Crossed(dv);
                    if (g.Orientation() == TopAbs_REVERSED) out.Reverse();
                    gp_Vec w(cy.Location(), pm), ad(cy.Axis().Direction());
                    gp_Vec radial = w - ad * w.Dot(ad);
                    bool inside = out.Dot(radial) > 0;   // the body is inside this cylinder there
                    gp_Pnt base = cy.Location().Translated(ad * (-2 * M + w.Dot(ad)));
                    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(gp_Ax2(base, cy.Axis().Direction()), cy.Radius(), 4 * M).Shape();
                    if (inside) {
                        BRepAlgoAPI_Common cm(tool, rod);
                        cm.Build();
                        if (!cm.IsDone()) return {};
                        tool = cm.Shape();
                    } else {
                        BRepAlgoAPI_Cut ct(tool, rod);
                        ct.Build();
                        if (!ct.IsDone()) return {};
                        tool = ct.Shape();
                    }
                } else {
                    return {};
                }
            }
        }
        if (tool.IsNull() || countSolids(tool) == 0) return {};
        GProp_GProps tg;
        BRepGProp::VolumeProperties(tool, tg);
        double tv = tg.Mass();
        if (!(tv > 1e-9)) return {};
        BRepAlgoAPI_Common inBody(body, tool);
        inBody.Build();
        if (!inBody.IsDone()) return {};
        GProp_GProps ig;
        BRepGProp::VolumeProperties(inBody.Shape(), ig);
        double shared = ig.Mass();
        if (offset < 0 ? std::abs(shared - tv) > 1e-6 * std::max(1.0, tv) + 1e-7 : shared > 1e-6 * std::max(1.0, tv) + 1e-7) return {};
        return tool;
    } catch (...) {
        return {};
    }
}
} // namespace

ShapePtr offsetFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces, double offset) {
    if (faces.empty()) throw std::runtime_error("Select a face first");
    if (std::abs(offset) < 1e-7) {
        auto h = std::make_shared<ShapeHandle>();
        h->shape = shape->shape;
        return h;
    }
    TopTools_IndexedMapOfShape fmap;
    TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
    for (uint32_t fi : faces)
        if ((int)fi >= fmap.Extent())
            throw std::runtime_error("The body changed and the stored faces no longer match. Edit the feature and reselect them.");

    // Flat faces: extrude the face along its outward normal and join (grow) or cut (shrink).
    // This is exactly what Fusion's Press Pull does on a planar face and is very robust.
    bool allPlanar = true;
    for (uint32_t fi : faces) {
        gp_Dir n;
        gp_Pnt p;
        if (!planarNormal(TopoDS::Face(fmap((int)fi + 1)), n, p)) allPlanar = false;
    }
    if (allPlanar) {
        try {
            TopoDS_Shape acc = copyShape(shape->shape);
            TopTools_IndexedMapOfShape cmap;
            TopExp::MapShapes(acc, TopAbs_FACE, cmap);
            std::vector<TopoDS_Shape> tools;
            TopTools_IndexedDataMapOfShapeListOfShape e2f;
            TopExp::MapShapesAndAncestors(acc, TopAbs_EDGE, TopAbs_FACE, e2f);
            for (uint32_t fi : faces) {
                const TopoDS_Face& f = TopoDS::Face(cmap((int)fi + 1));
                gp_Dir n;
                gp_Pnt p;
                planarNormal(f, n, p);
                // Like Fusion: the face moves and its neighbours stay on their own surfaces. So it slides
                // along the direction all its side faces share (the walls of a tab, a cylinder's axis),
                // not straight out along its normal: a sloped top would otherwise leave a step where the
                // walls are not square to it. The plane still moves exactly `offset` along its normal.
                gp_Vec slide(n);
                {
                    std::vector<gp_Dir> planes;
                    std::vector<gp_Dir> axes;
                    bool other = false;
                    for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next()) {
                        int k = e2f.FindIndex(ex.Current());
                        if (k <= 0) continue;
                        for (TopTools_ListIteratorOfListOfShape it(e2f(k)); it.More(); it.Next()) {
                            if (it.Value().IsSame(f)) continue;
                            BRepAdaptor_Surface ns(TopoDS::Face(it.Value()));
                            if (ns.GetType() == GeomAbs_Plane) planes.push_back(ns.Plane().Axis().Direction());
                            else if (ns.GetType() == GeomAbs_Cylinder) axes.push_back(ns.Cylinder().Axis().Direction());
                            else other = true;
                        }
                    }
                    gp_Vec cand(0, 0, 0);
                    bool have = false;
                    if (!other) {
                        if (!axes.empty()) { cand = gp_Vec(axes[0]); have = true; }
                        for (size_t i = 0; i < planes.size() && !have; ++i)
                            for (size_t j = i + 1; j < planes.size() && !have; ++j) {
                                gp_Vec c = gp_Vec(planes[i]).Crossed(gp_Vec(planes[j]));
                                if (c.Magnitude() > 1e-6) { cand = c.Normalized(); have = true; }
                            }
                        if (!have && !planes.empty()) {   // all walls parallel: the in-wall direction nearest the normal
                            gp_Vec m(planes[0]);
                            gp_Vec c = gp_Vec(n) - m * gp_Vec(n).Dot(m);
                            if (c.Magnitude() > 1e-6) { cand = c.Normalized(); have = true; }
                        }
                        if (have) {
                            for (auto& d : planes) if (std::abs(cand.Dot(gp_Vec(d))) > 1e-6) have = false;
                            for (auto& d : axes) if (!gp_Dir(cand).IsParallel(d, 1e-6)) have = false;
                        }
                    }
                    if (have) {
                        if (cand.Dot(gp_Vec(n)) < 0) cand.Reverse();
                        double c = cand.Dot(gp_Vec(n));
                        if (c > 0.15) slide = cand / c;   // so the plane moves 1 along its normal per unit
                        else have = false;
                    }
                    if (!have) {
                        // No shared direction (a face between round walls): the neighbours carried on
                        // bound what is added or taken away.
                        TopoDS_Shape col = columnTool(acc, f, e2f, n, p, offset);
                        if (!col.IsNull()) { tools.push_back(col); continue; }
                    }
                }
                gp_Vec v = slide * offset;
                BRepPrimAPI_MakePrism pr(f, v);
                pr.Build();
                if (!pr.IsDone()) throw std::runtime_error("Press Pull could not extrude that face");
                tools.push_back(pr.Shape());
            }
            for (auto& t : tools) acc = boolOp(offset > 0, acc, t, offset > 0 ? "Press Pull" : "Press Pull cut");
            if (countSolids(acc) == 0) throw std::runtime_error("Press Pull removed the whole body");
            auto h = std::make_shared<ShapeHandle>();
            h->shape = acc;
            return h;
        } catch (const Standard_Failure& e) {
            rethrowOcct(e, "press pull");
        }
    }
    // Full round faces (hole walls, shafts): set the new radius directly with a ring added or cut
    // over the face's length. Far more reliable than offsetting the face.
    {
        struct Round { gp_Ax2 base; double r, len; bool hole; };
        std::vector<Round> rounds;
        for (uint32_t fi : faces) {
            const TopoDS_Face& f = TopoDS::Face(fmap((int)fi + 1));
            BRepAdaptor_Surface s(f, Standard_True);
            if (s.GetType() != GeomAbs_Cylinder) break;
            gp_Cylinder c = s.Cylinder();
            double u0, u1, v0, v1;
            BRepTools::UVBounds(f, u0, u1, v0, v1);
            // Whole circle: this face alone or with the other faces on the same cylinder and span.
            double span = u1 - u0;
            for (int k = 1; k <= fmap.Extent() && span < 2 * kPiK - 1e-3; ++k) {
                if (k == (int)fi + 1) continue;
                const TopoDS_Face& g = TopoDS::Face(fmap(k));
                BRepAdaptor_Surface sg(g, Standard_True);
                if (sg.GetType() != GeomAbs_Cylinder) continue;
                gp_Cylinder cg = sg.Cylinder();
                if (std::abs(cg.Radius() - c.Radius()) > 1e-6 || !cg.Axis().IsCoaxial(c.Axis(), 1e-6, 1e-6)) continue;
                double a0, a1, b0, b1;
                BRepTools::UVBounds(g, a0, a1, b0, b1);
                bool sameDir = cg.Axis().Direction().IsEqual(c.Axis().Direction(), 1e-6);
                gp_Pnt g0 = cg.Location().Translated(gp_Vec(cg.Axis().Direction()) * b0);
                double gv0 = gp_Vec(c.Location(), g0).Dot(gp_Vec(c.Axis().Direction()));
                double gv1 = gv0 + (sameDir ? 1 : -1) * (b1 - b0);
                if (std::abs(std::min(gv0, gv1) - v0) < 1e-5 && std::abs(std::max(gv0, gv1) - v1) < 1e-5) span += a1 - a0;
            }
            if (span < 2 * kPiK - 1e-3) break;
            gp_Pnt pm;
            gp_Vec du, dv;
            s.D1((u0 + u1) / 2, (v0 + v1) / 2, pm, du, dv);
            gp_Vec n = du.Crossed(dv);
            if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
            gp_Vec w(c.Location(), pm);
            gp_Vec d(c.Axis().Direction());
            gp_Vec radial = w - d * w.Dot(d);
            gp_Pnt o = c.Location().Translated(d * v0);
            rounds.push_back({gp_Ax2(o, c.Axis().Direction()), c.Radius(), v1 - v0, n.Dot(radial) < 0});
        }
        if (rounds.size() == faces.size()) {
            try {
                TopoDS_Shape acc = copyShape(shape->shape);
                for (auto& rd : rounds) {
                    // offset runs along the outward normal: into the hole for a hole wall.
                    double rn = rd.hole ? rd.r - offset : rd.r + offset;
                    if (rn <= 1e-4) throw std::runtime_error("That would make the hole or shaft vanish (radius " + std::to_string(rd.r) + " mm)");
                    auto disc = [&](double r) { return BRepPrimAPI_MakeCylinder(rd.base, r, rd.len).Shape(); };
                    auto ring = [&](double a, double b) { return boolOp(false, disc(b), disc(a), "Press Pull ring"); };
                    double lo = std::min(rn, rd.r), hi = std::max(rn, rd.r);
                    bool addMaterial = rd.hole ? rn < rd.r : rn > rd.r;
                    if (addMaterial) acc = boolOp(true, acc, ring(lo, hi), "Press Pull");
                    else if (rd.hole) acc = boolOp(false, acc, disc(rn), "Press Pull cut");
                    else acc = boolOp(false, acc, ring(rn, rd.r + std::max(1.0, rd.r)), "Press Pull cut");
                }
                if (countSolids(acc) == 0) throw std::runtime_error("Press Pull removed the whole body");
                auto h = std::make_shared<ShapeHandle>();
                h->shape = acc;
                return h;
            } catch (const Standard_Failure& e) {
                rethrowOcct(e, "press pull");
            }
        }
    }
    // Curved faces (fillets, partial cylinders): offset just those faces and let the neighbours follow.
    try {
        TopoDS_Shape priv = copyShape(shape->shape);
        TopTools_IndexedMapOfShape cmap;
        TopExp::MapShapes(priv, TopAbs_FACE, cmap);
        BRepOffset_MakeOffset mo;
        mo.Initialize(priv, 0.0, 1e-4, BRepOffset_Skin, Standard_True, Standard_False, GeomAbs_Intersection, Standard_False);
        for (uint32_t fi : faces) mo.SetOffsetOnFace(TopoDS::Face(cmap((int)fi + 1)), offset);
        mo.MakeOffsetShape();
        if (mo.IsDone() && !mo.Shape().IsNull() && countSolids(mo.Shape()) > 0) {
            auto h = std::make_shared<ShapeHandle>();
            h->shape = mo.Shape();
            return h;
        }
    } catch (const Standard_Failure&) {
    } catch (const std::exception&) {
    }
    throw std::runtime_error("Press Pull cannot move this face (try a smaller offset)");
}

// ---- picking helpers -------------------------------------------------------------------

std::vector<uint32_t> faceEdges(const ShapePtr& shape, uint32_t face) {
    std::vector<uint32_t> out;
    try {
        TopTools_IndexedMapOfShape fmap, emap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
        if ((int)face >= fmap.Extent()) return out;
        for (TopExp_Explorer ex(fmap((int)face + 1), TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (BRep_Tool::Degenerated(e)) continue;
            int i = emap.FindIndex(e);
            if (i > 0 && std::find(out.begin(), out.end(), (uint32_t)(i - 1)) == out.end()) out.push_back((uint32_t)(i - 1));
        }
    } catch (...) {
    }
    return out;
}

std::vector<uint32_t> tangentChain(const ShapePtr& shape, uint32_t edge) {
    std::vector<uint32_t> out;
    try {
        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
        if ((int)edge >= emap.Extent()) return out;
        TopTools_IndexedDataMapOfShapeListOfShape v2e;
        TopExp::MapShapesAndAncestors(shape->shape, TopAbs_VERTEX, TopAbs_EDGE, v2e);
        // Tangent of edge e at vertex v, pointing away from v.
        auto tangentAt = [](const TopoDS_Edge& e, const TopoDS_Vertex& v, gp_Vec& t) {
            BRepAdaptor_Curve c(e);
            gp_Pnt pv = BRep_Tool::Pnt(v);
            double f = c.FirstParameter(), l = c.LastParameter();
            bool atStart = c.Value(f).Distance(pv) <= c.Value(l).Distance(pv);
            gp_Pnt p;
            c.D1(atStart ? f : l, p, t);
            if (!atStart) t.Reverse();
            return t.Magnitude() > 1e-12;
        };
        std::vector<char> seen(emap.Extent() + 1, 0);
        std::vector<int> stack{(int)edge + 1};
        seen[edge + 1] = 1;
        while (!stack.empty() && out.size() < 2000) {
            int ei = stack.back();
            stack.pop_back();
            out.push_back((uint32_t)(ei - 1));
            const TopoDS_Edge& e = TopoDS::Edge(emap(ei));
            TopoDS_Vertex vs[2];
            TopExp::Vertices(e, vs[0], vs[1]);
            for (auto& v : vs) {
                if (v.IsNull() || !v2e.Contains(v)) continue;
                gp_Vec t0;
                if (!tangentAt(e, v, t0)) continue;
                for (TopTools_ListIteratorOfListOfShape it(v2e.FindFromKey(v)); it.More(); it.Next()) {
                    const TopoDS_Edge& o = TopoDS::Edge(it.Value());
                    int oi = emap.FindIndex(o);
                    if (oi <= 0 || seen[oi] || BRep_Tool::Degenerated(o)) continue;
                    gp_Vec t1;
                    if (!tangentAt(o, v, t1)) continue;
                    // Smooth continuation: leaving directions are opposite.
                    if (t0.Normalized().Dot(t1.Normalized()) < -0.996) {
                        seen[oi] = 1;
                        stack.push_back(oi);
                    }
                }
            }
        }
    } catch (...) {
        if (out.empty()) out.push_back(edge);
    }
    return out;
}

bool edgeAxis(const ShapePtr& shape, uint32_t edge, glm::dvec3& origin, glm::dvec3& dir) {
    try {
        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
        if ((int)edge >= emap.Extent()) return false;
        BRepAdaptor_Curve c(TopoDS::Edge(emap((int)edge + 1)));
        if (c.GetType() == GeomAbs_Circle) {
            gp_Ax1 ax = c.Circle().Axis();
            origin = {ax.Location().X(), ax.Location().Y(), ax.Location().Z()};
            dir = {ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z()};
            return true;
        }
        if (c.GetType() == GeomAbs_Ellipse) {
            gp_Ax1 ax = c.Ellipse().Axis();
            origin = {ax.Location().X(), ax.Location().Y(), ax.Location().Z()};
            dir = {ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z()};
            return true;
        }
        if (c.GetType() == GeomAbs_Line) {
            gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
            gp_Vec v(a, b);
            if (v.Magnitude() < 1e-9) return false;
            v.Normalize();
            origin = {a.X(), a.Y(), a.Z()};
            dir = {v.X(), v.Y(), v.Z()};
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool edgeFrame(const ShapePtr& shape, uint32_t edge, glm::dvec3& mid, glm::dvec3& outward) {
    try {
        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
        if ((int)edge >= emap.Extent()) return false;
        TopoDS_Edge e = TopoDS::Edge(emap((int)edge + 1));
        BRepAdaptor_Curve c(e);
        gp_Pnt p = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
        mid = {p.X(), p.Y(), p.Z()};
        TopTools_IndexedDataMapOfShapeListOfShape ef;
        TopExp::MapShapesAndAncestors(shape->shape, TopAbs_EDGE, TopAbs_FACE, ef);
        if (!ef.Contains(e)) return false;
        gp_Vec sum(0, 0, 0);
        for (TopTools_ListIteratorOfListOfShape it(ef.FindFromKey(e)); it.More(); it.Next()) {
            TopoDS_Face f = TopoDS::Face(it.Value());
            GeomAPI_ProjectPointOnSurf proj(p, BRep_Tool::Surface(f));
            if (!proj.IsDone() || proj.NbPoints() == 0) continue;
            double u, v;
            proj.LowerDistanceParameters(u, v);
            BRepAdaptor_Surface s(f);
            BRepLProp_SLProps pr(s, u, v, 1, 1e-6);
            if (!pr.IsNormalDefined()) continue;
            gp_Dir n = pr.Normal();
            if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
            sum += gp_Vec(n);
        }
        if (sum.Magnitude() < 1e-9) return false;
        sum.Normalize();
        outward = {sum.X(), sum.Y(), sum.Z()};
        return true;
    } catch (...) {
        return false;
    }
}

bool faceInfo(const ShapePtr& shape, uint32_t face, glm::dvec3& center, glm::dvec3& normal, bool& planar) {
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) return false;
        const TopoDS_Face& f = TopoDS::Face(fmap((int)face + 1));
        GProp_GProps g;
        BRepGProp::SurfaceProperties(f, g);
        gp_Pnt c = g.CentreOfMass();
        center = {c.X(), c.Y(), c.Z()};
        gp_Dir n;
        gp_Pnt p;
        planar = planarNormal(f, n, p);
        if (!planar) {
            BRepAdaptor_Surface s(f);
            double u = (s.FirstUParameter() + s.LastUParameter()) * 0.5, v = (s.FirstVParameter() + s.LastVParameter()) * 0.5;
            BRepLProp_SLProps pr(s, u, v, 1, 1e-6);
            if (!pr.IsNormalDefined()) return false;
            n = pr.Normal();
            if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
        }
        normal = {n.X(), n.Y(), n.Z()};
        return true;
    } catch (...) {
        return false;
    }
}

bool faceCylinder(const ShapePtr& shape, uint32_t face, glm::dvec3& origin, glm::dvec3& dir, double& radius) {
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) return false;
        BRepAdaptor_Surface s(TopoDS::Face(fmap((int)face + 1)), Standard_False);
        if (s.GetType() != GeomAbs_Cylinder) return false;
        gp_Cylinder c = s.Cylinder();
        gp_Ax1 ax = c.Axis();
        origin = {ax.Location().X(), ax.Location().Y(), ax.Location().Z()};
        dir = {ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z()};
        radius = c.Radius();
        return radius > 1e-9;
    } catch (...) {
        return false;
    }
}

bool faceRoundAxis(const ShapePtr& shape, uint32_t face, glm::dvec3& origin, glm::dvec3& dir) {
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) return false;
        const TopoDS_Face& f = TopoDS::Face(fmap((int)face + 1));
        BRepAdaptor_Surface s(f, Standard_False);
        gp_Ax1 ax;
        switch (s.GetType()) {
        case GeomAbs_Cylinder: ax = s.Cylinder().Axis(); break;
        case GeomAbs_Cone: ax = s.Cone().Axis(); break;
        case GeomAbs_Torus: ax = s.Torus().Axis(); break;
        default: return false;
        }
        // The surface origin can sit anywhere on the axis. The face's middle is the snap.
        GProp_GProps g;
        BRepGProp::SurfaceProperties(f, g);
        gp_Vec along(ax.Direction());
        gp_Pnt o = ax.Location().Translated(along * gp_Vec(ax.Location(), g.CentreOfMass()).Dot(along));
        origin = {o.X(), o.Y(), o.Z()};
        dir = {ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z()};
        return true;
    } catch (...) {
        return false;
    }
}

namespace {
gp_Trsf trsfOf(const glm::mat4& m) {
    gp_Trsf tr;
    tr = trsfOfMat(m);
    return tr;
}

bool geomOfEdge(const TopoDS_Edge& edge, EdgeGeom& out) {
    if (BRep_Tool::Degenerated(edge)) return false;
    BRepAdaptor_Curve c(edge);
    double f = c.FirstParameter(), l = c.LastParameter();
    auto P = [](const gp_Pnt& p) { return glm::dvec3(p.X(), p.Y(), p.Z()); };
    out = EdgeGeom{};
    out.a = P(c.Value(f));
    out.b = P(c.Value(l));
    GCPnts_QuasiUniformDeflection disc(c, 0.02);
    if (disc.IsDone())
        for (int i = 1; i <= disc.NbPoints(); ++i) out.pts.push_back(P(disc.Value(i)));
    if (out.pts.size() < 2) out.pts = {out.a, out.b};
    if (c.GetType() == GeomAbs_Line) {
        out.type = 0;
    } else if (c.GetType() == GeomAbs_Circle) {
        gp_Circ ci = c.Circle();
        out.center = P(ci.Location());
        gp_Dir d = ci.Axis().Direction();
        out.normal = {d.X(), d.Y(), d.Z()};
        out.radius = ci.Radius();
        out.type = (l - f) > 2 * kPiK - 1e-6 ? 1 : 2;
    }
    return true;
}
} // namespace

ShapePtr patternFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces, const std::vector<glm::dmat4>& copies) {
    if (!shape || shape->shape.IsNull()) throw std::runtime_error("The body is missing");
    if (faces.empty()) throw std::runtime_error("Pick the faces to copy");
    ShapePtr filled;
    try {
        filled = deleteFaces(shape, faces);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(std::string("Those faces could not be lifted off the body to copy them. ") + e.what());
    }
    if (!filled || filled->shape.IsNull()) throw std::runtime_error("Those faces could not be lifted off the body to copy them");
    try {
        const TopoDS_Shape& B = shape->shape;
        const TopoDS_Shape& F = filled->shape;
        auto vol = [](const TopoDS_Shape& s) { GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass(); };
        auto cut = [](const TopoDS_Shape& a, const TopoDS_Shape& b) {
            BRepAlgoAPI_Cut c(a, b);
            return c.IsDone() ? c.Shape() : TopoDS_Shape();
        };
        auto solids = [&](const TopoDS_Shape& s) {
            std::vector<TopoDS_Shape> out;
            if (s.IsNull()) return out;
            for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next())
                if (vol(ex.Current()) > 1e-6) out.push_back(ex.Current());
            return out;
        };
        // What the faces take away (a hole's plug) and what they add (a boss).
        std::vector<TopoDS_Shape> taken = solids(cut(F, B)), added = solids(cut(B, F));
        if (taken.empty() && added.empty())
            throw std::runtime_error("Those faces do not make a feature that can be copied (a hole, a pocket or a boss)");
        // A copy that lands where the feature already is (the axis runs through the feature itself,
        // e.g. a hole's own edge) adds nothing: leave it out, and say so when every copy does.
        auto centre = [](const TopoDS_Shape& s) { GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.CentreOfMass(); };
        gp_Pnt c0 = centre(!taken.empty() ? taken[0] : added[0]);
        TopTools_ListOfShape takeCopies, addCopies;
        int kept = 0;
        for (const glm::dmat4& m : copies) {
            gp_Trsf t = trsfOfMat(glm::mat4(m));
            if (c0.Transformed(t).Distance(c0) < 1e-4) continue;
            TopLoc_Location loc(t);
            for (auto& s : taken) takeCopies.Append(s.Moved(loc));
            for (auto& s : added) addCopies.Append(s.Moved(loc));
            ++kept;
        }
        if (kept == 0 && !copies.empty())
            throw std::runtime_error("Every copy lands on the original: the axis runs through the faces being copied. "
                                     "Pick an axis in the middle of the part (the bore, or a round edge or face around the center).");
        // All at once; one copy at a time when that fails (overlapping copies, touching faces).
        auto boolAll = [](bool fuse, const TopoDS_Shape& base, const TopTools_ListOfShape& tools) -> TopoDS_Shape {
            for (double fuzzy : {0.0, 1e-5}) {
                TopTools_ListOfShape args;
                args.Append(base);
                if (fuse) {
                    BRepAlgoAPI_Fuse op;
                    op.SetArguments(args);
                    op.SetTools(tools);
                    if (fuzzy > 0) op.SetFuzzyValue(fuzzy);
                    op.Build();
                    if (op.IsDone() && !op.HasErrors()) return op.Shape();
                } else {
                    BRepAlgoAPI_Cut op;
                    op.SetArguments(args);
                    op.SetTools(tools);
                    if (fuzzy > 0) op.SetFuzzyValue(fuzzy);
                    op.Build();
                    if (op.IsDone() && !op.HasErrors()) return op.Shape();
                }
            }
            TopoDS_Shape cur = base;
            for (TopTools_ListIteratorOfListOfShape it(tools); it.More(); it.Next()) {
                TopoDS_Shape next;
                if (fuse) { BRepAlgoAPI_Fuse op(cur, it.Value()); if (op.IsDone()) next = op.Shape(); }
                else { BRepAlgoAPI_Cut op(cur, it.Value()); if (op.IsDone()) next = op.Shape(); }
                if (next.IsNull()) return TopoDS_Shape();
                cur = next;
            }
            return cur;
        };
        TopoDS_Shape cur = B;
        if (!addCopies.IsEmpty()) {
            cur = boolAll(true, cur, addCopies);
            if (cur.IsNull()) throw std::runtime_error("The copies could not be joined to the body");
        }
        if (!takeCopies.IsEmpty()) {
            cur = boolAll(false, cur, takeCopies);
            if (cur.IsNull()) throw std::runtime_error("The copies could not be cut into the body");
        }
        try { cur = unify(copyShape(cur)); } catch (...) {}
        int n = 0;
        for (TopExp_Explorer ex(cur, TopAbs_SOLID); ex.More(); ex.Next()) ++n;
        if (n == 0) throw std::runtime_error("The pattern left nothing of the body");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = cur;
        return h;
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Pattern of faces failed: ") + (e.GetMessageString() ? e.GetMessageString() : "geometry error"));
    }
}

ShapePtr placeShape(const ShapePtr& shape, const glm::mat4& xf) {
    if (xf == glm::mat4(1.f)) return shape;
    try {
        auto h = std::make_shared<ShapeHandle>();
        // A placement that scales cannot live in a TopLoc_Location (OCCT refuses it), so the shape
        // itself is transformed: uniform scale with Transform, uneven scale with GTransform.
        glm::dvec3 sc(glm::length(glm::dvec3(xf[0])), glm::length(glm::dvec3(xf[1])), glm::length(glm::dvec3(xf[2])));
        bool scaled = std::abs(sc.x - 1) > 1e-7 || std::abs(sc.y - 1) > 1e-7 || std::abs(sc.z - 1) > 1e-7;
        bool uniform = std::abs(sc.x - sc.y) < 1e-7 * sc.x && std::abs(sc.x - sc.z) < 1e-7 * sc.x;
        if (!scaled) {
            h->shape = shape->shape.Moved(TopLoc_Location(trsfOf(xf)));
        } else if (uniform) {
            h->shape = BRepBuilderAPI_Transform(shape->shape, trsfOf(xf), Standard_True).Shape();
        } else {
            gp_GTrsf g;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) g.SetValue(r + 1, c + 1, xf[c][r]);
                g.SetValue(r + 1, 4, xf[3][r]);
            }
            h->shape = BRepBuilderAPI_GTransform(shape->shape, g, Standard_True).Shape();
        }
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "place");
    }
}

bool edgeGeom(const ShapePtr& shape, const glm::mat4& xf, uint32_t edge, EdgeGeom& out) {
    try {
        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(shape->shape, TopAbs_EDGE, emap);
        if ((int)edge >= emap.Extent()) return false;
        TopoDS_Edge e = TopoDS::Edge(emap((int)edge + 1));
        if (BRep_Tool::Degenerated(e)) return false;
        e = TopoDS::Edge(BRepBuilderAPI_Transform(e, trsfOf(xf), Standard_True).Shape());
        return geomOfEdge(e, out);
    } catch (...) {
        return false;
    }
}

std::vector<EdgeGeom> allEdgeGeoms(const ShapePtr& shape, const glm::mat4& xf, size_t maxEdges) {
    std::vector<EdgeGeom> out;
    if (!shape || shape->shape.IsNull()) return out;
    try {
        ShapePtr placed = placeShape(shape, xf);
        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(placed->shape, TopAbs_EDGE, emap);
        size_t n = std::min((size_t)emap.Extent(), maxEdges);
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            try {
                if (!geomOfEdge(TopoDS::Edge(emap((int)i + 1)), out[i])) out[i] = EdgeGeom{}, out[i].pts.clear();
            } catch (...) { out[i] = EdgeGeom{}; out[i].pts.clear(); }
        }
    } catch (...) {}
    return out;
}

std::vector<EdgeGeom> sectionEdges(const ShapePtr& shape, const glm::mat4& xf, const glm::dvec3& origin, const glm::dvec3& normal) {
    std::vector<EdgeGeom> out;
    if (!shape || shape->shape.IsNull() || glm::length(normal) < 1e-12) return out;
    try {
        TopoDS_Shape placed = shape->shape.Moved(TopLoc_Location(trsfOf(xf)));
        gp_Pln pln(gp_Pnt(origin.x, origin.y, origin.z), gp_Dir(normal.x, normal.y, normal.z));
        BRepAlgoAPI_Section sec(placed, pln, Standard_False);
        sec.Approximation(Standard_True);
        sec.Build();
        if (!sec.IsDone()) return out;
        for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) {
            EdgeGeom g;
            if (geomOfEdge(TopoDS::Edge(ex.Current()), g)) out.push_back(std::move(g));
        }
    } catch (...) {}
    return out;
}

std::vector<EdgeGeom> projectOnFace(const ShapePtr& shape, const glm::mat4& xf, uint32_t face,
                                    const std::vector<DropCurve>& curves, const glm::dvec3& direction) {
    std::vector<EdgeGeom> out;
    if (!shape || shape->shape.IsNull() || curves.empty() || glm::length(direction) < 1e-12) return out;
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) return out;
        TopoDS_Face f = TopoDS::Face(BRepBuilderAPI_Transform(fmap((int)face + 1), trsfOf(xf), Standard_True).Shape());
        gp_Dir D(direction.x, direction.y, direction.z);
        auto keep = [&](const TopoDS_Shape& sh) {
            for (TopExp_Explorer ex(sh, TopAbs_EDGE); ex.More(); ex.Next()) {
                EdgeGeom g;
                if (geomOfEdge(TopoDS::Edge(ex.Current()), g)) out.push_back(std::move(g));
            }
        };
        for (const DropCurve& c : curves) {
            TopoDS_Edge edge;
            if (c.type == 0) {
                if (glm::length(c.b - c.a) < 1e-8) continue;
                BRepBuilderAPI_MakeEdge me(gp_Pnt(c.a.x, c.a.y, c.a.z), gp_Pnt(c.b.x, c.b.y, c.b.z));
                if (!me.IsDone()) continue;
                edge = me.Edge();
            } else if (c.type == 1 || c.type == 2) {
                glm::dvec3 n = c.normal;
                if (glm::length(n) < 1e-12 || c.radius < 1e-9) continue;
                n = glm::normalize(n);
                glm::dvec3 xdir = c.type == 2 ? c.a - c.center : glm::dvec3(0);
                if (glm::length(xdir) < 1e-9) {
                    glm::dvec3 ref = std::abs(n.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
                    xdir = glm::cross(ref, n);
                }
                if (glm::length(xdir) < 1e-12) continue;
                gp_Ax2 ax(gp_Pnt(c.center.x, c.center.y, c.center.z), gp_Dir(n.x, n.y, n.z), gp_Dir(xdir.x, xdir.y, xdir.z));
                gp_Circ circ(ax, c.radius);
                if (c.type == 1) {
                    BRepBuilderAPI_MakeEdge me(circ);
                    if (!me.IsDone()) continue;
                    edge = me.Edge();
                } else {
                    double sweep = wrap2pi(angleAround(c.b - c.center, n, c.a - c.center));
                    if (sweep < 1e-9) continue;
                    BRepBuilderAPI_MakeEdge me(circ, 0.0, sweep);
                    if (!me.IsDone()) continue;
                    edge = me.Edge();
                }
            } else continue;
            BRepProj_Projection proj(edge, f, D);
            if (proj.IsDone()) keep(proj.Shape());
        }
    } catch (...) {}
    return out;
}

Aabb shapeBounds(const ShapePtr& shape, const glm::mat4& xf) {
    Aabb out;
    try {
        Bnd_Box b;
        BRepBndLib::Add(shape->shape, b);
        if (b.IsVoid()) return out;
        Standard_Real x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        Aabb local;
        local.add(glm::vec3((float)x0, (float)y0, (float)z0));
        local.add(glm::vec3((float)x1, (float)y1, (float)z1));
        return local.transformed(xf);
    } catch (...) {
        return out;
    }
}

bool pointInside(const ShapePtr& shape, const glm::mat4& xf, const glm::dvec3& w) {
    try {
        glm::dvec3 l = glm::dvec3(glm::inverse(glm::dmat4(xf)) * glm::dvec4(w, 1.0));
        BRepClass3d_SolidClassifier c(shape->shape, gp_Pnt(l.x, l.y, l.z), 1e-6);
        return c.State() == TopAbs_IN;
    } catch (...) {
        return false;
    }
}

bool shapesTouch(const ShapePtr& a, const glm::mat4& xa, const ShapePtr& b, const glm::mat4& xb) {
    try {
        Aabb ba = shapeBounds(a, xa), bb = shapeBounds(b, xb);
        if (!ba.valid() || !bb.valid()) return false;
        float pad = 1e-3f;
        if (ba.max.x < bb.min.x - pad || bb.max.x < ba.min.x - pad || ba.max.y < bb.min.y - pad ||
            bb.max.y < ba.min.y - pad || ba.max.z < bb.min.z - pad || bb.max.z < ba.min.z - pad)
            return false;
        TopoDS_Shape sa = a->shape.Moved(TopLoc_Location(trsfOf(xa)));
        TopoDS_Shape sb = b->shape.Moved(TopLoc_Location(trsfOf(xb)));
        BRepExtrema_DistShapeShape d(sa, sb);
        if (d.IsDone() && d.NbSolution() > 0 && d.Value() < 1e-5) return true;
        // One completely inside the other: no boundary contact, but they still overlap.
        glm::dvec3 ca = glm::dvec3(ba.center()), cb = glm::dvec3(bb.center());
        return pointInside(b, xb, ca) || pointInside(a, xa, cb);
    } catch (...) {
        return false;
    }
}

int faceNear(const ShapePtr& shape, const glm::mat4& xf, const glm::dvec3& w, double tol) {
    try {
        glm::dvec3 l = glm::dvec3(glm::inverse(glm::dmat4(xf)) * glm::dvec4(w, 1.0));
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        BRepBuilderAPI_MakeVertex mv(gp_Pnt(l.x, l.y, l.z));
        int best = -1;
        double bestD = tol;
        for (int i = 1; i <= fmap.Extent(); ++i) {
            BRepExtrema_DistShapeShape d(mv.Vertex(), fmap(i));
            if (!d.IsDone() || d.NbSolution() == 0) continue;
            if (d.Value() < bestD) { bestD = d.Value(); best = i - 1; }
        }
        return best;
    } catch (...) {
        return -1;
    }
}

RegionLoops faceOutline(const ShapePtr& shape, const glm::mat4& xf, uint32_t face) {
    TopTools_IndexedMapOfShape fmap;
    TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
    if ((int)face >= fmap.Extent()) throw std::runtime_error("The picked face no longer exists. Edit the loft and pick it again.");
    gp_Trsf tr;
    tr = trsfOfMat(xf);
    TopoDS_Face f = TopoDS::Face(BRepBuilderAPI_Transform(fmap((int)face + 1), tr, Standard_True).Shape());
    gp_Dir pn;
    gp_Pnt pp;
    if (!planarNormal(f, pn, pp)) throw std::runtime_error("Loft takes flat faces only (that one is curved)");
    glm::dvec3 n(pn.X(), pn.Y(), pn.Z());
    auto P = [](const gp_Pnt& q) { return glm::dvec3(q.X(), q.Y(), q.Z()); };
    auto loopOf = [&](const TopoDS_Wire& w) {
    std::vector<CurveSeg> loop;
    for (BRepTools_WireExplorer we(w, f); we.More(); we.Next()) {
        const TopoDS_Edge& e = we.Current();
        if (BRep_Tool::Degenerated(e)) continue;
        bool rev = e.Orientation() == TopAbs_REVERSED;
        BRepAdaptor_Curve c(e);
        double t0 = c.FirstParameter(), t1 = c.LastParameter();
        glm::dvec3 s = P(c.Value(rev ? t1 : t0)), en = P(c.Value(rev ? t0 : t1));   // as the loop runs
        CurveSeg g;
        g.normal = n;
        if (c.GetType() == GeomAbs_Line) {
            g.kind = CurveSeg::Kind::Line;
            g.a = s;
            g.b = en;
            loop.push_back(g);
            continue;
        }
        if (c.GetType() == GeomAbs_Circle) {
            gp_Circ ci = c.Circle();
            gp_Dir ax = ci.Axis().Direction();
            bool ccw = ((ax.X() * n.x + ax.Y() * n.y + ax.Z() * n.z) > 0) != rev;   // runs counterclockwise about n
            g.center = P(ci.Location());
            g.radius = ci.Radius();
            if (t1 - t0 > kTwoPi - 1e-7) {
                g.kind = CurveSeg::Kind::Circle;
                g.a = s;
                g.back = !ccw;
            } else {
                g.kind = CurveSeg::Kind::Arc;
                g.a = ccw ? s : en;
                g.b = ccw ? en : s;
                g.back = !ccw;
            }
            loop.push_back(g);
            continue;
        }
        // Any other curve: short straight pieces along it.
        const int N = 24;
        glm::dvec3 prev = s;
        for (int i = 1; i <= N; ++i) {
            double t = rev ? t1 - (t1 - t0) * i / N : t0 + (t1 - t0) * i / N;
            glm::dvec3 q = P(c.Value(t));
            CurveSeg l;
            l.kind = CurveSeg::Kind::Line;
            l.normal = n;
            l.a = prev;
            l.b = q;
            loop.push_back(l);
            prev = q;
        }
    }
    return loop;
    };
    // The outer boundary first, then each hole (a ring face keeps its hole in the loft).
    TopoDS_Wire outerW = BRepTools::OuterWire(f);
    RegionLoops out{loopOf(outerW)};
    if (out[0].empty()) throw std::runtime_error("That face has no outline");
    for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) {
        if (wx.Current().IsSame(outerW)) continue;
        std::vector<CurveSeg> h = loopOf(TopoDS::Wire(wx.Current()));
        if (!h.empty()) out.push_back(std::move(h));
    }
    return out;
}

ShapePtr extrudeFace(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, double dist, bool symmetric) {
    if (std::abs(dist) < 1e-9) throw std::runtime_error("Distance must not be zero");
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) throw std::runtime_error("The picked face no longer exists. Edit the feature and pick it again.");
        gp_Trsf tr;
        tr = trsfOfMat(xf);
        TopoDS_Face f = TopoDS::Face(BRepBuilderAPI_Transform(fmap((int)face + 1), tr, Standard_True).Shape());
        gp_Dir n;
        gp_Pnt p;
        if (!planarNormal(f, n, p)) throw std::runtime_error("Only flat faces can be extruded");
        gp_Vec v(n);
        TopoDS_Shape shp;
        if (symmetric) {
            gp_Vec h = v * (dist * 0.5);
            TopoDS_Shape a = BRepPrimAPI_MakePrism(f, h).Shape();
            TopoDS_Shape b = BRepPrimAPI_MakePrism(f, -h).Shape();
            shp = boolOp(true, a, b, "Symmetric extrude");
        } else {
            shp = BRepPrimAPI_MakePrism(f, v * dist).Shape();
        }
        if (countSolids(shp) == 0) throw std::runtime_error("The face did not produce a solid");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = shp;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "extrude face");
    }
}

ShapePtr thickenFaces(const ShapePtr& shape, const glm::mat4& xf, const std::vector<uint32_t>& faces, double thickness, int side) {
    if (faces.empty()) throw std::runtime_error("Select a face to thicken");
    if (!(thickness > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
    if (side < 0 || side > 2) side = 0;
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        for (uint32_t fi : faces)
            if ((int)fi >= fmap.Extent())
                throw std::runtime_error("The body changed and the stored faces no longer match. Edit the feature and reselect them.");
        gp_Trsf tr;
        tr = trsfOfMat(xf);
        std::vector<TopoDS_Face> world;
        world.reserve(faces.size());
        for (uint32_t fi : faces)
            world.push_back(TopoDS::Face(BRepBuilderAPI_Transform(fmap((int)fi + 1), tr, Standard_True).Shape()));

        auto take = [](const TopoDS_Shape& s) -> TopoDS_Shape {
            if (s.IsNull() || countSolids(s) == 0) return {};
            return s;
        };
        auto prism = [&](const TopoDS_Face& f, double dist) -> TopoDS_Shape {
            gp_Dir n;
            gp_Pnt p;
            if (!planarNormal(f, n, p)) return {};
            gp_Vec v(n);
            v *= dist;
            BRepPrimAPI_MakePrism pr(f, v);
            pr.Build();
            if (!pr.IsDone()) return {};
            return take(pr.Shape());
        };
        // A full cylinder becomes a tube between the old radius and the offset radius.
        auto tube = [&](const TopoDS_Face& f, int sd) -> TopoDS_Shape {
            BRepAdaptor_Surface s(f, Standard_True);
            if (s.GetType() != GeomAbs_Cylinder) return {};
            gp_Cylinder c = s.Cylinder();
            double u0, u1, v0, v1;
            BRepTools::UVBounds(f, u0, u1, v0, v1);
            if (u1 - u0 < 2 * kPiK - 1e-2) return {};
            double len = v1 - v0;
            if (!(len > 1e-6)) return {};
            gp_Pnt pm;
            gp_Vec du, dv;
            s.D1((u0 + u1) * 0.5, (v0 + v1) * 0.5, pm, du, dv);
            gp_Vec n = du.Crossed(dv);
            if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
            gp_Vec axis(c.Axis().Direction());
            gp_Vec radial = gp_Vec(c.Location(), pm);
            radial = radial - axis * radial.Dot(axis);
            bool away = n.Dot(radial) > 0;
            double r = c.Radius();
            double inner = r, outer = r;
            if (sd == 2) {
                inner = r - thickness * 0.5;
                outer = r + thickness * 0.5;
            } else {
                double grow = sd == 0 ? thickness : -thickness;
                double r2 = r + (away ? grow : -grow);
                if (r2 <= 1e-4)
                    throw std::runtime_error("That thickness would cross the middle of the face");
                inner = std::min(r, r2);
                outer = std::max(r, r2);
            }
            if (inner <= 1e-4)
                throw std::runtime_error("That thickness would cross the middle of the face");
            gp_Pnt o = c.Location().Translated(axis * v0);
            gp_Ax2 base(o, c.Axis().Direction());
            TopoDS_Shape big = BRepPrimAPI_MakeCylinder(base, outer, len).Shape();
            TopoDS_Shape small = BRepPrimAPI_MakeCylinder(base, inner, len).Shape();
            return take(boolOp(false, big, small, "Thicken"));
        };
        auto simple = [&](double off) -> TopoDS_Shape {
            try {
                BRep_Builder bb;
                TopoDS_Shell shell;
                bb.MakeShell(shell);
                for (const TopoDS_Face& f : world) bb.Add(shell, f);
                BRepOffsetAPI_MakeThickSolid mk;
                mk.MakeThickSolidBySimple(shell, off);
                mk.Build();
                if (!mk.IsDone()) return {};
                return take(mk.Shape());
            } catch (const Standard_Failure&) {
                return {};
            }
        };

        bool allPlanar = true;
        for (const TopoDS_Face& f : world) {
            gp_Dir n;
            gp_Pnt p;
            if (!planarNormal(f, n, p)) allPlanar = false;
        }
        TopoDS_Shape shp;
        if (allPlanar) {
            std::vector<TopoDS_Shape> parts;
            for (const TopoDS_Face& f : world) {
                if (side == 2) {
                    TopoDS_Shape a = prism(f, thickness * 0.5);
                    TopoDS_Shape b = prism(f, -thickness * 0.5);
                    if (a.IsNull() || b.IsNull())
                        throw std::runtime_error("Thicken failed on this face. Try a thinner wall.");
                    parts.push_back(boolOp(true, a, b, "Thicken"));
                } else {
                    TopoDS_Shape a = prism(f, side == 1 ? -thickness : thickness);
                    if (a.IsNull()) throw std::runtime_error("Thicken failed on this face. Try a thinner wall.");
                    parts.push_back(a);
                }
            }
            shp = parts[0];
            for (size_t i = 1; i < parts.size(); ++i) shp = boolOp(true, shp, parts[i], "Thicken");
        } else if (world.size() == 1) {
            shp = tube(world[0], side);
            if (shp.IsNull()) {
                double off = side == 2 ? thickness * 0.5 : (side == 1 ? -thickness : thickness);
                shp = simple(off);
                if (side == 2 && !shp.IsNull()) {
                    TopoDS_Shape back = simple(-thickness * 0.5);
                    if (back.IsNull()) shp.Nullify();
                    else shp = boolOp(true, shp, back, "Thicken");
                }
            }
        } else {
            double off = side == 2 ? thickness * 0.5 : (side == 1 ? -thickness : thickness);
            shp = simple(off);
            if (side == 2 && !shp.IsNull()) {
                TopoDS_Shape back = simple(-thickness * 0.5);
                if (back.IsNull()) shp.Nullify();
                else shp = boolOp(true, shp, back, "Thicken");
            }
        }
        shp = take(shp);
        if (shp.IsNull()) throw std::runtime_error("Thicken failed on this face. Try a thinner wall.");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = shp;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "thicken");
    }
}

std::vector<ShapePtr> splitByPlane(const ShapePtr& shape, const glm::dvec3& p, const glm::dvec3& normal) {
    // Build a plane face comfortably larger than the body and use it as a splitting tool.
    Bnd_Box b;
    BRepBndLib::Add(shape->shape, b);
    Standard_Real x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    double r = 2.0 + 1.5 * std::sqrt(b.SquareExtent());
    glm::dvec3 n = glm::normalize(normal);
    glm::dvec3 ax = std::abs(n.z) < 0.9 ? glm::cross(n, glm::dvec3(0, 0, 1)) : glm::cross(n, glm::dvec3(0, 1, 0));
    ax = glm::normalize(ax);
    glm::dvec3 ay = glm::cross(n, ax);
    try {
        BRepBuilderAPI_MakeWire w;
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p - ax * r - ay * r), pnt(p + ax * r - ay * r)).Edge());
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p + ax * r - ay * r), pnt(p + ax * r + ay * r)).Edge());
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p + ax * r + ay * r), pnt(p - ax * r + ay * r)).Edge());
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p - ax * r + ay * r), pnt(p - ax * r - ay * r)).Edge());
        BRepBuilderAPI_MakeFace mf(gp_Pln(pnt(p), dir(n)), w.Wire());
        TopTools_ListOfShape args, tools;
        args.Append(copyShape(shape->shape));
        tools.Append(mf.Face());
        BRepAlgoAPI_Splitter sp;
        sp.SetArguments(args);
        sp.SetTools(tools);
        sp.Build();
        if (!sp.IsDone() || sp.Shape().IsNull())
            throw std::runtime_error("Split failed on this body");
        std::vector<ShapePtr> out;
        for (TopExp_Explorer ex(sp.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
            auto h = std::make_shared<ShapeHandle>();
            h->shape = ex.Current();
            out.push_back(h);
        }
        if (out.empty()) throw std::runtime_error("The plane does not cut this body");
        return out;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "split");
    }
}

std::vector<ShapePtr> splitByCylinder(const ShapePtr& shape, const glm::dvec3& p, const glm::dvec3& dir, double r) {
    if (!(r > 1e-6)) throw std::runtime_error("The round face has no radius");
    if (glm::length(dir) < 1e-9) throw std::runtime_error("The round face has no axis");
    // The cylinder's side face, from well below the body to well above it along the axis.
    Bnd_Box b;
    BRepBndLib::Add(shape->shape, b);
    Standard_Real x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    glm::dvec3 d = glm::normalize(dir);
    glm::dvec3 mid((x0 + x1) * 0.5, (y0 + y1) * 0.5, (z0 + z1) * 0.5);
    double half = 2.0 + std::sqrt(b.SquareExtent());
    double t = glm::dot(mid - p, d);
    glm::dvec3 base = p + d * (t - half);
    try {
        BRepPrimAPI_MakeCylinder mc(gp_Ax2(pnt(base), gp_Dir(d.x, d.y, d.z)), r, 2.0 * half);
        TopoDS_Face side;
        for (TopExp_Explorer ex(mc.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
            BRepAdaptor_Surface sf(TopoDS::Face(ex.Current()));
            if (sf.GetType() == GeomAbs_Cylinder) { side = TopoDS::Face(ex.Current()); break; }
        }
        if (side.IsNull()) throw std::runtime_error("Could not build the round cutting face");
        TopTools_ListOfShape args, tools;
        args.Append(copyShape(shape->shape));
        tools.Append(side);
        BRepAlgoAPI_Splitter sp;
        sp.SetArguments(args);
        sp.SetTools(tools);
        sp.Build();
        if (!sp.IsDone() || sp.Shape().IsNull()) throw std::runtime_error("Split failed on this body");
        std::vector<ShapePtr> out;
        for (TopExp_Explorer ex(sp.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
            auto h = std::make_shared<ShapeHandle>();
            h->shape = ex.Current();
            out.push_back(h);
        }
        if (out.empty()) throw std::runtime_error("The round face does not cut this body");
        return out;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "split");
    }
}

std::vector<ShapePtr> silhouetteSplit(const ShapePtr& target, const glm::dvec3& view, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("Select the body to split");
    if (glm::length(view) < 0.5) throw std::runtime_error("Bad view direction");
    glm::dvec3 d = glm::normalize(view);
    gp_Trsf tr;
    tr = trsfOfMat(xform);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    if (box.IsVoid()) throw std::runtime_error("Select the body to split");
    Standard_Real x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    glm::dvec3 at((x0 + x1) * 0.5, (y0 + y1) * 0.5, (z0 + z1) * 0.5);
    glm::dvec3 up = std::abs(d.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
    TopoDS_Shape edges;
    try {
        HLRAppli_ReflectLines reflect(body);
        reflect.SetAxes(d.x, d.y, d.z, at.x, at.y, at.z, up.x, up.y, up.z);
        reflect.Perform();
        edges = reflect.GetCompoundOf3dEdges(HLRBRep_OutLine, Standard_True, Standard_True);
        TopExp_Explorer vis(edges, TopAbs_EDGE);
        if (!vis.More()) edges = reflect.GetCompoundOf3dEdges(HLRBRep_OutLine, Standard_False, Standard_True);
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "silhouette");
    }
    std::vector<glm::dvec3> pts;
    for (TopExp_Explorer ex(edges, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        double f = c.FirstParameter(), l = c.LastParameter();
        if (!(l > f)) continue;
        for (int i = 0; i <= 8; ++i) {
            gp_Pnt p = c.Value(f + (l - f) * (i / 8.0));
            pts.push_back({p.X(), p.Y(), p.Z()});
        }
    }
    if (pts.size() < 3) throw std::runtime_error("No silhouette in that direction");
    glm::dvec3 acc(0);
    double umin = 1e300, umax = -1e300;
    for (auto& p : pts) {
        acc += p;
        double u = glm::dot(p, d);
        umin = std::min(umin, u);
        umax = std::max(umax, u);
    }
    if (umax - umin > 1.0) throw std::runtime_error("This silhouette is not flat along that direction");
    glm::dvec3 c = acc / static_cast<double>(pts.size());
    auto world = std::make_shared<ShapeHandle>();
    world->shape = body;
    auto pieces = splitByPlane(world, c, d);
    if (pieces.size() < 2) throw std::runtime_error("The silhouette does not split this body");
    return pieces;
}

ShapePtr splitFaces(const ShapePtr& shape, const glm::dvec3& p, const glm::dvec3& normal, const std::vector<uint32_t>& faces) {
    if (!shape || shape->shape.IsNull()) throw std::runtime_error("Nothing to split");
    if (glm::length(normal) < 1e-9) throw std::runtime_error("The splitting plane has no direction");
    Bnd_Box b;
    BRepBndLib::Add(shape->shape, b);
    double r = 2.0 + 1.5 * std::sqrt(b.SquareExtent());
    glm::dvec3 n = glm::normalize(normal);
    glm::dvec3 ax = std::abs(n.z) < 0.9 ? glm::cross(n, glm::dvec3(0, 0, 1)) : glm::cross(n, glm::dvec3(0, 1, 0));
    ax = glm::normalize(ax);
    glm::dvec3 ay = glm::cross(n, ax);
    try {
        BRepBuilderAPI_MakeWire w;
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p - ax * r - ay * r), pnt(p + ax * r - ay * r)).Edge());
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p + ax * r - ay * r), pnt(p + ax * r + ay * r)).Edge());
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p + ax * r + ay * r), pnt(p - ax * r + ay * r)).Edge());
        w.Add(BRepBuilderAPI_MakeEdge(pnt(p - ax * r + ay * r), pnt(p - ax * r - ay * r)).Edge());
        TopoDS_Face tool = BRepBuilderAPI_MakeFace(gp_Pln(pnt(p), dir(n)), w.Wire()).Face();
        TopoDS_Shape base = copyShape(shape->shape);
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(base, TopAbs_FACE, fmap);
        std::vector<std::pair<TopoDS_Edge, TopoDS_Face>> cuts;
        auto take = [&](int index) {
            if (index < 0 || index >= fmap.Extent()) return;
            TopoDS_Face face = TopoDS::Face(fmap(index + 1));
            BRepAlgoAPI_Section sec(face, tool, Standard_False);
            sec.ComputePCurveOn1(Standard_True);
            sec.Approximation(Standard_True);
            sec.Build();
            if (!sec.IsDone()) return;
            for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next())
                cuts.push_back({TopoDS::Edge(ex.Current()), face});
        };
        if (faces.empty()) {
            for (int i = 0; i < fmap.Extent(); ++i) take(i);
        } else {
            for (uint32_t fi : faces) take((int)fi);
        }
        if (cuts.empty()) throw std::runtime_error("That plane does not cross a face");
        auto build = [&](bool checkInterior) {
            BRepFeat_SplitShape spl(base);
            spl.SetCheckInterior(checkInterior ? Standard_True : Standard_False);
            for (auto& c : cuts) spl.Add(c.first, c.second);
            spl.Build();
            return spl.IsDone() && !spl.Shape().IsNull() ? spl.Shape() : TopoDS_Shape();
        };
        TopoDS_Shape out = build(true);
        if (out.IsNull() || countSolids(out) != 1) out = build(false);
        if (out.IsNull() || countSolids(out) != 1)
            throw std::runtime_error("Could not split the faces");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = out;
        if (faceCount(h) <= faceCount(shape))
            throw std::runtime_error("The plane did not divide a face");
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "split face");
    }
}

ShapePtr splitFacesByCurve(const ShapePtr& shape, const std::vector<CurveSeg>& segs, const glm::dvec3& dir, const std::vector<uint32_t>& faces) {
    if (!shape || shape->shape.IsNull()) throw std::runtime_error("Nothing to split");
    if (segs.empty()) throw std::runtime_error("The sketch has no curve");
    if (glm::length(dir) < 1e-9) throw std::runtime_error("The sketch has no direction to project along");
    Bnd_Box b;
    BRepBndLib::Add(shape->shape, b);
    double reach = 2.0;
    if (!b.IsVoid()) {
        Standard_Real x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        reach = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0)) + 2.0;
    }
    glm::dvec3 step = glm::normalize(dir) * reach;
    try {
        std::vector<TopoDS_Shape> walls;
        gp_Vec along(step.x, step.y, step.z);
        gp_Trsf back;
        back.SetTranslation(-along);
        for (const CurveSeg& c : segs) {
            TopoDS_Edge e = TopoDS::Edge(BRepBuilderAPI_Transform(edgeFromSeg(c, c.normal), back, Standard_True).Shape());
            TopoDS_Shape wall = BRepPrimAPI_MakePrism(e, along * 2).Shape();
            if (!wall.IsNull()) walls.push_back(wall);
        }
        if (walls.empty()) throw std::runtime_error("Could not project the sketch curve");
        TopoDS_Shape base = copyShape(shape->shape);
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(base, TopAbs_FACE, fmap);
        std::vector<std::pair<TopoDS_Edge, TopoDS_Face>> cuts;
        auto take = [&](int index) {
            if (index < 0 || index >= fmap.Extent()) return;
            TopoDS_Face face = TopoDS::Face(fmap(index + 1));
            for (const TopoDS_Shape& wall : walls) {
                BRepAlgoAPI_Section sec(face, wall, Standard_False);
                sec.ComputePCurveOn1(Standard_True);
                sec.Approximation(Standard_True);
                sec.Build();
                if (!sec.IsDone()) continue;
                for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next())
                    cuts.push_back({TopoDS::Edge(ex.Current()), face});
            }
        };
        if (faces.empty()) {
            for (int i = 0; i < fmap.Extent(); ++i) take(i);
        } else {
            for (uint32_t fi : faces) take((int)fi);
        }
        if (cuts.empty()) throw std::runtime_error("That curve does not cross a face");
        auto build = [&](bool checkInterior) {
            BRepFeat_SplitShape spl(base);
            spl.SetCheckInterior(checkInterior ? Standard_True : Standard_False);
            for (auto& c : cuts) spl.Add(c.first, c.second);
            spl.Build();
            return spl.IsDone() && !spl.Shape().IsNull() ? spl.Shape() : TopoDS_Shape();
        };
        TopoDS_Shape out = build(true);
        if (out.IsNull() || countSolids(out) != 1) out = build(false);
        if (out.IsNull() || countSolids(out) != 1)
            throw std::runtime_error("Could not split the faces");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = out;
        if (faceCount(h) <= faceCount(shape))
            throw std::runtime_error("The curve did not divide a face");
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "split face");
    }
}

ShapePtr mirrorShape(const ShapePtr& shape, const glm::dvec3& o, const glm::dvec3& n) {
    try {
        gp_Trsf t;
        t.SetMirror(gp_Ax2(pnt(o), dir(n)));
        BRepBuilderAPI_Transform x(shape->shape, t, Standard_True);
        if (x.Shape().IsNull() || countSolids(x.Shape()) == 0)
            throw std::runtime_error("Mirror failed on this body");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = x.Shape();
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "mirror");
    }
}

double shapeDiag(const ShapePtr& shape) {
    try {
        Bnd_Box b;
        BRepBndLib::Add(shape->shape, b);
        return b.IsVoid() ? 0.0 : std::sqrt(b.SquareExtent());
    } catch (...) {
        return 0.0;
    }
}

double solidVolume(const ShapePtr& shape) {
    if (!shape || shape->shape.IsNull()) return 0;
    try {
        GProp_GProps g;
        BRepGProp::VolumeProperties(shape->shape, g, 1e-6);
        return g.Mass();
    } catch (...) {
        return 0;
    }
}

double solidArea(const ShapePtr& shape) {
    if (!shape || shape->shape.IsNull()) return 0;
    try {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(shape->shape, g, 1e-6);
        return g.Mass();
    } catch (...) {
        return 0;
    }
}

const char* faceKind(const ShapePtr& shape, uint32_t face, double& radius) {
    radius = 0;
    if (!shape || shape->shape.IsNull()) return "missing";
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) return "missing";
        const TopoDS_Face& f = TopoDS::Face(fmap((int)face + 1));
        BRepAdaptor_Surface s(f);
        switch (s.GetType()) {
        case GeomAbs_Plane: return "plane";
        case GeomAbs_Cylinder: radius = s.Cylinder().Radius(); return "cylinder";
        case GeomAbs_Cone: radius = s.Cone().RefRadius(); return "cone";
        case GeomAbs_Sphere: radius = s.Sphere().Radius(); return "sphere";
        case GeomAbs_Torus: radius = s.Torus().MinorRadius(); return "torus";
        default: return "other";
        }
    } catch (...) {
        return "other";
    }
}

// ---------------------------------------------------------------- coil, pipe, draft

namespace {

TopoDS_Shape pipeAlong(const TopoDS_Wire& spine, double radius) {
    if (radius <= 1e-6) throw std::runtime_error("The pipe diameter must be above 0");
    TopExp_Explorer ex(spine, TopAbs_EDGE);
    if (!ex.More()) throw std::runtime_error("The pipe path is empty");
    Standard_Real f = 0, l = 0;
    Handle(Geom_Curve) c = BRep_Tool::Curve(TopoDS::Edge(ex.Current()), f, l);
    if (c.IsNull()) throw std::runtime_error("The pipe path has no curve");
    gp_Pnt p;
    gp_Vec tan;
    c->D1(ex.Current().Orientation() == TopAbs_REVERSED ? l : f, p, tan);
    if (ex.Current().Orientation() == TopAbs_REVERSED) tan.Reverse();
    if (tan.Magnitude() < 1e-9) throw std::runtime_error("The pipe path has no direction");
    gp_Circ circ(gp_Ax2(p, gp_Dir(tan)), radius);
    TopoDS_Wire loop = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circ).Edge()).Wire();
    TopoDS_Face face = BRepBuilderAPI_MakeFace(loop).Face();
    BRepOffsetAPI_MakePipe pipe(spine, face);
    pipe.Build();
    if (!pipe.IsDone() || pipe.Shape().IsNull() || countSolids(pipe.Shape()) == 0)
        throw std::runtime_error("Pipe failed. The path may cross itself, or the diameter is too big for the bends.");
    return pipe.Shape();
}

} // namespace

ShapePtr makeCoil(const glm::dvec3& origin, const glm::dvec3& axisIn, double diameter, double height,
                  double turns, double sectionDiameter, bool leftHand) {
    if (!(diameter > sectionDiameter) || !(sectionDiameter > 0) || !(std::abs(height) > 0) || !(std::abs(turns) > 0))
        throw std::runtime_error("Coil needs a centerline diameter larger than the wire, and a height and turn count above 0");
    glm::dvec3 axis = glm::length(axisIn) < 1e-12 ? glm::dvec3(0, 0, 1) : glm::normalize(axisIn);
    gp_Ax2 frame(pnt(origin), dir(axis));
    gp_Dir x = frame.XDirection(), y = frame.YDirection();
    double R = diameter * 0.5;
    double sign = leftHand ? -1.0 : 1.0;
    int n = std::max(24, (int)std::ceil(std::abs(turns) * 16));
    Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, n + 1);
    auto at = [&](double t) {
        double ang = sign * t * turns * 2.0 * kPiK;
        gp_Pnt p = pnt(origin);
        p.Translate(gp_Vec(x) * (R * std::cos(ang)) + gp_Vec(y) * (R * std::sin(ang)) + gp_Vec(dir(axis)) * (height * t));
        return p;
    };
    for (int i = 0; i <= n; ++i) pts->SetValue(i + 1, at((double)i / n));
    try {
        GeomAPI_Interpolate interp(pts, Standard_False, 1e-6);
        // Chord-length parameters, so the tangent must be unit length and unscaled.
        // Scaling uses a local fit and blows the far end out into a loop.
        auto vel = [&](double t) {
            double ang = sign * t * turns * 2.0 * kPiK;
            double dang = sign * turns * 2.0 * kPiK;
            return gp_Vec(x) * (-R * std::sin(ang) * dang) + gp_Vec(y) * (R * std::cos(ang) * dang) + gp_Vec(dir(axis)) * height;
        };
        gp_Vec t0 = vel(0), t1 = vel(1);
        t0.Normalize();
        t1.Normalize();
        interp.Load(t0, t1, Standard_False);
        interp.Perform();
        if (!interp.IsDone()) throw std::runtime_error("Could not build the coil centerline");
        TopoDS_Wire spine = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(interp.Curve()).Edge()).Wire();
        auto h = std::make_shared<ShapeHandle>();
        h->shape = pipeAlong(spine, sectionDiameter * 0.5);
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "coil");
    }
}

ShapePtr makePipe(const std::vector<CurveSeg>& path, double outerR, double innerR) {
    if (path.empty()) throw std::runtime_error("The pipe path is empty");
    if (!(outerR > 0)) throw std::runtime_error("The pipe diameter must be above 0");
    if (innerR < 0 || innerR >= outerR) throw std::runtime_error("The inner diameter must be smaller than the outside diameter");
    try {
        glm::dvec3 n = path[0].normal;
        if (glm::length(n) < 0.5) n = {0, 0, 1};
        TopoDS_Wire spine = wireFromSegs(path, n);
        TopoDS_Shape outer = pipeAlong(spine, outerR);
        if (innerR > 1e-6) outer = boolOp(false, outer, pipeAlong(spine, innerR), "Pipe");
        if (countSolids(outer) == 0) throw std::runtime_error("The pipe produced no solid");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = outer;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "pipe");
    }
}

ShapePtr draftFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces, double angleDeg, const glm::dvec3& pullIn) {
    if (faces.empty()) throw std::runtime_error("Select the faces to draft");
    if (!(std::abs(angleDeg) > 1e-6) || std::abs(angleDeg) >= 89.0)
        throw std::runtime_error("Draft angle must be between 0 and 89 degrees");
    if (glm::length(pullIn) < 1e-9) throw std::runtime_error("Draft needs a pull direction");
    glm::dvec3 pull = glm::normalize(pullIn);
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        for (uint32_t fi : faces)
            if ((int)fi >= fmap.Extent())
                throw std::runtime_error("The body changed and the stored faces no longer match. Edit the draft and reselect them.");
        Bnd_Box box;
        BRepBndLib::Add(shape->shape, box);
        Standard_Real x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        glm::dvec3 lo(x0, y0, z0), hi(x1, y1, z1);
        glm::dvec3 base(0);
        double best = 1e300;
        for (int i = 0; i < 8; ++i) {
            glm::dvec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
            double d = glm::dot(c, pull);
            if (d < best) { best = d; base = c; }
        }
        gp_Pln neutral(pnt(base), dir(pull));
        BRepOffsetAPI_DraftAngle mk(shape->shape);
        int added = 0;
        for (uint32_t fi : faces) {
            TopoDS_Face f = TopoDS::Face(fmap((int)fi + 1));
            gp_Dir n;
            gp_Pnt p;
            if (!planarNormal(f, n, p))
                throw std::runtime_error("Draft works on flat faces. A round face needs a different tool.");
            if (std::abs(gp_Vec(n).Dot(gp_Vec(dir(pull)))) > 0.9)
                throw std::runtime_error("That face is the top or the bottom along the pull direction. Draft the side faces.");
            mk.Add(f, dir(pull), angleDeg * kPiK / 180.0, neutral, Standard_True);
            if (!mk.AddDone())
                throw std::runtime_error("Draft failed on that face. Try a smaller angle.");
            ++added;
        }
        if (added == 0) throw std::runtime_error("No faces were drafted");
        mk.Build();
        if (!mk.IsDone() || mk.Shape().IsNull() || countSolids(mk.Shape()) == 0)
            throw std::runtime_error("Draft failed on this body");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = mk.Shape();
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "draft");
    }
}

// ---------------------------------------------------------------- M5: sweep and loft

namespace {

gp_Vec spinVec(const gp_Vec& v, const gp_Vec& axis, double ang) {
    if (axis.Magnitude() < 1e-12 || std::abs(ang) < 1e-12) return v;
    gp_Trsf tr;
    tr.SetRotation(gp_Ax1(gp::Origin(), gp_Dir(axis)), ang);
    return v.Transformed(tr);
}

gp_Vec carryNormal(gp_Vec nrm, const gp_Vec& fromT, const gp_Vec& toT) {
    gp_Vec axis = fromT.Crossed(toT);
    double s = axis.Magnitude();
    double c = fromT.Dot(toT);
    if (s > 1e-8) nrm = spinVec(nrm, axis, std::atan2(s, c));
    else if (c < 0) {
        gp_Vec side = std::abs(fromT.Z()) < 0.9 ? gp_Vec(0, 0, 1) : gp_Vec(1, 0, 0);
        nrm = spinVec(nrm, fromT.Crossed(side), kPiK);
    }
    nrm -= toT * nrm.Dot(toT);
    if (nrm.Magnitude() < 1e-9) {
        gp_Vec side = std::abs(toT.Z()) < 0.9 ? gp_Vec(0, 0, 1) : gp_Vec(1, 0, 0);
        nrm = toT.Crossed(side);
    }
    nrm.Normalize();
    return nrm;
}

// A curve beside the path. Its direction from the path turns by twistRad, so the
// swept profile follows it.
TopoDS_Wire twistGuide(const TopoDS_Wire& spine, double twistRad) {
    BRepAdaptor_CompCurve cc(spine, Standard_True);
    double f = cc.FirstParameter(), l = cc.LastParameter();
    double span = l - f;
    if (!(span > 1e-6)) throw std::runtime_error("The path is too short to twist");
    int n = std::clamp((int)std::ceil(std::abs(twistRad) / (kPiK / 36.0) + span / 5.0), 12, 80);
    gp_Pnt p0;
    gp_Vec t0;
    cc.D1(f, p0, t0);
    if (t0.Magnitude() < 1e-9) throw std::runtime_error("The path has no direction");
    t0.Normalize();
    gp_Vec side = std::abs(t0.Z()) < 0.9 ? gp_Vec(0, 0, 1) : gp_Vec(1, 0, 0);
    gp_Vec nrm = t0.Crossed(side);
    nrm.Normalize();
    gp_Vec prevT = t0;
    Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, n + 1);
    for (int i = 0; i <= n; ++i) {
        double u = f + span * i / n;
        gp_Pnt p;
        gp_Vec t;
        cc.D1(u, p, t);
        if (t.Magnitude() < 1e-9) t = prevT;
        else t.Normalize();
        nrm = carryNormal(nrm, prevT, t);
        gp_Vec spun = spinVec(nrm, t, twistRad * i / n);
        spun.Normalize();
        pts->SetValue(i + 1, p.Translated(spun));
        prevT = t;
    }
    GeomAPI_Interpolate interp(pts, Standard_False, 1e-6);
    interp.Perform();
    if (!interp.IsDone()) throw std::runtime_error("Could not build the twist along the path");
    return BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(interp.Curve()).Edge()).Wire();
}

TopoDS_Shape sweepTwisted(const TopoDS_Wire& spine, const TopoDS_Wire& profile, const TopoDS_Wire& guide, bool scale) {
    BRepOffsetAPI_MakePipeShell ps(spine);
    ps.SetMode(guide, Standard_True, scale ? BRepFill_ContactOnBorder : BRepFill_NoContact);
    ps.Add(profile, Standard_False, Standard_True);
    ps.Build();
    if (!ps.IsDone() || !ps.MakeSolid() || ps.Shape().IsNull() || countSolids(ps.Shape()) == 0)
        throw std::runtime_error(scale
            ? "Sweep failed. The profile must touch the start of the path, and the guide must meet the profile."
            : "Sweep failed. The profile must touch the start of the path, and the twist may be too sharp.");
    return ps.Shape();
}

} // namespace

ShapePtr sweepRegion(const RegionLoops& loops, const std::vector<CurveSeg>& pathSegs, double twistDeg,
                     const std::vector<CurveSeg>& guideSegs) {
    if (pathSegs.empty()) throw std::runtime_error("The path sketch has no geometry");
    if (loops.empty() || loops[0].empty()) throw std::runtime_error("The profile has empty geometry");
    try {
        glm::dvec3 n = pathSegs[0].normal;
        if (!guideSegs.empty()) {
            TopoDS_Wire spine = wireFromSegs(pathSegs, n);
            glm::dvec3 gn = glm::length(guideSegs[0].normal) < 1e-9 ? n : guideSegs[0].normal;
            TopoDS_Wire guide = wireFromSegs(guideSegs, gn);
            glm::dvec3 pn = loops[0][0].normal;
            if (glm::length(pn) < 1e-9) pn = n;
            TopoDS_Shape solid = sweepTwisted(spine, wireFromSegs(loops[0], pn), guide, true);
            for (size_t i = 1; i < loops.size(); ++i) {
                glm::dvec3 hn = loops[i].empty() || glm::length(loops[i][0].normal) < 1e-9 ? pn : loops[i][0].normal;
                solid = boolOp(false, solid, sweepTwisted(spine, wireFromSegs(loops[i], hn), guide, true), "sweep");
            }
            if (countSolids(solid) == 0) throw std::runtime_error("Sweep produced no solid");
            auto h = std::make_shared<ShapeHandle>();
            h->shape = solid;
            return h;
        }
        if (std::abs(twistDeg) < 1e-6) {
            ShapePtr prof = buildFace(loops);
            BRepOffsetAPI_MakePipe pipe(wireFromSegs(pathSegs, n), prof->shape);
            pipe.Build();
            if (!pipe.IsDone() || pipe.Shape().IsNull() || countSolids(pipe.Shape()) == 0)
                throw std::runtime_error("Sweep failed. The profile must touch the start of the path.");
            auto h = std::make_shared<ShapeHandle>();
            h->shape = pipe.Shape();
            return h;
        }
        TopoDS_Wire spine = wireFromSegs(pathSegs, n);
        TopoDS_Wire guide = twistGuide(spine, twistDeg * kPiK / 180.0);
        TopoDS_Shape solid = sweepTwisted(spine, wireFromSegs(loops[0], n), guide, false);
        for (size_t i = 1; i < loops.size(); ++i)
            solid = boolOp(false, solid, sweepTwisted(spine, wireFromSegs(loops[i], n), guide, false), "sweep");
        if (countSolids(solid) == 0) throw std::runtime_error("Sweep produced no solid");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = solid;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "sweep");
    }
}

namespace {

void sampleSeg(const CurveSeg& c, int n, std::vector<glm::dvec3>& pts) {
    if (n < 1) n = 1;
    if (c.kind == CurveSeg::Kind::Line) {
        for (int i = 0; i <= n; ++i) pts.push_back(c.a + (c.b - c.a) * ((double)i / n));
        return;
    }
    glm::dvec3 nrm = glm::length(c.normal) < 1e-9 ? glm::dvec3(0, 0, 1) : glm::normalize(c.normal);
    glm::dvec3 x = c.a - c.center;
    if (glm::length(x) < 1e-8) {
        glm::dvec3 up = std::abs(nrm.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
        x = glm::cross(nrm, up);
    }
    x = glm::normalize(x);
    glm::dvec3 y = glm::normalize(glm::cross(nrm, x));
    if (c.back) y = -y;
    double sweep = kTwoPi;
    if (c.kind == CurveSeg::Kind::Arc) {
        sweep = angleAround(c.b - c.center, nrm, c.a - c.center);
        if (sweep < 0) sweep += kTwoPi;
        if (sweep < 1e-6) sweep = kTwoPi;
    }
    for (int i = 0; i <= n; ++i) {
        double a = sweep * i / n;
        pts.push_back(c.center + (x * std::cos(a) + y * std::sin(a)) * c.radius);
    }
}

glm::dvec3 loopCenter(const std::vector<CurveSeg>& loop) {
    if (loop.size() == 1 && loop[0].kind != CurveSeg::Kind::Line) return loop[0].center;
    std::vector<glm::dvec3> pts;
    for (auto& c : loop) sampleSeg(c, 8, pts);
    glm::dvec3 c(0);
    if (pts.empty()) return c;
    for (auto& p : pts) c += p;
    return c / (double)pts.size();
}

double loopSupport(const std::vector<CurveSeg>& loop, const glm::dvec3& origin, const glm::dvec3& dir) {
    std::vector<glm::dvec3> pts;
    for (auto& c : loop) sampleSeg(c, 16, pts);
    double m = 0;
    for (auto& p : pts) m = std::max(m, glm::dot(p - origin, dir));
    return m;
}

} // namespace

namespace {
ShapePtr loftOuter(const std::vector<RegionLoops>& sections, const std::vector<std::vector<CurveSeg>>& rails,
                   bool hasPoint, const glm::dvec3& point, bool pointAtStart, const std::vector<CurveSeg>& centerline);

// A loop with its pieces joined back up: arcs one after another on the same circle become one arc
// (or the whole circle), straight pieces in line become one line. A face's edge is often cut into
// many pieces (where spokes or ports once met it); lofted as they are, every piece became its own
// strip of side face, and pieces that differ from loop to loop twisted the strips.
std::vector<CurveSeg> joinLoop(const std::vector<CurveSeg>& in) {
    if (in.size() < 2) return in;
    auto startOf = [](const CurveSeg& c) { return c.back ? c.b : c.a; };
    auto endOf = [](const CurveSeg& c) { return c.back ? c.a : c.b; };
    auto sameCircle = [](const CurveSeg& x, const CurveSeg& y) {
        if (x.kind != CurveSeg::Kind::Arc || y.kind != CurveSeg::Kind::Arc || x.back != y.back) return false;
        glm::dvec3 nx = glm::normalize(x.normal), ny = glm::normalize(y.normal);
        return glm::length(x.center - y.center) < 1e-5 && std::abs(x.radius - y.radius) < 1e-5 && glm::dot(nx, ny) > 1 - 1e-9;
    };
    auto inLine = [&](const CurveSeg& x, const CurveSeg& y) {
        if (x.kind != CurveSeg::Kind::Line || y.kind != CurveSeg::Kind::Line) return false;
        glm::dvec3 dx = endOf(x) - startOf(x), dy = endOf(y) - startOf(y);
        double lx = glm::length(dx), ly = glm::length(dy);
        if (lx < 1e-12 || ly < 1e-12) return false;
        return glm::length(glm::cross(dx / lx, dy / ly)) < 1e-9 && glm::dot(dx, dy) > 0;
    };
    auto joinable = [&](const CurveSeg& x, const CurveSeg& y) { return sameCircle(x, y) || inLine(x, y); };
    // Start where two pieces cannot join, so a run is never split by the loop's own start.
    size_t n = in.size(), start = n;
    for (size_t i = 0; i < n; ++i)
        if (!joinable(in[(i + n - 1) % n], in[i])) { start = i; break; }
    if (start == n) {
        // The whole loop is one circle (or a degenerate line loop: leave that alone).
        if (in[0].kind != CurveSeg::Kind::Arc) return in;
        CurveSeg c = in[0];
        c.kind = CurveSeg::Kind::Circle;
        c.a = startOf(in[0]);
        return {c};
    }
    std::vector<CurveSeg> out;
    for (size_t k = 0; k < n; ++k) {
        const CurveSeg& cur = in[(start + k) % n];
        if (!out.empty() && joinable(out.back(), cur)) {
            CurveSeg& m = out.back();
            if (m.kind == CurveSeg::Kind::Line) {
                if (m.back) m.a = endOf(cur); else m.b = endOf(cur);
            } else if (!m.back) {
                m.b = cur.b;   // counterclockwise a -> b, the next piece carries on from b
            } else {
                m.a = cur.a;   // run backwards: the next piece sits before this one around the circle
            }
            continue;
        }
        out.push_back(cur);
    }
    return out;
}

// A ring (or any profile with holes) lofted to the next: the outer and the hole walls are lofted as
// surfaces and closed with the profiles' own flat faces, then sewn into one solid. No booleans, so
// no film is left where a hole's end lay flat on the solid's end.
TopoDS_Shape loftWithHoles(const std::vector<RegionLoops>& sections, const std::vector<std::vector<size_t>>& chain) {
    size_t nh = chain.size();
    std::vector<TopoDS_Wire> ow;
    std::vector<std::vector<TopoDS_Wire>> hw(nh);
    for (size_t i = 0; i < sections.size(); ++i) {
        ow.push_back(wireFromSegs(sections[i][0], sections[i][0][0].normal));
        for (size_t k = 0; k < nh; ++k) {
            const auto& loop = sections[i][chain[k][i]];
            hw[k].push_back(wireFromSegs(loop, loop[0].normal));
        }
    }
    auto walls = [](const std::vector<TopoDS_Wire>& ws) {
        BRepOffsetAPI_ThruSections ts(Standard_False, Standard_False);
        ts.CheckCompatibility(Standard_True);
        for (auto& w : ws) ts.AddWire(w);
        ts.Build();
        if (!ts.IsDone() || ts.Shape().IsNull()) throw std::runtime_error("Loft failed on these profiles");
        return ts.Shape();
    };
    auto cap = [&](size_t i) {
        BRepBuilderAPI_MakeFace mf(ow[i], Standard_True);
        if (!mf.IsDone()) throw std::runtime_error("A loft profile is not flat");
        for (size_t k = 0; k < nh; ++k) mf.Add(hw[k][i]);
        TopoDS_Face f = mf.Face();
        Handle(ShapeFix_Face) fx = new ShapeFix_Face(f);
        fx->FixOrientationMode() = 1;
        fx->Perform();
        return fx->Face();
    };
    BRepBuilderAPI_Sewing sew(1e-5);
    sew.Add(walls(ow));
    for (size_t k = 0; k < nh; ++k) sew.Add(walls(hw[k]));
    sew.Add(cap(0));
    sew.Add(cap(sections.size() - 1));
    sew.Perform();
    TopoDS_Shape sewn = sew.SewedShape();
    TopExp_Explorer sx(sewn, TopAbs_SHELL);
    if (!sx.More()) return {};
    TopoDS_Shell shell = TopoDS::Shell(sx.Current());
    sx.Next();
    if (sx.More()) return {};   // more than one shell: it did not close up
    BRepBuilderAPI_MakeSolid ms(shell);
    if (!ms.IsDone()) return {};
    Handle(ShapeFix_Solid) fs = new ShapeFix_Solid(ms.Solid());
    fs->Perform();
    TopoDS_Shape sol = fs->Solid();
    TopExp_Explorer one(sol, TopAbs_SOLID);
    if (!one.More()) return {};
    TopoDS_Solid solid = TopoDS::Solid(one.Current());
    BRepLib::OrientClosedSolid(solid);
    GProp_GProps g;
    BRepGProp::VolumeProperties(solid, g);
    if (!(g.Mass() > 1e-9) || !BRepCheck_Analyzer(solid).IsValid()) return {};
    return solid;
}
}

// Holes: a ring lofted to a ring stays hollow. The outer loops make the solid and each hole is
// lofted on its own through the matching holes and taken out of it.
ShapePtr loftRegions(const std::vector<RegionLoops>& sectionsIn, const std::vector<std::vector<CurveSeg>>& rails,
                     bool hasPoint, const glm::dvec3& point, bool pointAtStart,
                     const std::vector<CurveSeg>& centerline) {
    // Each loop with its split pieces joined back up first.
    std::vector<RegionLoops> joined;
    for (auto& sec : sectionsIn) {
        RegionLoops r;
        for (auto& loop : sec) r.push_back(joinLoop(loop));
        joined.push_back(std::move(r));
    }
    const std::vector<RegionLoops>& sections = joined;
    bool holes = false;
    std::vector<RegionLoops> outers;
    for (auto& sec : sections) {
        if (sec.size() > 1) holes = true;
        outers.push_back(sec.empty() ? RegionLoops{} : RegionLoops{sec[0]});
    }
    if (!holes) return loftOuter(outers, rails, hasPoint, point, pointAtStart, centerline);
    if (hasPoint) throw std::runtime_error("A loft to a point cannot keep a hole: pick profiles without one");
    size_t nh = sections[0].size() - 1;
    for (auto& sec : sections)
        if (sec.size() - 1 != nh)
            throw std::runtime_error("Every profile needs the same number of holes to loft through them (a ring to a ring)");
    // Each hole of the first profile, followed to the nearest hole of the next one.
    std::vector<std::vector<size_t>> chain(nh);
    for (size_t k = 0; k < nh; ++k) chain[k].push_back(k + 1);
    for (size_t i = 1; i < sections.size(); ++i) {
        std::vector<bool> used(nh + 1, false);
        for (size_t k = 0; k < nh; ++k) {
            glm::dvec3 prev = loopCenter(sections[i - 1][chain[k].back()]);
            size_t best = 0;
            double bd = 1e300;
            for (size_t j = 1; j <= nh; ++j) {
                if (used[j]) continue;
                double d = glm::length(loopCenter(sections[i][j]) - prev);
                if (d < bd) { bd = d; best = j; }
            }
            if (!best) throw std::runtime_error("The holes of the profiles could not be matched up");
            used[best] = true;
            chain[k].push_back(best);
        }
    }
    if (rails.empty() && centerline.empty()) {
        try {
            TopoDS_Shape solid = loftWithHoles(sections, chain);
            if (!solid.IsNull()) {
                auto h = std::make_shared<ShapeHandle>();
                h->shape = unify(solid);
                return h;
            }
        } catch (const Standard_Failure&) {
        } catch (const std::runtime_error&) {
        }
    }
    // With rails or a centerline (or when sewing did not close): the solid, holes cut out of it.
    ShapePtr outer = loftOuter(outers, rails, hasPoint, point, pointAtStart, centerline);
    TopoDS_Shape res = outer->shape;
    for (size_t k = 0; k < nh; ++k) {
        std::vector<RegionLoops> hs;
        for (size_t i = 0; i < sections.size(); ++i) hs.push_back(RegionLoops{sections[i][chain[k][i]]});
        ShapePtr hole = loftOuter(hs, {}, false, point, pointAtStart, centerline);
        try {
            BRepAlgoAPI_Cut ct(res, hole->shape);
            if (!ct.IsDone() || ct.HasErrors() || ct.Shape().IsNull()) throw std::runtime_error("The loft's hole could not be cut");
            res = ct.Shape();
        } catch (const Standard_Failure& e) {
            rethrowOcct(e, "loft hole");
        }
    }
    auto h = std::make_shared<ShapeHandle>();
    h->shape = unify(res);
    return h;
}

namespace {
ShapePtr loftOuter(const std::vector<RegionLoops>& sections, const std::vector<std::vector<CurveSeg>>& rails,
                   bool hasPoint, const glm::dvec3& point, bool pointAtStart,
                   const std::vector<CurveSeg>& centerline) {
    if (hasPoint && !rails.empty()) throw std::runtime_error("A point end does not take rails");
    if (hasPoint && !centerline.empty()) throw std::runtime_error("A point end does not take a centerline");
    if (!centerline.empty() && !rails.empty()) throw std::runtime_error("A centerline does not take rails");
    if (hasPoint) {
        if (sections.empty()) throw std::runtime_error("Loft to a point needs a profile");
    } else if (sections.size() < 2) {
        throw std::runtime_error("Loft needs at least two profiles");
    }
    if (hasPoint) {
        const RegionLoops& end = pointAtStart ? sections.front() : sections.back();
        if (end.empty() || end[0].empty()) throw std::runtime_error("A loft profile is empty");
        glm::dvec3 n = end[0][0].normal;
        if (glm::length(n) > 1e-9 && std::abs(glm::dot(point - end[0][0].a, glm::normalize(n))) < 0.05)
            throw std::runtime_error("The point sits on the profile");
    }
    try {
        // A point end is ruled so the sides run straight to the tip. Two profiles stay smooth.
        BRepOffsetAPI_ThruSections ts(Standard_True, hasPoint ? Standard_True : Standard_False);
        ts.CheckCompatibility(hasPoint ? Standard_False : Standard_True);
        auto addPoint = [&] {
            ts.AddVertex(BRepBuilderAPI_MakeVertex(gp_Pnt(point.x, point.y, point.z)).Vertex());
        };
        if (!centerline.empty()) {
            std::vector<TopoDS_Wire> wires;
            std::vector<glm::dvec3> centers;
            for (auto& sec : sections) {
                if (sec.empty() || sec[0].empty()) throw std::runtime_error("A loft profile is empty");
                wires.push_back(wireFromSegs(sec[0], sec[0][0].normal));
                centers.push_back(loopCenter(sec[0]));
            }
            std::vector<glm::dvec3> spine;
            bool firstSeg = true;
            for (auto& seg : centerline) {
                size_t before = spine.size();
                sampleSeg(seg, seg.kind == CurveSeg::Kind::Line ? 8 : 32, spine);
                if (!firstSeg && spine.size() > before) spine.erase(spine.begin() + (ptrdiff_t)before);
                firstSeg = false;
            }
            if (spine.size() < 2) throw std::runtime_error("The centerline has no curve");
            auto lengthOf = [&](const std::vector<glm::dvec3>& pts) {
                std::vector<double> acc(pts.size(), 0);
                for (size_t i = 1; i < pts.size(); ++i) acc[i] = acc[i - 1] + glm::length(pts[i] - pts[i - 1]);
                return acc;
            };
            std::vector<double> acc = lengthOf(spine);
            double total = acc.back();
            if (total < 1e-6) throw std::runtime_error("The centerline has no curve");
            auto closestU = [&](const glm::dvec3& p, double& dist) {
                dist = 1e300;
                double u = 0;
                for (size_t i = 1; i < spine.size(); ++i) {
                    glm::dvec3 d = spine[i] - spine[i - 1];
                    double L2 = glm::dot(d, d);
                    double t = L2 < 1e-18 ? 0 : std::clamp(glm::dot(p - spine[i - 1], d) / L2, 0.0, 1.0);
                    glm::dvec3 q = spine[i - 1] + d * t;
                    double dd = glm::length(q - p);
                    if (dd < dist) { dist = dd; u = (acc[i - 1] + glm::length(q - spine[i - 1])) / total; }
                }
                return u;
            };
            std::vector<double> pu(centers.size());
            for (size_t i = 0; i < centers.size(); ++i) {
                double dist = 0;
                pu[i] = closestU(centers[i], dist);
                if (dist > 1.0) throw std::runtime_error("The centerline has to pass through the middle of the profiles");
            }
            if (pu.front() > pu.back()) {
                std::reverse(spine.begin(), spine.end());
                acc = lengthOf(spine);
                total = acc.back();
                for (size_t i = 0; i < centers.size(); ++i) {
                    double dist = 0;
                    pu[i] = closestU(centers[i], dist);
                }
            }
            for (size_t i = 1; i < pu.size(); ++i)
                if (pu[i] + 1e-3 < pu[i - 1]) throw std::runtime_error("The centerline visits the profiles out of order");
            auto pointAt = [&](double u) {
                double s = std::clamp(u, 0.0, 1.0) * total;
                for (size_t i = 1; i < spine.size(); ++i) {
                    if (acc[i] + 1e-9 < s) continue;
                    double seg = acc[i] - acc[i - 1];
                    double t = seg < 1e-12 ? 0 : (s - acc[i - 1]) / seg;
                    return spine[i - 1] + (spine[i] - spine[i - 1]) * t;
                }
                return spine.back();
            };
            auto meanR = [&](const std::vector<CurveSeg>& loop, const glm::dvec3& c) {
                if (loop.size() == 1 && loop[0].kind != CurveSeg::Kind::Line) return loop[0].radius;
                std::vector<glm::dvec3> pts;
                for (auto& seg : loop) sampleSeg(seg, 8, pts);
                if (pts.empty()) return 1.0;
                double s = 0;
                for (auto& p : pts) s += glm::length(p - c);
                return std::max(1e-6, s / (double)pts.size());
            };
            const int N = 8;
            for (int i = 0; i <= N; ++i) {
                double u = (double)i / N;
                if (i == 0) { ts.AddWire(wires.front()); continue; }
                if (i == N) { ts.AddWire(wires.back()); continue; }
                size_t k = 0;
                while (k + 1 < pu.size() && pu[k + 1] < u) ++k;
                if (k + 1 >= pu.size()) k = pu.size() - 2;
                double span = pu[k + 1] - pu[k];
                double t = span < 1e-6 ? 0 : std::clamp((u - pu[k]) / span, 0.0, 1.0);
                double r0 = meanR(sections[k][0], centers[k]);
                double r1 = meanR(sections[k + 1][0], centers[k + 1]);
                double scale = ((1.0 - t) * r0 + t * r1) / r0;
                glm::dvec3 station = pointAt(u);
                gp_Trsf sc, tr;
                sc.SetScale(gp_Pnt(centers[k].x, centers[k].y, centers[k].z), scale);
                tr.SetTranslation(gp_Vec(station.x - centers[k].x, station.y - centers[k].y, station.z - centers[k].z));
                TopoDS_Shape moved = BRepBuilderAPI_Transform(wires[k], tr * sc, Standard_True).Shape();
                TopExp_Explorer ex(moved, TopAbs_WIRE);
                if (!ex.More()) throw std::runtime_error("Loft could not place a section on the centerline");
                ts.AddWire(TopoDS::Wire(ex.Current()));
            }
        } else if (rails.empty()) {
            if (hasPoint && pointAtStart) addPoint();
            for (auto& sec : sections) {
                if (sec.empty() || sec[0].empty()) throw std::runtime_error("A loft profile is empty");
                ts.AddWire(wireFromSegs(sec[0], sec[0][0].normal));
            }
            if (hasPoint && !pointAtStart) addPoint();
        } else {
            std::vector<TopoDS_Wire> wires;
            std::vector<glm::dvec3> centers;
            for (auto& sec : sections) {
                if (sec.empty() || sec[0].empty()) throw std::runtime_error("A loft profile is empty");
                wires.push_back(wireFromSegs(sec[0], sec[0][0].normal));
                centers.push_back(loopCenter(sec[0]));
            }
            glm::dvec3 c0 = centers.front(), delta = centers.back() - c0;
            double axisLen = glm::length(delta);
            if (axisLen < 1e-6) throw std::runtime_error("The loft profiles sit on top of each other");
            glm::dvec3 ax = delta / axisLen;
            struct Rp { double u; glm::dvec3 p; };
            std::vector<Rp> railPts;
            for (auto& rail : rails) {
                std::vector<glm::dvec3> pts;
                bool first = true;
                for (auto& seg : rail) {
                    size_t before = pts.size();
                    sampleSeg(seg, seg.kind == CurveSeg::Kind::Line ? 1 : 24, pts);
                    if (!first && pts.size() > before) pts.erase(pts.begin() + (ptrdiff_t)before);
                    first = false;
                }
                for (auto& p : pts) railPts.push_back({glm::dot(p - c0, ax) / axisLen, p});
            }
            if (railPts.size() < 2) throw std::runtime_error("A rail needs a curve");
            std::sort(railPts.begin(), railPts.end(), [](const Rp& a, const Rp& b) { return a.u < b.u; });
            auto railAt = [&](double u) {
                if (u <= railPts.front().u) return railPts.front().p;
                if (u >= railPts.back().u) return railPts.back().p;
                for (size_t i = 1; i < railPts.size(); ++i) {
                    if (railPts[i].u + 1e-9 < u) continue;
                    double d = railPts[i].u - railPts[i - 1].u;
                    double t = d < 1e-9 ? 0 : (u - railPts[i - 1].u) / d;
                    return railPts[i - 1].p + (railPts[i].p - railPts[i - 1].p) * t;
                }
                return railPts.back().p;
            };
            const int N = 8;
            size_t realNext = 1;
            for (int i = 0; i <= N; ++i) {
                double u = (double)i / N;
                while (realNext + 1 < wires.size()) {
                    double pu = glm::dot(centers[realNext] - c0, ax) / axisLen;
                    if (pu > u + 1e-4) break;
                    ts.AddWire(wires[realNext]);
                    ++realNext;
                }
                if (i == 0) { ts.AddWire(wires.front()); continue; }
                if (i == N) { ts.AddWire(wires.back()); continue; }
                glm::dvec3 station = c0 + delta * u;
                glm::dvec3 radial = railAt(u) - station;
                radial -= ax * glm::dot(radial, ax);
                double dist = glm::length(radial);
                if (dist < 1e-3) throw std::runtime_error("A rail crosses the center of the loft");
                double sup = loopSupport(sections.front()[0], centers.front(), radial / dist);
                if (sup < 1e-4) throw std::runtime_error("The profile has no width toward the rail");
                gp_Trsf sc, tr;
                sc.SetScale(gp_Pnt(centers.front().x, centers.front().y, centers.front().z), dist / sup);
                tr.SetTranslation(gp_Vec(station.x - centers.front().x, station.y - centers.front().y, station.z - centers.front().z));
                TopoDS_Shape moved = BRepBuilderAPI_Transform(wires.front(), tr * sc, Standard_True).Shape();
                TopExp_Explorer ex(moved, TopAbs_WIRE);
                if (!ex.More()) throw std::runtime_error("Loft could not place a section on the rail");
                ts.AddWire(TopoDS::Wire(ex.Current()));
            }
        }
        ts.Build();
        if (!ts.IsDone() || ts.Shape().IsNull() || countSolids(ts.Shape()) == 0)
            throw std::runtime_error(!centerline.empty() ? "Loft failed on this centerline"
                                     : rails.empty() ? "Loft failed on these profiles"
                                                     : "Loft failed on these profiles and rails");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = ts.Shape();
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "loft");
    }
}
} // namespace

// ---------------------------------------------------------------- M7: measure

MeasureResult measureShapes(const ShapePtr& a, const glm::mat4& xa,
                            const ShapePtr& b, const glm::mat4& xb,
                            uint32_t faceA, uint32_t faceB, uint32_t edgeA, uint32_t edgeB) {
    MeasureResult r;
    try {
        gp_Trsf ta, tb;
        ta = trsfOfMat(xa);
        tb = trsfOfMat(xb);
        TopoDS_Shape sa = a->shape.Moved(TopLoc_Location(ta));
        TopoDS_Shape sb = b->shape.Moved(TopLoc_Location(tb));
        auto narrow = [](TopoDS_Shape& s, uint32_t face, uint32_t edge) {
            TopAbs_ShapeEnum kind = edge != 0xffffffffu ? TopAbs_EDGE : TopAbs_FACE;
            uint32_t idx = edge != 0xffffffffu ? edge : face;
            if (idx == 0xffffffffu) return;
            TopTools_IndexedMapOfShape m;
            TopExp::MapShapes(s, kind, m);
            if ((int)idx < m.Extent()) s = m((int)idx + 1);
        };
        narrow(sa, faceA, edgeA);
        narrow(sb, faceB, edgeB);
        BRepExtrema_DistShapeShape d(sa, sb);
        if (!d.IsDone() || d.NbSolution() == 0) return r;
        r.distance = d.Value();
        gp_Pnt p1 = d.PointOnShape1(1), p2 = d.PointOnShape2(1);
        r.pa = {p1.X(), p1.Y(), p1.Z()};
        r.pb = {p2.X(), p2.Y(), p2.Z()};
        r.ok = true;
        return r;
    } catch (...) {
        return r;
    }
}

namespace {

constexpr double kPiM = 3.14159265358979323846;

TopoDS_Shape movedShape(const ShapePtr& s, const glm::mat4& xf) {
    gp_Trsf t;
    t = trsfOfMat(xf);
    return s->shape.Moved(TopLoc_Location(t));
}

bool pickSub(const TopoDS_Shape& s, uint32_t face, uint32_t edge, TopoDS_Shape& out) {
    bool useEdge = edge != 0xffffffffu;
    if (!useEdge && face == 0xffffffffu) return false;
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(s, useEdge ? TopAbs_EDGE : TopAbs_FACE, m);
    uint32_t idx = useEdge ? edge : face;
    if ((int)idx >= m.Extent()) return false;
    out = m((int)idx + 1);
    return true;
}

double edgeLen(const TopoDS_Shape& e) {
    GProp_GProps g;
    BRepGProp::LinearProperties(e, g);
    return g.Mass();
}

double wireLen(const TopoDS_Shape& w) {
    double n = 0;
    for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next()) n += edgeLen(ex.Current());
    return n;
}

bool edgeDir(const TopoDS_Edge& e, gp_Dir& dir) {
    BRepAdaptor_Curve c(e);
    if (c.GetType() == GeomAbs_Line) { dir = c.Line().Direction(); return true; }
    gp_Pnt p;
    gp_Vec v;
    c.D1(0.5 * (c.FirstParameter() + c.LastParameter()), p, v);
    if (v.Magnitude() < 1e-12) return false;
    dir = gp_Dir(v);
    return true;
}

bool faceDir(const TopoDS_Face& f, gp_Dir& dir) {
    BRepAdaptor_Surface s(f);
    if (s.GetType() != GeomAbs_Plane) return false;
    dir = s.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED) dir.Reverse();
    return true;
}

} // namespace

CurveMeasure measureRadius(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, uint32_t edge) {
    CurveMeasure r;
    if (!shape) return r;
    try {
        TopoDS_Shape sub;
        if (!pickSub(movedShape(shape, xf), face, edge, sub)) return r;
        if (sub.ShapeType() == TopAbs_EDGE) {
            BRepAdaptor_Curve c(TopoDS::Edge(sub));
            if (c.GetType() != GeomAbs_Circle) return r;
            r.value = c.Circle().Radius();
            r.ok = r.value > 1e-9;
            return r;
        }
        BRepAdaptor_Surface s(TopoDS::Face(sub));
        if (s.GetType() == GeomAbs_Cylinder) r.value = s.Cylinder().Radius();
        else if (s.GetType() == GeomAbs_Sphere) r.value = s.Sphere().Radius();
        else if (s.GetType() == GeomAbs_Torus) r.value = s.Torus().MinorRadius();
        else return r;
        r.ok = r.value > 1e-9;
        return r;
    } catch (...) {
        return {};
    }
}

CurveMeasure measureArea(const ShapePtr& shape, const glm::mat4& xf, uint32_t face) {
    CurveMeasure r;
    if (!shape || face == 0xffffffffu) return r;
    try {
        TopoDS_Shape sub;
        if (!pickSub(movedShape(shape, xf), face, 0xffffffffu, sub)) return r;
        GProp_GProps g;
        BRepGProp::SurfaceProperties(sub, g);
        r.value = g.Mass();
        r.ok = r.value > 1e-9;
        return r;
    } catch (...) {
        return {};
    }
}

CurveMeasure measureLoop(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, uint32_t edge) {
    CurveMeasure r;
    if (!shape) return r;
    try {
        TopoDS_Shape body = movedShape(shape, xf);
        TopoDS_Shape sub;
        if (!pickSub(body, face, edge, sub)) return r;
        if (sub.ShapeType() == TopAbs_EDGE) {
            TopTools_IndexedDataMapOfShapeListOfShape map;
            TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_WIRE, map);
            if (!map.Contains(sub)) return r;
            const TopTools_ListOfShape& wires = map.FindFromKey(sub);
            if (wires.IsEmpty()) return r;
            r.value = wireLen(wires.First());
        } else {
            r.value = wireLen(BRepTools::OuterWire(TopoDS::Face(sub)));
        }
        r.ok = r.value > 1e-9;
        return r;
    } catch (...) {
        return {};
    }
}

CurveMeasure measureAngle(const ShapePtr& a, const glm::mat4& xa, uint32_t faceA, uint32_t edgeA,
                          const ShapePtr& b, const glm::mat4& xb, uint32_t faceB, uint32_t edgeB) {
    CurveMeasure r;
    if (!a || !b) return r;
    bool ea = edgeA != 0xffffffffu, eb = edgeB != 0xffffffffu;
    if (ea != eb) return r;
    try {
        TopoDS_Shape sa, sb;
        if (!pickSub(movedShape(a, xa), faceA, edgeA, sa)) return r;
        if (!pickSub(movedShape(b, xb), faceB, edgeB, sb)) return r;
        gp_Dir da, db;
        bool ok = ea ? edgeDir(TopoDS::Edge(sa), da) && edgeDir(TopoDS::Edge(sb), db)
                     : faceDir(TopoDS::Face(sa), da) && faceDir(TopoDS::Face(sb), db);
        if (!ok) return r;
        double c = std::min(1.0, std::abs(da.Dot(db)));
        r.value = std::acos(c) * 180.0 / kPiM;
        r.ok = true;
        return r;
    } catch (...) {
        return {};
    }
}

namespace {
// What one pick offers for lining up: a direction (normal, axis, line), a point (center, axis point,
// middle), and whether that point is a real center (round) or just the middle of a flat face.
struct AlignFeat {
    bool ok = false, flat = false, round = false, line = false, hasDir = false;
    glm::dvec3 dir{0}, pt{0};
    std::string what;
};
AlignFeat alignFeat(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, uint32_t edge) {
    AlignFeat r;
    TopoDS_Shape sub;
    if (!shape || !pickSub(movedShape(shape, xf), face, edge, sub)) return r;
    auto P = [](const gp_Pnt& p) { return glm::dvec3(p.X(), p.Y(), p.Z()); };
    auto D = [](const gp_Dir& d) { return glm::dvec3(d.X(), d.Y(), d.Z()); };
    if (sub.ShapeType() == TopAbs_EDGE) {
        BRepAdaptor_Curve c(TopoDS::Edge(sub));
        if (c.GetType() == GeomAbs_Circle) {
            gp_Circ ci = c.Circle();
            r.round = r.hasDir = true;
            r.dir = D(ci.Axis().Direction());
            r.pt = P(ci.Location());
            r.what = "circle edge";
        } else if (c.GetType() == GeomAbs_Line) {
            r.line = r.hasDir = true;
            r.dir = D(c.Line().Direction());
            r.pt = P(c.Value(0.5 * (c.FirstParameter() + c.LastParameter())));
            r.what = "straight edge";
        } else {
            return r;
        }
        r.ok = true;
        return r;
    }
    if (sub.ShapeType() != TopAbs_FACE) return r;
    const TopoDS_Face& f = TopoDS::Face(sub);
    BRepAdaptor_Surface sf(f);
    GProp_GProps g;
    BRepGProp::SurfaceProperties(f, g);
    gp_Pnt mid = g.CentreOfMass();
    auto axisPoint = [&](const gp_Ax1& ax) {   // the axis point level with the face's middle
        gp_Vec v(ax.Location(), mid);
        return P(ax.Location().Translated(gp_Vec(ax.Direction()) * v.Dot(gp_Vec(ax.Direction()))));
    };
    switch (sf.GetType()) {
    case GeomAbs_Plane: {
        gp_Dir n = sf.Plane().Axis().Direction();
        if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
        r.flat = r.hasDir = true;
        r.dir = D(n);
        r.pt = P(mid);
        r.what = "flat face";
        // A round outline (a disc or a ring): its circle's center is the face's true center.
        TopoDS_Wire ow = BRepTools::OuterWire(f);
        bool allCircle = true, any = false;
        gp_Pnt cc;
        for (TopExp_Explorer ex(ow, TopAbs_EDGE); ex.More() && allCircle; ex.Next()) {
            BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
            if (c.GetType() != GeomAbs_Circle) { allCircle = false; break; }
            gp_Pnt l = c.Circle().Location();
            if (any && l.Distance(cc) > 1e-6) allCircle = false;
            cc = l;
            any = true;
        }
        if (allCircle && any) {
            r.round = true;
            r.pt = P(cc);
            r.what = "round flat face";
        }
        break;
    }
    case GeomAbs_Cylinder:
        r.round = r.hasDir = true;
        r.dir = D(sf.Cylinder().Axis().Direction());
        r.pt = axisPoint(sf.Cylinder().Axis());
        r.what = "cylinder face";
        break;
    case GeomAbs_Cone:
        r.round = r.hasDir = true;
        r.dir = D(sf.Cone().Axis().Direction());
        r.pt = axisPoint(sf.Cone().Axis());
        r.what = "cone face";
        break;
    case GeomAbs_Torus:
        r.round = r.hasDir = true;
        r.dir = D(sf.Torus().Axis().Direction());
        r.pt = axisPoint(sf.Torus().Axis());
        r.what = "torus face";
        break;
    case GeomAbs_Sphere:
        r.round = true;
        r.pt = P(sf.Sphere().Location());
        r.what = "sphere face";
        break;
    default:
        return r;
    }
    r.ok = true;
    return r;
}
} // namespace

AlignMeasure measureAlign(const ShapePtr& a, const glm::mat4& xa, uint32_t faceA, uint32_t edgeA,
                          const ShapePtr& b, const glm::mat4& xb, uint32_t faceB, uint32_t edgeB) {
    AlignMeasure m;
    try {
        AlignFeat A = alignFeat(a, xa, faceA, edgeA), B = alignFeat(b, xb, faceB, edgeB);
        if (!A.ok || !B.ok) return m;
        m.ok = true;
        m.whatA = A.what;
        m.whatB = B.what;
        m.ca = A.pt;
        m.cb = B.pt;
        if (A.hasDir && B.hasDir) {
            double c = std::min(1.0, std::abs(glm::dot(glm::normalize(A.dir), glm::normalize(B.dir))));
            m.tilt = glm::degrees(std::acos(c));
            // A flat face against a line or an axis: the line should stand square to it (90 is aligned).
        }
        glm::dvec3 d = B.pt - A.pt;
        bool parallel = m.tilt >= 0 && m.tilt < 1.0;
        if (A.flat && B.flat && parallel) {
            m.hasHeight = true;
            m.height = glm::dot(d, glm::normalize(A.dir));
        }
        // Sideways: across the direction they share (an axis, a normal); for two points, straight.
        glm::dvec3 ax = A.hasDir ? glm::normalize(A.dir) : B.hasDir ? glm::normalize(B.dir) : glm::dvec3(0);
        bool centerish = (A.round || A.flat || A.line) && (B.round || B.flat || B.line);
        if (centerish && (m.tilt < 0 || parallel)) {
            glm::dvec3 lat = glm::length(ax) > 0 ? d - ax * glm::dot(d, ax) : d;
            m.offCenter = glm::length(lat);
        }
    } catch (const Standard_Failure&) {
        m.ok = false;
    }
    return m;
}

ShapePtr pointShape(const glm::dvec3& p) {
    try {
        auto h = std::make_shared<ShapeHandle>();
        h->shape = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z)).Shape();
        return h;
    } catch (...) {
        return nullptr;
    }
}

PickInfo measurePick(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, uint32_t edge) {
    PickInfo r;
    if (!shape) return r;
    try {
        if (shape->shape.ShapeType() == TopAbs_VERTEX) {   // a point: where it is
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(movedShape(shape, xf)));
            r.ok = true;
            r.what = "Point";
            r.hasCenter = true;
            r.center = {p.X(), p.Y(), p.Z()};
            return r;
        }
        TopoDS_Shape sub;
        if (!pickSub(movedShape(shape, xf), face, edge, sub)) return r;
        auto P = [](const gp_Pnt& p) { return glm::dvec3(p.X(), p.Y(), p.Z()); };
        auto D = [](const gp_Dir& d) { return glm::dvec3(d.X(), d.Y(), d.Z()); };
        if (sub.ShapeType() == TopAbs_EDGE) {
            const TopoDS_Edge& e = TopoDS::Edge(sub);
            r.isEdge = true;
            r.length = edgeLen(e);
            BRepAdaptor_Curve c(e);
            r.v0 = P(c.Value(c.FirstParameter()));
            r.v1 = P(c.Value(c.LastParameter()));
            switch (c.GetType()) {
            case GeomAbs_Line:
                r.what = "Line edge";
                r.hasDir = true;
                r.dir = D(c.Line().Direction());
                break;
            case GeomAbs_Circle: {
                gp_Circ ci = c.Circle();
                r.what = std::abs(c.LastParameter() - c.FirstParameter()) > 2 * kPiM - 1e-6 ? "Circle edge" : "Arc edge";
                r.radius = ci.Radius();
                r.hasCenter = true;
                r.center = P(ci.Location());
                r.axis = D(ci.Axis().Direction());
                break;
            }
            case GeomAbs_Ellipse: r.what = "Ellipse edge"; break;
            default: r.what = "Curved edge"; break;
            }
            r.ok = true;
            return r;
        }
        const TopoDS_Face& f = TopoDS::Face(sub);
        r.isFace = true;
        GProp_GProps g;
        BRepGProp::SurfaceProperties(f, g);
        r.area = g.Mass();
        BRepAdaptor_Surface s(f);
        switch (s.GetType()) {
        case GeomAbs_Plane: {
            r.what = "Flat face";
            gp_Dir n = s.Plane().Axis().Direction();
            if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
            r.hasDir = true;
            r.dir = D(n);
            break;
        }
        case GeomAbs_Cylinder:
            r.what = "Cylinder face";
            r.radius = s.Cylinder().Radius();
            r.hasAxis = true;
            r.center = P(s.Cylinder().Location());
            r.axis = D(s.Cylinder().Axis().Direction());
            break;
        case GeomAbs_Cone:
            r.what = "Cone face";
            r.radius = s.Cone().RefRadius();
            r.hasAxis = true;
            r.center = P(s.Cone().Location());
            r.axis = D(s.Cone().Axis().Direction());
            break;
        case GeomAbs_Sphere:
            r.what = "Sphere face";
            r.radius = s.Sphere().Radius();
            r.hasCenter = true;
            r.center = P(s.Sphere().Location());
            break;
        case GeomAbs_Torus:
            r.what = "Torus face";
            r.radius = s.Torus().MinorRadius();
            r.hasAxis = true;
            r.center = P(s.Torus().Location());
            r.axis = D(s.Torus().Axis().Direction());
            break;
        default: r.what = "Curved face"; break;
        }
        r.ok = true;
        return r;
    } catch (...) {
        return {};
    }
}

ShapePtr curveShape(const std::vector<CurveSeg>& segs, const glm::dvec3& planeN) {
    if (segs.empty()) return nullptr;
    try {
        auto h = std::make_shared<ShapeHandle>();
        if (segs.size() == 1) {
            h->shape = edgeFromSeg(segs[0], planeN);
            return h;
        }
        TopoDS_Compound comp;
        BRep_Builder bb;
        bb.MakeCompound(comp);
        for (auto& s : segs) {
            if (s.kind == CurveSeg::Kind::Line && glm::length(s.b - s.a) < 1e-9) continue;
            bb.Add(comp, edgeFromSeg(s, planeN));
        }
        h->shape = comp;
        return h;
    } catch (...) {
        return nullptr;
    }
}

PairMeasure measurePair(const ShapePtr& a, const glm::mat4& xa, uint32_t faceA, uint32_t edgeA,
                        const ShapePtr& b, const glm::mat4& xb, uint32_t faceB, uint32_t edgeB) {
    PairMeasure r;
    MeasureResult d = measureShapes(a, xa, b, xb, faceA, faceB, edgeA, edgeB);
    if (!d.ok) return r;
    r.ok = true;
    r.distance = d.distance;
    r.pa = d.pa;
    r.pb = d.pb;
    bool wholeA = faceA == 0xffffffffu && edgeA == 0xffffffffu && a->shape.ShapeType() != TopAbs_VERTEX,
         wholeB = faceB == 0xffffffffu && edgeB == 0xffffffffu && b->shape.ShapeType() != TopAbs_VERTEX;
    if (wholeA || wholeB) return r;
    PickInfo ia = measurePick(a, xa, faceA, edgeA), ib = measurePick(b, xb, faceB, edgeB);
    if (!ia.ok || !ib.ok) return r;
    // Centers and axes: a hole's edge or wall gives its center line, a ball its center.
    struct Ref { bool point = false, line = false; glm::dvec3 p{0}, u{0}; };
    auto refOf = [](const PickInfo& i) {
        Ref f;
        if (i.hasCenter && !i.hasAxis && i.isFace) { f.point = true; f.p = i.center; }   // sphere
        else if (i.hasCenter) { f.point = true; f.p = i.center; f.u = glm::normalize(i.axis); }   // circle edge
        else if (i.hasAxis) { f.line = true; f.p = i.center; f.u = glm::normalize(i.axis); }   // cylinder, cone, torus
        return f;
    };
    Ref ra = refOf(ia), rb = refOf(ib);
    auto onLine = [](const glm::dvec3& p, const glm::dvec3& o, const glm::dvec3& u) { return o + u * glm::dot(p - o, u); };
    if ((ra.point || ra.line) && (rb.point || rb.line)) {
        if (ra.point && rb.point) {
            r.ca = ra.p; r.cb = rb.p;
        } else if (ra.point && rb.line) {
            r.ca = ra.p; r.cb = onLine(ra.p, rb.p, rb.u);
        } else if (ra.line && rb.point) {
            r.ca = onLine(rb.p, ra.p, ra.u); r.cb = rb.p;
        } else {
            // Two axes: parallel ones by their spacing, others by their closest points.
            glm::dvec3 w = ra.p - rb.p;
            double b_ = glm::dot(ra.u, rb.u), den = 1 - b_ * b_;
            if (den < 1e-10) {
                r.ca = ra.p;
                r.cb = onLine(ra.p, rb.p, rb.u);
            } else {
                double d_ = glm::dot(ra.u, w), e_ = glm::dot(rb.u, w);
                double s = (b_ * e_ - d_) / den, t = (e_ - b_ * d_) / den;
                r.ca = ra.p + ra.u * s;
                r.cb = rb.p + rb.u * t;
            }
        }
        r.centers = glm::length(r.cb - r.ca);
    }
    // Angle: two straight edges, two flat faces, or a straight edge and a flat face.
    if (ia.hasDir && ib.hasDir) {
        glm::dvec3 u = glm::normalize(ia.dir), v = glm::normalize(ib.dir);
        if (ia.isEdge && ib.isEdge) {
            // Edges meeting at a corner: each one pointing away from the shared corner (a real
            // 0 to 180). Otherwise lines have no direction: the smaller angle, 0 to 90.
            const double tol = 1e-4;
            glm::dvec3 ea[2] = {ia.v0, ia.v1}, eb[2] = {ib.v0, ib.v1};
            bool met = false;
            for (int i = 0; i < 2 && !met; ++i)
                for (int j = 0; j < 2 && !met; ++j)
                    if (glm::length(ea[i] - eb[j]) < tol) {
                        u = glm::normalize(ea[1 - i] - ea[i]);
                        v = glm::normalize(eb[1 - j] - eb[j]);
                        met = true;
                    }
            double c = std::clamp(glm::dot(u, v), -1.0, 1.0);
            if (!met) c = std::abs(c);
            r.angle = std::acos(c) * 180.0 / kPiM;
            r.angleSigned = met;
        } else if (ia.isFace && ib.isFace) {
            // Between outward normals: two faces of a box corner give 90, a 45 degree chamfer and
            // its top face 45 (the panel also shows the other side, 180 minus it).
            r.angle = std::acos(std::clamp(glm::dot(u, v), -1.0, 1.0)) * 180.0 / kPiM;
            r.angleSigned = true;
        } else {
            // An edge against a face: the angle between the line and the face (0 lies in it).
            double c = std::abs(glm::dot(u, v));
            r.angle = 90.0 - std::acos(std::clamp(c, 0.0, 1.0)) * 180.0 / kPiM;
        }
    }
    return r;
}

// ---------------------------------------------------------------- M8: mesh to solid

ShapePtr meshToSolid(const MeshData& mesh) {
    const size_t nt = mesh.indices.size() / 3;
    if (nt == 0) throw std::runtime_error("That mesh is empty");
    if (nt > 200000) throw std::runtime_error("That mesh has over 200,000 triangles. Simplify it before converting.");
    try {
        BRepBuilderAPI_Sewing sew(1e-3);
        for (size_t t = 0; t < nt; ++t) {
            glm::vec3 p[3];
            for (int k = 0; k < 3; ++k) {
                const float* v = &mesh.verts[mesh.indices[t * 3 + k] * 6];
                p[k] = {v[0], v[1], v[2]};
            }
            if (glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) < 1e-12f) continue;   // sliver
            BRepBuilderAPI_MakePolygon pol;
            pol.Add(pnt(glm::dvec3(p[0])));
            pol.Add(pnt(glm::dvec3(p[1])));
            pol.Add(pnt(glm::dvec3(p[2])));
            pol.Close();
            if (pol.IsDone()) sew.Add(BRepBuilderAPI_MakeFace(pol.Wire()).Face());
        }
        sew.Perform();
        TopoDS_Shape shell = sew.SewedShape();
        if (shell.IsNull()) throw std::runtime_error("Could not sew the mesh (it may have big holes)");
        ShapeFix_Shape fix(shell);
        fix.Perform();
        TopoDS_Shape fixed = fix.Shape();
        if (countSolids(fixed) == 0)
            throw std::runtime_error("The mesh did not close into a solid. It probably has holes (typical for scans).");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = fixed;
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "mesh to solid");
    }
}

bool shapeIsValid(const ShapePtr& shape) {
    if (!shape) return false;
    try {
        return BRepCheck_Analyzer(shape->shape).IsValid() != 0;
    } catch (const Standard_Failure&) {
        return false;
    }
}

std::vector<ShapePtr> solidsOf(const ShapePtr& shape) {
    std::vector<ShapePtr> out;
    if (!shape) return out;
    for (TopExp_Explorer ex(shape->shape, TopAbs_SOLID); ex.More(); ex.Next()) {
        auto h = std::make_shared<ShapeHandle>();
        h->shape = ex.Current();
        out.push_back(h);
    }
    return out;
}

uint64_t featuresBuildStamp() {
    uint64_t h = 1469598103934665603ull;
    for (const char* c = __DATE__ " " __TIME__; *c; ++c) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    return h;
}

} // namespace kst::kernel
