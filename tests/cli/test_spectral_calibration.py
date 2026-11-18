"""Independent analytic spectrum / TAN / Gaussian FITS and saved-SPCC workflow oracle."""
import hashlib
import math
from pathlib import Path
import struct
import tempfile
import unittest
from test_channels import read, write, run


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def tiff16(path):
    data=path.read_bytes(); endian='<' if data[:2]==b'II' else '>'
    assert data[:2] in [b'II', b'MM'] and struct.unpack_from(endian+'H',data,2)[0]==42
    offset=struct.unpack_from(endian+'I',data,4)[0]; tags={}
    for i in range(struct.unpack_from(endian+'H',data,offset)[0]):
        at=offset+2+12*i; tag, kind, count=struct.unpack_from(endian+'HHI',data,at)
        if kind not in [3,4]: continue
        size, code=(2,'H') if kind==3 else (4,'I')
        ptr=at+8 if size*count<=4 else struct.unpack_from(endian+'I',data,at+8)[0]
        tags[tag]=struct.unpack_from(endian+str(count)+code,data,ptr)
    assert tags[259]==(1,) and set(tags[258])=={16} and tags.get(284,(1,))==(1,)
    payload=b''.join(data[o:o+n] for o,n in zip(tags[273],tags[279]))
    values=struct.unpack(endian+str(len(payload)//2)+'H',payload)
    return tags[256][0],tags[257][0],tags[277][0],values


def integral(a, b, low, high):
    slope = (b-a)/400
    return (a-350*slope)*(high*high-low*low)/2+slope*(high**3-low**3)/3


def fixture(root, count=64):
    bands = [(600, 700), (500, 600), (400, 500)]
    profile = ['PhotonStack-SpectralProfile-1', '"Analytic test" "broadband" "synthetic" "Analytic top-hat responses" "CC0"']
    for lo, hi in bands: profile += ['1', '2', f'{lo} 1', f'{hi} 1']
    profile += ['"Flat F_lambda" "Analytic constant spectrum" "CC0"', '2', '350 2', '750 2']
    (root/'profile.txt').write_text('\n'.join(profile)+'\n')
    catalog = ['PhotonStack-SpectralCatalog-1', '"Synthetic stars" 2026.5 "Independent inverse TAN and analytic spectra" "CC0"', str(count)]
    width = height = 360
    planes = [[-10+.001*x-.002*y+c for y in range(height) for x in range(width)] for c in range(3)]
    gain = [1.4, 1, .7]
    for i in range(64):
        x, y = 40.2+40*(i % 8), 40.3+40*(i//8)
        a, b = 1+.05*(i % 8), 1+.05*(i//8)
        xi, eta = math.radians((x+1-180)*-.001), math.radians((y+1-180)*.001)
        dec = math.radians(20)
        denominator = math.cos(dec)-eta*math.sin(dec)
        ra = 30+math.degrees(math.atan2(xi, denominator))
        de = math.degrees(math.atan2(math.sin(dec)+eta*math.cos(dec), math.hypot(denominator, xi)))
        if i < count:
            catalog += [f'"{i}" {ra:.17g} {de:.17g} 2', f'350 {a:.17g} .001', f'750 {b:.17g} .001']
        amplitudes = [integral(a, b, lo, hi)/(300*g) for (lo, hi), g in zip(bands, gain)]
        for dy in range(-12, 13):
            for dx in range(-12, 13):
                px, py = int(x)+dx, int(y)+dy
                g = math.exp(-((px-x)**2+(py-y)**2)/4.5)
                for c in range(3): planes[c][py*width+px] += amplitudes[c]*g
    (root/'catalog.txt').write_text('\n'.join(catalog)+'\n')
    coverage = [1.]*(width*height); coverage[1] = .25
    values = sum(planes, [])+coverage
    wcs = {'NAXIS': 3, 'NAXIS3': 4, 'PSCOLOR': "'LINEAR-SRGB'", 'CTYPE1': "'RA---TAN'", 'CTYPE2': "'DEC--TAN'",
           'RADESYS': "'ICRS'", 'CRPIX1': 180, 'CRPIX2': 180, 'CRVAL1': 30, 'CRVAL2': 20,
           'CD1_1': -.001, 'CD1_2': 0, 'CD2_1': 0, 'CD2_2': .001}
    write(root/'source.fits', values, width, height, wcs)
    return gain, bands


def calibrate(root, name='result', *options, ok=True):
    return run('spcc', '--input', root/'source.fits', '--profile', root/'profile.txt', '--catalog', root/'catalog.txt',
               '--output-dir', root/name, *options, ok=ok)


class SpectralCalibration(unittest.TestCase):
    def test_scientific_pixels_reopen_replay_export_and_shared_display(self):
        with tempfile.TemporaryDirectory(prefix='spectral space ') as d:
            root = Path(d); gains, bands = fixture(root)
            original = sha(root/'source.fits')
            result = calibrate(root)['report']; fit = result['fit']
            self.assertTrue(fit['valid']); self.assertEqual(fit['training']+fit['validation'], 64)
            self.assertEqual(fit['validation'], 12); self.assertGreaterEqual(fit['regions'], 4)
            for c in range(3):
                self.assertAlmostEqual(fit['instrumentGains'][c], gains[c], delta=3e-7)
            white = [integral(2, 2, *b) for b in bands]
            for actual, g, w in zip(fit['gains'], gains, white):
                self.assertAlmostEqual(actual, g/(w/white[1]), delta=3e-7)
            h, before = read(root/'source.fits'); output_header, after = read(root/'result/calibrated.fits')
            self.assertEqual(after[3], before[3]); self.assertLess(after[0][0], 0)
            for c in range(3):
                expected = tuple(struct.unpack('>f', struct.pack('>f', v*fit['gains'][c]))[0] for v in before[c])
                self.assertEqual(after[c], expected)
            for k in ['CTYPE1', 'CTYPE2', 'CRPIX1', 'CRPIX2', 'CRVAL1', 'CRVAL2', 'CD1_1', 'CD2_2', 'PSCOLOR']:
                if k.startswith(('CR', 'CD')): self.assertEqual(float(h[k]), float(output_header[k]))
                else: self.assertEqual(h[k].strip("' "), output_header[k].strip("' "))
            inspected = run('spcc-inspect', '--recipe', root/'result/recipe.txt', '--expected-sha', original)
            self.assertEqual(result, inspected['report'])
            run('spcc-replay', '--recipe', root/'result/recipe.txt', '--output-dir', root/'replay')
            self.assertEqual({p.name: p.read_bytes() for p in (root/'result').iterdir()},
                             {p.name: p.read_bytes() for p in (root/'replay').iterdir()})
            run('spcc-export', '--recipe', root/'result/recipe.txt', '--output', root/'export.fits')
            self.assertEqual((root/'export.fits').read_bytes(), (root/'result/calibrated.fits').read_bytes())
            run('spcc-preview', '--recipe', root/'result/recipe.txt', '--output', root/'display.tiff')
            self.assertEqual((root/'display.tiff').read_bytes(), (root/'result/after.tiff').read_bytes())
            run('spcc-preview', '--recipe', root/'result/recipe.txt', '--output', root/'full.tiff', '--full-resolution', '1')
            self.assertEqual((root/'full.tiff').read_bytes(), (root/'result/after.tiff').read_bytes())
            run('spcc-preview', '--recipe', root/'result/recipe.txt', '--output', root/'bad.tiff', '--full-resolution', '2', ok=False)
            run('spcc-preview', '--recipe', root/'result/recipe.txt', '--output', root/'display.tiff', ok=False)
            run('spcc-preview', '--recipe', root/'result/recipe.txt', '--output', root/'wrong.fits', ok=False)
            self.assertEqual(sha(root/'source.fits'), original)
            for p in ['before.tiff', 'after.tiff']: self.assertGreater((root/'result'/p).stat().st_size, 1000)
            self.assertIn('shared', result['preview']['transfer'])
            low,high=result['preview']['low'],result['preview']['high']
            span=max(high-low,1e-12)
            for name,planes,applied in [('before',before,[1,1,1]),('after',after,fit['gains'])]:
                w,h,n,values=tiff16(root/'result'/(name+'.tiff'))
                self.assertEqual((w,h,n),(360,360,4))
                maximum=0
                for i in range(w*h):
                    if planes[3][i] < 1: continue
                    for c in range(3):
                        value=math.asinh(10*max(0,(planes[c][i]-low*applied[c])/span))/math.asinh(10)
                        expected=round(max(0,min(1,value))*65535)
                        maximum=max(maximum,abs(values[i*n+c]-expected))
                self.assertLessEqual(maximum,1, 'preview does not use one transformed black reference and shared scale')

    def test_invalid_inputs_exclusions_and_existing_outputs(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); fixture(root)
            for i, options in enumerate([('--radius', '20'), ('--min-snr', 'nan'), ('--exclude', 'missing'),
                                         ('--exclude', '0,0'), ('--exclude', '0,'), ('--expected-sha', '0'*64),
                                         ('--radius', '8', '--radius', '9'), ('--recipe', 'unexpected'),
                                         ('--max-center-offset', '0'), ('--max-spectral-error', '.6'),
                                         ('--max-aperture-color-change', '.3'), ('--max-validation-log-error', 'nan')]):
                calibrate(root, str(i), *options, ok=False)
                self.assertFalse((root/str(i)).exists())
            report=calibrate(root, 'excluded', '--exclude', '0,7')['report']
            self.assertTrue(report['fit']['valid'])
            self.assertEqual([s['id'] for s in report['stars'] if s['status']=='excluded'], ['0', '7'])
            limits=calibrate(root, 'limits', '--max-center-offset', '1.5', '--max-spectral-error', '.08',
                             '--max-aperture-color-change', '.025', '--max-validation-log-error', '.18')['report']
            self.assertTrue(limits['fit']['valid'])
            for k,v in [('maxCenterOffset',1.5),('maxRelativeSpectralError',.08),('maxApertureColorChange',.025),('maxValidationLogError',.18)]:
                self.assertEqual(limits['options'][k],v)
            self.assertEqual(run('spcc-inspect', '--recipe', root/'limits/recipe.txt')['report'],limits)
            baseline={p.name: p.read_bytes() for p in (root/'excluded').iterdir()}
            calibrate(root, 'excluded', ok=False)
            self.assertEqual(baseline, {p.name: p.read_bytes() for p in (root/'excluded').iterdir()})
            for name, change in [('profile.txt', lambda s: s.replace('600 1', '600 -1')),
                                 ('profile.txt', lambda s: s+'unexpected\n'),
                                 ('catalog.txt', lambda s: s.replace('350 1 .001', '350 1 -1')),
                                 ('catalog.txt', lambda s: s.replace('"1" ', '"0" ', 1))]:
                path=root/name; old=path.read_text(); path.write_text(change(old))
                calibrate(root, 'invalid', ok=False); self.assertFalse((root/'invalid').exists()); path.write_text(old)
            source=root/'source.fits'; old=source.read_bytes(); header, planes=read(source)
            wcs={k:v for k,v in header.items() if k not in ['SIMPLE','BITPIX','NAXIS','NAXIS1','NAXIS2','NAXIS3']}
            write(source, planes[0], 360, 360, wcs)
            self.assertIn('RGB', calibrate(root, 'mono', ok=False)); self.assertFalse((root/'mono').exists())
            write(source, sum(map(list, planes), []), 360, 360, dict(wcs, NAXIS=3, NAXIS3=4, PSCOLOR="'SRGB'"))
            calibrate(root, 'display', ok=False); self.assertFalse((root/'display').exists()); source.write_bytes(old)

    def test_failed_review_is_inspectable_and_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); fixture(root, 10)
            report=calibrate(root)['report']; self.assertFalse(report['fit']['valid'])
            self.assertFalse((root/'result/calibrated.fits').exists()); self.assertFalse((root/'result/after.tiff').exists())
            self.assertEqual(run('spcc-inspect','--recipe',root/'result/recipe.txt')['report'], report)
            run('spcc-export','--recipe',root/'result/recipe.txt','--output',root/'no.fits',ok=False)
            run('spcc-preview','--recipe',root/'result/recipe.txt','--output',root/'no.tiff',ok=False)
            self.assertFalse((root/'no.fits').exists())
            run('spcc-replay','--recipe',root/'result/recipe.txt','--output-dir',root/'rejected-replay')
            for file in ['source.fits', 'result/profile.txt', 'result/catalog.txt', 'result/report.json', 'result/before.tiff']:
                path=root/file; old=path.read_bytes(); path.write_bytes(old+b'changed')
                run('spcc-inspect','--recipe',root/'result/recipe.txt',ok=False)
                run('spcc-preview','--recipe',root/'result/recipe.txt','--output',root/'no.tiff',ok=False)
                run('spcc-replay','--recipe',root/'result/recipe.txt','--output-dir',root/'no',ok=False)
                self.assertFalse((root/'no').exists()); path.write_bytes(old)
            path=root/'result/recipe.txt'; old=path.read_text(); path.write_text(old.replace('8 11 15 0 20 2', '8 11 15 0 21 2'))
            self.assertNotEqual(old, path.read_text())
            run('spcc-inspect','--recipe',path,ok=False)


if __name__=='__main__': unittest.main()
