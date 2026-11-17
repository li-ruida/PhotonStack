"""Measure mosaic astrometry against magnitude-limited Gaia DR3 sources.

Usage: python catalog_wcs_residuals.py mosaic.fits YYYY-MM-DD cache_dir
Requires numpy and astropy. Only field centers are sent to the public TAP service.
The nine original VOTables are cached for reproducible offline remeasurement.
"""
import math
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlencode
import xml.etree.ElementTree as ET

import numpy as np
from astropy.io import fits
from astropy.time import Time
from astropy.wcs import WCS


def catalog(ra, dec, cache):
    radius = 0.12
    key = f"gaia-dr3-{ra:.6f}-{dec:.6f}-{radius}.xml"
    file = cache / key
    if not file.exists():
        query = ("SELECT TOP 400 source_id,ra,dec,pmra,pmdec,phot_g_mean_mag "
                 "FROM gaiadr3.gaia_source WHERE 1=CONTAINS(POINT('ICRS',ra,dec),"
                 f"CIRCLE('ICRS',{ra:.6f},{dec:.6f},{radius})) AND phot_g_mean_mag BETWEEN 10 AND 14")
        url = "https://gaia.aip.de/tap/sync?" + urlencode(dict(REQUEST="doQuery", LANG="ADQL", QUERY=query))
        data = subprocess.check_output(["curl", "--fail", "--silent", "--show-error", "--max-time", "40", url])
    else:
        data = file.read_bytes()
    doc = ET.fromstring(data)
    if [i.get("value") for i in doc.findall(".//{*}INFO[@name='QUERY_STATUS']")] != ["OK"]:
        raise ValueError(f"Incomplete TAP result for {key}")
    rows = [[s.text for s in row] for row in doc.findall(".//{*}TR")]
    if len(rows) >= 400:
        raise ValueError("Query truncated: pick a smaller radius")
    if not file.exists():
        file.write_bytes(data)
    return rows


def measure(a, x, y):
    ix, iy = int(round(x)), int(round(y))
    cut = a[iy-8:iy+9, ix-8:ix+9].astype(float)
    if cut.shape != (17, 17) or not np.isfinite(cut).all():
        return None
    sky = np.median(cut)
    mad = 1.4826*np.median(np.abs(cut-sky))
    sy, sx = np.unravel_index(cut[4:13, 4:13].argmax(), (9, 9))
    py, px = sy+4, sx+4
    if cut[py, px]-sky < max(1e-5, 8*mad):
        return None
    flux = np.maximum(cut[py-3:py+4, px-3:px+4]-sky, 0)
    yy, xx = np.indices((7, 7))
    return (ix-8+px-3+(flux*xx).sum()/flux.sum(), iy-8+py-3+(flux*yy).sum()/flux.sum())


def main(path, date, cache):
    cache.mkdir(parents=True, exist_ok=True)
    with fits.open(path) as f:
        image = f[0].data
        a = image[0] if image.ndim == 3 else image
        wcs = WCS(f[0].header, naxis=2)
        h, w = a.shape
        fields = [(x*w, y*h) for y in (.10, .5, .90) for x in (.10, .5, .90)]
        world = wcs.all_pix2world(fields, 0)
        with ThreadPoolExecutor(max_workers=3) as pool:
            catalogs = list(pool.map(lambda center: catalog(*center, cache), world))
        dt = Time(date).jyear-2016.0
        summary = []
        for n, rows in enumerate(catalogs):
            eligible = []
            for sid, ra, dec, ma, md, mag in rows:
                if ma is None or md is None or not ma.strip() or not md.strip():
                    continue
                ra, dec = float(ra), float(dec)
                ra += dt*float(ma)/(3.6e6*math.cos(math.radians(dec)))
                dec += dt*float(md)/3.6e6
                eligible.append((int(sid), ra, dec))
            if not eligible:
                raise ValueError(f"No catalog candidates in field {n+1}")
            xy = wcs.all_world2pix([[r,d] for _,r,d in eligible],0)
            offsets = []
            for i,(x,y) in enumerate(xy):
                if not (10 < x < w-10 and 10 < y < h-10):
                    continue
                # Remove unresolved catalog pairs rather than counting their shared peak twice.
                if np.count_nonzero(np.hypot(xy[:,0]-x,xy[:,1]-y)<8) != 1:
                    continue
                c = measure(a,x,y)
                if c is not None:
                    offsets.append((c[0]-x,c[1]-y))
            if len(offsets)<6:
                raise ValueError(f"Field {n+1}: only {len(offsets)} usable stars")
            dx,dy = np.array(offsets).T
            r = np.hypot(dx,dy)
            summary.extend(r)
            print(f"field={n+1} catalog={len(rows)} measured={len(r)} median={np.median(r):.3f} "
                  f"p95={np.percentile(r,95):.3f} dx={np.median(dx):.3f} dy={np.median(dy):.3f} px")
        print(f"total={len(summary)} median={np.median(summary):.3f} p95={np.percentile(summary,95):.3f} px")


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))
