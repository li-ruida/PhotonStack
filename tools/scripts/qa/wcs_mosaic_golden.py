"""Regenerate the coordinate oracle for test_wcs_mosaic.py with Astropy from the repository root."""
import sys,runpy,numpy as np
from astropy.io import fits
from astropy.wcs import WCS
sys.argv.append('.')
m=runpy.run_path('tests/cli/test_wcs_mosaic.py'); ge=m['fixture']; w,h=m['W'],m['H']
inputs=[WCS(fits.Header(ge(i))) for i in (0,1)]
edge=np.r_[[[x,y] for x in range(w) for y in (0,h-1)],[[x,y] for y in range(h) for x in (0,w-1)]]
G={}
for preview in (0,48):
    s=preview/w if preview else 1.
    grid={k:v for k,v in ge(0).items() if not k.startswith(('A_','B_'))}
    for k in ['CTYPE1','CTYPE2']:grid[k]=grid[k][:-4]
    for k in ['CRPIX1','CRPIX2']:grid[k]=(grid[k]-.5)*s+.5
    for k in ['CD1_1','CD2_2']:grid[k]/=s
    out=WCS(fits.Header(grid))
    edges=np.concatenate([out.all_world2pix(z.all_pix2world(edge,0),0) for z in inputs])
    shift=-np.floor(edges.min(axis=0)); dims=np.ceil(edges.max(axis=0)+shift+1-.0001).astype(int)
    points=np.array([(int(x*dims[0]),int(y*dims[1])) for x,y in [(0,0),(.09,.4),(.5,.5),(.66,.5),(.82,.48),(.2,.72),(.90,.21),(.8,.85)]])
    world=out.all_pix2world(points-shift,0)
    vals=[[] for _ in points]
    for i,z in enumerate(inputs):
        for k,(x,y) in enumerate(z.all_world2pix(world,0,tolerance=1e-12)):
            wx=max(0,min(1,1+x,w-x)); wy=max(0,min(1,1+y,h-y))
            if wx*wy>0:
                color=1000*i+3*np.clip(x,0,w-1)-2*np.clip(y,0,h-1)
                vals[k].append((color,wx*wy))
    def sample(v):
        return (round(np.average([a for a,b in v],weights=[b for a,b in v]),6),
                round(np.mean([b for a,b in v]),6)) if v else (0,0)
    G[preview]=tuple(map(int,dims))+tuple((out.wcs.crpix+shift).tolist())+([(int(x),int(y),*sample(v)) for (x,y),v in zip(points,vals)],)
print(G)
