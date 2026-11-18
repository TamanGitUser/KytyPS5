# GTX (Maxwell) GPU crash diagnostics

The fork can collect Vulkan device-loss information through the following driver extensions when the installed driver exposes them:

- `VK_EXT_device_fault` for fault addresses and the NVIDIA vendor crash payload.
- `VK_NV_device_diagnostic_checkpoints` for the latest application-owned command markers reported at supported pipeline stages.
- NVIDIA Aftermath tooling can inspect the extracted vendor payload when the payload format and driver data are supported.

The emulator writes `_device_fault.nv-gpudmp` in its current working directory after a Vulkan device loss. The file includes a 56-byte application-side header followed by the NVIDIA vendor payload. Nsight Graphics may require the extracted vendor payload rather than the complete emulator file.

## Focused Demon’s Souls draw capture

The current GTX 970 investigation identified this shader pair in the failing draw:

- Vertex shader hash: `0x004c650d72ecd57c`
- Pixel shader hash: `0x94fd11e8abbdc3c8`

For this pair, the renderer writes a bounded `GPU diagnostic DrawSnapshot` to `_kyty.txt`. The snapshot includes draw parameters, color/depth attachment metadata, vertex and index buffer addresses, and up to 32 raw descriptors per resource class for the vertex and pixel stages. It does not dereference guest vertex-buffer memory.

The snapshot is deliberately limited to eight matching draws per process. If the failing shader pair changes after a game update or a different scene is tested, update the targeted hashes in `renderDraw.cpp` based on the new checkpoint report.

## Reading the checkpoints

Each checkpoint marker is an application-owned record retained for the process lifetime. The report decodes the marker address into its operation, sequence number, shader identifiers, and work dimensions. Nsight Aftermath may show the user-marker payload addresses; match those addresses to the `marker=0x...` values in `_kyty.txt`.

A marker at Top of Pipe indicates progress through the early pipeline stage. A later marker at Bottom of Pipe indicates that a later operation has progressed through the bottom stage. If the stages report different operations, treat the gap as an in-flight region to investigate—not proof that a specific shader instruction caused the fault.


## Windows implicit Vulkan-layer isolation

The Windows launcher now applies two environment overrides to Kyty's child process:

- `DISABLE_VULKAN_OBS_CAPTURE=1` uses OBS's manifest-defined implicit-layer disable switch.
- `VK_LOADER_LAYERS_DISABLE=*RTSS*` filters RTSS Vulkan layer names while leaving unrelated implicit layers enabled.

Unless the user already selected a loader-debug setting, the launcher enables Vulkan loader warning/layer messages so the exclusions are visible during startup. The Vulkan loader documents `VK_LOADER_LAYERS_DISABLE` as a comma-delimited list of layer-name globs. <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md>

## Bounded-volume diagnostic checkpoints

The checkpoint registry remains append-only so addresses submitted to the driver are never reused. To avoid exhausting the fixed registry during normal gameplay, Kyty now samples:
- every 16th command-buffer begin/before-submit marker,
- every 4096th graphics draw begin/end marker,
- every 1024th compute dispatch begin/end marker,
- every 256th PSSR dispatch begin/end marker.

The device-loss report prints observed, retained, sampled-out and capacity-dropped counts. The registry holds 262144 retained records rather than 65536.
