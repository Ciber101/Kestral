#pragma once
// Gear and thread feature values -> kernel specs (shared by the recompute and the dialogs).
#include "core/Document.h"
#include "core/Threads.h"
#include "kernel/Mechanical.h"

namespace kst {

const char* gearTypeName(int type);

// Throws std::runtime_error for values that do not evaluate.
kernel::GearSpec gearSpecOf(const GearParams& g, const ParamMap& params);
// Gear frame -> world: the chosen origin plane with the center at (x, y) on it.
glm::mat4 gearPlacement(const GearParams& g, const ParamMap& params);
GearParams defaultGear();
// Sensible values when switching to another gear type.
void gearTypeDefaults(GearParams& g, int newType);

struct ThreadSizeInfo {
    const threads::Family* family = nullptr;
    std::string name;
    double major = 0, pitch = 0;
    int starts = 1;
};
ThreadSizeInfo threadSize(const ThreadParams& t, const ParamMap& params);
bool threadInternal(const ThreadParams& t);
// The cutter for a modeled thread on the face as it is now.
kernel::ThreadCut threadCutOf(const ThreadParams& t, const kernel::RoundFace& face, const ParamMap& params);

// Where imported .dat airfoils live (%APPDATA%\Kestral\airfoils).
std::string airfoilFolder();
WingParams defaultWing();
// Throws std::runtime_error for values that do not evaluate or airfoils that do not load.
kernel::WingSpec wingSpecOf(const WingParams& w, const ParamMap& params);
// Planform numbers for the dialog: "Span ... Area ... AR ... MAC ...".
std::string wingInfo(const kernel::WingSpec& s);

const char* dovetailTypeName(int type);
DovetailParams defaultDovetail();
// Sizes that suit each kind, set when switching kinds.
void dovetailTypeDefaults(DovetailParams& d, int newType);
kernel::DovetailSpec dovetailSpecOf(const DovetailParams& d, const ParamMap& params);
// Local frame -> world: origin plane, (x, y) on it, z along its normal, turned by rot about the normal.
glm::mat4 dovetailPlacement(const DovetailParams& d, const ParamMap& params);

const char* bladeKindName(int kind);
BladeParams defaultBlade();
void bladeKindDefaults(BladeParams& b, int newKind);
kernel::BladeSpec bladeSpecOf(const BladeParams& b, const ParamMap& params);
// Rotor frame -> world: axis = the origin plane's normal through (x, y), raised z.
glm::mat4 bladePlacement(const BladeParams& b, const ParamMap& params);
std::string bladeInfo(const kernel::BladeSpec& s);

} // namespace kst
