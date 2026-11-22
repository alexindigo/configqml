#!/usr/bin/env python3
"""Count actual full-layer resolutions, independently of noisy elapsed timings."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess

from harness import guest_environment, run, write_json

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
source, build, output = a.source.resolve(), a.build.resolve(), a.output.resolve()
output.mkdir(exist_ok=False, parents=True)
env = guest_environment()
flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'Qt6Core'],
                       check=True, text=True, capture_output=True).stdout
shim = output/'source-cut-counter.so'
run(output, 'counter-build', ['c++', '-std=c++17', '-fPIC', '-shared',
    '-I'+str(source), source/'tests/generic_primitives/source_cut_counter.cpp',
    '-o', shim, *shlex.split(flags), '-ldl'], env=env)
env['LD_PRELOAD'] = str(shim)
env['QT_FORCE_STDERR_LOGGING'] = '1'
text = run(output, 'source-cuts', [build/'test_json_files',
           'shardsNoEchoFanoutAndGuiBurst'], env=env)
counts = re.findall(r'JSON_SOURCE_CUTS publications=(\d+) events=(\d+)', text)
resolutions = re.findall(r'SOURCE_CUT_RESOLUTIONS (\d+)', text)
if len(counts) != 1 or len(resolutions) != 1:
    raise RuntimeError('missing or ambiguous independent cut/work counts')
publications, events = map(int, counts[0])
actual = int(resolutions[0])
# The worker's final retire packet has no living facade subscriber.
expected = publications + events + 1
write_json(output/'work.json', dict(publications=publications, events=events,
           resolutions=actual, expected=expected))
if actual != expected:
    raise RuntimeError(f'full-tree resolutions {actual}, one per cut requires {expected}')
print('GENERIC_SOURCE_CUT_WORK_OK', flush=True)
