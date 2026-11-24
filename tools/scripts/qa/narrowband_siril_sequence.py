#!/usr/bin/env python3
"""Run a read-only Ha/OIII sequence comparison using isolated Siril copies/config.
Requires Siril 1.4+, PhotonStack, NumPy, SciPy and Astropy. Does not infer the
filter response or replace calibrated-data/SPCC acceptance. Output must be new.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time


def sha(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'output', 'siril', 'siril-config', 'photonstack'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--siril-scale', type=float, required=True,
                        help='Declared conversion to physical counts; 65535 for normalized unsigned 16-bit data')
    args = parser.parse_args()
    source, root = args.source.resolve(), args.output.resolve()
    if source == root or source in root.parents:
        parser.error('Place QA output outside the read-only source directory')
    files = sorted(p for p in source.iterdir() if p.is_file() and p.suffix.lower() in ('.fit', '.fits', '.fts'))
    if not 3 <= len(files) <= 2000:
        parser.error('Use 3–2000 frames in one target folder')
    root.mkdir(exist_ok=False)
    raw = root / 'siril'; raw.mkdir()
    records = []
    for i, path in enumerate(files):
        target = raw / f'input-{i + 1:06}.fit'
        digest = sha(path); shutil.copy2(path, target)
        if sha(target) != digest:
            raise RuntimeError('Source changed during copy: ' + str(path))
        records.append({'path': str(path), 'sha': digest, 'copy': str(target)})
    (root / 'sources.json').write_text(json.dumps(records, indent=2))
    shutil.copy2(args.siril_config, root / 'siril.cfg')
    script = ['requires 1.4.0', 'set32bits', 'setcpu 4', 'convert input -out=.',
              'seqextract_HaOIII input -resample=oiii',
              'register Ha_input -transf=similarity -interp=linear -minpairs=4',
              'stack r_Ha_input rej 3 3 -norm=none -32b -out=Ha_stack',
              'register OIII_input -transf=similarity -interp=linear -minpairs=4',
              'stack r_OIII_input rej 3 3 -norm=none -32b -out=OIII_stack']
    (root / 'run.ssf').write_text('\n'.join(script) + '\n')
    try:
        for name, command in [
            ('siril', [str(args.siril.resolve()), '-d', str(raw), '-i', str(root / 'siril.cfg'), '-s', str(root / 'run.ssf')]),
            ('photonstack', [str(args.photonstack.resolve()), 'narrowband-stack', '--output-dir', str(root / 'photonstack'), *map(str, files)])
        ]:
            start = time.monotonic()
            with (root / (name + '.log')).open('w') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            (root / (name + '-run.json')).write_text(json.dumps({
                'frames': len(files), 'seconds': time.monotonic() - start,
                'returncode': result.returncode, 'command': command,
                'executableSHA': sha(Path(command[0])),
                'timingScope': 'Warm/unknown caches; different algorithms and IO. Not a performance ranking.'}, indent=2))
            result.check_returncode()
        compare = Path(__file__).with_name('narrowband_siril_comparison.py')
        subprocess.run([sys.executable, str(compare),
                        '--photonstack-ha', str(root / 'photonstack/Ha.fits'),
                        '--photonstack-oiii', str(root / 'photonstack/OIII.fits'),
                        '--siril-ha', str(raw / 'Ha_stack.fit'), '--siril-oiii', str(raw / 'OIII_stack.fit'),
                        '--siril-scale', str(args.siril_scale), '--output', str(root / 'comparison.json')], check=True)
    finally:
        unchanged = all(sha(Path(item['path'])) == item['sha'] for item in records)
        (root / 'source-verification.json').write_text(json.dumps({'originalsUnchanged': unchanged, 'frames': len(records)}))
        if not unchanged:
            raise RuntimeError('Originals changed during validation')


if __name__ == '__main__':
    main()
