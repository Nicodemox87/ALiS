# Corresponding sources for the macOS package

The Mac installer is an additional distribution alongside the unchanged Windows packages. Its application includes third-party notices under `ALiS.app/Contents/Resources/ALiS/licenses`; Python distributions retain their bundled licence files.

For **alpha.5.9**, the matching ALiS Mac source ZIP and patched CloudCompare/CCCoreLib source archive are attached to [the current release](https://github.com/Nicodemox87/ALiS/releases/tag/v0.1.0-alpha.5.9). The runtime is pinned in `packaging/macos`. Qt remains 5.15.19: its corresponding source archive and patches remain accessible under the earlier release linked below. The new installer contains dependency/deployment audit reports and component licences. The following inventory documents the historical alpha.5.7 source set; do not treat its binary audit as an alpha.5.9 audit.

Historical source assets are attached to [alpha.5.7](https://github.com/Nicodemox87/ALiS/releases/tag/v0.1.0-alpha.5.7):

- `ALiS-0.1.0-alpha.5.7-macOS-Source.zip`: ALiS source and build/packaging scripts matching Mac build 1, with updated public documentation. No confidential research data or private Git history.
- `ALiS-macOS-CloudCompare-2.13.2-source.tar.gz`: CloudCompare at `49dbbb662f296c7780aae717897c85b3cb3764ed`, initialized submodules, the three tracked macOS patches already applied, and the ALiS plugin. Git metadata is excluded. ALiS and CloudCompare use GPL-2.0-or-later; component notices remain with their source.
- `qt-everywhere-opensource-src-5.15.19.tar.xz`: corresponding official Qt sources. Exact source/bottle provenance and SHA256 are in [qt-source.json](qt-source.json). Homebrew's five source patches are included here with their original paths and checksums recorded in that file. The bundled Qt frameworks are dynamically linked; the app is ad hoc signed, with no Developer ID requirement on a locally rebuilt replacement.
- `glib-2.90.1.tar.xz` and `gettext-1.0.tar.gz`: source distributions for bundled native dependencies. See [native-sources.json](native-sources.json) for upstream URLs, checksums, licenses and build provenance.
- `ALiS-macOS-SHA256SUMS.txt`: integrity hashes for the additional Mac assets.

Use [tools/build_macos.sh](../../tools/build_macos.sh) and [the packaging guide](../../docs/MAC_INSTALLER.md) to rebuild. This script fetches the pinned host and applies [the three patches](../../tools/patches); the source archive above already has them applied. The package builder audits dependencies, copies notices and signs the completed bundle. Re-sign locally after replacing a framework or other executable component. No user installation keys or private signing credentials are required for this preview.

The portable Python source release is pinned in [runtime-source.json](../../packaging/macos/runtime-source.json); exact Python distribution versions are in [requirements.lock](../../packaging/macos/requirements.lock). Other dependencies and licenses are described in the [main notices](../../THIRD_PARTY_NOTICES.md). These source archives are developer resources; end users only need the macOS `.pkg`.
