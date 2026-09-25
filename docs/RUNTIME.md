# Optional Python runtime

The installer contains the Python **worker source**, not a Python interpreter or downloaded packages. Native operations remain available without it. Python 3.12 x64 is the tested runtime; other versions are not certified by this release.

Use `tools/setup_cpu_runtime.ps1 -BasePython 'C:\path\to\python.exe'` for an isolated CPU environment, or `tools/setup_cuda_runtime.ps1` with the same argument for the tested CUDA 12.8 environment. Both scripts are also installed under `worker/ALiS`. Review scripts before running them. They use the official package indexes and do not change the system Python installation.

CPU setup sets **ALIS_PYTHON** for the current process and your Windows user after capability checks. Restart CloudCompare and its parent terminal. CUDA discovery requires `.alis/runtime-cu128/alis-runtime-ready.json`; the CUDA verification script creates this only after successful checks. A user-set ALIS_PYTHON overrides discovery. To switch to the verified CUDA environment, change that variable to its `Scripts/python.exe`, or remove your user override through Windows Environment Variables and restart the launching application.

Manual alternative: create a venv, install `plugin/worker/requirements.txt`, then set ALIS_PYTHON to the venv's Python executable. Install XGBoost and a suitable PyTorch wheel separately if needed. Use the [official PyTorch installation guidance](https://pytorch.org/get-started/locally/) for a different platform. Model serialisation compatibility depends on package versions; keep an environment record with your models.

Verify with `python plugin/worker/qal_ml_worker.py capabilities` using the actual runtime executable. CPU/GPU selection is model-specific, not a switch that accelerates every operation. A CUDA installation failure does not prevent using native ALiS tools or a separate CPU environment.

Pinned setup dependencies: NumPy 2.0.1, SciPy 1.15.3, scikit-learn 1.6.0, joblib 1.4.2, threadpoolctl 3.6.0, XGBoost 2.1.3, laspy 2.6.1; optional PyTorch 2.10.0 CPU or 2.10.0+cu128. Downloads require network/disk capacity. No trained models are included.
