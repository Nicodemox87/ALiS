"""Disk-only feature export: complete geometry, bounded arrays, atomic failure."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import laspy
import numpy as np
from qal_ml import disk_features as d
from qal_ml.preparation import Source


class DiskFeaturesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.n = 53
        self.keys = ['planarity@0.4', 'barycenter_offset_ratio@0.123456789123456']
        self.columns = [dict(name='qAL_ASPRS_Working', kind='sf'), dict(name='Intensity', kind='sf'),
                        dict(name='Return Number', kind='sf'), dict(name='Number Of Returns', kind='sf'),
                        dict(name='old field è', kind='sf'), *[dict(name=c, kind='rgb') for c in ('red','green','blue')],
                        dict(name='normal_x',kind='normal')]
        self.dtype = np.dtype([('xyz','<f8',(3,)), ('attrs','<f4',(len(self.columns),))])
        self.rows = np.zeros(self.n, dtype=self.dtype)
        self.rows['xyz'] = np.column_stack((np.arange(self.n)*.123456,np.arange(self.n)*.8,np.arange(self.n)*.009))+[530000,4473000,800]
        self.rows['attrs'][:,0] = np.where(np.arange(self.n)%2,2,6)
        self.rows['attrs'][:,1] = np.arange(self.n)*100
        self.rows['attrs'][:,2:4] = [1,5]
        self.rows['attrs'][:,4] = np.arange(self.n)*.2
        self.rows['attrs'][4,4] = np.nan
        self.rows['attrs'][:,5:8] = np.arange(self.n)[:,None]%256
        self.rows['attrs'][:,8] = .5
        self.rows.tofile(self.root/'export_snapshot.bin')
        self.coord = hashlib.sha256(self.rows['xyz'].copy().tobytes()).hexdigest()
        self.meta = dict(schema='alis-disk-feature-snapshot/1', point_count=self.n, columns=self.columns,
                         mins=self.rows['xyz'].min(axis=0).tolist(), maxs=self.rows['xyz'].max(axis=0).tolist(),
                         coordinate_sha256=self.coord, cc_metadata={})
        self.ledger = dict(disk_only=True, point_count=self.n, identity='fixture-identity', feature_keys=self.keys,
                           config=dict(coordinate_sha256=self.coord), blocks={})
        self.values = np.column_stack((np.arange(self.n)*.01,np.arange(self.n)*.03)).astype('f4')
        self.add_patch('B0001',np.arange(0,self.n,2))
        self.add_patch('B0002',np.arange(1,self.n,2))
        self.output = self.root/'result.las'

    def tearDown(self):
        self.temp.cleanup()

    def add_patch(self,key,ids):
        meta = json.dumps(dict(identity=self.ledger['identity'],features=self.keys,block=key)).encode()
        records = np.empty(len(ids),dtype=[('id','<u4'),('values','<f4',(len(self.keys),))])
        records['id'] = ids
        records['values'] = self.values[ids]
        raw = b'ALISF02'+struct.pack('<I',len(meta))+meta+struct.pack('<II',len(ids),len(self.keys))+records.tobytes()
        path = self.root/(key+'.alsf')
        path.write_bytes(raw)
        self.ledger['blocks'][key] = dict(points=len(ids),checkpoint=str(path),sha256=hashlib.sha256(raw).hexdigest())

    def export(self,chunk=7):
        (self.root/'export_snapshot.json').write_text(json.dumps(self.meta))
        (self.root/'disk_features.json').write_text(json.dumps(self.ledger))
        with contextlib.redirect_stdout(io.StringIO()):
            return d.export(self.root/'export_snapshot.json',self.root/'disk_features.json',self.output,chunk_limit=chunk)

    def test_streamed_values_geometry_fields_rgb_returns_and_alias_reuse(self):
        report = self.export()
        self.assertEqual(report['completed_feature_points'],self.n)
        las = laspy.read(self.output)
        dimension_names=list(las.point_format.dimension_names)
        self.assertEqual(len(dimension_names),len({name.casefold() for name in dimension_names}))
        np.testing.assert_allclose(np.column_stack((las.x,las.y,las.z)),self.rows['xyz'],atol=.000051,rtol=0)
        np.testing.assert_array_equal(las.classification,self.rows['attrs'][:,0])
        np.testing.assert_array_equal(las.red,self.rows['attrs'][:,5]*257)
        np.testing.assert_array_equal(las.intensity,self.rows['attrs'][:,1])
        np.testing.assert_array_equal(las.return_number,self.rows['attrs'][:,2])
        np.testing.assert_array_equal(las.number_of_returns,self.rows['attrs'][:,3])
        for i,key in enumerate(self.keys):
            np.testing.assert_array_equal(las[report['features'][key]],self.values[:,i])
        for index,alias in report['original_fields'].items():
            np.testing.assert_array_equal(las[alias],self.rows['attrs'][:,int(index)])
        source = Source(self.output,self.keys,True)
        self.assertEqual(source.columns[self.keys[1]],report['features'][self.keys[1]])
        original_source=Source(self.output,['sf:old%20field%20%C3%A8','rgb:red'],True)
        _,_,attrs=next(original_source.batches())
        np.testing.assert_array_equal(attrs['sf:old%20field%20%C3%A8'],self.rows['attrs'][:,4])
        np.testing.assert_array_equal(attrs['rgb:red'],self.rows['attrs'][:,5])
        self.assertLessEqual(report['chunk_rows'],7)

    def test_partial_job_keeps_all_points_and_marks_pending_nan(self):
        del self.ledger['blocks']['B0002']
        report = self.export()
        las = laspy.read(self.output)
        self.assertEqual(len(las.points),self.n)
        self.assertEqual(report['pending_feature_points'],self.n//2)
        self.assertTrue(np.isnan(las[report['features'][self.keys[0]]][1::2]).all())

    def test_corrupt_hash_retains_previous_target(self):
        self.output.write_bytes(b'prior output')
        self.ledger['blocks']['B0001']['sha256'] = '0'*64
        with self.assertRaisesRegex(ValueError,'integrity'):
            self.export()
        self.assertEqual(self.output.read_bytes(),b'prior output')
        self.assertEqual(list(self.root.glob('.alis-features-*')),[])

    def test_overlapping_cores_rejected(self):
        self.add_patch('B0003',np.array([0,1]))
        with self.assertRaisesRegex(ValueError,'Overlapping'):
            self.export()

    def test_changed_geometry_retains_previous_target(self):
        self.output.write_bytes(b'prior output')
        self.rows['xyz'][0,0] += .1
        self.rows.tofile(self.root/'export_snapshot.bin')
        with self.assertRaisesRegex(ValueError,'geometry/order'):
            self.export()
        self.assertEqual(self.output.read_bytes(),b'prior output')

    def test_wrong_schema_rejected(self):
        self.ledger['feature_keys'] = ['other@1','other@2']
        with self.assertRaisesRegex(ValueError,'schema'):
            self.export()

    def test_absent_rgb_is_not_offered_as_observed_model_features(self):
        for column in self.columns:
            if column['kind']=='rgb':column['kind']='sf'
        self.export()
        with self.assertRaisesRegex(ValueError,'RGB scaling'):
            Source(self.output,['rgb:red'],True)

    def test_missing_checkpoint_does_not_overwrite(self):
        Path(self.ledger['blocks']['B0001']['checkpoint']).unlink()
        self.output.write_bytes(b'prior output')
        with self.assertRaises(FileNotFoundError):
            self.export()
        self.assertEqual(self.output.read_bytes(),b'prior output')

    def test_duplicate_or_unsorted_ids_rejected(self):
        self.add_patch('B0001',np.array([4,0,2]))
        with self.assertRaisesRegex(ValueError,'unordered'):
            self.export()

    def test_las_label_range_never_silently_truncated(self):
        self.rows['attrs'][0,0]=256
        self.rows.tofile(self.root/'export_snapshot.bin')
        with self.assertRaisesRegex(ValueError,'ASPRS'):
            self.export()

    def test_memory_matrix_never_uses_full_cloud_rows(self):
        original=np.full
        observed=[]
        def checked(shape,*args,**kwargs):
            if isinstance(shape,tuple) and len(shape)==2 and shape[1]==len(self.keys):
                observed.append(shape[0])
                self.assertLessEqual(shape[0],5)
            return original(shape,*args,**kwargs)
        with patch.object(d.np,'full',side_effect=checked):
            self.export(chunk=5)
        self.assertTrue(observed)


if __name__ == '__main__':
    unittest.main()
