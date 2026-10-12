import hashlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
s=importlib.util.spec_from_file_location('update',Path(__file__).resolve().parents[2]/'plugin/worker/alis_mac_update.py')
u=importlib.util.module_from_spec(s);s.loader.exec_module(u)

class Updates(unittest.TestCase):
 def setUp(self):
  self.config={'version':'0.1.0-alpha.5.7','build':1,'include_prereleases':True}
  self.m={'schema':1,'version':'0.1.0-alpha.5.8','build':1,'arch':'arm64','platform':'macOS','minimum_macos':'15.0','filename':'ALiS-0.1.0-alpha.5.8-macOS-arm64-Setup.pkg','size':3,'sha256':hashlib.sha256(b'abc').hexdigest()}
  self.r={'tag_name':'v'+self.m['version'],'prerelease':True,'assets':[{'name':u.MANIFEST,'browser_download_url':u.PREFIX+'v'+self.m['version']+'/'+u.MANIFEST},{'name':self.m['filename'],'browser_download_url':u.PREFIX+'v'+self.m['version']+'/'+self.m['filename'],'size':3,'digest':'sha256:'+self.m['sha256']}]}
 def check(self):
  return u.check(self.config,lambda url:[self.r] if url==u.API else self.m,'15.0','arm64')
 def test_all_alpha_components_and_stable(self):
  self.assertGreater(u.version('0.1.0-alpha.5.10'),u.version('0.1.0-alpha.5.7'))
  self.assertGreater(u.version('0.1.0'),u.version('0.1.0-rc.99'))
 def test_update(self):self.assertEqual(self.check()['status'],'available')
 def test_old_release(self):
  self.config['version']='0.2.0';self.assertEqual(self.check()['status'],'current')
 def test_no_mac_assets(self):
  self.r['assets']=[];self.assertEqual(self.check()['status'],'no_mac_release')
 def test_build_revision(self):
  self.config['version']=self.m['version'];self.m['build']=2;self.assertEqual(self.check()['status'],'available')
 def test_minimum_os(self):
  self.m['minimum_macos']='27.1';self.assertEqual(self.check()['status'],'incompatible')
 def test_ignore_draft(self):
  self.r['draft']=True;self.assertEqual(self.check()['status'],'no_mac_release')
 def test_ignore_preview_when_disabled(self):
  self.config['include_prereleases']=False;self.assertEqual(self.check()['status'],'no_mac_release')
 def test_reject_wrong_repo(self):
  self.r['assets'][1]['browser_download_url']='https://github.com/attacker/ALiS/releases/download/v1/f.pkg'
  with self.assertRaises(ValueError):self.check()
 def test_digest_mismatch(self):
  self.r['assets'][1]['digest']='sha256:'+'0'*64
  with self.assertRaises(ValueError):self.check()
 def test_tag_mismatch(self):
  self.m['version']='0.9.0'
  with self.assertRaises(ValueError):self.check()
 def test_reject_wrong_platform(self):
  self.m['arch']='x86_64'
  with self.assertRaises(ValueError):self.check()
 def test_download_verified_and_quarantined(self):
  m=self.check()['release']
  with tempfile.TemporaryDirectory() as d, patch.object(u.subprocess,'run') as quarantine:
   p=Path(u.download(m,d,lambda url:io.BytesIO(b'abc')))
   self.assertEqual(p.read_bytes(),b'abc');quarantine.assert_called_once()
 def test_corrupt_download_removed(self):
  m=self.check()['release']
  with tempfile.TemporaryDirectory() as d:
   with self.assertRaises(ValueError):u.download(m,d,lambda url:io.BytesIO(b'bad'))
   self.assertEqual(list(Path(d).iterdir()),[])
 def test_excess_download_removed(self):
  m=self.check()['release']
  with tempfile.TemporaryDirectory() as d:
   with self.assertRaises(ValueError):u.download(m,d,lambda url:io.BytesIO(b'abcd'))
   self.assertEqual(list(Path(d).iterdir()),[])
 def test_http_error_not_up_to_date(self):
  def fail(url):raise OSError('offline')
  with self.assertRaises(OSError):u.check(self.config,fail,'15.0','arm64')

if __name__=='__main__':unittest.main()
