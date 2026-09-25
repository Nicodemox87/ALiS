from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import tempfile
import time
from dataclasses import asdict, dataclass, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import joblib
import numpy as np
import sklearn
from sklearn.metrics import (
    accuracy_score,
    balanced_accuracy_score,
    cohen_kappa_score,
    confusion_matrix,
    precision_recall_fscore_support,
)
from sklearn.impute import SimpleImputer
from sklearn.utils.class_weight import compute_sample_weight

from .dataset import Dataset, SchemaError, finite_row_mask
from .events import EventWriter, NullEventWriter
from .classifiers import build_classifier
from .reporting import write_technical_report
from .distributions import class_distributions
from .repository import register_model
from .split import spatial_block_split


MODEL_SCHEMA = "qal-ml-model/2.0"
LEGACY_MODEL_SCHEMA = "qal-ml-random-forest/1.0"
PREDICTION_SCHEMA = "qal-ml-prediction/1.0"
REPORT_SCHEMA = "qal-ml-training-report/1.0"


ASPRS_NAMES = {
    0: "Created, never classified",
    1: "Unclassified",
    2: "Ground",
    3: "Low vegetation",
    4: "Medium vegetation",
    5: "High vegetation",
    6: "Building",
    7: "Low point (noise)",
    8: "Reserved",
    9: "Water",
    10: "Rail",
    11: "Road surface",
    12: "Reserved",
    13: "Wire guard (shield)",
    14: "Wire conductor (phase)",
    15: "Transmission tower",
    16: "Wire connector (insulator)",
    17: "Bridge deck",
    18: "High noise",
    19: "Overhead structure",
    20: "Ignored ground",
    21: "Snow",
    22: "Temporal exclusion",
}


def asprs_name(value: int) -> str:
    if value in ASPRS_NAMES:
        return ASPRS_NAMES[value]
    if 23 <= value <= 63:
        return "Reserved"
    return "User definable"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass(frozen=True)
class TrainConfig:
    classifier: str = "random_forest"
    device: str = "auto"
    block_size: float = 20.0
    test_fraction: float = 0.2
    seed: int = 42
    trees: int = 300
    max_depth: int | None = None
    min_samples_leaf: int = 1
    max_features: str | float | int | None = "sqrt"
    class_weight: str | None = "balanced_subsample"
    criterion: str = "gini"
    subsample: float = 0.9
    column_sample: float = 0.9
    n_jobs: int = -1
    epochs: int = 30
    batch_size: int = 4096
    learning_rate: float = 0.001
    repository: str | None = None
    max_train_per_class: int = 0
    spatial_buffer: float = 0.0
    impute_missing: bool = False
    pointnet_radius: float = .5
    pointnet_neighbors: int = 64

    def validate(self) -> None:
        if not np.isfinite(self.pointnet_radius) or self.pointnet_radius <= 0 or not 8 <= self.pointnet_neighbors <= 512:
            raise ValueError("PointNet radius must be positive and neighbours in [8, 512]")
        if self.classifier == 'pointnet' and self.batch_size > 1024:
            raise ValueError("PointNet batches contain 3D patches: choose batch_size <= 1024 (128 recommended)")
        if self.trees < 1:
            raise ValueError("trees must be >= 1")
        if self.max_depth is not None and self.max_depth < 1:
            raise ValueError("max_depth must be >= 1 or omitted")
        if self.min_samples_leaf < 1:
            raise ValueError("min_samples_leaf must be >= 1")
        if self.criterion not in {"gini", "entropy", "log_loss"}:
            raise ValueError("criterion must be gini, entropy or log_loss")
        if not 0 < self.subsample <= 1 or not 0 < self.column_sample <= 1:
            raise ValueError("subsample and column_sample must be in (0, 1]")
        if self.epochs < 1 or self.batch_size < 1 or self.learning_rate <= 0:
            raise ValueError("epochs, batch_size and learning_rate must be positive")
        if self.max_train_per_class < 0 or not np.isfinite(self.spatial_buffer) or self.spatial_buffer < 0:
            raise ValueError('Training cap and spatial buffer must be nonnegative')


def _metrics(y_true: np.ndarray, y_pred: np.ndarray, classes: np.ndarray) -> dict[str, Any]:
    precision, recall, f1, support = precision_recall_fscore_support(
        y_true,
        y_pred,
        labels=classes,
        zero_division=0,
    )
    per_class = []
    for index, class_value in enumerate(classes):
        denominator = precision[index] + recall[index] - precision[index] * recall[index]
        iou = (precision[index] * recall[index] / denominator) if denominator > 0 else 0.0
        per_class.append(
            {
                "class": int(class_value),
                "name": asprs_name(int(class_value)),
                "precision": float(precision[index]),
                "recall": float(recall[index]),
                "f1": float(f1[index]),
                "iou": float(iou),
                "support": int(support[index]),
            }
        )
    kappa = float(cohen_kappa_score(y_true, y_pred))
    return {
        "accuracy": float(accuracy_score(y_true, y_pred)),
        "balanced_accuracy": float(balanced_accuracy_score(y_true, y_pred)),
        "cohen_kappa": kappa if np.isfinite(kappa) else None,
        "labels": [int(value) for value in classes],
        "confusion_matrix": confusion_matrix(y_true, y_pred, labels=classes).astype(int).tolist(),
        "per_class": per_class,
    }


def _artifact_fingerprint(dataset: Dataset, config: TrainConfig, test_dataset: Dataset | None = None) -> str:
    configuration = asdict(config)
    configuration.pop("repository", None)
    if config.classifier != 'pointnet':
        # Preserve identities/resume of all existing trained models.
        configuration.pop('pointnet_radius', None)
        configuration.pop('pointnet_neighbors', None)
    else:
        from .pointnet import coordinate_dataset
        dataset = coordinate_dataset(dataset)
        configuration['impute_missing'] = False
        configuration['spatial_buffer'] = max(config.spatial_buffer, 2 * config.pointnet_radius + .001)
    canonical = json.dumps(
        {
            "dataset_sha256": dataset.source_sha256,
            "test_dataset_sha256": test_dataset.source_sha256 if test_dataset is not None else None,
            "features": list(dataset.feature_names),
            "config": configuration,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def train(
    dataset: Dataset,
    output_path: str | Path,
    config: TrainConfig,
    events: EventWriter | None = None,
    report_path: str | Path | None = None,
    test_dataset: Dataset | None = None,
) -> dict[str, Any]:
    events = events or NullEventWriter("train")
    config.validate()
    if config.classifier == 'pointnet':
        from .pointnet import coordinate_dataset
        dataset = coordinate_dataset(dataset)
        if test_dataset is not None:
            test_dataset = coordinate_dataset(test_dataset)
        # XYZ cannot be meaningfully imputed. Holdout guard prevents patch overlap.
        config = replace(config, impute_missing=False,
                         spatial_buffer=max(config.spatial_buffer, 2 * config.pointnet_radius + .001))
    if dataset.labels is None:
        raise SchemaError("training requires labels")
    if not dataset.labels_trusted:
        raise SchemaError(
            "training labels are not marked trusted; verify them in CloudCompare and export labels_trusted=true"
        )
    if dataset.target_domain != "asprs_working":
        raise SchemaError(
            "this worker version trains only target_domain='asprs_working'; "
            "'archaeology_class' is reserved for a later classifier"
        )
    if test_dataset is not None:
        if test_dataset.labels is None or not test_dataset.labels_trusted:
            raise SchemaError("external test dataset requires trusted labels")
        if test_dataset.target_domain != dataset.target_domain:
            raise SchemaError("external test dataset target_domain mismatch")
        if tuple(test_dataset.feature_names) != tuple(dataset.feature_names):
            raise SchemaError("external test dataset feature names/order do not match training dataset")

    events.emit("progress", "validate", 0.08, "Validating trusted rows")
    finite_features = finite_row_mask(dataset.features)
    finite_xy = finite_row_mask(dataset.xy)
    finite = finite_xy if config.impute_missing else finite_features & finite_xy
    trusted = (
        np.asarray(dataset.trusted_mask, dtype=bool)
        if dataset.trusted_mask is not None
        else np.ones(dataset.point_count, dtype=bool)
    )
    valid = finite & trusted
    trusted_rows = int(np.count_nonzero(trusted))
    untrusted_rows = int(dataset.point_count - trusted_rows)
    invalid_rows = int(np.count_nonzero(trusted & ~finite))
    x = np.asarray(dataset.features[valid], dtype=np.float32)
    xy = np.asarray(dataset.xy[valid], dtype=np.float64)
    y = np.asarray(dataset.labels[valid], dtype=np.uint8)
    classes = np.unique(y)
    if len(y) < 4 or len(classes) < 2:
        raise SchemaError("training needs at least four finite rows and two ASPRS classes")

    external_validation = test_dataset is not None
    if external_validation:
        events.emit("progress", "split", 0.16, "Preparing independent external test cloud")
        test_finite_features = finite_row_mask(test_dataset.features)
        test_finite_xy = finite_row_mask(test_dataset.xy)
        test_finite = test_finite_xy if config.impute_missing else test_finite_features & test_finite_xy
        test_trusted = (np.asarray(test_dataset.trusted_mask, dtype=bool)
            if test_dataset.trusted_mask is not None else np.ones(test_dataset.point_count, dtype=bool))
        test_valid = test_finite & test_trusted
        x_test = np.asarray(test_dataset.features[test_valid], dtype=np.float32)
        test_y = np.asarray(test_dataset.labels[test_valid], dtype=np.uint8)
        if len(test_y) < 1:
            raise SchemaError("external test dataset has no finite trusted rows")
        train_indices = np.arange(len(y), dtype=np.int64)
        test_invalid_rows = int(np.count_nonzero(test_trusted & ~test_finite))
        test_nontrusted_rows = int(test_dataset.point_count - np.count_nonzero(test_trusted))
        split = None
    else:
        events.emit(
            "progress", "split", 0.16, "Building spatial holdout blocks",
            {"valid_trusted_rows": int(len(y)), "nontrusted_rows": untrusted_rows,
             "discarded_nonfinite_trusted_rows": invalid_rows},
        )
        split = spatial_block_split(xy, y, block_size=config.block_size,
            test_fraction=config.test_fraction, seed=config.seed, buffer_distance=config.spatial_buffer)
        train_indices = split.train_indices
        x_test = np.asarray(x[split.test_indices], dtype=np.float32)
        test_y = y[split.test_indices]
        test_finite_features = finite_features[valid][split.test_indices]
        test_invalid_rows = invalid_rows
        test_nontrusted_rows = untrusted_rows
    before_sampling = len(train_indices)
    if config.max_train_per_class:
        rng = np.random.default_rng(config.seed)
        selected = []
        for code in classes:
            candidates = train_indices[y[train_indices] == code]
            selected.append(rng.choice(candidates, size=min(len(candidates), config.max_train_per_class), replace=False))
        train_indices = np.sort(np.concatenate(selected))
    x_train = x[train_indices]
    feature_distributions = class_distributions(x_train, y[train_indices], dataset.feature_names, seed=config.seed)
    preprocessor = None
    if config.impute_missing:
        # Fit only on actual training rows. Missingness often identifies isolated
        # noise; do not silently remove this rare class from evaluation.
        preprocessor = SimpleImputer(strategy='median', add_indicator=True, keep_empty_features=True)
        x_train = preprocessor.fit_transform(np.where(np.isfinite(x_train), x_train, np.nan))
        x_test = preprocessor.transform(np.where(np.isfinite(x_test), x_test, np.nan))

    classifier_build = build_classifier(config)
    events.emit(
        "progress",
        "fit",
        0.25,
        f"Training {config.classifier} on {classifier_build.resolved_device}",
        {"train_rows": int(len(train_indices)), "test_rows": int(len(test_y)),
         "classifier": config.classifier, "device": classifier_build.resolved_device},
    )
    estimator = classifier_build.estimator
    if config.classifier == 'pointnet':
        events.emit('progress', 'context', .24, 'Indexing unlabelled XYZ context for PointNet')
        estimator.bind_context(dataset.features, events)
    train_y = y[train_indices]
    if classifier_build.prediction_encoding == "index":
        train_y = np.searchsorted(classes, train_y).astype(np.int32)
    training_started = time.perf_counter()
    fit_options = {}
    if config.classifier == 'xgboost' and config.class_weight is not None:
        fit_options['sample_weight'] = compute_sample_weight('balanced', train_y)
    estimator.fit(x_train, train_y, **fit_options)
    training_seconds = time.perf_counter() - training_started

    events.emit("progress", "evaluate", 0.80,
        "Evaluating independent test cloud" if external_validation else "Evaluating held-out spatial blocks")
    evaluation_started = time.perf_counter()
    if config.classifier == 'pointnet' and external_validation:
        estimator.bind_context(test_dataset.features, events)
    predicted = estimator.predict(x_test)
    evaluation_seconds = time.perf_counter() - evaluation_started
    if classifier_build.prediction_encoding == "index":
        predicted = classes[np.asarray(predicted, dtype=np.int64)]
    metric_classes = np.union1d(classes, np.unique(test_y))
    metrics = _metrics(test_y, predicted, metric_classes)
    def class_counts(values: np.ndarray) -> list[dict[str, int]]:
        unique, counts = np.unique(values, return_counts=True)
        return [{"class": int(value), "count": int(count)} for value, count in zip(unique, counts)]

    test_classes = np.unique(test_y)
    missing_test_classes = [int(value) for value in np.setdiff1d(classes, test_classes)]
    unseen_test_classes = [int(value) for value in np.setdiff1d(test_classes, classes)]
    validation_warnings: list[str] = []
    if missing_test_classes:
        validation_warnings.append(
            ("External test dataset" if external_validation else "Spatial holdout")
            + " has no trusted samples for ASPRS classes "
            + ", ".join(str(value) for value in missing_test_classes)
            + "; balanced accuracy and Cohen's kappa are not complete multiclass validation."
        )
    if unseen_test_classes:
        validation_warnings.append(
            "External test contains ASPRS classes absent from training: "
            + ", ".join(str(value) for value in unseen_test_classes)
            + "; these classes cannot be predicted by this model."
        )
    metrics["validation_complete"] = not missing_test_classes and not unseen_test_classes
    metrics["warnings"] = validation_warnings
    split_report = {
        "strategy": "external-certified-dataset" if external_validation else "spatial-block-group-holdout",
        "block_size": None if external_validation else float(config.block_size),
        "requested_test_fraction": None if external_validation else float(config.test_fraction),
        "actual_test_fraction": None if external_validation else float(len(split.test_indices) / len(y)),
        "train_rows": int(len(train_indices)),
        "train_rows_before_sampling": int(before_sampling),
        "spatial_buffer_m": None if external_validation else float(config.spatial_buffer),
        "buffer_excluded_rows": 0 if external_validation else int(len(y)-len(split.test_indices)-before_sampling),
        "sampling": "seeded per-class cap, training only" if config.max_train_per_class else "none",
        "test_rows": int(len(test_y)),
        "train_block_count": None if external_validation else int(len(split.train_blocks)),
        "test_block_count": None if external_validation else int(len(split.test_blocks)),
        "overlapping_block_count": None if external_validation else int(np.intersect1d(split.train_blocks, split.test_blocks).size),
        "discarded_nonfinite_rows": invalid_rows,
        "nontrusted_rows": untrusted_rows,
        "trusted_rows_before_finite_filter": trusted_rows,
        "trusted_rows_used": int(len(y)),
        "all_class_counts": class_counts(y),
        "train_class_counts": class_counts(y[train_indices]),
        "test_class_counts": class_counts(test_y),
        "missing_test_classes": missing_test_classes,
        "unseen_test_classes": unseen_test_classes,
        "test_dataset_sha256": test_dataset.source_sha256 if external_validation else dataset.source_sha256,
        "test_discarded_nonfinite_rows": test_invalid_rows,
        "test_nontrusted_rows": test_nontrusted_rows,
    }
    class_values = [int(value) for value in classes]
    configuration = asdict(config)
    configuration["resolved_device"] = classifier_build.resolved_device
    feature_importances = getattr(estimator, "feature_importances_", None)
    importance_values = ([float(value) for value in feature_importances]
                         if feature_importances is not None else [])
    if preprocessor is not None and importance_values:
        aggregated = np.array(importance_values[:dataset.feature_count])
        for index, column in enumerate(preprocessor.indicator_.features_):
            aggregated[column] += importance_values[dataset.feature_count+index]
        importance_values = aggregated.tolist()
    valid_ids = (np.asarray(dataset.point_ids)[valid] if dataset.point_ids is not None else np.flatnonzero(valid))
    train_ids = np.asarray(valid_ids[train_indices], dtype='<u8')
    if external_validation:
        test_ids = (np.asarray(test_dataset.point_ids)[test_valid] if test_dataset.point_ids is not None
            else np.flatnonzero(test_valid)).astype('<u8', copy=False)
    else:
        test_ids = np.asarray(valid_ids[split.test_indices], dtype='<u8')
    split_report['train_point_ids_sha256'] = hashlib.sha256(train_ids.tobytes()).hexdigest()
    split_report['test_point_ids_sha256'] = hashlib.sha256(test_ids.tobytes()).hexdigest()
    preprocessing_info = {'method': 'train-only median + missingness indicators' if preprocessor is not None else 'drop nonfinite rows',
        'indicator_input_columns': preprocessor.indicator_.features_.tolist() if preprocessor is not None else [],
        'importance_includes_missingness': preprocessor is not None,
        'source_rows_with_missing_features': int(np.count_nonzero(~finite_features)),
        'test_rows_with_missing_features': int(np.count_nonzero(~test_finite_features[test_valid])) if external_validation
            else int(np.count_nonzero(~test_finite_features))}
    artifact: dict[str, Any] = {
        "schema": MODEL_SCHEMA,
        "artifact_id": _artifact_fingerprint(dataset, config, test_dataset),
        "classifier_id": config.classifier,
        "model_family": classifier_build.family,
        "prediction_encoding": classifier_build.prediction_encoding,
        "target_domain": dataset.target_domain,
        "estimator": estimator,
        "preprocessor": preprocessor,
        "feature_schema": {
            "count": dataset.feature_count,
            "names": list(dataset.feature_names),
            "input_dtype": "float32",
            "order": "row-major",
        },
        "class_mapping": {
            "probability_column_to_asprs": class_values,
            "names": {str(value): asprs_name(value) for value in class_values},
        },
        "training": {
            "configuration": configuration,
            "split": split_report,
            "metrics": metrics,
            "feature_importances": importance_values,
            "learning_history": getattr(estimator, "training_history_", []),
            "training_seconds": training_seconds,
            "evaluation_seconds": evaluation_seconds,
            "preprocessing": preprocessing_info,
        },
        "provenance": {
            "dataset_path": str(dataset.source_path),
            "dataset_sha256": dataset.source_sha256,
            "dataset_schema": dataset.source_schema,
            "labels_trusted": True,
            "trusted_mask_present": dataset.trusted_mask is not None,
            "trusted_rows_used": int(len(y)),
            "label_source": dataset.label_source,
            "target_domain": dataset.target_domain,
            "dataset_metadata": dataset.metadata,
            "test_dataset_path": str(test_dataset.source_path) if external_validation else None,
            "test_dataset_sha256": test_dataset.source_sha256 if external_validation else None,
            "test_dataset_metadata": test_dataset.metadata if external_validation else None,
        },
        "software": {
            "python": platform.python_version(),
            "numpy": np.__version__,
            "scikit_learn": sklearn.__version__,
            "joblib": joblib.__version__,
            "worker": "0.3.0",
            "neural_runtime": getattr(estimator, "runtime_metadata_", None),
        },
        "created_utc": datetime.now(timezone.utc).isoformat(),
    }
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Per-model ID files make exact comparison membership auditable after restart.
    np.save(str(output)+'.train_point_ids.npy', train_ids, allow_pickle=False)
    np.save(str(output)+'.test_point_ids.npy', test_ids, allow_pickle=False)
    temp_handle, temp_name = tempfile.mkstemp(prefix=output.name + ".", suffix=".tmp", dir=output.parent)
    os.close(temp_handle)
    try:
        joblib.dump(artifact, temp_name, compress=("zlib", 3), protocol=4)
        os.replace(temp_name, output)
    finally:
        Path(temp_name).unlink(missing_ok=True)
    model_sha256 = _sha256_file(output)
    report = {
        "schema": REPORT_SCHEMA,
        "model_schema": MODEL_SCHEMA,
        "model_path": str(output.resolve()),
        "model_sha256": model_sha256,
        "artifact_id": artifact["artifact_id"],
        "classifier_id": artifact.get("classifier_id", "random_forest"),
        "model_family": artifact["model_family"],
        "target_domain": artifact["target_domain"],
        "feature_schema": artifact["feature_schema"],
        "class_mapping": artifact["class_mapping"],
        "training": artifact["training"],
        "provenance": artifact["provenance"],
        "software": artifact["software"],
        "warnings": validation_warnings,
        "feature_distributions": feature_distributions,
    }
    if report_path is not None:
        report_output = Path(report_path)
    elif output.name == "model.joblib":
        report_output = output.parent / "report.json"
    else:
        report_output = Path(str(output) + ".report.json")
    report_files = write_technical_report(report, report_output)
    model_manifest = {
        "schema": "qal-model-entry/1.0",
        "artifact_id": artifact["artifact_id"],
        "created_utc": artifact["created_utc"],
        "classifier_id": config.classifier,
        "model_family": artifact["model_family"],
        "device": classifier_build.resolved_device,
        "model": str(output.resolve()),
        "model_sha256": model_sha256,
        "reports": report_files,
        "dataset_sha256": dataset.source_sha256,
        "test_dataset_sha256": test_dataset.source_sha256 if external_validation else None,
        "validation_strategy": split_report["strategy"],
        "target_domain": dataset.target_domain,
        "feature_names": list(dataset.feature_names),
        "classes_asprs": class_values,
        "balanced_accuracy": metrics["balanced_accuracy"],
        # Compact acquisition-domain evidence used by the intelligent catalog.
        # The complete immutable provenance remains in report.json/model.joblib.
        "training_profile": dataset.metadata.get("source_cloud", {}).get("cloud_profile", {})
            if isinstance(dataset.metadata.get("source_cloud"), dict) else {},
        "training_metadata": dataset.metadata,
    }
    manifest_output = output.parent / "manifest.json" if output.name == "model.joblib" else Path(str(output) + ".manifest.json")
    manifest_output.write_text(
        json.dumps(model_manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if config.repository:
        register_model(config.repository, model_manifest)
    events.emit("metric", "evaluate", 0.92,
        "Independent test metrics" if external_validation else "Spatial holdout metrics", metrics)
    events.emit(
        "completed",
        "save",
        1.0,
        "Model artifact saved",
        {
            "model": str(output.resolve()),
            "model_sha256": model_sha256,
            "report": str(report_output.resolve()),
            "report_html": report_files["html"],
            "artifact_id": artifact["artifact_id"],
        },
    )
    return artifact


def validate_artifact(artifact: Any) -> dict[str, Any]:
    if not isinstance(artifact, dict) or artifact.get("schema") not in {MODEL_SCHEMA, LEGACY_MODEL_SCHEMA}:
        raise SchemaError("unsupported or malformed model artifact")
    estimator = artifact.get("estimator")
    if estimator is None or not callable(getattr(estimator, "predict", None)) or not callable(getattr(estimator, "predict_proba", None)):
        raise SchemaError("artifact estimator does not implement predict/predict_proba")
    schema = artifact.get("feature_schema")
    if not isinstance(schema, dict) or not isinstance(schema.get("names"), list):
        raise SchemaError("artifact feature schema is malformed")
    if schema.get("count") != len(schema["names"]) or len(set(schema["names"])) != len(schema["names"]):
        raise SchemaError("artifact feature schema count/names are inconsistent")
    mapping = artifact.get("class_mapping", {}).get("probability_column_to_asprs")
    if not isinstance(mapping, list) or len(mapping) < 2 or any(not isinstance(value, int) for value in mapping):
        raise SchemaError("artifact class mapping is missing or malformed")
    encoding = artifact.get("prediction_encoding", "direct")
    if encoding not in {"direct", "index"}:
        raise SchemaError("artifact prediction encoding is invalid")
    if encoding == "direct" and mapping != [int(value) for value in estimator.classes_]:
        raise SchemaError("artifact class mapping does not match estimator classes")
    if artifact.get("target_domain") not in {"asprs_working", "archaeology_class"}:
        raise SchemaError("artifact target_domain is missing or unsupported")
    return artifact


def load_artifact(path_like: str | Path) -> dict[str, Any]:
    path = Path(path_like)
    if not path.is_file():
        raise SchemaError(f"model not found: {path}")
    # joblib/pickle is executable input: callers must only load artifacts they trust.
    return validate_artifact(joblib.load(path))


def model_summary(artifact: dict[str, Any], path: str | Path) -> dict[str, Any]:
    model_path = Path(path)
    return {
        "schema": artifact["schema"],
        "path": str(model_path.resolve()),
        "sha256": _sha256_file(model_path),
        "artifact_id": artifact["artifact_id"],
        "classifier_id": artifact.get("classifier_id", "random_forest"),
        "model_family": artifact["model_family"],
        "target_domain": artifact["target_domain"],
        "feature_schema": artifact["feature_schema"],
        "class_mapping": artifact["class_mapping"],
        "training": artifact["training"],
        "provenance": artifact["provenance"],
        "software": artifact["software"],
    }


def _validate_prediction_schema(dataset: Dataset, artifact: dict[str, Any]) -> None:
    if dataset.target_domain != artifact["target_domain"]:
        raise SchemaError(
            f"target_domain mismatch: dataset={dataset.target_domain!r}, "
            f"model={artifact['target_domain']!r}"
        )
    expected = tuple(artifact["feature_schema"]["names"])
    if dataset.feature_names != expected:
        missing = [name for name in expected if name not in dataset.feature_names]
        extra = [name for name in dataset.feature_names if name not in expected]
        raise SchemaError(
            "feature schema/order mismatch; "
            f"expected={list(expected)!r}, received={list(dataset.feature_names)!r}, "
            f"missing={missing!r}, extra={extra!r}"
        )


def _prediction_metadata(
    dataset: Dataset,
    model_path: Path,
    artifact: dict[str, Any],
    valid_count: int,
    write_probabilities: bool,
) -> dict[str, Any]:
    class_values = [int(value) for value in artifact["class_mapping"]["probability_column_to_asprs"]]
    files: dict[str, Any] = {
        "predictions": {"path": "predictions.i16", "dtype": "<i2", "shape": [dataset.point_count], "nodata": -1},
        "confidence": {"path": "confidence.f32", "dtype": "<f4", "shape": [dataset.point_count], "nodata": "NaN"},
        "uncertainty": {"path": "uncertainty.f32", "dtype": "<f4", "shape": [dataset.point_count], "nodata": "NaN"},
    }
    if write_probabilities:
        files["probabilities"] = {
            "path": "probabilities.f32",
            "dtype": "<f4",
            "shape": [dataset.point_count, len(class_values)],
            "columns_asprs": class_values,
            "nodata": "NaN",
        }
        files["classes"] = {"path": "classes.u8", "dtype": "u1", "shape": [len(class_values)]}
    return {
        "schema": PREDICTION_SCHEMA,
        "point_count": dataset.point_count,
        "valid_prediction_count": int(valid_count),
        "invalid_nonfinite_row_count": int(dataset.point_count - valid_count),
        "classes_asprs": class_values,
        "target_domain": artifact["target_domain"],
        "class_names": {str(value): asprs_name(value) for value in class_values},
        "uncertainty_definition": "1 - maximum predicted class probability",
        "neural_prediction_device": getattr(artifact["estimator"], "last_prediction_device_", None),
        "alignment": "output row i corresponds exactly to input row i; invalid rows are preserved as NODATA",
        "files": files,
        "provenance": {
            "dataset_path": str(dataset.source_path),
            "dataset_sha256": dataset.source_sha256,
            "model_path": str(model_path.resolve()),
            "model_sha256": _sha256_file(model_path),
            "model_artifact_id": artifact["artifact_id"],
        },
    }


def _fill_predictions(
    dataset: Dataset,
    artifact: dict[str, Any],
    predictions: np.ndarray,
    confidence: np.ndarray,
    uncertainty: np.ndarray,
    probabilities: np.ndarray | None,
    *,
    chunk_size: int,
    events: EventWriter,
) -> int:
    if chunk_size < 1:
        raise ValueError("chunk_size must be >= 1")
    estimator = artifact["estimator"]
    preprocessor = artifact.get('preprocessor')
    class_values = np.asarray(
        artifact["class_mapping"]["probability_column_to_asprs"], dtype=np.int16
    )
    processed = 0
    for start in range(0, dataset.point_count, chunk_size):
        stop = min(dataset.point_count, start + chunk_size)
        local = (np.all(np.isfinite(dataset.xy[start:stop]), axis=1) if preprocessor is not None
                 else np.all(np.isfinite(dataset.features[start:stop]), axis=1))
        if np.any(local):
            x = np.asarray(dataset.features[start:stop][local], dtype=np.float32)
            if preprocessor is not None:
                x = preprocessor.transform(np.where(np.isfinite(x), x, np.nan))
            proba = estimator.predict_proba(x).astype(np.float32, copy=False)
            indices = np.flatnonzero(local) + start
            best = np.argmax(proba, axis=1)
            conf = proba[np.arange(len(proba)), best]
            predictions[indices] = class_values[best]
            confidence[indices] = conf
            uncertainty[indices] = 1.0 - conf
            if probabilities is not None:
                probabilities[indices, :] = proba
            processed += len(indices)
        events.emit(
            "progress",
            "predict",
            0.08 + 0.82 * (stop / dataset.point_count),
            "Predicting point chunks",
            {"rows_scanned": stop, "valid_rows_predicted": processed},
        )
    return processed


def _predict_bundle(
    dataset: Dataset,
    artifact: dict[str, Any],
    model_path: Path,
    output: Path,
    chunk_size: int,
    write_probabilities: bool,
    events: EventWriter,
) -> dict[str, Any]:
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise FileExistsError(f"prediction output already exists and is not empty: {output}")
        output.rmdir()
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix=output.name + ".", dir=output.parent))
    mappings: list[np.memmap] = []
    try:
        n = dataset.point_count
        class_values = artifact["class_mapping"]["probability_column_to_asprs"]
        class_count = len(class_values)
        predictions = np.memmap(temp / "predictions.i16", mode="w+", dtype="<i2", shape=(n,))
        mappings.append(predictions)
        confidence = np.memmap(temp / "confidence.f32", mode="w+", dtype="<f4", shape=(n,))
        mappings.append(confidence)
        uncertainty = np.memmap(temp / "uncertainty.f32", mode="w+", dtype="<f4", shape=(n,))
        mappings.append(uncertainty)
        predictions[:] = -1
        confidence[:] = np.nan
        uncertainty[:] = np.nan
        probabilities = None
        if write_probabilities:
            probabilities = np.memmap(
                temp / "probabilities.f32", mode="w+", dtype="<f4", shape=(n, class_count)
            )
            mappings.append(probabilities)
            probabilities[:] = np.nan
            np.asarray(class_values, dtype=np.uint8).tofile(temp / "classes.u8")
        valid_count = _fill_predictions(
            dataset,
            artifact,
            predictions,
            confidence,
            uncertainty,
            probabilities,
            chunk_size=chunk_size,
            events=events,
        )
        # Flush is not close: Windows forbids renaming a directory with open
        # mapped files. Release handles explicitly, regardless of estimator/GC.
        for array in mappings:
            array.flush()
            array._mmap.close()
        mappings.clear()
        del predictions, confidence, uncertainty, probabilities
        result = _prediction_metadata(dataset, model_path, artifact, valid_count, write_probabilities)
        (temp / "result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temp, output)
        return result
    except Exception:
        for array in mappings:
            if not array._mmap.closed:
                array._mmap.close()
        shutil.rmtree(temp, ignore_errors=True)
        raise


def _predict_npz(
    dataset: Dataset,
    artifact: dict[str, Any],
    model_path: Path,
    output: Path,
    chunk_size: int,
    write_probabilities: bool,
    events: EventWriter,
) -> dict[str, Any]:
    n = dataset.point_count
    class_values = artifact["class_mapping"]["probability_column_to_asprs"]
    c = len(class_values)
    predictions = np.full(n, -1, dtype=np.int16)
    confidence = np.full(n, np.nan, dtype=np.float32)
    uncertainty = np.full(n, np.nan, dtype=np.float32)
    probabilities = np.full((n, c), np.nan, dtype=np.float32) if write_probabilities else None
    valid_count = _fill_predictions(
        dataset,
        artifact,
        predictions,
        confidence,
        uncertainty,
        probabilities,
        chunk_size=chunk_size,
        events=events,
    )
    result = _prediction_metadata(dataset, model_path, artifact, valid_count, write_probabilities)
    arrays: dict[str, Any] = {
        "predicted_class": predictions,
        "confidence": confidence,
        "uncertainty": uncertainty,
        "classes": np.asarray(class_values, dtype=np.uint8),
        "result_json": np.asarray(json.dumps(result, sort_keys=True)),
    }
    if write_probabilities:
        arrays["probabilities"] = probabilities
    if dataset.point_ids is not None:
        arrays["point_ids"] = np.asarray(dataset.point_ids)
    output.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(prefix=output.name + ".", suffix=".npz", dir=output.parent)
    os.close(handle)
    try:
        np.savez_compressed(temp_name, **arrays)
        os.replace(temp_name, output)
    finally:
        Path(temp_name).unlink(missing_ok=True)
    return result


def predict(
    dataset: Dataset,
    model_path_like: str | Path,
    output_path_like: str | Path,
    *,
    chunk_size: int = 250_000,
    write_probabilities: bool = False,
    events: EventWriter | None = None,
) -> dict[str, Any]:
    events = events or NullEventWriter("predict")
    model_path = Path(model_path_like)
    artifact = load_artifact(model_path)
    if artifact.get('classifier_id') == 'pointnet':
        from .pointnet import coordinate_dataset
        dataset = coordinate_dataset(dataset)
        events.emit('progress', 'context', .04, 'Indexing target XYZ neighbourhoods for PointNet')
        artifact['estimator'].bind_context(dataset.features, events)
    _validate_prediction_schema(dataset, artifact)
    events.emit(
        "progress",
        "validate",
        0.06,
        "Dataset and model schemas match",
        {"point_count": dataset.point_count, "write_probabilities": write_probabilities},
    )
    output = Path(output_path_like)
    if output.suffix.lower() == ".npz":
        result = _predict_npz(
            dataset, artifact, model_path, output, chunk_size, write_probabilities, events
        )
    else:
        result = _predict_bundle(
            dataset, artifact, model_path, output, chunk_size, write_probabilities, events
        )
    events.emit(
        "completed",
        "save",
        1.0,
        "Prediction bundle saved",
        {
            "output": str(output.resolve()),
            "valid_prediction_count": result["valid_prediction_count"],
            "invalid_nonfinite_row_count": result["invalid_nonfinite_row_count"],
        },
    )
    return result
