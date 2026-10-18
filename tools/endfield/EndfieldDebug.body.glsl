// Endfield debug view: shows one channel of the main texture, scaled by exposure.
// channel: 0 RGB, 1 R, 2 G, 3 B, 4 A (as grey), 5 UV (u, v, 0).

void main() {
    vec4 c = texture(tex_main, v_uv);
    int ch = int(P_channel + 0.5);
    vec3 out3 = c.rgb;
    if (ch == 1) out3 = vec3(c.r);
    else if (ch == 2) out3 = vec3(c.g);
    else if (ch == 3) out3 = vec3(c.b);
    else if (ch == 4) out3 = vec3(c.a);
    else if (ch == 5) out3 = vec3(fract(v_uv), 0.0);
    o_color = vec4(clamp(out3 * P_exposure, 0.0, 1.0), c.a);
}
