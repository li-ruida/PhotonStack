"""Independent physical FITS oracle for registered mono channel composition."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = Path(sys.argv.pop(1)).resolve()


def write(path, values, w=8, h=6, extra=None):
    cards = {'SIMPLE':'T','BITPIX':-32,'NAXIS':2,'NAXIS1':w,'NAXIS2':h}
    cards.update(extra or {})
    header=''.join(f'{k:<8}= {str(v):>20}'.ljust(80) for k,v in cards.items())+'END'
    header=header.ljust(((len(header)+2879)//2880)*2880)
    data=struct.pack(f'>{len(values)}f',*values)
    path.write_bytes(header.encode()+data.ljust(((len(data)+2879)//2880)*2880,b'\0'))


def read(path):
    data=path.read_bytes();header={}
    for i in range(0,len(data),80):
        key=data[i:i+8].decode().strip()
        if key=='END':break
        header[key]=data[i+10:i+80].decode().strip()
    n=int(header['NAXIS1'])*int(header['NAXIS2']);c=int(header.get('NAXIS3',1))
    values=struct.unpack_from(f'>{n*c}f',data,((i+80+2879)//2880)*2880)
    return header,[values[j*n:(j+1)*n] for j in range(c)]


def run(*args,ok=True):
    r=subprocess.run([CLI,*map(str,args)],capture_output=True,text=True)
    if ok:
        if r.returncode:raise AssertionError(r.stderr)
        return json.loads(r.stdout)
    if not r.returncode:raise AssertionError('Invalid composition succeeded')
    return r.stderr


def compose(root, symbols, expressions, name='result', options=()):
    args=['channel-compose','--aligned','--output-dir',root/name]
    for symbol in symbols:args+=['--input',f'{symbol}={root/(symbol+".fits")}']
    for key,value in zip(('red','green','blue'),expressions):args+=['--'+key,value]
    return run(*args,*options)


class Channels(unittest.TestCase):
    def test_linear_mapping_mixing_functions_and_scale(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);h=[-20+i*.5 for i in range(48)];o=[30-i*.25 for i in range(48)]
            write(p/'Ha.fits',[(v+31)/2 for v in h],extra={'BSCALE':2,'BZERO':-31})
            write(p/'OIII.fits',o)
            before={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in p.glob('*.fits')}
            cases=[(('Ha','0.25*Ha+0.75*OIII','OIII'),(h,[.25*a+.75*b for a,b in zip(h,o)],o)),
                   (('-(Ha+2)*3','sqrt(abs(Ha))','min(max(OIII,20),28)+pow(2,3)/4'),
                    ([-(a+2)*3 for a in h],[math.sqrt(abs(a)) for a in h],[min(max(b,20),28)+2 for b in o])),
                   (('1e-3 + +Ha','Ha/2/5','OIII-Ha-2'),([a+.001 for a in h],[a/10 for a in h],[b-a-2 for a,b in zip(h,o)]))]
            for i,(expressions,expected) in enumerate(cases):
                r=compose(p,['Ha','OIII'],expressions,str(i));self.assertEqual(r['invalidPixels'],0)
                _,actual=read(p/str(i)/'RGB.fits')
                for plane,truth in zip(actual,expected):
                    for a,b in zip(plane,truth):self.assertAlmostEqual(a,b,delta=1e-5)
            self.assertEqual(before,{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in p.glob('*.fits')})

    def test_lrgb_preserves_ratios_and_rec709_luminance(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);rgb=[[1+i*(c+1) for i in range(48)] for c in range(3)];l=[50+i for i in range(48)]
            for name,values in zip(('R','G','B','L'),rgb+[l]):write(p/(name+'.fits'),values)
            expressions=[f'L*{c}/(0.2126*R+0.7152*G+0.0722*B)' for c in 'RGB']
            compose(p,list('RGBL'),expressions)
            _,planes=read(p/'result/RGB.fits')
            for i in range(48):
                self.assertAlmostEqual(sum(planes[c][i]*w for c,w in enumerate((.2126,.7152,.0722))),l[i],delta=1e-5)
                self.assertAlmostEqual(planes[0][i]/planes[2][i],rgb[0][i]/rgb[2][i],delta=1e-6)

    def test_invalid_dependency_domain_overflow_and_unused_source(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);a=[2.]*48;a[1]=math.nan;a[2]=0;a[3]=-2;a[4]=3e38;a[5]=math.inf
            write(p/'A.fits',a);write(p/'Unused.fits',[math.nan]*48)
            r=compose(p,['A','Unused'],['1/A','sqrt(A)','A*100'])
            self.assertEqual(r['invalidPixels'],5)
            _,valid=read(p/'result/validity.fits');_,rgb=read(p/'result/RGB.fits')
            for i in range(48):
                self.assertEqual(valid[0][i],0 if i in (1,2,3,4,5) else 1)
            # With all invalid values only in an unused master, finite constants
            # remain meaningful; max/min do not suppress invalid dependencies.
            compose(p,['Unused'],['2','3','4'],'constant')
            for n,expression in enumerate(('max(A,0)','min(0,A)','0*A')):
                r=compose(p,['A'],[expression,'A','A'],'finite-'+str(n))
                self.assertEqual(r['invalidPixels'],2)
            run('channel-compose','--aligned','--input',f'A={p/"A.fits"}','--red','1/0','--green','A','--blue','A','--output-dir',p/'none',ok=False)
            self.assertFalse((p/'none').exists())

    def test_recipe_reopen_replay_override_and_tamper_protection(self):
        with tempfile.TemporaryDirectory(prefix='channels spaces ') as d:
            p=Path(d);write(p/'Ha.fits',[1+i for i in range(48)]);write(p/'OIII.fits',[2.]*48)
            compose(p,['Ha','OIII'],['Ha','OIII','OIII'])
            recipe=p/'result/recipe.txt'
            r=run('channel-inspect','--recipe',recipe);self.assertEqual(r['expressions'],['Ha','OIII','OIII'])
            run('channel-replay','--recipe',recipe,'--output-dir',p/'replay','--workers','1')
            for name in ('RGB.fits','validity.fits'):self.assertEqual((p/'result'/name).read_bytes(),(p/'replay'/name).read_bytes())
            run('channel-replay','--recipe',recipe,'--output-dir',p/'changed','--green','0.5*Ha+OIII')
            self.assertNotEqual((p/'changed/RGB.fits').read_bytes(),(p/'result/RGB.fits').read_bytes())
            saved=(p/'result/RGB.fits').read_bytes()
            run('channel-replay','--recipe',recipe,'--output-dir',p/'result',ok=False)
            self.assertEqual((p/'result/RGB.fits').read_bytes(),saved)
            (p/'result/RGB.fits').write_bytes(saved[:-1]+b'X')
            run('channel-inspect','--recipe',recipe,ok=False)
            (p/'result/RGB.fits').write_bytes(saved)
            with (p/'Ha.fits').open('ab') as f:f.write(b'changed')
            run('channel-replay','--recipe',recipe,'--output-dir',p/'badsource',ok=False)
            self.assertFalse((p/'badsource').exists())

    def test_parser_bounds_and_bad_recipe(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);write(p/'A.fits',[2.]*48)
            invalid=['','Unknown','A;system(1)','sqrt(A,2)','pow(A)','A**2','A^2','min(A)','A+','1e309','('*34+'A'+')'*34,'A+'*200+'A','abs(A','foo(A)','A\nA','A+"x"']
            for n,expr in enumerate(invalid):
                run('channel-compose','--aligned','--input',f'A={p/"A.fits"}','--red',expr,'--green','A','--blue','A','--output-dir',p/str(n),ok=False)
                self.assertFalse((p/str(n)).exists())
            compose(p,['A'],['A']*3)
            recipe=p/'result/recipe.txt';original=recipe.read_text()
            run('channel-replay','--recipe',recipe,'--output-dir',p/'empty-expression','--red','',ok=False)
            self.assertFalse((p/'empty-expression').exists())
            for text in (original+'expression 0 "A"\n',original.replace('aligned 1','aligned 0'),original.replace('"RGB.fits"','"../outside.fits"'),original.replace('expression 2','expression 9')):
                recipe.write_text(text);run('channel-inspect','--recipe',recipe,ok=False)

    def test_mono_linear_grid_and_alignment_contracts(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);write(p/'A.fits',[2.]*48)
            args=['channel-compose','--input',f'A={p/"A.fits"}','--red','A','--green','A','--blue','A','--output-dir',p/'out']
            run(*args,ok=False)
            for extra in ({'BAYERPAT':"'RGGB'"},{'PSCOLOR':"'SRGB'"},{'CTYPE1':"'RA---TAN'"},{'CD2_2':.001},{'PC1_1':1}):
                write(p/'A.fits',[2.]*48,extra=extra);run(*args,'--aligned',ok=False)
            write(p/'A.fits',[2.]*144,extra={'NAXIS':3,'NAXIS3':3});run(*args,'--aligned',ok=False)
            write(p/'A.fits',[2.]*48);write(p/'B.fits',[2.]*49,7,7)
            run(*args,'--aligned','--input',f'B={p/"B.fits"}',ok=False)
            run(*args,'--aligned','--input',f'A={p/"A.fits"}',ok=False)
            self.assertFalse((p/'out').exists())

    def test_wcs_identity_preserved_mismatch_rejected_and_unknown_not_invented(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);wcs={'CTYPE1':"'RA---TAN-SIP'",'CTYPE2':"'DEC--TAN-SIP'",'CRVAL1':359.99,'CRVAL2':30,'CRPIX1':4.5,'CRPIX2':3.5,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001,'A_ORDER':2,'B_ORDER':2,'A_2_0':1e-5,'B_0_2':-1e-5}
            for name in ('A','B'):write(p/(name+'.fits'),[2.]*48,extra=wcs)
            r=compose(p,['A','B'],['A','B','A+B']);self.assertTrue(r['wcsPreserved'])
            header,_=read(p/'result/RGB.fits')
            for k,v in wcs.items():
                if isinstance(v,str):self.assertEqual(header[k],v)
                else:self.assertAlmostEqual(float(header[k]),v,delta=1e-12)
            write(p/'B.fits',[2.]*48,extra={**wcs,'CRPIX1':5.5})
            run('channel-compose','--aligned','--input',f'A={p/"A.fits"}','--input',f'B={p/"B.fits"}','--red','A','--green','B','--blue','A','--output-dir',p/'wrong',ok=False)
            write(p/'B.fits',[2.]*48)
            r=compose(p,['A','B'],['A','B','A+B'],'unknown');self.assertFalse(r['wcsPreserved'])
            self.assertNotIn('CTYPE1',read(p/'unknown/RGB.fits')[0])

    def test_parallel_determinism(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);write(p/'A.fits',[math.sin(i/13)*100 for i in range(320*200)],320,200)
            expressions=['A','abs(A)','pow(abs(A)+1,0.3)']
            for workers in (1,4):compose(p,['A'],expressions,str(workers),('--workers',str(workers)))
            for name in ('RGB.fits','validity.fits'):self.assertEqual((p/'1'/name).read_bytes(),(p/'4'/name).read_bytes())

if __name__=='__main__':unittest.main()
