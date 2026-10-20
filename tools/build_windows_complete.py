"""Build the online wrapper for the native version in plugin/info.json."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--iscc', required=True)
args = parser.parse_args()
version = json.loads((root/'plugin/info.json').read_text(encoding='utf-8'))['version']
out = root/'output/complete-bootstrap'
out.mkdir(parents=True, exist_ok=True)
subprocess.run([args.iscc, str(root/'packaging/windows-complete/CompleteSetup.iss')], check=True)
prefix = f'ALiS-{version}-Windows-x64-Complete-'
exe = out/(prefix+'Setup-r1.exe')
src = out/(prefix+'Source-r1.zip')
paths = sorted((root/'packaging/windows-complete').glob('*'))
paths += [root/'packaging/assets/ALiS.ico', root/'LICENSE', root/'plugin/info.json', root/'tools/build_windows_complete.py',
          root/'tools/tests/test_windows_complete.py', root/'docs/WINDOWS_COMPLETE_INSTALLER.md',
          root/'.github/workflows/windows-complete.yml']
with zipfile.ZipFile(src,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for p in paths:
        if p.is_file():
            z.write(p,p.relative_to(root).as_posix())
records = []
for p in (exe,src):
    with p.open('rb') as f:
        records.append(dict(name=p.name, bytes=p.stat().st_size, sha256=hashlib.file_digest(f,'sha256').hexdigest()))
(out/'ALiS-Windows-Complete-r1-SHA256SUMS.txt').write_text(
    ''.join(f"{x['sha256']}  {x['name']}\n" for x in records),encoding='ascii')
metadata = dict(revision=version+'-complete-r1', plugin_version=version,
                signed=False, online_installer=True, assets=records,
                dependency_manifest=json.loads((root/'packaging/windows-complete/dependencies.json').read_text()))
(out/'ALiS-Windows-Complete-r1.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
print(json.dumps(records,indent=2))
