#!/usr/bin/env python3
"""Independent SciPy/Astropy checks for master registration.
The real input is read-only. Generated moving image and synthetic WCS fixtures
are written only to a new output folder; injected motion is not a second night.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np
from astropy.io import fits
from astropy.wcs import WCS
from scipy.ndimage import map_coordinates,maximum_filter
from scipy.spatial import cKDTree


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli',required=True,type=Path)
    parser.add_argument('--input',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();out=args.output;out.mkdir(parents=True,exist_ok=False)
    before=digest(args.input);raw=fits.getdata(args.input).astype(float);h,w=raw.shape
    yy,xx=np.mgrid[:h,:w]
    truth=np.array([1.006,.013,6.35,-4.27]);scale,angle,dx,dy=truth;c=np.cos(angle);s=np.sin(angle)
    rx=scale*(c*xx-s*yy)+dx;ry=scale*(s*xx+c*yy)+dy
    injected=(.8*map_coordinates(raw,[ry,rx],order=1,mode='constant',cval=np.nan,prefilter=False)+17+.003*xx-.002*yy).astype(np.float32)
    source=out/'moving.fits';fits.writeto(source,injected)
    def run(name,mode,reference,moving):
        result=subprocess.run(['/usr/bin/time','-l',str(args.cli),'channel-register','--mode',mode,'--reference','Reference',
            '--input',f'Reference={reference}','--input',f'Moving={moving}','--output-dir',str(out/name)],capture_output=True,text=True)
        (out/(name+'.time')).write_text(result.stderr)
        assert result.returncode==0,result.stderr
        report=json.loads(result.stdout);(out/(name+'.json')).write_text(json.dumps(report,indent=2));return report
    report=run('stars','stars',args.input,source);m=report['masters'][1]
    estimated=np.array(m['transform']);scale,angle,dx,dy=estimated;c=np.cos(angle);s=np.sin(angle)
    corners=np.array([[0,0],[w-1,0],[0,h-1],[w-1,h-1],[w/2,h/2]])
    def transformed(t):
        scale,angle,dx,dy=t;c=np.cos(angle);s=np.sin(angle)
        return scale*corners@np.array([[c,s],[-s,c]])+np.array([dx,dy])
    geometry=np.linalg.norm(transformed(estimated)-transformed(truth),axis=1)
    assert geometry.max()<.1,geometry
    sx=(c*(xx-dx)+s*(yy-dy))/scale;sy=(-s*(xx-dx)+c*(yy-dy))/scale
    expected=map_coordinates(injected.astype(float),[sy,sx],order=1,mode='constant',cval=np.nan,prefilter=False).astype(np.float32)
    actual=fits.getdata(out/'stars/Moving.fits')
    assert np.array_equal(np.isfinite(actual),np.isfinite(expected))
    valid=np.isfinite(expected);difference=np.abs(actual[valid]-expected[valid])
    assert np.allclose(actual[valid],expected[valid],rtol=1e-6,atol=1e-4)
    # Independent aperture centroids, not the engine's PSF fits. Avoid blended
    # peaks and use identical sky-subtracted apertures on the physical images.
    finite=np.isfinite(raw);med=np.median(raw[finite]);mad=1.4826*np.median(np.abs(raw[finite]-med))
    py,px=np.nonzero(finite&(raw>med+15*mad)&(raw==maximum_filter(np.nan_to_num(raw,nan=-np.inf),size=25)))
    coords=np.column_stack((px,py));distance=cKDTree(coords).query(coords,k=2)[0][:,1]
    ay,ax=np.mgrid[-10:11,-10:11];r=np.hypot(ax,ay);aperture=r<=6;ring=(r>=8)&(r<=10)
    def centroid(image,x,y):
        region=image[y-10:y+11,x-10:x+11]
        if region.shape!=(21,21) or not np.isfinite(region).all():return None
        signal=np.maximum(region-np.median(region[ring]),0)*aperture
        flux=signal.sum()
        if flux<=0:return None
        return np.array([(signal*ax).sum()/flux,(signal*ay).sum()/flux])
    residual=[]
    for (x,y),separation in zip(coords,distance):
        if separation<24 or x<12 or y<12 or x>=w-12 or y>=h-12 or raw[y,x]>30000:continue
        a=centroid(raw,x,y);b=centroid(actual,x,y)
        if a is not None and b is not None:residual.append(np.linalg.norm(a-b))
    assert len(residual)>20
    # Resampling changes undersampled PSFs. Record spread, not only fit residual.
    centroid_metrics={'count':len(residual),'median':float(np.median(residual)),'p95':float(np.percentile(residual,95))}
    assert centroid_metrics['median']<.1 and centroid_metrics['p95']<.3,centroid_metrics
    hw,ww=128,160;wy,wx=np.mgrid[:hw,:ww]
    hdr={'CTYPE1':'RA---TAN-SIP','CTYPE2':'DEC--TAN-SIP','CRPIX1':80.5,'CRPIX2':64.5,'CRVAL1':359.98,'CRVAL2':70.,
         'CD1_1':-.003,'CD1_2':0.,'CD2_1':0.,'CD2_2':.003,'A_ORDER':2,'B_ORDER':2,'A_2_0':1e-5,'B_0_2':-1e-5}
    other={**hdr,'CRPIX1':84.2,'CRPIX2':61.1,'CRVAL1':.0005,'CD1_1':-.003*np.cos(.2),'CD1_2':.003*np.sin(.2),'CD2_1':.003*np.sin(.2),'CD2_2':.003*np.cos(.2),'A_2_0':2e-5}
    reference=out/'sky-reference.fits';moving=out/'sky-moving.fits'
    fits.writeto(reference,np.ones((hw,ww),np.float32),fits.Header(hdr))
    values=(2*wx-3*wy-17).astype(np.float32);values[30:33,40:45]=np.nan;values[:2]=np.nan
    fits.writeto(moving,values,fits.Header(other))
    wcs_report=run('wcs','wcs',reference,moving)
    sky=WCS(fits.Header(hdr));target=WCS(fits.Header(other))
    world=sky.all_pix2world(wx,wy,0);mx,my=target.all_world2pix(*world,0,tolerance=1e-12,maxiter=100)
    oracle=map_coordinates(values.astype(float),[my,mx],order=1,mode='constant',cval=np.nan,prefilter=False)
    actual=fits.getdata(out/'wcs/Moving.fits');valid=np.isfinite(oracle)
    assert np.array_equal(np.isfinite(actual),valid)
    wcs_error=float(np.max(np.abs(actual[valid]-oracle[valid])));assert wcs_error<1e-4,wcs_error
    written=WCS(fits.getheader(out/'wcs/Moving.fits'))
    wcs_delta=float(np.max(np.abs(np.array(sky.all_pix2world(wx,wy,0))-np.array(written.all_pix2world(wx,wy,0)))))
    assert wcs_delta<1e-10
    assert digest(args.input)==before
    metrics={'source_sha256':before,'known_transform':truth.tolist(),'measured_transform':estimated.tolist(),
        'transform_error_at_corners_and_center':geometry.tolist(),'resampling_max_abs':float(difference.max()),
        'independent_aperture_centroids':centroid_metrics,'rotated_sip_oracle_max_abs':wcs_error,'saved_wcs_error_degrees':wcs_delta,
        'wcs_check_stars':wcs_report['masters'][1]['checkStars']}
    (out/'metrics.json').write_text(json.dumps(metrics,indent=2));print(json.dumps(metrics,indent=2))

if __name__=='__main__':main()
