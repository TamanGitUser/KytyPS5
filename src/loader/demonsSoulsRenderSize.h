// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Loader {
struct Program;

namespace DemonsSoulsRenderSize {

// Adjust the game's 16:9 performance-mode render size while retaining its 3840x2160 output.
void Install(Program* program);

} // namespace DemonsSoulsRenderSize
} // namespace Loader
