#pragma once
// Airfoil outlines: NACA 4 and 5 digit generators, a few classic tables, and .dat files (Selig or
// Lednicer format) kept in the user's airfoil folder.
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace kst::airfoils {

// Names offered in the UI: the presets, then every .dat file in `folder` (by file name, no extension).
std::vector<std::string> library(const std::string& folder);

// Unit chord outline from the upper trailing edge round the leading edge to the lower trailing edge,
// `n` points per surface (2n - 1 in all, leading edge shared). Accepts "NACA 2412", "NACA 23012",
// preset names and .dat names in `folder`. Throws std::runtime_error.
std::vector<glm::dvec2> outline(const std::string& name, const std::string& folder, int n = 60);

// Reads a .dat file into a raw outline (any order), for checking an import. Throws.
std::vector<glm::dvec2> readDat(const std::string& path);

} // namespace kst::airfoils
