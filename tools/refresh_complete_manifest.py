"""Pin the complete wrapper to the native package just built from this checkout."""
import hashlib
import json
from pathlib import Path
def refresh(root,version,setup,dll_hash,worker_hash):
    manifest_path=root/'packaging/windows-complete/dependencies.json'
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    manifest['revision']=version+'-complete-r1'
    manifest['alis']=dict(filename=setup.name,url=f"https://github.com/Nicodemox87/ALiS/releases/download/v{version}/{setup.name}",sha256=hashlib.sha256(setup.read_bytes()).hexdigest(),dll_sha256=dll_hash,worker_sha256=worker_hash)
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':
    root=Path(__file__).resolve().parents[1]
    version=json.loads((root/'plugin/info.json').read_text(encoding='utf-8'))['version']
    meta=json.loads((root/'downloads/VERSION.json').read_text(encoding='utf-8'))
    assert meta['version']==version
    setup=next(a for a in meta['assets'] if a['name'].endswith('-Setup.exe'))
    path=root/'downloads'/setup['name']
    assert hashlib.sha256(path.read_bytes()).hexdigest()==setup['sha256']
    refresh(root,version,path,meta['native_dll_sha256'],meta['block_worker_sha256'])
