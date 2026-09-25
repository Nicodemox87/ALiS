# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$BasePython, [switch]$WithoutDeepLearning)
$ErrorActionPreference = 'Stop'
$runtimeRoot = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.alis\runtime-cpu'
$runtimePython = Join-Path $runtimeRoot 'Scripts\python.exe'
& $BasePython -c "import sys,struct; assert sys.version_info[:2] == (3,12) and struct.calcsize('P') == 8, 'Use Python 3.12 x64'"
if ($LASTEXITCODE -ne 0) { throw 'Python 3.12 x64 is required for this tested environment.' }
if (!(Test-Path -LiteralPath $runtimePython)) {
    & $BasePython -m venv $runtimeRoot
    if ($LASTEXITCODE -ne 0) { throw 'Could not create isolated ALiS runtime.' }
}
& $runtimePython -m pip install 'numpy==2.0.1' 'scipy==1.15.3' 'scikit-learn==1.6.0' 'joblib==1.4.2' 'threadpoolctl==3.6.0' 'xgboost==2.1.3' 'laspy==2.6.1'
if ($LASTEXITCODE -ne 0) { throw 'Scientific dependency installation failed.' }
if (!$WithoutDeepLearning) {
    & $runtimePython -m pip install 'torch==2.10.0+cpu' --index-url https://download.pytorch.org/whl/cpu
    if ($LASTEXITCODE -ne 0) { throw 'PyTorch CPU installation failed.' }
}
$worker = Join-Path $PSScriptRoot 'qal_ml_worker.py'
if (!(Test-Path -LiteralPath $worker)) { $worker = Join-Path $PSScriptRoot '..\plugin\worker\qal_ml_worker.py' }
& $runtimePython $worker capabilities
if ($LASTEXITCODE -ne 0) { throw 'Capability check failed; Python selection was not changed.' }
[Environment]::SetEnvironmentVariable('ALIS_PYTHON', $runtimePython, 'User')
$env:ALIS_PYTHON = $runtimePython
Write-Output "ALiS CPU runtime ready: $runtimePython. Restart CloudCompare and its launching terminal."
