#!/usr/bin/env python3
"""Materialize the visual AI review of the v22 evaluation contact sheets."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from datetime import date
from pathlib import Path


# One decision per contact-sheet tile, in the same order as index.json.
# R=removable, M=mixed protected/removable, P=protected, N=no trail,
# U=unsuitable benchmark material, A=ambiguous. H/M/L is confidence.
EVALUATION_SHEET_DECISIONS = {
    "removable-artifact-01.jpg": "R1H R1M R1H R2-4M R1H R1M R3H R1H R1M R1-2M",
    "removable-artifact-02.jpg": "M1H R1H R1H AL R1H R1-3M R1H R1M R2H UH",
    "removable-artifact-03.jpg": "R1H UH R10-20H R1H R1M R1H R2-4M M3-5H R1M R1H",
    "removable-artifact-04.jpg": "UH R1H R1-2H R2-3H UH R1H R1H R1H R10-30H UH",
    "removable-artifact-05.jpg": "R1H R1H UH R5-10H R1H R1H AL UH R1H R1H",
    "removable-artifact-06.jpg": "R2-4H NH R1H R1H R1H R1H UH R1H UH UH",
    "protected-meteor-01.jpg": "P1H P1H UH P1M P1H UH P1M P1H NM P1H",
    "protected-meteor-02.jpg": "P1H UH UH P1H UH UH P1H UH UH UH",
    "protected-meteor-03.jpg": "P1M P1H P1H P1H P1H UH UH P1H P1H NH",
    "protected-meteor-04.jpg": "P1H NH UH UH P1M P2H UH UH AM UH",
    "protected-meteor-05.jpg": "P1M UH P1H P1H UH P1H UH UH UH P1M",
    "protected-meteor-06.jpg": "P1H P2M UH UH UH NH UH UH P1H UH",
    "protected-meteor-07.jpg": "P1H UH P1H UH P1H P1H UH P1H NH PH",
    "protected-meteor-08.jpg": "NM P1H P1H PH UH UH UH P1H P1M P1M",
    "protected-meteor-09.jpg": "P1M UH AM UH P1H UH UH P1H UH P1M",
    "protected-star-trail-01.jpg": "M2H PH PH UH PH PH PH UH PH PH",
    "protected-star-trail-02.jpg": "PH PH PH PH PH PH PH PH M1M PH",
    "protected-star-trail-03.jpg": "PH PH PH PH PH PH PH PH PH M1M",
    "protected-star-trail-04.jpg": "PH M1M PH PH PH PH PH PH PH PH",
    "protected-star-trail-05.jpg": "PH PH PH PH UH PH UH PH PH UH",
    "protected-star-trail-06.jpg": "PH PH PH M1H PH PH PH PH PH PH",
    "protected-star-trail-07.jpg": "PH PH PH PH PH UH PH UH PH PH",
    "protected-star-trail-08.jpg": "UH PH P1H PH PH PH PH PH PH M1H",
    "protected-star-trail-09.jpg": "PH PH PH PH PH PH M2H PH PH PH",
    "protected-star-trail-10.jpg": "PH PH PH PH PH PH PH PH PH PH",
    "protected-star-trail-11.jpg": "PH PH PH PH PH PH AM UH UH PH",
    "protected-star-trail-12.jpg": "PH PH PH PH PH PH PH PH PH PH",
    "protected-star-trail-13.jpg": "PH PH PH PH PH PH PH UH PH PH",
    "protected-star-trail-14.jpg": "PH M1H PH PH M1M PH PH PH PH PH",
    "protected-star-trail-15.jpg": "PH PH PH PH PH PH M1H UH PH PH",
}

DEVELOPMENT_SHEET_DECISIONS = {
    "removable-artifact-01.jpg": "R2H R1H R1H R10-30H R1M R1H PH R1H NM R1H",
    "removable-artifact-02.jpg": "R1M R1H R1H UH M2H R1M R1H R1M PH R1M",
    "removable-artifact-03.jpg": "R1M R1H M10-30H R20-40H R2H UH R1H UH R1H UH",
    "removable-artifact-04.jpg": "UH UH R1M UH UH UH R1H UH R1H R1H",
    "protected-meteor-01.jpg": "P2H UH NM P1H UH UH P1H UH P1H P1H",
    "protected-meteor-02.jpg": "UH UH UH UH P1H P1H UH UH P1M P1H",
    "protected-meteor-03.jpg": "UH P1H P1H UH UH P1M P1M UH UH P1M",
    "protected-meteor-04.jpg": "UH P1H UH P1H P1H NH P1M PH P1H P1H",
    "protected-meteor-05.jpg": "P1H P2H UH UH P1H P1H P1M P1M P2M P1H",
    "protected-meteor-06.jpg": "UH P1H P1H NM UH NH UH P1H UH UH",
    "protected-star-trail-01.jpg": "PH M1M PH M1H PH PH PH PH PH PH",
    "protected-star-trail-02.jpg": "PH PH PH PH UH PH PH PH PH PH",
    "protected-star-trail-03.jpg": "PH PH PH PH UH PH PH PH PH PH",
    "protected-star-trail-04.jpg": "PH PH PH PH PH PH PH PH PH PH",
    "protected-star-trail-05.jpg": "M1H PH UH PH PH PH PH PH PH PH",
    "protected-star-trail-06.jpg": "PH PH PH PH PH PH PH PH PH UH",
    "protected-star-trail-07.jpg": "PH PH PH PH PH M1H PH PH PH PH",
    "protected-star-trail-08.jpg": "PH PH PH PH PH PH PH UH PH PH",
    "protected-star-trail-09.jpg": "PH PH PH UH PH PH PH PH PH PH",
    "protected-star-trail-10.jpg": "PH PH M1H PH PH PH PH M1H PH PH",
}

CODE_PATTERN = re.compile(r"^(?P<label>[RMPNUA])(?P<minimum>\d+)?(?:-(?P<maximum>\d+))?(?P<confidence>[HML])$")
DISPOSITIONS = {
    "R": "removable",
    "M": "mixed",
    "P": "protected",
    "N": "none",
    "U": "unsuitable",
    "A": "ambiguous",
}
CONFIDENCES = {"H": "high", "M": "medium", "L": "low"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--split", choices=("evaluation", "development"), default="evaluation")
    parser.add_argument("--manifest", type=Path, default=Path("tests/artifact-blindset/corpus-v22.json"))
    parser.add_argument(
        "--index",
        type=Path,
        default=None,
    )
    parser.add_argument(
        "--detector-report",
        type=Path,
        default=None,
    )
    parser.add_argument(
        "--fallback-detector-report",
        type=Path,
        help="Optional full-split report used for cases absent from a subset detector report.",
    )
    parser.add_argument(
        "--review-output",
        type=Path,
        default=None,
    )
    parser.add_argument(
        "--manifest-output",
        type=Path,
        default=None,
    )
    parser.add_argument(
        "--report-output",
        type=Path,
        default=None,
    )
    parser.add_argument(
        "--queue-output",
        type=Path,
        default=None,
    )
    arguments = parser.parse_args()
    suffix = "" if arguments.split == "evaluation" else "-development"
    arguments.index = arguments.index or Path(
        f"build/artifact-corpus/v22-ai-review-sheets{suffix}/index.json"
    )
    arguments.detector_report = arguments.detector_report or Path(
        "build/artifact-corpus/v22-first-run.json"
        if arguments.split == "evaluation"
        else "build/artifact-corpus/v22-development-baseline.json"
    )
    arguments.review_output = arguments.review_output or Path(
        f"tests/artifact-blindset/corpus-v22-ai-review{suffix}.json"
    )
    arguments.manifest_output = arguments.manifest_output or Path(
        f"tests/artifact-blindset/corpus-v22-ai-high-confidence{suffix}.json"
    )
    arguments.report_output = arguments.report_output or Path(
        f"tests/artifact-blindset/corpus-v22-ai-first-run-summary{suffix}.json"
    )
    arguments.queue_output = arguments.queue_output or Path(
        f"tests/artifact-blindset/corpus-v22-ai-manual-queue{suffix}.json"
    )
    return arguments


def decode(code: str) -> dict:
    match = CODE_PATTERN.fullmatch(code)
    if not match:
        raise ValueError(f"invalid review code: {code}")
    label = match.group("label")
    minimum_text = match.group("minimum")
    maximum_text = match.group("maximum")
    minimum = int(minimum_text) if minimum_text else None
    maximum = int(maximum_text) if maximum_text else minimum
    if label in {"R", "M"} and minimum is None:
        raise ValueError(f"positive review code needs a count: {code}")
    if label in {"N", "U", "A"} and minimum is not None:
        raise ValueError(f"review code must not have a count: {code}")
    return {
        "disposition": DISPOSITIONS[label],
        "confidence": CONFIDENCES[match.group("confidence")],
        "suitableForBenchmark": label not in {"U", "A"},
        "visibleRemovableMinimum": minimum if label in {"R", "M"} else 0,
        "visibleRemovableMaximum": maximum if label in {"R", "M"} else 0,
        "visibleProtectedMinimum": minimum if label == "P" and minimum is not None else None,
    }


def expectation_for(review: dict) -> dict:
    if review["disposition"] in {"removable", "mixed"}:
        # The detector may segment one visual trail into several pieces. Measure
        # missed removal here, while retaining the visual upper bound in review.
        return {"minimumArtifacts": review["visibleRemovableMinimum"]}
    return {"maximumArtifacts": 0}


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    corpus = json.loads(arguments.manifest.read_text(encoding="utf-8"))
    index = json.loads(arguments.index.read_text(encoding="utf-8"))["items"]
    detector = json.loads(arguments.detector_report.read_text(encoding="utf-8"))
    cases = {case["id"]: case for case in corpus["cases"] if case["split"] == arguments.split}
    results = {}
    if arguments.fallback_detector_report is not None:
        fallback = json.loads(arguments.fallback_detector_report.read_text(encoding="utf-8"))
        results.update({case["id"]: case for case in fallback["cases"]})
    results.update({case["id"]: case for case in detector["cases"]})
    sheet_decisions = (
        EVALUATION_SHEET_DECISIONS
        if arguments.split == "evaluation"
        else DEVELOPMENT_SHEET_DECISIONS
    )

    expected_sheets = Counter(item["sheet"] for item in index)
    if set(expected_sheets) != set(sheet_decisions):
        missing = sorted(set(expected_sheets) - set(sheet_decisions))
        extra = sorted(set(sheet_decisions) - set(expected_sheets))
        raise ValueError(f"sheet decision mismatch; missing={missing}, extra={extra}")
    for sheet, count in expected_sheets.items():
        actual = len(sheet_decisions[sheet].split())
        if actual != count:
            raise ValueError(f"{sheet}: expected {count} decisions, found {actual}")

    review_items = []
    high_confidence_cases = []
    queue_items = []
    relabeled_results = []
    by_sheet = {sheet: codes.split() for sheet, codes in sheet_decisions.items()}
    for item in index:
        case = cases[item["id"]]
        result = results[item["id"]]
        code = by_sheet[item["sheet"]][item["slot"] - 1]
        review = decode(code)
        annotated = {
            **item,
            "decisionCode": code,
            **review,
            "detectorTrails": result["trails"],
            "detectorProtectedMeteors": result["protectedMeteors"],
        }
        review_items.append(annotated)

        qualifies = review["confidence"] == "high" and review["suitableForBenchmark"]
        if qualifies:
            revised_case = dict(case)
            revised_case["expectation"] = expectation_for(review)
            revised_case["labelProvenance"] = "codex-ai-contact-sheet-visual-review"
            revised_case["aiReview"] = {
                "disposition": review["disposition"],
                "confidence": review["confidence"],
                "visibleRemovableMinimum": review["visibleRemovableMinimum"],
                "visibleRemovableMaximum": review["visibleRemovableMaximum"],
            }
            high_confidence_cases.append(revised_case)
            trails = result["trails"]
            expectation = revised_case["expectation"]
            minimum = expectation.get("minimumArtifacts")
            maximum = expectation.get("maximumArtifacts")
            passed = result["processed"] and (minimum is None or trails >= minimum) and (maximum is None or trails <= maximum)
            relabeled_results.append({
                "id": case["id"],
                "disposition": review["disposition"],
                "passed": passed,
                "trails": trails,
                "protectedMeteors": result["protectedMeteors"],
                "expectation": expectation,
            })
        else:
            queue_items.append(annotated)

    disposition_counts = Counter(item["disposition"] for item in review_items)
    confidence_counts = Counter(item["confidence"] for item in review_items)
    high_dispositions = Counter(case["aiReview"]["disposition"] for case in high_confidence_cases)
    passed = sum(item["passed"] for item in relabeled_results)
    failed = len(relabeled_results) - passed

    write_json(arguments.review_output, {
        "schemaVersion": 1,
        "reviewed": str(date.today()),
        "reviewer": "OpenAI Codex visual review",
        "sourceManifest": str(arguments.manifest),
        "sourceIndex": str(arguments.index),
        "split": arguments.split,
        "method": "Every evaluation image was inspected in a 1280 px contact-sheet tile; labels are an AI first pass, not human ground truth.",
        "decisionLegend": {
            "removable": "visible satellite/aircraft-like trail should be removable",
            "protected": "visible meteor or star trail should be preserved",
            "mixed": "protected content and removable trail coexist",
            "none": "no visible trail",
            "unsuitable": "diagram, illustration, composite, irrelevant scene, or otherwise unsuitable",
            "ambiguous": "visual evidence is insufficient for an evaluation label",
        },
        "summary": {
            "reviewed": len(review_items),
            "byDisposition": dict(sorted(disposition_counts.items())),
            "byConfidence": dict(sorted(confidence_counts.items())),
            "highConfidenceBenchmarkCases": len(high_confidence_cases),
            "manualQueueCases": len(queue_items),
        },
        "items": review_items,
    })
    write_json(arguments.manifest_output, {
        "schemaVersion": 1,
        "description": f"AI visually reviewed high-confidence subset of the frozen v22 {arguments.split} corpus.",
        "created": str(date.today()),
        "labelPolicy": {
            "reviewer": "OpenAI Codex visual review",
            "scope": "first-pass visual labels, not human ground truth",
            "positivePolicy": "minimum visible removable trails; no maximum because one trail may be segmented",
            "protectedPolicy": "maximumArtifacts 0 for high-confidence meteor/star-trail/no-trail images",
        },
        "cases": high_confidence_cases,
    })
    write_json(arguments.queue_output, {
        "schemaVersion": 1,
        "description": "v22 evaluation images requiring human review or exclusion.",
        "cases": queue_items,
    })
    write_json(arguments.report_output, {
        "schemaVersion": 1,
        "sourceDetectorReport": str(arguments.detector_report),
        "sourceAiManifest": str(arguments.manifest_output),
        "processed": len(relabeled_results),
        "passed": passed,
        "failed": failed,
        "passRate": passed / len(relabeled_results) if relabeled_results else None,
        "byDisposition": {
            disposition: {
                "cases": count,
                "passed": sum(item["passed"] for item in relabeled_results if item["disposition"] == disposition),
            }
            for disposition, count in sorted(high_dispositions.items())
        },
        "results": relabeled_results,
    })
    print(f"reviewed={len(review_items)} high_confidence={len(high_confidence_cases)} manual_queue={len(queue_items)}")
    print(f"AI-relabel pass={passed}/{len(relabeled_results)} ({passed / len(relabeled_results):.1%})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
