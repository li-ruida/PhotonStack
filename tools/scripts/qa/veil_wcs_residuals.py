"""Measure centroids against independent WCSLIB predictions in a Veil mosaic.
Usage: python veil_wcs_residuals.py east.fit west.fit mosaic.fits (requires astropy, numpy).
The flux threshold is for these Seestar masters, not a general star detector.
"""
import sys
from astropy.io import fits
from astropy.wcs import WCS
import numpy as np
def read(p):
    return fits.getdata(p)[0].astype(float), WCS(fits.getheader(p), naxis=2)
def centroid(a, x,y, find=3):
    ix,iy=np.rint([x,y]).astype(int)
    if not (10<ix<a.shape[1]-10 and 10<iy<a.shape[0]-10): return None
    cut=a[iy-find:iy+find+1,ix-find:ix+find+1]
    j,i=np.unravel_index(np.argmax(cut), cut.shape)
    cx,cy=ix+i-find, iy+j-find
    cut=a[cy-3:cy+4,cx-3:cx+4]
    weights=np.maximum(cut-np.median(cut),0)
    if weights.sum()==0:return None
    yy,xx=np.indices(weights.shape)
    return np.array([cx-3+(weights*xx).sum()/weights.sum(),cy-3+(weights*yy).sum()/weights.sum()])
inputs=[read(p) for p in sys.argv[1:3]]
out,wo=read(sys.argv[3]); print('output',out.shape)
for k,(a,w) in enumerate(inputs):
    b,wb=inputs[1-k]
    pts=[]
    for y in range(12,a.shape[0]-50,55):
        for x in range(12,a.shape[1]-50,55):
            cut=a[y:y+45,x:x+45];j,i=np.unravel_index(cut.argmax(),cut.shape)
            if cut.max()<np.median(cut)+300: continue
            c=centroid(a,x+i,y+j)
            if c is not None:pts.append(c)
    world=w.all_pix2world(pts,0)
    outpts=wo.all_world2pix(world,0); otherpts=wb.all_world2pix(world,0)
    overlap=[]; single=[]; both=[]
    for xy,other in zip(outpts,otherpts):
        c=centroid(out,*xy); q=centroid(b,*other)
        if c is None: continue
        if q is not None:
            overlap.append(np.linalg.norm(c-xy));both.append(np.linalg.norm(q-other))
        else:single.append(np.linalg.norm(c-xy))
    for name,r in [('single',single),('overlap',overlap),('T1T2',both)]:
        if r: print(k,name,len(r),np.percentile(r,[50,95]), 'within 1.5px',np.mean(np.array(r)<1.5))
