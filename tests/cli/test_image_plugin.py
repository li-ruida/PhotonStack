"""Exercise real external-process execution, provenance, rejection and timeout."""
import hashlib,json,os
from pathlib import Path
import struct,subprocess,sys,tempfile,time,unittest
CLI=Path(sys.argv.pop(1)).resolve()

def fits(path):
    values={'SIMPLE':'T','BITPIX':'-32','NAXIS':'3','NAXIS1':'32','NAXIS2':'32','NAXIS3':'4','PSCOLOR':"'Linear'",'BUNIT':"'ADU'"}
    header=''.join((f'{k:<8}= {v:>20}').ljust(80) for k,v in values.items())+'END'
    samples=[-20+p/10+c*4 if c<3 else 1 for c in range(4) for p in range(1024)]
    samples[10]=float('nan');samples[1024+10]=float('nan');samples[2048+10]=float('nan');samples[3072+10]=0
    data=struct.pack('>4096f',*samples);path.write_bytes(header.ljust(2880).encode()+data.ljust((len(data)+2879)//2880*2880,b'\0'))

def definition(path,exe,script,timeout=10):
    text='PhotonStack-ImagePlugin-1\n'
    for key,value in [('id','qa.gain'),('name','Gain fixture'),('executable',str(exe)),('sha256',hashlib.sha256(exe.read_bytes()).hexdigest()),('domain','linear-fits'),('grid','preserve'),('units','preserve'),('timeout',str(timeout)),('argument',str(script)),('argument','{input}'),('argument','{output}')]:text+=key+' '+json.dumps(value)+'\n'
    path.write_text(text)

def run(args,ok=True):
    p=subprocess.run([CLI,*map(str,args)],capture_output=True,text=True)
    if ok and p.returncode:raise AssertionError(p.stdout+p.stderr)
    if not ok and not p.returncode:raise AssertionError('Unexpected plugin success')
    return p

class Plugins(unittest.TestCase):
    def test_execution_is_explicit_and_provenance_is_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'source.fits';fits(source);original=source.read_bytes()
            script=root/'gain.py';script.write_text('import sys,struct,math\nfrom pathlib import Path\na=bytearray(Path(sys.argv[1]).read_bytes())\nfor n in range(3072):\n v=struct.unpack_from(">f",a,2880+n*4)[0]\n if math.isfinite(v):struct.pack_into(">f",a,2880+n*4,v*1.25)\nPath(sys.argv[2]).write_bytes(a)\n')
            config=root/'tool.psplugin';definition(config,Path(sys.executable).resolve(),script)
            inspected=json.loads(run(['plugin-inspect','--definition',config]).stdout);self.assertEqual(inspected['id'],'qa.gain');self.assertEqual(list(root.glob('*.log')),[])
            out=root/'result';run(['plugin-run','--definition',config,'--input',source,'--output-dir',out])
            self.assertEqual(source.read_bytes(),original);self.assertEqual((out/'input.fits').read_bytes(),original)
            for n in [0,11,500,2048]:self.assertAlmostEqual(struct.unpack_from('>f',(out/'result.fits').read_bytes(),2880+4*n)[0],struct.unpack_from('>f',original,2880+4*n)[0]*1.25,places=4)
            m=json.loads((out/'plugin.json').read_text());self.assertEqual(m['sourceSHA256'],hashlib.sha256(original).hexdigest());self.assertEqual(m['resultSHA256'],hashlib.sha256((out/'result.fits').read_bytes()).hexdigest())
            run(['plugin-run','--definition',config,'--input',source,'--output-dir',out],False)
            config.write_text(config.read_text().replace(m['executableSHA256'],'0'*64));run(['plugin-inspect','--definition',config],False)
    def test_invalid_output_input_mutation_timeout_and_atomic_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'source.fits';fits(source);original=source.read_bytes()
            programs={
              'missing':'pass',
              'exit':'raise SystemExit(9)',
              'invalid':'Path(sys.argv[2]).write_bytes(b"not fits")',
              'input':'Path(sys.argv[2]).write_bytes(a);Path(sys.argv[1]).write_bytes(b"changed")',
              'nan':'struct.pack_into(">f",a,2880,float("nan"));Path(sys.argv[2]).write_bytes(a)',
              'alpha':'struct.pack_into(">f",a,2880+3072*4,.5);Path(sys.argv[2]).write_bytes(a)',
              'units':'a=a.replace(b"ADU",b"mag");Path(sys.argv[2]).write_bytes(a)',
              'symlink':'Path(sys.argv[2]).symlink_to(sys.argv[1])',
              'timeout':'time.sleep(5)',
            }
            for name,body in programs.items():
                script=root/(name+'.py');script.write_text('import sys,struct,time\nfrom pathlib import Path\na=bytearray(Path(sys.argv[1]).read_bytes())\n'+body+'\n')
                config=root/(name+'.psplugin');definition(config,Path(sys.executable).resolve(),script,1)
                out=root/name;run(['plugin-run','--definition',config,'--input',source,'--output-dir',out],False)
                self.assertFalse(out.exists());self.assertEqual(source.read_bytes(),original);self.assertFalse(list(root.glob('.photonstack-product-*')))

if __name__=='__main__':unittest.main()
