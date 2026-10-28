#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace kst::sk {

using Vec2 = glm::dvec2;

enum class EntType : uint8_t { Point, Line, Circle, Arc, Spline, Ellipse, EllipArc, CtrlSpline };

// Everything is built from points. Line: points a,b. Circle: center point c and radius r.
// Arc: center point c, start point a, end point b, counter clockwise from a to b.
struct Entity {
    uint32_t id = 0;
    EntType type = EntType::Point;
    double x = 0, y = 0;          // point position
    double r = 0;                 // circle radius
    uint32_t a = 0, b = 0, c = 0;
    bool construction = false;
    bool locked = false;          // origin and axes: constants for the solver
    bool loop = false;            // spline: closed through the first fit point
    std::vector<uint32_t> pts;    // spline fit points, in order
};

enum class CType : uint8_t {
    Coincident, PointOnObject, Horizontal, Vertical, Parallel, Perpendicular, Equal, Tangent,
    Midpoint, Symmetric, Fix, Collinear,
    // dimensions (driving)
    Distance, HDistance, VDistance, Radius, Diameter, Angle, PointLineDistance,
};

inline bool isDimension(CType t) { return t >= CType::Distance; }

struct Constraint {
    uint32_t id = 0;
    CType type = CType::Coincident;
    uint32_t e[3] = {0, 0, 0};
    double value = 0, value2 = 0;   // dimension value (mm or radians), Fix target, Tangent flag
    std::string name;               // d1, d2 ... (user parameters hook in later)
    Vec2 label{0, 0};               // dimension text position in sketch coordinates
    bool labelSet = false;
    bool failed = false;
    bool driven = false;            // reports the current size and does not drive it
    std::string expr;               // when set, value = eval(expr) against user parameters
};

struct Plane {
    glm::dvec3 o{0}, x{1, 0, 0}, y{0, 1, 0};
    std::string name = "XY";
    glm::dvec3 normal() const { return glm::cross(x, y); }
    Vec2 toLocal(const glm::dvec3& w) const { return {glm::dot(w - o, x), glm::dot(w - o, y)}; }
    glm::dvec3 toWorld(const Vec2& p) const { return o + x * p.x + y * p.y; }
    // Ray hit in sketch coordinates. Returns false if parallel.
    bool intersect(const glm::dvec3& ro, const glm::dvec3& rd, Vec2& out) const;
};

class Sketch {
public:
    static constexpr uint32_t kOrigin = 1, kXAxis = 4, kYAxis = 5, kFirstUser = 16;

    uint32_t id = 0;
    uint32_t featureId = 0;
    std::string name = "Sketch";
    Plane plane;
    bool visible = true;
    std::map<uint32_t, Entity> ents;
    std::map<uint32_t, Constraint> cons;
    uint32_t nextId = kFirstUser;
    int dof = -1;                         // degrees of freedom of the last solve, -1 unknown
    std::set<uint32_t> defined;           // entity ids whose geometry is fully determined
    uint32_t dimCounter = 0;
    uint64_t rev = 1;                     // unique per edit across all sketches (undo safe cache key)

    Sketch();

    const Entity* find(uint32_t id) const { auto it = ents.find(id); return it == ents.end() ? nullptr : &it->second; }
    Entity* find(uint32_t id) { auto it = ents.find(id); return it == ents.end() ? nullptr : &it->second; }
    Vec2 pt(uint32_t pointId) const;
    void setPt(uint32_t pointId, Vec2 p);

    uint32_t addPoint(Vec2 p);
    uint32_t addLine(uint32_t a, uint32_t b, bool construction = false);
    uint32_t addCircle(uint32_t center, double r, bool construction = false);
    uint32_t addArc(uint32_t center, uint32_t start, uint32_t end, bool construction = false);
    // major is the long radius, minor the short one. rotation is the long axis, in radians, from sketch X.
    // The larger radius is stored as the major. An arc runs counter clockwise in eccentric angle from
    // the start point to the end point.
    uint32_t addEllipse(uint32_t center, double major, double minor, double rotation, bool construction = false);
    uint32_t addEllipArc(uint32_t center, double major, double minor, double rotation, uint32_t start, uint32_t end, bool construction = false);
    uint32_t addSpline(const std::vector<uint32_t>& fit, bool loop, bool construction = false);
    // Control points pull the curve. An open curve starts on the first and ends on the last.
    // A closed curve is periodic and does not pass through the control points.
    uint32_t addCtrlSpline(const std::vector<uint32_t>& ctrl, bool loop, bool construction = false);
    // Samples a fit-point spline. steps is the number of pieces along each span.
    std::vector<Vec2> splinePoly(const Entity& e, int steps = 8) const;
    std::vector<Vec2> ctrlSplinePoly(const Entity& e, int steps = 8) const;
    static std::vector<Vec2> sampleFitSpline(const std::vector<Vec2>& fit, bool loop, int steps = 8);
    static std::vector<Vec2> sampleCtrlSpline(const std::vector<Vec2>& ctrl, bool loop, int steps = 8);
    uint32_t addConstraint(CType t, uint32_t e0, uint32_t e1 = 0, uint32_t e2 = 0, double value = 0);
    void removeConstraint(uint32_t id) { cons.erase(id); touch(); }

    // Deletes geometry (and orphaned points, and constraints that referenced them).
    void deleteEntities(const std::set<uint32_t>& ids);
    bool hasConstraint(CType t, uint32_t e0, uint32_t e1 = 0, uint32_t e2 = 0) const;
    void touch() { rev = nextRev(); }
    static uint64_t nextRev();

    // Geometry queries (sketch coordinates)
    struct ArcGeom { Vec2 c; double r = 0, a0 = 0, sweep = 0; };   // a0 start angle, sweep in (0, 2pi]
    struct EllipseGeom { Vec2 c; double major = 0, minor = 0, rot = 0, a0 = 0, sweep = 0; };
    bool arcGeom(const Entity& e, ArcGeom& g) const;
    bool ellipseGeom(const Entity& e, EllipseGeom& g) const;
    Vec2 ellipsePoint(const EllipseGeom& g, double t) const;       // t in [0,1] along the eccentric sweep
    bool circleGeom(const Entity& e, Vec2& c, double& r) const;
    Vec2 arcPoint(const ArcGeom& g, double t) const;               // t in [0,1] along the sweep
    double distanceTo(const Entity& e, Vec2 p, Vec2* closest = nullptr) const;
    // The size a dimension is reporting right now. A driven dimension stores this instead of driving it.
    double measureDimension(const Constraint& c) const;
    // Point entity position or line midpoint etc. for labels
    Vec2 center(const Entity& e) const;
    bool isGeometry(const Entity& e) const { return e.type != EntType::Point && !e.locked; }
    void bounds(Vec2& lo, Vec2& hi) const;
    int geometryCount() const;

    // Editable text. An empty font is the block letters and is not stored here.
    // A named font keeps the string, so it can be rewritten. ents are the outline lines.
    struct TextNote {
        uint32_t id = 0;
        std::string text;
        std::string font;
        double height = 10;
        Vec2 origin{0, 0};
        bool construction = false;
        std::vector<uint32_t> ents;
    };
    std::vector<TextNote> notes;
    uint32_t nextNote = 1;
};

} // namespace kst::sk
