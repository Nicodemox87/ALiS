# Update checks

From alpha.5.9, ALiS schedules an asynchronous update check after CloudCompare's main window is visible on Windows and macOS. The option is enabled by default and can be disabled under **Settings → Updates**. Automatic checks are limited to once daily. **Check GitHub now** bypasses the daily limit. An offline/failed check is not evidence that the installed version is current.

The official public GitHub Releases API for `Nicodemox87/ALiS` is queried, including testing previews. The request exposes normal connection metadata (IP address, user agent) to GitHub but sends no project paths, clouds, models, labels or credentials. No telemetry is collected by ALiS.

- **Windows:** requires a newer numeric release with its matching complete x64 installer asset. A prompt offers the official release page, not silent execution. Download the complete installer, save work, close CloudCompare, run Setup and approve the Windows security prompt yourself. Settings also retains the downloaded-installer selection workflow. Windows' TLS trust store is used through the built-in PowerShell client; no extra OpenSSL or Python is needed for checking.
- **macOS:** requires the arm64 manifest and PKG with matching version/size/SHA256, compatible with the running macOS. Download is explicitly confirmed, integrity-checked, and quarantined for Gatekeeper. Installation remains manual after quitting the app. An ad-hoc signature is not Developer ID signing or notarization.

Older Windows versions do not gain the new startup checker until users manually install alpha.5.9 or later once. Existing Mac automatic checks can discover a new compatible Mac release after its verified assets are published. No loaded plugin DLL is replaced by the checker.

Progressive feature processing, classifiers and scientific models do not update in the background. Saved projects and user models are not deleted by update checks.
