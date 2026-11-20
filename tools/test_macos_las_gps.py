#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Round-trip synthetic GPS values through the actual Mac LAS importer/exporter.
Run Python with -B (especially a signed bundled runtime), with laspy/numpy,
and pass the CloudCompare executable.
No private survey data is required.
"""
import sys
sys.dont_write_bytecode = True
import os
from pathlib import Path
import subprocess
import tempfile
import laspy
import numpy as np

host = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='alis-gps-regression-') as folder:
    root = Path(folder)
    cases = {
        'no_data': np.array([-np.finfo(np.float32).max] * 3, dtype=np.float64),
        'normal': np.array([123456789.125, 123456790.25, 123456791.5]),
        'leading_zeros': np.array([0., 0., 1.]),
    }
    for name, times in cases.items():
        cloud = laspy.LasData(laspy.LasHeader(point_format=3, version='1.2'))
        cloud.x, cloud.y, cloud.z = [0, 1, 2], [2, 3, 4], [5, 6, 7]
        cloud.classification = [2, 4, 6]
        cloud.gps_time = times
        for field in ['Original cloud index', 'Original cloud index [2]']:
            cloud.add_extra_dim(laspy.ExtraBytesParams(name=field, type=np.float32))
            cloud[field] = [10, 11, 12]
        source, target = root / f'{name}.las', root / f'{name}_out.las'
        cloud.write(source)
        env = dict(os.environ, QT_QPA_PLATFORM='minimal')
        result = subprocess.run([str(host), '-SILENT', '-AUTO_SAVE', 'OFF',
                                 '-O', str(source), '-C_EXPORT_FMT', 'LAS',
                                 '-SAVE_CLOUDS', 'FILE', str(target)],
                                env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60, cwd=root)
        assert result.returncode == 0, result.stdout
        restored = laspy.read(target)
        np.testing.assert_array_equal(restored.gps_time, times)
        np.testing.assert_array_equal(restored.classification, cloud.classification)
        np.testing.assert_allclose(restored.xyz, cloud.xyz, rtol=0, atol=0.001)
        for field in cloud.point_format.extra_dimension_names:
            np.testing.assert_array_equal(restored[field], cloud[field])
        if name == 'no_data':
            assert 'outside the automatic shift range' in result.stdout, result.stdout
        print(f'PASS {name}: GPS, coordinates, labels and both extra fields preserved')
