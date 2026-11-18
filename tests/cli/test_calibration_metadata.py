"""Acquisition identities and calibration decisions from hand-written FITS, without pixel inference."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from test_channels import run


def quote(s): return "'"+s.replace("'", "''")+"'"


def make(path, changes=None, remove=(), extra=()):
    cards = dict(SIMPLE='T', BITPIX=-32, NAXIS=2, NAXIS1=12, NAXIS2=10,
                 INSTRUME=quote("Cam's / detector"), **{'CAM-SER':quote('A-001'), 'CCD-TEMP':-10,
                 'DATE-OBS':quote('2026-10-15T22:00:00'), 'SET-TEMP':-10},
                 FILTER=quote('Ha'), PSOPTICS=quote('scope-focuser-camera-rotation-v1'), READMODE=quote('fast'),
                 GAIN=100, OFFSET=20, EXPTIME=60, XBINNING=1, YBINNING=1, XORGSUBF=0, YORGSUBF=0,
                 BAYERPAT=quote('RGGB'), BUNIT=quote('adu'), PSCOLOR=quote('Linear'), IMAGETYP=quote('Light Frame'))
    cards.update(changes or {})
    for k in remove: cards.pop(k,None)
    pairs = list(cards.items())+list(extra)
    header = ''.join((f'{k:<8}= {str(v):>20}').ljust(80) for k,v in pairs)+'END'.ljust(80)
    header = header.ljust((len(header)+2879)//2880*2880)
    count = int(cards['NAXIS1'])*int(cards['NAXIS2'])*int(cards.get('NAXIS3',1))
    payload = struct.pack('>'+str(count)+'f', *[100+i/100 for i in range(count)])
    path.write_bytes(header.encode()+payload.ljust((len(payload)+2879)//2880*2880,b'\0'))


def inspect(path): return run('calibration-inspect',path)['entries'][0]['acquisition']


def match(root, role='dark', *options):
    return run('calibration-match','--input',root/'input.fits','--master',root/'master.fits','--role',role,*options)


def plan(root, records, tolerance=1):
    manifest=root/'plan.txt'
    manifest.write_text('PhotonStack-CalibrationPlan-1\ntemperature-tolerance '+str(tolerance)+'\n'+''.join(
        role+' '+json.dumps(str(path),ensure_ascii=False)+' '+sha+'\n' for role,path,sha in records))
    return run('calibration-plan','--manifest',manifest)


class CalibrationMetadata(unittest.TestCase):
    def test_fields_units_aliases_effective_cfa_and_input_preservation(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'source.fits'
            make(p, {'EXPTIME':'6.0D1','EXPOSURE':60, 'GAIN':0, 'CCD-TEMP':-10.5,
                     'EGAIN':.43,'XBAYROFF':-1, 'YBAYROFF':1,'BSCALE':2,'BZERO':32768})
            original=p.read_bytes(); r=inspect(p)
            self.assertEqual(r['sha'],hashlib.sha256(original).hexdigest())
            self.assertEqual((r['width'],r['height'],r['channels'],r['bitpix']),(12,10,1,-32))
            for k,v in dict(camera="Cam's / detector",cameraSerial='A-001',sampling='CFA:BGGR',units='ADU',domain='linear').items():
                self.assertEqual(r['text'][k],v)
            for k,v in dict(exposure=60,gain=0,sensorTemperature=-10.5,electronsPerADU=.43,offset=20,scale=2,zero=32768).items():
                self.assertEqual(r['numeric'][k],v)
            self.assertEqual(r['sourceKeys']['exposure'],['EXPTIME','EXPOSURE'])
            self.assertEqual(r['sourceKeys']['sensorTemperature'],['CCD-TEMP'])
            self.assertEqual(r['issues'],[])
            self.assertEqual(p.read_bytes(),original)

    def test_missing_is_not_zero_or_identity_and_setpoint_is_not_measurement(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); make(root/'input.fits')
            make(root/'master.fits', {'IMAGETYP':quote('Master Dark'),'EGAIN':.43},
                 remove=['GAIN','CCD-TEMP','XBINNING','YBINNING','CAM-SER','XORGSUBF'])
            r=inspect(root/'master.fits')
            for k in ['gain','iso','sensorTemperature','binX','binY','originX']: self.assertNotIn(k,r['numeric'])
            self.assertEqual(r['numeric']['setTemperature'],-10)
            m=match(root); self.assertEqual(m['status'],'review'); self.assertFalse(m['canApply'])
            evidence={e['field']:e for e in m['evidence']}
            self.assertEqual(evidence['gain']['status'],'missing')
            self.assertEqual(evidence['sensorTemperature']['status'],'missing')
            self.assertEqual(evidence['cameraSerial']['status'],'missing')

    def test_explicit_conflicts_never_turn_into_unknown(self):
        cases=[({'EXPOSURE':120},'exposure'),({'GAIN':'NaN'},'gain'),({'GAIN':quote('100')},'gain'),
               ({'GAIN':'0x1p+2'},'gain'),({'XBINNING':0},'binX'),({'YBINNING':1.5},'binY'),
               ({'TEMPUNIT':quote('K')},'sensorTemperature'),({'BAYERPAT':quote('RGBX')},'sampling'),
               ({'PSLAYOUT':quote('MONO')},'sampling'),({'PSCOLOR':quote('sRGB')},'domain'),
               ({'XBAYROFF':quote('1')},'sampling'),({'READMODE':'unquoted'},'readoutMode')]
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); make(root/'input.fits')
            for change,field in cases:
                with self.subTest(change=change):
                    make(root/'master.fits',{'IMAGETYP':quote('Dark'),**change})
                    r=inspect(root/'master.fits')
                    self.assertIn(field,[i['field'] for i in r['issues']])
                    self.assertEqual(match(root)['status'],'incompatible')
            for key,value in [('GAIN',100),('INSTRUME',quote('same')),('BITPIX',-32)]:
                make(root/'master.fits',{'IMAGETYP':quote('Dark')},extra=[(key,value)])
                self.assertTrue(inspect(root/'master.fits')['issues'])
                self.assertEqual(match(root)['status'],'incompatible')

    def test_two_nights_two_filters_exposures_and_bad_flat(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            for night in [15,16]:
                for filter_name in ['Ha','OIII']:
                    for exposure in [60,120]:
                        make(root/'input.fits',{'FILTER':quote(filter_name),'EXPTIME':exposure,
                             'DATE-OBS':quote(f'2026-10-{night}T22:00:00')})
                        # A dark from the other night and another filter remains a candidate.
                        make(root/'master.fits',{'IMAGETYP':quote('Master Dark'),'EXPTIME':exposure,
                             'FILTER':quote('SII'),'DATE-OBS':quote('2026-10-14T21:00:00')})
                        self.assertEqual(match(root)['status'],'compatible')
                        make(root/'master.fits',{'IMAGETYP':quote('Dark'),'EXPTIME':180})
                        self.assertEqual(match(root)['status'],'incompatible')
                        make(root/'master.fits',{'IMAGETYP':quote('Master Flat'),'EXPTIME':.1,
                             'FILTER':quote(filter_name),'CCD-TEMP':20})
                        self.assertEqual(match(root,'flat')['status'],'compatible')
                        make(root/'master.fits',{'IMAGETYP':quote('Flat'),'FILTER':quote('SII')})
                        result=match(root,'flat'); self.assertEqual(result['status'],'incompatible')
                        self.assertEqual(next(e['status'] for e in result['evidence'] if e['field']=='filter'),'conflict')
            make(root/'master.fits',{'IMAGETYP':quote('Flat'),'FILTER':quote('OIII'),'PSOPTICS':quote('rotated-v2')})
            self.assertEqual(match(root,'flat')['status'],'incompatible')
            make(root/'master.fits',{'IMAGETYP':quote('Flat'),'FILTER':quote('OIII')},remove=['PSOPTICS'])
            self.assertEqual(match(root,'flat')['status'],'review')
            before={p.name:p.read_bytes() for p in root.iterdir()}; match(root,'flat')
            self.assertEqual(before,{p.name:p.read_bytes() for p in root.iterdir()})

    def test_temperature_bias_darkflat_geometry_and_source_binding(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); make(root/'input.fits')
            make(root/'master.fits',{'IMAGETYP':quote('Dark'),'CCD-TEMP':-8.999})
            self.assertEqual(match(root)['status'],'incompatible')
            self.assertEqual(match(root,'dark','--temperature-tolerance','1.01')['status'],'compatible')
            make(root/'master.fits',{'IMAGETYP':quote('Bias'),'EXPTIME':0})
            self.assertEqual(match(root,'bias')['status'],'compatible')
            self.assertEqual(match(root,'dark')['status'],'incompatible')
            make(root/'input.fits',{'EXPTIME':0})
            make(root/'master.fits',{'IMAGETYP':quote('Dark'),'EXPTIME':0})
            self.assertEqual(match(root,'dark')['status'],'incompatible')
            make(root/'input.fits',{'IMAGETYP':quote('Flat'),'EXPTIME':.2})
            make(root/'master.fits',{'IMAGETYP':quote('Dark Flat'),'EXPTIME':.2})
            self.assertEqual(match(root,'dark-flat')['status'],'compatible')
            make(root/'master.fits',{'IMAGETYP':quote('Dark Flat'),'EXPTIME':.2,'XORGSUBF':2})
            self.assertEqual(match(root,'dark-flat')['status'],'incompatible')
            make(root/'master.fits',{'IMAGETYP':quote('Dark Flat'),'EXPTIME':.2,'XBAYROFF':1})
            self.assertEqual(match(root,'dark-flat')['status'],'incompatible')
            run('calibration-match','--input',root/'input.fits','--master',root/'master.fits','--role','dark-flat',
                '--expected-input-sha','0'*64,ok=False)
            run('calibration-match','--input',root/'input.fits','--master',root/'input.fits','--role','dark',ok=False)
            for tol in ['nan','-1','11','1x']:
                run('calibration-match','--input',root/'input.fits','--master',root/'master.fits','--role','dark',
                    '--temperature-tolerance',tol,ok=False)

    def test_no_sampling_guess_and_corrupt_or_display_geometry(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); make(root/'input.fits',remove=['BAYERPAT'])
            make(root/'master.fits',{'IMAGETYP':quote('Dark')},remove=['BAYERPAT'])
            self.assertNotIn('sampling',inspect(root/'input.fits')['text'])
            self.assertEqual(match(root)['status'],'review')
            make(root/'input.fits',{'PSLAYOUT':quote('MONO')},remove=['BAYERPAT'])
            make(root/'master.fits',{'IMAGETYP':quote('Dark'),'PSLAYOUT':quote('MONO')},remove=['BAYERPAT'])
            self.assertEqual(match(root)['status'],'compatible')
            make(root/'master.fits',{'IMAGETYP':quote('Dark'),'NAXIS':3,'NAXIS3':3},remove=['BAYERPAT'])
            self.assertEqual(match(root)['status'],'incompatible')
            p=root/'broken.fits'; make(p); p.write_bytes(p.read_bytes()[:2880+12*10*4-1])
            run('calibration-inspect',p,ok=False)
            p.write_bytes(b'COMMENT '.ljust(80)*14000)
            run('calibration-inspect',p,ok=False)
            make(p,{'NAXIS':quote('2')}); run('calibration-inspect',p,ok=False)
            make(p,{'INSTRUME':"'unclosed"})
            self.assertIn('camera',[i['field'] for i in inspect(p)['issues']])

    def test_different_devices_readout_gain_binning_and_units_are_not_interchangeable(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); make(root/'input.fits',{'EGAIN':.43})
            for changes,field in [({'INSTRUME':quote('another model')},'camera'),
                                  ({'CAM-SER':quote('another camera')},'cameraSerial'),
                                  ({'READMODE':quote('slow')},'readoutMode'),({'GAIN':101},'gain'),
                                  ({'OFFSET':21},'offset'),({'XBINNING':2},'binX'),
                                  ({'YBINNING':2},'binY'),({'BUNIT':quote('electron')},'units'),
                                  ({'EGAIN':.44},'electronsPerADU'),({'NAXIS1':14},'width'),
                                  ({'NAXIS2':12},'height')]:
                with self.subTest(field=field):
                    make(root/'master.fits',{'IMAGETYP':quote('Dark'),'EGAIN':.43,**changes})
                    r=match(root)
                    self.assertEqual(r['status'],'incompatible')
                    self.assertEqual(next(e['status'] for e in r['evidence'] if e['field']==field),'conflict')

    def test_plan_groups_two_nights_filters_and_exposures_checks_every_member(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); records=[]
            for night in [15,16]:
                for filt in ['Ha','OIII']:
                    for exposure in [60,120]:
                        for j,temp in enumerate([-10,-9.5,-8.9]):
                            p=root/f'{night}-{filt}-{exposure}-{j}.fits'
                            make(p,{'OBJECT':quote('Veil'),'FILTER':quote(filt),'EXPTIME':exposure,
                                    'CCD-TEMP':temp,'DATE-OBS':quote(f'2026-10-{night}T22:0{j}:00')})
                            records.append(('light',p,'-'))
            for exposure in [60,120]:
                for temp in [-10,-9.5]:
                    p=root/f'dark-{exposure}-{temp}.fits'
                    make(p,{'IMAGETYP':quote('Master Dark'),'EXPTIME':exposure,'CCD-TEMP':temp})
                    records.append(('dark',p,'-'))
            for filt in ['Ha','OIII','SII']:
                p=root/f'flat-{filt}.fits'
                make(p,{'IMAGETYP':quote('Master Flat'),'FILTER':quote(filt),'EXPTIME':.2,'CCD-TEMP':20})
                records.append(('flat',p,'-'))
            hashes={p:hashlib.sha256(p.read_bytes()).hexdigest() for _,p,_ in records}
            result=plan(root,records)
            self.assertEqual(len(result['groups']),8)
            self.assertEqual(result['dateGrouping'],'UTC-calendar-date')
            self.assertFalse(result['canApply']); self.assertFalse(result['pixelsValidated'])
            for g in result['groups']:
                self.assertEqual(len(g['members']),3)
                self.assertEqual(g['roles']['bias']['status'],'missing-master')
                self.assertEqual(g['roles']['dark']['status'],'unique-compatible')
                selected=result['masters'][g['roles']['dark']['suggestedMaster']]['acquisition']
                self.assertEqual(selected['numeric']['sensorTemperature'],-9.5)
                self.assertEqual(selected['numeric']['exposure'],g['numeric']['exposure'])
                flat=result['masters'][g['roles']['flat']['suggestedMaster']]['acquisition']
                self.assertEqual(flat['text']['filter'],g['text']['filter'])
                for c in g['candidates']:
                    m=result['masters'][c['master']]
                    if m['role']=='dark' and m['acquisition']['numeric']['exposure']==g['numeric']['exposure'] and m['acquisition']['numeric']['sensorTemperature']==-10:
                        self.assertEqual((c['compatibleCount'],c['incompatibleCount']),(2,1))
                        issue=next(i for i in c['issues'] if i['example']['field']=='sensorTemperature')
                        self.assertEqual(issue['count'],1)
                        self.assertEqual(result['lights'][issue['firstMember']]['numeric']['sensorTemperature'],-8.9)
            reverse=plan(root,records[::-1]); result.pop('manifestSHA'); reverse.pop('manifestSHA')
            self.assertEqual(result,reverse)
            for p,sha in hashes.items(): self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(),sha)

    def test_plan_ambiguity_review_and_wrong_flat(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); light=root/'light.fits'; first=root/'first.fits'; second=root/'second.fits'; flat=root/'flat.fits'
            make(light,{'OBJECT':quote('Veil')})
            make(first,{'IMAGETYP':quote('Dark')})
            make(second,{'IMAGETYP':quote('Dark'),'CCD-TEMP':-9.5})
            make(flat,{'IMAGETYP':quote('Flat'),'FILTER':quote('SII')})
            records=[('light',light,'-'),('dark',first,'-'),('dark',second,'-'),('flat',flat,'-')]
            result=plan(root,records)['groups'][0]
            self.assertEqual(result['roles']['dark']['status'],'ambiguous')
            self.assertIsNone(result['roles']['dark']['suggestedMaster'])
            self.assertEqual(len(result['roles']['dark']['compatibleMasters']),2)
            self.assertEqual(result['roles']['flat']['status'],'no-compatible-master')
            make(first,{'IMAGETYP':quote('Dark')},remove=['CCD-TEMP'])
            make(second,{'IMAGETYP':quote('Dark'),'EXPTIME':120})
            result=plan(root,records)['groups'][0]
            self.assertEqual(result['roles']['dark']['status'],'review-required')
            self.assertIsNone(result['roles']['dark']['suggestedMaster'])

    def test_plan_utc_dates_unknown_identity_and_mixed_settings(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); records=[]
            variants=[{'OBJECT':quote('Veil'),'DATE-OBS':quote('2024-02-29T23:59:59.5')},
                      {'OBJECT':quote('Veil'),'DATE-OBS':quote('2024-02-29T23:59:59.6')},
                      {'OBJECT':quote('Veil'),'DATE-OBS':quote('2026-02-29T23:59:59')},
                      {'OBJECT':quote('Veil'),'TIMESYS':quote('TAI')},
                      {'OBJECT':quote('Veil'),'TIMESYS':quote('UTC'),'DATE-OBS':quote('2024-02-29'),'GAIN':101},
                      {'OBJECT':quote('M31'),'DATE-OBS':quote('2024-02-29')},
                      {'DATE-OBS':quote('2024-02-29T22:00:00')},
                      {'DATE-OBS':quote('2024-02-29T22:00:01')}]
            for i,change in enumerate(variants):
                p=root/f'frame-{i}.fits'; make(p,change); records.append(('light',p,'-'))
            r=plan(root,records)
            self.assertEqual(len(r['groups']),7)
            merged=next(g for g in r['groups'] if len(g['members'])==2)
            self.assertEqual(merged['text']['utcDate'],'2024-02-29')
            self.assertEqual(sum(bool(g['issues']) for g in r['groups']),4)

    def test_plan_retains_per_frame_choices_when_one_dark_cannot_cover_whole_group(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); records=[]
            for i,temp in enumerate([-12,-12,-8,-8]):
                p=root/f'light-{i}.fits'
                make(p,{'OBJECT':quote('Veil'),'CCD-TEMP':temp,'DATE-OBS':quote(f'2026-10-15T22:0{i}:00')})
                records.append(('light',p,'-'))
            for temp in [-12,-8]:
                p=root/f'dark-{temp}.fits'; make(p,{'IMAGETYP':quote('Dark'),'CCD-TEMP':temp})
                records.append(('dark',p,'-'))
            r=plan(root,records)
            self.assertEqual(len(r['groups']),1)
            self.assertEqual(r['groups'][0]['roles']['dark']['status'],'no-compatible-master')
            self.assertEqual(len(r['frameChoices']),4)
            for choice in r['frameChoices']:
                self.assertEqual(choice['group'],0)
                selected=choice['roles']['dark']; self.assertEqual(selected['status'],'unique-compatible')
                master=r['masters'][selected['suggestedMaster']]['acquisition']
                light=r['lights'][choice['light']]
                self.assertEqual(master['numeric']['sensorTemperature'],light['numeric']['sensorTemperature'])

    def test_plan_rejects_duplicates_stale_sources_and_invalid_manifests(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); p=root/'light.fits'; q=root/'copy.fits'
            make(p,{'OBJECT':quote('Veil')}); q.write_bytes(p.read_bytes())
            manifest=root/'bad.txt'
            prefix='PhotonStack-CalibrationPlan-1\ntemperature-tolerance 1\n'
            invalid=[prefix, prefix+f'light "{p}" -\nlight "{q}" -\n',
                     prefix+f'light "{p}" '+('0'*64)+'\n',
                     prefix+f'light {p} -\n', prefix+'light "relative.fits" -\n',
                     prefix+f'light "{p}" - extra\n', prefix+f'unknown "{p}" -\n',
                     prefix.replace('tolerance 1','tolerance nan')+f'light "{p}" -\n']
            for content in invalid:
                manifest.write_text(content); run('calibration-plan','--manifest',manifest,ok=False)
            for records in [f'light "{p}" -\n'*4097,
                            f'light "{p}" -\n'+f'dark "{q}" -\n'*257,
                            f'light "{p}" -\n'*2049+f'dark "{q}" -\n'*128]:
                manifest.write_text(prefix+records)
                run('calibration-plan','--manifest',manifest,ok=False)
            make(p,{'OBJECT':quote('Veil'),'IMAGETYP':quote('Flat')})
            manifest.write_text(prefix+f'light "{p}" -\n')
            run('calibration-plan','--manifest',manifest,ok=False)


if __name__ == '__main__': unittest.main(argv=[__file__])
