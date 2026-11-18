#!/usr/bin/env python3
"""Read-only real master with an injected affine/exposure difference and contamination.
Requires NumPy and Astropy. Writes only to a new output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(reference,target,directory,report):
    ref=fits.getdata(reference).astype(np.float64);src=fits.getdata(target).astype(np.float64)
    actual=fits.getdata(directory/'matched.fits');valid=fits.getdata(directory/'validity.fits');res=fits.getdata(directory/'residual.fits')
    expected=(report['gain']*src+report['offset']).astype(np.float32)
    np.testing.assert_array_equal(actual,expected)
    np.testing.assert_array_equal(valid,np.isfinite(expected).astype(np.float32))
    expected_res=(expected.astype(np.float64)-ref).astype(np.float32)
    np.testing.assert_array_equal(res,expected_res)
    # Independent selection/statistics oracle. C++ uses interpolated rank quantiles;
    # NumPy's default quantile independently supplies the same definition.
    h,w=ref.shape;nx=min(w,256);ny=min(h,256);iy,ix=np.mgrid[:ny,:nx]
    hashes=(((ix.astype(np.uint64)+1)*73856093)^((iy.astype(np.uint64)+1)*19349663))&0xffffffff
    x0=ix*w//nx;x1=(ix+1)*w//nx;y0=iy*h//ny;y1=(iy+1)*h//ny
    xx=(x0+hashes%(x1-x0)).astype(int);yy=(y0+(hashes>>16)%(y1-y0)).astype(int)
    a=src[yy,xx];b=ref[yy,xx];finite=np.isfinite(a)&np.isfinite(b)
    lo,hi=np.quantile(a[finite],[report['low'],report['high']]);rlo,rhi=np.quantile(b[finite],[report['low'],report['high']])
    selected=finite&(a>=lo)&(a<=hi)&(b>=rlo)&(b<=rhi)
    bx=xx*8//w;by=yy*8//h;check=selected&((bx+by)%2==1)
    assert selected.sum()-check.sum()==report['fitSamples'] and check.sum()==report['checkSamples']
    before=a-b;after=report['gain']*a+report['offset']-b
    oracle={'beforeRms':np.sqrt(np.mean(before[check]**2)),'afterRms':np.sqrt(np.mean(after[check]**2)),
            'medianResidual':np.median(after[check]),'p95AbsoluteResidual':np.quantile(np.abs(after[check]),.95)}
    for k,v in oracle.items():np.testing.assert_allclose(report[k],v,rtol=1e-12,atol=1e-9)
    region=(by//2)*4+bx//2
    for i,r in enumerate(report['regions']):
        mask=check&(region==i);assert mask.sum()==r['count']
        if mask.any():
            for k,v in [('beforeMean',np.mean(before[mask])),('afterMean',np.mean(after[mask])),('afterRms',np.sqrt(np.mean(after[mask]**2)))]:
                np.testing.assert_allclose(r[k],v,rtol=1e-12,atol=1e-9)
    header=fits.getheader(target)
    if 'CTYPE1' in header:
        points=np.array([[x,y] for y in np.linspace(0,h-1,9) for x in np.linspace(0,w-1,9)])
        np.testing.assert_array_equal(WCS(header).all_pix2world(points,0),WCS(fits.getheader(directory/'matched.fits')).all_pix2world(points,0))
    return {k:float(v) for k,v in oracle.items()}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cli',type=Path,required=True);p.add_argument('--input',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False);before=sha(args.input)
    data=fits.getdata(args.input).astype(np.float64);header=fits.getheader(args.input)
    assert data.ndim==2
    rng=np.random.default_rng(835);gain=1.63;offset=-41.7
    # Noise is in the target. Conditional robust regression is a conditional y-on-x estimator; it is
    # not a symmetric errors-in-variables or photon-response calibration.
    for name,noise,contamination in [('exact',0,0),('noisy',.2,.03)]:
        target=(data-offset)/gain+rng.normal(0,noise,data.shape)
        contaminated=(rng.random(data.shape)<contamination)&np.isfinite(target)
        target[contaminated]+=rng.uniform(100,500,contaminated.sum())
        source=args.output/(name+'.fits');fits.PrimaryHDU(target.astype(np.float32),header).writeto(source)
        output=args.output/(name+'-result')
        report=json.loads(subprocess.check_output([str(args.cli),'channel-match','--aligned','--reference',f'Ha={args.input}','--input',f'OIII={source}','--output-dir',str(output)],text=True))
        metrics=verify(args.input,source,output,report)
        assert abs(report['gain']-gain)<(.00001 if name=='exact' else .003)
        assert abs(report['offset']-offset)<(.01 if name=='exact' else 3)
        (args.output/(name+'-metrics.json')).write_text(json.dumps({'truth':[gain,offset],'report':report,'independent':metrics},indent=2)+'\n')
        replay=args.output/(name+'-replay')
        subprocess.run([str(args.cli),'channel-match-replay','--recipe',str(output/'recipe.txt'),'--output-dir',str(replay)],check=True,stdout=subprocess.DEVNULL)
        for f in ['matched.fits','validity.fits','residual.fits']:assert sha(output/f)==sha(replay/f)
        print(name,'gain',report['gain'],'offset',report['offset'],'held-out RMS',report['afterRms'])
    assert before==sha(args.input)
    (args.output/'input-sha256.txt').write_text(before+'\n')

if __name__=='__main__':main()
