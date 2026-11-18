# GTX (Maxwell) diagnostics in the Chenxiao combination build

This branch is based on Chenxiao's `experiment/perf-40fps-20260926` branch. It keeps Chenxiao's current device-fault reporting, buffer-address history, submission breadcrumbs, pipeline names and optional Vulkan diagnostic checkpoints.

## Added GTX diagnostics

### NVIDIA vendor crash payload

When `VK_EXT_device_fault` supplies a vendor binary, the device-loss report writes the raw payload to:

```text
_device_fault.nv-gpudmp
```

The file contains the vendor payload itself; **there is no application-specific header**. It is written in the process working directory and a later dump can overwrite an earlier one. The exporter caps the payload at 64 MiB and caps collected address/vendor records at 256/64 respectively, logging when a report is truncated or cannot be saved.

The payload is optional: the Vulkan driver must expose and return vendor binary data for the file to be created.

### Targeted Demon’s Souls draw snapshot

The renderer logs at most eight snapshots for this VS/PS pair:

- VS: `0x004c650d72ecd57c`
- PS: `0x94fd11e8abbdc3c8`

Each snapshot records draw parameters, color/depth attachment metadata, vertex-buffer ranges and resolved VS/PS resource-binding ranges. It does not dereference guest vertex or index memory.

### Existing Chenxiao diagnostics retained

- Vulkan device-fault address reports and guest-buffer address history.
- Pipeline breadcrumbs for the most recent submissions, including pipeline labels with shader hashes.
- Optional `VK_NV_device_diagnostic_checkpoints` markers after pipeline binds, enabled by setting `KYTY_GPU_CHECKPOINTS=1` before starting Kyty. Checkpoints can add overhead; use them for diagnostic runs.

## Maxwell compatibility limitation

The GTX 970 (Maxwell) does not expose `VK_KHR_fragment_shader_barycentric` / `fragmentShaderBarycentric`. These diagnostic changes do not emulate that feature or guarantee that Demon’s Souls will render. A correct shader-recompiler compatibility implementation remains separate work.

## Test-installation safety

Use this build in a separate test directory. Do not delete or overwrite an existing Kyty installation, shader cache, or pipeline cache when comparing builds.
