# ALiS 0.1.0-alpha.5.8 — 20 October 2026

Windows maintenance release: rectangle/polygon/lasso event handling, live polygon edge and keyboard completion; conservative feature RAM and dense-neighbourhood protections, bounded CPU parallelism and memory-pressure cancellation; corrected numeric alpha-version update checks. Complete setup includes CloudCompare detection/installation and Python ML/DL. See [release notes, limits and validation](docs/RELEASE_ALPHA58.md). Mac binary corrections require a matching Mac build; the alpha.5.7 Mac installer remains available.

---

# Windows complete online installer — 12 October 2026

The main **Download Windows installer** link now supplies the new **Complete Setup r1**. It detects compatible CloudCompare 2.13.2 x64/Qt5 or installs the official host, installs the unchanged ALiS alpha.5.7 plugin, and prepares isolated Python, ML and PyTorch dependencies. Choose CPU or optional NVIDIA CUDA 12.8. Internet and confirmation of Windows administrator prompts are required; NVIDIA drivers are not installed.

SHA-256-verified native downloads, actual DLL/package/computation checks and runtime activation after successful checks are included. Previous datasets, models and incompatible host folders are not deleted. Failed prerequisites are not automatically rolled back. The wrapper is unsigned; retain Windows security protections.

The historical Windows plugin-only EXE/ZIP, native source and Mac assets remain unchanged. Installer sources and checksums have separate revisioned filenames. This is a packaging update, not new scientific validation or a new native plugin version. See [complete setup instructions](docs/WINDOWS_COMPLETE_INSTALLER.md) and [installation test evidence](docs/WINDOWS_COMPLETE_VALIDATION.md).

---

# macOS addition — 12 October 2026

ALiS now has separate Windows and macOS downloads. The existing Windows alpha.5.7 packages are unchanged. The added Mac build 1 is a complete Apple Silicon / macOS 27+ PKG with CloudCompare 2.13.2, portable Python and all six CPU classifiers, plus automatic/manual GitHub update checks. It is a research preview without Developer ID signing or notarization; normal macOS authorization is required. See [Mac installation](docs/MAC_INSTALLER.md) and [validation](docs/MAC_INSTALLER_TESTS.md). No private survey data, trained models or backups are included.

The original release tag is retained; use the separately named Mac source archive for this port. Mac packages and sources are additional assets, not replacements for Windows files.

---

# ALiS 0.1.0-alpha.5.7 — model-driven block preparation

For **CloudCompare 2.13.2 stable / Qt 5.15.x / Windows x64** only.

## New workflow

- **Prepare data for this model** reads required feature identities, order and radii from a trusted saved model.
- Use the selected CloudCompare cloud, including current fields, or LAS/LAZ directly from disk.
- Spatial cores with context overlap, adaptive subdivision and memory preflight; no automatic point decimation or radius changes.
- Separate native worker, progress reporting, cancellation and checked resume.
- Classify prepared blocks without retraining. Merge by original point ID; no duplicated halo points.
- Reports show class distribution and confidence. Loaded-cloud imports add Derived Classification/Confidence without overwriting Working/Trusted labels.
- Large in-process feature computations receive a memory preflight and a route to block preparation.

Existing supervised, annotation, terrain, clustering and local PointNet workflows are retained. No external pretrained weights, survey data or unpublished papers are distributed.

## Install or update

Save work and close CloudCompare, then run the new **Setup.exe** and select the folder containing CloudCompare.exe. The manual-install ZIP is an alternative, not a standalone host. Do not mix DLLs and workers from different releases. See [installation](https://github.com/Nicodemox87/ALiS/blob/main/docs/INSTALLATION.md).

The separate Python runtime is required for block orchestration and classifiers. Direct native terrain/features/annotation remain usable without Python. The package now includes ALiS_prepare_block.exe and the matching Qt minimal platform DLL, with notices and corresponding Qt source attached to this release. The installer is **unsigned**; verify checksums and origin, without disabling security protections.

## Validation and limits

- Native session: 92 checks; processing/workspace with UI: 375 checks passed.
- Five synthetic block-integration tests passed against the installed native executable, including compressed LAZ input.
- Real 585,001-point tile: blocked RF labels match whole-tile labels 100%, with the same global HAG.
- Real 28,283-point spatial crop: blocked local PointNet CUDA labels match whole-crop labels 100%.

These are software regression/equivalence checks, **not classification accuracy or independent-site validation**. No new full-survey run was performed for this release.

Memory budgets are estimates, not hard OS quotas. Tiled CSF/HAG is not guaranteed equivalent to global terrain; prefer compatible global HAG or explicitly reviewed settings. LAS RGB models currently require a CC snapshot. Waveform LAS formats 4/5/9/10 and unsupported/incomplete model schemas are rejected. Only trusted model files should be opened because joblib can execute code. Job folders retain restart data and may require substantial disk space.

See [full workflow and limitations](https://github.com/Nicodemox87/ALiS/blob/main/docs/MODEL_PREPARATION.md). Report reproducible problems through [Issues](https://github.com/Nicodemox87/ALiS/issues), excluding sensitive data and personal paths.
