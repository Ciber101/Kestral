#pragma once
#include "core/Params.h"
#include "core/Types.h"
#include "sketch/Sketch.h"
#include <map>
#include <mutex>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>

namespace kst {

// Appearance beyond the base color (Fusion style appearances). The library is in app/Materials.cpp.
enum class MatPattern : int { None = 0, Wood, CarbonFiber, Brushed, Knurl, Checker, Marble, Speckle, Hex, DiamondPlate, Count };
struct MatProps {
    float opacity = 1.f;            // 1 solid, lower is see through (glass, clear plastic)
    float metal = 0.f;              // 0 dielectric (plastic, paint, wood), 1 metal
    float rough = 0.45f;            // 0 mirror, 1 chalk
    MatPattern pattern = MatPattern::None;
    glm::vec3 color2{0.25f, 0.25f, 0.27f};   // second pattern color
    float scale = 10.f;             // pattern size, mm
    float glow = 0.f;               // emissive strength (LEDs, screens)
    bool fileColors = true;         // show face colors that came with an imported file
    std::string name;               // library name it came from (shown in the UI), empty = custom
    bool operator==(const MatProps& o) const = default;
};
// A face that looks different from its body (Fusion: appearance applied to faces).
struct FaceLook {
    glm::vec3 color{0.72f, 0.74f, 0.77f};
    MatProps mat;
    bool operator==(const FaceLook& o) const = default;
};

// Geometry prototype. Many bodies (instances) can share one part.
struct Part {
    uint32_t id = 0;
    std::string name;
    ShapePtr shape;      // B-rep, null for mesh bodies
    MeshPtr mesh;        // display mesh
    bool isMesh = false;
};

struct Body {
    uint32_t id = 0;
    std::string name;
    uint32_t partId = 0;
    uint32_t componentId = 0;
    uint32_t featureId = 0;   // timeline feature that created it
    uint32_t consumedBy = 0;  // feature whose outputs replaced it (0 = not consumed)
    glm::mat4 xform{1.f};
    glm::vec3 color{0.72f, 0.74f, 0.77f};
    MatProps mat;
    std::map<uint32_t, FaceLook> faceLooks;   // face index -> its own appearance
    bool visible = true;
};

struct Component {
    uint32_t id = 0;
    std::string name;
    uint32_t parentId = 0;    // 0 = this is the root
    uint32_t featureId = 0;   // feature that created it (0 = root / manual)
    bool visible = true;
    bool grounded = false;    // a move or joint will not shift bodies in this component
};

enum class FeatureType { Import, Sketch, Extrude, Revolve, Fillet, Chamfer, Shell, Pattern,
                         Mirror, Combine, Split, Hole, PressPull, Sweep, Loft, Move, MeshConvert,
                         ConstructPlane, Remove, DeleteFace, Gear, Thread, Wing, Dovetail, Blade, Align,
                         Draft, Coil, Pipe, ReplaceFace, ConstructAxis, ConstructPoint, SplitFace, Rib, Web, Emboss, Boss, Snap, Rest, Lip, Silhouette, Joint, AsBuilt, Rigid, Thicken, Other };

// ---- Feature payloads (shared_ptr so undo snapshots stay cheap; copy-on-write when editing) ----
// Boolean targets are stored as (featureId, slot) so they survive recomputes, saves and reloads:
// ids come and go, "the second body Extrude1 makes" does not.
using TargetRef = std::pair<uint32_t, uint32_t>;

// A planar face of a body produced by a feature, by (feature, slot) and face index.
struct FaceRef {
    uint32_t feature = 0, slot = 0, face = 0;
    uint32_t faceTotal = 0;         // face count when picked, detects changed bodies (0 = unknown)
};

struct ExtrudeParams {
    uint32_t sketchId = 0;
    std::string distExpr = "10";    // mm, expression. Negative extrudes the other way.
    std::string taperExpr;          // degrees, empty = none
    int op = 0;                     // 0 new body, 1 join, 2 cut, 3 intersect
    bool flip = false;
    bool symmetric = false;
    std::vector<TargetRef> targets; // for join / cut / intersect (empty = every body the result touches)
    std::vector<sk::Vec2> profilePts;  // a point inside each chosen sketch profile (empty = all profiles)
    std::vector<FaceRef> faces;     // planar body faces to extrude instead of sketch profiles
    int extent = 0;                 // 0 distance, 1 up to a face
    FaceRef to;                     // extent 1: the flat face the extrude stops on
    std::string thinExpr;           // blank is a solid. A wall of this thickness from an open curve.
    int thinSide = 0;               // 0 centered, 1 all on the left, 2 all on the right
};

struct RevolveParams {
    uint32_t sketchId = 0;
    std::string angleExpr = "360";  // degrees
    int axis = 0;                   // 0 = sketch Y, 1 = sketch X, 2 = line entity
    uint32_t axisEnt = 0;           // line entity when axis == 2
    int op = 0;
    bool flip = false;
    std::vector<TargetRef> targets;
    std::vector<sk::Vec2> profilePts;  // picked profiles (empty = all)
    std::string thinExpr;           // blank is a solid. A wall of this thickness from an open curve.
    int thinSide = 0;               // 0 centered, 1 all on the left, 2 all on the right
};

// Fillet, chamfer, shell, press pull: one shared payload shape.
struct ModifyParams {
    int kind = 0;                   // 0 fillet, 1 chamfer, 2 shell, 3 press pull, 4 delete face
    std::vector<TargetRef> targets; // bodies to modify
    std::vector<std::pair<uint32_t, uint32_t>> edges;  // (target index, edge index)
    std::vector<std::pair<uint32_t, uint32_t>> faces;  // (target index, face index)
    uint32_t edgeTotal = 0;         // edge count when picked, to detect changed bodies
    uint32_t faceTotal = 0;
    std::string valueExpr = "2";    // radius / distance / thickness / offset
    std::string endExpr;            // fillet: radius at the other end. chamfer: distance on the other face. Empty means one value the whole way.
    int axis = 2;                   // draft: pull direction 0 X, 1 Y, 2 Z
    int side = 0;                   // shell: 0 inside, 1 outside, 2 both. fillet: 1 replaces the picked face with a full round.
                                    // thicken: 0 along the outward normal, 1 the other way, 2 centered on the face
};

// Rib: an open sketch thickened into a wall and grown along the sketch normal until it meets a body.
// Emboss: a closed sketch projected onto a body. Depth raises a pad. Deboss cuts the same pad in.
struct EmbossParams {
    uint32_t sketchId = 0;
    std::string depthExpr = "1";
    bool deboss = false;
    bool flip = false;              // project along -normal
    std::vector<TargetRef> targets;
};

// Cantilever snap. The point is the center of the beam where it meets the face.
// The catch is a lip at the tip, sticking out across the beam thickness. Hook 0 is a plain beam.
struct SnapParams {
    std::string xExpr = "0", yExpr = "0", zExpr = "0";
    std::string lengthExpr = "12";
    std::string widthExpr = "6";
    std::string thickExpr = "2";
    std::string hookExpr = "1.5";
    std::string hookLenExpr = "3";
    int axis = 2;                   // beam direction: 0 X, 1 Y, 2 Z
    std::vector<TargetRef> targets;
};

// Joint. The moving body shifts so its snap point lands on the grounded body's snap point.
// Empty point expressions mean that body's center.
// kind 0 is rigid (shift only). kind 1 is revolute: after the shift, the mover turns
// about the stay point by angleExpr degrees, around X, Y, or Z.
// kind 2 is a slider: after the shift, the mover slides along that axis by slideExpr mm.
// As-built joint uses the same fields and stays rigid. Neither body moves.
struct JointParams {
    TargetRef ground{0, 0};
    TargetRef move{0, 0};
    int kind = 0;                   // 0 rigid, 1 revolute, 2 slider
    std::string gx, gy, gz;         // point on the body that stays, world mm
    std::string mx, my, mz;         // point on the body that moves, where it is now
    std::string angleExpr = "0";    // revolute, degrees, right hand about axis
    int axis = 2;                   // 0 X, 1 Y, 2 Z
    std::string slideExpr = "0";    // slider, mm along axis
};

// Rigid group. The bodies stay where they are. A later Move or Joint of one carries the rest.
// Scale and a full placement matrix do not.
struct RigidParams {
    std::vector<TargetRef> members;
};

// Silhouette split. The body is cut on the outline seen along axis. 0 X, 1 Y, 2 Z.
struct SilhouetteParams {
    int axis = 2;
    std::vector<TargetRef> targets;
};

// Lip: a closed sketch is the outside of a ridge. Width comes in from that outline. Height grows along the sketch normal.
// Groove cuts the same ridge into the body.
struct LipParams {
    uint32_t sketchId = 0;
    std::string widthExpr = "2";
    std::string heightExpr = "1";
    bool flip = false;
    bool groove = false;
    std::vector<TargetRef> targets;
};

// Rest: a closed sketch is one face of a flat pad. Thickness grows along the sketch normal and joins a body.
struct RestParams {
    uint32_t sketchId = 0;
    std::string thickExpr = "2";
    bool flip = false;
    std::vector<TargetRef> targets;
};

// Round boss standing on a face. The point is the center of the base. Hole 0 is solid.
struct BossParams {
    std::string xExpr = "0", yExpr = "0", zExpr = "0";
    std::string diaExpr = "10";
    std::string heightExpr = "8";
    std::string holeExpr = "0";
    int axis = 2;                   // 0 X, 1 Y, 2 Z
    std::vector<TargetRef> targets;
};

struct RibParams {
    uint32_t sketchId = 0;
    std::string thickExpr = "4";
    bool flip = false;              // grow along -normal instead of +normal
    int side = 0;                   // 0 centered on the sketch, 1 all of the thickness on one side
    std::vector<TargetRef> targets; // body to join. Empty: the body the wall hits.
};

// Coil and pipe. A coil is a round wire wound on a cylinder. A pipe follows a sketch chain,
// or a straight line of `heightExpr` along `axis` from (x, y, z) when pathSketch is 0.
struct SweepSolidParams {
    int axis = 2;
    bool leftHand = false;
    uint32_t pathSketch = 0;
    std::string diaExpr = "20";       // coil: centerline diameter. pipe: outside diameter
    std::string heightExpr = "30";    // coil height, or straight pipe length
    std::string turnsExpr = "3";
    std::string sectionExpr = "2";    // coil wire diameter
    std::string innerExpr = "0";      // pipe hole diameter, 0 = solid
    std::string xExpr = "0", yExpr = "0", zExpr = "0";
};

struct PatternParams {
    bool circular = false;
    bool onPath = false;           // copies walk a sketch curve or a body edge
    std::vector<TargetRef> targets;
    std::string countExpr = "3";
    std::string spacingExpr = "20"; // rect: distance between copies. path: spacing or total length
    std::string countYExpr = "1";
    std::string spacingYExpr = "0";
    int axis = 2;                   // circular: world axis 0 x, 1 y, 2 z, 3 picked edge
    std::string angleExpr = "360";
    glm::dvec3 axisO{0, 0, 0}, axisD{0, 0, 1};   // axis 3: a point on it and its direction (world)
    uint32_t pathSketch = 0;        // path: sketch to follow. 0 with pathBody set means an edge
    uint32_t pathEnt = 0;           // one curve. 0 chains every curve in the sketch
    int distType = 0;               // path: 0 spacing between copies, 1 length from first to last
    int orient = 0;                 // path: 0 keep rotation, 1 turn each copy along the path
    bool flip = false;
    TargetRef pathBody{0, 0};       // edge path: the body that owns the edge
    uint32_t pathEdge = 0;
};

struct MirrorParams {
    std::vector<TargetRef> targets;
    int plane = 0;                  // 0 XY, 1 XZ, 2 YZ, 3 picked face
    glm::dvec3 planeO{0}, planeN{0, 0, 1};   // plane 3: a point on it and its normal (world)
    std::string planeName;
};

struct CombineParams {
    int op = 1;                     // 1 join, 2 cut, 3 intersect
    TargetRef tool;                 // legacy single tool (files before tools existed)
    std::vector<TargetRef> targets; // Fusion style: targets[0] is the target body
    std::vector<TargetRef> tools;   // tool bodies (cut away from / joined to the target)
    bool keepTools = false;
};

// Removes bodies from the model from this point of the timeline on (Fusion's Remove feature).
struct RemoveParams {
    std::vector<TargetRef> targets;
};

struct SplitParams {
    std::vector<TargetRef> targets;
    int plane = 0;                  // 0 XY, 1 XZ, 2 YZ, 3 a picked face, edge or sketch line
    glm::dvec3 toolO{0}, toolN{0, 0, 1};
    std::string toolName;
    uint32_t sketchId = 0, sketchEnt = 0;   // a sketch line: the plane is rebuilt from it
};

// Split Face: divide faces where a plane or a projected sketch curve crosses them. The body stays one solid.
struct SplitFaceParams {
    std::vector<TargetRef> targets;
    int plane = 0;                  // 0 XY (normal +Z), 1 XZ (normal -Y), 2 YZ (normal +X)
    std::string offsetExpr = "0";   // mm along that normal. Unused when a sketch is set.
    std::vector<uint32_t> faces;    // empty = every face the tool crosses
    uint32_t faceTotal = 0;
    uint32_t sketchId = 0;          // project this sketch along its normal. 0 = use the plane.
};

// Align (Fusion style): move bodies so a snap on them lands on a snap on something else.
// Pick kind: 0 face center, 1 edge middle, 2 corner.
struct AlignParams {
    struct Pick {
        int kind = 0;
        TargetRef ref{0, 0};
        uint32_t sub = 0, total = 0;
        int end = 0;                // corner: 1 = start of the edge, 2 = end
    };
    Pick from, to;
    bool flip = false;
    std::vector<TargetRef> targets;
    std::string fromName, toName;
};

struct HoleParams {
    uint32_t sketchId = 0;          // 0 = holes placed straight on faces (points below)
    std::string diaExpr;            // empty = use each circle's own diameter (sketch holes)
    std::string depthExpr;          // empty = through
    std::vector<TargetRef> targets;
    // Placed holes: a point on a face and the direction into the material.
    struct Point { glm::dvec3 pos{0}; glm::dvec3 dir{0, 0, -1}; };
    std::vector<Point> points;
    int kind = 0;                   // 0 simple, 1 counterbore, 2 countersink
    std::string cbDiaExpr = "10", cbDepthExpr = "3";   // counterbore
    std::string csDiaExpr = "10", csAngleExpr = "90";  // countersink
    std::string spotDiaExpr, spotDepthExpr;            // blank = no spotface
    std::string drillExpr;                             // blank = flat bottom. Included angle, degrees. Blind holes only.
    bool tapped = false;
    std::string pitchExpr;                             // ISO metric form cut into the wall. Blank unless tapped.
};

// Sweep and loft: sketches in order (sweep: profile then path).
struct MultiSketchParams {
    std::vector<uint32_t> sketchIds;
    int op = 0;                     // 0 new body, 1 join, 2 cut, 3 intersect
    std::vector<TargetRef> targets;
    std::string twistExpr;          // sweep: degrees the profile turns along the path. empty is 0
    std::vector<uint32_t> railIds;  // loft rails, or the sweep guide. The section grows to meet them.
    std::string thinExpr;           // sweep: blank is a closed profile. A wall of this thickness from an open curve.
    int thinSide = 0;               // 0 centered, 1 all on the left, 2 all on the right
    int pointEnd = 0;               // loft: 0 none, 1 the loft starts at a point, 2 it ends at a point
    std::string pointX, pointY, pointZ; // loft point, world mm. Used when pointEnd is 1 or 2.
    uint32_t centerId = 0;          // loft: open sketch the profile centers follow. 0 is a straight loft.
};

struct MoveParams {
    std::vector<TargetRef> targets;
    std::string xExpr = "0", yExpr = "0", zExpr = "0";
    bool copy = false;
    // Rotation in degrees. rx/ry/rz are turns about the world axes. ang about (ax,ay,az) wins when
    // that direction is not zero. px/py/pz empty means the body's own center.
    std::string rxExpr = "0", ryExpr = "0", rzExpr = "0";
    std::string angExpr = "0", axExpr = "0", ayExpr = "0", azExpr = "0";
    std::string pxExpr, pyExpr, pzExpr;
    std::string sxExpr = "1", syExpr = "1", szExpr = "1";
    int op = 0;                        // 0 move, 1 scale (sx/sy/sz)
    bool hasMat = false;               // mat replaces the placement (row major, 16 numbers)
    double mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// A construction axis: a point and a unit direction.
struct ConstructAxis {
    glm::dvec3 origin{0};
    glm::dvec3 direction{0, 0, 1};
};

// Axis through two points, a straight edge, or the center of a cylinder.
struct ConstructAxisParams {
    int kind = 0;                   // 0 two points, 1 straight edge, 2 cylinder face
    std::string ax = "0", ay = "0", az = "0";
    std::string bx = "0", by = "0", bz = "40";
    TargetRef ref{0, 0};            // edge and cylinder: the body
    uint32_t sub = 0;               // edge index or face index
    uint32_t subTotal = 0;          // edge or face count when picked
};

// A construction point: coordinates, the middle of an edge, the center of a flat face, or a corner.
struct ConstructPointParams {
    int kind = 0;                   // 0 coordinates, 1 edge, 2 face, 3 corner
    std::string x = "0", y = "0", z = "0";
    TargetRef ref{0, 0};
    uint32_t sub = 0;
    uint32_t subTotal = 0;
};

// Construction geometry sketches can be drawn on.
struct ConstructPlaneParams {
    int kind = 0;                   // 0 offset from an origin plane, 1 plane at an angle, 2 midplane, 3 through three points, 4 tangent to a cylinder
    int base = 0;                   // 0 XY, 1 XZ, 2 YZ
    int axis = 0;                   // angle: 0 rotate about the plane X, 1 about the plane Y
    uint32_t refPlane = 0;          // midplane: the other construction plane
    std::string offsetExpr = "20";  // offset in mm, or the angle in degrees
    std::string x1 = "0", y1 = "0", z1 = "0";       // kind 3: first point, the plane origin
    std::string x2 = "10", y2 = "0", z2 = "0";
    std::string x3 = "0", y3 = "10", z3 = "0";
    TargetRef faceRef{0, 0};        // kind 4: the cylindrical face
    uint32_t faceSub = 0;
    uint32_t faceTotal = 0;
    std::string angleExpr = "0";    // kind 4: degrees around the cylinder axis
};

// Gear generator. Values are expressions keyed by name (module, teeth, pressure, width, bore, helix,
// backlash, shift, rim, mate, starts, wormDia, length, dp, x, y) so parameters can drive them.
struct GearParams {
    int type = 0;                   // kernel::GearType
    int plane = 0;                  // 0 XY, 1 XZ, 2 YZ
    int sizeBy = 0;                 // 0 module (mm), 1 diametral pitch (teeth per inch)
    bool leftHand = false;
    std::map<std::string, std::string> vals;
    std::string val(const std::string& k, const char* def) const {
        auto it = vals.find(k);
        return it == vals.end() || it->second.empty() ? std::string(def) : it->second;
    }
};

// Thread on a round face: modeled (cut into the body) or cosmetic (drawn only).
struct ThreadParams {
    FaceRef face;                   // the cylindrical face (shaft or hole)
    int family = 0;                 // threads::families() index
    int size = 0;                   // index in the family, -1 = custom diameter and pitch
    int side = 0;                   // 0 from the face, 1 external, 2 internal
    bool modeled = true;
    bool leftHand = false;
    bool flip = false;              // start from the other end of the face
    bool fullLength = true;
    std::string lengthExpr = "10", offsetExpr = "0", startsExpr;   // starts: empty = the size's own
    std::string diaExpr = "10", pitchExpr = "1.5";                 // custom size
    // The face as picked (world), for drawing cosmetic threads without the kernel.
    glm::dvec3 axisO{0}, axisD{0, 0, 1};
    double radius = 0, faceLen = 0;
    bool hole = false;
};

// Wing generator: a root section and panels outward from it (chord +X, span +Y, up +Z).
// vals: rootChord, x, y, z, skin, spar1, spar1Dia, spar2, spar2Dia, ribSpacing, ribThick.
struct WingParams {
    std::string rootAirfoil = "NACA 2412";
    struct Panel {
        std::string span = "400", tipChord = "150", sweep = "3", dihedral = "3", twist = "-2";
        std::string airfoil = "NACA 2412";
    };
    std::vector<Panel> panels;
    bool mirror = false;            // both halves
    bool hollow = false;            // printable shell with ribs and spar holes
    std::map<std::string, std::string> vals;
    std::string val(const std::string& k, const char* def) const {
        auto it = vals.find(k);
        return it == vals.end() || it->second.empty() ? std::string(def) : it->second;
    }
};

// Dovetail generator: kernel::DovetailType as an int, placed on an origin plane at (x, y), raised
// by z along its normal and turned by rot. Connectors work on targets: the first gets the tab
// (or keeps nothing for a key), the rest get the socket.
struct DovetailParams {
    int type = 1;
    int plane = 0;
    std::vector<TargetRef> targets;
    std::map<std::string, std::string> vals;
    std::string val(const std::string& k, const char* def) const {
        auto it = vals.find(k);
        return it == vals.end() || it->second.empty() ? std::string(def) : it->second;
    }
};

// Blade generator: kind 0 propeller, 1 helicopter rotor, 2 ducted fan, 3 wind turbine, 4 boat propeller.
// The rotor turns about the chosen origin plane's normal through (x, y), raised z along it.
struct BladeParams {
    int kind = 0;
    int plane = 0;
    bool leftHand = false;
    std::string airfoil = "Clark Y";
    std::map<std::string, std::string> vals;
    std::string val(const std::string& k, const char* def) const {
        auto it = vals.find(k);
        return it == vals.end() || it->second.empty() ? std::string(def) : it->second;
    }
};

struct Feature {
    uint32_t id = 0;
    FeatureType type = FeatureType::Other;
    std::string name;
    bool suppressed = false;
    bool failed = false;
    std::string error;
    uint32_t sketchId = 0;          // for FeatureType::Sketch
    std::string importPath;         // for FeatureType::Import
    std::shared_ptr<ExtrudeParams> extrude;
    std::shared_ptr<RevolveParams> revolve;
    std::shared_ptr<ModifyParams> modify;
    std::shared_ptr<PatternParams> pattern;
    std::shared_ptr<MirrorParams> mirror;
    std::shared_ptr<CombineParams> combine;
    std::shared_ptr<SplitParams> split;
    std::shared_ptr<SplitFaceParams> splitFace;
    std::shared_ptr<HoleParams> hole;
    std::shared_ptr<MultiSketchParams> multi;
    std::shared_ptr<MoveParams> move;
    std::shared_ptr<ConstructPlaneParams> cplane;
    std::shared_ptr<ConstructAxisParams> caxis;
    std::shared_ptr<ConstructPointParams> cpoint;
    std::shared_ptr<RemoveParams> remove;
    std::shared_ptr<GearParams> gear;
    std::shared_ptr<ThreadParams> thread;
    std::shared_ptr<WingParams> wing;
    std::shared_ptr<DovetailParams> dovetail;
    std::shared_ptr<BladeParams> blade;
    std::shared_ptr<AlignParams> align;
    std::shared_ptr<SweepSolidParams> pathSolid;   // coil and pipe
    std::shared_ptr<RibParams> rib;
    std::shared_ptr<RibParams> web;   // same payload: open sketch, thickness, flip, side, target
    std::shared_ptr<EmbossParams> emboss;
    std::shared_ptr<BossParams> boss;
    std::shared_ptr<SnapParams> snap;
    std::shared_ptr<RestParams> rest;
    std::shared_ptr<LipParams> lip;
    std::shared_ptr<SilhouetteParams> silhouette;
    std::shared_ptr<JointParams> joint;
    std::shared_ptr<JointParams> asbuilt;   // same payload; neither body moves
    std::shared_ptr<RigidParams> rigid;
    // recompute bookkeeping (not user visible state)
    uint64_t sig = 0;               // signature of everything this was computed from
    uint64_t chainSig = 0;          // own signature combined with everything upstream
    double lastMs = 0;              // last recompute time
};

// Name / color / visibility a user gave a computed body, keyed by (featureId, slot).
struct BodyOverride {
    std::string name;
    glm::vec3 color{0.72f, 0.74f, 0.77f};
    bool hasColor = false;
    bool visible = true;
    MatProps mat;                       // valid when hasColor
    std::map<uint32_t, FaceLook> faceLooks;
};

// Section analysis. A view cut. The solids do not change.
// axis 0 is the YZ plane (cut along X), 1 is XZ (cut along Y), 2 is XY (cut along Z).
// The half past offset along that axis is hidden. flip hides the other half.
struct SectionView {
    bool on = false;
    int axis = 2;
    double offset = 0;
    bool flip = false;
    bool keeps(glm::dvec3 p) const {
        if (!on) return true;
        double c = axis == 0 ? p.x : axis == 1 ? p.y : p.z;
        if (flip) return c + 1e-9 >= offset;
        return c <= offset + 1e-9;
    }
};

// Robot description for URDF / USD export (Isaac Sim / Isaac Lab, ROS). Each link is a component with
// everything inside it, except sub components that are links of their own. Joints are placed in the
// pose the design is in now (that is the zero position).
struct RobotJoint {
    std::string name;
    std::string parent, child;           // component paths ("Left leg/Thigh"), they survive recomputes
    int type = 1;                        // 0 fixed, 1 revolute, 2 continuous
    glm::dvec3 origin{0};                // world mm, on the axis
    glm::dvec3 axis{1, 0, 0};            // world direction, right hand
    double lower = -90, upper = 90;      // degrees
    double effort = 2.94;                // N m (STS3215 12 V: about 30 kg cm)
    double velocity = 4.7;               // rad/s
    double stiffness = 20, damping = 0.5;  // position drive for USD (N m per rad, N m s per rad)
};
struct RobotDef {
    std::string name;                    // robot name (blank: the design name)
    std::string base;                    // root link component path (blank: not set)
    double density = 0.85;               // g/cm3 for bodies without a set mass (printed PLA / PETG with infill)
    int front = 3;                       // which way the robot faces in the design: 0 +X, 1 -X, 2 +Y, 3 -Y (Kestral's front view)
    std::map<std::string, double> massG; // component path -> its total mass in grams (bought parts: servos, boards)
    std::vector<RobotJoint> joints;
    bool empty() const { return base.empty() && joints.empty() && massG.empty(); }
};

class Document {
public:
    Document();

    std::string name = "Untitled";
    std::string path;
    int version = 0;                 // goes up on every Ctrl+S (v1, v2...); older ones in the versions folder
    uint32_t rootId = 0;

    std::map<uint32_t, Part> parts;
    std::map<uint32_t, Body> bodies;
    std::map<uint32_t, Component> components;
    std::map<uint32_t, uint32_t> featureFolder; // feature id -> component for bodies it creates
    uint32_t activeComponent = 0;               // component that receives new bodies (0 = root)
    std::map<uint32_t, sk::Sketch> sketches;
    std::vector<Feature> timeline;
    int markerPos = 0;          // features [0, markerPos) are active (rollback marker)
    ParamMap params;            // user parameters
    std::map<uint64_t, BodyOverride> overrides;
    std::map<uint64_t, std::string> compNames;
    std::set<uint64_t> compHidden;               // (feature, nth component it makes) the user hid
    std::set<uint32_t> hiddenPlanes;
    std::set<uint64_t> autoHidden;               // (feature, sketch) uses already acted on: the sketch was hidden then             // construction planes the user hid   // (feature, nth component it makes) -> name the user gave it
    std::map<uint32_t, sk::Plane> constructPlanes;   // construction planes sketches can use
    std::map<uint32_t, ConstructAxis> constructAxes;
    std::map<uint32_t, glm::dvec3> constructPoints;
    SectionView section;
    RobotDef robot;

    uint32_t newId() { return nextId_++; }
    void remapNextId(uint32_t maxUsedId) { nextId_ = std::max(nextId_, maxUsedId + 1); }
    static uint64_t overrideKey(uint32_t featureId, uint32_t slot) {
        return ((uint64_t)featureId << 32) | slot;
    }
    void setOverride(uint32_t featureId, uint32_t slot, const BodyOverride& o) {
        overrides[overrideKey(featureId, slot)] = o;
    }
    const BodyOverride* findOverride(uint32_t featureId, uint32_t slot) const {
        auto it = overrides.find(overrideKey(featureId, slot));
        return it == overrides.end() ? nullptr : &it->second;
    }

    // Body is drawn: visible itself, all parent components visible, feature active.
    bool isBodyShown(const Body& b) const;
    // Exists at the current point of the timeline (made by an active feature, not replaced since).
    bool isBodyAlive(const Body& b) const;
    bool isFeatureActive(uint32_t featureId) const;
    bool isComponentShown(uint32_t compId) const;
    int featureIndex(uint32_t featureId) const;
    // Bodies created by a feature, in slot order (by id).
    std::vector<uint32_t> featureBodies(uint32_t featureId) const;
    // Slot index of a body among its feature's bodies (0 when unknown).
    uint32_t bodySlot(uint32_t bodyId) const;

    std::vector<uint32_t> childComponents(uint32_t compId) const;
    std::vector<uint32_t> bodiesIn(uint32_t compId) const;
    // All bodies in a component and its sub components.
    void collectBodies(uint32_t compId, std::vector<uint32_t>& out) const;
    int countBodiesDeep(uint32_t compId) const;

    Aabb bounds(bool shownOnly = true) const;
    void removeBodies(const std::set<uint32_t>& ids);
    void removeComponent(uint32_t compId);
    void gcParts();

    uint64_t revision = 1;      // bumps on any change that affects drawing
    void touch() { ++revision; }

private:
    uint32_t nextId_ = 1;
    // featureIndex() cache: feature id -> timeline index. Checked on every hit and rebuilt when stale,
    // so code that edits the timeline directly stays correct. Not copied with the document.
    struct IndexCache {
        mutable std::mutex mu;
        mutable std::unordered_map<uint32_t, int> map;
        IndexCache() = default;
        IndexCache(const IndexCache&) {}
        IndexCache& operator=(const IndexCache&) { std::lock_guard<std::mutex> lk(mu); map.clear(); return *this; }
    } fidx_;
};

// Snapshot undo. Documents are cheap to copy because geometry is shared.
class UndoStack {
public:
    void push(const Document& before, std::string label);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo(Document& doc);
    bool redo(Document& doc);
    std::string undoLabel() const { return undo_.empty() ? "" : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? "" : redo_.back().label; }
    void clear() { undo_.clear(); redo_.clear(); }
    void dropLast() { if (!undo_.empty()) undo_.pop_back(); }
private:
    struct Entry { Document doc; std::string label; };
    std::vector<Entry> undo_, redo_;
    static constexpr size_t kMax = 200;
};

} // namespace kst
