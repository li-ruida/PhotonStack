"""Signed, masked, WCS-preserving linear layer arithmetic independent of a model."""
import hashlib
import math
import struct
from pathlib import Path
import tempfile
import unittest
from test_channels import write,read,run

WCS={'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'RADESYS':"'ICRS'",'CRPIX1':12,'CRPIX2':10,
     'CRVAL1':30,'CRVAL2':40,'CD1_1':-.001,'CD1_2':0,'CD2_1':0,'CD2_2':.001,'PSCOLOR':"'Linear'"}

class LinearLayers(unittest.TestCase):
    def test_signed_layers_linear_gain_and_byte_exact_identity(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);values=[-40+2*x+3*y for y in range(20) for x in range(24)]
            values[0]=math.nan
            removed=[v+3 if i%3==0 else v-7 for i,v in enumerate(values)]
            write(r/'source.fits',values,24,20,WCS);write(r/'removed.fits',removed,24,20,WCS)
            original=(r/'source.fits').read_bytes()
            report=run('star-layers','linear-split','--input',r/'source.fits','--starless',r/'removed.fits','--output-dir',r/'layers')
            self.assertGreater(report['negativeSamples'],0);self.assertEqual(report['maskedPixels'],1)
            _,stars=read(r/'layers/stars.fits');_,starless=read(r/'layers/starless.fits')
            for i,v in enumerate(values):
                if i==0:
                    self.assertTrue(math.isnan(stars[0][i]));self.assertEqual(stars[3][i],0)
                else:self.assertEqual(stars[0][i]+starless[0][i],v)
            run('star-layers','linear-render','--layers',r/'layers','--output-dir',r/'identity')
            self.assertEqual(original,(r/'identity/combined.fits').read_bytes())
            run('star-layers','linear-render','--layers',r/'layers','--output-dir',r/'gain','--stars-ev',-1,'--nebula-ev',1)
            header,got=read(r/'gain/combined.fits')
            self.assertEqual(float(header['CRPIX1']),12);self.assertAlmostEqual(float(header['CD1_1']),-.001)
            for i,v in enumerate(values[1:],1):self.assertEqual(got[0][i],2*removed[i]+.5*(v-removed[i]))
            self.assertEqual((r/'source.fits').read_bytes(),original)
            run('star-layers','linear-render','--layers',r/'layers','--output-dir',r/'gamma','--nebula-gamma',2,ok=False)
            data=bytearray((r/'layers/stars.fits').read_bytes());data[2900]^=1;(r/'layers/stars.fits').write_bytes(data)
            run('star-layers','linear-render','--layers',r/'layers','--output-dir',r/'tampered',ok=False)

    def test_declared_pixel_units_are_checked_and_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, removed = root / 'source.fits', root / 'removed.fits'
            values = [1 + i / 10 for i in range(480)]
            write(source, values, 24, 20, WCS | {'BUNIT': "'mJy'"})
            write(removed, [v / 2 for v in values], 24, 20, WCS | {'BUNIT': "'MJy'"})
            def split(output, ok=True):
                return run('star-layers', 'linear-split', '--input', source, '--starless', removed,
                           '--output-dir', output, ok=ok)
            split(root / 'mismatched', ok=False)
            self.assertFalse((root / 'mismatched').exists())
            write(removed, [v / 2 for v in values], 24, 20, WCS | {'BUNIT': "'mJy'"})
            layers = root / 'layers'
            split(layers)
            run('star-layers', 'linear-render', '--layers', layers, '--stars-ev', -1, '--output-dir', root / 'adjusted')
            for product in [layers / 'starless.fits', layers / 'stars.fits', root / 'adjusted/combined.fits']:
                self.assertEqual(read(product)[0]['BUNIT'].strip("' "), 'mJy')

    def test_normalized_inference_adapter_restores_units_and_original_mask(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);values=[-300+200*x+300*y for y in range(20) for x in range(24)];values[0]=math.nan
            write(r/'source.fits',values,24,20,WCS)
            p=run('star-layers','linear-prepare','--input',r/'source.fits','--output-dir',r/'prepared')
            self.assertEqual(p['scale'],max(abs(v) for v in values[1:]));self.assertEqual(p['maskedPixels'],1)
            header,normalized=read(r/'prepared/input.fits')
            self.assertEqual(len(normalized),3)
            self.assertTrue(all(math.isfinite(v) and abs(v)<=1 for plane in normalized for v in plane))
            # Identity external prediction is sufficient to verify domain conversion,
            # not evidence of actual star removal; a real model is tested separately.
            (r/'prediction.fits').write_bytes((r/'prepared/input.fits').read_bytes())
            run('star-layers','linear-restore','--prepared',r/'prepared','--starless',r/'prediction.fits','--output-dir',r/'layers')
            _,actual=read(r/'layers/starless.fits')
            self.assertTrue(math.isnan(actual[0][0]));self.assertEqual(actual[3][0],0)
            for truth,v in zip(values[1:],actual[0][1:]):self.assertAlmostEqual(truth,v,delta=.001)
            run('star-layers','linear-render','--layers',r/'layers','--output-dir',r/'identity')
            self.assertEqual((r/'source.fits').read_bytes(),(r/'identity/combined.fits').read_bytes())
            (r/'prepared/input.fits').write_bytes(b'changed')
            run('star-layers','linear-restore','--prepared',r/'prepared','--starless',r/'prediction.fits','--output-dir',r/'changed',ok=False)

    def test_integer_blank_is_masked_before_physical_scaling(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d)
            for bitpix,fmt,blank in [(8,'B',255),(16,'h',-32768),(32,'i',-2147483648)]:
                cards={'SIMPLE':'T','BITPIX':bitpix,'NAXIS':2,'NAXIS1':24,'NAXIS2':20,
                       'BSCALE':2,'BZERO':-100,'BLANK':blank,'PSCOLOR':"'Linear'"}
                header=''.join(f'{k:<8}= {str(v):>20}'.ljust(80) for k,v in cards.items())+'END'
                header=header.ljust(((len(header)+2879)//2880)*2880)
                values=[blank]+[i%100 for i in range(1,480)]
                data=struct.pack('>'+fmt*len(values),*values)
                source=r/f'integer-{bitpix}.fits'
                source.write_bytes(header.encode()+data.ljust(((len(data)+2879)//2880)*2880,b'\0'))
                prepared=r/f'prepared-{bitpix}'
                report=run('star-layers','linear-prepare','--input',source,'--output-dir',prepared)
                self.assertEqual(report['maskedPixels'],1)
                self.assertEqual(report['scale'],100)
                _,got=read(prepared/'input.fits')
                for i in range(1,480): self.assertAlmostEqual(got[0][i],(values[i]*2-100)/100,delta=1e-7)
                layers=r/f'layers-{bitpix}'
                run('star-layers','linear-restore','--prepared',prepared,'--starless',prepared/'input.fits','--output-dir',layers)
                _,stars=read(layers/'stars.fits')
                self.assertTrue(math.isnan(stars[0][0]));self.assertEqual(stars[3][0],0)

    def test_rejects_display_cfa_moved_grid_and_missing_source_coverage(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);values=[10+i*.5 for i in range(480)]
            write(r/'source.fits',values,24,20,WCS)
            for i,extra in enumerate([WCS|{'CRPIX1':13},WCS|{'PSCOLOR':"'SRGB-DISPLAY'"},WCS|{'BAYERPAT':"'RGGB'"}]):
                write(r/'bad.fits',values,24,20,extra)
                run('star-layers','linear-split','--input',r/'source.fits','--starless',r/'bad.fits','--output-dir',r/f'bad-{i}',ok=False)
            values[20]=math.nan;write(r/'bad.fits',values,24,20,WCS)
            run('star-layers','linear-split','--input',r/'source.fits','--starless',r/'bad.fits','--output-dir',r/'missing',ok=False)

if __name__=='__main__':unittest.main()
