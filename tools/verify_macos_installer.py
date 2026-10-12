#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the real PKG payload and test its relocated, bundled runtime."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('package',type=Path);p.add_argument('directory',type=Path);p.add_argument('--stage',type=Path);a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
package=a.package.resolve();directory=a.directory.resolve()
manifest=json.loads((package.parent/'ALiS-macOS-arm64.json').read_text())
assert package.name==manifest['filename'] and package.stat().st_size==manifest['size']
assert hashlib.file_digest(package.open('rb'),'sha256').hexdigest()==manifest['sha256']
subprocess.run(['/usr/sbin/pkgutil','--expand-full',str(package),str(directory)],check=True)
app=directory/'ALiS-component.pkg/Payload/Applications/ALiS.app'
assert app.is_dir()
if a.stage:
 def inventory(root):
  result={}
  for f in root.rglob('*'):
   name=str(f.relative_to(root))
   if f.is_symlink():
    assert f.resolve().is_relative_to(root.resolve()),f
    result[name]=('symlink',os.readlink(f))
   elif f.is_file():result[name]=('file',hashlib.file_digest(f.open('rb'),'sha256').hexdigest())
  return result
 original=inventory(a.stage.resolve());extracted=inventory(app)
 assert original==extracted, 'PKG payload differs from signed staging app'
 print('PASS identical payload files and symlinks:',len(extracted),flush=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
python=app/'Contents/Resources/ALiS/runtime/bin/python3'
env=os.environ.copy();env['PATH']='/usr/bin:/bin:/usr/sbin:/sbin';env['PYTHONNOUSERSITE']='1';env['PYTHONDONTWRITEBYTECODE']='1'
for key in list(env):
 if key.startswith('DYLD_') or key in ('PYTHONHOME','PYTHONPATH','ALIS_PYTHON','QARCHAEOLIDAR_PYTHON'):env.pop(key)
code='''import sys, pathlib, importlib, threadpoolctl
root=pathlib.Path(sys.prefix).resolve()
for name in ['numpy','scipy.sparse.linalg','sklearn','xgboost','torch','laspy','lazrs','certifi']:
 m=importlib.import_module(name);assert pathlib.Path(m.__file__).resolve().is_relative_to(root),(name,m.__file__)
pools=threadpoolctl.threadpool_info();assert len([p for p in pools if p['internal_api']=='openmp'])==1,pools
assert all(pathlib.Path(p['filepath']).resolve().is_relative_to(root) for p in pools),pools
print('PASS: bundled modules, bundled thread libraries, exactly one OpenMP runtime')
'''
subprocess.run([str(python),'-I','-B','-c',code],env=env,check=True)
subprocess.run([str(python),'-E','-s','-B',str(repo/'tools/test_installed_macos.py'),str(app)],env=env,check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
print('PASS: verified PKG, relocated app, complete worker regressions, immutable signature',flush=True)
