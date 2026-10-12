#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the official public release channel; download only verified Mac installers.
No credentials, telemetry, installation, elevation, or shell commands.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import ssl
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

REPO = 'Nicodemox87/ALiS'
API = 'https://api.github.com/repos/' + REPO + '/releases?per_page=100'
PREFIX = 'https://github.com/' + REPO + '/releases/download/'
MANIFEST = 'ALiS-macOS-arm64.json'
MAX_PKG = 2 * 1024**3


def version(value):
    m = re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?', value)
    if not m:
        raise ValueError('Invalid version')
    pre = tuple((0, int(p)) if p.isdigit() else (1, p) for p in (m[4] or '').split('.'))
    return (int(m[1]), int(m[2]), int(m[3]), 1 if m[4] is None else 0, pre)


def mac_version(value):
    if not re.fullmatch(r'\d+(?:\.\d+){0,2}', value):
        raise ValueError('Invalid macOS version')
    return tuple((list(map(int, value.split('.'))) + [0, 0])[:3])


def asset_url(url):
    if not isinstance(url, str) or not url.startswith(PREFIX):
        raise ValueError('Update is not an official ALiS release asset')
    p = urllib.parse.urlsplit(url)
    if p.query or p.fragment or len(p.path.split('/')) != 7:
        raise ValueError('Invalid release asset URL')
    return url


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        p = urllib.parse.urlsplit(newurl)
        if p.scheme != 'https' or p.port not in (None, 443) or p.username or p.password or p.hostname not in ('github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com'):
            raise ValueError('Unexpected update download redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def connection(url):
    # A pinned CA bundle is shipped with the portable runtime; no Homebrew paths.
    import certifi
    opener = urllib.request.build_opener(SafeRedirect(), urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=certifi.where())))
    return opener.open(urllib.request.Request(url, headers={'User-Agent': 'ALiS-macOS-updater', 'Accept': 'application/vnd.github+json' if url.startswith('https://api.github.com/') else 'application/octet-stream'}), timeout=20)


def get_json(url):
    with connection(url) as f:
        raw = f.read(4 * 1024**2 + 1)
    if len(raw) > 4 * 1024**2:
        raise ValueError('Release metadata too large')
    return json.loads(raw)


def validate_manifest(m, release):
    if m.get('schema') != 1 or m.get('arch') != 'arm64' or m.get('platform') != 'macOS':
        raise ValueError('Unsupported Mac release manifest')
    if m['version'] != release['tag_name'].removeprefix('v'):
        raise ValueError('Release tag and Mac version disagree')
    version(m['version']); mac_version(m['minimum_macos'])
    if type(m.get('build')) is not int or m['build'] < 1:
        raise ValueError('Invalid Mac build number')
    expected = 'ALiS-' + m['version'] + '-macOS-arm64-Setup.pkg'
    if m.get('filename') != expected or not re.fullmatch(r'[0-9a-f]{64}', m.get('sha256', '')):
        raise ValueError('Invalid package name or SHA256')
    if type(m.get('size')) is not int or not 0 < m['size'] <= MAX_PKG:
        raise ValueError('Invalid package size')
    assets = [a for a in release.get('assets', []) if a['name'] == expected]
    if len(assets) != 1 or assets[0]['size'] != m['size']:
        raise ValueError('Package missing or size mismatch')
    a = assets[0]
    if a.get('digest') and a['digest'] != 'sha256:' + m['sha256']:
        raise ValueError('GitHub digest does not match the release manifest')
    m = dict(m)
    m['url'] = asset_url(a['browser_download_url'])
    return m


def check(config, fetch=get_json, os_version=None, arch=None):
    if (arch or platform.machine()) != 'arm64':
        raise ValueError('This update channel is for Apple Silicon')
    current = (version(config['version']), config['build'])
    os_v = mac_version(os_version or platform.mac_ver()[0])
    releases = fetch(API)
    if not isinstance(releases, list):
        raise ValueError('GitHub did not return a release list')
    available = []
    incompatible = False
    found = False
    for release in releases:
        if release.get('draft') or (release.get('prerelease') and not config.get('include_prereleases', True)):
            continue
        assets = [a for a in release.get('assets', []) if a['name'] == MANIFEST]
        if not assets:
            continue
        found = True
        if len(assets) != 1:
            raise ValueError('Duplicate Mac manifests')
        m = validate_manifest(fetch(asset_url(assets[0]['browser_download_url'])), release)
        if (version(m['version']), m['build']) <= current:
            continue
        if mac_version(m['minimum_macos']) > os_v:
            incompatible = True
            continue
        available.append(m)
    if available:
        return {'status': 'available', 'release': max(available, key=lambda m: (version(m['version']), m['build']))}
    return {'status': 'incompatible' if incompatible else ('current' if found else 'no_mac_release')}


def download(m, directory, connect=connection):
    directory = Path(directory).expanduser()
    directory.mkdir(parents=True, exist_ok=True)
    # A private new directory avoids following existing files/symlinks in Downloads.
    dest_dir = Path(tempfile.mkdtemp(prefix='ALiS-update-', dir=directory))
    path = dest_dir / m['filename']
    partial = path.with_suffix('.pkg.part')
    try:
        digest = hashlib.sha256(); total = 0
        with connect(asset_url(m['url'])) as src, partial.open('xb') as dst:
            while block := src.read(1024**2):
                total += len(block)
                if total > m['size']:
                    raise ValueError('Downloaded package exceeds declared size')
                digest.update(block); dst.write(block)
        if total != m['size'] or digest.hexdigest() != m['sha256']:
            raise ValueError('Package integrity verification failed; download discarded')
        partial.rename(path)
        # Keep Gatekeeper in charge even though this was not downloaded by a browser.
        subprocess.run(['/usr/bin/xattr', '-w', 'com.apple.quarantine', '0083;00000000;ALiS;', str(path)], check=True, capture_output=True)
        return str(path)
    except Exception:
        partial.unlink(missing_ok=True); path.unlink(missing_ok=True); dest_dir.rmdir()
        raise


def main():
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=['check', 'download'])
    args = p.parse_args()
    try:
        config = json.loads((Path(__file__).resolve().parent.parent / 'mac-release.json').read_text())
        result = check(config)
        if args.action == 'download' and result['status'] == 'available':
            result['path'] = download(result['release'], Path.home() / 'Downloads')
        print(json.dumps(result))
    except Exception as e:
        print(json.dumps({'status': 'error', 'message': str(e)})); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
