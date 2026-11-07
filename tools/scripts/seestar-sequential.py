#!/usr/bin/env python3
"""One-target-at-a-time, read-only-source Seestar ingestion with verified copies.

No processing or deletion happens automatically. finalize requires explicit visual
acceptance, verifies deliverables, then removes only files in this run's manifest.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

SOURCE = Path('/Volumes/ll/seestar')
ROOT = Path('/Users/lrd/Desktop/PhotonStack-ll-20261021')
CLI = Path(__file__).resolve().parents[2] / 'build/release/apps/PhotonStackCLI/photonstack'
EXTENSIONS = {'.fit', '.fits', '.fts', '.tif', '.tiff', '.jpg', '.jpeg', '.png'}


def natural(value):
    return [int(x) if x.isdigit() else x.casefold() for x in re.split(r'(\d+)', value)]


def save(path, obj):
    temporary = path.with_suffix(path.suffix + '.tmp')
    with temporary.open('w') as stream:
        json.dump(obj, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def inventory():
    if ROOT.exists():
        raise RuntimeError('Run folder already exists; inspect state, do not replace it')
    dirs = {p.name: p for p in SOURCE.iterdir() if p.is_dir() and not p.is_symlink()}
    excluded = [name for name in dirs if any(k in name for k in ('插件', '脚本'))]
    names = sorted({name.removesuffix('_sub') for name in dirs if name not in excluded}, key=natural)
    targets = [{'name': name, 'folders': [n for n in (name, name + '_sub') if n in dirs],
                'status': 'pending'} for name in names]
    ROOT.mkdir()
    (ROOT / '最终成片').mkdir()
    (ROOT / '工作区').mkdir()
    save(ROOT / 'queue.json', {'source': str(SOURCE), 'created': time.time(),
                              'excluded': excluded, 'targets': targets})
    print(json.dumps({'count': len(targets), 'targets': names, 'excluded': excluded}, ensure_ascii=False))


def copy_next():
    queue = json.loads((ROOT / 'queue.json').read_text())
    active = [t for t in queue['targets'] if t['status'] not in ('pending', 'complete', 'skipped')]
    if active:
        raise RuntimeError('Finish current target first: ' + repr(active))
    target = next((t for t in queue['targets'] if t['status'] == 'pending'), None)
    if target is None:
        print('All targets finished')
        return
    name = target['name']
    # Enumerate only this target; never follow symlinks or read plugin files.
    files = []
    for folder in target['folders']:
        for base, dirs, names in os.walk(SOURCE / folder, followlinks=False):
            dirs[:] = sorted([d for d in dirs if not d.startswith('.') and not (Path(base)/d).is_symlink()], key=natural)
            for filename in sorted(names, key=natural):
                p = Path(base) / filename
                if p.is_symlink() or p.suffix.lower() not in EXTENSIONS or filename.startswith('.'):
                    continue
                stat = p.stat()
                files.append({'relative': str(p.relative_to(SOURCE)), 'size': stat.st_size,
                              'mtime_ns': stat.st_mtime_ns})
    total = sum(f['size'] for f in files)
    # Conservative space reserve for decoded RGB registered images plus stack.
    needed = total * 10 + 20 * 1024**3
    if shutil.disk_usage(ROOT).free < needed:
        raise RuntimeError(f'Insufficient free SSD space for {name}: need {needed / 1024**3:.1f} GiB')
    work = ROOT / '工作区' / name
    work.mkdir()
    target.update(status='copying', bytes=total, file_count=len(files))
    save(ROOT / 'queue.json', queue)
    manifest = {'target': name, 'source': str(SOURCE), 'files': files}
    save(work / 'copy-manifest.json', manifest)
    started = time.monotonic()
    for index, f in enumerate(files):
        src = SOURCE / f['relative']
        dst = work / 'inputs' / f['relative']
        dst.parent.mkdir(parents=True, exist_ok=True)
        h = hashlib.sha256()
        partial = dst.with_suffix(dst.suffix + '.partial')
        with src.open('rb') as inp, partial.open('xb') as out:
            while True:
                block = inp.read(4 * 1024 * 1024)
                if not block:
                    break
                out.write(block)
                h.update(block)
            out.flush()
            os.fsync(out.fileno())
        stat = src.stat()
        if (stat.st_size, stat.st_mtime_ns) != (f['size'], f['mtime_ns']):
            raise RuntimeError('Source changed during copy: ' + str(src))
        if partial.stat().st_size != f['size'] or digest(partial) != h.hexdigest():
            raise RuntimeError('Copy verification failed: ' + str(src))
        partial.rename(dst)
        os.utime(dst, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        f['sha256'] = h.hexdigest()
        save(work / 'copy-manifest.json', manifest)
        print(json.dumps({'target': name, 'copied': index+1, 'total': len(files),
                          'file': src.name, 'seconds': round(time.monotonic()-started, 1)}, ensure_ascii=False), flush=True)
        time.sleep(0.05)
    target['status'] = 'copied'
    save(ROOT / 'queue.json', queue)


def process_current():
    queue = json.loads((ROOT / 'queue.json').read_text())
    active = [t for t in queue['targets'] if t['status'] == 'copied']
    if len(active) != 1:
        raise RuntimeError('Exactly one fully copied target required')
    target = active[0]
    name = target['name']
    if name in ('Meteor', 'Startrails', 'MilkyWay') or re.match(r'^(\d+P|C-)', name):
        raise RuntimeError('Special moving/wide-field target requires a dedicated reviewed recipe')
    work = ROOT / '工作区' / name
    manifest = json.loads((work / 'copy-manifest.json').read_text())
    lights = [work/'inputs'/f['relative'] for f in manifest['files']
              if Path(f['relative']).name.startswith('Light_') and Path(f['relative']).suffix.lower() in ('.fit', '.fits')]
    filters = {re.search(r'_([0-9.]+)s_(.+)_\d{8}-\d+', p.name).groups() for p in lights
               if re.search(r'_([0-9.]+)s_(.+)_\d{8}-\d+', p.name)}
    if len(filters) > 1:
        raise RuntimeError('Multiple exposure/filter groups require separate processing: ' + repr(filters))
    masters = [work/'inputs'/f['relative'] for f in manifest['files']
               if Path(f['relative']).name.startswith('Stacked_') and Path(f['relative']).suffix.lower() in ('.fit', '.fits')]
    if not lights and len(masters) != 1:
        raise RuntimeError('Choose among device masters explicitly: ' + repr(masters))
    if lights and not 3 <= len(lights) <= 2000:
        raise RuntimeError('Unsupported number of original lights')
    recipe = work/'run-recipe.txt'
    settings = {
        'decode.demosaic': 'malvar-guarded', 'registration.minimum-matches': '12',
        'registration.refine-centroids': 'on', 'background.model': 'polynomial',
        'background.polynomial-strength': '0.6', 'denoise.multiscale-luminance': '0.5',
        'denoise.multiscale-chroma': '0.7', 'develop.saturation': '1',
        'develop.shadow-neutralization': '0.7', 'develop.star-color-cleanup': '0.5',
        'develop.background': '0.03', 'crop.coverage': '1'}
    with recipe.open('x') as stream:
        stream.write('PHOTONSTACK_DEEP_SKY_RECIPE 1\n')
        for key, value in settings.items():
            stream.write(key + ' ' + json.dumps(value) + '\n')
        for path in sorted(lights):
            stream.write('input ' + json.dumps(str(path), ensure_ascii=False) + '\n')
    cmd = [str(CLI), 'deep-sky', '--recipe', str(recipe), '--output-dir', str(work/'processed-v1')]
    if not lights:
        cmd += ['--from-master', str(masters[0])]
    print(json.dumps({'target': name, 'lights': len(lights), 'groups': list(filters), 'command': cmd}, ensure_ascii=False), flush=True)
    last_time = 0
    last_stage = None
    with (work/'engine.log').open('x') as log:
        child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                 pass_fds=(RUN_LOCK.fileno(),))
        for line in child.stdout:
            log.write(line)
            log.flush()
            try:
                event = json.loads(line)
            except ValueError:
                continue
            stage = event.get('stage')
            if event.get('type') != 'progress':
                print(json.dumps({k: event[k] for k in ('type', 'success', 'message', 'selectedCount', 'crop') if k in event}), flush=True)
            elif stage != last_stage or time.monotonic() - last_time > 15:
                print(line.strip(), flush=True)
                last_time, last_stage = time.monotonic(), stage
        code = child.wait()
    if code:
        raise RuntimeError('Processing failed; retained all local files for investigation')
    subprocess.run([str(CLI), 'preview', '--input', str(work/'processed-v1/developed.tiff'),
                    '--output', str(work/'preview.png'), '--width', '1000'], check=True, stdout=subprocess.DEVNULL)
    print('READY_FOR_VISUAL_REVIEW ' + str(work/'preview.png'), flush=True)


def finalize(name, processed, note):
    if not name or not processed or not note:
        raise RuntimeError('Target, processed subdirectory, and visual acceptance note required')
    queue = json.loads((ROOT / 'queue.json').read_text())
    target = next(t for t in queue['targets'] if t['name'] == name)
    if target['status'] != 'copied':
        raise RuntimeError('Target must have a fully verified copy')
    work = ROOT / '工作区' / name
    if work.resolve().parent != (ROOT / '工作区').resolve() or work.is_symlink():
        raise RuntimeError('Unsafe work path')
    specifications = [processed] if isinstance(processed, str) else processed
    results = {}
    for specification in specifications:
        label, directory = specification.split('=', 1) if '=' in specification else (specification, specification)
        if not label or Path(label).name != label or label in ('.', '..') or label in results:
            raise RuntimeError('Unsafe or duplicate output group label')
        result = (work / directory).resolve()
        if not result.is_relative_to(work.resolve()):
            raise RuntimeError('Processing result must belong to this target')
        report = json.loads((result / 'report.json').read_text())
        if not report['success']:
            raise RuntimeError('Processing did not succeed: ' + label)
        results[label] = (result, report)
    plan_path = work/'group-plan.json'
    if plan_path.exists():
        expected = {g['id'] for g in json.loads(plan_path.read_text())}
        if set(results) != expected:
            raise RuntimeError('Every planned exposure/filter group must be reviewed before target cleanup')
    # The two final masters plus full-size viewing JPEG are retained, not caches.
    output = ROOT / '最终成片' / name
    output.mkdir()
    sources = {'原片校验.json': work / 'copy-manifest.json'}
    if plan_path.exists():
        sources['分组元数据.json'] = plan_path
    prefixes = {}
    for label, (result, report) in results.items():
        prefix = label+'/' if len(results) > 1 else ''
        prefixes[label] = prefix
        if prefix:
            (output/label).mkdir()
        for filename, source in {'成片.tiff': Path(report['developed']), '线性母版.fits': Path(report['master']),
                                 '处理配方.txt': result/'recipe.txt', '处理报告.json': result/'report.json'}.items():
            sources[prefix+filename] = source
    hashes = {}
    for filename, src in sources.items():
        if not src.resolve().is_relative_to(work.resolve()):
            raise RuntimeError('Output source is not inside owned work directory')
        dst = output / filename
        shutil.copyfile(src, dst)
        expected = digest(src)
        if digest(dst) != expected:
            raise RuntimeError('Published result checksum mismatch')
        hashes[filename] = expected
    for prefix in prefixes.values():
        subprocess.run([str(CLI), 'convert', '--input', str(output/(prefix+'成片.tiff')),
                        '--output', str(output/(prefix+'成片.jpg')), '--bit-depth', '8', '--quality', '0.96'], check=True)
        for filename in ('成片.tiff', '线性母版.fits', '成片.jpg'):
            subprocess.run([str(CLI), 'inspect', str(output/(prefix+filename))], check=True, stdout=subprocess.DEVNULL)
        hashes[prefix+'成片.jpg'] = digest(output/(prefix+'成片.jpg'))
    stages = {str(p.relative_to(work)): json.loads(p.read_text()) for p in work.rglob('report.json')}
    save(output/'阶段报告.json', stages)
    save(output / '验收.json', {'visual_note': note, 'sha256': hashes, 'time': time.time(),
                               'engine_sha256': digest(CLI),
                               'groups': {label: {'master_reused': report['masterReused'],
                                                 'selected_count': report['selectedCount']}
                                          for label, (_, report) in results.items()}})
    # Snapshot and remove exact owned files, never recursively delete a broad root.
    cleanup = []
    for base, dirs, files in os.walk(work, followlinks=False):
        for entry in dirs + files:
            if (Path(base)/entry).is_symlink():
                raise RuntimeError('Unexpected symlink; cleanup stopped')
        for filename in files:
            path = Path(base)/filename
            stat = path.stat()
            cleanup.append({'path': str(path), 'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns})
    save(output / '已清理过程文件.json', cleanup)
    for item in cleanup:
        p = Path(item['path'])
        stat = p.stat()
        if (stat.st_size, stat.st_mtime_ns) != (item['size'], item['mtime_ns']):
            raise RuntimeError('Working file changed; cleanup stopped')
        p.unlink()
    for base, dirs, files in os.walk(work, topdown=False):
        Path(base).rmdir()
    target.update(status='complete', output=str(output), visual_note=note,
                  cleaned_bytes=sum(x['size'] for x in cleanup))
    save(ROOT/'queue.json', queue)
    print(json.dumps(target, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['inventory', 'copy-next', 'process-current', 'finalize'])
    parser.add_argument('--target')
    parser.add_argument('--processed', action='append', help='Repeat per group; optionally GROUP=RESULT_DIRECTORY')
    parser.add_argument('--visual-note')
    args = parser.parse_args()
    if args.action == 'inventory':
        inventory()
    else:
        # The engine inherits this lock: even if the wrapper is interrupted,
        # cleanup/another reader cannot race a still-running engine process.
        with (ROOT/'.run.lock').open('a') as RUN_LOCK:
            fcntl.flock(RUN_LOCK, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if args.action == 'finalize':
                finalize(args.target, args.processed, args.visual_note)
            else:
                {'copy-next': copy_next, 'process-current': process_current}[args.action]()
