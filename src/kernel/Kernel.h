#pragma once
#include "core/Types.h"
#include <functional>
#include <string>
#include <vector>

namespace kst::kernel {

using ProgressFn = std::function<void(const std::string&)>;

// Result of importing a file: a small tree of components, unique parts and body instances.
struct ImportResult {
    struct PartOut { std::string name; ShapePtr shape; MeshPtr mesh; bool isMesh = false; std::vector<uint32_t> faceColors; };
    struct CompOut { std::string name; int parent = -1; };     // -1 = import root
    struct BodyOut {
        std::string name;
        int part = 0;
        int comp = -1;
        glm::mat4 xform{1.f};
        bool hasColor = false;
        glm::vec3 color{0.72f, 0.74f, 0.77f};
        // Where it sits in the source assembly (STEP): occurrence path like Fusion's
        // "Frame:1+Wheel:2", the part (component) name and the body's own name.
        std::string occPath, compDef, bodyName;
    };
    std::string rootName;
    bool firstCompIsRoot = false;   // comps[0] is the file's top assembly, merge it into the import root
    std::vector<PartOut> parts;
    std::vector<CompOut> comps;
    std::vector<BodyOut> bodies;
    std::vector<std::string> warnings;
};

// Reads STEP (.step/.stp, with assembly tree, names, colors and instancing), STL, OBJ.
// Throws std::runtime_error on failure. Call from a worker thread.
// STEP files over a few MB are cached after the first read; big ones are read in a second Kestral
// process (see runImportWorker) so Stop ends them at once.
ImportResult importFile(const std::string& path, const CancelToken& cancel, const ProgressFn& progress);
// Body of "Kestral --import-worker <step> <cache> <progress file>". Returns the process exit code.
int runImportWorker(const std::string& path, const std::string& cache, const std::string& progressFile);

// Disk cache of slow feature results, keyed by the feature's chain signature (its inputs and
// everything upstream), so reopening a design does not redo minutes of booleans and threads.
struct FeatureCacheData {
    std::string warn;
    std::vector<ShapePtr> shapes;                         // null = instance of another body
    std::vector<std::string> names;
    std::vector<glm::mat4> xforms;
    std::vector<std::pair<uint32_t, uint32_t>> instanceRefs, consumed;
};
bool saveFeatureCache(uint64_t key, const FeatureCacheData& d);
bool loadFeatureCache(uint64_t key, FeatureCacheData& d);

// Display tessellation of a B-rep (faces with per-triangle face ids plus feature edges).
MeshPtr tessellate(const ShapeHandle& shape, double relDeflection = 0.0005, double angDeflection = 0.2);

struct ExportItem {
    ShapePtr shape;
    MeshPtr mesh;
    glm::mat4 xform{1.f};
    std::string name;
    glm::vec3 color{0.7f};
    std::string component;   // folder path from the root, empty when the body is at the root
    int extruder = 0;        // 3MF for slicing: the filament it prints with (1 based, 0: the default)
    std::vector<std::pair<std::string, std::string>> settings;   // 3MF for slicing: per object settings (OrcaSlicer keys)
    // 3MF for slicing, per triangle of `mesh` (empty: none): painted filament (1 based slot), support (1 on, 2 off),
    // seam (1 here, 2 not here). With any of these the mesh is written as it is (not meshed again).
    std::vector<uint8_t> triColor, triSupport, triSeam;
};
// Binary STL. B-rep bodies are re-meshed finer for printing (on a copy, thread safe).
// Throws on failure.
void exportStl(const std::vector<ExportItem>& items, const std::string& path, const CancelToken& cancel);
// STEP AP214 solids (mesh-only bodies are skipped).
void exportStep(const std::vector<ExportItem>& items, const std::string& path, const CancelToken& cancel);
// 3MF (single object, millimeters).
void export3mf(const std::vector<ExportItem>& items, const std::string& path, const CancelToken& cancel);

// If face `faceIndex` (index used by tessellate) is planar, returns a point on it and its outward normal
// in the shape's own coordinates.
bool facePlane(const ShapeHandle& shape, uint32_t faceIndex, glm::dvec3& origin, glm::dvec3& normal);

std::string kernelVersion();

} // namespace kst::kernel
