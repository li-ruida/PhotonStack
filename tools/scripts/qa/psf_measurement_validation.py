#!/usr/bin/env python3
"""Independent SciPy Gaussian fitting and NumPy PSF/report checks.
The supplied mono/RGB linear FITS remains read-only; output must be new.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np
from astropy.io import fits
from scipy.optimize import least_squares


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli',type=Path,required=True);parser.add_argument('--input',type=Path,required=True)
    parser.add_argument('--channel',default='luminance',choices=['luminance','red','green','blue'])
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False);before=digest(args.input)
    destination=args.output/'measurement'
    report=json.loads(subprocess.check_output([str(args.cli),'psf-measure','--input',str(args.input),'--channel',args.channel,'--output-dir',str(destination)],text=True))
    data=fits.getdata(args.input).astype(np.float64)
    if data.ndim==3:
        data=(np.tensordot([.2126,.7152,.0722],data[:3],axes=1) if args.channel=='luminance' else data[['red','green','blue'].index(args.channel)])
        data=data.astype(np.float32).astype(np.float64)
    stars=[s for s in report['stars'] if s['status']=='accepted']
    covariance=np.array([[s[k] for k in ['xx','yy','xy']] for s in stars])
    np.testing.assert_allclose(np.mean(covariance,axis=0),[report[k] for k in ['xx','yy','xy']],rtol=1e-13,atol=1e-13)
    widths=2.3548200450309493*(covariance[:,0]*covariance[:,1]-covariance[:,2]**2)**.25
    np.testing.assert_allclose(np.quantile(widths,[.1,.5,.9]),[report[k] for k in ['fwhmP10','fwhmP50','fwhmP90']],rtol=1e-12)
    for i,r in enumerate(report['regions']):
        selected=[s for s in stars if int(s['x']*4/report['width'])+4*int(s['y']*4/report['height'])==i]
        assert len(selected)==r['count']
        if selected:
            cs=np.array([[s[k] for k in ['xx','yy','xy']] for s in selected]);np.testing.assert_allclose(cs.mean(axis=0),[r[k] for k in ['xx','yy','xy']],rtol=1e-13)
            np.testing.assert_allclose(r['fwhm'],np.median(2.3548200450309493*(cs[:,0]*cs[:,1]-cs[:,2]**2)**.25),rtol=1e-12)
    kernel=fits.getdata(destination/'psf.fits');radius=kernel.shape[0]//2
    y,x=np.mgrid[-radius:radius+1,-radius:radius+1];xx,yy,xy=[report[k] for k in ['xx','yy','xy']]
    expected=np.exp(-.5*(yy*x*x-2*xy*x*y+xx*y*y)/(xx*yy-xy*xy));expected/=expected.sum()
    np.testing.assert_allclose(kernel,expected.astype(np.float32),rtol=2e-7,atol=1e-10)
    assert abs(kernel.sum(dtype=float)-1)<1e-7
    discrete=[float(np.sum(kernel*x*x)),float(np.sum(kernel*y*y)),float(np.sum(kernel*x*y))]
    # Use a different optimizer, finite-difference Jacobian, and circular initial
    # shape. The scientific model is explicit; agreement does not establish that
    # real stellar wings are Gaussian or that an undersampled ePSF is identified.
    local_y,local_x=np.mgrid[-7:8,-7:8];records=[]
    ordered=sorted(stars,key=lambda s:(s['y'],s['x']))
    for index in np.unique(np.linspace(0,len(ordered)-1,min(96,len(ordered))).astype(int)):
        star=ordered[index];sx=star['sampleX'];sy=star['sampleY'];patch=data[sy-7:sy+8,sx-7:sx+8]
        sky=np.median(patch[local_x*local_x+local_y*local_y>30]);amplitude=np.max(patch[5:10,5:10])-sky
        normalized=(patch-sky)/amplitude
        def residual(p):
            amp,cx,cy,lx,ly,z,b,bx,by=p;sigx=np.exp(lx);sigy=np.exp(ly);rho=.9*np.tanh(z)
            u=(local_x-cx)/sigx;v=(local_y-cy)/sigy
            return (amp*np.exp(-.5*(u*u+v*v-2*rho*u*v)/(1-rho*rho))+b+bx*local_x+by*local_y-normalized).ravel()
        initial=[1,0,0,np.log(1.2),np.log(1.2),0,0,0,0]
        fit=least_squares(residual,initial,bounds=([.1,-3,-3,np.log(.5),np.log(.5),-2,-np.inf,-np.inf,-np.inf],[3,3,3,np.log(4),np.log(4),2,np.inf,np.inf,np.inf]),max_nfev=500,ftol=1e-12,xtol=1e-12,gtol=1e-12)
        assert fit.success
        _,cx,cy,lx,ly,z,*_=fit.x;cs=[np.exp(2*lx),np.exp(2*ly),.9*np.tanh(z)*np.exp(lx+ly)]
        difference=np.array(cs)-[star[k] for k in ['xx','yy','xy']]
        centroid=np.hypot(sx+cx-star['x'],sy+cy-star['y'])
        records.append({'id':star['id'],'covarianceDifference':difference.tolist(),'centroidDifference':float(centroid),'scipyRelativeResidual':float(np.sqrt(np.mean(fit.fun**2))/fit.x[0])})
    maximum=float(np.max(np.abs([r['covarianceDifference'] for r in records])))
    max_center=max(r['centroidDifference'] for r in records)
    assert maximum<.002,maximum
    assert max_center<.002,max_center
    assert before==digest(args.input)
    result={'sourceSHA256':before,'report':report,'discreteKernelCovariance':discrete,'independentFits':records,'maxCovarianceDifference':maximum,'maxCentroidDifference':max_center}
    (args.output/'metrics.json').write_text(json.dumps(result,indent=2)+'\n')
    print('accepted',report['accepted'],'independent',len(records),'max covariance delta',maximum,'max centroid delta',max_center)

if __name__=='__main__':main()
