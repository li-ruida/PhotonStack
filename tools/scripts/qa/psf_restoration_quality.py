#!/usr/bin/env python3
"""Known-scene noise and bright-star ring diagnostics; requires NumPy, SciPy, Astropy."""
import argparse
import numpy as np,json,subprocess
from pathlib import Path
from astropy.io import fits
from scipy.signal import convolve2d
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--cli',required=True);parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();root=args.output;root.mkdir(parents=True,exist_ok=False)
w,h=384,320;yy,xx=np.mgrid[:h,:w]
truth=100+8*np.exp(-.5*((xx-180)/50)**2-.5*((yy-165)/27)**2)+4*np.exp(-.5*((yy-100-.1*xx)/2)**2)
positions=[(32+64*x,32+64*y) for y in range(5) for x in range(6)]
for i,(x,y) in enumerate(positions):truth[y,x]+=200000 if i==14 else 4000+50*i
ky,kx=np.mgrid[-9:10,-9:10];kernel=np.exp(-.5*((kx*.9+ky*.43589)/1.8)**2-.5*((ky*.9-kx*.43589)/1.3)**2);kernel/=kernel.sum()
obs=convolve2d(truth,kernel,mode='same')/convolve2d(np.ones_like(truth),kernel,mode='same')
obs=np.minimum(obs+np.random.default_rng(42).normal(0,1.4,obs.shape),5000).astype(np.float32)
fits.writeto(root/'source.fits',obs);fits.writeto(root/'truth.fits',truth.astype(np.float32))
cli=args.cli
def run(*args):return json.loads(subprocess.check_output([cli,*map(str,args)],text=True))
measurement=run('psf-measure','--input',root/'source.fits','--output-dir',root/'psf','--saturation','5000')
near=np.zeros_like(truth,dtype=bool)
for x,y in positions:near|=(xx-x)**2+(yy-y)**2<16**2
background=(~near)&(truth<101);filament=(~near)&(truth>103)
bx,by=positions[14];rr=np.hypot(xx-bx,yy-by);ring=(rr>=6)&(rr<=15)
metrics={}
for name,noise,protect in [('plain',0,0),('damped',2,0),('protected',2,4000)]:
 r=run('psf-restore','--measurement',root/'psf/recipe.txt','--output-dir',root/name,'--iterations','16','--amount','1','--noise-sigma',noise,'--protect-above',protect)
 out=fits.getdata(root/name/'restored.fits')[0]
 metrics[name]={'backgroundRMSE':float(np.sqrt(np.mean((out[background]-truth[background])**2))),'filamentRMSE':float(np.sqrt(np.mean((out[filament]-truth[filament])**2))),'brightRingNegativeRMS':float(np.sqrt(np.mean(np.minimum(out[ring]-truth[ring],0)**2))),'protectedPixels':r['protectedPixels']}
metrics['input']={'backgroundRMSE':float(np.sqrt(np.mean((obs[background]-truth[background])**2))),'filamentRMSE':float(np.sqrt(np.mean((obs[filament]-truth[filament])**2))),'brightRingNegativeRMS':float(np.sqrt(np.mean(np.minimum(obs[ring]-truth[ring],0)**2)))}
(root/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n');print(json.dumps(metrics,indent=2))
assert metrics['damped']['backgroundRMSE']<metrics['plain']['backgroundRMSE']
assert metrics['protected']['brightRingNegativeRMS']<metrics['damped']['brightRingNegativeRMS']
