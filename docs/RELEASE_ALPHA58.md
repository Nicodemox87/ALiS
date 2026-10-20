# ALiS 0.1.0-alpha.5.8 — Annotation and feature safety

**Windows x64 / CloudCompare 2.13.2 stable / Qt 5.15.2. Research preview.**

**[Download complete Windows installer](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.8/ALiS-0.1.0-alpha.5.8-Windows-x64-Complete-Setup-r1.exe)** · [Plugin-only installer](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.8/ALiS-0.1.0-alpha.5.8-CloudCompare-2.13.2-Windows-x64-Setup.exe)

## Annotation Studio fixes

- Rectangles use the actual mouse-release position, including quick drags or remote desktop events that omit the final mouse move. Reverse-direction drags select the same region.
- Polygon selection previews the moving edge between clicks and includes the double-click position when closing. **Enter** completes a polygon; **Backspace** removes its last vertex; **Esc** cancels.
- Invalid clicks/collinear outlines leave the prior selection intact. Ctrl-add and full-resolution selection continue to work.

Selection projects all eligible original points through the active slab, including occluded points. The display sample is only for drawing; it does not reduce the points being labelled.

## Feature computation protections

- Conservative RAM checks run before feature arrays/octree allocation, including output fields and temporary search buffers. The GUI checks available RAM while the workers run and requests cancellation if its reserve is exhausted.
- Up to four workers by default, with a maximum of eight when requested internally. This limits concurrent neighbourhood buffers and leaves CPU capacity for the application.
- Dense/large-radius requests are checked against the existing octree index **before** allocating search buffers. A conservative surrounding-cell upper bound of **200,000 candidates per worker** triggers a controlled error; it does not clip neighbours, change radii or sample the cloud.
- Cancellation is checked at cell boundaries, during candidate checks and between points. Failed/cancelled compute runs do not publish a partial feature cache or new Scalar Fields.

These guards reduce memory-exhaustion risk; they are not an operating-system memory quota. A conservative candidate bound can reject a request even if the true spherical neighbour count is lower. Review radii and available memory in that case. For large classification jobs use **Classification → Supervised → Prepare data for this model**, selecting a loaded cloud or LAS/LAZ on disk. It prepares spatial blocks with full-radius overlap and supports resume. A standalone block feature-export workflow without a trained model is future work.

## Installation and updates

The complete online installer detects/installs a compatible CloudCompare, installs ALiS and prepares isolated Python with ML/DL (CPU, or optional NVIDIA CUDA). Save work and close CloudCompare first, run the installer normally and confirm Windows administrator prompts. [Complete installation instructions](https://github.com/Nicodemox87/ALiS/blob/main/docs/WINDOWS_COMPLETE_INSTALLER.md).

Update detection now compares every numeric component of `alpha.5.8` and accepts complete installers as well as plugin-only packages. Existing alpha.5.7 users can download and run this installer directly.

This release supplies Windows binaries and cross-platform sources. The previous [macOS alpha.5.7 installer](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.7/ALiS-0.1.0-alpha.5.7-macOS-arm64-Setup.pkg) remains available; a Mac build of these corrections requires compilation and acceptance on macOS.

## Verification scope

Publication is gated on a fresh native build, the native regression suites (including real mouse-event selection tests and memory/candidate-limit failures), a clean-host complete CPU installation and installed LAS/LAZ block integration tests. Installation/test logs are retained as a GitHub Actions artifact. No new full-survey accuracy benchmark is claimed for this maintenance release. The installer is unsigned; SHA-256 files accompany the assets.
