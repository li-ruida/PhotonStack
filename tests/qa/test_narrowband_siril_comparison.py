"""QA comparator regression. Run with the NumPy/SciPy/Astropy QA environment."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import numpy as np
from astropy.io import fits

SCRIPT=Path(__file__).resolve().parents[2]/'tools/scripts/qa/narrowband_siril_comparison.py'

class Comparison(unittest.TestCase):
    def test_missing_edge_support_is_excluded_without_creating_black_signal(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);y,x=np.mgrid[:256,:256];ha=np.full((256,256),1000.0)
            for cy in range(24,240,32):
                for cx in range(24,240,32):ha+=2000*np.exp(-((x-cx)**2+(y-cy)**2)/8)
            ha=ha.astype(np.float32);oxygen=(500+.6*ha).astype(np.float32)
            masked=oxygen.copy();masked[[0,-1],:]=np.nan;masked[:,[0,-1]]=np.nan
            for name,data in [('ha',ha),('oxygen',masked),('siril-ha',ha),('siril-oxygen',oxygen)]:fits.writeto(root/(name+'.fits'),data)
            base=[sys.executable,str(SCRIPT),'--photonstack-ha',str(root/'ha.fits'),'--photonstack-oiii',str(root/'oxygen.fits'),'--siril-ha',str(root/'siril-ha.fits'),'--siril-oiii',str(root/'siril-oxygen.fits'),'--siril-scale','1']
            out=root/'result.json';p=subprocess.run(base+['--output',str(out)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
            result=json.loads(out.read_text());self.assertEqual(result['missing_samples_excluded'],1020);self.assertEqual(result['common_finite_samples'],254**2)
            self.assertEqual(result['OIII_full_rms'],0);self.assertAlmostEqual(result['channels']['OIII']['highpass_correlation'],1)
            self.assertEqual(result['unsaturated_stars']['flux_ratio_median'],1)
            self.assertEqual(result['unsaturated_stars']['centroid_distance_p95'],0)
            # An internal hole would contaminate FFT and Gaussian morphology.
            # Fail explicitly instead of filling it with invented black signal.
            masked[128,128]=np.nan;fits.writeto(root/'oxygen.fits',masked,overwrite=True)
            failed=root/'invalid.json';p=subprocess.run(base+['--output',str(failed)],capture_output=True,text=True)
            self.assertNotEqual(p.returncode,0);self.assertIn('Interior has missing support',p.stderr);self.assertFalse(failed.exists())

if __name__=='__main__':unittest.main()
