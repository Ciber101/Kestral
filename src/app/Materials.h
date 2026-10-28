#pragma once
// Kestral's appearance library: plastics, metals, glass, wood and more, with the look each one
// gets in the viewport. Used by the Appearance panel and the AI assistant.
#include "core/Document.h"
#include <string>
#include <vector>

namespace kst {

struct LibMaterial {
    const char* category;   // Plastic, Metal, Glass, Wood, Other
    const char* name;
    glm::vec3 color;
    MatProps mat;           // mat.name is filled with `name`
};

const std::vector<LibMaterial>& materialLibrary();
// Case insensitive lookup by name (also matches a unique partial name), null if none.
const LibMaterial* findLibMaterial(const std::string& name);
const char* patternName(MatPattern p);

} // namespace kst
