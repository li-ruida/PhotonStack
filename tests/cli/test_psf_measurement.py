"""Native-grid Gaussian PSF, selection, source protection and saved-kernel oracles."""
import hashlib
import math
from pathlib import Path
import struct
import tempfile
import unittest
from test_channels import read,write,run


def covariance(major=2.2,minor=1.3,angle=.47):
    c=math.cos(angle);s=math.sin(angle)
    return major*major*c*c+minor*minor*s*s,major*major*s*s+minor*minor*c*c,(major*major-minor*minor)*c*s


def points():return [(32.23+64*x,32.37+64*y,500+20*(x+6*y)) for y in range(5) for x in range(6)]


def make(path,cov=None,alter=False,vary=False,background=-40):
    w,h=384,320;cov=cov or covariance();values=[background+.001*x-.002*y for y in range(h) for x in range(w)]
    positions=points()+([(5.,180.,700),(180.,90.,600),(190.,90.,600)] if alter else [])
    for i,(sx,sy,amplitude) in enumerate(positions):
        xx,yy,xy=covariance(2.2,.9,sx/w*math.pi) if vary=="angle" else covariance(1.4+sx/w,1.2+sx/w*.6,.3) if vary else cov
        if alter and i==0:xx,yy,xy=covariance(3.3,3.1,.1)
        if alter and i==3:amplitude=3500
        det=xx*yy-xy*xy
        for y in range(max(0,int(sy)-17),min(h,int(sy)+18)):
            for x in range(max(0,int(sx)-17),min(w,int(sx)+18)):
                dx=x-sx;dy=y-sy;values[y*w+x]+=amplitude*math.exp(-.5*(yy*dx*dx-2*xy*dx*dy+xx*dy*dy)/det)
    if alter:
        values=[min(v,1500) for v in values]
        values[round(positions[1][1])*w+round(positions[1][0])+6]=math.nan
    write(path,values,w,h)
    return values


def measure(p,name='result',*options,ok=True):
    return run('psf-measure','--input',p/'source.fits','--output-dir',p/name,*options,ok=ok)


class PsfMeasurement(unittest.TestCase):
    def test_rotated_gaussian_covariance_kernel_and_recipe(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);truth=covariance();make(p/'source.fits',truth);before=hashlib.sha256((p/'source.fits').read_bytes()).hexdigest()
            r=measure(p);self.assertEqual(r['accepted'],30)
            for k,v in zip(('xx','yy','xy'),truth):self.assertAlmostEqual(r[k],v,delta=2e-5)
            self.assertAlmostEqual(r['majorFwhm'],2.354820045*2.2,delta=1e-5)
            self.assertAlmostEqual(r['minorFwhm'],2.354820045*1.3,delta=1e-5)
            self.assertAlmostEqual(r['angle'],math.degrees(.47),delta=1e-4)
            self.assertFalse(r['spatiallyVariable']);self.assertEqual(sum(s['count'] for s in r['regions']),30)
            header,k=read(p/'result/psf.fits');width=int(header['NAXIS1']);self.assertEqual(width,19);self.assertNotIn('CRVAL1',header)
            xx,yy,xy=(r[k] for k in ('xx','yy','xy'));det=xx*yy-xy*xy;radius=width//2
            expected=[math.exp(-.5*(yy*x*x-2*xy*x*y+xx*y*y)/det) for y in range(-radius,radius+1) for x in range(-radius,radius+1)]
            total=sum(expected)
            for actual,truth in zip(k[0],expected):self.assertAlmostEqual(actual,truth/total,delta=1e-8)
            self.assertAlmostEqual(sum(k[0]),1,delta=1e-7)
            self.assertEqual(run('psf-inspect','--recipe',p/'result/recipe.txt'),r)
            measure(p,'again');self.assertEqual((p/'result/psf.fits').read_bytes(),(p/'again/psf.fits').read_bytes())
            self.assertEqual(before,hashlib.sha256((p/'source.fits').read_bytes()).hexdigest())

    def test_star_rejections_explicit_exclusion_and_spatial_variation(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);make(p/'source.fits',alter=True)
            r=measure(p,'result','--saturation','1500')
            statuses={s['status'] for s in r['stars']}
            self.assertTrue({'masked','saturated','crowded','edge','shape-outlier'}.issubset(statuses),statuses)
            selected=next(s['id'] for s in r['stars'] if s['status']=='accepted')
            other=measure(p,'excluded','--saturation','1500','--exclude',str(selected))
            self.assertEqual(other['accepted'],r['accepted']-1);self.assertEqual(other['stars'][selected]['status'],'excluded')
            measure(p,'badid','--exclude','2048',ok=False);self.assertFalse((p/'badid').exists())
            make(p/'source.fits',vary=True)
            v=measure(p,'variable');self.assertTrue(v['spatiallyVariable']);self.assertGreater(v['fwhmP90']/v['fwhmP10'],1.3)
            make(p/'source.fits',vary='angle')
            angle=measure(p,'angle');self.assertTrue(angle['spatiallyVariable']);self.assertLess(angle['fwhmP90']/angle['fwhmP10'],1.01)

    def test_all_negative_signals_and_rgb_channel_measurements(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);make(p/'source.fits',background=-3000)
            self.assertEqual(measure(p)['accepted'],30)
            planes=[make(p/f'{i}.fits',covariance(a,1.2,.2)) for i,a in enumerate([1.6,2.1,2.7])]
            cards={'SIMPLE':'T','BITPIX':-32,'NAXIS':3,'NAXIS1':384,'NAXIS2':320,'NAXIS3':3}
            header=(''.join(f'{k:<8}= {str(v):>20}'.ljust(80) for k,v in cards.items())+'END').ljust(2880)
            data=b''.join(struct.pack(f'>{len(v)}f',*v) for v in planes)
            (p/'source.fits').write_bytes(header.encode()+data.ljust(((len(data)+2879)//2880)*2880,b'\0'))
            for name,major in zip(('red','green','blue'),(1.6,2.1,2.7)):
                r=measure(p,name,'--channel',name);self.assertAlmostEqual(r['majorFwhm'],2.354820045*major,delta=1e-4)

    def test_rejects_insufficient_stars_bad_domain_options_and_changed_products(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);write(p/'source.fits',[2.]*4096,64,64)
            measure(p,'empty',ok=False);self.assertFalse((p/'empty').exists())
            narrow=make(p/'source.fits')
            padded=[]
            for row in range(320):padded+=narrow[row*384:(row+1)*384]+[-40.]*640
            write(p/'source.fits',padded,1024,320)
            self.assertIn('field coverage',measure(p,'clustered',ok=False))
            make(p/'source.fits')
            for options in [('--sigma','nan'),('--sigma','31'),('--saturation','-1'),('--channel','bad'),('--exclude','1,1'),('--exclude','1,'),('--exclude','-1')]:
                measure(p,'bad',*options,ok=False);self.assertFalse((p/'bad').exists())
            r=measure(p);recipe=p/'result/recipe.txt';saved=recipe.read_bytes()
            measure(p,ok=False);self.assertEqual(recipe.read_bytes(),saved)
            data=(p/'result/psf.fits').read_bytes();(p/'result/psf.fits').write_bytes(data[:-1]+b'X')
            run('psf-inspect','--recipe',recipe,ok=False)
            (p/'result/psf.fits').write_bytes(data)
            with (p/'source.fits').open('ab') as f:f.write(b'changed')
            run('psf-inspect','--recipe',recipe,ok=False)
            measure(p,'changed','--expected-sha',r['sha'],ok=False);self.assertFalse((p/'changed').exists())
            write(p/'source.fits',[2.]*4096,64,64,{'BAYERPAT':"'RGGB'"});self.assertIn('CFA',measure(p,'cfa',ok=False))
            write(p/'source.fits',[2.]*4096,64,64,{'PSCOLOR':"'sRGB'"});self.assertIn('linear',measure(p,'display',ok=False))

if __name__=='__main__':unittest.main()
