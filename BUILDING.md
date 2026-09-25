# Build the native plugin

The released binary targets **CloudCompare 2.13.2, Windows x64, Qt 5.15.2 MSVC2019_64**, compiled with Visual Studio 2022 Build Tools. It is not ABI-compatible with arbitrary hosts. The public `plugin/` snapshot corresponds to the alpha.5.6 production source; newer unpublished study batch adapters are not part of this release.

## Prerequisites

- Git, CMake, Visual Studio 2022 C++ desktop tools and Windows SDK.
- Qt 5.15.2 MSVC2019_64 including Widgets, OpenGL and SVG support.
- CloudCompare at commit `49dbbb662f296c7780aae717897c85b3cb3764ed`, with recursive submodules.
- Dependencies required by the selected host configuration; LAS/LAZ I/O in the reference host uses LASzip 3.4.4. Follow [upstream build instructions](https://github.com/CloudCompare/CloudCompare/blob/v2.13.2/BUILD.md) and inspect that version's CMake options.

```powershell
git clone --recursive --branch v2.13.2 https://github.com/CloudCompare/CloudCompare.git CloudCompare-src
git -C CloudCompare-src checkout 49dbbb662f296c7780aae717897c85b3cb3764ed
git -C CloudCompare-src submodule update --init --recursive
Copy-Item -Recurse .\plugin .\CloudCompare-src\plugins\private\qArchaeoLiDAR
cmake -S CloudCompare-src -B build -G 'Visual Studio 17 2022' -A x64 `
  -DCMAKE_PREFIX_PATH='C:/Qt/5.15.2/msvc2019_64' `
  -DCMAKE_INSTALL_PREFIX='C:/ALiS-dev' -DPLUGIN_STANDARD_ALIS=ON `
  -DALIS_BUILD_TESTS=ON -DBUILD_TESTING=ON -DOPTION_BUILD_CCVIEWER=OFF
cmake --build build --config Release --target ALIS_PLUGIN --parallel 8
```

These are configuration examples; supply your actual Qt and optional LASzip paths. Use a separate development prefix, not the system host installation. Building the full host/install target requires its enabled dependencies. The plugin directly compiles the **unmodified qCSF algorithm files from the pinned host source**; `third_party/qCSF` retains their source and notices for distribution provenance, but CMake uses the host copy.

## Tests and runtime

Build the ALiS test targets listed in `plugin/CMakeLists.txt`; the normal `all` build includes them. Run `ctest --test-dir build -C Release --output-on-failure` with matching Qt/CloudCompare runtime DLLs on PATH and `QT_QPA_PLATFORM=offscreen`. Optional real-survey tests require a separately prepared fixture; no survey fixture is shipped. Python tests use:

```powershell
$env:PYTHONPATH = (Resolve-Path plugin/worker).Path
python -m unittest discover -s plugin/worker/tests -v
```

Install optional PyTorch/XGBoost for their tests; capability skips are not successful GPU validations. See [runtime instructions](docs/RUNTIME.md). `tools/verify_plugin_compatibility.ps1 -Plugin <DLL> -CloudCompareDirectory <host>` checks PE architecture, imports and native loading without installing or launching the host.

## Packaging

`tools/package_public.py` accepts an explicit compiled DLL and an existing alpha.5.6 asset staging directory containing `ALiS.ico` and `ALiS-wizard.bmp`. It assembles a public allowlist payload and source archive, then invokes Inno Setup 6. `packaging/ALiS.iss` defines the installer. The existing icon and wizard raster are included in `packaging/assets` so no image-generation service is needed to reproduce packaging. The compiler and Python executables are external prerequisites. See `--help`.

The release does not bundle host/Qt/Python binaries. Full fresh-machine build and interactive installation testing remain community-validation work; local native-load, regression, archive and checksum checks are not a substitute for that.
