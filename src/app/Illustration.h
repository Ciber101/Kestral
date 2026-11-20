#pragma once
// Illustrations of a design: clean isometric (or any view) pictures for instructions, posts and the
// README, drawn from the real geometry. Shaded, toon or line art, optionally exploded, with numbered
// balloons and a parts list. PNG (any style) or SVG (vector lines and text). A software render: it
// runs on a worker thread and never touches the viewport or the graphics card.
#include "core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kst::illus {

struct Part {
    MeshPtr mesh;
    glm::mat4 xform{1.f};
    glm::vec3 color{0.72f, 0.74f, 0.77f};
    std::string name;
};

enum class View { Iso, Front, Top, Right, Left, Back, Bottom, Current };
enum class Style { Shaded, Toon, LineArt, ShadedNoLines };
enum class Background { Transparent, White, Dark };

struct Options {
    View view = View::Iso;
    glm::vec3 camFwd{0, 1, 0}, camUp{0, 0, 1};   // for View::Current
    Style style = Style::Toon;
    Background background = Background::White;
    float explode = 0.f;        // 0 assembled, 1 parts spread to about twice their distance
    float lineWeight = 1.f;     // 0.5 .. 3
    int width = 1920, height = 1080;
    bool numbers = false;       // numbered balloons on the parts
    bool partsList = false;     // a list of the numbered parts (name and count)
    std::string font;           // empty: the system's UI font
};

struct Result {
    std::vector<uint8_t> rgba;  // width * height, straight alpha
    int width = 0, height = 0;
    std::string svg;            // when asked for
    int partsNumbered = 0;
};

// Renders the picture (and the SVG when wantSvg). Safe on any thread.
Result render(const std::vector<Part>& parts, const Options& o, bool wantSvg);

// Writes a PNG (compressed, with alpha).
bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h);

const char* viewName(View v);
const char* styleName(Style s);

} // namespace kst::illus
