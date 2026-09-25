from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import urllib.request
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "alis-pretrained-model/1.0"
STATUS_SCHEMA = "alis-pretrained-status/1.0"
RUNTIME_SCHEMA = "alis-pretrained-runtime/1.0"
CATALOG_VERSION = "2026.09.04.1"
# Provider integration is deferred in alpha.3. Legacy package helpers are kept
# for migration/removal; no external weights are advertised or installed.
CURATED: dict[str, dict[str, Any]] = {}

for _package in CURATED.values():
    _package.setdefault("catalog_version", CATALOG_VERSION)
    _package.setdefault("obsolete", False)


def _safe_id(model_id: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9._-]+", model_id):
        raise ValueError("invalid pre-trained model id")
    return model_id


def _package_dir(repository: Path, model_id: str) -> Path:
    parent = (repository.resolve() / "models" / "pretrained").resolve()
    target = (parent / _safe_id(model_id)).resolve()
    if target.parent != parent:
        raise ValueError("package path escapes the repository")
    return target


def _download(asset: str | dict[str, str], destination: Path, events: Any, asset_index: int, asset_count: int) -> dict[str, Any]:
    descriptor = {"url": asset} if isinstance(asset, str) else asset
    url = descriptor["url"]
    temporary = destination.with_suffix(destination.suffix + ".part")
    digest = hashlib.sha256()
    md5 = hashlib.md5()
    written = 0
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "ALiS-model-manager/0.6"})
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
            total = int(response.headers.get("Content-Length", "0") or 0)
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
                digest.update(chunk)
                md5.update(chunk)
                written += len(chunk)
                fraction = written / total if total else 0.5
                progress = (asset_index + min(1.0, fraction)) / max(1, asset_count)
                events.emit("progress", "download", 0.05 + 0.85 * progress, f"Downloading {destination.name}")
        if descriptor.get("sha256") and digest.hexdigest().lower() != descriptor["sha256"].lower():
            raise ValueError(f"SHA-256 mismatch for {destination.name}")
        if descriptor.get("md5") and md5.hexdigest().lower() != descriptor["md5"].lower():
            raise ValueError(f"MD5 mismatch for {destination.name}")
        temporary.replace(destination)
    finally:
        if temporary.exists():
            temporary.unlink()
    return {
        "file": destination.name,
        "bytes": written,
        "sha256": digest.hexdigest(),
        "md5": md5.hexdigest(),
        "url": url,
        "etag": response.headers.get("ETag", "").strip('"'),
        "last_modified": response.headers.get("Last-Modified", ""),
        "resolved_url": response.geturl(),
    }


def _atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".part")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def _head_asset(asset: str | dict[str, str]) -> dict[str, Any]:
    descriptor = {"url": asset} if isinstance(asset, str) else asset
    request = urllib.request.Request(
        descriptor["url"], method="HEAD", headers={"User-Agent": "ALiS-model-manager/0.6"}
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        return {
            "url": descriptor["url"],
            "resolved_url": response.geturl(),
            "etag": response.headers.get("ETag", "").strip('"'),
            "last_modified": response.headers.get("Last-Modified", ""),
            "bytes": int(response.headers.get("Content-Length", "0") or 0),
            "published_sha256": descriptor.get("sha256", ""),
            "published_md5": descriptor.get("md5", ""),
        }


def _remote_state(package: dict[str, Any]) -> tuple[str, list[dict[str, Any]], list[str]]:
    records: list[dict[str, Any]] = []
    errors: list[str] = []
    for filename, asset in package["assets"].items():
        try:
            record = _head_asset(asset)
            record["file"] = filename
            records.append(record)
        except Exception as exc:  # an unavailable host must not corrupt the local catalog
            errors.append(f"{filename}: {exc}")
    canonical = json.dumps(
        [{key: record.get(key, "") for key in ("file", "etag", "last_modified", "bytes")}
         for record in records],
        sort_keys=True, separators=(",", ":"),
    ).encode("utf-8")
    revision = hashlib.sha256(canonical).hexdigest() if records else ""
    return revision, records, errors


def _read_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def _installed_assets_valid(target: Path, manifest: dict[str, Any]) -> bool:
    assets = manifest.get("assets", [])
    if not isinstance(assets, list) or not assets:
        return False
    for record in assets:
        if not isinstance(record, dict) or not (target / str(record.get("file", ""))).is_file():
            return False
    return True


def check_updates(repository: Path, events: Any | None = None) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    total = max(1, len(CURATED))
    for index, (model_id, package) in enumerate(CURATED.items()):
        if events is not None:
            events.emit("progress", "online-check", index / total, f"Checking {package['name']}")
        target = _package_dir(repository, model_id)
        manifest = _read_manifest(target / "manifest.json")
        installed = _installed_assets_valid(target, manifest)
        remote_revision, remote_assets, errors = _remote_state(package)
        installed_catalog = str(manifest.get("catalog_version", ""))
        installed_remote = str(manifest.get("remote_revision", ""))
        latest_catalog = str(package.get("catalog_version", CATALOG_VERSION))
        update_available = installed and (
            installed_catalog != latest_catalog
            or bool(installed_remote and remote_revision and installed_remote != remote_revision)
        )
        if package.get("obsolete"):
            status = "obsolete"
        elif not installed:
            status = "available"
        elif update_available:
            status = "update_available"
        elif errors and not remote_revision:
            status = "offline_unknown"
        else:
            status = "current"
        entries.append({
            "id": model_id,
            "name": package["name"],
            "status": status,
            "installed": installed,
            "installed_catalog_version": installed_catalog,
            "latest_catalog_version": latest_catalog,
            "installed_remote_revision": installed_remote,
            "remote_revision": remote_revision,
            "remote_assets": remote_assets,
            "errors": errors,
            "checked_utc": datetime.now(timezone.utc).isoformat(),
            "superseded_by": package.get("superseded_by", ""),
        })
    result = {
        "schema": STATUS_SCHEMA,
        "catalog_version": CATALOG_VERSION,
        "checked_utc": datetime.now(timezone.utc).isoformat(),
        "entries": entries,
    }
    _atomic_json(repository.resolve() / "catalog" / "pretrained_status.json", result)
    return result


def install_curated(repository: Path, model_id: str, events: Any) -> dict[str, Any]:
    if model_id not in CURATED:
        raise ValueError("this catalog entry has no verified one-click installer; open its official source and register a local manifest")
    package = CURATED[model_id]
    target = _package_dir(repository, model_id)
    parent = target.parent
    parent.mkdir(parents=True, exist_ok=True)
    staging = parent / f".{model_id}.update-{uuid.uuid4().hex}"
    staging.mkdir(parents=False, exist_ok=False)
    assets = package["assets"]
    records = []
    try:
        for index, (filename, url) in enumerate(assets.items()):
            records.append(_download(url, staging / filename, events, index, len(assets)))
        remote_canonical = json.dumps(
            [{key: record.get(key, "") for key in ("file", "etag", "last_modified", "bytes")}
             for record in records], sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        manifest = {key: value for key, value in package.items() if key != "assets"}
        manifest.update({
            "schema": SCHEMA,
            "id": model_id,
            "installed_utc": datetime.now(timezone.utc).isoformat(),
            "managed_by": "ALiS",
            "catalog_version": package.get("catalog_version", CATALOG_VERSION),
            "remote_revision": hashlib.sha256(remote_canonical).hexdigest(),
            "assets": records,
            "execution_notice": "The package may run only after its official adapter, runtime and input signature pass validation.",
        })
        _atomic_json(staging / "manifest.json", manifest)
        backup = parent / f".{model_id}.backup-{uuid.uuid4().hex}"
        if target.exists():
            os.replace(target, backup)
        try:
            os.replace(staging, target)
        except Exception:
            if backup.exists() and not target.exists():
                os.replace(backup, target)
            raise
        if backup.exists():
            shutil.rmtree(backup)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    manifest_path = target / "manifest.json"
    return {"schema": SCHEMA, "id": model_id, "manifest": str(manifest_path), "assets": records}


def update_installed(repository: Path, events: Any) -> dict[str, Any]:
    status = check_updates(repository, events)
    candidates = [entry["id"] for entry in status["entries"] if entry["status"] == "update_available"]
    updated = []
    for index, model_id in enumerate(candidates):
        events.emit("progress", "update", index / max(1, len(candidates)), f"Updating {model_id}")
        updated.append(install_curated(repository, model_id, events))
    final_status = check_updates(repository)
    return {"schema": STATUS_SCHEMA, "updated": [item["id"] for item in updated], "status": final_status}


def prune_obsolete(repository: Path) -> dict[str, Any]:
    parent = repository.resolve() / "models" / "pretrained"
    removed: list[str] = []
    if parent.is_dir():
        for target in parent.iterdir():
            if not target.is_dir() or target.name.startswith("."):
                continue
            manifest = _read_manifest(target / "manifest.json")
            if manifest.get("managed_by") != "ALiS":
                continue
            package = CURATED.get(target.name)
            if package is None or package.get("obsolete") is True:
                shutil.rmtree(target)
                removed.append(target.name)
    return {"schema": STATUS_SCHEMA, "removed": removed, "path": str(parent)}


def _module_available(name: str) -> bool:
    try:
        return importlib.util.find_spec(name) is not None
    except (ImportError, ModuleNotFoundError, ValueError):
        return False


def _command_available(name: str) -> bool:
    return shutil.which(name) is not None


def runtime_capabilities(repository: Path) -> dict[str, Any]:
    torch_available = _module_available("torch")
    cuda = False
    torch_version = ""
    if torch_available:
        try:
            import torch
            cuda = bool(torch.cuda.is_available())
            torch_version = str(torch.__version__)
        except Exception:
            pass
    open3d_ml = False
    open3d_reason = "Open3D-ML with a compatible PyTorch backend is not available."
    try:
        import open3d.ml.torch  # noqa: F401
        open3d_ml = True
        open3d_reason = "Open3D-ML PyTorch backend imported successfully."
    except Exception as exc:
        open3d_reason = str(exc)
    docker = _command_available("docker")
    # These probes only detect an upstream provider runtime.  They do *not* mean
    # that ALiS already has a validated input/output adapter for that provider.
    providers = {
        "myria3d": (_module_available("myria3d.models.model"), "Myria3D Python environment and official prediction entry point"),
        "open3dml": (open3d_ml, open3d_reason),
        "spt": (bool(os.environ.get("ALIS_SPT_ROOT")), "Set ALIS_SPT_ROOT to a validated official SPT checkout/environment."),
        "ezsp": (bool(os.environ.get("ALIS_SPT_ROOT")), "Set ALIS_SPT_ROOT to a validated official SPT/EZ-SP checkout/environment."),
        "segmentanytree": (docker, "Official SegmentAnyTree execution uses its GPU Docker image."),
        "adaf": (_module_available("adaf"), "ADAF Python package/repository with a selected archaeological checkpoint"),
        "utonia": (torch_available and _module_available("pointcept"), "Produces embeddings; a validated task head is required for classes."),
        "sonata": (torch_available and _module_available("pointcept"), "Produces embeddings; a validated task head is required for classes."),
        "frnet": (_module_available("onnxruntime"), "ONNX Runtime is required; FRNet also requires the matching HESAI scan geometry."),
    }
    setup_guides = {
        "myria3d": "Install Ubuntu 22.04 under WSL2, NVIDIA CUDA support and the official Myria3D environment. ALiS still needs its audited LAS-to-Myria3D and prediction-import adapter; installing Python alone is insufficient.",
        "open3dml": "Install a mutually compatible PyTorch and Open3D-ML environment, then validate the ALiS dataset adapter against the exact upstream checkpoint taxonomy.",
        "spt": "Use the official Linux/CUDA environment and checkpoint, set ALIS_SPT_ROOT, then validate ALiS superpoint export and point-wise result import.",
        "ezsp": "Use the official Linux/CUDA SPT/EZ-SP environment, set ALIS_SPT_ROOT, then validate ALiS superpoint export and point-wise result import.",
        "segmentanytree": "Install Docker with NVIDIA GPU support and the official SegmentAnyTree image. Run only on a vegetation subset; ALiS still needs the validated instance-ID import adapter.",
        "adaf": "Install the official ADAF environment and choose a compatible archaeological checkpoint. This is a DTM-derived 2.5D detection workflow, not direct raw-point classification.",
        "utonia": "Install the official Pointcept/PyTorch environment and fine-tune a documented classification head. The published encoder alone only generates embeddings.",
        "sonata": "Install the official Pointcept/PyTorch environment and fine-tune a documented classification head. The published encoder alone only generates embeddings.",
        "frnet": "Install the official ONNX/TensorRT runtime and use native sensor-centred OT128/QT128 frames. Georeferenced aerial tiles cannot be converted losslessly to this input geometry.",
    }
    # An execution adapter enters this allow-list only after an end-to-end test
    # verifies input conversion, upstream inference and aligned point outputs.
    implemented_execution_adapters: set[str] = set()
    entries = []
    for model_id, package in CURATED.items():
        adapter = str(package.get("adapter", ""))
        provider_known = adapter in providers
        backend_ready, reason = providers.get(adapter, (False, "No known provider runtime probe."))
        target = _package_dir(repository, model_id)
        installed = _installed_assets_valid(target, _read_manifest(target / "manifest.json"))
        classifier = adapter not in {"utonia", "sonata"}
        adapter_implemented = adapter in implemented_execution_adapters
        if not installed:
            blocker_code = "package_missing"
            explanation = "Checkpoint/package is not installed. Installation only downloads verified data; it does not install or execute provider code."
        elif not classifier:
            blocker_code = "classification_head_missing"
            explanation = f"{reason} This catalog entry is an encoder, not a classifier by itself."
        elif not provider_known:
            blocker_code = "provider_not_integrated"
            explanation = "No ALiS runtime probe or audited execution contract exists for this provider."
        elif not adapter_implemented:
            blocker_code = "alis_adapter_pending"
            explanation = "The upstream project is known, but ALiS has no end-to-end validated execution adapter for it yet. " + reason
        elif not backend_ready:
            blocker_code = "runtime_missing"
            explanation = reason
        else:
            blocker_code = ""
            explanation = "Checkpoint, ALiS adapter and provider runtime passed the local readiness check."
        entries.append({
            "id": model_id,
            "adapter": adapter,
            "provider_known": provider_known,
            "runtime_detected": bool(backend_ready),
            "adapter_implemented": adapter_implemented,
            "runtime_ready": bool(installed and backend_ready and classifier and adapter_implemented),
            "package_ready": installed,
            "cuda_available": cuda,
            "action": "classification" if classifier else "embeddings",
            "blocker_code": blocker_code,
            "reason": explanation,
            "setup_guide": setup_guides.get(adapter, "A provider-specific, audited ALiS execution adapter must be implemented and validated before inference."),
        })
    result = {
        "schema": RUNTIME_SCHEMA,
        "checked_utc": datetime.now(timezone.utc).isoformat(),
        "python": os.sys.executable,
        "torch_version": torch_version,
        "cuda_available": cuda,
        "entries": entries,
    }
    _atomic_json(repository.resolve() / "catalog" / "pretrained_runtime.json", result)
    return result


def synchronize_status(repository: Path, events: Any | None = None) -> dict[str, Any]:
    return {"updates": check_updates(repository, events), "runtimes": runtime_capabilities(repository)}


def remove_package(repository: Path, model_id: str) -> dict[str, Any]:
    target = _package_dir(repository, model_id)
    existed = target.is_dir()
    if existed:
        shutil.rmtree(target)
    return {"schema": SCHEMA, "id": model_id, "removed": existed, "path": str(target)}
