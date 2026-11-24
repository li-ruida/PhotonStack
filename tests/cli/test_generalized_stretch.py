"""GHS scientific/display boundary, exact cases, provenance, preview and replay."""
import hashlib
import math
from pathlib import Path
import tempfile
import unittest
from test_channels import read,write,run

class GHS(unittest.TestCase):
    def test_curve_domains_range_replay(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source=p/'source.fits';values=[-4+i*2 for i in range(48)]
            write(source,values,extra={'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'CRPIX1':4,'CRPIX2':3,'CRVAL1':313.6,'CRVAL2':30,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001})
            sha=hashlib.sha256(source.read_bytes()).hexdigest()
            r=run('ghs-stretch','--input',source,'--output-dir',p/'result','--log-stretch',2,'--local-intensity',1)
            self.assertEqual((r['black'],r['white']),(-4,90));self.assertEqual(r['sourceDomain'],'linear')
            self.assertEqual(r['coveredPixels'],48);self.assertEqual(sum(r['afterHistogram']),48)
            header,out=read(p/'result/display.fits');self.assertEqual(header['PSCOLOR'],"'SRGB-DISPLAY'");self.assertIn('CTYPE1',header)
            D=math.expm1(2)
            for x,y in zip(values,out[0]):
                n=(x+4)/94;self.assertAlmostEqual(y,n*(1+D)/(1+D*n),delta=4e-8)
            self.assertEqual(r,run('ghs-inspect','--recipe',p/'result/recipe.txt'))
            run('ghs-replay','--recipe',p/'result/recipe.txt','--output-dir',p/'replay')
            for name in ['display.fits','report.json','recipe.txt']:
                self.assertEqual((p/'result'/name).read_bytes(),(p/'replay'/name).read_bytes())
            run('ghs-stretch','--input',p/'result/display.fits','--output-dir',p/'identity','--log-stretch',0)
            self.assertEqual(out,read(p/'identity/display.fits')[1])
            run('psf-measure','--input',p/'result/display.fits','--output-dir',p/'bad-science',ok=False)
            preview=run('review-preview','--input',p/'result/display.fits','--output',p/'review.tiff')
            self.assertFalse(preview['stretched'])
            run('ghs-stretch','--input',p/'review.tiff','--output-dir',p/'review-check','--log-stretch',0)
            _,review=read(p/'review-check/display.fits')
            for actual,expected in zip(review[0],out[0]):self.assertAlmostEqual(actual,expected,delta=2/65535)
            self.assertEqual(sha,hashlib.sha256(source.read_bytes()).hexdigest())

    def test_coverage_preview_and_clipping(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);values=[i/47 for i in range(48)];values[10]=float('nan')
            write(p/'mono.fits',values)
            full=run('ghs-stretch','--input',p/'mono.fits','--output-dir',p/'full','--black',.1,'--white',.9)
            preview=run('ghs-stretch','--input',p/'mono.fits','--output-dir',p/'preview','--black',.1,'--white',.9,'--preview-width',4)
            self.assertEqual(full['afterHistogram'],preview['afterHistogram']);self.assertEqual(full['curve'],preview['curve'])
            self.assertEqual((full['coveredPixels'],full['belowBlack'],full['aboveWhite']),(47,5,5))
            self.assertFalse((p/'preview/display.fits').exists());self.assertTrue((p/'preview/after.tiff').exists())
            _,out=read(p/'full/display.fits');self.assertEqual(out[3][10],0)
            for plane in out:self.assertTrue(all(math.isfinite(v) and 0<=v<=1 for v in plane))
            display=run('ghs-stretch','--input',p/'full/developed.tiff','--output-dir',p/'display','--log-stretch',0)
            self.assertEqual(display['sourceDomain'],'display-srgb');self.assertEqual((display['black'],display['white']),(0,1))

    def test_inverse_chain_prefix_and_legacy_recipe(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source=p/'source.fits';values=[i/47 for i in range(48)];write(source,values)
            first=run('ghs-stretch','--input',source,'--output-dir',p/'first','--log-stretch',2,'--mode','independent')
            second=run('ghs-stretch','--input',source,'--chain-recipe',p/'first/recipe.txt','--output-dir',p/'second','--log-stretch',2,'--mode','independent','--inverse','on')
            self.assertEqual(second['stepCount'],2);self.assertTrue(second['inverse'])
            for actual,expected in zip(read(p/'second/display.fits')[1][0],values):self.assertAlmostEqual(actual,expected,delta=4e-7)
            run('ghs-replay','--recipe',p/'second/recipe.txt','--output-dir',p/'replay')
            self.assertEqual((p/'second/display.fits').read_bytes(),(p/'replay/display.fits').read_bytes())
            replaced=run('ghs-stretch','--input',source,'--chain-recipe',p/'second/recipe.txt','--chain-prefix-only','on','--output-dir',p/'replaced','--log-stretch',0)
            self.assertEqual(replaced['stepCount'],2)
            self.assertEqual(read(p/'first/display.fits')[1],read(p/'replaced/display.fits')[1])
            # A monochrome HSV saturation step must leave normalized values unchanged.
            saturation=run('ghs-stretch','--input',source,'--output-dir',p/'sat','--mode','saturation')
            self.assertTrue(saturation['saturation']);self.assertFalse(saturation['luminance'])
            for actual,expected in zip(read(p/'sat/display.fits')[1][0],values):self.assertAlmostEqual(actual,expected,delta=4e-8)
            # Old one-step recipes remain readable/replayable, with no inferred new flags.
            lines=(p/'first/recipe.txt').read_text().splitlines()
            legacy=['PhotonStack-GHS-1',lines[1],' '.join(lines[3].split()[:-1])+' 0',*lines[4:]]
            (p/'first/legacy.txt').write_text('\n'.join(legacy)+'\n')
            run('ghs-replay','--recipe',p/'first/legacy.txt','--output-dir',p/'legacy')
            self.assertEqual(read(p/'first/display.fits')[1],read(p/'legacy/display.fits')[1])
            write(p/'other.fits',[1+i/47 for i in range(48)])
            run('ghs-stretch','--input',p/'other.fits','--chain-recipe',p/'first/recipe.txt','--output-dir',p/'wrong',ok=False)
            self.assertFalse((p/'wrong').exists())

    def test_guards_and_tampering(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source=p/'source.fits';write(source,[i/47 for i in range(48)])
            for args in [('--log-stretch','nan'),('--log-stretch','11'),('--local-intensity','-6'),('--symmetry','1.1'),('--shadow-protection','.1'),('--white','1'),('--black','2','--white','1'),('--preview-width','1.5'),('--expected-sha','bad'),('--mode','rgb'),('--input',str(source))]:
                run('ghs-stretch','--input',source,'--output-dir',p/'bad',*args,ok=False);self.assertFalse((p/'bad').exists())
            run('ghs-stretch','--input',source,'--output-dir',p/'result')
            run('ghs-stretch','--input',source,'--output-dir',p/'result',ok=False)
            file=p/'result/report.json';saved=file.read_bytes();file.write_bytes(saved+b'bad')
            run('ghs-inspect','--recipe',p/'result/recipe.txt',ok=False);file.write_bytes(saved)
            source.write_bytes(source.read_bytes()+b'changed')
            run('ghs-replay','--recipe',p/'result/recipe.txt','--output-dir',p/'bad',ok=False);self.assertFalse((p/'bad').exists())
            write(source,[.3]*48);run('ghs-stretch','--input',source,'--output-dir',p/'bad',ok=False)
            write(source,[.3]*48,extra={'BAYERPAT':"'RGGB'"});run('ghs-stretch','--input',source,'--output-dir',p/'bad',ok=False)
            write(source,[.3]*48,extra={'PSCOLOR':"'unknown'"});run('ghs-stretch','--input',source,'--output-dir',p/'bad',ok=False)

if __name__=='__main__':unittest.main()
