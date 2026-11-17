"""Package the verified Windows plugin; do not include user datasets or environments."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.1.0-alpha.5.21'
EXPECTED_DLL = 'd58b60bc15887e30f8d905f16d15234b22c68093341e7190944e50d5e8e218f9'

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

INSTALL = r'''#Requires -RunAsAdministrator
param([string]$CloudCompareDir = "$env:ProgramFiles\CloudCompare")
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if(Get-Process CloudCompare -ErrorAction SilentlyContinue){throw 'Save your work and close CloudCompare before updating.'}
$hostExe = Join-Path $CloudCompareDir 'CloudCompare.exe'
if(!(Test-Path -LiteralPath $hostExe) -or !(Test-Path -LiteralPath (Join-Path $CloudCompareDir 'Qt5Core.dll'))){throw 'Select an existing CloudCompare 2.13.2 x64 / Qt5 installation.'}
$changelog = Join-Path $CloudCompareDir 'CHANGELOG.md'
if(!(Test-Path -LiteralPath $changelog) -or !((Get-Content -LiteralPath $changelog -TotalCount 9) -match '^v2\.13\.2(?:\s|$)')){throw 'CloudCompare CHANGELOG must identify v2.13.2 stable in its first nine lines.'}
$pe = [IO.File]::ReadAllBytes($hostExe)
$peOffset = [BitConverter]::ToInt32($pe,60)
if([BitConverter]::ToUInt16($pe,$peOffset+4) -ne 0x8664){throw 'CloudCompare must be x64.'}
$manifest = Get-Content -LiteralPath (Join-Path $root 'PAYLOAD_SHA256.json') -Raw | ConvertFrom-Json
$payload = [IO.Path]::GetFullPath((Join-Path $root 'payload'))
$targetRoot = [IO.Path]::GetFullPath($CloudCompareDir)
$entries = @()
foreach($item in $manifest){
    $relative = $item.path.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $source = [IO.Path]::GetFullPath((Join-Path $payload $relative))
    $target = [IO.Path]::GetFullPath((Join-Path $targetRoot $relative))
    if(!$source.StartsWith($payload+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or !$target.StartsWith($targetRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe payload path'}
    if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256){throw "Payload checksum mismatch: $relative"}
    $entries += @{Relative=$relative;Source=$source;Target=$target;Expected=$item.sha256;Existed=(Test-Path -LiteralPath $target)}
}
$backup = Join-Path $targetRoot ('ALiS-Backups\before-alpha.5.21-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $backup | Out-Null
# Back up every existing target before making the first replacement.
foreach($item in $entries){if($item.Existed){$saved=Join-Path $backup $item.Relative;New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($saved)) -Force | Out-Null;Copy-Item -LiteralPath $item.Target -Destination $saved}}
$changed = @()
try {
    foreach($item in $entries){
        $changed += $item
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($item.Target)) -Force | Out-Null
        Copy-Item -LiteralPath $item.Source -Destination $item.Target -Force
        if((Get-FileHash -LiteralPath $item.Target).Hash.ToLowerInvariant() -ne $item.Expected){throw "Installed checksum mismatch: $($item.Relative)"}
    }
} catch {
    foreach($item in $changed){if($item.Existed){Copy-Item -LiteralPath (Join-Path $backup $item.Relative) -Destination $item.Target -Force}else{if(Test-Path -LiteralPath $item.Target){Remove-Item -LiteralPath $item.Target -Force}}}
    throw
}
Write-Host "ALiS alpha.5.21 installed and verified. Backup: $backup" -ForegroundColor Green
Write-Host 'Reopen CloudCompare and your saved BIN project. No survey file or model was changed.'
Read-Host 'Press Enter to close'
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, required=True)
    parser.add_argument('--block-worker', type=Path, required=True)
    parser.add_argument('--qt-minimal', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    assert json.loads((ROOT/'plugin/info.json').read_text())['version'] == VERSION
    assert sha(args.dll) == EXPECTED_DLL, 'DLL differs from installed/tested build'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    payload = {'plugins/ALIS_PLUGIN.dll': args.dll, 'ALiS_prepare_block.exe': args.block_worker,
               'platforms/qminimal.dll': args.qt_minimal}
    for path in (ROOT/'plugin/worker').rglob('*'):
        rel = path.relative_to(ROOT/'plugin/worker')
        if path.is_file() and not any(p in ('tests','__pycache__') for p in rel.parts) and path.suffix.lower() in ('.py','.md','.txt','.json','.ps1'):
            payload['worker/ALiS/'+rel.as_posix()] = path
    for name in ('setup_cpu_runtime.ps1','setup_cuda_runtime.ps1','verify_cuda_runtime.py'):
        payload['worker/ALiS/'+name] = ROOT/'tools'/name
    for path in (ROOT/'docs').glob('*.md'):
        payload['doc/ALiS/'+path.name] = path
    for name in ('LICENSE','THIRD_PARTY_NOTICES.md','TESTING_NOTICE.txt'):
        payload['doc/ALiS/'+name] = ROOT/name
    for folder in ('third_party/qCSF','third_party/qt'):
        for path in (ROOT/folder).rglob('*'):
            if path.is_file() and path.suffix.lower() in ('.md','.txt'):
                payload['doc/ALiS/'+path.relative_to(ROOT).as_posix()] = path
    manifest = [{'path':name,'bytes':path.stat().st_size,'sha256':sha(path)} for name,path in sorted(payload.items())]
    product = out/f'ALiS-{VERSION}-CloudCompare-2.13.2-Windows-x64.zip'
    instructions = r'''ALiS alpha.5.21 Windows plugin update (not a complete host/runtime installer).
Requires existing CloudCompare 2.13.2 x64 / Qt5. Save BIN and close CloudCompare.
Verify the published ZIP checksum, extract, run Install_ALiS.cmd, approve Windows.
For another host path, an administrator may run:
  .\Install_ALiS.ps1 -CloudCompareDir 'D:\Apps\CloudCompare'
The script verifies payload hashes, backs up replaced files, and rolls back on error.
Alternatively, IT may deploy the contents of payload into the compatible host root,
after checking PAYLOAD_SHA256.json and backing up existing files. Do not copy a DLL
to an incompatible Qt6 host. No datasets, pretrained models, Python or GPU drivers included.
Read payload/doc/ALiS/USER_GUIDE_IT.md and RELEASE_ALPHA521.md.
macOS update is the next roadmap step; this Windows ZIP is not a Mac installer.
'''
    with zipfile.ZipFile(product,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,path in sorted(payload.items()): archive.write(path,'payload/'+name)
        archive.writestr('PAYLOAD_SHA256.json',json.dumps(manifest,indent=2)+'\n')
        archive.writestr('Install_ALiS.ps1',INSTALL)
        archive.writestr('Install_ALiS.cmd','@echo off\r\npowershell.exe -NoProfile -Command "Start-Process powershell.exe -Verb RunAs -ArgumentList @(\'-NoProfile\',\'-ExecutionPolicy\',\'Bypass\',\'-File\',(\'\"\'+\'%~dp0Install_ALiS.ps1\'+\'\"\'))"\r\n')
        archive.writestr('INSTALL.txt',instructions)
    # Only repository source/doc assets; never add downloads, local build outputs,
    # user surveys, trained models or a Python environment to the source archive.
    names = subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines()
    source = out/f'ALiS-{VERSION}-Source.zip'
    roots = {'plugin','profiles','tools','third_party','docs','.github','scripts','packaging'}
    with zipfile.ZipFile(source,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name in sorted(set(names)):
            path = ROOT/name
            if not path.is_file(): continue
            rel = Path(name)
            if rel.parts[0] not in roots and len(rel.parts)!=1: continue
            if any(p in ('stage','__pycache__','build','output','.venv') for p in rel.parts): continue
            if path.suffix.lower() in ('.las','.laz','.bin','.joblib','.pyc','.pdb','.dll','.exe'): continue
            archive.write(path,name)
    products=[product,source]
    (out/'CHECKSUMS_SHA256.txt').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in products),encoding='ascii')
    (out/'VERSION.json').write_text(json.dumps({'version':VERSION,'tag':'v'+VERSION,'prerelease':True,
        'platform':'Windows x64 / CloudCompare 2.13.2 Qt5','macos_update':'next roadmap step; not included',
        'native_dll_sha256':sha(args.dll),'block_worker_sha256':sha(args.block_worker),
        'assets':[{'name':p.name,'bytes':p.stat().st_size,'sha256':sha(p)} for p in products],
        'external_source_assets':[{'name':'qtbase-everywhere-src-5.15.2.tar.xz','bytes':50179672,
            'sha256':'909fad2591ee367993a75d7e2ea50ad4db332f05e1c38dd7a5a274e156a4e0f8',
            'url':'https://download.qt.io/archive/qt/5.15/5.15.2/submodules/qtbase-everywhere-src-5.15.2.tar.xz'}]},indent=2)+'\n')
    for p in products: print(p.name,p.stat().st_size,sha(p))

if __name__ == '__main__': main()
