#!/usr/bin/env python3
"""Independent NumPy/Astropy oracle and CPU benchmark; generated data only.

Usage: python script CLI NEW_OUTPUT_DIRECTORY
Produces all-pixel comparisons, deterministic worker/tile parity, cancellation,
input digests, and a 12 x 2160 x 3840 raw dark benchmark. Never modifies real data.
"""
import hashlib
import json
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import time
import warnings
import numpy as np
from astropy.io import fits


def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()


def header(role, index=0, mono=False):
    values={'INSTRUME':'Simulated detector','CAM-SER':'SIM-ONLY','CCD-TEMP':-10+index*.01,
            'DATE-OBS':f'2026-10-15T22:00:{index:02d}','GAIN':100,'OFFSET':20,'READMODE':'normal',
            'XBINNING':1,'YBINNING':1,'XORGSUBF':0,'YORGSUBF':0,'BUNIT':'ADU','PSCOLOR':'Linear',
            'IMAGETYP':role,'EXPTIME':0 if role=='Bias' else .2 if role=='Flat' else 60,
            'FILTER':'Ha','PSOPTICS':'simulated-train'}
    values.update({'PSLAYOUT':'MONO'} if mono else {'BAYERPAT':'GBRG','XBAYROFF':1,'YBAYROFF':0})
    return fits.Header(values)


def main():
    cli=Path(sys.argv[1]).resolve(strict=True);out=Path(sys.argv[2]).absolute();out.mkdir(parents=True,exist_ok=False)
    originals={};results=[]
    def source(name,data,role,index=0,mono=False):
        p=out/name;fits.writeto(p,data,header(role,index,mono));originals[p]=digest(p);return p
    def manifest(name,frames,role,correction=None):
        path=out/(name+'.txt')
        text='PhotonStack-SensorMaster-1\nrole '+role+'\nmethod median\n'
        if correction:text+='bias '+json.dumps(str(correction))+' '+digest(correction)+'\n'
        text+=''.join('frame '+json.dumps(str(p))+' '+originals[p]+'\n' for p in frames)
        path.write_text(text);return path
    def build(name,m,threads=4,tile=65536,memory=128):
        target=out/name
        cmd=[str(cli),'sensor-master-build','--manifest',str(m),'--output-dir',str(target),
             '--threads',str(threads),'--tile-pixels',str(tile),'--memory-mib',str(memory)]
        start=time.perf_counter();run=subprocess.run(['/usr/bin/time','-l',*cmd],capture_output=True,text=True,check=True)
        elapsed=time.perf_counter()-start
        (out/(name+'-stderr.txt')).write_text(run.stderr)
        rss=int(next(x.split()[0] for x in run.stderr.splitlines() if 'maximum resident set size' in x))
        report=json.loads((target/'report.json').read_text())
        results.append(dict(name=name,seconds=elapsed,peakRSSBytes=rss,workingBytes=report['workingBytes'],
                            workers=report['workers'],tilePixels=report['tilePixels']))
        return target,report
    def compare(directory,values):
        cube=np.stack(values)
        valid=np.isfinite(cube);counts=valid.sum(axis=0)
        with warnings.catch_warnings():
            warnings.simplefilter('ignore',RuntimeWarning)
            expected=np.nanmedian(np.where(valid,cube,np.nan),axis=0).astype(np.float32)
        expected[counts<2]=np.nan
        actual=fits.getdata(directory/'master.fits');accepted=fits.getdata(directory/'accepted.fits')
        assert np.array_equal(np.isnan(actual),np.isnan(expected))
        assert np.array_equal(accepted,counts)
        delta=np.abs(actual.astype(np.float64)-expected.astype(np.float64));finite=np.isfinite(delta)
        maximum=float(np.max(delta[finite]));assert maximum<=2e-5,maximum
        return dict(pixels=actual.size,invalid=int(np.isnan(actual).sum()),maximumAbsoluteError=maximum)
    rng=np.random.default_rng(777);shape=(256,384);y,x=np.indices(shape)
    pedestal=100+5*np.sin(x/20)+2*np.cos(y/17)
    biasValues=[(pedestal+rng.normal(0,2,shape)).astype(np.float32) for _ in range(7)]
    biasFrames=[source(f'bias-{i}.fits',v,'Bias',i) for i,v in enumerate(biasValues)]
    biasDir,_=build('bias',manifest('bias',biasFrames,'bias'))
    audits={'bias':compare(biasDir,biasValues)};bias=fits.getdata(biasDir/'master.fits')
    darkValues=[(pedestal-3+rng.normal(0,1,shape)).astype(np.float32) for _ in range(9)]
    for i,v in enumerate(darkValues):v[2,2]=np.nan;v[3,3]=np.inf if i<8 else 102
    darkValues[4][100,100]=50000 # a transient defect; median retains the central signal
    darkFrames=[source(f'dark-{i}.fits',v,'Dark',i) for i,v in enumerate(darkValues)]
    darkDir,_=build('dark',manifest('dark',darkFrames,'dark',biasDir/'master.fits'))
    corrected=[(v.astype(np.float64)-bias).astype(np.float32) for v in darkValues]
    audits['dark']=compare(darkDir,corrected)
    assert np.nanmedian(fits.getdata(darkDir/'master.fits'))<0
    phase=2*(y%2)+x%2;response=(1-.25*((x-shape[1]/2)/(shape[1]/2))**2)*(1-.1*(y/shape[0]))
    flatValues=[]
    for i,factor in enumerate([.7,.8,1,1.2,1.4]):
        v=(pedestal+factor*np.take([1500,2500,3000,1000],phase)*response+rng.normal(0,.3,shape)).astype(np.float32)
        v[5,5]=np.nan;v[6,6]=0;flatValues.append(v)
    flatFrames=[source(f'flat-{i}.fits',v,'Flat',i) for i,v in enumerate(flatValues)]
    flatDir,flatReport=build('flat',manifest('flat',flatFrames,'flat',biasDir/'master.fits'),tile=19)
    corrected=[(v.astype(np.float64)-bias).astype(np.float32) for v in flatValues]
    means=[]
    for v in corrected:
        v[(v<=0)|~np.isfinite(v)]=np.nan
        means.append([float(np.nanmean(v[phase==q],dtype=np.float64)) for q in range(4)])
    factors=np.asarray(means[0])/np.asarray(means)
    np.testing.assert_allclose(flatReport['factors'],factors,rtol=1e-13,atol=0)
    normalized=[(v.astype(np.float64)*f[phase]).astype(np.float32) for v,f in zip(corrected,factors)]
    audits['flat']=compare(flatDir,normalized)
    assert fits.getheader(flatDir/'master.fits')['BAYERPAT']=='BGGR'
    # Single worker / different tile layout must produce identical scientific bytes.
    flatOther,_=build('flat-other',out/'flat.txt',threads=1,tile=32768)
    for name in ['master.fits','accepted.fits']:assert digest(flatDir/name)==digest(flatOther/name)
    # Native-sized benchmark, no background normalization and no registered stars.
    shape=(3840,2160);frames=[]
    for i in range(12):
        values=(100+rng.normal(0,2,shape)).astype(np.float32)
        values[100:103,100:103]+=20
        frames.append(source(f'benchmark-{i:02d}.fits',values,'Dark',i))
    m=manifest('benchmark',frames,'dark');products=[]
    for i,workers in enumerate([1,4,4,4]):
        folder,report=build('benchmark-'+str(i),m,threads=workers)
        products.append(digest(folder/'master.fits'))
        assert report['workingBytes']<=128*1024*1024
    assert len(set(products))==1
    # Interrupt a known live process; wait for its terminal status before checking cleanup.
    cancelOut=out/'cancelled'
    p=subprocess.Popen([str(cli),'sensor-master-build','--manifest',str(m),'--output-dir',str(cancelOut)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    time.sleep(.2);assert p.poll() is None;p.send_signal(signal.SIGTERM);stdout,stderr=p.communicate(timeout=15)
    assert p.returncode!=0 and 'cancelled' in stderr and not cancelOut.exists() and not list(out.glob('.sensor-master-*'))
    # A destination created after launch must survive exclusive publication.
    raced=out/'concurrent-result'
    p=subprocess.Popen([str(cli),'sensor-master-build','--manifest',str(m),'--output-dir',str(raced)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    time.sleep(.2);assert p.poll() is None
    raced.mkdir();(raced/'keep').write_text('other writer')
    stdout,stderr=p.communicate(timeout=30)
    assert p.returncode!=0 and (raced/'keep').read_text()=='other writer'
    assert sorted(x.name for x in raced.iterdir())==['keep'] and not list(out.glob('.sensor-master-*'))
    for path,sha in originals.items():assert digest(path)==sha
    summary=dict(generatedDataOnly=True,audits=audits,benchmarkFrames=12,benchmarkShape=list(shape),
                 benchmarkOutputSHA=products[0],runs=results,sourceDigestsUnchanged=True,cancellationPassed=True,concurrentDestinationPreserved=True,
                 warmFourWorkerMedianSeconds=statistics.median(r['seconds'] for r in results if r['name'] in ['benchmark-1','benchmark-2','benchmark-3']))
    (out/'audit.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))


if __name__=='__main__':main()
