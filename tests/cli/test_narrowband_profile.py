"""Independent piecewise polynomial oracle; no native-generated curves form the truth."""
import math
from pathlib import Path
import struct
import tempfile
import unittest
from test_channels import read, run
from test_spectral_calibration import fixture, sha

QE = [([350, 540, 750], [.25, .4, .8]),
      ([350, 510, 750], [.2, .85, .15]),
      ([350, 450, 600, 750], [.3, .9, .2, .1])]
OPTIC = ([350, 750], [.8, .6])
CENTERS = [656.3, 500.7, 500.7]
WIDTHS = [7., 6., 6.]


def at(curve, x):
    nodes, values = curve
    for i in range(len(nodes)-1):
        if nodes[i] <= x <= nodes[i+1]:
            t = (x-nodes[i])/(nodes[i+1]-nodes[i])
            return values[i]*(1-t)+values[i+1]*t
    return 0.


def window(center, width):
    lo, hi = center-width/2, center+width/2
    return ([lo, lo+width*1e-4, hi-width*1e-4, hi], [0., 1., 1., 0.])


def band(layout, c):
    detector = QE[c]
    if layout == 'extracted-hoo' and c != 0:
        knots = sorted(set(QE[1][0]+QE[2][0]))
        detector = (knots, [(2*at(QE[1], x)+at(QE[2], x))/3 for x in knots])
    windows = [window(CENTERS[i], WIDTHS[i]) for i in ([c] if layout == 'mono' else [1, 0])]
    return [detector, OPTIC, (sum([w[0] for w in windows], []), sum([w[1] for w in windows], []))]


def integrate(curves, power=0):
    """Multiply local polynomials in t=(lambda-low)/(high-low), integrate coefficients."""
    low, high = max(c[0][0] for c in curves), min(c[0][-1] for c in curves)
    knots = sorted({low, high} | {x for c in curves for x in c[0] if low < x < high})
    terms = []
    for lo, hi in zip(knots, knots[1:]):
        polynomial = [1.]
        factors = [[at(c, lo), at(c, hi)-at(c, lo)] for c in curves] + [[lo, hi-lo]]*power
        for f in factors:
            product = [0.]*(len(polynomial)+1)
            for i, v in enumerate(polynomial):
                for j, w in enumerate(f): product[i+j] += v*w
            polynomial = product
        terms.append((hi-lo)*math.fsum(v/(i+1) for i, v in enumerate(polynomial)))
    return math.fsum(terms)


def base(path, optics=None):
    lines = ['PhotonStack-SpectralProfile-1', '"Base" "broadband" "measured" "Independent test response" "CC0"']
    for c in range(3):
        lines += ['2']
        for xs, ys in [QE[c], (optics or [OPTIC]*3)[c]]:
            lines += [str(len(xs))] + [f'{x:.17g} {y:.17g}' for x, y in zip(xs, ys)]
    lines += ['"Flat white" "Constant F_lambda" "CC0"', '2', '350 2', '750 2']
    path.write_text('\n'.join(lines)+'\n')


def generate(root, layout='mono', **replace):
    options = {'--profile': root/'profile.txt', '--layout': layout,
               '--centers': ','.join(map(str, CENTERS)), '--widths': ','.join(map(str, WIDTHS)),
               '--name': 'Nominal narrowband', '--output-dir': root/layout}
    ok = replace.pop('ok', True)
    options.update(replace)
    return run('spcc-narrowband-profile', *[v for kv in options.items() for v in kv], ok=ok)


class NarrowbandProfile(unittest.TestCase):
    def test_all_layouts_against_independent_polynomials_and_read_only_sources(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); base(root/'profile.txt')
            license = root/'response-source-license.txt'; license.write_text('Test license\n')
            original = {p.name: p.read_bytes() for p in root.iterdir()}
            for layout in ['mono', 'osc-dual', 'extracted-hoo']:
                output = generate(root, layout)
                path = Path(output['profile']); self.assertEqual(output['sha'], sha(path))
                report = run('spcc-profile-inspect', '--profile', path)
                self.assertEqual(report['profile']['mode'], 'narrowband')
                self.assertEqual(report['profile']['quality'], 'approximate')
                model = report['profile']['narrowband']
                self.assertEqual(model, dict(layout=layout, baseSHA=sha(root/'profile.txt'),
                    model='trapezoid-edge-1e-4', centers=CENTERS, widths=WIDTHS))
                for c, result in enumerate(report['bands']):
                    curves = band(layout, c); area = integrate(curves)
                    mean = integrate(curves, 1)/area
                    rms = math.sqrt(integrate(curves, 2)/area-mean*mean)
                    self.assertAlmostEqual(result['meanWavelength'], mean, delta=2e-10)
                    self.assertAlmostEqual(result['rmsBandwidth'], rms, delta=2e-8)
                    self.assertAlmostEqual(result['whiteFlux'], integrate(curves, 1)*2, delta=1e-8)
                    self.assertEqual(result['components'], 3)
                    for x, y in result['samples']:
                        self.assertAlmostEqual(y, math.prod(at(curve, x) for curve in curves), delta=3e-15)
                if layout == 'extracted-hoo': self.assertEqual(report['bands'][1], report['bands'][2])
                self.assertEqual((path.parent/'base-profile.txt').read_bytes(), original['profile.txt'])
                self.assertEqual((path.parent/license.name).read_bytes(), original[license.name])
                # Self-contained science after removing the optional base snapshot.
                (path.parent/'base-profile.txt').unlink()
                self.assertEqual(run('spcc-profile-inspect', '--profile', path), report)
            for name, content in original.items(): self.assertEqual((root/name).read_bytes(), content)

    def test_invalid_requests_models_and_existing_output(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); base(root/'profile.txt')
            for options in [ {'--layout':'other'}, {'--widths':'2,6,6'}, {'--widths':'101,6,6'},
                {'--widths':'7,nan,6'}, {'--centers':'336,500.7,500.7'}, {'--centers':'1020,500.7,500.7'},
                {'--centers':'700,500'}, {'--centers':'700,500,500 extra'}, {'--centers':'800,500,500'},
                {'--expected-sha':'0'*64}, {'--name':''}, {'--name':'   '}, {'--layout':'osc-dual','--centers':'501,500,500'},
                {'--layout':'osc-dual','--widths':'7,6,7'}, {'--layout':'extracted-hoo','--centers':'656,500,501'}]:
                generate(root, **options, ok=False)
                self.assertEqual(list(root.iterdir()), [root/'profile.txt'])
            generate(root)
            existing = {p.name: p.read_bytes() for p in (root/'mono').iterdir()}
            generate(root, ok=False)
            self.assertEqual(existing, {p.name: p.read_bytes() for p in (root/'mono').iterdir()})
            generate(root, **{'--profile':root/'mono/profile.txt','--output-dir':root/'twice'}, ok=False)
            self.assertFalse((root/'twice').exists())
            original = (root/'mono/profile.txt').read_text()
            for bad in [original.replace('"approximate"','"measured"'),
                        original.replace('656.29999999999995','655'), original+'trailing',
                        original.replace('PhotonStack-SpectralProfile-2','PhotonStack-SpectralProfile-1')]:
                self.assertNotEqual(bad, original)
                (root/'bad.txt').write_text(bad)
                run('spcc-profile-inspect', '--profile', root/'bad.txt', ok=False)
            base(root/'profile.txt', optics=[OPTIC, OPTIC, ([350,750],[.8,.5])])
            generate(root, 'extracted-hoo', ok=False)
            self.assertFalse((root/'extracted-hoo').exists())

    def test_known_gain_stars_scientific_pixels_wcs_replay_and_export(self):
        for layout in ['mono', 'osc-dual', 'extracted-hoo']:
            with self.subTest(layout=layout), tempfile.TemporaryDirectory() as d:
                root = Path(d)
                curves = [band(layout,c) for c in range(3)]
                def flux(a,b): return [integrate(band + [([350,750],[a,b])],1)*20 for band in curves]
                gains, _ = fixture(root, photon_flux=flux)
                base(root/'profile.txt'); generate(root,layout)
                original = sha(root/'source.fits')
                response = run('spcc', '--input',root/'source.fits', '--catalog',root/'catalog.txt',
                    '--profile',root/layout/'profile.txt','--output-dir',root/'result')['report']
                self.assertTrue(response['fit']['valid'], response['fit'])
                self.assertEqual(response['profile']['narrowband']['layout'],layout)
                self.assertEqual(response['fit']['training']+response['fit']['validation'],64)
                white = [integrate(band,1)*2 for band in curves]
                for c in range(3):
                    self.assertAlmostEqual(response['fit']['instrumentGains'][c],gains[c],delta=4e-7)
                    self.assertAlmostEqual(response['fit']['gains'][c],gains[c]*white[1]/white[c],delta=4e-7)
                header, before = read(root/'source.fits'); h, after = read(root/'result/calibrated.fits')
                for k in ['CRPIX1','CRPIX2','CRVAL1','CRVAL2','CD1_1','CD2_2','CTYPE1','CTYPE2']:
                    if k.startswith('CTYPE'): self.assertEqual(h[k], header[k])
                    else: self.assertEqual(float(h[k]), float(header[k]))
                self.assertEqual(before[3],after[3]); self.assertLess(after[0][0],0)
                for c in range(3):
                    expected = tuple(struct.unpack('>f',struct.pack('>f',v*response['fit']['gains'][c]))[0] for v in before[c])
                    self.assertEqual(after[c],expected)
                run('spcc-replay','--recipe',root/'result/recipe.txt','--output-dir',root/'replay')
                self.assertEqual({p.name:p.read_bytes() for p in (root/'result').iterdir()},
                                 {p.name:p.read_bytes() for p in (root/'replay').iterdir()})
                run('spcc-export','--recipe',root/'result/recipe.txt','--output',root/'export.fits')
                self.assertEqual(sha(root/'export.fits'),sha(root/'result/calibrated.fits'))
                self.assertEqual(sha(root/'source.fits'),original)


if __name__ == '__main__': unittest.main(argv=[__file__])
