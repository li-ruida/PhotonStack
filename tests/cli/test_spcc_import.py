"""Optional scientific dependency tests: run with NumPy and Astropy installed."""
import csv
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
import warnings
import numpy as np
from astropy.io import fits
from astropy.coordinates import SkyCoord
from astropy.time import Time
import astropy.units as u

SCRIPT=Path(__file__).resolve().parents[2]/'tools/scripts/import-spcc-data.py'


def fixture(root):
    sensor=[dict(type='OSC_SENSOR',channel=c,name=c,dataSource='Synthetic test data',dataQualityMarker=5,
                 wavelength=dict(units='nm',value=[400,700]),values=dict(range=1,value=[v,v]))
            for c,v in zip(['RED','GREEN','BLUE'],[1,.8,.6])]
    (root/'sensor.json').write_text(json.dumps(sensor)); (root/'LICENSE').write_text('CC0 synthetic test fixture\n')
    columns=[fits.Column(name='WAVELENGTH',format='D',unit='ANGSTROMS',array=[3500.,7500.]),
             fits.Column(name='FLUX',format='D',unit='FLAM',array=[2.,2.])]
    fits.HDUList([fits.PrimaryHDU(),fits.BinTableHDU.from_columns(columns)]).writeto(root/'white.fits')
    with (root/'gaia.csv').open('w') as f:
        writer=csv.writer(f); writer.writerow(['source_id','ra','dec','pmra','pmdec','phot_g_mean_mag'])
        writer.writerow(['123','359.999','60','1000','-200','10']); writer.writerow(['456','1','30','','','11'])
    with (root/'xp-0.download').open('w') as f:
        writer=csv.writer(f); writer.writerow(['source_id','ra','dec','flux','flux_error'])
        flux=np.full(343,2e-15); flux[100]=-1e-16
        for sid,ra,dec in [('123','359.999','60'),('456','1','30')]:
            writer.writerow([sid,ra,dec,'('+','.join(map(str,flux))+')','('+','.join(['1e-17']*343)+')'])


def run(root,name='result',ok=True):
    command=[sys.executable,str(SCRIPT),'--sensor',str(root/'sensor.json'),'--response-license-file',str(root/'LICENSE'),
             '--ideal-cutoff','400','700','--white-calspec',str(root/'white.fits'),'--white-name','Synthetic constant F_lambda',
             '--white-source','Test fixture','--white-license','CC0','--profile-name','Synthetic OSC',
             '--gaia-catalog',str(root/'gaia.csv'),'--xp-dir',str(root),'--observation-date','2026-08-18T20:04:18',
             '--output-dir',str(root/name)]
    result=subprocess.run(command,capture_output=True,text=True)
    if ok:
        if result.returncode: raise AssertionError(result.stderr)
        return json.loads(result.stdout)
    if not result.returncode: raise AssertionError('Invalid import succeeded')


class SpectralImport(unittest.TestCase):
    def test_motion_units_signed_spectra_provenance_and_reimport(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); fixture(root); original={p.name:p.read_bytes() for p in root.iterdir()}
            result=run(root); self.assertEqual(result['references'],1)
            self.assertEqual(result['skipped'],[{'id':'456','reason':'proper-motion-missing'}])
            tokens=iter(shlex.split((root/'result/catalog.txt').read_text()))
            self.assertEqual(next(tokens),'PhotonStack-SpectralCatalog-1');next(tokens);epoch=float(next(tokens));next(tokens);next(tokens)
            self.assertEqual(int(next(tokens)),1);self.assertEqual(next(tokens),'123');ra,dec=float(next(tokens)),float(next(tokens))
            self.assertEqual(int(next(tokens)),343)
            spectrum=np.array([[float(next(tokens)) for _ in range(3)] for i in range(343)])
            self.assertEqual(spectrum[100,1],-1e-16);self.assertEqual(spectrum[0,0],336);self.assertEqual(spectrum[-1,0],1020)
            with warnings.catch_warnings():
                warnings.simplefilter('ignore')
                expected=SkyCoord(ra=359.999*u.deg,dec=60*u.deg,pm_ra_cosdec=1000*u.mas/u.yr,pm_dec=-200*u.mas/u.yr,
                                  obstime=Time(2016,format='jyear')).apply_space_motion(new_obstime=Time(epoch,format='jyear'))
            self.assertLess(SkyCoord(ra=ra*u.deg,dec=dec*u.deg).separation(expected).arcsec, .001)
            profile=shlex.split((root/'result/profile.txt').read_text())
            self.assertEqual(profile[-5:],['2','350','0.02','750','0.02'])
            self.assertEqual((root/'result/response-source-license.txt').read_bytes(),original['LICENSE'])
            run(root,'again')
            self.assertEqual({p.name:p.read_bytes() for p in (root/'result').iterdir()},
                             {p.name:p.read_bytes() for p in (root/'again').iterdir()})
            self.assertEqual(original,{p.name:p.read_bytes() for p in root.iterdir() if p.is_file()})
            run(root,ok=False)

    def test_mismatched_identity_bad_uncertainty_and_bad_response_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);fixture(root)
            path=root/'gaia.csv';old=path.read_text();path.write_text(old.replace('359.999','359.998'))
            run(root,ok=False);self.assertFalse((root/'result').exists());path.write_text(old)
            path=root/'xp-0.download';old=path.read_text();path.write_text(old.replace('1e-17','-1e-17',1))
            run(root,ok=False);self.assertFalse((root/'result').exists());path.write_text(old)
            path=root/'sensor.json';records=json.loads(path.read_text());records[0]['values']['value'][0]=1.1;path.write_text(json.dumps(records))
            run(root,ok=False);self.assertFalse((root/'result').exists())


if __name__=='__main__': unittest.main()
