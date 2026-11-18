"""Independent Astropy/WCSLIB plate solve audit; source and outputs read-only.
Usage: python plate_solve_validation.py result_directory
Checks exact original payload/metadata, predicted pixels, held-out exclusion,
residual summaries, and all source/index hashes. No remote requests.
"""
import hashlib
import json
from pathlib import Path
import sys
import shlex
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS

def main(directory):
    r=json.loads((directory/'report.json').read_text());source=Path(r['source'])
    records=(directory/'recipe.txt').read_text().splitlines()
    count=int(records[3].split()[-1])
    for record in records[1:3]+records[4:4+count]:
        path,sha=shlex.split(record)
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==sha, path
    for record in records[4+count:]:
        name,sha=shlex.split(record)
        assert hashlib.sha256((directory/name).read_bytes()).hexdigest()==sha, name
    assert hashlib.sha256(source.read_bytes()).hexdigest()==r['sourceSHA']
    with fits.open(source) as a,fits.open(directory/'solved.fits') as b:
        # Lossless header replacement includes all extension HDUs, not merely
        # equality after FITS scaling or conversion to float.
        ao=a[0].fileinfo()['datLoc'];bo=b[0].fileinfo()['datLoc']
        assert source.read_bytes()[ao:]==(directory/'solved.fits').read_bytes()[bo:]
        assert np.array_equal(a[0].data,b[0].data,equal_nan=True)
        for key in ['EXPTIME','DATE-OBS','FILTER','BSCALE','BZERO','BAYERPAT','GAIN','OFFSET','INSTRUME','PSCOLOR']:
            assert a[0].header.get(key)==b[0].header.get(key),key
        w=WCS(b[0].header,naxis=2)
        assert w.wcs.radesys=='ICRS'
    # Compare against the raw external solution after resolving its fixed
    # EQUINOX card according to the user's explicit ICRS index declaration.
    raw=fits.getheader(directory/'solution.wcs');raw.pop('EQUINOX',None);raw['RADESYS']='ICRS'
    ext=WCS(raw,naxis=2)
    xx,yy=np.meshgrid(np.linspace(0,r['width']-1,33),np.linspace(0,r['height']-1,33))
    p=np.c_[xx.ravel(),yy.ravel()]
    assert np.max(np.abs(w.all_pix2world(p,0)-ext.all_pix2world(p,0)))<1e-10
    stars=r['stars'];submitted=fits.getdata(directory/'sources.xyls')
    train=np.array([[s['x']+1,s['y']+1] for s in stars if not s['heldOut']])
    assert np.array_equal(train,np.c_[submitted['X'],submitted['Y']])
    assert len(train)+sum(s['heldOut'] for s in stars)==len(stars)
    corr=fits.getdata(directory/'solution.corr');corr=corr[corr['match_weight']>=r['minimumFitMatchWeight']]
    xy=w.all_world2pix(np.c_[corr['index_ra'],corr['index_dec']],0)
    fit_residual=np.hypot(xy[:,0]+1-corr['field_x'],xy[:,1]+1-corr['field_y'])
    groups={False:[],True:[]}
    for v in r['residuals']:
        s=stars[v['source']];d=np.hypot(s['x']-v['predictedX'],s['y']-v['predictedY'])
        assert abs(d-v['distance'])<1e-10
        groups[s['heldOut']].append(d)
    assert np.max(np.abs(np.sort(groups[False])-np.sort(fit_residual)))<3e-5
    index=fits.getdata(directory/'solution-indx.xyls')
    for v in r['residuals']:
        if not stars[v['source']]['heldOut']:continue
        assert np.min(np.hypot(index['X']-1-v['predictedX'],index['Y']-1-v['predictedY']))<1e-10
    for key,held in [('fitted',False),('validation',True)]:
        values=groups[held];summary=r[key]
        assert len(values)==summary['count']
        assert np.allclose(np.percentile(values,[50,95,100]),[summary['medianPixels'],summary['p95Pixels'],summary['maximumPixels']],rtol=0,atol=1e-10)
    print(json.dumps({k:r[k] for k in ['source','width','height','centerRA','centerDec','scaleArcsecPerPixel','fitted','validation','elapsedSeconds']},indent=2))
    print('Exact source payload, metadata, held-out separation, WCSLIB coordinates and residuals verified.')
if __name__=='__main__':main(Path(sys.argv[1]))
