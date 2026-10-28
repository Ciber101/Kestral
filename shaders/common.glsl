// Shared push constants for mesh, id and line pipelines.
layout(push_constant) uniform PC {
    mat4  viewProj;
    vec4  camPos;    // xyz eye, w = 1 if orthographic
    vec4  viewDir;   // xyz forward
    uvec4 hover;     // x body id, y face id + 1 (0 = none), z mode (0 body, 1 face)
    vec4  params;    // x: see through pass. y: section axis (0 off, 1 X, 2 Y, 3 Z, negative flips). z: plane position
} pc;

#ifdef GL_FRAGMENT_SHADER
void clipSection(vec3 p) {
    float axis = pc.params.y;
    if (axis == 0.0) return;
    float a = abs(axis);
    float coord = a < 1.5 ? p.x : (a < 2.5 ? p.y : p.z);
    float side = coord - pc.params.z;
    if (axis > 0.0) { if (side > 0.02) discard; }
    else { if (side < -0.02) discard; }
}
#endif
