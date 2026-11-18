#!/usr/bin/env python3
"""Read-only real FITS acquisition audit using Astropy, independent of the engine.

Requires astropy. Does not validate calibrated pixels or manufacture masters.
Example: script CLI NEW_OUTPUT_DIR T1_sub T2_sub
"""
import datetime
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time

from astropy.io import fits


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            result.update(block)
    return result.hexdigest()


def main():
    cli = Path(sys.argv[1]).resolve(strict=True)
    output = Path(sys.argv[2]).absolute()
    sources = sorted({p.resolve() for folder in sys.argv[3:] for p in Path(folder).iterdir()
                      if p.suffix.lower() in {'.fits', '.fit', '.fts'}})
    if not sources:
        raise ValueError('No FITS inputs')
    output.mkdir(parents=True, exist_ok=False)
    oracle = {}
    for path in sources:
        header = fits.getheader(path, 0)
        numbers = {}
        for field, keyword in {
            'exposure': 'EXPTIME', 'gain': 'GAIN', 'iso': 'ISOSPEED', 'offset': 'OFFSET',
            'electronsPerADU': 'EGAIN', 'sensorTemperature': 'CCD-TEMP', 'setTemperature': 'SET-TEMP',
            'binX': 'XBINNING', 'binY': 'YBINNING', 'originX': 'XORGSUBF', 'originY': 'YORGSUBF',
            'focalLength': 'FOCALLEN',
        }.items():
            if keyword in header:
                numbers[field] = float(header[keyword])
        if 'EXPOSURE' in header:
            assert 'exposure' not in numbers or numbers['exposure'] == float(header['EXPOSURE'])
            numbers['exposure'] = float(header['EXPOSURE'])
        numbers.update(scale=float(header.get('BSCALE', 1)), zero=float(header.get('BZERO', 0)))
        strings = {}
        for field, keyword in {
            'camera': 'INSTRUME', 'cameraSerial': 'CAM-SER', 'filter': 'FILTER', 'opticalTrain': 'PSOPTICS',
            'readoutMode': 'READMODE', 'object': 'OBJECT', 'observedAt': 'DATE-OBS', 'timeSystem': 'TIMESYS',
            'frameType': 'IMAGETYP', 'units': 'BUNIT', 'domain': 'PSCOLOR', 'declaredLayout': 'PSLAYOUT',
            'bayer': 'BAYERPAT', 'temperatureUnit': 'TEMPUNIT',
        }.items():
            if keyword in header and str(header[keyword]).strip():
                strings[field] = str(header[keyword]).strip()
        if strings.get('units', '').upper() == 'ADU':
            strings['units'] = 'ADU'
        if strings.get('domain', '').upper() in {'LINEAR', 'LINEAR-SRGB'}:
            strings['domain'] = 'linear'
        if 'bayer' in strings:
            pattern = strings['bayer'].upper()
            dx, dy = int(header.get('XBAYROFF', 0)), int(header.get('YBAYROFF', 0))
            strings['sampling'] = 'CFA:' + ''.join(pattern[((y+dy) % 2)*2+(x+dx) % 2]
                                                  for y in range(2) for x in range(2))
        elif strings.get('declaredLayout', '').upper() == 'MONO':
            strings['sampling'] = 'mono'
        date = datetime.datetime.fromisoformat(strings['observedAt']).date().isoformat()
        assert strings.get('timeSystem', 'UTC') == 'UTC', 'This real-data oracle expects UTC inputs'
        oracle[str(path)] = dict(sha=digest(path), bytes=path.stat().st_size,
                                width=int(header['NAXIS1']), height=int(header['NAXIS2']),
                                channels=int(header.get('NAXIS3', 1)), bitpix=int(header['BITPIX']),
                                numeric=numbers, text=strings, utcDate=date)
    manifest = output/'plan.txt'
    manifest.write_text('PhotonStack-CalibrationPlan-1\ntemperature-tolerance 1\n'+''.join(
        'light '+json.dumps(str(p), ensure_ascii=False)+' '+oracle[str(p)]['sha']+'\n' for p in sources))
    durations = []
    result = None
    for _ in range(5):
        start = time.perf_counter()
        completed = subprocess.run([str(cli), 'calibration-plan', '--manifest', str(manifest)],
                                   check=True, capture_output=True, text=True)
        durations.append(time.perf_counter()-start)
        current = json.loads(completed.stdout)
        if result is not None:
            assert result == current, 'Nondeterministic plan'
        result = current
    assert len(result['lights']) == len(sources)
    expected_groups = {}
    for path, expected in oracle.items():
        text_keys = ['object', 'camera', 'cameraSerial', 'filter', 'opticalTrain', 'readoutMode', 'sampling', 'units', 'domain']
        number_keys = ['exposure', 'gain', 'iso', 'offset', 'binX', 'binY', 'originX', 'originY', 'focalLength', 'electronsPerADU']
        identity = [expected['width'], expected['height'], expected['channels'], expected['utcDate']]
        identity += [expected['text'].get(k) for k in text_keys]
        identity += [expected['numeric'].get(k) for k in number_keys]
        assert expected['text'].get('object'), 'This grouping oracle expects explicit targets'
        expected_groups.setdefault(tuple(identity), set()).add(path)
    for actual in result['lights']:
        expected = oracle[actual['path']]
        for key in ['sha', 'bytes', 'width', 'height', 'channels', 'bitpix', 'numeric', 'text']:
            assert actual[key] == expected[key], (actual['path'], key, actual[key], expected[key])
        assert not actual['issues'], actual['issues']
    actual_groups = {frozenset(result['lights'][i]['path'] for i in g['members']) for g in result['groups']}
    assert actual_groups == {frozenset(g) for g in expected_groups.values()}
    assert not result['canApply'] and not result['pixelsValidated']
    assert all(v['status'] == 'missing-master' for g in result['groups'] for v in g['roles'].values())
    assert len(result['frameChoices']) == len(sources)
    for i, choice in enumerate(result['frameChoices']):
        assert choice['light'] == i and i in result['groups'][choice['group']]['members']
        assert all(v['status'] == 'missing-master' and v['suggestedMaster'] is None and
                   not v['compatibleMasters'] and not v['reviewMasters'] for v in choice['roles'].values())
    for path in sources:
        assert digest(path) == oracle[str(path)]['sha'], 'Source changed during validation'
    report = dict(inputs=len(sources), sourceBytes=sum(v['bytes'] for v in oracle.values()),
                  sourceUnchanged=True, allMetadataEqualToAstropy=True, groups=[dict(
                      target=g['text'].get('object'), date=g['text'].get('utcDate'), frames=len(g['members']),
                      filter=g['text'].get('filter'), exposure=g['numeric'].get('exposure'),
                      temperatureRange=[min(result['lights'][i]['numeric']['sensorTemperature'] for i in g['members']),
                                        max(result['lights'][i]['numeric']['sensorTemperature'] for i in g['members'])]
                  ) for g in result['groups']], seconds=durations, medianSeconds=statistics.median(durations),
                  timingScope='CPU CLI startup, source hashing twice, header inspection, grouping and JSON output; warm file cache',
                  limitations='No master files supplied; no calibrated pixels or real calibration-match accuracy validated')
    (output/'oracle.json').write_text(json.dumps(oracle, ensure_ascii=False, indent=2))
    (output/'plan.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
    (output/'audit.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
