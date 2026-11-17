import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qal_ml.bootstrap import cluster, ALGORITHMS

class Events:
    def emit(self, *args):
        pass

class ClusteringFieldsTests(unittest.TestCase):
    def test_existing_ground_is_locked_and_excluded_from_fitting(self):
        rng = np.random.default_rng(24)
        features = np.vstack([np.full((20, 2), np.nan), rng.normal(0, .01, (30, 2)), rng.normal(10, .01, (30, 2))]).astype('float32')
        data = SimpleNamespace(features=features, point_count=80, feature_names=['x', 'y'], source_sha256='ground-fixture',
            labels=np.array([2]*20+[1]*60), metadata={'reuse_applied_ground': True, 'ground_revision': 7})
        for algorithm in ALGORITHMS:
            with self.subTest(algorithm=algorithm), tempfile.TemporaryDirectory() as folder:
                result = cluster(data, Path(folder), algorithm=algorithm, clusters=2, seed=42, fit_sample=80, chunk_size=13, events=Events(), device='cpu')
                labels = np.fromfile(Path(folder)/'clusters.i32', dtype='<i4')
                self.assertTrue(np.all(labels[:20] == 0))
                self.assertEqual(set(labels[20:]), {1, 2})
                self.assertEqual(result['cluster_histogram'][0], 20)
                self.assertEqual(result['unassigned_points'], 0)
                self.assertTrue(result['ground_reused'])
                self.assertEqual(result['ground_revision'], 7)

    def test_all_algorithms_on_rgb_and_existing_fields(self):
        rng = np.random.default_rng(42)
        features = np.vstack([rng.normal(v, 0.03, (100, 4)) for v in (0, 10, 20)]).astype('float32')
        features = np.column_stack([features, np.full(300, np.nan)])
        features[0] = np.nan
        data = SimpleNamespace(features=features, point_count=300,
            feature_names=['rgb:red','rgb:green','rgb:blue','sf:Intensity','sf:Empty'], source_sha256='fixture')
        for algorithm in ALGORITHMS:
            with self.subTest(algorithm=algorithm), tempfile.TemporaryDirectory() as folder:
                result = cluster(data, Path(folder), algorithm=algorithm, clusters=3, seed=42,
                    fit_sample=300, chunk_size=31, events=Events())
                labels = np.fromfile(Path(folder)/'clusters.i32', dtype='<i4')
                confidence = np.fromfile(Path(folder)/'confidence.f32', dtype='<f4')
                self.assertEqual(labels[0], -1)
                self.assertEqual(sum(result['cluster_histogram']), 299)
                self.assertEqual(result['ignored_empty_fields'], ['sf:Empty'])
                self.assertTrue(np.isfinite(confidence).all())
                self.assertTrue(((confidence >= 0) & (confidence <= 1)).all())
                self.assertEqual(len(set(labels[1:])), 3)

    def test_invalid_selection_has_clear_error(self):
        data = SimpleNamespace(features=np.full((10, 2), np.nan), point_count=10)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError, 'no finite values'):
                cluster(data, Path(folder), algorithm='birch', clusters=3, seed=42,
                    fit_sample=10, chunk_size=5, events=Events())

    def test_gmm_collapsed_float32_covariance_is_regularized(self):
        x = np.repeat(np.array([-1e15, 0, 1e15], dtype=np.float32), 100)
        matrix = np.column_stack([x, x, np.full(300, 3, dtype=np.float32)])
        data = SimpleNamespace(features=matrix, point_count=300,
            feature_names=['x', 'duplicate_x', 'constant'], source_sha256='degenerate')
        with tempfile.TemporaryDirectory() as folder:
            result = cluster(data, Path(folder), algorithm='gaussian_mixture', clusters=3,
                seed=42, fit_sample=300, chunk_size=31, events=Events(), device='cpu')
            self.assertEqual(result['fit_precision'], 'float64')
            self.assertGreaterEqual(result['reg_covar'], 1e-4)
            self.assertTrue(np.isfinite(np.fromfile(Path(folder)/'confidence.f32', dtype='<f4')).all())
            self.assertEqual(sum(result['cluster_histogram']), 300)

if __name__ == '__main__':
    unittest.main()
