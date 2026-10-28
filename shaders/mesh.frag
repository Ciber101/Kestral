#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Kestral surface shading: a small physically based model (GGX specular, Schlick Fresnel) lit by
// a studio rig that follows the camera plus a soft environment, so metals read as metal, glass
// is see through and plastics look clean. Appearances can carry a procedural texture computed
// in the part's own coordinates, so it sticks to the part when it moves.
layout(set=0, binding=0) readonly buffer TriFace { uint triFace[]; };
layout(set=0, binding=1) readonly buffer SelFaces { uint selFaces[]; };
layout(set=0, binding=2) readonly buffer Mats { vec4 mats[]; };      // 3 per material
layout(set=0, binding=3) readonly buffer FaceMats { uint faceMats[]; }; // per face: material + 1, 0 = body's
layout(location=0) in vec3 vWorld;
layout(location=1) in vec3 vNrm;
layout(location=2) flat in vec4 vColor;
layout(location=3) flat in uvec4 vInfo;
layout(location=4) flat in uvec4 vInfo2;
layout(location=5) in vec3 vObj;
layout(location=6) in vec3 vObjN;
layout(location=0) out vec4 outColor;

const float PI = 3.14159265;

float hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float noise3(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i + vec3(0,0,0)), hash3(i + vec3(1,0,0)), f.x),
                   mix(hash3(i + vec3(0,1,0)), hash3(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0,0,1)), hash3(i + vec3(1,0,1)), f.x),
                   mix(hash3(i + vec3(0,1,1)), hash3(i + vec3(1,1,1)), f.x), f.y), f.z);
}
float fbm(vec3 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 4; ++i) { s += a * noise3(p); p = p * 2.03 + 17.1; a *= 0.5; }
    return s;
}
// Coordinates on the box face the normal points at most (planar texture projection).
vec2 boxUV(vec3 p, vec3 n) {
    vec3 a = abs(n);
    if (a.z >= a.x && a.z >= a.y) return p.xy;
    if (a.x >= a.y) return p.yz;
    return p.xz;
}

// Studio environment for reflections: bright softbox above, horizon band, darker floor.
vec3 envColor(vec3 R, float rough, vec3 key) {
    // CAD studio: bright sky, light gray floor, a soft overhead light box and a horizon line.
    float z = R.z;
    vec3 sky = vec3(0.80, 0.83, 0.87), ground = vec3(0.47, 0.47, 0.48);
    vec3 c = mix(ground, sky, smoothstep(-0.35, 0.3, z));
    float w = 0.06 + rough * 0.5;
    c += vec3(0.45) * exp(-pow((z - 0.92) / w, 2.0)) * (1.0 - rough * 0.7);   // overhead softbox
    c -= vec3(0.18) * exp(-pow((z + 0.02) / (w * 0.6), 2.0)) * (1.0 - rough);  // dark horizon line (reads as metal)
    c += vec3(0.25) * exp(-pow((z - 0.12) / (w * 0.8), 2.0)) * (1.0 - rough);  // bright band above it
    return mix(c, vec3(0.66, 0.67, 0.69), rough * rough * 0.8);
}

float ggx(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float smithG(float ndv, float ndl, float a) {
    float k = a * 0.5;
    return (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
}

void main() {
    clipSection(vWorld);
    uint face = triFace[vInfo.z + uint(gl_PrimitiveID)];
    // Appearance: the body's, unless this face has its own.
    uint mi = vInfo2.x;
    if (vInfo2.y != 0u) {
        uint fm = faceMats[vInfo2.y - 1u + face];
        if (fm != 0u) mi = fm - 1u;
    }
    vec4 m0 = mats[mi * 3u], m1 = mats[mi * 3u + 1u], m2 = mats[mi * 3u + 2u];
    vec3 base = m0.rgb;
    float alpha = m0.a;
    vec3 col2 = m1.rgb;
    int pattern = int(m1.a + 0.5);
    float metal = m2.x, rough = clamp(m2.y, 0.04, 1.0), scale = max(m2.z, 0.01), glow = m2.w;

    // Two passes: solid things first, then see through things blended on top.
    bool transparentPass = pc.params.x > 0.5;
    if (transparentPass != (alpha < 0.999)) discard;

    vec3 N = normalize(vNrm);
    if (!gl_FrontFacing) N = -N;
    vec3 V = pc.camPos.w > 0.5 ? -pc.viewDir.xyz : normalize(pc.camPos.xyz - vWorld);

    // ---- procedural textures (object space, in mm / scale)
    if (pattern != 0) {
        vec3 p = vObj / scale;
        vec3 on = normalize(vObjN);
        float h = 0.0;          // bump height (mm)
        float bump = 0.0;
        if (pattern == 1) {            // wood: growth rings around the part's Z axis, with grain
            vec3 q = p * vec3(1.0, 1.0, 0.12);
            float r = length(q.xy + vec2(fbm(q * 0.6) * 1.6)) * 1.4;
            float ring = smoothstep(0.15, 0.85, fract(r)) * 0.75 + fbm(p * vec3(6.0, 6.0, 0.4)) * 0.25;
            base = mix(base, col2, ring);
        } else if (pattern == 2) {     // carbon fiber 2x2 twill
            vec2 uv = boxUV(p, on) * 2.0;
            vec2 cell = floor(uv), f = fract(uv);
            bool dirA = mod(cell.x + cell.y, 4.0) < 2.0;   // 2x2 twill: diagonal steps
            float s = dirA ? sin(f.x * PI) : sin(f.y * PI);
            base = mix(col2, base, 0.35 + 0.65 * s);
            h = s * 0.03 * scale;
            bump = 1.0;
            rough = mix(rough, rough * 0.5, s);
        } else if (pattern == 3) {     // brushed metal: fine streaks along X
            float n = noise3(vec3(p.x * 0.15, p.y * 60.0, p.z * 60.0)) * 0.6 + noise3(vec3(p.x * 0.05, p.y * 220.0, p.z * 220.0)) * 0.4;
            base *= 0.9 + 0.2 * n;
            rough = clamp(rough + (n - 0.5) * 0.15, 0.04, 1.0);
        } else if (pattern == 4) {     // knurl: crossed diagonal grooves
            vec2 uv = boxUV(p, on);
            float a = abs(fract(uv.x + uv.y) - 0.5), b = abs(fract(uv.x - uv.y) - 0.5);
            float ridge = min(a, b);
            h = ridge * 0.25 * scale;
            bump = 1.0;
            base = mix(col2, base, smoothstep(0.0, 0.18, ridge));
        } else if (pattern == 5) {     // checker (great for seeing curvature)
            float c = mod(floor(p.x) + floor(p.y) + floor(p.z), 2.0);
            base = mix(base, col2, c);
        } else if (pattern == 6) {     // marble veins
            float v = sin(p.x * 1.7 + p.y * 0.6 + fbm(p * 0.9) * 7.0);
            float vein = pow(1.0 - abs(v), 10.0);
            base = mix(base, col2, vein * 0.85 + fbm(p * 3.0) * 0.08);
        } else if (pattern == 7) {     // speckle: textured plastic, concrete, granite
            float n = noise3(p * 9.0);
            float dots = step(0.82, noise3(p * 23.0 + 5.0));
            base = mix(base, col2, dots * 0.8) * (0.93 + n * 0.14);
            h = n * 0.02 * scale;
            bump = 1.0;
        } else if (pattern == 8) {     // hexagon mesh
            vec2 uv = boxUV(p, on);
            vec2 r = vec2(1.0, 1.7320508), hh = r * 0.5;
            vec2 a = mod(uv, r) - hh, b = mod(uv - hh, r) - hh;
            vec2 g = dot(a, a) < dot(b, b) ? a : b;
            float d = max(abs(g.x) * 0.866 + abs(g.y) * 0.5, abs(g.y));
            float edge = smoothstep(0.40, 0.46, d);
            base = mix(base, col2, edge);
            h = (1.0 - edge) * 0.05 * scale;
            bump = 1.0;
        } else if (pattern == 9) {     // diamond (tread) plate
            vec2 uv = boxUV(p, on);
            vec2 c = floor(uv), f = fract(uv) - 0.5;
            bool flip = mod(c.x + c.y, 2.0) > 0.5;
            vec2 q = flip ? vec2(f.y, f.x) : f;
            q = vec2(q.x + q.y, q.x - q.y) * 0.7071;
            float lozenge = 1.0 - smoothstep(0.06, 0.1, abs(q.y)) ;
            lozenge *= 1.0 - smoothstep(0.28, 0.34, abs(q.x));
            h = lozenge * 0.12 * scale;
            bump = 1.0;
            base *= 0.92 + 0.12 * lozenge;
        }
        if (bump > 0.0) {
            // Bump from the height field (screen space derivatives, no tangents needed).
            vec3 dpx = dFdx(vWorld), dpy = dFdy(vWorld);
            float dhx = dFdx(h), dhy = dFdy(h);
            vec3 r1 = cross(dpy, N), r2 = cross(N, dpx);
            float det = dot(dpx, r1);
            if (abs(det) > 1e-12) {
                vec3 grad = sign(det) * (dhx * r1 + dhy * r2);
                N = normalize(abs(det) * N - grad);
            }
        }
    }

    // ---- lighting: key (above left of the camera), fill (right), hemisphere ambient, environment
    float ndv = max(dot(N, V), 1e-3);
    vec3 F0 = mix(vec3(0.04), base, metal);
    vec3 diffuseCol = base * (1.0 - metal);
    vec3 side = normalize(cross(V, vec3(0, 0, 1)) + vec3(1e-4));
    vec3 Ls[2] = vec3[](normalize(V + 0.35 * side + vec3(0, 0, 0.45)), normalize(V - 0.8 * side + vec3(0, 0, 0.1)));
    float Li[2] = float[](1.0, 0.32);
    float a = rough * rough;
    vec3 col = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        vec3 L = Ls[i];
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0) continue;
        vec3 H = normalize(L + V);
        float ndh = max(dot(N, H), 0.0), vdh = max(dot(V, H), 0.0);
        vec3 F = F0 + (1.0 - F0) * pow(1.0 - vdh, 5.0);
        vec3 spec = F * ggx(ndh, a) * smithG(ndv, ndl, a) / (4.0 * ndv * ndl + 1e-4);
        col += (diffuseCol * (1.0 - F) * 0.62 + spec * 0.55) * ndl * Li[i];
    }
    float hemi = 0.5 + 0.5 * N.z;
    vec3 amb = mix(vec3(0.42, 0.40, 0.38), vec3(0.62, 0.66, 0.72), hemi);
    col += diffuseCol * amb * 0.52 + diffuseCol * 0.1 * ndv;
    vec3 R = reflect(-V, N);
    vec3 Fe = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndv, 5.0);
    vec3 env = envColor(R, rough, Ls[0]);
    // A tall soft box beside the camera: the bright streak that sells polished metal on curves.
    vec3 Hs = normalize(V + normalize(V + 1.3 * side));
    // The strip stands upright, so it lights a vertical band on any wall facing it.
    float nl = length(N.xy), hl = length(Hs.xy);
    float az = (nl > 1e-3 && hl > 1e-3) ? dot(N.xy / nl, Hs.xy / hl) : 0.0;
    float band = smoothstep(0.965 - rough * 0.35, 0.999 - rough * 0.25, az) * smoothstep(0.35, 0.8, nl);
    env += vec3(0.9) * band * (1.0 - rough * 0.65);
    col += env * Fe * mix(0.55, 1.0, metal);
    col += base * glow;

    // ---- selection and hover tints
    uint flags = vInfo.y;
    bool selected = (flags & 1u) != 0u;
    bool hoverBody = (flags & 2u) != 0u || (pc.hover.z == 0u && pc.hover.x == vInfo.x);
    bool hoverFace = pc.hover.z == 1u && pc.hover.x == vInfo.x && pc.hover.y == face + 1u;
    uint selCount = vInfo.w & 255u, selStart = vInfo.w >> 8;
    bool faceSel = false;
    for (uint i = 0u; i < selCount; ++i)
        if (selFaces[selStart + i] == face) { faceSel = true; break; }
    if (selected) col = mix(col, vec3(1.0, 0.42, 0.21), 0.42);
    if (faceSel) col = mix(col, vec3(1.0, 0.38, 0.12), 0.6);
    bool browserHover = (flags & 2u) != 0u;   // row hovered in the browser: clearly visible, Fusion style
    if (browserHover) col = mix(col, vec3(1.0, 0.66, 0.45), faceSel ? 0.28 : 0.5);
    else if (hoverBody || hoverFace) col = mix(col, vec3(1.0, 0.66, 0.45), faceSel ? 0.2 : 0.35);
    col = pow(clamp(col, 0.0, 1.0), vec3(1.0 / 1.1));

    if (transparentPass) {
        // Premultiplied: glass keeps strong highlights and gets more opaque at grazing angles.
        float fres = pow(1.0 - ndv, 4.0);
        float outA = clamp(alpha + fres * 0.45 + ((selected || faceSel || hoverBody || hoverFace) ? 0.25 : 0.0), 0.0, 1.0);
        outColor = vec4(col * outA, outA);
    } else {
        outColor = vec4(col, 1.0);
    }
}
