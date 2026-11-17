"""Deterministic classification and bootstrap-label worker for ALiS."""

# Apply process-local caps BEFORE importing numpy/scikit-learn (including -m use).
# Do not modify Windows settings or override a smaller explicit thread budget.
import os as _os
for _key in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS", "NUMEXPR_MAX_THREADS"):
    try:
        _threads = int(_os.environ.get(_key, "24"))
    except ValueError:
        _threads = 24
    _os.environ[_key] = str(min(24, _threads) if _threads > 0 else 24)

from .dataset import DATASET_SCHEMA, Dataset, load_dataset
from .model import MODEL_SCHEMA, PREDICTION_SCHEMA

__all__ = [
    "DATASET_SCHEMA",
    "MODEL_SCHEMA",
    "PREDICTION_SCHEMA",
    "Dataset",
    "load_dataset",
]

__version__ = "0.5.0"
