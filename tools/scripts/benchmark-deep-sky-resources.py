#!/usr/bin/env python3
"""Measure one bounded, reproducible macOS deep-sky CLI run without editing inputs.

Requires an explicit recipe and a fresh output directory. Resource estimates are
sampled, not reservations; maximum RSS is the child process value from time(1).
"""
import argparse
import hashlib
import json
import re
import subprocess
import threading
import time
from pathlib import Path


def tree_bytes(root):
    total = 0
    if not root.exists():
        return total
    try:
        for path in root.rglob('*'):
            try:
                if path.is_file() and not path.is_symlink():
                    total += path.stat().st_size
            except FileNotFoundError:
                pass  # Owned temporary files may be released between samples.
    except FileNotFoundError:
        pass
    return total


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--recipe', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    cli, recipe, output = (p.resolve() for p in (args.cli, args.recipe, args.output_dir))
    if output.exists():
        parser.error('Output directory must be new; prior evidence is never overwritten')
    output.mkdir(parents=True)
    result = output / 'result'
    command = ['/usr/bin/time', '-l', '-o', str(output / 'resources.txt'), str(cli),
               'deep-sky', '--recipe', str(recipe), '--output-dir', str(result)]
    stop = threading.Event()
    observations = {'peak_work_bytes_sampled': 0, 'peak_output_bytes_sampled': 0,
                    'disk_sample_interval_seconds': .02, 'disk_samples': 0}

    def sample():
        while not stop.is_set():
            observations['peak_work_bytes_sampled'] = max(
                observations['peak_work_bytes_sampled'], tree_bytes(result / '.aligned-work'))
            observations['peak_output_bytes_sampled'] = max(
                observations['peak_output_bytes_sampled'], tree_bytes(result))
            observations['disk_samples'] += 1
            stop.wait(.02)

    started = time.monotonic()
    sampler = threading.Thread(target=sample)
    sampler.start()
    stages = []
    try:
        with (output / 'events.jsonl').open('w') as log:
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in child.stdout:
                log.write(line)
                try:
                    event = json.loads(line)
                    if event.get('type') == 'progress':
                        stage = event.get('stage')
                        if not stages or stages[-1]['stage'] != stage:
                            stages.append({'stage': stage, 'started_seconds': time.monotonic() - started})
                except json.JSONDecodeError:
                    pass
            code = child.wait()
    finally:
        stop.set()
        sampler.join()
    elapsed = time.monotonic() - started
    resource_text = (output / 'resources.txt').read_text()
    resource_values = {}
    for label in ['maximum resident set size', 'block input operations', 'block output operations',
                  'peak memory footprint']:
        match = re.search(r'^\s*(\d+)\s+' + re.escape(label) + r'\s*$', resource_text, re.MULTILINE)
        if match:
            resource_values[label.replace(' ', '_')] = int(match.group(1))
    cpu = re.search(r'([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys', resource_text)
    if cpu:
        resource_values.update(real_seconds=float(cpu[1]), user_seconds=float(cpu[2]), system_seconds=float(cpu[3]))
    outputs = {}
    if result.exists():
        for path in result.iterdir():
            if path.is_file() and path.suffix in {'.fits', '.tiff'}:
                outputs[path.name] = {'bytes': path.stat().st_size,
                                      'sha256': sha256(path)}
    summary = {'command': command, 'exit_code': code, 'elapsed_seconds': elapsed,
               'cli_sha256': sha256(cli),
               'recipe_sha256': sha256(recipe),
               'resources': resource_values, 'disk': observations, 'stages': stages,
               'outputs': outputs, 'temporary_work_cleaned': not (result / '.aligned-work').exists()}
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    raise SystemExit(code)


if __name__ == '__main__':
    main()
