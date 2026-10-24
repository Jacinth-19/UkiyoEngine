#include "engine/msdf.h"

#include <algorithm>
#include <cmath>
#include <limits>

// No STB_TRUETYPE_IMPLEMENTATION here: text_atlas.cpp defines it once per program, and this file only calls it. Every
// target that links this one links that one too (see CMakeLists.txt), which keeps the implementation in one place
// instead of duking it out with the linker.
#include "stb_truetype.h"

namespace ukiyo::msdf {
namespace {

constexpr int kRed = 1;
constexpr int kGreen = 2;
constexpr int kBlue = 4;
// A pair of channels per run of edges. Two runs that meet at a corner always differ in at least one channel, which is
// what lets the median see the corner.
constexpr int kPairs[3] = {kRed | kGreen, kGreen | kBlue, kRed | kBlue};
// Flattening tolerance, in pixels at the generator's scale. Well under a texel: a curve that stays this close to its
// chords is indistinguishable from them once the field is filtered.
constexpr double kFlattenTolerance = 0.03;

struct V2 {
    double x = 0.0, y = 0.0;
};

struct Edge {
    V2 a, b;
    int colour = kPairs[0];
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;

    void bounds() {
        minX = std::min(a.x, b.x);
        maxX = std::max(a.x, b.x);
        minY = std::min(a.y, b.y);
        maxY = std::max(a.y, b.y);
    }
};

double distanceSquared(const V2& a, const V2& b) {
    const double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Point to segment. For a distance field this is the right metric on both sides of an edge: outside it is the distance
// to the nearest point of the outline, and inside, near a join, it is the distance to the corner rather than to the
// infinite line, which would push the field outward through the corner.
double distanceToSegment(const V2& p, const V2& a, const V2& b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 1e-18) return std::sqrt(distanceSquared(p, a));
    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = std::clamp(t, 0.0, 1.0);
    return std::sqrt(distanceSquared(p, V2{a.x + t * dx, a.y + t * dy}));
}

void flattenQuadratic(const V2& p0, const V2& c, const V2& p1, double tolerance, std::vector<V2>* out, int depth = 0) {
    // Flat enough when the control point is within tolerance of the chord; otherwise split at the parameter midpoint.
    const double dx = p1.x - p0.x, dy = p1.y - p0.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (depth > 16 || len <= 1e-12) {
        out->push_back(p1);
        return;
    }
    const double cross = std::fabs((c.x - p0.x) * dy - (c.y - p0.y) * dx) / len;
    if (cross <= tolerance) {
        out->push_back(p1);
        return;
    }
    const V2 a{(p0.x + c.x) * 0.5, (p0.y + c.y) * 0.5};
    const V2 b{(c.x + p1.x) * 0.5, (c.y + p1.y) * 0.5};
    const V2 m{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
    flattenQuadratic(p0, a, m, tolerance, out, depth + 1);
    flattenQuadratic(m, b, p1, tolerance, out, depth + 1);
}

void flattenCubic(const V2& p0, const V2& c0, const V2& c1, const V2& p1, double tolerance, std::vector<V2>* out,
                  int depth = 0) {
    const double dx = p1.x - p0.x, dy = p1.y - p0.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (depth > 16 || len <= 1e-12) {
        out->push_back(p1);
        return;
    }
    const double d0 = std::fabs((c0.x - p0.x) * dy - (c0.y - p0.y) * dx) / len;
    const double d1 = std::fabs((c1.x - p0.x) * dy - (c1.y - p0.y) * dx) / len;
    if (std::max(d0, d1) <= tolerance) {
        out->push_back(p1);
        return;
    }
    const V2 a{(p0.x + c0.x) * 0.5, (p0.y + c0.y) * 0.5};
    const V2 b{(c0.x + c1.x) * 0.5, (c0.y + c1.y) * 0.5};
    const V2 c{(c1.x + p1.x) * 0.5, (c1.y + p1.y) * 0.5};
    const V2 ab{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
    const V2 bc{(b.x + c.x) * 0.5, (b.y + c.y) * 0.5};
    const V2 m{(ab.x + bc.x) * 0.5, (ab.y + bc.y) * 0.5};
    flattenCubic(p0, a, ab, m, tolerance, out, depth + 1);
    flattenCubic(m, bc, c, p1, tolerance, out, depth + 1);
}

// The turn at the junction of two edges, in degrees. A flattened curve's consecutive chords turn by a fraction of a
// degree, so anything above the threshold is a real corner.
double turnDegrees(const V2& before, const V2& at, const V2& after) {
    const V2 in{at.x - before.x, at.y - before.y};
    const V2 out{after.x - at.x, after.y - at.y};
    const double li = std::sqrt(in.x * in.x + in.y * in.y);
    const double lo = std::sqrt(out.x * out.x + out.y * out.y);
    if (li <= 1e-12 || lo <= 1e-12) return 0.0;
    const double c = std::clamp((in.x * out.x + in.y * out.y) / (li * lo), -1.0, 1.0);
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}

// Assigns a colour pair to each edge. Within a smooth run every edge shares a pair, so the median is continuous across
// it; at a corner the pair changes, so the median keeps the corner instead of averaging it away.
void colourContour(std::vector<Edge>& edges, size_t first, size_t count, double angleThreshold) {
    if (count == 0) return;
    // Where the contour turns sharply. A corner at index i is the junction between edge i-1 and edge i (cyclically).
    std::vector<bool> corner(count, false);
    int corners = 0;
    for (size_t i = 0; i < count; ++i) {
        const Edge& prev = edges[first + (i + count - 1) % count];
        const Edge& cur = edges[first + i];
        if (turnDegrees(prev.a, prev.b, cur.b) > angleThreshold) {
            corner[i] = true;
            ++corners;
        }
    }
    if (corners == 0) {
        // A smooth closed contour: one colour for all of it. Splitting it would put two channels on the same edge.
        for (size_t i = 0; i < count; ++i) edges[first + i].colour = kPairs[0];
        return;
    }
    // Walk the runs between corners, cycling the pair per run so consecutive runs always differ in a channel.
    int run = 0;
    size_t i = 0;
    while (i < count && !corner[i]) ++i;   // start at a corner, so the first run is not split by the wrap
    const size_t start = i % count;
    for (size_t k = 0; k < count; ++k) {
        const size_t at = (start + k) % count;
        edges[first + at].colour = kPairs[run % 3];
        const size_t next = (at + 1) % count;
        if (corner[next]) ++run;
    }
}

// Non-zero winding test on the flattened outline: a glyph's counters (the hole in an "o") wind the other way round, so
// a simple even-odd count would fill them in.
bool inside(const std::vector<Edge>& edges, const V2& p) {
    int winding = 0;
    for (const Edge& e : edges) {
        if ((e.a.y <= p.y && e.b.y > p.y) || (e.b.y <= p.y && e.a.y > p.y)) {
            const double t = (p.y - e.a.y) / (e.b.y - e.a.y);
            const double x = e.a.x + t * (e.b.x - e.a.x);
            if (x > p.x) winding += e.b.y > e.a.y ? 1 : -1;
        }
    }
    return winding != 0;
}

} // namespace

bool outline(const uint8_t* ttf, size_t size, int glyphIndex, double scale, std::vector<Segment>* out) {
    out->clear();
    if (!ttf || size == 0 || glyphIndex <= 0 || scale <= 0.0) return false;
    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, ttf, 0)) return false;
    stbtt_vertex* verts = nullptr;
    const int n = stbtt_GetGlyphShape(&font, glyphIndex, &verts);
    if (n <= 0 || !verts) return false;

    V2 pen{0.0, 0.0}, contourBegin{0.0, 0.0};
    bool open = false;
    auto closeContour = [&]() {
        if (!open) return;
        if (std::fabs(pen.x - contourBegin.x) > 1e-9 || std::fabs(pen.y - contourBegin.y) > 1e-9)
            out->push_back(Segment{pen.x, pen.y, contourBegin.x, contourBegin.y});
        open = false;
    };
    auto addCurve = [&](const std::vector<V2>& points) {
        V2 from = pen;
        for (const V2& q : points) {
            out->push_back(Segment{from.x, from.y, q.x, q.y});
            from = q;
        }
    };
    for (int i = 0; i < n; ++i) {
        const stbtt_vertex& v = verts[i];
        const V2 p{v.x * scale, v.y * scale};
        if (v.type == STBTT_vmove) {
            closeContour();
            pen = contourBegin = p;
            open = true;
        } else if (v.type == STBTT_vline) {
            out->push_back(Segment{pen.x, pen.y, p.x, p.y});
            pen = p;
        } else if (v.type == STBTT_vcurve) {
            std::vector<V2> points;
            flattenQuadratic(pen, V2{v.cx * scale, v.cy * scale}, p, kFlattenTolerance, &points);
            addCurve(points);
            pen = p;
        } else if (v.type == STBTT_vcubic) {
            std::vector<V2> points;
            flattenCubic(pen, V2{v.cx * scale, v.cy * scale}, V2{v.cx1 * scale, v.cy1 * scale}, p, kFlattenTolerance,
                         &points);
            addCurve(points);
            pen = p;
        }
    }
    closeContour();
    stbtt_FreeShape(&font, verts);
    return !out->empty();
}

double signedDistance(const std::vector<Segment>& outline, double x, double y) {
    const V2 p{x, y};
    double best = std::numeric_limits<double>::max();
    int winding = 0;
    for (const Segment& s : outline) {
        const V2 a{s.ax, s.ay}, b{s.bx, s.by};
        best = std::min(best, distanceToSegment(p, a, b));
        if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
            const double t = (y - a.y) / (b.y - a.y);
            if (a.x + t * (b.x - a.x) > x) winding += b.y > a.y ? 1 : -1;
        }
    }
    return winding != 0 ? best : -best;
}

bool build(const uint8_t* ttf, size_t size, int glyphIndex, double scale, const Params& params, Field* out) {
    if (!out) return false;
    out->width = out->height = 0;
    out->pixels.clear();

    std::vector<Segment> segments;
    if (!outline(ttf, size, glyphIndex, scale, &segments)) return false;

    // Colour per contour on its own: two contours never meet, so they may start from the same pair. The contour breaks
    // are the segments that jump, which flattening never produces within one contour.
    std::vector<Edge> edges;
    edges.reserve(segments.size());
    for (const Segment& s : segments) {
        Edge e{{s.ax, s.ay}, {s.bx, s.by}};
        e.bounds();
        edges.push_back(e);
    }
    size_t first = 0;
    for (size_t i = 0; i < edges.size(); ++i) {
        const Edge& e = edges[i];
        const Edge& prev = edges[i == 0 ? edges.size() - 1 : i - 1];
        if (i > first && (std::fabs(e.a.x - prev.b.x) > 1e-6 || std::fabs(e.a.y - prev.b.y) > 1e-6)) {
            colourContour(edges, first, i - first, params.angleThreshold);
            first = i;
        }
    }
    colourContour(edges, first, edges.size() - first, params.angleThreshold);

    // The cell is the outline's own bounding box, measured from the flattened segments so that it cannot disagree with
    // the geometry that is about to be fielded.
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (const Edge& e : edges) {
        minX = std::min({minX, e.a.x, e.b.x});
        minY = std::min({minY, e.a.y, e.b.y});
        maxX = std::max({maxX, e.a.x, e.b.x});
        maxY = std::max({maxY, e.a.y, e.b.y});
    }
    if (maxX <= minX || maxY <= minY) return false;

    const double range = std::max(params.range, 0.5);
    const int left = static_cast<int>(std::floor(minX - range));
    const int right = static_cast<int>(std::ceil(maxX + range));
    const int top = static_cast<int>(std::ceil(maxY + range));       // font y of the cell's first row's top edge
    const int bottom = static_cast<int>(std::floor(minY - range));   // font y of the last row's bottom edge
    const int width = right - left, height = top - bottom;
    if (width <= 0 || height <= 0) return false;

    out->width = width;
    out->height = height;
    out->xoff = left;
    out->yoff = -top;
    out->pixels.assign(static_cast<size_t>(width) * height * 4, 0);

    const int supersample = std::clamp(params.supersample, 1, 4);
    const double step = 1.0 / supersample;
    // Nothing outside `range` changes the encoded value: a texel farther than that from every edge is fully inside or
    // fully outside, and the encode below clamps it either way. So an edge whose bounds are farther than that from a
    // texel cannot contribute anything, and most edges in a glyph are that far from most texels. This one test is what
    // makes the generator fast enough to run at load time rather than in an offline step.
    const double useful = range * 1.05 + 1.0;
    // Two passes over the cell: the field per channel, then the bytes. Keeping the accumulation in doubles avoids the
    // rounding that would otherwise show up as banding when the cell is magnified.
    struct Sample {
        double r, g, b, cover;
    };
    std::vector<Sample> acc(static_cast<size_t>(width) * height);
    const double inf = std::numeric_limits<double>::max();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumCover = 0.0;
            for (int sy = 0; sy < supersample; ++sy) {
                for (int sx = 0; sx < supersample; ++sx) {
                    // Cell (x, y) samples font space with y up: column x starts at `left`, and row y's top edge is at
                    // font y = -yoff, so the sample walks down from there.
                    const V2 p{left + x + (sx + 0.5) * step, -out->yoff - (y + (sy + 0.5) * step)};
                    double dR = inf, dG = inf, dB = inf;
                    for (const Edge& e : edges) {
                        const double dx = std::max({e.minX - p.x, 0.0, p.x - e.maxX});
                        const double dy = std::max({e.minY - p.y, 0.0, p.y - e.maxY});
                        const double bound = dx * dx + dy * dy;
                        if (bound >= useful * useful) continue;
                        // And once it is close enough to matter, a channel that already has a nearer edge can skip it.
                        const bool wantR = (e.colour & kRed) && bound < dR * dR;
                        const bool wantG = (e.colour & kGreen) && bound < dG * dG;
                        const bool wantB = (e.colour & kBlue) && bound < dB * dB;
                        if (!wantR && !wantG && !wantB) continue;
                        const double d = distanceToSegment(p, e.a, e.b);
                        if (wantR && d < dR) dR = d;
                        if (wantG && d < dG) dG = d;
                        if (wantB && d < dB) dB = d;
                    }
                    const bool in = inside(edges, p);
                    // Encode: 0.5 is the outline, and the sign is flipped so that inside reads above 0.5. A channel
                    // with no edge of its colour inside `useful` (the middle of a flat run, or a texel well away from
                    // the glyph) takes the far value, which is exactly what the clamp would have produced.
                    auto encode = [&](double d) {
                        if (d == inf) return in ? 1.0 : 0.0;
                        const double sd = in ? -d : d;
                        return std::clamp(0.5 - sd / (2.0 * range), 0.0, 1.0);
                    };
                    const double er = encode(dR), eg = encode(dG), eb = encode(dB);
                    sumR += er;
                    sumG += eg;
                    sumB += eb;
                    sumCover += median(er, eg, eb);
                }
            }
            const double inv = 1.0 / double(supersample * supersample);
            acc[static_cast<size_t>(y) * width + x] = {sumR * inv, sumG * inv, sumB * inv, sumCover * inv};
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t at = static_cast<size_t>(y) * width + x;
            uint8_t* px = out->pixels.data() + at * 4;
            px[0] = static_cast<uint8_t>(std::lround(std::clamp(acc[at].r, 0.0, 1.0) * 255.0));
            px[1] = static_cast<uint8_t>(std::lround(std::clamp(acc[at].g, 0.0, 1.0) * 255.0));
            px[2] = static_cast<uint8_t>(std::lround(std::clamp(acc[at].b, 0.0, 1.0) * 255.0));
            px[3] = static_cast<uint8_t>(std::lround(std::clamp(acc[at].cover, 0.0, 1.0) * 255.0));
        }
    }
    return true;
}

} // namespace ukiyo::msdf
