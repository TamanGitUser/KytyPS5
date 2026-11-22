# Mode 4 PGO capture

This folder is included in the Windows package when the PGO-capture build is available.

1. Close Kyty.
2. Double-click pgo-capture.cmd.
3. Wait until the emulator log reports "Shader prefetch wait: done".
4. Load the same gameplay area and play normally for a few minutes. This is an instrumentation capture, not an FPS benchmark; the instrumented executable is slower.
5. Double-click pgo-save.cmd. It writes mode4.profraw and closes the emulator.
6. The capture script restores the original release kyty_emulator.exe. Upload only mode4.profraw for profile merging.

The capture uses the existing game installation and save. It does not delete or replace _SaveData, _PipelineCache, shader seeds, or game files. If the capture script is interrupted while Kyty is still running, close Kyty first, then restore kyty_emulator.release-backup.exe over the root kyty_emulator.exe.
