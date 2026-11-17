# ALiS 0.1.0-alpha.5.21 — Windows research preview

This release brings the locally developed alpha.5.10–5.21 work into the public CloudCompare plugin. **Windows x64, CloudCompare 2.13.2 / Qt5 only. Updating macOS to these capabilities is the next roadmap milestone; no matching Mac binary is included.** The previous Mac alpha.5.9 assets remain available, clearly labelled as older.

## Download and install

Use the Windows plugin ZIP and its included verified installer scripts. An existing compatible CloudCompare installation is required; this package does not install CloudCompare, Python, CUDA or drivers. Save the project and close CloudCompare before updating. Verify the release checksums. This unsigned research preview requires operator review, not disabled security protections.

**[Complete step-by-step user guide (Italian)](https://github.com/Nicodemox87/ALiS/blob/main/docs/USER_GUIDE_IT.md)** · [Windows installation](https://github.com/Nicodemox87/ALiS/blob/main/docs/INSTALLATION.md) · [Source/build instructions](https://github.com/Nicodemox87/ALiS/blob/main/BUILDING.md)

## Features and changes

- **Annotation Studio density:** 10–100%, in 10% steps, independently in every view; per-panel percentage/count, explicit Apply, memory warning, full-resolution annotation selections. No hidden 120k-point cap. Sections/visibility still restrict eligible points. GPU buffers are chunked; this is not an out-of-core viewer.
- **Persistent geometric features:** add/update scalar fields in the current cloud, full-cloud copy or explicit Disk-only output. Bounded adaptive core/halo processing, integrity-checked checkpoints, pilot/estimates, resume and direct recovery of saved blocks into SFs. New scales preserve existing fields. Names without finite values are not accepted as computed features.
- **Unified disk-feature LAS export:** current CC geometry/attributes plus external feature Extra Bytes, streamed without a full N×K feature array; coverage/name/radius mapping and integrity checks. Preserve the source and linked job folder.
- **Scalar-field manager:** field/statistics search, comparison operators, explicit value analysis, select/deselect filtered fields and guarded removal.
- **Display/processing scope:** class/scalar filters and separate opt-in frozen point selection. Supported operations respect it; unsupported paths refuse it explicitly rather than silently using the full cloud.
- **Requested semantic classes (experimental):** reuse the applied Ground, separate roof/wall/pole/wire custom classes, current HAG provenance checks, real multiscale geometry requirements, explained/tunable thresholds, Derived review before Apply. Rule scores are not calibrated probabilities.
- **Clustering robustness:** Ground excluded from non-ground fits; regularised/fp64 Gaussian Mixture handling and BLAS/OMP thread controls. Cluster IDs remain exploratory groups, not automatic ASPRS labels.
- **Building cleaning:** preview and undoable Apply for misclassified canopy/return evidence in Building/Roof/Wall, preserving Ground and non-target classes; weak or missing evidence requires review.
- **DFM workflow:** MIN Ground + MIN Building → merged MIN, or MIN Ground/MAX Building; selectable structural classes; mandatory overlap-conflict map with rectangle choices, Undo/Clear and recorded decisions; final void fill using local mean, IDW, TIN or ordinary Kriging with local support guards. Separate grid-cloud output with source-quality fields.
- **Existing workflow retained:** CSF/PMF presets, DTM/HAG, manual annotation, six supervised classifiers, spatial/external validation, model-driven disk preparation, history and recipe reuse.

## Validation scope

Fresh local Windows alpha.5.21 regression run: **110 Annotation Studio, 249 recovery/resume/semantic, 512 processing, 92 session and 2,061 feature-safety checks passed**, plus **4 Python clustering regressions**. The recovery/processing counts exclude additional optional screenshot checks enabled in previous GUI-fixture runs. Native x64/Qt5 load compatibility was verified. The installed alpha.5.21 was reopened successfully with a 29,664,478-point BIN and 129 recovered feature fields; each of four Studio views displayed 2,966,448 eligible points at 10%. No claim of a full-size 100% performance benchmark is made.

Tests establish bounded software behaviours, not classification accuracy or DFM scientific validity on an independent archaeological site. Requested-class geometry, building cleaning, neural classification and interpolation decisions require expert review. Original survey datasets, trained models, private paths and working outputs are not release assets.

## Known limits and next step

- **Next: macOS update, rebuild/package and native regression/GUI validation.** The new Windows release is not a Mac update.
- Large in-cloud SF matrices still require about 4×points×fields bytes; Disk only is deliberately different from visible full-cloud SFs.
- Display/processing subset support is not universal; native external CloudCompare commands are outside the ALiS scope guard.
- DFM outputs a grid cloud; no new dedicated ALiS GeoTIFF exporter is claimed.
- No pretrained semantic/foundation model, guaranteed unsupervised Building accuracy, or GPU acceleration for all processing is included.
- This release is the CloudCompare plugin, not the separately explored standalone application.
- The existing Windows update checker searches for complete-installer assets. This ZIP-only update must be downloaded from the release page; no new complete online EXE is advertised.

Developed by Dr Nicodemo Abate (CNR-ISPC) with OpenAI ChatGPT/Codex assistance. [Development-provenance disclosure](https://github.com/Nicodemox87/ALiS/blob/main/docs/AI_DISCLOSURE.md). Code GPL-2.0-or-later; original documentation CC BY 4.0, excluding third-party material and institutional marks.
