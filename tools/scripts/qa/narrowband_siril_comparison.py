#!/usr/bin/env python3
"""Compare same-scale CFA line products without interpreting appearance as accuracy.
Inputs must be in the same count scale and stored-row orientation. Requires
NumPy, SciPy and Astropy. Does not run Siril or modify its configuration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from astropy.io import fits
from scipy.ndimage import gaussian_filter, maximum_filter, binary_dilation


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('photonstack-ha','photonstack-oiii','siril-ha','siril-oiii','output'):
        parser.add_argument('--'+name,required=True,type=Path)
    args=parser.parse_args()
    paths=[args.photonstack_ha,args.photonstack_oiii,args.siril_ha,args.siril_oiii]
    ha,ps,sh,si=[fits.getdata(p).astype(np.float64) for p in paths]
    assert ha.shape==ps.shape==sh.shape==si.shape
    assert all(np.isfinite(a).all() for a in (ha,ps,sh,si))
    metrics={'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'shape':ha.shape,
             'Ha_max_abs':float(np.max(np.abs(sh-ha))),'OIII_full_rms':float(np.sqrt(np.mean((si-ps)**2)))}
    bg=np.median(ha);mad=np.median(np.abs(ha-bg))*1.4826
    compact=(ha>bg+10*mad)&(ha==maximum_filter(ha,size=21))
    exclusion=binary_dilation(compact,iterations=12)
    smooth=gaussian_filter(ha,6)
    background=(smooth<np.percentile(smooth,20))&~exclusion
    # This is an explicitly defined candidate mask, not a catalog-confirmed
    # filament segmentation. Inspect masks before making a morphology claim.
    diffuse=gaussian_filter(ha,3)-gaussian_filter(ha,24)
    candidates=(diffuse>np.percentile(diffuse,97.5))&~exclusion
    for name,mask in [('low_brightness',background),('diffuse_candidates',candidates)]:
        d=si[mask]-ps[mask]
        metrics[name]={'pixels':int(mask.sum()),'photonstack_median':float(np.median(ps[mask])),
                       'siril_median':float(np.median(si[mask])),'difference_median':float(np.median(d)),
                       'difference_rms':float(np.sqrt(np.mean(d*d)))}
    yy,xx=np.mgrid[-9:10,-9:10];radius=np.hypot(xx,yy)
    aperture=radius<=4; annulus=(radius>=6)&(radius<=9)
    def measure(a,y,x):
        roi=a[y-9:y+10,x-9:x+10];b=np.median(roi[annulus]);signal=roi-b
        flux=signal[aperture].sum();positive=np.maximum(signal,0)*aperture
        mass=positive.sum()
        if flux<=0 or mass<=0:return None
        cx=(positive*xx).sum()/mass;cy=(positive*yy).sum()/mass
        width=np.sqrt((positive*((xx-cx)**2+(yy-cy)**2)).sum()/mass/2)*2.35482
        return np.array([flux,cx,cy,width])
    stars=[]
    for y,x in zip(*np.nonzero(compact)):
        if y<9 or x<9 or y+9>=ha.shape[0] or x+9>=ha.shape[1] or ha[y,x]>=30000:continue
        if np.count_nonzero(ha[y-2:y+3,x-2:x+3]>bg+.15*(ha[y,x]-bg))<4:continue
        a,b=measure(ps,y,x),measure(si,y,x)
        if a is None or b is None or min(a[0],b[0])<1000:continue
        stars.append([int(x),int(y),*(b/a)[[0,3]].tolist(),*(b-a)[[1,2]].tolist()])
    values=np.array(stars)
    assert len(stars)>20
    metrics['unsaturated_stars']={'count':len(stars),'aperture_radius':4,'background_annulus':[6,9],
        'flux_ratio_median':float(np.median(values[:,2])),
        'flux_ratio_p10_p90':np.percentile(values[:,2],[10,90]).tolist(),
        'width_ratio_median':float(np.median(values[:,3])),
        'centroid_dx_dy_median':np.median(values[:,4:6],axis=0).tolist(),
        'centroid_distance_p95':float(np.percentile(np.hypot(values[:,4],values[:,5]),95))}
    metrics['star_rows_x_y_fluxratio_widthratio_dx_dy']=stars
    args.output.write_text(json.dumps(metrics,indent=2))
    print(json.dumps({k:v for k,v in metrics.items() if k not in ('source_sha256','star_rows_x_y_fluxratio_widthratio_dx_dy')},indent=2))


if __name__=='__main__':main()
