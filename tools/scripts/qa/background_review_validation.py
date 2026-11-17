"""Independent FITS checks for a background-review directory (requires numpy/astropy).
Usage: python background_review_validation.py input.fits review-directory
Does not modify source files.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS

def planes(path):
    with fits.open(path, memmap=False) as hdus:
        a=np.asarray(hdus[0].data,dtype=np.float32)
        header=hdus[0].header.copy()
    if a.ndim==2:a=np.stack([a,a,a])
    if a.shape[0]==3:a=np.concatenate([a,np.ones((1,*a.shape[1:]),dtype=np.float32)])
    if header.get("PSCOLOR","").upper() in ("SRGB","S-RGB"):
        a[:3]=np.where(a[:3]<=.04045,a[:3]/12.92,((a[:3]+.055)/1.055)**2.4)
    return a,header

def validate(source,folder):
    original,header=planes(source)
    report=json.loads((folder/"report.json").read_text())
    corrected,out_header=planes(folder/"corrected.fits")
    model,_=planes(folder/"model.fits")
    difference,_=planes(folder/"difference.fits")
    assert original.shape==corrected.shape==model.shape==difference.shape
    assert hashlib.sha256(source.read_bytes()).hexdigest()==report["sourceSHA"]
    covered=original[3]>0
    for name,data in (("corrected",corrected),("model",model),("difference",difference)):
        assert np.all(np.isfinite(data)),name
        assert np.array_equal(data[3],original[3]),name+" coverage"
    sampled=[s for s in report["samples"] if s["status"] in ("used","partial","outlier")]
    fit=np.array([s["fitted"] for s in sampled])
    pedestal=np.partition(fit,len(fit)//2,axis=0)[len(fit)//2] if report["preserveBrightness"] else np.zeros(3)
    maximum=0.
    for c in range(3):
        predicted=original[c].astype(float)-report["strength"]*(model[c].astype(float)-pedestal[c])
        maximum=max(maximum,float(np.max(np.abs(corrected[c,covered]-predicted[covered]))))
    measurement_error=0.
    for sample in sampled:
        x,y,r=int(sample["x"]+.5),int(sample["y"]+.5),sample["radius"]
        box=original[:3,y-r:y+r+1,x-r:x+r+1].reshape(3,-1)
        measured=np.partition(box,box.shape[1]//2,axis=1)[:,box.shape[1]//2]
        measurement_error=max(measurement_error,float(np.max(np.abs(measured-sample["measured"]))))
    algebra=float(np.max(np.abs((original[:3]-corrected[:3]-difference[:3])[:,covered])))
    scale=max(1,float(np.max(np.abs(original[:3,covered]))))
    assert maximum<scale*1e-6,(maximum,scale)
    assert measurement_error<scale*1e-6,measurement_error
    assert algebra<scale*1e-6,algebra
    sky_error=None
    if report["wcsPreserved"]:
        h,w=original.shape[1:]
        points=np.array([[x,y] for x in [0,w/2,w-1] for y in [0,h/2,h-1]])
        sky_error=float(np.max(np.abs(WCS(header,naxis=2).all_pix2world(points,0)-WCS(out_header,naxis=2).all_pix2world(points,0))))
        assert sky_error<1e-9,sky_error
    result=dict(width=report["width"],height=report["height"],samples=len(report["samples"]),
        used=sum(s["status"]=="used" for s in report["samples"]),partial=sum(s["status"]=="partial" for s in report["samples"]),
        measurementMaxError=measurement_error,correctionMaxError=maximum,algebraMaxError=algebra,
        skyMaxErrorDegrees=sky_error,fitRMS=report["residualRMS"],sourceSHA=report["sourceSHA"])
    return result

if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("source",type=Path);p.add_argument("folder",type=Path)
    a=p.parse_args();print(json.dumps(validate(a.source,a.folder),indent=2))
