#version 450
layout(push_constant) uniform G { mat4 viewProj; vec4 camPos; vec4 params; vec4 flags; } g;
layout(location=0) out vec3 vWorld;
void main() {
    const vec2 c[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));
    float s = g.params.y * 10.0;
    vec2 center = floor(g.camPos.xy / s) * s;
    vec2 p = center + c[gl_VertexIndex] * g.params.x;
    vWorld = vec3(p, 0.0);
    gl_Position = g.viewProj * vec4(vWorld, 1.0);
}
