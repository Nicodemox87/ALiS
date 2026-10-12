#!/usr/bin/env python3
"""Deploy, audit and ad-hoc sign the private native arm64 app (not notarization)."""
import argparse, json, os, plistlib, re, shutil, subprocess, struct
from pathlib import Path

def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)

def macho(p):
    return p.is_file() and not p.is_symlink() and p.open('rb').read(4) in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe')

def deps(p):
    return [x.strip().split(' (')[0] for x in run('otool','-L',p).splitlines()[1:]]

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('app',type=Path);ap.add_argument('--no-deploy',action='store_true');a=ap.parse_args()
    app=a.app.resolve();assert app.suffix=='.app' and (app/'Contents/MacOS/CloudCompare').exists()
    fw=app/'Contents/Frameworks'; mac=app/'Contents/MacOS'
    if not a.no_deploy:
        qt=run('brew','--prefix','qt@5').strip()
        extras=[mac/'ALiS_prepare_block',*sorted((app/'Contents/PlugIns/ccPlugins').glob('*.dylib'))]
        subprocess.run([qt+'/bin/macdeployqt',str(app),'-verbose=1','-always-overwrite',*['-executable='+str(p) for p in extras]],check=True)
    # Close dependencies, including non-Qt libraries used only by CC plugins.
    while True:
        copied=False
        for p in list(app.rglob('*')):
            if not macho(p):continue
            for d in deps(p):
                if d.startswith(('/System/','/usr/lib/')):continue
                if d.startswith('/'):
                    src=Path(d)
                    if '.framework/' in d:
                        relative=d[d.index('/',d.index('.framework'))+1:]
                        framework=d[:d.index('.framework')+len('.framework')]
                        target=fw/Path(framework).name/relative
                        if not target.exists():
                            subprocess.run(['ditto',framework,str(fw/Path(framework).name)],check=True);copied=True
                    else:
                        target=fw/src.name
                        if not target.exists():shutil.copy2(src,target);copied=True
                    subprocess.run(['install_name_tool','-change',d,'@loader_path/'+os.path.relpath(target,p.parent),str(p)],check=True)
                elif d.startswith('@rpath/'):
                    target=fw/d[len('@rpath/'):]
                    # Dylib's own install name is also printed by otool -L.
                    if target.exists():
                        subprocess.run(['install_name_tool','-change',d,'@loader_path/'+os.path.relpath(target,p.parent),str(p)],check=True)
                    elif Path(d).name!=p.name:
                        candidates={x.resolve() for x in Path('/opt/homebrew/opt').glob('*/lib/'+Path(d).name)}
                        if len(candidates)!=1:raise RuntimeError(f'Unresolved dependency: {p}: {d}: {candidates}')
                        shutil.copy2(candidates.pop(),target);copied=True
                        subprocess.run(['install_name_tool','-change',d,'@loader_path/'+os.path.relpath(target,p.parent),str(p)],check=True)
        if not copied:break
    binaries=[p for p in app.rglob('*') if macho(p)]
    audit=[]
    for p in binaries:
        with p.open('rb') as f: header=f.read(8)
        assert struct.unpack('<II',header)==(0xfeedfacf,0x100000c),(p,header)
        arch='arm64'
        ids=run('otool','-D',p).splitlines()[1:]
        if ids:subprocess.run(['install_name_tool','-id','@rpath/'+p.name,str(p)],check=True)
        for d in deps(p):
            if d.startswith(('/System/','/usr/lib/')):continue
            if d.startswith('@loader_path/'):
                target=(p.parent/d[len('@loader_path/'):]).resolve()
                assert target.is_relative_to(app) and target.exists(),(p,d)
            elif d.startswith('@executable_path/'):
                target=(mac/d[len('@executable_path/'):]).resolve()
                assert target.is_relative_to(app) and target.exists(),(p,d)
            elif d.startswith('@rpath/') and Path(d).name==p.name:pass
            else:raise RuntimeError(f'External dependency: {p}: {d}')
        # Remove build-tree and Homebrew search paths left by the linker.
        paths=re.findall(r'cmd LC_RPATH\n\s*cmdsize \d+\n\s*path (.*?) \(offset',run('otool','-l',p))
        for rp in set(paths):
            if rp.startswith('/'):
                subprocess.run(['install_name_tool','-delete_rpath',rp,str(p)],check=True)
        audit.append({'path':str(p.relative_to(app)),'arch':arch,'dependencies':deps(p)})
    repo=Path(__file__).resolve().parents[1]
    notices=app/'Contents/Resources/ALiS/licenses';notices.mkdir(parents=True,exist_ok=True)
    for name in ('LICENSE','THIRD_PARTY_NOTICES.md'):
        shutil.copy2(repo/name,notices/name)
    for f in (repo/'output/macos/CloudCompare-src').glob('*'):
        if f.is_file() and f.name.lower().startswith(('license','copying')):shutil.copy2(f,notices/('CloudCompare-'+f.name))
    for keg in Path('/opt/homebrew/Cellar').glob('*/*'):
        dest=notices/keg.parent.name;dest.mkdir(exist_ok=True)
        for f in keg.iterdir():
            if f.is_file() and (f.name.lower().startswith(('license','copying','notice','copyright')) or f.name=='INSTALL_RECEIPT.json'):shutil.copy2(f,dest/f.name)
    shutil.copy2(repo/'docs/MAC_INSTALL_LOCAL.md',app/'Contents/Resources/ALiS/MAC_INSTALL_LOCAL.md')
    info=app/'Contents/Info.plist'
    data=plistlib.loads(info.read_bytes());data['CFBundleIdentifier']='org.alis.CloudCompare';data['CFBundleName']='ALiS CloudCompare';data['CFBundleDisplayName']='ALiS CloudCompare';info.write_bytes(plistlib.dumps(data))
    report=app/'Contents/Resources/ALiS/native-bundle-audit.json';report.parent.mkdir(parents=True,exist_ok=True);report.write_text(json.dumps(audit,indent=2))
    for p in binaries:subprocess.run(['codesign','--force','--sign','-',str(p)],check=True,stdout=subprocess.DEVNULL)
    for p in sorted(app.rglob('*.framework'),key=lambda p:len(p.parts),reverse=True):
        subprocess.run(['codesign','--force','--sign','-',str(p)],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['codesign','--force','--sign','-',str(app)],check=True)
    subprocess.run(['codesign','--verify','--deep','--strict','--verbose=2',str(app)],check=True)
    print(f'PASS: {len(binaries)} native arm64 binaries; bundled dependency closure; ad-hoc signature. Not notarized.')
if __name__=='__main__':main()
