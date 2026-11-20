#pragma once
// Every button, menu, field, checkbox and window Kestral drew in the last frame, with its name and
// place on screen. Filled from Dear ImGui's item hooks (IMGUI_ENABLE_TEST_ENGINE), plus names that
// Kestral's own drawn widgets give themselves. Main thread only. Used by the MCP tools that click
// things by their label (app/McpUi.cpp).
#include <imgui.h>

#include <string>
#include <vector>

namespace kst::uiitems {

struct Item {
    unsigned id = 0;
    ImVec2 min, max;           // Dear ImGui coordinates
    std::string label;         // readable name ("" when it has none)
    std::string window;        // the window it is in (its top window's title)
    bool visible = true;       // inside its window's visible area
    bool disabled = false;
    bool checkable = false, checked = false;
    bool openable = false, opened = false;
    bool input = false;        // a text or number field
    bool menuItem = false;     // a row in a menu or popup
    unsigned windowId = 0;     // the (child) window it is drawn in, for scrolling it into view
};

void install();       // after ImGui::CreateContext
void beginFrame();    // before ImGui::NewFrame: the frame just drawn becomes "last"
const std::vector<Item>& last();

// Names for Kestral's own widgets (drawn by hand, so Dear ImGui does not know a label):
void nameLast(const std::string& name);                       // the item just submitted
void nameRect(const std::string& name, ImVec2 a, ImVec2 b);   // a region drawn by hand
void nameNext(const std::string& name);                       // the next item (a text label before a field)

// Scrolls the window holding an item so the item is in the middle of it. False when it cannot.
bool scrollTo(const Item& it);

} // namespace kst::uiitems
