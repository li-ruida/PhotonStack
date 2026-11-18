#!/usr/bin/env python3
"""Independently verify a saved Gaia snapshot with NumPy/Astropy; inputs stay read-only."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shlex
import warnings
import xml.etree.ElementTree as ET

import numpy as np
from astropy.coordinates import SkyCoord
from astropy.io import fits
from astropy.time import Time
from astropy.wcs import WCS
import astropy.units as u


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--directory", type=Path, required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    if args.output.exists():
        raise ValueError("Choose a new output file")
    root = args.directory
    originals = {file: sha(file) for file in [args.source, *root.iterdir()] if file.is_file()}
    m = json.loads((root / "query.json").read_text())
    assert m["schema"] == 1 and m["field"]["sha"] == sha(args.source)
    for file in m["files"]:
        assert file["name"] == Path(file["name"]).name
        assert sha(root / file["name"]) == file["sha"]
    acquisition = "\n".join(f'{f["name"]}\n{f["url"]}\n{f["sha"]}' for f in m["files"])
    assert hashlib.sha256(acquisition.encode()).hexdigest() == m["acquisitionSHA"]
    assert sha(root / "catalog.txt") == m["catalogSHA"]
    xml = ET.parse(root / "gaia.xml").getroot()
    assert [n.attrib["value"] for n in xml.iter() if n.tag.endswith("INFO") and n.attrib.get("name") == "QUERY_STATUS"] == ["OK"]
    table = next(n for n in xml.iter() if n.tag.rsplit("}", 1)[-1] == "TABLE")
    columns = [n.attrib["name"] for n in table if n.tag.endswith("FIELD")]
    raw = [dict(zip(columns, [(c.text or "").strip() for c in row])) for row in table.iter() if row.tag.endswith("TR")]
    chosen = list(csv.DictReader((root / "gaia.csv").open()))
    assert raw[:m["query"]["maximumStars"]] == chosen
    candidates = {s["source_id"]: s for s in chosen}
    assert len(candidates) == len(chosen)
    spectra = {}
    for file in m["files"][1:]:
        for row in csv.DictReader((root / file["name"]).open()):
            sid = row["source_id"]
            assert sid in candidates and sid not in spectra
            c = candidates[sid]
            np.testing.assert_allclose([float(row["ra"]), float(row["dec"])], [float(c["ra"]), float(c["dec"])], rtol=0, atol=1e-6)
            values = [np.array([float(x) for x in row[k].strip("()").split(",")]) for k in ["flux", "flux_error"]]
            assert all(len(v) == 343 and np.isfinite(v).all() for v in values)
            assert (values[1] >= 0).all()
            spectra[sid] = np.stack(values, axis=1)
    tokens = iter(shlex.split((root / "catalog.txt").read_text()))
    assert next(tokens) == "PhotonStack-SpectralCatalog-1"
    name, epoch, provenance, license_text = [next(tokens) for _ in range(4)]
    epoch = float(epoch)
    observation = Time(m["query"]["observationUTC"].removesuffix("Z"), scale="utc")
    assert abs(epoch - observation.jyear) < 1e-10
    count = int(next(tokens))
    assert count == m["count"] == len(spectra)
    ids, positions = [], []
    negative_samples = 0
    for _ in range(count):
        sid = next(tokens)
        ids.append(sid)
        positions.append([float(next(tokens)), float(next(tokens))])
        assert int(next(tokens)) == 343
        values = np.array([[float(next(tokens)) for _ in range(3)] for _ in range(343)])
        np.testing.assert_array_equal(values[:, 0], np.arange(336, 1021, 2))
        np.testing.assert_array_equal(values[:, 1:], spectra[sid])
        negative_samples += int((values[:, 1] < 0).sum())
    assert list(tokens) == [] and len(set(ids)) == count and set(ids) == set(spectra)
    assert set(candidates) == set(ids) | {s["id"] for s in m["skipped"]}
    coords = {k: np.array([float(candidates[sid][k]) for sid in ids]) for k in ["ra", "dec", "pmra", "pmdec"]}
    with warnings.catch_warnings(record=True):
        propagated = SkyCoord(ra=coords["ra"]*u.deg, dec=coords["dec"]*u.deg,
                              pm_ra_cosdec=coords["pmra"]*u.mas/u.yr, pm_dec=coords["pmdec"]*u.mas/u.yr,
                              obstime=Time(2016, format="jyear"), frame="icrs").apply_space_motion(new_obstime=Time(epoch, format="jyear"))
    actual = SkyCoord(ra=np.array(positions)[:, 0]*u.deg, dec=np.array(positions)[:, 1]*u.deg)
    motion_error = float(np.max(propagated.separation(actual).to_value(u.arcsec)))
    assert motion_error < 0.001
    header = fits.getheader(args.source)
    wcs = WCS(header, naxis=2)
    x, y = np.meshgrid(np.linspace(-.5, header["NAXIS1"]-.5, 65), np.linspace(-.5, header["NAXIS2"]-.5, 65))
    sky = wcs.pixel_to_world(x, y)
    center = SkyCoord(m["field"]["ra"]*u.deg, m["field"]["dec"]*u.deg)
    maximum = float(np.max(sky.separation(center).deg))
    assert maximum <= m["field"]["radius"]
    assert all(sha(file) == digest for file, digest in originals.items())
    result = dict(count=count, skipped=len(m["skipped"]), epoch=epoch,
                  negativeFluxSamplesPreserved=negative_samples, properMotionMaximumErrorArcsec=motion_error,
                  fieldMeshMaximumSeparationDeg=maximum, fieldBoundingRadiusDeg=m["field"]["radius"],
                  allRawSpectralSamplesEqual=True, allInputHashesUnchanged=True)
    with args.output.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
