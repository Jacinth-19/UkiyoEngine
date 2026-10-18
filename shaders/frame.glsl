// Per-frame globals (camera, lights, shadow matrices, sky ambient).
UBO(0, 0) FrameBlock {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invViewProj;
    mat4 shadowMatrix[3];
    vec4 camPos;         // xyz
    vec4 sunDir;         // xyz, direction TOWARDS the sun
    vec4 sunColor;       // rgb * intensity (HDR)
    vec4 skyColor;       // hemisphere ambient, upward
    vec4 groundColor;    // hemisphere ambient, downward
    vec4 cascadeSplits;  // view-space distance where cascade 0/1/2 ends
    vec4 shadowParams;   // x enabled, y atlas texel size, z normal offset, w depth bias
    vec4 screenParams;   // xy internal resolution, z time (s), w frame index
    vec4 pointPos[4];    // xyz position, w radius
    vec4 pointColor[4];  // rgb * intensity
    vec4 toonA;          // RealToon: x hair highlight, y hair shift, z hair exponent, w self shadow on (0 = off)
    vec4 toonB;          // RealToon: x self threshold, y self hardness, z gloss, w gloss power
    vec4 toonC;          // x cel band on (1 = three-step ramp in mtoon; 0 = off), y normal map strength, z material outline noise, w reflection strength
    vec4 toonD;          // xy head forward (x, z) for face shadow, z face shadow on, w smear strength
    vec4 toonE;          // x soft light, y light sweep strength, z sweep speed (cycles/s)
    vec4 toonF;          // camera dither fade: x fully dithered out distance, y fully visible distance (off when y <= x)
    vec4 toonG;          // x stylised PBR strength, y eye flatten, z eye parallax depth (UV), w eye matcap strength
    vec4 toonH;          // x eye highlight map strength
    vec4 toonI;          // occlusion target: xyz world point (the character), w radius (0 = off)
    vec4 waterArea;      // water: x centre x, y level, z centre z, w 1 = a volume is on
    vec4 waterHalf;      // water: x half x, y foam width, z half z, w refraction offset scale
    vec4 waterShallow;   // water: rgb shallow colour, w absorption
    vec4 waterDeep;      // water: rgb deep colour, w 1 = camera is under the volume
    vec4 waterWaveD[4];  // water: per wave xy unit direction, z wavelength, w amplitude
    vec4 waterWaveS[4];  // water: per wave x speed multiplier, y steepness
} frame;
