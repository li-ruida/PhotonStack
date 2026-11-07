#!/usr/bin/env python3
"""Read only verified local FITS headers; prepare explicit per-session recipes.

Never scans or reads the external disk. A missing/ambiguous group is an error,
not permission to silently select a subset of a target.
"""
import argparse
import fcntl
from collections import defaultdict
import importlib.util
import json
from pathlib import Path
import subprocess
import time

SPEC = importlib.util.spec_from_file_location('seestar', Path(__file__).with_name('seestar-sequential.py'))
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


def header(path):
    result = {}
    with path.open('rb') as stream:
        for block_index in range(128):
            block = stream.read(2880)
            if len(block) != 2880:
                raise ValueError('Incomplete FITS header: ' + str(path))
            if block_index == 0 and block[:8] != b'SIMPLE  ':
                raise ValueError('Invalid FITS primary header (missing SIMPLE): ' + str(path))
            for start in range(0, 2880, 80):
                card = block[start:start+80].decode('ascii')
                key = card[:8].strip()
                if key == 'END':
                    return result
                if card[8:10] != '= ':
                    continue
                value = card[10:].strip()
                if value.startswith("'"):
                    value = value[1:].split("'")[0].strip()
                else:
                    value = value.split('/')[0].strip()
                    try:
                        value = float(value.replace('D', 'E'))
                    except ValueError:
                        pass
                result[key] = value
    raise ValueError('FITS header exceeds safety limit: ' + str(path))


def plan(name, exclude_unreadable=()):
    queue = json.loads((RUN.ROOT/'queue.json').read_text())
    target = next(t for t in queue['targets'] if t['name'] == name)
    if target['status'] != 'copied':
        raise RuntimeError('Wait until the entire target is verified locally')
    work = RUN.ROOT/'工作区'/name
    manifest = json.loads((work/'copy-manifest.json').read_text())
    excluded_names = set(exclude_unreadable)
    if len(excluded_names) != len(exclude_unreadable):
        raise ValueError('Duplicate unreadable exclusion')
    if not excluded_names.issubset({f['relative'] for f in manifest['files']}):
        raise ValueError('Unreadable exclusions must be exact manifest-relative paths')
    excluded = []
    groups = defaultdict(lambda: {'lights': [], 'masters': [], 'headers': []})
    for f in manifest['files']:
        path = work/'inputs'/f['relative']
        if path.suffix.lower() not in ('.fit', '.fits', '.fts'):
            continue
        if 'sha256' not in f or path.stat().st_size != f['size']:
            raise RuntimeError('Unverified or changed input: ' + str(path))
        try:
            h = header(path)
        except ValueError as error:
            if f['relative'] not in excluded_names:
                raise
            if not path.name.startswith('Light_'):
                raise RuntimeError('Only unreadable light frames may be excluded explicitly') from error
            if RUN.digest(path) != f['sha256']:
                raise RuntimeError('Excluded input changed since verified copy: ' + str(path)) from error
            excluded.append({**f, 'reason': str(error), 'status': 'unreadable-not-stacked'})
            continue
        if f['relative'] in excluded_names:
            raise RuntimeError('Refusing to exclude readable FITS: ' + str(path))
        role = 'lights' if path.name.startswith('Light_') else 'masters' if path.name.startswith('Stacked_') else None
        if role is None:
            raise RuntimeError('Unclassified FITS: ' + str(path))
        key = (h.get('EXPTIME', h.get('EXPOSURE')), h.get('FILTER'), h.get('NAXIS1'),
               h.get('NAXIS2'), h.get('TELESCOP'), h.get('FOCALLEN'), h.get('WIDECAM'))
        if any(v is None for v in key):
            raise RuntimeError('Missing grouping metadata: ' + str(path))
        groups[key][role].append(str(path))
        groups[key]['headers'].append({'file': str(path), **{k: h.get(k) for k in
            ('DATE-OBS', 'RA', 'DEC', 'NAXIS', 'NAXIS3', 'BAYERPAT', 'EXPTIME', 'FILTER', 'TELESCOP')}})
    if len(excluded) != len(excluded_names):
        raise RuntimeError('Every exclusion must identify an unreadable FITS light frame')
    if not groups:
        raise RuntimeError('No readable FITS groups remain; retain files for review')
    result = []
    for i, (key, group) in enumerate(sorted(groups.items()), 1):
        exposure, filt, width, height, telescope, focal, wide = key
        light_headers = [h for h in group['headers'] if h['file'] in group['lights']]
        if len({(h['NAXIS'], h['NAXIS3'], h['BAYERPAT']) for h in light_headers}) > 1:
            raise RuntimeError('Mixed channel/CFA layouts; split explicitly')
        identifier = f'g{i:02}-{exposure:g}s-{filt}-{int(width)}x{int(height)}'
        if '/' in identifier or '\\' in identifier:
            raise RuntimeError('Unsafe group label')
        group.update(id=identifier, exposure=exposure, filter=filt, width=width, height=height,
                     telescope=telescope, focal=focal, wide=wide)
        if excluded:
            # Target-level audit repeated in each group; it is not inferred group membership.
            # finalize archives group-plan.json alongside the complete copy manifest.
            group['target_unreadable_inputs'] = excluded
        result.append(group)
    return work, result


def run_groups(work, groups, selected, result_tag=None):
    known = {g['id']: g for g in groups}
    if len(set(selected)) != len(selected) or any(s not in known for s in selected):
        raise ValueError('Unknown or duplicate group ID')
    if result_tag is not None and (not result_tag or not all(c.isascii() and (c.isalnum() or c == '-') for c in result_tag)):
        raise ValueError('Result tag must be a nonempty ASCII alphanumeric/hyphen label')
    with (RUN.ROOT/'.run.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        RUN.save(work/'group-plan.json', groups)
        for identifier in selected:
            g = known[identifier]
            lights = sorted(g['lights'])
            if lights and not 3 <= len(lights) <= 2000:
                raise RuntimeError('Unsupported light count; explicit handling required')
            if not lights and len(g['masters']) != 1:
                raise RuntimeError('Choose device master explicitly; no automatic overlap assumption')
            output_name = identifier + ('-' + result_tag if result_tag else '')
            output = work/output_name
            if output.exists():
                raise RuntimeError('Existing group output; inspect it instead of restarting: ' + str(output))
            recipe = work/(output_name+'.recipe.txt')
            settings = {
                'decode.demosaic': 'malvar-guarded', 'registration.minimum-matches': '30',
                'registration.refine-centroids': 'on', 'background.model': 'polynomial',
                'background.polynomial-strength': '0.5', 'denoise.multiscale-luminance': '0.5',
                'denoise.multiscale-chroma': '0.7', 'develop.saturation': '0.95',
                'develop.shadow-neutralization': '0.7', 'develop.star-color-cleanup': '0.5',
                'develop.background': '0.03', 'crop.coverage': '1'}
            with recipe.open('x') as stream:
                stream.write('PHOTONSTACK_DEEP_SKY_RECIPE 1\n')
                for k, v in settings.items():
                    stream.write(k+' '+json.dumps(v)+'\n')
                for path in lights:
                    stream.write('input '+json.dumps(path, ensure_ascii=False)+'\n')
            cmd = [str(RUN.CLI), 'deep-sky', '--recipe', str(recipe), '--output-dir', str(output)]
            if not lights:
                cmd += ['--from-master', g['masters'][0]]
            print('START_GROUP ' + identifier + ' lights=' + str(len(lights)), flush=True)
            last_time, last_stage = 0, None
            with (work/(output_name+'.engine.log')).open('x') as log:
                child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, pass_fds=(lock.fileno(),))
                for line in child.stdout:
                    log.write(line)
                    log.flush()
                    try:
                        event = json.loads(line)
                    except ValueError:
                        continue
                    stage = event.get('stage')
                    if event.get('type') != 'progress':
                        print(json.dumps({k:event[k] for k in ('type','success','message','selectedCount','crop') if k in event}), flush=True)
                    elif stage != last_stage or time.monotonic()-last_time > 15:
                        print(line.strip(), flush=True)
                        last_time, last_stage = time.monotonic(), stage
                code = child.wait()
            if code:
                raise RuntimeError('Group failed; all local files retained for inspection: ' + identifier)
            subprocess.run([str(RUN.CLI), 'preview', '--input', str(output/'developed.tiff'),
                            '--output', str(work/(output_name+'.png')), '--width', '1000'],
                           check=True, stdout=subprocess.DEVNULL)
            print('READY_FOR_VISUAL_REVIEW ' + str(work/(output_name+'.png')), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target')
    parser.add_argument('--run', nargs='+', help='Explicit group IDs after inspecting the printed plan')
    parser.add_argument('--result-tag', help='Explicit fresh result suffix for a reviewed retry; never overwrites prior output')
    parser.add_argument('--exclude-unreadable', action='append', default=[],
                        help='Reviewed exact manifest-relative light path; requires failed header parsing and unchanged SHA; archived in group metadata')
    args = parser.parse_args()
    work, groups = plan(args.target, args.exclude_unreadable)
    for g in groups:
        print(json.dumps({k: v for k, v in g.items() if k not in ('lights','headers')}, ensure_ascii=False))
        print('LIGHT_COUNT', len(g['lights']))
        for field in ('RA','DEC','DATE-OBS'):
            values = [h[field] for h in g['headers'] if h[field] is not None]
            print(field, min(values) if values else None, max(values) if values else None)
    if args.run:
        if args.target in ('Meteor','Startrails','MilkyWay') or args.target[0].isdigit() or args.target.startswith('C-'):
            raise RuntimeError('Dedicated moving/wide-field target recipe required')
        run_groups(work, groups, args.run, args.result_tag)
