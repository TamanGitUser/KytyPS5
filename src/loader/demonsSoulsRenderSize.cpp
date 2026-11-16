// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/demonsSoulsRenderSize.h"

#include "common/emulatorConfig.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/hostMemory.h"
#include "loader/elf.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Loader::DemonsSoulsRenderSize {
namespace {

constexpr uint32_t OutputWidth = 3840;
constexpr uint32_t OutputHeight = 2160;
constexpr uint32_t DefaultRenderWidth = 2560;
constexpr uint32_t DefaultRenderHeight = 1440;
constexpr size_t PerformanceModeIndex = 2;

// The 01.007.000 video-settings mode-3 case loads the output pair, then the 3D render pair.
// Relocations are wildcarded; the pointed-to pairs are checked independently below.
constexpr std::string_view PerformanceModeCase =
    "c5 fb 10 0d ?? ?? ?? ?? c5 fb 10 05 ?? ?? ?? ?? eb";

constexpr std::array<uint32_t, 5> ModeWidths  {1600, 1920, 2560, 3840, 2688};
constexpr std::array<uint32_t, 5> ModeHeights {900, 1080, 1440, 2160, 1512};

bool Readable(uint64_t address, uint64_t size) {
	uint64_t readable = 0;
	return size != 0 &&
	       Libs::Graphics::HostMemoryQueryReadable(address, size, readable) && readable >= size;
}

bool HoldsPair(uint64_t address, uint32_t width, uint32_t height) {
	if (!Readable(address, sizeof(uint32_t) * 2u)) return false;
	std::array<uint32_t, 2> pair {};
	std::memcpy(pair.data(), reinterpret_cast<const void*>(address), sizeof(pair));
	return pair[0] == width && pair[1] == height;
}

std::optional<uint64_t> ResolveRipTarget(uint64_t instruction, uint32_t instruction_length,
                                         int32_t displacement) {
	if (instruction > UINT64_MAX - instruction_length) return std::nullopt;
	const auto base = instruction + instruction_length;
	if (displacement >= 0) {
		const auto delta = static_cast<uint64_t>(displacement);
		if (base > UINT64_MAX - delta) return std::nullopt;
		return base + delta;
	}
	const auto delta = static_cast<uint64_t>(-static_cast<int64_t>(displacement));
	if (base < delta) return std::nullopt;
	return base - delta;
}

// Return the unique mode-3 render-size constant only when its paired output and initial render values match.
std::optional<uint64_t> FindRenderConstant(const Program& program) {
	const GuestCode::Pattern pattern(PerformanceModeCase);
	std::vector<uint64_t> constants;
	for (const auto site: GuestCode::Find(program, pattern, 64)) {
		if (!Readable(site + 4, sizeof(int32_t)) || !Readable(site + 12, sizeof(int32_t))) continue;
		int32_t output_displacement = 0;
		int32_t render_displacement = 0;
		std::memcpy(&output_displacement, reinterpret_cast<const void*>(site + 4),
		            sizeof(output_displacement));
		std::memcpy(&render_displacement, reinterpret_cast<const void*>(site + 12),
		            sizeof(render_displacement));
		const auto output = ResolveRipTarget(site, 8, output_displacement);
		const auto render = ResolveRipTarget(site + 8, 8, render_displacement);
		if (!output || !render || !HoldsPair(*output, OutputWidth, OutputHeight) ||
		    !HoldsPair(*render, DefaultRenderWidth, DefaultRenderHeight)) {
			continue;
		}
		if (std::find(constants.begin(), constants.end(), *render) == constants.end()) {
			constants.push_back(*render);
		}
	}
	return constants.size() == 1 ? std::optional(constants.front()) : std::nullopt;
}

// Find the unique copy of a constant table in the loaded program's PT_LOAD segments.
template <size_t N>
std::optional<uint64_t> FindUniqueData(const Program& program,
                                       const std::array<uint32_t, N>& values) {
	const auto* elf = program.elf.get();
	if (elf == nullptr || elf->GetEhdr() == nullptr || elf->GetPhdr() == nullptr) return std::nullopt;
	std::vector<uint64_t> locations;
	const auto* phdr = elf->GetPhdr();
	for (size_t i = 0; i < elf->GetEhdr()->e_phnum && locations.size() < 2; ++i) {
		if (phdr[i].p_type != PT_LOAD || phdr[i].p_filesz < sizeof(values) ||
		    phdr[i].p_vaddr > UINT64_MAX - program.base_vaddr) {
			continue;
		}
		const auto address = program.base_vaddr + phdr[i].p_vaddr;
		if (!Readable(address, phdr[i].p_filesz)) continue;
		const auto* begin = reinterpret_cast<const uint8_t*>(address);
		const auto* end = begin + static_cast<size_t>(phdr[i].p_filesz);
		const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
		const auto* bytes_end = bytes + sizeof(values);
		for (auto* at = std::search(begin, end, bytes, bytes_end);
		     at != end && locations.size() < 2;
		     at = std::search(at + 1, end, bytes, bytes_end)) {
			locations.push_back(reinterpret_cast<uint64_t>(at));
		}
	}
	return locations.size() == 1 ? std::optional(locations.front()) : std::nullopt;
}

bool IsSupportedGame(const Program& program) {
	if (!Common::EqualNoCase(program.file_name.filename().string(), "eboot.bin")) return false;
	std::string title_id;
	std::string version;
	return SystemContentParamSfoGetString("TITLE_ID", &title_id) &&
	       SystemContentParamSfoGetString("APP_VER", &version) &&
	       Common::EqualNoCase(title_id, "PPSA01341") && version == "01.007.000";
}

} // namespace

void Install(Program* program) {
	const uint32_t requested_height = Config::GetDemonsSoulsRenderHeight();
	if (requested_height == 0 || requested_height == DefaultRenderHeight) return;
	if (program == nullptr || !IsSupportedGame(*program)) {
		std::printf("Demon's Souls render profile: skipped (requires PPSA01341 01.007.000 eboot.bin)\n");
		return;
	}
	if (requested_height < 720 || requested_height > OutputHeight) {
		std::printf("Demon's Souls render profile: height %u is outside the supported 720-2160 range\n",
		            requested_height);
		return;
	}

	const auto render_constant = FindRenderConstant(*program);
	const auto widths = FindUniqueData(*program, ModeWidths);
	const auto heights = FindUniqueData(*program, ModeHeights);
	if (!render_constant || !widths || !heights) {
		std::printf("Demon's Souls render profile: known 01.007.000 mode-3 render signatures not found; "
		            "game defaults kept\n");
		return;
	}

	// The game's mode-3 width is aligned to 8 pixels (the target's render surfaces use tiled layouts).
	const auto width = static_cast<uint32_t>(
	                       std::lround((static_cast<double>(requested_height) * 16.0 / 9.0) / 8.0)) *
	                   8u;
	const std::array<uint32_t, 2> pair {width, requested_height};

	// All signatures and expected original values have been verified before making any writes.
	std::memcpy(reinterpret_cast<void*>(*render_constant), pair.data(), sizeof(pair));
	std::memcpy(reinterpret_cast<void*>(*widths + PerformanceModeIndex * sizeof(uint32_t)),
	            &width, sizeof(width));
	std::memcpy(reinterpret_cast<void*>(*heights + PerformanceModeIndex * sizeof(uint32_t)),
	            &requested_height, sizeof(requested_height));

	std::printf("Demon's Souls render profile: 3D %ux%u (mode 3) with 3840x2160 game output; "
	            "TAA and output-resolution effects remain enabled\n",
	            width, requested_height);
}

} // namespace Loader::DemonsSoulsRenderSize
