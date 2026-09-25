"""Optional CUDA assignment of *existing* K-Means centres; fitting stays sklearn.

No new clustering algorithm or feature definition is substituted. Auto measures
host-to-device + distance + device-to-host costs and checks CPU parity first.
"""
from __future__ import annotations

from time import perf_counter
from typing import Any

import numpy as np


class ClusterAssignment:
    def __init__(self, estimator: Any, algorithm: str, requested: str, rows: int):
        if requested not in {"auto", "cpu", "cuda"}:
            raise ValueError("device must be auto, cpu or cuda")
        self.estimator = estimator
        self.algorithm = algorithm
        self.requested = requested
        self.rows = rows
        self.torch = None
        self.centres = None
        self.ready = False
        self.cuda_active = False
        self.report = {"requested": requested, "fit_device": "cpu",
                       "assignment_device": "cpu", "reason": "CPU requested",
                       "gpu_assigned_points": 0, "cpu_assigned_points": 0}

    def _cpu(self, values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        labels = self.estimator.predict(values).astype(np.int32)
        distances = self.estimator.transform(values)
        nearest = np.partition(distances, 1, axis=1)[:, :2]
        nearest.sort(axis=1)
        scores = np.clip(1 - nearest[:, 0] / np.maximum(nearest[:, 1], 1e-12), 0, 1)
        return labels, scores.astype(np.float32)

    def _cuda(self, values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        torch = self.torch
        free, _ = torch.cuda.mem_get_info()
        # Reserve at least 75% of free VRAM for CC/the OS/other processes.
        per_row = 8 * (values.shape[1] + 3 * len(self.centres) + 16)
        batch = min(65536, int(free * 0.25 / per_row))
        if batch < 256:
            raise RuntimeError("Insufficient free GPU memory for safe assignment batches")
        labels = np.empty(len(values), dtype=np.int32)
        scores = np.empty(len(values), dtype=np.float32)
        with torch.inference_mode():
            for start in range(0, len(values), batch):
                stop = min(start + batch, len(values))
                x = torch.as_tensor(np.ascontiguousarray(values[start:stop]),
                                    dtype=torch.float64, device="cuda")
                # Direct FP64 differences avoid cancellation from the dot-product
                # distance identity; no TF32 or reduced-precision approximation.
                distances = torch.cdist(x, self.centres, p=2,
                    compute_mode="donot_use_mm_for_euclid_dist")
                pair = torch.topk(distances, 2, dim=1, largest=False, sorted=True).values
                ids = torch.argmin(distances, dim=1).cpu().numpy().astype(np.int32)
                margin = (pair[:, 1] - pair[:, 0]).cpu().numpy()
                conf = (1 - pair[:, 0] / pair[:, 1].clamp_min(1e-12)).clamp(0, 1).cpu().numpy()
                # Preserve sklearn tie behaviour even at a numerical boundary.
                ambiguous = margin <= 1e-10 * np.maximum(1, pair[:, 1].cpu().numpy())
                if np.any(ambiguous):
                    ids[ambiguous], conf[ambiguous] = self._cpu(values[start:stop][ambiguous])
                labels[start:stop], scores[start:stop] = ids, conf
        return labels, scores

    def _fallback(self, reason: str) -> None:
        self.cuda_active = False
        self.centres = None
        self.report["reason"] = reason

    def _prepare(self, values: np.ndarray) -> None:
        self.ready = True
        if self.requested == "cpu":
            return
        if self.algorithm != "minibatch_kmeans":
            self._fallback("This algorithm has no validated CUDA assignment backend")
            return
        if self.requested == "auto" and self.rows < 100000:
            self._fallback("Small workload: avoid CUDA initialization/transfer overhead")
            return
        started = perf_counter()
        try:
            import torch
            self.torch = torch
            if not torch.cuda.is_available():
                raise RuntimeError("CUDA-capable PyTorch/GPU not available")
            self.report["gpu_name"] = torch.cuda.get_device_name()
            self.report["torch_version"] = torch.__version__
            self.centres = torch.as_tensor(self.estimator.cluster_centers_, dtype=torch.float64, device="cuda")
            probe = values[:min(16384, len(values))]
            cpu = self._cpu(probe)
            gpu = self._cuda(probe)
            if not np.array_equal(cpu[0], gpu[0]) or not np.allclose(cpu[1], gpu[1], atol=2e-5, rtol=2e-5):
                raise RuntimeError("CPU/CUDA pilot equivalence check failed")
            cpu_times, gpu_times = [], []
            for _ in range(3):
                t = perf_counter()
                self._cpu(probe)
                cpu_times.append(perf_counter() - t)
                t = perf_counter()
                self._cuda(probe)  # .cpu() synchronizes, includes both transfers
                gpu_times.append(perf_counter() - t)
            cpu_s, gpu_s = float(np.median(cpu_times)), float(np.median(gpu_times))
            setup = perf_counter() - started
            self.report.update(pilot_rows=len(probe), pilot_cpu_seconds=cpu_s,
                               pilot_gpu_seconds=gpu_s, setup_seconds=setup,
                               parity_passed=True, precision="float64")
            saving = (cpu_s - gpu_s) * self.rows / len(probe)
            if self.requested == "auto" and (gpu_s >= 0.8 * cpu_s or saving <= setup):
                self._fallback("Measured CUDA advantage insufficient to amortize setup and transfers")
                return
            self.cuda_active = True
            self.report["reason"] = "CUDA requested and parity verified" if self.requested == "cuda" else "Pilot predicts a net CUDA speed benefit"
        except (ImportError, OSError, RuntimeError) as exc:
            self._fallback(f"CPU fallback: {type(exc).__name__}: {exc}")

    def predict(self, values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        if not self.ready:
            self._prepare(values)
        if self.cuda_active:
            try:
                result = self._cuda(values)
                self.report["gpu_assigned_points"] += len(values)
                self.report["assignment_device"] = "cuda" if not self.report["cpu_assigned_points"] else "mixed"
                return result
            except (RuntimeError, OSError) as exc:
                self._fallback(f"CUDA failed during assignment; current chunk retried on CPU: {exc}")
        result = self._cpu(values)
        self.report["cpu_assigned_points"] += len(values)
        self.report["assignment_device"] = "mixed" if self.report["gpu_assigned_points"] else "cpu"
        return result
