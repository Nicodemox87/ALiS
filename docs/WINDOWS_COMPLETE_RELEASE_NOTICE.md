<!-- alis-windows-complete-r1:start -->
## Windows complete setup — 12 October 2026

**[Download Windows complete installer (.exe)](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.7/ALiS-0.1.0-alpha.5.7-Windows-x64-Complete-Setup-r1.exe)**

One online wizard detects compatible CloudCompare 2.13.2 x64/Qt5 or installs it, installs ALiS and prepares Python with ML and deep learning. Choose CPU (recommended) or NVIDIA CUDA 12.8 with a compatible GPU/driver. Internet is required; allow at least 5 GiB free for CPU or 12 GiB for CUDA. Download size depends on the selected backend. NVIDIA drivers are not installed.

Save work and close CloudCompare. Run the wizard normally (not as administrator), confirm the Windows prompts for native components and wait for the installation checks. Then open CloudCompare and ALiS from Plugins. Setup is unsigned: verify its checksum and keep security protections enabled.

[Instructions](https://github.com/Nicodemox87/ALiS/blob/main/docs/WINDOWS_COMPLETE_INSTALLER.md) · [SHA-256 checksums](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.7/ALiS-Windows-Complete-r1-SHA256SUMS.txt) · [Installer sources](https://github.com/Nicodemox87/ALiS/releases/download/v0.1.0-alpha.5.7/ALiS-0.1.0-alpha.5.7-Windows-x64-Complete-Source-r1.zip) · [Installation validation](https://github.com/Nicodemox87/ALiS/blob/main/docs/WINDOWS_COMPLETE_VALIDATION.md)

This adds an installer, not new classification algorithms: the native alpha.5.7 plugin is unchanged. Historical plugin-only downloads and Mac files remain available below. CPU installation was tested on a fresh Windows CI host; interactive UAC and a fresh CUDA installation require machine-specific acceptance checks. The original release tag is unchanged; use the separately named complete-installer source archive for this addition.
<!-- alis-windows-complete-r1:end -->
