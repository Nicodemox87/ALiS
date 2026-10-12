# Complete Windows installer

**ALiS alpha.5.7, installer revision 1.** This new online setup wraps the unchanged native Windows plugin release. It does not include a new classification algorithm or silently replace historical release assets.

## One download, guided setup

1. Download `ALiS-0.1.0-alpha.5.7-Windows-x64-Complete-Setup-r1.exe` from the ALiS alpha.5.7 release. Compare its SHA-256 with `ALiS-Windows-Complete-r1-SHA256SUMS.txt`.
2. Save your work and close CloudCompare. Double-click the installer **normally**, not with Run as administrator.
3. Review the licences/download notice. Choose **CPU** (recommended, includes deep learning) or **NVIDIA CUDA 12.8** if you have a compatible GPU/driver.
4. The wizard finds compatible Python 3.12 x64 or installs it for your user, searches for CloudCompare 2.13.2 x64/Qt5, installs that host if missing, installs ALiS, and prepares an isolated ML/DL environment. Confirm the Windows administrator prompts for native components yourself.
5. Wait for verified success, then open CloudCompare from the ALiS shortcut. Restart an already-open terminal before launching from it. Load a small copy of a point cloud, select it, and open ALiS in Plugins.

Windows 10/11 x64, internet, and administrator credentials for native components are required. Allow at least **5 GiB free for CPU or 12 GiB for CUDA**; downloads can reach several GB. The installer does not install NVIDIA drivers. Windows ARM64 is not the target of this installer.

## What gets installed

- Compatible existing CloudCompare is reused. Otherwise the official 2.13.2 installer targets `C:\Program Files\CloudCompare-2.13.2-ALiS`. Existing incompatible folders are not overwritten, but the official host installer may update its Windows uninstall registration.
- The existing signed/unsigned status of upstream components is preserved; ALiS setup is **not signed**. Checksums verify download consistency, not independent author identity. Do not disable antivirus, SmartScreen or UAC.
- An existing Python 3.12 x64 is reused without adding global packages. If absent, the official 3.12.10 installer is used. It is the final full-maintenance Windows installer of that series, **not the latest security-only source release**; this compatibility baseline must be reviewed as the runtime evolves.
- Dedicated venv: `%USERPROFILE%\.alis\runtime-complete-r1-cpu` or `runtime-complete-r1-cuda`. No existing model catalogue, dataset or previous runtime is deleted.
- NumPy, SciPy, scikit-learn (RF, Extra Trees, HGB), XGBoost, joblib, threadpoolctl, laspy/lazrs and PyTorch for MLP/PointNet. No pretrained model weights or datasets are supplied.
- Pip uses explicit official package indexes and binary wheels, not a global interpreter. Top-level versions are pinned in `packaging/windows-complete/dependencies.json`; resolved transitive versions are recorded in `requirements-installed.txt`.

Native DLL loading, package consistency, tiny ML fit/predict, PyTorch forward/backward, the selected tensor device, LAZ write/read and worker capabilities must pass before `ALIS_PYTHON` is changed. These are installation smoke tests, not scientific accuracy validation. Geometry/RF are not made GPU algorithms by selecting CUDA.

## Errors, repair and removal

Logs and SUCCESS.json or FAILURE.txt: `%LOCALAPPDATA%\ALiS\InstallerLogs\<timestamp>`. Verified installers are cached at `%LOCALAPPDATA%\ALiS\InstallerCache`. Rerun to retry failed steps; a CUDA failure is explicit and CPU can be chosen on rerun. The wizard does not pretend to roll back independently installed prerequisites: a failure can leave CloudCompare/Python/ALiS installed, but preserves the previous selected runtime until checks succeed.

Uninstall the **ALiS plugin** through Windows Installed Apps. CloudCompare, Python, cached installers and user-owned runtimes are retained and can be reviewed separately. Do not delete shared Python installations or whole user-profile folders. No execution-policy or other security overrides are used.

The original plugin-only Setup.exe and manual ZIP remain available for administrators who already manage prerequisites; see [manual/plugin-only instructions](INSTALLATION.md).

## Provenance and validation

The host and Python download URLs are official vendor endpoints with pinned SHA-256. The existing ALiS installer is pinned to the original alpha.5.7 hash. See the source manifest and source archive for exact contents. A clean Windows CI install and smoke tests gate publication; UAC interaction under alternate administrator credentials and a real CUDA GPU still require machine-specific acceptance testing.
