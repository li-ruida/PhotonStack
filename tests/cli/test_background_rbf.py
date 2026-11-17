"""Thin-plate background regression; SciPy reference values are generated independently."""
from pathlib import Path
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
CLI=None
W,H=160,128

def image(path,width,height,values):
    cards=dict(SIMPLE=True,BITPIX=-32,NAXIS=3,NAXIS1=width,NAXIS2=height,NAXIS3=4)
    header="".join(f"{k:<8}= {('T' if v is True else str(v)):>20}".ljust(80) for k,v in cards.items())+"END"
    data=struct.pack(f">{len(values)}f",*values)
    path.write_bytes(header.ljust(2880).encode()+data.ljust(math.ceil(len(data)/2880)*2880,b"\0"))

def read(path):
    raw=path.read_bytes();header={}
    for i in range(0,len(raw),80):
        key=raw[i:i+8].strip().decode()
        if key=="END":break
        header[key]=raw[i+10:i+80].strip().decode()
    n=int(header["NAXIS1"])*int(header["NAXIS2"])
    data=struct.unpack_from(f">{4*n}f",raw,(i//2880+1)*2880)
    return [data[c*n:(c+1)*n] for c in range(4)]

def pointfile(path,points):
    path.write_text("PhotonStack-BackgroundSamples-1\n"+"".join(f"{x} {y} {r} {int(enabled)}\n" for x,y,r,enabled in points))

def fixture(root):
    points=[(x,y,2,True) for y in range(12,113,20) for x in range(12,153,20)]
    values=[100+30*c+.08*x-.04*y+16*math.sin(x/24)*math.cos(y/27)*(1+c*.2)
            for c in range(3) for y in range(H) for x in range(W)]
    values += [1]*(W*H)
    image(root/"source.fits",W,H,values)
    pointfile(root/"samples.txt",points)
    return points

def run(root,name="out",smoothing=.001,reject=False,model="rbf"):
    options=["--rbf-smoothing",str(smoothing),"--rbf-reject","on" if reject else "off"] if model=="rbf" else []
    return subprocess.run([CLI,"background-review","--input",root/"source.fits","--output-dir",root/name,
                           "--samples",root/"samples.txt","--model",model,"--degree","2","--strength","1",
                           "--preserve-brightness","off",*options],capture_output=True)

class RBF(unittest.TestCase):
    def test_scipy_golden_pixels_and_explicit_leave_one_out(self):
        golden=json.loads((Path(__file__).parent/"background-rbf-golden.json").read_text())
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);fixture(root)
            for case in golden["cases"]:
                name="smooth-"+str(case["smoothing"])
                r=run(root,name,smoothing=case["smoothing"])
                self.assertEqual(r.returncode,0,r.stderr)
                out=read(root/name/"model.fits")
                report=json.loads((root/name/"report.json").read_text())
                self.assertEqual(report["model"],"rbf")
                for x,y,colors in case["pixels"]:
                    for c in range(3):self.assertAlmostEqual(out[c][y*W+x],colors[c],delta=.00005)
                for i,expected in case["leaveOneOut"]:
                    for c in range(3):
                        self.assertAlmostEqual(report["samples"][i]["validationResidual"][c],expected[c],delta=.000001)
                if case["smoothing"]==0:self.assertLess(max(report["residualRMS"]),1e-8)

    def test_contaminated_aperture_rejected_without_clipping_or_changing_alpha(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t)
            points=[(x,y,2,True) for y in range(12,113,20) for x in range(12,153,20)]
            values=[-10+40*c+.08*x-.04*y for c in range(3) for y in range(H) for x in range(W)]
            for c in range(3):
                for y in range(50,55):
                    for x in range(70,75):values[c*W*H+y*W+x]+=200
            values += [0 if x<4 else .5 if x==4 else 1 for y in range(H) for x in range(W)]
            image(root/"source.fits",W,H,values);pointfile(root/"samples.txt",points)
            r=run(root,reject=True);self.assertEqual(r.returncode,0,r.stderr)
            report=json.loads((root/"out/report.json").read_text())
            idx=points.index((72,52,2,True))
            self.assertEqual(report["samples"][idx]["used"],[False]*3)
            self.assertEqual(report["samples"][idx]["status"],"outlier")
            out=read(root/"out/corrected.fits");model=read(root/"out/model.fits")
            for c in range(3):
                self.assertAlmostEqual(out[c][52*W+72],200,delta=.0001)
                for x,y in [(4,2),(10,14),(159,127),(68,48)]:
                    self.assertAlmostEqual(out[c][y*W+x],0,delta=.0001)
                    self.assertAlmostEqual(model[c][y*W+x],-10+40*c+.08*x-.04*y,delta=.0001)
            self.assertEqual(out[3],tuple(values[3*W*H:]))

    def test_local_gradient_recovery_preserves_protected_signal(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);w,h=256,192
            def background(x,y):
                return 40+.06*x-.03*y+22*math.sin(x/37)*math.cos(y/49)+60*math.exp(-((x-65)**2+(y-140)**2)/(35**2))
            def target(x,y):return 80*max(0,1-((x-139)/20)**2-((y-93)/15)**2)**3
            values=[background(x,y)*(1+c*.2)+target(x,y) for c in range(3) for y in range(h) for x in range(w)]
            values += [1]*(w*h)
            image(root/"source.fits",w,h,values)
            points=[(x,y,1,True) for y in range(8,h-4,16) for x in range(8,w-4,16)
                    if ((x-139)/26)**2+((y-93)/21)**2>1]
            pointfile(root/"samples.txt",points)
            for model in ["rbf","polynomial"]:
                r=run(root,model,smoothing=.00001,model=model);self.assertEqual(r.returncode,0,r.stderr)
            a=read(root/"rbf/corrected.fits")[0];b=read(root/"polynomial/corrected.fits")[0]
            good=[(x,y) for y in range(8,h-8) for x in range(8,w-8)]
            rms=lambda v:math.sqrt(sum((v[y*w+x]-target(x,y))**2 for x,y in good)/len(good))
            self.assertLess(rms(a),.7)
            self.assertLess(rms(a),rms(b)*.15)
            self.assertAlmostEqual(a[93*w+139],80,delta=1)

    def test_invalid_smoothing_duplicates_and_degenerate_geometry(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);points=fixture(root)
            for smoothing in ["nan",-1,1.01]:
                self.assertNotEqual(run(root,smoothing=smoothing).returncode,0)
                self.assertFalse((root/"out").exists())
            for pts in (points+[points[0]],[(12+i*4,12+i*2,1,True) for i in range(20)],points[:11]):
                pointfile(root/"samples.txt",pts)
                self.assertNotEqual(run(root).returncode,0)
                self.assertFalse((root/"out").exists())

if __name__=="__main__":
    CLI=Path(sys.argv.pop(1)).resolve()
    unittest.main()
