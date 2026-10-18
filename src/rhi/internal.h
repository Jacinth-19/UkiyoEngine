// Internal factory declarations shared by the device implementations.
#pragma once
#include "rhi/rhi.h"

namespace rhi {
std::unique_ptr<Device> createGLDevice(const DeviceConfig& config, GLFWwindow* window);
std::unique_ptr<Device> createVKDevice(const DeviceConfig& config, GLFWwindow* window);
}
