#!/usr/bin/env python3
"""Offline Gaia XP / CALSPEC / external response numerical audit.

Requires NumPy, SciPy, Astropy and the manual C++ spectral calibration probe.
Uses an explicitly approximate ideal 400..700 nm IRCUT, not a measured camera
filter. External response files and their licenses remain outside the project.
No network calls; source FITS and all supplied reference data remain read-only.
"""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import time

import numpy as np
from astropy.io import fits
from astropy.time import Time
from astropy.wcs import WCS
from scipy.optimize import least_squares


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def integrate(wavelength, flux, band):
    """Independent exact local-polynomial integration, without quadrature."""
    low = max(x[0][0] for x in band)
    high = min(x[0][-1] for x in band)
    assert wavelength[0] <= low < high <= wavelength[-1]
    knots = np.unique(np.concatenate(([low, high], wavelength, *[x[0] for x in band])))
    knots = knots[(knots >= low) & (knots <= high)]
    total = []
    for a, b in zip(knots[:-1], knots[1:]):
        p = np.array([1.])
        for x, y in band:
            ends = np.interp([a, b], x, y)
            p = np.polynomial.polynomial.polymul(p, [ends[0], ends[1] - ends[0]])
        q = np.polynomial.polynomial.polymul(p, [a, b-a])
        s = np.polynomial.polynomial.polymul(q, [a, b-a])
        f = np.interp([a, b], wavelength, flux)
        signal = np.polynomial.polynomial.polymul(q, [f[0], f[1] - f[0]])
        total.append([(b-a)*sum(v/np.arange(1, len(v)+1)) for v in [signal, p, q, s]])
    value, area, first, second = np.sum(total, axis=0)
    return np.array([value, first/area, np.sqrt(max(0., second/area-(first/area)**2))])


def photometry(pixels, x, y, radius=8., inner=11., outer=15.):
    # Python grid includes every pixel in the aperture or background annulus.
    cx, cy = int(np.floor(x+.5)), int(np.floor(y+.5))
    reach = int(np.ceil(outer+1))
    yy, xx = np.mgrid[cy-reach:cy+reach+1, cx-reach:cx+reach+1]
    dx, dy = xx-x, yy-y
    distance = np.hypot(dx, dy)
    sky = (distance >= inner) & (distance <= outer)
    weights = np.zeros(dx.shape)
    for u in (np.arange(8)+.5)/8-.5:
        for v in (np.arange(8)+.5)/8-.5:
            weights += np.hypot(dx+u, dy+v) <= radius
    weights /= 64
    basis = np.stack([np.ones(dx.shape), dx, dy], axis=-1)
    design = basis[sky]
    aperture_basis = (basis*weights[:, :, None]).sum(axis=(0, 1))
    values = pixels[:3, yy, xx]
    output = dict(flux=[], sigma=[], noise=[], plane=[], used=[], area=float(weights.sum()))
    for channel in values:
        samples = channel[sky]
        robust = np.ones(samples.shape)
        for _ in range(8):
            beta = np.linalg.lstsq(design*np.sqrt(robust)[:, None], samples*np.sqrt(robust), rcond=None)[0]
            residuals = samples-design@beta
            location = np.median(residuals)
            sigma = 1.4826*np.median(np.abs(residuals-location))
            scale = 4.685*max(sigma, 1e-12, abs(beta[0])*1e-7)
            u = (residuals-location)/scale
            robust = np.where(np.abs(u) < 1, (1-u*u)**2, 0)
        beta = np.linalg.lstsq(design*np.sqrt(robust)[:, None], samples*np.sqrt(robust), rcond=None)[0]
        residuals = samples-design@beta
        sigma = 1.4826*np.median(np.abs(residuals-np.median(residuals)))
        influence = np.linalg.solve(design.T@(robust[:, None]*design), aperture_basis)
        variance = np.sum((robust*(design@influence))**2)
        output['flux'].append(float(np.sum(weights*(channel-basis@beta))))
        output['sigma'].append(float(sigma))
        output['noise'].append(float(sigma*np.sqrt(np.sum(weights**2)+variance)))
        output['plane'].append(beta.tolist())
        output['used'].append(int(np.sum(robust > .01)))
    return output


def centroid(pixels, px, py):
    cx, cy = int(np.floor(px+.5)), int(np.floor(py+.5))
    tile = pixels[:3, cy-7:cy+8, cx-7:cx+8]
    data = np.einsum('c,cij->ij', [.2126, .7152, .0722], tile)
    y, x = np.mgrid[-7:8, -7:8]
    sky = np.median(data[x*x+y*y > 30])
    amp = np.max(data[5:10, 5:10])-sky
    normalized = (data-sky)/amp

    def residual(p):
        a, mx, my, lx, ly, z, b, bx, by = p
        u, v, rho = (x-mx)/np.exp(lx), (y-my)/np.exp(ly), .9*np.tanh(z)
        model = a*np.exp(-.5*(u*u+v*v-2*rho*u*v)/(1-rho*rho))+b+bx*x+by*y
        return (model-normalized).ravel()

    result = least_squares(residual, [1, px-cx, py-cy, np.log(1.4), np.log(1.4), 0, 0, 0, 0],
                           bounds=([.1, -3, -3, np.log(.5), np.log(.5), -2, -np.inf, -np.inf, -np.inf],
                                   [3, 3, 3, np.log(4), np.log(4), 2, np.inf, np.inf, np.inf]),
                           max_nfev=500, ftol=1e-12, xtol=1e-12, gtol=1e-12)
    assert result.success
    return np.array([cx+result.x[1], cy+result.x[2]])


def audit_fit(report, white, width, height):
    stars = sorted((s for s in report['stars'] if s['status'] == 'measured'), key=lambda s: (s['y'], s['x'], s['id']))
    if len(stars) < 30:
        assert not report['fit']['valid']
        return
    predicted = np.array([s['predicted'] for s in stars])
    observed = np.array([s['observed'] for s in stars])
    logs = np.log(predicted/predicted[:, 1, None])-np.log(observed/observed[:, 1, None])
    held = np.arange(len(stars)) % 5 == 4
    center = np.median(logs[~held], axis=0)
    scatter = 1.4826*np.median(np.abs(logs[~held]-center), axis=0)
    keep = np.all(np.abs(logs[:, [0, 2]]-center[[0, 2]]) <= 4.685*np.maximum(.005, scatter[[0, 2]]), axis=1)
    fit = ~held & keep
    center = np.median(logs[fit], axis=0)
    residual = center-logs
    native = report['fit']
    np.testing.assert_allclose(native['instrumentGains'], np.exp(center), atol=1e-12, rtol=1e-12)
    np.testing.assert_allclose(native['gains'], np.exp(center)/(white/white[1]), atol=1e-12, rtol=1e-12)
    np.testing.assert_allclose(native['trainingScatter'], 1.4826*np.median(np.abs(logs[fit]-center), axis=0), atol=1e-12)
    np.testing.assert_allclose(native['validationMedian'], np.median(residual[held], axis=0), atol=1e-12)
    np.testing.assert_allclose(native['validationP90'], np.quantile(np.abs(residual[held]), .9, axis=0), atol=1e-12)
    assert native['training'] == int(fit.sum()) and native['validation'] == int(held.sum())
    assert native['rejected'] == int((~held & ~keep).sum())
    regions = {(min(3, int(4*s['x']/width)), min(3, int(4*s['y']/height))) for s, h in zip(stars, held) if h}
    assert native['regions'] == len(regions)
    for i, s in enumerate(native['stars']):
        assert s['id'] == stars[i]['id']
        assert s['status'] == ('validation' if held[i] else 'fit' if fit[i] else 'fit-outlier')
        np.testing.assert_allclose(s['residual'], residual[i], atol=1e-12)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ['input', 'catalog', 'xp-dir', 'response', 'white', 'probe', 'output-dir']:
        parser.add_argument('--'+key, required=True, type=Path)
    parser.add_argument('--observation-date', required=True, help='Explicit assumed date when absent from master')
    args = parser.parse_args()
    out = args.output_dir
    if out.exists():
        raise ValueError('Output directory must be new')
    paths = [args.input, args.catalog, args.response, args.white, *sorted(args.xp_dir.glob('xp-*.download'))]
    hashes = {str(p.resolve()): sha(p) for p in paths}
    responses = json.loads(args.response.read_text())
    bands = []
    for color in ['RED', 'GREEN', 'BLUE']:
        curves = [r for r in responses if r['channel'] == color]
        assert len(curves) == 1 and curves[0]['wavelength']['units'] == 'nm'
        r = curves[0]
        assert r['values']['range'] == 1
        bands.append([(np.array(r['wavelength']['value']), np.array(r['values']['value'])),
                      (np.array([400., 700.]), np.ones(2))])
    with fits.open(args.white) as f:
        white_x = np.array(f[1].data['WAVELENGTH'], dtype=float)/10
        white_y = np.array(f[1].data['FLUX'], dtype=float)*.01
        assert f[1].columns['WAVELENGTH'].unit == 'ANGSTROMS' and f[1].columns['FLUX'].unit == 'FLAM'
        assert np.isfinite(white_y).all() and (white_y > 0).all()
    with fits.open(args.input) as f:
        pixels = np.array(f[0].data, dtype=float)
        wcs = WCS(f[0].header, naxis=2)
    assert pixels.ndim == 3 and pixels.shape[0] in [3, 4]
    spectra = {}
    for path in paths[4:]:
        for row in csv.DictReader(path.open()):
            sid = row['source_id']
            assert sid not in spectra
            spectra[sid] = tuple(np.fromstring(row[k].strip('()'), sep=',') for k in ['flux', 'flux_error'])
            assert all(len(a) == 343 and np.isfinite(a).all() for a in spectra[sid])
    catalog = list(csv.DictReader(args.catalog.open()))
    dt = Time(args.observation_date).jyear-2016.
    stars = []
    for row in catalog:
        if row['source_id'] not in spectra or not row['pmra'] or not row['pmdec']:
            continue
        ra, dec = float(row['ra']), float(row['dec'])
        stars.append((row['source_id'], ra+dt*float(row['pmra'])/(3.6e6*np.cos(np.deg2rad(dec))),
                      dec+dt*float(row['pmdec'])/3.6e6))
    out.mkdir(parents=True)
    fixture = out/'fixture.txt'

    def write_curve(stream, x, y):
        stream.write(str(len(x))+'\n')
        for a, b in zip(x, y):
            stream.write(f'{a:.17g} {b:.17g}\n')

    grid = np.arange(336., 1021., 2.)
    with fixture.open('w') as f:
        for band in bands:
            f.write(str(len(band))+'\n')
            for x, y in band:
                write_curve(f, x, y)
        write_curve(f, white_x, white_y)
        f.write(str(len(stars))+'\n')
        for sid, ra, dec in stars:
            f.write(f'{sid} {ra:.17g} {dec:.17g}\n')
            for a in spectra[sid]:
                write_curve(f, grid, a)
    start = time.monotonic()
    with (out/'native.json').open('w') as f:
        subprocess.run([str(args.probe), str(args.input), str(fixture), str(out/'calibrated.fits')], stdout=f, check=True)
    elapsed = time.monotonic()-start
    report = json.loads((out/'native.json').read_text())
    white = np.array([integrate(white_x, white_y, b)[0] for b in bands])
    np.testing.assert_allclose(report['white'], white, rtol=3e-13)
    xy = wcs.all_world2pix([[ra, dec] for _, ra, dec in stars], 0)
    flux_errors, aperture_errors, centroid_errors = [], [], []
    for index, native in enumerate(report['stars']):
        sid = stars[index][0]
        assert sid == native['id']
        np.testing.assert_allclose([native['predictedX'], native['predictedY']], xy[index], atol=1e-5, rtol=0)
        for c, band in enumerate(bands):
            expected = integrate(grid, spectra[sid][0], band)
            expected_error = integrate(grid, spectra[sid][1], band)[0]
            np.testing.assert_allclose(native['predicted'][c], expected[0], rtol=3e-13)
            np.testing.assert_allclose(native['errorBound'][c], expected_error, rtol=3e-13)
            np.testing.assert_allclose([native['meanWavelength'][c], native['rmsBandwidth'][c]], expected[1:], rtol=3e-12)
            flux_errors.append(abs(native['predicted'][c]/expected[0]-1))
        if native['status'] != 'measured':
            continue
        measured = photometry(pixels, native['x'], native['y'])
        for a, b in [('observed', 'flux'), ('skyPlane', 'plane'), ('skySigma', 'sigma'), ('backgroundNoise', 'noise')]:
            np.testing.assert_allclose(native[a], measured[b], rtol=2e-8, atol=1e-8)
        assert native['skyUsed'] == measured['used'] and native['area'] == measured['area']
        aperture_errors.append(float(np.max(np.abs(np.array(native['observed'])/measured['flux']-1))))
        if len(centroid_errors) < 48:
            fitted = centroid(pixels, native['predictedX'], native['predictedY'])
            centroid_errors.append(float(np.linalg.norm(fitted-[native['x'], native['y']])))
    assert centroid_errors and max(centroid_errors) < .003
    audit_fit(report, white, pixels.shape[2], pixels.shape[1])
    review = report['review']
    original_stars = {s['id']: s for s in report['stars']}
    selected = []
    expanded_errors = []
    for s in review['stars']:
        if s['status'] not in ['fit', 'validation', 'fit-outlier', 'aperture-color-unstable']:
            continue
        base = original_stars[s['id']]
        np.testing.assert_allclose(s['observed'], base['observed'], rtol=1e-12, atol=1e-9)
        expanded = photometry(pixels, s['expandedX'], s['expandedY'], 10, 13, 17)
        np.testing.assert_allclose(s['expandedFlux'], expanded['flux'], rtol=2e-8, atol=1e-8)
        growth = np.log(np.array(expanded['flux'])/expanded['flux'][1])-np.log(np.array(s['observed'])/s['observed'][1])
        np.testing.assert_allclose(s['apertureColorChange'], growth, atol=1e-9)
        expanded_errors.append(float(np.max(np.abs(np.array(s['expandedFlux'])/expanded['flux']-1))))
        if s['status'] == 'aperture-color-unstable':
            assert np.max(np.abs(growth)) > .03
            continue
        assert np.max(np.abs(growth)) <= .03
        assert np.max(np.array(base['errorBound'])/base['predicted']) <= .1
        selected.append(dict(base, status='measured', x=s['x'], y=s['y']))
    audit_fit({'stars': selected, 'fit': dict(review, stars=review['fitStars'])}, white, pixels.shape[2], pixels.shape[1])
    scientific_output = None
    if review['valid']:
        with fits.open(out/'calibrated.fits') as f:
            output_pixels = f[0].data
            output_wcs = WCS(f[0].header, naxis=2)
            assert f[0].header['PSCOLOR'] == 'LINEAR-SRGB'
        expected = pixels.astype(np.float32)
        valid = np.isfinite(pixels[:3]).all(axis=0)
        if pixels.shape[0] == 4:
            valid &= pixels[3] > 0
        for c, gain in enumerate(review['gains']):
            expected[c, valid] = (pixels[c, valid]*gain).astype(np.float32)
        np.testing.assert_array_equal(output_pixels, expected)
        yy, xx = np.meshgrid(np.linspace(0, pixels.shape[1]-1, 33), np.linspace(0, pixels.shape[2]-1, 33))
        points = np.column_stack([xx.ravel(), yy.ravel()])
        np.testing.assert_allclose(output_wcs.all_world2pix(wcs.all_pix2world(points, 0), 0), points, atol=1e-5, rtol=0)
        scientific_output = {'sha256': sha(out/'calibrated.fits'), 'allPixelsExactlyEqual': True, 'wcsGridVerified': True}
    else:
        assert not (out/'calibrated.fits').exists(), 'Invalid review wrote a calibrated image'
    for path in paths:
        assert sha(path) == hashes[str(path.resolve())]
    result = dict(sourceSHA256=hashes, observationDateAssumption=args.observation_date,
                  catalogRows=len(catalog), availableSpectra=len(spectra), propagatedReferences=len(stars),
                  missingProperMotionIDs=[r['source_id'] for r in catalog if not r['pmra'] or not r['pmdec']],
                  filterAssumption='Ideal unit transmission from 400 to 700 nm; sensor support truncated at supplied endpoints. Not measured S30 Pro IRCUT.',
                  whiteReference='CALSPEC solar spectrum; F_lambda FLAM converted to W m^-2 nm^-1',
                  method='Independent exact polynomial integration, NumPy weighted plane/aperture/hold-out audit, SciPy centroid fit',
                  probeSeconds=elapsed, statuses=dict(Counter(s['status'] for s in report['stars'])),
                  maximumRelativePhotonIntegralDifference=max(flux_errors),
                  maximumRelativeApertureDifference=max(aperture_errors),
                  scipyCentroidCount=len(centroid_errors), maximumCentroidDifferencePixels=max(centroid_errors),
                  fit={k: v for k, v in report['fit'].items() if k != 'stars'},
                  review={k: v for k, v in review.items() if k not in ['stars', 'fitStars']},
                  reviewStatuses=dict(Counter(s['status'] for s in review['stars'])),
                  maximumRelativeExpandedApertureDifference=max(expanded_errors, default=None),
                  scientificOutput=scientific_output,
                  limitations=['No atmosphere or measured full-system throughput.', 'Neighbor checks are limited by catalog coverage and detected peaks; unresolved companions and stellar variability can remain.',
                               'Numerical agreement does not establish physical color accuracy.'])
    (out/'audit.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'sourceSHA256'}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
