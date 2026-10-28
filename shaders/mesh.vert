#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNrm;
layout(location=2) in vec4 iM0;
layout(location=3) in vec4 iM1;
layout(location=4) in vec4 iM2;
layout(location=5) in vec4 iM3;
layout(location=6) in vec4 iColor;
layout(location=7) in uvec4 iInfo;  // x body id, y flags, z face base, w selected face range
layout(location=8) in uvec4 iInfo2; // x material, y face material table + 1 (0 = none)
layout(location=0) out vec3 vWorld;
layout(location=1) out vec3 vNrm;
layout(location=2) flat out vec4 vColor;
layout(location=3) flat out uvec4 vInfo;
layout(location=4) flat out uvec4 vInfo2;
layout(location=5) out vec3 vObj;
layout(location=6) out vec3 vObjN;
void main() {
    mat4 M = mat4(iM0, iM1, iM2, iM3);
    vec4 wp = M * vec4(inPos, 1.0);
    vWorld = wp.xyz;
    vNrm = mat3(M) * inNrm;
    vColor = iColor;
    vInfo = iInfo;
    vInfo2 = iInfo2;
    vObj = inPos;
    vObjN = inNrm;
    gl_Position = pc.viewProj * wp;
}
