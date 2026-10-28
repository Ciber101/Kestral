#pragma once
#include "sketch/Sketch.h"
#include <string>
#include <vector>

namespace kst::sk {

// Block letters as closed outlines when font is empty. I is one rectangle, 0.2 of the height wide.
// A named font (Arial, Segoe UI, ...) uses that font's outlines and is stored on the sketch so the
// words can be changed later. noteId, when set, receives that text's id. Returns the new line ids.
std::vector<uint32_t> addSketchText(Sketch& s, const std::string& text, Vec2 origin, double height, bool construction,
                                    const std::string& font = {}, uint32_t* noteId = nullptr);

// Rewrites a stored text. The old outlines are replaced. Returns false if the id is missing or the
// font produced nothing.
bool editSketchText(Sketch& s, uint32_t noteId, const std::string& text, double height, const std::string& font);

}
