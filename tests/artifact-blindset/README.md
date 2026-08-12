# Artifact-trail generalization benchmark

This benchmark keeps third-party images out of git while making the exact test
bytes reproducible. Every network image has a source page, a fixed 1920-pixel
download URL, a photographer/source group, and a SHA-256 digest in
`manifest.json`.

The split is by photographer/source group rather than by random crops. This is
important: crops or adjacent frames from one photographer must never appear on
both sides of the split.

Build the CLI, download the fixtures, and run the development split:

```sh
cmake --build build/debug --target photonstack
python3 tools/scripts/evaluate-artifact-blindset.py --split development --download
```

Run the locked evaluation split without changing detector code afterward:

```sh
python3 tools/scripts/evaluate-artifact-blindset.py --split evaluation --download
```

To exercise every photo directly on the user's Desktop as additional
informational coverage:

```sh
python3 tools/scripts/evaluate-artifact-blindset.py \
  --split evaluation \
  --desktop-dir "$HOME/Desktop" \
  --require-desktop
```

Desktop images are not copied, hashed, committed, or treated as training data.
Their counts are reported but do not define pass/fail until reviewed annotations
exist. A network negative passes only when no removable artifact is returned. A
network positive must meet its declared minimum artifact count; some segmented
trails also carry a conservative maximum to catch broad star-trail removal.

The evaluation split is a regression set after its first run. A genuinely new
blind acceptance run requires a separate manifest whose images and labels were
not inspected while changing the detector.

## Independent blind rounds

The `rounds/` manifests preserve six later acceptance rounds. Run all of them
against their hash-locked downloads with:

```sh
for manifest in tests/artifact-blindset/rounds/*.json; do
  python3 tools/scripts/evaluate-artifact-blindset.py \
    --manifest "$manifest" --split evaluation --download
done
```

The honest first-run history is retained even though every case becomes an
ordinary regression after detector changes:

| Round | Frozen first run | Result after structural fixes |
| --- | ---: | ---: |
| v16 | 4/6 | 6/6 |
| v17 | 4/6 | 6/6 |
| v18 | 3/6 | 6/6 |
| v19 | 5/6 | 6/6 |
| v20 | 5/6 | 6/6 |
| v21 | 7/8 | 8/8 |

Across these untouched first runs the detector passed 28 of 38 cases (73.7%).
That number, rather than the post-fix 38/38 regression result, is the useful
warning against claiming that the heuristic is universally solved. The JSON
reports written under the ignored `build/artifact-blindset/` directory retain
per-case counts for the current checkout.

The machine-readable first-run record is `history.json`. After running all
rounds and the Desktop coverage pass, refresh the Chinese maintenance document
with:

```sh
python3 tools/scripts/generate-artifact-dataset-document.py
```

The benchmark currently checks declared artifact-count bounds. It does not yet
prove pixel-perfect coverage of every visible trail or prove that every extra
candidate is correct; reviewed masks would be required for that stronger claim.

## v22 large weak-label corpus

`corpus-v22.json` expands coverage to 500 hash-locked Wikimedia Commons images:

- 100 images from satellite-trail, satellite-flare, and aircraft-light-trail categories;
- 150 images from meteor-related categories;
- 250 images from star-trail categories;
- 300 frozen `evaluation` images for the first operational run and 200 held-back
  `development` images.

Every entry records its Commons file page, fixed 1280-pixel download URL,
license, author/credit metadata, original dimensions, Commons SHA-1, and the
downloaded bytes' SHA-256. Image bytes stay in the ignored build cache. Rebuild
the corpus only when intentionally starting a new version:

```sh
python3 tools/scripts/collect-artifact-corpus.py --catalog-only
python3 tools/scripts/collect-artifact-corpus.py --workers 1
```

The Commons categories are broad, category-maintained weak labels rather than
individual human annotations. For example, meteor categories can contain book
scans, city scenes, or composites, and a star-trail image can also contain a
satellite or aircraft. Therefore `weakLabelConforming` is a triage signal, not
an accuracy claim and not a safe threshold-tuning target.

The untouched first run used the installed macOS CLI and processed all 300
evaluation images without a runtime or decoding failure. It matched the weak
category expectation on 144/300 images. The frozen aggregate record is
`corpus-v22-first-run-summary.json`; the ignored detailed report is
`build/artifact-corpus/v22-first-run.json`.

```sh
python3 tools/scripts/evaluate-artifact-blindset.py \
  --manifest tests/artifact-blindset/corpus-v22.json \
  --binary /Applications/PhotonStack.app/Contents/MacOS/photonstack \
  --cache build/artifact-corpus/v22-special-1280 \
  --split evaluation --workers 4 --quiet \
  --json-report build/artifact-corpus/v22-current.json
```

Promote an image from this weak-label corpus into a strict blind round only
after a person reviews the visible trails and writes an image-specific count
bound or, preferably, a centerline/mask annotation.

### AI visual triage of all 500 images

All 300 evaluation and 200 development images were inspected tile by tile at the frozen
1280-pixel rendition. This AI first pass replaces the Commons category label
with one of `removable`, `protected`, `mixed`, `none`, `unsuitable`, or
`ambiguous`, and records confidence plus a visible removable-trail count range.
It is useful triage, but it is not human or pixel-level ground truth.

- 349 images are suitable and high confidence; 151 are medium/low confidence or
  unsuitable and remain in the manual-review queue.
- Against those 349 AI labels, the pre-change baseline was 172/349 and the
  current detector is 224/349.
- Removable/mixed scenes currently pass 47/68; protected/no-trail scenes pass
  177/281.
- All 500 images are now a regression/development set because their pixels and failures
  have been inspected. Do not tune on them and call the next run blind.

Generate the contact sheets and materialize the recorded decisions with:

```sh
python3 tools/scripts/generate-artifact-ai-review-sheets.py --split evaluation
python3 tools/scripts/generate-artifact-ai-review.py --split evaluation
python3 tools/scripts/generate-artifact-ai-review-sheets.py --split development
python3 tools/scripts/generate-artifact-ai-review.py --split development
```

The tracked evaluation and `-development` outputs contain all 500 decisions,
349 high-confidence cases, 151 queued cases, plus pre-change and current
summary reports. They do not modify the frozen image bytes.
