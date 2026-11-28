"""Independent SER byte fixtures: convention, planes, CFA, dates and failure atomicity."""
import hashlib
import json
import struct
from pathlib import Path
import tempfile
import unittest
from test_channels import read,run

TICKS=638397614451234567 # 2024-01-02T03:04:05.1234567

def fixture(path,depth=16,color=0,little=True,flag=None,timestamps=True,frames=4):
    channels=3 if color>=100 else 1
    w,h=8,6
    header=bytearray(178);header[:14]=b'LUCAM-RECORDER'
    struct.pack_into('<7I',header,14,0,color,int(not little) if flag is None else flag,w,h,depth,frames)
    header[42:42+5]=b"O'Neil"[:5];header[82:82+6]=b'Camera';header[122:122+5]=b'Scope'
    struct.pack_into('<QQ',header,162,TICKS if timestamps else 0,TICKS)
    data=bytearray();truth=[]
    for f in range(frames):
        samples=[(f*13+c*211+p*3)%((1<<depth)-1) for p in range(w*h) for c in range(channels)]
        truth.append(samples)
        if depth<=8:data+=bytes(v<<(8-depth) for v in samples)
        else:data+=struct.pack(('<' if little else '>')+f'{len(samples)}H',*samples)
    if timestamps:data+=struct.pack('<'+f'{frames}Q',*(TICKS+f*100000 for f in range(frames)))
    path.write_bytes(header+data);return truth

class SerTests(unittest.TestCase):
    def test_byte_order_depth_cfa_and_rgb_are_exact(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d)
            for depth,color,little in [(1,0,True),(5,0,True),(8,0,True),(12,8,True),(16,9,False),(16,10,True),(16,11,False),(8,100,True),(16,101,False)]:
                name=f'{depth}-{color}-{little}';source=r/(name+'.ser');truth=fixture(source,depth,color,little)
                before=hashlib.sha256(source.read_bytes()).hexdigest()
                i=run('ser-inspect','--input',source)
                self.assertEqual(i['littleEndian'],little);self.assertEqual(i['channels'],3 if color>=100 else 1)
                output=r/name
                result=run('ser-extract','--input',source,'--output-dir',output,'--first',1,'--count',2,'--step',2)
                self.assertEqual(result['extracted'],2);self.assertEqual(result['missingTimestamps'],0)
                manifest=json.loads((output/'sequence.json').read_text());self.assertEqual(manifest['sourceSHA256'],before)
                for index in [1,3]:
                    h,p=read(output/f'frame-{index:08}.fits')
                    self.assertEqual(int(h['SERFRAME']),index)
                    for c in range(len(p)):
                        channel=2-c if color==101 else c
                        self.assertEqual(list(p[c]),truth[index][channel::len(p)])
                    if 8<=color<=11:self.assertEqual(h['BAYERPAT'].strip("'"),['RGGB','GRBG','GBRG','BGGR'][color-8])
                    self.assertEqual(h['DATE-OBS'].strip("'"),f'2024-01-02T03:04:05.{1234567+index*100000:07}')
                self.assertEqual(before,hashlib.sha256(source.read_bytes()).hexdigest())

    def test_explicit_specification_override_and_absent_frame_times(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);source=r/'spec.ser';truth=fixture(source,16,100,True,1,False)
            run('ser-extract','--input',source,'--output-dir',r/'out','--count',1,'--byte-order','specification')
            h,p=read(r/'out/frame-00000000.fits');self.assertEqual(list(p[0]),truth[0][0::3]);self.assertNotIn('DATE-OBS',h)
            report=json.loads((r/'out/sequence.json').read_text());self.assertIsNone(report['selected'][0]['utcTicks']);self.assertEqual(report['missingTimestamps'],1)
            self.assertFalse(run('ser-inspect','--input',source)['littleEndian'])
            self.assertTrue(run('ser-inspect','--input',source,'--byte-order','little')['littleEndian'])

    def test_malformed_ranges_metadata_and_partial_outputs(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);source=r/'source.ser';fixture(source,12)
            for args in [('--first','4'),('--step','0'),('--count','0'),('--count','10001'),('--first','3','--count','2'),('--byte-order','auto'),('--count','1.5')]:
                run('ser-extract','--input',source,'--output-dir',r/'bad',*args,ok=False);self.assertFalse((r/'bad').exists())
            original=source.read_bytes()
            cases=[]
            cases.append(original[:-1]);cases.append(original+b'x')
            b=bytearray(original);struct.pack_into('<I',b,18,16);cases.append(b) # unsupported CMYG
            b=bytearray(original);struct.pack_into('<I',b,22,2);cases.append(b)
            b=bytearray(original);b[82]=255;cases.append(b)
            b=bytearray(original);struct.pack_into('<H',b,178+96,65535);cases.append(b) # frame 2 contradicts 12-bit header
            for n,broken in enumerate(cases):
                path=r/f'broken-{n}.ser';path.write_bytes(broken)
                run('ser-extract','--input',path,'--output-dir',r/'bad',ok=False)
                self.assertFalse((r/'bad').exists());self.assertFalse(list(r.glob('.photonstack-product-*')))
            run('ser-extract','--input',source,'--output-dir',r/'good','--count',1)
            saved=(r/'good/sequence.json').read_bytes()
            run('ser-extract','--input',source,'--output-dir',r/'good',ok=False)
            self.assertEqual(saved,(r/'good/sequence.json').read_bytes())

if __name__=='__main__':unittest.main()
