"""Recover an independently injected gradient on real FITS without altering originals.
Requires numpy/astropy. Both fits use the same manual sky samples and degree 2.
This tests recovery of the added gradient, not the true astrophysical background.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np
from astropy.io import fits

def main():
    p=argparse.ArgumentParser()
    p.add_argument("source",type=Path);p.add_argument("samples",type=Path)
    p.add_argument("--cli",required=True,type=Path);p.add_argument("--output-dir",required=True,type=Path)
    p.add_argument("--model",choices=["polynomial","rbf"],default="polynomial")
    p.add_argument("--rbf-smoothing",type=float,default=.001)
    args=p.parse_args();root=args.output_dir;root.mkdir()
    before=hashlib.sha256(args.source.read_bytes()).hexdigest()
    with fits.open(args.source,memmap=False) as hdus:
        raw=np.array(hdus[0].data,dtype=np.float32);header=hdus[0].header.copy()
    assert raw.ndim==3 and raw.shape[0] in (3,4)
    h,w=raw.shape[1:]
    coverage=raw[3]>0 if len(raw)==4 else np.ones((h,w),dtype=bool)
    stride=max(1,w*h//100000)
    sample=raw[:3,coverage][:,::stride]
    span=float(np.percentile(sample,99)-np.percentile(sample,1))
    amplitude=max(span*.25,1)
    yy,xx=np.indices((h,w),dtype=np.float32)
    shape=(xx/max(1,w-1)-.5)+.65*(yy/max(1,h-1)-.5)
    gradient=np.array([amplitude*shape*f for f in (1,.7,1.2)],dtype=np.float32)
    raw[:3]+=gradient
    fits.PrimaryHDU(raw,header).writeto(root/"injected.fits")
    for name,path in (("baseline",args.source),("injected",root/"injected.fits")):
        command=[str(args.cli),"background-review","--input",str(path),"--samples",str(args.samples),
                 "--output-dir",str(root/name),"--degree","2","--strength","1","--preserve-brightness","off"]
        command += ["--model",args.model]
        if args.model=="rbf":command += ["--rbf-smoothing",str(args.rbf_smoothing)]
        subprocess.run(command,check=True,capture_output=True)
    before_model=fits.getdata(root/"baseline/model.fits")[:3]
    after_model=fits.getdata(root/"injected/model.fits")[:3]
    error=(after_model-before_model-gradient)[:,coverage]
    rms=np.sqrt(np.mean(error.astype(float)**2,axis=1))
    p95=np.percentile(np.abs(error),95,axis=1)
    result=dict(model=args.model,amplitude=amplitude,modelRecoveryRMS=rms.tolist(),modelRecoveryP95=p95.tolist(),
        rmsFractionOfInjectedAmplitude=(rms/amplitude).tolist(),sourceSHA=before)
    assert np.max(rms/amplitude)<.02,result
    assert hashlib.sha256(args.source.read_bytes()).hexdigest()==before
    (root/"validation.json").write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
if __name__=="__main__":main()
