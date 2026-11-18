"""Independent TAN field bound and Gaia catalog-inspection command contracts."""
import hashlib
import math
from pathlib import Path
import tempfile
import unittest
from test_channels import run, read, write
from test_spectral_calibration import fixture


class GaiaInspection(unittest.TestCase):
    def test_solved_rgb_field_bound_is_read_only_and_covers_pixel_edges(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); fixture(root)
            source = root/'source.fits'; original = source.read_bytes()
            report = run('spcc-field', '--input', source)
            self.assertEqual(report['sha'], hashlib.sha256(original).hexdigest())
            self.assertEqual([report[k] for k in ['width','height','ra','dec']], [360,360,30,20])
            radius = math.degrees(math.atan(math.hypot(.1805,.1805)*math.pi/180))
            self.assertAlmostEqual(report['radius'], radius, delta=1e-13)
            for x in [-.5,0,180,359,359.5]:
                for y in [-.5,0,180,359,359.5]:
                    angular = math.degrees(math.atan(math.hypot((x+1-180)*.001,(y+1-180)*.001)*math.pi/180))
                    self.assertLessEqual(angular, report['radius']+1e-12)
            self.assertEqual(source.read_bytes(), original)
            run('spcc-field', '--input', source, '--expected-sha', '0'*64, ok=False)
            run('spcc-field', '--input', source, '--input', source, ok=False)
            # A display-domain image cannot initiate a spectral query workflow.
            display = original.replace(b"'LINEAR-SRGB' ", b"'SRGB-DISPLAY'")
            self.assertEqual(len(display), len(original))
            self.assertNotEqual(display, original)
            source.write_bytes(display)
            run('spcc-field', '--input', source, ok=False)

    def test_catalog_counts_epoch_and_digest(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); fixture(root)
            catalog = root/'catalog.txt'
            result = run('spcc-catalog-inspect', '--catalog', catalog)
            self.assertEqual(result['count'], 64); self.assertEqual(result['epoch'], 2026.5)
            self.assertEqual(result['sha'], hashlib.sha256(catalog.read_bytes()).hexdigest())
            run('spcc-catalog-inspect', '--catalog', catalog, '--expected-sha', '0'*64, ok=False)
            catalog.write_text(catalog.read_text()+'extra\n')
            run('spcc-catalog-inspect', '--catalog', catalog, ok=False)


if __name__ == '__main__': unittest.main(argv=[__file__])
