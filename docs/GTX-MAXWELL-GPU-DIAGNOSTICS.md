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
