"""Independent FITS and affine/noise/contamination oracles for linear matching."""
import hashlib
import math
from pathlib import Path
import random
import tempfile
import unittest
from test_channels import read,write,run


def match(p,name='result',*extra,ok=True):
    return run('channel-match','--aligned','--reference',f'Ha={p/"Ha.fits"}','--input',f'OIII={p/"OIII.fits"}','--output-dir',p/name,*extra,ok=ok)


class LinearMatch(unittest.TestCase):
    def test_gain_offset_mask_residual_wcs_and_saved_replay(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);w=h=64
            x=[-100+2*i+.2*j for j in range(h) for i in range(w)]
            y=[2.5*v-17 for v in x];x[0]=math.nan;y[1]=math.nan
            wcs={'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'CRPIX1':32.5,'CRPIX2':32.5,'CRVAL1':359.99,'CRVAL2':30,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001}
            write(p/'Ha.fits',y,w,h,wcs);write(p/'OIII.fits',x,w,h,wcs)
            before={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in p.glob('*.fits')}
            r=match(p);self.assertAlmostEqual(r['gain'],2.5,delta=1e-7);self.assertAlmostEqual(r['offset'],-17,delta=2e-6)
            header,actual=read(p/'result/matched.fits');_,mask=read(p/'result/validity.fits');_,res=read(p/'result/residual.fits')
            target=read(p/'OIII.fits')[1][0];reference=read(p/'Ha.fits')[1][0]
            for i,(v,a,m) in enumerate(zip(target,actual[0],mask[0])):
                self.assertEqual(bool(m),math.isfinite(v))
                if m:self.assertAlmostEqual(a,r['gain']*v+r['offset'],delta=3e-5)
                else:self.assertTrue(math.isnan(a))
                if m and math.isfinite(reference[i]):self.assertAlmostEqual(res[0][i],a-reference[i],delta=1e-9)
                else:self.assertTrue(math.isnan(res[0][i]))
            self.assertLess(min(v for v in actual[0] if math.isfinite(v)),0)
            self.assertEqual(float(header['CRVAL1']),359.99)
            self.assertEqual(sum(q['count'] for q in r['regions']),r['checkSamples'])
            reopened=run('channel-match-inspect','--recipe',p/'result/recipe.txt');self.assertEqual(r,reopened)
            replay=run('channel-match-replay','--recipe',p/'result/recipe.txt','--output-dir',p/'again')
            self.assertEqual(replay['gain'],r['gain'])
            for f in ['matched.fits','validity.fits','residual.fits']:
                self.assertEqual((p/'result'/f).read_bytes(),(p/'again'/f).read_bytes())
            self.assertEqual(before,{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in p.glob('*.fits')})

    def test_robust_known_truth_with_noise_and_contaminants(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);rng=random.Random(43);w=h=128;x=[];y=[]
            for j in range(h):
                for i in range(w):
                    v=300*math.sin(i*.17)+100*math.cos(j*.19)+rng.uniform(-100,100)
                    x.append(v);y.append(1.7*v+53+rng.gauss(0,2)+(rng.uniform(300,800) if rng.random()<.07 else 0))
            write(p/'Ha.fits',y,w,h);write(p/'OIII.fits',x,w,h)
            r=match(p)
            self.assertAlmostEqual(r['gain'],1.7,delta=.003)
            self.assertAlmostEqual(r['offset'],53,delta=.4)
            self.assertGreater(r['downweighted'],100)
            self.assertGreater(r['p95AbsoluteResidual'],3)
            self.assertLess(r['afterRms'],r['beforeRms'])

    def test_target_outliers_do_not_bias_gain_in_low_contrast_background(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);rng=random.Random(83);reference=[];target=[]
            for _ in range(128*128):
                value=1200+rng.gauss(0,20)
                reference.append(value)
                target.append((value+41.7)/1.63+rng.gauss(0,.2)+(rng.uniform(60,150) if rng.random()<.03 else 0))
            write(p/'Ha.fits',reference,128,128);write(p/'OIII.fits',target,128,128)
            r=match(p)
            self.assertAlmostEqual(r['gain'],1.63,delta=.003)
            self.assertAlmostEqual(r['offset'],-41.7,delta=3)
            self.assertGreater(r['downweighted'],0)

    def test_held_out_spatial_gradient_is_visible(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);w=h=128;rng=random.Random(57);x=[rng.uniform(-100,100) for _ in range(w*h)]
            y=[2*v+7+(40 if ((i%w)*8//w+(i//w)*8//h)%2 else 0) for i,v in enumerate(x)]
            write(p/'Ha.fits',y,w,h);write(p/'OIII.fits',x,w,h)
            r=match(p)
            self.assertAlmostEqual(r['gain'],2,delta=1e-6);self.assertAlmostEqual(r['offset'],7,delta=1e-5)
            self.assertAlmostEqual(r['medianResidual'],-40,delta=1e-4)
            self.assertTrue(all(q['afterMean'] < -39.99 for q in r['regions']))

    def test_insufficient_range_coverage_invalid_options_and_wcs_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);x=[float(i%64) for i in range(4096)]
            write(p/'OIII.fits',x,64,64);write(p/'Ha.fits',[2.]*4096,64,64)
            self.assertIn('range',match(p,'constant',ok=False));self.assertFalse((p/'constant').exists())
            write(p/'Ha.fits',[2*v+7 for v in x],64,64)
            run('channel-match','--reference',f'Ha={p/"Ha.fits"}','--input',f'OIII={p/"OIII.fits"}','--output-dir',p/'unaligned',ok=False)
            self.assertFalse((p/'unaligned').exists())
            for extra in [('--low','nan'),('--high','1.1'),('--low','.95'),('--low','0.2x'),('--high','-inf')]:
                match(p,'bad',*extra,ok=False);self.assertFalse((p/'bad').exists())
            write(p/'Ha.fits',[-v for v in x],64,64);match(p,'negative',ok=False)
            write(p/'Ha.fits',[2*v+7 if i<512 else math.nan for i,v in enumerate(x)],64,64)
            self.assertIn('regions',match(p,'coverage',ok=False))
            wcs={'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'CRPIX1':32.5,'CRPIX2':32.5,'CRVAL1':10,'CRVAL2':30,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001}
            write(p/'Ha.fits',x,64,64,wcs);write(p/'OIII.fits',x,64,64,{**wcs,'CRPIX1':33.5})
            self.assertIn('WCS grids',match(p,'wcs',ok=False))

    def test_destination_and_hash_protection(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);x=[float(i%64) for i in range(4096)]
            write(p/'Ha.fits',[2*v+7 for v in x],64,64);write(p/'OIII.fits',x,64,64)
            match(p);recipe=p/'result/recipe.txt';saved=recipe.read_bytes()
            match(p,ok=False);self.assertEqual(recipe.read_bytes(),saved)
            data=(p/'result/matched.fits').read_bytes();(p/'result/matched.fits').write_bytes(data[:-1]+b'X')
            run('channel-match-inspect','--recipe',recipe,ok=False)
            (p/'result/matched.fits').write_bytes(data)
            with (p/'OIII.fits').open('ab') as f:f.write(b'changed')
            run('channel-match-replay','--recipe',recipe,'--output-dir',p/'changed',ok=False)
            self.assertFalse((p/'changed').exists())

if __name__=='__main__':unittest.main()
