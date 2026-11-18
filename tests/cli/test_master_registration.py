"""Master registration geometry, support and reproducibility with independent FITS I/O."""
import math
from pathlib import Path
import random
import struct
import tempfile
import unittest
from test_channels import CLI, read, write, run


def register(root,mode='stars',name='registered',symbols=('Ha','OIII')):
    args=['channel-register','--mode',mode,'--reference',symbols[0],'--output-dir',root/name]
    for symbol in symbols:args+=['--input',f'{symbol}={root/(symbol+".fits")}']
    return run(*args)


def stars():
    rng=random.Random(83);result=[]
    while len(result)<65:
        x=rng.uniform(22,298);y=rng.uniform(22,234)
        if all(math.hypot(x-a,y-b)>19 for a,b,_ in result):result.append((x,y,rng.uniform(300,1800)))
    return result


def scene(path,points,w,h,transform=(1,0,0,0),different=False,extra=None):
    scale,angle,dx,dy=transform;c=math.cos(angle);s=math.sin(angle)
    values=[]
    for y in range(h):
        for x in range(w):
            rx=scale*(c*x-s*y)+dx;ry=scale*(s*x+c*y)+dy
            value=-7+.002*rx-.003*ry
            for i,(sx,sy,amplitude) in enumerate(points):
                if different and i%9==0:continue
                if abs(rx-sx)>12 or abs(ry-sy)>12:continue
                gain=(.6+.2*(i%3)) if different else 1
                value+=gain*amplitude*math.exp(-((rx-sx)**2+(ry-sy)**2)/8)
            values.append(value)
    write(path,values,w,h,extra)


class MasterRegistration(unittest.TestCase):
    def test_stars_rotation_scale_translation_different_colors_and_sizes(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);points=stars();truth=(1.015,.035,8,-6)
            scene(p/'Ha.fits',points,320,256)
            scene(p/'OIII.fits',points,344,270,truth,True)
            report=register(p);m=report['masters'][1]
            for actual,expected,tolerance in zip(m['transform'],truth,(.0003,.0003,.07,.07)):
                self.assertAlmostEqual(actual,expected,delta=tolerance)
            self.assertGreaterEqual(m['fitStars'],12);self.assertGreaterEqual(m['checkStars'],12)
            self.assertLess(m['median'],.08);self.assertLess(m['p95'],.12)
            header,plane=read(p/'registered/OIII.fits');_,valid=read(p/'registered/OIII-validity.fits')
            self.assertEqual((int(header['NAXIS1']),int(header['NAXIS2'])),(320,256))
            a=plane[0];centroids=[];fluxes=[]
            source=read(p/'OIII.fits')[1][0]
            scale,angle,dx,dy=m['transform'];c=math.cos(angle);s=math.sin(angle)
            for y in range(8,248,3):
                for x in range(8,312,3):
                    sx=(c*(x-dx)+s*(y-dy))/scale;sy=(-s*(x-dx)+c*(y-dy))/scale
                    ix=math.floor(sx);iy=math.floor(sy);fx=sx-ix;fy=sy-iy
                    if not (0<=ix<343 and 0<=iy<269):continue
                    expected=sum(source[(iy+j)*344+ix+i]*(fx if i else 1-fx)*(fy if j else 1-fy) for j in range(2) for i in range(2))
                    expected=struct.unpack('f',struct.pack('f',expected))[0]
                    self.assertAlmostEqual(a[y*320+x],expected,delta=max(1e-5,abs(expected)*2e-7))
            for i,(sx,sy,amplitude) in enumerate(points):
                if i%9==0:continue
                weighted=[]
                for y in range(round(sy)-8,round(sy)+9):
                    for x in range(round(sx)-8,round(sx)+9):
                        value=a[y*320+x]
                        if not math.isfinite(value):continue
                        signal=value-(-7+.002*x-.003*y)
                        weighted.append((x,y,signal))
                flux=sum(v for _,_,v in weighted)
                cx=sum(x*v for x,_,v in weighted)/flux;cy=sum(y*v for _,y,v in weighted)/flux
                centroids.append(math.hypot(cx-sx,cy-sy))
                fluxes.append(flux/(amplitude*(.6+.2*(i%3))*8*math.pi))
            self.assertLess(max(centroids),.1)
            self.assertTrue(all(abs(r-1)<.015 for r in fluxes))
            self.assertTrue(any(v==0 for v in valid[0]));self.assertTrue(all(math.isfinite(v)==bool(mask) for v,mask in zip(a,valid[0])))
            self.assertEqual(read(p/'Ha.fits')[1],read(p/'registered/Ha.fits')[1])
            run('channel-registration-replay','--recipe',p/'registered/recipe.txt','--output-dir',p/'replayed')
            for name in ('Ha.fits','Ha-validity.fits','OIII.fits','OIII-validity.fits'):
                self.assertEqual((p/'registered'/name).read_bytes(),(p/'replayed'/name).read_bytes())

    def test_wcs_shift_strict_support_and_coordinate_preservation(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);wcs={'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'CRPIX1':5.5,'CRPIX2':4.5,'CRVAL1':.01,'CRVAL2':42,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001}
            write(p/'Ha.fits',[3.]*80,10,8,wcs)
            values=[2*x+3*y-5 for y in range(10) for x in range(12)];values[4*12+6]=math.nan
            write(p/'OIII.fits',values,12,10,{**wcs,'CRPIX1':8.5,'CRPIX2':2.5})
            report=register(p,'wcs');self.assertEqual(report['masters'][1]['checkStars'],0)
            header,planes=read(p/'registered/OIII.fits');_,mask=read(p/'registered/OIII-validity.fits')
            for y in range(8):
                for x in range(10):
                    sx=x+3;sy=y-2;i=y*10+x
                    valid=0<=sx<12 and 0<=sy<10 and (sx,sy)!=(6,4)
                    self.assertEqual(bool(mask[0][i]),valid)
                    if valid:self.assertAlmostEqual(planes[0][i],2*sx+3*sy-5,delta=1e-5)
                    else:self.assertTrue(math.isnan(planes[0][i]))
            for key in ('CRPIX1','CRPIX2','CRVAL1','CRVAL2','CD1_1','CD2_2'):self.assertAlmostEqual(float(header[key]),wcs[key])
            run('channel-registration-replay','--recipe',p/'registered/recipe.txt','--output-dir',p/'replay')
            self.assertEqual((p/'registered/OIII.fits').read_bytes(),(p/'replay/OIII.fits').read_bytes())

    def test_identity_sip_preserves_edges_and_wrong_wcs_fails_stellar_check(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            wcs={'CTYPE1':"'RA---TAN-SIP'",'CTYPE2':"'DEC--TAN-SIP'",'CRPIX1':160.5,'CRPIX2':128.5,'CRVAL1':359.99,'CRVAL2':30,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001,'A_ORDER':2,'B_ORDER':2,'A_2_0':1e-5,'B_0_2':-1e-5}
            scene(p/'Ha.fits',stars(),320,256,extra=wcs)
            scene(p/'OIII.fits',stars(),320,256,extra=wcs)
            register(p,'wcs')
            self.assertEqual(read(p/'OIII.fits')[1],read(p/'registered/OIII.fits')[1])
            self.assertTrue(all(v==1 for v in read(p/'registered/OIII-validity.fits')[1][0]))
            scene(p/'OIII.fits',stars(),320,256,extra={**wcs,'CRPIX1':162})
            message=run('channel-register','--mode','wcs','--reference','Ha','--input',f'Ha={p/"Ha.fits"}','--input',f'OIII={p/"OIII.fits"}','--output-dir',p/'wrong',ok=False)
            self.assertIn('disagree at common stars',message)
            self.assertFalse((p/'wrong').exists())

    def test_failures_do_not_publish_or_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);write(p/'Ha.fits',[2.]*48);write(p/'OIII.fits',[3.]*48)
            for mode in ('stars','wcs','bad'):
                run('channel-register','--mode',mode,'--reference','Ha','--input',f'Ha={p/"Ha.fits"}','--input',f'OIII={p/"OIII.fits"}','--output-dir',p/mode,ok=False)
                self.assertFalse((p/mode).exists())
            register(p,symbols=('Ha',))
            recipe=p/'registered/recipe.txt';original=recipe.read_bytes();data=(p/'registered/Ha.fits').read_bytes()
            run('channel-registration-replay','--recipe',recipe,'--output-dir',p/'registered',ok=False)
            self.assertEqual(recipe.read_bytes(),original)
            (p/'registered/Ha.fits').write_bytes(data[:-1]+b'X')
            run('channel-registration-inspect','--recipe',recipe,ok=False)
            (p/'registered/Ha.fits').write_bytes(data)
            with (p/'Ha.fits').open('ab') as f:f.write(b'changed')
            run('channel-registration-replay','--recipe',recipe,'--output-dir',p/'badsource',ok=False)
            self.assertFalse((p/'badsource').exists())

if __name__=='__main__':unittest.main()
