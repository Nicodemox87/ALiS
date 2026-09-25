from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np


DATASET_SCHEMA = "qal-ml-dataset/1.0"


class SchemaError(ValueError):
    """The input does not satisfy the ALiS ML schema."""


@dataclass(frozen=True)
class Dataset:
    features: np.ndarray
    feature_names: tuple[str, ...]
    xy: np.ndarray
    z: np.ndarray | None
    labels: np.ndarray | None
    labels_trusted: bool
    trusted_mask: np.ndarray | None
    label_source: str | None
    target_domain: str
    point_ids: np.ndarray | None
    metadata: dict[str, Any]
    source_path: Path
    source_sha256: str
    source_schema: str

    @property
    def point_count(self) -> int:
        return int(self.features.shape[0])

    @property
    def feature_count(self) -> int:
        return int(self.features.shape[1])


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_bundle(path: Path) -> str:
    digest = hashlib.sha256()
    for child in sorted((p for p in path.iterdir() if p.is_file()), key=lambda p: p.name):
        encoded = child.name.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "little"))
        digest.update(encoded)
        with child.open("rb") as stream:
            for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest()


def _scalar(value: np.ndarray, name: str) -> Any:
    array = np.asarray(value)
    if array.size != 1:
        raise SchemaError(f"'{name}' must be a scalar")
    return array.reshape(()).item()


def _decode_metadata(value: Any) -> dict[str, Any]:
    if value is None:
        return {}
    if isinstance(value, bytes):
        value = value.decode("utf-8")
    if not isinstance(value, str):
        raise SchemaError("metadata_json must contain one JSON string")
    decoded = json.loads(value)
    if not isinstance(decoded, dict):
        raise SchemaError("metadata_json must decode to an object")
    return decoded


def _validate(
    *,
    features: np.ndarray,
    feature_names: tuple[str, ...],
    xy: np.ndarray,
    z: np.ndarray | None,
    labels: np.ndarray | None,
    trusted_mask: np.ndarray | None,
    point_ids: np.ndarray | None,
) -> None:
    if features.ndim != 2 or features.shape[0] == 0 or features.shape[1] == 0:
        raise SchemaError("features must have shape (N,F) with N,F > 0")
    if features.dtype.kind != "f":
        raise SchemaError("features must use a floating-point dtype")
    if len(feature_names) != features.shape[1]:
        raise SchemaError("feature_names length does not match features.shape[1]")
    if any(not name.strip() for name in feature_names):
        raise SchemaError("feature names cannot be empty")
    if len(set(feature_names)) != len(feature_names):
        raise SchemaError("feature names must be unique")
    if xy.shape != (features.shape[0], 2) or xy.dtype.kind != "f":
        raise SchemaError("xy must be a floating-point array with shape (N,2)")
    if z is not None and (z.shape != (features.shape[0],) or z.dtype.kind != "f"):
        raise SchemaError("z must be a floating-point array with shape (N,)")
    if labels is not None:
        if labels.shape != (features.shape[0],) or labels.dtype.kind not in "iu":
            raise SchemaError("labels must be an integer array with shape (N,)")
        if np.any(labels < 0) or np.any(labels > 255):
            raise SchemaError("ASPRS class codes must be in [0,255]")
    if trusted_mask is not None:
        if trusted_mask.shape != (features.shape[0],) or trusted_mask.dtype.kind not in "biu":
            raise SchemaError("trusted mask must be a boolean/integer array with shape (N,)")
        for start in range(0, len(trusted_mask), 1_000_000):
            chunk = trusted_mask[start : start + 1_000_000]
            if np.any((chunk != 0) & (chunk != 1)):
                raise SchemaError("trusted mask values must be 0 or 1")
    if point_ids is not None:
        if point_ids.shape != (features.shape[0],) or point_ids.dtype.kind not in "iu":
            raise SchemaError("point_ids must be an integer array with shape (N,)")


def finite_row_mask(array: np.ndarray, chunk_size: int = 1_000_000) -> np.ndarray:
    """Bound peak temporary memory while checking a large memory-mapped matrix."""
    mask = np.empty(array.shape[0], dtype=bool)
    for start in range(0, array.shape[0], chunk_size):
        stop = min(array.shape[0], start + chunk_size)
        mask[start:stop] = np.all(np.isfinite(array[start:stop]), axis=1)
    return mask


def _load_npz(path: Path) -> Dataset:
    with np.load(path, allow_pickle=False) as archive:
        required = {"features", "feature_names", "xy"}
        missing = sorted(required.difference(archive.files))
        if missing:
            raise SchemaError(f"missing NPZ arrays: {', '.join(missing)}")
        features = np.asarray(archive["features"])
        feature_names = tuple(str(value) for value in np.asarray(archive["feature_names"]).tolist())
        xy = np.asarray(archive["xy"])
        z = np.asarray(archive["z"]) if "z" in archive.files else None
        labels = np.asarray(archive["labels"]) if "labels" in archive.files else None
        if "trusted_mask" in archive.files:
            trusted_mask = np.asarray(archive["trusted_mask"])
        elif "trusted" in archive.files:
            trusted_mask = np.asarray(archive["trusted"])
        else:
            trusted_mask = None
        point_ids = np.asarray(archive["point_ids"]) if "point_ids" in archive.files else None
        trusted = bool(_scalar(archive["labels_trusted"], "labels_trusted")) if "labels_trusted" in archive.files else False
        metadata = _decode_metadata(_scalar(archive["metadata_json"], "metadata_json")) if "metadata_json" in archive.files else {}
        label_source_raw = _scalar(archive["label_source"], "label_source") if "label_source" in archive.files else metadata.get("label_source")
        label_source = str(label_source_raw) if label_source_raw is not None else None
        if "target_domain" in archive.files:
            target_raw = _scalar(archive["target_domain"], "target_domain")
        elif "target_domain" in metadata:
            target_raw = metadata["target_domain"]
        else:
            raise SchemaError("target_domain is required")
        target_domain = str(target_raw)
        if target_domain not in {"asprs_working", "archaeology_class"}:
            raise SchemaError(f"unsupported target_domain: {target_domain!r}")
    _validate(features=features, feature_names=feature_names, xy=xy, z=z, labels=labels, trusted_mask=trusted_mask, point_ids=point_ids)
    return Dataset(
        features=features,
        feature_names=feature_names,
        xy=xy,
        z=z,
        labels=labels,
        labels_trusted=trusted,
        trusted_mask=trusted_mask,
        label_source=label_source,
        target_domain=target_domain,
        point_ids=point_ids,
        metadata=metadata,
        source_path=path.resolve(),
        source_sha256=_sha256_file(path),
        source_schema=DATASET_SCHEMA,
    )


def _read_exact_array(path: Path, dtype: str, shape: tuple[int, ...]) -> np.memmap:
    expected = int(np.prod(shape, dtype=np.int64)) * np.dtype(dtype).itemsize
    if not path.is_file():
        raise SchemaError(f"missing bundle file: {path.name}")
    if path.stat().st_size != expected:
        raise SchemaError(f"{path.name} has {path.stat().st_size} bytes; expected {expected}")
    return np.memmap(path, mode="r", dtype=dtype, shape=shape, order="C")


def _load_bundle(path: Path) -> Dataset:
    manifest_path = path / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise SchemaError("bundle is missing manifest.json") from exc
    except json.JSONDecodeError as exc:
        raise SchemaError(f"invalid manifest.json: {exc}") from exc
    if manifest.get("schema") != DATASET_SCHEMA:
        raise SchemaError(f"unsupported dataset schema: {manifest.get('schema')!r}")
    try:
        n = int(manifest["point_count"] if "point_count" in manifest else manifest["rows"])
        f = int(manifest["feature_count"] if "feature_count" in manifest else manifest["columns"])
        names_raw = manifest.get("feature_names")
        if names_raw is None:
            descriptors = sorted(manifest["features"], key=lambda item: int(item["column"]))
            if [int(item["column"]) for item in descriptors] != list(range(f)):
                raise SchemaError("manifest feature columns must be exactly 0..F-1")
            names_raw = [descriptor["key"] for descriptor in descriptors]
        feature_names = tuple(str(value) for value in names_raw)
    except (KeyError, TypeError, ValueError) as exc:
        raise SchemaError("manifest point_count/feature_count/feature_names are invalid") from exc
    if n <= 0 or f <= 0:
        raise SchemaError("manifest point_count and feature_count must be positive")
    features = _read_exact_array(path / "features.f32", "<f4", (n, f))
    xy = _read_exact_array(path / "xy.f64", "<f8", (n, 2))
    z = _read_exact_array(path / "z.f64", "<f8", (n,)) if (path / "z.f64").exists() else None
    labels = _read_exact_array(path / "labels.u8", "u1", (n,)) if (path / "labels.u8").exists() else None
    trusted_mask = _read_exact_array(path / "trusted.u8", "u1", (n,)) if (path / "trusted.u8").exists() else None
    point_ids = _read_exact_array(path / "point_ids.u64", "<u8", (n,)) if (path / "point_ids.u64").exists() else None
    _validate(features=features, feature_names=feature_names, xy=xy, z=z, labels=labels, trusted_mask=trusted_mask, point_ids=point_ids)
    metadata = manifest.get("metadata")
    if metadata is None:
        metadata = {
            key: manifest[key]
            for key in ("created_utc", "source", "provenance")
            if key in manifest
        }
    if not isinstance(metadata, dict):
        raise SchemaError("manifest metadata must be an object")
    label_source_raw = manifest.get("label_source", metadata.get("label_source"))
    target_raw = manifest.get("target_domain", metadata.get("target_domain"))
    if target_raw is None:
        raise SchemaError("manifest target_domain is required")
    target_domain = str(target_raw)
    if target_domain not in {"asprs_working", "archaeology_class"}:
        raise SchemaError(f"unsupported target_domain: {target_domain!r}")
    return Dataset(
        features=features,
        feature_names=feature_names,
        xy=xy,
        z=z,
        labels=labels,
        labels_trusted=manifest.get("labels_trusted") is True,
        trusted_mask=trusted_mask,
        label_source=str(label_source_raw) if label_source_raw is not None else None,
        target_domain=target_domain,
        point_ids=point_ids,
        metadata=metadata,
        source_path=path.resolve(),
        source_sha256=_sha256_bundle(path),
        source_schema=DATASET_SCHEMA,
    )


def load_dataset(path_like: str | Path) -> Dataset:
    path = Path(path_like)
    if path.is_dir():
        return _load_bundle(path)
    if not path.is_file():
        raise SchemaError(f"dataset not found: {path}")
    if path.suffix.lower() != ".npz":
        raise SchemaError("dataset must be an .npz file or a QALML bundle directory")
    return _load_npz(path)


def dataset_summary(dataset: Dataset) -> dict[str, Any]:
    classes: list[dict[str, int]] = []
    if dataset.labels is not None:
        values, counts = np.unique(dataset.labels, return_counts=True)
        classes = [{"class": int(value), "count": int(count)} for value, count in zip(values, counts)]
    finite_features = finite_row_mask(dataset.features)
    finite_xy = finite_row_mask(dataset.xy)
    trusted_rows = (
        int(np.count_nonzero(dataset.trusted_mask))
        if dataset.trusted_mask is not None
        else (dataset.point_count if dataset.labels_trusted and dataset.labels is not None else 0)
    )
    return {
        "schema": dataset.source_schema,
        "path": str(dataset.source_path),
        "sha256": dataset.source_sha256,
        "point_count": dataset.point_count,
        "feature_count": dataset.feature_count,
        "feature_names": list(dataset.feature_names),
        "labels_present": dataset.labels is not None,
        "labels_trusted": dataset.labels_trusted,
        "trusted_mask_present": dataset.trusted_mask is not None,
        "trusted_row_count": trusted_rows,
        "label_source": dataset.label_source,
        "target_domain": dataset.target_domain,
        "classes": classes,
        "nonfinite_feature_rows": int(dataset.point_count - np.count_nonzero(finite_features)),
        "nonfinite_xy_rows": int(dataset.point_count - np.count_nonzero(finite_xy)),
        "point_ids_present": dataset.point_ids is not None,
        "z_geometry_present": dataset.z is not None,
        "metadata": dataset.metadata,
    }
