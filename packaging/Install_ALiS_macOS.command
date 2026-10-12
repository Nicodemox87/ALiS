#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build/install from source. Never overwrite an existing CloudCompare application.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || { echo 'This installer needs an Apple Silicon Mac.'; exit 1; }
command -v brew >/dev/null || { echo 'Install Homebrew from https://brew.sh, then run this installer again.'; exit 1; }
xcode-select -p >/dev/null || { echo 'Install Apple Xcode Command Line Tools, then retry.'; exit 1; }
echo 'ALiS Apple Silicon developer preview: compile CloudCompare 2.13.2 + ALiS locally.'
echo 'This downloads build dependencies. Keep at least 15 GB free. No existing host is replaced.'
read -r -p 'Continue? [y/N] ' answer
[[ "$answer" == y || "$answer" == Y ]] || exit 0
brew install cmake ninja qt@5 eigen laszip python@3.12 libomp
bash "$repo/tools/build_macos.sh"
bash "$repo/tools/setup_macos_runtime.sh" "$(brew --prefix python@3.12)/bin/python3.12"
app="${ALIS_MAC_BUILD_DIR:-$repo/output/macos}/install/CloudCompare/CloudCompare.app"
echo "Build finished. Application: $app"
echo 'Keep this development folder while testing. Do not move the app before checking its dependencies.'
open -R "$app"
