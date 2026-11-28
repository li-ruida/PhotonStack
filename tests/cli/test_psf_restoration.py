"""Saved measured-PSF restoration: scientific products, provenance, guards."""
import hashlib
import math
import struct
from pathlib import Path
import tempfile
import unittest
from test_channels import read,write,run
from test_psf_measurement import make


def fixture(p):
    values=make(p/'source.fits',background=20)
    # Preserve a supported celestial grid through all restoration products.
    write(p/'source.fits',values,384,320,{'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'CRPIX1':192,'CRPIX2':160,'CRVAL1':313.6,'CRVAL2':30,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001})
    return run('psf-measure','--input',p/'source.fits','--output-dir',p/'psf')


def restore(p,name='restored',*options,ok=True):
    return run('psf-restore','--measurement',p/'psf/recipe.txt','--output-dir',p/name,*options,ok=ok)


class Restoration(unittest.TestCase):
    def test_linear_products_replay_and_wcs(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);fixture(p);before=hashlib.sha256((p/'source.fits').read_bytes()).hexdigest()
            r=restore(p);self.assertLess(r['afterRms'],r['beforeRms']);self.assertEqual(r['channel'],'luminance')
            h,source=read(p/'source.fits');_,result=read(p/'restored/restored.fits');_,difference=read(p/'restored/difference.fits')
            self.assertGreater(max(result[0]),1)
            for actual,original,delta in zip(result[0],source[0],difference[0]):self.assertEqual(struct.unpack('>f',struct.pack('>f',actual-original))[0],delta)
            for name in ['channel','restored','difference','residual','protection']:
                header,planes=read(p/f'restored/{name}.fits');self.assertIn('CTYPE1',header)
                for plane in planes:self.assertTrue(all(math.isfinite(v) for v in plane))
            self.assertEqual(run('psf-restoration-inspect','--recipe',p/'restored/recipe.txt'),r)
            run('psf-restoration-replay','--recipe',p/'restored/recipe.txt','--output-dir',p/'replay')
            for name in ['channel','restored','difference','residual','protection']:self.assertEqual((p/f'restored/{name}.fits').read_bytes(),(p/f'replay/{name}.fits').read_bytes())
            self.assertEqual(before,hashlib.sha256((p/'source.fits').read_bytes()).hexdigest())

    def test_pedestal_protection_and_identity(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);make(p/'source.fits',background=-40);run('psf-measure','--input',p/'source.fits','--output-dir',p/'psf')
            self.assertIn('pedestal',restore(p,ok=False));self.assertFalse((p/'restored').exists())
            restore(p,'identity','--pedestal','50','--amount','0');_,source=read(p/'source.fits');_,identity=read(p/'identity/restored.fits');self.assertEqual(source[0],identity[0])
            r=restore(p,'protected','--pedestal','50','--protect-above','300','--noise-sigma','2')
            self.assertGreater(r['protectedPixels'],0)
            _,mask=read(p/'protected/protection.fits');_,output=read(p/'protected/restored.fits')
            for a,b,w in zip(source[0],output[0],mask[0]):
                if w==1:self.assertEqual(a,b)

    def test_spatial_models_replay_and_old_recipe_compatibility(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);make(p/'source.fits',vary=True,background=20)
            run('psf-measure','--input',p/'source.fits','--output-dir',p/'psf')
            r=restore(p,'spatial','--spatial-mode','quadrants','--noise-model','poisson-gaussian',
                      '--electrons-per-unit','2','--read-noise-electrons','3')
            self.assertEqual(r['spatialMode'],'quadrants');self.assertEqual(r['noiseModel'],'poisson-gaussian')
            self.assertLess(r['afterRms'],r['beforeRms'])
            self.assertEqual(run('psf-restoration-inspect','--recipe',p/'spatial/recipe.txt'),r)
            run('psf-restoration-replay','--recipe',p/'spatial/recipe.txt','--output-dir',p/'replay')
            self.assertEqual((p/'spatial/restored.fits').read_bytes(),(p/'replay/restored.fits').read_bytes())
            restore(p,'global')
            recipe=p/'global/recipe.txt';lines=recipe.read_text().splitlines()
            lines[0]='PhotonStack-PSF-Restoration-1';del lines[3];recipe.write_text('\n'.join(lines)+'\n')
            old=run('psf-restoration-inspect','--recipe',recipe)
            self.assertEqual(old['spatialMode'],'global');self.assertEqual(old['noiseModel'],'constant')
            run('psf-restoration-replay','--recipe',recipe,'--output-dir',p/'old-replay')
            self.assertEqual((p/'global/restored.fits').read_bytes(),(p/'old-replay/restored.fits').read_bytes())

    def test_invalid_options_destinations_and_tampering(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);fixture(p)
            for options in [('--amount','nan'),('--amount','1.1'),('--iterations','0'),('--iterations','1.5'),('--threads','17'),('--noise-sigma','-1'),('--pedestal','-1'),('--protect-above','-2'),('--wat','0'),('--spatial-mode','tiles'),('--noise-model','unknown'),('--electrons-per-unit','0'),('--read-noise-electrons','-1')]:
                restore(p,'bad',*options,ok=False);self.assertFalse((p/'bad').exists())
            restore(p);saved=(p/'restored/recipe.txt').read_bytes();restore(p,ok=False);self.assertEqual(saved,(p/'restored/recipe.txt').read_bytes())
            product=p/'restored/restored.fits';original=product.read_bytes();product.write_bytes(original+b'changed')
            run('psf-restoration-inspect','--recipe',p/'restored/recipe.txt',ok=False);product.write_bytes(original)
            psf=p/'psf/psf.fits';psf.write_bytes(psf.read_bytes()+b'changed')
            run('psf-restoration-inspect','--recipe',p/'restored/recipe.txt',ok=False)
            restore(p,'changed',ok=False);self.assertFalse((p/'changed').exists())

if __name__=='__main__':unittest.main()
