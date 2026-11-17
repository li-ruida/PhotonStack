"""Independent physical-pixel checks for editable background models and diagnostics."""
from pathlib import Path
import hashlib
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
CLI=Path(sys.argv.pop(1)).resolve()
W,H=160,120

def write(path,values,w=W,h=H,sky=True):
    cards=dict(SIMPLE=True,BITPIX=-32,NAXIS=3,NAXIS1=w,NAXIS2=h,NAXIS3=4)
    if sky:
        cards.update(CTYPE1="RA---TAN-SIP",CTYPE2="DEC--TAN-SIP",CRPIX1=80.5,CRPIX2=60.5,
            CRVAL1=313.0,CRVAL2=30.0,CD1_1=-.001,CD2_2=.001,A_ORDER=2,B_ORDER=2,A_2_0=1e-5,B_0_2=-2e-5)
    def fmt(v):return "T" if v is True else repr(v)
    header="".join(f"{k:<8}= {fmt(v):>20}".ljust(80) for k,v in cards.items())+"END"
    data=struct.pack(f">{len(values)}f",*values)
    path.write_bytes(header.ljust(math.ceil(len(header)/2880)*2880).encode()+data.ljust(math.ceil(len(data)/2880)*2880,b"\0"))

def load(path):
    data=path.read_bytes();head={}
    for i in range(0,len(data),80):
        key=data[i:i+8].strip().decode()
        if key=="END":break
        head[key]=data[i+10:i+80].strip().decode()
    n=int(head["NAXIS1"])*int(head["NAXIS2"])
    flat=struct.unpack_from(f">{4*n}f",data,(i//2880+1)*2880)
    return head,[flat[c*n:(c+1)*n] for c in range(4)]

def samples(path,points):
    path.write_text("PhotonStack-BackgroundSamples-1\n"+"".join(f"{x} {y} {r} {int(e)}\n" for x,y,r,e in points))

def fixture(root):
    bg=lambda x,y,c:-12+50*c+.17*x-.11*y
    signal=lambda x,y: 75*max(0,1-((x-80)/30)**2-((y-60)/25)**2)**3
    values=[bg(x,y,c)+signal(x,y)*(c+1) for c in range(3) for y in range(H) for x in range(W)]
    values += [0 if x<4 else .5 if x==4 else 1 for y in range(H) for x in range(W)]
    write(root/"source.fits",values)
    points=[(x,y,4,True) for y in [8,28,48,68,88,108] for x in [8,26,44,62,98,116,134,152]
            if ((x-80)/36)**2+((y-60)/31)**2>1]
    samples(root/"samples.txt",points)
    return values,bg,signal,points

def run(root,out="review",*extra):
    return subprocess.run([CLI,"background-review","--input",root/"source.fits","--output-dir",root/out,
        "--samples",root/"samples.txt","--degree","1","--strength","1","--preserve-brightness","off",*extra],capture_output=True)

class Review(unittest.TestCase):
    def test_known_plane_preserves_nebula_signed_range_alpha_and_wcs(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);values,bg,signal,points=fixture(p);before=(p/"source.fits").read_bytes()
            r=run(p);self.assertEqual(r.returncode,0,r.stderr)
            header,out=load(p/"review/corrected.fits");_,model=load(p/"review/model.fits");_,diff=load(p/"review/difference.fits")
            self.assertEqual(header["CTYPE1"],"'RA---TAN-SIP'")
            self.assertAlmostEqual(float(header["A_2_0"]),1e-5)
            for y in range(H):
                for x in range(W):
                    i=y*W+x
                    self.assertEqual(out[3][i],values[3*W*H+i])
                    for c in range(3):
                        if x<4:
                            self.assertEqual(out[c][i],0);continue
                        self.assertAlmostEqual(model[c][i],bg(x,y,c),delta=.0001)
                        self.assertAlmostEqual(out[c][i],signal(x,y)*(c+1),delta=.0001)
                        self.assertAlmostEqual(diff[c][i]+out[c][i],values[c*W*H+i],delta=.0001)
            report=json.loads((p/"review/report.json").read_text())
            self.assertTrue(report["wcsPreserved"])
            self.assertLess(max(report["residualRMS"]),1e-5)
            self.assertEqual(report["sourceSHA"],hashlib.sha256(before).hexdigest())
            self.assertEqual((p/"source.fits").read_bytes(),before)

    def test_rejected_samples_and_existing_output_are_explicit(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);values,bg,signal,points=fixture(p)
            # One contaminated aperture is rejected independently in each color.
            for c in range(3):
                for y in range(4,13):
                    for x in range(22,31):values[c*W*H+y*W+x]+=400
            write(p/"source.fits",values)
            samples(p/"samples.txt",points+[(80,60,4,False),(1,60,4,True),(5,60,4,True)])
            r=run(p);self.assertEqual(r.returncode,0,r.stderr)
            report=json.loads((p/"review/report.json").read_text());states=[v["status"] for v in report["samples"]]
            for expected in ("outlier","disabled","edge","coverage"):self.assertIn(expected,states)
            self.assertEqual(report["samples"][1]["used"],[False]*3)
            before=(p/"review/corrected.fits").read_bytes()
            self.assertNotEqual(run(p).returncode,0)
            self.assertEqual((p/"review/corrected.fits").read_bytes(),before)

    def test_stale_recipe_degenerate_geometry_and_bad_samples(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);fixture(p)
            self.assertNotEqual(run(p,"bad","--expected-sha","0"*64).returncode,0)
            self.assertFalse((p/"bad").exists())
            samples(p/"samples.txt",[(10+i*5,10+i*3,4,True) for i in range(15)])
            self.assertNotEqual(run(p,"rank").returncode,0)
            self.assertFalse((p/"rank").exists())
            for points in ([],[(float("nan"),10,4,True)],[(W,10,4,True)],[(20,20,0,True)]):
                samples(p/"samples.txt",points)
                self.assertNotEqual(run(p,"invalid").returncode,0)
                self.assertFalse((p/"invalid").exists())

    def test_zero_strength_identity_and_inspection(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);fixture(p)
            r=subprocess.run([CLI,"background-review","--input",p/"source.fits","--output-dir",p/"inspect","--inspect-only","on"],capture_output=True)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertFalse((p/"inspect/corrected.fits").exists())
            self.assertEqual(len(json.loads((p/"inspect/report.json").read_text())["samples"]),96)
            r=subprocess.run([CLI,"background-review","--input",p/"source.fits","--output-dir",p/"zero",
                "--samples",p/"samples.txt","--degree","1","--strength","0"],capture_output=True)
            self.assertEqual(r.returncode,0,r.stderr)
            _,a=load(p/"source.fits");_,b=load(p/"zero/corrected.fits")
            for c in range(3):
                for i in range(W*H):
                    if a[3][i]>0:self.assertEqual(a[c][i],b[c][i])
            _,d=load(p/"zero/difference.fits")
            self.assertTrue(all(v==0 for c in d[:3] for v in c))
            # Zero residual must be display-code midgray, not gamma-encoded 0.5 linear.
            r=subprocess.run([CLI,"convert","--input",p/"zero/difference.tiff","--output",p/"display.fits"],capture_output=True)
            self.assertEqual(r.returncode,0,r.stderr)
            _,display=load(p/"display.fits")
            for c in range(3):
                self.assertAlmostEqual(display[c][60*W+80],((.5+.055)/1.055)**2.4,delta=3e-5)
if __name__=="__main__":unittest.main()
