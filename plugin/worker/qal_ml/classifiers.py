from __future__ import annotations

import importlib.util
import os
import subprocess
from dataclasses import dataclass
from typing import Any

import numpy as np
from sklearn.ensemble import ExtraTreesClassifier, HistGradientBoostingClassifier, RandomForestClassifier


CLASSIFIER_IDS = (
    "random_forest",
    "extra_trees",
    "hist_gradient_boosting",
    "xgboost",
    "multiscale_mlp",
    "pointnet",
)


@dataclass(frozen=True)
class ClassifierBuild:
    estimator: Any
    family: str
    prediction_encoding: str
    resolved_device: str


def _nvidia_gpus() -> list[dict[str, Any]]:
    try:
        completed = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,driver_version,memory.total,compute_cap", "--format=csv,noheader,nounits"],
            text=True,
            capture_output=True,
            check=True,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    result = []
    for index, line in enumerate(completed.stdout.splitlines()):
        values = [value.strip() for value in line.split(",")]
        if len(values) >= 4:
            result.append({
                "index": index,
                "name": values[0],
                "driver": values[1],
                "memory_mib": int(float(values[2])),
                "compute_capability": values[3],
            })
    return result


def capability_report() -> dict[str, Any]:
    gpus = _nvidia_gpus()
    xgboost_available = importlib.util.find_spec("xgboost") is not None
    torch_available = importlib.util.find_spec("torch") is not None
    torch_cuda = False
    torch_version = None
    if torch_available:
        try:
            import torch

            torch_version = torch.__version__
            torch_cuda = bool(torch.cuda.is_available())
        except Exception:
            torch_available = False
    xgboost_cuda = False
    xgboost_version = None
    if xgboost_available:
        try:
            import xgboost

            xgboost_version = xgboost.__version__
            build = xgboost.build_info()
            xgboost_cuda = bool(build.get("USE_CUDA", False)) and bool(gpus)
        except Exception:
            xgboost_available = False
    return {
        "schema": "qal-ml-capabilities/1.0",
        "gpus": gpus,
        "classifiers": [
            {"id": "random_forest", "name": "Random Forest", "kind": "ML", "available": True, "devices": ["cpu"]},
            {"id": "extra_trees", "name": "Extra Trees", "kind": "ML", "available": True, "devices": ["cpu"]},
            {"id": "hist_gradient_boosting", "name": "Histogram Gradient Boosting", "kind": "ML", "available": True, "devices": ["cpu"]},
            {"id": "xgboost", "name": "XGBoost", "kind": "ML", "available": xgboost_available,
             "devices": (["cpu", "cuda"] if xgboost_cuda else ["cpu"]), "version": xgboost_version},
            {"id": "multiscale_mlp", "name": "Multiscale MLP", "kind": "DL", "available": torch_available,
             "devices": (["cpu", "cuda"] if torch_cuda else ["cpu"]), "version": torch_version,
             "note": "Experimental neural baseline over engineered multiscale features; not PointNet/RandLA-Net."},
            {"id": "pointnet", "name": "PointNet (local 3D patches)", "kind": "DL", "available": torch_available,
             "devices": (["cpu", "cuda"] if torch_cuda else ["cpu"]), "version": torch_version,
             "note": "Experimental PointNet-style shared MLP/max-pool on raw XYZ neighbourhoods; no pretrained weights."},
        ],
    }


def _resolve_device(classifier: str, requested: str) -> str:
    if requested not in {"auto", "cpu", "cuda"}:
        raise ValueError("device must be auto, cpu or cuda")
    capabilities = {item["id"]: item for item in capability_report()["classifiers"]}
    spec = capabilities[classifier]
    if not spec["available"]:
        raise RuntimeError(f"classifier {classifier!r} is unavailable in this Python environment")
    available = spec["devices"]
    if requested == "auto":
        return "cuda" if "cuda" in available else "cpu"
    if requested not in available:
        raise RuntimeError(
            f"classifier {classifier!r} cannot use {requested!r} in this environment; available devices: {available}"
        )
    return requested


class TorchMultiscaleMLP:
    """Small deterministic neural baseline for already-computed multiscale features."""

    def __init__(self, *, epochs: int, batch_size: int, learning_rate: float, seed: int, device: str):
        self.epochs = int(epochs)
        self.batch_size = int(batch_size)
        self.learning_rate = float(learning_rate)
        self.seed = int(seed)
        self.device = device
        self.classes_: np.ndarray | None = None
        self.mean_: np.ndarray | None = None
        self.scale_: np.ndarray | None = None
        self.state_: dict[str, np.ndarray] | None = None
        self.input_features_: int = 0
        self.training_history_: list[dict[str, float | int]] = []

    def _network(self, torch: Any) -> Any:
        width = max(32, min(256, self.input_features_ * 4))
        return torch.nn.Sequential(
            torch.nn.Linear(self.input_features_, width),
            torch.nn.ReLU(),
            torch.nn.Dropout(0.10),
            torch.nn.Linear(width, max(16, width // 2)),
            torch.nn.ReLU(),
            torch.nn.Linear(max(16, width // 2), len(self.classes_)),
        )

    def fit(self, x: np.ndarray, y: np.ndarray) -> "TorchMultiscaleMLP":
        os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
        import torch

        if self.device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA was requested but this PyTorch build is CPU-only")
        torch.manual_seed(self.seed)
        torch.use_deterministic_algorithms(True)
        torch.backends.cudnn.benchmark = False
        if self.device == "cuda":
            torch.cuda.manual_seed_all(self.seed)
        self.runtime_metadata_ = {
            "torch": str(torch.__version__), "cuda_runtime": torch.version.cuda,
            "device": self.device,
            "gpu": torch.cuda.get_device_name(0) if self.device == "cuda" else None,
            "deterministic_algorithms": True, "seed": self.seed,
            "reproducibility_scope": "same hardware/software/configuration, not CPU-GPU bit equality",
        }
        self.classes_ = np.unique(y)
        self.input_features_ = int(x.shape[1])
        self.mean_ = np.mean(x, axis=0, dtype=np.float64).astype(np.float32)
        scale = np.std(x, axis=0, dtype=np.float64).astype(np.float32)
        self.scale_ = np.where(scale > 1.0e-8, scale, 1.0).astype(np.float32)
        normalized = ((x - self.mean_) / self.scale_).astype(np.float32)
        encoded = np.searchsorted(self.classes_, y).astype(np.int64)
        device = torch.device(self.device)
        network = self._network(torch).to(device)
        counts = np.bincount(encoded, minlength=len(self.classes_)).astype(np.float32)
        weights = (len(encoded) / np.maximum(counts, 1.0))
        weights /= np.mean(weights)
        loss_function = torch.nn.CrossEntropyLoss(weight=torch.as_tensor(weights, device=device))
        optimizer = torch.optim.AdamW(network.parameters(), lr=self.learning_rate, weight_decay=1.0e-4)
        generator = torch.Generator().manual_seed(self.seed)
        dataset = torch.utils.data.TensorDataset(torch.from_numpy(normalized), torch.from_numpy(encoded))
        loader = torch.utils.data.DataLoader(
            dataset,
            batch_size=min(self.batch_size, len(dataset)),
            shuffle=True,
            generator=generator,
            num_workers=0,
        )
        network.train()
        self.training_history_ = []
        for epoch in range(self.epochs):
            loss_sum = 0.0
            sample_count = 0
            for batch_x, batch_y in loader:
                optimizer.zero_grad(set_to_none=True)
                loss = loss_function(network(batch_x.to(device)), batch_y.to(device))
                loss.backward()
                optimizer.step()
                batch_count = int(batch_y.shape[0])
                loss_sum += float(loss.detach().cpu().item()) * batch_count
                sample_count += batch_count
            self.training_history_.append({
                "epoch": epoch + 1,
                "training_loss": loss_sum / max(sample_count, 1),
            })
        self.state_ = {key: value.detach().cpu().numpy() for key, value in network.state_dict().items()}
        return self

    def predict_proba(self, x: np.ndarray) -> np.ndarray:
        import torch

        if self.state_ is None or self.classes_ is None or self.mean_ is None or self.scale_ is None:
            raise RuntimeError("neural estimator is not fitted")
        device_name = self.device if self.device != "cuda" or torch.cuda.is_available() else "cpu"
        self.last_prediction_device_ = device_name
        device = torch.device(device_name)
        network = self._network(torch)
        network.load_state_dict({key: torch.from_numpy(value) for key, value in self.state_.items()})
        network.to(device).eval()
        normalized = ((x - self.mean_) / self.scale_).astype(np.float32)
        chunks = []
        with torch.no_grad():
            for start in range(0, len(normalized), self.batch_size):
                logits = network(torch.from_numpy(normalized[start:start + self.batch_size]).to(device))
                chunks.append(torch.softmax(logits, dim=1).cpu().numpy())
        return np.concatenate(chunks, axis=0).astype(np.float32, copy=False)

    def predict(self, x: np.ndarray) -> np.ndarray:
        return self.classes_[np.argmax(self.predict_proba(x), axis=1)]


def build_classifier(config: Any) -> ClassifierBuild:
    classifier = config.classifier
    if classifier not in CLASSIFIER_IDS:
        raise ValueError(f"unsupported classifier: {classifier!r}")
    device = _resolve_device(classifier, config.device)
    if classifier == "random_forest":
        estimator = RandomForestClassifier(
            n_estimators=config.trees,
            max_depth=config.max_depth,
            min_samples_leaf=config.min_samples_leaf,
            max_features=config.max_features,
            class_weight=config.class_weight,
            criterion=config.criterion,
            n_jobs=config.n_jobs,
            random_state=config.seed,
            bootstrap=True,
        )
        return ClassifierBuild(estimator, "sklearn.ensemble.RandomForestClassifier", "direct", device)
    if classifier == "extra_trees":
        estimator = ExtraTreesClassifier(
            n_estimators=config.trees,
            max_depth=config.max_depth,
            min_samples_leaf=config.min_samples_leaf,
            max_features=config.max_features,
            class_weight=config.class_weight,
            criterion=config.criterion,
            n_jobs=config.n_jobs,
            random_state=config.seed,
        )
        return ClassifierBuild(estimator, "sklearn.ensemble.ExtraTreesClassifier", "direct", device)
    if classifier == "hist_gradient_boosting":
        weight = "balanced" if config.class_weight is not None else None
        estimator = HistGradientBoostingClassifier(
            max_iter=config.trees,
            max_depth=config.max_depth,
            min_samples_leaf=max(1, config.min_samples_leaf),
            class_weight=weight,
            random_state=config.seed,
        )
        return ClassifierBuild(estimator, "sklearn.ensemble.HistGradientBoostingClassifier", "direct", device)
    if classifier == "xgboost":
        from xgboost import XGBClassifier

        estimator = XGBClassifier(
            n_estimators=config.trees,
            max_depth=config.max_depth or 6,
            learning_rate=config.learning_rate,
            subsample=config.subsample,
            colsample_bytree=config.column_sample,
            tree_method="hist",
            device=device,
            n_jobs=config.n_jobs,
            random_state=config.seed,
            objective="multi:softprob",
        )
        return ClassifierBuild(estimator, "xgboost.XGBClassifier", "index", device)
    if classifier == "pointnet":
        from .pointnet import TorchPointNet
        estimator = TorchPointNet(epochs=config.epochs, batch_size=config.batch_size,
            learning_rate=config.learning_rate, seed=config.seed, device=device,
            radius=config.pointnet_radius, neighbors=config.pointnet_neighbors, n_jobs=config.n_jobs)
        return ClassifierBuild(estimator, "ALiS.TorchPointNet.local-patch-v1", "direct", device)
    estimator = TorchMultiscaleMLP(
        epochs=config.epochs,
        batch_size=config.batch_size,
        learning_rate=config.learning_rate,
        seed=config.seed,
        device=device,
    )
    return ClassifierBuild(estimator, "ALiS.TorchMultiscaleMLP", "direct", device)
