"""Independent FITS fixtures and physical-sample oracle for CFA line extraction."""
import math
import json
import random
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = Path(sys.argv.pop(1)).resolve()


def write_source(path, pattern, ox, oy, values, width=5, height=5, metadata=None):
    cards = {"SIMPLE": "T", "BITPIX": "-32", "NAXIS": "2", "NAXIS1": width,
             "NAXIS2": height, "BSCALE": "2", "BZERO": "-31",
             "BAYERPAT": repr(pattern), "XBAYROFF": ox, "YBAYROFF": oy}
    cards.update(metadata or {})
    header = "".join(f"{k:<8}= {str(v):>20}".ljust(80) for k, v in cards.items())
    data = struct.pack(f">{len(values)}f", *values)
    path.write_bytes((header + "END").ljust(2880).encode("ascii") + data.ljust(2880, b"\0"))


def plane(path):
    data = path.read_bytes()
    header = {data[i:i+8].decode().strip(): data[i+10:i+80].decode().strip()
              for i in range(0, 2880, 80)}
    assert header["NAXIS"] == "2" and header["BITPIX"] == "-32"
    assert (header["NAXIS1"], header["NAXIS2"]) == ("2", "2")
    return struct.unpack_from(">4f", data, 2880)


class Extraction(unittest.TestCase):
    def test_patterns_offsets_scale_and_edges(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "input.fit"
            values = list(range(25))
            values[0] = math.nan
            for pattern in ("RGGB", "BGGR", "GRBG", "GBRG"):
                for ox, oy in ((0, 0), (-1, 0), (0, 3), (1, 1)):
                    with self.subTest(pattern=pattern, ox=ox, oy=oy):
                        dest = root / f"{pattern}-{ox}-{oy}"
                        write_source(source, pattern, ox, oy, values)
                        subprocess.run([CLI, "narrowband-extract", "--input", source, "--output-dir", dest],
                                       check=True, capture_output=True)
                        ha, oxygen = plane(dest / "Ha.fits"), plane(dest / "OIII.fits")
                        hv, ov = plane(dest / "Ha-valid.fits"), plane(dest / "OIII-valid.fits")
                        self.assertEqual(hv, tuple(float(math.isfinite(v)) for v in ha))
                        self.assertEqual(ov, tuple(float(math.isfinite(v)) for v in oxygen))
                        for cell in range(4):
                            samples = [(pattern[((y+oy)%2)*2 + (x+ox)%2], values[5*y+x]*2-31)
                                       for y in range(2*(cell//2), 2*(cell//2)+2)
                                       for x in range(2*(cell%2), 2*(cell%2)+2)]
                            red = next(v for c,v in samples if c == "R")
                            other = sum(v for c,v in samples if c != "R")/3
                            for got, expected in ((ha[cell], red), (oxygen[cell], other)):
                                if math.isnan(expected):
                                    self.assertTrue(math.isnan(got))
                                else:
                                    self.assertAlmostEqual(got, expected, places=5)

    def test_registered_line_masters_and_linear_remix(self):
        # A star field with a known two-pixel dither on the half-resolution grid.
        rng = random.Random(1034)
        width, height = 92, 68
        stars = [(rng.randrange(7,width-7), rng.randrange(7,height-7), rng.uniform(80,290)) for _ in range(45)]
        sky = [sum(a*math.exp(-((x-cx)**2+(y-cy)**2)/2) for cx,cy,a in stars)
               for y in range(height) for x in range(width)]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            frames = []
            for n, dx in enumerate((0, 2, -2)):
                samples = []
                for y in range(2*height):
                    for x in range(2*width):
                        sx = x//2 - dx
                        v = sky[(y//2)*width+sx] if 0 <= sx < width else 0
                        samples.append(10+v if x%2 == y%2 == 0 else 18+v*0.5)
                source = root / f"light{n}.fits"
                write_source(source, "RGGB", 0, 0, samples, 2*width, 2*height)
                frames.append(source)
            stack = root / "stack"
            subprocess.run([CLI, "narrowband-stack", "--output-dir", stack, *frames], check=True)
            def values(path, count):
                return struct.unpack_from(f">{count}f", path.read_bytes(), 2880)
            size = width*height
            hw = values(stack / "Ha-weight.fits", size)
            ow = values(stack / "OIII-weight.fits", size)
            coverage = values(stack / "geometric-coverage.fits", size)
            self.assertTrue(all(0 <= v <= 3 for v in hw + ow))
            self.assertTrue(all(0 <= v <= 1 for v in coverage))
            for y in range(5,height-5):
                for x in range(5,width-5):
                    k=y*width+x
                    self.assertAlmostEqual(hw[k],3,delta=1e-5)
                    self.assertAlmostEqual(ow[k],3,delta=1e-5)
                    self.assertAlmostEqual(coverage[k],1,delta=1e-5)
            repeated = root / "repeated-stack"
            subprocess.run([CLI,"narrowband-replay","--recipe",stack / "recipe.txt","--output-dir",repeated],check=True,capture_output=True)
            for name in ["Ha.fits","OIII.fits","HOO.fits","Ha-weight.fits","OIII-weight.fits","geometric-coverage.fits"]:
                self.assertEqual((stack/name).read_bytes(),(repeated/name).read_bytes())
            ha, oxygen = (values(stack / name, size) for name in ("Ha.fits", "OIII.fits"))
            # Check registered photometric values in the fully covered field.
            for y in range(5,height-5):
                for x in range(5,width-5):
                    k=y*width+x
                    self.assertAlmostEqual(ha[k], 2*(10+sky[k])-31, delta=0.15)
                    self.assertAlmostEqual(oxygen[k], 2*(18+sky[k]*0.5)-31, delta=0.15)
            mix = root / "mix"
            subprocess.run([CLI, "narrowband-compose", "--ha", stack / "Ha.fits", "--oiii",
                            stack / "OIII.fits", "--green-ha", "0.25", "--output-dir", mix], check=True)
            rgb = values(mix / "HOO.fits", 3*size)
            for k in range(size):
                if math.isfinite(ha[k]) and math.isfinite(oxygen[k]):
                    self.assertEqual(rgb[k], ha[k])
                    self.assertAlmostEqual(rgb[size+k], 0.25*ha[k]+0.75*oxygen[k], delta=0.00005)
                    self.assertEqual(rgb[2*size+k], oxygen[k])

    def test_recipe_source_and_result_protection_and_invalid_hoo_mask(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source with spaces.fit';write_source(source,"RGGB",0,0,list(range(25)))
            dest=root/'lines'
            subprocess.run([CLI,"narrowband-extract","--input",source,"--output-dir",dest],check=True,capture_output=True)
            inspected=subprocess.run([CLI,"narrowband-inspect","--recipe",dest/'recipe.txt'],check=True,capture_output=True,text=True)
            report=json.loads(inspected.stdout);self.assertEqual(report['operation'],'extract')
            duplicate=root/'repeated';subprocess.run([CLI,"narrowband-replay","--recipe",dest/'recipe.txt',"--output-dir",duplicate],check=True,capture_output=True)
            for name in report['outputs']:self.assertEqual((dest/name).read_bytes(),(duplicate/name).read_bytes())
            self.assertNotEqual(subprocess.run([CLI,"narrowband-replay","--recipe",dest/'recipe.txt',"--output-dir",duplicate],capture_output=True).returncode,0)
            original=source.read_bytes();source.write_bytes(original[:-1]+b'x')
            self.assertNotEqual(subprocess.run([CLI,"narrowband-replay","--recipe",dest/'recipe.txt',"--output-dir",root/'changed'],capture_output=True).returncode,0)
            self.assertFalse((root/'changed').exists());source.write_bytes(original)
            ha=dest/'Ha.fits';data=ha.read_bytes();ha.write_bytes(data[:-1]+b'x')
            self.assertNotEqual(subprocess.run([CLI,"narrowband-inspect","--recipe",dest/'recipe.txt'],capture_output=True).returncode,0)
            ha.write_bytes(data)
            # Independent invalid masks: only one emission line is invalid at each of two pixels.
            raw=bytearray(ha.read_bytes());struct.pack_into('>f',raw,2880,float('nan'));ha.write_bytes(raw)
            oxygen=dest/'OIII.fits';raw=bytearray(oxygen.read_bytes());struct.pack_into('>f',raw,2884,float('nan'));oxygen.write_bytes(raw)
            mixed=root/'mixed';subprocess.run([CLI,"narrowband-compose","--ha",ha,"--oiii",oxygen,"--green-ha","0.25","--output-dir",mixed],check=True,capture_output=True)
            self.assertEqual(plane(mixed/'validity.fits'),(0,0,1,1))
            rgba=struct.unpack_from('>16f',(mixed/'HOO.fits').read_bytes(),2880)
            self.assertTrue(all(math.isfinite(v) for v in rgba));self.assertEqual(rgba[12:],(0,0,1,1))
            for c in range(3):self.assertEqual(rgba[c*4:c*4+2],(0,0))
            again=root/'mixed-again';subprocess.run([CLI,"narrowband-replay","--recipe",mixed/'recipe.txt',"--output-dir",again],check=True,capture_output=True)
            self.assertEqual((mixed/'HOO.fits').read_bytes(),(again/'HOO.fits').read_bytes())
            changedmix=root/'green';subprocess.run([CLI,"narrowband-replay","--recipe",mixed/'recipe.txt',"--green-ha","0.75","--output-dir",changedmix],check=True,capture_output=True)
            actual=struct.unpack_from('>16f',(changedmix/'HOO.fits').read_bytes(),2880)
            self.assertAlmostEqual(actual[6],.75*actual[2]+.25*actual[10],places=5)

    def test_invalid_input_and_existing_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, dest = root / "input.fit", root / "lines"
            command = [CLI, "narrowband-extract", "--input", source, "--output-dir", dest]
            write_source(source, "RRRR", 0, 0, list(range(25)))
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertFalse(dest.exists())
            write_source(source, "RGGB", 0, 0, list(range(25)))
            dest.mkdir()
            marker = dest / "Ha.fits"
            marker.write_bytes(b"keep me")
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(marker.read_bytes(), b"keep me")


class SensorCalibration(unittest.TestCase):
    def run_cli(self, *args, ok=True):
        result = subprocess.run([CLI, *map(str, args)], capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0)
        return result.stderr

    def fixture(self, root, name, values, pattern="RGGB", offset=(0,0), metadata=None):
        path = root / (name + ".fits")
        meta = {"INSTRUME": "'camera-A'", "GAIN": 100, "EXPTIME": 60, "XBINNING": 1, "YBINNING": 1}
        meta.update(metadata or {})
        write_source(path, pattern, *offset, [(v+31)/2 for v in values], 4, 4, meta)
        return path

    def test_sensor_calibration_recovers_signed_signals_all_phases(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for pattern in ("RGGB", "GRBG", "GBRG", "BGGR"):
                for offset in ((0,0), (1,0), (0,-1), (1,1)):
                    for dark_state, flat_state in (("included","included"), ("removed","removed"), ("included","removed"), ("removed","included")):
                        truth=[]; bias=[]; dark=[]; flat=[]; light=[]
                        for y in range(4):
                            for x in range(4):
                                phase=(y%2)*2+x%2; cell=(y//2)*2+x//2
                                response=(.5,1,1.5,1)[cell]
                                signal=-20+5*cell if pattern[((y+offset[1])%2)*2+(x+offset[0])%2]=='R' else 40+phase*5+cell*5
                                b=10+phase; d=3+cell
                                truth.append(signal); bias.append(b)
                                light.append(signal*response+b+d)
                                dark.append(d+(b if dark_state=='included' else 0))
                                flat.append(100*(phase+1)*response+(b if flat_state=='included' else 0))
                        src=self.fixture(root,'light',light,pattern,offset)
                        b=self.fixture(root,'bias',bias,pattern,offset,{'EXPTIME':0})
                        d=self.fixture(root,'dark',dark,pattern,offset)
                        f=self.fixture(root,'flat',flat,pattern,offset,{'EXPTIME':2})
                        out=root/f'{pattern}-{offset}-{dark_state}-{flat_state}'
                        report=self.run_cli('narrowband-extract','--input',src,'--output-dir',out,'--bias',b,
                                            '--dark',d,'--dark-bias',dark_state,'--flat',f,'--flat-bias',flat_state)
                        self.assertEqual(report['calibration']['darkBiasState'],dark_state)
                        h,o=plane(out/'Ha.fits'),plane(out/'OIII.fits')
                        for cy in range(2):
                            for cx in range(2):
                                values=[(pattern[((y+offset[1])%2)*2+(x+offset[0])%2],truth[y*4+x])
                                    for y in range(cy*2,cy*2+2) for x in range(cx*2,cx*2+2)]
                                i=cy*2+cx
                                self.assertAlmostEqual(h[i],next(v for c,v in values if c=='R'),places=5)
                                self.assertAlmostEqual(o[i],sum(v for c,v in values if c!='R')/3,places=5)
                        again=out.with_name(out.name+'-again')
                        self.run_cli('narrowband-replay','--recipe',out/'recipe.txt','--output-dir',again)
                        for name in ('Ha.fits','OIII.fits','Ha-valid.fits','OIII-valid.fits'):
                            self.assertEqual((out/name).read_bytes(),(again/name).read_bytes())

    def test_missing_master_values_preserve_independent_line_validity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            source=self.fixture(root,'light',[20]*16)
            bias=[10]*16; bias[0]=math.nan
            b=self.fixture(root,'bias',bias)
            flat=[100]*16; flat[3]=0
            f=self.fixture(root,'flat',flat)
            out=root/'out'
            self.run_cli('narrowband-extract','--input',source,'--output-dir',out,'--bias',b,'--flat',f,'--flat-bias','removed')
            self.assertEqual(plane(out/'Ha-valid.fits'),(0,1,1,1))
            self.assertEqual(plane(out/'OIII-valid.fits'),(1,0,1,1))
            self.assertTrue(math.isnan(plane(out/'Ha.fits')[0]))
            self.assertTrue(math.isnan(plane(out/'OIII.fits')[1]))
            self.assertAlmostEqual(plane(out/'Ha.fits')[3],10)
            original=b.read_bytes(); corrupt=bytearray(original);corrupt[-1]^=1;b.write_bytes(corrupt)
            self.run_cli('narrowband-inspect','--recipe',out/'recipe.txt',ok=False)
            self.run_cli('narrowband-replay','--recipe',out/'recipe.txt','--output-dir',root/'replay',ok=False)
            self.assertFalse((root/'replay').exists())

    def test_rejects_ambiguous_or_incompatible_calibration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            src=self.fixture(root,'light',[20]*16); d=self.fixture(root,'dark',[10]*16)
            base=['narrowband-extract','--input',src,'--output-dir',root/'out']
            for args in (['--dark',d],['--dark',d,'--dark-bias','removed'],['--flat',d,'--flat-bias','included'],
                         ['--dark-bias','included'],['--bias',src],['--bias',d,'--dark',d,'--dark-bias','included']):
                self.run_cli(*base,*args,ok=False); self.assertFalse((root/'out').exists())
            for metadata in ({'GAIN':200},{'EXPTIME':120},{'INSTRUME':"'camera-B'"},{'XBINNING':2},{'GAIN':"'oops'"},{'EXPTIME':'NaN'}):
                self.fixture(root,'dark',[10]*16,metadata=metadata)
                self.run_cli(*base,'--dark',d,'--dark-bias','included',ok=False)
                self.assertFalse((root/'out').exists())
            self.fixture(root,'dark',[10]*16,pattern='GRBG')
            self.run_cli(*base,'--dark',d,'--dark-bias','included',ok=False)
            self.fixture(root,'dark',[0]*16)
            self.run_cli(*base,'--flat',d,'--flat-bias','removed',ok=False)
            self.assertFalse((root/'out').exists())
            self.fixture(root,'dark',[10]*16)
            damaged=src.read_bytes().replace(b'NAXIS1  =                    4',b'NAXIS1  =           4294967295')
            self.assertNotEqual(damaged,src.read_bytes())
            src.write_bytes(damaged)
            error=self.run_cli(*base,'--dark',d,'--dark-bias','included',ok=False)
            self.assertIn('truncated or too large',error)
            self.assertFalse((root/'out').exists())

    def test_calibrated_dither_stack_matches_undistorted_truth(self):
        rng=random.Random(147)
        w,h=92,68
        stars=[(rng.randrange(8,w-8),rng.randrange(8,h-8),rng.randrange(100,400)) for _ in range(45)]
        sky=[round(sum(a*math.exp(-((x-sx)**2+(y-sy)**2)/3) for sx,sy,a in stars)) for y in range(h) for x in range(w)]
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); clean=[]; damaged=[]
            for n,dx in enumerate((0,2,-2)):
                original=[]; sensor=[]; flat=[]
                for y in range(2*h):
                    for x in range(2*w):
                        sx=x//2-dx; phase=y%2*2+x%2
                        value=50+(sky[y//2*w+sx] if 0<=sx<w else 0)*(2 if phase==0 else 1)
                        response=(.5,1,1.5,1)[x//2%4]
                        original.append(value); sensor.append(value*response+32)
                        flat.append(response*100*(phase+1))
                a=root/f'clean{n}.fits';b=root/f'sensor{n}.fits'
                write_source(a,'RGGB',0,0,[(v+31)/2 for v in original],2*w,2*h)
                write_source(b,'RGGB',0,0,[(v+31)/2 for v in sensor],2*w,2*h)
                clean.append(a);damaged.append(b)
            dark=root/'dark.fits';flatfile=root/'flat.fits'
            write_source(dark,'RGGB',0,0,[(32+31)/2]*(4*w*h),2*w,2*h)
            write_source(flatfile,'RGGB',0,0,[(v+31)/2 for v in flat],2*w,2*h)
            self.run_cli('narrowband-stack','--output-dir',root/'truth',*clean)
            self.run_cli('narrowband-stack','--output-dir',root/'calibrated','--dark',dark,'--dark-bias','included',
                         '--flat',flatfile,'--flat-bias','removed',*damaged)
            for name in ('Ha.fits','OIII.fits','HOO.fits','Ha-weight.fits','OIII-weight.fits','geometric-coverage.fits'):
                self.assertEqual((root/'truth'/name).read_bytes(),(root/'calibrated'/name).read_bytes(),name)
            self.run_cli('narrowband-replay','--recipe',root/'calibrated/recipe.txt','--output-dir',root/'again')
            self.assertEqual((root/'calibrated/HOO.fits').read_bytes(),(root/'again/HOO.fits').read_bytes())

    def test_version_one_recipe_still_replays_without_calibration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); src=self.fixture(root,'light',[20]*16); out=root/'out'
            self.run_cli('narrowband-extract','--input',src,'--output-dir',out)
            recipe=out/'recipe.txt'
            lines=recipe.read_text().splitlines(); lines[0]='PhotonStack-Narrowband-1'
            recipe.write_text('\n'.join(line for line in lines if not line.startswith(('dark-bias','flat-bias')))+'\n')
            result=self.run_cli('narrowband-inspect','--recipe',recipe)
            self.assertIsNone(result['calibration'])
            self.run_cli('narrowband-replay','--recipe',recipe,'--output-dir',root/'again')
            self.assertEqual((out/'Ha.fits').read_bytes(),(root/'again/Ha.fits').read_bytes())


if __name__ == "__main__":
    unittest.main()
