import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('seestar', Path(__file__).resolve().parents[1] / 'tools/scripts/seestar-sequential.py')
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)
GROUP_SPEC = importlib.util.spec_from_file_location('seestar_groups', Path(__file__).resolve().parents[1] / 'tools/scripts/seestar-local-groups.py')
GROUP = importlib.util.module_from_spec(GROUP_SPEC)
GROUP_SPEC.loader.exec_module(GROUP)
GROUP.RUN = RUN


class SequentialIngestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='photonstack-ingest-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        RUN.SOURCE = self.base/'source'
        RUN.ROOT = self.base/'desktop-run'
        RUN.SOURCE.mkdir()
        for name in ('M 2', 'M 2_sub', 'M 10', 'StarNet_去星插件'):
            (RUN.SOURCE/name).mkdir()
        self.source = RUN.SOURCE/'M 2_sub'/'Light_a.fit'
        self.source.write_bytes(b'original photographic bytes')
        (RUN.SOURCE/'M 2'/'.DS_Store').write_bytes(b'not a photograph')
        (RUN.SOURCE/'M 10'/'next.fit').write_bytes(b'next target')

    def test_one_target_interlock_and_hash(self):
        original = RUN.digest(self.source)
        RUN.inventory()
        RUN.copy_next()
        queue = json.loads((RUN.ROOT/'queue.json').read_text())
        self.assertEqual([t['name'] for t in queue['targets']], ['M 2', 'M 10'])
        self.assertEqual(queue['targets'][0]['status'], 'copied')
        self.assertFalse((RUN.ROOT/'工作区'/'M 10').exists())
        self.assertEqual(RUN.digest(self.source), original)
        copied = RUN.ROOT/'工作区'/'M 2'/'inputs'/'M 2_sub'/'Light_a.fit'
        self.assertEqual(RUN.digest(copied), original)
        with self.assertRaisesRegex(RuntimeError, 'Finish current target first'):
            RUN.copy_next()
        self.assertFalse((RUN.ROOT/'工作区'/'M 10').exists())

    def test_retry_tag_cannot_escape_or_overwrite_existing_result(self):
        RUN.inventory()
        RUN.copy_next()
        work = RUN.ROOT/'工作区'/'M 2'
        group = {'id':'g01', 'lights':['a','b','c'], 'masters':[]}
        for invalid in ('', '../escape', 'v2/result', '标签'):
            with self.assertRaisesRegex(ValueError, 'Result tag'):
                GROUP.run_groups(work, [group], ['g01'], invalid)
        (work/'g01-v2').mkdir()
        sentinel = work/'g01-v2'/'sentinel'
        sentinel.write_text('retain')
        with self.assertRaisesRegex(RuntimeError, 'Existing group output'):
            GROUP.run_groups(work, [group], ['g01'], 'v2')
        self.assertEqual(sentinel.read_text(), 'retain')
        self.assertFalse((work/'g01-v2.recipe.txt').exists())

    def test_copy_mismatch_stops_without_touching_source(self):
        RUN.inventory()
        with patch.object(RUN, 'digest', return_value='bad-local-checksum'):
            with self.assertRaisesRegex(RuntimeError, 'Copy verification failed'):
                RUN.copy_next()
        self.assertEqual(self.source.read_bytes(), b'original photographic bytes')
        queue = json.loads((RUN.ROOT/'queue.json').read_text())
        self.assertEqual(queue['targets'][0]['status'], 'copying')
        self.assertEqual(queue['targets'][1]['status'], 'pending')

    def test_symlinks_are_not_followed_and_existing_run_is_not_replaced(self):
        (RUN.SOURCE/'M 2_sub'/'external.fit').symlink_to(RUN.SOURCE/'M 10'/'next.fit')
        RUN.inventory()
        RUN.copy_next()
        manifest = json.loads((RUN.ROOT/'工作区'/'M 2'/'copy-manifest.json').read_text())
        self.assertEqual(len(manifest['files']), 1)
        with self.assertRaisesRegex(RuntimeError, 'already exists'):
            RUN.inventory()

    def test_incomplete_group_set_cannot_publish_or_cleanup(self):
        RUN.inventory()
        RUN.copy_next()
        work = RUN.ROOT/'工作区'/'M 2'
        (work/'group-plan.json').write_text(json.dumps([{'id':'g01'}, {'id':'g02'}]))
        (work/'g01').mkdir()
        (work/'g01'/'report.json').write_text(json.dumps({'success':True}))
        with self.assertRaisesRegex(RuntimeError, 'Every planned'):
            RUN.finalize('M 2', ['g01'], 'test visual review')
        self.assertTrue((work/'inputs'/'M 2_sub'/'Light_a.fit').exists())
        self.assertFalse((RUN.ROOT/'最终成片'/'M 2').exists())

    def test_all_groups_publish_before_owned_work_cleanup(self):
        RUN.inventory()
        RUN.copy_next()
        work = RUN.ROOT/'工作区'/'M 2'
        (work/'group-plan.json').write_text(json.dumps([{'id':'g01'}, {'id':'g02'}]))
        for directory in ('g01', 'g02-finished'):
            result = work/directory
            result.mkdir()
            (result/'developed.tiff').write_bytes(b'processed display')
            (result/'master.fits').write_bytes(b'linear scientific master')
            (result/'recipe.txt').write_text('test recipe')
            (result/'report.json').write_text(json.dumps({
                'success': True, 'masterReused': False, 'selectedCount': 3,
                'developed': str(result/'developed.tiff'), 'master': str(result/'master.fits')}))
        def codec(command, **kwargs):
            if command[1] == 'convert':
                Path(command[command.index('--output')+1]).write_bytes(b'viewing JPEG')
        with patch.object(RUN.subprocess, 'run', side_effect=codec):
            RUN.finalize('M 2', ['g01', 'g02=g02-finished'], 'synthetic test acceptance')
        self.assertFalse(work.exists())
        self.assertEqual(self.source.read_bytes(), b'original photographic bytes')
        output = RUN.ROOT/'最终成片'/'M 2'
        for group in ('g01','g02'):
            self.assertTrue((output/group/'成片.jpg').is_file())
            self.assertTrue((output/group/'线性母版.fits').is_file())
        queue = json.loads((RUN.ROOT/'queue.json').read_text())
        self.assertEqual(queue['targets'][0]['status'], 'complete')
        self.assertEqual(queue['targets'][1]['status'], 'pending')

    def test_grouping_uses_fits_header_not_filename_exposure(self):
        RUN.inventory()
        RUN.copy_next()
        work = RUN.ROOT/'工作区'/'M 2'
        records = []
        for index, exposure in enumerate((30,60)):
            relative = f'M 2_sub/Light_wrong_10.0s_IRCUT_{index}.fit'
            path = work/'inputs'/relative
            values = {'SIMPLE':'T', 'NAXIS':2, 'NAXIS1':2160, 'NAXIS2':3840,
                      'EXPTIME':exposure, 'FILTER':"'IRCUT'", 'TELESCOP':"'test-camera'",
                      'FOCALLEN':160, 'WIDECAM':0, 'BAYERPAT':"'GRBG'", 'RA':100, 'DEC':20}
            cards = ''.join(f'{k:<8}= {str(v):>20}'.ljust(80) for k,v in values.items())+'END'.ljust(80)
            path.write_bytes(cards.ljust(((len(cards)+2879)//2880)*2880).encode('ascii'))
            records.append({'relative':relative, 'size':path.stat().st_size, 'sha256':RUN.digest(path)})
        (work/'copy-manifest.json').write_text(json.dumps({'files':records}))
        _, groups = GROUP.plan('M 2')
        self.assertEqual([g['exposure'] for g in groups], [30,60])
        self.assertEqual([len(g['lights']) for g in groups], [1,1])

    def test_truncated_header_is_rejected(self):
        path = self.base/'bad.fit'
        path.write_bytes(b'SIMPLE  = T')
        with self.assertRaisesRegex(ValueError, 'Incomplete FITS header'):
            GROUP.header(path)

    def prepare_unreadable_case(self):
        RUN.inventory()
        RUN.copy_next()
        work = RUN.ROOT/'工作区'/'M 2'
        valid = work/'inputs'/'M 2_sub'/'Light_a.fit'
        values = {'SIMPLE':'T', 'NAXIS':2, 'NAXIS1':2160, 'NAXIS2':3840,
                  'EXPTIME':30, 'FILTER':"'LP'", 'TELESCOP':"'test-camera'",
                  'FOCALLEN':160, 'WIDECAM':0, 'BAYERPAT':"'GRBG'"}
        cards = ''.join(f'{k:<8}= {str(v):>20}'.ljust(80) for k,v in values.items())+'END'.ljust(80)
        valid.write_bytes(cards.ljust(2880).encode('ascii'))
        bad = valid.with_name('Light_bad.fit')
        bad.write_bytes(bytes(2880))
        records = [{'relative':str(p.relative_to(work/'inputs')), 'size':p.stat().st_size,
                    'sha256':RUN.digest(p)} for p in (valid,bad)]
        (work/'copy-manifest.json').write_text(json.dumps({'files':records}))
        return work, valid, bad, records

    def test_zero_header_requires_explicit_audited_exclusion(self):
        work, valid, bad, records = self.prepare_unreadable_case()
        with self.assertRaisesRegex(ValueError, 'missing SIMPLE'):
            GROUP.plan('M 2')
        _, groups = GROUP.plan('M 2', [records[1]['relative']])
        self.assertEqual(groups[0]['lights'], [str(valid)])
        audit = groups[0]['target_unreadable_inputs'][0]
        self.assertEqual(audit['sha256'], records[1]['sha256'])
        self.assertEqual(audit['status'], 'unreadable-not-stacked')
        self.assertTrue(bad.exists())
        self.assertFalse((work/'group-plan.json').exists())  # planning remains read-only

    def test_readable_or_unknown_exclusions_cannot_hide_frames(self):
        _, _, _, records = self.prepare_unreadable_case()
        with self.assertRaisesRegex(RuntimeError, 'Refusing to exclude readable'):
            GROUP.plan('M 2', [records[0]['relative']])
        with self.assertRaisesRegex(ValueError, 'exact manifest-relative'):
            GROUP.plan('M 2', ['missing.fit'])
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            GROUP.plan('M 2', [records[1]['relative']]*2)

    def test_changed_unreadable_input_stops_instead_of_excluding(self):
        _, _, bad, records = self.prepare_unreadable_case()
        bad.write_bytes(b'X'+bytes(2879))
        with self.assertRaisesRegex(RuntimeError, 'changed since verified copy'):
            GROUP.plan('M 2', [records[1]['relative']])

    def test_unreadable_device_master_cannot_be_excluded(self):
        work, _, bad, records = self.prepare_unreadable_case()
        master = bad.with_name('Stacked_bad.fit')
        bad.rename(master)
        records[1]['relative'] = str(master.relative_to(work/'inputs'))
        (work/'copy-manifest.json').write_text(json.dumps({'files':records}))
        with self.assertRaisesRegex(RuntimeError, 'Only unreadable light'):
            GROUP.plan('M 2', [records[1]['relative']])


if __name__ == '__main__':
    unittest.main()
