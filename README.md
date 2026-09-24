<p align="center">
  <img src="docs/media/alis-banner.svg" alt="ALiS — Archaeological LiDAR Studio. From point clouds to reviewable evidence." width="100%">
</p>

<p align="center">
  <strong>A CloudCompare plugin for archaeological point-cloud analysis.</strong><br>
  Prepare data. Explore geometry. Annotate, classify and validate.
</p>

<p align="center">
  <img alt="Development status: research alpha" src="https://img.shields.io/badge/status-research_alpha-d9a441?style=flat-square">
  <img alt="Target: CloudCompare 2.13.2" src="https://img.shields.io/badge/CloudCompare-2.13.2-193b56?style=flat-square">
  <img alt="Platform: Windows x64" src="https://img.shields.io/badge/platform-Windows_x64-193b56?style=flat-square">
  <a href="LICENSE"><img alt="Software licence: GPL 2.0 or later" src="https://img.shields.io/badge/code-GPL--2.0--or--later-168b80?style=flat-square"></a>
</p>

<p align="center">
  <a href="#getting-started">Getting started</a> ·
  <a href="#capabilities">Capabilities</a> ·
  <a href="#workflow">Workflow</a> ·
  <a href="https://github.com/Nicodemox87/ALiS/releases">Releases</a> ·
  <a href="https://github.com/Nicodemox87/ALiS/issues">Report an issue</a>
</p>

---

## Why ALiS?

Archaeological LiDAR interpretation is not just a classification problem. It requires checking the data, separating terrain from vegetation and structures, choosing meaningful spatial scales, and reviewing uncertain results.

**ALiS brings these steps into a connected CloudCompare workflow**, combining geometric analysis, expert annotation and reusable classification models. Its purpose is to support interpretation and reproducible analysis—not to replace archaeological judgement.

> **Publication status:** this repository currently presents the project. Source code and installation packages are being prepared for upload. The current local development distribution is **0.1.0-alpha.5.4**; no downloadable GitHub release is claimed here.
>
> **Research alpha:** outputs require operator review. Performance on one survey does not establish accuracy on a different site, sensor or acquisition configuration.

<p align="center">
  <img src="docs/media/point-cloud-preview.png" alt="Oblique view of a manually labelled LiDAR point cloud at Piazza Armerina, with ground in brown, vegetation in green and buildings in red." width="800">
</p>

<p align="center"><sub><strong>Piazza Armerina, Sicily.</strong> Display-sampled view of the manually labelled reference cloud: ground (brown), vegetation (green), buildings (red). This is a dataset illustration, not an automatic-classification result or an application screenshot.</sub></p>

## Capabilities

| Workspace | What you can do |
| :--- | :--- |
| **Session & display** | Inspect cloud statistics and available metadata; display RGB or scalar fields; adjust palettes and ranges. Cached statistics avoid unnecessary recalculation. |
| **Preparation** | Access CloudCompare outlier, noise and subsampling tools; retain control over the data used downstream. |
| **Terrain** | Apply cloth simulation filtering (CSF) or progressive morphological filtering; review ground labels; derive a DTM and height above ground where supported. |
| **Multiscale features** | Compute geometric descriptors and neighbourhood statistics at selected scales; inspect the resulting scalar fields. |
| **Annotation Studio** | Work with simultaneous views and movable metric sections; select points with rectangle, polygon, lasso and similarity tools; assign and review labels. |
| **Classification** | Train, compare and reuse supervised models; cluster existing RGB/scalar-field inputs; explore vegetation–structure separation with manual review. |
| **Validation & reuse** | Inspect metrics, confusion matrices and confidence; export reports; retain models and their metadata in a local catalogue. |

## Workflow

<p align="center">
  <img src="docs/media/workflow.svg" alt="Import and inspect, prepare, derive terrain and features, choose manual annotation, supervised classification or clustering, then validate and export. Review can lead back to labels and settings." width="100%">
</p>

**Choose only the steps your task needs.** Existing scalar fields can be used for clustering without recomputing features. Manual review is part of the process: a vegetation–structure proposal is not a verified archaeological interpretation.

## Classification, with the differences made explicit

| Approach | Available methods in the development build | Intended use |
| :--- | :--- | :--- |
| **Supervised machine learning** | Random Forest, Extra Trees, Histogram Gradient Boosting, XGBoost | Learn from labelled examples, assess held-out performance and reuse compatible saved models. |
| **Neural classification** | Descriptor-based MLP; experimental PointNet-style local XYZ network | Compare learned nonlinear or local spatial representations with descriptor-based models. |
| **Unsupervised clustering** | MiniBatch K-Means, BIRCH, Gaussian Mixture | Explore groups in selected inputs. Cluster IDs are not automatically semantic or ASPRS classes. |
| **Vegetation / structures** | Adaptive geometric proposals and feature-range review | Assist separation after ground extraction, retaining uncertain points for validation. Remaining points are not automatically buildings. |

<details>
<summary><strong>Deep learning and GPU: what is—and is not—included</strong></summary>

The PointNet-style classifier operates on local XYZ neighbourhoods. It is **not canonical PointNet with T-Nets, PointNet++, or a bundled pretrained foundation model**. No external pretrained weights are supplied.

Supervised CUDA paths are available for XGBoost, MLP and the local PointNet-style model when their dependencies, driver and GPU are compatible. Other operations remain CPU-based; PointNet neighbourhood lookup also uses the CPU. GPU acceleration is not a blanket guarantee of faster processing.

</details>

<details>
<summary><strong>Validation and responsible interpretation</strong></summary>

- Keep training and test data separate; favour spatially separated evaluation or an independent labelled cloud over testing on training points.
- Inspect per-class metrics and confusion matrices, not only overall accuracy.
- Model confidence is not necessarily a calibrated probability of correctness.
- Density, sampling geometry, occlusion, return metadata and feature scales affect model transfer.
- Review ground labels before terrain modelling, particularly below vegetation and near structures.
- Generalisation to independent archaeological sites remains an evaluation objective, not an established guarantee.
- Load only trusted model files: some serialisation formats can execute code.

</details>

## Getting started

**Packages are not yet uploaded.** Once a release is published, download its installer and follow the matching installation notes from [Releases](https://github.com/Nicodemox87/ALiS/releases).

| Component | Current development target |
| :--- | :--- |
| Operating system | Windows x64 |
| Host application | CloudCompare **2.13.2 stable**, **Qt 5.15.x** |
| Native operations | Terrain, features and annotation do not require Python |
| ML / DL operations | Separate Python runtime; optional XGBoost and PyTorch dependencies |
| GPU | Optional; supported methods and compatible CUDA hardware/runtime only |

The plugin is **not a standalone CloudCompare application** and is not compatible with arbitrary CloudCompare or Qt 6 builds. CloudCompare, Python, CUDA, training clouds and trained models are not bundled with the plugin installer.

The intended installation sequence is:

1. Install the matching official CloudCompare build.
2. Close CloudCompare and run the ALiS installer from a published release.
3. Select the directory containing `CloudCompare.exe`.
4. Reopen CloudCompare, load a cloud, select it and open ALiS.
5. Inspect metadata and coordinate units before processing.

## Roadmap

Work in progress—not a list of shipped capabilities:

- Broader validation across archaeological sites and acquisition conditions.
- Improved tutorials, worked examples and documented reproducibility checks.
- Validated adapters for external pretrained models.
- Community-contributed datasets and model sharing with provenance, compatible labels and quality review.

## Feedback & collaboration

Use [GitHub Issues](https://github.com/Nicodemox87/ALiS/issues) for bugs, ideas and feature requests. Include the ALiS version, CloudCompare version, operating system, processing steps and relevant logs. Remove personal paths and sensitive information before sharing files.

**Contact:** [nicodemo.abate@cnr.it](mailto:nicodemo.abate@cnr.it)

## Author & affiliation

**Created by Dr Nicodemo Abate**  
Tecnologo — CNR-ISPC, Sede Secondaria di Milano  
Consiglio Nazionale delle Ricerche — Istituto di Scienze del Patrimonio Culturale

<img src="docs/media/cnr-ispc.png" alt="CNR-ISPC institutional logo" width="240">

The affiliation identifies the author's institution; it does not imply institutional certification of the software or its outputs.

## Citation

Zenodo deposition and associated scientific publications are in preparation. Verified DOI links and formal citation metadata will be added when available. For now, identify **ALiS — Archaeological LiDAR Studio**, Nicodemo Abate, the software version used and this repository URL in your methods.

## Licence

**Software:** GNU General Public License, version 2 or later (**GPL-2.0-or-later**). See [LICENSE](LICENSE). Third-party components retain their respective licences.

**Original documentation:** unless otherwise indicated, [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/). Institutional logos and third-party materials are excluded and remain subject to their respective rights. See [media notes](docs/media/README.md).

Research-alpha status describes maturity; it does not add a non-commercial or research-only restriction to the software licence.
