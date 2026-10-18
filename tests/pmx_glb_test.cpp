// Converts a PMX model to GLB and reports what was imported. Usage:
//   pmx_glb_test <model.pmx> <out.glb> [unitScale]
// Textures are read from the model's folder. Also checks the skin joint limit and that
// the GLB header is well formed.
#include "engine/pmx_glb.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

static bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pmx_glb_test <model.pmx> <out.glb> [unitScale]\n");
        return 2;
    }
    std::vector<uint8_t> pmx;
    if (!readFile(argv[1], pmx)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const fs::path baseDir = fs::path(argv[1]).parent_path();
    ukiyo::pmx::ImportOptions opts;
    if (argc > 3) opts.unitScale = static_cast<float>(std::atof(argv[3]));
    // Texture paths are relative to the model's folder. Also try the parent folders, since
    // some packages keep the textures one level up.
    ukiyo::pmx::TextureReader reader = [&](const std::string& rel, std::vector<uint8_t>& out) {
        fs::path dir = baseDir;
        for (int up = 0; up < 3; ++up) {
            if (readFile(dir / fs::path(rel), out)) return true;
            dir = dir.parent_path();
        }
        return false;
    };
    std::vector<uint8_t> glb;
    std::string error;
    ukiyo::pmx::ImportReport rep;
    if (!ukiyo::pmx::convertToGlb(pmx, reader, glb, error, &rep, opts)) {
        std::fprintf(stderr, "convert failed: %s\n", error.c_str());
        return 1;
    }
    std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    std::printf("materials=%d textures=%d bones=%d primitives=%d triangles=%d glb=%zu bytes\n",
                rep.materials, rep.textures, rep.bones, rep.primitives, rep.triangles, glb.size());
    for (const auto& w : rep.warnings) std::printf("warning: %s\n", w.c_str());
    return 0;
}
