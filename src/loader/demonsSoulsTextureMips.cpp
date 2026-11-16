// SPDX-License-Identifier: GPL-2.0-or-later
// Based on the Demon’s Souls texture-streamer analysis in Nova5 (GPL-2.0).
// Restricted to PPSA01341 01.007.000; paired with host-side BC mip-layout validation.
#include "loader/demonsSoulsTextureMips.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "kernel/memory.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#ifndef XBYAK_NO_EXCEPTION
#define XBYAK_NO_EXCEPTION
#endif
#include <xbyak/xbyak.h>

namespace Loader::DemonsSoulsTextureMips {
namespace {
constexpr uint64_t PageSize = 0x4000;
constexpr uint64_t CaveOffset = 0x8000000 + 8 * PageSize;
constexpr uint32_t Formats = 512;
constexpr uint64_t SiteOffset = 31;
constexpr uint32_t SiteSize = 10;

constexpr std::string_view Pick =
    "45 85 ff 41 0f 9f c0 85 c9 41 89 4e 18 41 0f 95 c1 41 20 f1 45 20 c1 "
    "41 0f b6 f1 4c 8b 4a 78 41 29 f7 48 8b 35 ?? ?? ?? ?? 8b 36 85 f6 44 0f 49 fe";

uint64_t g_cave_page = 0;
uint32_t g_reduce_levels = 0;

bool IsSupportedGame(const Program& program) {
    if (!Common::EqualNoCase(program.file_name.filename().string(), "eboot.bin")) return false;
    std::string title_id;
    std::string version;
    return SystemContentParamSfoGetString("TITLE_ID", &title_id) &&
           SystemContentParamSfoGetString("APP_VER", &version) &&
           Common::EqualNoCase(title_id, "PPSA01341") && version == "01.007.000";
}

bool ResolveRipTarget(uint64_t instruction, uint32_t instruction_length,
                      int32_t displacement, uint64_t& target) {
    if (instruction > UINT64_MAX - instruction_length) return false;
    const uint64_t base = instruction + instruction_length;
    if (displacement >= 0) {
        const uint64_t delta = static_cast<uint32_t>(displacement);
        if (base > UINT64_MAX - delta) return false;
        target = base + delta;
        return true;
    }
    const uint64_t delta = static_cast<uint64_t>(-static_cast<int64_t>(displacement));
    if (base < delta) return false;
    target = base - delta;
    return true;
}
} // namespace

uint32_t ReduceLevels() {
    return g_reduce_levels;
}

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
    g_reduce_levels = 0;
    if (program == nullptr || !IsSupportedGame(*program) ||
        program->mapped_size > CaveOffset ||
        program->base_vaddr > UINT64_MAX - CaveOffset - PageSize) {
        return;
    }
    const uint32_t levels = Config::GetDemonsSoulsTextureReduceLevels();
    if (levels == 0 || levels > 2 || g_cave_page != 0) return;

    const auto pick = GuestCode::FindUnique(*program, GuestCode::Pattern(Pick));
    if (!pick || *pick > UINT64_MAX - SiteOffset - SiteSize) {
        std::printf("Demon's Souls texture reduction: streamer signature is not unique; texture memory is unchanged\n");
        return;
    }
    const uint64_t site = *pick + SiteOffset;
    int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(site + 6), sizeof(displacement));
    uint64_t override_pointer = 0;
    if (!ResolveRipTarget(site, SiteSize, displacement, override_pointer)) {
        std::printf("Demon's Souls texture reduction: invalid streamer pointer; texture memory is unchanged\n");
        return;
    }

    const uint64_t requested = program->base_vaddr + CaveOffset;
    const uint64_t allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
        requested, PageSize, Common::VirtualMemory::Mode::ExecuteReadWrite,
        "demons_souls_texture_mips", true);
    if (allocated != requested) {
        if (allocated != 0) Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
        std::printf("Demon's Souls texture reduction: code-cave allocation failed; texture memory is unchanged\n");
        return;
    }

    auto* formats = reinterpret_cast<uint8_t*>(allocated + PageSize - Formats / 8);
    std::memset(formats, 0, Formats / 8);
    for (uint32_t format = 0; format < Formats; ++format) {
        if (Libs::Graphics::Prospero::BlockCompressedBytesPerBlock(
                static_cast<Libs::Graphics::Prospero::BufferFormat>(format)) != 0) {
            formats[format / 8] |= static_cast<uint8_t>(1u << (format % 8));
        }
    }

    using namespace Xbyak::util;
    Xbyak::ClearError();
    Xbyak::CodeGenerator code(PageSize - Formats / 8, reinterpret_cast<void*>(allocated));
    Xbyak::Label keep;
    code.sub(r15d, esi);
    code.cmp(r15d, levels);
    code.jge(keep);
    code.cmp(dword[rdx + 0x26c], 1);
    code.jne(keep);
    code.cmp(dword[rdx + 0x274], levels);
    code.jbe(keep);
    code.mov(esi, dword[rdx + 0x264]);
    code.mov(r8d, dword[rdx + 0x268]);
    code.cmp(esi, r8d);
    code.cmovb(esi, r8d);
    code.cmp(esi, 1024);
    code.jb(keep);
    code.mov(esi, dword[rdx + 0xc4]);
    code.shr(esi, 14);
    code.and_(esi, 0x3fff);
    code.inc(esi);
    code.cmp(esi, r8d);
    code.jne(keep);
    code.mov(esi, dword[rdx + 0xc8]);
    code.shr(esi, 28);
    code.cmp(esi, 9);
    code.jne(keep);
    code.mov(esi, dword[rdx + 0xc0]);
    code.shr(esi, 20);
    code.and_(esi, Formats - 1);
    code.mov(r8, reinterpret_cast<uint64_t>(formats));
    code.bt(dword[r8], esi);
    code.jnc(keep);
    code.mov(r15d, levels);
    code.L(keep);
    code.mov(rsi, override_pointer);
    code.mov(rsi, qword[rsi]);
    code.jmp(reinterpret_cast<const void*>(site + SiteSize));
    code.ready();

    if (Xbyak::GetError() != 0 || code.getSize() == 0 ||
        !Common::VirtualMemory::FlushInstructionCache(allocated, code.getSize()) ||
        !Libs::LibKernel::Memory::ProtectGuestMemory(
            allocated, PageSize, Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
        Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
        std::printf("Demon's Souls texture reduction: could not prepare the streamer stub; texture memory is unchanged\n");
        return;
    }

    const uint64_t after_jump = site + 5;
    int32_t relative = 0;
    if (allocated >= after_jump) {
        const uint64_t delta = allocated - after_jump;
        if (delta > static_cast<uint64_t>(INT32_MAX)) {
            Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
            return;
        }
        relative = static_cast<int32_t>(delta);
    } else {
        const uint64_t delta = after_jump - allocated;
        if (delta > static_cast<uint64_t>(INT32_MAX) + 1u) {
            Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
            return;
        }
        relative = delta == static_cast<uint64_t>(INT32_MAX) + 1u
                       ? INT32_MIN
                       : -static_cast<int32_t>(delta);
    }

    std::array<uint8_t, SiteSize> original {};
    std::memcpy(original.data(), reinterpret_cast<const void*>(site), original.size());
    std::array<uint8_t, SiteSize> patch {};
    patch.fill(0xcc);
    patch[0] = 0xe9;
    std::memcpy(patch.data() + 1, &relative, sizeof(relative));
    std::memcpy(reinterpret_cast<void*>(site), patch.data(), patch.size());
    if (!Common::VirtualMemory::FlushInstructionCache(site, patch.size())) {
        std::memcpy(reinterpret_cast<void*>(site), original.data(), original.size());
        if (!Common::VirtualMemory::FlushInstructionCache(site, original.size())) {
            EXIT("Demon's Souls texture reduction: failed to restore streamer code after a cache-flush error\n");
        }
        Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
        std::printf("Demon's Souls texture reduction: instruction-cache flush failed; texture memory is unchanged\n");
        return;
    }

    g_cave_page = allocated;
    g_reduce_levels = levels;
    std::printf("Demon's Souls texture reduction: game streamer and host cache will omit %u largest mip level(s) "
                "from eligible BC 2D textures (site eboot+0x%llx)\n",
                levels, static_cast<unsigned long long>(site - program->base_vaddr));
#else
    (void)program;
#endif
}

void Clear() {
    g_reduce_levels = 0;
    if (g_cave_page != 0) {
        if (!Libs::LibKernel::Memory::FreeGuestMemory(g_cave_page, PageSize)) {
            EXIT("Demon's Souls texture reduction: failed to free streamer code cave\n");
        }
        g_cave_page = 0;
    }
}
} // namespace Loader::DemonsSoulsTextureMips
