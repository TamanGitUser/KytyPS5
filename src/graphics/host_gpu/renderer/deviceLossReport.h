// GTX (Maxwell) fork changes: modified/added 2026-11-09. See FORK-NOTICE.md; retain upstream notices.
#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

struct GraphicContext;

// Checkpoint markers are opaque application-owned values. Each record is kept in a
// process-lifetime registry so it remains valid while the GPU is executing submitted work.
enum class DiagnosticCheckpointKind : uint32_t {
	CommandBufferBegin,
	CommandBufferBeforeSubmit,
	ComputeDispatchBegin,
	ComputeDispatchEnd,
	ComputeDispatchIndirectBegin,
	ComputeDispatchIndirectEnd,
	GraphicsDrawBegin,
	GraphicsDrawEnd,
	PssrDispatchBegin,
	PssrDispatchEnd,
};

void SetDiagnosticCheckpoint(GraphicContext& graphics, vk::CommandBuffer command_buffer,
                             DiagnosticCheckpointKind kind, uint64_t primary = 0,
                             uint64_t auxiliary = 0, uint32_t x = 0, uint32_t y = 0,
                             uint32_t z = 0);

// Prints optional Vulkan device-fault and checkpoint details after a device loss.
// No-op for other results.
void DeviceLossReport(GraphicContext& graphics, vk::Result result);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_
