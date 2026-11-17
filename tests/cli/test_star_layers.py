"""Reversible star-layer CLI contracts, using independent TIFF/FITS byte readers."""
from pathlib import Path
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = Path(sys.argv.pop(1)).resolve()

def tiff(path, width, height, pixels):
    # Minimal uncompressed, chunky RGB, 16 bit TIFF. Values are sRGB code values.
    entries = [(256,4,1,width),(257,4,1,height),(258,3,3,0),(259,3,1,1),(262,3,1,2),
               (273,4,1,0),(277,3,1,3),(278,4,1,height),(279,4,1,width*height*6),(284,3,1,1)]
    end = 8+2+len(entries)*12+4
    entries[2]=(258,3,3,end)
    entries[5]=(273,4,1,end+6)
    data = b"II"+struct.pack("<HIH",42,8,len(entries))
    for tag,kind,count,value in entries:
        data+=struct.pack("<HHII",tag,kind,count,value)
    data+=struct.pack("<IHHH",0,16,16,16)
    data+=struct.pack("<"+"H"*len(pixels),*pixels)
    path.write_bytes(data)

def fits(path):
    raw=path.read_bytes();header={}
    for i in range(0,len(raw),80):
        key=raw[i:i+8].decode().strip()
        if key=="END":break
        header[key]=raw[i+10:i+80].decode().strip()
    count=int(header["NAXIS1"])*int(header["NAXIS2"])
    channels=int(header.get("NAXIS3","1"))
    vals=struct.unpack_from(f">{count*channels}f",raw,(i//2880+1)*2880)
    return [vals[i*count:(i+1)*count] for i in range(channels)]

def call(*args):
    return subprocess.run([CLI,*map(str,args)],capture_output=True)

class Layers(unittest.TestCase):
    def test_split_linear_roundtrip_and_recipe(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            values=[1000+(i*5317)%62000 for i in range(12*9*3)]
            other=[min(65535,v+1300) if i%11==0 else v//2 for i,v in enumerate(values)]
            tiff(p/"original.tiff",12,9,values);tiff(p/"removed.tiff",12,9,other)
            original_bytes=(p/"original.tiff").read_bytes()
            self.assertEqual(call("convert","--input",p/"original.tiff","--output",p/"original.fits").returncode,0)
            r=call("star-layers","split","--input",p/"original.tiff","--starless",p/"removed.tiff","--output-dir",p/"layers")
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertGreater(json.loads(r.stdout)["negativeSamples"],0)
            s,n=fits(p/"layers/stars.fits"),fits(p/"layers/starless.fits")
            original=fits(p/"original.fits")
            for c in range(3):
                for a,b,expected in zip(s[c],n[c],original[c]):
                    self.assertAlmostEqual(a+b,expected,delta=8e-8)
            for ev in (0,-1):
                dest=p/f"render{ev}"
                r=call("star-layers","render","--layers",p/"layers","--output-dir",dest,"--stars-ev",ev)
                self.assertEqual(r.returncode,0,r.stderr)
                out=fits(dest/"combined.fits")
                for c in range(3):
                    for a,b,value in zip(s[c],n[c],out[c]):
                        self.assertAlmostEqual(a*2**ev+b,value,delta=8e-8)
                recipe=json.loads((dest/"recipe.json").read_text())
                self.assertEqual(recipe["starsEV"],ev)
                self.assertTrue((dest/"combined.tiff").is_file())
            self.assertEqual((p/"original.tiff").read_bytes(),original_bytes)
            # Never replace a successful edit or silently accept changed bases.
            self.assertNotEqual(call("star-layers","render","--layers",p/"layers","--output-dir",p/"render0").returncode,0)
            with (p/"layers/stars.fits").open("ab") as f:f.write(b"x")
            self.assertNotEqual(call("star-layers","render","--layers",p/"layers","--output-dir",p/"bad").returncode,0)
            self.assertFalse((p/"bad").exists())

    def test_dimensions_arguments_and_linear_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            tiff(p/"a.tiff",2,2,[3000]*12);tiff(p/"b.tiff",3,2,[2000]*18)
            r=call("star-layers","split","--input",p/"a.tiff","--starless",p/"b.tiff","--output-dir",p/"layers")
            self.assertNotEqual(r.returncode,0);self.assertFalse((p/"layers").exists())
            r=call("star-layers","render","--layers",p,"--output-dir",p/"bad","--stars-ev","nan")
            self.assertNotEqual(r.returncode,0);self.assertFalse((p/"bad").exists())

if __name__=="__main__":unittest.main()
