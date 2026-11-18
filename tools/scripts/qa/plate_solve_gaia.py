"""Offline absolute-astrometry check against cached Gaia DR3 VOTables.
Usage: python plate_solve_gaia.py solved.fits observation-date cache-dir output.json
Requires NumPy/Astropy. No query, image modification or residual clipping.
Observation date is explicit because some masters lack DATE-OBS.
"""
import hashlib
import json
import math
from pathlib import Path
import sys
import xml.etree.ElementTree as ET
import numpy as np
from astropy.io import fits
from astropy.time import Time
from astropy.wcs import WCS
from catalog_wcs_residuals import measure

def main(source,date,cache,output):
    if output.exists():raise ValueError('Choose a new report path')
    original=hashlib.sha256(source.read_bytes()).hexdigest()
    with fits.open(source) as f:
        pixels=f[0].data
        a=pixels if pixels.ndim==2 else .2126*pixels[0]+.7152*pixels[1]+.0722*pixels[2]
        if pixels.ndim==3 and pixels.shape[0]==4:a=np.where(pixels[3]>=.999999,a,np.nan)
        w=WCS(f[0].header,naxis=2)
    rows={};files={}
    for file in sorted(cache.glob('gaia-dr3-*.xml')):
        data=file.read_bytes();doc=ET.fromstring(data)
        if [i.get('value') for i in doc.findall('.//{*}INFO[@name="QUERY_STATUS"]')]!=['OK']:raise ValueError('Invalid catalog cache')
        files[str(file.resolve())]=hashlib.sha256(data).hexdigest()
        for row in doc.findall('.//{*}TR'):
            values=[s.text for s in row];rows[values[0]]=values
    stars=[];dt=Time(date).jyear-2016
    for sid,ra,dec,ma,md,mag in rows.values():
        if not ma or not md:continue
        ra=float(ra);dec=float(dec)
        stars.append((sid,ra+dt*float(ma)/(3.6e6*math.cos(math.radians(dec))),dec+dt*float(md)/3.6e6))
    if not stars:raise ValueError('No cached Gaia stars with proper motions')
    xy=w.all_world2pix([[ra,dec] for _,ra,dec in stars],0);matches=[]
    for i,(x,y) in enumerate(xy):
        if not(10<x<a.shape[1]-10 and 10<y<a.shape[0]-10):continue
        if np.count_nonzero(np.hypot(xy[:,0]-x,xy[:,1]-y)<8)!=1:continue
        c=measure(a,x,y)
        if c is not None:matches.append([stars[i][0],x,y,c[0]-x,c[1]-y])
    if not matches:raise ValueError('No measurable isolated Gaia candidates')
    z=np.array([r[3:] for r in matches]);distance=np.hypot(z[:,0],z[:,1])
    result={'source':str(source.resolve()),'sourceSHA':original,'observationDate':date,'catalogs':files,'count':len(matches),
        'medianPixels':float(np.median(distance)),'p95Pixels':float(np.percentile(distance,95)),'maximumPixels':float(distance.max()),
        'medianOffset':np.median(z,axis=0).tolist(),'matches':matches}
    assert hashlib.sha256(source.read_bytes()).hexdigest()==original
    with output.open('x') as f:json.dump(result,f,indent=2)
    print(json.dumps({k:v for k,v in result.items() if k not in ['matches','catalogs']},indent=2))
if __name__=='__main__':main(Path(sys.argv[1]),sys.argv[2],Path(sys.argv[3]),Path(sys.argv[4]))
