from __future__ import annotations
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
from sklearn.cluster import MiniBatchKMeans

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qal_ml.cluster_device import ClusterAssignment


class ClusterDeviceTests(unittest.TestCase):
    def setUp(self):
        self.x = np.random.default_rng(42).normal(size=(600, 4))
        self.model = MiniBatchKMeans(n_clusters=4, random_state=42, n_init=1).fit(self.x)

    def test_cpu_matches_estimator(self):
        engine = ClusterAssignment(self.model, "minibatch_kmeans", "cpu", len(self.x))
        labels, scores = engine.predict(self.x)
        np.testing.assert_array_equal(labels, self.model.predict(self.x))
        self.assertTrue(np.all((scores >= 0) & (scores <= 1)))
        self.assertEqual(engine.report["cpu_assigned_points"], len(self.x))

    def test_small_auto_skips_torch(self):
        engine = ClusterAssignment(self.model, "minibatch_kmeans", "auto", len(self.x))
        engine.predict(self.x)
        self.assertIsNone(engine.torch)
        self.assertIn("Small workload", engine.report["reason"])

    def test_runtime_failure_retries_current_chunk(self):
        engine = ClusterAssignment(self.model, "minibatch_kmeans", "cuda", len(self.x))
        engine.ready = True
        engine.cuda_active = True
        with patch.object(engine, "_cuda", side_effect=RuntimeError("simulated CUDA out of memory")):
            labels, _ = engine.predict(self.x)
        np.testing.assert_array_equal(labels, self.model.predict(self.x))
        self.assertEqual(engine.report["gpu_assigned_points"], 0)
        self.assertIn("retried on CPU", engine.report["reason"])

    def test_invalid_device_rejected(self):
        with self.assertRaises(ValueError):
            ClusterAssignment(self.model, "minibatch_kmeans", "typo", 100)

    def test_actual_cuda_parity_if_available(self):
        try:
            import torch
        except ImportError:
            self.skipTest("PyTorch not installed")
        if not torch.cuda.is_available():
            self.skipTest("CUDA PyTorch not available")
        engine = ClusterAssignment(self.model, "minibatch_kmeans", "cuda", len(self.x))
        labels, scores = engine.predict(self.x)
        np.testing.assert_array_equal(labels, self.model.predict(self.x))
        self.assertTrue(np.all(np.isfinite(scores)))
        self.assertEqual(engine.report["assignment_device"], "cuda")


if __name__ == "__main__":
    unittest.main()
