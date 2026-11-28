#!/usr/bin/env python3
"""Compare completed CPU/Metal benchmark products and actual stage backends.
Requires NumPy, SciPy and Astropy. Reports differences without declaring a
universal scientific tolerance, cold-cache measurement, or speed guarantee.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from astropy.io import fits
from scipy.ndimage import maximum_filter


def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1048576),b''):h.update(block)
    return h.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('cpu','metal','output','sources'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    summaries=[json.loads((p/'summary.json').read_text()) for p in (args.cpu,args.metal)]
    assert all(s['exit_code']==0 for s in summaries)
    assert summaries[0]['cli_sha256']==summaries[1]['cli_sha256'] and summaries[0]['recipe_sha256']==summaries[1]['recipe_sha256']
    records=json.loads(args.sources.read_text())
    assert all(digest(Path(r['path']))==r['sha'] for r in records)
    reports=[json.loads((p/'result/report.json').read_text()) for p in (args.cpu,args.metal)]
    assert [[f['input'] for f in r['frames']] for r in reports]==[[f['path'] for f in records]]*2
    result={'originalsUnchanged':True,'inputCount':len(records),'sourceManifestSHA':digest(args.sources),
            'selectionIdentical':[(f['selected'],f['reference']) for f in reports[0]['frames']]==[(f['selected'],f['reference']) for f in reports[1]['frames']],
            'registrationTransformsIdentical':[f['registrationTransform'] for f in reports[0]['frames']]==[f['registrationTransform'] for f in reports[1]['frames']],
            'inputFwhmMaximumDifference':max(abs(a['fwhm']-b['fwhm']) for a,b in zip(reports[0]['frames'],reports[1]['frames'])),
            'runs':[],'products':{}}
    for name,s in zip(('cpu','metal'),summaries):
        result['runs'].append({'name':name,'elapsedSeconds':s['elapsed_seconds'],'resources':s['resources'],
                              'actualStages':s['performance']['stages'],'temporaryWorkCleaned':s['temporary_work_cleaned']})
    for name in summaries[0]['outputs'].keys() & summaries[1]['outputs'].keys():
        paths=[p/'result'/name for p in (args.cpu,args.metal)]
        record={'equalSHA':digest(paths[0])==digest(paths[1])}
        if name.endswith('.fits'):
            a,b=[fits.getdata(p).astype(np.float64) for p in paths]
            assert a.shape==b.shape
            valid=np.isfinite(a)&np.isfinite(b);d=(a-b)[valid]
            record.update(shape=list(a.shape),finiteMaskIdentical=bool(np.array_equal(np.isfinite(a),np.isfinite(b))),
                          maximumAbsolute=float(np.max(abs(d))),rms=float(np.sqrt(np.mean(d*d))),
                          changedSamples=int(np.count_nonzero(d)),comparedSamples=int(d.size))
            if a.ndim==3 and a.shape[0]==4:record['coverageIdentical']=bool(np.array_equal(a[3],b[3]))
        result['products'][name]=record
    a,b=[fits.getdata(p/'result/stack-linear.fits').astype(np.float64) for p in (args.cpu,args.metal)]
    valid=(a[3]>0)&(b[3]>0)&np.isfinite(a[:3]).all(axis=0)&np.isfinite(b[:3]).all(axis=0)
    x,y=[np.einsum('c,cyx->yx',[.2126,.7152,.0722],v[:3]) for v in (a,b)]
    sky=np.median(x[valid]);noise=1.4826*np.median(abs(x[valid]-sky))
    peaks=valid&(x==maximum_filter(x,size=21))&(x>sky+10*noise)&(x<50000)
    yy,xx=np.mgrid[-9:10,-9:10];radius=np.hypot(xx,yy);ap=radius<=4;ann=(radius>=6)&(radius<=9)
    def measure(image,cy,cx):
        roi=image[cy-9:cy+10,cx-9:cx+10];signal=roi-np.median(roi[ann]);flux=signal[ap].sum()
        mass=np.maximum(signal,0)*ap;total=mass.sum()
        if flux<=0 or total<=0:return None
        dx=(mass*xx).sum()/total;dy=(mass*yy).sum()/total
        width=np.sqrt((mass*((xx-dx)**2+(yy-dy)**2)).sum()/total/2)*2.35482
        return np.array([flux,dx,dy,width])
    stars=[]
    for cy,cx in zip(*np.nonzero(peaks)):
        if cy<10 or cx<10 or cy+10>=x.shape[0] or cx+10>=x.shape[1] or not valid[cy-9:cy+10,cx-9:cx+10].all():continue
        m,n=measure(x,cy,cx),measure(y,cy,cx)
        if m is None or n is None or not 1<m[3]<6:continue
        stars.append([n[0]/m[0],n[3]/m[3],np.hypot(n[1]-m[1],n[2]-m[2])])
    if stars:
        v=np.array(stars);result['unsaturatedCandidateStars']={'count':len(stars),'fluxRatioMedian':float(np.median(v[:,0])),
            'fluxRatioP01P99':np.percentile(v[:,0],[1,99]).tolist(),'widthRatioMedian':float(np.median(v[:,1])),
            'centroidDistanceP95':float(np.percentile(v[:,2],95)),'apertureRadius':4,'backgroundAnnulus':[6,9]}
    low=valid&(x<np.percentile(x[valid],20))
    result['lowBrightness']={'pixels':int(low.sum()),'differenceMedian':float(np.median((y-x)[low])),'differenceRMS':float(np.sqrt(np.mean((y-x)[low]**2)))}
    result['scope']='Fixed input subset, full deep-sky flow. OS cache state unknown; other apps may be active. Not full-product performance acceptance.'
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='runs'},indent=2))


if __name__=='__main__':main()
