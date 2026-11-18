"""Independent raw FITS samples: master arithmetic, masks, provenance and replay."""
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct
import tempfile
import unittest
from test_channels import write, read, run


def cards(role='Dark', **changes):
    h=dict(INSTRUME="'Detector'", **{'CAM-SER':"'serial-1'", 'CCD-TEMP':-10, 'DATE-OBS':"'2026-10-15T22:00:00'"},
           FILTER="'Ha'", PSOPTICS="'train-1'", READMODE="'normal'", GAIN=100, OFFSET=20, EXPTIME=60,
           XBINNING=1, YBINNING=1, XORGSUBF=0, YORGSUBF=0, BAYERPAT="'RGGB'", BUNIT="'ADU'", PSCOLOR="'Linear'", IMAGETYP=f"'{role}'")
    h.update(changes)
    return {k:v for k,v in h.items() if v is not None}


def frame(path, values, role='Dark', **changes):
    write(path,values,w=12,h=10,extra=cards(role,**changes))
    return path


def manifest(root, paths, role='dark', correction='', method='median', extra=''):
    p=root/'input-list.txt'
    p.write_text('PhotonStack-SensorMaster-1\nrole '+role+'\nmethod '+method+'\n'+correction+extra+''.join(
        'frame '+json.dumps(str(x))+' '+hashlib.sha256(x.read_bytes()).hexdigest()+'\n' for x in paths))
    return p


def build(root, paths, name='built', role='dark', correction='', method='median', extra='', options=()):
    m=manifest(root,paths,role,correction,method,extra)
    run('sensor-master-build','--manifest',m,'--output-dir',root/name,*options)
    return root/name


def correction(kind, path, state=''):
    return kind+' '+json.dumps(str(path))+' '+hashlib.sha256(path.read_bytes()).hexdigest()+(' '+state if state else '')+'\n'


class SensorMasters(unittest.TestCase):
    def assertPixels(self, actual, expected):
        self.assertEqual(len(actual),len(expected))
        for a,b in zip(actual,expected):
            if math.isnan(b): self.assertTrue(math.isnan(a))
            else: self.assertAlmostEqual(a,b,delta=max(2e-5,abs(b)*3e-7))

    def test_bias_dark_signed_physical_samples_headers_and_replay(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            biases=[frame(root/f'b{i}.fits',[10+i+p/10 for p in range(120)],'Bias',EXPTIME=0) for i in range(3)]
            original={p:p.read_bytes() for p in biases}
            bias=build(root,biases,'bias','bias')
            self.assertPixels(read(bias/'master.fits')[1][0],[11+p/10 for p in range(120)])
            darks=[frame(root/f'd{i}.fits',[4+i+p/5 for p in range(120)],BSCALE=2,BZERO=0,**{'CCD-TEMP':-10+i*.2}) for i in range(3)]
            dark=build(root,darks,correction=correction('bias',bias/'master.fits'),options=('--tile-pixels','7','--threads','1'))
            header,pixels=read(dark/'master.fits')
            self.assertPixels(pixels[0],[-1+p*.3 for p in range(120)])
            self.assertEqual(header['PSBIAS'],"'removed'")
            self.assertEqual(header['BAYERPAT'],"'RGGB'")
            self.assertEqual(int(header['STACKCNT']),3)
            self.assertNotIn('DATE-OBS',header); self.assertNotIn('FILTER',header)
            self.assertEqual(float(header['PSTMIN']),-10);self.assertAlmostEqual(float(header['PSTMAX']),-9.6)
            report=run('sensor-master-inspect','--recipe',dark/'recipe.txt')
            self.assertEqual(report['invalidPixels'],0)
            replay=root/'replay'
            run('sensor-master-replay','--recipe',dark/'recipe.txt','--output-dir',replay)
            for p in dark.iterdir():self.assertEqual(p.read_bytes(),(replay/p.name).read_bytes())
            for p,b in original.items():self.assertEqual(p.read_bytes(),b)

    def test_flat_correction_independent_phases_normalization_and_holes(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            phase=lambda p:2*((p//12)%2)+p%2
            bias=frame(root/'bias.fits',[10]*120,'Master Bias',PSBIAS="'included'",EXPTIME=0)
            source=[];expected=[]
            for i,brightness in enumerate([1.,2.,4.]):
                signal=[brightness*[100,200,300,400][phase(p)]*(1+.001*p) for p in range(120)]
                pixels=[10+x for x in signal]
                pixels[0]=float('nan');pixels[1]=10 # flat must be positive after bias subtraction
                source.append(frame(root/f'f{i}.fits',pixels,'Flat',EXPTIME=.2))
                expected.append(signal)
            out=build(root,source,role='flat',correction=correction('bias',bias),options=('--tile-pixels','13',))
            wanted=expected[0];wanted[0]=wanted[1]=float('nan')
            self.assertPixels(read(out/'master.fits')[1][0],wanted)
            accepted=read(out/'accepted.fits')[1][0]
            self.assertEqual(accepted[:2],(0,0));self.assertEqual(accepted[2:],(3,)*118)
            report=run('sensor-master-inspect','--recipe',out/'recipe.txt')
            self.assertEqual(report['missingSamples'],6);self.assertEqual(report['invalidPixels'],2)
            for factors,expectedFactor in zip(report['factors'],[1,.5,.25]):
                for v in factors:self.assertAlmostEqual(v,expectedFactor,delta=2e-7)
            self.assertEqual(read(out/'master.fits')[0]['FILTER'],"'Ha'")

    def test_dark_flat_included_and_removed_equivalent_without_double_bias(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            bias=frame(root/'bias.fits',[10]*120,'Master Bias',EXPTIME=0,PSBIAS="'included'")
            darkFlats=[frame(root/f'df{i}.fits',[11+i]*120,'Dark',EXPTIME=.2) for i in range(3)]
            inc=build(root,darkFlats,'dark-flat','dark-flat')/'master.fits'
            self.assertEqual(read(inc)[0]['IMAGETYP'],"'Master dark-flat'")
            rem=frame(root/'rem.fits',[2]*120,'Master Dark-Flat',EXPTIME=.2,PSBIAS="'removed'")
            paths=[frame(root/f'f{i}.fits',[12+(i+1)*(100+p) for p in range(120)],'Flat',EXPTIME=.2) for i in range(3)]
            one=build(root,paths,'one','flat',correction('dark-flat',inc,'included'))
            two=build(root,paths,'two','flat',correction('dark-flat',rem,'removed')+correction('bias',bias))
            self.assertEqual((one/'master.fits').read_bytes(),(two/'master.fits').read_bytes())
            for c in [correction('dark-flat',inc,'included')+correction('bias',bias),correction('dark-flat',rem,'removed'),correction('dark-flat',inc,'removed')+correction('bias',bias)]:
                m=manifest(root,paths,'flat',c)
                run('sensor-master-build','--manifest',m,'--output-dir',root/'bad',ok=False)
                self.assertFalse((root/'bad').exists())

    def test_mono_median_nan_minimum_and_phase_canonicalization(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);values=[[float(i+p) for p in range(120)] for i in range(4)]
            values[0][0]=float('nan');values[1][0]=float('inf');values[2][0]=float('nan')
            paths=[frame(root/f'm{i}.fits',v,BAYERPAT=None,PSLAYOUT="'MONO'") for i,v in enumerate(values)]
            out=build(root,paths,extra='minimum-samples 3\n')
            expected=[statistics.median(v) for v in zip(*values)];expected[0]=float('nan')
            self.assertPixels(read(out/'master.fits')[1][0],expected)
            h=read(out/'master.fits')[0];self.assertEqual(h['PSLAYOUT'],"'MONO'");self.assertNotIn('BAYERPAT',h)
            for i,p in enumerate(paths):frame(p,values[i],BAYERPAT="'RGGB'",XBAYROFF=1,YBAYROFF=1)
            b=build(root,paths,'cfa',method='mean')
            self.assertEqual(read(b/'master.fits')[0]['BAYERPAT'],"'BGGR'")
            self.assertEqual(read(b/'master.fits')[0]['XBAYROFF'],'0')
            bias=frame(root/'bias.fits',[10]*120,'Master Bias',BAYERPAT=None,PSLAYOUT="'MONO'",EXPTIME=0,PSBIAS="'included'")
            for i,p in enumerate(paths):frame(p,[10+(i+1)*(100+p) for p in range(120)],'Flat',BAYERPAT=None,PSLAYOUT="'MONO'")
            flat=build(root,paths,'mono-flat','flat',correction('bias',bias))
            self.assertPixels(read(flat/'master.fits')[1][0],[100+p for p in range(120)])

    def test_temperature_range_used_for_master_matching(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            paths=[frame(root/f'd{i}.fits',[20+i]*120,**{'CCD-TEMP':-10+i*.4}) for i in range(3)]
            out=build(root,paths)
            light=frame(root/'light.fits',[100]*120,'Light',**{'CCD-TEMP':-10.4})
            result=run('calibration-match','--input',light,'--master',out/'master.fits','--role','dark')
            self.assertEqual(result['status'],'incompatible')
            frame(light,[100]*120,'Light',**{'CCD-TEMP':-9.6})
            self.assertEqual(run('calibration-match','--input',light,'--master',out/'master.fits','--role','dark')['status'],'compatible')

    def test_invalid_acquisition_recipe_sources_and_existing_output(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);paths=[frame(root/f'd{i}.fits',[20+i]*120) for i in range(3)]
            out=build(root,paths)
            before={p:p.read_bytes() for p in out.iterdir()}
            run('sensor-master-build','--manifest',root/'input-list.txt','--output-dir',out,ok=False)
            for p,b in before.items():self.assertEqual(p.read_bytes(),b)
            for changes in [dict(GAIN=101),dict(BAYERPAT="'BGGR'"),dict(EXPTIME=30),dict(READMODE=None),{'CCD-TEMP':-12},dict(PSCOLOR="'SRGB-DISPLAY'"),dict(IMAGETYP="'Master Dark'")]:
                frame(paths[1],[21]*120,**changes)
                m=manifest(root,paths)
                run('sensor-master-build','--manifest',m,'--output-dir',root/'bad',ok=False)
                self.assertFalse((root/'bad').exists())
            frame(paths[1],[21]*120)
            m=manifest(root,[paths[0],paths[0]])
            run('sensor-master-build','--manifest',m,'--output-dir',root/'bad',ok=False)
            recipe=out/'recipe.txt';data=recipe.read_text();recipe.write_text(data.replace('method median','method mean'))
            run('sensor-master-inspect','--recipe',recipe,ok=False)
            recipe.write_text(data)
            (out/'master.fits').write_bytes((out/'master.fits').read_bytes()+b'changed')
            run('sensor-master-inspect','--recipe',recipe,ok=False)
            self.assertFalse(list(root.glob('.sensor-master-*')))

    def test_integer_blank_physical_scaling_and_overflow(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);paths=[]
            for i in range(3):
                p=frame(root/f'i{i}.fits',[0]*120,BITPIX=16,BLANK=-32768,BSCALE=2,BZERO=100)
                raw=p.read_bytes();start=2880
                data=struct.pack('>120h',-32768,*[i+p for p in range(1,120)])
                p.write_bytes(raw[:start]+data.ljust(2880,b'\0'));paths.append(p)
            out=build(root,paths)
            self.assertPixels(read(out/'master.fits')[1][0],[float('nan')]+[100+2*(p+1) for p in range(1,120)])
            frame(paths[1],[3e38]*120,BSCALE=10)
            m=manifest(root,paths)
            run('sensor-master-build','--manifest',m,'--output-dir',root/'overflow',ok=False)
            self.assertFalse((root/'overflow').exists())

    def test_limits_invalid_ranges_and_input_permutation(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);paths=[frame(root/f'd{i}.fits',[20+i]*120) for i in range(3)]
            first=build(root,paths,'first')
            second=build(root,list(reversed(paths)),'second',options=('--tile-pixels','1','--threads','32'))
            for name in ['master.fits','accepted.fits']:
                self.assertEqual((first/name).read_bytes(),(second/name).read_bytes())
            for key,value in [('--memory-mib','1'),('--memory-mib','8193'),('--tile-pixels','0'),('--threads','33')]:
                run('sensor-master-build','--manifest',root/'input-list.txt','--output-dir',root/'bad',key,value,ok=False)
            for changes in [dict(PSTMIN=-11),dict(PSTMIN=-8,PSTMAX=-12),dict(PSTMIN=-12,PSTMAX=-8),dict(PSBIAS="'unknown'")]:
                frame(paths[1],[21]*120,**changes)
                m=manifest(root,paths)
                run('sensor-master-build','--manifest',m,'--output-dir',root/'bad',ok=False)
            for i,p in enumerate(paths):frame(p,[float('nan')]*120,**{'DATE-OBS':f"'2026-10-15T22:00:0{i}'"})
            m=manifest(root,paths)
            run('sensor-master-build','--manifest',m,'--output-dir',root/'empty',ok=False)
            self.assertFalse((root/'empty').exists())
            self.assertFalse(list(root.glob('.sensor-master-*')))


if __name__=='__main__':unittest.main()
