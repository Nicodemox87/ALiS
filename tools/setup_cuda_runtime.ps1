# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$BasePython)
$ErrorActionPreference = 'Stop'
$runtimeRoot = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.alis\runtime-cu128'
$runtimePython = Join-Path $runtimeRoot 'Scripts\python.exe'
if (!(Test-Path -LiteralPath $runtimePython)) {
    & $BasePython -m venv $runtimeRoot
    if ($LASTEXITCODE -ne 0) { throw 'Could not create isolated ALiS runtime.' }
}
# Preserve sklearn ABI used by existing trusted models; no global Python changes.
& $runtimePython -m pip install 'numpy==2.0.1' 'scipy==1.15.3' 'scikit-learn==1.6.0' 'joblib==1.4.2' 'threadpoolctl==3.6.0' 'xgboost==2.1.3' 'laspy==2.6.1'
if ($LASTEXITCODE -ne 0) { throw 'Could not install ALiS scientific dependencies.' }
& $runtimePython -m pip install 'torch==2.10.0+cu128' --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) { throw 'Could not install official PyTorch CUDA wheel.' }
& $runtimePython (Join-Path $PSScriptRoot 'verify_cuda_runtime.py')
if ($LASTEXITCODE -ne 0) { throw 'GPU/dependency verification failed; default runtime NOT changed.' }
Write-Output "Verified ALiS runtime: $runtimePython"
Write-Output 'ALiS automatically discovers this isolated runtime. ALIS_PYTHON remains an explicit override.'
