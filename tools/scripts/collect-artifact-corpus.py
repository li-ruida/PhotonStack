#!/usr/bin/env python3
"""Freeze a large, license-recorded Wikimedia Commons artifact corpus."""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


API_URL = "https://commons.wikimedia.org/w/api.php"
USER_AGENT = (
    "PhotonStackArtifactBenchmark/2.0 "
    "(https://github.com/li-ruida/PhotonStack; Wikimedia Commons reproducibility corpus)"
)
THUMBNAIL_WIDTH = 1280
SOURCE_SPECS = (
    {
        "id": "satellite",
        "label": "removable-artifact",
        "categories": ("Satellite trails", "Satellite flares"),
        "evaluation": 40,
        "development": 25,
        "expectation": {"minimumArtifacts": 1},
    },
    {
        "id": "aircraft",
        "label": "removable-artifact",
        "categories": ("Aircraft light trails",),
        "evaluation": 20,
        "development": 15,
        "expectation": {"minimumArtifacts": 1},
    },
    {
        "id": "meteor",
        "label": "protected-meteor",
        "categories": ("Meteor trails", "Meteors", "Meteor showers"),
        "evaluation": 90,
        "development": 60,
        "expectation": {"maximumArtifacts": 0},
    },
    {
        "id": "startrail",
        "label": "protected-star-trail",
        "categories": ("Star trails", "Circumpolar stars in star trails"),
        "evaluation": 150,
        "development": 100,
        "expectation": {"maximumArtifacts": 0},
    },
)
ALLOWED_MIME_TYPES = {"image/jpeg": ".jpg", "image/png": ".png"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22.json"),
    )
    parser.add_argument(
        "--cache",
        type=Path,
        default=Path("build/artifact-corpus/v22-special-1280"),
    )
    parser.add_argument(
        "--catalog-cache",
        type=Path,
        default=Path("build/artifact-corpus/v22-1280/catalog.json"),
    )
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--catalog-only", action="store_true")
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def api_query(parameters: dict[str, str]) -> dict:
    query = urllib.parse.urlencode({"format": "json", "formatversion": "2", **parameters})
    request = urllib.request.Request(f"{API_URL}?{query}", headers={"User-Agent": USER_AGENT})
    for attempt in range(5):
        try:
            time.sleep(2.5)
            with urllib.request.urlopen(request, timeout=90) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if error.code not in {429, 500, 502, 503, 504} or attempt == 4:
                raise
            retry_after = error.headers.get("Retry-After")
            delay = min(30.0, float(retry_after) if retry_after and retry_after.isdigit() else 2.0 ** (attempt + 2))
            print(f"API rate limited; retrying in {delay:g}s", file=sys.stderr)
            time.sleep(delay)
    raise RuntimeError("unreachable API retry state")


def clean_metadata(value: object) -> str:
    if not isinstance(value, dict):
        return ""
    raw = html.unescape(str(value.get("value", "")))
    return re.sub(r"<[^>]+>", "", raw).strip()


def collect_category(category: str) -> list[dict]:
    continuation: dict[str, str] = {}
    collected: list[dict] = []
    while True:
        response = api_query({
            "action": "query",
            "generator": "categorymembers",
            "gcmtitle": f"Category:{category}",
            "gcmtype": "file",
            "gcmlimit": "max",
            "prop": "imageinfo",
            "iiprop": "url|mime|size|sha1|extmetadata",
            "iiurlwidth": str(THUMBNAIL_WIDTH),
            **continuation,
        })
        for page in response.get("query", {}).get("pages", []):
            infos = page.get("imageinfo", [])
            if not infos:
                continue
            info = infos[0]
            mime = info.get("mime")
            if mime not in ALLOWED_MIME_TYPES:
                continue
            width = info.get("width")
            height = info.get("height")
            if not isinstance(width, int) or not isinstance(height, int) or min(width, height) < 600:
                continue
            download_url = info.get("thumburl") or info.get("url")
            if not isinstance(download_url, str) or not download_url.startswith("https://"):
                continue
            metadata = info.get("extmetadata", {})
            title = page.get("title", "")
            if not title.startswith("File:"):
                continue
            collected.append({
                "pageId": page["pageid"],
                "title": title,
                "category": category,
                "mime": mime,
                "originalWidth": width,
                "originalHeight": height,
                "commonsSha1": info.get("sha1", ""),
                "downloadUrl": download_url,
                "sourcePage": "https://commons.wikimedia.org/wiki/" + urllib.parse.quote(
                    title.replace(" ", "_"), safe=":_(),'%!"
                ),
                "license": clean_metadata(metadata.get("LicenseShortName")),
                "licenseUrl": clean_metadata(metadata.get("LicenseUrl")),
                "artist": clean_metadata(metadata.get("Artist")),
                "credit": clean_metadata(metadata.get("Credit")),
            })
        if "continue" not in response:
            break
        continuation = {
            key: str(value)
            for key, value in response["continue"].items()
            if key != "continue"
        }
    return collected


def candidate_rank(source_id: str, candidate: dict) -> str:
    value = f"photonstack-v22|{source_id}|{candidate['pageId']}|{candidate['title']}"
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def stable_download_url(title: str) -> str:
    filename = title.removeprefix("File:")
    return (
        "https://commons.wikimedia.org/wiki/Special:Redirect/file/"
        + urllib.parse.quote(filename, safe="")
        + f"?width={THUMBNAIL_WIDTH}"
    )


def download_candidate(candidate: dict, destination: Path) -> tuple[dict, Path, str] | None:
    if destination.is_file() and destination.stat().st_size >= 4096:
        digest = hashlib.sha256()
        with destination.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return candidate, destination, digest.hexdigest()
    temporary = destination.with_suffix(destination.suffix + ".partial")
    try:
        time.sleep(1.25)
        completed = subprocess.run(
            [
                "curl", "-fsSL", "--retry", "5", "--retry-all-errors", "--retry-delay", "2",
                "--max-time", "180", "--user-agent", USER_AGENT,
                "--output", str(temporary), candidate["downloadUrl"],
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr.strip() or f"curl exited {completed.returncode}")
        total = temporary.stat().st_size
        if total > 25 * 1024 * 1024:
            raise RuntimeError("thumbnail exceeds 25 MiB")
        if total < 4096:
            raise RuntimeError("thumbnail is unexpectedly small")
        digest_builder = hashlib.sha256()
        with temporary.open("rb") as handle:
            signature = handle.read(8)
            digest_builder.update(signature)
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest_builder.update(chunk)
        if candidate["mime"] == "image/jpeg" and not signature.startswith(b"\xff\xd8\xff"):
            raise RuntimeError("download is not a JPEG")
        if candidate["mime"] == "image/png" and signature != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError("download is not a PNG")
        digest = digest_builder.hexdigest()
        temporary.replace(destination)
        return candidate, destination, digest
    except (OSError, RuntimeError) as error:
        print(f"skip {candidate['title']}: {error}", file=sys.stderr)
        return None
    finally:
        if temporary.exists():
            temporary.unlink()


def freeze_source(
    spec: dict,
    candidates: list[dict],
    cache: Path,
    workers: int,
    global_hashes: set[str],
) -> list[dict]:
    target = spec["evaluation"] + spec["development"]
    ranked = sorted(candidates, key=lambda item: candidate_rank(spec["id"], item))
    selected: list[dict] = []
    attempted = 0
    while len(selected) < target and attempted < len(ranked):
        remaining = target - len(selected)
        batch = ranked[attempted : attempted + remaining + max(12, workers * 2)]
        attempted += len(batch)
        jobs = []
        for candidate in batch:
            candidate = {**candidate, "downloadUrl": stable_download_url(candidate["title"])}
            extension = ALLOWED_MIME_TYPES[candidate["mime"]]
            filename = f"v22-{spec['id']}-{candidate['pageId']}{extension}"
            jobs.append((candidate, cache / filename))
        with ThreadPoolExecutor(max_workers=workers) as executor:
            outcomes = list(executor.map(lambda job: download_candidate(*job), jobs))
        for outcome in outcomes:
            if outcome is None or len(selected) >= target:
                continue
            candidate, path, digest = outcome
            if digest in global_hashes:
                path.unlink(missing_ok=True)
                continue
            global_hashes.add(digest)
            split = "evaluation" if len(selected) < spec["evaluation"] else "development"
            selected.append({
                "id": f"v22-{spec['id']}-{candidate['pageId']}",
                "split": split,
                "group": f"commons-page-{candidate['pageId']}",
                "filename": path.name,
                "downloadUrl": candidate["downloadUrl"],
                "sourcePage": candidate["sourcePage"],
                "sha256": digest,
                "expectation": spec["expectation"],
                "categoryLabel": spec["label"],
                "labelProvenance": "wikimedia-commons-category-weak-label",
                "sourceCategory": candidate["category"],
                "commonsPageId": candidate["pageId"],
                "commonsTitle": candidate["title"],
                "commonsSha1": candidate["commonsSha1"],
                "license": candidate["license"],
                "licenseUrl": candidate["licenseUrl"],
                "artist": candidate["artist"],
                "credit": candidate["credit"],
                "originalWidth": candidate["originalWidth"],
                "originalHeight": candidate["originalHeight"],
            })
        print(f"{spec['id']}: froze {len(selected)}/{target}")
    if len(selected) != target:
        raise RuntimeError(f"{spec['id']}: only froze {len(selected)} of {target} required images")
    return selected


def main() -> int:
    arguments = parse_arguments()
    if arguments.workers < 1 or arguments.workers > 12:
        raise SystemExit("--workers must be between 1 and 12")
    if arguments.output.exists() and not arguments.force and not arguments.catalog_only:
        raise SystemExit(f"refusing to overwrite frozen manifest: {arguments.output}; use --force")
    arguments.cache.mkdir(parents=True, exist_ok=True)

    category_cache: dict[str, list[dict]] = {}
    if arguments.catalog_cache.is_file():
        category_cache = json.loads(arguments.catalog_cache.read_text(encoding="utf-8"))
        print(f"loaded catalog cache with {len(category_cache)} categories")
    category_names = [
        category
        for spec in SOURCE_SPECS
        for category in spec["categories"]
    ]
    for category in category_names:
        if category in category_cache:
            continue
        category_cache[category] = collect_category(category)
        arguments.catalog_cache.parent.mkdir(parents=True, exist_ok=True)
        arguments.catalog_cache.write_text(
            json.dumps(category_cache, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"cataloged {category}: {len(category_cache[category])} usable files")
    if arguments.catalog_only:
        print(f"catalog ready: {arguments.catalog_cache}")
        return 0

    all_cases: list[dict] = []
    seen_pages: set[int] = set()
    content_hashes: set[str] = set()
    for spec in SOURCE_SPECS:
        candidates: list[dict] = []
        for category in spec["categories"]:
            print(f"category {category}: {len(category_cache[category])} usable files")
            for candidate in category_cache[category]:
                if candidate["pageId"] not in seen_pages:
                    candidates.append(candidate)
                    seen_pages.add(candidate["pageId"])
        cases = freeze_source(spec, candidates, arguments.cache, arguments.workers, content_hashes)
        all_cases.extend(cases)

    source_path = Path("engine/src/ArtifactTrailRemover.cpp")
    detector_digest = hashlib.sha256(source_path.read_bytes()).hexdigest()
    document = {
        "schemaVersion": 1,
        "description": (
            "PhotonStack v22 500-image Wikimedia Commons corpus. Labels are category-derived weak "
            "labels; 300 evaluation images are frozen for the first run and 200 development images "
            "are held in reserve. Image bytes remain in the ignored build cache."
        ),
        "created": "2026-08-04",
        "thumbnailWidth": THUMBNAIL_WIDTH,
        "detectorSourceSha256BeforeFirstRun": detector_digest,
        "labelPolicy": "category-derived weak labels; not pixel masks or individual human review",
        "cases": all_cases,
    }
    if len(all_cases) != 500:
        raise RuntimeError(f"expected 500 cases, produced {len(all_cases)}")
    if sum(case["split"] == "evaluation" for case in all_cases) != 300:
        raise RuntimeError("expected exactly 300 evaluation cases")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {arguments.output}: 500 images, 300 evaluation, 200 development")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
