#!/usr/bin/env python3
"""SPURS kernel workload selection (SPURS_RPCS3_SCHED) through the real cellSpurs.c, on a synthetic instance."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires POSIX C toolchain')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spurs-select-') as name:
    binary = Path(name) / 'fixture'
    linker = ['-Wl,-dead_strip'] if platform.system() == 'Darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT), '-I' + str(ROOT / 'include'),
                    '-I' + str(ROOT / 'runtime/spu'), str(ROOT / 'tests/test_spurs_select.c'),
                    str(ROOT / 'libs/spurs/spurs_taskset.c'),
                    *linker, '-pthread', '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10, cwd=name)
