// GTX (Maxwell) fork changes: modified/added 2026-11-09. See FORK-NOTICE.md; retain upstream notices.
#include "graphics/host_gpu/renderer/deviceLossReport.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fmt/format.h>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

namespace {
constexpr uint32_t MaxDiagnosticCheckpointMarkers = 65536;

struct DiagnosticCheckpointRecord {
	uint64_t                  sequence = 0;
	uint64_t                  primary  = 0;
	uint64_t                  auxiliary = 0;
	DiagnosticCheckpointKind  kind = DiagnosticCheckpointKind::CommandBufferBegin;
	uint32_t                  x = 0;
	uint32_t                  y = 0;
	uint32_t                  z = 0;
};

std::array<DiagnosticCheckpointRecord, MaxDiagnosticCheckpointMarkers> g_checkpoint_markers {};
std::atomic<uint32_t> g_checkpoint_marker_count {0};

std::mutex g_device_loss_report_mutex;
bool       g_device_loss_reported = false;

const char* CheckpointKindName(DiagnosticCheckpointKind kind) {
	switch (kind) {
	case DiagnosticCheckpointKind::CommandBufferBegin: return "command buffer begin";
	case DiagnosticCheckpointKind::CommandBufferBeforeSubmit: return "command buffer before submit";
	case DiagnosticCheckpointKind::ComputeDispatchBegin: return "compute dispatch begin";
	case DiagnosticCheckpointKind::ComputeDispatchEnd: return "compute dispatch end";
	case DiagnosticCheckpointKind::ComputeDispatchIndirectBegin: return "indirect compute dispatch begin";
	case DiagnosticCheckpointKind::ComputeDispatchIndirectEnd: return "indirect compute dispatch end";
	case DiagnosticCheckpointKind::GraphicsDrawBegin: return "graphics draw begin";
	case DiagnosticCheckpointKind::GraphicsDrawEnd: return "graphics draw end";
	case DiagnosticCheckpointKind::PssrDispatchBegin: return "PSSR dispatch begin";
	case DiagnosticCheckpointKind::PssrDispatchEnd: return "PSSR dispatch end";
	}
	return "unknown Kyty checkpoint";
}

bool FindCheckpointRecord(const void* marker, const DiagnosticCheckpointRecord*& record) {
	if (marker == nullptr) {
		return false;
	}
	const auto address = reinterpret_cast<uintptr_t>(marker);
	const auto begin = reinterpret_cast<uintptr_t>(g_checkpoint_markers.data());
	const auto end = begin + sizeof(g_checkpoint_markers);
	if (address < begin || address >= end) {
		return false;
	}
	const auto offset = address - begin;
	if (offset % sizeof(DiagnosticCheckpointRecord) != 0) {
		return false;
	}
	const auto index = static_cast<uint32_t>(offset / sizeof(DiagnosticCheckpointRecord));
	if (index >= std::min(g_checkpoint_marker_count.load(std::memory_order_acquire),
	                      MaxDiagnosticCheckpointMarkers)) {
		return false;
	}
	record = &g_checkpoint_markers[index];
	return true;
}

void ReportDiagnosticCheckpoints(GraphicContext& graphics) {
	if (!graphics.device_diagnostic_checkpoints_enabled) {
		return;
	}
	if (graphics.queue == nullptr) {
		Log::WriteToConsoleAndLog(
		    "VK_NV_device_diagnostic_checkpoints is enabled, but the graphics queue is unavailable.\n");
		return;
	}
	const auto get_checkpoints = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV;
	if (get_checkpoints == nullptr) {
		Log::WriteToConsoleAndLog(
		    "VK_NV_device_diagnostic_checkpoints query function is unavailable.\n");
		return;
	}

	const auto queue = static_cast<VkQueue>(graphics.queue);
	uint32_t   total_count = 0;
	get_checkpoints(queue, &total_count, nullptr);
	if (total_count == 0) {
		Log::WriteToConsoleAndLog(
		    "VK_NV_device_diagnostic_checkpoints: the driver returned no checkpoint records.\n");
		return;
	}

	constexpr uint32_t MaxCheckpointRecords = 128;
	const auto         capacity = std::min(total_count, MaxCheckpointRecords);
	std::array<VkCheckpointDataNV, MaxCheckpointRecords> checkpoints {};
	for (uint32_t i = 0; i < capacity; ++i) {
		checkpoints[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
	}
	uint32_t checkpoint_count = capacity;
	get_checkpoints(queue, &checkpoint_count, checkpoints.data());
	checkpoint_count = std::min(checkpoint_count, capacity);

	Log::WriteToConsoleAndLog(fmt::format(
	    "VK_NV_device_diagnostic_checkpoints: {} record(s){}\n", checkpoint_count,
	    total_count > capacity ? " (truncated to 128)" : ""));
	for (uint32_t i = 0; i < checkpoint_count; ++i) {
		const auto& checkpoint = checkpoints[i];
		const auto  marker = checkpoint.pCheckpointMarker;
		const DiagnosticCheckpointRecord* record = nullptr;
		const bool known = FindCheckpointRecord(marker, record);
		Log::WriteToConsoleAndLog(fmt::format(
		    "  checkpoint[{}]: stage={} marker=0x{:016x} ({})"
		    " sequence={} primary=0x{:016x} auxiliary=0x{:016x} work={}x{}x{}\n",
		    i, vk::to_string(static_cast<vk::PipelineStageFlagBits>(checkpoint.stage)),
		    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(marker)),
		    known ? CheckpointKindName(record->kind) : "driver/unknown marker",
		    known ? record->sequence : 0,
		    known ? record->primary : 0,
		    known ? record->auxiliary : 0,
		    known ? record->x : 0, known ? record->y : 0, known ? record->z : 0));
	}
	const auto total_markers =
	    std::min(g_checkpoint_marker_count.load(std::memory_order_acquire),
	             MaxDiagnosticCheckpointMarkers);
	Log::WriteToConsoleAndLog(fmt::format(
	    "Kyty diagnostic markers recorded: {}{}\n", total_markers,
	    total_markers == MaxDiagnosticCheckpointMarkers ? " (capacity reached)" : ""));
}
} // namespace

void SetDiagnosticCheckpoint(GraphicContext& graphics, vk::CommandBuffer command_buffer,
                             DiagnosticCheckpointKind kind, uint64_t primary,
                             uint64_t auxiliary, uint32_t x, uint32_t y, uint32_t z) {
	if (!graphics.device_diagnostic_checkpoints_enabled || command_buffer == nullptr) {
		return;
	}
	const auto set_checkpoint = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdSetCheckpointNV;
	if (set_checkpoint == nullptr) {
		return;
	}
	const auto index = g_checkpoint_marker_count.fetch_add(1, std::memory_order_relaxed);
	if (index >= MaxDiagnosticCheckpointMarkers) {
		return;
	}
	auto& record = g_checkpoint_markers[index];
	record.sequence = static_cast<uint64_t>(index) + 1;
	record.primary  = primary;
	record.auxiliary = auxiliary;
	record.kind = kind;
	record.x = x;
	record.y = y;
	record.z = z;
	set_checkpoint(static_cast<VkCommandBuffer>(command_buffer), &record);
}

void DeviceLossReport(GraphicContext& graphics, vk::Result result) {
	if (result != vk::Result::eErrorDeviceLost) {
		return;
	}
	std::scoped_lock lock(g_device_loss_report_mutex);
	if (g_device_loss_reported) {
		return;
	}
	g_device_loss_reported = true;

	Log::WriteToConsoleAndLog("--- Vulkan device-loss report ---\n");
	ReportDiagnosticCheckpoints(graphics);
	if (!graphics.device_fault_enabled) {
		Log::WriteToConsoleAndLog(
		    "VK_EXT_device_fault is unavailable; no vendor fault records can be queried.\n");
		Log::Flush();
		return;
	}

	vk::DeviceFaultCountsEXT counts {};
	auto fault_result = graphics.device.getFaultInfoEXT(&counts, nullptr);
	if (fault_result != vk::Result::eSuccess && fault_result != vk::Result::eIncomplete) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "vkGetDeviceFaultInfoEXT (counts) failed: {}\n", vk::to_string(fault_result)));
		Log::Flush();
		return;
	}

	const auto address_total = counts.addressInfoCount;
	const auto vendor_total  = counts.vendorInfoCount;
	const auto binary_total  = counts.vendorBinarySize;
	// Bound driver-controlled allocations while retaining complete ordinary crash payloads.
	constexpr size_t MaxVendorBinaryBytes = 64u * 1024u * 1024u;
	const bool binary_too_large = binary_total > MaxVendorBinaryBytes;
	std::vector<uint8_t> vendor_binary;
	if (!binary_too_large) {
		vendor_binary.resize(binary_total);
	}
	std::array<vk::DeviceFaultAddressInfoEXT, 8> addresses {};
	std::array<vk::DeviceFaultVendorInfoEXT, 8> vendors {};
	counts.addressInfoCount = std::min(address_total, static_cast<uint32_t>(addresses.size()));
	counts.vendorInfoCount  = std::min(vendor_total, static_cast<uint32_t>(vendors.size()));
	counts.vendorBinarySize = vendor_binary.size();

	vk::DeviceFaultInfoEXT info {};
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendors.data();
	info.pVendorBinaryData = vendor_binary.empty() ? nullptr : vendor_binary.data();
	fault_result = graphics.device.getFaultInfoEXT(&counts, &info);
	if (fault_result != vk::Result::eSuccess && fault_result != vk::Result::eIncomplete) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "vkGetDeviceFaultInfoEXT (details) failed: {}\n", vk::to_string(fault_result)));
		Log::Flush();
		return;
	}

	const auto binary_size = std::min(vendor_binary.size(), counts.vendorBinarySize);
	Log::WriteToConsoleAndLog(fmt::format(
	    "VK_EXT_device_fault: \"{}\"; {} address record(s), {} vendor record(s), "
	    "{} vendor-binary byte(s){}\n",
	    info.description.data(), address_total, vendor_total, binary_size,
	    binary_too_large ? " (driver-reported binary exceeded 64 MiB; skipped)" : ""));
	if (binary_size != 0) {
		if (auto* file = std::fopen("_device_fault.nv-gpudmp", "wb"); file != nullptr) {
			const auto written = std::fwrite(vendor_binary.data(), 1, binary_size, file);
			std::fclose(file);
			Log::WriteToConsoleAndLog(fmt::format(
			    "  vendor crash data written to _device_fault.nv-gpudmp ({} / {} bytes)\n",
			    written, binary_size));
		} else {
			Log::WriteToConsoleAndLog(
			    "  could not create _device_fault.nv-gpudmp in the current directory\n");
		}
	}
	const auto address_count =
	    std::min(counts.addressInfoCount, static_cast<uint32_t>(addresses.size()));
	const auto vendor_count =
	    std::min(counts.vendorInfoCount, static_cast<uint32_t>(vendors.size()));
	for (uint32_t i = 0; i < address_count; ++i) {
		const auto& address = addresses[i];
		const auto  precision = std::max<vk::DeviceSize>(address.addressPrecision, 1);
		const auto  begin = address.reportedAddress & ~(precision - 1);
		Log::WriteToConsoleAndLog(fmt::format(
		    "  address[{}]: {} reported=0x{:016x} precision={} range=[0x{:016x}, 0x{:016x})\n",
		    i, vk::to_string(address.addressType), address.reportedAddress, precision, begin,
		    begin + precision));
	}
	for (uint32_t i = 0; i < vendor_count; ++i) {
		const auto& vendor = vendors[i];
		Log::WriteToConsoleAndLog(fmt::format(
		    "  vendor[{}]: {} code=0x{:016x} data=0x{:016x}\n", i,
		    vendor.description.data(), vendor.vendorFaultCode, vendor.vendorFaultData));
	}
	Log::Flush();
}

} // namespace Libs::Graphics
