#version 450
layout(push_constant) uniform BG { vec4 top; vec4 bottom; } bg;
layout(location=0) in float vT;
layout(location=0) out vec4 outColor;
void main() { outColor = mix(bg.top, bg.bottom, clamp(vT, 0.0, 1.0)); }
