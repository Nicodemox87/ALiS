> Historical development notes. For the complete offline Mac installer and current requirements, see [MAC_INSTALLER.md](MAC_INSTALLER.md). Validation is recorded in [MAC_INSTALLER_TESTS.md](MAC_INSTALLER_TESTS.md).

# Apple Silicon development installer

This is a **native arm64 source-build installer**, not a Windows package and not a notarised prebuilt application. It keeps the existing CloudCompare installation intact and builds the reference 2.13.2 host and ALiS together. Intel Macs are not the target of this preview.

## Requirements

- Apple Silicon Mac, macOS 15 or later, at least 15 GB free storage and preferably 16 GB RAM or more.
- Apple's Xcode Command Line Tools, Git and [Homebrew](https://brew.sh).
- Network access for the pinned CloudCompare source and build/runtime dependencies.

Review the scripts before executing them. Homebrew currently supplies Qt 5.15.19; Qt 5 is deprecated and the build records the actual installed dependency versions. This version is used to maintain compatibility with the reference host; a future host/Qt migration requires separate validation. [Qt 5 formula and lifecycle](https://formulae.brew.sh/formula/qt@5).

## Install without replacing your current CloudCompare

1. Download and unpack the source-installer ZIP, or clone the macOS development branch of the repository.
2. From Terminal, in the repository folder, run `bash packaging/Install_ALiS_macOS.command`.
3. Confirm the build/dependency downloads when prompted. Compilation and native regression tests run before runtime setup.
4. The app is created at `output/macos/install/CloudCompare/CloudCompare.app`; Finder opens at this location. Keep the build folder while testing this developer preview.
5. Open the newly built host, select ALiS, and test a small copy of a cloud. The Python runtime is `~/.alis/runtime-macos/bin/python3`. No Windows virtual environment should be copied to the Mac.

Do not disable Gatekeeper, remove quarantine, or modify system security settings. This local developer build is not an Apple Developer ID signed/notarised distribution. If institutional policy prevents it, use an approved development environment or arrange code signing with the maintainer/IT administrator.

## GPU and models

Native features, ground filtering and most classifiers initially use CPU. The setup does not enable CUDA or promise Apple GPU support. PyTorch is optional: run `ALIS_WITH_TORCH=1 bash tools/setup_macos_runtime.sh /path/to/native/python3.12` for the CPU neural comparison path. Review any saved model's requested device; a CUDA-trained configuration cannot simply be assumed runnable on Apple hardware. Metal/MPS remains a separate implementation and validation task.

## Before wider distribution

Validate actual GUI interaction on the user's M4 Max: cloud import, CRS/shift, features, ground/HAG, annotation, save/reopen, classifier input matching, and disk/CC block preparation. Automated tests on a Mac runner do not replace this acceptance test. Test copied research fixtures only; never launch the full survey by default.

The public Windows installer remains on its existing release. No Windows binary or source release is silently replaced by this preview.
