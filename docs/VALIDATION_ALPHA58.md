# ALiS alpha.5.8 — maintenance validation

Date: 20 October 2026. Scope: Annotation Studio input, geometric-feature resource handling and Windows packaging. This record describes software checks; it does not measure archaeological classification accuracy.

## Local native checks

Built from the public source tree against pinned CloudCompare `49dbbb662f296c7780aae717897c85b3cb3764ed`, CCCoreLib `a8ce4270dfa41d5817842447b27f36094f9dfb51` and official Qt 5.15.2 MSVC2019_64. All **8 native suites passed (782 checks)**:

| Suite | Checks |
| --- | ---: |
| Processing/workspace | 368 |
| Persistence | 84 |
| Feature safety and update versions | 15 |
| Annotation Studio | 95 |
| General logic | 31 |
| Workspace logic | 88 |
| Session | 92 |
| Ground filter | 9 |

Selection tests send real Qt mouse/key events to canvas handlers: fast rectangle release without a move, reverse dragging, polygon hover/final vertex/Enter/Backspace, Ctrl-add, all camera projections, full-resolution point IDs, sections and live view sync.

Feature tests reject a one-byte memory budget before building the octree, reject dense candidate searches before allocation, retain prior completed cache after errors/cancellation, and compare accepted parallel values with serial results using a `2e-5` floating-point tolerance.

The new DLL also passed native loading against the existing official CloudCompare installation. No local survey was processed or modified for this maintenance check.

## Python and worker checks

All 14 complete-installer unit tests passed locally. Local block integration ran 6 tests: 5 passed, with compressed LAZ skipped because the existing local CUDA environment lacks lazrs. The CI installation supplies lazrs and runs that test against the newly installed worker.

A synthetic 200,001-point dense block tests the actual native worker failure path: non-zero exit, a readable neighbourhood-limit error and no committed `features.f32`. Further block tests cover point IDs, halo/core boundaries, supplied fields, resume/cancellation and unsupported-input rejection.

## Independent Windows acceptance

[Build, clean-host CPU installation and publication workflow](https://github.com/Nicodemox87/ALiS/actions/runs/39394561216) **passed**. All 8 native suites, 14 bootstrap unit tests, complete CPU installation and all **6 installed-worker block tests (including LAZ, no skips)** passed. The 200,001-point dense-error regression passed against the installed executable. The release was published after these checks succeeded.

[Setup and test logs](https://github.com/Nicodemox87/ALiS/actions/runs/39394561216/artifacts/12250924518) are retained for 30 days as an Actions artifact. The release's JSON manifests and SHA-256 files identify the actual published binaries; downloads were fetched again and hashes verified locally. Both native and complete installer source archives were included.

The CI machine is a Windows Server 2022 x64 runner. It does not reproduce every physical Windows PC, interactive UAC policy or GPU driver. A fresh CUDA installation and a native Mac build of these changes remain separate acceptance work.

## Resource limits

The RAM preflight is conservative and the live monitor requests cancellation; neither is an OS memory quota. The 200,000-candidate cell-cube bound can exceed the true spherical neighbour count. Rejected requests require reviewed scales/memory/block settings; neighbours are never silently truncated. Large classifications can use the existing model-driven block workflow from a loaded cloud or LAS/LAZ on disk. General standalone block feature export is not implemented in this release.
