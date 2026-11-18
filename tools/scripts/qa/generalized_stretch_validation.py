#!/usr/bin/env python3
"""Independent NumPy/Astropy GHS oracle, real-image pixels, coverage, report, replay.
Requires NumPy and Astropy. Original files are only read. Output must be new.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time
import numpy as np
import tifffile
from astropy.io import fits


def curve(x, log_d, b, sp, lp, hp):
    d=math.expm1(log_d)
    if d==0:return np.asarray(x)
    def radial(t):
        if b==0:return 1-np.exp(-d*t)
        if b==-1:return np.log(1+d*t)
        if b>0:return 1-(1+b*d*t)**(-1/b)
        return (1-(1-b*d*t)**((b+1)/b))/(b+1)
    def slope(t):
        if b==0:return d*np.exp(-d*t)
        return d*(1+abs(b)*d*t)**((-1-1/b) if b>0 else 1/b)
    def signed(v):return np.sign(v-sp)*radial(np.abs(v-sp))
    x=np.asarray(x,dtype=np.float64)
    q=np.where(x<lp,signed(lp)+(x-lp)*slope(sp-lp),np.where(x>hp,signed(hp)+(x-hp)*slope(hp-sp),signed(x)))
    low=signed(lp)-lp*slope(sp-lp);high=signed(hp)+(1-hp)*slope(hp-sp)
    return np.clip((q-low)/(high-low),0,1)


def reference(data,r):
    if data.ndim==2:data=np.stack([data]*3+[np.ones_like(data)])
    elif len(data)==3:data=np.concatenate([data,np.ones_like(data[:1])])
    rgb=np.asarray(data[:3],dtype=np.float64);alpha=np.clip(data[3],0,1)
    valid=(alpha>1e-6)&np.isfinite(rgb).all(axis=0);alpha=np.where(valid,alpha,0)
    before=np.clip((rgb-r['black'])/(r['white']-r['black']),0,1)
    before[:,~valid]=0
    def f(x):return curve(x,r['logStretch'],r['localIntensity'],r['symmetry'],r['shadowProtection'],r['highlightProtection'])
    independent=f(before);lum=np.einsum('i,ijk->jk',[.2126,.7152,.0722],before)
    blended=0
    if r['luminance']:
        factor=np.divide(f(lum),lum,out=np.ones_like(lum),where=lum>0);scaled=before*factor
        blend=np.min(np.where(scaled>1,np.divide(1-independent,scaled-independent,out=np.ones_like(scaled),where=scaled>1),1),axis=0)
        out=blend*scaled+(1-blend)*independent;blended=int(np.sum((blend<1)&valid))
    else:out=independent
    out=np.clip(out,0,1);out[:,~valid]=0
    hist_before=np.bincount(np.minimum((lum[valid]*256).astype(int),255),minlength=256)
    after_lum=.2126*out[0]+.7152*out[1]+.0722*out[2]
    hist_after=np.bincount(np.minimum((after_lum[valid]*256).astype(int),255),minlength=256)
    assert np.array_equal(hist_before,r['beforeHistogram'])
    assert np.array_equal(hist_after,r['afterHistogram'])
    assert int(np.sum((rgb<r['black']).any(axis=0)&valid))==r['belowBlack']
    assert int(np.sum((rgb>r['white']).any(axis=0)&valid))==r['aboveWhite']
    return np.concatenate([out,alpha[None]]).astype(np.float32),valid,blended


def main():
    ap=argparse.ArgumentParser();ap.add_argument('cli',type=Path);ap.add_argument('source',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args()
    if a.output.exists():raise SystemExit('Output must be new')
    a.output.mkdir();sha=hashlib.sha256(a.source.read_bytes()).hexdigest()
    results=[]
    for mode in ['independent','luminance']:
        dest=a.output/mode
        args=[str(a.cli.resolve()),'ghs-stretch','--input',str(a.source.resolve()),'--output-dir',str(dest.resolve()),'--log-stretch','5','--local-intensity','3','--symmetry','.012','--shadow-protection','.002','--highlight-protection','.85','--mode',mode]
        started=time.perf_counter();run=subprocess.run(args,check=True,text=True,capture_output=True);elapsed=time.perf_counter()-started;r=json.loads(run.stdout)
        data=fits.getdata(a.source);expected,valid,blended=reference(data,r);actual=fits.getdata(dest/'display.fits')
        error=np.abs(actual-expected);maximum=float(np.max(error));rms=float(np.sqrt(np.mean(np.square(error,dtype=np.float64))))
        assert maximum<1.2e-7,(maximum,mode)
        assert np.array_equal(actual[3],expected[3])
        assert r['coveredPixels']==int(valid.sum()) and r['gamutBlendedPixels']==blended
        assert fits.getheader(dest/'display.fits')['PSCOLOR']=='SRGB-DISPLAY'
        tiff=tifffile.imread(dest/'developed.tiff')
        assert tiff.dtype==np.uint16 and tiff.shape==np.moveaxis(actual,0,-1).shape
        quantized=np.floor(np.moveaxis(actual,0,-1).astype(np.float64)*65535+.5).astype(np.int64)
        tiff_error=int(np.max(np.abs(tiff.astype(np.int64)-quantized)))
        assert tiff_error<=1,tiff_error
        subprocess.run([str(a.cli.resolve()),'ghs-replay','--recipe',str(dest/'recipe.txt'),'--output-dir',str(a.output/(mode+'-replay'))],check=True,capture_output=True)
        assert (dest/'display.fits').read_bytes()==(a.output/(mode+'-replay')/'display.fits').read_bytes()
        results.append(dict(mode=mode,max_abs=maximum,rms=rms,covered=r['coveredPixels'],gamut_blended=blended,seconds=elapsed,tiff_max_code_error=tiff_error))
    assert hashlib.sha256(a.source.read_bytes()).hexdigest()==sha
    report=dict(source=str(a.source.resolve()),source_sha256=sha,results=results)
    (a.output/'validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))

if __name__=='__main__':main()
