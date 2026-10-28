#pragma once
// Design previews for the data panel: a small software render of the bodies (no GPU work, safe on
// a worker thread), saved as PNG in Kestral's cache folder.
#include "core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kst::thumb {

struct Item {
    MeshPtr mesh;
    glm::mat4 xform{1.f};
    glm::vec3 color{0.72f, 0.74f, 0.77f};
};

constexpr int kSize = 128;   // pixels, square

// Renders from the home view direction (front, right, top), fitted, on a transparent background.
// Returns kSize * kSize RGBA (straight alpha), empty when there is nothing to draw.
std::vector<uint8_t> render(const std::vector<Item>& items, int size = kSize);

// A picture of the design for the assistant: looking along -eyeDir (eyeDir points from the model
// to the camera), screen up near upHint, fitted, shaded, with the B-rep edges drawn as lines and a
// small axis sign in the corner. Opaque RGBA, w * h.
std::vector<uint8_t> renderView(const std::vector<Item>& items, int w, int h, glm::vec3 eyeDir, glm::vec3 upHint);

// Where a design's preview lives (Kestral's cache folder, one file per design path).
std::string pathFor(const std::string& designPath);

bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h);
// Reads PNG files written by writePng (uncompressed). Anything else returns false.
bool readPng(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h);

} // namespace kst::thumb
