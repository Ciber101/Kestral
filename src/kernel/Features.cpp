#include "kernel/Features.h"
#include "kernel/ShapeHandle.h"
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
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <gp_Pln.hxx>
#include <gp_Cylinder.hxx>
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
#include <BRep_Tool.hxx>
#include <TopoDS_Shell.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeLine.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
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
    if (r < 1e-9) throw std::runtime_error("Degenerate arc in the profile");
    gp_Ax2 ax(pnt(c.center), dir(n), dir(c.a - c.center));
    gp_Circ circ(ax, r);
    // Both arguments are vectors from the center. Passing the endpoint itself is only
    // correct when the center is the world origin, and an offset arc then sweeps the wrong way.
    double sweep = wrap2pi(angleAround(c.b - c.center, n, c.a - c.center));
    if (sweep < 1e-9 || sweep > kTwoPi - 1e-7) return BRepBuilderAPI_MakeEdge(circ).Edge();
    return BRepBuilderAPI_MakeEdge(circ, 0.0, sweep).Edge();
}

TopoDS_Wire wireFromSegs(const std::vector<CurveSeg>& segs, const glm::dvec3& planeN) {
    BRepBuilderAPI_MakeWire mw;
    for (const CurveSeg& c : segs) {
        TopoDS_Edge e = edgeFromSeg(c, planeN);
        mw.Add(c.back ? TopoDS::Edge(e.Reversed()) : e);
    }
    if (!mw.IsDone()) throw std::runtime_error("Could not build a closed profile loop");
    return mw.Wire();
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
TopoDS_Wire offsetOutward(const TopoDS_Wire& w, double d, const glm::dvec3& planeN) {
    double before = extent2D(w);
    for (int sign : {1, -1}) {
        try {
            BRepOffsetAPI_MakeOffset mo(w, GeomAbs_Arc);
            mo.Perform(sign * d);
            TopoDS_Shape res = mo.Shape();
            // Result is a wire (possibly with several edges).
            TopExp_Explorer ex(res, TopAbs_WIRE);
            if (!ex.More()) continue;
            TopoDS_Wire out = TopoDS::Wire(ex.Current());
            if (extent2D(out) >= before - 1e-9) return out;
        } catch (const Standard_Failure&) {
            continue;
        }
    }
    throw std::runtime_error("Taper is too large for this profile (offset failed)");
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
TopoDS_Shape unify(const TopoDS_Shape& s) {
    try {
        // Merging faces can break a solid that was fine: two halves of a hole wall (the hole through
        // a wall and through a slab pressed onto it) merge into one cylinder whose seam no longer
        // fits, and the hole then draws torn and blocks fillets and chamfers. The merge runs on a
        // copy (it changes shared edges in place), and the unmerged solid is kept when the merged
        // one is broken.
        bool wasValid = BRepCheck_Analyzer(s).IsValid() != 0;
        TopoDS_Shape work = wasValid ? BRepBuilderAPI_Copy(s, Standard_True, Standard_False).Shape() : s;
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

TopoDS_Shape taperedPrism(const RegionLoops& loops, const glm::dvec3& dirN, double dist, double taperDeg) {
    double grow = dist * std::tan(taperDeg * kPiK / 180.0);
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
            TopoDS_Wire base = wireFromSegs(loops[i], n);
            TopoDS_Wire top = offsetOutward(base, grow, n);
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
            shape = taperedPrism(loops, dirN, half, std::abs(taperDeg));
            if (symmetric) {
                TopoDS_Shape other = taperedPrism(loops, -dirN, half, std::abs(taperDeg));
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
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
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
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
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
    glm::dvec3 d = glm::normalize(dir);
    gp_Trsf tr;
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
    TopoDS_Shape body = target->shape.Moved(TopLoc_Location(tr));
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    if (box.IsVoid()) throw std::runtime_error("The emboss does not meet a body");
    Standard_Real x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    glm::dvec3 corners[8] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                             {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
    double u0 = glm::dot(planeO, d);
    double reach = 0;
    for (auto& c : corners) reach = std::max(reach, glm::dot(c, d) - u0);
    reach += 5.0;
    TopoDS_Shape tool;
    bool any = false;
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
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
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
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
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
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
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
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
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
                tr.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                             m[0][1], m[1][1], m[2][1], m[3][1],
                             m[0][2], m[1][2], m[2][2], m[3][2]);
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
            tr.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                         m[0][1], m[1][1], m[2][1], m[3][1],
                         m[0][2], m[1][2], m[2][2], m[3][2]);
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
                GCPnts_QuasiUniformDeflection disc(c, defl);
                if (disc.IsDone() && disc.NbPoints() >= 2) {
                    int np = disc.NbPoints(), stepN = std::max(1, np / 200);   // cap points per edge
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
        GCPnts_QuasiUniformDeflection disc(c, 0.05);
        if (disc.IsDone() && disc.NbPoints() >= 2) {
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

ShapePtr deleteFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces) {
    if (faces.empty()) throw std::runtime_error("Select the faces to delete");
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        for (uint32_t fi : faces)
            if ((int)fi >= fmap.Extent())
                throw std::runtime_error("The body changed and the stored faces no longer match. Delete this step and pick the faces again.");
        BRepAlgoAPI_Defeaturing df;
        df.SetShape(shape->shape);
        for (uint32_t fi : faces) df.AddFaceToRemove(fmap((int)fi + 1));
        df.SetRunParallel(false);
        df.SetToFillHistory(false);
        df.Build();
        bool ok = df.IsDone() && !df.HasErrors() && !df.Shape().IsNull() && countSolids(df.Shape()) > 0;
        if (ok) {
            // Nothing actually removed means the gap could not be healed.
            TopTools_IndexedMapOfShape rmap;
            TopExp::MapShapes(df.Shape(), TopAbs_FACE, rmap);
            if (rmap.Extent() >= fmap.Extent()) ok = false;
        }
        if (!ok)
            throw std::runtime_error("Those faces cannot be deleted: the faces around them cannot grow to close the gap. Try deleting the whole feature (all its faces) at once.");
        auto h = std::make_shared<ShapeHandle>();
        h->shape = unify(df.Shape());
        return h;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "delete face");
    }
}

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
            for (uint32_t fi : faces) {
                const TopoDS_Face& f = TopoDS::Face(cmap((int)fi + 1));
                gp_Dir n;
                gp_Pnt p;
                planarNormal(f, n, p);
                gp_Vec v(n);
                v *= offset;
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

namespace {
gp_Trsf trsfOf(const glm::mat4& m) {
    gp_Trsf tr;
    tr.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                 m[0][1], m[1][1], m[2][1], m[3][1],
                 m[0][2], m[1][2], m[2][2], m[3][2]);
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

ShapePtr extrudeFace(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, double dist, bool symmetric) {
    if (std::abs(dist) < 1e-9) throw std::runtime_error("Distance must not be zero");
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) throw std::runtime_error("The picked face no longer exists. Edit the feature and pick it again.");
        gp_Trsf tr;
        tr.SetValues(xf[0][0], xf[1][0], xf[2][0], xf[3][0],
                     xf[0][1], xf[1][1], xf[2][1], xf[3][1],
                     xf[0][2], xf[1][2], xf[2][2], xf[3][2]);
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
        tr.SetValues(xf[0][0], xf[1][0], xf[2][0], xf[3][0],
                     xf[0][1], xf[1][1], xf[2][1], xf[3][1],
                     xf[0][2], xf[1][2], xf[2][2], xf[3][2]);
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

std::vector<ShapePtr> silhouetteSplit(const ShapePtr& target, const glm::dvec3& view, const glm::mat4& xform) {
    if (!target) throw std::runtime_error("Select the body to split");
    if (glm::length(view) < 0.5) throw std::runtime_error("Bad view direction");
    glm::dvec3 d = glm::normalize(view);
    gp_Trsf tr;
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
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

ShapePtr loftRegions(const std::vector<RegionLoops>& sections, const std::vector<std::vector<CurveSeg>>& rails,
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

// ---------------------------------------------------------------- M7: measure

MeasureResult measureShapes(const ShapePtr& a, const glm::mat4& xa,
                            const ShapePtr& b, const glm::mat4& xb,
                            uint32_t faceA, uint32_t faceB, uint32_t edgeA, uint32_t edgeB) {
    MeasureResult r;
    try {
        gp_Trsf ta, tb;
        ta.SetValues(xa[0][0], xa[1][0], xa[2][0], xa[3][0],
                     xa[0][1], xa[1][1], xa[2][1], xa[3][1],
                     xa[0][2], xa[1][2], xa[2][2], xa[3][2]);
        tb.SetValues(xb[0][0], xb[1][0], xb[2][0], xb[3][0],
                     xb[0][1], xb[1][1], xb[2][1], xb[3][1],
                     xb[0][2], xb[1][2], xb[2][2], xb[3][2]);
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
    t.SetValues(xf[0][0], xf[1][0], xf[2][0], xf[3][0],
                xf[0][1], xf[1][1], xf[2][1], xf[3][1],
                xf[0][2], xf[1][2], xf[2][2], xf[3][2]);
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

} // namespace kst::kernel
