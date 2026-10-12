"""ALiS online Windows installer coordinator. No global pip/policy changes.

Runs in the original user's context; only native installers request elevation.
SHA-pinned downloads, separate venv, real readiness checks, activation last.
"""
import argparse
import csv
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parent
LOG = None


def say(message):
    print(message, flush=True)
    if LOG:
        LOG.write(message + '\n')
        LOG.flush()


def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def download(item, cache):
    url = item['url']
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != 'https' or parsed.hostname not in ('www.cloudcompare.org', 'www.python.org', 'github.com'):
        raise ValueError('Unapproved download origin')
    if Path(item['filename']).name != item['filename'] or not re.fullmatch('[0-9a-f]{64}', item['sha256']):
        raise ValueError('Invalid dependency manifest')
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / item['filename']
    if target.is_file() and digest(target) == item['sha256']:
        say('Verified cached download: ' + target.name)
        return target
    temp = target.with_suffix(target.suffix + '.partial')
    request = urllib.request.Request(url, headers={'User-Agent': 'ALiS-Complete-Setup/1'})
    say('Downloading ' + target.name)
    with urllib.request.urlopen(request, timeout=60) as response, temp.open('wb') as output:
        if urllib.parse.urlparse(response.geturl()).scheme != 'https':
            raise ValueError('Insecure redirect rejected')
        total = int(response.headers.get('Content-Length', 0))
        count, last = 0, 0.0
        while data := response.read(1024 * 1024):
            output.write(data)
            count += len(data)
            if time.monotonic() - last > 3:
                say(f'Download: {count // 1048576} MiB' + (f' / {total // 1048576} MiB' if total else ''))
                last = time.monotonic()
    if digest(temp) != item['sha256']:
        raise ValueError('Checksum mismatch. File will NOT run: ' + target.name)
    temp.replace(target)
    return target


def is_x64(path):
    try:
        with Path(path).open('rb') as f:
            if f.read(2) != b'MZ':
                return False
            f.seek(0x3c)
            offset = struct.unpack('<I', f.read(4))[0]
            f.seek(offset)
            return f.read(6) == b'PE\0\0\x64\x86'
    except (OSError, struct.error):
        return False


def compatible_host(folder):
    folder = Path(folder)
    try:
        lines = (folder / 'CHANGELOG.md').read_text(encoding='utf-8-sig').splitlines()[:10]
        return (any(re.match(r'^v2\.13\.2\b', s) for s in lines)
                and is_x64(folder / 'CloudCompare.exe') and is_x64(folder / 'Qt5Core.dll')
                and not (folder / 'Qt6Core.dll').exists()
                and not (folder / 'Qt5Core_conda.dll').exists())
    except (OSError, UnicodeError):
        return False


def host_candidates():
    import winreg
    paths = []
    for env in ('ProgramW6432', 'ProgramFiles', 'LOCALAPPDATA'):
        if os.environ.get(env):
            paths.extend([Path(os.environ[env]) / 'CloudCompare',
                          Path(os.environ[env]) / 'CloudCompare-2.13.2-ALiS'])
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                with winreg.OpenKey(hive, r'Software\Microsoft\Windows\CurrentVersion\Uninstall', 0,
                                    winreg.KEY_READ | view) as root:
                    for index in range(winreg.QueryInfoKey(root)[0]):
                        try:
                            with winreg.OpenKey(root, winreg.EnumKey(root, index)) as key:
                                name = winreg.QueryValueEx(key, 'DisplayName')[0]
                                if 'cloudcompare' in str(name).lower():
                                    paths.append(Path(winreg.QueryValueEx(key, 'InstallLocation')[0]))
                        except OSError:
                            continue
            except OSError:
                continue
    return list(dict.fromkeys(paths))


def select_host(candidates, destination):
    for path in candidates:
        if compatible_host(path):
            return Path(path), False
    destination = Path(destination)
    if destination.exists() and any(destination.iterdir()):
        raise RuntimeError('Dedicated host folder is non-empty and incompatible; nothing overwritten: ' + str(destination))
    return destination, True


def cloudcompare_running():
    result = subprocess.run(['tasklist.exe', '/FI', 'IMAGENAME eq CloudCompare.exe', '/FO', 'CSV', '/NH'],
                            capture_output=True, text=True, check=True, creationflags=0x08000000)
    return any(row and row[0].casefold() == 'cloudcompare.exe' for row in csv.reader(result.stdout.splitlines()))


def run(command, env=None):
    say('Running: ' + subprocess.list2cmdline([str(x) for x in command]))
    with subprocess.Popen([str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding='utf-8', errors='replace', env=env,
                          creationflags=0x08000000 if os.name == 'nt' else 0) as proc:
        for line in proc.stdout:
            say(line.rstrip())
        code = proc.wait()
    if code:
        raise RuntimeError(f'Command failed (exit {code}); see the installer log.')


def elevated_installer(executable, args):
    class ShellExecuteInfo(ctypes.Structure):
        _fields_ = [('cbSize', wintypes.DWORD), ('fMask', wintypes.ULONG), ('hwnd', wintypes.HWND),
                    ('lpVerb', wintypes.LPCWSTR), ('lpFile', wintypes.LPCWSTR),
                    ('lpParameters', wintypes.LPCWSTR), ('lpDirectory', wintypes.LPCWSTR),
                    ('nShow', ctypes.c_int), ('hInstApp', wintypes.HINSTANCE), ('lpIDList', ctypes.c_void_p),
                    ('lpClass', wintypes.LPCWSTR), ('hkeyClass', wintypes.HKEY), ('dwHotKey', wintypes.DWORD),
                    ('hIcon', wintypes.HANDLE), ('hProcess', wintypes.HANDLE)]
    info = ShellExecuteInfo()
    info.cbSize, info.fMask, info.lpVerb = ctypes.sizeof(info), 0x40, 'runas'
    info.lpFile, info.lpParameters, info.nShow = str(executable), subprocess.list2cmdline(args), 1
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    shell.ShellExecuteExW.argtypes = [ctypes.POINTER(ShellExecuteInfo)]
    shell.ShellExecuteExW.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    if not shell.ShellExecuteExW(ctypes.byref(info)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        while kernel.WaitForSingleObject(info.hProcess, 5000) == 258:
            say('Installing native component; confirm any Windows administrator prompt...')
        code = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(info.hProcess, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        if code.value == 3010:
            raise RuntimeError('Restart Windows, then run the complete installer again (restart required).')
        if code.value:
            raise RuntimeError(f'Native installer exited with {code.value}; setup is not complete.')
    finally:
        kernel.CloseHandle(info.hProcess)


def native_check(host):
    host = Path(host)
    if not compatible_host(host):
        raise RuntimeError('Incompatible CloudCompare / Qt / architecture')
    with os.add_dll_directory(str(host)):
        handles = [ctypes.WinDLL(str(host / f)) for f in
                   ('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'CCCoreLib.dll',
                    'QCC_DB_LIB.dll', 'QCC_GL_LIB.dll', 'CCPluginAPI.dll')]
        handles.append(ctypes.WinDLL(str(host / 'plugins/ALIS_PLUGIN.dll')))
        say('Native ALiS plugin load: PASS')


def activate_runtime(python):
    import winreg
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, 'Environment') as key:
        try:
            old = winreg.QueryValueEx(key, 'ALIS_PYTHON')[0]
        except FileNotFoundError:
            old = None
        winreg.SetValueEx(key, 'ALIS_PYTHON', 0, winreg.REG_SZ, str(python))
    os.environ['ALIS_PYTHON'] = str(python)
    user32 = ctypes.WinDLL('user32')
    user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t,
                                         wintypes.LPCWSTR, wintypes.UINT, wintypes.UINT, ctypes.c_void_p]
    user32.SendMessageTimeoutW(0xffff, 0x1a, 0, 'Environment', 2, 5000, None)
    return old


def install(args):
    manifest = json.loads((HERE / 'dependencies.json').read_text(encoding='utf-8'))
    if sys.platform != 'win32' or struct.calcsize('P') != 8 or sys.version_info[:2] != (3, 12):
        raise RuntimeError('Windows x64 and Python 3.12 are required.')
    candidates = [Path(args.host)] if args.host else host_candidates()
    default_host = Path(os.environ.get('ProgramW6432', os.environ['ProgramFiles'])) / 'CloudCompare-2.13.2-ALiS'
    host, needs_host = select_host(candidates, Path(args.host) if args.host else default_host)
    say('STEP 1/5: CloudCompare ' + ('will be installed at ' if needs_host else 'compatible installation found: ') + str(host))
    if args.plan:
        say(json.dumps({'host': str(host), 'install_host': needs_host, 'backend': args.backend}))
        return
    if cloudcompare_running():
        raise RuntimeError('Save your work and close ALL CloudCompare windows, then retry.')
    required = 12 if args.backend == 'cuda' else 5
    if shutil.disk_usage(args.cache.anchor).free < required * 1024**3:
        raise RuntimeError(f'At least {required} GiB free on the user-data drive is required.')
    if needs_host:
        setup = download(manifest['cloudcompare'], args.cache)
        elevated_installer(setup, ['/SP-', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
                                  '/RESTARTEXITCODE=3010', '/DIR=' + str(host), '/LOG=' + str(args.logs / 'cloudcompare.log')])
    if not compatible_host(host):
        raise RuntimeError('CloudCompare compatibility verification failed. ALiS was not installed.')
    say('STEP 2/5: Install ALiS plugin and native block worker')
    setup = download(manifest['alis'], args.cache)
    elevated_installer(setup, ['/SP-', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
                              '/RESTARTEXITCODE=3010', '/DIR=' + str(host), '/TASKS=desktopicon',
                              '/LOG=' + str(args.logs / 'alis-plugin.log')])
    for filename, key in [('plugins/ALIS_PLUGIN.dll', 'dll_sha256'), ('ALiS_prepare_block.exe', 'worker_sha256')]:
        if digest(host / filename) != manifest['alis'][key]:
            raise RuntimeError('Installed native file hash mismatch: ' + filename)
    run([sys.executable, HERE / 'bootstrap.py', '--native-check', host])
    say('STEP 3/5: Create isolated ML + deep-learning runtime (' + args.backend + ')')
    runtime = Path.home() / '.alis' / ('runtime-complete-r1-' + args.backend)
    python = runtime / 'Scripts/python.exe'
    if runtime.exists() and not (runtime / 'pyvenv.cfg').is_file():
        raise RuntimeError('Refusing to change a non-venv directory: ' + str(runtime))
    if not python.exists():
        run([sys.executable, '-m', 'venv', runtime])
    env = os.environ.copy()
    env.update(PIP_DISABLE_PIP_VERSION_CHECK='1', PYTHONUTF8='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    env.pop('PYTHONHOME', None)
    pip = [python, '-m', 'pip', '--isolated', 'install', '--only-binary=:all:']
    run(pip + ['--index-url', 'https://pypi.org/simple'] + manifest['packages'], env)
    index = 'https://download.pytorch.org/whl/' + ('cu128' if args.backend == 'cuda' else 'cpu')
    run(pip + ['--index-url', index, manifest['torch'][args.backend]], env)
    say('STEP 4/5: Verify ML, PyTorch, LAS/LAZ, dependencies and worker')
    run([python, '-m', 'pip', '--isolated', 'check'], env)
    run([python, HERE / 'verify_runtime.py', '--backend', args.backend], env)
    worker = host / 'worker/ALiS/qal_ml_worker.py'
    run([python, worker, 'capabilities'], env)
    freeze = subprocess.check_output([str(python), '-m', 'pip', '--isolated', 'freeze'], text=True, env=env)
    (args.logs / 'requirements-installed.txt').write_text(freeze, encoding='utf-8')
    say('STEP 5/5: Activate the verified runtime for the current Windows user')
    previous = activate_runtime(python)
    report = dict(schema='alis-complete-install/1', success=True, backend=args.backend, host=str(host),
                  python=str(python), previous_alis_python=previous, installed_host=needs_host,
                  packages=manifest['packages'], torch=manifest['torch'][args.backend])
    (args.logs / 'SUCCESS.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    say('SUCCESS: ALiS + ML + deep learning are ready. Restart CloudCompare and its launching terminal.')
    say('Logs: ' + str(args.logs))


def main():
    global LOG
    parser = argparse.ArgumentParser()
    parser.add_argument('--backend', choices=['cpu', 'cuda'], default='cpu')
    parser.add_argument('--host')
    parser.add_argument('--plan', action='store_true')
    parser.add_argument('--native-check', type=Path)
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--logs', type=Path)
    args = parser.parse_args()
    if args.native_check:
        native_check(args.native_check)
        return 0
    if not args.cache or not args.logs:
        parser.error('--cache and --logs are required')
    args.cache = args.cache.resolve()
    args.logs.mkdir(parents=True, exist_ok=True)
    with (args.logs / 'complete-setup.log').open('a', encoding='utf-8') as log:
        LOG = log
        try:
            install(args)
            return 0
        except Exception as error:
            say('FAILED: ' + str(error))
            say('Installation is incomplete. Existing data and earlier runtimes were not deleted. Logs: ' + str(args.logs))
            (args.logs / 'FAILURE.txt').write_text(str(error), encoding='utf-8')
            return 1


if __name__ == '__main__':
    raise SystemExit(main())
