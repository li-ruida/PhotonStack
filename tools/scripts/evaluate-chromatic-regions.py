#!/usr/bin/env python3
"""Audit fixed native-pixel regions without shipping private photographic data.

The manifest binds the source by SHA-256. Pixel checks run through the engine's
ImageCodec, not an 8-bit screenshot or the detector being evaluated. This is a
sampled acceptance check, not a claim of ground truth at every image pixel.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--regions", type=Path, default=root / "tests/fixtures/chromatic-beacon-native.tsv")
    parser.add_argument("--binary", type=Path,
                        default=root / "build/release/tests/engine/photonstack_chromatic_beacon_tests")
    args = parser.parse_args()
    hashes = re.findall(r"^# Source SHA256: ([a-f0-9]{64})$", args.regions.read_text(), re.MULTILINE)
    if len(hashes) != 1:
        parser.error("region manifest must bind exactly one source SHA-256")
    source_hash = digest(args.source)
    if source_hash != hashes[0]:
        parser.error("source SHA-256 differs from the human-reviewed fixture; no pixel assertions were run")
    result_hash = digest(args.result)
    completed = subprocess.run(
        [str(args.binary.resolve()), "--regions", str(args.source.resolve()),
         str(args.result.resolve()), str(args.regions.resolve())],
        capture_output=True, text=True, check=False,
    )
    if completed.returncode not in (0, 1):
        raise RuntimeError(f"native review failed: {completed.stderr.strip()}")
    report = json.loads(completed.stdout)
    report.update(schemaVersion=1, sourceSHA256=source_hash, resultSHA256=result_hash,
                  manifest=str(args.regions.resolve()))
    print(json.dumps(report, indent=2))
    return 0 if completed.returncode == 0 and report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
