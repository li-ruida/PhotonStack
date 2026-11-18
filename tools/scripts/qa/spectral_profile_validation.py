#!/usr/bin/env python3
"""Verify native response inspection with independent polynomial integrals.

Requires NumPy, SciPy and Astropy (shared audit helpers). The supplied response
remains read-only; no network is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import numpy as np
from spectral_calibration_validation import integrate


def narrowband_metadata(tokens, version, profile):
    assert version in ['PhotonStack-SpectralProfile-1', 'PhotonStack-SpectralProfile-2']
    if version.endswith('-1'):
        assert profile['mode'] == 'broadband' and 'narrowband' not in profile
        return None
    layout, base = next(tokens), next(tokens)
    pairs = [[float(next(tokens)), float(next(tokens))] for _ in range(3)]
    model = dict(layout=layout, baseSHA=base, model='trapezoid-edge-1e-4',
                 centers=[p[0] for p in pairs], widths=[p[1] for p in pairs])
    assert profile['mode'] == 'narrowband' and profile['quality'] == 'approximate'
    assert model == profile['narrowband']
    assert layout in ['mono', 'osc-dual', 'extracted-hoo']
    assert len(base) == 64 and all(c in '0123456789abcdef' for c in base)
    for center, width in pairs:
        assert 3 <= width <= 100 and center-width/2 >= 336 and center+width/2 <= 1020
    if layout != 'mono':
        assert pairs[1] == pairs[2] and abs(pairs[0][0]-pairs[1][0]) > (pairs[0][1]+pairs[1][1])/2
    return model


def narrowband_curves(model, bands):
    if model is None: return
    for c, band in enumerate(bands):
        indices = [c] if model['layout'] == 'mono' else sorted([0,1], key=lambda i: model['centers'][i])
        expected = []
        for i in indices:
            center, width = model['centers'][i], model['widths'][i]
            low, high = center-width/2, center+width/2
            expected += [low, low+width*.0001, high-width*.0001, high]
        np.testing.assert_array_equal(band[-1][0], expected)
        np.testing.assert_array_equal(band[-1][1], [0,1,1,0]*len(indices))
        assert len(band) >= 2
        assert all(xs[0] <= expected[0] and xs[-1] >= expected[-1] for xs, _ in band[:-1])
    if model['layout'] == 'extracted-hoo':
        for g, b in zip(bands[1], bands[2]): np.testing.assert_array_equal(g,b)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = args.profile.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    report = json.loads(subprocess.check_output([str(args.cli), 'spcc-profile-inspect', '--profile', str(args.profile), '--expected-sha', digest]))
    tokens = iter(shlex.split(original.decode()))
    version = next(tokens)
    assert [next(tokens) for _ in range(5)] == [report['profile'][k] for k in ['name', 'mode', 'quality', 'provenance', 'license']]
    model = narrowband_metadata(tokens, version, report['profile'])
    def curve():
        n = int(next(tokens))
        xy = np.array([[float(next(tokens)), float(next(tokens))] for _ in range(n)])
        return xy[:, 0], xy[:, 1]
    bands = [[curve() for _ in range(int(next(tokens)))] for _ in range(3)]
    narrowband_curves(model, bands)
    assert [next(tokens) for _ in range(3)] == [report['profile'][k] for k in ['whiteName', 'whiteProvenance', 'whiteLicense']]
    wx, wy = curve()
    assert not list(tokens)
    differences = []
    sample_errors = []
    for band, actual in zip(bands, report['bands']):
        expected = integrate(wx, wy, band)
        measured = [actual[k] for k in ['whiteFlux', 'meanWavelength', 'rmsBandwidth']]
        np.testing.assert_allclose(measured, expected, rtol=3e-13, atol=1e-10)
        differences.append(np.abs(np.array(measured) / expected - 1).tolist())
        points = np.array(actual['samples'])
        response = np.prod([np.interp(points[:, 0], x, y) for x, y in band], axis=0)
        np.testing.assert_allclose(points[:, 1], response, rtol=1e-12, atol=1e-14)
        sample_errors.append(float(np.max(np.abs(points[:, 1] - response))))
    white = np.array(report['whiteSamples'])
    assert report['whiteNormalization'] == np.max(np.abs(wy))
    np.testing.assert_allclose(white[:, 1], np.interp(white[:, 0], wx, wy) / np.max(np.abs(wy)), rtol=1e-13, atol=1e-14)
    assert args.profile.read_bytes() == original
    audit = dict(profileSHA=digest, relativeDifferencesFluxMeanRMS=differences, maximumPlotResponseError=sample_errors,
                 profileUnchanged=True, sampleCounts=[len(b['samples']) for b in report['bands']])
    with args.output.open('x') as f:
        json.dump(audit, f, indent=2); f.write('\n')
    print(json.dumps(audit, indent=2))


if __name__ == '__main__':
    main()
