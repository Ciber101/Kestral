#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(set=0, binding=0) readonly buffer TriFace { uint triFace[]; };
layout(location=0) in vec3 vWorld;
layout(location=3) flat in uvec4 vInfo;
layout(location=0) out uvec2 outId;
void main() {
    clipSection(vWorld);
    uint face = triFace[vInfo.z + uint(gl_PrimitiveID)];
    outId = uvec2(vInfo.x, face + 1u);
}
