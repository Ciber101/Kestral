// Kestral's built in AI assistant. Talks to Claude (Anthropic API, your key), a local model through
// Ollama, or any OpenAI compatible server (LM Studio, llama.cpp, vLLM...). It knows the app (every
// command, shortcut and how things work) and can act on the design through a small set of tools:
// read the design, run any command, select, show / hide, color, move, delete, rename, set user
// parameters and create boxes and cylinders. Every action goes through the normal undo stack.
#include "app/App.h"

#include <GLFW/glfw3.h>
#include "app/Materials.h"
#include "app/SliceStyle.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Params.h"
#include "kernel/Features.h"
#include "platform/Platform.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <mutex>
#include <cmath>
#include <set>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

namespace kst {

using json::Value;
using namespace ui;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string hexColor(const glm::vec3& c) {
    char b[16];
    std::snprintf(b, sizeof b, "#%02X%02X%02X", (int)std::lround(std::clamp(c.r, 0.f, 1.f) * 255),
                  (int)std::lround(std::clamp(c.g, 0.f, 1.f) * 255), (int)std::lround(std::clamp(c.b, 0.f, 1.f) * 255));
    return b;
}

bool parseHex(const std::string& s, glm::vec3& out) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6) return false;
    unsigned v = 0;
    for (char c : h) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return false;
    }
    out = {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f};
    return true;
}

// Removes <think>...</think> blocks some local models (Qwen3, DeepSeek R1) put in replies.
// thought (optional) gets what was inside them.
std::string stripThinking(std::string s, std::string* thought = nullptr) {
    for (;;) {
        auto a = s.find("<think>");
        if (a == std::string::npos) break;
        auto b = s.find("</think>", a);
        if (thought) *thought += s.substr(a + 7, (b == std::string::npos ? s.size() : b) - a - 7);
        if (b == std::string::npos) { s.erase(a); break; }
        s.erase(a, b + 8 - a);
    }
    size_t f = s.find_first_not_of(" \n\r\t");
    return f == std::string::npos ? std::string() : s.substr(f);
}

// ---------------------------------------------------------------- tool schemas

struct ToolDef { const char* name; const char* desc; const char* schema; };

const ToolDef kTools[] = {
    {"find_tools", "Search Kestral's tools and commands by words (e.g. \"thread\", \"chamfer\", \"gear\", \"brim\", \"AMS slots\", \"appearance wood\"). Kestral has about 110 tools; only the most common are loaded. The ones found are loaded for your next step: call them then. Also lists matching commands for run_command and appearance names.",
     R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})"},
    {"get_design", "Read the current design: components (folders), bodies with path, size, center and color, the timeline, user parameters and the current selection. Call this before changing anything.",
     R"({"type":"object","properties":{}})"},
    {"look_at_design", "Take a picture of the design to check your work with your own eyes: shapes, holes, chamfers, positions, "
     "colors. Views: iso (front right top), front (looking along +Y), back, left, right (looking along -X), top (looking down), "
     "bottom, current (what the user sees). Several views come as one picture, panels left to right then top to bottom in the "
     "order asked. The axis sign in the corner shows X red, Y green, Z blue. Use it after building something, to compare with "
     "the user's request or picture. If you cannot see pictures, Kestral's picture helper looks and answers your question.",
     R"({"type":"object","properties":{"views":{"type":"array","items":{"type":"string","enum":["iso","front","back","left","right","top","bottom","current"]},"description":"default: iso, front, top, right"},"bodies":{"type":"array","items":{"type":"string"},"description":"only these body paths, default every visible body"},"question":{"type":"string","description":"what to check, e.g. 'is the hole centred on the top face?'"}}})"},
    {"run_command", "Run any Kestral command by its id (see the command list in the instructions), exactly like clicking it. Commands that need picks or values open their dialog for the user.",
     R"({"type":"object","properties":{"command_id":{"type":"string"}},"required":["command_id"]})"},
    {"select", "Select bodies by name or path (\"Folder/Body\"). A folder name selects every body in it. add=true keeps the current selection.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"add":{"type":"boolean"}},"required":["names"]})"},
    {"save_design", "Save the open design (like Ctrl+S). Every save is a new version (v1, v2...).",
     R"({"type":"object","properties":{}})"},
    {"open_design", "Open a design by name (or full path). Searches the Kestral designs folder and recent designs. version (optional) opens that older version in place.",
     R"({"type":"object","properties":{"name":{"type":"string"},"version":{"type":"integer"}},"required":["name"]})"},
    {"list_designs", "List the saved designs (name, version, path). query filters by name.",
     R"({"type":"object","properties":{"query":{"type":"string"}}})"},
    {"set_visibility", "Show or hide bodies (names, paths or folder names), sketches or construction planes by name, or \"all sketches\" / \"all planes\". Undo reverses it.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"visible":{"type":"boolean"}},"required":["names","visible"]})"},
    {"set_color", "Set the color of bodies. color is #RRGGBB.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"color":{"type":"string"}},"required":["names","color"]})"},
    {"set_appearance", "Give bodies (or folders) a look. material is a name from the appearance library in the instructions (e.g. 'Aluminum - Brushed', 'Glass - Clear', 'Wood - Oak', 'Carbon Fiber'). Any of color (#RRGGBB), opacity (1 solid, 0.2 glassy), metal (0..1), roughness (0..1) and glow (0..1) can be given too, alone or to tune the material.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"material":{"type":"string"},"color":{"type":"string"},"opacity":{"type":"number"},"metal":{"type":"number"},"roughness":{"type":"number"},"glow":{"type":"number"}},"required":["names"]})"},
    {"section_analysis", "Cut the view on an origin plane. The solids do not change. plane is XY, XZ or YZ. offset is the plane position in mm along Z, Y or X. flip hides the other side. on false turns the section off.",
     R"({"type":"object","properties":{"plane":{"type":"string"},"offset":{"type":"number"},"flip":{"type":"boolean"},"on":{"type":"boolean"}}})"},
    {"move", "Move bodies by an offset in millimeters (adds a Move step to the timeline). copy=true moves copies instead. If a body is in a rigid group, the other bodies in that group move by the same amount.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"copy":{"type":"boolean"}},"required":["names"]})"},
    {"delete", "Delete bodies (adds a Remove step to the timeline, undoable).",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}}},"required":["names"]})"},
    {"rename", "Rename a body or a folder.",
     R"({"type":"object","properties":{"name":{"type":"string"},"new_name":{"type":"string"}},"required":["name","new_name"]})"},
    {"set_parameter", "Create or change a user parameter. expression can be a number (mm or degrees) or math using other parameters.",
     R"({"type":"object","properties":{"name":{"type":"string"},"expression":{"type":"string"}},"required":["name","expression"]})"},
    {"create_box", "Create a solid box. x,y,z is the corner with the smallest coordinates, in mm. width, depth and height may be numbers or parameter expressions. name and component (a browser folder) are optional.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"width":{},"depth":{},"height":{},"name":{"type":"string"},"component":{"type":"string"}},"required":["width","depth","height"]})"},
    {"create_cylinder", "Create a cylinder or a tube. x,y,z is the center of the base. axis is x, y or z (default z) or [dx,dy,dz]. inner_diameter makes a tube. Sizes may be expressions. name and component optional.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"diameter":{},"height":{},"inner_diameter":{},"axis":{},"name":{"type":"string"},"component":{"type":"string"}},"required":["diameter","height"]})"},
    {"create_cone", "Cone or frustum. bottom_diameter, top_diameter (0 for a point), height. axis x, y or z. Base center at x,y,z.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"bottom_diameter":{"type":"number"},"top_diameter":{"type":"number"},"height":{"type":"number"},"axis":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"}},"required":["bottom_diameter","height"]})"},
    {"create_sphere", "Sphere. diameter, center x,y,z.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"diameter":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}},"required":["diameter"]})"},
    {"create_torus", "Torus. major_diameter is the centerline diameter, tube_diameter is the ring thickness. axis x, y or z.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"major_diameter":{"type":"number"},"tube_diameter":{"type":"number"},"axis":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"}},"required":["major_diameter","tube_diameter"]})"},
    {"create_wedge", "Right triangular prism. length along X, width along Y, height along Z, from corner x,y,z.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"length":{"type":"number"},"width":{"type":"number"},"height":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}},"required":["length","width","height"]})"},
    {"create_tube", "Tube. Same as create_cylinder with inner_diameter.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"diameter":{},"inner_diameter":{},"height":{},"axis":{},"name":{"type":"string"},"component":{"type":"string"}},"required":["diameter","inner_diameter","height"]})"},
    {"create_hole", "Drill holes straight into faces, no sketch needed. positions: list of [x,y,z] points in mm ON the face. diameter in mm, depth in mm (leave out or 0 for through the target). target_bodies limits the cut to those bodies; without it a through hole cuts every body it reaches. type: simple, counterbore or countersink. direction [dx,dy,dz] only if the hole should not go straight into the face. spotface_diameter and spotface_depth add a flat seat. drill_angle is the included angle of the point on a blind hole, blank for a flat bottom. tapped true cuts an ISO metric thread into the wall; pitch is required then.",
     R"({"type":"object","properties":{"positions":{"type":"array","items":{"type":"array","items":{"type":"number"}}},"diameter":{"type":"number"},"depth":{"type":"number"},"target_bodies":{"type":"array","items":{"type":"string"}},"type":{"type":"string","enum":["simple","counterbore","countersink"]},"counterbore_diameter":{"type":"number"},"counterbore_depth":{"type":"number"},"countersink_diameter":{"type":"number"},"countersink_angle":{"type":"number"},"spotface_diameter":{"type":"number"},"spotface_depth":{"type":"number"},"drill_angle":{"type":"number"},"tapped":{"type":"boolean"},"pitch":{"type":"number"},"direction":{"type":"array","items":{"type":"number"}}},"required":["positions","diameter"]})"},
    {"fillet", "Round edges. edges: all, vertical, horizontal, top, bottom, outline (rounds the corners of a plate seen from its face, however it stands), or x, y, z (straight edges along that axis). Or edge_refs: [{\"body\":\"Name\",\"edges\":[0,3]}] using ids from get_body. radius in mm. radius_end, when set, is the radius at the other end of each edge, changing linearly along it.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"radius":{"type":"number"},"radius_end":{"type":"number"},"edges":{"type":"string"},"edge_refs":{"type":"array"}},"required":["radius"]})"},
    {"full_round", "Replace one flat face with a round tangent to the two parallel faces beside it. body names the solid. face is the face id from get_body. The radius is half the distance between those faces. A face that is not between two parallel faces is refused.",
     R"({"type":"object","properties":{"body":{"type":"string"},"names":{"type":"array","items":{"type":"string"}},"face":{"type":"number"}},"required":["face"]})"},
    {"chamfer", "Bevel edges. edges groups (all, vertical, horizontal, top, bottom, outline, x, y, z) or edge_refs like fillet. distance in mm. distance2, when set, is the distance along the other face. Leave it out for the same distance on both faces. Swap the two numbers to swap the faces.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"distance":{"type":"number"},"distance2":{"type":"number"},"edges":{"type":"string"},"edge_refs":{"type":"array"}},"required":["distance"]})"},
    {"get_body", "List one body's faces: id, type (plane, cylinder, cone, sphere, torus), center, normal, radius. body is a name or path from get_design.",
     R"({"type":"object","properties":{"body":{"type":"string"}},"required":["body"]})"},
    {"create_component", "Make a component and activate it. It goes inside the active component; parent names another one, or \"\" for the top of the design. New bodies go into the active component until you activate another. name is the component. New bodies can also name a component in their component field.",
     R"({"type":"object","properties":{"name":{"type":"string"},"parent":{"type":"string"}},"required":["name"]})"},
    {"activate_component", "Make a component the active one. New bodies go there. Leave name empty to activate the root design.",
     R"({"type":"object","properties":{"name":{"type":"string"}}})"},
    {"ground_component", "Lock a component so a move or joint cannot shift its bodies. grounded false unlocks it. The root design cannot be grounded.",
     R"({"type":"object","properties":{"name":{"type":"string"},"grounded":{"type":"boolean"}},"required":["name"]})"},
    {"export_step", "Write solid bodies to a STEP file. Each body keeps its name, and a body in a component stays in that folder. path is the file. names limits it to those bodies; without names, every visible solid is written. Mesh bodies are skipped.",
     R"({"type":"object","properties":{"path":{"type":"string"},"names":{"type":"array","items":{"type":"string"}}},"required":["path"]})"},
    {"import_file", "Import a STEP, STL, or OBJ file from a path on disk.",
     R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"},
    {"create_sketch", "Start a sketch and return its id. plane is XY, XZ, YZ, or an object {origin, normal, x_dir}, or set body and face (a flat face index from get_body). offset shifts it along the normal, in mm. Coordinates later are in this plane.",
     R"({"type":"object","properties":{"plane":{},"offset":{"type":"number"},"body":{"type":"string"},"face":{"type":"number"}}})"},
    {"sketch_add", "Add curves to a sketch. entities: line {p1,p2}, rect {p1,p2} or {x,y,w,h}, rect_3pt {p1,p2,p3}, circle {center,d} or {x,y,r}, arc {center,start,end} or {p1,p2,p3}, polygon {center,sides,r}, polygon_in {center,sides,r}, slot {p1,p2,width}, slot_overall {p1,p2,width}. For an overall slot, p1 and p2 are the outer ends and the length includes the rounded caps. slot_center {center, end, width}. end is the center of one cap, and the other cap mirrors through center. slot_arc {p1, p2, p3, width}. p1 and p3 are the ends and p2 lies on the arc. point {p}, spline {points:[[x,y],...], closed}, control_spline {points:[[x,y],...], closed}, ellipse {center, major, minor, rotation}, ellipse_arc {center, major, minor, rotation, start, end}, text {text, x, y, height, font}. rotation, start and end are degrees. An ellipse arc runs counter clockwise from start to end. With no font, text is block-letter outlines. I is a rectangle 0.2 of the height wide. A font name such as Arial uses that font and returns text_id. Call again with that text_id, a new text, height, and font to rewrite it. construction true draws construction geometry. Returns entity ids.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"entities":{"type":"array"},"construction":{"type":"boolean"}},"required":["sketch_id","entities"]})"},
    {"sketch_constrain", "Add constraints. Each item has type (coincident, horizontal, vertical, parallel, perpendicular, equal, tangent, fix, distance, radius, diameter, angle) and refs (entity or point ids). Dimensions take value as a number or an expression. driven true makes a dimension report the current size and not drive it.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"constraints":{"type":"array"}},"required":["sketch_id","constraints"]})"},
    {"sketch_ops", "offset, trim, break, fillet, chamfer, mirror, move, rotate, scale, copy, pattern_rect, pattern_circ, project, intersect, project_face, or point_cross on a sketch. offset: {op, entity, distance}. trim: {op, entity, at:[x,y]}. break: {op, entity} splits that line, arc, or circle where other curves cross it. fillet: {op, corner, radius} a point id where two lines meet. chamfer: {op, corner, distance} the same corner, cut back by that distance on both lines. mirror: {op, entities, axis} axis is a line id. move: {op, entities, dx, dy}. rotate: {op, entities, angle, pivot:[x,y]} angle is degrees, counter clockwise, pivot defaults to the origin. scale: {op, entities, factor, pivot:[x,y]}. copy: {op, entities, dx, dy} duplicates the curves and shifts the copies. copy also takes angle, factor, and pivot. pattern_rect: {op, entities, count_x, spacing_x, count_y, spacing_y}. pattern_circ: {op, entities, count, angle, pivot:[x,y]}. Counts include the original. A full circle uses angle 360, so the last copy does not land on the first. project: {op, body} projects that body's edges. intersect: {op, body} adds the outline where that body crosses the sketch plane. project_face: {op, body, entities, face or at:[x,y,z], direction:[x,y,z]} drops those curves onto the face along that direction and adds the outline to the sketch. The curves stay. Direction defaults to the sketch normal. Omit entities to drop every line, arc, and circle. point_cross: {op, at:[x,y]} or {op, entities:[a,b], at} adds a point where two curves cross. The point stays on both. at picks which crossing when they meet twice. Lines, arcs, and circles.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"op":{"type":"string"},"entity":{"type":"number"},"entities":{"type":"array"},"distance":{"type":"number"},"corner":{"type":"number"},"axis":{"type":"number"},"at":{"type":"array"},"body":{"type":"string"}},"required":["sketch_id","op"]})"},
    {"get_sketch", "Sketch contents: entities, profiles (id, area, closed) and remaining degrees of freedom.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"}},"required":["sketch_id"]})"},
    {"extrude", "Extrude sketch profiles. distance (number or expression), or to_body (a body name) to stop on that body's flat face. to_face is an optional face id from get_body. direction one, symmetric or two, taper_angle, operation new, join, cut or intersect, target_bodies for join/cut/intersect, profile index or all. thickness makes a wall from an open curve. side is center, one, or other.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"distance":{},"to_body":{"type":"string"},"to_face":{"type":"number"},"direction":{"type":"string"},"taper_angle":{},"operation":{"type":"string"},"target_bodies":{"type":"array","items":{"type":"string"}},"profile":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_id"]})"},
    {"revolve", "Revolve sketch profiles. axis is X or Y (the sketch axes) or a line entity id. angle in degrees. operation and target_bodies like extrude. thickness makes a wall from an open curve. side is center, one, or other.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"angle":{},"axis":{},"operation":{"type":"string"},"target_bodies":{"type":"array","items":{"type":"string"}},"profile":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_id"]})"},
    {"sweep", "Sweep a closed profile sketch along a path sketch. twist is degrees the profile turns from the start of the path to the end. 0 leaves it as drawn. guide_sketch is an open sketch. The profile scales so it meets that curve. A guide replaces the twist. thickness makes a wall from an open profile curve. side is center, one, or other.",
     R"({"type":"object","properties":{"profile_sketch":{"type":"number"},"path_sketch":{"type":"number"},"guide_sketch":{"type":"number"},"twist":{},"operation":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"}},"required":["profile_sketch","path_sketch"]})"},
    {"loft", "Loft through two or more profile sketches, in order. rails is a list of open sketch ids. The loft grows so each section meets those curves. point is [x, y, z] in world mm. point_end is start or end. The loft comes to that point, and one profile is enough. A point does not take rails. centerline is an open sketch id. The middle of each profile follows that curve, and the profiles keep their size. A centerline does not take rails or a point.",
     R"({"type":"object","properties":{"sketch_ids":{"type":"array","items":{"type":"number"}},"rails":{"type":"array","items":{"type":"number"}},"operation":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_ids"]})"},
    {"rotate", "Turn bodies. axis is x, y, z or [dx,dy,dz]. angle in degrees. pivot is center or [x,y,z]. copy true keeps the original.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"axis":{},"angle":{"type":"number"},"pivot":{},"copy":{"type":"boolean"}},"required":["names","angle"]})"},
    {"scale", "Scale bodies. factor, or x, y and z for a non-uniform scale. pivot is center or a point.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"factor":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"pivot":{}},"required":["names"]})"},
    {"set_transform", "Place bodies with a 4x4 row-major matrix (16 numbers). This replaces their placement.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"matrix":{"type":"array","items":{"type":"number"}}},"required":["names","matrix"]})"},
    {"align", "Move a body so a face, edge middle or corner lands on one on another body. No clicks. kind is face, edge or corner. index is the face or edge id from get_body. corner end is 1 or 2.",
     R"({"type":"object","properties":{"source_body":{"type":"string"},"source_kind":{"type":"string"},"source_index":{"type":"number"},"source_end":{"type":"number"},"target_body":{"type":"string"},"target_kind":{"type":"string"},"target_index":{"type":"number"},"target_end":{"type":"number"},"flip":{"type":"boolean"}},"required":["source_body","target_body"]})"},
    {"combine", "Boolean. operation join, cut or intersect. target is the body that remains. tools are the other bodies. keep_tools leaves the tools in the design.",
     R"({"type":"object","properties":{"target":{"type":"string"},"tools":{"type":"array","items":{"type":"string"}},"operation":{"type":"string"},"keep_tools":{"type":"boolean"}},"required":["target","tools","operation"]})"},
    {"circular_pattern", "Copy bodies around an axis. count includes the original. angle is the total sweep in degrees (360 for a full circle). axis is x, y or z.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"count":{"type":"number"},"angle":{"type":"number"},"axis":{"type":"string"}},"required":["names","count"]})"},
    {"rect_pattern", "Copy bodies on a grid in world X and Y. count_x and count_y include the original. spacing is the step in mm.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"count_x":{"type":"number"},"spacing_x":{"type":"number"},"count_y":{"type":"number"},"spacing_y":{"type":"number"}},"required":["names","count_x","spacing_x"]})"},
    {"pattern_on_path", "Copy bodies along a sketch curve, or along an edge. count includes the original. distance is the spacing in mm, or the length from the first copy to the last when distance_type is extent. orientation identical keeps the rotation. path turns each copy to follow the curve. flip reverses the path. Pass sketch_id, or edge_body plus edge.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"sketch_id":{"type":"number"},"entity":{"type":"number"},"edge_body":{"type":"string"},"edge":{"type":"number"},"count":{"type":"number"},"distance":{"type":"number"},"distance_type":{"type":"string"},"orientation":{"type":"string"},"flip":{"type":"boolean"}},"required":["names","count","distance"]})"},
    {"mirror", "Mirror bodies across an origin plane: XY, XZ or YZ.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"plane":{"type":"string"}},"required":["names","plane"]})"},
    {"shell", "Hollow a body. thickness in mm. side is inside (default), outside, or both (wall centered on the faces, closed shells only). remove_faces is a list of face ids to open (from get_body). Leave it out for a closed cavity.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"thickness":{"type":"number"},"side":{"type":"string"},"remove_faces":{"type":"array","items":{"type":"number"}}},"required":["names","thickness"]})"},
    {"thicken", "Give faces a thickness and make a new solid. The body you picked stays. thickness in mm. side is one (default, along the outward normal), other, or both (centered on the face). faces is a list of face ids from get_body. at is a point on the face when faces is left out.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"thickness":{"type":"number"},"side":{"type":"string"},"faces":{"type":"array","items":{"type":"number"}},"at":{"type":"array"},"name":{"type":"string"}},"required":["names","thickness"]})"},
    {"split_body", "Split bodies with an origin plane XY, XZ or YZ (through the world origin), along a sketch line (sketch_id and line, the line's entity id: the cut is square to that sketch), or with any plane (origin and normal, e.g. a face's plane from get_body).",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"plane":{"type":"string"},"sketch_id":{"type":"number"},"line":{"type":"number"},"origin":{"type":"array"},"normal":{"type":"array"}},"required":["names"]})"},
    {"gear", "Spur, helical, herringbone, internal, rack, bevel, worm or worm_wheel. module, teeth, width, pressure_angle, helix, bore. plane XY, XZ or YZ.",
     R"({"type":"object","properties":{"type":{"type":"string"},"module":{"type":"number"},"teeth":{"type":"number"},"width":{"type":"number"},"pressure_angle":{"type":"number"},"helix":{"type":"number"},"bore":{"type":"number"},"plane":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}}})"},
    {"blades", "Propeller, heli, ducted_fan, wind or marine. diameter, blade_count, pitch, hub_diameter, airfoil and the other generator fields.",
     R"({"type":"object","properties":{"type":{"type":"string"},"diameter":{"type":"number"},"blade_count":{"type":"number"},"pitch":{"type":"number"},"hub_diameter":{"type":"number"},"airfoil":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"}}})"},
    {"thread", "Thread a round face with a smooth helix. body, and face (a cylinder face id from get_body, or omit to use the first round face). diameter and pitch for a custom size. cosmetic true draws it without cutting. internal true for a hole.",
     R"({"type":"object","properties":{"body":{"type":"string"},"face":{"type":"number"},"diameter":{"type":"number"},"pitch":{"type":"number"},"cosmetic":{"type":"boolean"},"internal":{"type":"boolean"}},"required":["body"]})"},
    {"wing", "Fixed wing. root_chord, span, tip_chord, airfoil (a NACA name or a library name). hollow and mirror are optional.",
     R"({"type":"object","properties":{"root_chord":{"type":"number"},"span":{"type":"number"},"tip_chord":{"type":"number"},"airfoil":{"type":"string"},"hollow":{"type":"boolean"},"mirror":{"type":"boolean"},"name":{"type":"string"},"component":{"type":"string"}}})"},
    {"dovetail", "Dovetail. type: key, tab, sliding, puzzle, through, half_blind, secret, slide. length, width, depth, angle, clearance.",
     R"({"type":"object","properties":{"type":{"type":"string"},"length":{"type":"number"},"width":{"type":"number"},"depth":{"type":"number"},"angle":{"type":"number"},"clearance":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}}})"},
    {"robot", "Robot description for URDF and USD export (Isaac Sim, Isaac Lab, ROS). Links are components (with everything inside them). action: show (the setup and a check: links, masses, problems), set (name, base = the root link component, density in g/cm3 for printed bodies, front = the way the robot faces in the design: x, -x, y or -y, default -y; the export turns it so front is +X), mass (component or body, grams: the real mass of a bought part like a servo, board or battery; 0 clears it), joint (name, parent, child = component paths, type revolute / continuous / fixed, origin [x,y,z] mm on the axis, axis [dx,dy,dz] or x / y / z / -x..., lower and upper in degrees, effort N m, velocity rad/s, stiffness, damping; the same name replaces it), remove_joint (name), export (path to a .urdf; also writes the .usda and the meshes next to it; blank path uses designs/Robots). The design's current pose is the zero pose.",
     R"({"type":"object","properties":{"action":{"type":"string","enum":["show","set","mass","joint","remove_joint","export"]},"name":{"type":"string"},"base":{"type":"string"},"density":{"type":"number"},"front":{"type":"string"},"component":{"type":"string"},"grams":{"type":"number"},"parent":{"type":"string"},"child":{"type":"string"},"type":{"type":"string"},"origin":{"type":"array","items":{"type":"number"}},"axis":{},"lower":{"type":"number"},"upper":{"type":"number"},"effort":{"type":"number"},"velocity":{"type":"number"},"stiffness":{"type":"number"},"damping":{"type":"number"},"path":{"type":"string"}},"required":["action"]})"},
    {"interference", "Report which of the named bodies share volume. Touching faces do not count.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}}},"required":["names"]})"},
    {"measure", "Measure. kind is distance (default), angle, radius, area, or loop. Distance: points is two [x,y,z], or names is two bodies (center to center). Angle: points is three [x,y,z] with the corner in the middle, or body with face and face2, or edge and edge2. The angle is the smaller one, 0 to 90 degrees. Radius: a cylinder, sphere, torus, or circular edge. Area: a face, in mm2. Loop: the outline of a face, or the wire an edge sits on, in mm. body names the solid. Leave face out to use the first round face or the first flat face.",
     R"({"type":"object","properties":{"kind":{"type":"string","enum":["distance","angle","radius","area","loop"]},"points":{"type":"array"},"names":{"type":"array","items":{"type":"string"}},"body":{"type":"string"},"face":{"type":"number"},"face2":{"type":"number"},"edge":{"type":"number"},"edge2":{"type":"number"}}})"},
    {"offset_plane", "Construction plane offset from XY, XZ or YZ. offset in mm along the plane normal.",
     R"({"type":"object","properties":{"plane":{"type":"string"},"offset":{"type":"number"}},"required":["offset"]})"},
    {"plane_at_angle", "Construction plane tilted from XY, XZ or YZ. angle is degrees. axis is x or y of that plane. The hinge stays on the origin.",
     R"({"type":"object","properties":{"plane":{"type":"string"},"axis":{"type":"string"},"angle":{"type":"number"}},"required":["angle"]})"},
    {"midplane", "Construction plane halfway between an origin plane and another construction plane. other_plane is a plane_id from offset_plane or plane_at_angle. If you leave it out, the newest other plane is used.",
     R"({"type":"object","properties":{"plane":{"type":"string"},"other_plane":{"type":"number"}},"required":["plane"]})"},
    {"plane_3pt", "Construction plane through three points. x1,y1,z1 is the origin. x2,y2,z2 and x3,y3,z3 set the plane. The three points must not lie on one line. Returns plane_id, origin and normal.",
     R"({"type":"object","properties":{"x1":{},"y1":{},"z1":{},"x2":{},"y2":{},"z2":{},"x3":{},"y3":{},"z3":{}},"required":["x1","y1","z1","x2","y2","z2","x3","y3","z3"]})"},
    {"plane_tangent", "Construction plane tangent to a cylinder. body names the solid. face is the face id, or omit it to use the first cylinder face. angle is degrees around the cylinder axis and picks which side. A cone or a sphere is refused. Returns plane_id, origin and normal.",
     R"({"type":"object","properties":{"body":{"type":"string"},"face":{"type":"number"},"angle":{}},"required":["body"]})"},
    {"axis", "Construction axis. Through two points (x1,y1,z1 and x2,y2,z2), or along a straight edge (body and edge, an edge id), or through a cylinder (body, and face of a round face, or omit face to use the first round face). Returns axis_id, origin and direction.",
     R"({"type":"object","properties":{"x1":{},"y1":{},"z1":{},"x2":{},"y2":{},"z2":{},"body":{"type":"string"},"edge":{"type":"number"},"face":{"type":"number"}}})"},
    {"point", "Construction point. At coordinates x, y, z, or at the middle of an edge (body and edge id), or at the center of a flat face (body and face id), or on a corner (body and vertex id). Returns point_id and the position.",
     R"({"type":"object","properties":{"x":{},"y":{},"z":{},"body":{"type":"string"},"edge":{"type":"number"},"face":{"type":"number"},"vertex":{"type":"number"}}})"},
    {"split_face", "Divide faces. The body stays one solid and its volume does not change. With sketch_id, that sketch's curve is projected along the sketch normal. Without it, plane is XY, XZ or YZ and offset is mm along that plane's normal (XY offset is Z).",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"plane":{"type":"string"},"offset":{},"sketch_id":{"type":"number"}},"required":["names"]})"},
    {"create_coil", "Coil, a round wire wound into a spring. diameter is the centerline diameter, section is the wire diameter, turns is the number of revolutions, height is the length along the axis. axis is x, y or z. left_hand winds the other way.",
     R"({"type":"object","properties":{"diameter":{"type":"number"},"height":{"type":"number"},"turns":{"type":"number"},"section":{"type":"number"},"axis":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"left_hand":{"type":"boolean"},"name":{"type":"string"},"component":{"type":"string"}},"required":["diameter","height","turns","section"]})"},
    {"joint", "Joint. stay is the body that does not move. move shifts so its snap point lands on stay's snap point. type is rigid (default), revolute, or slider. Revolute then turns move about the stay point by angle degrees around axis x, y, or z. Slider then slides move along that axis by distance mm. Leave the points out to use each body's center. stay_x, stay_y, stay_z and move_x, move_y, move_z are world points.",
     R"({"type":"object","properties":{"stay":{"type":"string"},"move":{"type":"string"},"type":{"type":"string","enum":["rigid","revolute","slider"]},"angle":{"type":"number"},"distance":{"type":"number"},"axis":{"type":"string"},"stay_x":{"type":"number"},"stay_y":{"type":"number"},"stay_z":{"type":"number"},"move_x":{"type":"number"},"move_y":{"type":"number"},"move_z":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}},"required":["stay","move"]})"},
    {"rigid_group", "Lock two or more bodies together. They stay where they are. A later move of one moves the rest by the same amount. Scale and set_transform do not. names is the bodies.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["names"]})"},
    {"asbuilt_joint", "As-built rigid joint. stay and move are two bodies that are already in the right place. Neither body moves. Leave the points out to use each body's center. stay_x, stay_y, stay_z and move_x, move_y, move_z are world points for the joint origin.",
     R"({"type":"object","properties":{"stay":{"type":"string"},"move":{"type":"string"},"stay_x":{"type":"number"},"stay_y":{"type":"number"},"stay_z":{"type":"number"},"move_x":{"type":"number"},"move_y":{"type":"number"},"move_z":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}},"required":["stay","move"]})"},
    {"silhouette", "Split a body on the outline seen along an axis. axis is x, y, or z. names is the body. The outline has to lie in one plane, and that plane has to cut through the body. A sphere along z becomes two hemispheres.",
     R"({"type":"object","properties":{"axis":{"type":"string"},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["names"]})"},
    {"lip", "A ridge around a closed sketch, joined to a body. The sketch is the outside of the ridge. width is how far it comes in from that outline. height grows along the sketch normal. flip grows the other way. groove cuts the ridge into the body instead. names is the body.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"width":{"type":"number"},"height":{"type":"number"},"flip":{"type":"boolean"},"groove":{"type":"boolean"},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_id","width","height","names"]})"},
    {"rest", "A flat pad joined to a body. The closed sketch is one face of the pad. thickness grows along the sketch normal. flip grows the other way. names is the body. The pad has to meet that body.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"thickness":{"type":"number"},"flip":{"type":"boolean"},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_id","thickness","names"]})"},
    {"snap", "A cantilever hook on a face. x, y, z is the center of the beam where it meets the body. length is along axis. width and thickness size the beam. hook is how far the catch sticks out, 0 for a plain beam. hook_length is how long the catch is at the tip. On Z the catch goes toward +X. names is the body.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"length":{"type":"number"},"width":{"type":"number"},"thickness":{"type":"number"},"hook":{"type":"number"},"hook_length":{"type":"number"},"axis":{"type":"string"},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["length","width","thickness","names"]})"},
    {"boss", "A round post on a face. x, y, z is the center of the base. diameter and height in mm. hole is the through hole diameter, 0 for solid. axis is x, y or z. names is the body it joins.",
     R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"diameter":{"type":"number"},"height":{"type":"number"},"hole":{"type":"number"},"axis":{"type":"string"},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["diameter","height","names"]})"},
    {"emboss", "Project a closed sketch onto a body along the sketch normal and raise it by depth. deboss cuts that depth in instead. flip projects the other way. names is the body. The sketch can sit off the face. The pad starts where the profile hits.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"depth":{"type":"number"},"deboss":{"type":"boolean"},"flip":{"type":"boolean"},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_id","depth"]})"},
    {"web", "Thin wall whose open sketch is the side view. Thickness sticks out of the sketch plane. The wall grows across the plane, perpendicular to the sketch curve, until it meets a body. flip grows the other way. side is centered or one. names is the body to join.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"thickness":{"type":"number"},"flip":{"type":"boolean"},"side":{"type":"string","enum":["centered","one"]},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_id","thickness"]})"},
    {"rib", "Thin wall from an open sketch. It grows along the sketch normal until it meets a body, then joins that body. thickness in mm. flip grows the other way. side is centered or one. names is the body to join; leave it out and the rib joins whatever it hits.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"thickness":{"type":"number"},"flip":{"type":"boolean"},"side":{"type":"string","enum":["centered","one"]},"names":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"component":{"type":"string"}},"required":["sketch_id","thickness"]})"},
    {"pipe", "Pipe along a path. sketch_id is a path sketch (one chain of lines). Without it, a straight pipe of length along axis from x,y,z. diameter is the outside, inner_diameter 0 is solid.",
     R"({"type":"object","properties":{"sketch_id":{"type":"number"},"diameter":{"type":"number"},"inner_diameter":{"type":"number"},"length":{"type":"number"},"axis":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"name":{"type":"string"},"component":{"type":"string"}},"required":["diameter"]})"},
    {"draft", "Lean side faces so a mold releases. angle in degrees. direction is the pull (x, y or z). The base opposite the pull stays the same size. faces is an optional list of face ids from get_body; without it every side face is drafted.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"angle":{"type":"number"},"direction":{"type":"string"},"faces":{"type":"array","items":{"type":"number"}}},"required":["names","angle"]})"},
    {"press_pull", "Move faces. distance in mm, positive grows the body. faces is a list of face ids from get_body. direction, when faces is omitted, picks the face whose normal matches it (default +z).",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"distance":{"type":"number"},"faces":{"type":"array","items":{"type":"number"}},"direction":{"type":"string"}},"required":["names","distance"]})"},
    {"offset_face", "Same as press_pull. Shift faces along their normal.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"distance":{"type":"number"},"faces":{"type":"array","items":{"type":"number"}},"direction":{"type":"string"}},"required":["names","distance"]})"},
    {"replace_face", "Move flat faces onto an origin plane. plane is XY, XZ or YZ. offset is millimetres along that plane's normal. faces is optional; without it the face whose normal matches the plane is used.",
     R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"plane":{"type":"string"},"offset":{"type":"number"},"faces":{"type":"array","items":{"type":"number"}}},"required":["names"]})"},
    {"print_setup", "Check the 3D printing setup: is OrcaSlicer installed, which printer / process / filament profiles it has selected, which printers it knows on the network, and how Kestral reaches the printer. Call this first for anything about slicing or printing.",
     R"({"type":"object","properties":{}})"},
    {"list_print_profiles", "Find OrcaSlicer profiles. type is machine (printer), process (quality / strength) or filament. search is words to match, e.g. \"a1 mini\", \"petg\", \"0.20mm strength\". The user's own profiles come first.",
     R"({"type":"object","properties":{"type":{"type":"string","enum":["machine","process","filament"]},"search":{"type":"string"}},"required":["type"]})"},
    {"slice", "Slice the visible design (or only the selection with what=selected) with OrcaSlicer's engine (it runs hidden inside Kestral). Profiles default to what the Slice tab has picked; give names from list_print_profiles to use others. settings changes any OrcaSlicer setting for this print, e.g. {\"wall_loops\":\"3\",\"sparse_infill_density\":\"25%\"}. Returns print time, filament used and warnings.",
     R"({"type":"object","properties":{"printer_profile":{"type":"string"},"process_profile":{"type":"string"},"filament_profile":{"type":"string"},"bed_type":{"type":"string"},"settings":{"type":"object"},"what":{"type":"string","enum":["visible","selected"]},"arrange":{"type":"boolean"}}})"},
    {"slice_view", "Kestral's Slice tab, the same things the user does there: open it (tab=slice) or go back (tab=design), pick profiles for the tab (printer_profile, process_profile, filament_profile, bed_type), show the design or the sliced toolpaths (view=design or preview), choose the layer range shown (top_layer, bottom_layer, 1 based), show or hide features by name (features, e.g. {\"Sparse infill\":false}), change settings in the tab the way the user would (settings, OrcaSlicer keys, e.g. {\"wall_loops\":\"3\"}; an empty value puts one back to the profile's), Auto orient (auto_orient), and the plate like the user edits it in Prepare: plate is a list of {body (name), x, y (plate position of the center, mm), rotate (degrees), scale (percent), copies, on_plate, lay_flat (true: onto its biggest flat face), build_plate (1 based: which build plate it sits on)}; arrange_now packs everything onto the plate; build_plates sets how many build plates there are; active_plate (1 based) picks the plate that Slice plate slices. Returns what the tab shows: the plate (each part, copies, positions, size, on or off the bed), layers, heights, features and their share.",
     R"({"type":"object","properties":{"settings":{"type":"object"},"plate":{"type":"array","items":{"type":"object"}},"arrange_now":{"type":"boolean"},"build_plates":{"type":"number"},"active_plate":{"type":"number"},"auto_orient":{"type":"boolean"},"tab":{"type":"string","enum":["slice","design"]},"view":{"type":"string","enum":["design","preview"]},"printer_profile":{"type":"string"},"process_profile":{"type":"string"},"filament_profile":{"type":"string"},"bed_type":{"type":"string"},"top_layer":{"type":"number"},"bottom_layer":{"type":"number"},"features":{"type":"object"}}})"},
    {"find_slice_settings", "Find any OrcaSlicer setting of the Slice tab by words (e.g. \"brim\", \"wall loops\", \"seam\", \"start gcode\", \"retraction\", \"ironing\"): its key, page and group, value now, unit and choices. Then change it with slice_view settings (any key works, G-code settings take the whole text).",
     R"({"type":"object","properties":{"search":{"type":"string"}},"required":["search"]})"},
    {"slice_tab", "Everything else in the Slice tab (OrcaSlicer layout), the same as the user: page (home, prepare, preview, device, project, calibration); filaments: list of {number (1 based; leave out to add one), profile (name or words), color (#RRGGBB)}; remove_filaments: numbers; sync_from_ams: take the filaments from the printer's AMS; flushing: {matrix (N x N mm3), multiplier, auto}; parts: list of {name, filament (number, or \"auto\": by its color), settings (per object OrcaSlicer settings, empty value removes one), auto_orient, split (objects or parts), cut_height (mm above the plate) with cut_keep (both, upper, lower), paint {type color|support|seam, faces (face numbers), value (color: filament number; support / seam: 1 on, 2 off; 0 clears), clear}, select}; auto_orient_all; add_plate; plates: list of {number, name, plate_type, locked, auto_orient, arrange, to_front, delete}; view_type (line_type, filament, speed, layer_height, line_width, fan, temperature, flow); project {name, author, license, description}; ams_slots: the AMS slot for each filament of the last slice, in order (\"A1-1\".. \"A4-4\", \"Ext\"), used when printing; save_process_preset (a name: the current process with the tab's changes, saved as an OrcaSlicer user preset); calibration (flow_coarse, flow_fine, temperature, pressure_advance: puts the test on its own tab); slice (plate or all). Returns the filaments, plates, parts with their filament and own settings, the AMS slots and what the AMS holds.",
     R"({"type":"object","properties":{"page":{"type":"string"},"filaments":{"type":"array","items":{"type":"object"}},"remove_filaments":{"type":"array","items":{"type":"number"}},"sync_from_ams":{"type":"boolean"},"flushing":{"type":"object"},"parts":{"type":"array","items":{"type":"object"}},"auto_orient_all":{"type":"boolean"},"add_plate":{"type":"boolean"},"plates":{"type":"array","items":{"type":"object"}},"view_type":{"type":"string"},"project":{"type":"object"},"ams_slots":{"type":"array","items":{"type":"string"}},"save_process_preset":{"type":"string"},"calibration":{"type":"string"},"slice":{"type":"string","enum":["plate","all"]}}})"},
    {"printer_device", "The Device tab's controls of the real printer. Only when the user asks for it: nozzle_temp, bed_temp (C), fan_part, fan_aux, fan_chamber (percent), light (on / off), speed (silent, standard, sport, ludicrous), home, move {x, y, z mm, relative}, extrude_mm (needs a hot nozzle; negative pulls back), unload (AMS filament out of the nozzle), gcode (any G-code lines), camera (on / off), show (opens the Device tab). Returns the printer's last status.",
     R"({"type":"object","properties":{"nozzle_temp":{"type":"number"},"bed_temp":{"type":"number"},"fan_part":{"type":"number"},"fan_aux":{"type":"number"},"fan_chamber":{"type":"number"},"light":{"type":"boolean"},"speed":{"type":"string"},"home":{"type":"boolean"},"move":{"type":"object"},"extrude_mm":{"type":"number"},"unload":{"type":"boolean"},"gcode":{"type":"string"},"camera":{"type":"boolean"},"show":{"type":"boolean"}}})"},
    {"bambu_printers", "The Bambu Lab printers on the user's Bambu account (through Bambu's network plugin and the login OrcaSlicer has on this computer): signed in or not, and each printer's id (serial), name, model and online state. Use an id with set_printer connection bambu_orca.",
     R"({"type":"object","properties":{}})"},
    {"slicer_engine", "Kestral's slicer engine (OrcaSlicer's engine, run hidden). action status: version in use and whether an update was found. check: look on OrcaSlicer's GitHub releases for a newer one. install: download and install the newest (asks the user first). restart: restart Kestral to finish an installed update (the user is asked to save first).",
     R"({"type":"object","properties":{"action":{"type":"string","enum":["status","check","install","restart"]}},"required":["action"]})"},
    {"start_print", "Send the last sliced print to the printer. Kestral shows the user a Print / Cancel button with the details and sends only when they press Print, unless the user allowed the assistant to start prints without asking (Preferences > Printer). Reports whether the printer started.",
     R"({"type":"object","properties":{}})"},
    {"printer_control", "Pause, resume or stop (cancel) the job on the printer. Only when the user asks for it. stop asks the user first unless they allowed the assistant to act without asking.",
     R"({"type":"object","properties":{"action":{"type":"string","enum":["pause","resume","stop"]}},"required":["action"]})"},
    {"printer_status", "Ask the printer how it is doing: idle, printing (percent, time left), finished, temperatures.",
     R"({"type":"object","properties":{}})"},
    {"set_printer", "Set how Kestral reaches the printer. connection: bambu_orca (Bambu Lab in normal mode: Kestral sends through Bambu's cloud with Bambu's network plugin, like OrcaSlicer; serial is a printer id from bambu_printers), bambu_connect (Bambu Lab via the Bambu Connect app), bambu_lan (Bambu Lab in LAN only + Developer mode: fully automatic), octoprint, moonraker (Klipper, Mainsail, Fluidd) or prusalink. from_orcaslicer_printer (a serial, name or model from print_setup) fills in the IP, serial and access code OrcaSlicer already saved. Otherwise give host (IP) and for Bambu serial and access_code, for the others api_key.",
     R"({"type":"object","properties":{"connection":{"type":"string"},"name":{"type":"string"},"from_orcaslicer_printer":{"type":"string"},"host":{"type":"string"},"serial":{"type":"string"},"access_code":{"type":"string"},"api_key":{"type":"string"},"use_ams":{"type":"boolean"},"ams_slot":{"type":"number"}},"required":["connection"]})"},
};

// ---------------------------------------------------------------- provider formats

struct ProviderCfg {
    int kind; std::string url, model, key; int thinking = 0; int ctx = 0;
    int keepAliveMin = 30;        // Ollama
    float temperature = -1.f;     // < 0: model default
    int compact = 0, compactAt = 75;
};
// What one round trip cost (for the live status and the log).
struct CallStats { int promptTok = 0, outTok = 0; double promptSecs = 0, genSecs = 0, loadSecs = 0; };   // thinking: 0 default, 1 off, 2 low, 3 medium, 4 high   // kind: 0 Anthropic, 1 Ollama, 2 OpenAI compatible

// Built in providers. Anything that speaks the OpenAI chat format can also be added as a custom one.
struct ProviderPreset { const char* id; const char* label; int kind; const char* url; const char* model; bool needsKey; const char* help; };
const ProviderPreset kPresets[] = {
    {"claude", "Claude (Anthropic)", 0, "https://api.anthropic.com", "claude-sonnet-5-5", true, "Key from console.anthropic.com"},
    {"openai", "ChatGPT (OpenAI)", 2, "https://api.openai.com/v1", "gpt-5", true, "Key from platform.openai.com"},
    {"gemini", "Gemini (Google)", 2, "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-flash-latest", true, "Key from aistudio.google.com (has a free tier)"},
    {"zai", "Z.ai GLM (coding plan)", 2, "https://api.z.ai/api/coding/paas/v4", "glm-5.2", true, "Key from z.ai (API keys page). Uses your GLM Coding Plan quota"},
    {"grok", "Grok (xAI)", 2, "https://api.x.ai/v1", "grok-4", true, "Key from console.x.ai"},
    {"deepseek", "DeepSeek", 2, "https://api.deepseek.com/v1", "deepseek-chat", true, "Key from platform.deepseek.com"},
    {"mistral", "Mistral", 2, "https://api.mistral.ai/v1", "mistral-large-latest", true, "Key from console.mistral.ai"},
    {"openrouter", "OpenRouter (hundreds of models)", 2, "https://openrouter.ai/api/v1", "openrouter/auto", true, "One key from openrouter.ai reaches most models"},
    {"groq", "Groq (fast)", 2, "https://api.groq.com/openai/v1", "llama-3.3-70b-versatile", true, "Key from console.groq.com (has a free tier)"},
    {"ollama", "Ollama (local, free)", 1, "http://localhost:11434", "qwen3:14b", false, "Runs on your PC. Pick a model with tool support (qwen3, llama3.1+, mistral-nemo)"},
    {"lmstudio", "LM Studio / llama.cpp (local)", 2, "http://localhost:1234/v1", "local-model", false, "Any local server with an OpenAI style API (LM Studio, llama.cpp, vLLM, Jan)"},
};

const ProviderPreset* presetFor(const std::string& id) {
    for (auto& p : kPresets)
        if (id == p.id) return &p;
    return nullptr;
}
int kindOfFormat(int format) { return format == 1 ? 0 : format == 2 ? 1 : 2; }

std::string trimSlash(std::string u) {
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    return u;
}
// Endpoint for a provider base address.
std::string endpoint(int kind, const std::string& base, const char* what) {
    std::string u = trimSlash(base);
    if (kind == 0) {   // Anthropic: <base>/v1/messages
        if (u.size() < 3 || u.compare(u.size() - 3, 3, "/v1") != 0) u += "/v1";
        return u + "/" + what;
    }
    if (kind == 1) return u + (std::string(what) == "models" ? "/api/tags" : "/api/chat");
    // Add /v1 only when the address names no API version of its own (Z.ai's ends in /v4, Gemini's in /openai).
    bool versioned = false;
    {
        size_t sl = u.rfind('/');
        std::string last = sl == std::string::npos ? u : u.substr(sl + 1);
        versioned = last.size() >= 2 && last[0] == 'v' && std::isdigit((unsigned char)last[1]);
    }
    if (!versioned && u.find("/v1") == std::string::npos && u.find("/openai") == std::string::npos) u += "/v1";
    return u + "/" + (std::string(what) == "models" ? "models" : "chat/completions");
}

Value toolsFor(int kind, const std::set<std::string>* only = nullptr) {
    Value arr = Value::array();
    for (auto& t : kTools) {
        if (only && !only->count(t.name)) continue;
        Value schema = Value::parse(t.schema);
        if (kind == 0) {
            Value o = Value::object();
            o.set("name", t.name);
            o.set("description", t.desc);
            o.set("input_schema", schema);
            arr.push(o);
        } else {
            Value fn = Value::object();
            fn.set("name", t.name);
            fn.set("description", t.desc);
            fn.set("parameters", schema);
            Value o = Value::object();
            o.set("type", "function");
            o.set("function", fn);
            arr.push(o);
        }
    }
    return arr;
}

// One picture as a content block (Anthropic or OpenAI style).
Value imageBlock(int kind, const App::AiImage& im) {
    Value b = Value::object();
    if (kind == 0) {
        Value src = Value::object();
        src.set("type", "base64");
        src.set("media_type", im.mime);
        src.set("data", im.b64);
        b.set("type", "image");
        b.set("source", src);
    } else {
        Value u = Value::object();
        u.set("url", "data:" + im.mime + ";base64," + im.b64);
        b.set("type", "image_url");
        b.set("image_url", u);
    }
    return b;
}

Value messagesFor(int kind, const std::string& system, const std::vector<App::AiTurn>& h) {
    Value msgs = Value::array();
    if (kind != 0) {
        Value m = Value::object();
        m.set("role", "system");
        m.set("content", system);
        msgs.push(m);
    }
    // Pictures go with the last two messages that had any; older ones become a short note
    // (sending every picture again on each step would fill the window).
    size_t picsFrom = h.size();
    {
        int seen = 0;
        for (size_t i = h.size(); i-- > 0;)
            if (!h[i].images.empty() && ++seen <= 2) picsFrom = i;
    }
    for (size_t i = 0; i < h.size(); ++i) {
        const auto& t = h[i];
        if (t.role == "user") {
            Value m = Value::object();
            m.set("role", "user");
            if (t.images.empty()) {
                m.set("content", t.text);
            } else if (i < picsFrom) {
                m.set("content", t.text + "\n[" + std::to_string(t.images.size()) + " picture(s) were attached here earlier]");
            } else if (kind == 1) {
                // Ollama: plain base64 pictures next to the text.
                m.set("content", t.text);
                Value imgs = Value::array();
                for (auto& im : t.images) imgs.push(Value(im->b64));
                m.set("images", imgs);
            } else {
                Value content = Value::array();
                for (auto& im : t.images) content.push(imageBlock(kind, *im));
                Value tb = Value::object();
                tb.set("type", "text");
                tb.set("text", t.text);
                content.push(tb);
                m.set("content", content);
            }
            msgs.push(m);
        } else if (t.role == "assistant") {
            Value m = Value::object();
            m.set("role", "assistant");
            if (kind == 0 && !t.raw.empty()) {
                // Exactly what the model sent (with its thinking blocks): required for tool use.
                m.set("content", Value::parse(t.raw));
            } else if (kind == 0) {
                Value content = Value::array();
                if (!t.text.empty()) {
                    Value b = Value::object();
                    b.set("type", "text");
                    b.set("text", t.text);
                    content.push(b);
                }
                for (auto& c : t.calls) {
                    Value b = Value::object();
                    b.set("type", "tool_use");
                    b.set("id", c.id);
                    b.set("name", c.name);
                    Value args = Value::parse(c.argsJson);
                    b.set("input", args.isObject() ? args : Value::object());
                    content.push(b);
                }
                m.set("content", content);
            } else {
                m.set("content", t.text);
                if (!t.calls.empty()) {
                    Value calls = Value::array();
                    for (auto& c : t.calls) {
                        Value fn = Value::object();
                        fn.set("name", c.name);
                        if (kind == 1) {
                            Value args = Value::parse(c.argsJson);
                            fn.set("arguments", args.isObject() ? args : Value::object());
                        } else {
                            fn.set("arguments", c.argsJson);
                        }
                        Value tc = Value::object();
                        if (kind == 2) { tc.set("id", c.id); tc.set("type", "function"); }
                        tc.set("function", fn);
                        calls.push(tc);
                    }
                    m.set("tool_calls", calls);
                }
            }
            msgs.push(m);
        } else if (t.role == "tool") {
            if (kind == 0) {
                // Anthropic: consecutive tool results go in one user message.
                Value content = Value::array();
                size_t j = i;
                for (; j < h.size() && h[j].role == "tool"; ++j) {
                    Value b = Value::object();
                    b.set("type", "tool_result");
                    b.set("tool_use_id", h[j].callId);
                    if (!h[j].images.empty() && j >= picsFrom) {
                        // A picture Kestral took (look_at_design) goes inside the tool result.
                        Value parts = Value::array();
                        Value tb = Value::object();
                        tb.set("type", "text");
                        tb.set("text", h[j].text);
                        parts.push(tb);
                        for (auto& im : h[j].images) parts.push(imageBlock(0, *im));
                        b.set("content", parts);
                    } else {
                        b.set("content", h[j].text);
                    }
                    content.push(b);
                }
                i = j - 1;
                Value m = Value::object();
                m.set("role", "user");
                m.set("content", content);
                msgs.push(m);
            } else {
                Value m = Value::object();
                m.set("role", "tool");
                m.set("content", t.text);
                if (kind == 2) m.set("tool_call_id", t.callId);
                else m.set("tool_name", t.toolName);
                msgs.push(m);
                // Tool messages cannot carry pictures here: the picture follows as a message from Kestral,
                // after the last tool result of this step.
                bool lastTool = i + 1 >= h.size() || h[i + 1].role != "tool";
                if (lastTool) {
                    size_t first = i;
                    while (first > 0 && h[first - 1].role == "tool") --first;
                    for (size_t k = first; k <= i; ++k) {
                        if (h[k].images.empty() || k < picsFrom) continue;
                        Value pm = Value::object();
                        pm.set("role", "user");
                        std::string note = "[Kestral, not the user: the picture " + h[k].toolName + " took for you.]";
                        if (kind == 1) {
                            pm.set("content", note);
                            Value imgs = Value::array();
                            for (auto& im : h[k].images) imgs.push(Value(im->b64));
                            pm.set("images", imgs);
                        } else {
                            Value parts = Value::array();
                            Value tb = Value::object();
                            tb.set("type", "text");
                            tb.set("text", note);
                            parts.push(tb);
                            for (auto& im : h[k].images) parts.push(imageBlock(kind, *im));
                            pm.set("content", parts);
                        }
                        msgs.push(pm);
                    }
                }
            }
        }
    }
    return msgs;
}

// Typical context windows when a service does not report it (tokens).
int typicalContext(const std::string& model) {
    std::string m = model;
    for (auto& c : m) c = (char)std::tolower((unsigned char)c);
    struct K { const char* key; int ctx; };
    static const K table[] = {
        {"gemini", 1048576}, {"gpt-4.1", 1047576}, {"gpt-5", 400000}, {"gpt-4o", 128000}, {"o3", 200000}, {"o4", 200000},
        {"claude", 200000}, {"grok-4", 256000}, {"grok-3", 131072}, {"grok", 131072}, {"deepseek", 128000},
        {"codestral", 256000}, {"mistral", 128000}, {"llama-3", 131072}, {"llama3", 131072}, {"llama4", 131072},
        {"qwen3", 40960}, {"qwen2.5", 32768}, {"gpt-oss", 131072}, {"gemma3", 131072}, {"phi4", 16384},
        {"openrouter/auto", 128000},
    };
    for (auto& k : table)
        if (m.find(k.key) != std::string::npos) return k.ctx;
    return 32768;
}

// Drops the oldest exchanges until the conversation fits the context window (about 3.5
// characters a token), keeping room for the answer. Always starts at a user message.
std::vector<App::AiTurn> fitHistory(const std::vector<App::AiTurn>& h, int ctx, size_t systemChars) {
    if (ctx <= 0) return h;
    double budget = (ctx - std::min(ctx / 4, 8192)) * 3.5 - (double)systemChars - 6000.0;   // tools schema too
    auto sizeOf = [](const App::AiTurn& t) {
        size_t n = t.text.size() + t.raw.size() + 40 + t.images.size() * 5600;
        for (auto& c : t.calls) n += c.argsJson.size() + c.name.size() + 20;
        return n;
    };
    double total = 0;
    for (auto& t : h) total += (double)sizeOf(t);
    size_t start = 0;
    while (total > budget && start + 1 < h.size()) {
        total -= (double)sizeOf(h[start]);
        ++start;
        while (start < h.size() && h[start].role != "user") { total -= (double)sizeOf(h[start]); ++start; }
    }
    if (start >= h.size()) start = h.size() - 1;
    return std::vector<App::AiTurn>(h.begin() + (long)start, h.end());
}

// Live text while a reply streams in: (thinking so far, answer so far).
using DeltaFn = std::function<void(const std::string& thinking, const std::string& text)>;

// One model round trip. Fills reply (text + tool calls + thinking) or returns an error message.
// Ollama replies stream, so onDelta sees the thinking as it is written.
std::string callModelOnce(const ProviderCfg& cfg, const std::string& system, const std::vector<App::AiTurn>& h,
                          App::AiTurn& reply, const std::atomic<bool>* cancel, bool useThinking, bool& thinkingRejected,
                          const DeltaFn& onDelta, const std::set<std::string>* toolSet, CallStats* stats) {
    Value body = Value::object();
    const int th = useThinking ? cfg.thinking : 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string url;
    body.set("model", cfg.model);
    if (cfg.kind == 0) {
        url = endpoint(0, cfg.url, "messages");
        if (cfg.key.empty()) return "Add your API key: click Setup at the top of this panel.";
        headers = {{"x-api-key", cfg.key}, {"anthropic-version", "2023-06-01"}};
        body.set("system", system);
        if (th >= 2) {
            int budget = th == 2 ? 2048 : th == 3 ? 8000 : 24000;
            Value t = Value::object();
            t.set("type", "enabled");
            t.set("budget_tokens", budget);
            body.set("thinking", t);
            body.set("max_tokens", budget + 4096);
        } else {
            body.set("max_tokens", 4096);
        }
    } else if (cfg.kind == 1) {
        url = endpoint(1, cfg.url, "chat");
        body.set("stream", true);   // one JSON object per line, so thinking can be shown live
        // Ollama's own default window is tiny (2-4K) and would cut off the instructions.
        Value opts = Value::object();
        opts.set("num_ctx", cfg.ctx > 0 ? cfg.ctx : 16384);
        if (cfg.temperature >= 0) opts.set("temperature", (double)cfg.temperature);
        body.set("options", opts);
        // Stays loaded between messages (no reload wait), for as long as the user set.
        body.set("keep_alive", cfg.keepAliveMin < 0 ? Value(-1) : Value(std::to_string(cfg.keepAliveMin) + "m"));
        // Ollama: think on / off (gpt-oss models take a level instead).
        bool levels = cfg.model.find("gpt-oss") != std::string::npos;
        if (th >= 2) {
            if (levels) body.set("think", th == 2 ? "low" : th == 3 ? "medium" : "high");
            else body.set("think", true);
        } else if (th == 1 || th == 0) {
            body.set("think", levels ? Value("low") : Value(false));
        }
    } else {
        url = endpoint(2, cfg.url, "chat");
        if (!cfg.key.empty()) headers = {{"Authorization", "Bearer " + cfg.key}};
        if (th >= 1) body.set("reasoning_effort", th == 1 ? "none" : th == 2 ? "low" : th == 3 ? "medium" : "high");
        if (cfg.temperature >= 0) body.set("temperature", (double)cfg.temperature);
    }
    if (cfg.kind == 0 && cfg.temperature >= 0 && th < 2) body.set("temperature", (double)cfg.temperature);
    Value toolsV = toolsFor(cfg.kind, toolSet);
    body.set("messages", messagesFor(cfg.kind, system, fitHistory(h, cfg.ctx > 0 ? cfg.ctx : (cfg.kind == 1 ? 16384 : 0), system.size() + toolsV.dump().size())));
    if (!(toolSet && toolSet->empty())) body.set("tools", toolsV);
    // Ollama streams: gather each line's thinking, text and tool calls as they arrive.
    std::string lineBuf, sThink, sText, sErr;
    std::vector<App::AiCall> sCalls;
    bool sDone = false, sStarted = false;
    auto onLine = [&](const std::string& line) {
        if (line.find_first_not_of(" \r\t") == std::string::npos) return;
        Value v = Value::parse(line);
        if (!v.isObject()) return;
        if (v["error"].isString()) { sErr = v["error"].asString(); return; }
        const Value& m = v["message"];
        bool changed = false;
        if (m["thinking"].isString() && !m["thinking"].asString().empty()) { sThink += m["thinking"].asString(); changed = true; }
        if (m["content"].isString() && !m["content"].asString().empty()) { sText += m["content"].asString(); changed = true; }
        for (auto& tc : m["tool_calls"].items()) {
            const Value& fn = tc["function"];
            std::string args = fn["arguments"].isString() ? fn["arguments"].asString() : fn["arguments"].dump();
            sCalls.push_back({tc["id"].asString(), fn["name"].asString(), args.empty() ? "{}" : args});
        }
        if (v["done"].asBool(false)) {
            sDone = true;
            if (stats) {
                stats->promptTok = (int)v["prompt_eval_count"].asNumber(0);
                stats->outTok = (int)v["eval_count"].asNumber(0);
                stats->promptSecs = v["prompt_eval_duration"].asNumber(0) / 1e9;
                stats->genSecs = v["eval_duration"].asNumber(0) / 1e9;
                stats->loadSecs = v["load_duration"].asNumber(0) / 1e9;
            }
        }
        if (!sStarted) { sStarted = true; changed = true; }   // first line: the model has started
        if (changed && onDelta) onDelta(sThink, sText);
    };
    platform::HttpChunkFn chunkFn;
    if (cfg.kind == 1)
        chunkFn = [&](const char* d, size_t n) {
            lineBuf.append(d, n);
            size_t nl;
            while ((nl = lineBuf.find('\n')) != std::string::npos) {
                onLine(lineBuf.substr(0, nl));
                lineBuf.erase(0, nl + 1);
            }
        };
    // Local models on a busy GPU (or partly on the CPU) can take minutes for one answer.
    // The wait is for the first piece of the reply; after that the stream keeps it alive. A model
    // larger than the graphics card's memory runs partly on the CPU and can need a long time to start.
    auto r = platform::httpPost(url, headers, body.dump(), cfg.kind == 1 ? 3600000 : 300000, cancel, chunkFn);
    if (cfg.kind == 1 && r.error.empty() && r.status >= 200 && r.status < 300) {
        onLine(lineBuf);
        if (!sErr.empty()) return "Ollama said: " + sErr;
        if (!sDone && sText.empty() && sCalls.empty()) return "The reply from Ollama stopped early. Try again.";
        reply = App::AiTurn{};
        reply.role = "assistant";
        reply.thinking = sThink;
        reply.text = stripThinking(sText, &reply.thinking);
        int k = 0;
        for (auto& c : sCalls) {
            if (c.id.empty()) c.id = "call_" + std::to_string(++k) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
            reply.calls.push_back(c);
        }
        return {};
    }
    if (!r.error.empty()) {
        if (cfg.kind == 1 && r.status == 0 && r.error.find("timed out") != std::string::npos)
            return "Ollama took over an hour to start answering, so Kestral gave up. The model is probably too big for the "
                   "graphics card and runs mostly on the CPU. Try a smaller one (for example a q4 version, or 14b), or a "
                   "smaller context in Setup.";
        if (cfg.kind == 1 && r.status == 0 && r.error != "cancelled")
            return "Could not reach Ollama at " + cfg.url + ". Is Ollama running? (ollama serve)";
        return r.error;
    }
    std::string perr;
    Value resp = Value::parse(r.body, &perr);
    if (r.status < 200 || r.status >= 300) {
        std::string msg = resp["error"].isObject() ? resp["error"]["message"].asString() : resp["error"].asString();
        if (msg.empty()) msg = r.body.substr(0, 300);
        std::string lm = msg;
        for (auto& ch : lm) ch = (char)std::tolower((unsigned char)ch);
        if (th != 0 && r.status == 400 && (lm.find("think") != std::string::npos || lm.find("reason") != std::string::npos ||
                                           lm.find("budget") != std::string::npos || lm.find("unrecognized") != std::string::npos ||
                                           lm.find("unknown") != std::string::npos || lm.find("extra") != std::string::npos)) {
            thinkingRejected = true;
            return msg;
        }
        if (r.status == 404 || msg.find("model") != std::string::npos)
            msg += "\n(Check the model name in Setup. \"Get list\" shows the models this service has.)";
        if (r.status == 401 || r.status == 403) msg += "\n(Check the API key in Setup.)";
        return "The model service said (" + std::to_string(r.status) + "): " + msg;
    }
    if (resp.isNull()) return "Could not read the reply: " + perr;
    if (stats) {
        const Value& u = resp["usage"];
        stats->promptTok = (int)(u["prompt_tokens"].isNumber() ? u["prompt_tokens"].asNumber() : u["input_tokens"].asNumber(0));
        stats->outTok = (int)(u["completion_tokens"].isNumber() ? u["completion_tokens"].asNumber() : u["output_tokens"].asNumber(0));
    }
    reply = App::AiTurn{};
    reply.role = "assistant";
    int n = 0;
    auto newId = [&] { return "call_" + std::to_string(++n) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000); };
    if (cfg.kind == 0) {
        bool thought = false;
        for (auto& b : resp["content"].items()) {
            std::string ty = b["type"].asString();
            if (ty == "text") reply.text += b["text"].asString();
            else if (ty == "tool_use") reply.calls.push_back({b["id"].asString(), b["name"].asString(), b["input"].dump()});
            else if (ty == "thinking" || ty == "redacted_thinking") {
                thought = true;
                if (b["thinking"].isString()) reply.thinking += b["thinking"].asString();
            }
        }
        if (thought) reply.raw = resp["content"].dump();
    } else {
        const Value& m = cfg.kind == 1 ? resp["message"] : resp["choices"][(size_t)0]["message"];
        // OpenAI style services put the reasoning in reasoning_content (DeepSeek, LM Studio) or reasoning.
        if (m["reasoning_content"].isString()) reply.thinking = m["reasoning_content"].asString();
        else if (m["reasoning"].isString()) reply.thinking = m["reasoning"].asString();
        reply.text = stripThinking(m["content"].asString(), &reply.thinking);
        for (auto& tc : m["tool_calls"].items()) {
            const Value& fn = tc["function"];
            std::string args = fn["arguments"].isString() ? fn["arguments"].asString() : fn["arguments"].dump();
            std::string id = tc["id"].asString();
            reply.calls.push_back({id.empty() ? newId() : id, fn["name"].asString(), args.empty() ? "{}" : args});
        }
    }
    return {};
}

std::string callModel(const ProviderCfg& cfg, const std::string& system, const std::vector<App::AiTurn>& h,
                      App::AiTurn& reply, const std::atomic<bool>* cancel, const DeltaFn& onDelta,
                      const std::set<std::string>* toolSet = nullptr, CallStats* stats = nullptr) {
    bool rejected = false;
    std::string err = callModelOnce(cfg, system, h, reply, cancel, true, rejected, onDelta, toolSet, stats);
    if (rejected) {
        // This model or service does not take a thinking setting: ask again without it.
        rejected = false;
        err = callModelOnce(cfg, system, h, reply, cancel, false, rejected, onDelta, toolSet, stats);
    }
    return err;
}

// When the conversation fills most of the model's window, the older part becomes a summary written
// by the same model, so a long job keeps its memory of what was built and decided. Recent turns stay
// as they are. True when it compacted (note says how much).
bool compactHistory(const ProviderCfg& cfg, std::vector<App::AiTurn>& h, size_t systemChars, const std::atomic<bool>* cancel,
                    std::string& note) {
    if (cfg.compact == 2) return false;   // off: the oldest messages simply fall out of the window
    int ctx = cfg.ctx > 0 ? cfg.ctx : cfg.kind == 1 ? 16384 : cfg.kind == 0 ? 180000 : 32768;
    double budget = (ctx - std::min(ctx / 4, 8192)) * 3.5 - (double)systemChars - 6000.0;
    if (budget < 8000) return false;
    auto sizeOf = [](const App::AiTurn& t) {
        size_t n = t.text.size() + t.raw.size() + 40;
        for (auto& c : t.calls) n += c.argsJson.size() + c.name.size() + 20;
        return (double)n;
    };
    double total = 0;
    for (auto& t : h) total += sizeOf(t);
    if (total < budget * std::clamp(cfg.compactAt, 30, 95) / 100.0 || h.size() < 6) return false;
    // Keep the newest ~35% of the room as it is. The cut goes before a user message (then a short
    // reply follows the summary) or before an assistant turn (never between a call and its result).
    double keep = 0;
    size_t cut = h.size();
    for (size_t i = h.size(); i-- > 1;) {
        keep += sizeOf(h[i]);
        if (keep > budget * 0.35) break;
        if (h[i].role == "user" || h[i].role == "assistant") cut = i;
    }
    if (cut >= h.size() || cut < 2) return false;
    if (cfg.compact == 1) {   // drop the oldest part, no summary
        h.erase(h.begin(), h.begin() + (long)cut);
        if (!h.empty() && h[0].role != "user") {
            App::AiTurn u;
            u.role = "user";
            u.text = "[Older messages were left out to save room.]";
            h.insert(h.begin(), u);
        }
        note = "Left out " + std::to_string(cut) + " older messages to save room";
        return true;
    }
    // The old part as plain text for the summary.
    std::string tr;
    for (size_t i = 0; i < cut; ++i) {
        const auto& t = h[i];
        if (t.role == "user") tr += "USER: " + t.text + "\n";
        else if (t.role == "assistant") {
            if (!t.text.empty()) tr += "ASSISTANT: " + t.text + "\n";
            for (auto& c : t.calls) tr += "ASSISTANT CALLED " + c.name + " " + c.argsJson.substr(0, 400) + "\n";
        } else {
            tr += "RESULT of " + t.toolName + ": " + t.text.substr(0, 700) + (t.text.size() > 700 ? " ..." : "") + "\n";
        }
    }
    size_t maxTr = (size_t)std::max(6000.0, budget * 0.6);
    if (tr.size() > maxTr) tr = tr.substr(0, maxTr / 3) + "\n[...]\n" + tr.substr(tr.size() - maxTr * 2 / 3);
    ProviderCfg c2 = cfg;
    c2.thinking = 1;   // a summary needs no long thinking
    std::vector<App::AiTurn> req(1);
    req[0].role = "user";
    req[0].text = "Summarize this earlier part of our work in Kestral (a CAD program) so you can continue without it. Keep every "
                  "fact needed to go on: what the user asked for and their wishes and preferences, what was built (body, sketch "
                  "and feature names, sizes, ids), decisions made, the printer, slicing and print state, problems met and how they "
                  "were solved, and what is still left to do. Short bullet points, no tool calls, no greeting.\n\n" + tr;
    App::AiTurn reply;
    std::string err = callModel(c2, "You write compact, exact summaries of work sessions.", req, reply, cancel, [](const std::string&, const std::string&) {});
    if (!err.empty() || reply.text.size() < 40) return false;
    std::vector<App::AiTurn> out;
    App::AiTurn sum;
    sum.role = "user";
    sum.text = "[The earlier part of this conversation was compacted to save room. Summary of it:]\n" + reply.text;
    out.push_back(sum);
    if (h[cut].role == "user") {
        App::AiTurn ack;
        ack.role = "assistant";
        ack.text = "Understood, I have the summary and will continue from there.";
        out.push_back(ack);
    }
    out.insert(out.end(), h.begin() + (long)cut, h.end());
    note = "Compacted " + std::to_string(cut) + " older messages into a summary, so the assistant can keep going";
    h = std::move(out);
    return true;
}

std::vector<std::string> stringList(const Value& v) {
    std::vector<std::string> out;
    if (v.isString()) out.push_back(v.asString());
    for (auto& e : v.items())
        if (e.isString()) out.push_back(e.asString());
    return out;
}

} // namespace

// ===================================================================== knowledge

std::string App::aiSystemPrompt() const {
    std::ostringstream o;
    o << "You are the assistant built into Kestral, a desktop parametric CAD program for Windows and Linux that works like "
         "Autodesk Fusion 360 (same layout, workflow and shortcuts). Units are millimeters and degrees. Z is up. "
         "Answer questions about using Kestral briefly and concretely, naming the exact command and shortcut. "
         "When the user asks you to make, build or change something (\"make a bat\"), do it yourself with the tools: "
         "call get_design first, then build it. Do not answer with steps for the user unless they ask how to do it. Every change "
         "is undoable with Ctrl+Z. Never invent body names: use the paths get_design returns. Keep replies short.\n"
         "Prefer the dedicated tools (create_box, create_cylinder, create_hole, fillet, chamfer, move, set_color, "
         "create_sketch, sketch_add, extrude, revolve, rotate, combine, and the rest listed with your tools). "
         "run_command only works for commands that need no clicks in the viewport; commands that wait for clicks are "
         "closed again and their messages come back to you in the tool result. Read those results: if something failed, "
         "say so and try another way instead of repeating the same call. "
         "Sizes may be numbers or parameter expressions such as \"Side*2\". Give bodies a name and a component (browser folder). "
         "Cuts (holes, extrude cut, combine cut) take target_bodies so they do not cut everything in the way. "
         "get_design reports volume, area, face and edge counts and a stable id. get_body lists faces. "
         "undo tells you which step it reverted.\n\n";
    o << "HOW KESTRAL WORKS\n"
         "- Layout: toolbar with SOLID and UTILITIES tabs, browser (folders = components, bodies, sketches) on the left, "
         "timeline at the bottom (double click a step to edit it, drag the marker to roll back), view cube top right with "
         "step arrows above it, navigation bar at the bottom.\n"
         "- Mouse (default Fusion style, changeable in Preferences): middle drag pans, Shift + middle drag orbits around "
         "the point under the cursor, Shift + middle click sets the view center, wheel zooms at the cursor, double "
         "middle click fits. Right click opens the marking menu.\n"
         "- Selecting: click picks a face, edge or sketch profile; double click a face picks the whole body, double "
         "click an edge picks its tangent chain. Ctrl/Shift click adds. Drag left to right selects inside a window, "
         "right to left selects anything crossing it.\n"
         "- Sketching: press L, R, C, P (or another sketch tool) and pick a plane or flat face. Type numbers while "
         "drawing (Tab to the next field) to set dimensions. D dimension, X construction, T trim, O offset, P project. "
         "E or H inside a sketch finishes it and starts Extrude or Hole.\n"
         "- Holes: H (outside a sketch) starts Hole: click faces to place holes, snapping to the grid, corners, edge middles "
         "and circle centers; simple, counterbore or countersink; H again or Enter finishes.\n"
         "- Commands: Enter is OK, Esc cancels, typing a number goes into the value box, drag arrows on the model change "
         "values live. Delete on a body adds a Remove step; on faces it deletes them and heals the body.\n"
         "- Files: designs save as .kst (in the designs folder inside the Kestral folder) and autosave every 30 seconds; STEP, "
         "STL and OBJ import with Ctrl+I keeping Fusion's folders and colors; Ctrl+P sends to the slicer.\n"
         "- Keyboard shortcuts can be changed: right click a toolbar command or Preferences > Keyboard Shortcuts.\n\n";
    o << "SLICING AND PRINTING\n"
         "- Kestral slices with the user's own OrcaSlicer profiles and sends prints to their printer. Always call "
         "print_setup first. Use the profiles OrcaSlicer has selected unless the user names others.\n"
         "- When asked to print: slice (with any setting changes asked for), tell the user the print time and filament, "
         "then call start_print. Use the profiles OrcaSlicer has selected: never swap in other profiles unless the user names "
         "them. If slicing fails, tell the user the exact error instead of trying other profiles. start_print shows the user a Print button "
         "(or starts at once when the user allowed that in Preferences): never claim a print started unless the tool says so. If the user only "
         "asked to slice or to check, do not call start_print.\n"
         "- \"Model it and print it\" (often while the user is away): build the model, check it (get_design), slice, then call start_print "
         "yourself in the same turn. Do not stop to ask in between.\n"
         "- Turn plain words into settings: \"stronger\" -> more walls (wall_loops 3-4) and more infill (20-40%), "
         "\"faster\" -> fewer walls, less infill, thicker layers (0.24-0.28), \"smoother / finer\" -> thinner layers "
         "(0.12-0.16), \"needs supports\" -> enable_support 1. Settings: " << print::commonSettingsHelp() << "\n"
         "- If something is missing, walk the user through it, one step at a time: (1) no OrcaSlicer: install it free from "
         "github.com/SoftFever/OrcaSlicer/releases and run its setup wizard to pick the printer; (2) no profile for their "
         "printer or filament: find built in ones with list_print_profiles (search the printer model, e.g. \"A1 mini\", "
         "and the filament, e.g. \"PLA basic A1M\") and pass them to slice; (3) no printer connection: Bambu Lab printers "
         "normally use bambu_orca (works as is, fully hands off: Kestral presses OrcaSlicer's buttons; set from_orcaslicer_printer "
         "when print_setup lists the printer so its status can be read too) or bambu_connect; bambu_lan is fully automatic but needs LAN only + Developer mode on the "
         "printer, which turns off Bambu Handy and the cloud, so only suggest it if they want that; other printers: "
         "OctoPrint, Klipper (moonraker) or PrusaLink with the printer's IP and API key.\n"
         "- Never ask the user to type an access code or API key into this chat: they can enter it in Preferences > "
         "3D printer, or it can come from OrcaSlicer (from_orcaslicer_printer).\n\n";
    o << "APPEARANCE LIBRARY (for set_appearance)\n";
    {
        std::string cat;
        for (auto& lm : materialLibrary()) {
            if (cat != lm.category) { o << (cat.empty() ? "" : "\n") << lm.category << ": "; cat = lm.category; }
            else o << "; ";
            o << lm.name;
        }
        o << "\n\n";
    }
    o << "COMMANDS (id: name [shortcut], where)\n";
    std::set<std::string> seen;
    for (size_t i = 0; i < tools_.size(); ++i) {
        const Tool& t = tools_[i];
        if (!t.available() || !seen.insert(t.id).second) continue;
        o << "- " << t.id << ": " << t.label;
        if (!t.shortcut.empty()) o << " [" << t.shortcut << "]";
        o << ", " << (t.global ? "anywhere" : (i < toolIsSketch_.size() && toolIsSketch_[i]) ? "in a sketch" : "model");
        if (!t.tip.empty()) o << ". " << t.tip;
        o << "\n";
    }
    return o.str();
}

std::string App::aiBodyPath(uint32_t bodyId) const {
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return {};
    std::string path = bit->second.name;
    uint32_t c = bit->second.componentId;
    for (int guard = 0; c && c != doc_.rootId && guard < 64; ++guard) {
        auto cit = doc_.components.find(c);
        if (cit == doc_.components.end()) break;
        path = cit->second.name + "/" + path;
        c = cit->second.parentId;
    }
    return path;
}

std::vector<uint32_t> App::aiFindBodies(const std::vector<std::string>& names, std::string& missing) const {
    std::vector<uint32_t> out;
    for (auto& raw : names) {
        std::string n = lower(raw);
        size_t before = out.size();
        // Exact path, then body name, then folder name (all bodies in it).
        for (auto& [id, b] : doc_.bodies)
            if (doc_.isBodyAlive(b) && lower(aiBodyPath(id)) == n) out.push_back(id);
        if (out.size() == before)
            for (auto& [id, b] : doc_.bodies)
                if (doc_.isBodyAlive(b) && lower(b.name) == n) out.push_back(id);
        if (out.size() == before)
            for (auto& [cid, c] : doc_.components)
                if (lower(c.name) == n) {
                    std::vector<uint32_t> v;
                    doc_.collectBodies(cid, v);
                    out.insert(out.end(), v.begin(), v.end());
                }
        if (out.size() == before) missing += (missing.empty() ? "" : ", ") + raw;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ===================================================================== tools (main thread)

std::string App::aiRunTool(const std::string& name, const std::string& argsJson) {
    Value a = Value::parse(argsJson);
    auto bodiesArg = [&](std::string& err) {
        std::string missing;
        auto ids = aiFindBodies(stringList(a["names"]), missing);
        if (!missing.empty()) err = "Not found: " + missing + ". Use paths from get_design.";
        return ids;
    };
    if (dlg_.open || tdlg_.open || sk_.choosingPlane) {
        // A command the assistant opened and nobody picked anything for: close it quietly.
        // One the user had open: finish it first.
        if (aiOpenedCommand_) {
            commandCancel();
            if (sk_.choosingPlane) sketchCancel();
        } else {
            commandOk();
        }
    }
    aiOpenedCommand_ = false;
    if (name == "robot") return robotTool(argsJson);
    if (name == "get_design") {
        Value d = Value::object();
        d.set("name", doc_.name);
        d.set("units", "mm");
        Value bodies = Value::array();
        for (auto& [id, b] : doc_.bodies) {
            if (!doc_.isBodyAlive(b)) continue;
            Value bo = Value::object();
            bo.set("id", (int)id);
            bo.set("path", aiBodyPath(id));
            bo.set("visible", bodyShown(b));
            bo.set("color", hexColor(b.color));
            if (!b.mat.name.empty()) bo.set("material", b.mat.name);
            auto pit = doc_.parts.find(b.partId);
            if (pit != doc_.parts.end() && pit->second.shape) {
                try {
                    ShapePtr sh = pit->second.shape;
                    glm::mat3 L(b.xform);
                    double det = std::abs(glm::determinant(L));
                    if (std::abs(det - 1.0) > 1e-3) sh = kernel::placeShape(sh, b.xform);
                    double vol = std::abs(kernel::solidVolume(sh));
                    double area = kernel::solidArea(sh);
                    bo.set("volume_mm3", vol);
                    bo.set("area_mm2", area);
                    bo.set("faces", (int)kernel::faceCount(pit->second.shape));
                    bo.set("edges", (int)kernel::edgeCount(pit->second.shape));
                    auto dens = [](std::string n) {
                        for (char& c : n) c = (char)std::tolower((unsigned char)c);
                        if (n.find("alum") != std::string::npos) return 2.70;
                        if (n.find("steel") != std::string::npos) return 7.85;
                        if (n.find("brass") != std::string::npos) return 8.50;
                        if (n.find("copper") != std::string::npos) return 8.96;
                        if (n.find("titan") != std::string::npos) return 4.51;
                        if (n.find("wood") != std::string::npos) return 0.60;
                        if (n.find("glass") != std::string::npos) return 2.50;
                        if (n.find("plastic") != std::string::npos || n.find("pla") != std::string::npos || n.find("abs") != std::string::npos) return 1.20;
                        if (n.find("carbon") != std::string::npos) return 1.60;
                        return 0.0;
                    };
                    double rho = dens(b.mat.name);
                    if (rho > 0) bo.set("mass_g", vol * rho / 1000.0);
                } catch (const std::exception&) {}
            }
            if (pit != doc_.parts.end() && pit->second.mesh) {
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                if (bb.valid()) {
                    glm::vec3 sz = bb.max - bb.min, c = bb.center();
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "%.2f x %.2f x %.2f", sz.x, sz.y, sz.z);
                    bo.set("size_mm", buf);
                    std::snprintf(buf, sizeof buf, "%.2f, %.2f, %.2f", c.x, c.y, c.z);
                    bo.set("center", buf);
                }
            }
            bo.set("solid", pit != doc_.parts.end() && (bool)pit->second.shape);
            bodies.push(bo);
        }
        d.set("bodies", bodies);
        Value tl = Value::array();
        for (int i = 0; i < (int)doc_.timeline.size(); ++i) {
            auto& f = doc_.timeline[i];
            std::string s = f.name + (f.suppressed ? " (suppressed)" : "") + (f.failed ? " (FAILED: " + f.error + ")" : "") +
                            (i >= doc_.markerPos ? " (rolled back)" : "");
            tl.push(s);
        }
        d.set("timeline", tl);
        Value ps = Value::object();
        for (auto& [n, p] : doc_.params) ps.set(n, p.expr + " = " + std::to_string(p.value));
        d.set("parameters", ps);
        Value sel = Value::array();
        for (uint32_t id : selection_) sel.push(aiBodyPath(id));
        for (auto& f : selFaces_) sel.push(aiBodyPath(f.first) + " (face " + std::to_string(f.second) + ")");
        for (auto& e : selEdges_) sel.push(aiBodyPath(e.first) + " (edge " + std::to_string(e.second) + ")");
        d.set("selection", sel);
        std::string out = d.dump();
        if (out.size() > 60000) out = out.substr(0, 60000) + "...(truncated)";
        return out;
    }
    if (name == "run_command") {
        std::string id = a["command_id"].asString();
        int ti = findTool(id);
        if (ti < 0) return "No command with id '" + id + "'.";
        if (!tools_[ti].available()) return "'" + tools_[ti].label + "' is not built yet.";
        if (id == "undo") {
            if (!undo_.canUndo()) return "Nothing to undo.";
            std::string label = undo_.undoLabel();
            undo();
            return "Undid \"" + label + "\".";
        }
        if (id == "redo") {
            if (!undo_.canRedo()) return "Nothing to redo.";
            std::string label = undo_.redoLabel();
            redo();
            return "Redid \"" + label + "\".";
        }
        runTool(id);
        if (dlg_.open || tdlg_.open || sk_.choosingPlane) {
            // A command that waits for clicks in the viewport. When the assistant has its own tool for
            // it, the window closes again at once and the model is pointed to that tool.
            static const std::pair<const char*, const char*> kAlt[] = {
                {"box", "create_box"}, {"cylinder", "create_cylinder"}, {"sphere", "create_sphere"}, {"cone", "create_cone"},
                {"torus", "create_torus"}, {"wedge", "create_wedge"}, {"tube", "create_tube"}, {"hole", "create_hole"},
                {"circ_pattern", "circular_pattern"}, {"path_pattern", "pattern_on_path"}, {"solid_mirror", "mirror"},
                {"coil", "create_coil"}, {"sketch", "create_sketch"}, {"new_component", "create_component"},
                {"angle_plane", "plane_at_angle"}, {"mid_plane", "midplane"}, {"section", "section_analysis"},
                {"appearance", "set_appearance"}, {"parameters", "set_parameter"}};
            std::string alt;
            for (auto& [cmdId, tool] : kAlt) if (id == cmdId) alt = tool;
            if (alt.empty())
                for (auto& t : kTools) if (id == t.name) alt = t.name;
            if (!alt.empty()) {
                commandCancel();
                if (sk_.choosingPlane) sketchCancel();
                return "Kestral did not open " + tools_[ti].label + ": that window waits for clicks in the viewport, which you cannot do. "
                       "Use the `" + alt + "` tool instead (it is loaded now) and give it the bodies, edges or faces by name.";
            }
            aiOpenedCommand_ = true;
            return "Opened " + tools_[ti].label + ". It now waits for the user to click things in the viewport, which you cannot "
                   "do; it will be closed when you use your next tool. To act yourself, use the dedicated tools instead "
                   "(create_box, create_cylinder, create_hole, fillet, chamfer, move, set_color...), or select bodies first.";
        }
        return "Ran " + tools_[ti].label + ".";
    }
    if (name == "select") {
        std::string err;
        auto ids = bodiesArg(err);
        selectBodies(ids, a["add"].asBool(false));
        return err.empty() ? "Selected " + std::to_string(ids.size()) + " bodies." : err;
    }
    if (name == "save_design") {
        if (doc_.path.empty()) return "This design has no file yet: ask the user to Save As once (it needs a name and folder).";
        saveDocumentNow();
        return "Saved " + doc_.name + " as v" + std::to_string(doc_.version) + ".";
    }
    if (name == "list_designs" || name == "open_design") {
        namespace fs = std::filesystem;
        std::string q = toLower(a[name == "open_design" ? "name" : "query"].asString());
        std::vector<std::string> found;
        std::error_code ec;
        if (name == "open_design" && fs::exists(u8path(a["name"].asString()), ec)) found.push_back(a["name"].asString());
        int n = 0;
        for (auto it = fs::recursive_directory_iterator(u8path(platform::designsDir()), fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && n < 20000; it.increment(ec), ++n) {
            if (ec) break;
            std::string fn = pathToU8(it->path().filename());
            if (!fn.empty() && fn[0] == '.' && it->is_directory(ec)) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec) || toLower(pathToU8(it->path().extension())) != ".kst") continue;
            std::string stem = toLower(pathToU8(it->path().stem()));
            if (q.empty() || stem.find(q) != std::string::npos) found.push_back(pathToU8(it->path()));
        }
        for (auto& r : settings_.recentFiles)
            if ((q.empty() || toLower(pathToU8(u8path(r).stem())).find(q) != std::string::npos) &&
                std::find(found.begin(), found.end(), r) == found.end() && fs::exists(u8path(r), ec))
                found.push_back(r);
        if (name == "list_designs") {
            std::string out;
            for (size_t i = 0; i < found.size() && i < 60; ++i) {
                int v = latestVersion(found[i]);
                out += pathToU8(u8path(found[i]).stem()) + "  v" + std::to_string(v) + "  " + found[i] + "\n";
            }
            return out.empty() ? "No designs match." : out;
        }
        if (found.empty()) return "No design named like \"" + a["name"].asString() + "\". Use list_designs.";
        // The closest name: an exact match first, then the shortest one containing the words.
        std::string best = found[0];
        for (auto& f : found)
            if (toLower(pathToU8(u8path(f).stem())) == q) { best = f; break; }
            else if (pathToU8(u8path(f).stem()).size() < pathToU8(u8path(best).stem()).size()) best = f;
        int ver = a["version"].isNumber() ? (int)a["version"].asNumber() : 0;
        if (ver > 0) openDesignVersion(best, ver);
        else openDocumentPath(best);
        return "Opened " + best + (ver > 0 ? " (v" + std::to_string(ver) + ")" : "") + ". The geometry rebuilds in the background.";
    }
    if (name == "set_visibility") {
        bool vis = a["visible"].asBool(true);
        // Sketches and construction planes by name (or "all sketches" / "all planes") first.
        {
            auto names = stringList(a["names"]);
            std::vector<std::string> rest;
            int nsk = 0, npl = 0;
            bool pushed = false;
            auto low = [](std::string x) { for (auto& ch : x) ch = (char)std::tolower((unsigned char)ch); return x; };
            for (auto& n : names) {
                std::string l = low(n);
                bool hit = false;
                for (auto& [sid, sk] : doc_.sketches)
                    if (l == "all sketches" || low(sk.name) == l) {
                        if (!pushed) { pushUndo(vis ? "Show" : "Hide"); pushed = true; }
                        sk.visible = vis; ++nsk; hit = true;
                    }
                for (auto& [pid, pl] : doc_.constructPlanes)
                    if (l == "all planes" || low(pl.name) == l) {
                        if (!pushed) { pushUndo(vis ? "Show" : "Hide"); pushed = true; }
                        if (vis) doc_.hiddenPlanes.erase(pid); else doc_.hiddenPlanes.insert(pid);
                        ++npl; hit = true;
                    }
                if (!hit) rest.push_back(n);
            }
            if (nsk || npl) {
                doc_.touch();
                markSceneDirty();
                std::string out = std::to_string(nsk) + " sketches, " + std::to_string(npl) + " planes " + (vis ? "shown." : "hidden.");
                if (rest.empty()) return out;
                std::string err;
                auto ids = aiFindBodies(rest, err);
                for (uint32_t id : ids) { doc_.bodies[id].visible = vis; syncBodyOverride(id); }
                return out + " " + std::to_string(ids.size()) + " bodies too." + (err.empty() ? "" : " Not found: " + err);
            }
        }
        std::string err;
        auto ids = bodiesArg(err);
        pushUndo(vis ? "Show" : "Hide");
        for (uint32_t id : ids) {
            doc_.bodies[id].visible = vis;
            syncBodyOverride(id);
        }
        doc_.touch();
        markSceneDirty();
        return (err.empty() ? "" : err + " ") + std::to_string(ids.size()) + (vis ? " shown." : " hidden.");
    }
    if (name == "set_color") {
        std::string err;
        auto ids = bodiesArg(err);
        glm::vec3 c;
        if (!parseHex(a["color"].asString(), c)) return "color must be #RRGGBB";
        pushUndo("Appearance");
        for (uint32_t id : ids) {
            doc_.bodies[id].color = c;
            doc_.bodies[id].mat.fileColors = false;
            syncBodyOverride(id);
        }
        doc_.touch();
        markSceneDirty();
        return (err.empty() ? "" : err + " ") + "Colored " + std::to_string(ids.size()) + " bodies.";
    }
    if (name == "set_appearance") {
        std::string err;
        auto ids = bodiesArg(err);
        if (ids.empty()) return err.empty() ? "Nothing to change." : err;
        auto it = doc_.bodies.find(ids[0]);
        glm::vec3 c = it->second.color;
        MatProps m = it->second.mat;
        std::string mname = a["material"].asString();
        if (!mname.empty()) {
            const LibMaterial* lm = findLibMaterial(mname);
            if (!lm) return "No appearance called '" + mname + "'. Use a name from the library list.";
            c = lm->color;
            m = lm->mat;
        }
        if (a.has("color")) {
            glm::vec3 cc;
            if (!parseHex(a["color"].asString(), cc)) return "color must be #RRGGBB";
            c = cc;
            if (mname.empty()) m.name.clear();
        }
        if (a.has("opacity")) m.opacity = (float)std::clamp(a["opacity"].asNumber(1), 0.05, 1.0);
        if (a.has("metal")) m.metal = (float)std::clamp(a["metal"].asNumber(0), 0.0, 1.0);
        if (a.has("roughness")) m.rough = (float)std::clamp(a["roughness"].asNumber(0.45), 0.0, 1.0);
        if (a.has("glow")) m.glow = (float)std::clamp(a["glow"].asNumber(0), 0.0, 1.5);
        m.fileColors = false;
        pushUndo("Appearance");
        applyLook(c, m, false, ids, {});
        return (err.empty() ? "" : err + " ") + "Changed the look of " + std::to_string(ids.size()) + " bodies.";
    }
    if (name == "section_analysis") {
        runSection();
        std::string plane = lower(a["plane"].asString("xy"));
        sectionAxis_ = (plane == "yz" || plane == "x") ? 0 : (plane == "xz" || plane == "y") ? 1 : 2;
        if (a.has("offset")) std::snprintf(sectionOff_, sizeof sectionOff_, "%g", a["offset"].asNumber(0));
        sectionFlip_ = a["flip"].asBool(false);
        bool on = a.has("on") ? a["on"].asBool(true) : true;
        std::string err = applySection(on);
        sectionOpen_ = false;
        if (!err.empty()) return err;
        return on ? "Section is on." : "Section is off.";
    }
    if (name == "move") {
        std::string err;
        auto ids = bodiesArg(err);
        if (ids.empty()) return err.empty() ? "Nothing to move." : err;
        clearSelection();
        selectBodies(ids, false);
        startToolFeature(FeatureType::Move);
        if (!tdlg_.open) return "Could not start Move.";
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", a["x"].asNumber(0));
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", a["y"].asNumber(0));
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%g", a["z"].asNumber(0));
        tdlg_.option2 = a["copy"].asBool(false) ? 1 : 0;
        applyToolDialog();
        closeToolDialog(true);
        return (err.empty() ? "" : err + " ") + "Moved " + std::to_string(ids.size()) + " bodies.";
    }
    if (name == "delete") {
        std::string err;
        auto ids = bodiesArg(err);
        if (ids.empty()) return err.empty() ? "Nothing to delete." : err;
        clearSelection();
        selectBodies(ids, false);
        deleteSelection();
        return (err.empty() ? "" : err + " ") + "Deleted " + std::to_string(ids.size()) + " bodies.";
    }
    if (name == "rename") {
        std::string from = lower(a["name"].asString()), to = a["new_name"].asString();
        if (to.empty()) return "new_name is empty.";
        for (auto& [id, b] : doc_.bodies)
            if (doc_.isBodyAlive(b) && (lower(b.name) == from || lower(aiBodyPath(id)) == from)) {
                pushUndo("Rename");
                b.name = to;
                syncBodyOverride(id);
                doc_.touch();
                return "Renamed the body to " + to + ".";
            }
        for (auto& [cid, c] : doc_.components)
            if (lower(c.name) == from || lower(compPath(cid)) == from) {
                pushUndo("Rename");
                c.name = to;
                rememberComponentName(cid);
                doc_.touch();
                return "Renamed the folder to " + to + ".";
            }
        return "No body or folder named '" + a["name"].asString() + "'.";
    }
    if (name == "set_parameter") {
        std::string n = a["name"].asString(), e = a["expression"].asString();
        if (n.empty() || !std::isalpha((unsigned char)n[0])) return "Parameter names start with a letter.";
        try { (void)peval::eval(e, doc_.params); } catch (const std::runtime_error& ex) { return std::string("Bad expression: ") + ex.what(); }
        pushUndo("Parameter value");
        Param& p = doc_.params[n];
        p.name = n;
        p.expr = e;
        std::string perr;
        updateParamValues(doc_.params, perr);
        resolveParametricSketches();
        doc_.touch();
        scheduleRecompute(0);
        return perr.empty() ? n + " = " + e : perr;
    }
    if (name == "create_box") {
        auto exprNum = [&](const Value& v, double def, std::string& expr, const char* what) {
            try {
                if (v.isString()) { expr = v.asString(); return peval::eval(expr, doc_.params); }
                if (v.isNumber()) { expr.clear(); return v.asNumber(); }
            } catch (const std::runtime_error& ex) {
                throw std::runtime_error(std::string(what) + ": " + ex.what());
            }
            expr.clear();
            return def;
        };
        double x = a["x"].asNumber(0), y = a["y"].asNumber(0), z = a["z"].asNumber(0);
        std::string wexpr, dexpr, hexpr;
        double w = 0, dpt = 0, h = 0;
        try {
            w = exprNum(a["width"], 0, wexpr, "width");
            dpt = exprNum(a["depth"], 0, dexpr, "depth");
            h = exprNum(a["height"], 0, hexpr, "height");
        } catch (const std::runtime_error& ex) { return ex.what(); }
        if (!(w > 0) || !(dpt > 0) || !(std::abs(h) > 0)) return "Sizes must be greater than zero.";
        uint32_t folder = aiEnsureFolder(a["component"].asString());
        if (sk_.active) finishSketch();
        sk::Plane pl;
        pl.x = {1, 0, 0};
        pl.y = {0, 1, 0};
        pl.o = {0, 0, z};
        pl.name = "XY";
        createSketchOn(pl, std::string());
        sk::Sketch* s = curSketch();
        if (!s) return "Could not start a sketch.";
        uint32_t p0 = s->addPoint({x, y}), p1 = s->addPoint({x + w, y}), p2 = s->addPoint({x + w, y + dpt}), p3 = s->addPoint({x, y + dpt});
        uint32_t l0 = s->addLine(p0, p1), l1 = s->addLine(p1, p2), l2 = s->addLine(p2, p3), l3 = s->addLine(p3, p0);
        if (!wexpr.empty() || !dexpr.empty()) {
            s->addConstraint(sk::CType::Horizontal, l0);
            s->addConstraint(sk::CType::Horizontal, l2);
            s->addConstraint(sk::CType::Vertical, l1);
            s->addConstraint(sk::CType::Vertical, l3);
            uint32_t fix = s->addConstraint(sk::CType::Fix, p0);
            s->cons[fix].value = x;
            s->cons[fix].value2 = y;
            if (!wexpr.empty()) s->cons[s->addConstraint(sk::CType::Distance, p0, p1, 0, w)].expr = wexpr;
            if (!dexpr.empty()) s->cons[s->addConstraint(sk::CType::Distance, p0, p3, 0, dpt)].expr = dexpr;
            sk::solve(*s, {});
        }
        s->touch();
        uint32_t sid = s->id;
        finishSketch();
        clearSelection();
        selProfiles_.insert({sid, 0});
        runExtrude();
        if (!dlg_.open) return "Could not start Extrude.";
        if (!hexpr.empty()) std::snprintf(dlg_.dist, sizeof dlg_.dist, "%s", hexpr.c_str());
        else std::snprintf(dlg_.dist, sizeof dlg_.dist, "%g", h);
        dlg_.opAuto = false;
        dlg_.op = 0;   // new body
        applyDialogValues();
        aiNameOutput(dlg_.featureId, a["name"].asString(), folder);
        closeFeatureDialog(true);
        return a["name"].asString().empty() ? "Created a box." : "Created " + a["name"].asString() + ".";
    }
    if (name == "create_hole") {
        std::vector<std::pair<glm::dvec3, glm::dvec3>> pts;
        glm::dvec3 forced(0);
        bool hasDir = a["direction"].size() == 3;
        if (hasDir) forced = {a["direction"][(size_t)0].asNumber(), a["direction"][(size_t)1].asNumber(), a["direction"][(size_t)2].asNumber()};
        std::vector<glm::dvec3> where;
        for (auto& p : a["positions"].items())
            if (p.size() >= 3) where.push_back({p[(size_t)0].asNumber(), p[(size_t)1].asNumber(), p[(size_t)2].asNumber()});
        if (where.empty() && a.has("x")) where.push_back({a["x"].asNumber(), a["y"].asNumber(), a["z"].asNumber()});
        if (where.empty()) return "Give positions: a list of [x,y,z] points on the face.";
        std::string notes;
        for (auto& w : where) {
            glm::dvec3 dir(0, 0, -1);
            bool found = false;
            if (!hasDir) {
                // The face the point is on decides the direction (straight into it).
                for (auto& [id, b] : doc_.bodies) {
                    if (!bodyShown(b)) continue;
                    auto pit = doc_.parts.find(b.partId);
                    if (pit == doc_.parts.end() || !pit->second.shape) continue;
                    double tol = std::max(0.05, kernel::shapeDiag(pit->second.shape) * 1e-3);
                    int fidx = kernel::faceNear(pit->second.shape, b.xform, w, tol);
                    if (fidx < 0) continue;
                    glm::dvec3 c, n;
                    bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, (uint32_t)fidx, c, n, planar)) continue;
                    n = glm::normalize(glm::dvec3(glm::dmat4(b.xform) * glm::dvec4(n, 0.0)));
                    dir = -n;
                    found = true;
                    break;
                }
                if (!found) notes = " (No face at one of the points, so that hole goes straight down; give points on a face.)";
            } else {
                dir = forced;
            }
            pts.push_back({w, dir});
        }
        std::string t = toLower(a["type"].asString("simple"));
        int kind = t == "counterbore" ? 1 : t == "countersink" ? 2 : 0;
        double extraDia = kind == 1 ? a["counterbore_diameter"].asNumber(0) : a["countersink_diameter"].asNumber(0);
        double extra = kind == 1 ? a["counterbore_depth"].asNumber(0) : a["countersink_angle"].asNumber(0);
        std::vector<TargetRef> only;
        if (a.has("target_bodies")) {
            std::string missing;
            auto ids = aiFindBodies(stringList(a["target_bodies"]), missing);
            if (!missing.empty()) return "Not found: " + missing + ". Use paths from get_design.";
            if (ids.empty()) return "Name the bodies the hole should cut in target_bodies.";
            for (uint32_t id : ids) {
                TargetRef r = refOfBody(id);
                if (!r.first) return "One of those bodies cannot be cut yet.";
                only.push_back(r);
            }
        }
        if (a["tapped"].asBool(false) && !(a["pitch"].asNumber(0) > 0))
            return "A tapped hole needs a pitch.";
        return addPlacedHoles(pts, a["diameter"].asNumber(0), a["depth"].asNumber(0), kind, extraDia, extra, only,
                              a["spotface_diameter"].asNumber(0), a["spotface_depth"].asNumber(0), a["drill_angle"].asNumber(0),
                              a["tapped"].asBool(false), a["pitch"].asNumber(0)) + notes;
    }
    if (name == "fillet" || name == "chamfer") {
        std::string err;
        auto ids = bodiesArg(err);
        double v = a[name == "fillet" ? "radius" : "distance"].asNumber(0);
        if (!(v > 0)) return "The size must be greater than zero.";
        std::string which = toLower(a["edges"].asString("all"));
        clearSelection();
        size_t count = 0;
        bool listed = a["edge_refs"].isArray() && a["edge_refs"].size() > 0;
        if (listed) {
            for (auto& item : a["edge_refs"].items()) {
                std::string missing;
                auto bids = aiFindBodies(stringList(item["body"].isString() ? item["body"] : item["names"]), missing);
                if (!item["body"].isString()) return "Each edge_ref needs a body name.";
                if (bids.empty()) return missing.empty() ? "No body for those edges." : "Not found: " + missing;
                for (auto& e : item["edges"].items())
                    if (e.isNumber()) { selEdges_.insert({bids[0], (uint32_t)e.asNumber()}); ++count; }
            }
            if (!count) return "edge_refs listed no edges.";
        }
        if (!listed && ids.empty()) return err.empty() ? "Name the bodies." : err;
        for (uint32_t id : ids) {
            if (listed) break;
            auto bit = doc_.bodies.find(id);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit == doc_.parts.end() || !pit->second.shape) continue;
            std::vector<std::vector<glm::dvec3>> polys;
            kernel::edgePolylines(pit->second.shape, 100000, polys);
            for (uint32_t sidx : kernel::seamEdges(pit->second.shape))
                if (sidx < polys.size()) polys[sidx].clear();   // seams of round faces are not edges
            glm::dmat4 xf(bit->second.xform);
            double zmin = 1e300, zmax = -1e300;
            for (auto& pl : polys)
                for (auto& p : pl) {
                    double z = (xf * glm::dvec4(p, 1.0)).z;
                    zmin = std::min(zmin, z);
                    zmax = std::max(zmax, z);
                }
            double tol = std::max(1e-3, (zmax - zmin) * 1e-4);
            // "outline": the straight edges that run through the thinnest direction of the body, which rounds
            // the outline of a plate however it stands. "x", "y", "z": straight edges along that axis.
            int alongAxis = which == "x" ? 0 : which == "y" ? 1 : which == "z" ? 2 : -1;
            if (which == "outline") {
                glm::dvec3 lo3(1e300), hi3(-1e300);
                for (auto& pl : polys)
                    for (auto& p : pl) {
                        glm::dvec3 w = glm::dvec3(xf * glm::dvec4(p, 1.0));
                        lo3 = glm::min(lo3, w);
                        hi3 = glm::max(hi3, w);
                    }
                glm::dvec3 ext = hi3 - lo3;
                alongAxis = ext.x <= ext.y && ext.x <= ext.z ? 0 : ext.y <= ext.z ? 1 : 2;
            }
            for (size_t e = 0; e < polys.size(); ++e) {
                const auto& pl = polys[e];
                if (pl.size() < 2) continue;
                glm::dvec3 p0 = glm::dvec3(xf * glm::dvec4(pl.front(), 1.0)), p1 = glm::dvec3(xf * glm::dvec4(pl.back(), 1.0));
                double lo = 1e300, hi = -1e300;
                for (auto& p : pl) {
                    double z = (xf * glm::dvec4(p, 1.0)).z;
                    lo = std::min(lo, z);
                    hi = std::max(hi, z);
                }
                glm::dvec3 d = p1 - p0;
                bool flat = hi - lo < tol;
                bool vertical = glm::length(d) > tol && std::abs(glm::normalize(d).z) > 0.999 && pl.size() == 2;
                bool along = alongAxis >= 0 && pl.size() == 2 && glm::length(d) > tol && std::abs(glm::normalize(d)[alongAxis]) > 0.999;
                bool take = which == "all" || along || (which == "vertical" && vertical) || (which == "horizontal" && flat) ||
                            (which == "top" && flat && std::abs(hi - zmax) < tol) || (which == "bottom" && flat && std::abs(lo - zmin) < tol);
                if (take) { selEdges_.insert({id, (uint32_t)e}); ++count; }
            }
        }
        if (!count) return "No edges matched '" + which + "'.";
        startToolFeature(name == "fillet" ? FeatureType::Fillet : FeatureType::Chamfer);
        if (!tdlg_.open) return "Could not start " + name + ".";
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", v);
        if (name == "fillet" && a.has("radius_end")) {
            double e = a["radius_end"].asNumber(0);
            if (!(e > 0)) { closeToolDialog(false); return "The end radius must be greater than zero."; }
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", e);
        }
        if (name == "chamfer" && a.has("distance2")) {
            double e = a["distance2"].asNumber(0);
            if (!(e > 0)) { closeToolDialog(false); return "The second distance must be greater than zero."; }
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", e);
        }
        applyToolDialog();
        closeToolDialog(true);
        return (err.empty() ? "" : err + " ") + (name == "fillet" ? "Rounded " : "Beveled ") + std::to_string(count) + " edges.";
    }
    if (name == "full_round") {
        std::string missing;
        std::vector<uint32_t> ids;
        if (a["body"].isString()) ids = aiFindBodies({a["body"].asString()}, missing);
        else {
            std::string err;
            ids = bodiesArg(err);
            if (ids.empty()) return err.empty() ? "Name the body." : err;
        }
        if (ids.empty()) return missing.empty() ? "Name the body." : "Not found: " + missing;
        auto bit = doc_.bodies.find(ids[0]);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        if (pit == doc_.parts.end() || !pit->second.shape) return "That body has no solid.";
        int face = a.has("face") ? (int)a["face"].asNumber(-1) : -1;
        glm::dvec3 c, n;
        bool planar = false;
        if (face < 0 || !kernel::faceInfo(pit->second.shape, (uint32_t)face, c, n, planar) || !planar)
            return "That face is not flat.";
        TargetRef r = refOfBody(ids[0]);
        if (!r.first) return "That body has no solid.";
        clearSelection();
        startToolFeature(FeatureType::Fillet);
        if (!tdlg_.open) return "Could not start the fillet.";
        tdlg_.option2 = 1;
        tdlg_.targets = {r};
        tdlg_.faces = {{0, (uint32_t)face}};
        tdlg_.edges.clear();
        tdlg_.faceTotal = kernel::faceCount(pit->second.shape);
        tdlg_.edgeTotal = kernel::edgeCount(pit->second.shape);
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "1");
        applyToolDialog();
        bool picked = tdlg_.option2 == 1 && !tdlg_.faces.empty();
        closeToolDialog(true);
        if (!picked) return "The face was not picked.";
        return "Replaced the face with a full round.";
    }
    if (name == "export_step") {
        std::string path = a["path"].asString();
        if (path.empty()) return "Give a path ending in .step.";
        std::vector<std::string> wanted;
        if (a["names"].isArray())
            for (auto& e : a["names"].items())
                if (e.isString()) wanted.push_back(e.asString());
        std::string missing;
        std::vector<uint32_t> ids;
        if (!wanted.empty()) {
            ids = aiFindBodies(wanted, missing);
            if (!missing.empty()) return "Not found: " + missing + ". Use paths from get_design.";
        } else {
            for (auto& [id, b] : doc_.bodies)
                if (doc_.isBodyAlive(b) && bodyShown(b)) ids.push_back(id);
        }
        std::vector<kernel::ExportItem> items;
        for (uint32_t id : ids) {
            auto bit = doc_.bodies.find(id);
            if (bit == doc_.bodies.end()) continue;
            auto pit = doc_.parts.find(bit->second.partId);
            if (pit == doc_.parts.end() || !pit->second.shape) continue;
            kernel::ExportItem it;
            it.shape = pit->second.shape;
            it.mesh = pit->second.mesh;
            it.xform = bit->second.xform;
            it.name = bit->second.name;
            it.color = bit->second.color;
            it.component = componentPath(bit->second.componentId);
            items.push_back(std::move(it));
        }
        if (items.empty()) return "Nothing solid to export.";
        try {
            CancelToken cancel;
            kernel::exportStep(items, path, cancel);
        } catch (const std::runtime_error& ex) {
            return std::string(ex.what());
        }
        return "saved=" + path + " bodies=" + std::to_string(items.size());
    }
    if (name == "import_file") {
        std::string path = a["path"].asString();
        if (path.empty()) return "Give a path.";
        importPath(path);
        return "Importing " + path;
    }
    std::string modeled;
    if (aiModelTool(name, argsJson, modeled)) return modeled;
    return "Unknown tool " + name;
}

// ===================================================================== providers

namespace {
struct ResolvedProvider { std::string label, help; bool needsKey = false; bool custom = false; ProviderCfg cfg; };
}

static ResolvedProvider resolveProvider(const Settings& st, const std::string& id) {
    ResolvedProvider r;
    auto it = st.ai.find(id);
    Settings::AiProvider sp = it != st.ai.end() ? it->second : Settings::AiProvider{};
    if (const ProviderPreset* p = presetFor(id)) {
        r.label = p->label;
        r.help = p->help;
        r.needsKey = p->needsKey;
        r.cfg.kind = p->kind;
        r.cfg.url = sp.url.empty() ? p->url : sp.url;
        r.cfg.model = sp.model.empty() ? p->model : sp.model;
    } else {
        r.custom = true;
        r.label = sp.name.empty() ? id : sp.name;
        r.help = "Your own provider: any OpenAI style, Anthropic style or Ollama server";
        r.cfg.kind = kindOfFormat(sp.format);
        r.cfg.url = sp.url;
        r.cfg.model = sp.model;
    }
    r.cfg.key = platform::unprotectSecret(sp.key);
    r.cfg.thinking = std::clamp(sp.thinking, 0, 4);
    r.cfg.ctx = std::max(0, sp.ctx);
    return r;
}

// ===================================================================== conversation

// Every chat is written to ai_logs/<date>.txt in Kestral's settings folder (one file per day), so
// a session can be looked at afterwards.
void App::aiLog(const std::string& who, const std::string& text) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char day[32], clock[16];
    std::strftime(day, sizeof day, "%Y-%m-%d", &tmv);
    std::strftime(clock, sizeof clock, "%H:%M:%S", &tmv);
    std::string dir = platform::appDataDir() + "/ai_logs";
    std::error_code ec;
    std::filesystem::create_directories(u8path(dir), ec);
    std::ofstream f(u8path(dir + "/" + day + ".txt"), std::ios::app);
    if (!f) return;
    if (who == "user") {
        ResolvedProvider cur = resolveProvider(settings_, settings_.aiProvider);
        f << "\n=== " << clock << "  " << cur.label << " | " << cur.cfg.model << " ===\n";
    }
    f << clock << " " << who << ": " << text << "\n";
}

// ---------------------------------------------------------------- smart tools

namespace {

// Loaded with every message (smart mode); the rest come in through find_tools or the user's words.
const char* kCoreTools[] = {"find_tools", "get_design", "get_body", "select", "create_box", "create_cylinder", "create_sketch", "sketch_add",
                            "extrude", "revolve", "fillet", "chamfer", "create_hole", "move", "delete", "set_color", "combine", "run_command", "look_at_design"};
const char* kSliceTools[] = {"print_setup", "slice", "slice_view", "slice_tab", "find_slice_settings", "start_print", "printer_status"};

// Words people use for a tool that are not in its name.
struct Syn { const char* word; const char* tools; };
const Syn kSyn[] = {
    {"screenshot", "look_at_design"}, {"look", "look_at_design"}, {"check", "look_at_design"}, {"picture", "look_at_design"},
    {"photo", "look_at_design"}, {"image", "look_at_design"}, {"see", "look_at_design"},
    {"print", "print_setup slice slice_view start_print find_slice_settings"}, {"slice", "print_setup slice slice_view slice_tab find_slice_settings"},
    {"layer", "slice_view find_slice_settings"}, {"infill", "slice_view find_slice_settings"}, {"wall", "slice_view find_slice_settings"},
    {"brim", "slice_view find_slice_settings"}, {"support", "slice_view find_slice_settings"}, {"filament", "slice_tab"}, {"ams", "slice_tab printer_device"},
    {"plate", "slice_view slice_tab"}, {"printer", "printer_status printer_device set_printer"}, {"nozzle", "printer_device slice_tab"},
    {"camera", "printer_device"}, {"temperature", "printer_device find_slice_settings"}, {"pause", "printer_control"}, {"stop", "printer_control"},
    {"colour", "set_color"}, {"color", "set_color"}, {"material", "set_appearance"}, {"wood", "set_appearance"}, {"metal", "set_appearance"},
    {"appearance", "set_appearance"}, {"look", "set_appearance"}, {"hole", "create_hole"}, {"round", "fillet full_round"}, {"bevel", "chamfer"},
    {"pattern", "rect_pattern circular_pattern pattern_on_path"}, {"array", "rect_pattern circular_pattern"}, {"text", "emboss"}, {"engrave", "emboss"},
    {"letters", "emboss"}, {"screw", "thread create_hole"}, {"bolt", "thread"}, {"spring", "create_coil"}, {"coil", "create_coil"},
    {"hollow", "shell"}, {"save", "save_design"}, {"open", "open_design list_designs"}, {"designs", "list_designs open_design"},
    {"hide", "set_visibility"}, {"show", "set_visibility"}, {"visible", "set_visibility"}, {"rename", "rename"}, {"name", "rename"},
    {"remove", "delete"}, {"turn", "rotate"}, {"rotate", "rotate"}, {"bigger", "scale"}, {"smaller", "scale"}, {"resize", "scale set_transform"},
    {"measure", "measure"}, {"distance", "measure"}, {"export", "export_step"}, {"step", "export_step"}, {"stl", "export_step"},
    {"import", "import_file"}, {"sphere", "create_sphere"}, {"ball", "create_sphere"}, {"cone", "create_cone"}, {"torus", "create_torus"},
    {"ring", "create_torus create_tube"}, {"tube", "create_tube pipe"}, {"pipe", "pipe"}, {"wedge", "create_wedge"}, {"cut", "split_body combine"},
    {"split", "split_body split_face"}, {"join", "combine"}, {"union", "combine"}, {"subtract", "combine"}, {"mirror", "mirror"},
    {"sweep", "sweep"}, {"loft", "loft"}, {"rib", "rib"}, {"draft", "draft"}, {"thicken", "thicken"}, {"plane", "offset_plane plane_at_angle midplane"},
    {"component", "create_component activate_component"}, {"folder", "create_component"}, {"robot", "robot"}, {"joint", "joint rigid_group asbuilt_joint"},
    {"parameter", "set_parameter"}, {"section", "section_analysis"}, {"constraint", "sketch_constrain"}, {"dimension", "sketch_constrain"},
    {"trim", "sketch_ops"}, {"offset", "sketch_ops offset_face"}, {"push", "press_pull"}, {"pull", "press_pull"}, {"align", "align"},
    {"snap", "snap"}, {"lip", "lip"}, {"boss", "boss"}, {"web", "web"}, {"gear", "gear"}, {"blade", "blades"}, {"fan", "blades"},
    {"propeller", "blades"}, {"wing", "wing"}, {"dovetail", "dovetail"}, {"interference", "interference"}, {"collide", "interference"},
};

bool toolExists(const std::string& n) {
    for (auto& t : kTools) if (n == t.name) return true;
    return false;
}

// "Thread M8 bolt" -> lower case words.
std::vector<std::string> wordsOf(const std::string& text) {
    std::vector<std::string> w;
    std::string cur;
    for (char c : text) {
        if (std::isalnum((unsigned char)c)) cur += (char)std::tolower((unsigned char)c);
        else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

bool wordMatch(const std::vector<std::string>& words, const std::string& key) {
    for (auto& w : words) {
        if (w == key) return true;
        if (key.size() >= 4 && w.size() >= 4 && (w.rfind(key, 0) == 0 || key.rfind(w, 0) == 0)) return true;   // chamfer / chamfers
    }
    return false;
}

// Ollama: what is loaded right now.
struct PsInfo {
    bool ok = false, loaded = false;
    int ctx = 0;
    double size = 0, vram = 0;
    std::vector<std::pair<std::string, double>> others;   // other models and their size on the card (bytes)
};
PsInfo ollamaPs(const std::string& url, const std::string& model) {
    PsInfo r;
    auto h = platform::httpGet(trimSlash(url) + "/api/ps", {}, 4000, nullptr);
    if (!h.error.empty() || h.status != 200) return r;
    r.ok = true;
    Value v = Value::parse(h.body);
    for (auto& m : v["models"].items()) {
        std::string nm = m["name"].asString(m["model"].asString());
        if (nm == model || nm == model + ":latest") {
            r.loaded = true;
            r.ctx = (int)m["context_length"].asNumber(0);
            r.size = m["size"].asNumber(0);
            r.vram = m["size_vram"].asNumber(0);
        } else {
            r.others.push_back({nm, m["size_vram"].asNumber(m["size"].asNumber(0))});
        }
    }
    return r;
}

// The graphics card: total and free memory (GB) and the programs using it, from nvidia-smi.
struct GpuInfo { bool ok = false; double totalGb = 0, usedGb = 0; std::vector<std::pair<std::string, double>> apps; };
GpuInfo gpuInfo() {
    GpuInfo g;
    auto r = platform::runProcess("nvidia-smi", {"--query-gpu=memory.used,memory.total", "--format=csv,noheader,nounits"}, 4000);
    if (r.exitCode != 0 || r.output.empty()) return g;
    double used = 0, total = 0;
    if (std::sscanf(r.output.c_str(), "%lf, %lf", &used, &total) == 2) { g.ok = true; g.usedGb = used / 1024.0; g.totalGb = total / 1024.0; }
    auto a = platform::runProcess("nvidia-smi", {"--query-compute-apps=process_name,used_memory", "--format=csv,noheader,nounits"}, 4000);
    std::istringstream in(a.output);
    std::string line;
    while (std::getline(in, line)) {
        size_t c = line.rfind(',');
        if (c == std::string::npos) continue;
        std::string name = line.substr(0, c);
        size_t sl = name.find_last_of("\\/");
        if (sl != std::string::npos) name = name.substr(sl + 1);
        double mb = std::atof(line.c_str() + c + 1);
        std::string ln = name;
        for (auto& ch : ln) ch = (char)std::tolower((unsigned char)ch);
        if (ln.find("ollama") != std::string::npos || ln.find("kestral") != std::string::npos) continue;
        if (mb >= 300) g.apps.push_back({name, mb / 1024.0});
    }
    return g;
}

// The model's weights size from Ollama's list (bytes), 0 when unknown.
// What an Ollama model can do ("completion", "vision", "tools", "thinking"...), from /api/show.
std::set<std::string> ollamaCaps(const std::string& url, const std::string& model) {
    std::set<std::string> caps;
    Value b = Value::object();
    b.set("model", model);
    b.set("name", model);   // older Ollama versions read this one
    auto r = platform::httpPost(trimSlash(url) + "/api/show", {{"Content-Type", "application/json"}}, b.dump(), 20000, nullptr);
    if (r.status != 200) {
        log::warn("Assistant: Ollama /api/show for " + model + " said " + std::to_string(r.status) + " " + r.error);
        return caps;
    }
    Value v = Value::parse(r.body);
    for (auto& c : v["capabilities"].items()) caps.insert(c.asString());
    // Older Ollama versions do not list capabilities: a vision projector or vision layers also tell.
    if (v["projector_info"].isObject() && !v["projector_info"].members().empty()) caps.insert("vision");
    for (auto& [k, val] : v["model_info"].members())
        if (k.find(".vision.") != std::string::npos) { caps.insert("vision"); break; }
    for (auto& f : v["details"]["families"].items())
        if (f.asString() == "clip" || f.asString() == "mllama") caps.insert("vision");
    caps.insert("answered");
    return caps;
}

double ollamaModelSize(const std::string& url, const std::string& model) {
    auto h = platform::httpGet(trimSlash(url) + "/api/tags", {}, 4000, nullptr);
    if (h.status != 200) return 0;
    for (auto& m : Value::parse(h.body)["models"].items()) {
        std::string nm = m["name"].asString(m["model"].asString());
        if (nm == model || nm == model + ":latest") return m["size"].asNumber(0);
    }
    return 0;
}

} // namespace

std::set<std::string> App::aiPickTools(const std::string& userText) const {
    std::set<std::string> out(std::begin(kCoreTools), std::end(kCoreTools));
    if (sliceWs_) out.insert(std::begin(kSliceTools), std::end(kSliceTools));
    auto words = wordsOf(userText);
    for (auto& t : kTools) {
        std::string n = t.name;
        // The tool's own name words ("create_hole" -> "hole").
        size_t st = 0;
        while (st < n.size()) {
            size_t e = n.find('_', st);
            std::string part = n.substr(st, e == std::string::npos ? std::string::npos : e - st);
            static const std::set<std::string> generic = {"create", "get", "set", "on", "at", "to", "body", "design", "path", "tab", "view", "file"};
            if (!generic.count(part) && part.size() >= 3 && wordMatch(words, part)) out.insert(n);
            if (e == std::string::npos) break;
            st = e + 1;
        }
    }
    for (auto& sy : kSyn) {
        if (!wordMatch(words, sy.word)) continue;
        std::istringstream in(sy.tools);
        std::string tn;
        while (in >> tn) if (toolExists(tn)) out.insert(tn);
    }
    return out;
}

std::string App::aiFindTools(const std::string& query, std::set<std::string>& active) const {
    auto words = wordsOf(query);
    if (words.empty()) return "Give a few words, e.g. find_tools {\"query\":\"thread\"}.";
    // Score every tool by its name, description and the synonyms that point at it.
    std::vector<std::pair<int, const ToolDef*>> hits;
    for (auto& t : kTools) {
        std::string hay = toLower(std::string(t.name) + " " + t.desc);
        for (auto& c : hay) if (c == '_') c = ' ';
        int score = 0;
        for (auto& w : words) {
            if (w.size() < 2) continue;
            if (std::string(t.name).find(w) != std::string::npos) score += 5;
            else if (hay.find(w) != std::string::npos) score += 2;
        }
        for (auto& sy : kSyn)
            if (wordMatch(words, sy.word) && std::string(" ") .append(sy.tools).append(" ").find(std::string(" ") + t.name + " ") != std::string::npos) score += 4;
        if (score > 0) hits.push_back({score, &t});
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::ostringstream o;
    int n = 0;
    std::vector<std::string> loaded;
    for (auto& [sc, t] : hits) {
        if (++n > 8) break;
        std::string d = t->desc;
        size_t dot = d.find(". ");
        if (dot != std::string::npos && dot < 220) d = d.substr(0, dot + 1);
        else if (d.size() > 220) d = d.substr(0, 217) + "...";
        o << "- " << t->name << ": " << d << "\n";
        if (!active.count(t->name)) loaded.push_back(t->name);
        active.insert(t->name);
    }
    // Commands for run_command, and appearance names.
    std::string cmds;
    int nc = 0;
    for (size_t i = 0; i < tools_.size() && nc < 8; ++i) {
        const Tool& t = tools_[i];
        if (!t.available()) continue;
        std::string hay = toLower(t.id + " " + t.label + " " + t.tip);
        bool all = true;
        for (auto& w : words) all &= hay.find(w) != std::string::npos;
        if (!all) continue;
        cmds += "- " + t.id + ": " + t.label + (t.shortcut.empty() ? "" : " [" + t.shortcut + "]") + "\n";
        ++nc;
    }
    std::string looks;
    for (auto& lm : materialLibrary()) {
        std::string hay = toLower(std::string(lm.category) + " " + lm.name);
        for (auto& w : words)
            if (w.size() >= 3 && hay.find(w) != std::string::npos) { looks += (looks.empty() ? "" : "; ") + std::string(lm.name); break; }
    }
    std::string out;
    if (n) out += "Tools (loaded for your next step" + std::string(loaded.empty() ? "" : ": " + std::to_string(loaded.size()) + " new") + "):\n" + o.str();
    if (!cmds.empty()) out += "Commands for run_command:\n" + cmds;
    if (!looks.empty()) out += "Appearances for set_appearance: " + looks + "\n";
    if (out.empty()) out = "Nothing matches \"" + query + "\". Try other words (what it does, e.g. \"round edges\", \"hole\", \"copy around a circle\").";
    return out;
}

std::string App::aiSystemPromptLean() const {
    std::ostringstream o;
    o << "You are the assistant inside Kestral, a desktop parametric CAD program that works like Fusion 360, with a built in "
         "slicer (OrcaSlicer's engine) in its Slice tab and printer control. Millimeters and degrees, Z is up.\n"
         "- When the user asks you to make or change something, do it yourself with the tools. Explain steps only when asked.\n"
         "- Call get_design before changing a design and use the exact body paths it returns. Name new bodies and put them in "
         "a component (folder).\n"
         "- Only the common tools are loaded. For anything else (holes, chamfers, patterns, threads, gears, text, shells, "
         "materials, joints, planes, slice and printer controls...) call find_tools with a few words, then use the tool it loads.\n"
         "- Cuts (holes, combine cut) take target_bodies. Read every tool result: if something failed, say so and try another way "
         "instead of repeating the same call. Every change can be undone (Ctrl+Z).\n"
         "- Printing: call print_setup first and use the user's own profiles. Slice, tell the print time and filament, and only "
         "call start_print when the user asked to print. Any slicer setting can be changed: find its key with find_slice_settings. "
         "Stronger: more walls (wall_loops 3-4) and infill (20-40%). Faster: thicker layers (0.24-0.28), less infill. Finer: "
         "thinner layers (0.12-0.16).\n"
         "- Each user message starts with [Kestral context: ...]: where the user is in Kestral right now and what is selected. "
         "Use it, never repeat it back.\n"
         "- Check your work with look_at_design after building something (and before saying it is done when the user gave a "
         "picture): look, compare, fix what differs.\n"
         "- Keep replies short and concrete.\n";
    return o.str();
}

std::string App::aiContextNote() const {
    // Where the user is, so "this", "here" and "the selected part" mean something to the model.
    std::ostringstream o;
    if (sliceWs_) {
        const char* tops[] = {"Home", "Prepare", "Preview", "Device", "Project", "Calibration"};
        std::string tab = sliceActive_ >= 0 && sliceActive_ < (int)sliceDocs_.size() ? sliceDocs_[(size_t)sliceActive_].name : doc_.name;
        o << "Slice tab \"" << tab << "\", " << tops[std::clamp(sliceCurTop(), 0, 5)] << " page";
        int parts = 0;
        for (auto& [id, p] : slice_.plate) parts += p.onPlate;
        o << "; " << parts << " part" << (parts == 1 ? "" : "s") << " on " << slice_.plates << " plate" << (slice_.plates == 1 ? "" : "s")
          << " (Slice plate slices plate " << slice_.activePlate + 1 << ")";
        std::string sel;
        for (auto [oid, k] : plateSelection()) {
            auto it = slice_.plate.find(oid);
            if (it != slice_.plate.end()) sel += (sel.empty() ? "" : ", ") + it->second.name;
        }
        if (!sel.empty()) o << "; selected: " << sel;
        o << "; printer profile " << slice_.machine << ", process " << slice_.process << ", " << slice_.ex.fils.size() << " filament(s)";
        {
            std::lock_guard lock(const_cast<std::mutex&>(print_.m));
            if (print_.last.ok) o << "; last slice " << print::formatDuration(print_.last.seconds) << ", " << (int)std::lround(print_.last.grams) << " g";
        }
        if (slice_.devStatus.ok) o << "; printer is " << slice_.devStatus.state;
        if (!slice_.overrides.empty()) o << "; " << slice_.overrides.size() << " setting(s) changed in the tab";
    } else if (startOpen_) {
        o << "Start page (no design open)";
    } else {
        o << "Design \"" << doc_.name << "\" (" << (doc_.path.empty() ? "not saved yet" : "saved") << ")";
        int bodies = 0;
        for (auto& [id, b] : doc_.bodies) bodies += doc_.isBodyAlive(b) ? 1 : 0;
        o << ", " << bodies << " bodies, " << doc_.timeline.size() << " timeline steps";
        if (recomputeQueuedFrom_ >= 0 || recomputeRunning_) o << ", geometry still rebuilding";
        if (sk_.active) {
            auto it = doc_.sketches.find(sk_.sketchId);
            o << "; editing sketch " << (it != doc_.sketches.end() ? it->second.name : std::string("?")) << " (sketch id " << sk_.sketchId << ")";
            if (!sk_.tool.empty()) o << " with the " << sk_.tool << " tool";
        }
        if (dlg_.open || tdlg_.open) o << "; a command dialog is open";
        std::string sel;
        int n = 0;
        for (uint32_t id : selection_) if (++n <= 6) sel += (sel.empty() ? "" : ", ") + aiBodyPath(id);
        if (n > 6) sel += " and " + std::to_string(n - 6) + " more";
        if (!sel.empty()) o << "; selected bodies: " << sel;
        if (!selFaces_.empty()) o << "; " << selFaces_.size() << " face(s) selected";
        if (!selEdges_.empty()) o << "; " << selEdges_.size() << " edge(s) selected";
    }
    if (settingsOpen_) o << "; Preferences window open";
    if (dataOpen_) o << "; Data panel open";
    return o.str();
}

void App::aiSend(const std::string& textIn, bool showLine, std::vector<std::shared_ptr<const AiImage>> pics) {
    if (showLine) pics.swap(aiAttach_);   // pictures waiting in the box go with this message
    if (ai_.busy || (textIn.empty() && pics.empty())) { if (!pics.empty()) aiAttach_ = pics; return; }
    std::string text = textIn.empty() ? std::string("Model what this picture shows.") : textIn;
    if (showLine) {
        AiLine ln{0, textIn};
        ln.images = pics;
        ai_.lines.push_back(ln);
        aiLog("user", text + (pics.empty() ? "" : " [" + std::to_string(pics.size()) + " picture(s)]"));
        aiFollow_ = true;   // you just wrote: back to the bottom
    }
    ai_.scroll = true;
    AiTurn u;
    u.role = "user";
    // The model also sees where the user is right now (page, selection), the chat shows what they typed.
    u.text = settings_.aiPageContext ? "[Kestral context: " + aiContextNote() + "]\n" + text : text;
    u.images = pics;
    ai_.history.push_back(u);
    ai_.busy = true;
    ai_.cancel = std::make_shared<std::atomic<bool>>(false);

    ResolvedProvider rp = resolveProvider(settings_, settings_.aiProvider);
    ProviderCfg cfg = rp.cfg;
    cfg.keepAliveMin = settings_.aiKeepAliveMin;
    cfg.temperature = settings_.aiTemperature;
    cfg.compact = settings_.aiCompact;
    cfg.compactAt = settings_.aiCompactAt;
    int modelMax = 0;
    if (ai_.ctxFor == settings_.aiProvider + "|" + cfg.model && ai_.ctxMax > 0) {
        modelMax = ai_.ctxMax;
        if (cfg.ctx > ai_.ctxMax) cfg.ctx = ai_.ctxMax;
    }
    if (cfg.url.empty()) {
        ai_.lines.push_back({3, "This provider has no address yet. Open the assistant settings (gear)."});
        ai_.busy = false;
        return;
    }
    // Smart tools (local models by default): a small core, the tools the message names, and find_tools.
    bool local = cfg.kind == 1 || settings_.aiProvider == "lmstudio" || cfg.url.find("localhost") != std::string::npos || cfg.url.find("127.0.0.1") != std::string::npos;
    bool smart = settings_.aiToolsMode == 1 || (settings_.aiToolsMode == 0 && local);
    std::set<std::string> active;
    if (smart) {
        active = aiPickTools(text);
        active.insert(aiActiveTools_.begin(), aiActiveTools_.end());
    }
    {
        std::lock_guard<std::mutex> lk(aiLiveMx_);
        aiLiveIsOllama_ = cfg.kind == 1;
        aiLiveUrl_ = cfg.url;
        aiLiveModel_ = cfg.model;
        aiLiveOllama_.clear();
        aiLiveStage_ = "Starting";
        aiLiveStarted_ = false;
        aiLiveT0_ = glfwGetTime();
        aiPsAt_ = -1e9;
    }
    std::string system = smart ? aiSystemPromptLean() : aiSystemPrompt();
    bool anyPics = false;
    for (auto& t : ai_.history) anyPics |= !t.images.empty();
    if (anyPics)
        system += "\nPICTURES: when the user attaches a picture (a photo, a screenshot, a sketch or a drawing), model what it shows as "
                  "exactly as you can. Use every dimension written on it. Without dimensions, keep the proportions and ask for one real "
                  "size (or use a sensible one and say which). Build it feature by feature, then compare with the picture.\n";
    if (!settings_.aiCustomPrompt.empty()) system += "\nTHE USER'S OWN INSTRUCTIONS (always follow them):\n" + settings_.aiCustomPrompt + "\n";
    auto history = ai_.history;
    auto cancel = ai_.cancel;
    bool gpuCheck = settings_.aiGpuCheck && cfg.kind == 1;
    bool autoCtx = cfg.kind == 1 && cfg.ctx == 0;
    int maxSteps = settings_.aiMaxSteps;
    std::string dismissed = aiGpuDismissed_;
    int visionSetting = settings_.aiMainVision;
    std::string visionModel = settings_.aiVisionModel;
    int visionKnown = ai_.visionFor == settings_.aiProvider + "|" + cfg.model ? ai_.mainVision : -1;
    std::string visionKey = settings_.aiProvider + "|" + cfg.model;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("assistant", [this, jobs, cfg, system, history, cancel, smart, active, gpuCheck, autoCtx, modelMax, maxSteps, dismissed,
                                visionSetting, visionModel, visionKnown, visionKey]() mutable {
        auto post = [&](std::function<void()> fn) { jobs->post(std::move(fn)); };
        auto stage = [&](const std::string& st) {
            {
                std::lock_guard<std::mutex> lk(aiLiveMx_);
                if (!aiLiveStarted_) aiLiveT0_ = glfwGetTime();   // the clock shows how long this stage takes
                aiLiveStage_ = st;
            }
            glfwPostEmptyEvent();
        };
        // A question card in the chat; the answer comes back here (-1: stopped).
        auto ask = [&](const std::string& title, const std::string& body, std::vector<std::string> options) {
            auto prom = std::make_shared<std::promise<int>>();
            auto fut = prom->get_future();
            post([this, prom, title, body, options] {
                aiChoice_.open = true;
                aiChoice_.title = title;
                aiChoice_.text = body;
                aiChoice_.options = options;
                aiChoice_.answer = prom;
                ai_.scroll = true;
                activity();
            });
            while (!cancel->load())
                if (fut.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) return fut.get();
            post([this] { aiChoice_.open = false; aiChoice_.answer.reset(); });
            return -1;
        };
        // ---- Before the first word: is the graphics card free for this model?
        if (gpuCheck) {
            stage("Checking the graphics card");
            PsInfo ps = ollamaPs(cfg.url, cfg.model);
            GpuInfo g = gpuInfo();
            double need = ps.loaded ? 0 : ollamaModelSize(cfg.url, cfg.model) / 1e9;
            double otherOllama = 0;
            std::string others;
            for (auto& [n, b] : ps.others) { otherOllama += b / 1e9; char t[64]; std::snprintf(t, sizeof t, "%s (%.1f GB)", n.c_str(), b / 1e9); others += (others.empty() ? "" : ", ") + std::string(t); }
            double freeGb = g.ok ? g.totalGb - g.usedGb : 0;
            std::string apps;
            double appsGb = 0;
            for (auto& [n, gb] : g.apps) { appsGb += gb; char t[96]; std::snprintf(t, sizeof t, "%s %.1f GB", n.c_str(), gb); apps += (apps.empty() ? "" : ", ") + std::string(t); }
            bool tight = g.ok && need > 0 && need + 1.5 > freeGb;   // weights plus a little for the window
            std::string sig = others + "|" + apps + "|" + (tight ? "t" : "");
            if ((!others.empty() || (tight && (!apps.empty() || otherOllama > 0))) && sig != dismissed) {
                std::ostringstream b;
                char line[200];
                if (g.ok) { std::snprintf(line, sizeof line, "The graphics card has %.1f of %.1f GB free.", freeGb, g.totalGb); b << line << " "; }
                if (need > 0) { std::snprintf(line, sizeof line, "%s needs about %.1f GB plus its context window.", cfg.model.c_str(), need); b << line << " "; }
                if (!others.empty()) b << "Ollama still has " << others << " loaded. ";
                if (!apps.empty()) b << "Other programs on the card: " << apps << ". ";
                b << "If the model does not fit, part of it runs on the CPU and answers get much slower.";
                std::vector<std::string> opts;
                if (!others.empty()) opts.push_back("Unload the other models and continue");
                opts.push_back("Continue anyway");
                opts.push_back("Cancel");
                int pick = ask("The graphics card is busy", b.str(), opts);
                std::string chosen = pick >= 0 && pick < (int)opts.size() ? opts[(size_t)pick] : "Cancel";
                if (chosen == "Cancel") {
                    post([this] { ai_.busy = false; ai_.lines.push_back({3, "Stopped before sending."}); });
                    return;
                }
                if (chosen.rfind("Unload", 0) == 0) {
                    stage("Unloading the other models");
                    for (auto& [n, sz] : ps.others) {
                        Value bd = Value::object();
                        bd.set("model", n);
                        bd.set("keep_alive", 0);
                        platform::httpPost(trimSlash(cfg.url) + "/api/generate", {}, bd.dump(), 20000, cancel.get());
                    }
                    post([this, others] { ai_.lines.push_back({2, "Unloaded " + others + " from Ollama"}); });
                } else {
                    post([this, sig] { aiGpuDismissed_ = sig; });
                }
            }
        }
        // ---- Pictures. A model that sees gets them as they are; for one that cannot, the picture helper
        // (a vision model) looks first and its words go to the chat model instead.
        int seesCached = -1;
        auto mainSees = [&]() -> bool {
            if (seesCached >= 0) return seesCached == 1;
            bool sees = visionSetting == 1;
            if (visionSetting == 0) {
                if (cfg.kind != 1) sees = true;   // cloud models (Claude, GPT, Gemini, Grok) see pictures
                else if (visionKnown >= 0) sees = visionKnown == 1;
                else {
                    stage("Checking what the model can do");
                    auto caps = ollamaCaps(cfg.url, cfg.model);
                    sees = caps.count("vision") > 0 || (!caps.count("answered") && visionModel.empty());   // no answer and no helper: just try
                    if (caps.count("answered")) post([this, visionKey, sees] { ai_.visionFor = visionKey; ai_.mainVision = sees ? 1 : 0; });
                }
            }
            seesCached = sees ? 1 : 0;
            return sees;
        };
        const char* noHelper = "This model cannot see pictures. Pick a vision model in the assistant settings (Pictures) to describe them for it, "
                               "or use a model that sees pictures.";
        // The helper looks at the pictures; its thinking and words stream into the chat and stay as a card.
        // Returns its description, or sets err ("\x01" when stopped).
        auto describe = [&](const std::vector<std::shared_ptr<const AiImage>>& imgs, const std::string& system2, const std::string& prompt,
                            std::string& err) -> std::string {
            stage("Looking at the picture with " + visionModel);
            ProviderCfg vc = cfg;
            vc.model = visionModel;
            // Same thinking setting as the chat model; its thoughts show in the chat like the main model's.
            if (vc.kind == 1) { vc.ctx = 8192; vc.keepAliveMin = 0; }   // unloaded right after, the main model needs the card
            AiTurn ask;
            ask.role = "user";
            ask.images = imgs;
            ask.text = prompt;
            AiTurn vr;
            std::set<std::string> none;
            {
                std::lock_guard<std::mutex> lk(aiLiveMx_);
                aiLiveThink_.clear();
                aiLiveText_.clear();
                aiLiveStarted_ = false;
                aiLiveT0_ = glfwGetTime();
            }
            double vt0 = glfwGetTime();
            auto vDelta = [this, visionModel](const std::string& th, const std::string& tx) {
                {
                    std::lock_guard<std::mutex> lk(aiLiveMx_);
                    aiLiveStarted_ = true;
                    aiLiveThink_ = th;
                    aiLiveText_ = tx;
                    aiLiveStage_ = (tx.empty() ? visionModel + " is looking at the picture" : visionModel + " is describing the picture");
                }
                glfwPostEmptyEvent();
            };
            err = callModel(vc, system2, {ask}, vr, cancel.get(), vDelta, &none, nullptr);
            std::string vThink;
            std::string desc = stripThinking(vr.text, &vThink);
            if (vThink.empty()) vThink = vr.thinking;
            {
                std::lock_guard<std::mutex> lk(aiLiveMx_);
                aiLiveThink_.clear();
                aiLiveText_.clear();
                aiLiveStarted_ = false;
            }
            while (!vThink.empty() && std::isspace((unsigned char)vThink.back())) vThink.pop_back();
            while (!vThink.empty() && std::isspace((unsigned char)vThink.front())) vThink.erase(vThink.begin());
            double vSecs = glfwGetTime() - vt0;
            if (cancel->load()) { err = "\x01"; return {}; }
            if (!err.empty() || desc.empty()) {
                err = visionModel + " could not describe the picture" + (err.empty() ? "." : ": " + err);
                return {};
            }
            post([this, desc, visionModel, vThink, vSecs] {
                if (!vThink.empty()) ai_.lines.push_back({4, vThink, vSecs});
                ai_.lines.push_back({5, visionModel + "\n" + desc, vSecs});   // the description card
                aiLog("picture description by " + visionModel, desc);
                ai_.scroll = true;
            });
            return desc;
        };
        // The user's pictures (with the message, or sent while it works). Returns a problem to show ("" fine).
        auto lookAt = [&](AiTurn& u) -> std::string {
            if (u.images.empty() || mainSees()) return {};
            if (visionModel.empty() || visionModel == cfg.model) return noHelper;
            std::string req = u.text;
            size_t ctxEnd = req.rfind("]\n", 400);
            if (req.rfind("[Kestral context:", 0) == 0 && ctxEnd != std::string::npos) req = req.substr(ctxEnd + 2);
            if (req.rfind("[The user wrote this", 0) == 0 && req.find(":] ") != std::string::npos) req = req.substr(req.find(":] ") + 3);
            std::string vsys =
                "You are the eyes of a CAD assistant that cannot see. It will model what is in these pictures from your words alone, "
                "so describe them precisely and completely. Say what the object is, then its overall shape and size proportions "
                "(width : depth : height). List every feature: holes, slots, pockets, bosses, ribs, fillets, chamfers, threads, text, "
                "with their shape, count, position (from which edge or centre), spacing and symmetry. Copy every dimension, number and "
                "unit written in the picture exactly. If it is a technical drawing, read each view. If it is a screenshot of software, "
                "say what the screen shows. Estimate sizes as ratios when none are given. No chit chat, no guessing beyond what you see.";
            std::string err;
            std::string desc = describe(u.images, vsys, "The user's request about these pictures: \"" + req + "\"\n\nDescribe the picture(s) for the CAD assistant.", err);
            if (!err.empty()) return err;
            u.text += "\n\n[The user attached " + std::to_string(u.images.size()) + " picture(s). You cannot see them; " + visionModel +
                      " looked at them for you and describes them like this:]\n" + desc;
            u.images.clear();
            return {};
        };
        if (!history.empty() && history.back().role == "user") {
            std::string prob = lookAt(history.back());
            if (prob == "\x01") return;
            if (!prob.empty()) {
                post([this, prob] { ai_.busy = false; ai_.lines.push_back({3, prob}); });
                return;
            }
        }
        // look_at_design: renders the design here (off the main thread) and hands the picture to the model,
        // or to the helper with the model's question.
        auto lookAtDesign = [&](const std::string& argsJson, AiTurn& tr) {
            Value a = Value::parse(argsJson);
            std::vector<std::string> views;
            for (auto& v : a["views"].items()) views.push_back(lower(v.asString()));
            if (views.empty()) views = {"iso", "front", "top", "right"};
            if (views.size() > 8) views.resize(8);
            std::vector<std::string> bodies;
            for (auto& b : a["bodies"].items()) bodies.push_back(b.asString());
            std::string question = a["question"].asString();
            stage("Taking a picture of the design");
            auto prom = std::make_shared<std::promise<AiShotScene>>();
            auto fut = prom->get_future();
            post([this, prom, bodies] { prom->set_value(aiShotScene(bodies)); });
            AiShotScene sc;
            for (;;) {
                if (cancel->load()) { tr.text = "Stopped."; return; }
                if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) { sc = fut.get(); break; }
            }
            if (sc.items.empty()) { tr.text = "Nothing to look at: no visible bodies" + (sc.missing.empty() ? std::string(".") : " (not found: " + sc.missing + ")."); return; }
            // Views and their directions (from the model toward the camera) and screen up.
            auto dirFor = [&](const std::string& v, glm::vec3& eye, glm::vec3& up) {
                up = glm::vec3(0, 0, 1);
                if (v == "front") eye = {0, -1, 0};
                else if (v == "back") eye = {0, 1, 0};
                else if (v == "right") eye = {1, 0, 0};
                else if (v == "left") eye = {-1, 0, 0};
                else if (v == "top") { eye = {0, 0, 1}; up = {0, 1, 0}; }
                else if (v == "bottom") { eye = {0, 0, -1}; up = {0, 1, 0}; }
                else if (v == "current") { eye = -sc.camFwd; up = sc.camUp; }
                else eye = glm::normalize(glm::vec3(1, -1, 0.8f));   // iso, the home view
            };
            int n = (int)views.size();
            int cols = n == 1 ? 1 : n <= 4 ? 2 : n <= 6 ? 3 : 4, rows = (n + cols - 1) / cols;
            int pw = n == 1 ? 960 : 560, ph = n == 1 ? 720 : 480;
            int W = cols * pw + (cols - 1) * 4, H = rows * ph + (rows - 1) * 4;
            std::vector<uint8_t> sheet((size_t)W * H * 4, 255);
            for (size_t i = 0; i < sheet.size(); i += 4) { sheet[i] = 60; sheet[i + 1] = 62; sheet[i + 2] = 68; }   // gaps between panels
            for (int k = 0; k < n; ++k) {
                glm::vec3 eye, up;
                dirFor(views[(size_t)k], eye, up);
                auto px = thumb::renderView(sc.items, pw, ph, eye, up);
                int ox = (k % cols) * (pw + 4), oy = (k / cols) * (ph + 4);
                for (int y = 0; y < ph; ++y)
                    std::memcpy(&sheet[((size_t)(oy + y) * W + ox) * 4], &px[(size_t)y * pw * 4], (size_t)pw * 4);
            }
            if (const char* dump = std::getenv("KESTRAL_SHOT_DUMP")) thumb::writePng(dump, sheet, W, H);   // developer check
            AiImageBuilt built = aiBuildImage(sheet.data(), W, H);
            std::string order;
            for (auto& v : views) order += (order.empty() ? "" : ", ") + v;
            // Size of what is shown, so the picture has a scale.
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (auto& it : sc.items) {
                const auto& v = it.mesh->verts;
                for (size_t i = 0; i + 5 < v.size(); i += 6) {
                    glm::vec3 p = glm::vec3(it.xform * glm::vec4(v[i], v[i + 1], v[i + 2], 1.f));
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
            }
            char box[160];
            std::snprintf(box, sizeof box, "%d bodies, overall %.1f x %.1f x %.1f mm (X x Y x Z)", (int)sc.items.size(), hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
            std::string head = "Picture of the design: " + order + " (panels left to right, then top to bottom). " + box + ".";
            post([this, built, order]() mutable {
                aiImageTexture(built);
                AiLine ln{6, "Looked at the design: " + order};
                ln.images = {built.img};
                ai_.lines.push_back(ln);
                ai_.scroll = true;
            });
            if (mainSees()) {
                tr.text = head + " Compare it with what you meant to build.";
                tr.images = {built.img};
                return;
            }
            if (visionModel.empty() || visionModel == cfg.model) {
                tr.text = head + " You cannot see pictures and no picture helper is set in the assistant settings: check with get_design and "
                                 "measure instead.";
                return;
            }
            std::string vsys =
                "You are the eyes of a CAD assistant that cannot see. The picture shows the 3D model it is building in Kestral, rendered "
                "from the views listed (grey shaded solids, dark lines are the model's edges, axis sign: X red, Y green, Z blue). "
                "Answer its question first, plainly (yes or no and why when it asks one). Then describe what is actually there: the "
                "overall shape, every feature (holes, slots, chamfers, fillets, bosses, text) with its position and count, and anything "
                "that looks wrong, missing, floating, misaligned or not connected. Be precise and short.";
            std::string err;
            std::string desc = describe({built.img}, vsys, head + "\nThe assistant asks: " + (question.empty() ? "Does it look right? What is there?" : question), err);
            if (err == "\x01") { tr.text = "Stopped."; return; }
            if (!err.empty()) { tr.text = head + " " + err; return; }
            tr.text = head + " You cannot see it; " + visionModel + " looked at it for you:\n" + desc;
        };
        // Tools must see the model after the last change finished rebuilding (up to a minute).
        auto waitIdle = [&] {
            for (int k = 0; k < 600 && !cancel->load(); ++k) {
                auto p = std::make_shared<std::promise<bool>>();
                auto f = p->get_future();
                post([this, p] { p->set_value(recomputeQueuedFrom_ < 0 && !recomputeRunning_ && !recomputeResults_); });
                bool idle = false;
                while (!cancel->load()) {
                    if (f.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) { idle = f.get(); break; }
                }
                if (idle) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        };
        // No step limit unless the user set one: the model decides when it is done or stuck. Kestral
        // only points out when it keeps making the exact same call with the same result.
        int emptyReplies = 0, badCalls = 0, steps = 0;
        bool finished = false;
        std::string lastCallKey;
        int sameCalls = 0;
        // Messages the user typed while this runs go into the conversation before the next step.
        auto takeSteer = [&]() {
            std::vector<AiSteer> msgs;
            {
                std::lock_guard<std::mutex> lk(aiSteerMx_);
                msgs.swap(aiSteer_);
            }
            for (auto& m : msgs) {
                AiTurn u;
                u.role = "user";
                u.text = "[The user wrote this while you were working. Take it into account from now on:] " +
                         (m.text.empty() ? std::string("Model what this picture shows.") : m.text);
                u.images = m.images;
                std::string prob = lookAt(u);   // a picture for a model that cannot see: described first
                if (!prob.empty() && prob != "\x01") {
                    u.images.clear();
                    u.text += "\n[The user attached a picture here, but it could not be described.]";
                    post([this, prob] { ai_.lines.push_back({3, prob}); });
                }
                history.push_back(u);
            }
            return !msgs.empty();
        };
        for (;;) {
            if (cancel->load()) break;
            if (takeSteer() && smart)
                for (auto& t : history) if (t.role == "user") { auto more = aiPickTools(t.text); active.insert(more.begin(), more.end()); }
            if (maxSteps > 0 && steps >= maxSteps) {
                post([this, maxSteps] { ai_.lines.push_back({3, "Stopped after " + std::to_string(maxSteps) + " steps (the limit in the assistant settings)."}); });
                break;
            }
            ++steps;
            AiTurn reply;
            {
                std::lock_guard<std::mutex> lk(aiLiveMx_);
                aiLiveThink_.clear();
                aiLiveText_.clear();
                aiLiveStarted_ = false;
                aiLiveT0_ = glfwGetTime();
                aiLiveStage_ = "Waiting for the model";
            }
            auto onDelta = [this](const std::string& th, const std::string& tx) {
                {
                    std::lock_guard<std::mutex> lk(aiLiveMx_);
                    if (!aiLiveStarted_) {
                        aiLiveStarted_ = true;
                        log::info("Assistant: the model started answering after " + std::to_string((int)(glfwGetTime() - aiLiveT0_)) + " s");
                    }
                    aiLiveThink_ = th;
                    aiLiveText_ = tx;
                    aiLiveStage_ = tx.empty() ? "Thinking" : "Writing";
                }
                glfwPostEmptyEvent();   // wake the UI so the text shows as it arrives
            };
            {
                std::string note;
                if (compactHistory(cfg, history, system.size(), cancel.get(), note)) {
                    log::info("Assistant: " + note);
                    post([this, note, history] {
                        ai_.history = history;
                        ai_.lines.push_back({2, note});
                        activity();
                    });
                }
            }
            // Ollama, window on Automatic: just big enough for this conversation (a small window keeps the
            // whole model on the graphics card), or the one it is loaded with when that is big enough
            // (no reload).
            ProviderCfg c2 = cfg;
            const std::set<std::string>* toolSet = smart ? &active : nullptr;
            size_t toolsChars = toolsFor(cfg.kind, toolSet).dump().size();
            if (autoCtx) {
                size_t chars = system.size() + toolsChars;
                for (auto& t : history) { chars += t.text.size() + 40 + t.images.size() * 5600; for (auto& c : t.calls) chars += c.argsJson.size() + 30; }
                int need = (int)(chars / 3.2) + (cfg.thinking >= 2 ? 8192 : 4096);
                int pick = 8192;
                for (int sz : {8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072})
                    if (sz >= need) { pick = sz; break; } else pick = sz;
                if (modelMax > 0) pick = std::min(pick, modelMax);
                PsInfo ps = ollamaPs(cfg.url, cfg.model);
                if (ps.loaded && ps.ctx >= need && ps.ctx <= std::max(pick * 2, 16384)) pick = ps.ctx;   // already loaded with room: no reload
                c2.ctx = pick;
            }
            {
                std::lock_guard<std::mutex> lk(aiLiveMx_);
                aiCtxWindow_ = c2.ctx;
                aiPromptToolCount_ = smart ? (int)active.size() : (int)(sizeof kTools / sizeof kTools[0]);
            }
            CallStats stats;
            std::string err = callModel(c2, system, history, reply, cancel.get(), onDelta, toolSet, &stats);
            double thinkSecs = glfwGetTime() - aiLiveT0_;
            log::info("Assistant: reply after " + std::to_string((int)thinkSecs) + " s (" + std::to_string(stats.promptTok) + " tokens in, " +
                      std::to_string(stats.outTok) + " out, window " + std::to_string(c2.ctx) + ", " + std::to_string(toolSet ? toolSet->size() : 0) +
                      " tools), " + std::to_string(reply.thinking.size()) + " chars of thinking, " + std::to_string(reply.text.size()) +
                      " chars of text, " + std::to_string(reply.calls.size()) + " tool calls" + (err.empty() ? std::string() : ", error: " + err));
            {
                std::lock_guard<std::mutex> lk(aiLiveMx_);
                aiLiveThink_.clear();
                aiLiveText_.clear();
                if (stats.promptTok > 0) aiLastPromptTok_ = stats.promptTok;
                if (stats.outTok > 0) aiLastOutTok_ = stats.outTok;
                if (stats.genSecs > 0.05) aiLastOutRate_ = stats.outTok / stats.genSecs;
                if (stats.promptSecs > 0.05) aiLastInRate_ = stats.promptTok / stats.promptSecs;
            }
            if (!reply.thinking.empty()) {
                size_t f = reply.thinking.find_first_not_of(" \n\r\t");
                std::string th = f == std::string::npos ? std::string() : reply.thinking.substr(f);
                while (!th.empty() && std::isspace((unsigned char)th.back())) th.pop_back();
                if (!th.empty())
                    post([this, th, thinkSecs] {
                        aiLog("thinking", th);
                        ai_.lines.push_back({4, th, thinkSecs});
                        ai_.scroll = true;
                    });
            }
            if (cancel->load()) break;
            if (!err.empty() && err.find("error parsing tool call") != std::string::npos && ++badCalls <= 3) {
                // The model wrote a tool call that is not valid JSON (Ollama refuses it). Tell it and let it try again.
                size_t rp = err.find("raw='");
                std::string raw = rp == std::string::npos ? std::string() : err.substr(rp + 5, std::min<size_t>(400, err.size() - rp - 5));
                size_t q = raw.rfind("', err=");
                if (q != std::string::npos) raw.resize(q);
                AiTurn nudge;
                nudge.role = "user";
                nudge.text = "(Kestral: your last tool call was not valid JSON, so it did not run: " + raw +
                             " Send that call again with valid JSON.)";
                history.push_back(nudge);
                post([this] { aiLog("kestral", "the model sent a broken tool call, asked again"); });
                continue;
            }
            if (!err.empty()) {
                post([this, err] { aiLog("error", err); ai_.lines.push_back({3, err}); ai_.busy = false; ai_.scroll = true; });
                return;
            }
            if (reply.text.empty() && reply.calls.empty()) {
                // Some local models now and then answer with nothing at all. Ask once more
                // instead of stopping silently.
                if (++emptyReplies <= 2) {
                    AiTurn nudge;
                    nudge.role = "user";
                    nudge.text = "(Kestral: your last answer was empty. Continue: call the next tool, or tell the user the result.)";
                    history.push_back(nudge);
                    post([this] { aiLog("kestral", "empty answer from the model, asked again"); });
                    continue;
                }
                post([this] { aiLog("error", "empty answers"); ai_.lines.push_back({3, "The model keeps giving empty answers. Try again, or try another model."}); });
                finished = true;
                break;
            }
            emptyReplies = 0;
            history.push_back(reply);
            {
                std::string calls;
                for (auto& c : reply.calls) calls += "\n  call " + c.name + " " + c.argsJson;
                post([this, t = reply.text + calls] { aiLog("assistant", t); });
            }
            if (!reply.text.empty()) {
                std::string t = reply.text;
                post([this, t] { ai_.lines.push_back({1, t}); ai_.scroll = true; });
            }
            if (reply.calls.empty()) {
                if (takeSteer()) continue;   // the user added something: answer that too
                finished = true;
                break;
            }
            for (auto& c : reply.calls) {
                stage("Running " + c.name);
                if (smart && !active.count(c.name) && toolExists(c.name)) active.insert(c.name);   // it knew the name: fine
                if (c.name == "find_tools") {
                    // Answered right here: the tools found are loaded for the next step.
                    std::string q = Value::parse(c.argsJson)["query"].asString();
                    auto p2 = std::make_shared<std::promise<std::string>>();
                    auto f2 = p2->get_future();
                    std::set<std::string> act = active;
                    post([this, p2, q, act]() mutable {
                        std::string r = aiFindTools(q, act);
                        p2->set_value(r);
                    });
                    std::string result;
                    while (!cancel->load())
                        if (f2.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) { result = f2.get(); break; }
                    // The tools it listed are loaded for the next step.
                    for (auto& t : kTools)
                        if (result.find(std::string("- ") + t.name + ":") != std::string::npos) active.insert(t.name);
                    post([this, q, result] { aiLog("tool find_tools", q + " -> " + result); ai_.lines.push_back({2, "Looked up tools for \"" + q + "\""}); });
                    AiTurn tr;
                    tr.role = "tool";
                    tr.callId = c.id;
                    tr.toolName = c.name;
                    tr.text = result;
                    history.push_back(tr);
                    continue;
                }
                if (c.name == "look_at_design") {
                    waitIdle();   // the picture shows the design after the last change finished
                    if (cancel->load()) break;
                    AiTurn tr;
                    tr.role = "tool";
                    tr.callId = c.id;
                    tr.toolName = c.name;
                    std::string perr;
                    if (!platform::guarded("look at design", [&] { lookAtDesign(c.argsJson, tr); }, perr)) tr.text = "Error: " + perr;
                    if (cancel->load()) break;
                    post([this, r = tr.text, n = (int)tr.images.size()] { aiLog("tool look_at_design", r + (n ? " [picture]" : "")); });
                    history.push_back(tr);
                    continue;
                }
                if (aiIsPrintTool(c.name)) {
                    // Slicing and printer talk run right here (they take a while); they reach the
                    // main thread themselves when they need to.
                    std::string result, perr;
                    if (!platform::guarded("print tool", [&] { result = aiPrintTool(c.name, c.argsJson, post, cancel); }, perr))
                        result = "Error: " + perr;
                    if (cancel->load()) break;
                    if (c.name != "print_setup" && c.name != "list_print_profiles")
                        post([this, result] { ai_.lines.push_back({2, result}); ai_.scroll = true; });
                    post([this, n = c.name, result] { aiLog("tool " + n, result); });
                    AiTurn tr;
                    tr.role = "tool";
                    tr.callId = c.id;
                    tr.toolName = c.name;
                    tr.text = result;
                    history.push_back(tr);
                    continue;
                }
                // Tools touch the document: run them on the main thread and wait for the answer.
                auto prom = std::make_shared<std::promise<std::string>>();
                auto fut = prom->get_future();
                std::string nm = c.name, args = c.argsJson;
                waitIdle();
                if (cancel->load()) break;
                auto before = std::make_shared<std::set<uint32_t>>();
                post([this, prom, nm, args, before] {
                    for (auto& [id, b] : doc_.bodies) if (doc_.isBodyAlive(b)) before->insert(id);
                    aiCaptured_.clear();
                    aiCapture_ = true;
                    std::string res, perr;
                    if (!platform::guarded("assistant tool", [&] { res = aiRunTool(nm, args); }, perr)) res = "Error: " + perr;
                    if (nm != "get_design") ai_.lines.push_back({2, res});
                    ai_.scroll = true;
                    prom->set_value(res);
                });
                std::string result;
                while (!cancel->load()) {
                    if (fut.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) { result = fut.get(); break; }
                }
                if (cancel->load()) break;
                if (nm != "get_design" && nm != "select") {
                    // Let the model rebuild, then tell the AI the exact names of anything new so its
                    // next step can refer to it, and what Kestral said meanwhile (errors, hints).
                    stage("Rebuilding the design");
                    waitIdle();
                    auto p2 = std::make_shared<std::promise<std::string>>();
                    auto f2 = p2->get_future();
                    post([this, p2, before] {
                        std::string made;
                        for (auto& [id, b] : doc_.bodies)
                            if (doc_.isBodyAlive(b) && !before->count(id)) made += (made.empty() ? "" : ", ") + aiBodyPath(id);
                        // Failed timeline steps count too.
                        std::string failed;
                        for (auto& f : doc_.timeline)
                            if (f.failed && !f.error.empty()) failed += (failed.empty() ? "" : "; ") + f.name + ": " + f.error;
                        std::string said;
                        for (auto& m : aiCaptured_) said += (said.empty() ? "" : " | ") + m;
                        aiCapture_ = false;
                        aiCaptured_.clear();
                        std::string out;
                        if (!made.empty()) out += " Bodies now in the design from this step: " + made + ".";
                        if (!said.empty()) out += " Kestral showed the user: \"" + said + "\".";
                        if (!failed.empty()) out += " Timeline steps with errors: " + failed + ".";
                        p2->set_value(out);
                    });
                    while (!cancel->load()) {
                        if (f2.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) {
                            result += f2.get();
                            break;
                        }
                    }
                }
                if (nm == "get_design" || nm == "select") post([this] { aiCapture_ = false; aiCaptured_.clear(); });
                post([this, nm, result] { aiLog("tool " + nm, result); });
                std::string key = nm + "|" + args + "|" + result;
                sameCalls = key == lastCallKey ? sameCalls + 1 : 0;
                lastCallKey = key;
                if (sameCalls >= 2)
                    result += " (Kestral: this exact call has now given this exact result " + std::to_string(sameCalls + 1) +
                              " times. If you are stuck, stop and tell the user what is blocking you.)";
                if (c.name == "run_command")   // pointed to a dedicated tool: load it
                    for (auto& t : kTools)
                        if (result.find(std::string("`") + t.name + "`") != std::string::npos) active.insert(t.name);
                AiTurn tr;
                tr.role = "tool";
                tr.callId = c.id;
                tr.toolName = c.name;
                tr.text = result;
                history.push_back(tr);
            }
        }
        (void)finished;
        post([this, history, cancel, active, smart] {
            if (smart) aiActiveTools_ = active;
            if (!cancel->load()) ai_.history = history;
            ai_.busy = false;
            ai_.scroll = true;
            // A message that came in right as it finished: start on it now.
            std::vector<AiSteer> late;
            {
                std::lock_guard<std::mutex> lk(aiSteerMx_);
                late.swap(aiSteer_);
            }
            if (!late.empty() && !cancel->load()) {
                std::string t;
                std::vector<std::shared_ptr<const AiImage>> pics;
                for (auto& m : late) {
                    if (!m.text.empty()) t += (t.empty() ? "" : "\n") + m.text;
                    pics.insert(pics.end(), m.images.begin(), m.images.end());
                }
                aiSend(t, false, pics);   // already shown in the chat when it was typed
                return;
            }
            if (!cancel->load()) {
                std::string last;
                for (auto it = history.rbegin(); it != history.rend(); ++it)
                    if (it->role == "assistant" && !it->text.empty()) { last = it->text; break; }
                notifyAssistantDone(last);
            }
        });
    }, [this](const std::string& e) {
        ai_.lines.push_back({3, "Assistant error: " + e});
        ai_.busy = false;
    });
}

// ===================================================================== model list

void App::aiFetchModels() {
    ProviderCfg cfg = resolveProvider(settings_, settings_.aiProvider).cfg;
    ai_.fetching = true;
    ai_.fetchMsg = "Asking the service for its models...";
    JobSystem* jobs = jobs_.get();
    jobs_->submit("model list", [this, jobs, cfg] {
        std::vector<std::pair<std::string, std::string>> headers;
        if (cfg.kind == 0) headers = {{"x-api-key", cfg.key}, {"anthropic-version", "2023-06-01"}};
        else if (!cfg.key.empty()) headers = {{"Authorization", "Bearer " + cfg.key}};
        auto r = platform::httpGet(endpoint(cfg.kind, cfg.url, "models"), headers, 20000, nullptr);
        std::vector<std::string> names;
        std::string msg;
        if (!r.error.empty()) msg = r.error;
        else if (r.status < 200 || r.status >= 300) msg = "The service said " + std::to_string(r.status) + (r.status == 401 ? " (check the API key)" : "");
        else {
            Value v = Value::parse(r.body);
            const Value& arr = cfg.kind == 1 ? v["models"] : v["data"];
            for (auto& m : arr.items()) {
                std::string n = cfg.kind == 1 ? m["name"].asString() : m["id"].asString();
                if (n.rfind("models/", 0) == 0) n = n.substr(7);   // Gemini
                if (!n.empty()) names.push_back(n);
            }
            std::sort(names.begin(), names.end());
            msg = names.empty() ? "No models found" : std::to_string(names.size()) + " models";
        }
        // Ollama says which models can see pictures.
        std::set<std::string> seeing;
        if (cfg.kind == 1) {
            int answered = 0;
            for (size_t i = 0; i < names.size() && i < 80; ++i) {
                auto caps = ollamaCaps(cfg.url, names[i]);
                answered += caps.count("answered") ? 1 : 0;
                if (caps.count("vision")) seeing.insert(names[i]);
            }
            std::string list;
            for (auto& m : seeing) list += (list.empty() ? "" : ", ") + m;
            log::info("Assistant: Ollama described " + std::to_string(answered) + " of " + std::to_string(names.size()) + " models; these see pictures: " +
                      (list.empty() ? "none" : list));
        }
        jobs->post([this, names, msg, seeing] {
            ai_.visionModels = seeing;
            ai_.models = names;
            ai_.fetchMsg = msg;
            ai_.fetching = false;
        });
    }, [this](const std::string& e) {
        ai_.fetching = false;
        ai_.fetchMsg = e;
    });
}

// ===================================================================== model info (context window)

void App::aiFetchModelInfo() {
    ProviderCfg cfg = resolveProvider(settings_, settings_.aiProvider).cfg;
    std::string key = settings_.aiProvider + "|" + cfg.model;
    ai_.ctxFor = key;
    ai_.ctxMax = typicalContext(cfg.model);
    ai_.ctxReported = false;
    if (cfg.model.empty() || cfg.url.empty()) return;
    ai_.ctxFetching = true;
    JobSystem* jobs = jobs_.get();
    std::string provider = settings_.aiProvider;
    jobs_->submit("model info", [this, jobs, cfg, key, provider] {
        int found = 0;
        auto numberIn = [](const Value& v, std::initializer_list<const char*> names) {
            for (auto n : names)
                if (v[n].isNumber() && v[n].asNumber() > 0) return (int)v[n].asNumber();
            return 0;
        };
        try {
            if (cfg.kind == 1) {
                // Ollama: /api/show has "<arch>.context_length" in model_info.
                Value b = Value::object();
                b.set("model", cfg.model);
                auto r = platform::httpPost(trimSlash(cfg.url) + "/api/show", {}, b.dump(), 15000, nullptr);
                Value v = Value::parse(r.body);
                for (auto& [k, val] : v["model_info"].members())
                    if (k.size() > 15 && k.compare(k.size() - 15, 15, ".context_length") == 0 && val.isNumber()) found = (int)val.asNumber();
                if (v["capabilities"].isArray()) {   // older Ollama versions do not say
                    bool sees = false;
                    for (auto& c : v["capabilities"].items()) sees |= c.asString() == "vision";
                    jobs->post([this, key, sees] { ai_.visionFor = key; ai_.mainVision = sees ? 1 : 0; });
                }
            } else if (provider == "gemini") {
                // Google's native API says inputTokenLimit.
                std::string m = cfg.model.rfind("models/", 0) == 0 ? cfg.model : "models/" + cfg.model;
                auto r = platform::httpGet("https://generativelanguage.googleapis.com/v1beta/" + m, {{"x-goog-api-key", cfg.key}}, 15000, nullptr);
                found = numberIn(Value::parse(r.body), {"inputTokenLimit"});
            } else if (cfg.kind == 2) {
                std::vector<std::pair<std::string, std::string>> hd;
                if (!cfg.key.empty()) hd = {{"Authorization", "Bearer " + cfg.key}};
                // LM Studio's own API knows the loaded model's window.
                std::string base = trimSlash(cfg.url);
                if (base.size() > 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
                    auto r = platform::httpGet(base.substr(0, base.size() - 3) + "/api/v0/models/" + cfg.model, hd, 8000, nullptr);
                    if (r.status == 200) found = numberIn(Value::parse(r.body), {"max_context_length", "loaded_context_length"});
                }
                if (!found) {
                    // Many services list it with their models (OpenRouter, Groq, Mistral, vLLM...).
                    auto r = platform::httpGet(endpoint(2, cfg.url, "models"), hd, 15000, nullptr);
                    Value v = Value::parse(r.body);
                    for (auto& m : v["data"].items())
                        if (m["id"].asString() == cfg.model) {
                            found = numberIn(m, {"context_length", "context_window", "max_context_length", "max_model_len", "max_input_tokens"});
                            if (!found) found = numberIn(m["top_provider"], {"context_length"});
                        }
                }
            } else if (cfg.kind == 0) {
                auto r = platform::httpGet(endpoint(0, cfg.url, ("models/" + cfg.model).c_str()),
                                           {{"x-api-key", cfg.key}, {"anthropic-version", "2023-06-01"}}, 15000, nullptr);
                found = numberIn(Value::parse(r.body), {"max_input_tokens", "context_window"});
            }
        } catch (...) {}
        jobs->post([this, key, found] {
            ai_.ctxFetching = false;
            if (ai_.ctxFor != key) return;
            if (found > 0) { ai_.ctxMax = found; ai_.ctxReported = true; }
        });
    }, [this](const std::string&) { ai_.ctxFetching = false; });
}

// ===================================================================== panel

// ---------------------------------------------------------------- chat look

namespace {

// Light markdown for replies: **bold**, `code`, # headings, - bullets, 1. lists, ``` code blocks.
// Words are laid out by hand so bold and code can sit inside a wrapped line.
void drawMarkdown(const std::string& text, float width, ImFont* bold, ImU32 textCol, float S) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float y = 0, lineH = ImGui::GetTextLineHeight();
    float fs = ImGui::GetFontSize();
    ImFont* reg = ImGui::GetFont();
    ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    std::istringstream in(text);
    std::string line;
    bool inCode = false;
    auto runLine = [&](const std::string& src, float indent, bool heading, float scale) {
        // Split into styled words.
        struct Word { std::string t; bool b, code; };
        std::vector<Word> words;
        bool b = heading, code = false;
        std::string cur;
        auto push = [&] { if (!cur.empty()) { words.push_back({cur, b, code}); cur.clear(); } };
        for (size_t i = 0; i < src.size(); ++i) {
            if (!code && src.compare(i, 2, "**") == 0) { push(); b = !b || heading; if (heading) b = true; ++i; continue; }
            if (src[i] == '`') { push(); code = !code; continue; }
            if (src[i] == ' ') { cur += ' '; push(); continue; }
            cur += src[i];
        }
        push();
        float x = indent;
        float lh = lineH * scale;
        for (auto& w : words) {
            ImFont* f = w.b ? bold : reg;
            float sz = fs * scale;
            ImVec2 ws = f->CalcTextSizeA(sz, FLT_MAX, 0.f, w.t.c_str());
            if (x > indent && x + ws.x > width) { x = indent; y += lh + 2 * S; }
            ImVec2 at(origin.x + x, origin.y + y);
            if (w.code) {
                std::string t = w.t;
                while (!t.empty() && t.back() == ' ') t.pop_back();
                ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0.f, t.c_str());
                dl->AddRectFilled(ImVec2(at.x - 2 * S, at.y), ImVec2(at.x + ts.x + 2 * S, at.y + lh), IM_COL32(128, 128, 128, 45), 3 * S);
                dl->AddText(f, sz, at, sl::orangeU(), t.c_str());
            } else {
                dl->AddText(f, sz, at, textCol, w.t.c_str());
            }
            x += ws.x;
        }
        y += lh + 4 * S;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("```", 0) == 0) {
            inCode = !inCode;
            y += 2 * S;
            continue;
        }
        if (inCode) {
            dl->AddRectFilled(ImVec2(origin.x, origin.y + y - 1 * S), ImVec2(origin.x + width, origin.y + y + lineH + 1 * S), IM_COL32(128, 128, 128, 30));
            dl->AddText(reg, fs, ImVec2(origin.x + 6 * S, origin.y + y), dim, line.c_str());
            y += lineH + 2 * S;
            continue;
        }
        size_t lead = line.find_first_not_of(' ');
        if (lead == std::string::npos) { y += lineH * 0.5f; continue; }
        std::string t = line.substr(lead);
        float ind = std::min<float>(lead, 8) * 3 * S;
        if (t.rfind("### ", 0) == 0 || t.rfind("## ", 0) == 0 || t.rfind("# ", 0) == 0) {
            size_t sp = t.find(' ');
            y += 3 * S;
            runLine(t.substr(sp + 1), ind, true, t[1] == '#' ? 1.05f : 1.15f);
        } else if (t.rfind("- ", 0) == 0 || t.rfind("* ", 0) == 0) {
            dl->AddCircleFilled(ImVec2(origin.x + ind + 6 * S, origin.y + y + lineH * 0.5f), 2.5f * S, textCol);
            runLine(t.substr(2), ind + 16 * S, false, 1.f);
        } else if (t.size() > 2 && std::isdigit((unsigned char)t[0]) && (t[1] == '.' || (std::isdigit((unsigned char)t[1]) && t.size() > 3 && t[2] == '.'))) {
            size_t dot = t.find('.');
            std::string num = t.substr(0, dot + 1);
            dl->AddText(bold, fs, ImVec2(origin.x + ind, origin.y + y), sl::orangeU(), num.c_str());
            runLine(t.substr(dot + 1 < t.size() && t[dot + 1] == ' ' ? dot + 2 : dot + 1), ind + 20 * S, false, 1.f);
        } else {
            runLine(t, ind, false, 1.f);
        }
    }
    ImGui::Dummy(ImVec2(width, std::max(0.f, y - 4 * S)));
}

std::string secsText(double s) {
    char b[32];
    if (s < 60) std::snprintf(b, sizeof b, "%.0f s", s);
    else std::snprintf(b, sizeof b, "%d min %02d s", (int)(s / 60), (int)s % 60);
    return b;
}

std::string kTok(int t) {
    char b[32];
    if (t >= 1000) std::snprintf(b, sizeof b, "%.1fK", t / 1000.0);
    else std::snprintf(b, sizeof b, "%d", t);
    return b;
}

} // namespace

void App::drawAssistant() {
    drawPrintConfirm();
    if (!ai_.open) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 490 * S, vp->Pos.y + 130 * S), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470 * S, 680 * S), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360 * S, 420 * S), ImVec2(1400 * S, 2000 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * S);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Panel));
    bool visible = ImGui::Begin("Assistant##panel", &ai_.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!visible) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ResolvedProvider cur = resolveProvider(settings_, settings_.aiProvider);
    const float pad = 14 * S;

    // ---- header: logo, name, model chip, new chat, settings, close
    float hh = 58 * S;
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + hh), col::Card, 12 * S, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(wp.x, wp.y + hh), ImVec2(wp.x + ws.x, wp.y + hh), col::PanelBorder);
    ui::drawLogo(dl, ImVec2(wp.x + pad, wp.y + 13 * S), 32 * S);
    dl->AddText(fonts_.bold, ImGui::GetFontSize() * 1.1f, ImVec2(wp.x + pad + 42 * S, wp.y + 10 * S), col::Text, "Assistant");
    {
        // The model chip: status dot and name; click to switch provider or model.
        std::string chip = cur.cfg.model.empty() ? cur.label : cur.cfg.model;
        if (chip.size() > 34) chip = chip.substr(0, 16) + "..." + chip.substr(chip.size() - 14);
        ImVec2 cp(wp.x + pad + 42 * S, wp.y + 32 * S);
        ImVec2 csz = ImGui::CalcTextSize(chip.c_str());
        ImGui::SetCursorScreenPos(ImVec2(cp.x - 4 * S, cp.y - 1 * S));
        if (ImGui::InvisibleButton("##modelchip", ImVec2(csz.x + 26 * S, csz.y + 4 * S))) ImGui::OpenPopup("##modelmenu");
        bool hv = ImGui::IsItemHovered();
        if (hv) dl->AddRectFilled(ImVec2(cp.x - 4 * S, cp.y - 1 * S), ImVec2(cp.x + csz.x + 22 * S, cp.y + csz.y + 3 * S), col::Hover, 4 * S);
        ImU32 dot = ai_.busy ? sl::orangeU() : IM_COL32(70, 190, 110, 255);
        dl->AddCircleFilled(ImVec2(cp.x + 4 * S, cp.y + csz.y * 0.5f + 1 * S), 3.5f * S, dot);
        dl->AddText(ImVec2(cp.x + 12 * S, cp.y), col::TextDim, chip.c_str());
        if (hv) ImGui::SetTooltip("%s  |  %s\nClick to switch", cur.label.c_str(), cur.cfg.model.c_str());
        ImGui::SetNextWindowSizeConstraints(ImVec2(280 * S, 0), ImVec2(460 * S, 520 * S));
        if (ImGui::BeginPopup("##modelmenu")) {
            sl::menuHeader("PROVIDER");
            for (auto& pr : kPresets)
                if (sl::menuRow(pr.label, settings_.aiProvider == pr.id)) { settings_.aiProvider = pr.id; ai_.models.clear(); settings_.save(settingsPath_); }
            for (auto& [id, sp] : settings_.ai)
                if (id.rfind("custom", 0) == 0 && sl::menuRow((sp.name.empty() ? id : sp.name).c_str(), settings_.aiProvider == id, "custom")) { settings_.aiProvider = id; settings_.save(settingsPath_); }
            if (!ai_.models.empty()) {
                sl::menuHeader("MODEL");
                for (auto& m : ai_.models)
                    if (sl::menuRow(m.c_str(), m == cur.cfg.model)) { settings_.ai[settings_.aiProvider].model = m; settings_.save(settingsPath_); }
            } else if (sl::menuRow("Show this provider's models", false)) {
                aiFetchModels();
            }
            ImGui::Separator();
            if (sl::menuRow("Assistant settings...", false)) { settingsOpen_ = true; prefSection_ = 5; }
            ImGui::EndPopup();
        }
    }
    auto headBtn = [&](const char* id, sl::OIcon ic, float x, const char* tip) {
        float bs = 30 * S;
        ImVec2 p(x, wp.y + (hh - bs) * 0.5f);
        ImGui::SetCursorScreenPos(p);
        bool c = ImGui::InvisibleButton(id, ImVec2(bs, bs));
        bool hv = ImGui::IsItemHovered();
        if (hv) { dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + bs), col::Hover, 7 * S); ImGui::SetTooltip("%s", tip); }
        sl::drawOIcon(dl, ImVec2(p.x + bs * 0.5f, p.y + bs * 0.5f), 15 * S, ic, col::Text);
        return c;
    };
    float bx = wp.x + ws.x - pad - 30 * S;
    if (headBtn("##aiclose", sl::OIcon::Close, bx, "Close")) ai_.open = false;
    bx -= 34 * S;
    if (headBtn("##aiset", sl::OIcon::Gear, bx, "Assistant settings")) { settingsOpen_ = true; prefSection_ = 5; }
    bx -= 34 * S;
    if (headBtn("##ainew", sl::OIcon::Plus, bx, "New chat") && !ai_.busy) aiClearChat();
    // The context meter: how full the window was on the last step.
    {
        int win = aiCtxWindow_ > 0 ? aiCtxWindow_ : (cur.cfg.ctx > 0 ? cur.cfg.ctx : ai_.ctxMax);
        float frac = win > 0 && aiLastPromptTok_ > 0 ? std::min(1.f, (float)(aiLastPromptTok_ + aiLastOutTok_) / (float)win) : 0.f;
        ImVec2 m0(wp.x, wp.y + hh - 2 * S);
        dl->AddRectFilled(m0, ImVec2(m0.x + ws.x * frac, m0.y + 2 * S), frac > 0.85f ? col::Error : sl::orangeU(160));
        ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + hh - 6 * S));
        ImGui::InvisibleButton("##ctxmeter", ImVec2(ws.x, 6 * S));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Last step: %s tokens read, %s written%s\nContext window: %s tokens%s\nTools sent: %d", kTok(aiLastPromptTok_).c_str(),
                              kTok(aiLastOutTok_).c_str(), aiLastOutRate_ > 0 ? (" (" + std::to_string((int)aiLastOutRate_) + " tokens/s)").c_str() : "",
                              win > 0 ? kTok(win).c_str() : "?", cur.cfg.kind == 1 && cur.cfg.ctx == 0 ? " (automatic)" : "", aiPromptToolCount_);
    }

    // ---- conversation
    float inputLines = 1;
    {
        float wrapW = ws.x - 2 * pad - 96 * S;
        float th = ImGui::CalcTextSize(ai_.input.empty() ? "x" : ai_.input.c_str(), nullptr, false, wrapW).y;
        inputLines = std::clamp(std::round(th / ImGui::GetTextLineHeight()), 1.f, 7.f);
    }
    float stripH = aiAttach_.empty() ? 0.f : 74 * S;   // pictures waiting to be sent
    float inputH = inputLines * ImGui::GetTextLineHeight() + 54 * S + stripH;
    float footerH = inputH + 34 * S;
    ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + hh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 12 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * S, 10 * S));
    ImGui::BeginChild("##ailog", ImVec2(ws.x, ws.y - hh - footerH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    {
        // Following the newest message unless you scrolled up to read: then new messages wait below
        // and a button shows how many came in.
        float sy = ImGui::GetScrollY(), smax = ImGui::GetScrollMaxY();
        bool atBottom = sy >= smax - 30 * S;
        if (aiFollow_ && sy < aiLastScrollY_ - 1.f && !atBottom) aiFollow_ = false;   // you scrolled up
        if (!aiFollow_ && atBottom && sy > aiLastScrollY_ + 0.5f) aiFollow_ = true;    // and back down to the end
        if (aiFollow_) aiSeenLines_ = (int)ai_.lines.size();
        if ((int)ai_.lines.size() < aiSeenLines_) aiSeenLines_ = (int)ai_.lines.size();   // a new chat
    }
    float cw = ImGui::GetContentRegionAvail().x;
    ImDrawList* cl = ImGui::GetWindowDrawList();
    if (ai_.lines.empty() && !ai_.busy) {
        // Empty chat: a greeting and ideas for this page.
        ImGui::Dummy(ImVec2(0, 30 * S));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ui::drawLogo(cl, ImVec2(p.x + cw * 0.5f - 26 * S, p.y), 52 * S);
        ImGui::Dummy(ImVec2(0, 60 * S));
        const char* hi = "How can I help?";
        ImGui::PushFont(fonts_.bold, ImGui::GetFontSize() * 1.35f);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(hi).x) * 0.5f);
        ImGui::TextUnformatted(hi);
        ImGui::PopFont();
        const char* sub = sliceWs_ ? "I can change any slice setting, arrange plates, pick filaments and AMS slots, and print."
                                   : "I can build and change your design, answer how-to questions, slice and print.";
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        float sw = std::min(cw, ImGui::CalcTextSize(sub).x);
        ImGui::SetCursorPosX(pad + (cw - sw) * 0.5f);
        ImGui::TextWrapped("%s", sub);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6 * S));
        std::vector<const char*> ideas = sliceWs_ ? std::vector<const char*>{"Make this print stronger", "Use 0.12 mm layers and a brim", "Put every part on its own plate",
                                                                             "Which AMS slot has the white PLA?", "Slice and print plate 1"}
                                                  : std::vector<const char*>{"Make a 40 mm cube with 3 mm fillets", "Hollow the selected body with 2 mm walls",
                                                                             "Make the frame red", "How do I add a thread?", "Slice this and print it"};
        for (auto* idea : ideas) {
            ImVec2 ts = ImGui::CalcTextSize(idea);
            float bw = ts.x + 26 * S, bh = ts.y + 14 * S;
            ImGui::SetCursorPosX(pad + (cw - bw) * 0.5f);
            ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::PushID(idea);
            if (ImGui::InvisibleButton("##idea", ImVec2(bw, bh))) { ai_.input = idea; ai_.focus = true; }
            bool hv = ImGui::IsItemHovered();
            ImGui::PopID();
            cl->AddRectFilled(q, ImVec2(q.x + bw, q.y + bh), hv ? col::Hover : col::Card, bh * 0.5f);
            cl->AddRect(q, ImVec2(q.x + bw, q.y + bh), hv ? sl::orangeU() : col::PanelBorder, bh * 0.5f);
            cl->AddText(ImVec2(q.x + 13 * S, q.y + 7 * S), col::Text, idea);
        }
    }
    // A fold out "Thought for ..." block (live: the newest lines stay in view).
    auto thinkingBlock = [&](const char* id, const std::string& text, const std::string& title, bool live, bool* open) {
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (open) ImGui::SetNextItemOpen(*open);
        bool isOpen = ImGui::TreeNodeEx("##think", fl, "%s", title.c_str());
        if (open) *open = isOpen;
        if (isOpen || (live && !text.empty())) {
            const ImGuiStyle& st = ImGui::GetStyle();
            float lineH = ImGui::GetTextLineHeight();
            float boxW = ImGui::GetContentRegionAvail().x;
            float wrapW = std::max(40.f, boxW - st.WindowPadding.x * 2 - st.ScrollbarSize);
            float textH = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(), false, wrapW).y;
            int lines = isOpen ? 14 : 3;
            float viewH = std::min(textH, lineH * lines);
            float boxH = viewH + st.WindowPadding.y * 2;
            ImGui::SetNextWindowContentSize(ImVec2(0.f, textH));
            if (live && aiLiveFollow_) ImGui::SetNextWindowScroll(ImVec2(0.f, std::max(0.f, textH - viewH)));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(col::Card));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * S);
            ImGui::BeginChild(isOpen ? "##thinkbox" : "##thinktail", ImVec2(boxW, boxH), ImGuiChildFlags_AlwaysUseWindowPadding,
                              isOpen ? ImGuiWindowFlags_None : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
            ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
            ImGui::PopTextWrapPos();
            if (live && isOpen) aiLiveFollow_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - lineH * 1.5f;
            else if (live) aiLiveFollow_ = true;
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    };
    auto words = [](const std::string& t) {
        size_t n = 0;
        bool in = false;
        for (char ch : t) {
            bool sp = std::isspace((unsigned char)ch) != 0;
            if (!sp && !in) ++n;
            in = !sp;
        }
        return n;
    };
    int lineNo = 0;
    int lastError = -1;
    for (int i = 0; i < (int)ai_.lines.size(); ++i) if (ai_.lines[(size_t)i].kind == 3) lastError = i;
    for (auto& l : ai_.lines) {
        ++lineNo;
        ImGui::PushID(lineNo);
        if (l.kind == 4) {
            if (settings_.aiShowThinking) thinkingBlock("t", l.text, "Thought for " + secsText(l.secs) + " (" + std::to_string(words(l.text)) + " words)", false, nullptr);
        } else if (l.kind == 5) {
            // What the picture helper saw: a card with its description (the chat model works from it).
            size_t nl = l.text.find('\n');
            std::string who = nl == std::string::npos ? l.text : l.text.substr(0, nl);
            std::string desc = nl == std::string::npos ? std::string() : l.text.substr(nl + 1);
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImDrawListSplitter split;
            split.Split(cl, 2);
            split.SetCurrentChannel(cl, 1);
            ImGui::SetCursorScreenPos(ImVec2(a.x + 12 * S, a.y + 10 * S));
            ImGui::BeginGroup();
            {
                // Eye icon, then who looked and how long it took.
                ImVec2 ic = ImGui::GetCursorScreenPos();
                float r = ImGui::GetTextLineHeight() * 0.5f;
                ImVec2 c(ic.x + 8 * S, ic.y + r);
                cl->PathEllipticalArcTo(c, ImVec2(8 * S, 4.5f * S), 0.f, 0.f, 6.2832f, 24);
                cl->PathStroke(sl::orangeU(), ImDrawFlags_Closed, 1.5f * S);
                cl->AddCircleFilled(c, 2.4f * S, sl::orangeU());
                ImGui::SetCursorScreenPos(ImVec2(ic.x + 22 * S, ic.y));
                ImGui::PushFont(fonts_.bold, 0.f);
                ImGui::Text("What %s sees", who.c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", secsText(l.secs).c_str());
            }
            ImGui::PushID("desc");
            bool isOpen = !l.folded;
            if (isOpen) {
                ImGui::Dummy(ImVec2(0, 2 * S));
                drawMarkdown(desc, cw - 24 * S, fonts_.bold, col::Text, S);
            }
            ImGui::PopID();
            ImGui::EndGroup();
            ImVec2 b = ImGui::GetItemRectMax();
            float bottom = b.y + 10 * S;
            split.SetCurrentChannel(cl, 0);
            cl->AddRectFilled(a, ImVec2(a.x + cw, bottom), col::Card, 10 * S);
            cl->AddRect(a, ImVec2(a.x + cw, bottom), sl::orangeU(110), 10 * S);
            split.Merge(cl);
            // Click the title row to fold or unfold it; right-click to copy.
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton("##desccard", ImVec2(cw, ImGui::GetTextLineHeight() + 14 * S));
            if (ImGui::IsItemClicked()) l.folded = isOpen;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(isOpen ? "Click to fold. Right-click to copy." : "Click to show the description");
            if (ImGui::BeginPopupContextItem("##descctx")) {
                if (ImGui::MenuItem("Copy description")) ImGui::SetClipboardText(desc.c_str());
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x, bottom));
            ImGui::Dummy(ImVec2(0, 0));
        } else if (l.kind == 6) {
            // A picture the assistant took of the design (look_at_design): what it looked at, hover to enlarge.
            ImVec2 a = ImGui::GetCursorScreenPos();
            float th = ImGui::GetTextLineHeight();
            ImU32 ok = IM_COL32(80, 190, 120, 255);
            ImVec2 c(a.x + 12 * S, a.y + th * 0.5f + 5 * S);
            cl->PathEllipticalArcTo(c, ImVec2(6.5f * S, 3.8f * S), 0.f, 0.f, 6.2832f, 20);
            cl->PathStroke(ok, ImDrawFlags_Closed, 1.4f * S);
            cl->AddCircleFilled(c, 2.f * S, ok);
            ImGui::SetCursorScreenPos(ImVec2(a.x + 26 * S, a.y + 5 * S));
            ImGui::TextDisabled("%s", l.text.c_str());
            float y = a.y + th + 12 * S;
            for (auto& im : l.images) {
                if (!im->tex) continue;
                float iw = std::min(cw, 380 * S), ih = iw * im->h / std::max(1, im->w);
                ImVec2 i0(a.x, y), i1(a.x + iw, y + ih);
                cl->AddImageRounded(im->tex->GetTexRef(), i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8 * S);
                cl->AddRect(i0, i1, col::PanelBorder, 8 * S);
                if (ImGui::IsMouseHoveringRect(i0, i1) && ImGui::IsWindowHovered()) {
                    ImGui::BeginTooltip();
                    float bigW = std::min(900 * S, (float)im->w), bigH = bigW * im->h / std::max(1, im->w);
                    ImGui::Image(im->tex->GetTexRef(), ImVec2(bigW, bigH));
                    ImGui::EndTooltip();
                }
                y += ih + 6 * S;
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x, y));
            ImGui::Dummy(ImVec2(0, 0));
        } else if (l.kind == 0) {
            // You: a bubble at the right.
            float maxW = cw * 0.82f;
            ImVec2 ts = l.text.empty() ? ImVec2(0, 0) : ImGui::CalcTextSize(l.text.c_str(), nullptr, false, maxW - 24 * S);
            // Its pictures in a row above the words.
            float picH = l.images.empty() ? 0.f : 96 * S, picsW = 0;
            for (auto& im : l.images) picsW += picH * im->w / std::max(1, im->h) + 6 * S;
            picsW = std::min(picsW, maxW - 18 * S);
            float bw = std::min(maxW, std::max(ts.x, picsW) + 24 * S), bh = ts.y + 18 * S + (picH > 0 ? picH + (l.text.empty() ? 0 : 8 * S) : 0) - (l.text.empty() ? 6 * S : 0);
            float x0 = ImGui::GetCursorScreenPos().x + cw - bw;
            ImVec2 a(x0, ImGui::GetCursorScreenPos().y);
            cl->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), sl::orangeU(40), 14 * S);
            cl->AddRect(a, ImVec2(a.x + bw, a.y + bh), sl::orangeU(90), 14 * S);
            {
                float px = a.x + 12 * S;
                for (size_t k = 0; k < l.images.size(); ++k) {
                    auto& im = l.images[k];
                    float iw = picH * im->w / std::max(1, im->h);
                    if (px + iw > a.x + bw - 8 * S) break;
                    ImVec2 i0(px, a.y + 9 * S), i1(px + iw, a.y + 9 * S + picH);
                    if (im->tex) cl->AddImageRounded(im->tex->GetTexRef(), i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8 * S);
                    if (ImGui::IsMouseHoveringRect(i0, i1) && im->tex) {
                        // A bigger look on hover.
                        ImGui::BeginTooltip();
                        float bigW = std::min(520 * S, (float)im->w), bigH = bigW * im->h / std::max(1, im->w);
                        ImGui::Image(im->tex->GetTexRef(), ImVec2(bigW, bigH));
                        ImGui::EndTooltip();
                    }
                    px += iw + 6 * S;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x + 12 * S, a.y + 9 * S + (picH > 0 ? picH + 8 * S : 0)));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw - 24 * S);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + pad, a.y + bh));
            ImGui::Dummy(ImVec2(0, 0));
        } else if (l.kind == 1) {
            // The assistant: its text with markdown, right-click to copy.
            ImVec2 a = ImGui::GetCursorScreenPos();
            drawMarkdown(l.text, cw, fonts_.bold, col::Text, S);
            ImVec2 b = ImGui::GetItemRectMax();
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton("##msg", ImVec2(cw, std::max(1.f, b.y - a.y)));
            if (ImGui::BeginPopupContextItem("##msgctx")) {
                if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(l.text.c_str());
                ImGui::EndPopup();
            }
        } else if (l.kind == 2) {
            // A step it took: a compact row with a check mark (full text on hover).
            ImVec2 a = ImGui::GetCursorScreenPos();
            std::string t = l.text;
            size_t nl = t.find('\n');
            if (nl != std::string::npos) t = t.substr(0, nl);
            std::string shown = t.size() > 120 ? t.substr(0, 117) + "..." : t;
            float h = ImGui::CalcTextSize(shown.c_str(), nullptr, false, cw - 30 * S).y + 10 * S;
            cl->AddRectFilled(a, ImVec2(a.x + cw, a.y + h), col::Card, 8 * S);
            ImVec2 c(a.x + 12 * S, a.y + 5 * S + ImGui::GetTextLineHeight() * 0.5f);
            ImU32 ok = IM_COL32(80, 190, 120, 255);
            cl->AddLine(ImVec2(c.x - 4 * S, c.y), ImVec2(c.x - 1 * S, c.y + 3 * S), ok, 1.8f * S);
            cl->AddLine(ImVec2(c.x - 1 * S, c.y + 3 * S), ImVec2(c.x + 5 * S, c.y - 4 * S), ok, 1.8f * S);
            ImGui::SetCursorScreenPos(ImVec2(a.x + 24 * S, a.y + 5 * S));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw - 30 * S);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(shown.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton("##act", ImVec2(cw, h));
            if (ImGui::IsItemHovered() && l.text.size() > shown.size()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
                ImGui::TextUnformatted(l.text.substr(0, 3000).c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        } else {
            // A problem: a red card, Try again on the latest one.
            ImVec2 a = ImGui::GetCursorScreenPos();
            float h = ImGui::CalcTextSize(l.text.c_str(), nullptr, false, cw - 36 * S).y + 16 * S;
            bool retry = lineNo - 1 == lastError && !ai_.busy;
            if (retry) h += ImGui::GetFrameHeight() + 6 * S;
            cl->AddRectFilled(a, ImVec2(a.x + cw, a.y + h), IM_COL32(220, 60, 50, 38), 8 * S);
            cl->AddRect(a, ImVec2(a.x + cw, a.y + h), IM_COL32(220, 60, 50, 140), 8 * S);
            cl->AddCircleFilled(ImVec2(a.x + 14 * S, a.y + 8 * S + ImGui::GetTextLineHeight() * 0.5f), 6 * S, IM_COL32(220, 60, 50, 255));
            cl->AddText(ImVec2(a.x + 12 * S, a.y + 8 * S), IM_COL32_WHITE, "!");
            ImGui::SetCursorScreenPos(ImVec2(a.x + 28 * S, a.y + 8 * S));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw - 36 * S);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopTextWrapPos();
            if (retry) {
                ImGui::SetCursorScreenPos(ImVec2(a.x + 28 * S, a.y + h - ImGui::GetFrameHeight() - 6 * S));
                if (sl::orangeButton("Try again")) aiSend("Try again.", false);
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
            ImGui::Dummy(ImVec2(0, 0));
        }
        ImGui::PopID();
    }
    // A question from Kestral (graphics card busy...): its own card with the choices.
    if (aiChoice_.open) {
        ImVec2 a = ImGui::GetCursorScreenPos();
        float h = ImGui::CalcTextSize(aiChoice_.text.c_str(), nullptr, false, cw - 28 * S).y + ImGui::GetTextLineHeight() +
                  (ImGui::GetFrameHeight() + 6 * S) * (float)aiChoice_.options.size() + 34 * S;
        cl->AddRectFilled(a, ImVec2(a.x + cw, a.y + h), col::Card, 10 * S);
        cl->AddRect(a, ImVec2(a.x + cw, a.y + h), sl::orangeU(), 10 * S, 0, 1.5f * S);
        ImGui::SetCursorScreenPos(ImVec2(a.x + 14 * S, a.y + 10 * S));
        ImGui::BeginGroup();
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(aiChoice_.title.c_str());
        ImGui::PopFont();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw - 28 * S);
        ImGui::TextUnformatted(aiChoice_.text.c_str());
        ImGui::PopTextWrapPos();
        for (int i = 0; i < (int)aiChoice_.options.size(); ++i) {
            bool first = i == 0;
            bool clicked = first ? sl::orangeButton(aiChoice_.options[(size_t)i].c_str(), ImVec2(cw - 28 * S, 0))
                                 : ImGui::Button(aiChoice_.options[(size_t)i].c_str(), ImVec2(cw - 28 * S, 0));
            if (clicked && aiChoice_.answer) {
                aiChoice_.answer->set_value(i);
                aiChoice_.answer.reset();
                aiChoice_.open = false;
            }
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + 4 * S));
        ImGui::Dummy(ImVec2(0, 0));
    }
    if (ai_.busy) {
        std::string liveThink, liveText, stageNow, said;
        double t0;
        bool started, ollama;
        std::string url, model;
        {
            std::lock_guard<std::mutex> lk(aiLiveMx_);
            liveThink = aiLiveThink_;
            liveText = aiLiveText_;
            t0 = aiLiveT0_;
            started = aiLiveStarted_;
            stageNow = aiLiveStage_;
            ollama = aiLiveIsOllama_;
            url = aiLiveUrl_;
            model = aiLiveModel_;
            said = aiLiveOllama_;
        }
        std::string answer = stripThinking(liveText, &liveThink);
        double w = glfwGetTime() - t0;
        // What it is doing, with a spinner: loading, reading, thinking, writing, running a tool.
        std::string st = stageNow;
        if (!started && (st == "Waiting for the model" || st.empty())) {
            if (said.find("still loading") != std::string::npos) st = "Loading the model into memory";
            else if (said.find("loaded") != std::string::npos) {
                st = "Reading your request";
                if (aiLastInRate_ > 1 && aiLastPromptTok_ > 0) st += " (about " + secsText(aiLastPromptTok_ / aiLastInRate_) + " last time)";
            }
        }
        {
            ImVec2 a = ImGui::GetCursorScreenPos();
            float r = 7 * S;
            ImVec2 c(a.x + r + 2 * S, a.y + ImGui::GetTextLineHeight() * 0.5f);
            float ang = (float)ImGui::GetTime() * 5.f;
            cl->PathArcTo(c, r, ang, ang + 4.2f, 20);
            cl->PathStroke(sl::orangeU(), 0, 2.f * S);
            ImGui::SetCursorScreenPos(ImVec2(a.x + 2 * r + 10 * S, a.y));
            ImGui::TextUnformatted(st.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", secsText(w).c_str());
        }
        if (!said.empty() && !started && said.find("on the CPU") != std::string::npos) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
            ImGui::TextColored(sl::changed(), "%s", said.c_str());
            ImGui::PopTextWrapPos();
        }
        // Ask Ollama what it is doing with the model (every few seconds, off the main thread).
        if (ollama && !started && !aiPsBusy_ && glfwGetTime() - aiPsAt_ > 4.0) {
            aiPsBusy_ = true;
            aiPsAt_ = glfwGetTime();
            std::thread([this, url, model] {
                PsInfo ps = ollamaPs(url, model);
                std::string msg;
                if (!ps.ok) msg = "Ollama did not answer at " + url + ".";
                else if (!ps.loaded) msg = "Ollama is still loading " + model + " into memory.";
                else {
                    int gpu = ps.size > 0 ? (int)std::lround(ps.vram * 100.0 / ps.size) : 100;
                    char gb[32];
                    std::snprintf(gb, sizeof gb, "%.1f GB", ps.size / 1e9);
                    msg = "Ollama has " + model + " loaded (" + gb + (ps.ctx > 0 ? ", " + std::to_string(ps.ctx / 1024) + "K window" : "") + ")";
                    if (gpu >= 99) msg += ", all of it on the graphics card.";
                    else msg += ": " + std::to_string(gpu) + "% on the graphics card, " + std::to_string(100 - gpu) +
                                "% on the CPU. That part is slow: a smaller model or a smaller window keeps it all on the card.";
                }
                {
                    std::lock_guard<std::mutex> lk(aiLiveMx_);
                    if (msg != aiLiveOllama_) log::info("Assistant: " + msg);
                    aiLiveOllama_ = msg;
                }
                aiPsBusy_ = false;
                glfwPostEmptyEvent();
            }).detach();
        }
        if (!liveThink.empty() && settings_.aiShowThinking)
            thinkingBlock("live", liveThink, "Thinking  " + std::to_string(words(liveThink)) + " words", true, &aiLiveOpen_);
        if (!answer.empty()) drawMarkdown(answer, cw, fonts_.bold, col::Text, S);
        activity();
    }
    // While a reply streams in, stay at the bottom unless the user scrolled up to read.
    if (aiFollow_ && (ai_.scroll || ai_.busy)) {
        ImGui::SetScrollHereY(1.0f);
        if (ai_.scroll) activity();
    }
    ai_.scroll = false;
    aiLastScrollY_ = ImGui::GetScrollY();
    if (!aiFollow_ && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 30 * S) {
        // Scrolled up: a round "jump to the newest" button over the bottom of the chat, with how many
        // messages came in since.
        int fresh = 0;
        for (int i = std::max(0, aiSeenLines_); i < (int)ai_.lines.size(); ++i) fresh += ai_.lines[(size_t)i].kind != 4 ? 1 : 0;
        ImVec2 wpos = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
        float r = 17 * S;
        ImVec2 c(wpos.x + wsz.x * 0.5f, wpos.y + wsz.y - r - 10 * S);
        ImVec2 keep = ImGui::GetCursorScreenPos();
        // Checked by hand: the messages underneath have their own (invisible) buttons that would take the click.
        bool hv = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                  ImGui::IsMouseHoveringRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
        bool go = hv && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 36.f;
        if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* fl = ImGui::GetWindowDrawList();
        fl->AddCircleFilled(ImVec2(c.x, c.y + 2 * S), r + 1 * S, IM_COL32(0, 0, 0, 70), 32);   // soft shadow
        fl->AddCircleFilled(c, r, hv ? col::Hover : col::Card, 32);
        fl->AddCircle(c, r, fresh > 0 ? sl::orangeU() : col::PanelBorder, 32, 1.5f * S);
        ImU32 ac = hv ? col::Text : col::TextDim;
        fl->AddLine(ImVec2(c.x, c.y - 6 * S), ImVec2(c.x, c.y + 6 * S), ac, 2.f * S);
        fl->AddLine(ImVec2(c.x - 5 * S, c.y + 1 * S), ImVec2(c.x, c.y + 6 * S), ac, 2.f * S);
        fl->AddLine(ImVec2(c.x + 5 * S, c.y + 1 * S), ImVec2(c.x, c.y + 6 * S), ac, 2.f * S);
        if (fresh > 0) {
            char nb[16];
            std::snprintf(nb, sizeof nb, "%d", fresh);
            ImVec2 ts = ImGui::CalcTextSize(nb);
            float bw = std::max(ts.y + 4 * S, ts.x + 10 * S);
            ImVec2 b0(c.x + r * 0.45f, c.y - r - 4 * S);
            fl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + ts.y + 4 * S), sl::orangeU(), (ts.y + 4 * S) * 0.5f);
            fl->AddText(ImVec2(b0.x + (bw - ts.x) * 0.5f, b0.y + 2 * S), IM_COL32_WHITE, nb);
        }
        if (hv) ImGui::SetTooltip(fresh == 1 ? "1 new message" : fresh > 1 ? "%d new messages" : "Jump to the newest", fresh);
        if (go) {
            aiFollow_ = true;
            ai_.scroll = true;
            activity();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    // ---- input: a rounded card, the text wraps and the box grows; Enter sends, Shift+Enter a new line
    ImVec2 f0(wp.x + pad, wp.y + ws.y - footerH + 6 * S);
    ImVec2 f1(wp.x + ws.x - pad, f0.y + inputH);
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::GetIO().WantTextInput;
    dl->AddRectFilled(f0, f1, col::Card, 14 * S);
    dl->AddRect(f0, f1, focused ? sl::orangeU() : col::PanelBorder, 14 * S, 0, focused ? 1.5f * S : 1.f * S);
    // Pictures waiting to go: small cards at the top of the box, each with a remove button.
    if (!aiAttach_.empty()) {
        float px = f0.x + 12 * S, th = 58 * S;
        int removeAt = -1;
        for (size_t k = 0; k < aiAttach_.size(); ++k) {
            auto& im = aiAttach_[k];
            float iw = std::clamp(th * im->w / std::max(1, im->h), 30 * S, 120 * S);
            ImVec2 i0(px, f0.y + 10 * S), i1(px + iw, f0.y + 10 * S + th);
            if (im->tex) dl->AddImageRounded(im->tex->GetTexRef(), i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 7 * S);
            dl->AddRect(i0, i1, col::PanelBorder, 7 * S);
            bool hvImg = ImGui::IsMouseHoveringRect(i0, i1);
            if (hvImg && im->tex) {
                ImGui::BeginTooltip();
                float bigW = std::min(480 * S, (float)im->w), bigH = bigW * im->h / std::max(1, im->w);
                ImGui::Image(im->tex->GetTexRef(), ImVec2(bigW, bigH));
                ImGui::TextDisabled("%d x %d", im->w, im->h);
                ImGui::EndTooltip();
            }
            ImVec2 xc(i1.x - 9 * S, i0.y + 9 * S);
            ImGui::SetCursorScreenPos(ImVec2(xc.x - 8 * S, xc.y - 8 * S));
            ImGui::PushID((int)k + 7100);
            if (ImGui::InvisibleButton("##rmpic", ImVec2(16 * S, 16 * S))) removeAt = (int)k;
            bool hvX = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddCircleFilled(xc, 7.5f * S, hvX ? IM_COL32(220, 60, 50, 255) : IM_COL32(20, 20, 24, 210));
            dl->AddLine(ImVec2(xc.x - 3 * S, xc.y - 3 * S), ImVec2(xc.x + 3 * S, xc.y + 3 * S), IM_COL32_WHITE, 1.5f * S);
            dl->AddLine(ImVec2(xc.x + 3 * S, xc.y - 3 * S), ImVec2(xc.x - 3 * S, xc.y + 3 * S), IM_COL32_WHITE, 1.5f * S);
            px += iw + 8 * S;
        }
        if (removeAt >= 0) {
            ImTextureData* t = aiAttach_[(size_t)removeAt]->tex;
            aiAttach_.erase(aiAttach_.begin() + removeAt);
            aiImageTex_.erase(std::remove(aiImageTex_.begin(), aiImageTex_.end(), t), aiImageTex_.end());
            dropTexture(t);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(f0.x + 12 * S, f0.y + 9 * S + stripH));
    if (ai_.focus) { ImGui::SetKeyboardFocusHere(); ai_.focus = false; }
    // Ctrl+V with a picture on the clipboard (a screenshot, a copied image or image file) attaches it.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) aiPasteImage();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    float tw = f1.x - f0.x - 96 * S;
    bool send = ImGui::InputTextMultiline("##aiin", &ai_.input, ImVec2(tw, inputLines * ImGui::GetTextLineHeight() + 4 * S),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap |
                                              ImGuiInputTextFlags_NoHorizontalScroll);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (ai_.input.empty() && !ImGui::IsItemActive())
        dl->AddText(ImVec2(f0.x + 12 * S, f0.y + 9 * S + stripH), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    ai_.busy ? "Add something while it works..." : !aiAttach_.empty() ? "Say what to make from the picture (or just send)" : sliceWs_ ? "Ask about this print, or tell me what to change..." : "Ask anything, or tell me what to make...");
    // The page it will know about, at the bottom left of the card.
    {
        static const char* const tops[] = {"Home", "Prepare", "Preview", "Device", "Project", "Calibration"};
        std::string where = sliceWs_ ? std::string("Slice tab, ") + tops[std::clamp(sliceCurTop(), 0, 5)]
                                     : startOpen_ ? std::string("Start page") : (sk_.active ? "Sketch in " : "Design: ") + doc_.name;
        if (!sliceWs_ && !selection_.empty()) where += ", " + std::to_string(selection_.size()) + " selected";
        if (where.size() > 48) where = where.substr(0, 46) + "..";
        ImVec2 ws2 = ImGui::CalcTextSize(where.c_str());
        ImVec2 c0(f0.x + 10 * S, f1.y - ws2.y - 12 * S);
        dl->AddRectFilled(c0, ImVec2(c0.x + ws2.x + 14 * S, c0.y + ws2.y + 6 * S), col::Chip, 8 * S);
        dl->AddText(ImVec2(c0.x + 7 * S, c0.y + 3 * S), col::TextDim, where.c_str());
        if (ImGui::IsMouseHoveringRect(c0, ImVec2(c0.x + ws2.x + 14 * S, c0.y + ws2.y + 6 * S)))
            ImGui::SetTooltip(settings_.aiPageContext ? "The assistant knows you are here and what is selected" : "Page context is off (assistant settings)");
    }
    // Send (an orange circle with an arrow) or Stop (a square) at the bottom right.
    {
        float r = 15 * S;
        ImVec2 c(f1.x - r - 10 * S, f1.y - r - 8 * S);
        ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
        bool canSend = !ai_.input.empty() || !aiAttach_.empty();
        if (ImGui::InvisibleButton("##sendbtn", ImVec2(r * 2, r * 2))) {
            if (ai_.busy && !canSend) {
                if (ai_.cancel) ai_.cancel->store(true);
                ai_.busy = false;
                ai_.lines.push_back({3, "Stopped."});
                if (aiChoice_.answer) { aiChoice_.answer->set_value(-1); aiChoice_.answer.reset(); aiChoice_.open = false; }
            } else if (canSend) {
                send = true;
            }
        }
        bool hv = ImGui::IsItemHovered();
        bool stop = ai_.busy && !canSend;
        ImU32 bg = stop ? (hv ? IM_COL32(230, 80, 70, 255) : IM_COL32(200, 60, 50, 255)) : canSend ? (hv ? sl::orangeU() : sl::orangeDarkU()) : col::Chip;
        dl->AddCircleFilled(c, r, bg, 28);
        if (stop) dl->AddRectFilled(ImVec2(c.x - 5 * S, c.y - 5 * S), ImVec2(c.x + 5 * S, c.y + 5 * S), IM_COL32_WHITE, 1.5f * S);
        else {
            ImU32 ac = canSend ? IM_COL32_WHITE : col::TextDim;
            dl->AddLine(ImVec2(c.x, c.y + 6 * S), ImVec2(c.x, c.y - 6 * S), ac, 2.f * S);
            dl->AddLine(ImVec2(c.x - 5 * S, c.y - 1 * S), ImVec2(c.x, c.y - 6 * S), ac, 2.f * S);
            dl->AddLine(ImVec2(c.x + 5 * S, c.y - 1 * S), ImVec2(c.x, c.y - 6 * S), ac, 2.f * S);
        }
        if (hv) ImGui::SetTooltip(stop ? "Stop" : ai_.busy ? "Send: it reads this before its next step" : "Send (Enter)");
        // Add a picture: a framed landscape icon left of Send.
        ImVec2 pc(c.x - 2 * r - 6 * S, c.y);
        ImGui::SetCursorScreenPos(ImVec2(pc.x - r, pc.y - r));
        bool pick = ImGui::InvisibleButton("##picbtn", ImVec2(r * 2, r * 2));
        bool phv = ImGui::IsItemHovered();
        if (phv) dl->AddCircleFilled(pc, r, col::Hover, 28);
        ImU32 ic = phv ? col::Text : col::TextDim;
        dl->AddRect(ImVec2(pc.x - 8 * S, pc.y - 6 * S), ImVec2(pc.x + 8 * S, pc.y + 6 * S), ic, 2 * S, 0, 1.4f * S);
        dl->AddTriangleFilled(ImVec2(pc.x - 6 * S, pc.y + 4.5f * S), ImVec2(pc.x - 1 * S, pc.y - 1.5f * S), ImVec2(pc.x + 3 * S, pc.y + 4.5f * S), ic);
        dl->AddCircleFilled(ImVec2(pc.x + 4 * S, pc.y - 2.5f * S), 1.6f * S, ic);
        if (phv) ImGui::SetTooltip("Add a picture (or paste one with Ctrl+V, or drop it here)\nThe assistant models what it shows");
        if (pick) {
            std::string f = platform::openFileDialog("Add a picture for the assistant", "Pictures", "*.png;*.jpg;*.jpeg;*.bmp;*.gif");
            if (!f.empty()) aiAttachFile(f);
        }
    }
    {
        const char* hint = "Enter to send, Shift+Enter for a new line, Ctrl+V pastes a picture";
        ImVec2 hs = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(wp.x + (ws.x - hs.x) * 0.5f, f1.y + 6 * S), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }
    if (aiTestSend_ && glfwGetTime() > 6.0) { aiTestSend_ = false; send = true; }   // developer check
    static bool lateTest = std::getenv("KESTRAL_AI_LATE") != nullptr;   // developer check: messages arriving while scrolled up
    if (lateTest && glfwGetTime() > 20.0) {
        lateTest = false;
        ai_.lines.push_back({2, "fillet: 4 edges, 2 mm"});
        ai_.lines.push_back({1, "Done, the edges are rounded."});
        ai_.scroll = true;
    }
    static bool steerTest = std::getenv("KESTRAL_AI_STEER_PIC") != nullptr;
    if (steerTest && ai_.busy && glfwGetTime() > 9.0) {   // developer check: a picture sent while it works
        steerTest = false;
        aiAttachFile(std::getenv("KESTRAL_AI_STEER_PIC"));
        ai_.input = "also this";
        send = true;
    }
    if (send) {
        std::string t = ai_.input;
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        ai_.input.clear();
        ai_.focus = true;
        if (!ai_.busy) {
            aiSend(t);
        } else if (!t.empty() || !aiAttach_.empty()) {
            // Read before its next step, pictures included.
            AiLine ln{0, t};
            ln.images = aiAttach_;
            ai_.lines.push_back(ln);
            ai_.scroll = true;
            aiFollow_ = true;
            aiLog("user (while working)", t + (aiAttach_.empty() ? "" : " [" + std::to_string(aiAttach_.size()) + " picture(s)]"));
            std::lock_guard<std::mutex> lk(aiSteerMx_);
            aiSteer_.push_back({t, aiAttach_});
            aiAttach_.clear();
        }
    }
    ImGui::End();
}

// ===================================================================== Preferences > Assistant

void App::drawAssistantPrefs() {
    const float S = uiScale_;
    ResolvedProvider cur = resolveProvider(settings_, settings_.aiProvider);
    auto head = [&](const char* t) {
        ImGui::Spacing();
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        flashItem(std::string("pref:") + t);
        ImGui::Separator();
    };
    auto label = [&](const char* t, const char* tip = nullptr) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t);
        if (tip) ImGui::SetItemTooltip("%s", tip);
        flashItem(std::string("pref:") + t);
        ImGui::SameLine(190 * S);
        ImGui::SetNextItemWidth(-1);
    };
    auto pick = [&](const char* id, int& v, const char* const* items, int n) {
        bool ch = false;
        if (sl::beginMenuCombo(id, items[std::clamp(v, 0, n - 1)], 420 * S)) {
            for (int i = 0; i < n; ++i)
                if (sl::menuRow(items[i], v == i)) { v = i; ch = true; }
            sl::endMenuCombo();
        }
        return ch;
    };
    head("Model");
    {
        std::vector<std::pair<std::string, std::string>> list;
        for (auto& p : kPresets) list.push_back({p.id, p.label});
        for (auto& [id, sp] : settings_.ai)
            if (id.rfind("custom", 0) == 0) list.push_back({id, (sp.name.empty() ? id : sp.name) + "  (custom)"});
        label("Provider");
        if (sl::beginMenuCombo("##prov", cur.label.c_str(), 360 * S)) {
            for (auto& [id, lab] : list)
                if (sl::menuRow(lab.c_str(), id == settings_.aiProvider)) {
                    settings_.aiProvider = id;
                    ai_.models.clear();
                    ai_.fetchMsg.clear();
                    settings_.save(settingsPath_);
                }
            if (sl::menuRow("+ Add your own provider...", false)) {
                int n = 1;
                while (settings_.ai.count("custom" + std::to_string(n))) ++n;
                std::string id = "custom" + std::to_string(n);
                auto& sp = settings_.ai[id];
                sp.name = "My provider " + std::to_string(n);
                sp.format = 0;
                sp.url = "http://localhost:8080/v1";
                settings_.aiProvider = id;
                ai_.models.clear();
                settings_.save(settingsPath_);
            }
            sl::endMenuCombo();
        }
    }
    cur = resolveProvider(settings_, settings_.aiProvider);
    Settings::AiProvider& sp = settings_.ai[settings_.aiProvider];
    const ProviderPreset* preset = presetFor(settings_.aiProvider);
    if (cur.custom) {
        label("Name");
        if (ImGui::InputText("##cname", &sp.name)) settings_.save(settingsPath_);
        static const char* fmts[] = {"OpenAI style (most servers)", "Anthropic style", "Ollama"};
        label("API format");
        int f = std::clamp(sp.format, 0, 2);
        if (pick("##cfmt", f, fmts, 3)) { sp.format = f; settings_.save(settingsPath_); }
    }
    label("Address");
    if (ImGui::InputTextWithHint("##url", preset ? preset->url : "https://...", &sp.url)) settings_.save(settingsPath_);
    label("Model");
    {
        ImGui::SetNextItemWidth(-100 * S);
        if (ImGui::InputTextWithHint("##model", preset ? preset->model : "model name", &sp.model)) settings_.save(settingsPath_);
        ImGui::SameLine();
        if (ImGui::Button(ai_.fetching ? "..." : "Get list", ImVec2(-1, 0)) && !ai_.fetching) aiFetchModels();
    }
    if (!ai_.models.empty()) {
        label("Installed models");
        if (sl::beginMenuCombo("##models", cur.cfg.model.c_str(), 380 * S)) {
            for (auto& m : ai_.models)
                if (sl::menuRow(m.c_str(), m == cur.cfg.model)) { sp.model = m; settings_.save(settingsPath_); }
            sl::endMenuCombo();
        }
    }
    if (!ai_.fetchMsg.empty()) ImGui::TextDisabled("%s", ai_.fetchMsg.c_str());
    if (cur.needsKey || cur.custom || settings_.aiProvider == "lmstudio") {
        bool has = !sp.key.empty();
        label(cur.needsKey ? "API key" : "API key (if needed)");
        ImGui::SetNextItemWidth(-150 * S);
        ImGui::InputTextWithHint("##key", has ? "saved (encrypted). Paste to replace" : "paste key", &ai_.keyEdit, ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        bool flc3_ = ImGui::Button("Save") && !ai_.keyEdit.empty();
        flashItem("pref:Save");
        if (flc3_) {
            sp.key = platform::protectSecret(ai_.keyEdit);
            ai_.keyEdit.clear();
            settings_.save(settingsPath_);
            toast("API key saved, encrypted to your Windows account", ToastKind::Success);
        }
        if (has) {
            ImGui::SameLine();
            if (ImGui::Button("Forget")) { sp.key.clear(); settings_.save(settingsPath_); } flashItem("pref:Forget");
        }
    }
    ImGui::TextDisabled("%s", cur.help.c_str());
    if (cur.custom && ImGui::SmallButton("Remove this provider")) {
        settings_.ai.erase(settings_.aiProvider);
        settings_.aiProvider = "ollama";
        settings_.save(settingsPath_);
    }

    head("Speed and memory");
    {
        std::string key = settings_.aiProvider + "|" + cur.cfg.model;
        if (ai_.ctxFor != key && !ai_.ctxFetching) aiFetchModelInfo();
        auto lab = [](int t) {
            char b[32];
            if (t >= 1000000) std::snprintf(b, sizeof b, "%.3gM", t / 1048576.0);
            else std::snprintf(b, sizeof b, "%dK", (int)std::lround(t / 1024.0));
            return std::string(b);
        };
        int mx = std::max(ai_.ctxMax, 2048);
        std::vector<int> sizes;
        for (int s2 : {4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576})
            if (s2 < mx) sizes.push_back(s2);
        sizes.push_back(mx);
        int chosen = sp.ctx > 0 ? std::min(sp.ctx, mx) : 0;
        std::string autoL = cur.cfg.kind == 1 ? "Automatic (sized to the chat, stays on the card)" : "Automatic (the model's full " + lab(mx) + ")";
        label("Context window", "How much of the chat the model reads at once. Bigger windows use more memory on the graphics card.");
        if (sl::beginMenuCombo("##ctx", chosen == 0 ? autoL.c_str() : (lab(chosen) + " tokens").c_str(), 420 * S)) {
            if (sl::menuRow(autoL.c_str(), chosen == 0)) { sp.ctx = 0; settings_.save(settingsPath_); }
            for (int s2 : sizes)
                if (sl::menuRow((lab(s2) + " tokens").c_str(), chosen == s2, s2 == mx ? "maximum" : nullptr)) { sp.ctx = s2; settings_.save(settingsPath_); }
            sl::endMenuCombo();
        }
        ImGui::TextDisabled("%s", ai_.ctxFetching ? "Asking the service about this model..."
                                  : (ai_.ctxReported ? "This model takes up to " + lab(mx) + "." : "Up to about " + lab(mx) + " (typical for this model).").c_str());
        static const char* toolModes[] = {"Automatic (smart for local models, all for cloud models)", "Smart: core tools, the rest found when needed",
                                          "All tools with every message"};
        label("Tools sent", "Smart sends ~15 common tools and lets the model search the other ~100 (find_tools): a few thousand tokens instead\n"
                            "of ~25 thousand, so local models start answering in seconds. Big cloud models handle All well.");
        if (sl::beginMenuCombo("##tools", toolModes[std::clamp(settings_.aiToolsMode, 0, 2)], 440 * S)) {
            for (int i = 0; i < 3; ++i)
                if (sl::menuRow(toolModes[i], settings_.aiToolsMode == i)) { settings_.aiToolsMode = i; settings_.save(settingsPath_); }
            sl::endMenuCombo();
        }
        static const char* levels[] = {"Model default", "Off (fastest)", "Low", "Medium", "High (slow, careful)"};
        label("Thinking", "How long the model may reason before answering. Models that cannot think ignore it.");
        int lv = std::clamp(sp.thinking, 0, 4);
        if (pick("##thinking", lv, levels, 5)) { sp.thinking = lv; settings_.save(settingsPath_); }
        if (cur.cfg.kind == 1) {
            label("Keep the model loaded", "After a reply, how long Ollama keeps the model in memory (no loading wait next time).");
            int ka = settings_.aiKeepAliveMin;
            const char* kaL[] = {"5 minutes", "30 minutes", "2 hours", "Always"};
            const int kaV[] = {5, 30, 120, -1};
            int ki = 1;
            for (int i = 0; i < 4; ++i) if (kaV[i] == ka) ki = i;
            if (pick("##keep", ki, kaL, 4)) { settings_.aiKeepAliveMin = kaV[ki]; settings_.save(settingsPath_); }
            if (ImGui::Checkbox("Check the graphics card before sending (other models or programs using it)", &settings_.aiGpuCheck)) settings_.save(settingsPath_); flashItem("pref:Check the graphics card before sending (other models or programs using it)");
        }
        label("Temperature", "Lower is more exact and repeatable, higher more varied. Default: the model's own.");
        float tmp = settings_.aiTemperature;
        bool def = tmp < 0;
        if (ImGui::Checkbox("Model default##tdef", &def)) { settings_.aiTemperature = def ? -1.f : 0.3f; settings_.save(settingsPath_); } flashItem("pref:Model default");
        if (!def) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##temp", &settings_.aiTemperature, 0.f, 1.5f, "%.2f")) settings_.save(settingsPath_);
        }
    }

    head("Pictures");
    {
        // Pasted screenshots and photos: a model that sees them reads them itself, others get a
        // description from a vision model first.
        std::string key = settings_.aiProvider + "|" + cur.cfg.model;
        bool known = ai_.visionFor == key && ai_.mainVision >= 0;
        std::string autoL = cur.cfg.kind != 1 ? "Find out (cloud models see pictures)"
                            : !known       ? "Find out from Ollama"
                            : ai_.mainVision ? "Find out (this model sees pictures)" : "Find out (this model cannot see them)";
        const char* modes[] = {autoL.c_str(), "Yes, it reads pictures itself", "No, the helper below describes them"};
        label("Chat model sees pictures", "Paste a screenshot (Ctrl+V), drop a picture on the chat or click the picture button.\n"
                                          "Models that see read it directly. For others, the helper model looks first and writes a\n"
                                          "precise description (shapes, features, every dimension) for the chat model.");
        if (pick("##mainvis", settings_.aiMainVision, modes, 3)) settings_.save(settingsPath_);
        label("Picture helper model", "A vision model on the same service. It is unloaded right after it describes a picture, so\n"
                                      "your chat model keeps the graphics card.");
        std::string shown = settings_.aiVisionModel.empty() ? std::string("None") : settings_.aiVisionModel;
        if (sl::beginMenuCombo("##vismodel", shown.c_str(), 420 * S)) {
            if (sl::menuRow("None", settings_.aiVisionModel.empty())) { settings_.aiVisionModel.clear(); settings_.save(settingsPath_); }
            // Only models the service confirmed can see.
            if (!ai_.visionModels.empty()) {
                sl::menuHeader("INSTALLED MODELS THAT SEE PICTURES");
                for (auto& m : ai_.visionModels)
                    if (sl::menuRow(m.c_str(), m == settings_.aiVisionModel)) { settings_.aiVisionModel = m; settings_.save(settingsPath_); }
            }
            if (sl::menuRow(ai_.fetching ? "Checking the installed models..." : "Check the installed models again", false) && !ai_.fetching) aiFetchModels();
            sl::endMenuCombo();
        }
        if (cur.cfg.kind == 1 && ai_.models.empty() && !ai_.fetching && ai_.fetchMsg.empty()) aiFetchModels();
        if (!settings_.aiVisionModel.empty() && !ai_.models.empty() && !ai_.fetching && !ai_.visionModels.count(settings_.aiVisionModel))
            ImGui::TextColored(sl::changed(), "%s is not confirmed to see pictures. Pick one from the list.", settings_.aiVisionModel.c_str());
        if (ai_.fetching)
            ImGui::TextDisabled("Asking Ollama which installed models see pictures...");
        else if (cur.cfg.kind == 1 && !ai_.models.empty() && ai_.visionModels.empty())
            ImGui::TextDisabled("Ollama did not report any installed model that sees pictures (the log says what it answered).");
        else
            ImGui::TextDisabled("Models that see pictures and code well: Claude, GPT, Gemini, and locally Qwen3.6 or Gemma 3.");
    }

    head("Long chats");
    {
        static const char* cm[] = {"Summarize the older part (keeps what was decided)", "Leave out the oldest messages", "Off"};
        label("When the window fills");
        int c = std::clamp(settings_.aiCompact, 0, 2);
        if (pick("##compact", c, cm, 3)) { settings_.aiCompact = c; settings_.save(settingsPath_); }
        label("Start at");
        if (ImGui::SliderInt("##compactat", &settings_.aiCompactAt, 40, 95, "%d%% of the window")) settings_.save(settingsPath_);
        label("Step limit", "Tool steps one request may take before it stops (0: no limit)");
        if (ImGui::SliderInt("##steps", &settings_.aiMaxSteps, 0, 200, settings_.aiMaxSteps == 0 ? "no limit" : "%d steps")) settings_.save(settingsPath_);
    }

    head("Behaviour");
    {
        if (ImGui::Checkbox("Tell it which page I'm on and what is selected", &settings_.aiPageContext)) settings_.save(settingsPath_); flashItem("pref:Tell it which page I'm on and what is selected");
        if (ImGui::Checkbox("Show its thinking", &settings_.aiShowThinking)) settings_.save(settingsPath_); flashItem("pref:Show its thinking");
        if (ImGui::Checkbox("Let it start prints without asking", &settings_.aiAutoPrint)) settings_.save(settingsPath_); flashItem("pref:Let it start prints without asking");
        ImGui::TextUnformatted("Your own instructions (sent with every chat)");
        if (ImGui::InputTextMultiline("##custom", &settings_.aiCustomPrompt, ImVec2(-1, 90 * S), ImGuiInputTextFlags_WordWrap)) settings_.save(settingsPath_);
        ImGui::TextDisabled("For example: \"Always use PETG profiles\", \"Answer in German\", \"Ask before deleting bodies\".");
    }
}

} // namespace kst
