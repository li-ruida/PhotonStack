"""Independent FITS fixtures and physical-sample oracle for CFA line extraction."""
import math
import random
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = Path(sys.argv.pop(1)).resolve()


def write_source(path, pattern, ox, oy, values, width=5, height=5):
    cards = {"SIMPLE": "T", "BITPIX": "-32", "NAXIS": "2", "NAXIS1": width,
             "NAXIS2": height, "BSCALE": "2", "BZERO": "-31",
             "BAYERPAT": repr(pattern), "XBAYROFF": ox, "YBAYROFF": oy}
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


if __name__ == "__main__":
    unittest.main()
