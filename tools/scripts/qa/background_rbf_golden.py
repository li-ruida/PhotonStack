"""Generate an independent RBF oracle using SciPy's TPS and explicit held-out refits.
Run in a venv with scipy 1.13.1. No PhotonStack fitting or output is read.
"""
import json
from pathlib import Path
import sys
import tempfile
import numpy as np
import scipy
from scipy.interpolate import RBFInterpolator
root=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root/"tests/cli"))
from test_background_rbf import fixture,read,W,H
with tempfile.TemporaryDirectory() as t:
    path=Path(t);points=fixture(path);data=np.array(read(path/"source.fits")).reshape(4,H,W)
    coordinates=np.array([[x,y] for x,y,_,_ in points],dtype=float)/max(W,H)
    values=np.array([np.median(data[:3,y-r:y+r+1,x-r:x+r+1].reshape(3,-1),axis=1) for x,y,r,_ in points])
    result={"generator":"SciPy "+scipy.__version__+" RBFInterpolator; explicit leave-one-out refitting","cases":[]}
    positions=[(0,0),(159,127),(80,64),(12,12),(72,52),(31,75),(141,7),(63,99),(7,121)]
    for smoothing in (0,.001,.1):
        model=RBFInterpolator(coordinates,values,kernel="thin_plate_spline",degree=1,smoothing=smoothing)
        colors=model(np.array(positions,dtype=float)/max(W,H))
        loo=[]
        for i in (0,12,19,28,39,47):
            mask=np.arange(len(points))!=i
            held=RBFInterpolator(coordinates[mask],values[mask],kernel="thin_plate_spline",degree=1,smoothing=smoothing)
            error=values[i]-held(coordinates[i:i+1])[0]
            loo.append([i,error.tolist()])
        result["cases"].append(dict(smoothing=smoothing,pixels=[[x,y,c.tolist()] for (x,y),c in zip(positions,colors)],leaveOneOut=loo))
    (root/"tests/cli/background-rbf-golden.json").write_text(json.dumps(result,indent=2)+"\n")
