"""Plate-solve orchestration with an independently generated analytic star field.
The deterministic local solver fixture exercises the contract, not real solving;
real Astrometry.net + catalog checks live in tools/scripts/qa and validation docs.
"""
import hashlib
import json
import math
from pathlib import Path
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from test_channels import CLI, write

FIXTURE = r'''
import math,struct,sys
from pathlib import Path

def header(cards):
 s=''.join(f'{k:<8}= {str(v):>20}'.ljust(80) for k,v in cards.items())+'END'
 return s.ljust((len(s)+2879)//2880*2880).encode()
def table(file,columns):
 keys=list(columns);n=len(columns[keys[0]])
 p=header({'SIMPLE':'T','BITPIX':8,'NAXIS':0,'EXTEND':'T'})
 h={'XTENSION':"'BINTABLE'",'BITPIX':8,'NAXIS':2,'NAXIS1':len(keys)*8,'NAXIS2':n,'PCOUNT':0,'GCOUNT':1,'TFIELDS':len(keys)}
 for i,k in enumerate(keys,1):h['TTYPE'+str(i)]=repr(k);h['TFORM'+str(i)]="'1D'"
 data=b''.join(struct.pack('>d',columns[k][r]) for r in range(n) for k in keys)
 Path(file).write_bytes(p+header(h)+data.ljust((len(data)+2879)//2880*2880,b'\0'))
def read(file):
 b=Path(file).read_bytes();h={}
 for i in range(2880,len(b),80):
  k=b[i:i+8].decode().strip()
  if k=='END':break
  h[k]=b[i+10:i+80].decode().strip().strip("'")
 keys=[h['TTYPE'+str(i+1)] for i in range(int(h['TFIELDS']))]
 offset=(i+80+2879)//2880*2880
 data=struct.unpack_from('>'+str(int(h['NAXIS2'])*len(keys))+'d',b,offset)
 return {k:list(data[j::len(keys)]) for j,k in enumerate(keys)}
s=read('sources.xyls');n=len(s['X']);mode=Path(sys.argv[0]).stem
if mode=='unsolved':sys.exit(0)
# Independent inverse gnomonic transform, chosen pointing across RA=0.
a=math.radians(359.9);d=math.radians(30)
def world(x,y):
 xi=math.radians(-(x-128)*.002);eta=math.radians((y-128)*.002)
 ra=a+math.atan2(xi,math.cos(d)-eta*math.sin(d))
 dec=math.atan2(math.sin(d)+eta*math.cos(d),math.hypot(math.cos(d)-eta*math.sin(d),xi))
 return math.degrees(ra)%360,math.degrees(dec)
ra,dec=zip(*(world(x,y) for x,y in zip(s['X'],s['Y'])))
wcs={'SIMPLE':'T','BITPIX':8,'NAXIS':0,'WCSAXES':2,'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'CRPIX1':128,'CRPIX2':128,'CRVAL1':359.9,'CRVAL2':30,'CD1_1':-.002,'CD1_2':0,'CD2_1':0,'CD2_2':.002,'EQUINOX':2000}
if mode=='wrong':wcs['CRVAL1']=40
Path('solution.wcs').write_bytes(header(wcs));Path('solution.solved').write_bytes(b'\x01')
table('solution.corr',{'field_x':s['X'],'field_y':s['Y'],'index_ra':ra,'index_dec':dec,'match_weight':[.99]*n})
xs=[20+30*i+.2+1 for j in range(8) for i in range(8)];ys=[20+30*j+.35+1 for j in range(8) for i in range(8)]
if mode=='noheld':xs=s['X'];ys=s['Y']
table('solution-indx.xyls',{'X':xs,'Y':ys})
'''

def setup(p):
    n=256;values=[-10.]*n*n
    for j in range(8):
        for i in range(8):
            x=20+30*i+.2;y=20+30*j+.35;amp=100+(8*j+i)*11
            for yy in range(int(y)-6,int(y)+7):
                for xx in range(int(x)-6,int(x)+7):values[yy*n+xx]+=amp*math.exp(-((xx-x)**2+(yy-y)**2)/(2*1.25**2))
    source=p/'source.fits';write(source,values,n,n,{'CTYPE1':"'BROKEN'",'DATE-OBS':"'2026-09-09'",'EXPTIME':180,'FILTER':"'Ha'"})
    index=p/'index.fits';index.write_bytes(b'fixture index, not a real sky catalog')
    for name in ['solver','wrong','unsolved','noheld']:
        f=p/name;f.write_text('#!'+sys.executable+'\n'+FIXTURE);f.chmod(0o755)
    sleeper = p/'sleep'
    sleeper.write_text('#!/bin/sh\n/bin/sleep 60 &\necho "$!" > child.tmp\n/bin/mv child.tmp child.pid\nwait\n')
    sleeper.chmod(0o755)
    return source,index

def command(p,out='out',solver='solver',extra=()):
    return [str(CLI),'plate-solve','--input',str(p/'source.fits'),'--solver',str(p/solver),'--index',str(p/'index.fits'),'--index-frame','ICRS','--output-dir',str(p/out),*extra]

class PlateSolve(unittest.TestCase):
    def test_validation_preservation_replay(self):
        with tempfile.TemporaryDirectory(prefix='plate solve ') as d:
            p=Path(d);source,index=setup(p);original=source.read_bytes()
            result=subprocess.run(command(p),capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            r=json.loads(result.stdout);self.assertGreaterEqual(r['validation']['count'],6);self.assertLess(r['validation']['p95Pixels'],.001)
            self.assertTrue(r['originalWcsIgnored']);self.assertEqual(source.read_bytes(),original)
            result2=subprocess.run([CLI,'plate-solve-inspect','--recipe',p/'out/recipe.txt'],capture_output=True,text=True)
            self.assertEqual(json.loads(result2.stdout),r)
            replay=subprocess.run([CLI,'plate-solve-replay','--recipe',p/'out/recipe.txt','--output-dir',p/'replay'],capture_output=True,text=True)
            self.assertEqual(replay.returncode,0,replay.stdout+replay.stderr)
            self.assertEqual((p/'out/solved.fits').read_bytes(),(p/'replay/solved.fits').read_bytes())
            exported=subprocess.run([CLI,'plate-solve-export','--recipe',p/'out/recipe.txt','--output-dir',p/'exported'],capture_output=True,text=True)
            self.assertEqual(exported.returncode,0,exported.stdout+exported.stderr)
            self.assertEqual((p/'out/solved.fits').read_bytes(),(p/'exported/solved.fits').read_bytes())
            digest=hashlib.sha256((p/'out/solved.fits').read_bytes()).hexdigest()
            self.assertNotEqual(subprocess.run(command(p),capture_output=True).returncode,0)
            self.assertEqual(hashlib.sha256((p/'out/solved.fits').read_bytes()).hexdigest(),digest)
            (p/'out/solution.corr').write_bytes(b'corrupt')
            self.assertNotEqual(subprocess.run([CLI,'plate-solve-inspect','--recipe',p/'out/recipe.txt'],capture_output=True).returncode,0)

    def test_false_solutions_and_dependencies(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source,index=setup(p)
            for name in ['wrong','unsolved','noheld']:
                result=subprocess.run(command(p,name+'-out',name),capture_output=True,text=True)
                self.assertNotEqual(result.returncode,0,name);self.assertFalse((p/(name+'-out')/'solved.fits').exists())
                self.assertTrue((p/(name+'-out')/'failure.txt').exists())
            for extra in [['--timeout','0'],['--scale-low','2'],['--scale-low','3','--scale-high','2'],['--index-frame','FK5']]:
                self.assertNotEqual(subprocess.run(command(p,extra=extra),capture_output=True).returncode,0)
            result=subprocess.run(command(p),capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout)
            index.write_bytes(b'changed')
            self.assertNotEqual(subprocess.run([CLI,'plate-solve-replay','--recipe',p/'out/recipe.txt','--output-dir',p/'changed'],capture_output=True).returncode,0)
            source.write_bytes(source.read_bytes()+b'x')
            self.assertNotEqual(subprocess.run([CLI,'plate-solve-inspect','--recipe',p/'out/recipe.txt'],capture_output=True).returncode,0)

    @unittest.skipUnless(os.name=='posix','POSIX process groups')
    def test_timeout_and_cancel_kill_descendants(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);setup(p)
            for mode in ['timeout','cancel']:
                start=time.monotonic();proc=subprocess.Popen(command(p,mode,'sleep',['--timeout','3' if mode=='timeout' else '60']),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                child=p/mode/'child.pid'
                while not child.exists() and proc.poll() is None and time.monotonic()-start<12:time.sleep(.02)
                if not child.exists():
                    if proc.poll() is None: proc.terminate()
                    stdout, stderr = proc.communicate(timeout=5)
                    self.fail(f'{mode} solver did not reach fork: {stdout!r} {stderr!r}')
                pid=int(child.read_text())
                if mode=='cancel':proc.terminate()
                _, stderr = proc.communicate(timeout=6)
                self.assertNotEqual(proc.returncode,0)
                self.assertIn(b'timed out' if mode=='timeout' else b'cancelled', stderr)
                for _ in range(50):
                    try:os.kill(pid,0)
                    except ProcessLookupError:break
                    time.sleep(.02)
                else:self.fail('Solver grandchild survived cancellation')
                self.assertFalse((p/mode/'solved.fits').exists())

if __name__=='__main__':unittest.main()
