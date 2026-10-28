#pragma once
// Gear and thread geometry: involute gears of every common kind, and helical thread cutters.
#include "core/Types.h"

#include <string>
#include <vector>

namespace kst::kernel {

enum class GearType { Spur, Helical, Herringbone, Internal, Rack, Bevel, Worm, WormWheel, Count };

struct GearSpec {
    GearType type = GearType::Spur;
    int teeth = 24;
    double module = 2;          // mm (normal module for helical kinds, axial module for worms)
    double pressureDeg = 20;
    double width = 10;          // face width (bevel: along the cone)
    double bore = 0;            // 0 = solid
    double helixDeg = 20;       // helical / herringbone / internal
    double backlash = 0;        // mm taken off each tooth at the pitch circle
    double shift = 0;           // profile shift coefficient
    double rim = 5;             // ring gear wall past the roots, rack base below the roots
    int mateTeeth = 24;         // bevel: teeth of the mating gear (sets the cone)
    int starts = 1;             // worm threads
    double wormDia = 20;        // worm and worm wheel: worm pitch diameter
    double length = 40;         // worm length
    bool leftHand = false;
};

// The gear in its own frame: axis +Z starting at z = 0 (rack: teeth along +X pointing +Y, width +Z;
// worm: axis +Z). Throws std::runtime_error with a readable reason.
ShapePtr makeGear(const GearSpec& g);

// Key sizes for the dialog (pitch / outside / root diameters, mesh distance).
std::string gearInfo(const GearSpec& g);

// A cylindrical face of a placed body, in world space.
struct RoundFace {
    glm::dvec3 origin{0}, dir{0, 0, 1};   // axis point at one end of the face, toward the other end
    double radius = 0, length = 0;
    bool hole = false;                    // material outside the face (a hole) rather than inside (a shaft)
};
bool roundFace(const ShapePtr& shape, const glm::mat4& xform, uint32_t face, RoundFace& out);

struct ThreadCut {
    glm::dvec3 origin{0}, dir{0, 0, 1};   // world: where the thread starts and which way it runs
    double length = 10;
    double pitch = 1;
    int starts = 1;
    bool leftHand = false;
    double taperDeg = 0;                  // radius grows by tan(taper) per mm along dir
    std::vector<glm::dvec2> groove;       // (radius, axial offset) closed polygon, one groove
};
// The helical groove material (to cut away). World space. clipToLength trims it to [0, length].
ShapePtr makeThreadTool(const ThreadCut& t, bool clipToLength = true);

// Cut that groove out of a placed body. The body comes back in world space. Throws if the helix
// misses the face or would eat the whole solid.
// warn (optional) gets a note when some turns could not be cut.
ShapePtr cutThread(const ShapePtr& body, const glm::mat4& xform, const ThreadCut& t, std::string* warn = nullptr);

// Wing: chord along +X, span along +Y, up +Z. Sections stay parallel to the XZ plane and are joined
// by straight (ruled) panels, the way real wings are lofted.
struct WingSection {
    glm::dvec3 le{0};                 // leading edge point
    double chord = 100;
    double twistDeg = 0;              // nose up positive, about the quarter chord
    std::vector<glm::dvec2> foil;     // unit chord outline, TE upper -> LE -> TE lower, same count for all
};
struct WingSpar { double pos = 0.25; double dia = 6; };   // chord fraction, rod diameter
struct WingSpec {
    std::vector<WingSection> sections;
    bool mirror = false;              // both halves (mirrored across XZ)
    bool hollow = false;
    double skin = 1.2;
    std::vector<WingSpar> spars;      // holes along the span (hollow or not)
    double ribSpacing = 0, ribThick = 1.2;   // hollow: ribs every ribSpacing mm (0 = none)
};
ShapePtr makeWing(const WingSpec& w);

// Rotor blades (propellers, rotors, fans, turbines). Rotation axis +Z, counterclockwise seen from
// +Z, blade 0 along +X. A section at radius r has its chord at angle beta to the rotation plane,
// leading edge toward +Y and up (+Z, the thrust side).
struct BladeSection {
    double r = 10, chord = 10, betaDeg = 20;
    double thick = 1;                 // scales the airfoil thickness
    double skewDeg = 0, rake = 0;     // marine: swept back about Z, moved along Z
    std::vector<glm::dvec2> foil;     // unit chord outline, same point count for all sections
};
struct BladeSpec {
    std::vector<BladeSection> sections;   // root to tip
    int blades = 2;
    double pivot = 0.33;                  // chord fraction on the blade's reference line
    double hubDia = 20, hubLen = 10, bore = 0;
    bool leftHand = false;
    // helicopter: a root grip block per blade from the center (or hub) to gripTo
    double gripTo = 0, gripW = 0, gripT = 0, boltDia = 0, boltR = 0;
    // ducted fan: a ring around the tips (a separate body). ductThick 0 = none.
    double ductGap = 0.5, ductThick = 0, ductLen = 0;
};
struct BladeParts { ShapePtr rotor, duct; };
BladeParts makeBlades(const BladeSpec& s);

// Dovetails (Joinery.cpp). Connector kinds give a male piece and a female cutter with clearance,
// in a local frame: the seam is the plane u = 0 (first body on u < 0, the others on u > 0), v runs
// along the seam, the piece is extruded along +Z from 0 to depth. Joint and slide kinds give whole
// bodies in their own frame.
enum class DovetailType {
    Key,            // bowtie / butterfly key across a seam (separate piece)
    Tab,            // dovetail tab on the first body, socket in the others
    Sliding,        // dovetail rail and slot (same cross section, long extrusion)
    Puzzle,         // jigsaw tab: neck and round head
    Through,        // woodworking: through dovetail, tail board + pin board
    HalfBlind,      // woodworking: half blind (lap on the pin board's face)
    SecretMitered,  // woodworking: laps on both boards, mitered outer corner
    MachineSlide,   // machine ways: base with channel, slide with tongue, gib strip
    Count
};
struct DovetailSpec {
    DovetailType type = DovetailType::Tab;
    // connectors
    double length = 12, width = 10, depth = 5, angleDeg = 10, head = 10, clearance = 0.15;
    int count = 1;
    double spacing = 30;
    // woodworking (board width along X, tail board along +Y flat, pin board upright)
    double boardW = 100, tailT = 18, pinT = 18, tailL = 150, pinL = 150, pinW = 6, lap = 4;
    int tails = 3;
    // machine slide (cross section in XY, length along +Z)
    double baseW = 60, baseH = 20, chanW = 30, chanD = 10, flankDeg = 60, slideW = 60, slideH = 20, gib = 3, slideLen = 100;
};
struct DovetailParts {
    ShapePtr male, female;                             // connectors
    std::vector<std::pair<std::string, ShapePtr>> bodies;   // joints and slides
};
DovetailParts makeDovetail(const DovetailSpec& s);
bool dovetailIsConnector(DovetailType t);

} // namespace kst::kernel
