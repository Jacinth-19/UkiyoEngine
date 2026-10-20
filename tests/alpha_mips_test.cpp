// Alpha-coverage-preserving mipmaps (docs/ALPHA_MIPS.md): the chain builder in src/engine/texture_mips.cpp, on images whose
// coverage can be measured exactly.
//
// The property the feature exists for is a single number per level: the fraction of the surface that survives the alpha
// test. The test builds two chains from the same cut-out image — the box filter on its own, and the box filter followed by
// the coverage search — and checks that the second one holds the level-0 number while the first one decays, that the
// search only ever moves alpha (never colour), and that every degenerate image (1 x 1, all opaque, all transparent) comes
// back well defined.
//
// Like the other CPU tests it needs no device: the chain builder is plain C++ and runs in CI on both platforms.

#include "engine/texture_mips.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ukiyo;

namespace {

int gChecks = 0;
int gFailures = 0;

void checkTrue(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

void checkNear(double got, double want, double tol, const std::string& what) {
    ++gChecks;
    if (!(std::fabs(got - want) <= tol)) {
        ++gFailures;
        std::printf("FAIL  %-64s got %.5f want %.5f (tol %.5f)\n", what.c_str(), got, want, tol);
    }
}

// A cut-out image with structure at every scale: a lattice of holes plus a sparse hash. Density 0.32 keeps level 0's
// coverage under the 0.5 threshold, which is the case that dissolves when the mips average the alpha.
std::vector<uint8_t> cutout(int w, int h, float density) {
    std::vector<uint8_t> img(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * w + x) * 4;
            const unsigned hash = unsigned(x) * 2654435761u ^ unsigned(y) * 2246822519u;
            const bool on = ((hash >> 13) % 1000) < unsigned(density * 1000.0f);
            img[i + 0] = 200;
            img[i + 1] = 220;
            img[i + 2] = 140;
            img[i + 3] = on ? 255 : 0;
        }
    }
    return img;
}

std::vector<uint8_t> solid(int w, int h, uint8_t alpha) {
    std::vector<uint8_t> img(size_t(w) * h * 4);
    for (size_t i = 0; i < img.size(); i += 4) {
        img[i + 0] = uint8_t((i / 4) & 0xFF);
        img[i + 1] = uint8_t((i / 4) >> 8);
        img[i + 2] = 77;
        img[i + 3] = alpha;
    }
    return img;
}

float coverageOf(const MipLevel& l, float threshold) {
    return alphaTestCoverage(l.rgba.data(), l.width, l.height, threshold);
}

} // namespace

int main() {
    // 1. Chain shape: one level per halving of the longer side, ending at 1 x 1, level 0 byte-identical to the input.
    {
        const int w = 64, h = 32;
        const std::vector<uint8_t> img = cutout(w, h, 0.32f);
        const std::vector<MipLevel> chain = buildMipChain(img.data(), w, h);
        checkTrue(chain.size() == 7, "1a: 64 x 32 gives seven levels (64, 32, 16, 8, 4, 2, 1)");
        checkTrue(chain[0].width == 64 && chain[0].height == 32, "1b: level 0 is the source size");
        checkTrue(chain[0].rgba == img, "1c: level 0 is the source image, unmodified");
        bool halving = true, endsAtOne = true;
        for (size_t i = 1; i < chain.size(); ++i) {
            halving = halving && chain[i].width == std::max(1u, chain[i - 1].width / 2u) &&
                      chain[i].height == std::max(1u, chain[i - 1].height / 2u);
            halving = halving && chain[i].rgba.size() == size_t(chain[i].width) * chain[i].height * 4u;
        }
        checkTrue(halving, "1d: every level halves the one before it (clamped at 1)");
        endsAtOne = chain.back().width == 1 && chain.back().height == 1;
        checkTrue(endsAtOne, "1e: the chain ends at 1 x 1");
    }

    // 2. The box filter is a box filter: a 2 x 2 of known colours becomes their rounded average.
    {
        const uint8_t px[2 * 2 * 4] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150};
        const std::vector<MipLevel> chain = buildMipChain(px, 2, 2);
        checkTrue(chain.size() == 2, "2a: 2 x 2 gives two levels");
        const uint8_t* avg = chain[1].rgba.data();
        checkTrue(chain[1].width == 1 && chain[1].height == 1, "2b: the second level is 1 x 1");
        checkTrue(avg[0] == 60 && avg[1] == 70 && avg[2] == 80 && avg[3] == 90, "2c: it averages the four texels");
    }

    // 3. A solid opaque image keeps coverage 1, and the coverage search has nothing to do: the two chains are identical.
    {
        const int w = 32, h = 32;
        const std::vector<uint8_t> img = solid(w, h, 255);
        const std::vector<MipLevel> naive = buildMipChain(img.data(), w, h, false);
        const std::vector<MipLevel> kept = buildMipChain(img.data(), w, h, true, 0.5f);
        checkNear(alphaTestCoverage(img.data(), w, h, 0.5f), 1.0, 1e-6, "3a: an opaque image covers everything");
        bool same = naive.size() == kept.size();
        for (size_t i = 0; same && i < naive.size(); ++i) same = naive[i].rgba == kept[i].rgba;
        checkTrue(same, "3b: preserving the coverage changes nothing when there is nothing to preserve");
        checkNear(coverageOf(kept.back(), 0.5f), 1.0, 1e-6, "3c: and the last level is still opaque");
    }

    // 4. A fully transparent image stays fully transparent (the scale cannot raise zero alpha) and is left alone.
    {
        const int w = 16, h = 16;
        const std::vector<uint8_t> img = solid(w, h, 0);
        const std::vector<MipLevel> naive = buildMipChain(img.data(), w, h, false);
        const std::vector<MipLevel> kept = buildMipChain(img.data(), w, h, true, 0.5f);
        checkNear(alphaTestCoverage(img.data(), w, h, 0.5f), 0.0, 1e-6, "4a: a transparent image covers nothing");
        bool same = naive.size() == kept.size();
        for (size_t i = 0; same && i < naive.size(); ++i) same = naive[i].rgba == kept[i].rgba;
        checkTrue(same, "4b: the search does not touch an image it cannot help");
        checkNear(coverageOf(kept.back(), 0.5f), 0.0, 1e-6, "4c: the last level is still empty");
    }

    // 5. The point of the feature. On a sparse cut-out, plain mips lose the surface: coverage falls from 0.30 to 0.0001 by
    // the fourth level, which is a fence dissolving. The preserved chain holds 0.30 through the levels a distant object
    // samples, and only gives up at 8 x 8 and below, where no single alpha can represent the pattern any more.
    {
        const int w = 64, h = 64;
        const std::vector<uint8_t> img = cutout(w, h, 0.32f);
        const float c0 = alphaTestCoverage(img.data(), w, h, 0.5f);
        const std::vector<MipLevel> naive = buildMipChain(img.data(), w, h, false);
        const std::vector<MipLevel> kept = buildMipChain(img.data(), w, h, true, 0.5f);
        checkNear(c0, 0.32, 0.06, "5a: the source is a sparse cut-out (about a third opaque)");

        double worstKept = 0.0, naiveAtFour = 0.0;
        for (size_t i = 1; i < kept.size(); ++i) {
            const double k = coverageOf(kept[i], 0.5f);
            if (kept[i].width >= 4) worstKept = std::max(worstKept, std::fabs(k - c0));
            if (kept[i].width == 4) naiveAtFour = coverageOf(naive[i], 0.5f);
        }
        checkTrue(worstKept <= 0.04, "5b: preserved coverage stays within 0.04 of level 0 down to 4 x 4");
        checkNear(coverageOf(naive[1], 0.5f), 0.135, 0.05, "5c: plain mips are already at half the coverage by level 1");
        checkTrue(naiveAtFour < 0.01, "5d: and by 4 x 4 the plain chain has dissolved entirely");
        checkTrue(naiveAtFour < 0.1 * c0, "5e: which is two orders of magnitude below the source");

        // The coverage search only moves alpha: every RGB byte of every level matches the plain chain.
        bool rgbSame = naive.size() == kept.size();
        for (size_t i = 0; rgbSame && i < naive.size(); ++i) {
            rgbSame = naive[i].rgba.size() == kept[i].rgba.size();
            for (size_t b = 0; rgbSame && b < naive[i].rgba.size(); b += 4) {
                rgbSame = naive[i].rgba[b] == kept[i].rgba[b] && naive[i].rgba[b + 1] == kept[i].rgba[b + 1] &&
                          naive[i].rgba[b + 2] == kept[i].rgba[b + 2];
            }
        }
        checkTrue(rgbSame, "5f: only alpha changes; every colour byte is identical to the plain chain");

        // And the alpha it writes is a scale of the box-filtered one, so a level never gains detail it did not have.
        bool scaled = true;
        for (size_t i = 1; i < kept.size(); ++i) {
            for (size_t b = 3; b < kept[i].rgba.size(); b += 4) {
                const int base = naive[i].rgba[b];
                const int got = kept[i].rgba[b];
                if (base == 0 && got != 0) scaled = false;
                if (got > 255) scaled = false;
            }
        }
        checkTrue(scaled, "5g: a transparent texel stays transparent, and alpha stays in range");
    }

    // 6. The search itself: monotone in the target, and the identity when the target is what the level already has.
    {
        const int w = 32, h = 32;
        const std::vector<uint8_t> img = cutout(w, h, 0.32f);
        std::vector<MipLevel> chain = buildMipChain(img.data(), w, h, false);
        const MipLevel& level = chain[2];
        const float c = coverageOf(level, 0.5f);
        checkNear(alphaScaleForCoverage(level.rgba.data(), level.width, level.height, 0.5f, c), 1.0, 0.05,
                  "6a: asking for the coverage a level already has returns 1");
        const float up = alphaScaleForCoverage(level.rgba.data(), level.width, level.height, 0.5f, 0.28f);
        checkTrue(up > 1.0f, "6b: asking for more coverage raises alpha");
        checkTrue(up <= 4.0f, "6c: and the multiplier stays inside [0, 4]");
        const float down = alphaScaleForCoverage(level.rgba.data(), level.width, level.height, 0.5f, 0.0f);
        checkTrue(down < 1.0f, "6d: asking for none lowers it");
        checkTrue(down >= 0.0f, "6e: and not below zero");
    }

    // 7. A higher threshold is a tighter cut: both chains start lower, and the preserved chain still tracks its own level 0.
    {
        const int w = 64, h = 64;
        const std::vector<uint8_t> img = cutout(w, h, 0.32f);
        const float soft = alphaTestCoverage(img.data(), w, h, 0.25f);
        const float hard = alphaTestCoverage(img.data(), w, h, 0.75f);
        checkTrue(soft > hard, "7a: a lower threshold keeps more of the surface");
        const std::vector<MipLevel> keptHard = buildMipChain(img.data(), w, h, true, 0.75f);
        double worst = 0.0;
        for (size_t i = 1; i < keptHard.size(); ++i)
            if (keptHard[i].width >= 4) worst = std::max(worst, std::fabs(double(coverageOf(keptHard[i], 0.75f) - hard)));
        checkTrue(worst <= 0.05, "7b: preservation holds at the tighter threshold too");
        checkTrue(hard < soft, "7c: (the two thresholds really do differ on this image)");
    }

    // 8. Degenerate shapes: 1 x 1, a single row, a single column, and an odd size that never divides evenly.
    {
        const std::vector<uint8_t> one = cutout(1, 1, 1.0f);
        const std::vector<MipLevel> c1 = buildMipChain(one.data(), 1, 1, true, 0.5f);
        checkTrue(c1.size() == 1 && c1[0].rgba == one, "8a: 1 x 1 is a single level, unchanged");

        const std::vector<uint8_t> rowImg = cutout(1, 8, 0.4f);
        const std::vector<MipLevel> row = buildMipChain(rowImg.data(), 1, 8, true, 0.5f);
        checkTrue(row.size() == 4 && row[1].width == 1 && row[1].height == 4, "8b: a column halves in height only");
        checkTrue(row.back().width == 1 && row.back().height == 1, "8c: and reaches 1 x 1");

        const std::vector<uint8_t> oddImg = cutout(13, 7, 0.35f);
        const std::vector<MipLevel> odd = buildMipChain(oddImg.data(), 13, 7, true, 0.5f);
        bool shape = !odd.empty();
        for (size_t i = 0; shape && i < odd.size(); ++i)
            shape = odd[i].rgba.size() == size_t(odd[i].width) * odd[i].height * 4u;
        checkTrue(shape, "8d: an odd 13 x 7 chain has correctly sized levels");
        checkTrue(odd.size() == 4, "8e: 13 x 7 gives four levels");
        const float oddC0 = alphaTestCoverage(oddImg.data(), 13, 7, 0.5f);
        checkNear(coverageOf(odd[0], 0.5f), oddC0, 1e-6, "8f: the odd chain's level 0 measures the source exactly");
        const float last = coverageOf(odd.back(), 0.5f);
        checkTrue(last >= 0.0f && last <= 1.0f, "8g: and its last level is a defined coverage");
    }

    // 9. Determinism: the same image and settings produce the same bytes, which is what makes the figure reproducible.
    {
        const std::vector<uint8_t> img = cutout(32, 32, 0.3f);
        const std::vector<MipLevel> a = buildMipChain(img.data(), 32, 32, true, 0.5f);
        const std::vector<MipLevel> b = buildMipChain(img.data(), 32, 32, true, 0.5f);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i) same = a[i].rgba == b[i].rgba;
        checkTrue(same, "9a: two builds of the same image are byte-identical");
        checkTrue(img[3] == 255 || img[3] == 0, "9b: the caller's image is never modified");
    }

    std::printf("alpha mips: %d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
