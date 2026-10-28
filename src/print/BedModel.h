#pragma once
// The printer's real build plate, the way OrcaSlicer shows it: the vendor's bed model (an STL),
// the plate with its 10 mm grid, and the plate type's printed labels (Bambu) or the vendor's logo
// texture (other printers), all as plain colored meshes Kestral's renderer draws like any part.
#include "core/Types.h"
#include "print/Orca.h"

#include <string>
#include <utility>
#include <vector>

namespace kst::print {

struct BedDecal {
    MeshPtr mesh;
    uint32_t rgba = 0xFFFFFFFFu;   // RGBA8, R in the low byte
};

struct BedScene {
    MeshPtr plate;                 // the printable area (its bounds are the plate everywhere in Kestral)
    MeshPtr model;                 // the vendor's bed (null: none, the plate alone like Orca's default bed)
    MeshPtr gridThin, gridBold;
    std::vector<BedDecal> decals;  // plate type labels or the vendor logo
    std::string modelFile;         // what was used (for the log)
};

// machine: the printer preset name. bedType: "Textured PEI Plate"... (empty: the printer's default).
BedScene loadBedScene(const OrcaSetup& s, const std::string& machine, const std::string& bedType);

} // namespace kst::print
