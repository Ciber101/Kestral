// Print preview: reads the G-code inside a sliced .gcode.3mf and turns every extrusion into small
// 3D ribbons, one mesh per layer and feature (outer wall, infill, support...), drawn by Kestral's
// own renderer. Same feature names and colors as OrcaSlicer's preview.
#pragma once
#include "core/Types.h"

#include <array>
#include <memory>
#include <atomic>
#include <string>
#include <vector>

namespace kst::print {

enum class Feature : uint8_t {
    OuterWall, InnerWall, OverhangWall, SparseInfill, SolidInfill, TopSurface, BottomSurface, Bridge,
    GapFill, Ironing, Skirt, Brim, Support, SupportInterface, SupportTransition, PrimeTower, Custom,
    Travel, Retract, Unretract, Seam, ToolChange,   // moves and markers (not printed lines)
    Count
};
constexpr int kFeatureCount = (int)Feature::Count;
const char* featureName(int f);
uint32_t featureColor(int f);   // RGBA8, A = 255
inline bool featureIsMarker(int f) { return f >= (int)Feature::Retract && f <= (int)Feature::ToolChange; }
inline bool featureIsMove(int f) { return f >= (int)Feature::Travel; }   // travel or a marker: not extruded
// Preview colors by a value (Orca's view types): 0 line type, 1 filament, then these.
enum ViewMode { ViewFeature = 0, ViewTool = 1, ViewSpeed = 2, ViewHeight = 3, ViewWidth = 4, ViewFan = 5, ViewTemp = 6, ViewFlow = 7, ViewModes = 8 };
constexpr int kBins = 11;                      // Orca's gradient steps
uint32_t binColor(int bin);                    // RGBA8 of a gradient step (blue to red)

struct ToolSeg {
    float x0, y0, x1, y1;
    float width;
    uint8_t feature;
    uint8_t tool = 0;   // filament (T0, T1...)
    uint8_t fan = 0;    // part fan, percent
    float speed = 0;    // mm/s
    uint16_t temp = 0;  // nozzle, C
    uint32_t line = 0;  // G-code line that ends it (0 based)
};

struct ToolLayer {
    float z = 0, height = 0.2f;
    std::vector<ToolSeg> segs;
};

struct Toolpaths {
    std::vector<ToolLayer> layers;
    std::array<double, kFeatureCount> length{};   // mm extruded per feature
    std::array<double, kFeatureCount> seconds{};  // printing time per feature (from the feed rates; the
                                                  // tab scales these to OrcaSlicer's total estimate)
    std::array<bool, kFeatureCount> used{};
    std::array<bool, 16> toolUsed{};               // filaments the print uses
    std::array<double, kFeatureCount> filamentMm{};  // filament pushed per feature (mm of filament)
    std::array<int, kFeatureCount> count{};          // markers: how many (retracts, seams...)
    std::array<float, ViewModes> lo{}, hi{};         // value range of each view type
    std::shared_ptr<const std::string> gcode;        // the G-code text (the G-code viewer)
    std::vector<uint32_t> lineStart;                 // where each line starts in it
    int toolChanges = 0;
    std::array<double, 16> toolMm{};                 // filament pushed per filament (mm)
    Aabb bounds;
    size_t segments = 0;
    std::string error;   // empty: read fine
    bool ok() const { return error.empty() && !layers.empty(); }
};

// From G-code text (OrcaSlicer's comments mark layers and features).
Toolpaths parseGcode(const std::string& gcode, const std::atomic<bool>* cancel = nullptr);
// From a .gcode.3mf (its first plate), or a plain .gcode file.
Toolpaths loadToolpaths(const std::string& path, const std::atomic<bool>* cancel = nullptr);
// One layer's ribbons of one feature as a mesh (nullptr when that layer has none of it).
// upTo: only the layer's first upTo lines (the move slider), in printing order.
MeshPtr toolpathMesh(const ToolLayer& layer, int feature, size_t upTo = (size_t)-1, int mode = ViewFeature, float lo = 0.f, float hi = 1.f);
// mode ViewTool: "feature" is a filament index; ViewSpeed...: "feature" is a gradient step of that value in [lo, hi].
// The value of one line for a view type (speed, height, width, fan, temperature, flow).
float segValue(const ToolSeg& s, float layerHeight, int mode);

// The printer's bed as a thin plate, from the printer profile's "printable_area" (["0x0","256x0",...]).
MeshPtr bedMesh(const std::vector<std::pair<float, float>>& outline);

std::vector<std::pair<float, float>> parsePrintableArea(const std::string& resolvedMachineJson);

} // namespace kst::print
