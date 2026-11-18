#!/usr/bin/env python3
"""Independent NumPy/Astropy oracle and full-size sensor-job CPU measurements.

Generated fixtures only. Run outside concurrent builds for useful timing:
  python calibration_job_validation.py --cli ... --output /tmp/new-audit --large
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def header(role, exposure=60, day=15, filt='Ha', state=None):
    h=fits.Header()
    for k,v in {'INSTRUME':'Oracle camera','CAM-SER':'oracle-1','CCD-TEMP':-10,'GAIN':100,'OFFSET':20,
                'READMODE':'normal','EXPTIME':exposure,'XBINNING':1,'YBINNING':1,'XORGSUBF':0,'YORGSUBF':0,
                'BAYERPAT':'BGGR','XBAYROFF':1,'YBAYROFF':1,'BUNIT':'ADU','PSCOLOR':'Linear','PSOPTICS':'train-1',
                'FILTER':filt,'IMAGETYP':role,'OBJECT':'Generated sensor field','DATE-OBS':f'2026-10-{day}T22:00:00'}.items():h[k]=v
    if state:h['PSBIAS']=state
    return h


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--cli',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--large',action='store_true')
    args=parser.parse_args();root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
    width,height=(2160,3840) if args.large else (384,256)
    rows,cols=np.indices((height,width),dtype=np.int32);phase=2*(rows%2)+cols%2
    bias=(10+(cols%31)/16+(rows%13)/32).astype(np.float32)
    dark=(bias+3+(rows%17)/32).astype(np.float32)
    flat=(np.array([100,200,300,400],dtype=np.float32)[phase]*(.5+((rows+cols)%1024)/1024)).astype(np.float32)
    flat[0,0]=0;flat[0,2]=np.nan;dark[0,7]=np.nan
    masters={}
    for name,data,role,state in [('bias',bias,'Master Bias','included'),('dark',dark,'Master Dark','included'),('flat',flat,'Master Flat','removed')]:
        p=root/(name+'.fits');fits.writeto(p,data,header(role,exposure=0 if name=='bias' else 60,state=state));masters[name]=p
    lights=[]
    for i in range(4):
        data=(dark+(cols%257-100)/8+(rows%67)/32+i*2).astype(np.float32)
        data[0,5]=np.nan
        h=header('Light',day=15+i//2)
        h['DATE-OBS']=f'2026-10-{15+i//2}T22:0{i}:00'
        h['CTYPE1']='RA---TAN-SIP';h['CTYPE2']='DEC--TAN-SIP';h['CRPIX1']=width/2+.5;h['CRPIX2']=height/2+.5
        h['CRVAL1']=313.6;h['CRVAL2']=29.7;h['CD1_1']=-.001;h['CD1_2']=0.;h['CD2_1']=0.;h['CD2_2']=.001
        h['A_ORDER']=2;h['B_ORDER']=2;h['A_2_0']=1e-7;h['B_0_2']=-2e-7
        p=root/f'light-{i}.fits';fits.writeto(p,data,h);lights.append(p)
    original={str(p):sha(p) for p in list(masters.values())+lights}
    manifests=[]
    for workers,tile in [(1,65536),(4,65536),(4,131072)]:
        text=f'PhotonStack-CalibrationJob-1\nthreads {workers}\ntile-pixels {tile}\nmemory-mib 512\n'
        for name,p in masters.items():text+=f'master {name} {name} {json.dumps(str(p))} {sha(p)} '+('removed' if name=='flat' else 'included')+'\n'
        for p in lights:text+=f'light {json.dumps(str(p))} {sha(p)} bias dark flat\n'
        m=root/f'job-{workers}-{tile}.txt';m.write_text(text);manifests.append((workers,tile,m))
    runs=[]
    for workers,tile,m in manifests:
        output=root/f'result-{workers}-{tile}'
        begin=time.monotonic()
        process=subprocess.run(['/usr/bin/time','-l',str(args.cli.resolve()),'calibration-run','--manifest',str(m),'--output-dir',str(output)],capture_output=True,text=True,check=True)
        elapsed=time.monotonic()-begin
        (root/f'{output.name}-stderr.txt').write_text(process.stderr)
        summary=json.loads(process.stdout);assert summary['completed']==4 and summary['status']=='complete'
        rss=re.search(r'(\d+)\s+maximum resident set size',process.stderr)
        reports=[json.loads((output/f'frame-{i+1:06}/report.json').read_text()) for i in range(4)]
        runs.append({'workersRequested':workers,'tilePixelsRequested':tile,'seconds':elapsed,
                     'peakRSSBytes':int(rss.group(1)) if rss else None,'actualWorkers':[r['workers'] for r in reports],
                     'workingBytes':[r['workingBytes'] for r in reports],'path':str(output)})
    oracle=[]
    means=np.array([np.mean(flat[(phase==p)&np.isfinite(flat)&(flat>0)],dtype=np.float64) for p in range(4)])
    response=flat.astype(np.float64)/means[phase]
    base=Path(runs[0]['path'])
    for i,p in enumerate(lights):
        source=fits.getdata(p).astype(np.float64)
        with np.errstate(invalid='ignore',divide='ignore'):
            expected=(source-dark.astype(np.float64))/response
        valid=np.isfinite(expected)&np.isfinite(response)&(response>1e-6)
        expected[~valid]=np.nan;expected=expected.astype(np.float32)
        output=base/f'frame-{i+1:06}'
        actual=fits.getdata(output/'calibrated.fits');mask=fits.getdata(output/'valid.fits')
        assert np.array_equal(np.isnan(actual),~valid) and np.array_equal(mask,valid.astype(np.float32))
        assert np.array_equal(actual[valid],expected[valid])
        report=json.loads((output/'report.json').read_text())
        assert report['invalidPixels']==int((~valid).sum()) and report['negativePixels']==int((actual[valid]<0).sum())
        assert np.allclose(report['flatMeans'],means,rtol=2e-13,atol=0)
        originalWCS=WCS(fits.getheader(p));outputWCS=WCS(fits.getheader(output/'calibrated.fits'))
        points=np.array([[0,0],[width-1,0],[0,height-1],[width-1,height-1],[width/2,height/2]])
        assert np.array_equal(originalWCS.all_pix2world(points,0),outputWCS.all_pix2world(points,0))
        for run in runs[1:]:
            for name in ['calibrated.fits','valid.fits']:
                assert sha(output/name)==sha(Path(run['path'])/f'frame-{i+1:06}'/name)
        oracle.append({'frame':i,'pixels':int(actual.size),'maxAbsoluteError':float(np.max(abs(actual[valid]-expected[valid]))),
                       'invalidPixels':int((~valid).sum()),'negativePixels':int((actual[valid]<0).sum()),'WCSUnchanged':True})
    assert all(sha(Path(p))==h for p,h in original.items())
    audit={'dataset':'generated','width':width,'height':height,'frames':4,'runs':runs,'oracle':oracle,
           'scientificBytesEqualAcrossWorkersAndTiles':True,'sourceHashesUnchanged':True,'sources':original}
    (root/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    print(json.dumps({k:v for k,v in audit.items() if k!='sources'},indent=2))


if __name__=='__main__':main()
