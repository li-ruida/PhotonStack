"""Independent sensor-coordinate and point-source oracles for dual-band sampling."""
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from test_narrowband import CLI, write_source


def read_plane(path):
    data=path.read_bytes()
    cards={data[i:i+8].decode().strip():data[i+10:i+80].decode().strip() for i in range(0,2880,80)}
    w,h=int(cards['NAXIS1']),int(cards['NAXIS2'])
    return w,h,struct.unpack_from(f'>{w*h}f',data,2880)


def write_physical(path, values, w, h, pattern='RGGB', offset=(0,0)):
    write_source(path,pattern,*offset,[(v+31)/2 for v in values],w,h)


def run(*args,ok=True):
    result=subprocess.run([CLI,*map(str,args)],capture_output=True,text=True)
    if ok:
        if result.returncode:raise AssertionError(result.stderr)
        return json.loads(result.stdout)
    if not result.returncode:raise AssertionError('Invalid request succeeded')
    return result.stderr


class Sampling(unittest.TestCase):
    def test_default_grid_matches_physical_red_coordinates_all_phases_and_edges(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for w,h in ((8,6),(7,9),(2,2)):
                for pattern in ('RGGB','BGGR','GRBG','GBRG'):
                    for ox,oy in ((0,0),(-1,0),(0,3),(1,1)):
                        values=[(20+3*x+5*y)*(1+(2*(y%2)+x%2)/2)+7*(2*(y%2)+x%2)
                                for y in range(h) for x in range(w)]
                        source=root/'source.fits';write_physical(source,values,w,h,pattern,(ox,oy))
                        out=root/f'{w}-{h}-{pattern}-{ox}-{oy}'
                        report=run('narrowband-extract','--input',source,'--output-dir',out)
                        self.assertEqual(report['sampling'],'red-grid')
                        red=pattern.index('R')^(ox%2)^((oy%2)<<1);rx,ry=red%2,red//2
                        width,height,ha=read_plane(out/'Ha.fits');_,_,oiii=read_plane(out/'OIII.fits')
                        _,_,mask=read_plane(out/'OIII-valid.fits')
                        for y in range(height):
                            for x in range(width):
                                i=y*width+x;sx,sy=2*x+rx,2*y+ry
                                self.assertEqual(ha[i],values[sy*w+sx])
                                valid=0<sx<w-1 and 0<sy<h-1
                                self.assertEqual(mask[i],float(valid))
                                if not valid:self.assertTrue(math.isnan(oiii[i]));continue
                                # Each phase is an independent linear scene. Evaluating
                                # all three at the red site's coordinates has known truth.
                                expected=sum((20+3*sx+5*sy)*(1+p/2)+7*p for p in range(4) if p!=red)/3
                                self.assertAlmostEqual(oiii[i],expected,delta=3e-5)

    def test_missing_samples_do_not_become_zero_or_partial_stencils(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);w=h=8
            values=[-50+2*x+3*y for y in range(h) for x in range(w)]
            values[2*w+2]=math.nan # A missing red sample affects Hα only.
            values[5*w+4]=math.nan # Green sample affects neighboring OIII estimates.
            source=root/'source.fits';write_physical(source,values,w,h)
            out=root/'out';run('narrowband-extract','--input',source,'--output-dir',out)
            _,_,ha=read_plane(out/'Ha.fits');_,_,o=read_plane(out/'OIII.fits')
            _,_,hm=read_plane(out/'Ha-valid.fits');_,_,om=read_plane(out/'OIII-valid.fits')
            for cy in range(4):
                for cx in range(4):
                    x,y=2*cx,2*cy;i=cy*4+cx
                    self.assertEqual(hm[i],float(math.isfinite(values[y*w+x])))
                    neighbors=[values[yy*w+xx] for yy in range(y-1,y+2) for xx in range(x-1,x+2)
                               if (xx,yy)!=(x,y) and 0<=xx<w and 0<=yy<h]
                    valid=len(neighbors)==8 and all(math.isfinite(v) for v in neighbors)
                    self.assertEqual(om[i],float(valid))
                    if valid:self.assertAlmostEqual(o[i],-50+2*x+3*y,places=5)
                    else:self.assertTrue(math.isnan(o[i]))
            self.assertEqual(hm[5],0);self.assertEqual(om[5],1)

    def test_calibration_precedes_interpolation_and_replay_restores_sampling(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);w=h=12;light=[];flat=[];dark=[]
            for y in range(h):
                for x in range(w):
                    p=(y%2)*2+x%2
                    response=(.5,1,1.5)[x//2%3]
                    signal=-40+2*x+3*y
                    dark.append(10+p)
                    flat.append(100*(p+1)*response)
                    light.append(signal*response+10+p)
            for name,values in [('source',light),('dark',dark),('flat',flat)]:write_physical(root/(name+'.fits'),values,w,h)
            out=root/'out'
            run('narrowband-extract','--input',root/'source.fits','--output-dir',out,
                '--dark',root/'dark.fits','--dark-bias','included','--flat',root/'flat.fits','--flat-bias','removed')
            _,_,o=read_plane(out/'OIII.fits')
            for y in range(1,6):
                for x in range(1,6):self.assertEqual(o[y*6+x],-40+4*x+6*y)
            again=root/'again';run('narrowband-replay','--recipe',out/'recipe.txt','--output-dir',again)
            for name in ('Ha.fits','OIII.fits','Ha-valid.fits','OIII-valid.fits'):
                self.assertEqual((out/name).read_bytes(),(again/name).read_bytes())
            legacy=root/'legacy'
            report=run('narrowband-replay','--recipe',out/'recipe.txt','--output-dir',legacy,'--sampling','cell-average')
            self.assertEqual(report['sampling'],'cell-average')
            self.assertNotEqual((out/'OIII.fits').read_bytes(),(legacy/'OIII.fits').read_bytes())
            # V3 must never silently fall back to a different algorithm.
            original=(out/'recipe.txt').read_text()
            for changed in (original.replace('sampling "red-grid"\n',''),original+'sampling "cell-average"\n',
                            original.replace('sampling "red-grid"','sampling "unknown"')):
                (out/'recipe.txt').write_text(changed)
                run('narrowband-inspect','--recipe',out/'recipe.txt',ok=False)
            (out/'recipe.txt').write_text(original)

    def test_registered_stack_keeps_both_lines_on_the_same_grid(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);w,h=128,96
            stars=[(14.25+18*x,14.6+18*y,600+17*x+13*y) for y in range(4) for x in range(6)]
            frames=[]
            for n,(dx,dy) in enumerate(((0,0),(4,0),(-2,2))):
                values=[]
                for y in range(h):
                    for x in range(w):
                        star=sum(a*math.exp(-((x-cx-dx)**2+(y-cy-dy)**2)/8) for cx,cy,a in stars)
                        values.append(50+star*(2 if x%2==y%2==0 else 1))
                path=root/f'frame-{n}.fits';write_physical(path,values,w,h);frames.append(path)
            out=root/'stack';report=run('narrowband-stack','--output-dir',out,*frames)
            self.assertEqual(report['sampling'],'red-grid')
            width,height,ha=read_plane(out/'Ha.fits');_,_,oxygen=read_plane(out/'OIII.fits')
            def center(values,x,y):
                mass=mx=my=0
                for yy in range(max(0,int(y)-4),min(height,int(y)+6)):
                    for xx in range(max(0,int(x)-4),min(width,int(x)+6)):
                        if (xx-x)**2+(yy-y)**2>16:continue
                        value=values[yy*width+xx]-50
                        self.assertTrue(math.isfinite(value));mass+=value;mx+=value*xx;my+=value*yy
                return mx/mass,my/mass,mass
            for x,y,_ in stars:
                a,b=center(ha,x/2,y/2),center(oxygen,x/2,y/2)
                self.assertLess(math.hypot(a[0]-b[0],a[1]-b[1]),.006)
                self.assertAlmostEqual(b[2]/a[2],.5,delta=.002)
            again=root/'again';run('narrowband-replay','--recipe',out/'recipe.txt','--output-dir',again)
            for name in report['outputs']:self.assertEqual((out/name).read_bytes(),(again/name).read_bytes())

    def test_point_source_centroid_flux_and_expected_interpolation_width(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);w,h=112,96;cx,cy=51.27,43.61;amplitude=1000;bg=-20
            def moments(data):
                ww,hh,values=data;weights=[max(0,v-bg) if math.isfinite(v) else 0 for v in values]
                flux=sum(weights);mx=sum(weights[i]*(i%ww) for i in range(len(weights)))/flux
                my=sum(weights[i]*(i//ww) for i in range(len(weights)))/flux
                variance=sum(weights[i]*((i%ww-mx)**2+(i//ww-my)**2) for i in range(len(weights)))/flux/2
                return flux,mx,my,math.sqrt(variance)
            for sigma in (2,3,5):
                values=[bg+amplitude*math.exp(-((x-cx)**2+(y-cy)**2)/(2*sigma*sigma)) for y in range(h) for x in range(w)]
                for red in range(4):
                    pattern=('RGGB','GRBG','GBRG','BGGR')[red]
                    source=root/'source.fits';write_physical(source,values,w,h,pattern)
                    out=root/f'{sigma}-{red}';run('narrowband-extract','--input',source,'--output-dir',out)
                    hp=moments(read_plane(out/'Ha.fits'));op=moments(read_plane(out/'OIII.fits'))
                    expected=((cx-red%2)/2,(cy-red//2)/2)
                    for got in (hp,op):
                        self.assertAlmostEqual(got[1],expected[0],delta=.001)
                        self.assertAlmostEqual(got[2],expected[1],delta=.001)
                        self.assertAlmostEqual(got[0]/(amplitude*2*math.pi*sigma*sigma/4),1,delta=.00001)
                    self.assertAlmostEqual(op[3],math.sqrt(sigma*sigma+2/3)/2,delta=.001)
                    self.assertAlmostEqual(hp[3],sigma/2,delta=.001)
                    old=root/f'old-{sigma}-{red}'
                    run('narrowband-extract','--input',source,'--output-dir',old,'--sampling','cell-average')
                    lp=moments(read_plane(old/'OIII.fits'))
                    for axis in (0,1):
                        r=(red%2,red//2)[axis]
                        expected_bias=(r-(2-r)/3)/2
                        self.assertAlmostEqual(lp[axis+1]-hp[axis+1],expected_bias,delta=.001)


if __name__=='__main__':unittest.main()
