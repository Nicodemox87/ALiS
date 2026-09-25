import unittest
from types import SimpleNamespace
import numpy as np
from qal_ml.vegetation import geometry_transform,predict_verified,RAW_FEATURES
from qal_ml.dataset import SchemaError
from qal_ml.events import NullEventWriter
from qal_ml.distributions import class_distributions,distribution_svg

class VegetationAdapterTests(unittest.TestCase):
    def setUp(self):
        self.radii=[.291,.5]
        self.names=[f'{f}@{r:.17g}' for r in self.radii for f in RAW_FEATURES]
        self.x=np.ones((20,len(self.names)),dtype=np.float32)*.1
        for j,key in enumerate(self.names):
            if key.startswith('neighbor_count'):self.x[:,j]=20
    def test_dimensions_and_support(self):
        x,names,valid=geometry_transform(self.x,self.names,self.radii)
        self.assertEqual(x.shape,(20,27));self.assertEqual(len(names),27);self.assertTrue(valid.all())
        self.x[0,0]=3
        self.assertFalse(geometry_transform(self.x,self.names,self.radii)[2][0])
    def test_radii_mismatch_rejected(self):
        with self.assertRaises(SchemaError):geometry_transform(self.x,self.names,[.3,.5])
    def test_untrusted_model_never_opened(self):
        with self.assertRaisesRegex(SchemaError,'Explicit'):
            predict_verified('missing','missing','missing',NullEventWriter())
    def test_distribution_missing_values_and_escape(self):
        x=np.array([[1.,np.nan],[2.,np.nan],[3.,np.nan]])
        result=class_distributions(x,np.array([2,2,6]),['<P>','empty'])
        self.assertEqual(result['rows'][0]['quantiles'][2],1.5)
        self.assertIsNone(result['rows'][1]['quantiles'])
        chart=distribution_svg([r for r in result['rows'] if r['feature']=='<P>'],'<P>')
        self.assertIn('&lt;P&gt;',chart);self.assertIn('<rect',chart)

if __name__=='__main__':unittest.main()
