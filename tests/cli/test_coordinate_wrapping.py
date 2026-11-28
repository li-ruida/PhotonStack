"""Catalog boundary regressions without requiring offline astronomical catalogs."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[2]/'tools/scripts'


class CoordinateWrapping(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec('numpy'), 'NumPy is required for proper-motion propagation')
    def test_tiny_negative_proper_motion_stays_in_catalog_ra_domain(self):
        spec = importlib.util.spec_from_file_location('spcc_import', SCRIPTS/'import-spcc-data.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for motion in [-1e-14, -1000, 0, 1000]:
            ra, dec = module.propagate(0, 0, motion, 0, 1)
            self.assertGreaterEqual(ra, 0)
            self.assertLess(ra, 360)
            self.assertEqual(dec, 0)

    def test_openngc_outline_wraps_rounded_full_circle_to_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root/'upstream'; output = root/'catalog'
            for name in ['database_files/NGC.csv', 'database_files/addendum.csv', 'LICENSES/CC-BY-SA-4.0.txt',
                         'README.md', 'AUTHORS', 'NGC_guide.txt', 'outlines/metodology.txt']:
                path = source/name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('fixture\n')
            outlines = source/'outlines/objects'; outlines.mkdir()
            (outlines/'test.txt').write_text('RAJ2000\tDEJ2000\tCont_Flag\n-1e-15\t30\t\n-1e-13\t30.5\t\n1\t31\t\n2\t32\t*\n')
            result = subprocess.run([sys.executable, SCRIPTS/'import-openngc.py', source, output, '0'*40], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            points = (output/'outlines.txt').read_text().splitlines()[2:]
            self.assertEqual(len(points), 4)
            for point in points:
                self.assertGreaterEqual(float(point.split()[0]), 0)
                self.assertLess(float(point.split()[0]), 360)


if __name__ == '__main__':
    unittest.main()
