# Install ALiS on Windows

**For ALiS 0.1.0-alpha.5.6 — research testing preview.**

[Download ALiS installer](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.6/ALiS-0.1.0-alpha.5.6-CloudCompare-2.13.2-Windows-x64-Setup.exe) · [All release assets](https://github.com/Nicodemox87/ALiS/releases/tag/v0.1.0-alpha.5.6) · [Back to home](../README.md)

![Six installation steps, with a separate optional Python runtime step.](media/install-guide.svg)

## 1. Install the matching CloudCompare version

ALiS is a plugin, not a standalone application. This binary requires **Windows x64, CloudCompare 2.13.2 stable and Qt 5.15.x**. It is not built for CloudCompare 2.14, Qt 6, Conda builds, macOS or Linux.

Open the [official CloudCompare downloads page](https://www.cloudcompare.org/release/). Find **2.13.2**, then choose **Windows 64bits — Installer version**. Do not choose the beta section or ccViewer. You can also use the [official 2.13.2 Windows installer](https://www.cloudcompare.org/release/CloudCompare_v2.13.2_setup_x64.exe).

Run that installer and note the installation directory. The default is normally `C:\Program Files\CloudCompare`. If CloudCompare is already installed, check its version under **Help → About** before proceeding. CloudCompare's GitHub release contains source archives; the Windows installer is supplied on its official downloads website.

## 2. Download ALiS — choose the installer, not the source

Open the [ALiS alpha.5.6 release](https://github.com/Nicodemox87/ALiS/releases/tag/v0.1.0-alpha.5.6). Expand **Assets** if necessary and download:

`ALiS-0.1.0-alpha.5.6-CloudCompare-2.13.2-Windows-x64-Setup.exe`

| File | Intended use |
| --- | --- |
| `...-Setup.exe` | Recommended: guided installation into an existing CloudCompare folder. |
| `...-Windows-x64.zip` | Alternative manual installation; the same plugin and worker payload. |
| `...-Source.zip` / GitHub “Source code” | Developers: source files, not an executable installation. |
| `CHECKSUMS_SHA256.txt` | Verify that downloaded files match this release. |

You need **one** installation route: Setup.exe **or** the manual ZIP. Neither includes CloudCompare, Python, trained models or a LiDAR dataset.

## 3. Verify the download and close CloudCompare

Download [CHECKSUMS_SHA256.txt](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.6/CHECKSUMS_SHA256.txt). In PowerShell, calculate the installer checksum, replacing the example path with its actual location:

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\path\to\ALiS-0.1.0-alpha.5.6-CloudCompare-2.13.2-Windows-x64-Setup.exe'
```

Compare the entire hash against the matching filename in the checksum file; uppercase/lowercase is irrelevant. If it differs, do not run the file.

The alpha installer is **not digitally signed**. Windows may display a SmartScreen warning or an unknown-publisher prompt. Check the source and checksum before deciding whether to continue; if unsure, stop and consult your IT administrator. **Do not disable antivirus or Windows security protections.** Matching a hash confirms consistency with the published file, not independent publisher identity.

Close all CloudCompare windows before installing or updating. Save any work you want to keep first.

## 4. Run Setup.exe and select the CloudCompare root folder

1. Double-click the downloaded ALiS installer.
2. Review the licence and installation notes.
3. Select the directory containing **CloudCompare.exe**, usually `C:\Program Files\CloudCompare`.
4. **Do not select the `plugins` subfolder.** The installer puts the DLL there itself.
5. Choose whether to create a desktop shortcut, then finish installation. Windows may request administrator approval to write into Program Files.

The resulting layout includes:

```text
CloudCompare/
├── CloudCompare.exe          existing host — not replaced
├── plugins/
│   └── ALIS_PLUGIN.dll
├── worker/
│   └── ALiS/                 optional classifier worker and setup scripts
├── doc/
│   └── ALiS/                 local getting-started instructions
├── ALiS.ico
└── ALiS Launcher.cmd
```

ALiS installs beside other plugins; it does not replace the CloudCompare executable or unrelated plugins. If the installer rejects the selected host version, select a matching installation rather than forcing files into an incompatible host.

<details>
<summary><strong>Alternative: manual ZIP installation</strong></summary>

Close CloudCompare. Extract the Windows-x64 ZIP into a temporary folder. Copy its `plugins`, `worker` and `doc` folders into the CloudCompare root, merging those folders. Copy `ALiS.ico`, `ALiS Launcher.cmd` and the accompanying notices too. Preserve unrelated files. Administrator rights may be needed for Program Files.

Do not copy the entire ZIP or its containing directory into `plugins`. Verify that the final DLL path is `CloudCompare\plugins\ALIS_PLUGIN.dll` and that `worker\ALiS` is beside `plugins`, not inside it.

</details>

## 5. Open ALiS inside CloudCompare

Start CloudCompare normally, or use the ALiS shortcut. Open ALiS through the **Plugins** menu or its toolbar icon. The shortcut starts the same host, not a separate standalone application.

If ALiS is missing, check the troubleshooting table below before reinstalling.

## 6. Load and select a first point cloud

Use **File → Open** to load a small **copy** of a LAS/LAZ file, then click the point-cloud entity in CloudCompare's DB tree. Selecting a parent group instead of the point cloud may leave operations unavailable.

Inspect the **Session** information, fields and coordinate units. Retain a consistent global shift when CloudCompare needs it for large georeferenced coordinates. Confirm metres only when the source coordinates really are metric; an absent CRS cannot be reconstructed automatically from an assumption.

Try one operation at a time. Inspect results, save to a **new** file and reopen a sample to check labels and fields. Never use your only copy of a survey as an installation test.

## Optional: enable classifiers and deep learning

**Native terrain, geometric features and annotation do not require Python.** ML/clustering need a separate Python runtime; MLP and PointNet also require PyTorch. No pretrained weights ship with this release.

Install **Python 3.12 x64** from a trusted source, such as the [official Python website](https://www.python.org/downloads/windows/), and identify the actual `python.exe` path. For an installation registered with the Python launcher, `py -3.12 -c "import sys; print(sys.executable)"` can show it.

Run **one** of the following in PowerShell, adjusting both paths for your PC:

```powershell
# CPU environment: ML and CPU deep learning
& 'C:\Program Files\CloudCompare\worker\ALiS\setup_cpu_runtime.ps1' -BasePython 'C:\path\to\python.exe'

# OR: NVIDIA CUDA environment, for compatible hardware and drivers
& 'C:\Program Files\CloudCompare\worker\ALiS\setup_cuda_runtime.ps1' -BasePython 'C:\path\to\python.exe'
```

The scripts download dependencies into an isolated environment under your user profile. CUDA downloads can require several GB. They do not replace your global Python installation or relax PowerShell execution policy. If policy blocks a script, review it with your IT administrator rather than disabling protections.

After setup, restart CloudCompare and any terminal used to launch it. CPU setup sets `ALIS_PYTHON`; this override takes priority over CUDA autodiscovery. If switching later to CUDA, update or clear that override as explained in the [runtime guide](RUNTIME.md).

GPU support is model-specific. Native geometry, Random Forest, Extra Trees and Histogram Gradient Boosting remain primarily CPU-based. Successful plugin installation alone does not prove GPU availability.

## Troubleshooting

| Symptom | Check first |
| --- | --- |
| No ALiS entry or icon | Exact host version, Windows x64, Qt5; DLL in the host's `plugins` folder; CloudCompare console/plugin-loading messages. |
| Qt/ABI warning | This binary is for 2.13.2/Qt5, not Qt6/2.14 or arbitrary builds. Use the matching official host. |
| Permission denied during installation | Close CloudCompare; confirm the intended folder; ask your administrator if Program Files is restricted. |
| Controls are disabled | Load a cloud and select its point-cloud entity in the DB tree. |
| Python worker or classifier unavailable | Install the optional runtime; inspect `ALIS_PYTHON`; restart the launching application. See [runtime checks](RUNTIME.md). |
| GPU unavailable | Check compatible NVIDIA hardware/driver, CUDA runtime and whether the selected method supports it. CPU/native tools can still be used. |
| Large memory use or slow processing | Start with a small test copy and modest feature radii; neighbourhood size matters. |

If a problem remains, [submit a bug report](https://github.com/Nicodemox87/ALiS/issues/new/choose). Include software versions, steps, parameters and sanitised logs. Do not post restricted survey data or personal paths.

## Updates and removal

For a later version, back up your data/catalogue, read its compatibility notes, close CloudCompare and use that version's installer. Do not mix DLL and worker files from different releases.

For an installer-based installation, uninstall **ALiS**, not CloudCompare, through Windows Installed Apps. User datasets, catalogues and separately installed Python environments are retained. A manually copied ZIP has no installer-managed uninstall: remove only the ALiS-specific files after backing up anything you need.
