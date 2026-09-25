"""PointNet-style local-patch classification, implemented from Qi et al. (CVPR 2017).

This is an explicitly named adaptation, not the authors' pretrained network:
shared point MLP, symmetric masked maximum, query-centred metric neighbourhoods.
No T-Net, no absolute coordinates, no ground-truth neighbourhood attributes.
"""
from __future__ import annotations

import os
import tempfile
import time
from dataclasses import replace

import numpy as np
from scipy.spatial import cKDTree

INPUT_NAMES = ("xyz:x", "xyz:y", "xyz:z")


def coordinate_dataset(dataset):
    """Subtract double-precision origin BEFORE casting georeferenced XYZ to f32."""
    from .dataset import SchemaError
    if dataset.z is None:
        raise SchemaError("PointNet requires XYZ coordinates (the dataset has no Z).")
    xyz = np.column_stack((dataset.xy, dataset.z)).astype(np.float64)
    finite = np.all(np.isfinite(xyz), axis=1)
    if not finite.any():
        raise SchemaError("PointNet requires at least one finite XYZ point.")
    origin = np.min(xyz[finite], axis=0)
    xyz -= origin
    return replace(dataset, features=xyz.astype(np.float32), feature_names=INPUT_NAMES)


class TorchPointNet:
    def __init__(self, *, epochs=30, batch_size=128, learning_rate=.001, seed=42,
                 device="cpu", radius=.5, neighbors=64, n_jobs=1):
        self.epochs = int(epochs)
        self.batch_size = int(batch_size)
        self.learning_rate = float(learning_rate)
        self.seed = int(seed)
        self.device = device
        self.radius = float(radius)
        self.neighbors = int(neighbors)
        self.n_jobs = int(n_jobs)
        self.state_ = None
        self.classes_ = None
        self.training_history_ = []

    def __getstate__(self):
        # Reusable weights must NEVER embed a source cloud, KD tree or callback.
        return {k: v for k, v in self.__dict__.items() if not k.startswith("_context")
                and k not in {"_tree", "_events"}}

    def bind_context(self, xyz, events=None):
        xyz = np.asarray(xyz, dtype=np.float32)
        if xyz.ndim != 2 or xyz.shape[1] != 3:
            raise ValueError("PointNet context must be N x 3 raw coordinates")
        finite = np.all(np.isfinite(xyz), axis=1)
        self._context = np.ascontiguousarray(xyz[finite])
        if not len(self._context):
            raise ValueError("PointNet context has no finite XYZ points")
        self._tree = cKDTree(self._context)
        self._events = events
        return self

    def _emit(self, stage, progress, message, details=None):
        if getattr(self, "_events", None) is not None:
            self._events.emit("progress", stage, progress, message, details or {})

    def patches(self, queries):
        if not hasattr(self, "_tree"):
            raise RuntimeError("Bind the target cloud XYZ context before using PointNet")
        queries = np.asarray(queries, dtype=np.float32)
        distances, indices = self._tree.query(queries, k=self.neighbors,
            distance_upper_bound=self.radius, workers=self.n_jobs)
        valid = np.isfinite(distances)
        safe = np.where(valid, indices, 0)
        points = (self._context[safe] - queries[:, None, :]) / self.radius
        points[~valid] = 0
        # Absent neighbours are masked, never treated as actual observations.
        return points.astype(np.float32), valid

    def _network(self, torch):
        class LocalPointNet(torch.nn.Module):
            def __init__(self, classes):
                super().__init__()
                self.shared = torch.nn.Sequential(
                    torch.nn.Linear(3, 64), torch.nn.ReLU(),
                    torch.nn.Linear(64, 64), torch.nn.ReLU(),
                    torch.nn.Linear(64, 128), torch.nn.ReLU(),
                    torch.nn.Linear(128, 256), torch.nn.ReLU())
                self.head = torch.nn.Sequential(
                    torch.nn.Linear(256, 128), torch.nn.ReLU(), torch.nn.Dropout(.2),
                    torch.nn.Linear(128, 64), torch.nn.ReLU(),
                    torch.nn.Linear(64, classes))

            def forward(self, points, valid):
                values = self.shared(points).masked_fill(~valid[..., None], -1.e9)
                pooled = values.max(dim=1).values
                pooled = torch.where(valid.any(dim=1, keepdim=True), pooled, 0.)
                return self.head(pooled)
        return LocalPointNet(len(self.classes_))

    def fit(self, x, y):
        os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
        import torch
        if self.device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA requested but unavailable for PointNet")
        torch.manual_seed(self.seed)
        torch.use_deterministic_algorithms(True)
        torch.backends.cudnn.benchmark = False
        if self.device == "cuda":
            torch.cuda.manual_seed_all(self.seed)
        self.classes_ = np.unique(y)
        encoded = np.searchsorted(self.classes_, y).astype(np.int64)
        counts = np.bincount(encoded, minlength=len(self.classes_)).astype(np.float32)
        weights = len(y) / np.maximum(counts, 1)
        weights /= weights.mean()
        network = self._network(torch).to(self.device)
        optimizer = torch.optim.AdamW(network.parameters(), lr=self.learning_rate, weight_decay=1.e-4)
        loss_fn = torch.nn.CrossEntropyLoss(weight=torch.as_tensor(weights, device=self.device))
        # Disk-backed once-per-fit patches bound RAM independently of training size.
        self.training_history_ = []
        with tempfile.TemporaryDirectory(prefix="alis_pointnet_") as directory:
            patches = np.memmap(os.path.join(directory, "patches.f32"), dtype="float32", mode="w+",
                                shape=(len(x), self.neighbors, 3))
            masks = np.memmap(os.path.join(directory, "valid.u8"), dtype="bool", mode="w+",
                              shape=(len(x), self.neighbors))
            try:
                for start in range(0, len(x), 8192):
                    stop = min(start + 8192, len(x))
                    patches[start:stop], masks[start:stop] = self.patches(x[start:stop])
                    self._emit("patches", .25 + .1 * stop / len(x), "Preparing unlabelled XYZ neighbourhoods",
                               {"rows": stop, "total": len(x)})
                rng = np.random.default_rng(self.seed)
                for epoch in range(self.epochs):
                    started = time.perf_counter()
                    network.train()
                    order = rng.permutation(len(x))
                    numerator = denominator = 0.
                    for start in range(0, len(order), self.batch_size):
                        selected = order[start:start + self.batch_size]
                        bx = torch.from_numpy(np.asarray(patches[selected])).to(self.device)
                        bm = torch.from_numpy(np.asarray(masks[selected])).to(self.device)
                        by = torch.from_numpy(encoded[selected]).to(self.device)
                        optimizer.zero_grad(set_to_none=True)
                        loss = loss_fn(network(bx, bm), by)
                        loss.backward()
                        optimizer.step()
                        weight_sum = float(weights[encoded[selected]].sum())
                        numerator += float(loss.detach().cpu()) * weight_sum
                        denominator += weight_sum
                    row = {"epoch": epoch + 1, "training_loss": numerator / denominator,
                           "seconds": time.perf_counter() - started}
                    self.training_history_.append(row)
                    self._emit("fit", .35 + .44 * (epoch + 1) / self.epochs,
                               f"PointNet epoch {epoch + 1}/{self.epochs}", row)
            finally:
                patches._mmap.close()
                masks._mmap.close()
        self.state_ = {k: v.detach().cpu().numpy() for k, v in network.state_dict().items()}
        self.runtime_metadata_ = {
            "architecture": "ALiS PointNet local-patch variant v1 (not canonical PointNet/T-Net)",
            "shared_mlp": [3, 64, 64, 128, 256], "head": [256, 128, 64, len(self.classes_)],
            "pooling": "masked symmetric maximum", "dropout": .2,
            "input": "query-centred raw XYZ divided by radius; no RGB/SF/absolute elevation",
            "radius_m": self.radius, "maximum_neighbors": self.neighbors,
            "sampling": "nearest K within radius; absent neighbours masked",
            "context": "unlabelled full input geometry; no neighbour labels",
            "torch": str(torch.__version__), "cuda_runtime": torch.version.cuda,
            "device": self.device, "gpu": torch.cuda.get_device_name(0) if self.device == "cuda" else None,
            "deterministic_algorithms": True, "seed": self.seed,
            "reference": "https://arxiv.org/abs/1612.00593",
        }
        return self

    def predict_proba(self, x):
        import torch
        if self.state_ is None:
            raise RuntimeError("PointNet is not fitted")
        device = self.device if self.device != "cuda" or torch.cuda.is_available() else "cpu"
        self.last_prediction_device_ = device
        network = self._network(torch)
        network.load_state_dict({k: torch.from_numpy(v) for k, v in self.state_.items()})
        network.to(device).eval()
        result = np.empty((len(x), len(self.classes_)), dtype=np.float32)
        with torch.inference_mode():
            for start in range(0, len(x), 8192):
                stop = min(start + 8192, len(x))
                points, masks = self.patches(x[start:stop])
                for offset in range(0, len(points), self.batch_size):
                    end = min(offset + self.batch_size, len(points))
                    logits = network(torch.from_numpy(points[offset:end]).to(device),
                                     torch.from_numpy(masks[offset:end]).to(device))
                    result[start+offset:start+end] = torch.softmax(logits, dim=1).cpu().numpy()
        return result

    def predict(self, x):
        return self.classes_[self.predict_proba(x).argmax(axis=1)]
