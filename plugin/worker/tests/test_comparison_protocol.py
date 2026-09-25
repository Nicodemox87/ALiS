from __future__ import annotations
import sys
import tempfile
import unittest
from pathlib import Path
import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qal_ml.dataset import load_dataset
from qal_ml.model import TrainConfig, train, load_artifact, predict
from qal_ml.split import spatial_block_split
from qal_ml.comparison import export_prediction_las, sha256
from sklearn.model_selection import GroupShuffleSplit
from test_worker_e2e import write_dataset


class ComparisonProtocolTest(unittest.TestCase):
    def test_las_prediction_preserves_every_other_byte_and_flags(self):
        import laspy
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            source=root/'source.las'; output=root/'predicted.las'; bundle=root/'prediction'
            bundle.mkdir()
            cloud=laspy.LasData(laspy.LasHeader(point_format=3,version='1.2'))
            cloud.x=[441000.01,441001.02,441002.03,441003.04]
            cloud.y=[4135000.11,4135001.12,4135002.13,4135003.14]
            cloud.z=[600.1,601.2,602.3,603.4]
            cloud.classification=[2,4,6,7]
            cloud.synthetic=[0,1,0,1]; cloud.key_point=[1,0,1,0]; cloud.withheld=[0,0,1,1]
            cloud.intensity=[100,200,300,400]; cloud.red=[123,456,789,1011]
            cloud.write(source)
            before=source.read_bytes(); digest=sha256(source)
            np.array([6,2,4,-1],dtype='<i2').tofile(bundle/'predictions.i16')
            result=export_prediction_las(source,output,bundle,digest)
            actual=bytearray(output.read_bytes()); expected=bytearray(before)
            with laspy.open(source) as reader:
                offset=reader.header.offset_to_point_data; stride=reader.header.point_format.size
            for row,code in enumerate([6,2,4,1]):
                index=offset+row*stride+15
                expected[index]=(expected[index]&224)|code
            self.assertEqual(actual,expected)
            self.assertEqual(source.read_bytes(),before)
            self.assertEqual(result['points'],4)
            with self.assertRaises(FileExistsError): export_prediction_las(source,output,bundle,digest)
            np.array([2,4,6,32],dtype='<i2').tofile(bundle/'predictions.i16')
            with self.assertRaises(ValueError): export_prediction_las(source,root/'invalid.las',bundle,digest)
            self.assertFalse((root/'invalid.las').exists())

    def test_same_split_train_only_imputation_cap_and_reload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/'dataset'
            write_dataset(source)
            data = load_dataset(source)
            artifacts = []
            for name in ('random_forest', 'extra_trees'):
                model = root/name/'model.joblib'
                artifact = train(data, model, TrainConfig(classifier=name, device='cpu', trees=5,
                    n_jobs=1, max_train_per_class=12, spatial_buffer=.25, impute_missing=True))
                artifacts.append(artifact)
                training = np.load(str(model)+'.train_point_ids.npy')
                heldout = np.load(str(model)+'.test_point_ids.npy')
                self.assertEqual(len(np.intersect1d(training,heldout)),0)
                self.assertTrue(np.all(cKDTree(data.xy[heldout]).query(data.xy[training])[0] > .25))
                self.assertTrue(all(item['count'] <= 12 for item in artifact['training']['split']['train_class_counts']))
                np.testing.assert_allclose(artifact['preprocessor'].statistics_, np.nanmedian(data.features[training],axis=0))
                self.assertEqual(len(artifact['training']['feature_importances']),data.feature_count)
                loaded = load_artifact(model)
                self.assertIsNotNone(loaded['preprocessor'])
                output = root/(name+'_predictions')
                predict(data,model,output)
                values = np.fromfile(output/'predictions.i16',dtype='<i2')
                self.assertEqual(len(values),data.point_count)
                self.assertTrue(np.all(values >= 0))  # includes the deliberately nonfinite source row
            for key in ('train_point_ids_sha256','test_point_ids_sha256'):
                self.assertEqual(artifacts[0]['training']['split'][key],artifacts[1]['training']['split'][key])
            del data  # Release Windows memmap handles before TemporaryDirectory cleanup.

    def test_invalid_protocol_parameters(self):
        for config in (TrainConfig(max_train_per_class=-1), TrainConfig(spatial_buffer=-1), TrainConfig(spatial_buffer=float('nan')),
                       TrainConfig(criterion='invalid'), TrainConfig(subsample=0), TrainConfig(column_sample=1.1)):
            with self.assertRaises(ValueError): config.validate()

    def test_fast_block_scoring_matches_legacy_exactly(self):
        rng=np.random.default_rng(777)
        xy=rng.uniform(0,100,(5000,2)); labels=rng.choice([2,4,6,7],len(xy))
        cells=np.floor((xy-xy.min(axis=0))/20).astype(np.int64)
        _,ids=np.unique(cells,axis=0,return_inverse=True)
        best=None
        for training,testing in GroupShuffleSplit(n_splits=96,test_size=.2,random_state=42).split(xy,labels,groups=ids):
            if len(np.setdiff1d(np.unique(labels),np.unique(labels[training]))): continue
            missing=len(np.setdiff1d(np.unique(labels),np.unique(labels[testing])))
            score=missing*10+abs(len(testing)-len(labels)*.2)/len(labels)
            if best is None or score<best[0]: best=(score,training,testing)
        actual=spatial_block_split(xy,labels,block_size=20,test_fraction=.2,seed=42)
        np.testing.assert_array_equal(actual.train_indices,best[1])
        np.testing.assert_array_equal(actual.test_indices,best[2])


if __name__ == '__main__': unittest.main()
