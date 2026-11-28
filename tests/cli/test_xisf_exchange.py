"""Independent XISF/FITS byte fixtures; no production reader used as the oracle."""
import ctypes
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

CLI=Path(sys.argv.pop(1)).resolve()
FORMATS={'UInt8':('B',8,0),'UInt16':('H',16,32768),'UInt32':('I',32,2147483648),'Float32':('f',-32,0),'Float64':('d',-64,0)}

def codec_compress(codec,data):
    if codec=='zlib':return zlib.compress(data)
    if codec=='lz4':
        lib=ctypes.CDLL('/opt/homebrew/lib/liblz4.dylib');lib.LZ4_compressBound.argtypes=[ctypes.c_int];lib.LZ4_compressBound.restype=ctypes.c_int
        capacity=lib.LZ4_compressBound(len(data));out=ctypes.create_string_buffer(capacity)
        lib.LZ4_compress_default.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int];lib.LZ4_compress_default.restype=ctypes.c_int
        n=lib.LZ4_compress_default(data,out,len(data),capacity);assert n>0;return out.raw[:n]
    lib=ctypes.CDLL('/opt/homebrew/lib/libzstd.dylib');lib.ZSTD_compressBound.argtypes=[ctypes.c_size_t];lib.ZSTD_compressBound.restype=ctypes.c_size_t
    capacity=lib.ZSTD_compressBound(len(data));out=ctypes.create_string_buffer(capacity)
    lib.ZSTD_compress.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_int];lib.ZSTD_compress.restype=ctypes.c_size_t
    n=lib.ZSTD_compress(out,capacity,data,len(data),3);assert 0<n<=capacity;return out.raw[:n]

def fixture(path,fmt='UInt16',channels=1,endian='little',storage='planar',codec=None,shuffle=False,subblocks=False,extra='',attributes=None):
    code,_,_=FORMATS[fmt];width,height=8,6;plane=width*height
    if fmt.startswith('UInt'):
        limit=2**(struct.calcsize(code)*8)-1;values=[(p*451+c*971)%limit for c in range(channels) for p in range(plane)];values[0]=0;values[1]=limit
    else:
        values=[-17+c*5+p*.3125 for c in range(channels) for p in range(plane)];values[:4]=[float('nan'),float('inf'),float('-inf'),-0.0]
        if fmt=='Float64':values[4:7]=[1+2**-40,1e-250,1e250]
    stored=values if storage=='planar' else [values[c*plane+p] for p in range(plane) for c in range(channels)]
    data=struct.pack(('<' if endian=='little' else '>')+str(len(stored))+code,*stored);size=struct.calcsize(code)
    if shuffle:data=b''.join(data[k::size] for k in range(size))
    attrs={'geometry':f'{width}:{height}:{channels}','sampleFormat':fmt,'colorSpace':'Gray' if channels==1 else 'RGB','byteOrder':endian,'pixelStorage':storage.title()}
    if codec:
        parts=[data[:len(data)//2],data[len(data)//2:]] if subblocks else [data];encoded=[codec_compress(codec,p) for p in parts]
        attrs['compression']=codec+('+sh' if shuffle else '')+':'+str(len(data))+(':'+str(size) if shuffle else '')
        if subblocks:attrs['subblocks']=':'.join(f'{len(a)},{len(b)}' for a,b in zip(encoded,parts))
        data=b''.join(encoded)
    attrs['location']=f'attachment:4096:{len(data)}';attrs['checksum']='sha-256:'+hashlib.sha256(data).hexdigest()
    if attributes:attrs.update(attributes)
    image=ET.Element('Image',attrs)
    if channels==1:ET.SubElement(image,'ColorFilterArray',{'width':'2','height':'2','pattern':'GRBG'})
    for name,value in [('CRPIX1','3.25'),('CRPIX2','4.5'),('CD1_1','-0.001'),('CD1_2','0'),('CD2_1','0'),('CD2_2','0.001'),('CTYPE1',"'RA---TAN'"),('CTYPE2',"'DEC--TAN'"),('CRVAL1','123.5'),('CRVAL2','40'),('BUNIT',"'ADU'"),('PSCOLOR',"'Linear'")]:
        ET.SubElement(image,'FITSKeyword',{'name':name,'value':value,'comment':''})
    text='<xisf version="1.0">'+ET.tostring(image,encoding='unicode')+extra+'</xisf>';xml=text.encode()
    path.write_bytes((b'XISF0100'+struct.pack('<II',len(xml),0)+xml).ljust(4096,b'\0')+data)
    return values

def fits(path):
    raw=path.read_bytes();header={};offset=0
    while True:
        card=raw[offset:offset+80].decode();offset+=80;name=card[:8].strip()
        if name=='END':break
        if card[8]=='=':header[name]=card[10:].split(' / ')[0].strip()
    start=(offset+2879)//2880*2880;bit=int(header['BITPIX']);count=math.prod(int(header[f'NAXIS{i}']) for i in range(1,int(header['NAXIS'])+1))
    code={8:'B',16:'h',32:'i',-32:'f',-64:'d'}[bit];values=struct.unpack_from('>'+str(count)+code,raw,start)
    if bit>0:values=tuple(v+int(header.get('BZERO','0')) for v in values)
    return header,values,raw[start:start+count*abs(bit)//8]

def run(command,source,dest,*options,ok=True):
    p=subprocess.run([CLI,command,'--input',source,'--output-dir',dest,*options],capture_output=True,text=True)
    if ok and p.returncode:raise AssertionError(p.stdout+p.stderr)
    if not ok and p.returncode==0:raise AssertionError('Unexpected success')
    return p

class Xisf(unittest.TestCase):
    def equalValues(self,a,b):
        self.assertEqual(len(a),len(b))
        for x,y in zip(a,b):
            if math.isnan(x):self.assertTrue(math.isnan(y))
            else:self.assertEqual(x,y)
    def test_lossless_formats_byte_order_channels_and_roundtrip(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for fmt in FORMATS:
                for endian in ['little','big']:
                    for channels in [1,3,4]:
                        name=f'{fmt}-{endian}-{channels}';source=root/(name+'.xisf');expected=fixture(source,fmt,channels,endian,'normal' if channels>1 else 'planar')
                        before=hashlib.sha256(source.read_bytes()).hexdigest();out=root/name;run('xisf-import',source,out)
                        header,values,data=fits(out/'image.fits');self.equalValues(expected,values)
                        self.assertEqual(header['CRPIX1'],'3.25');self.assertEqual(header['PSCOLOR'],"'Linear'")
                        if channels==1:self.assertEqual(header['BAYERPAT'],"'GRBG'")
                        exported=root/(name+'-export');run('xisf-export',out/'image.fits',exported)
                        second=root/(name+'-second');run('xisf-import',exported/'image.xisf',second)
                        self.assertEqual(fits(second/'image.fits')[2],data)
                        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)
                        manifest=json.loads((out/'exchange.json').read_text());self.assertEqual(manifest['sourceSHA256'],before)
                        self.assertEqual(manifest['productSHA256'],hashlib.sha256((out/'image.fits').read_bytes()).hexdigest())
    def test_fits_commentary_cards_round_trip_all_72_columns(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            history = 'HISTORY ' + '0123456789ABCDEF' * 4 + '01234567'
            comment = 'COMMENT ' + '  observer / note'.ljust(72)
            header = ''.join((f'{key:<8}= {value:>20}').ljust(80)
                             for key, value in [('SIMPLE', 'T'), ('BITPIX', 8), ('NAXIS', 2), ('NAXIS1', 1), ('NAXIS2', 1)])
            source = root / 'commentary.fits'
            source.write_bytes((header + history + comment + 'END').ljust(2880).encode() + bytes(2880))
            run('xisf-export', source, root / 'exported', '--compression', 'none')
            run('xisf-import', root / 'exported/image.xisf', root / 'imported')
            result = (root / 'imported/image.fits').read_bytes()[:2880]
            cards = [result[i:i + 80].decode() for i in range(0, len(result), 80)]
            self.assertIn(history, cards)
            self.assertIn(comment, cards)

    def test_compression_shuffle_subblocks_and_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            codecs=['zlib']+[c for c in ['lz4','zstd'] if Path('/opt/homebrew/lib/lib'+c+'.dylib').exists()]
            for codec in codecs:
                for shuffle in [False,True]:
                    for sub in [False,True]:
                        name=f'{codec}-{shuffle}-{sub}';source=root/(name+'.xisf');expected=fixture(source,'Float64',3,'big','normal',codec,shuffle,sub)
                        p=run('xisf-import',source,root/name)
                        self.equalValues(expected,fits(root/name/'image.fits')[1])
                        bad=bytearray(source.read_bytes());bad[-1]^=1;source.write_bytes(bad)
                        run('xisf-import',source,root/(name+'-bad'),ok=False);self.assertFalse((root/(name+'-bad')).exists())
    def test_malformed_unsupported_and_multiple_images(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for n,attrs in enumerate([{'geometry':'999999999999999:2:3'},{'orientation':'180'},{'location':'url:https://example.com/data'},{'compression':'zlib:9999999999'},{'pixelStorage':'other'},{'byteOrder':'other'},{'checksum':'sha256:0000'},{'compression':'zlib:96','subblocks':'500,200'},{'location':'attachment:0:96'}]):
                source=root/f'bad{n}.xisf';fixture(source,attributes=attrs);run('xisf-import',source,root/f'bad{n}',ok=False)
            source=root/'multiple.xisf';expected=fixture(source,extra='<Image geometry="8:6:1" sampleFormat="UInt16" location="attachment:4096:96"/>')
            run('xisf-import',source,root/'second','--image','1');self.equalValues(expected,fits(root/'second'/'image.fits')[1])
            run('xisf-import',source,root/'absent','--image','2',ok=False)
            data=source.read_bytes();xml=b'<!DOCTYPE xisf [<!ENTITY evil SYSTEM "file:///etc/passwd">]><xisf version="1.0">&evil;</xisf>'
            source.write_bytes(b'XISF0100'+struct.pack('<II',len(xml),0)+xml);run('xisf-import',source,root/'entities',ok=False)
            source.write_bytes(data[:-10]);run('xisf-import',source,root/'truncated',ok=False)
            source.write_bytes(data);run('xisf-import',source,root/'good');before=(root/'good'/'image.fits').read_bytes();run('xisf-import',source,root/'good',ok=False);self.assertEqual((root/'good'/'image.fits').read_bytes(),before)
            self.assertFalse(list(root.glob('.photonstack-product-*')))

if __name__=='__main__':unittest.main()
