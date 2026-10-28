#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec3 inPos;
layout(location=2) in vec4 iM0;
layout(location=3) in vec4 iM1;
layout(location=4) in vec4 iM2;
layout(location=5) in vec4 iM3;
layout(location=6) in vec4 iColor;
layout(location=7) in uvec4 iInfo;
layout(location=0) flat out vec4 vColor;
layout(location=1) out vec3 vWorld;
void main() {
    mat4 M = mat4(iM0, iM1, iM2, iM3);
    vec4 wp = M * vec4(inPos, 1.0);
    vWorld = wp.xyz;
    gl_Position = pc.viewProj * wp;
    bool selected = (iInfo.y & 1u) != 0u;
    vColor = selected ? vec4(0.05, 0.38, 0.85, 1.0) : vec4(vec3(0.13), 1.0);
}
