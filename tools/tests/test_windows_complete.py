import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'packaging/windows-complete/bootstrap.py'
spec = importlib.util.spec_from_file_location('bootstrap', MODULE)
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


class CompleteSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
    def tearDown(self):
        self.temp.cleanup()
    def pe(self, path, machine=b'\x64\x86'):
        data = bytearray(512)
        data[:2] = b'MZ'
        data[0x3c:0x40] = struct.pack('<I', 128)
        data[128:134] = b'PE\0\0' + machine
        path.write_bytes(data)
    def host(self, version='v2.13.2', machine=b'\x64\x86'):
        p = self.root / 'host'
        p.mkdir()
        (p / 'CHANGELOG.md').write_text(version + '\n')
        self.pe(p / 'CloudCompare.exe', machine)
        self.pe(p / 'Qt5Core.dll', machine)
        return p
    def test_compatible_existing_host_reused(self):
        p = self.host()
        self.assertEqual(b.select_host([p], self.root/'new'), (p, False))
    def test_missing_host_installs_dedicated(self):
        target = self.root/'new'
        self.assertEqual(b.select_host([], target), (target, True))
    def test_other_version_not_overwritten(self):
        p = self.host('v2.14.0')
        with self.assertRaisesRegex(RuntimeError, 'nothing overwritten'):
            b.select_host([p], p)
    def test_no_version_prefix_false_positive(self):
        self.assertFalse(b.compatible_host(self.host('v2.13.20')))
    def test_no_x86(self):
        self.assertFalse(b.compatible_host(self.host(machine=b'\x4c\x01')))
    def test_no_qt6(self):
        p = self.host()
        (p/'Qt6Core.dll').touch()
        self.assertFalse(b.compatible_host(p))
    def test_no_conda(self):
        p = self.host()
        (p/'Qt5Core_conda.dll').touch()
        self.assertFalse(b.compatible_host(p))
    def test_corrupt_pe(self):
        p = self.root/'bad.exe'
        p.write_bytes(b'MZ')
        self.assertFalse(b.is_x64(p))
    def test_download_unapproved_url_rejected(self):
        with self.assertRaises(ValueError):
            b.download({'url':'http://github.com/x', 'filename':'x', 'sha256':'a'*64}, self.root)
    def test_download_path_traversal_rejected(self):
        with self.assertRaises(ValueError):
            b.download({'url':'https://github.com/x', 'filename':'../x', 'sha256':'a'*64}, self.root)
    def test_matching_cached_download_needs_no_network(self):
        p = self.root/'installer.exe'
        p.write_bytes(b'verified test')
        with patch.object(b.urllib.request, 'urlopen', side_effect=AssertionError('Network used')):
            self.assertEqual(b.download({'url':'https://github.com/x','filename':p.name,'sha256':b.digest(p)}, self.root), p)
    def test_manifest_hashes_and_python_bootstrap_match(self):
        m = json.loads((MODULE.parent/'dependencies.json').read_text())
        iss = (MODULE.parent/'CompleteSetup.iss').read_text(encoding='utf-8')
        for name in ('cloudcompare','python','alis'):
            self.assertRegex(m[name]['sha256'], r'^[0-9a-f]{64}$')
        for key in ('dll_sha256','worker_sha256'):
            self.assertRegex(m['alis'][key], r'^[0-9a-f]{64}$')
        self.assertIn(m['python']['sha256'], iss)
        self.assertIn(m['python']['url'], iss)
    def test_no_execution_policy_override(self):
        self.assertNotIn('ExecutionPolicy', MODULE.read_text())
    def test_complete_setup_version_supplied_by_build(self):
        script=(MODULE.parents[2]/'tools/build_windows_complete.py').read_text()
        iss=(MODULE.parent/'CompleteSetup.iss').read_text(encoding='utf-8')
        self.assertIn("'/DVersion='+version",script)
        self.assertIn('#ifndef Version',iss)
    def test_checksum_failure_never_runs_payload(self):
        import io
        response = io.BytesIO(b'wrong payload')
        response.headers = {'Content-Length':'13'}
        response.geturl = lambda:'https://github.com/x'
        with patch.object(b.urllib.request,'urlopen',return_value=response):
            with self.assertRaisesRegex(ValueError,'Checksum mismatch'):
                b.download({'url':'https://github.com/x','filename':'bad.exe','sha256':'a'*64},self.root)
        self.assertFalse((self.root/'bad.exe').exists())

if __name__ == '__main__':
    unittest.main()
