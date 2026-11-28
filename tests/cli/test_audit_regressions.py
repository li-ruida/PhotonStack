"""Public CLI contracts regressed by path spelling, errors and malformed arguments."""
import json
import struct
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_channels import CLI, write
from test_sensor_master import frame, manifest as sensor_manifest
from test_calibration_job import manifest as calibration_manifest
from test_plate_solve import setup, command


def invoke(*args):
    return subprocess.run([str(CLI), *map(str, args)], capture_output=True, text=True)


class AuditRegressions(unittest.TestCase):
    def test_gray_alpha_tiff_has_inline_bits_per_sample(self):
        with tempfile.TemporaryDirectory() as directory:
            for order, marker in [('<', b'II'), ('>', b'MM')]:
                pack = lambda fmt, *args: struct.pack(order + fmt, *args)
                entries = [(256, 4, 1, pack('I', 3)), (257, 4, 1, pack('I', 2)),
                           (258, 3, 2, pack('HH', 16, 16)), (277, 3, 1, pack('HH', 2, 0))]
                path = Path(directory) / 'gray-alpha.tiff'
                path.write_bytes(marker + pack('HIH', 42, 8, len(entries)) +
                                 b''.join(pack('HHI', tag, typ, count) + value for tag, typ, count, value in entries) +
                                 pack('I', 0))
                result = invoke('inspect', path)
                self.assertEqual(result.returncode, 0, result.stderr)
                metadata = json.loads(result.stdout)
                self.assertEqual((metadata['channels'], metadata['bitsPerChannel']), (2, 16))

    def test_default_recipe_publishes_once(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'recipe.txt'
            def export():
                return invoke('deep-sky', '--write-default-recipe', path)
            self.assertEqual(export().returncode, 0)
            self.assertIn('PHOTONSTACK_DEEP_SKY_RECIPE', path.read_text())
            path.write_text('original')
            self.assertNotEqual(export().returncode, 0)
            self.assertEqual(path.read_text(), 'original')

    def test_expensive_commands_check_output_parent_before_decode(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            invalid = root / 'unreadable.fits'
            invalid.touch()
            output = root / 'absent' / 'result'
            for args in [('star-layers', 'prepare', '--input', invalid),
                         ('background-review', '--input', invalid),
                         ('ghs-stretch', '--input', invalid),
                         ('psf-restore', '--measurement', invalid),
                         ('channel-compose', '--aligned', '--input', 'A=' + str(invalid)),
                         ('channel-register', '--input', 'A=' + str(invalid)),
                         ('channel-match', '--aligned', '--input', 'A=' + str(invalid)),
                         ('narrowband-extract', '--input', invalid)]:
                result = invoke(*args, '--output-dir', output)
                self.assertNotEqual(result.returncode, 0, args)
                self.assertIn('existing parent', result.stderr, args)
            self.assertFalse(output.parent.exists())

    def test_output_directories_accept_trailing_slash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            darks = [frame(root/f'd{i}.fits', [100+i]*120) for i in range(3)]
            recipe = sensor_manifest(root, darks)
            result = invoke('sensor-master-build', '--manifest', recipe, '--output-dir', str(root/'master')+'/')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root/'master/master.fits').is_file())
            light = frame(root/'light.fits', [100]*120, 'Light')
            bias = frame(root/'bias.fits', [10]*120, 'Master Bias', PSBIAS="'included'", EXPTIME=0)
            recipe = calibration_manifest(root, [(light, 'b', '-', '-')], [('b', 'bias', bias, 'included')])
            result = invoke('calibration-run', '--manifest', recipe, '--output-dir', str(root/'job')+'/')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root/'job/frame-000001/calibrated.fits').is_file())
            inspected = invoke('calibration-job-inspect', '--job', str(root/'job')+'/')
            self.assertEqual(inspected.returncode, 0, inspected.stderr)

    def test_empty_solver_log_still_reports_json_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            setup(root)
            result = subprocess.run(command(root, solver='unsolved'), capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stderr)['code'], 'PlateSolveFailed')

    def test_control_characters_in_paths_produce_valid_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/'source.fits'
            write(source, [1]*48)
            target = root/'output\x01\b\f.fits'
            result = invoke('crop', '--input', source, '--output', target)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['output'], str(target))

    def test_workflow_rejects_unclosed_quote_before_running_any_step(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root/'source.fits', root/'output.fits'
            write(source, [1]*48)
            workflow = root/'workflow.txt'
            workflow.write_text(f'crop --input "{source}" --output "{output}"\ninspect --input "unterminated # comment\n')
            result = invoke('run', workflow)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stderr)['code'], 'ArgumentInvalid')
            self.assertFalse(output.exists())

    def test_mosaic_zero_parameters_are_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/'source.fits'
            write(source, list(range(48)))
            for option in ['--columns', '--alignment-width']:
                result = invoke('mosaic', '--output', root/'out.fits', option, '0', source)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(result.stderr)['code'], 'ArgumentInvalid')
            result = invoke('mosaic', '--output', root/'out.fits', '--preview-width', '0', source)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreater(json.loads(result.stdout)['width'], 1)

    def test_deep_sky_error_has_common_code(self):
        result = invoke('deep-sky', '--unknown', 'value')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stderr)['code'], 'RecipeInvalid')


    def test_directory_aliases_never_reach_output_creation_or_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root/'kept'; target.mkdir(); marker = target/'original'; marker.write_text('keep')
            missing = root/'missing'
            for name, destination in [('link', target), ('dangling', missing)]:
                alias = root/name; alias.symlink_to(destination, target_is_directory=True)
                commands = ['channel-compose', 'channel-register', 'channel-match', 'ghs-stretch',
                            'psf-measure', 'psf-restore', 'plate-solve', 'sky-annotate', 'spcc',
                            'narrowband-extract', 'spcc-narrowband-profile', 'star-layers', 'deep-sky', 'sensor-master-build']
                for command in commands:
                    for suffix in ['', '/', '/.', '///']:
                        result = invoke(command, '--output-dir', str(alias)+suffix)
                        self.assertNotEqual(result.returncode, 0, command)
                        self.assertEqual(json.loads(result.stderr)['code'], 'PathInvalid', result.stderr)
                        self.assertEqual(marker.read_text(), 'keep')
                        self.assertFalse(missing.exists())
                        self.assertTrue(alias.is_symlink())
                for command in ['calibration-resume', 'calibration-job-inspect']:
                    result = invoke(command, '--job', str(alias)+'/')
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(json.loads(result.stderr)['code'], 'PathInvalid')

    def test_channel_workers_reject_wrapping_negative_integer(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root/'mono.fits'; write(source, [1]*48)
            for workers in ['-18446744073709551615', '-1', '1junk']:
                result = invoke('channel-compose', '--aligned', '--input', 'A='+str(source),
                                '--red', 'A', '--green', 'A', '--blue', 'A', '--workers', workers,
                                '--output-dir', root/'output')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Workers must be 1..64', result.stderr)
                self.assertFalse((root/'output').exists())
            result = invoke('channel-compose', '--aligned', '--input', 'A='+str(source),
                            '--red', 'A', '--green', 'A', '--blue', 'A', '--workers', '1',
                            '--output-dir', str(root/'output')+'/')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root/'output/RGB.fits').exists())

    def test_detection_rejects_unwritten_image_output(self):
        for mode in ['detect', 'temporal-detect']:
            result = invoke('clouds', mode, '--input', 'missing.fits', '--output', 'unused.fits')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('does not write an image', json.loads(result.stderr)['message'])


if __name__ == '__main__':
    unittest.main()
