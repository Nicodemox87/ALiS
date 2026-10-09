# macOS port: restart here

Status on 9 October 2026: **Windows alpha.5.7 is released. No macOS binary has been built or validated.** Do not rename a Windows DLL/installer or advertise it as a Mac release.

## Recreate the development workspace

1. Clone `https://github.com/Nicodemox87/ALiS.git` into a local folder on the Mac. The Windows baseline is tag `v0.1.0-alpha.5.7`.
2. In the ChatGPT desktop app, sign in and add that folder as a local project; use Codex for code/build work. Chat history and access to local folders are separate. See [official desktop setup](https://learn.chatgpt.com/docs/app) and [projects and local files](https://learn.chatgpt.com/docs/projects).
3. Read README.md, BUILDING.md, docs/MODEL_PREPARATION.md and this file. Use a new `codex/macos-port` branch; preserve the Windows release.
4. Copy authorised test LAS/LAZ, trusted model files and research reports separately to a private folder outside this public repository. They are deliberately not in GitHub. Do not copy Windows virtual environments or credentials.
5. Inventory macOS version, architecture, RAM, Xcode command-line tools, CMake, Qt, CloudCompare and Python before installing dependencies. Target native arm64 first for Apple Silicon. Do not mix x86_64 and arm64 libraries.

## Known porting work (not yet implemented)

- `WorkspacePreparation.cpp`: replace the hard-coded `ALiS_prepare_block.exe` lookup with a platform-aware executable path.
- `WorkspaceController.cpp`: Python discovery currently favours Windows `Scripts/python.exe`; add an explicit supported macOS venv/bin/python3 path and app-bundle-aware worker lookup.
- `MemorySafety.h`: non-Windows code currently uses a conservative fixed fallback. Add tested macOS available-memory detection.
- `qal_ml/preparation.py`: `SC_AVPHYS_PAGES` is not portable to Darwin. Add tested memory reporting and failure-safe fallback; retain the memory budget logic.
- `SettingsDialog.cpp`: update installation currently targets Windows Setup.exe. Implement a Mac-specific route or clearly disable it with an explanation.
- CMake/deployment: decide and test the CloudCompare.app bundle locations for the plugin, native worker, Python sources and Qt minimal platform plugin. Validate dynamic-library paths/rpaths, architecture and Qt ABI. Preserve the existing Windows layout.
- Keep CloudCompare 2.13.2 as the reference stable host unless an explicitly approved compatibility change is required. Check the exact Mac distribution's Qt and architecture, rather than assuming the Windows SDK applies.
- Python: build a fresh native venv. Start with CPU ML/DL parity. CUDA is not a Mac backend; do not claim Metal/MPS acceleration until it is implemented and verified for each model.

## Acceptance gates

1. Native arm64 build of plugin and worker against the chosen host; record all dependency versions.
2. Native non-GUI tests, synthetic LAS/LAZ block tests, then interactive plugin loading, features, ground/terrain, annotation and save/reopen.
3. Reuse a trusted small model/cloud fixture: compare feature masks/numeric tolerances, point count/order, classes and confidence against Windows. Validate both CC-snapshot and disk-block routes.
4. Check clean installation, update/removal and a second launch; preserve unrelated plugins, user settings and data.
5. Package a clearly labelled testing release, with source, licences, checksums and signing/notarisation status. Never advise disabling Gatekeeper or removing quarantine to bypass security.
6. Publish separate Windows and macOS assets only after the relevant gates pass. Keep historical release assets immutable.

Do not launch the full production survey automatically. Current evidence: 92 session checks, 375 processing/UI checks, five synthetic block tests; local RF and PointNet blocked/whole-crop equivalence checks. These are regression checks, not independent-site scientific accuracy claims. See docs/RELEASE_VALIDATION.md.

## Prompt to resume on the Mac

> Resume ALiS development from this repository. First read docs/MACOS_HANDOFF.md, BUILDING.md and docs/MODEL_PREPARATION.md. Target my Apple Silicon Mac with a native arm64 build. Inventory the local host/dependencies, then implement and test the macOS port on a separate branch while preserving the released Windows version. Do not claim a usable Mac release until native and interactive tests pass. Use only small authorised fixtures; do not run the full survey or upload private clouds, models or research reports. Keep a progress log and report any user action needed for signing or installation. Keep explanations concise.
