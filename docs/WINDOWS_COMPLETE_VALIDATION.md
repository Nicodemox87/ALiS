# Windows Complete Setup r1 — validation record

Date: **12 October 2026**. Native plugin: **0.1.0-alpha.5.7**, unchanged. Installer source revision: [`0847c8c311bc3f637eb10bdcb67ba23ac7151a9b`](https://github.com/Nicodemox87/ALiS/commit/0847c8c311bc3f637eb10bdcb67ba23ac7151a9b).

## Published artifact

`ALiS-0.1.0-alpha.5.7-Windows-x64-Complete-Setup-r1.exe`

- Size: **2,104,039 bytes** (online bootstrap, not an offline dependency bundle).
- SHA-256: `4582f66dfb14a6c533f7534c1fb444e0eb21c69991c9cfcd85e1fd95f4406ccb`.
- Installer source ZIP SHA-256: `0f5990a7b01a6b90ce061c534c75042c9f6ec4cf7f85afeb9e53d2d54717ce86`.
- Release downloads were retrieved again and their hashes matched the published metadata.

## Automated acceptance

[Successful build, installation, regression and publication run](https://github.com/Nicodemox87/ALiS/actions/runs/38308759484), on GitHub's **windows-2022 x64** runner. This is a Windows Server CI environment, not a physical Windows 10/11 consumer PC.

| Check | Result / scope |
| --- | --- |
| Bootstrap unit suite | 14 passed: compatible host reuse, missing host, incompatible directory protection, version/architecture/Qt checks, approved download origins, manifest hashes, cache reuse and checksum failure. |
| Inno compilation | Successful. |
| Full CPU wizard | Successful exit; new CloudCompare host required and verified at `C:\Program Files\CloudCompare-2.13.2-ALiS`; ALiS installed; dedicated runtime activated only after checks. |
| Native payload | Installed plugin and block-worker SHA-256 matched the existing alpha.5.7 manifest; DLL dependency loading passed. |
| Runtime | Package consistency; tiny RF, Extra Trees, HGB and XGBoost fits/predictions; PyTorch CPU matrix computation and neural forward/backward; LAZ round trip; installed worker capabilities passed. PyTorch `2.10.0+cpu` verified after setup. |
| Installed-worker block suite | 5 passed. Python package origin asserted under the installed host, not the source checkout; native worker from the installed host. |
| Block regression coverage | LAS/LAZ equivalence, core/halo seams, original point IDs and attributes, prediction/resume, cancellation/integrity, CloudCompare snapshot fields, and rejection of unsupported units/fields/budgets. Synthetic clouds only. |
| Publishing | Four separately named complete-setup assets added after successful tests; no replacement of historical release files. |

[CI evidence artifact](https://github.com/Nicodemox87/ALiS/actions/runs/38308759484/artifacts/11774430584) contains setup logs, SUCCESS.json and the resolved package list. GitHub login may be required; artifact retention is 30 days. Workflow logs and source document the checks after that expiry.

Local read-only checks also found the existing compatible CloudCompare and successfully loaded its native ALiS DLL. The local CloudCompare session was not closed or modified for these checks. The installation infographic was rendered and inspected.

## Limits and remaining acceptance work

- No fresh CUDA installation on a physical NVIDIA computer was performed for this installer revision. The CUDA option performs a real tensor computation before activation and fails explicitly if the GPU/driver is unsuitable.
- CI uses an explicit administrator-test override; it does not simulate interactive UAC, alternate administrator credentials, restricted institutional policies, proxies or SmartScreen. Those need real-user acceptance testing.
- Python discovery/installation is part of the wizard. This run is not a separate test matrix of every Python-present/Python-absent and registry configuration.
- These are **installation and software regression checks**, not accuracy measurements, a full six-model scientific benchmark or a new full-survey experiment.
- Top-level package versions and native installers are pinned. Transitive package versions are resolved by pip and recorded, rather than supplied as a fully hashed offline wheel lock.
- On partial failure, previously installed prerequisites may remain. Earlier runtime selection is preserved until the final successful activation; this is not a transactional rollback of CloudCompare/Python installations.

See [installation and recovery instructions](WINDOWS_COMPLETE_INSTALLER.md).
