// Fragment-only dither helpers for fades. Include after common.glsl, in fragment shaders only (gl_FragCoord
// does not exist in the vertex stage).

// Ordered dither threshold (4x4 Bayer), in (0, 1).
float ditherBayer4(vec2 p) {
    ivec2 q = ivec2(mod(p, 4.0));
    const float m[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    return (m[q.x + q.y * 4] + 0.5) / 16.0;
}

// Distance fade of a whole model (pc.p2.x: the fraction dithered out, set per draw by the renderer). Every colour
// and shadow fragment shader of a model calls it, so the fade is the same in all of them.
void distanceFadeDiscard() {
    if (pc.p2.x > 0.0 && ditherBayer4(gl_FragCoord.xy) < pc.p2.x) discard;
}
