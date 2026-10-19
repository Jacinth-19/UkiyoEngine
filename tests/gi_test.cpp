// Real-time GI: the estimator's maths, on the CPU (docs/PATHTRACE.md §4).
//
// The pass itself is GLSL, and CI has no GPU: shaders/gi_trace.frag is compiled by glslang and audited for bindings,
// but nothing there proves the *algorithm* is right. This test mirrors the shader's march, its hemisphere model and
// its accumulation weight line for line, and checks the properties the image depends on:
//
//   1. the march lands where the ray actually crosses the heightfield (a flat plane has an analytic answer),
//   2. a ray that leaves the hemisphere's sky is not reported as a hit, and a ray in a pit is,
//   3. the cosine-weighted mean of the sky model equals the hemisphere ambient the terrain shader already added — the
//      identity that makes an open flat surface come out at zero instead of double-counting the sky,
//   4. a flat, open surface therefore produces (almost exactly) no delta, while a pit produces a negative one,
//   5. the accumulation weight behaves like the sixteen-frame history the doc claims: a reset frame is the sample,
//      and a constant signal converges to itself.
//
// Numbers here are metres and linear radiance, the units the shader works in. Nothing in this file touches the RHI.
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool ok, const char* what, double got, double want, double tol) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %-58s got %.6f want %.6f (tol %.6f)\n", what, got, want, tol);
    } else {
        std::printf("ok    %-58s %.6f\n", what, got);
    }
}

void checkNear(double got, double want, double tol, const char* what) {
    check(std::fabs(got - want) <= tol, what, got, want, tol);
}

void checkTrue(bool ok, const char* what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("ok    %s\n", what);
    }
}

// ---------------------------------------------------------------- deterministic noise
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 32);
    }
    float unit() { return float(next() & 0xffffff) / float(0x1000000); }
};

// ---------------------------------------------------------------- the heightfield
// Mirrors the texture the shader samples: size x size texels, R = height normalised to `scale`, bilinear between
// texel centres, clamped at the edges. `base` is the tile origin's y (gi_trace.frag pc.p2.z).
struct Field {
    int size = 0;
    float cell = 1.0f;
    float scale = 1.0f;
    float base = 0.0f;
    std::vector<float> h;   // normalised 0..1, sample-major

    float sampleAt(int x, int z) const {
        x = x < 0 ? 0 : (x >= size ? size - 1 : x);
        z = z < 0 ? 0 : (z >= size ? size - 1 : z);
        return h[size_t(z) * size_t(size) + size_t(x)];
    }
    // Metres, at a point in metres from the first sample. Texel i is centred at (i + 0.5) * cell.
    float heightMetres(float x, float z) const {
        const float fx = x / cell - 0.5f, fz = z / cell - 0.5f;
        const float x0f = std::floor(fx), z0f = std::floor(fz);
        const int x0 = int(x0f), z0 = int(z0f);
        const float tx = fx - x0f, tz = fz - z0f;
        const float a = sampleAt(x0, z0), b = sampleAt(x0 + 1, z0);
        const float c = sampleAt(x0, z0 + 1), d = sampleAt(x0 + 1, z0 + 1);
        const float n = (a * (1.0f - tx) + b * tx) * (1.0f - tz) + (c * (1.0f - tx) + d * tx) * tz;
        return base + n * scale;
    }
    glm::vec3 normalAt(float x, float z) const {
        const float e = 0.35f;
        const float hl = heightMetres(x - e, z), hr = heightMetres(x + e, z);
        const float hd = heightMetres(x, z - e), hu = heightMetres(x, z + e);
        return glm::normalize(glm::vec3(hl - hr, 2.0f * e, hd - hu));
    }
};

// ---------------------------------------------------------------- the shader's march
// Exactly gi_trace.frag's loop: a step bounded by how far the ray can fall before reaching the ground, capped, and
// eight bisection refinements once it crosses.
bool march(const Field& f, glm::vec3 ro, glm::vec3 rd, float tmax, float* tout, glm::vec3* nout) {
    const float extent = f.cell * float(f.size - 1);
    float t = 0.02f;
    for (int i = 0; i < 48; ++i) {
        if (t >= tmax) return false;
        const glm::vec3 p = ro + rd * t;
        if (p.x < -4.0f || p.z < -4.0f || p.x > extent + 4.0f || p.z > extent + 4.0f) return false;
        const float h = f.heightMetres(p.x, p.z);
        if (p.y <= h) {
            float lo = std::max(0.0f, t - 2.0f), hi = t;
            for (int k = 0; k < 8; ++k) {
                const float mid = 0.5f * (lo + hi);
                const glm::vec3 q = ro + rd * mid;
                if (q.y <= f.heightMetres(q.x, q.z)) hi = mid; else lo = mid;
            }
            if (tout) *tout = hi;
            if (nout) *nout = f.normalAt((ro + rd * hi).x, (ro + rd * hi).z);
            return true;
        }
        const float air = p.y - h;
        t += std::min(std::max(std::min(air * 0.55f, air / std::max(0.08f, -rd.y + 0.08f)), 0.08f), 0.6f);
    }
    return false;
}

// ---------------------------------------------------------------- the shader's ambient model
const glm::vec3 kGround(0.10f, 0.09f, 0.07f);
const glm::vec3 kSky(0.45f, 0.60f, 0.85f);

glm::vec3 skyRadiance(glm::vec3 d) {
    return glm::mix(kGround, kSky, 0.5f + 0.5f * d.y);
}
// The closed form the shader subtracts: the cosine-weighted mean of the linear sky model, 0.5 + n.y / 3.
glm::vec3 hemiAmbient(glm::vec3 n) {
    return glm::mix(kGround, kSky, 0.5f + n.y / 3.0f);
}

// Cosine-weighted direction around n, the shader's sampling (stratified by i and the frame seed).
glm::vec3 cosineDir(glm::vec3 n, float u1, float u2) {
    const float r = std::sqrt(u1);
    const float phi = 6.2831853f * u2;
    const glm::vec3 tangent = glm::normalize(std::fabs(n.y) < 0.99f ? glm::cross(glm::vec3(0, 1, 0), n)
                                                                   : glm::cross(glm::vec3(1, 0, 0), n));
    const glm::vec3 bitangent = glm::cross(n, tangent);
    return glm::normalize(tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) +
                          n * std::sqrt(std::max(0.0f, 1.0f - u1)));
}

// ---------------------------------------------------------------- a field builder
Field flatField(int size, float cell, float metres) {
    Field f;
    f.size = size;
    f.cell = cell;
    f.scale = 1.0f;
    f.h.assign(size_t(size) * size_t(size), metres);
    return f;
}

// A pit: open ground outside `wallRadius`, a floor `depth` metres down inside `floorRadius`, sloping between.
Field pitField(int size, float cell) {
    Field f = flatField(size, cell, 0.0f);
    const glm::vec2 c(float(size) * cell * 0.5f);
    // A shaft rather than a saucer: the rim subtends 45 degrees from the floor, so it blocks a large share of the
    // cosine hemisphere — the case the pass exists to darken.
    const float floorR = 2.0f, wallR = 6.0f, depth = 6.0f;
    for (int z = 0; z < size; ++z) {
        for (int x = 0; x < size; ++x) {
            const glm::vec2 p((float(x) + 0.5f) * cell, (float(z) + 0.5f) * cell);
            const float r = glm::length(p - c);
            float t = (r <= floorR) ? 1.0f : (r >= wallR ? 0.0f : 1.0f - (r - floorR) / (wallR - floorR));
            f.h[size_t(z) * size_t(size) + size_t(x)] = -depth * t;   // negative: below the tile origin
        }
    }
    return f;
}

} // namespace

int main() {
    std::printf("real-time GI estimator: the algorithm on the CPU (mirrors shaders/gi_trace.frag)\n\n");

    // 1. The march finds the analytic intersection with a flat plane. From 5 m up, 6 m/s down-range per 8 down, the
    //    ray meets y = 0 at t = 6.25; the bisection's own resolution is 2 / 256 = 0.008 m.
    {
        const Field f = flatField(129, 1.0f, 0.0f);
        float t = 0.0f;
        glm::vec3 n(0.0f);
        const glm::vec3 ro(0.0f, 5.0f, 60.0f);
        const glm::vec3 rd = glm::normalize(glm::vec3(0.6f, -0.8f, 0.0f));
        const bool hit = march(f, ro, rd, 12.0f, &t, &n);
        checkTrue(hit, "1a: a downward ray from 5 m hits flat ground");
        checkNear(double(t), 6.25, 0.02, "1b: hit distance is the analytic 6.25 m");
        checkNear(double((ro + rd * t).y), 0.0, 0.02, "1c: the hit is on the plane");
        checkNear(double(n.y), 1.0, 1e-3, "1d: a flat plane's normal is straight up");
    }

    // 2. A ray into the sky is not a hit; a ray inside a pit is. This is the occlusion signal the whole estimate is
    //    built from, so it is checked before anything that depends on it.
    {
        const Field f = flatField(129, 1.0f, 0.0f);
        float t = 0.0f;
        checkTrue(!march(f, glm::vec3(60, 0.05f, 60), glm::vec3(0, 1, 0), 12.0f, &t, nullptr),
                  "2a: a ray into the sky is not a hit");

        const Field pit = pitField(129, 1.0f);
        const float cx = 129.0f * 0.5f * 1.0f;   // the pit's centre in metres
        const glm::vec3 floorP(cx, pit.heightMetres(cx, cx) + 0.05f, cx);
        int hits = 0;
        const int rays = 64;
        for (int i = 0; i < rays; ++i) {
            const float u1 = (float(i) + 0.5f) / float(rays);
            const float u2 = std::fmod(float(i) * 0.61803398875f, 1.0f);
            const glm::vec3 d = cosineDir(glm::vec3(0, 1, 0), u1, u2);
            if (march(pit, floorP, d, 12.0f, &t, nullptr)) ++hits;
        }
        checkTrue(hits > 0, "2b: rays from the floor of a pit are occluded");
        checkTrue(hits < rays, "2c: the pit does not occlude the whole hemisphere");
        std::printf("      (%d of %d rays hit the pit's wall)\n", hits, rays);
    }

    // 3. The identity behind the delta composite: the cosine-weighted average of the sky model over the hemisphere
    //    equals the closed form the shader subtracts. If this were wrong, every frame with GI on would change
    //    brightness even on open, flat ground.
    {
        const glm::vec3 n = glm::normalize(glm::vec3(0.25f, 0.55f, 0.35f));
        const glm::vec3 want = hemiAmbient(n);
        double sum[3] = {0, 0, 0};
        const int samples = 200000;
        Rng rng(12345);
        for (int i = 0; i < samples; ++i) {
            const glm::vec3 d = cosineDir(n, rng.unit(), rng.unit());
            const glm::vec3 s = skyRadiance(d);
            for (int c = 0; c < 3; ++c) sum[c] += double(s[c]);
        }
        const double inv = 1.0 / double(samples);
        const glm::vec3 got(float(sum[0] * inv), float(sum[1] * inv), float(sum[2] * inv));
        checkNear(double(glm::length(got - want)), 0.0, 2e-3,
                  "3: cosine mean of the sky model = hemiAmbient(n)");
    }

    // 4. Open flat ground: every ray escapes, so the traced mean is the sky mean and the delta is zero to Monte
    //    Carlo noise. This is the property that keeps the pass from brightening the scene.
    {
        const Field f = flatField(129, 1.0f, 0.0f);
        const float cx = 64.0f;
        const glm::vec3 P(cx, 0.0f, cx);
        const glm::vec3 N(0, 1, 0);
        const glm::vec3 albedo(0.22f, 0.30f, 0.12f);
        // Eight rays a frame for sixteen frames with a fresh seed each: the accumulation's effective sampling.
        glm::vec3 mean(0.0f);
        int used = 0;
        for (int frame = 0; frame < 16; ++frame) {
            Rng rng(uint64_t(0x9e3779b9u) * uint64_t(frame + 1));
            glm::vec3 sum(0.0f);
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 d = cosineDir(N, rng.unit(), rng.unit());
                float t = 0.0f;
                sum += march(f, P + N * 0.05f, d, 12.0f, &t, nullptr) ? glm::vec3(0.0f) : skyRadiance(d);
            }
            mean += sum / 8.0f;
            ++used;
        }
        mean /= float(used);
        const glm::vec3 delta = albedo * (mean - hemiAmbient(N));
        checkNear(double(glm::length(delta)), 0.0, 5e-3,
                  "4a: open flat ground gives no delta (no double-counted sky)");
    }

    // 5. The same measurement inside the pit must be clearly negative: the sky the raster gave that surface is
    //    blocked, and the pass is what takes it away and puts the bounce in its place.
    {
        const Field pit = pitField(129, 1.0f);
        const float cx = 64.5f;
        const glm::vec3 P(cx, pit.heightMetres(cx, cx), cx);
        const glm::vec3 N(0, 1, 0);
        glm::vec3 sum(0.0f);
        int hits = 0;
        const int rays = 256;
        Rng rng(999);
        for (int i = 0; i < rays; ++i) {
            const glm::vec3 d = cosineDir(N, rng.unit(), rng.unit());
            float t = 0.0f;
            if (march(pit, P + N * 0.05f, d, 12.0f, &t, nullptr)) {
                ++hits;
                // Whatever the wall returns, it is not the sky: model it as the wall's own lit colour.
                sum += glm::vec3(0.08f, 0.10f, 0.05f);
            } else {
                sum += skyRadiance(d);
            }
        }
        const glm::vec3 traced = sum / float(rays);
        const glm::vec3 delta = glm::vec3(0.25f, 0.24f, 0.20f) * (traced - hemiAmbient(N));
        checkTrue(hits > rays / 8, "5a: the pit occludes a large share of the hemisphere");
        checkTrue(delta.x < -0.01f && delta.g < -0.01f && delta.b < -0.01f,
                  "5b: a pit's delta removes the sky the raster could not know was blocked");
        std::printf("      (delta inside the pit: %.3f %.3f %.3f, %d of %d rays hit)\n", delta.x, delta.g, delta.b,
                    hits, rays);
    }

    // 6. The accumulation: a reset frame is the sample itself, and a constant signal converges to itself with the
    //    sixteen-frame weight the settings expose. gi_accum.frag's mix is reproduced exactly.
    {
        auto step = [](float prev, float cur, float w) { return prev + (cur - prev) * std::max(w, 1.0f / 16.0f); };
        checkNear(double(step(0.0f, 0.7f, 1.0f)), 0.7, 1e-6, "6a: a reset frame is the new sample");
        float v = 0.0f;
        for (int i = 0; i < 16; ++i) v = step(v, 1.0f, 1.0f / 16.0f);
        checkNear(double(v), 1.0 - std::pow(15.0 / 16.0, 16.0), 1e-5, "6b: sixteen frames reach the published weight");
        for (int i = 0; i < 256; ++i) v = step(v, 1.0f, 1.0f / 16.0f);
        checkNear(double(v), 1.0, 1e-6, "6c: a constant signal converges to itself");
    }

    std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
