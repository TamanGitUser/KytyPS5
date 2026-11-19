# GTX (MAXWELL) portable diagnostic profile

This package is intended to be extracted **directly into `D:\emu\alt`**. Its archive is flattened: `kyty_emulator.exe`, its matching `kyty_emulator.pdb`, the launcher, scripts and configuration files are at the extraction root, not inside a version-named subfolder.

## Default emulator profile

The local `launch.json` is read by `run-windows.ps1` on each launch. It enables these settings without requiring Windows user-level environment variables:

| Setting | Default | Purpose |
|---|---:|---|
| `KYTY_VULKAN_RECORDING` | `0` | Bypass the local Vulkan command-recording worker implicated by the previous Aftermath call stack |
| `KYTY_DEFERRED_SUBMIT` | `0` | Do not defer queue submission through that recording path |
| `KYTY_SPECULATE` | `0` | Disable speculative command-buffer recording/passive worker segments |
| `KYTY_GPU_CHECKPOINTS` | `1` | Request NVIDIA Vulkan diagnostic checkpoints |
| `KYTY_FAULT_BINDINGS` | `1` | Request GPU address-binding history when the installed driver exposes the extension |

This is a **diagnostic isolation profile**, not a performance profile and not proof that the fault is fixed. Disabling recording and speculation may reduce performance. The address-binding extension may not be exposed by NVIDIA driver 582.78; in that case the emulator should report that the feature is unavailable and continue without it.

The profile deliberately leaves the compute-chain safe mode unchanged so that this build isolates the recording/speculation path first.

## Files and paths

All persistent Kyty configuration is kept beside the executable:

- `launch.json`: emulator command line and diagnostic environment.
- `launcher-settings.json`: the Windows launcher's saved options.
- `game-path.txt`: the selected game installation.
- `logs\`, `_PipelineCache\`, `_SaveData\`, seed files and precompile outputs: created or maintained inside this same directory.

The selected game remains at `D:\Program Files\demonsouls\PPSA01341-app0`; the game data itself is intentionally not copied into the emulator package. The scripts resolve the package configuration and working directory relative to their own folder. The precompiler creates short-lived per-process NVIDIA cache directories under Windows' temporary directory and removes them when it exits; the persistent pipeline cache and logs remain in this package folder.

## Nsight

The emulator path remains `D:\emu\alt\kyty_emulator.exe`, so an existing Nsight Aftermath application-whitelist entry for that exact path does not need to be changed. The matching PDB is shipped beside the EXE. When Nsight needs an application-symbol search path, set it once to `D:\emu\alt`; keep this path unchanged across updates.

Extract future builds over this same folder and replace files when Windows asks. Do not remove `_PipelineCache`, `_SaveData`, `logs`, `game-path.txt` or the launcher settings unless intentionally resetting them.
