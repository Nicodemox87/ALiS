#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || { echo 'Native Apple Silicon macOS is required.'; exit 1; }
repo="$(cd "$(dirname "$0")/.." && pwd)"
work="${ALIS_MAC_BUILD_DIR:-$repo/output/macos}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"
host="$work/CloudCompare-src"
if [[ ! -d "$host/.git" ]]; then
  git clone --depth 1 --branch v2.13.2 https://github.com/CloudCompare/CloudCompare.git "$host"
fi
[[ "$(git -C "$host" rev-parse HEAD)" == 49dbbb662f296c7780aae717897c85b3cb3764ed ]] || { echo 'Unexpected host revision'; exit 1; }
# Only CCCoreLib is needed. Unrelated optional plugins can have unavailable
# upstream repositories and must not prevent a clean ALiS build.
git -C "$host" submodule update --init --recursive --depth 1 libs/qCC_db/extern/CCCoreLib
# The pinned host has three unqualified vector types rejected by Apple Clang.
for patch in "$repo"/tools/patches/cloudcompare-2.13.2-*.patch; do
if git -C "$host" apply --check "$patch" 2>/dev/null; then
  git -C "$host" apply "$patch"
else
  git -C "$host" apply --reverse --check "$patch" || { echo 'Host patch conflict'; exit 1; }
fi
done
mkdir -p "$host/plugins/private/qArchaeoLiDAR"
cp -R "$repo/plugin/." "$host/plugins/private/qArchaeoLiDAR/"
qt="$(brew --prefix qt@5)"
cmake -S "$host" -B "$work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_CXX_FLAGS="-I$(brew --prefix laszip)/include/laszip" \
  -DCMAKE_PREFIX_PATH="$qt;$(brew --prefix laszip);$(brew --prefix eigen)" \
  -DCMAKE_INSTALL_PREFIX="$work/install" \
  -DEIGEN_ROOT_DIR="$(brew --prefix eigen)/include/eigen3" \
  -DPLUGIN_STANDARD_ALIS=ON -DALIS_BUILD_TESTS=ON -DBUILD_TESTING=ON \
  -DOPTION_MAC_CUSTOM_DEPLOYMENT=ON -DOPTION_BUILD_CCVIEWER=OFF -DPLUGIN_IO_QLAS=ON -DPLUGIN_STANDARD_QCSF=ON \
  -DCCCORELIB_USE_TBB=OFF -DCCCORELIB_USE_CGAL=OFF
cmake --build "$work/build" --parallel "${ALIS_BUILD_JOBS:-3}"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$work/build" -C Release -R '^ALiS\.' --output-on-failure
cmake --install "$work/build"
python3 "$repo/tools/finalize_macos_bundle.py" "$work/install/CloudCompare/CloudCompare.app"
echo "Installed development host: $work/install/CloudCompare/CloudCompare.app"
