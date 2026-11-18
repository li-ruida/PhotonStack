#!/usr/bin/env python3
"""Compare GHS gray ramps against an installed Siril CLI, using isolated config."""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np
from astropy.io import fits

p=argparse.ArgumentParser();p.add_argument('cli',type=Path);p.add_argument('siril',type=Path);p.add_argument('config',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
if a.output.exists():raise SystemExit('Choose a new output directory')
a.output.mkdir();(a.output/'siril.cfg').write_bytes(a.config.read_bytes())
fits.writeto(a.output/'ramp.fits',np.linspace(0,1,4096,dtype=np.float32).reshape(64,64))
# Siril 1.4.2 CLI rejects negative B, despite newer manual advertising it.
# Negative branches and zero strength are independently checked by the NumPy oracle and native limits tests.
cases=[(2,b,.2,.1,.8) for b in [0,.1,1,3,15]]+[(5,3,0,0,1),(7,1,.5,.2,.7)]
script=['requires 1.4.0']
for i,(d,b,sp,lp,hp) in enumerate(cases):
    script+=['load ramp.fits',f'ght -D={d} -B={b} -SP={sp} -LP={lp} -HP={hp}',f'save siril-{i}.fit','close']
(a.output/'compare.ssf').write_text('\n'.join(script)+'\n')
run=subprocess.run([str(a.siril.resolve()),'-d',str(a.output.resolve()),'-i',str((a.output/'siril.cfg').resolve()),'-s',str((a.output/'compare.ssf').resolve())],text=True,capture_output=True)
(a.output/'siril.log').write_text(run.stdout+run.stderr);run.check_returncode();results=[]
for i,(d,b,sp,lp,hp) in enumerate(cases):
    subprocess.run([str(a.cli.resolve()),'ghs-stretch','--input',str(a.output/'ramp.fits'),'--output-dir',str(a.output/f'photon-{i}'),'--log-stretch',str(d),'--local-intensity',str(b),'--symmetry',str(sp),'--shadow-protection',str(lp),'--highlight-protection',str(hp)],check=True,capture_output=True)
    truth=fits.getdata(a.output/f'siril-{i}.fit');actual=fits.getdata(a.output/f'photon-{i}/display.fits')[0]
    delta=np.abs(actual-truth);maximum=float(delta.max());rms=float(np.sqrt(np.mean(np.square(delta,dtype=np.float64))))
    assert maximum<1e-6,(i,maximum)
    results.append(dict(logStretch=d,b=b,sp=sp,lp=lp,hp=hp,max_abs=maximum,rms=rms))
(a.output/'validation.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))

# Independently evaluate all three mathematical branches, including b=-1 and D=0.
from generalized_stretch_validation import curve
ramp=fits.getdata(a.output/'ramp.fits').astype(np.float64)
oracle=[]
for i,(d,b) in enumerate([(2,b) for b in [-5,-2,-1,-.1,0,.1,1,3,15]]+[(0,1)]):
    dest=a.output/f'numpy-{i}'
    subprocess.run([str(a.cli.resolve()),'ghs-stretch','--input',str(a.output/'ramp.fits'),'--output-dir',str(dest),'--log-stretch',str(d),'--local-intensity',str(b),'--symmetry','.2','--shadow-protection','.1','--highlight-protection','.8'],check=True,capture_output=True)
    expected=curve(ramp,d,b,.2,.1,.8).astype(np.float32)
    actual=fits.getdata(dest/'display.fits')[0]
    maximum=float(np.abs(actual-expected).max());assert maximum<6e-8,(b,maximum)
    oracle.append(dict(logStretch=d,b=b,max_abs=maximum))
(a.output/'numpy-validation.json').write_text(json.dumps(oracle,indent=2)+'\n')
