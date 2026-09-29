"""Regression tests for CC snapshots / disk LAS and exact fixed-radius block context."""
import json, os, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch
import joblib, laspy, numpy as np
from sklearn.ensemble import RandomForestClassifier
from sklearn.impute import SimpleImputer
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from qal_ml import preparation as p
from qal_ml.model import MODEL_SCHEMA

NATIVE=os.environ.get('ALIS_TEST_BLOCK_WORKER')

@unittest.skipUnless(NATIVE,'Set ALIS_TEST_BLOCK_WORKER to the built native executable')
class PreparationTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        rng=np.random.default_rng(849);n=1500
        xyz=np.column_stack((rng.uniform(0,8,n),rng.uniform(0,8,n),rng.uniform(0,.3,n)))+[440000,4130000,580]
        xyz[:5,:2]=[[440000,4130000],[440004,4130004],[440008,4130008],[440004,4130000],[440000,4130004]]
        header=laspy.LasHeader(point_format=3,version='1.2');header.scales=[.001]*3;header.offsets=[440000,4130000,580]
        las=laspy.LasData(header);las.x=xyz[:,0];las.y=xyz[:,1];las.z=xyz[:,2];las.classification=np.full(n,7,dtype='u1')
        las.intensity=np.arange(n,dtype='u2');las.return_number=np.ones(n,dtype='u1');las.red=np.arange(n,dtype='u2')*10
        self.las=self.root/'source.las';las.write(self.las);self.xyz=np.column_stack((las.x,las.y,las.z));self.n=n
        self.names=['neighbor_count@1.2','planarity@1.2','roughness@1.2','z_range@1.2','sf:intensity']
        globaldir=self.root/'global';globaldir.mkdir();self.xyz.astype('<f8').tofile(globaldir/'xyz.f64');np.asarray(las.intensity,dtype='<f4').tofile(globaldir/'input.f32')
        request=dict(directory=str(globaldir),point_count=n,origin=self.xyz.min(axis=0).tolist(),features=self.names,supplied={'sf:intensity':'input.f32'},cache_bytes=128*1024**2,threads=2)
        p.save(globaldir/'request.json',request)
        p.run_native({'native':NATIVE},globaldir,self.root)
        self.global_features=np.fromfile(globaldir/'features.f32',dtype='<f4').reshape(n,-1)
        prep=SimpleImputer(strategy='median').fit(self.global_features);est=RandomForestClassifier(n_estimators=10,random_state=1,n_jobs=1).fit(prep.transform(self.global_features),np.where(np.arange(n)>750,6,2))
        self.model=self.root/'model.joblib';joblib.dump(dict(schema=MODEL_SCHEMA,artifact_id='block-test',classifier_id='random_forest',model_family='random_forest',estimator=est,preprocessor=prep,feature_schema={'names':self.names,'count':len(self.names)},class_mapping={'probability_column_to_asprs':[2,6]},target_domain='asprs_working',provenance={}),self.model)
        self.config=dict(model=str(self.model),source=str(self.las),native=NATIVE,metric_units=True,block_m=4,memory_gib=.6)
    def tearDown(self):self.temp.cleanup()
    def assembled(self,job):
        entries=p.read(job/'prepared.json')['blocks'];out=np.empty_like(self.global_features);seen=np.zeros(self.n,dtype=int)
        for b in entries:
            folder=job/'blocks'/b['key'];ids=np.fromfile(folder/'point_ids.u64',dtype='<u8');mask=np.fromfile(folder/'core.u8',dtype='u1').astype(bool)
            values=np.fromfile(folder/'features.f32',dtype='<f4').reshape(len(ids),-1);out[ids[mask]]=values[mask];np.add.at(seen,ids[mask],1)
        np.testing.assert_array_equal(seen,1);return out
    def test_disk_seams_predict_attributes_and_resume(self):
        job=self.root/'disk';p.prepare(self.config,job)
        np.testing.assert_allclose(self.assembled(job),self.global_features,rtol=2e-5,atol=2e-5,equal_nan=True)
        p.classify(job);before=p.digest(job/'classified.las');p.prepare(self.config,job);p.classify(job);self.assertEqual(before,p.digest(job/'classified.las'))
        out=laspy.read(job/'classified.las');src=laspy.read(self.las)
        for name in src.point_format.dimension_names:
            if name!='classification':np.testing.assert_array_equal(np.asarray(out[name]),np.asarray(src[name]))
        artifact=joblib.load(self.model);expected=artifact['estimator'].predict(artifact['preprocessor'].transform(self.global_features))
        np.testing.assert_array_equal(out.classification,expected);np.testing.assert_array_equal(src.classification,7)
        self.assertIsNone(p.read(job/'result/result.json')['accuracy']);self.assertTrue((job/'result/REPORT.html').exists())
    def test_cc_snapshot_matches_disk_and_reuses_explicit_fields(self):
        snap=self.root/'snapshot';snap.mkdir();columns=self.names
        dtype=np.dtype([('xyz','<f8',(3,)),('attrs','<f4',(len(columns),))]);raw=np.empty(self.n,dtype=dtype);raw['xyz']=self.xyz;raw['attrs']=self.global_features;raw.tofile(snap/'points.bin')
        p.save(snap/'snapshot.json',dict(point_count=self.n,origin=self.xyz.min(axis=0).tolist(),columns=columns))
        config=dict(self.config,source=str(snap),reuse_features=True);job=self.root/'cc';p.prepare(config,job)
        np.testing.assert_array_equal(self.assembled(job),self.global_features);p.classify(job)
        self.assertFalse((job/'classified.las').exists());self.assertEqual(p.read(job/'result/result.json')['point_count'],self.n)
    @unittest.skipUnless(laspy.LazBackend.detect_available(), 'Install the laspy[lazrs] extra')
    def test_laz_input_matches_las(self):
        compressed=self.root/'source.laz';laspy.read(self.las).write(compressed)
        job=self.root/'laz';p.prepare(dict(self.config,source=str(compressed)),job)
        np.testing.assert_allclose(self.assembled(job),self.global_features,rtol=2e-5,atol=2e-5,equal_nan=True)
        p.classify(job)
        output=laspy.read(job/'classified.las');source=laspy.read(compressed)
        np.testing.assert_array_equal(output.X,source.X)
        np.testing.assert_array_equal(output.intensity,source.intensity)
        np.testing.assert_array_equal(source.classification,7)
    def test_cancel_resume_integrity_and_changed_model(self):
        job=self.root/'cancel';job.mkdir();(job/'CANCEL').touch()
        with self.assertRaises(InterruptedError):p.prepare(self.config,job)
        (job/'CANCEL').unlink();p.prepare(self.config,job)
        block=p.read(job/'prepared.json')['blocks'][0];path=job/'blocks'/block['key']/'core.u8'
        with path.open('r+b') as f:f.write(b'\x09')
        with self.assertRaisesRegex(ValueError,'integrity'):p.classify(job)
        with self.assertRaisesRegex(ValueError,'changed'):p.prepare(dict(self.config,block_m=5),job)
    def test_units_fields_hag_and_bad_budget_fail_closed(self):
        with self.assertRaisesRegex(ValueError,'metric'):p.plan(dict(self.config,metric_units=False))
        with self.assertRaises(ValueError):p.plan(dict(self.config,memory_gib=.1))
        a=joblib.load(self.model);a['feature_schema']={'names':['hag@0'],'count':1};joblib.dump(a,self.model)
        with self.assertRaisesRegex(ValueError,'HAG is missing'):p.plan(self.config)
        with self.assertRaisesRegex(ValueError,'terrain provenance'):p.plan(dict(self.config,allow_tiled_hag=True))

if __name__=='__main__':unittest.main()
