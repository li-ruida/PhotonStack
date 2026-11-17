"""Independent numerical checks of mosaic overlap measurements, without third-party Python dependencies."""
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
W, H = 96, 88

def f32(v): return struct.unpack('f', struct.pack('f', v))[0]
def med(a): return sorted(a)[len(a)//2]
def write(path, panel, masked=False):
    header = dict(SIMPLE=True, BITPIX=-32, NAXIS=3, NAXIS1=W, NAXIS2=H, NAXIS3=4)
    cards = ''.join(f'{k:<8}= {("T" if v is True else str(v)):>20}'.ljust(80) for k,v in header.items())+'END'
    values=[]
    for c in range(4):
        for y in range(H):
            for x in range(W):
                if c==3: v=0.5 if masked and (y<30 or (45<x<70 and 50<y<70)) else 1
                else:
                    sky = -30+20*c+.7*(x+panel*48)-.23*y + 4*math.sin((x+panel*48)/10)
                    v=sky if panel==0 else sky*(1+.1*c)+1+c
                values.append(f32(v))
    data=struct.pack(f'>{len(values)}f', *values)
    path.write_bytes(cards.ljust(2880).encode()+data.ljust(math.ceil(len(data)/2880)*2880,b'\0'))
    return [values[c*W*H:(c+1)*W*H] for c in range(4)]

def run(root, name, files, *options):
    p=subprocess.run([CLI,'mosaic','--alignment','manual','--overlap-pixels','48','--output',root/(name+'.fits'),
                      *options,*files],capture_output=True,text=True)
    if p.returncode: raise AssertionError(p.stdout+p.stderr)
    decoder=json.JSONDecoder(); pos=0; objects=[]
    while pos<len(p.stdout):
        start=p.stdout.find('{',pos)
        if start<0: break
        obj,n=decoder.raw_decode(p.stdout[start:]); objects.append(obj);pos=start+n
    return next(o for o in objects if o.get('type')=='complete')

def expected(a,b, pair, corrections):
    bounds=pair['summary']['bounds']; left,top,right,bottom=map(int,bounds)
    nx,ny=min(48,right-left),min(48,bottom-top)
    samples=[];cells=[[] for _ in range(16)];candidates=[0]*16
    for gy in range(ny):
        for gx in range(nx):
            x=left+int((gx+.5)*(right-left)/nx);y=top+int((gy+.5)*(bottom-top)/ny)
            cell=min(3,int((y-top)*4/(bottom-top)))*4+min(3,int((x-left)*4/(right-left)))
            candidates[cell]+=1
            patch=[]
            for dy in [-1,0,1]:
                for dx in [-1,0,1]:
                    ax,ay=x+dx,y+dy;bx=ax-48
                    if not (0<=ax<W and 0<=bx<W and 0<=ay<H): continue
                    ai=ay*W+ax;bi=ay*W+bx
                    if a[3][ai]<.99 or b[3][bi]<.99: continue
                    raw=[[a[c][ai] for c in range(3)],[b[c][bi] for c in range(3)]]
                    adjusted=[[raw[k][c]*corrections[k]['gains'][c]+corrections[k]['offsets'][c] for c in range(3)] for k in range(2)]
                    patch.append(raw+adjusted)
            if len(patch)!=9:continue
            value=[[med([p[k][c] for p in patch]) for c in range(3)] for k in range(4)]
            samples.append(value);cells[cell].append(value)
    return samples,cells,candidates

def stats(samples,offset):
    if not samples:return None
    result=dict(samples=len(samples),firstMedian=[],secondMedian=[],medianDifference=[],rmsDifference=[],p95Absolute=[],madDifference=[])
    for c in range(3):
        a=[s[offset][c] for s in samples];b=[s[offset+1][c] for s in samples];d=[y-x for x,y in zip(a,b)];center=med(d)
        for key,v in dict(firstMedian=med(a),secondMedian=med(b),medianDifference=center,
                          rmsDifference=math.sqrt(sum(v*v for v in d)/len(d)),
                          p95Absolute=sorted(map(abs,d))[math.ceil(.95*len(d))-1],
                          madDifference=med([abs(v-center) for v in d])).items(): result[key].append(v)
    return result

class Seams(unittest.TestCase):
    def compare(self,actual,expected):
        self.assertEqual(actual is None,expected is None)
        if expected is None:return
        self.assertEqual(actual['samples'],expected['samples'])
        for k in expected:
            if k=='samples':continue
            for a,b in zip(actual[k],expected[k]):self.assertAlmostEqual(a,b,delta=1e-8)
    def test_matching_and_blends_keep_science_pixels_unchanged_by_diagnostics(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);files=[root/'a.fits',root/'b.fits'];a=write(files[0],0);b=write(files[1],1)
            hashes=[hashlib.sha256(p.read_bytes()).hexdigest() for p in files]
            for blend in ['average','feather','multiband']:
                for matching in ['off','on']:
                    name=blend+matching
                    report=run(root,name,files,'--blend',blend,'--exposure-match',matching)
                    seam=report['seams'];pair=seam['pairs'][0]
                    self.assertEqual((seam['width'],seam['height']),(144,88));self.assertFalse(seam['preview'])
                    samples,cells,candidates=expected(a,b,pair,seam['corrections'])
                    for key,offset in [('before',0),('after',2)]:
                        self.compare(pair['summary'][key],stats(samples,offset))
                        for i,cell in enumerate(pair['cells']):
                            self.compare(cell[key],stats(cells[i],offset));self.assertEqual(cell['candidates'],candidates[i])
                    if matching=='on':
                        for before,after in zip(pair['summary']['before']['rmsDifference'],pair['summary']['after']['rmsDifference']):
                            self.assertLess(after,before*.001)
                    else:self.assertEqual(pair['summary']['before'],pair['summary']['after'])
                    disabled=run(root,name+'off',files,'--blend',blend,'--exposure-match',matching,'--seam-diagnostics','off')
                    self.assertIsNone(disabled['seams'])
                    self.assertEqual((root/(name+'.fits')).read_bytes(),(root/(name+'off.fits')).read_bytes())
            self.assertEqual(hashes,[hashlib.sha256(p.read_bytes()).hexdigest() for p in files])
    def test_local_gradient_is_visible_when_global_bias_is_small(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);files=[root/'a.fits',root/'b.fits'];write(files[0],0);write(files[1],1)
            raw=bytearray(files[1].read_bytes())
            for c in range(3):
                for y in range(H):
                    for x in range(W):
                        sky=-30+20*c+.7*(x+48)-.23*y+4*math.sin((x+48)/10)
                        struct.pack_into('>f',raw,2880+4*(c*W*H+y*W+x),sky+(y-(H-1)/2)*.5)
            files[1].write_bytes(raw)
            pair=run(root,'gradient',files,'--exposure-match','off')['seams']['pairs'][0]
            global_bias=abs(pair['summary']['before']['medianDifference'][0])
            self.assertLess(global_bias,1)
            self.assertGreater(pair['summary']['before']['rmsDifference'][0],10)
            local=[c['before']['medianDifference'][0] for c in pair['cells'] if c['before']]
            self.assertLess(min(local),-10);self.assertGreater(max(local),10)

    def test_masked_empty_and_disconnected_pairs(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);files=[root/'a.fits',root/'b.fits'];a=write(files[0],0);b=write(files[1],1,True)
            report=run(root,'masked',files,'--exposure-match','off');seam=report['seams'];pair=seam['pairs'][0]
            samples,cells,_=expected(a,b,pair,seam['corrections'])
            self.compare(pair['summary']['before'],stats(samples,0))
            self.assertTrue(any(c['before'] is None for c in pair['cells']))
            for i,c in enumerate(pair['cells']):self.compare(c['before'],stats(cells[i],0))
            thin=run(root,'thin',files,'--overlap-pixels','1')['seams']['pairs'][0]
            self.assertIsNone(thin['summary']['before']);self.assertGreater(thin['summary']['candidates'],0)
            none=run(root,'none',files,'--overlap-pixels','0')['seams'];self.assertEqual(none['pairs'],[])
            third=root/'c.fits';write(third,2)
            chain=run(root,'chain',files+[third])['seams'];self.assertEqual([(p['first'],p['second']) for p in chain['pairs']],[(0,1),(1,2)])
            preview=run(root,'preview',files,'--preview-width','48')['seams'];self.assertTrue(preview['preview'])
            self.assertEqual(preview['width'],72)

if __name__=='__main__':unittest.main()
