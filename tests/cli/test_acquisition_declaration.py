"""Declared copies preserve scientific bytes and retain an auditable source binding."""
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from test_channels import run, write
from test_sensor_master import frame


def payload(path):
    data = path.read_bytes()
    for i in range(0, len(data), 80):
        if data[i:i+8].strip() == b'END':
            return data[((i+80+2879)//2880)*2880:]
    raise AssertionError('missing END')


def declaration(root, source, fields, reason='camera log'):
    manifest = root/'declarations.txt'
    text = 'PhotonStack-AcquisitionDeclaration-1\nreason '+json.dumps(reason)+'\n'
    text += 'source '+json.dumps(str(source))+' '+hashlib.sha256(source.read_bytes()).hexdigest()+'\n'
    text += ''.join('set '+key+' '+json.dumps(str(value))+'\n' for key,value in fields.items())
    manifest.write_text(text)
    return manifest


class Declarations(unittest.TestCase):
    def test_preserves_payload_and_extensions_and_replaces_aliases(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            source=frame(root/'source.fits',[-20+i/4 for i in range(120)],'Light',EXPOSURE=60,
                         CTYPE1="'RA---TAN'",CRVAL1=313.4,CRVAL2=29.7)
            with source.open('ab') as out: out.write(b'extension bytes'.ljust(2880,b' '))
            original=source.read_bytes()
            manifest=declaration(root,source,{'EXPTIME':'120','CAM-SER':"camera's 2",'PSOPTICS':'new train'})
            result=run('calibration-declare','--manifest',manifest,'--output-dir',root/'copy')
            target=root/'copy/frame-000001.fits'
            self.assertEqual(payload(source),payload(target))
            self.assertEqual(source.read_bytes(),original)
            inspected=run('calibration-inspect',target)['entries'][0]['acquisition']
            self.assertEqual(inspected['numeric']['exposure'],120)
            self.assertEqual(inspected['text']['cameraSerial'],"camera's 2")
            self.assertEqual(inspected['sourceKeys']['exposure'],['EXPTIME'])
            self.assertFalse(inspected['issues'])
            self.assertEqual(result['frames'][0]['sourceSHA'],hashlib.sha256(original).hexdigest())
            self.assertEqual(result['frames'][0]['outputSHA'],hashlib.sha256(target.read_bytes()).hexdigest())
            self.assertEqual(json.loads((root/'copy/declarations.json').read_text()),result)
            self.assertIn(b'CRVAL1',target.read_bytes()[:2880])
            run('calibration-declare','--manifest',manifest,'--output-dir',root/'copy',ok=False)
            self.assertEqual(payload(source),payload(target))

    def test_rejects_physical_changes_invalid_values_and_changed_source(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); source=frame(root/'source.fits',[10]*120,'Light')
            for key,value in [('BITPIX','16'),('BAYERPAT','BGGR'),('PSCALIB','none'),('GAIN','nan'),('EXPTIME','-1'),('XBINNING','1.5')]:
                m=declaration(root,source,{key:value})
                run('calibration-declare','--manifest',m,'--output-dir',root/'bad',ok=False)
                self.assertFalse((root/'bad').exists())
                self.assertFalse(list(root.glob('.photonstack-product-*')))
            m=declaration(root,source,{'FILTER':'Ha'})
            source.write_bytes(source.read_bytes()+b'changed')
            run('calibration-declare','--manifest',m,'--output-dir',root/'changed',ok=False)
            self.assertFalse((root/'changed').exists())

if __name__=='__main__': unittest.main()
