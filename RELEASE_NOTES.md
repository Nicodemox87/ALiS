# ALiS 0.1.0-alpha.5.6 — public testing package

For **CloudCompare 2.13.2 stable / Qt 5.15.x / Windows x64** only.

- Native plugin, worker source, supervised ML and experimental local PointNet-style model.
- Terrain filtering, multiscale features, multi-view annotation, geometric separation and clustering.
- Approved ALiS tree/temple identity and CNR-ISPC affiliation mark.
- Public installer, manual-install ZIP, corresponding source archive and SHA-256 checksums.
- Installation, CPU/CUDA runtime, build, reporting and security instructions.

This public packaging revision preserves the existing alpha.5.6 native DLL and scientific code. It replaces unpublished study documentation in the distribution with public getting-started documentation and adds an optional CPU-runtime setup script. No survey data, trained models or unpublished paper are distributed.

## Install

Install the matching [official CloudCompare host](https://github.com/CloudCompare/CloudCompare/releases/tag/v2.13.2), close it, then run Setup.exe and select its root directory. See [Getting started](docs/GETTING_STARTED.txt). The ZIP is an alternative manual installation, not a standalone host.

Python dependencies are separate downloads for ML/DL. Native terrain, features and annotation do not require them. The installer is **not code-signed**; verify its SHA-256 and origin. Do not disable antivirus or Windows security protections.

## Validation scope

The alpha.5.6 native build passed 1,074 existing checks and Qt 5 resource rendering checks in the development environment. Public packaging is additionally checked for native loading against the installed official host, archive contents, source/worker consistency and download hashes. These are software checks, not a claim of cross-site classification accuracy or a clean-PC acceptance test.

## Known limitations

- Research alpha: review all outputs; keep original clouds unchanged.
- No external pretrained weights. The local XYZ PointNet-style model is not canonical PointNet/T-Net.
- Geometric OtherSurface/candidate structures are not automatically verified buildings or archaeology.
- Models must match input field definitions, scales, label semantics and software dependencies.
- No universal automatic CRS reconstruction when metadata are absent.
- Performance depends on density, chosen scales, RAM and backend; CPU operations remain CPU-based.
- Some worker help text refers to development-only benchmark reports not included here.

Report reproducible problems through [Issues](https://github.com/Nicodemox87/ALiS/issues), excluding sensitive survey data or paths.
