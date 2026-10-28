#include "kernel/Mechanical.h"
#include "kernel/ShapeHandle.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFill_TypeOfContact.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Line.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Solid.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec2d.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace kst::kernel {
namespace {

const double kPi = 3.14159265358979323846;

double rad(double d) { return d * kPi / 180.0; }
double inv(double a) { return std::tan(a) - a; }

ShapePtr wrap(const TopoDS_Shape& s) {
    auto h = std::make_shared<ShapeHandle>();
    h->shape = s;
    return h;
}

bool valid(const TopoDS_Shape& s) {
    if (s.IsNull()) return false;
    BRepCheck_Analyzer a(s);
    return a.IsValid();
}

// Solids out of sweeps can come out inside out.
TopoDS_Shape fixOrientation(TopoDS_Shape s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    if (g.Mass() < 0) s.Reverse();
    return s;
}

// ---------------------------------------------------------------- involute outline

struct Outline {
    int z = 20;
    double m = 2;          // transverse module
    double alpha = 0.349;  // transverse pressure angle, radians
    double ha = 1.0;       // addendum (x module)
    double hf = 1.25;      // dedendum (x module)
    double shift = 0;
    double thin = 0;       // mm taken off the tooth thickness at the pitch circle
};

gp_Pnt polar(double r, double a) { return gp_Pnt(r * std::cos(a), r * std::sin(a), 0); }

TopoDS_Wire gearWire(const Outline& o) {
    if (o.z < 3) throw std::runtime_error("A gear needs at least 3 teeth");
    if (o.m <= 0) throw std::runtime_error("The module must be above 0");
    const double r = o.m * o.z / 2, rb = r * std::cos(o.alpha);
    double ra = r + o.m * (o.ha + o.shift);
    const double rf = r - o.m * (o.hf - o.shift);
    if (rf <= 1e-3) throw std::runtime_error("The teeth are too deep for this many teeth (root circle below 0)");
    const double s = o.m * (kPi / 2 + 2 * o.shift * std::tan(o.alpha)) - o.thin;
    if (s <= 0) throw std::runtime_error("The backlash is bigger than the teeth");
    const double psi = s / (2 * r);
    auto theta = [&](double rho) {
        double a = std::acos(std::min(1.0, rb / rho));
        return psi + inv(o.alpha) - inv(a);
    };
    const double r0 = std::max(rb, rf);
    // Pointed teeth: stop the tip where the tooth still has some land.
    for (int i = 0; i < 2000 && ra > r0 + 0.05 * o.m && theta(ra) < 0.06 * psi; ++i) ra -= 0.005 * o.m;
    if (ra <= r0 + 1e-6) throw std::runtime_error("The teeth have no working depth: try more teeth or a larger profile shift");
    const double th0 = theta(r0);
    const double half = kPi / o.z;
    if (th0 >= half - 1e-4) throw std::runtime_error("The teeth overlap at the root: try fewer teeth or a profile shift");
    const double thA = theta(ra);

    const int N = 9;
    const double t0 = std::sqrt(std::max(0.0, (r0 / rb) * (r0 / rb) - 1)), tA = std::sqrt(std::max(0.0, (ra / rb) * (ra / rb) - 1));
    std::vector<double> rho(N);
    for (int i = 0; i < N; ++i) {
        double t = t0 + (tA - t0) * i / (N - 1);
        rho[i] = rb * std::sqrt(1 + t * t);
    }
    rho[0] = r0;
    rho[N - 1] = ra;

    BRepBuilderAPI_MakeWire mw;
    auto add = [&](const TopoDS_Edge& e) {
        mw.Add(e);
        if (!mw.IsDone()) throw std::runtime_error("Could not build the tooth outline");
    };
    auto seg = [&](const gp_Pnt& a, const gp_Pnt& b) { add(BRepBuilderAPI_MakeEdge(a, b).Edge()); };
    auto arc = [&](const gp_Pnt& a, const gp_Pnt& m, const gp_Pnt& b) {
        Handle(Geom_TrimmedCurve) c = GC_MakeArcOfCircle(a, m, b).Value();
        add(BRepBuilderAPI_MakeEdge(c).Edge());
    };
    auto spline = [&](const std::vector<gp_Pnt>& pts) {
        Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, (int)pts.size());
        for (size_t i = 0; i < pts.size(); ++i) arr->SetValue((int)i + 1, pts[i]);
        GeomAPI_Interpolate ip(arr, Standard_False, 1e-9);
        ip.Perform();
        if (!ip.IsDone()) throw std::runtime_error("Could not build the involute flank");
        add(BRepBuilderAPI_MakeEdge(ip.Curve()).Edge());
    };
    const bool radial = rf < rb - 1e-9;
    for (int k = 0; k < o.z; ++k) {
        double phi = 2 * kPi * k / o.z;
        if (radial) seg(polar(rf, phi - th0), polar(r0, phi - th0));
        std::vector<gp_Pnt> left, right;
        for (int i = 0; i < N; ++i) left.push_back(polar(rho[i], phi - theta(rho[i])));
        for (int i = N - 1; i >= 0; --i) right.push_back(polar(rho[i], phi + theta(rho[i])));
        left.front() = polar(r0, phi - th0);
        right.back() = polar(r0, phi + th0);
        left.back() = polar(ra, phi - thA);
        right.front() = polar(ra, phi + thA);
        spline(left);
        arc(polar(ra, phi - thA), polar(ra, phi), polar(ra, phi + thA));
        spline(right);
        if (radial) seg(polar(r0, phi + th0), polar(rf, phi + th0));
        double next = 2 * kPi * (k + 1) / o.z;
        gp_Pnt end = k + 1 == o.z ? polar(rf, -th0) : polar(rf, next - th0);
        arc(polar(rf, phi + th0), polar(rf, phi + half), end);
    }
    TopoDS_Wire w = mw.Wire();
    if (!w.Closed()) throw std::runtime_error("The tooth outline did not close");
    return w;
}

TopoDS_Face faceOf(const TopoDS_Wire& w) {
    BRepBuilderAPI_MakeFace mf(w, Standard_True);
    if (!mf.IsDone()) throw std::runtime_error("Could not make the gear face");
    return mf.Face();
}

TopoDS_Shape transformed(const TopoDS_Shape& s, const gp_Trsf& t) { return BRepBuilderAPI_Transform(s, t, Standard_True).Shape(); }

// A helix wire on a cylinder (or cone) about `ax`, from height z0 rising `height`.
TopoDS_Wire helixWire(const gp_Ax3& ax, double radius, double lead, double z0, double height, bool left, double taper) {
    double angle = 2 * kPi * height / lead * (left ? -1 : 1);
    Handle(Geom_Surface) surf;
    gp_Pnt2d start;
    gp_Vec2d dv;
    if (std::abs(taper) > 1e-9) {
        double c = std::cos(taper);
        surf = new Geom_ConicalSurface(ax, taper, radius);
        start = gp_Pnt2d(0, z0 / c);
        dv = gp_Vec2d(angle, height / c);
    } else {
        surf = new Geom_CylindricalSurface(ax, radius);
        start = gp_Pnt2d(0, z0);
        dv = gp_Vec2d(angle, height);
    }
    Handle(Geom2d_Line) line = new Geom2d_Line(start, gp_Dir2d(dv));
    // One edge per turn: a single 3D approximation of a long helix drifts, and a sweep along it
    // then crosses itself.
    const int pieces = std::max(1, (int)std::ceil(height / lead - 1e-9));
    const double L = dv.Magnitude();
    BRepBuilderAPI_MakeWire mw;
    for (int i = 0; i < pieces; ++i) {
        TopoDS_Edge e = BRepBuilderAPI_MakeEdge(line, surf, L * i / pieces, L * (i + 1) / pieces).Edge();
        BRepLib::BuildCurves3d(e, 1e-6, GeomAbs_C1, 14, 2000);
        mw.Add(e);
    }
    return mw.Wire();
}

TopoDS_Shape pipeShell(const TopoDS_Wire& spine, const TopoDS_Wire& profile, const TopoDS_Wire* aux) {
    BRepOffsetAPI_MakePipeShell ps(spine);
    if (aux) ps.SetMode(*aux, Standard_True, BRepFill_NoContact);
    else ps.SetMode(Standard_True);
    ps.Add(profile, Standard_False, Standard_False);
    ps.Build();
    if (!ps.IsDone() || !ps.MakeSolid()) return TopoDS_Shape();
    return fixOrientation(ps.Shape());
}

// The outline swept up `h` while turning `twist` radians (helical gears).
TopoDS_Shape twistedPrism(const TopoDS_Wire& w, double h, double twist) {
    if (std::abs(twist) < 1e-9) return BRepPrimAPI_MakePrism(faceOf(w), gp_Vec(0, 0, h)).Shape();
    try {
        TopoDS_Wire spine = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, h)).Edge()).Wire();
        Handle(Geom_Surface) cyl = new Geom_CylindricalSurface(gp_Ax3(gp::XOY()), 1.0);
        gp_Vec2d dv(twist, h);
        Handle(Geom2d_Curve) line = new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(dv));
        TopoDS_Edge e = BRepBuilderAPI_MakeEdge(line, cyl, 0, dv.Magnitude()).Edge();
        BRepLib::BuildCurves3d(e, 1e-7, GeomAbs_C1, 14, 200);
        TopoDS_Wire aux = BRepBuilderAPI_MakeWire(e).Wire();
        TopoDS_Shape s = pipeShell(spine, w, &aux);
        if (valid(s)) return s;
    } catch (const Standard_Failure&) {
    }
    // Fallback: a smooth loft through turned copies.
    int n = std::clamp((int)std::ceil(std::abs(twist) * 180 / kPi / 3.0), 3, 24);
    BRepOffsetAPI_ThruSections ts(Standard_True, Standard_False);
    ts.CheckCompatibility(Standard_False);
    for (int i = 0; i <= n; ++i) {
        gp_Trsf r, t;
        r.SetRotation(gp::OZ(), twist * i / n);
        t.SetTranslation(gp_Vec(0, 0, h * i / n));
        ts.AddWire(TopoDS::Wire(transformed(w, t * r)));
    }
    ts.Build();
    if (!ts.IsDone()) throw std::runtime_error("Could not twist the helical teeth");
    return ts.Shape();
}

TopoDS_Shape herringbone(const TopoDS_Wire& w, double h, double twist) {
    TopoDS_Shape lower = twistedPrism(w, h / 2, twist / 2);
    gp_Trsf mir;
    mir.SetMirror(gp_Ax2(gp_Pnt(0, 0, h / 2), gp_Dir(0, 0, 1)));
    TopoDS_Shape upper = transformed(lower, mir);
    BRepAlgoAPI_Fuse fu(lower, upper);
    if (!fu.IsDone()) throw std::runtime_error("Could not join the herringbone halves");
    ShapeUpgrade_UnifySameDomain u(fu.Shape(), Standard_True, Standard_True, Standard_False);
    u.Build();
    return u.Shape();
}

TopoDS_Shape cut(const TopoDS_Shape& a, const TopoDS_Shape& b) {
    BRepAlgoAPI_Cut c(a, b);
    if (!c.IsDone()) throw std::runtime_error("A boolean cut failed while building the gear");
    return c.Shape();
}

TopoDS_Shape boreOut(const TopoDS_Shape& s, double bore, double h) {
    if (bore <= 0) return s;
    return cut(s, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -1), gp_Dir(0, 0, 1)), bore / 2, h + 2).Shape());
}

TopoDS_Wire polygon(const std::vector<gp_Pnt>& pts) {
    BRepBuilderAPI_MakePolygon mp;
    for (auto& p : pts) mp.Add(p);
    mp.Close();
    if (!mp.IsDone()) throw std::runtime_error("Could not build a profile");
    return mp.Wire();
}

// One helical thread (or worm tooth) swept around `ax`: profile points are (radius, axial offset)
// at the helix start, which sits at height z0.
TopoDS_Shape helicalSweep(const gp_Ax3& ax, const std::vector<glm::dvec2>& prof, double lead, double z0, double height,
                          bool left, double taper) {
    double rh = 1e18;
    for (auto& p : prof) rh = std::min(rh, p.x);
    rh = std::max(rh, 0.05);
    double shift = z0 * std::tan(taper);
    gp_Vec X(ax.XDirection()), Z(ax.Direction());
    std::vector<gp_Pnt> pts;
    for (auto& p : prof) pts.push_back(ax.Location().Translated(X * (p.x + shift) + Z * (z0 + p.y)));
    TopoDS_Wire spine = helixWire(ax, rh, lead, z0, height, left, taper);
    TopoDS_Wire pw = polygon(pts);
    TopoDS_Shape s = pipeShell(spine, pw, nullptr);
    if (s.IsNull()) throw std::runtime_error("Could not sweep the helix");
    return s;
}

// One turn of the groove, kept in the axial plane by a second helix. A multi-turn
// sweep self-intersects outside the shaft and the boolean then deletes the body.
TopoDS_Shape helicalTurn(const gp_Ax3& ax, const std::vector<glm::dvec2>& prof, double lead, double z0, bool left, double taper,
                         double height = 0) {
    double rIn = 1e18, rOut = 0;
    for (auto& p : prof) {
        rIn = std::min(rIn, p.x);
        rOut = std::max(rOut, p.x);
    }
    rIn = std::max(rIn, 0.05);
    rOut = std::max(rOut, rIn + 0.5);
    if (height <= 0) height = lead * 1.08;
    double shift = z0 * std::tan(taper);
    gp_Vec X(ax.XDirection()), Z(ax.Direction());
    std::vector<gp_Pnt> pts;
    for (auto& p : prof) pts.push_back(ax.Location().Translated(X * (p.x + shift) + Z * (z0 + p.y)));
    TopoDS_Wire spine = helixWire(ax, rIn, lead, z0, height, left, taper);
    TopoDS_Wire aux = helixWire(ax, rOut, lead, z0, height, left, taper);
    BRepOffsetAPI_MakePipeShell ps(spine);
    ps.SetMode(aux, Standard_True, BRepFill_NoContact);
    ps.Add(polygon(pts), Standard_False, Standard_False);
    ps.Build();
    if (!ps.IsDone() || !ps.MakeSolid()) return TopoDS_Shape();
    return fixOrientation(ps.Shape());
}

double transverseAlpha(double alphaN, double beta) { return std::atan(std::tan(alphaN) / std::cos(beta)); }

} // namespace

// ---------------------------------------------------------------- gears

ShapePtr makeGear(const GearSpec& g) {
    try {
        const double an = rad(g.pressureDeg);
        if (g.pressureDeg < 5 || g.pressureDeg > 40) throw std::runtime_error("Pressure angle should be between 5 and 40 degrees");
        if (g.module <= 0) throw std::runtime_error("The module must be above 0");
        if (g.width <= 0 && g.type != GearType::Worm) throw std::runtime_error("The face width must be above 0");
        const double hand = g.leftHand ? -1.0 : 1.0;
        TopoDS_Shape out;
        switch (g.type) {
        case GearType::Spur: {
            Outline o{g.teeth, g.module, an, 1.0, 1.25, g.shift, g.backlash};
            out = boreOut(BRepPrimAPI_MakePrism(faceOf(gearWire(o)), gp_Vec(0, 0, g.width)).Shape(), g.bore, g.width);
            break;
        }
        case GearType::Helical:
        case GearType::Herringbone: {
            double b = rad(std::clamp(g.helixDeg, -60.0, 60.0));
            if (std::abs(b) < 1e-6) b = rad(1);
            double mt = g.module / std::cos(b);
            Outline o{g.teeth, mt, transverseAlpha(an, b), 1.0 * g.module / mt, 1.25 * g.module / mt, g.shift * g.module / mt,
                      g.backlash};
            double r = mt * g.teeth / 2;
            double twist = hand * g.width * std::tan(b) / r;
            TopoDS_Wire w = gearWire(o);
            out = g.type == GearType::Helical ? twistedPrism(w, g.width, twist) : herringbone(w, g.width, twist);
            out = boreOut(out, g.bore, g.width);
            break;
        }
        case GearType::Internal: {
            // Ring = cylinder minus a "space gear" whose teeth are the ring's tooth spaces.
            double b = rad(std::clamp(g.helixDeg, -60.0, 60.0));
            double mt = g.module / std::cos(b);
            double k = g.module / mt;
            Outline o{g.teeth, mt, transverseAlpha(an, b), 1.25 * k, 1.0 * k, 0, -g.backlash};
            double r = mt * g.teeth / 2;
            double rOut = r + 1.25 * g.module + std::max(g.rim, 0.5);
            double twist = std::abs(b) < 1e-6 ? 0.0 : hand * g.width * std::tan(b) / r;
            TopoDS_Wire w = gearWire(o);
            gp_Trsf down;
            down.SetTranslation(gp_Vec(0, 0, -0.5));
            gp_Trsf rot;
            rot.SetRotation(gp::OZ(), -twist * 0.5 / g.width);
            TopoDS_Shape spaces = transformed(twistedPrism(TopoDS::Wire(transformed(w, rot)), g.width + 1, twist * (g.width + 1) / g.width), down);
            TopoDS_Shape ring = BRepPrimAPI_MakeCylinder(gp_Ax2(gp::Origin(), gp::DZ()), rOut, g.width).Shape();
            out = cut(ring, spaces);
            break;
        }
        case GearType::Rack: {
            if (g.teeth < 1) throw std::runtime_error("A rack needs at least 1 tooth");
            const double m = g.module, p = kPi * m, ta = std::tan(an);
            const double base = -1.25 * m - std::max(g.rim, 0.5);
            const double wTop = p / 4 - m * ta - g.backlash / 2, wRoot = p / 4 + 1.25 * m * ta - g.backlash / 2;
            if (wTop <= 0.01 * m) throw std::runtime_error("The rack teeth come to a point at this pressure angle");
            if (wRoot >= p / 2) throw std::runtime_error("The rack teeth overlap at the root at this pressure angle");
            std::vector<gp_Pnt> pts;
            pts.push_back(gp_Pnt(0, base, 0));
            pts.push_back(gp_Pnt(0, -1.25 * m, 0));
            for (int i = 0; i < g.teeth; ++i) {
                double c = p / 2 + i * p;
                pts.push_back(gp_Pnt(c - wRoot, -1.25 * m, 0));
                pts.push_back(gp_Pnt(c - wTop, m, 0));
                pts.push_back(gp_Pnt(c + wTop, m, 0));
                pts.push_back(gp_Pnt(c + wRoot, -1.25 * m, 0));
            }
            pts.push_back(gp_Pnt(g.teeth * p, -1.25 * m, 0));
            pts.push_back(gp_Pnt(g.teeth * p, base, 0));
            double skew = std::abs(g.helixDeg) < 1e-6 ? 0.0 : hand * g.width * std::tan(rad(std::clamp(g.helixDeg, -60.0, 60.0)));
            out = BRepPrimAPI_MakePrism(faceOf(polygon(pts)), gp_Vec(skew, 0, g.width)).Shape();
            break;
        }
        case GearType::Bevel: {
            if (g.mateTeeth < 3) throw std::runtime_error("The mating gear needs at least 3 teeth");
            Outline o{g.teeth, g.module, an, 1.0, 1.25, g.shift, g.backlash};
            const double r = g.module * g.teeth / 2;
            const double delta = std::atan2((double)g.teeth, (double)g.mateTeeth);   // pitch cone half angle (90 deg shafts)
            const double R = r / std::sin(delta), H = r / std::tan(delta);
            const double b = std::min(g.width, R * 0.45);
            const double k = (R - b) / R;
            TopoDS_Wire w0 = gearWire(o);
            gp_Trsf sc;
            sc.SetScale(gp_Pnt(0, 0, H), k);
            TopoDS_Wire w1 = TopoDS::Wire(transformed(w0, sc));
            BRepOffsetAPI_ThruSections ts(Standard_True, Standard_True);
            ts.CheckCompatibility(Standard_False);
            ts.AddWire(w0);
            ts.AddWire(w1);
            ts.Build();
            if (!ts.IsDone()) throw std::runtime_error("Could not build the bevel teeth");
            out = boreOut(ts.Shape(), g.bore, H * (1 - k));
            break;
        }
        case GearType::Worm: {
            if (g.starts < 1 || g.starts > 8) throw std::runtime_error("A worm has 1 to 8 starts");
            if (g.length <= 0) throw std::runtime_error("The worm length must be above 0");
            const double m = g.module, p = kPi * m, lead = g.starts * p;
            const double r1 = g.wormDia / 2, ra = r1 + m, rf = r1 - 1.25 * m, rc = rf - 0.05 * m;
            if (rf <= 0.2 * m) throw std::runtime_error("The worm is too thin for this module: raise its pitch diameter");
            auto hw = [&](double rho) { return p / 4 - (rho - r1) * std::tan(an) - g.backlash / 4; };
            if (hw(ra) <= 0.02 * p) throw std::runtime_error("The worm teeth come to a point at this pressure angle");
            std::vector<glm::dvec2> prof = {{rc, -hw(rc)}, {ra, -hw(ra)}, {ra, hw(ra)}, {rc, hw(rc)}};
            gp_Ax3 ax(gp::XOY());
            TopoDS_Shape clip = BRepPrimAPI_MakeCylinder(gp_Ax2(gp::Origin(), gp::DZ()), ra + 1, g.length).Shape();
            TopoDS_Shape body = BRepPrimAPI_MakeCylinder(gp_Ax2(gp::Origin(), gp::DZ()), rf, g.length).Shape();
            for (int s = 0; s < g.starts; ++s) {
                TopoDS_Shape th = helicalSweep(ax, prof, lead, -lead, g.length + 2 * lead, g.leftHand, 0);
                if (s) {
                    gp_Trsf rot;
                    rot.SetRotation(gp::OZ(), 2 * kPi * s / g.starts);
                    th = transformed(th, rot);
                }
                BRepAlgoAPI_Common cm(th, clip);
                if (!cm.IsDone()) throw std::runtime_error("Could not trim the worm thread");
                BRepAlgoAPI_Fuse fu(body, cm.Shape());
                if (!fu.IsDone()) throw std::runtime_error("Could not join the worm thread");
                body = fu.Shape();
            }
            ShapeUpgrade_UnifySameDomain u(body, Standard_True, Standard_True, Standard_False);
            u.Build();
            out = boreOut(u.Shape(), g.bore, g.length);
            break;
        }
        case GearType::WormWheel: {
            const double m = g.module;
            const double r1 = g.wormDia / 2;
            if (r1 - 1.25 * m <= 0) throw std::runtime_error("The worm pitch diameter is too small for this module");
            const double gamma = std::atan(std::max(1, g.starts) * m / g.wormDia);   // lead angle
            Outline o{g.teeth, m, an, 1.6, 1.25, 0, g.backlash};
            const double r2 = m * g.teeth / 2, a = r1 + r2;
            double twist = hand * g.width * std::tan(gamma) / r2;
            TopoDS_Shape wheel = twistedPrism(gearWire(o), g.width, twist);
            // Throat: the worm's reach swung around the wheel axis.
            TopoDS_Shape torus = BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(0, 0, g.width / 2), gp::DZ()), a, r1 - m).Shape();
            out = boreOut(cut(wheel, torus), g.bore, g.width);
            break;
        }
        default: throw std::runtime_error("Unknown gear type");
        }
        if (out.IsNull()) throw std::runtime_error("The gear came out empty");
        return wrap(out);
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Gear geometry failed: ") + (e.GetMessageString() ? e.GetMessageString() : "kernel error"));
    }
}

std::string gearInfo(const GearSpec& g) {
    char b[256];
    const double m = g.module;
    switch (g.type) {
    case GearType::Spur:
    case GearType::Bevel: {
        double d = m * g.teeth;
        std::snprintf(b, sizeof b, "Pitch dia %.3f   Outside %.3f\nRoot %.3f   Circular pitch %.3f", d, d + 2 * m * (1 + g.shift),
                      d - 2 * m * (1.25 - g.shift), kPi * m);
        if (g.type == GearType::Bevel) {
            double delta = std::atan2((double)g.teeth, (double)g.mateTeeth) * 180 / kPi;
            size_t n = std::strlen(b);
            std::snprintf(b + n, sizeof b - n, "\nPitch cone %.2f deg (mate %.2f deg)", delta, 90 - delta);
        }
        break;
    }
    case GearType::Helical:
    case GearType::Herringbone: {
        double d = m * g.teeth / std::cos(rad(g.helixDeg));
        std::snprintf(b, sizeof b, "Pitch dia %.3f   Outside %.3f\nRoot %.3f   (normal module %.3g)", d, d + 2 * m, d - 2.5 * m, m);
        break;
    }
    case GearType::Internal: {
        double d = m * g.teeth / std::cos(rad(g.helixDeg));
        std::snprintf(b, sizeof b, "Pitch dia %.3f   Inside (tips) %.3f\nOutside %.3f", d, d - 2 * m, d + 2.5 * m + 2 * g.rim);
        break;
    }
    case GearType::Rack:
        std::snprintf(b, sizeof b, "Length %.3f   Pitch %.3f\nPitch line %.3f above the base", g.teeth * kPi * m, kPi * m, 1.25 * m + g.rim);
        break;
    case GearType::Worm: {
        double gamma = std::atan(g.starts * m / g.wormDia) * 180 / kPi;
        std::snprintf(b, sizeof b, "Lead %.3f   Lead angle %.2f deg\nOutside %.3f   Root %.3f", g.starts * kPi * m, gamma, g.wormDia + 2 * m,
                      g.wormDia - 2.5 * m);
        break;
    }
    case GearType::WormWheel: {
        double d = m * g.teeth;
        std::snprintf(b, sizeof b, "Pitch dia %.3f   Throat %.3f\nCenter distance to the worm %.3f\nRatio %d:%d", d, d + 2 * m,
                      (d + g.wormDia) / 2, g.teeth, std::max(1, g.starts));
        break;
    }
    default: b[0] = 0;
    }
    return b;
}

// ---------------------------------------------------------------- threads

bool roundFace(const ShapePtr& shape, const glm::mat4& xform, uint32_t face, RoundFace& out) {
    if (!shape) return false;
    try {
        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape->shape, TopAbs_FACE, fmap);
        if ((int)face >= fmap.Extent()) return false;
        const TopoDS_Face& f = TopoDS::Face(fmap((int)face + 1));
        BRepAdaptor_Surface s(f, Standard_True);
        if (s.GetType() != GeomAbs_Cylinder) return false;
        gp_Cylinder c = s.Cylinder();
        double u0, u1, v0, v1;
        BRepTools::UVBounds(f, u0, u1, v0, v1);
        gp_Pnt loc = c.Location();
        gp_Dir d = c.Axis().Direction();
        gp_Pnt o = loc.Translated(gp_Vec(d) * v0);
        gp_Pnt pm;
        gp_Vec du, dv;
        s.D1((u0 + u1) / 2, (v0 + v1) / 2, pm, du, dv);
        gp_Vec n = du.Crossed(dv);
        if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
        gp_Vec w(loc, pm);
        gp_Vec radial = w - gp_Vec(d) * w.Dot(gp_Vec(d));
        glm::dmat4 xf(xform);
        out.origin = glm::dvec3(xf * glm::dvec4(o.X(), o.Y(), o.Z(), 1.0));
        out.dir = glm::normalize(glm::dvec3(xf * glm::dvec4(d.X(), d.Y(), d.Z(), 0.0)));
        double scale = glm::length(glm::dvec3(xf[0]));
        out.radius = c.Radius() * scale;
        out.length = (v1 - v0) * scale;
        out.hole = n.Dot(radial) < 0;
        return out.radius > 0 && out.length > 0;
    } catch (const Standard_Failure&) {
        return false;
    }
}

ShapePtr makeThreadTool(const ThreadCut& t, bool clipToLength) {
    if (t.pitch <= 0) throw std::runtime_error("The pitch must be above 0");
    if (t.length <= 0) throw std::runtime_error("The thread length must be above 0");
    if (t.groove.size() < 3) throw std::runtime_error("The thread profile is empty");
    if (t.length / t.pitch > 2000) throw std::runtime_error("That is over 2000 turns: shorten the thread or use a cosmetic thread");
    try {
        const int starts = std::clamp(t.starts, 1, 12);
        const double lead = t.pitch * starts;
        const double taper = rad(t.taperDeg);
        gp_Ax3 ax(gp_Pnt(t.origin.x, t.origin.y, t.origin.z), gp_Dir(t.dir.x, t.dir.y, t.dir.z));
        double rMax = 0;
        for (auto& p : t.groove) rMax = std::max(rMax, p.x);
        rMax += std::abs(std::tan(taper)) * t.length + 1;
        TopoDS_Shape clip = BRepPrimAPI_MakeCylinder(ax.Ax2(), rMax, t.length).Shape();
        const double z0 = -lead - t.pitch;
        const double height = t.length + 2 * lead + 2 * t.pitch;
        BRep_Builder bb;
        TopoDS_Compound all;
        bb.MakeCompound(all);
        for (int s = 0; s < starts; ++s) {
            TopoDS_Shape th = helicalSweep(ax, t.groove, lead, z0, height, t.leftHand, taper);
            if (s) {
                gp_Trsf rot;
                rot.SetRotation(ax.Axis(), 2 * kPi * s / starts * (t.leftHand ? -1 : 1));
                th = transformed(th, rot);
            }
            if (clipToLength) {
                BRepAlgoAPI_Common cm(th, clip);
                if (!cm.IsDone()) throw std::runtime_error("Could not trim the thread to the face");
                th = cm.Shape();
            }
            bb.Add(all, th);
        }
        return wrap(all);
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Thread geometry failed: ") + (e.GetMessageString() ? e.GetMessageString() : "kernel error"));
    }
}

namespace {

double solidVol(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

int solidCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) ++n;
    return n;
}

} // namespace

ShapePtr cutThread(const ShapePtr& body, const glm::mat4& xform, const ThreadCut& t, std::string* warn) {
    if (!body) throw std::runtime_error("The thread has no body");
    if (t.pitch <= 0) throw std::runtime_error("The pitch must be above 0");
    if (t.length <= 0) throw std::runtime_error("The thread length must be above 0");
    if (t.groove.size() < 3) throw std::runtime_error("The thread profile is empty");
    gp_Trsf tr;
    tr.SetValues(xform[0][0], xform[1][0], xform[2][0], xform[3][0],
                 xform[0][1], xform[1][1], xform[2][1], xform[3][1],
                 xform[0][2], xform[1][2], xform[2][2], xform[3][2]);
    TopoDS_Shape acc = body->shape.Moved(TopLoc_Location(tr));
    const double full = solidVol(acc);
    double before = full;
    // Each piece is one smooth turn of the helix. Overlapping turns meet, so the groove
    // is continuous. A single sweep of the whole thread self-intersects and the boolean
    // then deletes the shaft, so a turn is kept only when the shaft survives.
    gp_Ax3 ax(gp_Pnt(t.origin.x, t.origin.y, t.origin.z), gp_Dir(t.dir.x, t.dir.y, t.dir.z));
    const int starts = std::clamp(t.starts, 1, 12);
    const double lead = t.pitch * starts;
    const double hand = t.leftHand ? -1.0 : 1.0;
    const double taper = rad(t.taperDeg);
    const int nTurn = (int)std::ceil(t.length / lead) + 2;
    int cuts = 0, skipped = 0;
    try {
        // Fast path: the whole groove as one sweep per start (one helix edge per turn keeps it from
        // drifting into itself) and one boolean. Seconds, where the turn by turn fallback below takes
        // a minute or more.
        {
            TopTools_ListOfShape args, tools;
            args.Append(acc);
            for (int s = 0; s < starts; ++s) {
                TopoDS_Shape groove = helicalTurn(ax, t.groove, lead, -lead, t.leftHand, taper, (nTurn + 1) * lead);
                if (groove.IsNull()) { tools.Clear(); break; }
                if (s) {
                    gp_Trsf rot;
                    rot.SetRotation(ax.Axis(), hand * 2 * kPi * s / starts);
                    groove = transformed(groove, rot);
                }
                tools.Append(groove);
            }
            if (!tools.IsEmpty()) {
                BRepAlgoAPI_Cut op;
                op.SetArguments(args);
                op.SetTools(tools);
                op.SetFuzzyValue(0.02);
                op.SetRunParallel(Standard_True);
                op.Build();
                if (op.IsDone() && !op.HasErrors() && solidCount(op.Shape()) == 1) {
                    TopoDS_Shape r = op.Shape();
                    double after = solidVol(r);
                    if (after < 0) { r.Reverse(); after = -after; }
                    // The groove takes metal but never most of the shaft, and the result must be valid.
                    bool ok = BRepCheck_Analyzer(r).IsValid();
                    if (!ok) {   // usually just tolerances where the groove meets another face
                        ShapeFix_Shape fix(r);
                        fix.Perform();
                        r = fix.Shape();
                        ok = BRepCheck_Analyzer(r).IsValid() && solidCount(r) == 1;
                    }
                    if (ok && after > full * 0.55 && after < full - 0.05) return wrap(r);
                }
            }
        }
        for (int s = 0; s < starts; ++s) {
            for (int ti = -1; ti < nTurn; ++ti) {
                TopoDS_Shape turn = helicalTurn(ax, t.groove, lead, ti * lead, t.leftHand, taper);
                if (turn.IsNull()) continue;
                if (s) {
                    gp_Trsf rot;
                    rot.SetRotation(ax.Axis(), hand * 2 * kPi * s / starts);
                    turn = transformed(turn, rot);
                }
                BRepAlgoAPI_Cut op(acc, turn);
                op.SetFuzzyValue(0.02);
                op.Build();
                if (!op.IsDone()) { ++skipped; continue; }
                TopoDS_Shape r = op.Shape();
                if (solidCount(r) != 1) { ++skipped; continue; }
                double after = solidVol(r);
                if (after < 0) { r.Reverse(); after = -after; }
                if (after >= before - 0.05) continue;   // this turn is past the end of the face
                if (after > full * 0.55) {
                    acc = r;
                    before = after;
                    ++cuts;
                } else {
                    ++skipped;
                }
            }
        }
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Thread geometry failed: ") + (e.GetMessageString() ? e.GetMessageString() : "kernel error"));
    }
    if (!cuts) throw std::runtime_error("The thread does not cut this face");
    // A turn the kernel could not cut leaves a gap in the groove: say so instead of passing it off.
    if (skipped > 0 && warn)
        *warn = std::to_string(skipped) + (skipped == 1 ? " turn" : " turns") +
                " of the thread could not be cut, so the groove has a gap. Try a slightly different length or a cosmetic thread.";
    return wrap(acc);
}

// ---------------------------------------------------------------- wings

namespace {

struct Foil {
    std::vector<glm::dvec2> upper, lower;   // leading edge -> trailing edge
};

Foil splitFoil(const std::vector<glm::dvec2>& f) {
    Foil o;
    size_t n = (f.size() + 1) / 2;
    for (size_t i = n; i-- > 0;) o.upper.push_back(f[i]);
    for (size_t i = n - 1; i < f.size(); ++i) o.lower.push_back(f[i]);
    return o;
}

// y of a surface at x (linear between points; x must rise along the list after the nose).
double surfY(const std::vector<glm::dvec2>& s, double x) {
    if (x <= s.front().x) return s.front().y;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i].x >= x) {
            double d = s[i].x - s[i - 1].x;
            double t = d > 1e-12 ? (x - s[i - 1].x) / d : 0.0;
            return s[i - 1].y + (s[i].y - s[i - 1].y) * t;
        }
    return s.back().y;
}

std::vector<glm::dvec2> offsetSurf(const std::vector<glm::dvec2>& s, double d, bool upper) {
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i < s.size(); ++i) {
        glm::dvec2 t = s[std::min(i + 1, s.size() - 1)] - s[i ? i - 1 : 0];
        double l = glm::length(t);
        if (l < 1e-12) continue;
        t /= l;
        glm::dvec2 n = upper ? glm::dvec2(t.y, -t.x) : glm::dvec2(-t.y, t.x);
        glm::dvec2 p = s[i] + n * d;
        if (!out.empty() && p.x <= out.back().x) continue;   // keep x rising
        out.push_back(p);
    }
    return out;
}

gp_Pnt placeFoil(const WingSection& s, glm::dvec2 uv) {
    double c = s.chord, a = rad(s.twistDeg);
    double dx = uv.x * c - 0.25 * c, dz = uv.y * c;
    double x = dx * std::cos(a) + dz * std::sin(a), z = -dx * std::sin(a) + dz * std::cos(a);
    return gp_Pnt(s.le.x + 0.25 * c + x, s.le.y, s.le.z + z);
}

TopoDS_Wire outerWire(const WingSection& s) {
    Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, (int)s.foil.size());
    for (size_t i = 0; i < s.foil.size(); ++i) arr->SetValue((int)i + 1, placeFoil(s, s.foil[i]));
    GeomAPI_Interpolate ip(arr, Standard_False, 1e-9);
    ip.Perform();
    if (!ip.IsDone()) throw std::runtime_error("Could not fit the airfoil curve");
    BRepBuilderAPI_MakeWire mw(BRepBuilderAPI_MakeEdge(ip.Curve()).Edge());
    gp_Pnt a = arr->Value(1), b = arr->Value(arr->Length());
    if (a.Distance(b) > 1e-7) mw.Add(BRepBuilderAPI_MakeEdge(b, a).Edge());
    if (!mw.IsDone()) throw std::runtime_error("Could not close the airfoil");
    return mw.Wire();
}

// The inside of the skin as a closed polygon: offset surfaces, cut off where they meet near the tail.
TopoDS_Wire innerWire(const WingSection& s, double skin) {
    Foil f = splitFoil(s.foil);
    double t = skin / s.chord;
    std::vector<glm::dvec2> u = offsetSurf(f.upper, t, true), l = offsetSurf(f.lower, t, false);
    if (u.size() < 4 || l.size() < 4) throw std::runtime_error("The skin is too thick for this chord");
    double xa = std::max({u[1].x, l[1].x, 1.5 * t, 0.015});
    double xb = xa;
    for (double x = xa; x <= 0.995; x += 0.002) {
        if (surfY(u, x) - surfY(l, x) <= 0.6 * t) break;
        xb = x;
    }
    if (xb - xa < 0.08) throw std::runtime_error("The skin is too thick for a " + std::to_string((int)std::lround(s.chord)) + " mm chord: thin it or make the wing solid");
    const int M = 30;
    std::vector<gp_Pnt> pts;
    for (int i = 0; i < M; ++i) {
        double x = xa + (xb - xa) * 0.5 * (1 - std::cos(kPi * i / (M - 1)));
        pts.push_back(placeFoil(s, {x, surfY(u, x)}));
    }
    for (int i = M - 1; i >= 0; --i) {
        double x = xa + (xb - xa) * 0.5 * (1 - std::cos(kPi * i / (M - 1)));
        pts.push_back(placeFoil(s, {x, surfY(l, x)}));
    }
    return polygon(pts);
}

TopoDS_Shape loftWires(const std::vector<TopoDS_Wire>& ws, const char* what) {
    BRepOffsetAPI_ThruSections ts(Standard_True, Standard_True);
    ts.CheckCompatibility(Standard_False);
    for (auto& w : ws) ts.AddWire(w);
    ts.Build();
    if (!ts.IsDone()) throw std::runtime_error(std::string("Could not loft the ") + what);
    return fixOrientation(ts.Shape());
}

TopoDS_Shape rod(const gp_Pnt& a, const gp_Pnt& b, double r) {
    gp_Vec v(a, b);
    if (v.Magnitude() < 1e-6) return TopoDS_Shape();
    return BRepPrimAPI_MakeCylinder(gp_Ax2(a, gp_Dir(v)), r, v.Magnitude()).Shape();
}

// One boolean for many tools (far faster than cutting them one by one).
TopoDS_Shape cutMany(const TopoDS_Shape& a, const std::vector<TopoDS_Shape>& tools) {
    TopTools_ListOfShape args, tl;
    for (auto& t : tools)
        if (!t.IsNull()) tl.Append(t);
    if (tl.IsEmpty()) return a;
    args.Append(a);
    BRepAlgoAPI_Cut c;
    c.SetArguments(args);
    c.SetTools(tl);
    c.SetRunParallel(Standard_True);
    c.Build();
    if (!c.IsDone()) throw std::runtime_error("A boolean cut failed while building the wing");
    return c.Shape();
}

} // namespace

ShapePtr makeWing(const WingSpec& w) {
    if (w.sections.size() < 2) throw std::runtime_error("A wing needs at least one panel");
    for (auto& s : w.sections) {
        if (s.chord <= 0) throw std::runtime_error("Every chord must be above 0");
        if (s.foil.size() != w.sections[0].foil.size() || s.foil.size() < 7) throw std::runtime_error("The airfoil outlines do not match");
    }
    for (size_t i = 1; i < w.sections.size(); ++i)
        if (w.sections[i].le.y <= w.sections[i - 1].le.y + 1e-6) throw std::runtime_error("Every panel span must be above 0");
    try {
        std::vector<TopoDS_Wire> outer;
        for (auto& s : w.sections) outer.push_back(outerWire(s));
        TopoDS_Shape body = loftWires(outer, "wing");
        const double y0 = w.sections.front().le.y, y1 = w.sections.back().le.y;
        double xmin = 1e18, xmax = -1e18, zmin = 1e18, zmax = -1e18;
        for (auto& s : w.sections) {
            xmin = std::min(xmin, s.le.x - s.chord);
            xmax = std::max(xmax, s.le.x + 2 * s.chord);
            zmin = std::min(zmin, s.le.z - s.chord);
            zmax = std::max(zmax, s.le.z + s.chord);
        }
        // Spar lines through every section at the chord fraction, on the mean line.
        std::vector<std::vector<gp_Pnt>> sparPts;
        for (auto& sp : w.spars) {
            std::vector<gp_Pnt> pts;
            for (auto& s : w.sections) {
                Foil f = splitFoil(s.foil);
                double x = std::clamp(sp.pos, 0.02, 0.95);
                pts.push_back(placeFoil(s, {x, 0.5 * (surfY(f.upper, x) + surfY(f.lower, x))}));
            }
            sparPts.push_back(pts);
        }
        auto sparParts = [&](size_t k, double r, bool hole) {
            std::vector<gp_Pnt> pts = sparPts[k];
            gp_Vec first(pts[0], pts[1]), last(pts[pts.size() - 2], pts.back());
            // Open at the root (rods slide in), stop short of the tip.
            pts.front().Translate(first.Normalized() * -(1.0 + w.skin) * (hole ? 1 : 0));
            double back = 2 * w.skin + (hole ? 0 : -w.skin);
            if (last.Magnitude() > back) pts.back().Translate(last.Normalized() * -back);
            std::vector<TopoDS_Shape> parts;
            for (size_t i = 0; i + 1 < pts.size(); ++i) parts.push_back(rod(pts[i], pts[i + 1], r));
            for (size_t i = 1; i + 1 < pts.size(); ++i) parts.push_back(BRepPrimAPI_MakeSphere(pts[i], r).Shape());
            return parts;
        };
        // The hollow (cells between ribs, around the spar tubes) is built first and added as voids
        // at the end: it sits wholly inside the skin, so no boolean against the skin is needed.
        TopoDS_Shape cavity;
        if (w.hollow) {
            if (w.skin <= 0) throw std::runtime_error("The skin must be above 0");
            if (y1 - y0 <= 4 * w.skin) throw std::runtime_error("The wing is too short to hollow");
            std::vector<TopoDS_Wire> inner;
            for (auto& s : w.sections) inner.push_back(innerWire(s, w.skin));
            cavity = loftWires(inner, "inside of the wing");
            TopoDS_Shape keep = BRepPrimAPI_MakeBox(gp_Pnt(xmin, y0 + w.skin, zmin), xmax - xmin, y1 - y0 - 2 * w.skin, zmax - zmin).Shape();
            BRepAlgoAPI_Common cm(cavity, keep);
            if (!cm.IsDone()) throw std::runtime_error("Could not close the wing ends");
            cavity = cm.Shape();
            std::vector<TopoDS_Shape> solidBits;
            if (w.ribSpacing > 0 && w.ribThick > 0) {
                int n = 0;
                for (double y = y0 + w.ribSpacing; y < y1 - 0.3 * w.ribSpacing && n < 500; y += w.ribSpacing, ++n)
                    solidBits.push_back(BRepPrimAPI_MakeBox(gp_Pnt(xmin, y - w.ribThick / 2, zmin), xmax - xmin, w.ribThick, zmax - zmin).Shape());
            }
            for (size_t k = 0; k < w.spars.size(); ++k)
                if (w.spars[k].dia > 0)
                    for (auto& s : sparParts(k, w.spars[k].dia / 2 + w.skin, false)) solidBits.push_back(s);
            cavity = cutMany(cavity, solidBits);
        }
        std::vector<TopoDS_Shape> holes;
        for (size_t k = 0; k < w.spars.size(); ++k)
            if (w.spars[k].dia > 0)
                for (auto& s : sparParts(k, w.spars[k].dia / 2, true)) holes.push_back(s);
        body = cutMany(body, holes);
        gp_Trsf mir;
        mir.SetMirror(gp_Ax2(gp_Pnt(0, y0, 0), gp_Dir(0, 1, 0)));
        if (w.mirror) {
            BRepAlgoAPI_Fuse fu(body, transformed(body, mir));
            if (!fu.IsDone()) throw std::runtime_error("Could not join the two wing halves");
            ShapeUpgrade_UnifySameDomain u(fu.Shape(), Standard_True, Standard_True, Standard_False);
            u.Build();
            body = u.Shape();
        }
        if (!cavity.IsNull()) {
            TopExp_Explorer outerEx(body, TopAbs_SOLID);
            if (!outerEx.More()) throw std::runtime_error("The wing skin is not a solid");
            BRep_Builder bb;
            TopoDS_Solid res;
            bb.MakeSolid(res);
            for (TopExp_Explorer sh(outerEx.Current(), TopAbs_SHELL); sh.More(); sh.Next()) bb.Add(res, sh.Current());
            std::vector<TopoDS_Shape> cavities{cavity};
            if (w.mirror) cavities.push_back(transformed(cavity, mir));
            for (auto& cv : cavities)
                for (TopExp_Explorer so(cv, TopAbs_SOLID); so.More(); so.Next())
                    for (TopExp_Explorer sh(so.Current(), TopAbs_SHELL); sh.More(); sh.Next()) bb.Add(res, sh.Current().Reversed());
            body = res;
        }
        return wrap(body);
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Wing geometry failed: ") + (e.GetMessageString() ? e.GetMessageString() : "kernel error"));
    }
}

// ---------------------------------------------------------------- blades

namespace {

gp_Pnt placeBlade(const BladeSection& s, glm::dvec2 uv, double pivot, bool left) {
    double b = rad(s.betaDeg);
    double a = (pivot - uv.x) * s.chord, t = uv.y * s.thick * s.chord;
    glm::dvec3 cd(0, std::cos(b), std::sin(b)), nd(0, -std::sin(b), std::cos(b));
    glm::dvec3 p = glm::dvec3(s.r, 0, 0) + cd * a + nd * t;
    p.z += s.rake;
    double k = -rad(s.skewDeg);
    glm::dvec3 q(p.x * std::cos(k) - p.y * std::sin(k), p.x * std::sin(k) + p.y * std::cos(k), p.z);
    if (left) q.y = -q.y;
    return gp_Pnt(q.x, q.y, q.z);
}

TopoDS_Wire bladeWire(const BladeSection& s, double pivot, bool left) {
    Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, (int)s.foil.size());
    for (size_t i = 0; i < s.foil.size(); ++i) arr->SetValue((int)i + 1, placeBlade(s, s.foil[i], pivot, left));
    GeomAPI_Interpolate ip(arr, Standard_False, 1e-9);
    ip.Perform();
    if (!ip.IsDone()) throw std::runtime_error("Could not fit a blade section");
    BRepBuilderAPI_MakeWire mw(BRepBuilderAPI_MakeEdge(ip.Curve()).Edge());
    gp_Pnt a = arr->Value(1), b = arr->Value(arr->Length());
    if (a.Distance(b) > 1e-7) mw.Add(BRepBuilderAPI_MakeEdge(b, a).Edge());
    if (!mw.IsDone()) throw std::runtime_error("Could not close a blade section");
    return mw.Wire();
}

TopoDS_Shape fuseMany(const TopoDS_Shape& a, const std::vector<TopoDS_Shape>& tools) {
    TopTools_ListOfShape args, tl;
    for (auto& t : tools)
        if (!t.IsNull()) tl.Append(t);
    if (tl.IsEmpty()) return a;
    args.Append(a);
    BRepAlgoAPI_Fuse f;
    f.SetArguments(args);
    f.SetTools(tl);
    f.SetRunParallel(Standard_True);
    f.Build();
    if (!f.IsDone()) throw std::runtime_error("Could not join the blades to the hub");
    return f.Shape();
}

} // namespace

BladeParts makeBlades(const BladeSpec& s) {
    if (s.sections.size() < 2) throw std::runtime_error("A blade needs at least two sections");
    if (s.blades < 1 || s.blades > 64) throw std::runtime_error("Blades must be 1 to 64");
    for (auto& sec : s.sections) {
        if (sec.chord <= 0) throw std::runtime_error("Every chord must be above 0");
        if (sec.foil.size() != s.sections[0].foil.size() || sec.foil.size() < 7) throw std::runtime_error("The airfoil outlines do not match");
    }
    try {
        std::vector<TopoDS_Wire> ws;
        for (auto& sec : s.sections) ws.push_back(bladeWire(sec, s.pivot, s.leftHand));
        TopoDS_Shape blade;
        {
            BRepOffsetAPI_ThruSections ts(Standard_True, Standard_False);
            ts.CheckCompatibility(Standard_False);
            for (auto& w : ws) ts.AddWire(w);
            ts.Build();
            if (ts.IsDone() && valid(ts.Shape())) blade = fixOrientation(ts.Shape());
        }
        if (blade.IsNull()) blade = loftWires(ws, "blade");
        if (s.gripTo > 0 && s.gripW > 0 && s.gripT > 0) {
            double r0 = s.blades > 1 ? 0.0 : -s.gripW * 0.3;
            TopoDS_Shape grip = BRepPrimAPI_MakeBox(gp_Pnt(r0, -s.gripW / 2, -s.gripT / 2), s.gripTo - r0, s.gripW, s.gripT).Shape();
            BRepAlgoAPI_Fuse fu(blade, grip);
            if (!fu.IsDone()) throw std::runtime_error("Could not join the root grip");
            blade = fu.Shape();
            if (s.boltDia > 0 && s.blades == 1)
                blade = cut(blade, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(s.boltR, 0, -s.gripT), gp_Dir(0, 0, 1)), s.boltDia / 2, 2 * s.gripT).Shape());
        }
        std::vector<TopoDS_Shape> copies;
        for (int i = 1; i < s.blades; ++i) {
            gp_Trsf t;
            t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 2 * kPi * i / s.blades);
            copies.push_back(transformed(blade, t));
        }
        TopoDS_Shape rotor;
        if (s.hubDia > 0 && s.hubLen > 0) {
            TopoDS_Shape hub = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -s.hubLen / 2), gp_Dir(0, 0, 1)), s.hubDia / 2, s.hubLen).Shape();
            copies.push_back(blade);
            rotor = fuseMany(hub, copies);
        } else {
            rotor = fuseMany(blade, copies);
        }
        ShapeUpgrade_UnifySameDomain u(rotor, Standard_True, Standard_True, Standard_False);
        u.Build();
        if (!u.Shape().IsNull()) rotor = u.Shape();
        if (s.bore > 0) {
            double h = std::max({s.hubLen, s.gripT, 1.0}) * 4;
            rotor = cut(rotor, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -h / 2), gp_Dir(0, 0, 1)), s.bore / 2, h).Shape());
        }
        BladeParts out;
        out.rotor = wrap(rotor);
        if (s.ductThick > 0 && s.ductLen > 0) {
            double ri = s.sections.back().r + s.ductGap, ro = ri + s.ductThick;
            gp_Ax2 ax(gp_Pnt(0, 0, -s.ductLen / 2), gp_Dir(0, 0, 1));
            gp_Ax2 axi(gp_Pnt(0, 0, -s.ductLen / 2 - 1), gp_Dir(0, 0, 1));
            out.duct = wrap(cut(BRepPrimAPI_MakeCylinder(ax, ro, s.ductLen).Shape(), BRepPrimAPI_MakeCylinder(axi, ri, s.ductLen + 2).Shape()));
        }
        return out;
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Blade geometry failed: ") + (e.GetMessageString() ? e.GetMessageString() : "kernel error"));
    }
}

} // namespace kst::kernel
