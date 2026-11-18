#!/usr/bin/env python3
"""Independently audit a real saved SPCC result, offline Gaia import and export.
Requires NumPy, SciPy, Astropy and Pillow. All supplied inputs remain read-only.
"""
import argparse
import csv
import json
from pathlib import Path
import shlex
import subprocess
import warnings
import numpy as np
from astropy.coordinates import SkyCoord
from astropy.io import fits
from astropy.time import Time
import astropy.units as u
from PIL import Image
from spectral_calibration_validation import integrate, photometry, audit_fit, sha


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ['directory','gaia-catalog','cli','output-dir']:
        parser.add_argument('--'+key,required=True,type=Path)
    parser.add_argument('--expect-rejected', action='store_true', help='Audit held-out rejection and refusal to export instead of a successful calibration')
    args=parser.parse_args(); root=args.directory; out=args.output_dir
    if out.exists(): raise ValueError('Choose a new audit directory')
    report=json.loads((root/'report.json').read_text()); source=Path(report['source'])
    original={str(p.resolve()):sha(p) for p in [source,args.gaia_catalog,*root.iterdir()]}
    assert original[str(source.resolve())]==report['sourceSHA']
    profile=iter(shlex.split((root/'profile.txt').read_text()))
    assert next(profile)=='PhotonStack-SpectralProfile-1'
    meta=[next(profile) for _ in range(5)]
    assert meta==[report['profile'][k] for k in ['name','mode','quality','provenance','license']]

    def curve(tokens):
        n=int(next(tokens)); xy=np.array([[float(next(tokens)),float(next(tokens))] for _ in range(n)])
        return xy[:,0],xy[:,1]

    bands=[[curve(profile) for _ in range(int(next(profile)))] for c in range(3)]
    white_meta=[next(profile) for _ in range(3)]; wx,wy=curve(profile)
    assert white_meta==[report['profile'][k] for k in ['whiteName','whiteProvenance','whiteLicense']]
    assert list(profile)==[]
    tokens=iter(shlex.split((root/'catalog.txt').read_text()))
    assert next(tokens)=='PhotonStack-SpectralCatalog-1'
    name=next(tokens); epoch=float(next(tokens)); provenance=next(tokens); license_text=next(tokens)
    assert [name,epoch,provenance,license_text]==[report['catalog'][k] for k in ['name','epoch','provenance','license']]
    stars={}
    for _ in range(int(next(tokens))):
        sid=next(tokens); ra,dec=float(next(tokens)),float(next(tokens)); n=int(next(tokens))
        spectrum=np.array([[float(next(tokens)) for _ in range(3)] for i in range(n)])
        stars[sid]=(ra,dec,spectrum)
    assert list(tokens)==[]
    catalog={s['source_id']:s for s in csv.DictReader(args.gaia_catalog.open())}
    originals=[catalog[sid] for sid in stars]
    ra,dec,ma,md=[np.array([float(s[k]) for s in originals]) for k in ['ra','dec','pmra','pmdec']]
    with warnings.catch_warnings(record=True) as caught:
        propagated=SkyCoord(ra=ra*u.deg,dec=dec*u.deg,pm_ra_cosdec=ma*u.mas/u.yr,pm_dec=md*u.mas/u.yr,
                            obstime=Time(2016,format='jyear'),frame='icrs').apply_space_motion(new_obstime=Time(epoch,format='jyear'))
    imported=SkyCoord(ra=[s[0] for s in stars.values()]*u.deg,dec=[s[1] for s in stars.values()]*u.deg,frame='icrs')
    motion_error=float(np.max(propagated.separation(imported).to_value(u.arcsec)))
    assert motion_error<.001, motion_error
    white=np.array([integrate(wx,wy,b)[0] for b in bands])
    np.testing.assert_allclose(report['whiteFlux'],white,rtol=3e-13)
    with fits.open(source) as f: pixels=np.array(f[0].data,dtype=float); source_header=f[0].header.copy()
    assert report['fit']['valid'] != args.expect_rejected, report['fit']['reason']
    candidates=[]; flux_differences=[]
    for star in report['stars']:
        sid=star['id']; ra,dec,spectrum=stars[sid]
        if any(v!=0 for v in star['predictedFlux']):
            signal=np.array([integrate(spectrum[:,0],spectrum[:,1],b)[0] for b in bands])
            error=np.array([integrate(spectrum[:,0],spectrum[:,2],b)[0] for b in bands])
            np.testing.assert_allclose(star['predictedFlux'],signal,rtol=3e-13)
            np.testing.assert_allclose(star['spectralErrorBound'],error,rtol=3e-13)
        for key,radius,inner,outer in [('photometry',8,11,15),('expandedPhotometry',10,13,17)]:
            p=star[key]
            if not p['valid']: continue
            measured=photometry(pixels,p['x'],p['y'],radius,inner,outer)
            for a,b in [('flux','flux'),('backgroundNoise','noise'),('skySigma','sigma'),('skyPlane','plane')]:
                np.testing.assert_allclose(p[a],measured[b],rtol=2e-8,atol=1e-8)
            flux_differences.append(float(np.max(np.abs(np.array(p['flux'])/measured['flux']-1))))
        if star['status'] in ['fit','fit-outlier','validation']:
            candidates.append(dict(id=sid,x=star['photometry']['x'],y=star['photometry']['y'],status='measured',
                                   predicted=star['predictedFlux'],observed=star['photometry']['flux']))
    fit_stars=sorted((s for s in report['stars'] if s['status'] in ['fit','fit-outlier','validation']),
                     key=lambda s:(s['photometry']['y'],s['photometry']['x'],s['id']))
    audit_fit(dict(stars=candidates,fit=dict(report['fit'],stars=fit_stars)),white,report['width'],report['height'])
    if args.expect_rejected:
        # audit_fit independently recomputes the held-out residuals above.
        assert max(report['fit']['validationP90']) > report['options']['maxValidationLogError']
        assert not (root/'calibrated.fits').exists() and not (root/'after.tiff').exists()
        out.mkdir(parents=True)
        inspected=json.loads(subprocess.check_output([str(args.cli),'spcc-inspect','--recipe',str(root/'recipe.txt')],text=True))
        assert inspected['report']==report
        subprocess.run([str(args.cli),'spcc-replay','--recipe',str(root/'recipe.txt'),'--output-dir',str(out/'replayed')],check=True,capture_output=True)
        for p in root.iterdir(): assert p.read_bytes()==(out/'replayed'/p.name).read_bytes()
        for command,name in [('spcc-export','exported.fits'),('spcc-preview','calibrated-display.tiff')]:
            attempted=subprocess.run([str(args.cli),command,'--recipe',str(root/'recipe.txt'),'--output',str(out/name)],capture_output=True,text=True)
            assert attempted.returncode != 0 and not (out/name).exists()
        assert all(sha(Path(p))==digest for p,digest in original.items())
        result=dict(maximumMotionDifferenceArcsec=motion_error,maximumRelativeApertureDifference=max(flux_differences),
                    rejected=True,rejectionIndependentlyVerified=True,replayAllFilesEqual=True,exportAndCalibratedPreviewRejected=True,
                    allInputHashesUnchanged=True,sourceSHA256=report['sourceSHA'],profileQuality=report['profile']['quality'],fit=report['fit'])
        (out/'audit.json').write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result,indent=2))
        return
    with fits.open(root/'calibrated.fits') as f: calibrated=np.array(f[0].data); output_header=f[0].header.copy()
    expected=pixels.astype(np.float32)
    for c,gain in enumerate(report['fit']['gains']): expected[c]=(pixels[c]*gain).astype(np.float32)
    np.testing.assert_array_equal(calibrated,expected)
    from astropy.wcs import WCS
    y,x=np.mgrid[0:report['height']:33j,0:report['width']:33j]
    points=np.column_stack([x.ravel(),y.ravel()]); sky=WCS(source_header,naxis=2).all_pix2world(points,0)
    np.testing.assert_allclose(WCS(output_header,naxis=2).all_world2pix(sky,0),points,rtol=0,atol=1e-5)
    # Both previews must use the documented one-source black reference/scale.
    # Check full native TIFF precision after independently reproducing the
    # engine's area resampling on this integer/rational resize geometry.
    preview_errors=[]
    for filename,data,gains in [('before.tiff',pixels,[1,1,1]),('after.tiff',calibrated,report['fit']['gains'])]:
        image=Image.open(root/filename); width,height=image.size; tags=image.tag_v2
        assert tags[259]==1 and set(tags[258])=={16} and tags.get(284,1)==1
        endian='<' if (root/filename).read_bytes()[:2]==b'II' else '>'
        raw=(root/filename).read_bytes()
        chunks=b''.join(raw[o:o+n] for o,n in zip(tags[273],tags[279]))
        preview=np.frombuffer(chunks,dtype=endian+'u2').reshape(height,width,4)
        # Sample 289 destination pixels; full science pixels are checked above.
        errors=[]; low=report['preview']['low']; span=max(report['preview']['high']-low,1e-12)
        for yy in np.linspace(0,height-1,17).astype(int):
            for xx in np.linspace(0,width-1,17).astype(int):
                x0,x1=xx*data.shape[2]/width,(xx+1)*data.shape[2]/width
                y0,y1=yy*data.shape[1]/height,(yy+1)*data.shape[1]/height
                accum=np.zeros(3); weight=0.
                for sy in range(int(np.floor(y0)),int(np.ceil(y1))):
                    for sx in range(int(np.floor(x0)),int(np.ceil(x1))):
                        w=max(0,min(sx+1,x1)-max(sx,x0))*max(0,min(sy+1,y1)-max(sy,y0))*data[3,sy,sx]
                        accum+=data[:3,sy,sx]*w; weight+=w
                if weight<=0: continue
                value=(accum/weight).astype(np.float32).astype(float)
                mapped=np.clip(np.arcsinh(10*np.maximum(0,(value-low*np.array(gains))/span))/np.arcsinh(10),0,1)
                integer=np.rint(mapped*65535)
                errors.append(float(np.max(np.abs(preview[yy,xx,:3].astype(float)-integer))))
        preview_errors.append(max(errors))
    # This is a strict display check, not a tolerance large enough to hide
    # separately recomputed black points or the former background-color bug.
    assert max(preview_errors)<=2, preview_errors
    out.mkdir(parents=True)
    def run(*words):
        return json.loads(subprocess.check_output([str(args.cli),*map(str,words)],text=True))
    inspected=run('spcc-inspect','--recipe',root/'recipe.txt','--expected-sha',report['sourceSHA'])
    assert inspected['report']==report
    run('spcc-replay','--recipe',root/'recipe.txt','--output-dir',out/'replayed')
    for p in root.iterdir(): assert p.read_bytes()==(out/'replayed'/p.name).read_bytes()
    run('spcc-export','--recipe',root/'recipe.txt','--output',out/'exported.fits')
    assert sha(out/'exported.fits')==sha(root/'calibrated.fits')
    # Main-canvas 1:1 viewing needs native dimensions, not the small review proxy.
    run('spcc-preview','--recipe',root/'recipe.txt','--output',out/'full-display.tiff','--full-resolution','1')
    full=Image.open(out/'full-display.tiff'); tags=full.tag_v2
    assert full.size==(report['width'],report['height']) and tags[259]==1 and set(tags[258])=={16}
    raw=(out/'full-display.tiff').read_bytes(); endian='<' if raw[:2]==b'II' else '>'
    payload=b''.join(raw[o:o+n] for o,n in zip(tags[273],tags[279]))
    full_pixels=np.frombuffer(payload,dtype=endian+'u2').reshape(report['height'],report['width'],4)
    full_errors=[]; covered=calibrated[3]>0
    for c,gain in enumerate(report['fit']['gains']):
        normalized=np.maximum(0,(calibrated[c].astype(float)-report['preview']['low']*gain)/max(report['preview']['high']-report['preview']['low'],1e-12))
        mapped=np.clip(np.arcsinh(10*normalized)/np.arcsinh(10),0,1).astype(np.float32)
        expected_integer=np.rint(mapped.astype(float)*65535)
        error=float(np.max(np.abs(full_pixels[:,:,c][covered].astype(float)-expected_integer[covered])))
        assert error<=1, (c,error)
        full_errors.append(error)
    assert np.max(np.abs(full_pixels[:,:,3].astype(float)-np.rint(np.clip(calibrated[3],0,1).astype(float)*65535)))<=1
    for p in [source,args.gaia_catalog,*root.iterdir()]: assert sha(p)==original[str(p.resolve())]
    result=dict(maximumMotionDifferenceArcsec=motion_error,motionReferenceWarnings=sorted({str(w.message) for w in caught}),
                maximumRelativeApertureDifference=max(flux_differences),previewMaximum16BitDifference=preview_errors,
                allSciencePixelsEqual=True,wcsGridVerified=True,replayAllFilesEqual=True,fullDisplayMaximum16BitDifference=full_errors,
                fullDisplaySize=list(full.size),exportSHA256=sha(out/'exported.fits'),
                sourceSHA256=report['sourceSHA'],profileQuality=report['profile']['quality'],fit=report['fit'])
    (out/'audit.json').write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result,indent=2))


if __name__=='__main__': main()
