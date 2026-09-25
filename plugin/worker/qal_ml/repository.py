from __future__ import annotations

import json
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPOSITORY_SCHEMA = "qal-model-repository/1.0"


def _atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def ensure_repository(path_like: str | Path) -> Path:
    root = Path(path_like).expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    for name in ("datasets", "models", "runs", "catalog"):
        (root / name).mkdir(exist_ok=True)
    manifest_path = root / "repository.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("schema") != REPOSITORY_SCHEMA:
            raise ValueError(f"unsupported repository schema in {manifest_path}")
    else:
        _atomic_json(manifest_path, {
            "schema": REPOSITORY_SCHEMA,
            "created_utc": datetime.now(timezone.utc).isoformat(),
            "description": "Local ALiS datasets, models, runs and technical reports",
        })
    return root


def register_model(root_like: str | Path, record: dict[str, Any]) -> None:
    root = ensure_repository(root_like)
    index_path = root / "catalog" / "models.json"
    if index_path.exists():
        index = json.loads(index_path.read_text(encoding="utf-8"))
        if not isinstance(index, list):
            raise ValueError("repository model catalog must be a JSON array")
    else:
        index = []
    index = [item for item in index if item.get("artifact_id") != record.get("artifact_id")]
    index.append(record)
    index.sort(key=lambda item: (item.get("created_utc", ""), item.get("artifact_id", "")))
    _atomic_json(index_path, index)


def repository_summary(root_like: str | Path) -> dict[str, Any]:
    root = ensure_repository(root_like)
    models_path = root / "catalog" / "models.json"
    models = json.loads(models_path.read_text(encoding="utf-8")) if models_path.exists() else []
    datasets = [path for path in (root / "datasets").iterdir() if path.is_dir()]
    return {
        "schema": REPOSITORY_SCHEMA,
        "path": str(root),
        "dataset_count": len(datasets),
        "model_count": len(models),
        "models": models,
    }
