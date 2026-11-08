// GTX (Maxwell) fork changes: modified/added 2026-11-08. See FORK-NOTICE.md; retain upstream notices.
#include "graphics/host_gpu/renderer/deviceLossReport.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fmt/format.h>
#include <mutex>
#include <vector>

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
