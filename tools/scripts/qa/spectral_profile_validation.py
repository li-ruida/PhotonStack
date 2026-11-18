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
    assert next(tokens) == 'PhotonStack-SpectralProfile-1'
    assert [next(tokens) for _ in range(5)] == [report['profile'][k] for k in ['name', 'mode', 'quality', 'provenance', 'license']]
    def curve():
        n = int(next(tokens))
        xy = np.array([[float(next(tokens)), float(next(tokens))] for _ in range(n)])
        return xy[:, 0], xy[:, 1]
    bands = [[curve() for _ in range(int(next(tokens)))] for _ in range(3)]
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
