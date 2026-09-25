# Public package checks — 25 September 2026

Version: **0.1.0-alpha.5.6**, Windows x64, CloudCompare 2.13.2 stable / Qt 5.

- Native DLL imports and dependency loading checked against the installed official CloudCompare host: passed.
- Worker regression suite from the public source snapshot: **26 tests passed** in 111.896 seconds in the existing Python 3.12 CUDA environment. Includes local PointNet, classifier persistence/inference, spatial comparison, vegetation adapter and clustering-field tests.
- Independent package audit: **204 checks passed** for archive CRC, SHA-256 manifests, worker/source consistency, corresponding native source, third-party source/notices and absence of survey/model/paper files or personal workstation identifiers in checked text files.
- Installer compiled with Inno Setup 6.7.3; signature status: **unsigned**.

These checks do not constitute an interactive clean-PC acceptance test, universal classification validation, antivirus certification or independent-site scientific validation. Python environments are downloaded separately. Recheck SHA-256 after download using CHECKSUMS_SHA256.txt.

The native algorithms and DLL are unchanged from the existing alpha.5.6 build; packaging documentation and setup helpers were prepared for public testing. See [release notes](../RELEASE_NOTES.md).
