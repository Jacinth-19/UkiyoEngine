// Single compilation unit that provides the implementations of header-only
// third-party libraries. Everything else includes these headers declaration-only.
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
