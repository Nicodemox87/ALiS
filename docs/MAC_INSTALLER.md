# ALiS for macOS — complete Apple Silicon installer

ALiS **0.1.0-alpha.5.7, Mac build 1** is available alongside the existing Windows release. The Mac installer includes CloudCompare 2.13.2, ALiS, Python 3.12 and all six CPU classifiers. It is a research preview, not a production-certified product.

**[Download macOS (.pkg)](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.7/ALiS-0.1.0-alpha.5.7-macOS-arm64-Setup.pkg)** · [Checksums](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.7/ALiS-macOS-SHA256SUMS.txt) · [Release](https://github.com/Nicodemox87/ALiS/releases/tag/v0.1.0-alpha.5.7) · [Windows guide](INSTALLATION.md)

## Requirements and contents

- **Apple Silicon (M1 or later), macOS 27 or later.** This package does not support Intel Macs or macOS 15/26: several bundled native libraries require macOS 27.
- Approximately **173 MB** to download and **670 MB** installed. Allow at least 2 GB free for download, installation and a backup of an existing app; cloud processing needs additional storage and RAM appropriate to the job.
- An administrator must authorize installation into `/Applications/ALiS.app`. Institutional security policies may require IT assistance.
- No Homebrew, Xcode, system Python or network dependency download is needed to install or begin using ALiS. Network access is used for optional GitHub update checks.
- Bundled: CloudCompare 2.13.2 / Qt 5.15.19; Python, NumPy, SciPy, scikit-learn, XGBoost, PyTorch, laspy and lazrs. Random Forest, Extra Trees, Histogram Gradient Boosting, XGBoost, MLP and the experimental PointNet-style model run on CPU. CUDA is unavailable on macOS; Metal/MPS is not enabled.
- No training clouds, pretrained weights, private models or confidential reports are included. Load only trusted saved models.

## Install and authorize

1. Download the `.pkg` from the official release. The GitHub-generated “Source code” archives are not installers; Windows `.exe` and `.zip` packages are separate downloads.
2. Optionally verify the installer against `ALiS-macOS-SHA256SUMS.txt`. SHA256: `4b7bf8c5064989b1951899ed2687af1451d4394aa5205ca62462669fd6dd10bc`.
3. Save any ALiS session and quit ALiS and its processing jobs. Open the package, follow Installer and authorize with your administrator credentials or Touch ID when prompted.
4. This preview is **not signed with Apple Developer ID and is not notarized**. Its app has an ad hoc integrity signature. If macOS blocks the package, first attempt to open it, then open **System Settings → Privacy & Security → Open Anyway** and confirm. Repeat for ALiS if needed on first launch. Do not disable Gatekeeper. [Apple's instructions](https://support.apple.com/en-us/102445).
5. Open **Applications → ALiS**. Its workspace appears automatically. No separate Python setup is required.

## First cloud and everyday work

Start with a small copy of a LAS/LAZ cloud, select it in the DB tree, inspect attributes and confirm coordinate units. If CRS information is absent, leave it unknown; do not invent an EPSG. Native terrain, geometric features and annotation are available through the workspace. Classifiers use the bundled runtime automatically, without copying or reusing a Windows environment.

For an existing trusted model, use **Classification → Supervised workflow → Prepare data for this model**. Keep training and evaluation points spatially separate. Review the model report and class-specific metrics before applying predictions. Terrain/HAG requires explicit compatible settings. [Model preparation](MODEL_PREPARATION.md).

## GitHub updates

The app checks `Nicodemox87/ALiS` releases in the background at most once every 24 hours, including testing prereleases. It sends no clouds, models or research data.

**Plugins → ALiS → ALiS Settings → Updates** lets you disable automatic checks or choose **Check GitHub for Mac updates now**. The updater selects only an Apple Silicon package compatible with the current macOS and compares both the release version and Mac build number. A Windows-only release is not a Mac update.

When an update exists, confirm the download. ALiS verifies its size and SHA256 against the release manifest and compares the GitHub digest when available. The verified package is selected in Finder. **Save your work, quit ALiS, then open it.** Installation requires normal macOS authorization; the app does not silently replace itself or close your work. Quarantine is preserved. Checksums provide integrity relative to the GitHub manifest, not independent Apple publisher authentication.

Offline, the installed app continues to work. A failed manual check reports an error rather than claiming the app is up to date. Automatic failures remain quiet and can be checked manually.

## Existing installations, backup and removal

The package owns only `/Applications/ALiS.app`. Before replacing it, Installer preserves the whole previous bundle in `/Library/Application Support/ALiS/Backups/ALiS-<timestamp>-<id>.app`, accessible to an administrator. Backups require space and are not automatically deleted.

Other CloudCompare installations, third-party plugins outside ALiS, shared dependencies, preferences, external clouds, models and reports are retained. Plugins added inside an older ALiS bundle remain in its backup; verify their ABI/host compatibility before adding them to the new version. Keep personal files outside ALiS.app, because the active bundle is replaced on update. The older dedicated `~/.alis/runtime-macos` is retained but is no longer required by this installer.

To uninstall, quit ALiS and move ALiS.app to the Trash. External data, settings and backups remain. To restore an earlier app, close ALiS and have an administrator copy the verified backup to Applications.

## Validation and limitations

Tested on **M4 Max, macOS 27.0.1**: 7 native tests, 32 Python tests passed (one CUDA-only test skipped), and 16 updater tests. All six classifiers completed synthetic training/prediction. The extracted and installed app matched the signed payload, and the installed GUI's GitHub check was verified. A second clean Mac has not yet been tested. These checks do not certify scientific accuracy or full-survey performance. [Validation details](MAC_INSTALLER_TESTS.md).

## Build and publish a later Mac version

Developer prerequisites: Xcode Command Line Tools and Homebrew `cmake ninja qt@5 eigen laszip python@3.12 libomp`. End users do not need these tools.

1. Run `bash tools/build_macos.sh` to build the pinned CloudCompare host, apply tracked patches and run native tests.
2. Set the version, Mac build and OS requirement in `packaging/macos/mac-release.json`; keep the installer distribution/preinstall requirements consistent. Run `python3 tools/build_macos_installer.py`. It downloads SHA256-pinned portable Python, installs pinned wheels, unifies OpenMP, audits every native dependency/deployment target, signs the app ad hoc and creates the installer plus `ALiS-macOS-arm64.json` in `output/mac-installer`.
3. Run `python3 tools/verify_macos_installer.py <package.pkg> <new-extraction-directory> --stage output/mac-installer/stage/Applications/ALiS.app`, then verify actual installation and GUI behavior. A lower compiler deployment target alone does not make bundled libraries compatible with older macOS.
4. Attach the matching `ALiS-<version>-macOS-arm64-Setup.pkg` and **`ALiS-macOS-arm64.json`** to release **`v<version>`**; upload the manifest last. Increase the Mac build for a later package of the same release. Preserve existing Windows assets and publish the corresponding Mac sources/checksums. Never publish private validation datasets or backups.
5. Future Developer ID signing/notarization requires Apple credentials and a separately validated signing pipeline. Regenerate the manifest on the final signed/notarized package before upload.

**Mac source:** use the separately named Mac source ZIP and corresponding source archives attached to the release. The existing tag and GitHub-generated source archive remain associated with the original Windows release; they are not the full Mac port. See [Mac third-party sources](../third_party/macos/README.md).

## Installazione rapida in italiano

Scarica il pacchetto **Mac**, chiudi ALiS e avvialo. Servono Apple Silicon e macOS 27 o successivo. Autorizza l'installazione; se macOS blocca il file, dopo il tentativo di apertura usa **Impostazioni di Sistema → Privacy e sicurezza → Apri comunque**. Apri ALiS da Applicazioni: Python e tutti i classificatori sono già inclusi. Gli aggiornamenti si controllano in **ALiS Settings → Updates**; dopo il download salva, chiudi ALiS e apri il nuovo installer. I file Windows rimangono disponibili separatamente.
