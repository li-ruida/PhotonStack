#!/usr/bin/env python3
"""Known-scene CFA reconstruction QA; requires NumPy, SciPy and Astropy.
New output directory only. Synthetic dense dithers are not real-data acceptance.
Compare scientific masters before finishing, at identical physical pixel centers.
"""
import argparse, json, subprocess, time, hashlib
from pathlib import Path
import numpy as np
from scipy.special import erf
from scipy.ndimage import map_coordinates
from astropy.io import fits
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--cli',type=Path,required=True)
args=parser.parse_args(); root=args.output.resolve();cli=args.cli.resolve();root.mkdir(parents=True,exist_ok=False)
w,h=240,192
rng=np.random.default_rng(571)
stars=[]
while len(stars)<35:
 x,y=rng.uniform(14,w-14),rng.uniform(14,h-14)
 if all((x-a)**2+(y-b)**2>15**2 for a,b,_,_ in stars):stars.append((x,y,rng.uniform(500,1500),rng.uniform(.65,.85)))
y,x=np.mgrid[:h,:w]
def signal(x,y,dx=0,dy=0,aperture=1):
 out=np.full(x.shape,100.)
 for a,b,flux,sig in stars:
  k=2**.5*sig
  ex=erf((x-dx-a+aperture/2)/k)-erf((x-dx-a-aperture/2)/k)
  ey=erf((y-dy-b+aperture/2)/k)-erf((y-dy-b-aperture/2)/k)
  out+=flux*ex*ey/(4*aperture**2)
 return out
paths=[];shifts=[]
for j,dy in enumerate(np.arange(0,2,.25)):
 for i,dx in enumerate(np.arange(0,2,.25)):
  n=j*8+i;p=root/f'input-{n:03}.fits'; paths.append(p);shifts.append([float(dx),float(dy)])
  if not p.exists():
   header=fits.Header({'BAYERPAT':'RGGB','INSTRUME':'synthetic','GAIN':100,'EXPTIME':30,'BUNIT':'ADU','PSCOLOR':'Linear'})
   fits.writeto(p,(signal(x,y,dx,dy)+rng.normal(0,.03,x.shape)).astype(np.float32),header)
sources=[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in paths]
(root/'sources.json').write_text(json.dumps(sources,indent=2))
base={'stack.method':'average','stack.median-mad':'off','stack.rejection-maps':'off','stack.normalize-background':'off','registration.mode':'similarity','registration.reference':'0','registration.minimum-matches':'5','registration.refine-centroids':'on','registration.match-tolerance':'2','decode.demosaic':'bilinear','selection.apply':'off','selection.measure-trails':'off','background.model':'none','crop.coverage':'1','develop.align-channels':'off','develop.stellar-balance':'off','stack.drizzle-scale':'2','stack.drizzle-pixfrac':'.5'}
for name,on in [('native','off'),('drizzle','on')]:
 recipe=root/(name+'.txt'); values=base|{'stack.cfa-drizzle':on}
 recipe.write_text('PHOTONSTACK_DEEP_SKY_RECIPE 1\n'+''.join(k+' '+json.dumps(v)+'\n' for k,v in values.items())+''.join('input '+json.dumps(str(p))+'\n' for p in paths))
 if not (root/name/'report.json').exists():
  with (root/(name+'.log')).open('w') as log:
   r=subprocess.run([cli,'deep-sky','--recipe',recipe,'--output-dir',root/name,'--compute-backend','cpu','--analysis-workers','4'],stdout=log,stderr=subprocess.STDOUT)
  if r.returncode:
   report=json.loads((root/name/'report.json').read_text());raise Exception((report['errorCode'],report['message']))
reports={n:json.loads((root/n/'report.json').read_text()) for n in ['native','drizzle']}
yy,xx=np.mgrid[:h*2,:w*2]; xx=(xx+.5)/2-.5; yy=(yy+.5)/2-.5
truth=signal(xx,yy,aperture=.5)
a=fits.getdata(root/'native/stack-linear.fits');b=fits.getdata(root/'drizzle/stack-linear.fits')
upsample=np.stack([map_coordinates(p,[yy,xx],order=1,mode='nearest') for p in a[:3]])
mask=(xx>10)&(xx<w-10)&(yy>10)&(yy<h-10)&(b[3]>0)
bright=mask&(truth>102)
def metrics(im):
 err=im[:3]-truth
 return {'rms_all':float(np.sqrt(np.mean(err[:,mask]**2))),'rms_signal':float(np.sqrt(np.mean(err[:,bright]**2))),'absolute_bias':float(np.mean(err[:,mask]))}
trans=[]
for f,(dx,dy) in zip(reports['drizzle']['frames'],shifts):
 t=f['registrationTransform'];trans.append(((t['dx']+dx)**2+(t['dy']+dy)**2)**.5)
result={'fixture':'64 physical pixel-integrated Gaussian CFA exposures, 8x8 dither phases; sigma 0.65..0.85 native pixels; deterministic read noise 0.03 ADU','reference':'independent analytic scene integrated over 0.5 native pixel at output centers','native_bilinear_then_bilinear_resize':metrics(upsample),'cfa_drizzle':metrics(b),'registration_shift_error_median':float(np.median(trans)),'registration_shift_error_max':max(trans),'coverage':reports['drizzle']['drizzleCoverage'],'complete_coverage':reports['drizzle']['drizzleCompleteCoverage'],'limits':'Synthetic known PSF and dense dithers; does not prove real capture resolution gain.'}
assert all(hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()==item['sha256'] for item in sources)
result['engineSHA256']=hashlib.sha256(cli.read_bytes()).hexdigest()
result['sourceHashesUnchanged']=True
assert result['cfa_drizzle']['rms_signal'] < result['native_bilinear_then_bilinear_resize']['rms_signal']*.5
(root/'validation.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
