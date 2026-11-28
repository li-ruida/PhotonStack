"""Independent CFA color-phase oracle through the public deep-sky command."""
import hashlib
import json
import math
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_channels import CLI, write, read
from test_sensor_master import cards


def recipe(root, paths, **overrides):
    values = {
        'stack.cfa-drizzle': 'on', 'stack.drizzle-scale': '2', 'stack.drizzle-pixfrac': '1',
        'stack.method': 'average', 'stack.median-mad': 'off', 'stack.rejection-maps': 'off',
        'stack.normalize-background': 'off', 'registration.mode': 'none',
        'registration.refine-centroids': 'off', 'registration.reference': '0',
        'selection.apply': 'off', 'selection.measure-stars': 'off', 'selection.measure-trails': 'off',
        'denoise.h':'0', 'denoise.multiscale-luminance':'0', 'denoise.multiscale-chroma':'0', 'denoise.background-strength':'0',
        'background.model': 'none', 'crop.coverage': '0', 'develop.align-channels': 'off', 'develop.stellar-balance': 'off',
    }
    values.update(overrides)
    path = root/'recipe.txt'
    path.write_text('PHOTONSTACK_DEEP_SKY_RECIPE 1\n'+''.join(k+' '+json.dumps(v)+'\n' for k,v in values.items())+
                    ''.join('input '+json.dumps(str(p))+'\n' for p in paths))
    return path


def execute(root, paths, name='result', ok=True, **overrides):
    r = subprocess.run([CLI, 'deep-sky', '--recipe', recipe(root,paths,**overrides),
                        '--output-dir', root/name, '--compute-backend', 'cpu'], text=True,capture_output=True)
    if ok and r.returncode:
        report=root/name/'report.json'
        detail=json.loads(report.read_text()) if report.exists() else {}
        raise AssertionError(str((detail.get('errorCode'),detail.get('message')))+r.stderr[-1000:])
    if not ok:
        if not r.returncode: raise AssertionError('Invalid CFA workflow succeeded')
        return r.stderr+r.stdout
    return json.loads((root/name/'report.json').read_text())


def fixtures(root, calibrated=False, same_phase=False):
    paths=[]; w,h=40,32
    patterns=['RGGB','GRBG','GBRG','BGGR']
    colors=[[0,1,1,2],[1,0,2,1],[1,2,0,1],[2,1,1,0]]
    for i,pattern in enumerate(patterns):
        phase=0 if same_phase else i
        values=[[-25,100,300][colors[phase][(y%2)*2+x%2]]+x*.25+y*.5 for y in range(h) for x in range(w)]
        extra=cards('Light', BAYERPAT=repr(patterns[phase]), PSCOLOR=None,
                    **{'DATE-OBS':f"'2026-10-15T22:0{i}:00'"})
        extra.update(CTYPE1="'RA---TAN'", CTYPE2="'DEC--TAN'", RADESYS="'ICRS'",
                     CRPIX1=20, CRPIX2=16, CRVAL1=30, CRVAL2=40,
                     CD1_1=-.001, CD1_2=0, CD2_1=0, CD2_2=.001)
        if calibrated: extra.update(PSCALIB="'sensor-v1'",PSCOLOR="'Linear'")
        path=root/f'light-{i}.fits';write(path,values,w,h,extra);paths.append(path)
    return paths


class Drizzle(unittest.TestCase):
    def test_preflight_checks_each_bayer_header_without_creating_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = fixtures(root)
            output = root / 'planned'
            command = [CLI, 'deep-sky', '--preflight', '--recipe', recipe(root, paths), '--output-dir', output]
            plan = json.loads(subprocess.check_output(command, text=True))
            self.assertEqual(plan['issues'], [])
            self.assertEqual(plan['inspectedCount'], len(paths))
            self.assertIsNotNone(plan['stackBufferBytes'])
            header, planes = read(paths[1])
            header['PSCOLOR'] = "'SRGB-DISPLAY'"
            write(paths[1], planes[0], 40, 32, header)
            plan = json.loads(subprocess.check_output(command, text=True))
            self.assertEqual([(i['code'], i['path']) for i in plan['issues']], [('CfaSourceInvalid', str(paths[1]))])
            self.assertIsNone(plan['stackBufferBytes'])
            self.assertFalse(output.exists())

    def test_color_phase_signed_samples_scaled_wcs_and_no_source_changes(self):
        for calibrated in (False,True):
            with tempfile.TemporaryDirectory() as d:
                root=Path(d);paths=fixtures(root,calibrated)
                hashes=[hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]
                report=execute(root,paths)
                self.assertEqual(report['selectedCount'],4)
                self.assertEqual(report['drizzleCoverage'],[1,1,1])
                self.assertEqual(report['drizzleCompleteCoverage'],1)
                header,planes=read(root/'result/stack-linear.fits')
                self.assertEqual((int(header['NAXIS1']),int(header['NAXIS2'])),(80,64))
                self.assertAlmostEqual(float(header['CRPIX1']),39.5)
                self.assertAlmostEqual(float(header['CRPIX2']),31.5)
                self.assertAlmostEqual(float(header['CD1_1']),-.0005)
                weights=read(root/'result/drizzle-weights.fits')[1]
                for y in range(64):
                    for x in range(80):
                        p=y*80+x
                        for c in range(3):
                            truth=[-25,100,300][c]+(x//2)*.25+(y//2)*.5
                            self.assertAlmostEqual(planes[c][p],truth,places=5)
                            self.assertAlmostEqual(weights[c][p],.5 if c==1 else .25)
                self.assertEqual(hashes,[hashlib.sha256(p.read_bytes()).hexdigest() for p in paths])
                self.assertIn('rejection none',Path(report['drizzleProvenance']).read_text())

    def test_integer_blank_stays_missing_and_nonlinear_input_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); paths=fixtures(root)
            header,planes=read(paths[0])
            header.update(BITPIX='16',BSCALE='.25',BZERO='0',BLANK='-32768')
            text=''.join((f'{k:<8}= {v:>20}').ljust(80) for k,v in header.items())+'END'
            text=text.ljust(((len(text)+2879)//2880)*2880)
            values=[-32768]+[round(v*4) for v in planes[0][1:]]
            data=struct.pack(f'>{len(values)}h',*values)
            paths[0].write_bytes(text.encode()+data.ljust(((len(data)+2879)//2880)*2880,b'\0'))
            report=execute(root,paths)
            _,science=read(root/'result/stack-linear.fits')
            _,weights=read(root/'result/drizzle-weights.fits')
            for p in (0,1,80,81):
                self.assertTrue(math.isnan(science[0][p]))
                self.assertEqual(science[3][p],0)
                self.assertEqual(weights[0][p],0)
                self.assertGreater(weights[1][p],0)
            self.assertAlmostEqual(report['drizzleCompleteCoverage'],1-1/1280)
            header,planes=read(paths[1]); header['PSCOLOR']="'SRGB-DISPLAY'"
            write(paths[1],planes[0],40,32,header)
            execute(root,paths,name='nonlinear',ok=False)

    def test_missing_color_is_not_filled_and_unsupported_options_fail(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);paths=fixtures(root,same_phase=True)
            error=execute(root,paths,ok=False)
            self.assertIn('No pixels have all three colors',error)
            _,planes=read(root/'result/stack-linear.fits')
            self.assertTrue(all(v==0 for v in planes[3]))
            self.assertTrue(any(math.isnan(v) for v in planes[0]))
            for i,change in enumerate([{'stack.method':'sigma'}, {'stack.normalize-background':'on'},
                                       {'registration.mode':'distortion'}, {'stack.drizzle-scale':'10'}]):
                execute(root,paths,name=f'invalid-{i}',ok=False,**change)


if __name__=='__main__': unittest.main()
