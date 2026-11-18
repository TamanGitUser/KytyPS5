# GTX (MAXWELL) game shader and pipeline precompiler

## What it does

The game precompiler is cumulative. It learns real shader and pipeline combinations while the game is running, appends complete records to the title-specific shader store, and rebuilds the accumulated host shader modules and Vulkan pipelines at the next compatible launch.

The store is written to `_PipelineCache/<TITLE_ID>.shaders`. The separate Vulkan driver cache is written to `_PipelineCache/<TITLE_ID>.bin` and remains signed against the current GPU vendor/device, driver version and Vulkan pipeline-cache UUID.

Each session can add:
- shader modules, including linked guest function code for callable shader variants;
- graphics pipeline state keys and their shader-stage IDs;
- tessellation layouts.

Compute pipelines are reconstructed from every stored compute shader module. On NVIDIA, warm-up remains serial because the existing implementation avoids parallel pipeline creation against one NVIDIA `VkPipelineCache` due to driver stability concerns.

## Cumulative cache compatibility

The shader store no longer uses the executable file timestamp as its compatibility key. That old check invalidated the accumulated set on every new Kyty executable build, including builds that changed only diagnostics or launcher code.

The new compatibility header includes:
- shader-store record format version;
- shader compiler/SPIR-V compatibility version;
- running title ID, content ID and application version;
- the host SPIR-V capability profile;
- maximum compute shared-memory size.

Increment `StoreFormatVersion` when changing record serialization or stored structures. Increment `ShaderCompilerCacheVersion` when changes to shader translation, SPIR-V generation or specialization semantics make old shader modules unsafe to reuse. Unrelated executable rebuilds should not invalidate the accumulated cache.

The first build using this header format will start a new compatible store once. After that, compatible builds reuse the accumulated store.

## What "complete" means and its limit

The precompiler can compile every compatible shader module and pipeline record that has been learned by the emulator. Each new area, menu, boss, effect or other game path can add more variants; later launches precompile the growing set before normal gameplay.

It cannot statically enumerate every shader or pipeline state hidden in a game that has never executed. Exact pipeline state can depend on runtime guest registers, render-target formats, vertex layouts, resource specialization and dynamic callable functions. A truly complete game-wide set therefore requires the game content to be exercised. The cache accumulates that coverage over sessions rather than guessing pipeline states.

Precompilation moves shader and pipeline creation away from first use and can reduce compilation hitches. It does not reduce the CPU cost of emulating guest GPU command streams or the physical GPU work required to render each frame. It should not be treated as a guaranteed fix for sustained low FPS.
