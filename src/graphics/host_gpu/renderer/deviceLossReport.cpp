#include "graphics/host_gpu/renderer/deviceLossReport.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <mutex>

namespace Libs::Graphics {

namespace {
std::mutex g_device_loss_report_mutex;
bool       g_device_loss_reported = false;
} // namespace

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
	if (!graphics.device_fault_enabled) {
		Log::WriteToConsoleAndLog(
		    "VK_EXT_device_fault is unavailable; no vendor fault records can be queried.\n");
		Log::Flush();
		return;
	}

	vk::DeviceFaultCountsEXT counts {};
	auto fault_result = graphics.device.getFaultInfoEXT(&counts, nullptr);
	if (fault_result != vk::Result::eSuccess) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "vkGetDeviceFaultInfoEXT (counts) failed: {}\n", vk::to_string(fault_result)));
		return;
	}

	const auto address_total = counts.addressInfoCount;
	const auto vendor_total  = counts.vendorInfoCount;
	// Keep the diagnostic allocation bounded: the first records are sufficient to identify
	// common instruction-pointer and access-fault patterns, and this path runs after a GPU loss.
	std::array<vk::DeviceFaultAddressInfoEXT, 8> addresses {};
	std::array<vk::DeviceFaultVendorInfoEXT, 8> vendors {};
	counts.addressInfoCount = std::min<uint32_t>(address_total, addresses.size());
	counts.vendorInfoCount  = std::min<uint32_t>(vendor_total, vendors.size());
	counts.vendorBinarySize = 0;

	vk::DeviceFaultInfoEXT info {};
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendors.data();
	fault_result       = graphics.device.getFaultInfoEXT(&counts, &info);
	if (fault_result != vk::Result::eSuccess && fault_result != vk::Result::eIncomplete) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "vkGetDeviceFaultInfoEXT (details) failed: {}\n", vk::to_string(fault_result)));
		return;
	}

	Log::WriteToConsoleAndLog(fmt::format(
	    "VK_EXT_device_fault: \"{}\"; {} address record(s), {} vendor record(s)\n",
	    info.description.data(), address_total, vendor_total));
	for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
		const auto& address = addresses[i];
		const auto  precision = std::max<vk::DeviceSize>(address.addressPrecision, 1);
		const auto  begin = address.reportedAddress & ~(precision - 1);
		Log::WriteToConsoleAndLog(fmt::format(
		    "  address[{}]: {} reported=0x{:016x} precision={} range=[0x{:016x}, 0x{:016x})\n",
		    i, vk::to_string(address.addressType), address.reportedAddress, precision, begin,
		    begin + precision));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
		const auto& vendor = vendors[i];
		Log::WriteToConsoleAndLog(fmt::format(
		    "  vendor[{}]: {} code=0x{:016x} data=0x{:016x}\n", i,
		    vendor.description.data(), vendor.vendorFaultCode, vendor.vendorFaultData));
	}
	Log::Flush();
}

} // namespace Libs::Graphics
