#!/usr/bin/env python3
"""Independent SciPy finite-domain weighted RL oracle and real-image diagnostics."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import numpy as np
from astropy.io import fits
from scipy.signal import fftconvolve
from scipy.ndimage import distance_transform_cdt


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def oracle(data,coverage,kernel,iterations,amount,pedestal,noise,protect):
    # SciPy performs full linear convolution and crops the centered result.
    # No native FFT code, padding sizes, or engine intermediate images reused.
    h=lambda x:fftconvolve(x,kernel,mode='same')
    ht=lambda x:fftconvolve(x,kernel[::-1,::-1],mode='same')
    covered=coverage>0;support=h(coverage);weight=np.divide(coverage,support,out=np.zeros_like(data),where=covered)
    sensitivity=ht(weight);observed=np.where(covered,data+pedestal,0)
    floor=max(observed.max()*1e-12,np.finfo(float).tiny);estimate=np.where(covered,np.maximum(observed,floor),0)
    forward=lambda x:np.divide(h(x*coverage),support,out=np.zeros_like(data),where=covered).clip(0)
    before=forward(observed)
    for _ in range(iterations):
        predicted=np.maximum(forward(estimate),floor);delta=observed-predicted
        damp=(delta/np.hypot(delta,noise))**2 if noise>0 else 1
        ratio=(1+damp*delta/predicted).clip(0)
        update=np.divide(ht(weight*ratio),sensitivity,out=np.zeros_like(data),where=covered).clip(0)
        estimate*=update
    radius=max(1,max(kernel.shape)//2);mask=np.zeros_like(data)
    core=covered&(data>=protect)
    if protect>0 and np.any(core):
        distance=distance_transform_cdt(~core,metric='chessboard')
        mask=np.where(distance<=radius,1,np.where(distance<2*radius,.5*(1+np.cos(np.pi*(distance/radius-1))),0))
    output=(data+amount*(1-mask)*(estimate-observed)).astype(np.float32)
    final=forward(np.where(covered,output.astype(float)+pedestal,0))
    return output,observed-final,mask,before


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cli',type=Path,required=True);p.add_argument('--measurement',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--iterations',type=int,default=8);p.add_argument('--amount',type=float,default=.7);p.add_argument('--pedestal',type=float,default=0);p.add_argument('--noise',type=float,default=0);p.add_argument('--protect',type=float,default=0)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    measurement=json.loads(subprocess.check_output([str(a.cli),'psf-inspect','--recipe',str(a.measurement)],text=True));source=Path(measurement['source']);beforeHash=digest(source)
    original=fits.getdata(source).astype(float);data=original
    if data.ndim==3:
        coverage=np.clip(data[3],0,1) if data.shape[0]==4 else np.ones(data.shape[1:])
        finite=np.all(np.isfinite(data[:3]),axis=0)&np.isfinite(coverage)
        data=(np.tensordot([.2126,.7152,.0722],data[:3],axes=1) if measurement['channel']=='luminance' else data[['red','green','blue'].index(measurement['channel'])]).astype(np.float32).astype(float)
    else:coverage=np.ones_like(data);finite=np.isfinite(data)
    coverage=np.where(finite&(coverage>1e-6),coverage,0);data=np.where(coverage>0,data,0)
    kernel=fits.getdata(a.measurement.parent/'psf.fits').astype(float);kernel/=kernel.sum()
    t=time.perf_counter()
    report=json.loads(subprocess.check_output([str(a.cli),'psf-restore','--measurement',str(a.measurement),'--output-dir',str(a.output/'native'),'--iterations',str(a.iterations),'--amount',str(a.amount),'--pedestal',str(a.pedestal),'--noise-sigma',str(a.noise),'--protect-above',str(a.protect)],text=True))
    nativeSeconds=time.perf_counter()-t;t=time.perf_counter()
    # The explicit Float32 channel product defines the numeric input boundary.
    # BLAS reduction vs scalar/FMA can differ by one ULP at half-way RGB sums.
    selected=fits.getdata(a.output/'native/channel.fits')[0].astype(float)
    np.testing.assert_allclose(selected[coverage>0],data[coverage>0],rtol=np.finfo(np.float32).eps,atol=0)
    data=np.where(coverage>0,selected,0)
    expected,residual,mask,blurred=oracle(data,coverage,kernel,a.iterations,a.amount,a.pedestal,a.noise,a.protect)
    scipySeconds=time.perf_counter()-t
    actual=fits.getdata(a.output/'native/restored.fits').astype(float);result=actual[0];covered=coverage>0
    error=np.abs(result[covered]-expected[covered]);tolerance=2*np.abs(np.spacing(expected[covered]))+max(1.,np.max(np.abs(data)))*1e-10
    assert np.all(error<=tolerance),(error.max(),tolerance.min())
    np.testing.assert_allclose(actual[3],coverage,rtol=0,atol=0)
    change=fits.getdata(a.output/'native/difference.fits')[0]
    np.testing.assert_array_equal(change[covered],(result-data).astype(np.float32)[covered])
    nativeResidual=fits.getdata(a.output/'native/residual.fits')[0]
    np.testing.assert_allclose(nativeResidual[covered],residual[covered],rtol=2e-6,atol=max(1.,np.max(np.abs(data)))*1e-9)
    nativeMask=fits.getdata(a.output/'native/protection.fits');np.testing.assert_allclose(nativeMask[covered],mask[covered],rtol=1e-7,atol=1e-7)
    np.testing.assert_array_equal(result[covered&(mask==1)],data[covered&(mask==1)])
    weighted=lambda v:float(np.sum(v[covered]*coverage[covered]))
    np.testing.assert_allclose([report['beforeFlux'],report['afterFlux']],[weighted(data),weighted(result)],rtol=1e-10,atol=1e-5)
    np.testing.assert_allclose(report['afterRms'],np.sqrt(weighted(residual**2)/coverage.sum()),rtol=1e-9)
    # Apertures are diagnostics, not assertions of photometric improvement.
    apertures=[]
    for star in measurement['stars']:
        x,y=star['x'],star['y'];sx,sy=round(x),round(y)
        if star['status']!='accepted' or min(sx,sy,data.shape[1]-sx-1,data.shape[0]-sy-1)<12:continue
        yy,xx=np.mgrid[sy-12:sy+13,sx-12:sx+13];r=np.hypot(xx-x,yy-y);inside=r<=6;annulus=(r>=8)&(r<=11)
        if not np.all(covered[yy,xx]):continue
        values=[]
        for image in [data,result]:
            patch=image[yy,xx];sky=np.median(patch[annulus]);flux=np.sum(patch[inside]-sky)
            values.append(float(flux))
        if values[0]>0:apertures.append(values[1]/values[0])
    metrics={'nativeSeconds':nativeSeconds,'scipySeconds':scipySeconds,'maxAbsoluteDifference':float(error.max()),'sourceSHA256':beforeHash,'report':report,
             'apertures':len(apertures),'apertureFluxRatioP10P50P90':np.quantile(apertures,[.1,.5,.9]).tolist() if apertures else []}
    assert digest(source)==beforeHash
    (a.output/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
    print(json.dumps(metrics,indent=2))

if __name__=='__main__':main()
