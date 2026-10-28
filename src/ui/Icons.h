#pragma once
#include <imgui.h>

namespace kst::ui {

// Kestral's own vector icon set, drawn with ImDrawList so it is crisp at any DPI.
enum class Icon {
    None, NewComponent, Sketch, Extrude, Revolve, Sweep, Loft, Rib, Hole, Thread, Box, Cylinder,
    RectPattern, CircPattern, Mirror, PressPull, Fillet, Chamfer, Shell, Draft, Scale, Combine,
    SplitBody, MoveCopy, Align, OffsetFace, Parameters, Joint, AsBuiltJoint, RigidGroup,
    OffsetPlane, AnglePlane, MidPlane, Axis, Point, Measure, Interference, Section, InsertMesh,
    InsertStep, ExportStl, Slicer, Select, WindowSelect, Line, Rectangle, Circle, Arc, Polygon,
    Slot, Spline, Dimension, Appearance, Settings, Save, Undo, Redo, File, Home, Eye, EyeOff,
    Component, Body, Folder, Origin, Orbit, Pan, Zoom, Fit, LookAt, Display, Grid, Search,
    Play, StepBack, StepFwd, ToStart, ToEnd, ImportFeature, Gear, Close, Palette, Chevron,
    Trim, Offset, Project, Construction, FinishSketch, CenterRect, ConCoincident, ConCollinear, ConConcentric,
    ConMidpoint, ConFix, ConParallel, ConPerpendicular, ConHorizontal, ConVertical, ConTangent, ConEqual,
    ConSymmetric, SketchOptions, Break, Intersect, ProjectFace, CrossPoint, Sphere, Cone, Torus, Wedge, Tube,
};

void drawIcon(ImDrawList* dl, ImVec2 center, float size, Icon icon, bool enabled = true);
// Kestral's logo (the app icon): a K whose upper arm is a falcon's wing. topLeft, size in pixels.
void drawLogo(ImDrawList* dl, ImVec2 topLeft, float size);

} // namespace kst::ui
