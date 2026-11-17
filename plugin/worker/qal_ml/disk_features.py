"""Stream CC snapshots + sorted core journals to one LAS, without N x K RAM.

No model, labels inferred, decimation or halo in the final output. Pending rows
remain NaN. Integrity is checked before the staged output replaces its target.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
from collections import OrderedDict
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile

import numpy as np


def emit(phase, done, total, message):
    print(json.dumps(dict(phase=phase, done=done, total=total, message=message)), flush=True)


class Handles:
    def __init__(self):
        self.files = OrderedDict()

    def read(self, path, offset, size):
        f = self.files.pop(path, None)
        if f is None:
            f = path.open('rb')
        self.files[path] = f
        if len(self.files) > 64:
            self.files.popitem(last=False)[1].close()
        f.seek(offset)
        data = f.read(size)
        if len(data) != size:
            raise ValueError('Truncated checkpoint: ' + str(path))
        return data

    def close(self):
        for f in self.files.values():
            f.close()


class Patch:
    def __init__(self, entry, ledger, handles, batch):
        self.path = Path(entry['checkpoint'])
        self.handles = handles
        self.expected = entry.get('sha256', '')
        if not re.fullmatch(r'[0-9a-f]{64}', self.expected):
            raise ValueError('Missing checkpoint integrity hash')
        first = handles.read(self.path, 0, 11)
        if first[:7] != b'ALISF02':
            raise ValueError('Invalid checkpoint magic')
        length = struct.unpack('<I', first[7:])[0]
        if not 0 < length <= 4 * 1024**2:
            raise ValueError('Invalid checkpoint metadata size')
        rest = handles.read(self.path, 11, length + 8)
        meta = json.loads(rest[:length])
        self.rows, columns = struct.unpack('<II', rest[length:])
        if (meta.get('identity') != ledger['identity'] or
                meta.get('features') != ledger['feature_keys'] or
                self.rows != entry['points'] or columns != len(ledger['feature_keys'])):
            raise ValueError('Checkpoint identity/schema/count mismatch')
        self.dtype = np.dtype([('id', '<u4'), ('values', '<f4', (columns,))])
        self.offset = 11 + len(rest)
        if self.path.stat().st_size != self.offset + self.rows * self.dtype.itemsize:
            raise ValueError('Checkpoint size mismatch')
        self.hash = hashlib.sha256(first + rest)
        self.remaining = self.rows
        self.batch = batch
        self.buffer = np.empty(0, dtype=self.dtype)
        self.position = 0
        self.last_id = -1
        self.n = int(ledger['point_count'])

    def load(self):
        count = min(self.remaining, self.batch)
        raw = self.handles.read(self.path, self.offset, count * self.dtype.itemsize)
        self.hash.update(raw)
        self.offset += len(raw)
        self.remaining -= count
        self.buffer = np.frombuffer(raw, dtype=self.dtype)
        self.position = 0
        ids = self.buffer['id']
        if len(ids) and (int(ids[0]) <= self.last_id or int(ids[-1]) >= self.n or
                         np.any(ids[1:] <= ids[:-1])):
            raise ValueError('Invalid, unordered or duplicated checkpoint row IDs')
        if len(ids):
            self.last_id = int(ids[-1])
        if np.isinf(self.buffer['values']).any():
            raise ValueError('Infinite feature values')

    def scatter(self, first, end, features, covered):
        while True:
            if self.position == len(self.buffer):
                if not self.remaining:
                    return
                self.load()
            rows = self.buffer[self.position:]
            if int(rows['id'][0]) >= end:
                return
            count = int(np.searchsorted(rows['id'], end))
            chosen = rows[:count]
            if int(chosen['id'][0]) < first:
                raise ValueError('Point ordering changed')
            ids = chosen['id'].astype('i8') - first
            if covered[ids].any():
                raise ValueError('Overlapping core ownership; export refused')
            covered[ids] = True
            features[ids] = chosen['values']
            self.position += count

    def finish(self):
        if self.remaining or self.position != len(self.buffer) or self.hash.hexdigest() != self.expected:
            raise ValueError('Checkpoint integrity failure; existing output retained')


def export(snapshot, ledger_path, output, chunk_limit=16384):
    import laspy
    from laspy.vlrs.vlrlist import VLRList
    snapshot = Path(snapshot)
    ledger_path = Path(ledger_path)
    output = Path(output)
    meta = json.loads(snapshot.read_text(encoding='utf-8'))
    ledger = json.loads(ledger_path.read_text(encoding='utf-8'))
    if meta.get('schema') != 'alis-disk-feature-snapshot/1' or not ledger.get('disk_only'):
        raise ValueError('Unsupported disk feature job')
    n = int(meta['point_count'])
    keys = ledger['feature_keys']
    columns = meta['columns']
    if not n or n != ledger['point_count'] or not keys or len(keys) != len(set(keys)):
        raise ValueError('Snapshot/schema mismatch')
    if meta['coordinate_sha256'] != ledger['config']['coordinate_sha256']:
        raise ValueError('Snapshot coordinate/order identity mismatch')
    entries = list(ledger['blocks'].values())
    if not entries:
        raise ValueError('No completed feature blocks')
    source = snapshot.with_name('export_snapshot.bin')
    dtype = np.dtype([('xyz', '<f8', (3,)), ('attrs', '<f4', (len(columns),))])
    if source.stat().st_size != n * dtype.itemsize:
        raise ValueError('Snapshot size mismatch')
    if output.suffix.lower() != '.las':
        raise ValueError('Choose .las for the bounded uncompressed export')
    if output.resolve() in (source.resolve(), snapshot.resolve(), ledger_path.resolve()):
        raise ValueError('Output would overwrite input')
    header = laspy.LasHeader(point_format=7, version='1.4')
    lo, hi = np.asarray(meta['mins']), np.asarray(meta['maxs'])
    if not np.isfinite(lo).all() or not np.isfinite(hi).all():
        raise ValueError('Nonfinite cloud bounds')
    header.offsets = lo
    header.scales = np.maximum(0.0001, (hi - lo) / 2_000_000_000.)
    projection = meta.get('cc_metadata', {}).get('LAS.projection', '')
    prefix = 'Coordinate System WKT: '
    if isinstance(projection, str) and projection.startswith(prefix):
        from laspy.vlrs.known import WktCoordinateSystemVlr
        header.vlrs.append(WktCoordinateSystemVlr(projection[len(prefix):].rstrip('\x00')))
        header.global_encoding.wkt = True
    names = {name.casefold() for name in header.point_format.dimension_names}
    original = {}
    feature_map = {}

    def name_for(label, prefix, index):
        candidate = label
        if (not candidate.isascii() or len(candidate.encode('ascii')) > 32 or
                not candidate or candidate.casefold() in names or '\x00' in candidate):
            candidate = f'ALiS_{prefix}{index:05d}'
            if prefix == 'F' and '@' in label:
                feature, radius = label.rsplit('@', 1)
                candidate = (feature[:19] + '_r' + format(float(radius), '.6g'))[:32]
        suffix = 0
        base = candidate
        while candidate.casefold() in names:
            suffix += 1
            candidate = base[:20] + '_' + str(suffix)
        names.add(candidate.casefold())
        return candidate

    params = []
    for i, column in enumerate(columns):
        if column['kind'] != 'rgb':
            alias = name_for(column['name'], 'S', i)
            original[str(i)] = alias
            params.append(laspy.ExtraBytesParams(name=alias, type=np.float32))
    for i, key in enumerate(keys):
        alias = name_for(key, 'F', i)
        feature_map[key] = alias
        params.append(laspy.ExtraBytesParams(name=alias, type=np.float32))
    header.add_extra_dims(params)
    description = dict(schema='alis-disk-feature-export/1', features=feature_map,
                       original_columns=columns, original_fields=original,
                       rgb_encoding='CC byte multiplied by 257' if {c['name'] for c in columns if c['kind']=='rgb'}=={'red','green','blue'} else None,
                       coordinate_sha256=meta['coordinate_sha256'], identity=ledger['identity'],
                       warning='Current CC attributes only. No missing source LAS attributes or CRS are invented.')
    record = laspy.VLR(user_id='ALiS', record_id=1, description='ALiS feature field mapping',
                       record_data=json.dumps(description, ensure_ascii=False).encode('utf-8'))
    extended = VLRList()
    if len(record.record_data) <= 65535:
        header.vlrs.append(record)
    else:
        extended.append(record)
    output.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(output.parent).free < n * header.point_format.size + 512 * 1024**2:
        raise OSError('Not enough free disk space for the unified LAS')
    batch = max(1, min(256, (64 * 1024**2) // (len(entries) * (4 + 4 * len(keys)))))
    chunk = max(1, min(chunk_limit, (32 * 1024**2) // (dtype.itemsize + 4 * len(keys) + header.point_format.size)))
    handles = Handles()
    readers = []
    coordinate_hash = hashlib.sha256()
    completed = 0
    rgb = {c['name']: i for i, c in enumerate(columns) if c['kind'] == 'rgb'}
    labels = next((i for label in ('qAL_ASPRS_Working', 'Classification', 'qAL_ASPRS_Original')
                   for i, c in enumerate(columns) if c['kind'] == 'sf' and c['name'] == label), None)
    native = {}
    limits = dict(intensity=65535, return_number=15, number_of_returns=15,
                  point_source_id=65535, user_data=255, scan_direction_flag=1, edge_of_flight_line=1)
    for i, c in enumerate(columns):
        name = re.sub('[^a-z0-9]', '', c['name'].lower())
        for field in limits:
            if c['kind'] == 'sf' and name == field.replace('_', ''):
                native[field] = i
    try:
        readers = [Patch(entry, ledger, handles, batch) for entry in entries]
        # A unique staging directory is on the target volume: final replace is atomic.
        with tempfile.TemporaryDirectory(prefix='.alis-features-', dir=output.parent) as staging:
            partial = Path(staging) / 'features.las'
            with source.open('rb') as src, laspy.open(partial, mode='w', header=header) as dst:
                for first in range(0, n, chunk):
                    count = min(chunk, n - first)
                    rows = np.fromfile(src, dtype=dtype, count=count)
                    if len(rows) != count or not np.isfinite(rows['xyz']).all():
                        raise ValueError('Invalid/truncated source snapshot')
                    coordinate_hash.update(np.ascontiguousarray(rows['xyz']).tobytes())
                    features = np.full((count, len(keys)), np.nan, dtype='f4')
                    covered = np.zeros(count, dtype=bool)
                    for reader in readers:
                        reader.scatter(first, first + count, features, covered)
                    completed += int(covered.sum())
                    points = laspy.ScaleAwarePointRecord.zeros(count, header=header)
                    points.x, points.y, points.z = rows['xyz'].T
                    for index, alias in original.items():
                        points[alias] = rows['attrs'][:, int(index)]
                    for i, alias in enumerate(feature_map.values()):
                        points[alias] = features[:, i]
                    for field, index in native.items():
                        values = rows['attrs'][:, index]
                        valid = np.isfinite(values)
                        if np.any((values[valid] < 0) | (values[valid] > limits[field]) | (values[valid] != np.rint(values[valid]))):
                            raise ValueError('Invalid native LAS attribute: ' + field)
                        points[field] = np.where(valid, values, 0).astype('u2')
                    if labels is not None:
                        values = rows['attrs'][:, labels]
                        valid = np.isfinite(values)
                        if np.any((values[valid] < 0) | (values[valid] > 255) | (values[valid] != np.rint(values[valid]))):
                            raise ValueError('Working ASPRS labels cannot be represented without truncation')
                        points.classification = np.where(valid, values, 0).astype('u1')
                    for channel, index in rgb.items():
                        points[channel] = np.rint(rows['attrs'][:, index] * 257).astype('u2')
                    dst.write_points(points)
                    emit('export LAS', first + count, n, 'All points retained; pending feature rows are NaN')
                for reader in readers:
                    reader.finish()
                if coordinate_hash.hexdigest() != meta['coordinate_sha256']:
                    raise ValueError('Source geometry/order hash changed; existing output retained')
                if completed != sum(int(e['points']) for e in entries):
                    raise ValueError('Core coverage count mismatch')
                if extended:
                    dst.write_evlrs(extended)
            os.replace(partial, output)
        report = dict(description, point_count=n, completed_feature_points=completed,
                      pending_feature_points=n-completed, classification_source=columns[labels]['name'] if labels is not None else None,
                      coordinate_scales=header.scales.tolist(), output=str(output), chunk_rows=chunk,
                      reader_buffer_budget_bytes=64*1024**2, feature_matrix_budget_bytes=32*1024**2)
        report_path = output.with_suffix('.features.json')
        # This sidecar also preserves CC metadata that cannot be represented as LAS fields.
        report['cc_metadata'] = meta.get('cc_metadata', {})
        temp_report = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=output.parent, delete=False) as f:
                temp_report = f.name
                json.dump(report, f, indent=2, ensure_ascii=False, allow_nan=False)
            os.replace(temp_report, report_path)
        except (OSError, ValueError, TypeError) as error:
            # The LAS itself is already fully verified and committed; never report
            # that it was rolled back just because a separate sidecar is locked.
            if temp_report:
                Path(temp_report).unlink(missing_ok=True)
            emit('warning', n, n, 'Verified LAS saved, but sidecar could not be updated: '+str(error))
            report['sidecar_warning'] = str(error)
        emit('complete', n, n, str(output))
        return report
    finally:
        handles.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--snapshot', required=True, type=Path)
    parser.add_argument('--ledger', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        export(args.snapshot, args.ledger, args.output)
        return 0
    except Exception as e:
        emit('error', 0, 1, str(e))
        return 1
