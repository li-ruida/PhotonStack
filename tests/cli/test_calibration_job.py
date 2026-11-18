"""Independent sensor arithmetic, explicit assignments and durable per-frame jobs."""
import hashlib
import json
import math
import os
import fcntl
import signal
import subprocess
from pathlib import Path
import struct
import tempfile
import unittest
from test_channels import CLI, read, run, write
from test_sensor_master import frame, cards


def manifest(root, lights, masters, name='manifest.txt', settings=''):
    path = root / name
    text = 'PhotonStack-CalibrationJob-1\n' + settings
    for identifier, role, source, state in masters:
        text += f'master {identifier} {role} {json.dumps(str(source))} {hashlib.sha256(source.read_bytes()).hexdigest()} {state}\n'
    for source, bias, dark, flat in lights:
        text += f'light {json.dumps(str(source))} {hashlib.sha256(source.read_bytes()).hexdigest()} {bias} {dark} {flat}\n'
    path.write_text(text)
    return path


class CalibrationJobs(unittest.TestCase):
    def assertPixels(self, path, expected):
        actual = read(path)[1][0]
        self.assertEqual(len(actual), len(expected))
        for a, b in zip(actual, expected):
            if math.isnan(b): self.assertTrue(math.isnan(a))
            else: self.assertAlmostEqual(a, b, delta=max(2e-5, abs(b)*2e-7))

    def test_physical_cfa_math_negative_values_masks_and_wcs(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            b = [10+p/8 for p in range(120)]
            signal = [-20+p/2 for p in range(120)]
            dark = [v+3 for v in b]
            flat = [[100, 200, 300, 400][2*((p//12)%2)+p%2]*(1+p/128) for p in range(120)]
            flat[2] = 0; flat[8] = math.nan
            signal[5] = math.nan
            wcs = dict(CTYPE1="'RA---TAN-SIP'", CTYPE2="'DEC--TAN-SIP'", CRPIX1=6.5, CRPIX2=5.5,
                       CRVAL1=313.6, CRVAL2=29.7, CD1_1=-.001, CD1_2=0, CD2_1=0, CD2_2=.001,
                       A_ORDER=2, B_ORDER=2, A_2_0=.00001, B_0_2=-.00002, DATAMIN=-100, DATAMAX=10000)
            light = frame(root/'light.fits', [(v+z-5)/2 for v,z in zip(signal,dark)], 'Light', BSCALE=2, BZERO=5,
                          BAYERPAT="'BGGR'", XBAYROFF=1, YBAYROFF=1, OBJECT="'Veil'", **wcs)
            bias = frame(root/'bias.fits', b, 'Master Bias', PSBIAS="'included'", EXPTIME=0)
            darkIncluded = frame(root/'dark.fits', dark, 'Master Dark', PSBIAS="'included'")
            flatRemoved = frame(root/'flat.fits', flat, 'Master Flat', PSBIAS="'removed'")
            m = manifest(root, [(light,'b','d','f')], [('b','bias',bias,'included'),('d','dark',darkIncluded,'included'),('f','flat',flatRemoved,'removed')],
                         settings='tile-pixels 7\nthreads 1\n')
            before = {p:p.read_bytes() for p in (light,bias,darkIncluded,flatRemoved)}
            result = run('calibration-run','--manifest',m,'--output-dir',root/'job')
            self.assertEqual(result['status'],'complete');self.assertTrue(result['sourcesVerified'])
            self.assertEqual(result['masters']['d']['sha'],hashlib.sha256(darkIncluded.read_bytes()).hexdigest())
            self.assertEqual(set(result['frames'][0]['products']),{'calibrated.fits','valid.fits','report.json'})
            for name,digest in result['frames'][0]['products'].items():
                self.assertEqual(digest,hashlib.sha256((root/'job/frame-000001'/name).read_bytes()).hexdigest())
            means = []
            for q in range(4):
                values = [v for p,v in enumerate(flat) if 2*((p//12)%2)+p%2==q and math.isfinite(v) and v>0]
                means.append(sum(values)/len(values))
            expected = [v*means[2*((p//12)%2)+p%2]/flat[p] if math.isfinite(flat[p]) and flat[p]>0 else math.nan for p,v in enumerate(signal)]
            output = root/'job/frame-000001'
            self.assertPixels(output/'calibrated.fits',expected)
            self.assertPixels(output/'valid.fits',[1 if math.isfinite(v) else 0 for v in expected])
            report = json.loads((output/'report.json').read_text())
            self.assertFalse(report['biasOnLight']);self.assertFalse(report['biasOnFlat'])
            self.assertEqual(report['invalidPixels'],3);self.assertGreater(report['negativePixels'],0)
            header = read(output/'calibrated.fits')[0]
            for key in wcs:
                if key not in ('DATAMIN','DATAMAX'): self.assertEqual(header[key],read(light)[0][key])
            for key in ('OBJECT','DATE-OBS','BAYERPAT','XBAYROFF','YBAYROFF','GAIN','EXPTIME'):
                self.assertEqual(header[key],read(light)[0][key])
            self.assertEqual(header['PSBIAS'],"'removed'");self.assertEqual(header['PSCALIB'],"'sensor-v1'")
            self.assertEqual(header['BITPIX'],'-32');self.assertEqual(header['BSCALE'],'1');self.assertEqual(header['BZERO'],'0')
            self.assertNotIn('DATAMIN',header);self.assertNotIn('DATAMAX',header)
            for p,raw in before.items(): self.assertEqual(p.read_bytes(),raw)
            # Equivalent explicit bias-removed dark and raw flat paths.
            darkRemoved = frame(root/'dark-removed.fits',[3]*120,'Master Dark',PSBIAS="'removed'")
            flatIncluded = frame(root/'flat-included.fits',[a+z for a,z in zip(flat,b)],'Master Flat',PSBIAS="'included'")
            m = manifest(root,[(light,'b','d','f')],[('b','bias',bias,'included'),('d','dark',darkRemoved,'removed'),('f','flat',flatIncluded,'included')],settings='tile-pixels 13\nthreads 4\n')
            run('calibration-run','--manifest',m,'--output-dir',root/'equivalent')
            self.assertEqual((output/'calibrated.fits').read_bytes(),(root/'equivalent/frame-000001/calibrated.fits').read_bytes())
            self.assertEqual((output/'valid.fits').read_bytes(),(root/'equivalent/frame-000001/valid.fits').read_bytes())

    def test_explicit_per_frame_masters_groups_resume_and_offline_inspection(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            darks=[frame(root/f'dark-{e}.fits',[float(e)]*120,'Master Dark',EXPTIME=e,PSBIAS="'included'") for e in (60,120)]
            lights=[]
            for day in (15,16):
                for i,e in enumerate((60,120)):
                    p=frame(root/f'light-{day}-{e}.fits',[e+day+q/4 for q in range(120)],'Light',EXPTIME=e,**{'DATE-OBS':f"'2026-10-{day}T22:00:00'"})
                    lights.append((p,'-',f'd{i}','-'))
            m=manifest(root,lights,[(f'd{i}','dark',p,'included') for i,p in enumerate(darks)])
            job=root/'job'
            partial=run('calibration-run','--manifest',m,'--output-dir',job,'--max-frames',1)
            self.assertEqual(partial['completed'],1);self.assertEqual(partial['status'],'partial')
            self.assertEqual(len({f['group'] for f in partial['frames']}),4)
            first={p.name:(p.read_bytes(),p.stat().st_mtime_ns) for p in (job/'frame-000001').iterdir()}
            done=run('calibration-resume','--job',job)
            self.assertEqual(done['completed'],4);self.assertEqual(done['status'],'complete')
            for name,(data,mtime) in first.items():
                p=job/'frame-000001'/name;self.assertEqual(p.read_bytes(),data);self.assertEqual(p.stat().st_mtime_ns,mtime)
            for i in range(4): self.assertPixels(job/f'frame-{i+1:06}/calibrated.fits',[(15+i//2)+p/4 for p in range(120)])
            events=subprocess.run([CLI,'calibration-resume','--job',job],capture_output=True,text=True,check=True,
                                  env={**os.environ,'PHOTONSTACK_PROGRESS':'1'})
            rows=[json.loads(line) for line in events.stdout.splitlines()]
            self.assertEqual([row for row in rows if row.get('command')=='calibration-job'][-1]['status'],'complete')
            progress=[row for row in rows if row.get('type')=='progress']
            self.assertGreater(len(progress),1)
            self.assertEqual([row['progress'] for row in progress],sorted(row['progress'] for row in progress))
            self.assertEqual(progress[-1]['progress'],1)
            for p,*_ in lights: p.unlink()
            offline=run('calibration-job-inspect','--job',job)
            self.assertFalse(offline['sourcesVerified']);self.assertTrue(offline['productsVerified']);self.assertEqual(offline['completed'],4)
            run('calibration-resume','--job',job,ok=False)

    def test_corrupted_completed_frame_and_changed_input_never_silently_resume(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);bias=frame(root/'bias.fits',[10]*120,'Master Bias',EXPTIME=0,PSBIAS="'included'")
            lights=[frame(root/f'l{i}.fits',[20+i]*120,'Light') for i in range(2)]
            m=manifest(root,[(p,'b','-','-') for p in lights],[('b','bias',bias,'included')])
            for name,target in [('pixels','calibrated.fits'),('mask','valid.fits'),('report','report.json'),('receipt','receipt.txt')]:
                job=root/name;run('calibration-run','--manifest',m,'--output-dir',job,'--max-frames',1)
                p=job/'frame-000001'/target;p.write_bytes(p.read_bytes()+b'changed')
                run('calibration-resume','--job',job,ok=False);run('calibration-job-inspect','--job',job,ok=False)
                self.assertFalse((job/'frame-000002').exists())
            job=root/'changed-input';run('calibration-run','--manifest',m,'--output-dir',job,'--max-frames',1)
            lights[1].write_bytes(lights[1].read_bytes()+b'changed')
            run('calibration-resume','--job',job,ok=False);self.assertFalse((job/'frame-000002').exists())
            (job/'job.txt').write_text((job/'job.txt').read_text().replace('threads 4','threads 3'))
            run('calibration-job-inspect','--job',job,ok=False)

    def test_preflight_blocks_wrong_states_missing_metadata_and_second_calibration(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);light=frame(root/'light.fits',[100]*120,'Light')
            bias=frame(root/'bias.fits',[10]*120,'Master Bias',EXPTIME=0,PSBIAS="'included'")
            dark=frame(root/'dark.fits',[5]*120,'Master Dark',PSBIAS="'removed'")
            flat=frame(root/'flat.fits',[1000]*120,'Master Flat',PSBIAS="'removed'")
            wrongFlat=frame(root/'wrong-flat.fits',[1000]*120,'Master Flat',PSBIAS="'removed'",FILTER="'OIII'")
            coldFlat=frame(root/'cold-flat.fits',[1000]*120,'Master Flat',PSBIAS="'included'",**{'CCD-TEMP':-20})
            normalizedRawFlat=frame(root/'normalized-raw-flat.fits',[1000]*120,'Master Flat',PSBIAS="'included'",PSNORM="'phase-mean'")
            cases=[([(light,'-','d','-')],[('d','dark',dark,'removed')]),
                   ([(light,'-','d','-')],[('d','dark',dark,'included')]),
                   ([(light,'-','-','f')],[('f','flat',flat,'removed')]),
                   ([(light,'f','-','-')],[('f','flat',flat,'removed')]),
                   ([(light,'b','-','f')],[('b','bias',bias,'included'),('f','flat',wrongFlat,'removed')]),
                   ([(light,'b','-','f')],[('b','bias',bias,'included'),('f','flat',coldFlat,'included')]),
                   ([(light,'b','-','f')],[('b','bias',bias,'included'),('f','flat',normalizedRawFlat,'included')])]
            for i,(ls,ms) in enumerate(cases):
                m=manifest(root,ls,ms);out=root/f'bad-{i}'
                run('calibration-run','--manifest',m,'--output-dir',out,ok=False);self.assertFalse(out.exists())
            for key,value in [('GAIN',None),('GAIN',200),('BAYERPAT',"'GRBG'"),('PSCOLOR',"'SRGB-DISPLAY'"),('PSCALIB',"'sensor-v1'")]:
                bad=frame(root/'bad.fits',[100]*120,'Light',**{key:value})
                m=manifest(root,[(light,'b','-','-'),(bad,'b','-','-')],[('b','bias',bias,'included')])
                out=root/'bad-later';run('calibration-run','--manifest',m,'--output-dir',out,ok=False);self.assertFalse(out.exists())
            m=manifest(root,[(light,'b','-','-')],[('b','bias',bias,'included')])
            run('calibration-run','--manifest',m,'--output-dir',root/'good')
            calibrated=root/'good/frame-000001/calibrated.fits'
            m=manifest(root,[(calibrated,'b','-','-')],[('b','bias',bias,'included')])
            run('calibration-run','--manifest',m,'--output-dir',root/'twice',ok=False)
            self.assertFalse((root/'twice').exists())

    def test_mono_flat_invalid_phases_overflow_and_resource_limits(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);layout=dict(BAYERPAT=None,PSLAYOUT="'MONO'")
            bias=frame(root/'bias.fits',[10]*120,'Master Bias',EXPTIME=0,PSBIAS="'included'",**layout)
            light=frame(root/'light.fits',[9+p for p in range(120)],'Light',**layout)
            flat=frame(root/'flat.fits',[100+p for p in range(120)],'Master Flat',PSBIAS="'removed'",**layout)
            m=manifest(root,[(light,'b','-','f')],[('b','bias',bias,'included'),('f','flat',flat,'removed')])
            run('calibration-run','--manifest',m,'--output-dir',root/'mono')
            self.assertPixels(root/'mono/frame-000001/calibrated.fits',[(p-1)*159.5/(100+p) for p in range(120)])
            for value in [0,math.nan]:
                frame(flat,[value]*120,'Master Flat',PSBIAS="'removed'",**layout)
                m=manifest(root,[(light,'b','-','f')],[('b','bias',bias,'included'),('f','flat',flat,'removed')])
                out=root/('zero' if value==0 else 'nan');run('calibration-run','--manifest',m,'--output-dir',out,ok=False)
                self.assertEqual(run('calibration-job-inspect','--job',out)['completed'],0)
                self.assertFalse(any(p.name.startswith('.calibration-stage') for p in out.iterdir()))
            huge=frame(root/'huge.fits',[3e38]*120,'Light',**layout)
            negativeBias=frame(root/'negative-bias.fits',[-3e38]*120,'Master Bias',PSBIAS="'included'",EXPTIME=0,**layout)
            m=manifest(root,[(huge,'b','-','-')],[('b','bias',negativeBias,'included')])
            run('calibration-run','--manifest',m,'--output-dir',root/'overflow',ok=False)
            self.assertFalse((root/'overflow/frame-000001').exists())
            for option in ['threads 0','threads 33','memory-mib 3','tile-pixels 0','temperature-tolerance nan']:
                m=manifest(root,[(light,'b','-','-')],[('b','bias',bias,'included')],settings=option+'\n')
                run('calibration-run','--manifest',m,'--output-dir',root/'bad-option',ok=False)
                self.assertFalse((root/'bad-option').exists())

    def test_links_receipt_identity_and_existing_destination_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);bias=frame(root/'bias.fits',[10]*120,'Master Bias',EXPTIME=0,PSBIAS="'included'")
            lights=[frame(root/f'l{i}.fits',[20+i]*120,'Light') for i in range(2)]
            m=manifest(root,[(p,'b','-','-') for p in lights],[('b','bias',bias,'included')])
            job=root/'job';run('calibration-run','--manifest',m,'--output-dir',job)
            run('calibration-run','--manifest',m,'--output-dir',job,ok=False)
            with (job/'.lock').open('rb') as lock:
                fcntl.flock(lock,fcntl.LOCK_EX | fcntl.LOCK_NB)
                run('calibration-resume','--job',job,ok=False)
                run('calibration-job-inspect','--job',job,ok=False)
            first=job/'frame-000001/receipt.txt';second=job/'frame-000002/receipt.txt'
            original=second.read_bytes();second.write_bytes(first.read_bytes())
            run('calibration-job-inspect','--job',job,ok=False);second.write_bytes(original)
            mask=job/'frame-000001/valid.fits';copy=root/'copy.fits';mask.rename(copy);mask.symlink_to(copy)
            run('calibration-job-inspect','--job',job,ok=False)

    def test_live_cancellation_resumes_committed_frames_without_rewriting(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            def large(path,value,role):
                write(path,[],w=1024,h=1024,extra=cards(role,EXPTIME=0 if role=='Master Bias' else 60,
                    PSBIAS="'included'" if role=='Master Bias' else None))
                with path.open('ab') as f:
                    payload=struct.pack('>f',value)*(1024*1024)
                    f.write(payload);f.write(b'\0'*((-len(payload))%2880))
                return path
            bias=large(root/'bias.fits',10,'Master Bias')
            lights=[large(root/f'l{i}.fits',20+i,'Light') for i in range(6)]
            m=manifest(root,[(p,'b','-','-') for p in lights],[('b','bias',bias,'included')]);job=root/'job'
            original={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in lights+[bias]}
            process=subprocess.Popen([CLI,'calibration-run','--manifest',m,'--output-dir',job],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                line=process.stderr.readline()
                self.assertIn('Calibrated 1/6',line)
                process.send_signal(signal.SIGTERM)
                stdout,stderr=process.communicate(timeout=15)
                self.assertNotEqual(process.returncode,0);self.assertIn('cancelled',stderr)
            finally:
                if process.poll() is None:process.kill();process.communicate()
            state=run('calibration-job-inspect','--job',job)
            self.assertGreaterEqual(state['completed'],1);self.assertLess(state['completed'],6)
            committed={str(p.relative_to(job)):(hashlib.sha256(p.read_bytes()).hexdigest(),p.stat().st_mtime_ns) for p in job.glob('frame-*/*')}
            self.assertFalse(any(p.name.startswith('.calibration-stage') for p in job.iterdir()))
            result=run('calibration-resume','--job',job)
            self.assertEqual(result['completed'],6)
            for name,(sha,mtime) in committed.items():
                p=job/name;self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(),sha);self.assertEqual(p.stat().st_mtime_ns,mtime)
            for p,sha in original.items():self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(),sha)


if __name__ == '__main__': unittest.main()
