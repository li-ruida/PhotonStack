#!/usr/bin/env python3
"""Compare ordinary and CFA Drizzle masters in physical native-pixel coordinates.
Requires NumPy/SciPy/Astropy. Moment widths are not an independent resolution oracle.
Input directories use benchmark-deep-sky-resources.py layout; originals remain read-only.
"""
import argparse,hashlib,json
from pathlib import Path
import numpy as np
from astropy.io import fits
from scipy.ndimage import maximum_filter
parser=argparse.ArgumentParser(description=__doc__)
for name in ('native','drizzle','sources','output'): parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--scale',type=float,required=True)
parser.add_argument('--pixfrac',type=float,required=True)
args=parser.parse_args()
if not 1<=args.scale<=3 or not .1<=args.pixfrac<=1: parser.error('Invalid reconstruction geometry')
folders=(args.native,args.drizzle)
rs=[json.loads((n/'result/report.json').read_text()) for n in folders]
assert [f['input'] for f in rs[0]['frames']]==[f['input'] for f in rs[1]['frames']]
a,b=[fits.getdata(n/'result/stack-linear.fits') for n in folders]
x,y=[np.einsum('c,cyx->yx',[.2126,.7152,.0722],v[:3]) for v in (a,b)]
valid=(a[3]>.99)&np.isfinite(x)
sky=np.median(x[valid]);noise=np.median(abs(x[valid]-sky))*1.4826
peaks=valid&(x==maximum_filter(x,size=21))&(x>sky+10*noise)&(x<50000)
def measure(im,mask,cx,cy,scale):
 x0,x1=int((cx-12+.5)*scale),int((cx+12+.5)*scale)+1
 y0,y1=int((cy-12+.5)*scale),int((cy+12+.5)*scale)+1
 if x0<0 or y0<0 or x1>=im.shape[1] or y1>=im.shape[0]:return
 cut=im[y0:y1,x0:x1]; v=mask[y0:y1,x0:x1]
 yy,xx=np.mgrid[y0:y1,x0:x1];xx=(xx+.5)/scale-.5-cx;yy=(yy+.5)/scale-.5-cy
 rad=np.hypot(xx,yy); ap=rad<=6;ann=(rad>=9)&(rad<=12)
 if not v[rad<=12].all():return
 s=cut-np.median(cut[ann]);flux=s[ap].sum()/scale**2
 m=np.maximum(s,0)*ap;mass=m.sum()
 if flux<=0 or mass<=0:return
 dx=(m*xx).sum()/mass;dy=(m*yy).sum()/mass
 width=np.sqrt((m*((xx-dx)**2+(yy-dy)**2)).sum()/mass/2)*2.35482
 return np.array([flux,dx,dy,width])
stars=[]
for cy,cx in zip(*np.nonzero(peaks)):
 m=measure(x,valid,cx,cy,1);n=measure(y,(b[3]>0)&np.isfinite(y),cx,cy,args.scale)
 if m is None or n is None or not 1<m[3]<8:continue
 stars.append([n[0]/m[0],n[3]/m[3],np.hypot(n[1]-m[1],n[2]-m[2])])
s=np.array(stars)
sources=json.loads(args.sources.read_text())
def sha(p):
 h=hashlib.sha256()
 with open(p,'rb') as f:
  for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
 return h.hexdigest()
unchanged=all(sha(r['path'])==r['sha'] for r in sources);assert unchanged
result={'inputs':len(sources),'originalsUnchanged':unchanged,'registrationIdentical':all(f['registrationTransform']==g['registrationTransform'] for f,g in zip(rs[0]['frames'],rs[1]['frames'])),'selectedIdentical':all(f['selected']==g['selected'] for f,g in zip(rs[0]['frames'],rs[1]['frames'])),'scale':args.scale,'pixfrac':args.pixfrac,'coverageRGB':rs[1]['drizzleCoverage'],'completeRGB':rs[1]['drizzleCompleteCoverage'],'candidateStars':len(stars),'apertureRadiusNativePixels':6,'annulusNativePixels':[9,12],'fluxRatioMedian':float(np.median(s[:,0])),'fluxRatioP10P90':np.percentile(s[:,0],[10,90]).tolist(),'momentWidthRatioMedian':float(np.median(s[:,1])),'centroidDistanceNativePixelsP95':float(np.percentile(s[:,2],95)),'runs':[{k:v for k,v in json.loads((n/'summary.json').read_text()).items() if k in ['elapsed_seconds','resources','cli_sha256']} for n in folders],'interpretation':'Real uncalibrated M31 subset with unknown optimal dither sampling. Width changes alone do not establish real resolution gain. Surface-brightness photometry integrates output-pixel area (1/scale^2). Timing has unknown OS cache and other apps active.'}
args.output.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
