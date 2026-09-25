from __future__ import annotations

import importlib.util
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
from sklearn.cluster import Birch, MiniBatchKMeans
from sklearn.impute import SimpleImputer
from sklearn.mixture import GaussianMixture
from sklearn.preprocessing import StandardScaler

from .dataset import Dataset
from .events import EventWriter
from .cluster_device import ClusterAssignment


BOOTSTRAP_SCHEMA = "alis-bootstrap-result/1.0"
ALGORITHMS = ("minibatch_kmeans", "birch", "gaussian_mixture")


def capability_report() -> dict[str, Any]:

    return {
        "schema": "alis-bootstrap-capabilities/1.0",
        "clustering": [
            {"id": "minibatch_kmeans", "available": True, "speed": "fast"},
            {"id": "birch", "available": True, "speed": "balanced"},
            {"id": "gaussian_mixture", "available": True, "speed": "thorough"},
        ],
        "pretrained": [],
        "pretrained_status": "Deferred in this release",
    }


def _fit_sample(features: np.ndarray, maximum: int, seed: int) -> np.ndarray:
    n = features.shape[0]
    if n <= maximum:
        return np.asarray(features)
    rng = np.random.default_rng(seed)
    return np.asarray(features[rng.choice(n, size=maximum, replace=False)])


def cluster(
    dataset: Dataset,
    output: Path,
    *,
    algorithm: str,
    clusters: int,
    seed: int,
    fit_sample: int,
    chunk_size: int,
    events: EventWriter,
    device: str = "auto",
) -> dict[str, Any]:
    if algorithm not in ALGORITHMS:
        raise ValueError(f"unsupported bootstrap algorithm: {algorithm}")
    if fit_sample < 2 or chunk_size < 1:
        raise ValueError("fit_sample must be >= 2 and chunk_size must be positive")
    if clusters < 2:
        raise ValueError("clusters must be >= 2")
    if device not in {"auto", "cpu", "cuda"}:
        raise ValueError("device must be auto, cpu or cuda")
    output.mkdir(parents=True, exist_ok=True)
    events.emit("progress", "sample", 0.05, "Sampling and imputing feature space")
    sample = _fit_sample(dataset.features, fit_sample, seed)
    sample = np.where(np.isfinite(sample), sample, np.nan)
    active = np.any(np.isfinite(sample), axis=0)
    if not np.any(active):
        raise ValueError("Selected fields contain no finite values in the fitting sample; choose other fields or increase the sample.")
    valid_sample = np.any(np.isfinite(sample[:, active]), axis=1)
    sample = sample[valid_sample][:, active]
    if len(sample) < clusters or len(np.unique(np.nan_to_num(sample), axis=0)) < clusters:
        raise ValueError("Not enough distinct valid sample points for the requested cluster count.")
    imputer = SimpleImputer(strategy="median", add_indicator=True)
    scaler = StandardScaler()
    sample_scaled = scaler.fit_transform(imputer.fit_transform(sample))

    if algorithm == "minibatch_kmeans":
        estimator = MiniBatchKMeans(
            n_clusters=clusters,
            random_state=seed,
            batch_size=min(16384, max(1024, len(sample_scaled))),
            n_init="auto",
        )
    elif algorithm == "birch":
        estimator = Birch(n_clusters=clusters, threshold=0.5)
    else:
        estimator = GaussianMixture(
            n_components=clusters,
            covariance_type="diag",
            random_state=seed,
            max_iter=200,
        )
    events.emit("progress", "fit", 0.20, f"Fitting {algorithm} on CPU")
    estimator.fit(sample_scaled)
    assignment = ClusterAssignment(estimator, algorithm, device, dataset.point_count)
    device_report = {"requested": device, "fit_device": "cpu", "assignment_device": "cpu",
                     "reason": "This algorithm has no validated CUDA backend"}

    ids = np.memmap(output / "clusters.i32", mode="w+", dtype="<i4", shape=(dataset.point_count,))
    confidence = np.memmap(output / "confidence.f32", mode="w+", dtype="<f4", shape=(dataset.point_count,))
    histogram = np.zeros(clusters, dtype=np.int64)
    invalid_count = 0
    for start in range(0, dataset.point_count, chunk_size):
        stop = min(dataset.point_count, start + chunk_size)
        raw = np.asarray(dataset.features[start:stop])[:, active]
        raw = np.where(np.isfinite(raw), raw, np.nan)
        valid_rows = np.any(np.isfinite(raw), axis=1)
        transformed = scaler.transform(imputer.transform(raw))
        if algorithm == "minibatch_kmeans":
            if not assignment.ready:
                events.emit("progress", "device", 0.30, "Checking CPU/CUDA assignment cost and numerical equivalence…")
            previous_reason = assignment.report["reason"]
            labels, scores = assignment.predict(transformed)
            device_report = assignment.report
            if start == 0 or previous_reason != device_report["reason"]:
                events.emit("progress", "device", 0.30 + 0.65 * start / dataset.point_count,
                            f"Assignment: {device_report['assignment_device'].upper()} — {device_report['reason']}", device_report)
        elif algorithm == "gaussian_mixture":
            probabilities = estimator.predict_proba(transformed)
            labels = np.argmax(probabilities, axis=1).astype(np.int32)
            scores = probabilities[np.arange(len(labels)), labels].astype(np.float32)
        else:
            labels = estimator.predict(transformed).astype(np.int32)
            if hasattr(estimator, "transform"):
                distances = np.asarray(estimator.transform(transformed))
                if algorithm == "birch":
                    # BIRCH distances refer to subclusters, not final cluster IDs.
                    distances = np.column_stack([np.min(distances[:, estimator.subcluster_labels_ == label], axis=1)
                        if np.any(estimator.subcluster_labels_ == label) else np.full(len(labels), np.inf)
                        for label in range(clusters)])
                nearest = np.sort(distances, axis=1)[:, :2]
                scores = np.clip(1.0 - nearest[:, 0] / np.maximum(nearest[:, 1], 1e-12), 0.0, 1.0).astype(np.float32)
            else:
                scores = np.ones(len(labels), dtype=np.float32)
        labels[~valid_rows] = -1
        scores[~valid_rows] = 0
        invalid_count += int(np.count_nonzero(~valid_rows))
        ids[start:stop] = labels
        confidence[start:stop] = scores
        histogram += np.bincount(labels[labels >= 0], minlength=clusters)
        progress = 0.30 + 0.65 * stop / dataset.point_count
        events.emit("progress", "predict", progress, f"Assigning clusters [{device_report['assignment_device'].upper()}]: {stop}/{dataset.point_count}")
    ids.flush()
    confidence.flush()
    # Release Windows mappings before callers read, replace or clean outputs.
    ids._mmap.close()
    confidence._mmap.close()

    result = {
        "schema": BOOTSTRAP_SCHEMA,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "mode": "explore_clusters",
        "algorithm": algorithm,
        "execution": device_report,
        "cluster_count": clusters,
        "cluster_histogram": [int(value) for value in histogram],
        "feature_names": list(dataset.feature_names),
        "dataset_sha256": dataset.source_sha256,
        "point_count": dataset.point_count,
        "ignored_empty_fields": [name for name, used in zip(dataset.feature_names, active) if not used],
        "unassigned_points": invalid_count,
        "confidence_definition": "GMM posterior" if algorithm == "gaussian_mixture" else "relative separation of nearest two final clusters; not calibrated probability",
        "missing_value_policy": "Median imputation fitted on sample; all-missing points remain cluster -1",
        "seed": seed,
        "fit_sample": min(fit_sample, dataset.point_count),
        "semantics": "Unsupervised cluster IDs; not ASPRS classes and not trusted training labels.",
    }
    (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    events.emit("completed", "complete", 1.0, f"Clusters ready — fit: CPU; assignment: {device_report['assignment_device'].upper()}. {device_report['reason']}", result)
    return result
