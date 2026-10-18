// Water surface shading (item 16). Refracts the copied scene (rgb colour, a = view depth, from water_copy.frag),
// absorbs it by the water column depth, blends in sky reflection by Fresnel, adds the sun glint with shadow,
// and puts foam where the scene is shallow under the surface and on the wave crests.
#include "common.glsl"
#include "frame.glsl"
#include "shadow_sample.glsl"

TEX(1, 2) uniform sampler2D uRefr;   // Post set binding 2 (binding 1 is the shadow map): rgb scene colour, a linear view depth (metres)

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vN;
layout(location = 2) in float vCrest;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vN);
    if (!gl_FrontFacing) N = -N;   // seen from below: shade with the normal facing the viewer
    vec3 V = normalize(frame.camPos.xyz - vWorld);
    vec3 L = normalize(frame.sunDir.xyz);
    vec2 suv = gl_FragCoord.xy / frame.screenParams.xy;
    float waterZ = -(frame.view * vec4(vWorld, 1.0)).z;

    // Refraction: offset the lookup by the normal's horizontal tilt. Fall back to the straight lookup when the
    // offset sample is in front of the surface (it would show something above the water).
    vec2 duv = suv + N.xz * frame.waterHalf.w;
    vec4 refr = texture(uRefr, duv);
    if (refr.a < waterZ) {
        duv = suv;
        refr = texture(uRefr, suv);
    }
    float d = max(refr.a - waterZ, 0.0);   // water column length along the view ray (metres)

    // Absorption: red fades first. Scattering colour fills in with depth.
    float absorb = frame.waterShallow.w;
    float sh = shadowFactor(vWorld, N);
    vec3 lit = frame.skyColor.rgb * 0.7 + frame.sunColor.rgb * 0.3 * sh;
    vec3 transm = exp(-d * absorb * vec3(0.6, 0.25, 0.12));
    vec3 body = mix(frame.waterShallow.rgb, frame.waterDeep.rgb, 1.0 - exp(-d * 0.4)) * lit;
    vec3 under = refr.rgb * transm;
    under = mix(under, body, 1.0 - exp(-d * absorb));

    // Fresnel between the refracted scene and the sky reflection.
    float cosT = clamp(dot(N, V), 0.0, 1.0);
    float F = 0.02 + 0.98 * pow(1.0 - cosT, 5.0);
    vec3 R = reflect(-V, N);
    vec3 sky = mix(frame.groundColor.rgb, frame.skyColor.rgb, smoothstep(-0.2, 0.4, R.y));
    float sunDot = max(dot(R, L), 0.0);
    vec3 glint = frame.sunColor.rgb * (pow(sunDot, 600.0) * 2.0 + pow(sunDot, 60.0) * 0.15) * sh;
    vec3 col = mix(under, sky, F) + glint;

    // Foam: shallow scene under the surface (edge foam), and crests. Noise breaks both up.
    float edge = 1.0 - smoothstep(0.0, max(frame.waterHalf.y, 1e-3), d);
    float t = frame.screenParams.z;
    float n1 = 0.5 + 0.5 * sin(vWorld.x * 2.3 + t * 1.7) * sin(vWorld.z * 1.9 - t * 1.1 + vWorld.x * 0.7);
    float foam = clamp(edge * (0.35 + 0.9 * n1) + vCrest * (0.25 + 0.5 * n1), 0.0, 1.0);
    col = mix(col, vec3(0.92, 0.95, 0.97) * lit, foam * 0.6);

    outColor = vec4(col, 1.0);
}
