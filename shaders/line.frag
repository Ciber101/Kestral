#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) flat in vec4 vColor;
layout(location=1) in vec3 vWorld;
layout(location=0) out vec4 outColor;
void main() {
    clipSection(vWorld);
    outColor = vColor;
}
