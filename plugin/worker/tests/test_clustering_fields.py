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

if __name__ == '__main__':
    unittest.main()
