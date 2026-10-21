# Alpha.5.9 release validation — 21 October 2026

Release: [v0.1.0-alpha.5.9](https://github.com/Nicodemox87/ALiS/releases/tag/v0.1.0-alpha.5.9).
Build source: `b864fc7eb877475b93a22410e400df2f38e80638`.
Both native builds and publication: [GitHub Actions run 39639787567](https://github.com/Nicodemox87/ALiS/actions/runs/39639787567). Job logs and evidence artifacts record actual results; no previous Mac binary was renamed or reused.

## Windows x64

- CloudCompare 2.13.2 source, official Qt 5.15.2 SDK and MSVC x64.
- **9/9 native CTest tests passed**, including progressive spatial blocks and annotation.
- **904 distribution integrity checks** passed (allowlisted contents, original worker/source bytes, checksums).
- **31 updater/installer unit tests** passed (shared Windows/Mac updater tests included).
- Complete setup installed CloudCompare, the plugin and isolated CPU ML/DL dependencies on a clean Windows CI host. **6 installed-worker preparation regressions passed** against the actual installed native executable and Python package.
- NVIDIA CUDA hardware execution and alternate-user UAC behavior were not retested in this CI release. Windows installers remain unsigned.

## macOS arm64

- Native Apple Silicon CI on the `xcode-27` runner; CloudCompare 2.13.2, Qt 5.15.19. Minimum macOS **27.0** is enforced by deployment audit and installer.
- **9/9 native CTest tests passed**. The painted spatial-panel test uses Qt's offscreen backend, as do the other painted UI tests; the Mac minimal backend failed this test. The native isolated worker still uses minimal. Assertions were not skipped or reduced.
- **87 native arm64 binaries** passed dependency-closure audit and ad-hoc signature verification before runtime assembly. Runtime deployment, dependencies and signatures are audited again in the complete installer.
- The actual PKG was expanded and its files/symlinks compared with the signed staging app. The relocated app loaded only bundled Python modules and thread libraries, with exactly one OpenMP runtime.
- **34 installed-worker Python tests ran: 33 passed, 1 CUDA-only test skipped**. Tests exercise all six CPU classifier paths, worker and preparation behavior; this is not a new independent archaeological accuracy benchmark.
- Signature remained valid after the tests. No Developer ID signature or notarization is claimed. New hands-on GUI acceptance on the user's M4 Max remains outstanding; the older alpha.5.7 M4 results are historical evidence only.

## Progressive feature checks on the development PC

The real isolated-worker regression passed **25,216 grid/edge/halo assertions**, **5,079 progressive SF/BIN resume/pause-stop/single-copy assertions**, **368 general processing checks**, and **22 feature-safety/update-selection checks**. These assertion counts are not numbers of independent datasets or accuracy experiments.

## Published-download verification

After publication, the actual public Windows complete installer, native installer and Mac PKG were downloaded again on the development PC. Their SHA256 values matched GitHub release asset digests and the release manifests:

| Installer | Bytes | SHA256 |
| --- | ---: | --- |
| Windows complete r1 | 2,104,042 | `5b98b07e0be67fc180e12f840afb0b0e0520cf13b449333f76780dd476a77c0a` |
| Windows plugin-only | 4,255,540 | `9eac9b4e69272c5e3713ab515a73ea07ad30c30e3cc17c2a9f5f4bc572233681` |
| macOS arm64 PKG | 171,550,063 | `8c84f855db1a6dcbd6376b2c3df4253680ba18166e2bd87479758c2d2866399f` |

This maintenance release does not claim new scientific validation on the full Piazza Armerina survey, guaranteed memory bounds enforced by the OS, production certification or automatic model transferability. See [progressive memory/persistence limits](PROGRESSIVE_FEATURES.md), [update privacy and consent](UPDATES.md), and [AI-assisted development provenance](AI_DISCLOSURE.md).
