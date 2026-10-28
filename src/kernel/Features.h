#pragma once
#include "core/Curves.h"
#include "kernel/Kernel.h"

namespace kst::kernel {

// Builds solid geometry from sketch regions. All functions run on worker threads and throw
// std::runtime_error with a readable message on failure; the caller catches and shows it.

enum class BoolOp : uint8_t { NewBody, Join, Cut, Intersect };

struct BoolTarget {
    ShapePtr shape;
    glm::mat4 xform{1.f};
};

// Face from one region (loops[0] outer, rest holes) on its plane.
ShapePtr buildFace(const RegionLoops& loops);

// Straight or tapered extrusion of one region. dir is unit length; dist > 0.
// taperDeg > 0 widens away from the sketch plane. symmetric extrudes dist/2 each way.
ShapePtr extrudeRegion(const RegionLoops& loops, const glm::dvec3& dir, double dist, bool symmetric, double taperDeg);

// Thin wall from an open chain, extruded `dist` along `dir`. side 0 centers the thickness on the
// chain, side 1 puts it all on the left of the chain (left is cross(plane normal, tangent)).
ShapePtr ribExtrusion(const std::vector<CurveSeg>& chain, const glm::dvec3& planeN, double thickness, int side, const glm::dvec3& dir, double dist);

// Closed profile of a wall around an open chain. side 0 centers it, side 1 puts it all on the
// left, side 2 puts it all on the right. The caller extrudes, revolves, or sweeps the result.
RegionLoops thinProfile(const std::vector<CurveSeg>& chain, const glm::dvec3& planeN, double thickness, int side);

// Extrudes the rib from the sketch plane along `dir` through `target`, keeps the piece on the
// sketch side of the body, and joins it. The result is in world space.
ShapePtr ribToNext(const std::vector<CurveSeg>& chain, const glm::dvec3& planeO, const glm::dvec3& planeN,
                   double thickness, int side, const glm::dvec3& dir, const ShapePtr& target, const glm::mat4& xform);

// Web: the open chain is the side view. Thickness sticks out of the sketch plane. The wall grows
// in the sketch plane, perpendicular to the chain, until it meets the body. flip reverses that growth.
ShapePtr webExtrusion(const std::vector<CurveSeg>& chain, const glm::dvec3& planeN, double thickness, int side, bool flip, double dist);
ShapePtr webToNext(const std::vector<CurveSeg>& chain, const glm::dvec3& planeO, const glm::dvec3& planeN,
                   double thickness, int side, bool flip, const ShapePtr& target, const glm::mat4& xform);

// Projects closed profiles along `dir` onto `target`. `deboss` cuts the depth into the body.
// Otherwise the depth is added on the near side of the hit. Result is in world space.
ShapePtr embossOnBody(const std::vector<RegionLoops>& regions, const glm::dvec3& planeO, const glm::dvec3& dir,
                      double depth, bool deboss, const ShapePtr& target, const glm::mat4& xform);

// Cylinder standing on `origin`, growing along `axis` by `height`, joined to the body.
// holeDia 0 is solid. A smaller hole is cut through the boss and a hair into the body.
ShapePtr bossOnBody(const glm::dvec3& origin, const glm::dvec3& axis, double diameter, double height, double holeDia,
                    const ShapePtr& target, const glm::mat4& xform);

// Cantilever hook joined to the body. `origin` is the center of the beam base.
// The catch sticks out at the tip, across the thickness. hook 0 is a plain beam.
// On Z the catch goes toward +X. On X it goes toward +Y. On Y it goes toward +Z.
ShapePtr snapOnBody(const glm::dvec3& origin, const glm::dvec3& axis, double length, double width, double thick,
                    double hook, double hookLen, const ShapePtr& target, const glm::mat4& xform);

// Flat pad. The profile is one face. Thickness grows along dir and is joined to the body.
// A hair of the pad is buried so the join does not rely on a shared face.
ShapePtr restOnBody(const std::vector<RegionLoops>& regions, const glm::dvec3& dir, double thickness,
                    const ShapePtr& target, const glm::mat4& xform);

// Ridge around a closed profile. Width is the inward thickness of the ridge. Height grows along dir.
// groove cuts the ridge into the body instead of adding it.
ShapePtr lipOnBody(const std::vector<RegionLoops>& regions, const glm::dvec3& dir, double width, double height,
                   bool groove, const ShapePtr& target, const glm::mat4& xform);

// Revolution of one region around (axisO + t*axisD). angleDeg in [0, 360].
ShapePtr revolveRegion(const RegionLoops& loops, const glm::dvec3& axisO, const glm::dvec3& axisD, double angleDeg);

// Boolean combination. NewBody: {tool}. Join: one fused body. Cut: tool cut out of every
// target (one body per target). Intersect: common of tool and every target.
std::vector<ShapePtr> applyBool(BoolOp op, const ShapePtr& tool, const std::vector<BoolTarget>& targets);

// ---- M4: solid modification ----------------------------------------------------------

// Edge / face indices refer to the shape's TopExp indexed map (what tessellate and the
// renderer's ID buffer already use). Counts are stored with features so a changed body is
// detected instead of silently filleting the wrong edges.
uint32_t edgeCount(const ShapePtr& shape);
uint32_t faceCount(const ShapePtr& shape);
uint32_t vertexCount(const ShapePtr& shape);
// World position of a corner. vertex is the index from vertexCount.
bool vertexPoint(const ShapePtr& shape, const glm::mat4& xf, uint32_t vertex, glm::dvec3& world);
void vertexPoints(const ShapePtr& shape, const glm::mat4& xf, std::vector<glm::dvec3>& out);
// Discretized edge for picking and highlight overlays. False if the edge does not exist.
// Every edge's polyline in one pass (edge index order). Big bodies: one map, not one per edge.
void edgePolylines(const ShapePtr& shape, uint32_t maxEdges, std::vector<std::vector<glm::dvec3>>& out);
// Indices (as edgePolylines / filletEdges count them) of seam edges: the line where a round face
// closes on itself. They are not real edges and never get rounded.
std::vector<uint32_t> seamEdges(const ShapePtr& shape);
bool edgePolyline(const ShapePtr& shape, uint32_t edgeIndex, std::vector<glm::dvec3>& out);

struct FilletReport {
    struct FailedEdge { uint32_t edge = 0; double maxRadius = 0; };
    std::vector<FailedEdge> failed;   // edges that could not take the radius, with the best found
    bool partial = false;             // some edges succeeded, some did not
};

// Constant radius fillet. Tries all edges at once (best quality); when that fails it applies
// edges one by one and reports each failing edge with the largest radius that worked, so the
// user always gets a result and an explanation. Throws only if nothing could be filleted.
// endRadius <= 0, or equal to radius, keeps a constant fillet. Otherwise the radius changes
// linearly from radius at the edge's first vertex to endRadius at its last.
ShapePtr filletEdges(const ShapePtr& shape, const std::vector<uint32_t>& edges, double radius, FilletReport& report, double endRadius = 0);

// Replaces one flat face with a round tangent to the two parallel faces beside it.
// The radius is half the distance between those faces. A face that is not between
// two parallel faces is refused.
ShapePtr fullRoundFace(const ShapePtr& shape, uint32_t face);

// Chamfer. dist2 left at 0, or equal to dist, cuts the same distance on both faces.
// A different dist2 cuts that distance along the other face. Swap the two numbers to swap the faces.
ShapePtr chamferEdges(const ShapePtr& shape, const std::vector<uint32_t>& edges, double dist, double dist2 = 0);

// Hollows the solid, removing the listed faces (empty list = closed cavity). Thickness inward.
ShapePtr shellBody(const ShapePtr& shape, const std::vector<uint32_t>& removeFaces, double thickness, int side = 0);

// Moves the listed faces by offset (press pull). Positive grows the body.
ShapePtr offsetFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces, double offset);
// Fusion's Delete Face: removes faces and heals the solid around them (fillets, holes, bosses).
ShapePtr deleteFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces);

// Splits by an infinite plane. Returns one shape per resulting piece (may be empty on no cut).
std::vector<ShapePtr> splitByPlane(const ShapePtr& shape, const glm::dvec3& p, const glm::dvec3& normal);

// Cuts the body on the outline seen along view. The outline has to lie in one plane.
// Pieces are in world space.
std::vector<ShapePtr> silhouetteSplit(const ShapePtr& target, const glm::dvec3& view, const glm::mat4& xform);
// Divides faces where the plane crosses them. One solid comes back, with more faces. faces empty = every face.
ShapePtr splitFaces(const ShapePtr& shape, const glm::dvec3& p, const glm::dvec3& normal, const std::vector<uint32_t>& faces);
// Divides faces where a sketch curve, projected along dir, crosses them. One solid comes back.
ShapePtr splitFacesByCurve(const ShapePtr& shape, const std::vector<CurveSeg>& segs, const glm::dvec3& dir, const std::vector<uint32_t>& faces);

// Edges bounding a face, and the chain of edges tangent-connected to an edge (Fusion's
// "tangent chain"), as edge indices.
std::vector<uint32_t> faceEdges(const ShapePtr& shape, uint32_t face);
std::vector<uint32_t> tangentChain(const ShapePtr& shape, uint32_t edge);
// Centre and outward normal of a face (shape coordinates). planar tells whether it is flat.
// Midpoint of an edge and the outward direction between its two faces (fillet / chamfer arrow).
// Axis of an edge: a circle or arc gives its center and normal, a straight edge its own line.
bool edgeAxis(const ShapePtr& shape, uint32_t edge, glm::dvec3& origin, glm::dvec3& dir);
bool edgeFrame(const ShapePtr& shape, uint32_t edge, glm::dvec3& mid, glm::dvec3& outward);
bool faceInfo(const ShapePtr& shape, uint32_t face, glm::dvec3& center, glm::dvec3& normal, bool& planar);
// Extrudes a flat face of a body (with the body's transform applied) along its outward normal.
// dist may be negative (into the body). Result is in world coordinates.
ShapePtr extrudeFace(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, double dist, bool symmetric);

// A new solid of the given thickness built on the listed faces. The source body is not changed.
// side 0 grows along the outward normal, 1 the other way, 2 half on each side. Result is in world space.
ShapePtr thickenFaces(const ShapePtr& shape, const glm::mat4& xf, const std::vector<uint32_t>& faces, double thickness, int side);

// Copy of the shape with xf baked into its geometry (world coordinates).
ShapePtr placeShape(const ShapePtr& shape, const glm::mat4& xf);

// Exact description of one edge in world space (for projecting into sketches).
struct EdgeGeom {
    int type = 3;                    // 0 line, 1 full circle, 2 circular arc, 3 other (use pts)
    glm::dvec3 a{0}, b{0};           // ends (line / arc: a = start, b = end along the edge)
    glm::dvec3 center{0}, normal{0, 0, 1};   // circle / arc axis: arc runs CCW about normal from a to b
    double radius = 0;
    std::vector<glm::dvec3> pts;     // polyline (all types)
};
bool edgeGeom(const ShapePtr& shape, const glm::mat4& xf, uint32_t edge, EdgeGeom& out);

// Edges where the placed solid crosses the plane. Empty when the plane misses the body.
std::vector<EdgeGeom> sectionEdges(const ShapePtr& shape, const glm::mat4& xf, const glm::dvec3& origin, const glm::dvec3& normal);

// A curve in world space, dropped onto a face. 0 line (a to b), 1 full circle, 2 arc from a to b
// counter clockwise about normal.
struct DropCurve {
    int type = 0;
    glm::dvec3 a{0}, b{0}, center{0}, normal{0, 0, 1};
    double radius = 0;
};

// Where those curves meet the placed face, traveling along direction. The sign of direction does
// not matter. Empty when nothing hits the face.
std::vector<EdgeGeom> projectOnFace(const ShapePtr& shape, const glm::mat4& xf, uint32_t face,
                                    const std::vector<DropCurve>& curves, const glm::dvec3& direction);

// World bounding box of a shape placed with xf.
Aabb shapeBounds(const ShapePtr& shape, const glm::mat4& xf);
// True when the two placed shapes overlap or touch (distance ~0 or one inside the other).
bool shapesTouch(const ShapePtr& a, const glm::mat4& xa, const ShapePtr& b, const glm::mat4& xb);
// True when the world point lies inside the placed solid.
bool pointInside(const ShapePtr& shape, const glm::mat4& xf, const glm::dvec3& world);
// Face of the placed shape that passes closest to the world point (within tol), or -1.
int faceNear(const ShapePtr& shape, const glm::mat4& xf, const glm::dvec3& world, double tol);

// Mirrored copy of the shape across the plane through o with the given normal.
ShapePtr mirrorShape(const ShapePtr& shape, const glm::dvec3& o, const glm::dvec3& n);

// Largest fillet radius that works on all the given edges, searching from `upTo` downward.
double maxFilletRadius(const ShapePtr& shape, const std::vector<uint32_t>& edges, double upTo);

// Bounding box diagonal, for defaults like through hole depth.
double shapeDiag(const ShapePtr& shape);
// Solid volume (mm^3) and surface area (mm^2). Volume is signed; a good solid is positive.
double solidVolume(const ShapePtr& shape);
// BRepCheck: true when the solid is well formed (for KESTRAL_CHECK diagnostics).
bool shapeIsValid(const ShapePtr& shape);
double solidArea(const ShapePtr& shape);
// Face kind: plane, cylinder, cone, sphere, torus, other, or missing. radius is 0 when it has none
// (a cone reports its reference radius, a torus its tube radius).
const char* faceKind(const ShapePtr& shape, uint32_t face, double& radius);

// Round wire wound about `axis` through `origin`. diameter is the centerline, sectionDiameter the wire.
ShapePtr makeCoil(const glm::dvec3& origin, const glm::dvec3& axis, double diameter, double height,
                  double turns, double sectionDiameter, bool leftHand);

// Pipe along a path. outerR and innerR are radii (innerR 0 = solid). The path is the centerline.
ShapePtr makePipe(const std::vector<CurveSeg>& path, double outerR, double innerR);

// Draft (taper) the listed planar faces. angleDeg positive removes material on the pull side of
// the neutral plane, which sits on the face of the body opposite the pull direction.
ShapePtr draftFaces(const ShapePtr& shape, const std::vector<uint32_t>& faces, double angleDeg, const glm::dvec3& pull);

// ---- M5: sweep, loft ----

// Sweeps one profile region along an open wire path (the path is a chained list of segments).
ShapePtr sweepRegion(const RegionLoops& loops, const std::vector<CurveSeg>& pathSegs, double twistDeg = 0,
                     const std::vector<CurveSeg>& guide = {});

// Loft through profile regions in order (smooth, not ruled). A point makes that end a pyramid tip
// (ruled). pointAtStart puts the point before the first profile. A centerline moves each profile so
// its middle follows that curve, without scaling it. Rails, a point, and a centerline are one at a time.
ShapePtr loftRegions(const std::vector<RegionLoops>& sections, const std::vector<std::vector<CurveSeg>>& rails = {},
                     bool hasPoint = false, const glm::dvec3& point = {}, bool pointAtStart = false,
                     const std::vector<CurveSeg>& centerline = {});

// ---- M7: measure ----

struct MeasureResult {
    double distance = 0;
    glm::dvec3 pa{0}, pb{0};
    bool ok = false;
};
MeasureResult measureShapes(const ShapePtr& a, const glm::mat4& xa,
                            const ShapePtr& b, const glm::mat4& xb,
                            uint32_t faceA, uint32_t faceB,   // face indices 0xffffffff = whole body
                            uint32_t edgeA = 0xffffffffu, uint32_t edgeB = 0xffffffffu);   // an edge wins over a face

// One face or edge. radius is a circle, cylinder, sphere, or torus (minor). area is the face.
// loop is the face outline, or the wire an edge sits on. value is mm, mm2, or mm. ok is false when
// the pick has no such measure.
struct CurveMeasure { bool ok = false; double value = 0; };
CurveMeasure measureRadius(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, uint32_t edge);
CurveMeasure measureArea(const ShapePtr& shape, const glm::mat4& xf, uint32_t face);
CurveMeasure measureLoop(const ShapePtr& shape, const glm::mat4& xf, uint32_t face, uint32_t edge);
// The smaller angle, 0 to 90 degrees, between two edges or two flat faces.
CurveMeasure measureAngle(const ShapePtr& a, const glm::mat4& xa, uint32_t faceA, uint32_t edgeA,
                          const ShapePtr& b, const glm::mat4& xb, uint32_t faceB, uint32_t edgeB);

// ---- M8: mesh to solid ----

// Converts a display mesh into a sewn solid body. Slow on huge meshes; throws with guidance.
ShapePtr meshToSolid(const MeshData& mesh);

// The separate solids of a shape (Slice tab: split to objects).
std::vector<ShapePtr> solidsOf(const ShapePtr& shape);

} // namespace kst::kernel
