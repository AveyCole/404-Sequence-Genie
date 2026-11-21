import hashlib
import json
import sys
from pathlib import Path

parent, target, output = map(Path, sys.argv[1:])
before, after = parent.read_bytes(), target.read_bytes()
segments = []
offset = 0
while offset < len(before):
    if before[offset] == after[offset]:
        offset += 1
        continue
    start = offset
    while offset < len(before) and before[offset] != after[offset]:
        offset += 1
    segments.append([start, after[start:offset].hex()])
segments.append([len(before), after[len(before):].hex()])
output.write_text(json.dumps({
    'input': hashlib.sha256(before).hexdigest(),
    'output': hashlib.sha256(after).hexdigest(),
    'size': len(after),
    'segments': segments,
}, separators=(',', ':')) + '\n', encoding='utf-8')
