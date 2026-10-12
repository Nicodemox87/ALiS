#!/usr/bin/env python3
"""Run regression tests against the installed Python package and native worker."""
import argparse,os,sys,unittest
from pathlib import Path
sys.dont_write_bytecode = True
ap=argparse.ArgumentParser();ap.add_argument('app',type=Path);args=ap.parse_args()
repo=Path(__file__).resolve().parents[1];app=args.app.resolve();worker=app/'Contents/Resources/ALiS/worker'
os.environ['ALIS_TEST_BLOCK_WORKER']=str(app/'Contents/MacOS/ALiS_prepare_block')
sys.path.insert(0,str(worker))
import qal_ml
assert Path(qal_ml.__file__).resolve().is_relative_to(worker)
suite=unittest.defaultTestLoader.discover(str(repo/'plugin/worker/tests'))
import test_worker_e2e
test_worker_e2e.WORKER_ROOT=worker;test_worker_e2e.ENTRYPOINT=worker/'qal_ml_worker.py'
for name,mod in list(sys.modules.items()):
 if name.startswith('qal_ml') and getattr(mod,'__file__',None):assert Path(mod.__file__).resolve().is_relative_to(worker),(name,mod.__file__)
print('Installed Python worker:',worker,flush=True)
print('Installed native worker:',os.environ['ALIS_TEST_BLOCK_WORKER'],flush=True)
result=unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(not result.wasSuccessful())
