// ukiyo_pak: packs the resource folders of a project into one resources.pak.
//
//   ukiyo_pak <out.pak> <project root> <folder>...
//
// Example: ukiyo_pak project/resources.pak project assets scripts
// The pack should sit in the project root, because its keys are paths relative to the folder that holds it.
#include <cstdio>
#include <string>
#include <vector>

#include "engine/vfs.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: ukiyo_pak <out.pak> <project root> <folder>...\n");
        return 2;
    }
    const std::vector<std::string> dirs(argv + 3, argv + argc);
    std::string error;
    int count = 0;
    if (!ukiyo::vfs::buildPak(argv[2], dirs, argv[1], &error, &count)) {
        std::fprintf(stderr, "ukiyo_pak: %s\n", error.c_str());
        return 1;
    }
    std::printf("packed %d files into %s\n", count, argv[1]);
    return 0;
}
