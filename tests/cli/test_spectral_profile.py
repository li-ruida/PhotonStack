"""Independent analytic passband moments and inspection-only file contract."""
import hashlib
import math
from pathlib import Path
import tempfile
import unittest
from test_channels import run


def profile(path, narrow=False):
    lines = ['PhotonStack-SpectralProfile-1', '"Response test" "broadband" "synthetic" "Analytic components" "CC0"']
    for _ in range(3):
        if narrow:
            lines += ['1', '5', '400 0', '500.123 0', '500.124 1', '500.125 0', '600 0']
        else:
            # Product t(1-t), with t=(lambda-400)/200. Beta(2,2) shape.
            lines += ['2', '2', '400 0', '600 1', '2', '400 1', '600 0']
    lines += ['"Flat white" "Constant F_lambda" "CC0"', '2', '350 2', '750 2']
    path.write_text('\n'.join(lines)+'\n')


class ProfileInspection(unittest.TestCase):
    def test_moments_product_samples_and_read_only(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'profile.txt'; profile(p)
            original = p.read_bytes(); sha = hashlib.sha256(original).hexdigest()
            r = run('spcc-profile-inspect', '--profile', p, '--expected-sha', sha)
            self.assertEqual(r['profile']['sha'], sha)
            self.assertEqual(r['whiteNormalization'], 2)
            self.assertEqual(r['whiteSamples'], [[350, 1], [750, 1]])
            for band in r['bands']:
                self.assertEqual(band['components'], 2)
                self.assertEqual(band['support'], [400, 600])
                self.assertAlmostEqual(band['meanWavelength'], 500, delta=1e-10)
                self.assertAlmostEqual(band['rmsBandwidth'], math.sqrt(2000), delta=1e-10)
                self.assertAlmostEqual(band['whiteFlux'], 2*200*500/6, delta=1e-9)
                self.assertLessEqual(len(band['samples']), 2048)
                for x, y in band['samples']:
                    t = (x-400)/200
                    self.assertAlmostEqual(y, t*(1-t), delta=1e-15)
            self.assertEqual(p.read_bytes(), original)
            self.assertEqual(list(Path(d).iterdir()), [p])
            run('spcc-profile-inspect', '--profile', p, '--expected-sha', '0'*64, ok=False)
            run('spcc-profile-inspect', '--profile', p, '--profile', p, ok=False)
            run('spcc-profile-inspect', '--profile', p, '--output', Path(d)/'wrong', ok=False)

    def test_narrow_component_peak_survives_display_reduction_and_negative_white(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'profile.txt'; profile(p, narrow=True)
            p.write_text(p.read_text().replace('350 2\n750 2', '350 -1\n750 5'))
            r = run('spcc-profile-inspect', '--profile', p)
            self.assertEqual(r['whiteSamples'], [[350, -.2], [750, 1]])
            for band in r['bands']:
                self.assertIn([500.124, 1], band['samples'])
                self.assertAlmostEqual(band['meanWavelength'], 500.124, delta=1e-9)
                self.assertTrue(all(band['samples'][i][0] < band['samples'][i+1][0] for i in range(len(band['samples'])-1)))

    def test_invalid_profile_is_rejected_before_producing_a_report(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'profile.txt'; profile(p); original = p.read_text()
            for bad in [original+'trailing\n', original.replace('400 0', '400 -1'),
                        original.replace('350 2', '450 2'), original.replace('600 1', '600 nan'),
                        original.replace('"broadband"', '"narrowband"'), original[:100]]:
                p.write_text(bad); run('spcc-profile-inspect', '--profile', p, ok=False)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
