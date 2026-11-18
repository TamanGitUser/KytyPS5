# GTX (MAXWELL) of KytyPS5 — provenance and distribution notice

Last updated: 2026-11-08

## Project identity

GTX (MAXWELL) of KytyPS5 is an independent community fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5). It is not an official KytyPS5 release and is not affiliated with NVIDIA, Sony Interactive Entertainment, or PlayStation. “GTX” and “Maxwell” are used only to describe the intended GPU compatibility focus.

Upstream source and project history remain credited to their respective authors. This fork does not claim ownership of upstream code.

## License obligations

- KytyPS5 is distributed under **GNU GPL version 2 only (GPL-2.0-only)**. This fork keeps that license; it does not relicense the combined derivative work under a different license.
- The original Kyty MIT license notice is retained at `LICENSES/Kyty-MIT.txt`.
- Preserve all upstream copyright, license, attribution, and warranty-disclaimer notices.
- Fork-modified and fork-added source files carry a dated notice. The affected files are listed in the repository history and compare view.
- When distributing binaries, provide the complete corresponding source for the exact release, including build/install scripts and required submodule source, under GPL-2.0-only. Each binary release should include the matching source archive on the same GitHub release page, plus the GPL license and relevant third-party notices.
- Third-party dependencies remain under their own licenses. Review the licenses and notices shipped with each dependency before changing dependencies or packaging.

## Distribution boundaries

This project distributes emulator software only. Releases must not include game dumps, PS5 firmware, BIOS, encryption keys, copyrighted system software, PKG files, proprietary SDKs, or other Sony/game publisher assets. Do not add such files to Git, build artifacts, issue attachments, or releases.

Use only game data and system software that you are legally entitled to use. The project does not provide instructions or tools to obtain unauthorized copies or bypass access controls.

## Releases

The fork's release workflow targets Windows x64 and Linux x86-64. It does not publish macOS builds. Releases use the **GTX (MAXWELL)** product label and a `gtx-maxwell-<version>` tag. The source archive must correspond to the exact commit used to build the binaries.

## No warranty

The upstream GPL and third-party licenses contain their own warranty disclaimers. This fork is provided as-is, without additional warranty or guarantee of compatibility. Nothing in this notice is legal advice or a guarantee that every use is lawful in every jurisdiction.
