#!/usr/bin/env python3
"""Separate extraction differences from an existing full Siril/native sequence run.
Requires NumPy/SciPy/Astropy and the read-only evidence directory created by
narrowband_siril_sequence.py. Runs only PhotonStack, never third-party models.
Both Siril sample scales must be explicit: extraction and stack file types can
differ even within one set32bits script. Outputs require a fresh directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run-dir', 'photonstack', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    for name in ('extraction-scale', 'stack-scale'):
        parser.add_argument('--' + name, type=float, required=True)
    args = parser.parse_args()
    root, output, cli = (p.resolve() for p in (args.run_dir, args.output, args.photonstack))
    if output == root or root in output.parents:
        parser.error('Write stage evidence outside the existing read-only run')
    records = json.loads((root / 'sources.json').read_text())
    if len(records) < 3:
        parser.error('At least three source records are required')
    selected = sorted({0, len(records) // 2, len(records) - 1})
    originals = [(Path(records[i]['path']), records[i]['sha']) for i in selected]
    if any(output == path.parent or path.parent in output.parents for path, _ in originals):
        parser.error('Keep original source directories read-only')
    for path, digest in originals:
        if sha(path) != digest:
            raise RuntimeError('Original changed: ' + str(path))
    output.mkdir(exist_ok=False)
    comparator = Path(__file__).with_name('narrowband_siril_comparison.py')
    summaries = []

    def compare(name, native, siril_ha, siril_oiii, scale):
        report = output / (name + '.json')
        command = [sys.executable, str(comparator), '--photonstack-ha', str(native / 'Ha.fits'),
                   '--photonstack-oiii', str(native / 'OIII.fits'), '--siril-ha', str(siril_ha),
                   '--siril-oiii', str(siril_oiii), '--siril-scale', str(scale), '--output', str(report)]
        with (output / (name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        data = json.loads(report.read_text())
        if name.startswith('extraction-') and data['Ha_max_abs'] > 0.01:
            raise RuntimeError('Direct H-alpha samples differ; verify declared extraction scale and CFA orientation before interpreting OIII')
        summaries.append({'stage': name, 'sirilScale': scale,
                          'haMaximumAbsolute': data['Ha_max_abs'],
                          'oxygen': data['channels']['OIII'], 'stars': data['unsaturated_stars'],
                          'excludedMissingSamples': data['missing_samples_excluded']})

    try:
        for index in selected:
            record = records[index]
            source = Path(record['copy'])
            if sha(source) != record['sha']:
                raise RuntimeError('Siril source copy changed: ' + str(source))
            native = output / f'native-{index + 1:05}'
            with (output / f'extract-{index + 1:05}.log').open('w') as log:
                subprocess.run([str(cli), 'narrowband-extract', '--input', str(source),
                                '--output-dir', str(native)], stdout=log, stderr=subprocess.STDOUT, check=True)
            compare(f'extraction-{index + 1:05}', native,
                    root / 'siril' / f'Ha_input_{index + 1:05}.fit',
                    root / 'siril' / f'OIII_input_{index + 1:05}.fit', args.extraction_scale)
        compare('stack', root / 'photonstack', root / 'siril/Ha_stack.fit',
                root / 'siril/OIII_stack.fit', args.stack_scale)
        report = {'version': 1, 'sourceCount': len(records), 'selectedIndicesZeroBased': selected,
                  'cliSHA256': sha(cli), 'sourceManifestSHA256': sha(root / 'sources.json'),
                  'comparisons': summaries,
                  'interpretation': 'Different extraction and registration policies. Stage differences are observations, not a causal variance decomposition or calibration acceptance. No fitted gain or subpixel correction is applied.'}
        (output / 'stages.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
    finally:
        unchanged = all(sha(path) == digest for path, digest in originals)
        (output / 'source-verification.json').write_text(json.dumps({'selectedOriginalsUnchanged': unchanged}))
        if not unchanged:
            raise RuntimeError('Source changed during stage comparison')


if __name__ == '__main__':
    main()
