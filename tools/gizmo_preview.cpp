// Draws the Scene view's three gizmo tools and the corner view gizmo into a PNG, using the same geometry the editor
// uses (src/engine/gizmo_math.cpp) and the same layout rules as editor/src/scene_gizmo.cpp: the handle length, the
// plane squares, the near/far split of a ring, Blender's visibility rule, and the back-to-front order of the view
// widget. It is a drawing of the handles, not a screenshot of the editor: it lets the shape of the thing be reviewed
// in a diff, and it stops building if the math file's API drifts.
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_image_write.h"

#include "engine/camera.h"
#include "engine/gizmo_math.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kSuper = 2;                 // supersampling: draw big, shrink once at the end
constexpr double kPanel = 440.0;          // each tool is drawn in a square view, then placed side by side
constexpr double kHandleFraction = 0.18;  // of the view height, as in editor/src/scene_gizmo.cpp

struct Colour {
    unsigned char r, g, b;
};

const Colour kBackground{27, 27, 31};
const Colour kRule{44, 44, 49};
const Colour kText{232, 232, 234};
const Colour kFaint{106, 106, 114};

Colour axisColour(int axis) {
    if (axis == 0) return {229, 72, 77};
    if (axis == 1) return {70, 167, 88};
    return {62, 99, 221};
}

// Every primitive below takes logical pixels; the buffer is kSuper times bigger and each coordinate is scaled on the
// way in, so the callers can talk about the picture they want and not about the supersampling.
struct Canvas {
    int w = 0, h = 0;
    std::vector<unsigned char> px;

    static double s(double v) { return v * double(kSuper); }

    void init(int width, int height, Colour fill) {
        w = width * kSuper;
        h = height * kSuper;
        px.assign(size_t(w) * size_t(h) * 3, 0);
        for (size_t i = 0; i < px.size(); i += 3) {
            px[i] = fill.r;
            px[i + 1] = fill.g;
            px[i + 2] = fill.b;
        }
    }

    // Logical pixels in, for the odd caller that thinks in the final picture.
    void blend(double x, double y, Colour c, double a) { blendRaw(int(s(x)), int(s(y)), c, a); }

    void blendRaw(int x, int y, Colour c, double a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0) return;
        const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 3;
        const double k = std::clamp(a, 0.0, 1.0);
        px[i] = (unsigned char)(px[i] * (1.0 - k) + c.r * k);
        px[i + 1] = (unsigned char)(px[i + 1] * (1.0 - k) + c.g * k);
        px[i + 2] = (unsigned char)(px[i + 2] * (1.0 - k) + c.b * k);
    }

    // A line with a round cap: a pixel is covered by the distance from its centre to the segment.
    void line(double x0, double y0, double x1, double y1, Colour c, double width, double alpha = 1.0) {
        x0 = s(x0); y0 = s(y0); x1 = s(x1); y1 = s(y1);
        const double wid = s(width);
        const double pad = wid * 0.5 + 1.0;
        const int xa = int(std::floor(std::min(x0, x1) - pad)), xb = int(std::ceil(std::max(x0, x1) + pad));
        const int ya = int(std::floor(std::min(y0, y1) - pad)), yb = int(std::ceil(std::max(y0, y1) + pad));
        const double dx = x1 - x0, dy = y1 - y0;
        const double len2 = dx * dx + dy * dy;
        for (int y = ya; y <= yb; ++y) {
            for (int x = xa; x <= xb; ++x) {
                double t = len2 > 1e-9 ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0.0;
                t = std::clamp(t, 0.0, 1.0);
                const double qx = x0 + dx * t, qy = y0 + dy * t;
                const double d = std::hypot(x - qx, y - qy);
                const double cover = std::clamp(wid * 0.5 + 0.5 - d, 0.0, 1.0);
                blendRaw(x, y, c, cover * alpha);
            }
        }
    }

    void disc(double cx, double cy, double r, Colour c, double alpha = 1.0) {
        cx = s(cx); cy = s(cy); r = s(r);
        for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y) {
            for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
                const double d = std::hypot(x - cx, y - cy);
                blendRaw(x, y, c, std::clamp(r + 0.5 - d, 0.0, 1.0) * alpha);
            }
        }
    }

    void rect(double cx, double cy, double half, Colour c, double alpha = 1.0) {
        cx = s(cx); cy = s(cy); half = s(half);
        for (int y = int(cy - half - 1); y <= int(cy + half + 1); ++y) {
            for (int x = int(cx - half - 1); x <= int(cx + half + 1); ++x) {
                const double dx = std::max(std::fabs(x - cx) - half, 0.0);
                const double dy = std::max(std::fabs(y - cy) - half, 0.0);
                blendRaw(x, y, c, std::clamp(1.0 - std::hypot(dx, dy), 0.0, 1.0) * alpha);
            }
        }
    }

    // A filled polygon by the even-odd rule, with one-pixel softening on the edges.
    void poly(const std::vector<glm::dvec2>& pts, Colour c, double alpha) {
        if (pts.size() < 3) return;
        std::vector<glm::dvec2> q;
        q.reserve(pts.size());
        for (const glm::dvec2& p : pts) q.push_back({s(p.x), s(p.y)});
        const std::vector<glm::dvec2>& pts2 = q;
        double xa = pts2[0].x, xb = pts2[0].x, ya = pts2[0].y, yb = pts2[0].y;
        for (const glm::dvec2& p : pts2) {
            xa = std::min(xa, p.x);
            xb = std::max(xb, p.x);
            ya = std::min(ya, p.y);
            yb = std::max(yb, p.y);
        }
        for (int y = int(ya); y <= int(yb) + 1; ++y) {
            for (int x = int(xa); x <= int(xb) + 1; ++x) {
                bool in = false;
                for (size_t i = 0, j = pts2.size() - 1; i < pts2.size(); j = i++) {
                    const bool up = (pts2[i].y > y) != (pts2[j].y > y);
                    if (up && x < (pts2[j].x - pts2[i].x) * (y - pts2[i].y) / (pts2[j].y - pts2[i].y) + pts2[i].x)
                        in = !in;
                }
                if (in) blendRaw(x, y, c, alpha);
            }
        }
    }

    void save(const char* path) {
        // Box-downsample the supersampled buffer.
        const int ow = w / kSuper, oh = h / kSuper;
        std::vector<unsigned char> out(size_t(ow) * size_t(oh) * 3);
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                int r = 0, g = 0, b = 0;
                for (int sy = 0; sy < kSuper; ++sy) {
                    for (int sx = 0; sx < kSuper; ++sx) {
                        const size_t i = (size_t(y * kSuper + sy) * size_t(w) + size_t(x * kSuper + sx)) * 3;
                        r += px[i];
                        g += px[i + 1];
                        b += px[i + 2];
                    }
                }
                const size_t o = (size_t(y) * size_t(ow) + size_t(x)) * 3;
                out[o] = (unsigned char)(r / (kSuper * kSuper));
                out[o + 1] = (unsigned char)(g / (kSuper * kSuper));
                out[o + 2] = (unsigned char)(b / (kSuper * kSuper));
            }
        }
        stbi_write_png(path, ow, oh, 3, out.data(), ow * 3);
    }
};

// Text, from the font the editor ships, with a baseline at `y`.
struct Face {
    stbtt_fontinfo info{};
    std::vector<unsigned char> data;

    bool load(const char* path) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        data.resize(size_t(size));
        const bool ok = std::fread(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        return ok && stbtt_InitFont(&info, data.data(), 0) != 0;
    }

    double widthOf(const std::string& s, double px) const {
        const double scale = stbtt_ScaleForPixelHeight(&info, float(px));
        int advance = 0, lsb = 0;
        double total = 0.0;
        for (char ch : s) {
            stbtt_GetCodepointHMetrics(&info, ch, &advance, &lsb);
            total += advance * scale;
        }
        return total;
    }

    void draw(Canvas& c, double x, double baseline, double px, const std::string& s, Colour colour) const {
        x = x * double(kSuper);
        baseline = baseline * double(kSuper);
        const double scale = stbtt_ScaleForPixelHeight(&info, float(px)) * double(kSuper);
        double pen = x;
        for (char ch : s) {
            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char* bitmap = stbtt_GetCodepointBitmap(&info, 0, float(scale), ch, &w, &h, &xoff, &yoff);
            for (int row = 0; row < h; ++row)
                for (int col = 0; col < w; ++col) {
                    const unsigned char a = bitmap[row * w + col];
                    if (a) c.blendRaw(int(pen) + xoff + col, int(baseline) + yoff + row, colour, a / 255.0);
                }
            stbtt_FreeBitmap(bitmap, nullptr);
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&info, ch, &advance, &lsb);
            pen += advance * scale;
        }
    }
};

ukiyo::OrbitCamera sceneCam() {
    ukiyo::OrbitCamera cam;
    cam.target = glm::vec3(0.0f);
    cam.yaw = 0.62f;
    cam.pitch = 0.34f;
    cam.distance = 4.0f;
    return cam;
}

// Screen position inside a panel, in supersampled pixels: `offset` is the panel's left edge.
glm::dvec2 project(const ukiyo::OrbitCamera& cam, const glm::vec3& p, double offset) {
    const glm::vec4 clip = cam.proj(1.0f) * cam.view() * glm::vec4(p, 1.0f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return {offset + (ndc.x * 0.5 + 0.5) * kPanel, (0.5 - ndc.y * 0.5) * kPanel};
}

float handleLength(const ukiyo::OrbitCamera& cam) {
    const float dist = glm::length(cam.target - cam.eye());
    return float(kHandleFraction * 2.0f * dist * std::tan(cam.fovY * 0.5f));
}

glm::vec3 axisDir(int axis) {
    if (axis == 0) return {1, 0, 0};
    if (axis == 1) return {0, 1, 0};
    return {0, 0, 1};
}

glm::vec3 planeNormal(int plane) {
    if (plane == 0) return {1, 0, 0};   // YZ
    if (plane == 1) return {0, 1, 0};   // ZX
    return {0, 0, 1};                   // XY
}

void caption(Canvas& c, const Face& face, double centreX, double y, const std::string& title,
             const std::string& subtitle) {
    face.draw(c, centreX - face.widthOf(title, 19.0) * 0.5, y, 19.0, title, kText);
    face.draw(c, centreX - face.widthOf(subtitle, 12.0) * 0.5, y + 22.0, 12.0, subtitle, kFaint);
}

void drawMove(Canvas& c, const ukiyo::OrbitCamera& cam, double offset) {
    const float len = handleLength(cam);
    const glm::vec3 viewDir = cam.forward();
    for (int plane = 0; plane < 3; ++plane) {
        const glm::vec3 n = planeNormal(plane);
        if (!ukiyo::gizmo::axisVisible(viewDir, n, true)) continue;
        const glm::vec3 a = std::abs(n.x) > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 b = glm::cross(n, a);
        const float off = 0.34f * len;
        const float half = len * 0.09f;
        const glm::vec3 k = (a + b) * off;
        std::vector<glm::dvec2> quad;
        for (const glm::vec3& corner :
             {k - a * half - b * half, k + a * half - b * half, k + a * half + b * half, k - a * half + b * half})
            quad.push_back(project(cam, corner, offset));
        c.poly(quad, axisColour(plane), 0.28);
        for (size_t i = 0; i < 4; ++i)
            c.line(quad[i].x, quad[i].y, quad[(i + 1) % 4].x, quad[(i + 1) % 4].y, axisColour(plane), 2.0);
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!ukiyo::gizmo::axisVisible(viewDir, axisDir(axis), false)) continue;
        const glm::dvec2 a = project(cam, glm::vec3(0.0f), offset);
        const glm::dvec2 b = project(cam, axisDir(axis) * len, offset);
        c.line(a.x, a.y, b.x, b.y, axisColour(axis), 2.6);
        double dx = b.x - a.x, dy = b.y - a.y;
        const double n = std::hypot(dx, dy);
        if (n < 1e-3) continue;
        dx /= n;
        dy /= n;
        const glm::dvec2 s1{b.x - dx * 15.0 - dy * 6.0, b.y - dy * 15.0 + dx * 6.0};
        const glm::dvec2 s2{b.x - dx * 15.0 + dy * 6.0, b.y - dy * 15.0 - dx * 6.0};
        c.poly({b, s1, s2}, axisColour(axis), 1.0);
    }
}

void drawRotate(Canvas& c, const ukiyo::OrbitCamera& cam, double offset) {
    const float len = handleLength(cam);
    const glm::vec3 viewDir = cam.forward();
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 a = axisDir(axis);
        const glm::vec3 u = ukiyo::gizmo::perpendicular(a);
        const glm::vec3 v = glm::cross(a, u);
        glm::dvec2 prev{};
        bool have = false, prevFront = true;
        for (int i = 0; i <= 128; ++i) {
            const float t = 6.2831853f * float(i) / 128.0f;
            const glm::vec3 world = len * (std::cos(t) * u + std::sin(t) * v);
            const glm::dvec2 s = project(cam, world, offset);
            const bool front = glm::dot(world, viewDir) <= 0.0f;
            if (have && front == prevFront) c.line(prev.x, prev.y, s.x, s.y, axisColour(axis), front ? 2.6 : 1.6);
            prev = s;
            prevFront = front;
            have = true;
        }
    }
}

void drawScale(Canvas& c, const ukiyo::OrbitCamera& cam, double offset) {
    const float len = handleLength(cam);
    const glm::vec3 viewDir = cam.forward();
    for (int axis = 0; axis < 3; ++axis) {
        if (!ukiyo::gizmo::axisVisible(viewDir, axisDir(axis), false)) continue;
        const glm::dvec2 a = project(cam, glm::vec3(0.0f), offset);
        const glm::dvec2 b = project(cam, axisDir(axis) * len, offset);
        c.line(a.x, a.y, b.x, b.y, axisColour(axis), 2.6);
        c.rect(b.x, b.y, 5.5, axisColour(axis));
    }
    const glm::dvec2 centre = project(cam, glm::vec3(0.0f), offset);
    c.rect(centre.x, centre.y, 9.0, Colour{200, 200, 200});
}

// The corner widget: six balls, back to front, mixed toward the view background by depth.
void drawViewWidget(Canvas& c, const Face& face, const ukiyo::OrbitCamera& cam, double cx, double cy) {
    struct Ball {
        int index;
        glm::dvec2 at;
        float depth;
    };
    std::vector<Ball> balls;
    const glm::vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const glm::mat3 rot(cam.view());
    const double r = 36.0;
    for (int i = 0; i < 6; ++i) {
        const glm::vec3 v = rot * dirs[i];
        balls.push_back({i, {cx + double(v.x) * r, cy - double(v.y) * r}, float(-v.z)});
    }
    std::sort(balls.begin(), balls.end(), [](const Ball& a, const Ball& b) { return a.depth < b.depth; });
    for (const Ball& b : balls) {
        const double fade = 0.3 + 0.7 * std::clamp((double(b.depth) + 1.0) * 0.5, 0.0, 1.0);
        const Colour base = axisColour(b.index / 2);
        const Colour mixed{(unsigned char)(kBackground.r + (base.r - kBackground.r) * fade),
                           (unsigned char)(kBackground.g + (base.g - kBackground.g) * fade),
                           (unsigned char)(kBackground.b + (base.b - kBackground.b) * fade)};
        if (b.index % 2 == 0) c.line(cx, cy, b.at.x, b.at.y, mixed, 2.0);
        c.disc(b.at.x, b.at.y, 13.0, mixed);
        const char* label = b.index == 0 ? "X" : b.index == 2 ? "Y" : b.index == 4 ? "Z" : "";
        if (label[0] != '\0') face.draw(c, b.at.x - face.widthOf(label, 13.0) * 0.5, b.at.y + 4.5, 13.0, label,
                                        Colour{18, 18, 20});
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string outPath = "gizmo.png";
    std::string fontPath = "editor/res/fonts/IBMPlexSansJP-Regular.ttf";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (arg == "--font" && i + 1 < argc) fontPath = argv[++i];
    }
    Face face;
    if (!face.load(fontPath.c_str())) {
        std::fprintf(stderr, "gizmo_preview: cannot read the font %s\n", fontPath.c_str());
        return 1;
    }

    const ukiyo::OrbitCamera cam = sceneCam();
    Canvas c;
    c.init(int(kPanel * 3.0), int(kPanel), kBackground);
    for (int i = 1; i < 3; ++i) c.line(kPanel * i, 0.0, kPanel * i, kPanel, kRule, 1.4);

    drawMove(c, cam, 0.0);
    drawRotate(c, cam, kPanel);
    drawScale(c, cam, kPanel * 2.0);
    drawViewWidget(c, face, cam, kPanel - 64.0, 64.0);

    caption(c, face, kPanel * 0.5, 372.0, "Move", "arrows and plane squares");
    caption(c, face, kPanel * 1.5, 372.0, "Rotate", "one ring per axis, far half faint");
    caption(c, face, kPanel * 2.5, 372.0, "Scale", "per-axis cubes and a centre cube");
    face.draw(c, 14.0, kPanel - 12.0, 11.0, "handles drawn by src/engine/gizmo_math.cpp", kFaint);

    c.save(outPath.c_str());
    std::printf("gizmo_preview: wrote %s\n", outPath.c_str());
    return 0;
}
