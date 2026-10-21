#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create the complete offline Apple Silicon PKG. Does not publish or install it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import urllib.request

REPO = Path(__file__).resolve().parents[1]
CONFIG = REPO / 'packaging/macos'
MAGICS = (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe')


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def macho(p):
    if p.is_file() and not p.is_symlink():
        with p.open('rb') as f:
            return f.read(4) in MAGICS
    return False


def dependencies(p):
    return [line.strip().split(' (')[0] for line in run('otool','-L',p).splitlines()[1:] if ' (compatibility version ' in line]


def runtime_audit(runtime, app):
    """Resolve wheel RPATHs locally. Fail if any dependency escapes the app."""
    report=[]
    for p in runtime.rglob('*'):
        if not macho(p): continue
        architectures=run('lipo','-archs',p).strip().split()
        if 'arm64' not in architectures: raise RuntimeError(f'No arm64 code: {p}')
        if len(architectures)>1:
            tmp=p.with_name(p.name+'.thin')
            run('lipo',p,'-thin','arm64','-output',tmp);os.replace(tmp,p)
        # Dylib ID may retain the wheel build-machine path; it is not a dependency.
        ids=run('otool','-D',p).splitlines()[1:]
        own_id=ids[0].strip() if ids else None
        import re
        rpaths=re.findall(r'cmd LC_RPATH\n\s*cmdsize \d+\n\s*path (.*?) \(offset',run('otool','-l',p))
        def expand(d):
            return Path(d.replace('@loader_path',str(p.parent)).replace('@executable_path',str(runtime/'bin')))
        for d in dependencies(p):
            if d==own_id: continue
            if d.startswith(('/System/','/usr/lib/')): continue
            if Path(d).name == 'libomp.dylib':
                candidates=[runtime/'lib/libomp.dylib']
            elif d.startswith('@rpath/'):
                candidates=[expand(r)/d[7:] for r in rpaths]+[runtime/'lib'/d[7:]]
            else: candidates=[expand(d)]
            candidates=[t.resolve() for t in candidates if t.exists() and t.resolve().is_relative_to(app)]
            if not candidates: raise RuntimeError(f'External/unresolved dependency: {p}: {d}')
            run('install_name_tool','-change',d,'@loader_path/'+os.path.relpath(candidates[0],p.parent),p)
        if own_id:run('install_name_tool','-id','@rpath/'+p.name,p)
        for r in set(rpaths):
            if r.startswith('/'):run('install_name_tool','-delete_rpath',r,p)
        report.append({'path':str(p.relative_to(app)), 'arch':'arm64','dependencies':dependencies(p)})
    (app/'Contents/Resources/ALiS/runtime-binary-audit.json').write_text(json.dumps(report,indent=2))
    return report


def prepare_runtime(cache):
    metadata=json.loads((CONFIG/'runtime-source.json').read_text())
    archive=cache/'python.tar.gz'
    if not archive.exists():urllib.request.urlretrieve(metadata['url'],archive)
    if hashlib.file_digest(archive.open('rb'),'sha256').hexdigest()!=metadata['sha256']:raise RuntimeError('Python archive checksum mismatch')
    root=cache/'fresh-runtime'
    if root.exists():shutil.rmtree(root)
    root.mkdir()
    with tarfile.open(archive) as t:t.extractall(root,filter='data')
    runtime=root/'python'
    run(runtime/'bin/python3','-m','pip','install','--only-binary=:all:','-r',CONFIG/'requirements.lock')
    run(runtime/'bin/python3','-m','pip','check')
    return runtime


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--app',type=Path,default=REPO/'output/macos/install/CloudCompare/CloudCompare.app')
    ap.add_argument('--runtime',type=Path,help='Already prepared PBS runtime, otherwise download pinned source')
    ap.add_argument('--output',type=Path,default=REPO/'output/mac-installer')
    args=ap.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    release=json.loads((CONFIG/'mac-release.json').read_text())
    stage=out/'stage'
    if stage.exists():shutil.rmtree(stage)
    app=stage/'Applications/ALiS.app';app.parent.mkdir(parents=True)
    run('ditto',args.app,app)
    resources=app/'Contents/Resources/ALiS'
    runtime=resources/'runtime'
    source=(args.runtime or prepare_runtime(out)).resolve()
    run(source/'bin/python3','-I','-B','-c',"import sys,platform; assert sys.version_info[:3]==(3,12,15) and platform.machine()=='arm64'")
    run('ditto',source,runtime)
    # XGBoost's Mac wheel expects an OpenMP runtime supplied by the distributor.
    shutil.copy2('/opt/homebrew/opt/libomp/lib/libomp.dylib', runtime/'lib/libomp.dylib')
    for extra_omp in (runtime/'lib/python3.12/site-packages').rglob('libomp.dylib'):extra_omp.unlink()
    # Do not ship bytecode or developer-only launch scripts with absolute shebangs.
    for p in runtime.rglob('__pycache__'):shutil.rmtree(p)
    for p in (runtime/'bin').iterdir():
        if p.name not in ('python3','python3.12'):p.unlink()
    for p in (runtime/'lib').glob('*.a'):p.unlink()
    if (runtime/'include').exists():shutil.rmtree(runtime/'include')
    shutil.copy2(CONFIG/'mac-release.json',resources/'mac-release.json')
    shutil.copy2(CONFIG/'requirements.lock',resources/'requirements.lock')
    shutil.copy2(CONFIG/'runtime-source.json',resources/'runtime-source.json')
    shutil.copy2(REPO/'docs/MAC_INSTALLER.md',resources/'INSTALLAZIONE.md')
    (resources/'MAC_INSTALL_LOCAL.md').unlink(missing_ok=True)
    for receipt in (resources/'licenses').rglob('INSTALL_RECEIPT.json'): receipt.unlink()
    # Runtime licences and wheel dist-info licences remain inside the app.
    run(runtime/'bin/python3','-I','-B','-m','pip','check')
    run(runtime/'bin/python3','-I','-B','-c',"import importlib.metadata as m; from pathlib import Path; rows=Path(__import__('sys').argv[1]).read_text().splitlines(); assert all(m.version(n)==v for n,v in (row.split('==') for row in rows))",CONFIG/'requirements.lock')
    runtime_audit(runtime,app)
    minimum=tuple((list(map(int, release['minimum_macos'].split('.')))+[0,0])[:3])
    deployment=[]
    for binary in app.rglob('*'):
        if not macho(binary):continue
        load_commands=run('otool','-l',binary)
        versions=re.findall(r'\bminos (\d+(?:\.\d+)*)',load_commands)+re.findall(r'cmd LC_VERSION_MIN_MACOSX\n\s*cmdsize \d+\n\s*version (\d+(?:\.\d+)*)',load_commands)
        for v in versions:
            if tuple((list(map(int,v.split('.')))+[0,0])[:3]) > minimum:raise RuntimeError(f'{binary} requires macOS {v}, beyond declared minimum')
        deployment.append({'path':str(binary.relative_to(app)),'minimum_macos':versions})
    (resources/'deployment-audit.json').write_text(json.dumps(deployment,indent=2))
    info=app/'Contents/Info.plist';plist=plistlib.loads(info.read_bytes())
    plist['CFBundleShortVersionString']=release['version'].split('-')[0]
    numbers=re.findall(r'\d+',release['version'])
    plist['CFBundleVersion']='.'.join(numbers[:2]+[str(int(''.join(numbers[2:]))*100+release['build'])])
    plist['LSMinimumSystemVersion']=release['minimum_macos']
    info.write_bytes(plistlib.dumps(plist))
    for p in app.rglob('*'):
        if macho(p):run('codesign','--force','--sign','-',p)
    for p in sorted(app.rglob('*.framework'),key=lambda p:len(p.parts),reverse=True):run('codesign','--force','--sign','-',p)
    run('codesign','--force','--sign','-',app)
    run('codesign','--verify','--deep','--strict',app)
    components=out/'components.plist'
    run('pkgbuild','--analyze','--root',stage,components)
    entries=plistlib.loads(components.read_bytes())
    for item in entries:
        item.update(BundleIsRelocatable=False,BundleHasStrictIdentifier=True,BundleIsVersionChecked=False,BundleOverwriteAction='upgrade')
    components.write_bytes(plistlib.dumps(entries))
    payload=out/'ALiS-component.pkg'
    run('pkgbuild','--root',stage,'--install-location','/','--identifier','org.alis.mac.install','--version',plist['CFBundleVersion'],'--component-plist',components,'--scripts',CONFIG/'scripts',payload)
    distribution=out/'Distribution.xml'
    distribution.write_text('''<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
<title>ALiS — Archaeological LiDAR Studio</title>
<organization>org.alis</organization>
<domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
<options customize="never" require-scripts="false" hostArchitectures="arm64"/>
<volume-check><allowed-os-versions><os-version min="'''+release['minimum_macos']+'''"/></allowed-os-versions></volume-check>
<welcome file="Welcome.html" mime-type="text/html"/>
<conclusion file="Conclusion.html" mime-type="text/html"/>
<license file="LICENSE.txt" mime-type="text/plain"/>
<choices-outline><line choice="default"/></choices-outline>
<choice id="default" visible="false"><pkg-ref id="org.alis.mac.install"/></choice>
<pkg-ref id="org.alis.mac.install" version="'''+plist['CFBundleVersion']+'''" onConclusion="none">ALiS-component.pkg</pkg-ref>
</installer-gui-script>''')
    installer_resources=out/'resources';installer_resources.mkdir(exist_ok=True)
    for name in ('Welcome.html','Conclusion.html'):shutil.copy2(CONFIG/name,installer_resources/name)
    shutil.copy2(REPO/'LICENSE',installer_resources/'LICENSE.txt')
    filename=f"ALiS-{release['version']}-macOS-arm64-Setup.pkg"
    pkg=out/filename
    run('productbuild','--distribution',distribution,'--package-path',out,'--resources',installer_resources,pkg)
    release.update(filename=filename,sha256=hashlib.file_digest(pkg.open('rb'),'sha256').hexdigest(),size=pkg.stat().st_size)
    (out/'ALiS-macOS-arm64.json').write_text(json.dumps(release,indent=2)+'\n')
    print(json.dumps({'package':str(pkg),'sha256':release['sha256'],'size':release['size'],'signing':'ad-hoc app; unsigned installer; not notarized'},indent=2))

if __name__=='__main__':main()
