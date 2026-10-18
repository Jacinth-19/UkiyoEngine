#pragma once

// Draws a generated material on a sphere under the engine's sun and sky. The sphere is drawn into
// the device's backbuffer, so the material's pipeline must be built for rhi::Format::Backbuffer.
// Returns the RGBA8 pixels, top row first, sized to the backbuffer.

#include <cstdint>
#include <vector>

#include "rhi/rhi.h"

namespace ukiyo {

class GraphMaterial;

std::vector<uint8_t> renderMaterialSphere(rhi::Device& dev, GraphMaterial& material);

} // namespace ukiyo
