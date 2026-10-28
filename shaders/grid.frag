#version 450
layout(push_constant) uniform G { mat4 viewProj; vec4 camPos; vec4 params; vec4 flags; } g;
layout(location=0) in vec3 vWorld;
layout(location=0) out vec4 outColor;
float gridLine(vec2 p, float s) {
    vec2 q = p / s;
    vec2 d = abs(fract(q - 0.5) - 0.5) / max(fwidth(q), vec2(1e-6));
    return 1.0 - min(min(d.x, d.y), 1.0);
}
float axisLine(float v) {
    return 1.0 - min(abs(v) / max(fwidth(v), 1e-6) * 0.6, 1.0);
}
void main() {
    float minor = g.params.y;
    // params.w > 0.5: origin axes only (the ground grid is turned off).
    float a = g.params.w > 0.5 ? 0.0 : gridLine(vWorld.xy, minor) * 0.22 + gridLine(vWorld.xy, minor * 10.0) * 0.30;
    float dist = length(vWorld.xy - g.camPos.xy);
    float fade = 1.0 - smoothstep(g.params.z * 0.45, g.params.z, dist);
    vec3 col = g.flags.yzw;
    vec4 res = vec4(col, a * fade);
    if (g.flags.x > 0.5) {
        float ax = axisLine(vWorld.y); // X axis runs along y = 0
        float ay = axisLine(vWorld.x);
        if (ax > 0.01) res = mix(res, vec4(0.85, 0.22, 0.20, 0.9 * fade), ax);
        if (ay > 0.01) res = mix(res, vec4(0.25, 0.70, 0.25, 0.9 * fade), ay);
    }
    if (res.a < 0.004) discard;
    outColor = res;
}
