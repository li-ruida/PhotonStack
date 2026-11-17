#!/usr/bin/env python3
"""Inject known sensor defects into read-only real CFA images and check recovery.
Uses legacy cell-average sampling to reproduce the recorded calibration baseline.
Requires NumPy and Astropy. All generated files must go to a new directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import numpy as np
from astropy.io import fits


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('inputs', nargs='+', type=Path)
    args = parser.parse_args()
    assert len(args.inputs) >= 3
    args.output.mkdir(parents=True, exist_ok=False)
    before = {str(p): digest(p) for p in args.inputs}
    image, header = fits.getdata(args.inputs[0], header=True)
    h, w = image.shape
    yy, xx = np.mgrid[:h, :w]
    phase = (yy % 2) * 2 + xx % 2
    response = 1 - .18 * ((xx-w/2)/(w/2))**2 - .12 * ((yy-h/2)/(h/2))**2
    flat = (response * (1000 + 500*phase)).astype(np.float32)
    bias = (30 + 4*xx/w + 3*yy/h).astype(np.float32)
    dark = (bias + 6 + 2*np.sin(xx/71)).astype(np.float32)
    normalization = np.empty_like(response)
    for p in range(4):
        mask = phase == p
        normalization[mask] = flat[mask] / flat[mask].astype(np.float64).mean()
    # All three masters are generated test data, not camera calibration captures.
    def write(name, data, source_header=header):
        out = args.output / name
        copied = source_header.copy()
        for key in ('BSCALE', 'BZERO', 'BLANK', 'CHECKSUM', 'DATASUM'):
            copied.remove(key, ignore_missing=True)
        fits.writeto(out, data.astype(np.float32), copied)
        return out
    bias_path = write('bias.fits', bias)
    dark_path = write('dark.fits', dark)
    flat_path = write('flat.fits', flat)
    damaged = []
    for n, source in enumerate(args.inputs):
        raw, hdr = fits.getdata(source, header=True)
        assert raw.shape == (h,w)
        damaged.append(write(f'injected-{n}.fits', raw.astype(np.float64)*normalization+dark, hdr))
    options = ['--bias', bias_path, '--dark', dark_path, '--dark-bias', 'included', '--flat', flat_path, '--flat-bias', 'removed']
    metrics = {}
    def run(label, command):
        started = time.perf_counter()
        result = subprocess.run(['/usr/bin/time', '-l', str(args.cli), command[0], '--sampling', 'cell-average', *map(str,command[1:])], capture_output=True, text=True)
        (args.output / (label+'.log')).write_text(result.stderr)
        if result.returncode: raise RuntimeError(result.stderr)
        metrics[label+'_seconds'] = time.perf_counter()-started
        metrics[label+'_report'] = json.loads(result.stdout)
    run('calibrated-extract',['narrowband-extract','--input',damaged[0],'--output-dir',args.output/'extracted',*options])
    pattern = header['BAYERPAT'].strip().upper()
    rx = int(header.get('XBAYROFF',0)) % 2
    ry = int(header.get('YBAYROFF',0)) % 2
    planes = [(pattern[((y+ry)%2)*2+(x+rx)%2], image[y:h-h%2:2,x:w-w%2:2].astype(np.float64)) for y in range(2) for x in range(2)]
    truths = {'Ha':next(v for c,v in planes if c=='R'), 'OIII':sum(v for c,v in planes if c!='R')/3}
    for name, truth in truths.items():
        actual = fits.getdata(args.output/'extracted'/(name+'.fits')).astype(np.float64)
        delta = actual-truth
        metrics[name+'_extraction_max_abs'] = float(np.nanmax(np.abs(delta)))
        metrics[name+'_extraction_rms'] = float(np.sqrt(np.nanmean(delta**2)))
        assert np.array_equal(np.isfinite(actual),np.isfinite(truth))
        assert np.nanmax(np.abs(delta)) < .02, (name,metrics)
    run('baseline-stack',['narrowband-stack','--output-dir',args.output/'baseline',*args.inputs])
    run('calibrated-stack',['narrowband-stack','--output-dir',args.output/'calibrated',*options,*damaged])
    for name in ('Ha','OIII','HOO','Ha-weight','OIII-weight','geometric-coverage'):
        baseline=fits.getdata(args.output/'baseline'/(name+'.fits')).astype(np.float64)
        calibrated=fits.getdata(args.output/'calibrated'/(name+'.fits')).astype(np.float64)
        assert np.array_equal(np.isfinite(calibrated),np.isfinite(baseline)),name
        delta=calibrated-baseline
        metrics[name+'_stack_max_abs']=float(np.nanmax(np.abs(delta)))
        metrics[name+'_stack_rms']=float(np.sqrt(np.nanmean(delta**2)))
        metrics[name+'_stack_max_relative'] = float(np.nanmax(np.abs(delta)/np.maximum(1,np.abs(baseline))))
        # Float32 defect injection rounds bright cores before registration; a
        # fixed absolute count limit does not express the error at every level.
        # Bound both fractional error (0.01%) and full-frame RMS (0.001 count).
        # Rejection/coverage changes are checked separately and more strictly.
        if name in ('Ha','OIII','HOO'):
            assert metrics[name+'_stack_max_relative'] < 1e-4,(name,metrics)
            assert metrics[name+'_stack_rms'] < .001,(name,metrics)
        else: assert metrics[name+'_stack_max_abs'] < 1e-6,(name,metrics)
    assert before == {str(p):digest(p) for p in args.inputs}
    metrics['originals_unchanged'] = before
    (args.output/'metrics.json').write_text(json.dumps(metrics,indent=2))
    print(json.dumps({k:v for k,v in metrics.items() if not k.endswith('_report') and k!='originals_unchanged'},indent=2))


if __name__ == '__main__':
    main()
