// GTX (Maxwell) fork changes: modified/added 2026-11-09. See FORK-NOTICE.md; retain upstream notices.
#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

struct GraphicContext;

// Stable application-owned markers inserted into diagnostic command buffers.
extern const char kKytyCheckpointCommandBufferBegin[];
extern const char kKytyCheckpointCommandBufferSubmit[];

// Prints optional Vulkan device-fault and checkpoint details after a device loss.
// No-op for other results.
void DeviceLossReport(GraphicContext& graphics, vk::Result result);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_
