# ALiS classifier worker

This isolated Python process trains supervised ASPRS classifiers over the exact
multiscale feature schema exported by CloudCompare. It supports Random Forest,
Extra Trees, Histogram Gradient Boosting, optional XGBoost CPU/CUDA and an
experimental PyTorch Multiscale MLP. It never installs packages or rewrites source data.

## Environment

Python 3.10+ is supported. Create an environment explicitly and install the pinned
ranges; the worker never runs `pip` itself.

```powershell
python -m venv .venv
.\.venv\Scripts\python -m pip install -r plugin\worker\requirements.txt
.\.venv\Scripts\python plugin\worker\qal_ml_worker.py --version
```

All normal stdout records are JSONL with schema `qal-ml-event/1.0`. An `error`
record and exit code `2` mean failure.

## Commands

```powershell
python plugin\worker\qal_ml_worker.py inspect --dataset C:\temp\training.qalml
python plugin\worker\qal_ml_worker.py capabilities
python plugin\worker\qal_ml_worker.py bootstrap-capabilities
python plugin\worker\qal_ml_worker.py cluster --dataset C:\temp\working.qalml --output C:\temp\clusters --algorithm minibatch_kmeans --clusters 8
python plugin\worker\qal_ml_worker.py repository --path C:\qal_repository
python plugin\worker\qal_ml_worker.py train --dataset C:\temp\training.qalml --repository C:\qal_repository --classifier xgboost --device cuda --block-size 20 --trees 300 --seed 42
python plugin\worker\qal_ml_worker.py inspect --model C:\temp\rf.joblib
python plugin\worker\qal_ml_worker.py predict --dataset C:\temp\working.qalml --model C:\temp\rf.joblib --output C:\temp\prediction.qalml
```

Training rejects untrusted labels, uses whole XY blocks for holdout (no block can
occur in both train and test), drops and counts non-finite training rows, and
reports accuracy, balanced accuracy, Cohen's kappa, confusion matrix and
precision/recall/F1/support for every ASPRS code present. A fixed seed controls
the split and forest. Classes are not collapsed to 2/3/6.

Worker 0.3.0 also accepts `--max-train-per-class N` (zero means no cap),
`--spatial-buffer METRES` and `--impute-missing`. Imputation keeps rows with finite
XY even when feature values are missing; medians and missingness indicators are
fitted only on the actual training rows and serialized for inference. The default
without this flag retains the original non-finite-row exclusion behavior. Training
and test point IDs and their hashes are persisted beside the model. Buffer exclusion
and per-class capping affect training only, not the holdout.

For the reproducible RF/Extra Trees/XGBoost CUDA batch comparison, use
`python -m qal_ml.comparison --dataset <bundle> --repository <new-repository>` with
`PYTHONPATH=plugin/worker`. It saves a shared-membership comparison and can write a
new predicted LAS copy; it does not modify the reference LAS or mark predictions
trusted. See [comparison protocol](../../docs/CLASSIFIER_COMPARISON.md), including
known HAG and rare-class limitations. This command is separate from the current UI.

Training writes JSON, HTML, CSV and SVG reports with configuration, spatial split,
per-class precision/recall/F1/IoU, confusion matrix, feature importance and cloud
provenance without requiring the UI to unpickle joblib.
If a class is absent from the holdout, the report marks validation incomplete and
warns that balanced accuracy/kappa are not a complete multiclass validation.

## C++-friendly dataset bundle

A bundle is a directory with these exact files and little-endian layouts:

- `manifest.json`
- `features.f32`: row-major `<f4`, shape `N x F`
- `xy.f64`: row-major `<f8`, shape `N x 2`, metric source coordinates
- `z.f64`: optional `<f8`, shape `N`, metric source elevation
- `labels.u8`: `u1`, shape `N` (required only for training)
- `trusted.u8`: optional `u1`, shape `N`; `1` selects a manually trusted training row
- `point_ids.u64`: optional `<u8`, shape `N`, stable row IDs (the exporter owns uniqueness)

Minimal manifest:

```json
{
  "schema": "qal-ml-dataset/1.0",
  "rows": 1000,
  "columns": 3,
  "features": [
    {"column": 0, "key": "Planarity@0.5m"},
    {"column": 1, "key": "Roughness@0.5m"},
    {"column": 2, "key": "HAG"}
  ],
  "labels_trusted": true,
  "label_source": "manual_verified",
  "target_domain": "asprs_working",
  "metadata": {"source_cloud_uid": 1234}
}
```

Canonical aliases `point_count`, `feature_count`, `feature_names` are accepted too.

The loader also accepts a compact `.npz` with arrays `features`, `feature_names`,
`xy`, optional `labels`, `labels_trusted`, `label_source`, `target_domain`,
`trusted_mask`, `point_ids`, and scalar JSON string `metadata_json`. In a full-N
bundle, `labels_trusted:true` attests that `trusted.u8` is authoritative; training
uses only rows where that mask is 1. Without a mask it means every label is trusted.
Feature names and order are
part of the model contract. `target_domain` is also part of it: prediction rejects
a mismatch. This version trains only `asprs_working`; `archaeology_class` is
reserved for a later independent model.

## Model and prediction output

The joblib object has schema `qal-ml-model/2.0` and contains the estimator,
exact feature schema, `target_domain`, ASPRS probability-column mapping, seed and
hyperparameters, spatial split audit, metrics, dataset SHA-256/provenance, feature
importance and software versions. **Joblib is pickle-based: load only trusted
model files.**

A directory prediction has:

- `result.json`, schema `qal-ml-prediction/1.0`
- `predictions.i16`: `<i2`, ASPRS code; `-1` is NODATA (class 0 remains valid)
- `confidence.f32`: `<f4`, maximum class probability
- `uncertainty.f32`: `<f4`, `1 - confidence`

Rows stay exactly aligned with the input. A row containing any non-finite feature
is not imputed: it receives `-1`, `NaN`, `NaN`. Full `probabilities.f32` (`N x C`)
and `classes.u8` are written only with `--write-probabilities`, because they are
usually too large for a 200M-point cloud. `.npz` prediction output is supported for
small tests but the directory bundle is the production format.

## Tests

```powershell
python -m unittest discover -s plugin\worker\tests -v
```
