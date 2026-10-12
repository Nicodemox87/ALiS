"""Real minimal CPU/CUDA/ML/LAZ checks before selecting a new ALiS venv."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import joblib
import laspy
import numpy as np
import scipy
import sklearn
from sklearn.ensemble import RandomForestClassifier, ExtraTreesClassifier, HistGradientBoostingClassifier
from xgboost import XGBClassifier
import torch

parser = argparse.ArgumentParser()
parser.add_argument('--backend', choices=['cpu', 'cuda'], required=True)
args = parser.parse_args()
assert sys.prefix != sys.base_prefix, 'Use an isolated virtual environment'
x = np.array([[0, 0], [0, 1], [3, 2], [3, 3]], dtype=np.float32)
y = np.array([0, 0, 1, 1])
for model in (RandomForestClassifier(n_estimators=3, random_state=1),
              ExtraTreesClassifier(n_estimators=3, random_state=1),
              HistGradientBoostingClassifier(max_iter=3, min_samples_leaf=1),
              XGBClassifier(n_estimators=3, n_jobs=1)):
    model.fit(x, y)
    assert model.predict(x).shape == (4,)
device = 'cuda' if args.backend == 'cuda' else 'cpu'
if device == 'cuda':
    assert torch.cuda.is_available(), 'CUDA requested but unavailable; update driver or rerun CPU setup'
a = torch.ones((32, 32), device=device)
assert (a @ a)[0, 0].item() == 32
net = torch.nn.Linear(2, 2).to(device)
loss = torch.nn.functional.cross_entropy(net(torch.tensor(x, device=device)), torch.tensor(y, device=device))
loss.backward()
assert net.weight.grad is not None
with tempfile.TemporaryDirectory() as folder:
    cloud = laspy.LasData(laspy.LasHeader(point_format=3, version='1.2'))
    cloud.x = [1, 2, 3]
    cloud.y = [4, 5, 6]
    cloud.z = [7, 8, 9]
    cloud.classification = [2, 4, 6]
    path = Path(folder) / 'runtime-test.laz'
    cloud.write(path)
    np.testing.assert_array_equal(laspy.read(path).classification, [2, 4, 6])
print(json.dumps(dict(python=sys.version, numpy=np.__version__, scipy=scipy.__version__,
                     sklearn=sklearn.__version__, torch=torch.__version__, device=device,
                     ml_fit_predict=True, dl_forward_backward=True, laz_roundtrip=True)))
