#!/usr/bin/env python3
"""Check common-grid line products with independent coordinates and star apertures.
Requires NumPy, SciPy and Astropy; reads existing products without changing them.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from astropy.io import fits
from scipy.ndimage import maximum_filter, map_coordinates
from scipy.spatial import cKDTree


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--legacy',required=True,type=Path)
    parser.add_argument('--aligned',required=True,type=Path)
    parser.add_argument('--source',type=Path,help='Single original CFA; omit for stacked products')
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    arrays={name:[fits.getdata(folder/(name+'.fits')).astype(float) for folder in (args.legacy,args.aligned)] for name in ('Ha','OIII')}
    metrics={}
    if args.source:
        raw,header=fits.getdata(args.source,header=True)
        h,w=raw.shape;yy,xx=np.mgrid[:h//2,:w//2]
        red=header['BAYERPAT'].strip().upper().index('R')^(int(header.get('XBAYROFF',0))%2)^((int(header.get('YBAYROFF',0))%2)<<1)
        rx,ry=red%2,red//2
        # Resample three independently extracted sublattices using SciPy, rather
        # than reproducing the engine's eight-neighbor stencil or streaming code.
        reconstructed=[]
        for py in range(2):
            for px in range(2):
                if (px,py)==(rx,ry):continue
                phase=raw[py::2,px::2].astype(float)
                reconstructed.append(map_coordinates(phase,[yy+(ry-py)/2,xx+(rx-px)/2],order=1,mode='constant',cval=np.nan,prefilter=False))
        expected=np.mean(reconstructed,axis=0).astype(np.float32)
        actual=arrays['OIII'][1]
        assert np.array_equal(expected,actual,equal_nan=True),'Independent coordinate oracle differs'
        assert np.array_equal(raw[ry:2*(h//2):2,rx:2*(w//2):2],arrays['Ha'][1])
        metrics['coordinate_oracle']='bit-exact Float32 samples and validity'
        metrics['source_sha256']=hashlib.sha256(args.source.read_bytes()).hexdigest()
    reference=arrays['Ha'][1]
    valid=np.logical_and.reduce([np.isfinite(a) for pair in arrays.values() for a in pair])
    med=np.median(reference[valid]);mad=1.4826*np.median(np.abs(reference[valid]-med))
    peaks=valid&(reference>med+15*mad)&(reference==maximum_filter(np.nan_to_num(reference,nan=-np.inf),size=25))
    ys,xs=np.nonzero(peaks);coords=np.column_stack([xs,ys])
    assert len(coords)>30
    distances=cKDTree(coords).query(coords,k=2)[0][:,1]
    dy,dx=np.mgrid[-11:12,-11:12];radius=np.hypot(dx,dy)
    aperture=radius<=6;annulus=(radius>=8)&(radius<=11)
    def measure(a,x,y):
        region=a[y-11:y+12,x-11:x+12]
        if not np.isfinite(region).all():return None
        background=np.median(region[annulus]);signal=region-background
        weights=np.maximum(signal,0)*aperture;mass=weights.sum();flux=signal[aperture].sum()
        if flux<2500 or mass<=0:return None
        cx=(dx*weights).sum()/mass;cy=(dy*weights).sum()/mass
        variance=(((dx-cx)**2+(dy-cy)**2)*weights).sum()/mass/2
        return np.array([flux,cx,cy,np.sqrt(variance)*2.35482,background])
    rows=[]
    for (x,y),separation in zip(coords,distances):
        if separation<24 or not (11<=x<reference.shape[1]-11 and 11<=y<reference.shape[0]-11):continue
        if reference[y,x]>30000:continue
        if np.count_nonzero(reference[y-2:y+3,x-2:x+3]>med+.15*(reference[y,x]-med))<4:continue
        values=[measure(arrays[name][mode],x,y) for mode in (0,1) for name in ('Ha','OIII')]
        if any(v is None for v in values):continue
        lh,lo,ah,ao=values
        rows.append([int(x),int(y),*(lo-lh)[1:3].tolist(),*(ao-ah)[1:3].tolist(),ao[0]/lo[0],ao[3]/lo[3],*(ah-lh)[1:3].tolist()])
    data=np.array(rows);assert len(data)>20
    metrics['stars']={'count':len(data),'aperture_radius':6,'annulus':[8,11],'minimum_peak_separation':24,
        'legacy_OIII_minus_Ha_median_xy':np.median(data[:,2:4],axis=0).tolist(),
        'aligned_OIII_minus_Ha_median_xy':np.median(data[:,4:6],axis=0).tolist(),
        'legacy_cross_line_distance_p95':float(np.percentile(np.hypot(data[:,2],data[:,3]),95)),
        'aligned_cross_line_distance_p95':float(np.percentile(np.hypot(data[:,4],data[:,5]),95)),
        'OIII_new_over_old_flux_median':float(np.median(data[:,6])),
        'OIII_flux_ratio_p10_p90':np.percentile(data[:,6],[10,90]).tolist(),
        'OIII_width_ratio_median':float(np.median(data[:,7])),
        'Ha_new_minus_old_median_xy':np.median(data[:,8:10],axis=0).tolist()}
    metrics['valid_pixels']=[int(np.isfinite(arrays['OIII'][i]).sum()) for i in (0,1)]
    metrics['star_rows_x_y_legacy_dxdy_aligned_dxdy_fluxratio_widthratio_Ha_dxdy']=rows
    args.output.write_text(json.dumps(metrics,indent=2))
    print(json.dumps({k:v for k,v in metrics.items() if not k.startswith('star_rows')},indent=2))


if __name__=='__main__':main()
