import subprocess
import unittest
from unittest.mock import patch
from qal_ml import preparation as p

class PlatformMemoryTests(unittest.TestCase):
    def test_darwin_pages(self):
        output='Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free: 100.\nPages inactive: 200.\n'
        with patch.object(p.os,'name','posix'),patch.object(p.sys,'platform','darwin'),patch.object(p.subprocess,'run',return_value=subprocess.CompletedProcess([],0,output,'')):
            self.assertEqual(p.available_memory(),300*16384)
    def test_darwin_fallback(self):
        with patch.object(p.os,'name','posix'),patch.object(p.sys,'platform','darwin'),patch.object(p.subprocess,'run',side_effect=OSError('unavailable')):
            self.assertEqual(p.available_memory(),2*1024**3)

if __name__=='__main__':unittest.main()
