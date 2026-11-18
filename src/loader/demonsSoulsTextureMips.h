// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Loader {
struct Program;

namespace DemonsSoulsTextureMips {
void Install(Program* program);
void Clear();
[[nodiscard]] uint32_t ReduceLevels();

} // namespace DemonsSoulsTextureMips
} // namespace Loader
