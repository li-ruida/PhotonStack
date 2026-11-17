"""Independent Astropy/WCSLIB + NumPy audit of a WCS mosaic seam report.
Usage: python mosaic_seam_validation.py cli-report.json output.fits panel1.fits panel2.fits [...]
Checks measurements before/after gains using independent reprojection of the ORIGINAL panels.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS


def read_report(path):
    text=path.read_text(); decoder=json.JSONDecoder(); index=0
    while index<len(text):
        start=text.find('{',index)
        if start<0:break
        obj,n=decoder.raw_decode(text[start:]);index=start+n
        if obj.get('type')=='complete':return obj['seams']
    raise ValueError('No complete mosaic report')


def read(path):
    with fits.open(path,memmap=False) as hdus:
        image=np.array(hdus[0].data,dtype=float); header=hdus[0].header.copy()
    if image.ndim==2:image=np.stack([image]*3)
    if image.shape[0]==3:image=np.concatenate([image,np.ones_like(image[:1])])
    assert header.get('PSCOLOR','LINEAR').upper()=='LINEAR'
    return image,WCS(header,naxis=2)


def sample(image,wcs,sky):
    # Engine stores inverse-projected source locations in Float32.
    xy=wcs.all_world2pix(sky,0,tolerance=1e-10,maxiter=100).astype(np.float32)
    h,w=image.shape[1:];x,y=xy[:,0],xy[:,1];x0=np.floor(x).astype(int);y0=np.floor(y).astype(int)
    fx=x-x0;fy=y-y0
    color=np.zeros((len(x),3));coverage=np.zeros(len(x))
    for dy in [0,1]:
        for dx in [0,1]:
            xx=x0+dx;yy=y0+dy;valid=(xx>=0)&(xx<w)&(yy>=0)&(yy<h)
            weight=(fx if dx else 1-fx)*(fy if dy else 1-fy)
            ids=np.flatnonzero(valid);a=np.clip(image[3,yy[ids],xx[ids]],0,1)*weight[ids]
            coverage[ids]+=a;color[ids]+=image[:3,yy[ids],xx[ids]].T*a[:,None]
    color=np.divide(color,coverage[:,None],out=np.zeros_like(color),where=coverage[:,None]>1e-8).astype(np.float32).astype(float)
    return color,coverage


def median(a):return np.partition(a,len(a)//2,axis=0)[len(a)//2]
def statistics(a,b):
    if len(a)==0:return None
    d=b-a;m=median(d)
    return dict(samples=len(a),firstMedian=median(a),secondMedian=median(b),medianDifference=m,
                rmsDifference=np.sqrt(np.mean(d*d,axis=0)),p95Absolute=np.sort(abs(d),axis=0)[int(np.ceil(.95*len(d)))-1],
                madDifference=median(abs(d-m)))


def validate(report_path,output,panels):
    report=read_report(report_path)
    assert report['encoding']=='linear' and not report['preview']
    with fits.open(output) as hdus: output_wcs=WCS(hdus[0].header,naxis=2)
    images=[read(path) for path in panels];summary=[]
    for pair in report['pairs']:
        left,top,right,bottom=map(int,pair['summary']['bounds']);width,height=right-left,bottom-top
        nx,ny=min(48,width),min(48,height)
        xx=left+((np.arange(nx)+.5)*width/nx).astype(int); yy=top+((np.arange(ny)+.5)*height/ny).astype(int)
        x,y=np.meshgrid(xx,yy);xy=np.column_stack([x.ravel(),y.ravel()])
        cell=((xy[:,1]-top)*4//height)*4+(xy[:,0]-left)*4//width
        points=np.array([xy+[dx,dy] for dy in [-1,0,1] for dx in [-1,0,1]]).transpose(1,0,2).reshape(-1,2)
        sky=output_wcs.all_pix2world(points,0)
        a,ac=sample(*images[pair['first']],sky);b,bc=sample(*images[pair['second']],sky)
        valid=((ac>=.99)&(bc>=.99)).reshape(-1,9).all(axis=1)
        a=np.median(a.reshape(-1,9,3),axis=1)[valid];b=np.median(b.reshape(-1,9,3),axis=1)[valid];cell=cell[valid]
        ca=report['corrections'][pair['first']];cb=report['corrections'][pair['second']]
        aa=a*ca['gains']+ca['offsets'];bb=b*cb['gains']+cb['offsets']
        scale=max(1,float(np.max(abs(a))),float(np.max(abs(b)))) if len(a) else 1
        errors={}
        for region,select in [(pair['summary'],np.ones(len(a),dtype=bool))]+[(r,cell==i) for i,r in enumerate(pair['cells'])]:
            for stage,first,second in [('before',a,b),('after',aa,bb)]:
                expected=statistics(first[select],second[select]);actual=region[stage]
                assert (actual is None)==(expected is None)
                if expected is None:continue
                assert actual['samples']==expected['samples'],(actual['samples'],expected['samples'])
                for key,value in expected.items():
                    if key=='samples':continue
                    error=float(np.max(abs(np.asarray(actual[key])-value)))
                    errors[key]=max(errors.get(key,0),error)
                    # Float32 canvas/source coordinates are independently reconstructed;
                    # steep star slopes amplify subpixel rounding. Report actual errors.
                    assert error<scale*2e-5,(key,error,scale)
        summary.append(dict(first=pair['first'],second=pair['second'],samples=len(a),scale=scale,maxErrors=errors))
    return summary

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('report',type=Path);p.add_argument('output',type=Path);p.add_argument('panels',type=Path,nargs='+')
    a=p.parse_args();print(json.dumps(validate(a.report,a.output,a.panels),indent=2))
