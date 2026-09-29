"""Read-only integrity checks for the versioned public package."""
# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def sha(data):
    return hashlib.sha256(data).hexdigest()

def main():
    manifest = json.loads((ROOT/'downloads/VERSION.json').read_text())
    version = manifest['version']
    checks = 0
    for asset in manifest['assets']:
        path = ROOT/'downloads'/asset['name']
        assert path.stat().st_size == asset['bytes'], path
        assert sha(path.read_bytes()) == asset['sha256'], path
        checks += 2
        if path.suffix != '.zip':
            continue
        with zipfile.ZipFile(path) as archive:
            assert archive.testzip() is None
            checks += 1
            for name in archive.namelist():
                assert Path(name).suffix.lower() not in {'.las', '.laz', '.joblib', '.pdf', '.docx', '.qalf', '.pdb'}, name
                assert not name.startswith(('/', '../')) and '..' not in Path(name).parts, name
                checks += 2
            if '-Source.zip' in path.name:
                for folder in ['plugin', 'third_party', 'docs', 'tools']:
                    for item in (ROOT/folder).rglob('*'):
                        if item.is_file() and '__pycache__' not in item.parts and item.suffix != '.pyc':
                            assert archive.read(item.relative_to(ROOT).as_posix()) == item.read_bytes(), item
                            checks += 1
            else:
                assert sha(archive.read('plugins/ALIS_PLUGIN.dll')) == manifest['native_dll_sha256']
                assert sha(archive.read('ALiS_prepare_block.exe')) == manifest['block_worker_sha256']
                for name in ['platforms/qminimal.dll', 'doc/ALiS/MODEL_PREPARATION.md', 'doc/ALiS/third_party/qt/LICENSE.LGPL3', 'worker/ALiS/alis_prepare.py']:
                    assert name in archive.namelist(), name
                    checks += 1
                for line in archive.read('SHA256SUMS.txt').decode().splitlines():
                    digest, name = line.split('  ', 1)
                    assert sha(archive.read(name)) == digest, name
                    checks += 1
                for item in (ROOT/'plugin/worker').rglob('*'):
                    if item.is_file() and not {'tests','__pycache__'}.intersection(item.relative_to(ROOT/'plugin/worker').parts) and item.suffix != '.pyc':
                        name = 'worker/ALiS/'+item.relative_to(ROOT/'plugin/worker').as_posix()
                        assert archive.read(name) == item.read_bytes(), name
                        checks += 1
    for line in (ROOT/'downloads/CHECKSUMS_SHA256.txt').read_text().splitlines():
        digest, name = line.split('  ', 1)
        assert sha((ROOT/'downloads'/name).read_bytes()) == digest, name
        checks += 1
    print(json.dumps({'version': version, 'checks_passed': checks, 'assets': len(manifest['assets'])}, indent=2))

if __name__ == '__main__':
    main()
