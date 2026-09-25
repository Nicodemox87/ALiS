# Third-party components and notices

ALiS original software is GPL-2.0-or-later. Keep original notices in source files; no licence is granted over institutional marks. Documentation is CC BY 4.0 except identified third-party content. This inventory is not a relicensing of dependencies.

| Component | Relationship | Licence/source |
| --- | --- | --- |
| CloudCompare 2.13.2 / CCCoreLib / plugin API | Separately installed native host and linked libraries | [Pinned upstream source and licences](https://github.com/CloudCompare/CloudCompare/tree/49dbbb662f296c7780aae717897c85b3cb3764ed) |
| qCSF algorithm | Compiled unchanged into ALIS_PLUGIN.dll from pinned CloudCompare source | Source headers say GPL v2 or later; upstream also supplies a GPL v3 LICENSE. Both are retained with the algorithm source under third_party/qCSF. |
| Qt 5.15.2 | Dynamically linked; supplied by the separately installed host | [Qt open-source licensing](https://www.qt.io/licensing/open-source-lgpl-obligations); module-specific LGPL/GPL notices remain applicable. No Qt binaries bundled here. |
| NumPy, SciPy, scikit-learn | Optional separate Python installations | BSD-family; retain each wheel's notices. |
| joblib / threadpoolctl | Optional separate Python installations | BSD-family licences in their distributions. |
| XGBoost | Optional separate Python installation | Apache-2.0. |
| PyTorch | Optional separate Python installation | BSD-style with additional bundled third-party notices. |
| laspy | Optional separate Python installation | BSD-2-Clause. |
| Inno Setup | Installer compiler | Inno Setup licence: https://jrsoftware.org/files/is/license.txt |

The `third_party/qCSF/ORIGIN.md` file identifies the exact upstream source location. The full corresponding ALiS source is distributed alongside binaries; obtain host dependencies from their named upstream projects. Source archives do not include independent commercial or proprietary components, LiDAR survey data or pretrained weights.

qCSF method: Zhang et al. (2016), *An Easy-to-Use Airborne LiDAR Data Filtering Method Based on Cloth Simulation*, Remote Sensing 8(6), 501. https://doi.org/10.3390/rs8060501

CNR-ISPC marks identify author affiliation; they do not imply institutional certification or grant trademark rights.
