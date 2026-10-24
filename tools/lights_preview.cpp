// The figure for docs/LIGHTS.md: three authored lights on one surface, shaded with the same maths shaders/mtoon.frag
// uses.
//
// The tool builds a `SceneLight` list exactly as a scene file would (one directional key, one point, one spot), packs it
// with the shipping `packSceneLights`, and then shades a wall of sample cells at that packed light's parameters: the
// toon ramp, the windowed inverse-square attenuation and the spot cone. It is the shader's arithmetic on the CPU, so the
// figure changes when the shader does.
//
//   lights_preview --out docs/lights.png
//
// No GPU, no window: CI runs it on every push, which is what stops the picture drifting away from the code.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "engine/lights.h"
#include "engine/text_atlas.h"

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA8
};

void blendPixel(Image& img, int x, int y, const glm::vec4& c) {
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
    uint8_t* p = &img.px[(size_t(y) * img.w + x) * 4];
    const float a = std::clamp(c.a, 0.0f, 1.0f);
    p[0] = uint8_t(std::lround(float(p[0]) * (1 - a) + std::clamp(c.r, 0.0f, 1.0f) * 255.0f * a));
    p[1] = uint8_t(std::lround(float(p[1]) * (1 - a) + std::clamp(c.g, 0.0f, 1.0f) * 255.0f * a));
    p[2] = uint8_t(std::lround(float(p[2]) * (1 - a) + std::clamp(c.b, 0.0f, 1.0f) * 255.0f * a));
    p[3] = 255;
}

void disc(Image& img, float cx, float cy, float r, const glm::vec4& c) {
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y) {
        for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
            const float d = std::sqrt((float(x) + 0.5f - cx) * (float(x) + 0.5f - cx) +
                                      (float(y) + 0.5f - cy) * (float(y) + 0.5f - cy));
            const float cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) blendPixel(img, x, y, glm::vec4(c.r, c.g, c.b, c.a * cov));
        }
    }
}

void line(Image& img, float ax, float ay, float bx, float by, const glm::vec4& c) {
    const float dx = bx - ax, dy = by - ay;
    const int steps = std::max(1, int(std::sqrt(dx * dx + dy * dy)));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        blendPixel(img, int(ax + dx * t), int(ay + dy * t), c);
    }
}

// mtoon.frag's linearstep: a linear ramp with the given width, not a smoothstep curve.
float linearstep(float e0, float e1, float x) {
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    return std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
}

// The shader's contribution of one packed light at a surface point, with a normal pointing at the camera (+Z) and the
// same toon ramp the MToon fragment shader uses.
glm::vec3 shadeLight(const ukiyo::GpuLight& light, const glm::vec3& albedo, const glm::vec3& shadeTint,
                     const glm::vec3& p, const glm::vec3& n, float toony, float shift) {
    glm::vec3 toLight;
    if (light.params.x < 0.5f) {
        // Directional: direction points the way the light travels, so the vector to the light is its negation.
        toLight = -glm::normalize(glm::vec3(light.dirCosOuter));
        const float ndl = glm::dot(n, toLight);
        const float ramp = linearstep(-1.0f + toony, 1.0f - toony, ndl + shift);
        return mix(albedo * shadeTint, albedo, ramp) * glm::vec3(light.colorIntensity) * light.colorIntensity.w;
    }
    const glm::vec3 toCentre = glm::vec3(light.posRadius) - p;
    const float dist = glm::length(toCentre);
    const glm::vec3 l = toCentre / std::max(dist, 1e-4f);
    const float reach = light.posRadius.w;
    const float window = std::clamp(1.0f - dist / std::max(reach, 1e-3f), 0.0f, 1.0f);
    float att = window * window / (1.0f + dist * dist);
    if (light.params.x > 1.5f) {
        const float cd = glm::dot(-l, glm::normalize(glm::vec3(light.dirCosOuter)));
        att *= std::clamp((cd - light.dirCosOuter.w) / std::max(light.params.y - light.dirCosOuter.w, 1e-3f), 0.0f, 1.0f);
    }
    if (att <= 0.0f) return glm::vec3(0.0f);
    const float ndl = glm::dot(n, l);
    const float ramp = linearstep(-1.0f + toony, 1.0f - toony, ndl + shift);
    return mix(albedo * shadeTint, albedo, ramp) * glm::vec3(light.colorIntensity) * light.colorIntensity.w * att;
}

struct Text {
    const ukiyo::FontAtlas* font = nullptr;
    void draw(Image& img, float pen, float baseline, const std::string& str, const glm::vec4& col) const {
        if (!font || !font->loaded()) return;
        const int w = font->width(), h = font->height();
        auto texel = [&](int tx, int ty) {
            return float(font->pixels()[(size_t(std::clamp(ty, 0, h - 1)) * size_t(w) + size_t(std::clamp(tx, 0, w - 1))) * 4 + 3]) / 255.0f;
        };
        auto sample = [&](float u, float v) {
            const float fx = u * float(w) - 0.5f, fy = v * float(h) - 0.5f;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const float ax = fx - float(x0), ay = fy - float(y0);
            const float a = texel(x0, y0) * (1 - ax) + texel(x0 + 1, y0) * ax;
            const float b = texel(x0, y0 + 1) * (1 - ax) + texel(x0 + 1, y0 + 1) * ax;
            return a * (1 - ay) + b * ay;
        };
        for (uint32_t cp : ukiyo::FontAtlas::decodeUtf8(str)) {
            const ukiyo::GlyphMetrics* g = const_cast<ukiyo::FontAtlas*>(font)->glyph(cp);
            if (!g) continue;
            const float gx = pen + g->xoff, gy = baseline + g->yoff;
            const int iw = int(std::lround(g->width)), ih = int(std::lround(g->height));
            for (int y = 0; y < ih; ++y)
                for (int x = 0; x < iw; ++x) {
                    const float u = g->u0 + (g->u1 - g->u0) * (float(x) + 0.5f) / float(iw);
                    const float v = g->v0 + (g->v1 - g->v0) * (float(y) + 0.5f) / float(ih);
                    const float a = sample(u, v);
                    if (a > 0.008f) blendPixel(img, int(gx) + x, int(gy) + y, glm::vec4(col.r, col.g, col.b, col.a * a));
                }
            pen += g->advance;
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string out = "docs/lights.png";
    std::string fontPath = "editor/res/fonts/IBMPlexSansJP-Regular.ttf";
    int width = 2000, height = 900;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--font" && i + 1 < argc) fontPath = argv[++i];
        else if (a == "--width" && i + 1 < argc) width = std::stoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) height = std::stoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: lights_preview [--out docs/lights.png] [--font font.ttf] [--width 2000] [--height 900]\n");
            return 0;
        }
    }

    // --- The scene's lights: exactly what a .ukscene would carry -------------------------------------
    std::vector<ukiyo::SceneLight> scene;
    {
        ukiyo::SceneLight key;
        key.type = ukiyo::LightType::Directional;
        key.direction = glm::vec3(0.35f, -1.0f, 0.45f);   // the way the light travels
        key.color = glm::vec3(1.0f, 0.96f, 0.90f);
        key.intensity = 1.05f;
        scene.push_back(key);

        ukiyo::SceneLight cool;
        cool.type = ukiyo::LightType::Point;
        cool.position = glm::vec3(-2.62f, 1.32f, 1.15f);
        cool.color = glm::vec3(0.38f, 0.66f, 1.0f);
        cool.intensity = 3.6f;
        cool.radius = 3.6f;
        scene.push_back(cool);

        ukiyo::SceneLight warm;
        warm.type = ukiyo::LightType::Spot;
        warm.position = glm::vec3(2.15f, 1.95f, 1.35f);
        warm.direction = glm::vec3(-0.10f, -1.0f, -0.75f);
        warm.color = glm::vec3(1.0f, 0.66f, 0.36f);
        warm.intensity = 4.6f;
        warm.radius = 5.0f;
        warm.spotAngleDeg = 46.0f;
        warm.spotSoftness = 0.35f;
        scene.push_back(warm);
    }

    // The shipping selection, with the camera looking at the middle of the wall from three metres out.
    const glm::vec3 focus(0.0f, 1.1f, 0.0f);
    ukiyo::GpuLight packed[ukiyo::kFrameLights];
    const int count = ukiyo::packSceneLights(scene, focus, glm::vec3(0.0f, 0.0f, -1.0f), ukiyo::kFrameLights, packed);
    std::printf("lights_preview: %d of %zu lights selected\n", count, scene.size());

    Image img;
    img.w = width;
    img.h = height;
    img.px.assign(size_t(width) * size_t(height) * 4, 0);

    // --- The studio: a floor, a back wall and three spheres, ray traced per pixel and shaded with the shader's maths --
    // This is the same arithmetic mtoon.frag runs: the toon ramp on N.L, the windowed inverse-square falloff, and the spot
    // cone. It is on the CPU because CI has no device, but nothing here is a picture of a light - it is the light.
    const float toony = 0.9f, shift = 0.0f;
    const glm::vec3 albedoFloor(0.46f, 0.47f, 0.50f);
    const glm::vec3 albedoWall(0.50f, 0.49f, 0.52f);
    const glm::vec3 albedoSph[3] = {{0.86f, 0.44f, 0.40f}, {0.95f, 0.93f, 0.90f}, {0.42f, 0.62f, 0.88f}};
    const glm::vec3 shadeTint(0.46f, 0.46f, 0.56f);
    const glm::vec3 ambient(0.045f, 0.05f, 0.07f);

    struct Sphere { glm::vec3 c; float r; glm::vec3 albedo; };
    const Sphere spheres[3] = {
        {{-2.05f, 0.52f, -0.15f}, 0.52f, albedoSph[0]},
        {{0.10f, 0.44f, -0.75f}, 0.44f, albedoSph[1]},
        {{2.05f, 0.60f, -0.30f}, 0.60f, albedoSph[2]},
    };

    const glm::vec3 eye(0.0f, 1.78f, 5.35f);
    const glm::vec3 look(0.0f, 1.05f, -0.75f);
    const glm::vec3 fwd = glm::normalize(look - eye);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, fwd);
    const float tanHalf = std::tan(0.5f * 0.74f);   // about a 35-degree vertical field of view

    auto trace = [&](const glm::vec3& ro, const glm::vec3& rd, float* tHit, glm::vec3* albedo, glm::vec3* nrm) {
        float best = 1e30f;
        glm::vec3 a(0.0f), n(0.0f);
        // Floor.
        if (rd.y < -1e-5f) {
            const float t = -ro.y / rd.y;
            if (t > 0.0f && t < best) {
                best = t;
                a = albedoFloor;
                n = glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }
        // Back wall.
        if (rd.z < -1e-5f) {
            const float t = (-2.4f - ro.z) / rd.z;
            if (t > 0.0f && t < best) {
                best = t;
                a = albedoWall;
                n = glm::vec3(0.0f, 0.0f, 1.0f);
            }
        }
        // Spheres.
        for (const Sphere& sp : spheres) {
            const glm::vec3 oc = ro - sp.c;
            const float b = glm::dot(oc, rd);
            const float c = glm::dot(oc, oc) - sp.r * sp.r;
            const float disc = b * b - c;
            if (disc <= 0.0f) continue;
            const float t = -b - std::sqrt(disc);
            if (t > 1e-4f && t < best) {
                best = t;
                a = sp.albedo;
                n = glm::normalize(ro + rd * t - sp.c);
            }
        }
        if (best > 1e29f) return false;
        *tHit = best;
        *albedo = a;
        *nrm = n;
        return true;
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float ndcX = (2.0f * (float(x) + 0.5f) / float(width) - 1.0f) * tanHalf * (float(width) / float(height));
            const float ndcY = (1.0f - 2.0f * (float(y) + 0.5f) / float(height)) * tanHalf;
            const glm::vec3 rd = glm::normalize(fwd + right * ndcX + up * ndcY);
            float t = 0.0f;
            glm::vec3 albedo(0.0f), n(0.0f);
            glm::vec3 col;
            if (!trace(eye, rd, &t, &albedo, &n)) {
                // Background: a dark studio gradient, so an unlit corner is still a frame and not a hole.
                const float g = 0.5f * (1.0f - ndcY / tanHalf);
                col = glm::vec3(0.020f + 0.030f * g, 0.024f + 0.034f * g, 0.034f + 0.044f * g);
            } else {
                const glm::vec3 p = eye + rd * t;
                glm::vec3 c = albedo * ambient;
                for (int i = 0; i < count; ++i) {
                    // Contact shading: a surface facing away from a light gets nothing, which is the shadow the sun's
                    // cascades would give it if this were the real renderer.
                    c += shadeLight(packed[i], albedo, shadeTint, p, n, toony, shift);
                }
                // A faint distance falloff, so the floor recedes.
                col = c * (1.0f - 0.28f * std::clamp((t - 4.0f) / 9.0f, 0.0f, 1.0f));
            }
            blendPixel(img, x, y, glm::vec4(col, 1.0f));
        }
    }

    // The lights themselves, drawn where they are, as a glow with a hot core.
    auto project = [&](const glm::vec3& p) {
        const glm::vec3 d = p - eye;
        const float z = glm::dot(d, fwd);
        const float sx = glm::dot(d, right) / (z * tanHalf * (float(width) / float(height)));
        const float sy = glm::dot(d, up) / (z * tanHalf);
        return glm::vec2((sx * 0.5f + 0.5f) * float(width), (0.5f - sy * 0.5f) * float(height));
    };
    for (int i = 0; i < count; ++i) {
        const ukiyo::GpuLight& g = packed[i];
        const bool directional = g.params.x < 0.5f;
        const glm::vec3 lc = glm::vec3(g.colorIntensity) * g.colorIntensity.w;
        const glm::vec3 hot(std::min(1.0f, lc.r), std::min(1.0f, lc.g), std::min(1.0f, lc.b));
        if (directional) {
            // No position to draw: an arrow in the corner, the way a sun is drawn in a lighting diagram.
            const glm::vec2 at(150.0f, 300.0f);
            const glm::vec2 dir = project(glm::vec3(0.0f) + glm::vec3(0.35f, -1.0f, 0.45f) * 0.6f + eye) - project(eye);
            const glm::vec2 d = glm::normalize(dir);
            disc(img, at.x, at.y, 12.0f, glm::vec4(hot.r, hot.g, hot.b, 0.9f));
            for (float k = 0.0f; k < 1.0f; k += 0.04f)
                disc(img, at.x + d.x * 20.0f + d.x * k * 150.0f, at.y + d.y * 20.0f + d.y * k * 150.0f, 2.0f,
                     glm::vec4(hot.r, hot.g, hot.b, 0.85f - 0.35f * k));
            line(img, at.x + d.x * 100.0f + d.y * 8.0f, at.y + d.y * 100.0f - d.x * 8.0f,
                 at.x + d.x * 150.0f, at.y + d.y * 150.0f, glm::vec4(hot.r, hot.g, hot.b, 0.7f));
            line(img, at.x + d.x * 100.0f - d.y * 8.0f, at.y + d.y * 100.0f + d.x * 8.0f,
                 at.x + d.x * 150.0f, at.y + d.y * 150.0f, glm::vec4(hot.r, hot.g, hot.b, 0.7f));
        } else {
            const glm::vec2 at = project(glm::vec3(g.posRadius));
            for (int r = 46; r > 8; r -= 4)
                disc(img, at.x, at.y, float(r), glm::vec4(hot.r, hot.g, hot.b, 0.030f));
            disc(img, at.x, at.y, 7.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.95f));
            // A spot's cone, the two rays to where it meets the wall, so the angle is visible.
            if (g.params.x > 1.5f) {
                const glm::vec3 dir = glm::normalize(glm::vec3(g.dirCosOuter));
                const float half = std::acos(std::clamp(g.dirCosOuter.w, -1.0f, 1.0f));
                const glm::vec3 sideRef = std::fabs(dir.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                const glm::vec3 perp = glm::normalize(glm::cross(dir, sideRef));
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    const glm::vec3 edge = glm::normalize(dir * std::cos(half) + perp * (std::sin(half) * float(sgn)));
                    for (float k = 0.0f; k < 1.0f; k += 0.015f) {
                        const glm::vec2 e = project(glm::vec3(g.posRadius) + edge * (k * 2.3f));
                        disc(img, e.x, e.y, 1.5f, glm::vec4(hot.r, hot.g, hot.b, 0.26f * (1.0f - 0.5f * k)));
                    }
                }
                // The far arc, closing the cone where it ends.
                for (float a = -half; a <= half; a += 0.02f) {
                    const glm::vec3 e = glm::vec3(g.posRadius) +
                                        glm::normalize(dir * std::cos(a) + perp * std::sin(a)) * 2.3f;
                    const glm::vec2 q = project(e);
                    disc(img, q.x, q.y, 1.2f, glm::vec4(hot.r, hot.g, hot.b, 0.16f));
                }
            }
        }
    }

    // --- Labels --------------------------------------------------------------------------------------
    ukiyo::FontAtlas title, body;
    const bool haveTitle = title.loadFromFile(fontPath, 34.0f, 1024);
    const bool haveBody = body.loadFromFile(fontPath, 20.0f, 1024);
    if (haveTitle) title.preloadAscii();
    if (haveBody) body.preloadAscii();
    if (haveTitle && haveBody) {
        const Text head{&title}, text{&body};
        head.draw(img, 44.0f, 74.0f, "One scene, three lights, the shipping shader", glm::vec4(0.97f, 0.97f, 0.98f, 1.0f));
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "directional key  +  cool point light at (-2.62, 1.32, 1.15), reach 3.6 m  +  warm spot at (2.15, 1.95, 1.35)");
        text.draw(img, 46.0f, 108.0f, buf, glm::vec4(0.88f, 0.90f, 0.94f, 1.0f));
        text.draw(img, 46.0f, float(height) - 40.0f,
                  "every pixel is shaded with mtoon.frag's ramp, windowed inverse-square falloff and spot cone - 8 lights fit "
                  "the frame block", glm::vec4(0.80f, 0.83f, 0.88f, 1.0f));
        // A label beside each light marker.
        for (int i = 0; i < count; ++i) {
            const ukiyo::GpuLight& g = packed[i];
            const bool directional = g.params.x < 0.5f;
            const char* name = directional ? "the scene's directional light: the key, and the only shadow caster"
                             : g.params.x > 1.5f ? "spot light, cone 46 deg, 35 % soft"
                                                 : "point light, reach 3.6 m";
            const glm::vec2 at = directional ? glm::vec2(150.0f, 300.0f) : project(glm::vec3(g.posRadius));
            // Left of the marker when there is room, so a label never sits on a sphere.
            const float lw = 12.0f * float(name ? std::char_traits<char>::length(name) : 0);
            const bool left = !directional && at.x + 20.0f + lw > float(width) - 20.0f;
            text.draw(img, left ? at.x - 20.0f - lw : at.x + 20.0f, at.y + 6.0f, name,
                      glm::vec4(1.0f, 1.0f, 1.0f, 0.94f));
        }
    } else {
        std::printf("lights_preview: no font at %s, writing the figure unlabelled\n", fontPath.c_str());
    }

    if (!stbi_write_png(out.c_str(), img.w, img.h, 4, img.px.data(), img.w * 4)) {
        std::printf("lights_preview: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("lights_preview: wrote %s (%dx%d)\n", out.c_str(), img.w, img.h);
    return 0;
}
