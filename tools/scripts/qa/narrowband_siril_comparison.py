#!/usr/bin/env python3
"""Compare same-scale CFA line products without interpreting appearance as accuracy.
Inputs must be in the same stored-row orientation. Use --siril-scale to declare
the conversion from normalized Siril output to detector counts. Requires
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
    parser.add_argument('--siril-scale',type=float,default=1)
    parser.add_argument('--edge-margin',type=int,default=32)
    args=parser.parse_args()
    assert np.isfinite(args.siril_scale) and args.siril_scale>0
    assert args.edge_margin>=0
    paths=[args.photonstack_ha,args.photonstack_oiii,args.siril_ha,args.siril_oiii]
    ha,ps,sh,si=[fits.getdata(p).astype(np.float64) for p in paths]
    sh*=args.siril_scale;si*=args.siril_scale
    assert ha.shape==ps.shape==sh.shape==si.shape and ha.ndim==2
    assert min(ha.shape)>2*args.edge_margin+20
    finite=np.logical_and.reduce([np.isfinite(a) for a in (ha,ps,sh,si)])
    assert finite.any(), 'No common finite samples'
    # Missing interpolation support is not black signal. Aggregate metrics use
    # common finite samples; FFT/morphology require a wholly finite interior.
    crop=(slice(args.edge_margin,-args.edge_margin or None),slice(args.edge_margin,-args.edge_margin or None))
    assert finite[crop].all(), 'Interior has missing support; choose a finite ROI or increase --edge-margin'

    metrics={'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'shape':ha.shape,
             'Ha_max_abs':float(np.max(np.abs((sh-ha)[np.isfinite(ha)&np.isfinite(sh)]))),'OIII_full_rms':float(np.sqrt(np.mean((si[finite]-ps[finite])**2))),
             'common_finite_samples':int(finite.sum()),'missing_samples_excluded':int(finite.size-finite.sum())}
    metrics['siril_scale']=args.siril_scale
    metrics['input_bitpix']={str(p):fits.getheader(p)['BITPIX'] for p in paths}
    metrics['edge_margin']=args.edge_margin
    common=finite.copy()
    margin=args.edge_margin
    if margin:
        common[:margin]=False;common[-margin:]=False;common[:,:margin]=False;common[:,-margin:]=False
    metrics['channels']={}
    for line,a,b in [('Ha',ha,sh),('OIII',ps,si)]:
        crop=(slice(margin,-margin or None),slice(margin,-margin or None))
        af=a[crop]-gaussian_filter(a[crop],3);bf=b[crop]-gaussian_filter(b[crop],3)
        cross=np.fft.rfft2(af)*np.conj(np.fft.rfft2(bf))
        corr=np.fft.irfft2(cross/np.maximum(np.abs(cross),1e-12),s=af.shape)
        dy,dx=np.unravel_index(np.argmax(corr),corr.shape)
        dy=int(dy if dy<af.shape[0]//2 else dy-af.shape[0]);dx=int(dx if dx<af.shape[1]//2 else dx-af.shape[1])
        d=(b-a)[common]
        metrics['channels'][line]={'highpass_correlation':float(np.corrcoef(af.ravel(),bf.ravel())[0,1]),
            'integer_shift_x_y':[dx,dy],'difference_median':float(np.median(d)),
            'difference_rms':float(np.sqrt(np.mean(d*d))),'difference_abs_p95':float(np.percentile(abs(d),95))}
    assert np.isfinite(ha).all(), 'H-alpha morphology input must be finite; select a finite ROI'
    bg=np.median(ha);mad=np.median(np.abs(ha-bg))*1.4826
    compact=(ha>bg+10*mad)&(ha==maximum_filter(ha,size=21))
    exclusion=binary_dilation(compact,iterations=12)
    smooth=gaussian_filter(ha,6)
    background=(smooth<np.percentile(smooth,20))&~exclusion
    # This is an explicitly defined candidate mask, not a catalog-confirmed
    # filament segmentation. Inspect masks before making a morphology claim.
    diffuse=gaussian_filter(ha,3)-gaussian_filter(ha,24)
    candidates=(diffuse>np.percentile(diffuse,97.5))&~exclusion
    for name,mask in [('low_brightness',background&common),('diffuse_candidates',candidates&common)]:
        d=si[mask]-ps[mask]
        metrics[name]={'pixels':int(mask.sum()),'photonstack_median':float(np.median(ps[mask])),
                       'siril_median':float(np.median(si[mask])),'difference_median':float(np.median(d)),
                       'difference_rms':float(np.sqrt(np.mean(d*d)))}
    yy,xx=np.mgrid[-9:10,-9:10];radius=np.hypot(xx,yy)
    aperture=radius<=4; annulus=(radius>=6)&(radius<=9)
    def measure(a,y,x):
        roi=a[y-9:y+10,x-9:x+10]
        if not np.isfinite(roi).all():return None
        b=np.median(roi[annulus]);signal=roi-b
        flux=signal[aperture].sum();positive=np.maximum(signal,0)*aperture
        mass=positive.sum()
        if flux<=0 or mass<=0:return None
        cx=(positive*xx).sum()/mass;cy=(positive*yy).sum()/mass
        width=np.sqrt((positive*((xx-cx)**2+(yy-cy)**2)).sum()/mass/2)*2.35482
        return np.array([flux,cx,cy,width])
    stars=[]
    for y,x in zip(*np.nonzero(compact)):
        if not common[y,x]:continue
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
    metrics['interpretation']='Algorithm comparison, not a calibration or SPCC acceptance. Diffuse candidates are not confirmed nebular filaments. No fitted photometric rescaling or subpixel registration has been applied.'
    args.output.write_text(json.dumps(metrics,indent=2))
    print(json.dumps({k:v for k,v in metrics.items() if k not in ('source_sha256','star_rows_x_y_fluxratio_widthratio_dx_dy')},indent=2))


if __name__=='__main__':main()
