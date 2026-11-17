#!/usr/bin/env python3
"""Independent real-master composition checks. Inputs are read-only.
Requires NumPy/Astropy. Outputs go to a new directory. The L test plane is
computed from RGB; it is not a separately captured luminance exposure.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli',required=True,type=Path)
    parser.add_argument('--rgb',required=True,type=Path)
    parser.add_argument('--ha',required=True,type=Path)
    parser.add_argument('--oiii',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();out=args.output;out.mkdir(parents=True,exist_ok=False)
    originals={str(p):digest(p) for p in (args.rgb,args.ha,args.oiii)}
    raw,header=fits.getdata(args.rgb,header=True)
    assert raw.ndim==3 and raw.shape[0] in (3,4)
    valid=np.isfinite(raw[:3]).all(axis=0)
    if raw.shape[0]==4:valid&=raw[3]>0
    planes={name:np.where(valid,raw[c],np.nan).astype(np.float32) for c,name in enumerate('RGB')}
    planes['L']=(.2126*planes['R'].astype(float)+.7152*planes['G'].astype(float)+.0722*planes['B'].astype(float)).astype(np.float32)
    for name,data in planes.items():
        hdr=header.copy()
        for key in ('BAYERPAT','BSCALE','BZERO','CHECKSUM','DATASUM','CD3_3','NAXIS3'):hdr.remove(key,ignore_missing=True)
        fits.writeto(out/(name+'.fits'),data,hdr)
    metrics={}
    def run(name,inputs,expressions,workers):
        command=[str(args.cli),'channel-compose','--aligned','--output-dir',str(out/name),'--workers',str(workers)]
        for symbol,path in inputs:command+=['--input',f'{symbol}={path}']
        for channel,expression in zip(('red','green','blue'),expressions):command+=['--'+channel,expression]
        started=time.perf_counter();result=subprocess.run(['/usr/bin/time','-l',*command],capture_output=True,text=True)
        (out/(name+'.time')).write_text(result.stderr)
        assert result.returncode==0,result.stderr
        metrics[name]={'seconds':time.perf_counter()-started,'report':json.loads(result.stdout)}
        return fits.getdata(out/name/'RGB.fits')
    inputs=[(name,out/(name+'.fits')) for name in 'RGB']
    for workers in (1,8):
        actual=run(f'rgb-{workers}',inputs,list('RGB'),workers)
        assert np.array_equal(actual[3]>0,valid)
        for c,name in enumerate('RGB'):assert np.array_equal(actual[c][valid],planes[name][valid])
    for name in ('RGB.fits','validity.fits'):assert digest(out/'rgb-1'/name)==digest(out/'rgb-8'/name)
    expressions=[f'L*{c}/(0.2126*R+0.7152*G+0.0722*B)' for c in 'RGB']
    actual=run('lrgb',inputs+[('L',out/'L.fits')],expressions,8)
    denominator=sum(w*planes[c].astype(float) for c,w in zip('RGB',(.2126,.7152,.0722)))
    expected_valid=valid&(denominator!=0)
    assert np.array_equal(actual[3]>0,expected_valid)
    errors=[]
    for c,name in enumerate('RGB'):
        expected=(planes['L'].astype(float)[expected_valid]*planes[name].astype(float)[expected_valid]/denominator[expected_valid]).astype(np.float32)
        errors.append(float(np.max(np.abs(actual[c][expected_valid]-expected))))
    assert max(errors)==0,errors
    metrics['lrgb']['independent_float32_max_abs']=errors
    weights=np.array([.2126,.7152,.0722])
    luminance=sum(w*actual[c].astype(float) for c,w in enumerate(weights))
    relative=np.abs(luminance[expected_valid]-planes['L'][expected_valid])/np.maximum(1,np.abs(planes['L'][expected_valid]))
    metrics['lrgb']['luminance_relative_max']=float(relative.max())
    assert relative.max()<1e-6
    if 'CTYPE1' in header:
        w1=WCS(header,naxis=2);w2=WCS(fits.getheader(out/'rgb-8/RGB.fits'),naxis=2)
        y,x=np.mgrid[0:raw.shape[1]:9j,0:raw.shape[2]:9j]
        a=np.array(w1.all_pix2world(x,y,0));b=np.array(w2.all_pix2world(x,y,0))
        metrics['wcs_max_degrees']=float(np.max(np.abs(a-b)));assert metrics['wcs_max_degrees']<1e-10
    h=fits.getdata(args.ha);o=fits.getdata(args.oiii)
    actual=run('hoo',[('Ha',args.ha),('OIII',args.oiii)],['Ha','0.25*Ha+0.75*OIII','OIII'],8)
    valid=np.isfinite(h)&np.isfinite(o)
    assert np.array_equal(actual[3]>0,valid)
    for c,truth in enumerate((h,(.25*h.astype(float)+.75*o.astype(float)).astype(np.float32),o)):
        assert np.array_equal(actual[c][valid],truth[valid])
    assert originals=={str(p):digest(p) for p in (args.rgb,args.ha,args.oiii)}
    metrics['source_sha256']=originals
    metrics['rgb_reference']='read-only RGB master split into mono planes; synthetic luminance from Rec.709 weights'
    (out/'metrics.json').write_text(json.dumps(metrics,indent=2))
    print(json.dumps(metrics,indent=2))

if __name__=='__main__':main()
