#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

struct GraphicContext;

// Prints optional VK_EXT_device_fault details after a device loss. No-op for other results.
void DeviceLossReport(GraphicContext& graphics, vk::Result result);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_
