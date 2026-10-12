#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || { echo 'Native Apple Silicon macOS required'; exit 1; }
python="${1:-python3.12}"
"$python" -c 'import platform,sys; assert platform.machine()=="arm64" and sys.version_info[:2]==(3,12), "Use native arm64 Python 3.12"'
runtime="$HOME/.alis/runtime-macos"
[[ -x "$runtime/bin/python3" ]] || "$python" -m venv "$runtime"
"$runtime/bin/python3" -m pip install 'numpy==2.0.1' 'scipy==1.16.3' 'scikit-learn==1.6.0' 'joblib==1.4.2' 'threadpoolctl==3.6.0' 'laspy[lazrs]==2.6.1' 'xgboost==2.1.3'
if [[ "${ALIS_WITH_TORCH:-0}" == 1 ]]; then
  "$runtime/bin/python3" -m pip install 'torch==2.10.0'
fi
"$runtime/bin/python3" -m pip check
"$runtime/bin/python3" -c 'import scipy.sparse.linalg, sklearn, xgboost, laspy; print("Native runtime imports passed")'
"$runtime/bin/python3" -m pip freeze > "$runtime/requirements-installed.txt"
echo "ALiS runtime ready at $runtime/bin/python3. Restart CloudCompare."
echo 'CUDA is not available on macOS; Apple GPU inference is not enabled by this setup.'
