#!/usr/bin/env python3
"""Run PhotonStack artifact detection against a hash-locked image manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


PHOTO_EXTENSIONS = {
    ".arw", ".cr2", ".cr3", ".dng", ".fits", ".fit", ".fts", ".heic",
    ".jpeg", ".jpg", ".nef", ".orf", ".pef", ".png", ".raf", ".rw2",
    ".srw", ".tif", ".tiff",
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Evaluate satellite/artifact detection without embedding third-party images in git."
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("tests/artifact-blindset/manifest.json"),
    )
    parser.add_argument(
        "--binary",
        type=Path,
        default=Path("build/debug/apps/PhotonStackCLI/photonstack"),
    )
    parser.add_argument("--cache", type=Path, default=Path("build/artifact-blindset"))
    parser.add_argument(
        "--split",
        choices=("development", "evaluation", "all"),
        default="evaluation",
    )
    parser.add_argument(
        "--download",
        action="store_true",
        help="Download missing network fixtures after verifying their declared SHA-256 hashes.",
    )
    parser.add_argument(
        "--desktop-dir",
        type=Path,
        help="Also run every supported photo directly inside this directory as informational coverage.",
    )
    parser.add_argument("--require-desktop", action="store_true")
    parser.add_argument("--json-report", type=Path)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Print periodic progress and processing errors instead of every case.",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_manifest(document: dict) -> list[dict]:
    if document.get("schemaVersion") != 1:
        raise ValueError("manifest schemaVersion must be 1")
    cases = document.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("manifest cases must be a non-empty array")

    identifiers: set[str] = set()
    hashes: set[str] = set()
    groups_by_split: dict[str, set[str]] = {}
    for case in cases:
        identifier = case.get("id")
        split = case.get("split")
        group = case.get("group")
        digest = case.get("sha256")
        expectation = case.get("expectation", {})
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("every case needs a non-empty id")
        if identifier in identifiers:
            raise ValueError(f"duplicate case id: {identifier}")
        identifiers.add(identifier)
        if split not in {"development", "evaluation"}:
            raise ValueError(f"{identifier}: split must be development or evaluation")
        if not isinstance(group, str) or not group:
            raise ValueError(f"{identifier}: group is required")
        groups_by_split.setdefault(group, set()).add(split)
        if not isinstance(digest, str) or len(digest) != 64:
            raise ValueError(f"{identifier}: sha256 must contain 64 hexadecimal characters")
        int(digest, 16)
        if digest in hashes:
            raise ValueError(f"{identifier}: duplicate image content hash")
        hashes.add(digest)
        if "minimumArtifacts" not in expectation and "maximumArtifacts" not in expectation:
            raise ValueError(f"{identifier}: expectation needs a minimum or maximum artifact count")
        minimum = expectation.get("minimumArtifacts")
        maximum = expectation.get("maximumArtifacts")
        if minimum is not None and (not isinstance(minimum, int) or minimum < 0):
            raise ValueError(f"{identifier}: minimumArtifacts must be a nonnegative integer")
        if maximum is not None and (not isinstance(maximum, int) or maximum < 0):
            raise ValueError(f"{identifier}: maximumArtifacts must be a nonnegative integer")
        if minimum is not None and maximum is not None and minimum > maximum:
            raise ValueError(f"{identifier}: minimumArtifacts exceeds maximumArtifacts")

    leaking_groups = sorted(group for group, splits in groups_by_split.items() if len(splits) > 1)
    if leaking_groups:
        raise ValueError(f"photographer/source groups cross splits: {', '.join(leaking_groups)}")
    return cases


def download(case: dict, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".partial")
    try:
        completed = subprocess.run(
            [
                "curl", "-fsSL", "--retry", "5", "--retry-all-errors", "--retry-delay", "2",
                "--max-time", "180",
                "--user-agent",
                "PhotonStackArtifactBenchmark/2.0 "
                "(https://github.com/li-ruida/PhotonStack; reproducibility fixture)",
                "--output", str(temporary), case["downloadUrl"],
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr.strip() or f"curl exited {completed.returncode}")
        actual = sha256(temporary)
        if actual != case["sha256"]:
            raise RuntimeError(
                f"{case['id']}: downloaded SHA-256 {actual} does not match {case['sha256']}"
            )
        temporary.replace(destination)
    finally:
        if temporary.exists():
            temporary.unlink()


def fixture_path(case: dict, cache: Path, allow_download: bool) -> Path:
    destination = cache / case["filename"]
    if not destination.exists():
        if not allow_download:
            raise FileNotFoundError(
                f"{destination} is missing; rerun with --download to fetch hash-locked fixtures"
            )
        download(case, destination)
    actual = sha256(destination)
    if actual != case["sha256"]:
        raise RuntimeError(
            f"{case['id']}: cached SHA-256 {actual} does not match {case['sha256']}"
        )
    return destination


def detect(binary: Path, image: Path) -> dict:
    completed = subprocess.run(
        [str(binary), "artifacts", "detect", "--input", str(image), "--preserve-meteors", "on"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"detector failed for {image} ({completed.returncode}): {completed.stderr.strip()}"
        )
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"detector emitted invalid JSON for {image}: {error}") from error
    if report.get("type") != "complete" or report.get("command") != "artifacts detect":
        raise RuntimeError(f"detector emitted an unexpected report for {image}")
    return report


def evaluate(case: dict, report: dict) -> tuple[bool, list[str]]:
    artifacts = report.get("trails")
    meteors = report.get("protectedMeteors")
    if not isinstance(artifacts, int) or artifacts < 0:
        return False, ["invalid trails count"]
    if not isinstance(meteors, int) or meteors < 0:
        return False, ["invalid protectedMeteors count"]

    expectation = case["expectation"]
    failures: list[str] = []
    minimum = expectation.get("minimumArtifacts")
    maximum = expectation.get("maximumArtifacts")
    if minimum is not None and artifacts < minimum:
        failures.append(f"artifacts {artifacts} < minimum {minimum}")
    if maximum is not None and artifacts > maximum:
        failures.append(f"artifacts {artifacts} > maximum {maximum}")
    return not failures, failures


def run_network_case(case: dict, binary: Path, cache: Path, allow_download: bool) -> dict:
    try:
        image = fixture_path(case, cache, allow_download)
        report = detect(binary, image)
        passed, reasons = evaluate(case, report)
        processed = True
    except (OSError, RuntimeError) as error:
        report = {"trails": None, "protectedMeteors": None}
        passed = False
        processed = False
        reasons = [str(error)]
    return {
        "id": case["id"],
        "split": case["split"],
        "processed": processed,
        "passed": passed,
        "reasons": reasons,
        "trails": report.get("trails"),
        "protectedMeteors": report.get("protectedMeteors"),
    }


def desktop_photos(directory: Path) -> list[Path]:
    if not directory.is_dir():
        raise FileNotFoundError(f"desktop directory does not exist: {directory}")
    return sorted(
        path for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() in PHOTO_EXTENSIONS
    )


def main() -> int:
    arguments = parse_arguments()
    if arguments.workers < 1 or arguments.workers > 12:
        print("error: --workers must be between 1 and 12", file=sys.stderr)
        return 2
    if not arguments.binary.is_file():
        print(f"error: detector binary does not exist: {arguments.binary}", file=sys.stderr)
        return 2
    try:
        document = json.loads(arguments.manifest.read_text(encoding="utf-8"))
        cases = validate_manifest(document)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: invalid manifest: {error}", file=sys.stderr)
        return 2

    selected = [case for case in cases if arguments.split == "all" or case["split"] == arguments.split]
    results: list[dict] = []
    network_failures = 0
    with ThreadPoolExecutor(max_workers=arguments.workers) as executor:
        outcomes = executor.map(
            lambda case: run_network_case(case, arguments.binary, arguments.cache, arguments.download),
            selected,
        )
        for index, (case, result) in enumerate(zip(selected, outcomes), start=1):
            network_failures += 0 if result["passed"] else 1
            if not arguments.quiet:
                status = "PASS" if result["passed"] else "FAIL"
                print(
                    f"{status:4} {case['id']:<36} "
                    f"artifacts={str(result.get('trails')):<4} meteors={result.get('protectedMeteors')}"
                )
                for reason in result["reasons"]:
                    print(f"     {reason}")
            elif not result["processed"]:
                print(f"ERROR {case['id']}: {'; '.join(result['reasons'])}")
            elif index % 25 == 0 or index == len(selected):
                processed_so_far = sum(item["processed"] for item in results) + 1
                passed_so_far = sum(item["passed"] for item in results) + int(result["passed"])
                print(
                    f"progress: {index}/{len(selected)} attempted, "
                    f"{processed_so_far} processed, {passed_so_far} label-conforming"
                )
            results.append(result)

    desktop_results: list[dict] = []
    desktop_failures = 0
    if arguments.desktop_dir is not None:
        try:
            photos = desktop_photos(arguments.desktop_dir)
        except OSError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        if arguments.require_desktop and not photos:
            print("error: no supported desktop photos were found", file=sys.stderr)
            return 2
        for image in photos:
            try:
                report = detect(arguments.binary, image)
                print(
                    f"INFO desktop/{image.name:<28} "
                    f"artifacts={report['trails']:<4} meteors={report['protectedMeteors']}"
                )
                desktop_results.append(
                    {
                        "filename": image.name,
                        "trails": report["trails"],
                        "protectedMeteors": report["protectedMeteors"],
                    }
                )
            except RuntimeError as error:
                desktop_failures += 1
                print(f"FAIL desktop/{image.name}: {error}")

    summary = {
        "schemaVersion": 1,
        "manifest": str(arguments.manifest),
        "split": arguments.split,
        "cases": results,
        "desktop": desktop_results,
        "passed": len(results) - network_failures,
        "failed": network_failures,
        "processed": sum(result["processed"] for result in results),
        "processingFailed": sum(not result["processed"] for result in results),
        "desktopFailed": desktop_failures,
    }
    if arguments.json_report is not None:
        arguments.json_report.parent.mkdir(parents=True, exist_ok=True)
        arguments.json_report.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(
        f"summary: {len(results) - network_failures} passed, {network_failures} failed, "
        f"{desktop_failures} desktop errors"
    )
    return 0 if network_failures == 0 and desktop_failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
