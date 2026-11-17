"""FITS mosaic regression. The golden samples are independent Astropy/WCSLIB projections."""
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = Path(sys.argv.pop(1)).resolve()
W, H = 96, 88

def fixture(panel):
    h = dict(CTYPE1="RA---TAN-SIP", CTYPE2="DEC--TAN-SIP", CRVAL1=359.89,
             CRVAL2=32.0, CRPIX1=46.3, CRPIX2=48.2, CD1_1=-.003, CD2_2=.003,
             A_ORDER=2, B_ORDER=2, A_2_0=-.00007, B_1_1=.00005)
    if panel:
        del h["CD1_1"], h["CD2_2"]
        h.update(CRVAL1=.071, CRVAL2=32.041, CDELT1=-.003, CDELT2=.003,
                 PC1_1=.94, PC1_2=-.33, PC2_1=.33, PC2_2=.94)
    return h

def write(path, h, k, stars=False):
    cards = dict(SIMPLE=True,BITPIX=-32,NAXIS=2,NAXIS1=W,NAXIS2=H,**h)
    def val(v):
        if v is True: return "T"
        return repr(v).replace("e-","E-") if not isinstance(v,str) else repr(v)
    header = "".join(f"{key:<8}= {val(v):>20}".ljust(80) for key,v in cards.items())+"END"
    rng=random.Random(929)
    peaks=[(rng.randrange(6,W-6),rng.randrange(6,H-6),rng.randrange(90,360)) for _ in range(80)] if stars else []
    vals=[1000*k+3*x-2*y+sum(a*math.exp(-((x-cx)**2+(y-cy)**2)/1.5) for cx,cy,a in peaks)
          for y in range(H) for x in range(W)]
    data = struct.pack(f">{W*H}f",*vals)
    path.write_bytes(header.ljust(2880).encode()+data.ljust(math.ceil(len(data)/2880)*2880,b"\0"))

def load(path):
    d=path.read_bytes()
    cards={}
    for i in range(0,len(d),80):
        key=d[i:i+8].strip().decode()
        if key=="END": break
        cards[key]=d[i+10:i+80].strip().decode()
    off=(i//2880+1)*2880
    width,height=int(cards["NAXIS1"]),int(cards["NAXIS2"])
    return cards,struct.unpack_from(f">{width*height*4}f",d,off)

def command(root, *args):
    return subprocess.run([CLI,"mosaic","--alignment","wcs","--blend","average","--exposure-match","off",
                           "--output",root/"out.fits",*args,root/"a.fits",root/"b.fits"],capture_output=True)

# Each row is (width, height, CRPIX1, CRPIX2, [(x,y,expected ADU,coverage), ...]).
GOLDEN = {0: (158, 117, 108.3, 49.2, [(0, 0, 0, 0), (14, 46, 908.541149, 1.0), (79, 58, 531.145831, 1.0), (104, 58, 612.515188, 1.0), (129, 56, 91.116122, 1.0), (31, 84, 933.400653, 1.0), (142, 24, 194.170062, 1.0), (126, 99, 0, 0)]), 48: (81, 60, 55.4, 25.35, [(0, 0, 0, 0), (7, 24, 904.014225, 0.308269), (40, 30, 527.344965, 1.0), (53, 30, 611.975192, 1.0), (66, 28, 96.630343, 1.0), (16, 43, 930.291705, 1.0), (72, 12, 196.674483, 1.0), (64, 51, 0, 0)])}

class Mosaic(unittest.TestCase):
    def test_rotated_sip_crossing_ra_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for i,name in enumerate(["a.fits","b.fits"]):write(root/name,fixture(i),i)
            for preview,(w,h,cx,cy,samples) in GOLDEN.items():
                r=command(root,*(["--preview-width",str(preview)] if preview else []))
                self.assertEqual(r.returncode,0,r.stderr)
                cards,values=load(root/"out.fits")
                self.assertEqual((int(cards["NAXIS1"]),int(cards["NAXIS2"])),(w,h))
                self.assertAlmostEqual(float(cards["CRPIX1"]),cx)
                self.assertAlmostEqual(float(cards["CRPIX2"]),cy)
                self.assertEqual(cards["CTYPE1"],"'RA---TAN'")
                self.assertNotIn("A_ORDER",cards)
                for x,y,value,alpha in samples:
                    self.assertAlmostEqual(values[3*w*h+y*w+x],alpha,delta=.00002)
                    self.assertAlmostEqual(values[y*w+x],value,delta=.0005)

    def test_invalid_header_and_false_solution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            write(root/"a.fits",fixture(0),0,True)
            for change in (dict(CD1_1=0),dict(CUNIT1="rad"),dict(A_ORDER=9),dict(CRPIX1=50.3),
                           dict(CRVAL1=180),dict(CTYPE2="DEC--SIN")):
                h=dict(fixture(0),**change)
                write(root/"b.fits",h,0,True)
                r=command(root)
                self.assertNotEqual(r.returncode,0,change)
                self.assertFalse((root/"out.fits").exists())

if __name__=="__main__":unittest.main()
