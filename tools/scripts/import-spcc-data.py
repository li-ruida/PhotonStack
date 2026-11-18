#!/usr/bin/env python3
"""Import offline OSC response JSON, CALSPEC white and Gaia DR3 XP into SPCC inputs.

Requires NumPy and Astropy for CALSPEC FITS/time handling. Does not download data
or infer equipment/filter identity. Curves and their licenses remain user data.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil

import numpy as np
from astropy.io import fits
from astropy.time import Time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quote(text):
    text = str(text)
    if not text or len(text.encode()) > 4096 or any(ord(c) < 32 or ord(c) == 127 for c in text):
        raise ValueError('Invalid spectral metadata')
    return '"'+text.replace('\\', '\\\\').replace('"', '\\"')+'"'


def response_curve(record):
    units = {'nm': 1, 'angstrom': .1, 'angstroms': .1, 'micron': 1000, 'um': 1000, 'm': 1e9}
    wave, signal = record['wavelength'], record['values']
    x = np.asarray(wave['value'], dtype=float)*units[wave['units'].lower()]
    scale = float(signal['range'])
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError('Invalid response normalization range')
    y = np.asarray(signal['value'], dtype=float)/scale
    if len(x) < 2 or len(x) > 100000 or x.shape != y.shape or not np.isfinite(x).all() or not np.isfinite(y).all() or \
            not (np.diff(x) > 0).all() or x[0] <= 0 or x[-1] > 1e6 or (y < 0).any() or (y > 1).any():
        raise ValueError('Invalid response wavelengths or transmission')
    return x, y


def curve_text(x, y):
    return str(len(x))+'\n'+''.join(f'{a:.17g} {b:.17g}\n' for a, b in zip(x, y))


def propagate(ra, dec, pmra, pmdec, dt):
    # Normalize position + tangent velocity; no unknown distance, radial
    # velocity, parallax or atmospheric apparent-position terms are invented.
    ra, dec = math.radians(ra), math.radians(dec)
    p = np.array([math.cos(dec)*math.cos(ra), math.cos(dec)*math.sin(ra), math.sin(dec)])
    east = np.array([-math.sin(ra), math.cos(ra), 0.])
    north = np.array([-math.sin(dec)*math.cos(ra), -math.sin(dec)*math.sin(ra), math.cos(dec)])
    p += dt*math.radians(1/3.6e6)*(pmra*east+pmdec*north)
    return math.degrees(math.atan2(p[1], p[0])) % 360, math.degrees(math.atan2(p[2], math.hypot(p[0], p[1])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ['sensor', 'response-license-file', 'white-calspec', 'gaia-catalog', 'xp-dir', 'output-dir']:
        parser.add_argument('--'+key, type=Path, required=True)
    for key in ['profile-name', 'white-name', 'white-source', 'white-license', 'observation-date']:
        parser.add_argument('--'+key, required=True)
    filters = parser.add_mutually_exclusive_group(required=True)
    filters.add_argument('--filter', type=Path, help='One OSC_FILTER or OSC_LPF record in response JSON')
    filters.add_argument('--ideal-cutoff', nargs=2, type=float, metavar=('LOW_NM', 'HIGH_NM'), help='Explicit approximate top-hat, never treated as measured')
    args = parser.parse_args()
    out = args.output_dir
    if out.exists():
        raise ValueError('Output directory must be new')
    epoch = float(Time(args.observation_date).jyear)
    if not 1800 <= epoch <= 2200:
        raise ValueError('Observation epoch outside supported range')
    xp_paths = sorted(args.xp_dir.glob('xp-*.download'))
    if not xp_paths:
        raise ValueError('No offline Gaia XP sampled CSV files')
    paths = [args.sensor, args.response_license_file, args.white_calspec, args.gaia_catalog, *xp_paths]
    if args.filter:
        paths.append(args.filter)
    for path in paths:
        if not path.is_file() or path.stat().st_size > 256*1024*1024:
            raise ValueError('Invalid or oversized input file')
    hashes = {str(p.resolve()): sha(p) for p in paths}
    sensor = json.loads(args.sensor.read_text())
    bands = []
    sources = []
    for channel in ['RED', 'GREEN', 'BLUE']:
        records = [r for r in sensor if r.get('channel') == channel and r.get('type') == 'OSC_SENSOR']
        if len(records) != 1:
            raise ValueError('Select one OSC sensor with exactly three channels')
        bands.append([response_curve(records[0])])
        sources.append({k: records[0].get(k) for k in ['name', 'dataQualityMarker', 'dataSource']})
    if args.filter:
        records = json.loads(args.filter.read_text())
        if len(records) != 1 or records[0]['type'] not in ['OSC_FILTER', 'OSC_LPF']:
            raise ValueError('Select one common OSC filter curve')
        filter_curve = response_curve(records[0])
        assumption = 'Imported component curves; no measured full-system or atmospheric throughput is asserted.'
    else:
        low, high = args.ideal_cutoff
        if not (math.isfinite(low) and math.isfinite(high) and 336 <= low < high <= 1020):
            raise ValueError('Ideal filter must lie within Gaia XP sampled coverage')
        filter_curve = (np.array([low, high]), np.ones(2))
        assumption = f'Assumed ideal unit-transmission filter {low:g}..{high:g} nm; not a measured camera filter.'
    for band in bands:
        band.append(filter_curve)
    with fits.open(args.white_calspec) as file:
        if file[1].columns['WAVELENGTH'].unit != 'ANGSTROMS' or file[1].columns['FLUX'].unit != 'FLAM':
            raise ValueError('CALSPEC input must declare ANGSTROMS and FLAM')
        wx = np.array(file[1].data['WAVELENGTH'], dtype=float)/10
        wy = np.array(file[1].data['FLUX'], dtype=float)*.01
        if wx.ndim != 1 or not 2 <= len(wx) <= 100000 or not np.isfinite(wx).all() or not np.isfinite(wy).all() or \
                not (np.diff(wx) > 0).all():
            raise ValueError('Invalid CALSPEC wavelength or flux samples')
    for band in bands:
        low, high = max(c[0][0] for c in band), min(c[0][-1] for c in band)
        if not (low < high and wx[0] <= low and wx[-1] >= high and low >= 336 and high <= 1020):
            raise ValueError('White or Gaia spectrum does not cover the selected response')
    spectra = {}
    spectrum_positions = {}
    for path in xp_paths:
        with path.open() as f:
            for row in csv.DictReader(f):
                sid = row['source_id']
                if sid in spectra:
                    raise ValueError('Duplicate Gaia spectrum')
                if len(spectra) >= 10000:
                    raise ValueError('At most 10000 Gaia spectra are supported')
                arrays = []
                for column in ['flux', 'flux_error']:
                    text = row[column]
                    if not (text.startswith('(') and text.endswith(')')):
                        raise ValueError('Expected Gaia sampled CSV array')
                    values = np.array([float(x) for x in text[1:-1].split(',')])
                    if len(values) != 343 or not np.isfinite(values).all():
                        raise ValueError('Expected 343 finite Gaia samples at 336..1020 nm')
                    arrays.append(values)
                if (arrays[1] < 0).any():
                    raise ValueError('Negative spectral uncertainty')
                spectra[sid] = arrays
                position = [float(row[k]) for k in ['ra', 'dec']]
                if not all(math.isfinite(v) for v in position) or not (0 <= position[0] < 360 and -90 <= position[1] <= 90):
                    raise ValueError('Invalid XP source position')
                spectrum_positions[sid] = position
    with args.gaia_catalog.open() as f:
        catalog = list(csv.DictReader(f))
    if not 1 <= len(catalog) <= 10000:
        raise ValueError('At most 10000 catalog rows are supported')
    rows, skipped, seen = [], [], set()
    for row in catalog:
        sid = row['source_id']
        if not sid.isascii() or not sid.isdigit() or len(sid) > 32 or sid in seen:
            raise ValueError('Invalid or duplicate Gaia source ID')
        seen.add(sid)
        if sid not in spectra or not row['pmra'] or not row['pmdec']:
            skipped.append({'id': sid, 'reason': 'spectrum-missing' if sid not in spectra else 'proper-motion-missing'})
            continue
        ra, dec, ma, md = [float(row[k]) for k in ['ra', 'dec', 'pmra', 'pmdec']]
        if not all(math.isfinite(v) for v in [ra, dec, ma, md]) or not (0 <= ra < 360 and -90 <= dec <= 90):
            raise ValueError('Invalid Gaia astrometry')
        xp_ra, xp_dec = spectrum_positions[sid]
        dra = math.remainder(xp_ra-ra, 360)*math.cos(math.radians(dec))
        if math.hypot(dra, xp_dec-dec) > 1e-6:
            raise ValueError('XP source position does not match Gaia catalog ID')
        ra, dec = propagate(ra, dec, ma, md, epoch-2016)
        rows.append((sid, ra, dec))
    if not rows:
        raise ValueError('No Gaia references with spectra and proper motion')
    aggregate = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
    source_note = f'Sensor SHA256 {sha(args.sensor)}; ' + assumption + ' OEM source: ' + str(sources[0]['dataSource'])
    license_note = f'External user response data; original license SHA256 {sha(args.response_license_file)}; retain response-source-license.txt from this import.'
    profile = 'PhotonStack-SpectralProfile-1\n'+' '.join(quote(x) for x in [args.profile_name, 'broadband', 'approximate', source_note, license_note])+'\n'
    for band in bands:
        profile += str(len(band))+'\n'+''.join(curve_text(x, y) for x, y in band)
    white_source = f'{args.white_source}; SHA256 {sha(args.white_calspec)}; ANGSTROMS/FLAM converted to nm/W m^-2 nm^-1.'
    profile += ' '.join(quote(x) for x in [args.white_name, white_source, args.white_license])+'\n'+curve_text(wx, wy)
    provenance = f'Gaia DR3 XP sampled; ICRS J2016 propagated by normalized tangent velocity to supplied epoch, no radial/parallax terms. Input manifest SHA256 {aggregate}; https://gea.esac.esa.int/'
    references = 'PhotonStack-SpectralCatalog-1\n'+quote('Gaia DR3 XP')+f' {epoch:.17g} '+quote(provenance)+' '+quote('ESA Gaia DR3; acknowledge ESA Gaia and DPAC data release')+'\n'+str(len(rows))+'\n'
    wavelengths = np.arange(336, 1021, 2)
    for sid, ra, dec in rows:
        references += quote(sid)+f' {ra:.17g} {dec:.17g} 343\n'
        references += ''.join(f'{w} {f:.17g} {e:.17g}\n' for w, f, e in zip(wavelengths, *spectra[sid]))
    for path in paths:
        if sha(path) != hashes[str(path.resolve())]:
            raise RuntimeError('Input changed during spectral import')
    manifest = dict(schema=1, inputs=hashes, inputManifestSHA256=aggregate, observationDate=args.observation_date,
                    epoch=epoch, responseMetadata=sources, responseAssumption=assumption, skipped=skipped,
                    references=len(rows), note='Native spcc validates curves and photometry again. No physical response accuracy is inferred.')
    out.mkdir(parents=True)
    try:
        (out/'profile.txt').write_text(profile)
        (out/'catalog.txt').write_text(references)
        shutil.copy2(args.response_license_file, out/'response-source-license.txt')
        manifest['outputs'] = {p.name: sha(p) for p in out.iterdir()}
        (out/'import.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    except Exception:
        for name in ['profile.txt', 'catalog.txt', 'response-source-license.txt', 'import.json']:
            (out/name).unlink(missing_ok=True)
        out.rmdir()
        raise
    print(json.dumps({'directory': str(out.resolve()), 'references': len(rows), 'skipped': skipped}, ensure_ascii=False))


if __name__ == '__main__':
    main()
