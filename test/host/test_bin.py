"""Checks the binary fetch format: decodes records.bin (written by ./test) with BattBench's decoder and compares it
with records.txt. Usage: python test_bin.py <BattBench checkout>"""
import sys
import zlib

sys.path.insert(0, sys.argv[1])
from battbench.device_net import decode_records  # noqa: E402

data = open('records.bin', 'rb').read()
d = decode_records(data)
want = [line.split() for line in open('records.txt')]
bad = 0
for r, w in zip(d['records'], want):
    if [str(r[0]), 'None' if r[1] is None else f'{r[1]:.1f}', str(r[2]), str(r[3]), r[4].hex()] != w:
        bad += 1
        if bad <= 5:
            print('differs:', r, w)
print(f"{len(d['records'])} records (expected {len(want)}), {bad} differ, timed {sum(r[1] is not None for r in d['records'])}")
print(f'{len(data)} bytes, deflated {len(zlib.compress(data, 4))}')
sys.exit(1 if bad or len(d['records']) != len(want) else 0)
