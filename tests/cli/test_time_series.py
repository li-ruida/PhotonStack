"""Independent time-variable stars, transparency, timing and rejection checks."""
import csv,datetime,hashlib,json,math
from pathlib import Path
import statistics,struct,subprocess,sys,tempfile,unittest
CLI=Path(sys.argv.pop(1)).resolve()
W=192;STARS=[(48,48),(136,48),(48,136)]

def image(path,index,invalid=False):
    date=datetime.datetime(2024,2,28,23,59,50,tzinfo=datetime.timezone.utc)+datetime.timedelta(seconds=30*index)
    exposure=10+index%3;cloud=.8+.2*math.cos(index*.4);target=1+.03*math.sin(index*.5)
    fields={'SIMPLE':'T','BITPIX':'-32','NAXIS':'2','NAXIS1':str(W),'NAXIS2':str(W),'PSCOLOR':"'Linear'",'BUNIT':"'ADU'",'EXPTIME':str(exposure),'DATE-OBS':"'"+date.strftime('%Y-%m-%dT%H:%M:%S')+"'",'TIMESYS':"'UTC'",'FILTER':"'Test'"}
    if invalid and index==21:del fields['DATE-OBS']
    if invalid and index==24:fields['FILTER']="'Other'"
    header=''.join((f'{k:<8}= {v:>20}').ljust(80) for k,v in fields.items())+'END';header=header.ljust((len(header)+2879)//2880*2880)
    positions=[(x+.15*math.sin(index),y+.1*math.cos(index)) for x,y in STARS]
    values=[]
    for y in range(W):
        for x in range(W):
            v=100+.02*x-.03*y+.001*math.sin(x*2.3+y*.7+index)
            for k,(cx,cy) in enumerate(positions):v+=cloud*exposure*[40*target,30,50][k]*math.exp(-((x-cx)**2+(y-cy)**2)/(2*1.5**2))
            if invalid and index==22 and x==48 and y==48:v=60000
            if invalid and index==23 and x==136+12 and y==48:v=float('nan')
            values.append(v)
    data=struct.pack('>'+str(len(values))+'f',*values);path.write_bytes(header.encode()+data.ljust((len(data)+2879)//2880*2880,b'\0'))
    return dict(ratio=.5*target,jd=date.timestamp()/86400+2440587.5+exposure/172800,flux=cloud*exposure*40*target*2*math.pi*1.5**2)

def recipe(path,frames,extra=''):
    text='''PhotonStack-TimeSeries-1
aligned "true"
filter "Test"
channel "mono"
effective-gain "1.5"
aperture "8"
sky-inner "11"
sky-outer "15"
saturation "50000"
min-snr "5"
max-shift "3"
target "target" 48 48
comparison "c1" 136 48
comparison "c2" 48 136
'''
    for frame in frames:text+='frame '+json.dumps(str(frame))+'\n'
    path.write_text(text+extra)

def run(path,out,ok=True):
    p=subprocess.run([CLI,'time-series','--recipe',path,'--output-dir',out],capture_output=True,text=True)
    if ok and p.returncode:raise AssertionError(p.stdout+p.stderr)
    if not ok and not p.returncode:raise AssertionError('Unexpected success')
    return p

class TimeSeries(unittest.TestCase):
    def test_recovers_variation_through_transparency_with_timing_noise_and_flags(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);frames=[root/f'f{i}.fits' for i in range(25)];truth=[image(p,i,True) for i,p in enumerate(frames)]
            hashes=[hashlib.sha256(p.read_bytes()).hexdigest() for p in frames];config=root/'sequence.txt';recipe(config,frames);out=root/'result';run(config,out)
            report=json.loads((out/'report.json').read_text());rows=list(csv.DictReader((out/'lightcurve.csv').read_text().splitlines()));measurements=list(csv.DictReader((out/'measurements.csv').read_text().splitlines()))
            self.assertEqual(hashlib.sha256((out/'report.json').read_bytes()).hexdigest(),(out/'report.sha256').read_text().strip());self.assertEqual(len(report['points']),25);self.assertEqual(report['accepted'],21);self.assertEqual(len(rows),25);self.assertTrue(report['saturationKnown'])
            for i,row in enumerate(rows):
                self.assertNotIn(None,row)
                if i<21:
                    self.assertEqual(row['status'],'measured');self.assertAlmostEqual(float(row['jd_utc_mid']),truth[i]['jd'],delta=1e-9)
                    self.assertAlmostEqual(float(row['target_over_ensemble']),truth[i]['ratio'],delta=3e-5)
                    self.assertAlmostEqual(float(row['target_flux'])/truth[i]['flux'],1,delta=.0001)
                    self.assertAlmostEqual(float(row['target_flux_error'])/math.sqrt(truth[i]['flux']/1.5),1,delta=.01)
                    self.assertAlmostEqual(float(row['magnitude_error']),(2.5/math.log(10))*float(row['ratio_error'])/float(row['target_over_ensemble']),delta=1e-12)
                else:self.assertNotEqual(row['status'],'measured');self.assertEqual(row['relative_flux'],'')
            expectedBaseline=statistics.median(t['ratio'] for t in truth[:21]);self.assertAlmostEqual(report['normalization'],expectedBaseline,delta=3e-5)
            self.assertTrue(all(c['leaveOneOutRelativeRMS']<.0001 for c in report['comparisons']))
            for m in measurements:
                if m['status']=='measured':self.assertAlmostEqual(float(m['flux_error'])**2,float(m['background_only_error'])**2+float(m['source_shot_coefficient'])/1.5,delta=1e-8)
            self.assertEqual(hashes,[hashlib.sha256(p.read_bytes()).hexdigest() for p in frames])
            for name,sha in report['products'].items():self.assertEqual(hashlib.sha256((out/name).read_bytes()).hexdigest(),sha)
            run(config,out,False)
    def test_bad_declarations_duplicates_and_explicit_utc(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);frames=[root/f'f{i}.fits' for i in range(3)]
            for i,p in enumerate(frames):image(p,i)
            config=root/'sequence.txt';recipe(config,frames);original=config.read_text()
            for n,(a,b) in enumerate([('aligned "true"','aligned "false"'),('effective-gain "1.5"','effective-gain "0"'),('channel "mono"','channel "green"'),('136 48','49 48')]):
                config.write_text(original.replace(a,b));run(config,root/f'bad{n}',False)
            config.write_text(original+'frame '+json.dumps(str(frames[0]))+'\n');run(config,root/'duplicate',False)
            lines=original.splitlines();lines=[line+' "2024-02-29T12:00:00Z" 20' if line.startswith('frame ') else line for line in lines];config.write_text('\n'.join(lines)+'\n');out=root/'timing';run(config,out)
            rows=list(csv.DictReader((out/'lightcurve.csv').read_text().splitlines()));self.assertEqual(rows[0]['status'],'measured');self.assertTrue(all(row['status']=='frame-rejected' for row in rows[1:]));self.assertIn('Duplicate',rows[1]['detail'])
            config.write_text(original.replace('2024','invalid')+'unknown "x"\n');run(config,root/'unknown',False)

    def test_utc_leap_boundaries_and_exposure_conflicts(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);frames=[root/f'f{i}.fits' for i in range(3)]
            for i,p in enumerate(frames):image(p,i)
            config=root/'sequence.txt';recipe(config,frames);original=config.read_text()
            for n,(stamp,exposure,accepted) in enumerate([
                ('2016-12-31T12:00:00Z',20,0),('2016-12-30T23:59:55Z',20,0),
                ('2017-01-01T00:00:00Z',20,1),('2027-06-28T00:00:00Z',20,0),
                ('2024-02-29T12:00:00Z',90000,0),('2024-02-29T12:00:00e1',20,0)]):
                text='\n'.join(line+f' "{stamp}" {exposure}' if line.startswith('frame ') else line for line in original.splitlines())+'\n'
                config.write_text(text);out=root/f'time{n}';run(config,out)
                report=json.loads((out/'report.json').read_text());self.assertEqual(report['accepted'],accepted)
            data=frames[0].read_bytes();end=data.index(b'END'+b' '*77)
            # Preserve the pixel offset: replace an unused header card, not the data.
            source=frames[0].read_bytes();frames[0].write_bytes(source[:end]+f'{"EXPOSURE":<8}= {11:>20}'.ljust(80).encode()+b'END'.ljust(80)+source[end+160:])
            config.write_text(original);out=root/'exposure';run(config,out)
            report=json.loads((out/'report.json').read_text());self.assertEqual(report['accepted'],2);self.assertIn('disagree',report['points'][0]['detail'])

    def test_rejected_frame_does_not_reserve_a_valid_frames_midpoint(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            frames = [root / f'f{i}.fits' for i in range(3)]
            for path, index in zip(frames, [0, 0, 1]):
                image(path, index)
            frames[0].write_bytes(frames[0].read_bytes().replace(b"'Test'", b"'Blue'", 1))
            duration = f'{"EXPTIME":<8}= {11:>20}'.encode()
            fits_double = f'{"EXPTIME":<8}= {"1.1D+01":>20}'.encode()
            frames[2].write_bytes(frames[2].read_bytes().replace(duration, fits_double, 1))
            config = root / 'sequence.txt'
            recipe(config, frames)
            out = root / 'measured'
            run(config, out)
            report = json.loads((out / 'report.json').read_text())
            self.assertEqual([p['status'] for p in report['points']], ['frame-rejected', 'measured', 'measured'])
            self.assertIn('filter', report['points'][0]['detail'])
            self.assertEqual(report['accepted'], 2)

if __name__=='__main__':unittest.main()
