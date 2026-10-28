#pragma once
#include "sketch/Profiles.h"
#include "sketch/Sketch.h"
#include <imgui.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace kst {

enum class SnapKind { None, Grid, Point, OnCurve, Midpoint, Horizontal, Vertical };

// What the cursor locked onto and what a click there should create.
struct SnapInfo {
    sk::Vec2 pos{0, 0};
    SnapKind kind = SnapKind::None;
    uint32_t point = 0;      // existing point to reuse
    uint32_t curve = 0;      // curve the new point lies on (OnCurve) or the line whose midpoint it is
};

// Everything the sketch editor remembers between frames (not part of the document).
struct SketchState {
    bool active = false;
    uint32_t sketchId = 0;
    bool choosingPlane = false;
    std::string pendingTool;           // tool to start once a plane is picked
    std::string tool;                  // "" = select
    int step = 0;
    std::vector<SnapInfo> clicks;      // points clicked so far for the current command
    std::vector<uint32_t> picks;       // entities picked (dimension, constraints, mirror)
    std::set<uint32_t> selEnt, selCon;
    uint32_t hoverEnt = 0, hoverCon = 0;
    int hoverPlane = -1;

    // cursor
    bool cursorValid = false;
    sk::Vec2 raw{0, 0};
    SnapInfo snap;
    std::string snapLabel;

    // options (Sketch Palette)
    bool optGrid = true, optSnap = true, optProfiles = true, optPoints = true, optDims = true, optCons = true;
    bool construction = false;         // draw new geometry as construction
    int polySides = 6;
    char textBuf[64] = "A";
    char textH[32] = "10";
    char textFont[64] = "Arial";
    uint32_t textNote = 0;             // the text being edited, 0 if the next click places a new one
    bool polyCircumscribed = false;
    bool offerLookAt = true;
    int xformKind = 0;                 // 0 move, 1 rotate, 2 scale
    bool xformCopy = false;
    char xformX[32] = "10";
    char xformY[32] = "0";
    char xformAng[32] = "90";
    char xformFac[32] = "2";
    char xformPx[32] = "0";
    char xformPy[32] = "0";
    int patKind = 0;                   // 0 rectangular, 1 circular
    char patNx[16] = "3";
    char patNy[16] = "1";
    char patSx[32] = "20";
    char patSy[32] = "20";
    char patCount[16] = "4";
    char patAng[32] = "360";

    // dragging geometry
    bool pressed = false;              // left button went down in select mode
    ImVec2 pressPos{0, 0};
    uint32_t pressEnt = 0, pressCon = 0;
    bool dragging = false;
    std::map<uint32_t, sk::Vec2> dragOrig;   // dragged point id -> start position
    sk::Vec2 dragStart{0, 0};
    bool boxSel = false;
    ImVec2 boxA{0, 0};

    // dimension being placed or edited
    bool dimPlacing = false;
    sk::Constraint dim;
    uint32_t dimEdit = 0;
    char dimBuf[48] = {};
    bool dimFocus = false;

    // per frame transforms
    glm::dmat4 view{1.0}, proj{1.0}, vp{1.0}, invVp{1.0};
    float W = 1, H = 1;
    double ppu = 1;                    // screen pixels per sketch unit near the cursor
    double gridStep = 10;

    // cached derived data
    std::vector<sk::Profile> profiles;
    uint64_t profRev = 0;
    uint32_t profSketch = 0;

    // hit rectangles from the last drawn frame
    std::map<uint32_t, ImVec4> dimRects, conRects;   // id -> (x0,y0,x1,y1)

    // Heads-up value boxes while drawing (Fusion's dimension input): type a number, Tab to the
    // next box, Enter or click to place. Typed values become dimensions.
    struct Hud {
        int n = 0, focus = 0;
        const char* label[2] = {nullptr, nullptr};
        std::string buf[2];
        double live[2] = {0, 0};      // what the cursor gives right now
        std::string key;              // tool + step the boxes belong to
    };
    Hud hud;

    uint32_t chainStart = 0;           // first point of the current line chain
    bool offsetClosed = false;         // offset tool: picked chain is closed
    std::vector<char> offsetRev;       // offset tool: travel direction per picked curve
    double lastSolveMs = 0;
};

} // namespace kst
