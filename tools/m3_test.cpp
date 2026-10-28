// Standalone M3 smoke test: exercises the parameter evaluator, sketch profile extraction,
// extrude / revolve / boolean kernel paths and the .kst save / load round trip with the real
// OpenCASCADE. Console app, no GPU needed. Build and run with tools/m3_test.bat.
#include "core/Curves.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/SaveLoad.h"
#include "kernel/Features.h"
#include "kernel/ShapeHandle.h"
#include "sketch/Profiles.h"
#include "sketch/Sketch.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace kst;
static int g_fail = 0, g_pass = 0;

void check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; std::printf("  ok   %s\n", what.c_str()); }
    else { ++g_fail; std::printf("  FAIL %s\n", what.c_str()); }
}
void checkNear(double got, double want, double tol, const std::string& what) {
    bool ok = std::abs(got - want) <= tol;
    char b[160];
    std::snprintf(b, sizeof b, "%s (got %.3f, want %.3f)", what.c_str(), got, want);
    check(ok, b);
}

// Runs a section; kernel throws are reported as failures instead of killing the process.
template <typename F>
void runSection(const char* name, F&& f) {
    std::printf("%s\n", name);
    try {
        f();
    } catch (const std::exception& e) {
        ++g_fail;
        std::printf("  FAIL %s threw: %s\n", name, e.what());
    }
}
#define SECTION(name, ...) runSection(name, [&]() -> void { __VA_ARGS__ })

double volume(const ShapePtr& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s->shape, g);
    return g.Mass();
}
int solids(const ShapePtr& s) {
    int n = 0;
    for (TopExp_Explorer e(s->shape, TopAbs_SOLID); e.More(); e.Next()) ++n;
    return n;
}

// Sketch with one rectangle (w x h at origin) and optionally a circle hole at its center.
sk::Sketch rectSketch(double w, double h, double holeR = 0.0) {
    sk::Sketch s;
    uint32_t p1 = s.addPoint({0, 0}), p2 = s.addPoint({w, 0}), p3 = s.addPoint({w, h}), p4 = s.addPoint({0, h});
    s.addLine(p1, p2);
    s.addLine(p2, p3);
    s.addLine(p3, p4);
    s.addLine(p4, p1);
    if (holeR > 0) {
        uint32_t c = s.addPoint({w / 2, h / 2});
        s.addCircle(c, holeR);
    }
    return s;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    SECTION("params", {
        ParamMap pm;
        pm["width"] = Param{"width", "40", "", 0};
        pm["thk"] = Param{"thk", "width / 8", "", 0};
        std::string err;
        check(updateParamValues(pm, err), "resolve params");
        checkNear(pm["width"].value, 40, 1e-9, "width = 40");
        checkNear(pm["thk"].value, 5, 1e-9, "thk = width/8");
        checkNear(peval::eval("thk * 2 + 1", pm), 11, 1e-9, "expr thk*2+1");
        checkNear(peval::eval("sin(30)", pm), 0.5, 1e-9, "sin in degrees");
        checkNear(peval::eval("sqrt(2)^2", pm), 2, 1e-9, "sqrt and power");
        check(peval::names("width + thk").size() == 2, "collect names");
        check(peval::plainNumber("12.5") && !peval::plainNumber("width"), "plain number check");
        pm["bad"] = Param{"bad", "nope + 1", "", 0};
        check(!updateParamValues(pm, err) && !err.empty(), "unknown name reported");
    });

    std::printf("profiles\n");
    sk::Sketch rs = rectSketch(20, 10, 3);
    std::vector<sk::Profile> profs = sk::findProfiles(rs);
    check(profs.size() == 2, "rect + inner circle = 2 regions (ring and disk), like Fusion");
    const sk::Profile* ring = nullptr;
    const sk::Profile* disk = nullptr;
    for (auto& p : profs) {
        if (p.holes.empty()) disk = &p;
        else ring = &p;
    }
    check(ring && ring->holes.size() == 1, "one region has the circle as a hole");
    check(disk && disk->area > 0, "the other region is the disk itself");
    std::vector<RegionLoops> regions = sketchRegions(rs);
    check(regions.size() == 2, "two world regions");

    // Find the ring region (2 loops) for the geometry tests.
    const RegionLoops* ringLoops = nullptr;
    for (auto& r : regions)
        if (r.size() == 2) ringLoops = &r;
    check(ringLoops != nullptr, "ring region has outer + hole loops");

    SECTION("extrude", {
        ShapePtr solid = kernel::extrudeRegion(*ringLoops, {0, 0, 1}, 5, false, 0);
        check((bool)solid, "extrude builds");
        check(solids(solid) == 1, "one solid");
        checkNear(volume(solid), (20 * 10 - 9 * M_PI) * 5, 0.05, "volume with hole");
        ShapePtr sym = kernel::extrudeRegion(*ringLoops, {0, 0, 1}, 6, true, 0);
        checkNear(volume(sym), (20 * 10 - 9 * M_PI) * 6, 0.05, "symmetric = full distance");
        ShapePtr tap = kernel::extrudeRegion(*ringLoops, {0, 0, 1}, 5, false, 15);
        check((bool)tap && volume(tap) > 920, "taper widens");
        ShapePtr diskOnly = kernel::extrudeRegion(regions[0].size() == 1 ? regions[0] : regions[1], {0, 0, 1}, 5, false, 0);
        checkNear(volume(diskOnly), 9 * M_PI * 5, 0.05, "plain circle extrude");
        bool threw = false;
        try { kernel::extrudeRegion(*ringLoops, {0, 0, 1}, 0, false, 0); }
        catch (const std::runtime_error&) { threw = true; }
        check(threw, "zero distance rejected");
    });

    SECTION("disjoint profiles", {
        sk::Sketch s;
        uint32_t c1 = s.addPoint({-15, 0});
        s.addCircle(c1, 2);
        uint32_t c2 = s.addPoint({15, 0});
        s.addCircle(c2, 2);
        auto regs = sketchRegions(s);
        check(regs.size() == 2, "two circles = two regions");
    });

    SECTION("revolve", {
        sk::Sketch s;
        uint32_t p1 = s.addPoint({5, 0}), p2 = s.addPoint({10, 0}), p3 = s.addPoint({10, 4}), p4 = s.addPoint({5, 4});
        s.addLine(p1, p2);
        s.addLine(p2, p3);
        s.addLine(p3, p4);
        s.addLine(p4, p1);
        auto regs = sketchRegions(s);
        check(regs.size() == 1, "one region");
        ShapePtr ring = kernel::revolveRegion(regs[0], {0, 0, 0}, {0, 1, 0}, 360);
        check((bool)ring, "revolve builds");
        checkNear(volume(ring), M_PI * (100 - 25) * 4, 0.05, "annulus volume");
        ShapePtr half = kernel::revolveRegion(regs[0], {0, 0, 0}, {0, 1, 0}, 180);
        checkNear(volume(half), M_PI * (100 - 25) * 4 / 2, 0.05, "half revolve");
        bool threw = false;
        try { kernel::revolveRegion(regs[0], {0, 0, 0}, {0, 1, 0}, 400); }
        catch (const std::runtime_error&) { threw = true; }
        check(threw, "angle over 360 rejected");
    });

    SECTION("boxcut probe", {
        auto mk = [](double x) {
            auto h = std::make_shared<ShapeHandle>();
            h->shape = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), x, 10, 5).Shape();
            return h;
        };
        ShapePtr ba = mk(20), bb = mk(10);
        // Cut removes the tool (bb) from the target (ba): 20x10 - 10x10 overlap = 500.
        auto cut = kernel::applyBool(kernel::BoolOp::Cut, bb, {{ba, glm::mat4(1.f)}});
        check(cut.size() == 1 && solids(cut[0]) == 1, "box cut keeps solids");
        if (!cut.empty()) checkNear(volume(cut[0]), 500, 0.05, "box cut volume");
    });

    SECTION("booleans", {
        ShapePtr a = kernel::extrudeRegion(sketchRegions(rectSketch(20, 10))[0], {0, 0, 1}, 5, false, 0);
        ShapePtr b = kernel::extrudeRegion(sketchRegions(rectSketch(10, 10))[0], {0, 0, 1}, 5, false, 0);
        std::printf("  .. a: solids=%d vol=%.1f   b: solids=%d vol=%.1f\n", solids(a), volume(a), solids(b), volume(b));
        kernel::BoolTarget ta{a, glm::mat4(1.f)};
        auto cut = kernel::applyBool(kernel::BoolOp::Cut, b, {ta});
        check(cut.size() == 1, "cut output");
        checkNear(volume(cut[0]), 500, 0.05, "cut volume");
        auto fused = kernel::applyBool(kernel::BoolOp::Join, b, {ta});
        checkNear(volume(fused[0]), 20 * 10 * 5, 0.05, "join volume (b inside a)");
        auto common = kernel::applyBool(kernel::BoolOp::Intersect, b, {ta});
        checkNear(volume(common[0]), 10 * 10 * 5, 0.05, "intersect volume");
        kernel::BoolTarget moved{b, glm::translate(glm::mat4(1.f), glm::vec3(30, 0, 0))};
        bool threw = false;
        try { kernel::applyBool(kernel::BoolOp::Intersect, b, {moved}); }
        catch (const std::runtime_error&) { threw = true; }
        check(threw, "no overlap raises a clean error");
    });

    SECTION("save / load", {
        Document d;
        d.name = "TestPart";
        d.params["len"] = Param{"len", "40", "part length", 40};
        Feature sk;
        sk.id = d.newId();
        sk.type = FeatureType::Sketch;
        sk.name = "Sketch1";
        sk.sketchId = 77;
        d.timeline.push_back(sk);
        d.markerPos = 1;
        sk::Sketch s = rectSketch(20, 10);
        s.id = 77;
        s.featureId = sk.id;
        s.name = "Sketch1";
        s.cons[900] = sk::Constraint{900, sk::CType::Distance, 1, 2, 0, 25, 0, "d1"};
        d.sketches[77] = s;
        Feature ex;
        ex.id = d.newId();
        ex.type = FeatureType::Extrude;
        ex.name = "Extrude1";
        auto xp = std::make_shared<ExtrudeParams>();
        xp->sketchId = 77;
        xp->distExpr = "len / 4";
        xp->taperExpr = "2";
        xp->op = 2;
        xp->targets = {{sk.id, 0}};
        ex.extrude = xp;
        d.timeline.push_back(ex);
        d.markerPos = 2;
        d.setOverride(ex.id, 0, BodyOverride{"MyBody", {1, 0, 0}, true, false});

        std::string path = "m3_test_roundtrip.kst";
        check(saveDocument(d, path), "save");
        LoadResult r = loadDocument(path);
        check(r.ok, "load ok: " + r.error);
        if (r.ok) {
            check(r.doc.name == "TestPart", "name");
            check(r.doc.markerPos == 2, "marker");
            check(r.doc.params.count("len") && r.doc.params["len"].expr == "40", "param");
            check(r.doc.timeline.size() == 2 && r.doc.timeline[1].extrude, "features");
            auto& x2 = r.doc.timeline[1].extrude;
            check(x2->distExpr == "len / 4" && x2->op == 2 && x2->targets == std::vector<TargetRef>{{sk.id, 0}}, "extrude params");
            size_t baseEnts = sk::Sketch().ents.size();
            check(r.doc.sketches.count(77) && r.doc.sketches[77].ents.size() == baseEnts + 8,
                  "sketch entities (got " + std::to_string(r.doc.sketches.count(77) ? r.doc.sketches[77].ents.size() : 0) + ")");
            check(r.doc.sketches[77].cons.count(900) && r.doc.sketches[77].cons[900].value == 25, "sketch constraint");
            const BodyOverride* o = r.doc.findOverride(ex.id, 0);
            check(o && o->name == "MyBody" && !o->visible, "override");
        }
        std::remove(path.c_str());
    });

    SECTION("M4 fillet / chamfer", {
        ShapePtr box = kernel::extrudeRegion(sketchRegions(rectSketch(20, 10))[0], {0, 0, 1}, 5, false, 0);
        checkNear(volume(box), 1000, 0.05, "box volume");
        uint32_t ne = kernel::edgeCount(box);
        check(ne == 12, "box edge count");
        std::vector<glm::dvec3> poly;
        check(kernel::edgePolyline(box, 0, poly) && poly.size() >= 2, "edge polyline");

        std::vector<uint32_t> all(ne);
        for (uint32_t i = 0; i < ne; ++i) all[i] = i;
        kernel::FilletReport rep;
        ShapePtr fil = kernel::filletEdges(box, all, 1.0, rep);
        check((bool)fil && !rep.partial, "fillet all edges at r=1");
        double v = volume(fil);
        check(v < 1000 && v > 900, "fillet removes material");

        ShapePtr cham = kernel::chamferEdges(box, all, 0.5);
        check((bool)cham && volume(cham) < 1000, "chamfer all edges");

        ShapePtr plate = kernel::extrudeRegion(sketchRegions(rectSketch(20, 10))[0], {0, 0, 1}, 1, false, 0);
        kernel::FilletReport rep2;
        try {
            ShapePtr f2 = kernel::filletEdges(plate, all, 8.0, rep2);
            check((bool)f2 && rep2.partial && !rep2.failed.empty(), "impossible fillet falls back with report");
        } catch (const std::runtime_error& e) {
            check(std::string(e.what()).find("max") != std::string::npos, "fillet error explains max radius");
        }
        double mr = kernel::maxFilletRadius(plate, all, 8.0);
        check(mr > 0 && mr < 1.0, "max radius finder bounded by thickness");
    });

    SECTION("M4 shell / press pull / split / mirror", {
        ShapePtr box = kernel::extrudeRegion(sketchRegions(rectSketch(20, 10))[0], {0, 0, 1}, 5, false, 0);
        ShapePtr shelled = kernel::shellBody(box, {}, 1.0);
        check((bool)shelled && volume(shelled) < 1000 && volume(shelled) > 400, "closed shell keeps walls");

        ShapePtr pulled = kernel::offsetFaces(box, {0}, 2.0);
        check((bool)pulled, "press pull face 0");

        auto pieces = kernel::splitByPlane(box, {0, 0, 2.5}, {0, 0, 1});
        check(pieces.size() == 2, "split in two");
        if (pieces.size() == 2)
            checkNear(volume(pieces[0]) + volume(pieces[1]), 1000, 0.1, "pieces sum to original");

        ShapePtr mir = kernel::mirrorShape(box, {0, 0, 0}, {1, 0, 0});
        check((bool)mir && solids(mir) == 1, "mirror solid");
        checkNear(volume(mir), 1000, 0.05, "mirror volume");
    });

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
