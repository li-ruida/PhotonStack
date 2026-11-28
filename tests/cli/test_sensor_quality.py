"""Independent phase statistics and defect-mask application checks."""
import hashlib
import json
import math
import statistics
import tempfile
import unittest
from pathlib import Path
from test_channels import run, read
from test_sensor_master import frame
from test_calibration_job import manifest

class Quality(unittest.TestCase):
    def test_cfa_response_and_masked_calibration_resume(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            values=[[100,200,300,400][2*((p//12)%2)+p%2] for p in range(120)]
            values[0]=0;values[1]=1000;values[2]=math.nan
            flat=frame(root/'flat.fits',values,'Master Flat',PSBIAS="'removed'")
            original=flat.read_bytes()
            result=run('sensor-quality','--input',flat,'--output-dir',root/'quality')
            self.assertEqual(result['rejected'],3)
            self.assertEqual([p['median'] for p in result['phases']],[100,200,300,400])
            mask=root/'quality/bad-pixels.fits'
            self.assertEqual(list(read(mask)[1][0]),[1 if p in (0,1,2) else 0 for p in range(120)])
            self.assertEqual(flat.read_bytes(),original)
            bias=frame(root/'bias.fits',[10]*120,'Master Bias',EXPTIME=0,PSBIAS="'included'")
            lights=[frame(root/f'light-{i}.fits',[100+i]*120,'Light',**{'DATE-OBS':f"'2026-10-15T22:0{i}:00'"}) for i in range(3)]
            m=manifest(root,[(p,'b','-','-') for p in lights],[('b','bias',bias,'included')],
                       settings=f'bad-pixels {json.dumps(str(mask))} {hashlib.sha256(mask.read_bytes()).hexdigest()}\n')
            first=run('calibration-run','--manifest',m,'--output-dir',root/'job','--max-frames',1)
            saved=(root/'job/frame-000001/calibrated.fits').read_bytes()
            done=run('calibration-resume','--job',root/'job')
            self.assertEqual(done['completed'],3);self.assertEqual(done['badPixels']['sha'],first['badPixels']['sha'])
            self.assertEqual(saved,(root/'job/frame-000001/calibrated.fits').read_bytes())
            for i in range(3):
                pixels=read(root/f'job/frame-{i+1:06}/calibrated.fits')[1][0]
                self.assertTrue(all(math.isnan(pixels[p]) if p in (0,1,2) else pixels[p]==90+i for p in range(120)))
            wrong=frame(root/'wrong-filter.fits',[100]*120,'Light',FILTER="'OIII'")
            bad=manifest(root,[(wrong,'b','-','-')],[('b','bias',bias,'included')],
                         settings=f'bad-pixels {json.dumps(str(mask))} {hashlib.sha256(mask.read_bytes()).hexdigest()}\n')
            run('calibration-run','--manifest',bad,'--output-dir',root/'wrong-job',ok=False)
            self.assertFalse((root/'wrong-job').exists())
            mask.rename(root/'moved-map.fits')
            self.assertEqual(run('calibration-job-inspect','--job',root/'job')['completed'],3)
            run('calibration-resume','--job',root/'job',ok=False)

    def test_phase_mad_matches_independent_statistics_and_identity_is_checked(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); values=[100+((p//2)%11) for p in range(120)]; values[9]=10000
            dark=frame(root/'dark.fits',values,'Master Dark')
            report=run('sensor-quality','--input',dark,'--output-dir',root/'q','--sigma',6,'--absolute-tolerance',2)
            expected=[]
            for q,stats in enumerate(report['phases']):
                phase=[v for p,v in enumerate(values) if 2*((p//12)%2)+p%2==q]
                median=statistics.median(phase);sigma=1.482602218505602*statistics.median([abs(v-median) for v in phase])
                self.assertAlmostEqual(stats['median'],median);self.assertAlmostEqual(stats['madSigma'],sigma)
                expected.append((median,max(2,6*sigma)))
            mask=read(root/'q/bad-pixels.fits')[1][0]
            for p,v in enumerate(values):
                m,t=expected[2*((p//12)%2)+p%2];self.assertEqual(mask[p],int(abs(v-m)>t))
            bias=frame(root/'bias.fits',[10]*120,'Master Bias',EXPTIME=0,OFFSET=21,PSBIAS="'included'")
            light=frame(root/'light.fits',[100]*120,'Light',OFFSET=21)
            m=manifest(root,[(light,'b','-','-')],[('b','bias',bias,'included')],settings=f'bad-pixels {json.dumps(str(root/"q/bad-pixels.fits"))} -\n')
            run('calibration-run','--manifest',m,'--output-dir',root/'incompatible',ok=False)
            self.assertFalse((root/'incompatible').exists())
            run('sensor-quality','--input',light,'--output-dir',root/'notmaster',ok=False)

if __name__=='__main__': unittest.main()
