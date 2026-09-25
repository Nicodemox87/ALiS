"""Assemble an allowlisted ALiS testing distribution; never include survey outputs."""
# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import zipfile

VERSION = '0.1.0-alpha.5.6'
ROOT = Path(__file__).resolve().parents[1]

def digest(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()

def copy(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)

def files(root):
    return sorted(p for p in root.rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc')

def archive(root, dest, selected=None):
    with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in selected or files(root):
            z.write(p, p.relative_to(root).as_posix())

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, required=True)
    parser.add_argument('--assets', type=Path, default=ROOT/'packaging/assets')
    parser.add_argument('--iscc', type=Path, required=True)
    args = parser.parse_args()
    stage = ROOT/'packaging/stage'
    if stage.exists():
        raise SystemExit('Staging already exists; use a fresh source checkout/staging directory. Nothing overwritten.')
    out = ROOT/'downloads'
    out.mkdir(exist_ok=True)
    copy(args.dll, stage/'plugins/ALIS_PLUGIN.dll')
    for p in files(ROOT/'plugin/worker'):
        if 'tests' not in p.relative_to(ROOT/'plugin/worker').parts:
            copy(p, stage/'worker/ALiS'/p.relative_to(ROOT/'plugin/worker'))
    for name in ['setup_cpu_runtime.ps1', 'setup_cuda_runtime.ps1', 'verify_cuda_runtime.py']:
        copy(ROOT/'tools'/name, stage/'worker/ALiS'/name)
    for name in ['GETTING_STARTED.txt', 'RUNTIME.md']:
        copy(ROOT/'docs'/name, stage/'doc/ALiS'/name)
    for name in ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'TESTING_NOTICE.txt', 'RELEASE_NOTES.md']:
        copy(ROOT/name, stage/'doc/ALiS'/name)
    for p in files(ROOT/'third_party/qCSF'):
        if p.name in ['LICENSE', 'license.txt', 'ORIGIN.md']:
            copy(p, stage/'doc/ALiS/third_party/qCSF'/p.name)
    copy(ROOT/'LICENSE', stage/'LICENSE.txt')
    copy(ROOT/'TESTING_NOTICE.txt', stage/'TESTING_NOTICE.txt')
    copy(ROOT/'docs/GETTING_STARTED.txt', stage/'INSTALL.txt')
    copy(ROOT/'packaging/ALiS Launcher.cmd', stage/'ALiS Launcher.cmd')
    for name in ['ALiS.ico', 'ALiS-wizard.bmp']:
        copy(args.assets/name, stage/name)
    (stage/'SHA256SUMS.txt').write_text(''.join(f'{digest(p)}  {p.relative_to(stage).as_posix()}\n' for p in files(stage)), encoding='ascii')
    zip_path = out/f'ALiS-{VERSION}-CloudCompare-2.13.2-Windows-x64.zip'
    archive(stage, zip_path)
    subprocess.run([str(args.iscc), '/O'+str(out), str(ROOT/'packaging/ALiS.iss')], check=True)
    source_paths = []
    for folder in ['plugin', 'profiles', 'tools', 'third_party', 'docs', '.github']:
        source_paths.extend(files(ROOT/folder))
    source_paths.extend(p for p in ROOT.iterdir() if p.is_file() and p.suffix in ['.md', '.txt', '.cff'])
    source_paths.extend([ROOT/'LICENSE', ROOT/'.gitignore', ROOT/'packaging/ALiS.iss', ROOT/'packaging/ALiS Launcher.cmd'])
    source_paths.extend(files(ROOT/'packaging/assets'))
    archive(ROOT, out/f'ALiS-{VERSION}-Source.zip', sorted(set(source_paths)))
    products = sorted(out.glob('*.zip')) + sorted(out.glob('*.exe'))
    (out/'CHECKSUMS_SHA256.txt').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in products), encoding='ascii')
    (out/'VERSION.json').write_text(json.dumps({'version': VERSION, 'tag': 'v'+VERSION, 'prerelease': True, 'native_dll_sha256': digest(args.dll), 'assets': [{'name': p.name, 'bytes': p.stat().st_size, 'sha256': digest(p)} for p in products]}, indent=2)+'\n', encoding='utf8')
    for p in products:
        print(p.name, p.stat().st_size, digest(p))

if __name__ == '__main__':
    main()
