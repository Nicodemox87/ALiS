# Public package checks — 29 September 2026

Version **0.1.0-alpha.5.7**, Windows x64, CloudCompare 2.13.2 stable / Qt 5.15.2.

## Completed development validation

- Native session integration: **92 checks passed**.
- Processing/workspace regression with UI: **375 checks passed**.
- Synthetic block integration: **5 tests passed in 14.700 seconds**, including disk LAS/LAZ, CC snapshot, exact ownership, halo seams, attribute preservation, saved feature reuse, cancellation/resume, checksum rejection and invalid-input handling. Passed using the installed native executable without an SDK plugin-path override; lazrs 0.8.2 was tested in an isolated dependency directory without modifying the active user runtime.
- Real LiDAR tile: **585,001 points**, four cores, 41 inputs; RF label agreement with whole-tile inference **100%**, maximum feature difference 2.33e-10. Both used identical global HAG.
- Local PointNet: **28,283 points**, four blocks, CUDA; **100% label agreement** with whole-crop inference, without decimation.
- New native binaries installed and loaded in the official local CloudCompare host. Installed/built binary hashes matched.

The earlier alpha.5.6 public worker suite had 26 passing tests; that historical count is not presented as a fresh alpha.5.7 full-suite run.

## Package verification

Release packaging checks archive integrity, required payloads, SHA-256 manifests, corresponding source, worker consistency and exclusion of survey/model/paper files. GitHub Actions checks the downloadable payload hashes before publishing. The Qt source asset is independently size/hash-checked against VERSION.json. The installer is built with Inno Setup and is **unsigned**.

These checks are not a clean-PC acceptance test, antivirus certification, independent-site accuracy assessment or proof of tiled-ground equivalence. No full-survey processing was launched for this release. Check the published hashes after downloading and review all research outputs.
