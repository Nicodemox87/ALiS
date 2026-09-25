import copy
import dataclasses
import tempfile
import unittest
from pathlib import Path
import sys

import joblib
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qal_ml.pointnet import TorchPointNet, coordinate_dataset, INPUT_NAMES
from qal_ml.dataset import load_dataset, SchemaError
from qal_ml.model import TrainConfig, train, predict, _artifact_fingerprint
from test_worker_e2e import write_dataset


class PointNetTest(unittest.TestCase):
    def test_network_permutation_mask_and_roundtrip(self):
        import torch
        torch.set_num_threads(2)
        rng = np.random.default_rng(1)
        xyz = rng.normal(0, .2, (96, 3)).astype('f4')
        model = TorchPointNet(epochs=2, batch_size=16, neighbors=8, radius=.5).bind_context(xyz)
        model.fit(xyz, np.where(xyz[:, 2] > 0, 6, 4).astype('u1'))
        pts, mask = model.patches(xyz[:8])
        network = model._network(torch)
        network.load_state_dict({k: torch.from_numpy(v) for k, v in model.state_.items()})
        network.eval()
        perm = rng.permutation(8)
        with torch.inference_mode():
            a = network(torch.from_numpy(pts), torch.from_numpy(mask))
            b = network(torch.from_numpy(pts[:, perm].copy()), torch.from_numpy(mask[:, perm].copy()))
        np.testing.assert_allclose(a, b, atol=1.e-6)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)/'model.joblib'
            joblib.dump(model, path)
            reloaded = joblib.load(path)
            self.assertFalse(hasattr(reloaded, '_tree'))
            self.assertFalse(hasattr(reloaded, '_context'))
            with self.assertRaises(RuntimeError): reloaded.predict_proba(xyz[:2])
            reloaded.bind_context(xyz)
            np.testing.assert_allclose(model.predict_proba(xyz), reloaded.predict_proba(xyz), atol=1.e-7)
        # Padded slots do not affect the symmetric pooled representation.
        sparse = TorchPointNet(neighbors=8).bind_context(xyz[:1])
        pts, mask = sparse.patches(xyz[:1])
        self.assertEqual(int(mask.sum()), 1)
        pts2 = pts.copy(); pts2[~mask] = 1000
        with torch.inference_mode():
            np.testing.assert_allclose(network(torch.from_numpy(pts), torch.from_numpy(mask)),
                network(torch.from_numpy(pts2), torch.from_numpy(mask)), atol=1.e-6)

    def test_pipeline_schema_external_context_and_fingerprint(self):
        import torch
        torch.set_num_threads(2)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            write_dataset(root/'dataset')
            source = load_dataset(root/'dataset')
            mappings = [v for v in vars(source).values() if isinstance(v, np.memmap)]
            source = dataclasses.replace(source, **{k: np.array(v) for k, v in vars(source).items() if isinstance(v, np.memmap)})
            for mapping in mappings: mapping._mmap.close()
            config = TrainConfig(classifier='pointnet', device='cpu', epochs=1, batch_size=64,
                                 pointnet_neighbors=8, max_train_per_class=30)
            artifact = train(source, root/'model.joblib', config)
            self.assertEqual(artifact['feature_schema']['names'], list(INPUT_NAMES))
            self.assertEqual(artifact['artifact_id'], _artifact_fingerprint(source, config))
            self.assertEqual(artifact['training']['split']['discarded_nonfinite_rows'], 0)
            self.assertGreaterEqual(artifact['training']['split']['spatial_buffer_m'], 1.001)
            changed = dataclasses.replace(source, features=np.full_like(source.features, np.nan),
                feature_names=('other:a', 'other:b', 'other:c'))
            result = predict(changed, root/'model.joblib', root/'predictions')
            self.assertEqual(result['valid_prediction_count'], source.point_count)
            shifted = dataclasses.replace(source, xy=source.xy+np.array([500000., 4200000.]), z=source.z+900.)
            np.testing.assert_allclose(coordinate_dataset(source).features,
                coordinate_dataset(shifted).features, atol=1.e-5)
            # Independent target geometry must replace (not reuse) training context.
            external = train(source, root/'external.joblib', config, test_dataset=shifted)
            self.assertEqual(external['training']['split']['strategy'], 'external-certified-dataset')
            np.testing.assert_allclose(external['estimator']._context, coordinate_dataset(shifted).features)
            with self.assertRaises(SchemaError): coordinate_dataset(dataclasses.replace(source, z=None))

    def test_config_rejects_excessive_patch_batch(self):
        with self.assertRaises(ValueError): TrainConfig(classifier='pointnet').validate()
        with self.assertRaises(ValueError): TrainConfig(pointnet_radius=float('nan')).validate()


if __name__ == '__main__': unittest.main()
