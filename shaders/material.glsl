// Per-material parameters (MToon-style) and textures.
UBO(1, 8) MaterialBlock {
    vec4 baseColor;   // rgba factor
    vec4 shadeColor;  // rgb shade factor
    vec4 emissive;    // rgb emissive factor
    vec4 toon;        // x shadingShift, y shadingToony, z giEqualization, w rimLightingMix
    vec4 rim;         // rgb rim colour, w fresnel power
    vec4 rimLift;     // x lift
    vec4 outline;     // rgb colour, w width
    vec4 flags;       // x alpha mode (0 opaque,1 mask,2 blend), y cutoff, z double-sided, w unlit
    vec4 flags2;      // x outline mode (0 none,1 world,2 screen)
    vec4 toon2;       // RealToon: x gloss texture present, y normal texture present, z face SDF material, w eye material
    vec4 fx0;         // per-character: x dissolve cut, y glow width, z darken, w get-hit flash
    vec4 fx1;         // rgb dissolve glow colour, w upgrade pulse amplitude
    vec4 fx2;         // xyz hair face anchor, w hair dither radius
    vec4 fx3;         // rgb get-hit colour, w excluded from occlusion
    vec4 fx4;         // rgb upgrade pulse colour
    vec4 leaf;        // x leaf transmission strength (0 = off), y view falloff power
} mtl;
TEX(1, 4) uniform sampler2D uBaseTex;
TEX(1, 5) uniform sampler2D uShadeTex;
TEX(1, 6) uniform sampler2D uEmissiveTex;
TEX(1, 3) uniform sampler2D uNormalTex;   // tangent-space normal map (flat when absent)
TEX(1, 7) uniform sampler2D uGlossTex;    // gloss mask in G = 1 - roughness (white when absent)
TEX(1, 2) uniform sampler2D uFaceSdfTex;  // face shadow SDF atlas (3x3, nine light angles); white when absent
TEX(1, 9) uniform sampler2D uMatcapTex;     // eye matcap (view-space normal lookup); used when flags2.z is set
TEX(1, 10) uniform sampler2D uHighlightTex; // eye highlight map; used when flags2.w is set
