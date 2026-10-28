// Dovetails: print connectors (key, tab, sliding rail, puzzle), woodworking corner joints and
// machine slide ways.
#include "kernel/Mechanical.h"
#include "kernel/ShapeHandle.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace kst::kernel {

namespace {

const double kPiJ = 3.14159265358979323846;

ShapePtr wrapJ(const TopoDS_Shape& s) {
    auto h = std::make_shared<ShapeHandle>();
    h->shape = s;
    return h;
}

// Closed polygon at height z0 (points in XY) pushed up to z1.
TopoDS_Shape prismXY(const std::vector<glm::dvec2>& pts, double z0, double z1) {
    BRepBuilderAPI_MakePolygon mp;
    for (auto& p : pts) mp.Add(gp_Pnt(p.x, p.y, z0));
    mp.Close();
    if (!mp.IsDone()) throw std::runtime_error("Could not build the dovetail outline");
    BRepBuilderAPI_MakeFace mf(mp.Wire(), Standard_True);
    if (!mf.IsDone()) throw std::runtime_error("The dovetail outline is not flat");
    return BRepPrimAPI_MakePrism(mf.Face(), gp_Vec(0, 0, z1 - z0)).Shape();
}

// Closed polygon in YZ at x = 0 pushed along +X by len.
TopoDS_Shape prismYZ(const std::vector<glm::dvec2>& pts, double len) {
    BRepBuilderAPI_MakePolygon mp;
    for (auto& p : pts) mp.Add(gp_Pnt(0, p.x, p.y));
    mp.Close();
    BRepBuilderAPI_MakeFace mf(mp.Wire(), Standard_True);
    if (!mf.IsDone()) throw std::runtime_error("Could not build the miter");
    return BRepPrimAPI_MakePrism(mf.Face(), gp_Vec(len, 0, 0)).Shape();
}

TopoDS_Shape boxJ(double x0, double y0, double z0, double x1, double y1, double z1) {
    return BRepPrimAPI_MakeBox(gp_Pnt(std::min(x0, x1), std::min(y0, y1), std::min(z0, z1)), std::abs(x1 - x0), std::abs(y1 - y0),
                               std::abs(z1 - z0)).Shape();
}

TopoDS_Shape unifyJ(const TopoDS_Shape& s) {
    try {
        ShapeUpgrade_UnifySameDomain u(s, Standard_True, Standard_True, Standard_False);
        u.Build();
        return u.Shape().IsNull() ? s : u.Shape();
    } catch (...) {
        return s;
    }
}

TopoDS_Shape boolJ(bool fuse, const TopoDS_Shape& a, const std::vector<TopoDS_Shape>& tools) {
    TopTools_ListOfShape args, tl;
    args.Append(a);
    for (auto& t : tools)
        if (!t.IsNull()) tl.Append(t);
    if (tl.IsEmpty()) return a;
    TopoDS_Shape r;
    if (fuse) {
        BRepAlgoAPI_Fuse op;
        op.SetArguments(args);
        op.SetTools(tl);
        op.Build();
        if (!op.IsDone()) throw std::runtime_error("Joining the dovetail failed");
        r = op.Shape();
    } else {
        BRepAlgoAPI_Cut op;
        op.SetArguments(args);
        op.SetTools(tl);
        op.Build();
        if (!op.IsDone()) throw std::runtime_error("Cutting the dovetail failed");
        r = op.Shape();
    }
    return unifyJ(r);
}

TopoDS_Shape compound(const std::vector<TopoDS_Shape>& list) {
    BRep_Builder b;
    TopoDS_Compound c;
    b.MakeCompound(c);
    for (auto& s : list) b.Add(c, s);
    return c;
}

TopoDS_Shape movedY(const TopoDS_Shape& s, double dy) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, dy, 0));
    return s.Moved(TopLoc_Location(t));
}

// One connector, male (grow = 0) or female (grow = clearance), seam at u = 0.
TopoDS_Shape connector(const DovetailSpec& s, double grow, bool female) {
    const double t = std::tan(s.angleDeg * kPiJ / 180.0);
    const double c = grow;
    const double ov = std::clamp(s.length * 0.25, 0.5, 2.0);   // reaches back into the first body
    const double e = female ? std::max(c, 0.02) : 0.0;   // the socket opens past both faces
    const double z0 = std::min(0.0, s.depth) - e, z1 = std::max(0.0, s.depth) + e;
    double w = s.width + 2 * c;
    switch (s.type) {
    case DovetailType::Key: {
        double h = s.length / 2 + c;                 // half length
        double W = s.width + s.length * t + 2 * c;   // wide ends
        return prismXY({{-h, -W / 2}, {0, -w / 2}, {h, -W / 2}, {h, W / 2}, {0, w / 2}, {-h, W / 2}}, z0, z1);
    }
    case DovetailType::Tab:
    case DovetailType::Sliding: {
        double L = s.length + c;
        double W = s.width + 2 * s.length * t + 2 * c;
        return prismXY({{-ov, -w / 2}, {0, -w / 2}, {L, -W / 2}, {L, W / 2}, {0, w / 2}, {-ov, w / 2}}, z0, z1);
    }
    case DovetailType::Puzzle: {
        double r = s.head / 2 + c;
        double cu = s.length - s.head / 2;
        if (cu <= 0) throw std::runtime_error("The puzzle head is longer than the tab");
        TopoDS_Shape neck = prismXY({{-ov, -w / 2}, {cu, -w / 2}, {cu, w / 2}, {-ov, w / 2}}, z0, z1);
        TopoDS_Shape head = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(cu, 0, z0), gp_Dir(0, 0, 1)), r, z1 - z0).Shape();
        return boolJ(true, neck, {head});
    }
    default: break;
    }
    throw std::runtime_error("Not a connector");
}

TopoDS_Shape row(const DovetailSpec& s, double grow, bool female) {
    TopoDS_Shape one = connector(s, grow, female);
    int n = std::clamp(s.count, 1, 200);
    if (n == 1) return one;
    std::vector<TopoDS_Shape> copies;
    for (int i = 0; i < n; ++i) copies.push_back(movedY(one, (i - (n - 1) / 2.0) * s.spacing));
    return compound(copies);
}

void woodJoint(const DovetailSpec& s, DovetailParts& out) {
    const double W = s.boardW, Tt = s.tailT, Tp = s.pinT, c = s.clearance;
    const double t = std::tan(s.angleDeg * kPiJ / 180.0);
    if (W <= 0 || Tt <= 0 || Tp <= 0 || s.tailL <= Tp || s.pinL <= Tt) throw std::runtime_error("Board sizes must be above 0 (and longer than the other board is thick)");
    double lf = 0, lt = 0;   // laps: on the pin board's face, on the tail board's face
    if (s.type == DovetailType::HalfBlind) lf = s.lap;
    if (s.type == DovetailType::SecretMitered) lf = lt = s.lap;
    if (lf >= Tp - 1 || lt >= Tt - 1) throw std::runtime_error("The lap must be thinner than the boards");
    const int n = std::clamp(s.tails, 1, 50);
    const double D = Tp - lf;                // tail length
    const double sp = W / n;
    const double tw = sp - s.pinW;           // tail width at its wide end
    const double tn = tw - 2 * D * t;        // and at the shoulder
    if (s.pinW <= 0 || tw <= 0) throw std::runtime_error("The pins are too wide for that many tails");
    if (tn < 1) throw std::runtime_error("The tails get too narrow at the shoulder: use fewer tails, narrower pins or a smaller angle");
    std::vector<TopoDS_Shape> tails, sockets;
    for (int i = 0; i < n; ++i) {
        double x = (i + 0.5) * sp;
        tails.push_back(prismXY({{x - tn / 2, 0.5}, {x - tn / 2, 0}, {x - tw / 2, -D}, {x + tw / 2, -D}, {x + tn / 2, 0}, {x + tn / 2, 0.5}}, lt, Tt));
        double yEnd = lf > 0 ? -D - c : -Tp - 1;
        double grow = c + (yEnd - -D) * -t;   // keep the flank line, wider past the end
        sockets.push_back(prismXY({{x - tn / 2 - c, 0.5}, {x - tn / 2 - c, 0}, {x - tw / 2 - grow, yEnd}, {x + tw / 2 + grow, yEnd},
                                   {x + tn / 2 + c, 0}, {x + tn / 2 + c, 0.5}},
                                  lt > 0 ? lt - c : -1, Tt));
    }
    TopoDS_Shape tailBoard = boxJ(0, 0, 0, W, s.tailL - Tp, Tt);
    TopoDS_Shape pinBoard = boxJ(0, -Tp, 0, W, 0, s.pinL);
    std::vector<TopoDS_Shape> addTail = tails, cutPin = sockets;
    if (lt > 0) {
        // Mitered corner: the tail board's lip under the pin board, split on the 45 degree line.
        TopoDS_Shape wedge = prismYZ({{-Tp, 0}, {0, 0}, {0, lt}, {-Tp + lt, lt}}, W);
        addTail.push_back(wedge);
        cutPin.push_back(wedge);
    }
    out.bodies.push_back({"Tail board", wrapJ(boolJ(true, tailBoard, addTail))});
    out.bodies.push_back({"Pin board", wrapJ(boolJ(false, pinBoard, cutPin))});
}

void machineSlide(const DovetailSpec& s, DovetailParts& out) {
    const double Wb = s.baseW, Hb = s.baseH, wt = s.chanW, d = s.chanD, g = s.gib, c = s.clearance, L = s.slideLen;
    if (Wb <= 0 || Hb <= 0 || wt <= 0 || d <= 0 || L <= 0 || s.slideW <= 0 || s.slideH <= 0) throw std::runtime_error("Every slide size must be above 0");
    if (d >= Hb - 1) throw std::runtime_error("The channel must be shallower than the base");
    if (s.flankDeg < 20 || s.flankDeg > 85) throw std::runtime_error("The flank angle must be 20 to 85 degrees (60 is usual)");
    const double k = 1.0 / std::tan(s.flankDeg * kPiJ / 180.0);
    const double wb = wt + 2 * d * k;
    if (wb + g >= Wb - 2) throw std::runtime_error("The channel is too wide for the base");
    const double yb = Hb - d;
    auto f = [&](double y) { return wb / 2 - (y - yb) * k; };   // channel half width at height y
    if (wt / 2 - c <= 0.5) throw std::runtime_error("The channel opening is too narrow");
    TopoDS_Shape base = boxJ(-Wb / 2, 0, 0, Wb / 2, Hb, L);
    TopoDS_Shape chan = prismXY({{-wb / 2, yb}, {wb / 2 + g, yb}, {wt / 2 + g, Hb}, {wt / 2 + g, Hb + 1}, {-wt / 2, Hb + 1}, {-wt / 2, Hb}}, -1, L + 1);
    out.bodies.push_back({"Base", wrapJ(boolJ(false, base, {chan}))});
    double y0 = yb + c, y1 = Hb + c + 0.5;
    TopoDS_Shape block = boxJ(-s.slideW / 2, Hb + c, 0, s.slideW / 2, Hb + c + s.slideH, L);
    TopoDS_Shape tongue = prismXY({{-f(y0) + c, y0}, {f(y0) - c, y0}, {f(y1) - c, y1}, {-f(y1) + c, y1}}, 0, L);
    out.bodies.push_back({"Slide", wrapJ(boolJ(true, block, {tongue}))});
    if (g > 2 * c + 0.2) {
        double gy1 = Hb;
        out.bodies.push_back({"Gib", wrapJ(prismXY({{f(y0), y0}, {f(y0) + g - c, y0}, {f(gy1) + g - c, gy1}, {f(gy1), gy1}}, 0, L))});
    }
}

} // namespace

bool dovetailIsConnector(DovetailType t) {
    return t == DovetailType::Key || t == DovetailType::Tab || t == DovetailType::Sliding || t == DovetailType::Puzzle;
}

DovetailParts makeDovetail(const DovetailSpec& s) {
    DovetailParts out;
    try {
        if (dovetailIsConnector(s.type)) {
            if (s.length <= 0 || s.width <= 0 || std::abs(s.depth) < 1e-3) throw std::runtime_error("Length, width and depth must be above 0");
            if (s.clearance < 0 || s.clearance > 5) throw std::runtime_error("The clearance must be 0 to 5 mm");
            if (s.angleDeg < 0 || s.angleDeg > 40) throw std::runtime_error("The dovetail angle must be 0 to 40 degrees");
            if (s.type == DovetailType::Puzzle && (s.head <= s.width)) throw std::runtime_error("The puzzle head must be wider than the neck");
            if (s.count > 1 && s.spacing <= 0) throw std::runtime_error("The spacing must be above 0");
            out.male = wrapJ(row(s, 0, false));
            out.female = wrapJ(row(s, s.clearance, true));
        } else if (s.type == DovetailType::MachineSlide) {
            machineSlide(s, out);
        } else {
            woodJoint(s, out);
        }
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Dovetail geometry failed: ") + (e.GetMessageString() ? e.GetMessageString() : "kernel error"));
    }
    return out;
}

} // namespace kst::kernel
