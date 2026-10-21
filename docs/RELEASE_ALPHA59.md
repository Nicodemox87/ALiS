# ALiS 0.1.0-alpha.5.9 — Progressive features and update transparency

Research testing preview, 21 October 2026. Not a certified production release.

## Changes

- **Prepare → Features → Spatial blocks:** RGB/elevation XY preview, metric grid, selected blocks or all blocks, progress colours, pilot estimate, pause/stop/retry, bounded isolated processing with radius-preserving halo.
- Progressive scalar fields on the current cloud, or one full-cloud copy: no geometry copy for every completed block. Pending rows remain NaN. Compact row/value checkpoints and BIN persistence record completed cores, exact recipe and ordered-geometry fingerprint. See [limits and resume instructions](PROGRESSIVE_FEATURES.md).
- Explicit selected-block training subset, without treating generated labels as certified training.
- Existing annotation selection and conservative feature safety corrections retained.
- Optional startup update checks on both platforms; Settings controls and manual checks. No silent download/installation on Windows; Mac downloads remain explicitly confirmed and verified.
- Transparent AI-assisted development statement in Credits and project documentation, with human scientific responsibility and primary regulatory references.

## Installation and compatibility

Windows x64: use the complete installer for CloudCompare 2.13.2/Qt5 detection or installation and isolated CPU/optional CUDA ML runtime. Plugin-only setup remains available for compatible hosts. macOS has a separate native arm64 package, not a Windows DLL; minimum OS and integrity metadata are in `ALiS-macOS-arm64.json`. A Mac package is downloadable only after its native build, tests and payload audit succeed. Never rename an old binary to match this version.

Both platforms remain unsigned/not notarized research distribution as documented in their installation guides. Preserve original data and save work before updating.

## Validation and limits

Native regression tests cover full-resolution grid ownership, halo bounds, progressive NaN fields, preservation of source labels, compact checkpoints, BIN save/reopen/resume, cancellation and single-copy output. A new real-survey scientific accuracy benchmark is not claimed by this maintenance release. Final full-cloud scalar fields still consume RAM proportional to point count × field count; disk streaming is available in model-driven preparation, not the standalone loaded-cloud spatial panel. Compact journals alone do not restore fields into an independently reimported LAS: save and reopen the BIN.

Update selection tests reject drafts, foreign URLs, absent/wrong-platform installers and older versions. Network failures never imply successful updates. [AI disclosure](AI_DISCLOSURE.md) and [update/privacy behaviour](UPDATES.md) are included with the distribution.
